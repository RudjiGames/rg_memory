/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 */

#ifndef RG_MEMORY_H_HEADER_GUARD
#define RG_MEMORY_H_HEADER_GUARD

#include <stdint.h> /* uint*_t */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/*--------------------------------------------------------------------------
 * Structures describing memory data
 *------------------------------------------------------------------------*/

    /* Memory arena control state.
     *
     * Caller-owned: place this struct wherever you like (stack, .data,
     * embedded inside another struct). rgArenaCreate populates it in place;
     * rgArenaDestroy releases the VM reservation it owns. A zero-initialised
     * Arena is treated as uninitialised and every API call on it is a safe
     * no-op (Alloc returns 0, queries return 0, Destroy does nothing).
     *
     * Each live Arena owns a VM reservation; do not copy or memcpy a live
     * Arena struct -- both copies would refer to the same reservation and
     * double-destroy would corrupt the address space. Move ownership by
     * destroying the source after copying the bytes.
     *
     * Treat the fields as private to the implementation; do not read or
     * mutate them directly.
     */
    typedef struct Arena
    {
        uint8_t*    m_base;      /* 0 marks the Arena as uninitialised. */
        uint64_t    m_reserved;  /* total reserved bytes (page-aligned for  */
                                 /* anonymous arenas; rgArenaOpenShared     */
                                 /* stores the exact FILE size, unrounded). */
        uint64_t    m_committed; /* bytes currently committed              */
                                 /* (page-aligned; grows lazily).          */
        uint64_t    m_pos;       /* offset of next byte to allocate.       */
        uint32_t    m_backing;   /* 0 = anonymous VM (default), 1 = file / */
                                 /* shared-memory mapping. Selects the     */
                                 /* teardown path; no OS handle is kept    */
                                 /* (the mapping outlives a closed handle).*/

    } Arena;

    /* Free list control state.
     *
     * Caller-owned: place this struct wherever you like (stack, .data,
     * embedded inside another struct). rgFreeListCreate /
     * rgFreeListCreateFromMemory populate it in place; there is no
     * matching Destroy because the library owns no resources tied to
     * the struct itself. A zero-initialised FreeList is treated as
     * uninitialised and every API call on it is a safe no-op
     * (Alloc returns 0, Free does nothing, size queries return 0).
     *
     * Treat the fields as private to the implementation; do not read
     * or mutate them directly.
     */
    typedef struct FreeList
    {
        uint32_t    m_maxBlocks;
        uint32_t    m_blockSize;
        uint32_t    m_blocksFree;
        uint32_t    m_initHigh;     /* Lazy bump watermark: blocks [m_initHigh,  */
                                    /* m_maxBlocks) are not yet linked into the  */
                                    /* chain and are handed out by bump-alloc.   */
                                    /* Eager Create sets this == m_maxBlocks so  */
                                    /* the bump path is never taken.             */
        uint8_t*    m_buffer;       /* 0 marks the FreeList as uninitialised. */
        uint8_t*    m_next;         /* Head of free chain, or 0 when empty.   */

    } FreeList;

    /* Dense-packed pool: live blocks always occupy the prefix [0, count)
     * of the underlying buffer. rgDenseListFree(ptr) overwrites ptr's
     * slot with the contents of the last live block and decrements the
     * count, so iteration over live elements is a tight loop over a
     * contiguous range with no holes.
     *
     * Trade-off versus FreeList: no stable pointers (every Free may
     * relocate any element's data) in exchange for cache-friendly
     * iteration and no per-block chain overhead. Use this when you walk
     * the live set more often than you hold references into it
     * (particle systems, transient draw lists, ECS component arrays).
     *
     * Caller-owned like FreeList; a zero-initialised DenseList is inert
     * and every API call on it is a safe no-op. Treat the fields as
     * private to the implementation.
     */
    typedef struct DenseList
    {
        uint32_t    m_maxBlocks;
        uint32_t    m_blockSize;
        uint32_t    m_count;        /* live blocks, occupying [0, m_count). */
        uint8_t*    m_buffer;       /* 0 marks the DenseList as uninitialised. */

    } DenseList;

    /* Probabilistic set membership filter (Bloom), **cache-line blocked**.
     *
     * Bit array partitioned into 512-bit blocks (one cache line each).
     * Every Add / Test for a key picks a single block from the key's first
     * hash, then sets / tests all k bits inside that one block: one cache
     * line touched per operation regardless of k. Compared to a classic
     * unblocked Bloom this trades ~30% memory for ~k-fold fewer cache-line
     * loads per query -- a 3-7x latency win once the filter spills out of
     * L1 / L2.
     *
     * Sized in bits (rounded up to a whole power-of-two number of 512-bit
     * blocks, minimum 1 block = 64 bytes / 512 bits). The library uses
     * double-hashing (Kirsch & Mitzenmacher, 2006) within each block to
     * derive the k bit positions from two underlying hashes.
     *
     * Caller-owned (rgBloomFilterCreate / CreateFromMemory populate in place;
     * no Destroy). A zero-initialised BloomFilter is inert: Add is a no-op,
     * Test returns 0, queries return 0.
     *
     * Concurrent Add + Test is safe: Add uses release-ordered atomic OR per
     * bit-set, Test uses acquire-ordered atomic loads per bit-read. The
     * monotone bits-set-only invariant makes Bloom naturally lock-free; no
     * CAS retry, no ABA concerns. Clear is NOT concurrent-safe (it bulk-
     * zeroes); call it only when no other thread touches the filter.
     *
     * Treat the fields as private; do not read or mutate them directly.
     */
    typedef struct BloomFilter
    {
        uint64_t    m_blockMask;    /* m_blockCount - 1 (power of two), fast h&mask. */
        uint32_t    m_blockCount;   /* number of 512-bit blocks; bit array is m_blockCount * 8 uint64s. */
        uint32_t    m_hashCount;    /* k -- number of bit positions per Add / Test, all inside the chosen block. */
        uint64_t*   m_bits;         /* 0 marks the BloomFilter uninitialised. */

    } BloomFilter;

    /* Toggle the direct-mapped top-level index.
     *
     * 1 (default): each hash structure begins with a flat array of
     *              2^RG_HASH_TOP_BITS absolute root pointers indexed by
     *              the top RG_HASH_TOP_BITS of the key's wyhash digest.
     *              The 4-ary trie descent picks up from there.
     * 0          : single m_root slot, like before the top-level index
     *              was introduced. Useful for size-constrained embedded
     *              targets where the 32 KiB-per-struct footprint is too
     *              high, or for A/B benchmarking.
     *
     * Toggling this changes the public struct layout, so the entire
     * rg_memory TU and all of its consumers must agree on the value.
     */
    #define RG_HASH_USE_TOP_INDEX 1

    /* Direct-mapped top-level index width (only meaningful when
     * RG_HASH_USE_TOP_INDEX is 1).
     *
     * 12 bits = 4096 slots = 32 KiB / index, which matches Zen 4's 32 KiB
     * L1 D-cache exactly and keeps the entire top array L1-resident on
     * hot benchmarks. Shrinking the index trades L1 footprint for a deeper
     * residual trie walk; widening past 14 bits spills out of L1 with no
     * commensurate gain.
     *
     * Public so callers can size their own embeddings of these structs
     * if they need to. Each Init zeroes the array.
     */
    #define RG_HASH_TOP_BITS   12u
    #define RG_HASH_TOP_SIZE   (1u << RG_HASH_TOP_BITS)

    /* Hash map (single-threaded), 4-ary hash trie backed by an arena.
     *
     * Stores arbitrary byte-keys mapped to opaque 64-bit values. Slots
     * hold 32-bit offsets into the host arena's m_base (0 = 0). The
     * encoding caps the host arena at 4 GiB, but halves the size of
     * every slot vs the 64-bit-absolute-pointer scheme used by HashTrie
     * and HashIndex -- the m_top array drops from 32 KiB to 16 KiB and
     * every node's 4 child pointers drop from 32 B to 16 B.
     *
     * Init reserves the first cache line of a fresh arena as a sentinel
     * so no real HashMap node ever lives at offset 0; that lets 0
     * unambiguously mean "empty slot" everywhere a slot is read.
     *
     * Caller-owned: place the struct anywhere; rgHashMapInit binds it to
     * an arena. A zero-initialised HashMap (m_arena == 0) is treated
     * as uninitialised and every API call on it is a safe no-op.
     *
     * Sizable: the inline m_top array is 16 KiB on the default
     * RG_HASH_TOP_BITS. Heap-allocate the struct if that's a problem
     * for stack-based use.
     *
     * NOT thread-safe. Use HashTrie for concurrent access (HashTrie
     * keeps 64-bit absolute pointers so its multi-writer model can
     * stitch nodes from multiple per-thread arenas together).
     */
    typedef struct HashMap
    {
        Arena*        m_arena;                  /* host arena; 0 marks the HashMap uninit. */
#if RG_HASH_USE_TOP_INDEX
        uint32_t      m_top[RG_HASH_TOP_SIZE];  /* direct-mapped top-level roots (arena offsets), 0 = empty. */
#else
        uint32_t      m_root;                   /* offset of root node into arena (bytes), 0 = empty. */
#endif

    } HashMap;

    /* Concurrent hash trie -- the lock-free counterpart to HashMap, with
     * the same node layout. Slot updates are strong CAS with release-
     * on-success / acquire-on-failure ordering, matching the pattern
     * from nullprogram 2023-09-30. Slots are 64-bit absolute pointers,
     * so nodes belonging to several arenas can coexist in one trie --
     * each writer brings its own arena to rgHashTriePut.
     *
     * Concurrency model:
     *   - Concurrent Get calls            : safe (atomic loads only).
     *   - Concurrent Get + Put            : safe (CAS-published nodes).
     *   - Concurrent Put + Put on a slot  : structurally safe -- one CAS wins,
     *                                       losers retry from the slot's new
     *                                       value. Loser's allocated node and
     *                                       key bytes stay in the arena (never
     *                                       reachable, bounded by retry rate).
     *   - Allocation                      : rgArenaAlloc is single-threaded by
     *                                       convention. Multi-writer Put is
     *                                       supported by giving each writer
     *                                       thread its own arena and passing
     *                                       that arena to rgHashTriePut.
     *
     * Values are 64-bit and can be atomically overwritten by a later Put;
     * readers see either the old or new value, never a torn read. The trie
     * does not support deletion (matches Wellons' algorithm; the "pointer
     * once set never changes" invariant is what eliminates ABA).
     *
     * Zero-initialised HashTrie is a valid empty trie. rgHashTrieInit is
     * provided for symmetry and to clear an existing trie back to empty.
     * The lifetime of every arena that contributed a node must extend
     * past the last Get on the trie -- destroying an arena early leaves
     * the trie holding dangling pointers.
     */
    typedef struct HashTrie
    {
#if RG_HASH_USE_TOP_INDEX
        uint64_t      m_top[RG_HASH_TOP_SIZE];  /* direct-mapped top-level roots, 0 = empty. */
#else
        uint64_t      m_root;                   /* absolute pointer to root node, 0 = empty. */
#endif
        uint32_t      m_flags;                  /* RGM_HASHTRIE_FLAG_* (e.g. NOCOPY). */
                                                /* 0 (zero-init) = default copy mode. */

    } HashTrie;

    /* Flags accepted by rgHashTrieInitEx (bitwise OR-able). */
    #define RGM_HASHTRIE_FLAG_NONE   0u

    /* Do not copy keys: store a pointer to the caller's key bytes instead of
     * an inline copy. The caller guarantees every key passed to Put stays
     * valid and immutable for as long as the trie may read it. Saves the copy
     * and the per-key storage -- a memory win for long, stable keys (e.g.
     * interned strings, asset paths held elsewhere).
     *
     * Contract: on a NOCOPY trie use ONLY the generic byte-key API
     * (rgHashTriePut / rgHashTrieGet / rgHashTrieForEach). The uint64 and
     * batch-uint64 specialisations assume inline keys and must not be mixed
     * into a NOCOPY trie. ForEach reports the caller's original key pointer. */
    #define RGM_HASHTRIE_FLAG_NOCOPY (1u << 0)

    /* Arena save point.
     *
     * Plain offset into the arena's bump region. Pair rgArenaSave with
     * rgArenaRestore to implement scratch / temporary sub-lifetimes without
     * explicit per-allocation frees. Save / Restore pairs nest naturally
     * as long as the caller restores in LIFO order. The caller is
     * responsible for pairing a save point with the arena it was taken from.
     */
    typedef uint64_t ArenaSavePoint;

    /* Sentinel returned by rgArenaSave for a 0 or uninitialised arena.
     * Compares unequal to any save point a live arena can produce (no arena
     * can fill the entire 64-bit address space). Passing this value to
     * rgArenaRestore is a no-op. */
    #define RGM_ARENA_INVALID_SAVE_POINT ((ArenaSavePoint)UINT64_MAX)

    /* Error codes returned by the Create variants.
     *   0     : success
     *  -1     : invalid argument (0 out, 0 buffer, zero size/blocks)
     *  -2     : size overflow
     *  -3     : out of memory (VM reservation failed, or arena exhausted)
     *  -4     : caller buffer too small
     *  -5     : file I/O / mapping failure (shared-arena create / open)
     *  -6     : bad / unrecognised persisted format (rgHashMapOpen)
     */
    #define RGM_ERROR_OK                   0
    #define RGM_ERROR_ERR_INVALID         -1
    #define RGM_ERROR_ERR_OVERFLOW        -2
    #define RGM_ERROR_ERR_NO_MEMORY       -3
    #define RGM_ERROR_ERR_TOO_SMALL       -4
    #define RGM_ERROR_ERR_IO              -5
    #define RGM_ERROR_ERR_FORMAT          -6

    /* Flags accepted by rgArenaCreateEx (bitwise OR-able). */
    #define RGM_ARENA_FLAG_NONE            0u

    /* Request huge / large pages for the reservation.
     *
     * Cuts TLB pressure dramatically on large arenas (tens of MiB and up).
     * The library treats this flag as a *hint*: if the OS refuses, the
     * arena is created with normal pages and the call still succeeds.
     *
     * Platform notes:
     *   - Linux / Android / *BSD : mmap + madvise(MADV_HUGEPAGE). The
     *     kernel decides whether to back the reservation with huge pages;
     *     lazy commit semantics are unchanged.
     *   - macOS : no effect (POSIX MADV_HUGEPAGE is Linux-specific).
     *     The arena uses normal pages.
     *   - Windows : VirtualAlloc with MEM_LARGE_PAGES. Requires the
     *     calling process to hold SeLockMemoryPrivilege (configured in
     *     "Lock pages in memory" under Local Security Policy). Large
     *     pages on Windows must be committed eagerly, so the entire
     *     reservation is physically backed at create time when this flag
     *     is used and the privilege is held. Without the privilege the
     *     call silently falls back to lazy normal pages.
     */
    #define RGM_ARENA_FLAG_HUGE_PAGES      (1u << 0)

/*--------------------------------------------------------------------------
 * Arena API
 *------------------------------------------------------------------------*/

    /* Initialise an arena reserving the given amount of virtual address space.
     *
     * The arena reserves _arenaSize bytes of virtual address space up-front
     * but only commits physical pages as allocations advance the bump pointer,
     * so a large reservation is cheap.
     *
     * @param[out] _arena     - Arena to initialise. Must be non-0.
     * @param[in]  _arenaSize - Total size, in bytes, to reserve for the arena.
     * @returns 0 on success, or a negative error code on failure:
     *           -1 invalid argument (0 _arena, zero _arenaSize),
     *           -3 OS rejected the reservation.
     *          On failure *_arena is left unchanged.
     */
    int32_t rgArenaCreate(Arena* _arena, uint64_t _arenaSize);

    /* Initialise an arena with optional creation flags.
     *
     * Same contract as rgArenaCreate, with an extra _flags parameter.
     * See RGM_ARENA_FLAG_* for the available flags. Pass 0 / RGM_ARENA_FLAG_NONE
     * for behavior identical to rgArenaCreate.
     *
     * Unrecognised flag bits are ignored. Hint-style flags
     * (e.g. RGM_ARENA_FLAG_HUGE_PAGES) never cause creation to fail; the
     * library falls back to the equivalent normal-pages path when the OS
     * refuses the hint.
     *
     * @param[out] _arena     - Arena to initialise. Must be non-0.
     * @param[in]  _arenaSize - Total size, in bytes, to reserve.
     * @param[in]  _flags     - Bitwise OR of RGM_ARENA_FLAG_* values.
     * @returns Same codes as rgArenaCreate.
     */
    int32_t rgArenaCreateEx(Arena* _arena, uint64_t _arenaSize, uint32_t _flags);

    /* Create an arena backed by a memory-mapped file (persistence / shared
     * memory) instead of anonymous virtual memory.
     *
     * The file at _path is created (truncating any existing file) at
     * _arenaSize bytes (rounded up to the page size) and mapped MAP_SHARED /
     * a writable file view. Allocations land directly in the file, so the
     * arena's contents survive process exit and can be re-mapped later with
     * rgArenaOpenShared, or shared between processes that map the same path.
     *
     * Unlike the anonymous arena, the whole file is backed up front (no lazy
     * commit), so rgArenaShrink is a no-op and the reservation equals the
     * file size. No OS handle is retained -- rgArenaDestroy unmaps the view
     * (which also flushes dirty pages); the file itself remains on disk.
     *
     * A HashMap built in such an arena is position-independent (all inter-
     * node links are arena-relative offsets); persist its top-level index
     * with rgHashMapSave and reconstruct it after reopening with
     * rgHashMapOpen.
     *
     * Caveat: the on-disk layout is build-specific (host endianness and the
     * RG_HASH_* configuration). Files are not portable across architectures.
     *
     * @param[out] _arena     - Arena to initialise. Must be non-0.
     * @param[in]  _path      - Filesystem path for the backing file.
     * @param[in]  _arenaSize - File / mapping size in bytes (page-rounded).
     * @returns 0 on success, -1 invalid argument, -5 on file/mapping failure.
     */
    int32_t rgArenaCreateShared(Arena* _arena, const char* _path, uint64_t _arenaSize);

    /* Like rgArenaCreateShared, but the backing file is a TEMPORARY that the OS
     * removes automatically once the mapping is released -- on rgArenaDestroy
     * (unmap) and, crucially, on process exit of ANY kind including a crash,
     * kill or TerminateProcess. Windows opens it FILE_FLAG_DELETE_ON_CLOSE (the
     * live view holds the file until unmap / process death, then it is deleted);
     * POSIX unlink()s the path immediately after open (the inode lives on via
     * the mapping and vanishes when it is released). The file therefore never
     * leaks even if the process never runs its destructors. If the platform /
     * filesystem cannot honour auto-delete the call transparently falls back to
     * a plain shared file (the caller is then responsible for removing _path).
     *
     * Use this for scratch / spill files that must not outlive the process;
     * use rgArenaCreateShared for files meant to persist (e.g. on-disk maps).
     *
     * @param[out] _arena     - Arena to initialise. Must be non-0.
     * @param[in]  _path      - Filesystem path for the backing temp file.
     * @param[in]  _arenaSize - File / mapping size in bytes (page-rounded).
     * @returns 0 on success, -1 invalid argument, -5 on file/mapping failure.
     */
    int32_t rgArenaCreateSharedTemp(Arena* _arena, const char* _path, uint64_t _arenaSize);

    /* Map an existing arena file created by rgArenaCreateShared.
     *
     * Maps the whole file at its current size. The arena initially reports
     * itself as fully used (m_pos == reserved); rgHashMapOpen rewinds m_pos
     * to the persisted high-water mark so the reopened map can still be
     * appended to. For a read-only consumer pass _readOnly != 0 (writes to
     * the mapping then fault).
     *
     * @param[out] _arena    - Arena to initialise. Must be non-0.
     * @param[in]  _path     - Path to a file previously created shared.
     * @param[in]  _readOnly - Non-zero maps read-only; zero maps read/write.
     * @returns 0 on success, -1 invalid argument, -5 on file/mapping failure.
     */
    int32_t rgArenaOpenShared(Arena* _arena, const char* _path, int _readOnly);

    /* Flush a file-backed arena's dirty pages to disk (msync / FlushViewOfFile).
     * No-op on anonymous arenas. rgArenaDestroy already flushes via unmap;
     * use this for an explicit mid-session durability checkpoint.
     *
     * @param[in] _arena - Arena to flush.
     */
    void rgArenaFlush(Arena* _arena);

    /* Test whether an Arena is initialised and live.
     *
     * @param[in] _arena - Arena to test.
     * @returns 1 if _arena is non-0 and currently holds a live VM
     *          reservation, 0 otherwise.
     */
    int rgArenaIsValid(Arena* _arena);

    /* Destroy an arena, decommitting and releasing all memory it owns.
     *
     * Sets *_arena to the uninitialised state so subsequent API calls
     * become safe no-ops. Idempotent: a second Destroy on the same Arena
     * is a no-op. No-op when _arena is 0.
     *
     * @param[in] _arena - Arena to destroy.
     */
    void rgArenaDestroy(Arena* _arena);

    /* Reset an arena to its empty state.
     *
     * All allocations made from the arena become invalid, but the underlying
     * memory reservation is kept alive for subsequent allocations. No-op
     * when _arena is 0 or uninitialised.
     *
     * @param[in] _arena - Arena to clear.
     */
    void rgArenaClear(Arena* _arena);

    /* Allocate a block of memory from the arena, 16-byte aligned.
     *
     * @param[in] _arena - Arena to allocate from.
     * @param[in] _size  - Number of bytes to allocate.
     * @returns Pointer to the allocated block, or 0 when the arena is
     *          exhausted, _arena is 0 or uninitialised, or _size is zero.
     */
    void* rgArenaAlloc(Arena* _arena, uint64_t _size);

    /* Allocate an aligned block of memory from the arena.
     *
     * @param[in] _arena     - Arena to allocate from.
     * @param[in] _size      - Number of bytes to allocate.
     * @param[in] _alignment - Required alignment, in bytes. Must be a power of two.
     * @returns Pointer to the allocated block, or 0 on failure.
     */
    void* rgArenaAllocAligned(Arena* _arena, uint64_t _size, uint64_t _alignment);

    /* Pop the most recent allocation off the arena by size.
     *
     * Rewinds the bump pointer by _size bytes. The caller is responsible for
     * passing the exact size they originally allocated; this is unchecked.
     * Allocations must be popped in LIFO order.
     *
     * Note: alignment padding inserted by the matching alloc call (if any)
     * is not recovered and remains below the new high-water mark until the
     * arena is cleared or restored.
     *
     * @param[in] _arena - Arena that owns the allocation.
     * @param[in] _size  - Number of bytes to rewind. Must match the alloc.
     */
    void rgArenaPop(Arena* _arena, uint64_t _size);

    /* Pop the most recent aligned allocation off the arena by size.
     *
     * Same as rgArenaPop but takes the alignment used at allocation time so
     * the resulting high-water mark can be validated against it in debug builds.
     *
     * @param[in] _arena     - Arena that owns the allocation.
     * @param[in] _size      - Number of bytes to rewind. Must match the alloc.
     * @param[in] _alignment - Alignment passed to the original alloc.
     */
    void rgArenaPopAligned(Arena* _arena, uint64_t _size, uint64_t _alignment);

    /* Capture the arena's current allocation high-water mark.
     *
     * Use rgArenaSave / rgArenaRestore as a "scratch" mechanism: take a
     * save point, allocate freely, then rewind back to drop every
     * allocation made in the meantime in one shot. Pairs nest naturally
     * as long as the caller restores in LIFO order.
     *
     * @param[in] _arena - Arena to snapshot.
     * @returns Current pos as a save point, or RGM_ARENA_INVALID_SAVE_POINT
     *          when _arena is 0 or uninitialised. A freshly created
     *          (empty) arena returns 0, distinct from the sentinel.
     */
    ArenaSavePoint rgArenaSave(Arena* _arena);

    /* Restore an arena to a previously saved high-water mark.
     *
     * All allocations made from _arena since _save was taken become invalid.
     * The caller is responsible for pairing _save with the arena it was taken
     * from; passing a save point from a different arena is undefined.
     *
     * Passing RGM_ARENA_INVALID_SAVE_POINT is a silent no-op, so a
     * Save / Restore pair survives an invalid arena on Save without
     * requiring an explicit validity check at the call site.
     *
     * @param[in] _arena - Arena to restore.
     * @param[in] _save  - Save point previously returned by rgArenaSave.
     */
    void rgArenaRestore(Arena* _arena, ArenaSavePoint _save);

    /* Report the arena's current allocation high-water mark, in bytes.
     *
     * @param[in] _arena - Arena to query.
     * @returns Number of bytes currently allocated from _arena, including
     *          any alignment padding inserted by previous allocations.
     *          Returns 0 when _arena is 0 or uninitialised.
     */
    uint64_t rgArenaUsed(Arena* _arena);

    /* Report the arena's total reserved capacity, in bytes.
     *
     * @param[in] _arena - Arena to query.
     * @returns Total bytes of virtual address space reserved for _arena
     *          (page-aligned, may exceed the size requested at create time).
     *          Returns 0 when _arena is 0 or uninitialised.
     */
    uint64_t rgArenaCapacity(Arena* _arena);

    /* Decommit pages above the current high-water mark, returning their
     * physical backing to the OS while keeping the VM reservation intact.
     *
     * Useful for arenas with bursty peak usage that should give physical
     * memory back between bursts -- e.g. a frame-scoped arena that wants
     * to shrink during loading screens. The reservation itself is
     * untouched so subsequent allocations grow the committed region back
     * up using the existing geometric path.
     *
     * Rounds the new committed boundary up to the OS page size, so the
     * page containing the current high-water mark is kept committed.
     *
     * No-op when _arena is 0, uninitialised, or when nothing above
     * the current high-water mark is committed. No-op when the OS
     * refuses the decommit (e.g. eager-commit huge pages on Windows
     * that cannot be partially decommitted) -- the arena keeps working.
     *
     * @param[in] _arena - Arena to shrink.
     */
    void rgArenaShrink(Arena* _arena);

    /* Like rgArenaShrink, but retains _slackBytes of committed memory above the
     * high-water mark before decommitting the rest. rgArenaShrink(_arena) is
     * exactly rgArenaShrinkKeep(_arena, 0).
     *
     * The slack is hysteresis for oscillating / streaming workloads that grow
     * and shrink repeatedly around a working-set size: keeping a small window
     * committed avoids the decommit-then-recommit-then-fault churn that a
     * zero-slack shrink causes when the next allocation immediately re-grows.
     * Aggressive-reclaim callers (bounded-RSS mmap arenas that shrink precisely
     * to drop cold pages) should keep using rgArenaShrink / slack 0.
     *
     * _slackBytes is added to the high-water mark and the sum is rounded up to
     * the page size; a value large enough to reach the committed boundary makes
     * this a no-op. Same file-backed / huge-page / not-live no-ops as
     * rgArenaShrink.
     *
     * @param[in] _arena      - Arena to shrink.
     * @param[in] _slackBytes - Committed bytes to retain above the high-water mark.
     */
    void rgArenaShrinkKeep(Arena* _arena, uint64_t _slackBytes);

    /* Release physical pages from the FRONT of the arena - everything strictly below _upToOffset
     * (rounded DOWN to the page size, so only fully-consumed pages are dropped). Unlike
     * rgArenaShrink (which trims the unused tail above the high-water mark), this punches a
     * decommitted hole at the base and does NOT preserve the contiguous-commit invariant: the
     * arena is afterwards valid only for reading past _upToOffset, until rgArenaDestroy releases
     * the whole reservation. Designed for streaming filter-compaction - walk a source arena
     * front-to-back, copy survivors into a destination arena, and decommit consumed source pages
     * behind the cursor so peak commit stays ~= one arena instead of source + destination.
     *
     *   - Windows : VirtualFree(base, len, MEM_DECOMMIT)
     *   - POSIX   : madvise(base, len, MADV_DONTNEED) + mprotect(PROT_NONE), so a stray
     *               access below _upToOffset traps like it does on Windows instead of
     *               silently refaulting zero pages
     *
     * No-op on 0/uninitialised/file-backed arenas, or when nothing is fully consumed yet.
     *
     * @param[in] _arena      - Arena whose front pages to release.
     * @param[in] _upToOffset - Bytes from the base that have been consumed; pages wholly below
     *                          this (page-aligned down) are decommitted.
     */
    void rgArenaDecommitFront(Arena* _arena, uint64_t _upToOffset);

/*--------------------------------------------------------------------------
 * FreeList API
 *------------------------------------------------------------------------*/

    /* Create a fixed-block free list with its buffer backed by the given arena.
     *
     * Populates *_list in place. The block buffer is allocated from
     * _arena; the FreeList struct itself is caller-owned and needs no
     * explicit destroy. rgArenaClear / rgArenaDestroy reclaim the buffer,
     * after which _list must not be used again.
     *
     * _blockSize is clamped up to sizeof(uint32_t) (so the in-place free
     * chain can stash next-index links) and then rounded up to a multiple
     * of 16 so block N inherits the arena's 16-byte alignment for all N.
     *
     * @param[in]  _arena      - Arena to allocate the block buffer from.
     * @param[out] _list       - FreeList to initialise. Must be non-0.
     * @param[in]  _blockSize  - Block size, in bytes.
     * @param[in]  _maxBlocks  - Maximum number of blocks in the list.
     * @returns 0 on success, or a negative error code on failure:
     *           -1 invalid argument (0 _list, 0 / uninitialised _arena,
     *              zero _maxBlocks),
     *           -2 size overflow,
     *           -3 arena exhausted.
     *          On failure *_list is left unchanged.
     */
    int32_t rgFreeListCreate(Arena* _arena, FreeList* _list,
                             uint64_t _blockSize, uint32_t _maxBlocks);

    /* Lazy-init variant of rgFreeListCreate. Identical arguments, return codes
     * and allocation behaviour, but does NOT walk the buffer linking every
     * block into the free chain at create time. Instead blocks are handed out
     * by a bump watermark on first use and only join the chain once freed.
     *
     * For a large pool this turns an O(_maxBlocks) pass of scattered
     * cache-line writes (one per block, faulting every page of the buffer
     * before it is ever used) into O(1) setup - the pages fault in lazily as
     * blocks are actually allocated. The observable behaviour is identical to
     * an eager list: allocation still returns blocks in ascending order
     * (0,1,2,...) until the first Free, and Free/Alloc/queries behave the
     * same. Prefer this for big or sparsely-used pools; the eager
     * rgFreeListCreate remains fine for small ones.
     *
     * @param[in]  _arena      - Arena to allocate the block buffer from.
     * @param[out] _list       - FreeList to initialise. Must be non-0.
     * @param[in]  _blockSize  - Block size, in bytes.
     * @param[in]  _maxBlocks  - Maximum number of blocks in the list.
     * @returns Same codes as rgFreeListCreate.
     */
    int32_t rgFreeListCreateLazy(Arena* _arena, FreeList* _list,
                                 uint64_t _blockSize, uint32_t _maxBlocks);

    /* Create a fixed-block free list using a caller-owned memory buffer.
     *
     * Same semantics as rgFreeListCreate but the block buffer is supplied by
     * the caller instead of being allocated from an arena. The buffer must
     * outlive the FreeList; nothing in this library frees or otherwise
     * touches _buffer after this call returns.
     *
     * _blockSize is clamped and rounded up the same way as rgFreeListCreate.
     * The effective per-block size (visible via rgFreeListBlockSize) may
     * therefore exceed the value originally passed in. _bufferSize must be
     * at least the effective _blockSize times _maxBlocks; use
     * rgFreeListBufferSize to compute that requirement up front.
     *
     * _buffer must be 16-byte aligned so block N inherits the alignment for
     * every N. Misaligned buffers trigger an assertion in debug builds.
     *
     * @param[in]  _buffer     - Caller-owned, 16-byte-aligned memory.
     * @param[in]  _bufferSize - Size of _buffer, in bytes.
     * @param[out] _list       - FreeList to initialise. Must be non-0.
     * @param[in]  _blockSize  - Block size, in bytes.
     * @param[in]  _maxBlocks  - Maximum number of blocks in the list.
     * @returns 0 on success, or a negative error code on failure:
     *           -1 invalid argument (0 _buffer, 0 _list, zero _maxBlocks),
     *           -2 size overflow,
     *           -4 _bufferSize is smaller than the effective layout requires.
     *          On failure *_list is left unchanged.
     */
    int32_t rgFreeListCreateFromMemory(void* _buffer, uint64_t _bufferSize, FreeList* _list,
                                       uint64_t _blockSize, uint32_t _maxBlocks);

    /* Compute the buffer size rgFreeListCreateFromMemory would require.
     *
     * Returns the effective per-block size (after the clamp + 16-byte round-
     * up) multiplied by _maxBlocks. Use this to size a caller-owned buffer
     * exactly, without duplicating the rounding rules.
     *
     * @param[in] _blockSize  - Block size, in bytes (as passed to Create).
     * @param[in] _maxBlocks  - Maximum number of blocks in the list.
     * @returns Required buffer size in bytes, or 0 on overflow / zero input.
     */
    uint64_t rgFreeListBufferSize(uint64_t _blockSize, uint32_t _maxBlocks);

    /* Allocate one fixed-size block from the free list.
     *
     * @param[in] _list - Free list to allocate from.
     * @returns Pointer to a block of rgFreeListBlockSize(_list) bytes,
     *          or 0 when the list is exhausted, _list is 0, or
     *          _list is uninitialised (zero-initialised struct).
     */
    void* rgFreeListAlloc(FreeList* _list);

    /* Return a block to the free list.
     *
     * No-op when _list is 0 or uninitialised.
     *
     * @param[in] _list - Free list that owns the block.
     * @param[in] _ptr  - Pointer previously returned by rgFreeListAlloc on
     *                    the same list. Must not already be free; double-
     *                    free corrupts the chain (caught only by an
     *                    assertion if the duplicate is at the chain head).
     */
    void rgFreeListFree(FreeList* _list, void* _ptr);

    /* Coarse "is this pointer inside the list's buffer" probe.
     *
     * Does not verify block alignment or that the pointer is currently
     * allocated -- it's only a range check against the underlying buffer.
     *
     * @param[in] _list - Free list to query.
     * @param[in] _ptr  - Pointer to test.
     * @returns 1 if _ptr lies within the list's block buffer, 0 otherwise
     *          (including when _list is 0 or uninitialised).
     */
    int rgFreeListCheckPtr(FreeList* _list, void* _ptr);

    /* Report the maximum number of blocks the free list was created with.
     *
     * @param[in] _list - Free list to query.
     * @returns The configured block count, or 0 if _list is 0 or
     *          uninitialised.
     */
    uint32_t rgFreeListMaxBlocks(FreeList* _list);

    /* Report the effective per-block size of the free list, in bytes.
     *
     * This is the size actually used by the allocator after rgFreeListCreate
     * clamped the requested size up to sizeof(uint32_t) and rounded it up to
     * a multiple of 16. It may exceed the value originally passed to Create.
     *
     * @param[in] _list - Free list to query.
     * @returns The effective block size in bytes, or 0 if _list is 0
     *          or uninitialised.
     */
    uint32_t rgFreeListBlockSize(FreeList* _list);

    /* Report the number of blocks currently available for allocation.
     *
     * @param[in] _list - Free list to query.
     * @returns The count of unallocated blocks, or 0 if _list is 0
     *          or uninitialised.
     */
    uint32_t rgFreeListBlocksFree(FreeList* _list);

/*--------------------------------------------------------------------------
 * DenseList API
 *------------------------------------------------------------------------*/

    /* Create a dense-packed pool with its buffer backed by the given arena.
     *
     * Populates *_list in place. The block buffer is allocated from
     * _arena; the DenseList struct itself is caller-owned and needs no
     * explicit destroy. rgArenaClear / rgArenaDestroy reclaim the buffer,
     * after which _list must not be used again.
     *
     * _blockSize is rounded up to a multiple of 16 so block N inherits
     * the arena's 16-byte alignment for every N. There is no minimum
     * block size beyond that round-up (unlike FreeList, which needs to
     * stash chain links in each free block).
     *
     * @param[in]  _arena      - Arena to allocate the block buffer from.
     * @param[out] _list       - DenseList to initialise. Must be non-0.
     * @param[in]  _blockSize  - Block size, in bytes.
     * @param[in]  _maxBlocks  - Maximum number of blocks in the list.
     * @returns 0 on success, or a negative error code on failure:
     *           -1 invalid argument (0 _list, 0 / uninitialised _arena,
     *              zero _maxBlocks),
     *           -2 size overflow,
     *           -3 arena exhausted.
     *          On failure *_list is left unchanged.
     */
    int32_t rgDenseListCreate(Arena* _arena, DenseList* _list,
                              uint64_t _blockSize, uint32_t _maxBlocks);

    /* Create a dense-packed pool using a caller-owned memory buffer.
     *
     * Same semantics as rgDenseListCreate but the block buffer is supplied
     * by the caller. The buffer must outlive the DenseList; nothing in
     * this library frees or otherwise touches _buffer after this call.
     *
     * _blockSize is rounded up the same way as rgDenseListCreate; the
     * effective per-block size (visible via rgDenseListBlockSize) may
     * exceed the value originally passed in. _bufferSize must be at
     * least the effective _blockSize times _maxBlocks; use
     * rgDenseListBufferSize to compute that requirement up front.
     *
     * _buffer must be 16-byte aligned so block N inherits the alignment
     * for every N. Misaligned buffers trigger an assertion in debug builds.
     *
     * @param[in]  _buffer     - Caller-owned, 16-byte-aligned memory.
     * @param[in]  _bufferSize - Size of _buffer, in bytes.
     * @param[out] _list       - DenseList to initialise. Must be non-0.
     * @param[in]  _blockSize  - Block size, in bytes.
     * @param[in]  _maxBlocks  - Maximum number of blocks in the list.
     * @returns 0 on success, or a negative error code on failure:
     *           -1 invalid argument (0 _buffer, 0 _list, zero _maxBlocks),
     *           -2 size overflow,
     *           -4 _bufferSize is smaller than the effective layout requires.
     *          On failure *_list is left unchanged.
     */
    int32_t rgDenseListCreateFromMemory(void* _buffer, uint64_t _bufferSize,
                                        DenseList* _list,
                                        uint64_t _blockSize, uint32_t _maxBlocks);

    /* Compute the buffer size rgDenseListCreateFromMemory would require.
     *
     * Returns the effective per-block size (after the 16-byte round-up)
     * multiplied by _maxBlocks.
     *
     * @param[in] _blockSize  - Block size, in bytes (as passed to Create).
     * @param[in] _maxBlocks  - Maximum number of blocks in the list.
     * @returns Required buffer size in bytes, or 0 on overflow / zero input.
     */
    uint64_t rgDenseListBufferSize(uint64_t _blockSize, uint32_t _maxBlocks);

    /* Reset the live count to zero in O(1).
     *
     * The block buffer is left as-is; subsequent Allocs will overwrite
     * old contents. Cheaper than re-creating the list. No-op when
     * _list is 0 or uninitialised.
     *
     * @param[in] _list - DenseList to clear.
     */
    void rgDenseListClear(DenseList* _list);

    /* Allocate the next dense slot.
     *
     * Returns a pointer to slot m_count and post-increments m_count.
     * The block's contents are uninitialised.
     *
     * @param[in] _list - DenseList to allocate from.
     * @returns Pointer to a block of rgDenseListBlockSize(_list) bytes,
     *          or 0 when the list is full, _list is 0, or _list
     *          is uninitialised.
     */
    void* rgDenseListAlloc(DenseList* _list);

    /* Remove the block at _ptr by overwriting it with the contents of
     * the last live block, then decrementing m_count. If _ptr already
     * points at the last live block, no copy happens.
     *
     * INVALIDATION: every pointer previously returned by Alloc may now
     * refer to a different logical element. Callers must refetch the
     * pointer via rgDenseListAt / rgDenseListData before using it again.
     *
     * No-op when _list is 0, _list is uninitialised, or the list is
     * empty. _ptr must be the start of one of the live blocks at slots
     * [0, m_count) -- equivalently, a pointer previously returned by
     * rgDenseListAlloc / rgDenseListAt / rgDenseListData and not yet
     * invalidated by an intervening Free. Violations (out of range,
     * not on a block boundary) are asserted in debug builds.
     *
     * @param[in] _list - DenseList that owns the block.
     * @param[in] _ptr  - Pointer to the block to remove.
     */
    void rgDenseListFree(DenseList* _list, void* _ptr);

    /* Base pointer of the live region.
     *
     * Combine with rgDenseListCount and rgDenseListBlockSize for tight
     * iteration:
     *
     *   uint8_t* base   = (uint8_t*)rgDenseListData(&dl);
     *   uint32_t count  = rgDenseListCount(&dl);
     *   uint32_t stride = rgDenseListBlockSize(&dl);
     *   for (uint32_t i = 0; i < count; ++i) {
     *       MyType* x = (MyType*)(base + i * stride);
     *       ...
     *   }
     *
     * When freeing during iteration, walk backwards so the swapped-in
     * element ends up at an already-visited index:
     *
     *   for (uint32_t i = rgDenseListCount(&dl); i-- > 0; ) {
     *       MyType* x = (MyType*)rgDenseListAt(&dl, i);
     *       if (should_remove(x)) rgDenseListFree(&dl, x);
     *   }
     *
     * @param[in] _list - DenseList to query.
     * @returns Base pointer, or 0 when _list is 0 / uninitialised.
     */
    void* rgDenseListData(DenseList* _list);

    /* Pointer to slot _index.
     *
     * @param[in] _list  - DenseList to query.
     * @param[in] _index - Logical index in [0, count).
     * @returns Pointer to slot _index, or 0 when _index >= m_count
     *          or _list is 0 / uninitialised.
     */
    void* rgDenseListAt(DenseList* _list, uint32_t _index);

    /* Compute the slot index of a pointer previously returned by Alloc
     * (or by Data / At). Useful for parallel-array bookkeeping.
     *
     * Does not validate liveness; the returned index may be >= count
     * if the caller passes a stale pointer. Asserts in debug builds
     * when _ptr is not on a block boundary.
     *
     * @param[in] _list - DenseList to query.
     * @param[in] _ptr  - Pointer to translate.
     * @returns Slot index in [0, m_maxBlocks), or UINT32_MAX when
     *          _list is 0 / uninitialised, _ptr is 0, or _ptr
     *          lies outside the underlying buffer.
     */
    uint32_t rgDenseListIndexOf(DenseList* _list, void* _ptr);

    /* Report the maximum number of blocks the list was created with.
     *
     * @param[in] _list - DenseList to query.
     * @returns The configured block count, or 0 if _list is 0 or
     *          uninitialised.
     */
    uint32_t rgDenseListMaxBlocks(DenseList* _list);

    /* Report the effective per-block size, in bytes.
     *
     * This is the size actually used by the allocator after Create
     * rounded the requested size up to a multiple of 16. It may exceed
     * the value originally passed to Create.
     *
     * @param[in] _list - DenseList to query.
     * @returns The effective block size in bytes, or 0 if _list is 0
     *          or uninitialised.
     */
    uint32_t rgDenseListBlockSize(DenseList* _list);

    /* Report the number of live blocks (equivalent to "size" or "length").
     *
     * @param[in] _list - DenseList to query.
     * @returns The live block count, or 0 if _list is 0 or
     *          uninitialised.
     */
    uint32_t rgDenseListCount(DenseList* _list);

    /* Coarse "is this pointer inside the list's buffer" probe.
     *
     * Does not verify the pointer is within the live range -- only that
     * it falls inside [m_buffer, m_buffer + m_maxBlocks * m_blockSize).
     * Combine with rgDenseListCount if you need that distinction.
     *
     * @param[in] _list - DenseList to query.
     * @param[in] _ptr  - Pointer to test.
     * @returns 1 if _ptr lies within the buffer, 0 otherwise (including
     *          when _list is 0 or uninitialised).
     */
    int rgDenseListCheckPtr(DenseList* _list, void* _ptr);

/*--------------------------------------------------------------------------
 * HashMap API (single-threaded, arena-backed 4-ary hash trie)
 *
 * Stores arbitrary byte-keys -> 64-bit values. Inter-node references are
 * 32-bit relative offsets into the host arena. Lookups walk 2 hash bits
 * per level (4-ary branching), so depth is ~log4(N) on average.
 *------------------------------------------------------------------------*/

    /* Bind a HashMap to an arena and clear it.
     *
     * @param[out] _map   - HashMap to initialise. Must be non-0.
     * @param[in]  _arena - Live arena that nodes and keys will be allocated
     *                      from. The arena must remain live for the
     *                      lifetime of the HashMap.
     * @returns 0 on success, -1 if _map is 0 or _arena is not live.
     *          On failure *_map is left unchanged.
     */
    int32_t rgHashMapInit(HashMap* _map, Arena* _arena);

    /* Upsert a key, returning a pointer to its value slot.
     *
     * Keys are copied into the host arena on first insertion (the caller's
     * buffer can be reused / freed after Put returns). On a fresh insert
     * the value is zero-initialised; on a key that already exists the
     * stored value is left untouched. Either way the caller writes the
     * value it wants through the returned pointer:
     *
     *     uint64_t* v = rgHashMapPut(m, key, keyLen);
     *     if (v != 0) *v = value;
     *
     * The returned pointer is an absolute pointer into the host arena
     * (only the trie's inter-node child references are kept as offsets);
     * it stays valid until the arena is cleared or destroyed.
     *
     * @param[in] _map     - Live HashMap (rgHashMapInit'd).
     * @param[in] _key     - Pointer to key bytes.
     * @param[in] _keyLen  - Length of _key, in bytes. May be zero.
     * @returns Pointer to the value slot, or 0 on bad arguments or if
     *          the arena is exhausted (the key was not inserted).
     */
    uint64_t* rgHashMapPut(HashMap* _map, const void* _key, uint64_t _keyLen);

    /* Lookup a value by key.
     *
     * @param[in]  _map       - Live HashMap.
     * @param[in]  _key       - Pointer to key bytes.
     * @param[in]  _keyLen    - Length of _key, in bytes.
     * @returns Pointer to the stored value on hit, or 0 on bad
     *          arguments or key-not-found. The pointer is an absolute
     *          pointer into the host arena and stays valid until the arena
     *          is cleared or destroyed; the caller may read or overwrite
     *          the value through it.
     */
    uint64_t* rgHashMapGet(HashMap* _map, const void* _key, uint64_t _keyLen);

    /* Upsert, specialised for 8-byte uint64 keys.
     *
     * Equivalent to rgHashMapPut(m, &k, sizeof(k)) but with the hash
     * computed inline (no function call, no length dispatch) and the key
     * match testing a single uint64 == instead of a byte loop. ~10-15%
     * faster than the generic Put on uint64-keyed workloads. Returns a
     * pointer to the value slot (0 on failure); see rgHashMapPut.
     */
    uint64_t* rgHashMapPutU64(HashMap* _map, uint64_t _key);

    /* Lookup, specialised for 8-byte uint64 keys. Same speedup story
     * as rgHashMapPutU64. Returns a pointer to the value on hit, 0 on
     * miss. */
    uint64_t* rgHashMapGetU64(HashMap* _map, uint64_t _key);

    /* Callback type for rgHashMapForEach. Return 0 to continue iteration,
     * non-zero to stop early. _key / _keyLen point at the inline key bytes
     * inside the node (valid until the host arena is cleared/destroyed). */
    typedef int (*rgHashMapForEachFn)(const void* _key, uint64_t _keyLen,
                                      uint64_t _value, void* _userData);

    /* Visit every entry in the map exactly once, in trie-traversal order
     * (not insertion order). Iteration order is deterministic for a given
     * key set but should not be relied upon -- it depends on hash bits.
     *
     * Traversal uses a fixed 64-frame stack. Reaching depth 64 requires
     * engineered full-digest hash collisions (natural depth is ~log4(N));
     * if it ever happens, debug builds trap and release builds skip the
     * overflowing subtree.
     *
     * @param[in] _map      - Map to iterate.
     * @param[in] _fn       - Callback to invoke per entry. 0 is a no-op.
     * @param[in] _userData - Opaque pointer forwarded to the callback.
     * @returns Number of entries visited (== map size when iteration
     *          completes; less when the callback stops early).
     */
    uint64_t rgHashMapForEach(HashMap* _map, rgHashMapForEachFn _fn, void* _userData);

    /* Bulk insert / overwrite of uint64-keyed entries.
     *
     * Equivalent to looping over rgHashMapPutU64; provided as an API entry
     * point so a real future implementation can pipeline if profiling on
     * the engine's workload shows it's worth it. Returns the first error
     * encountered (e.g. arena exhausted partway through), in which case
     * earlier entries are inserted and later ones are not. Returns OK
     * when all _count entries were inserted.
     */
    int32_t rgHashMapPutBatchU64(HashMap* _map,
                                 const uint64_t* _keys, const uint64_t* _values,
                                 uint32_t _count);

    /* Bulk lookup of uint64-keyed entries with software-pipelined trie
     * walks: groups of keys are walked in lockstep so the CPU can issue
     * independent slot loads in parallel, hiding cache miss latency that
     * a serial loop would expose one-by-one. On hit, _outValues[i] is set
     * and _outFound[i] is set to 1; on miss, _outFound[i] is set to 0 and
     * _outValues[i] is left unchanged. _outFound may be 0 to skip the
     * per-key flag (callers can detect hits another way, e.g. by
     * pre-initialising _outValues to a sentinel).
     *
     * @returns Number of hits, or a negative error code on bad arguments.
     */
    int32_t rgHashMapGetBatchU64(HashMap* _map,
                                 const uint64_t* _keys, uint64_t* _outValues,
                                 int* _outFound, uint32_t _count);

    /* Persist a HashMap's top-level index so the map can be reconstructed
     * after its (file-backed) arena is unmapped and re-mapped.
     *
     * HashMap node links are already arena-relative offsets, so the node
     * graph is position-independent on its own; the only state that lives
     * outside the arena is the top-level index (inline in the HashMap
     * struct). Save copies that index into the arena and writes a small
     * header at arena offset 0 (the sentinel cache line reserved by Init),
     * recording the index location and the allocation high-water mark.
     *
     * Pair with rgArenaCreateShared and rgArenaFlush. Save is an explicit
     * checkpoint: index mutations made by Puts after a Save are not on disk
     * until the next Save. Call it on the final, fully-built map (or at each
     * checkpoint), then rgArenaFlush / rgArenaDestroy to flush to disk.
     *
     * Requires the map to have been rgHashMapInit'd on a fresh, dedicated
     * arena (so it owns the offset-0 sentinel). Works on any arena kind, but
     * is only meaningful for a file-backed one.
     *
     * @param[in] _map - Live HashMap to checkpoint.
     * @returns 0 on success, -1 if _map is not live, -3 if the arena had no
     *          room for the index copy.
     */
    int32_t rgHashMapSave(HashMap* _map);

    /* Reconstruct a HashMap over an arena re-mapped with rgArenaOpenShared.
     *
     * Validates the header written by rgHashMapSave (magic / version / index
     * config must match this build), copies the persisted index back into
     * *_map, binds _map to _arena, and rewinds the arena's high-water mark
     * to the saved value so the reopened map can be appended to. After this
     * call _map behaves exactly as the saved map did, at whatever address
     * the file was mapped to.
     *
     * @param[out] _map   - HashMap to populate. Must be non-0.
     * @param[in]  _arena - Arena mapped by rgArenaOpenShared. Must be live.
     * @returns 0 on success, -1 on bad arguments, -6 if the arena does not
     *          contain a compatible persisted HashMap.
     */
    int32_t rgHashMapOpen(HashMap* _map, Arena* _arena);

/*--------------------------------------------------------------------------
 * HashTrie API (concurrent, lock-free hash trie)
 *
 * Same node layout as HashMap. Slot updates use atomic CAS so Get / Put
 * can run from multiple threads. Each Put takes an arena, so a multi-
 * writer setup can give each thread its own arena and still write to a
 * single shared trie -- nodes from different arenas coexist because
 * slots store absolute 64-bit pointers.
 *------------------------------------------------------------------------*/

    /* Clear a HashTrie to the empty state.
     *
     * Equivalent to memset(0); provided for API symmetry with HashMap and
     * to re-empty a trie that has accumulated entries. A zero-initialised
     * HashTrie is already a valid empty trie; calling Init is optional.
     *
     * @param[out] _trie - HashTrie to clear. Must be non-0.
     * @returns 0 on success, -1 if _trie is 0.
     */
    int32_t rgHashTrieInit(HashTrie* _trie);

    /* Clear a HashTrie and set creation flags (see RGM_HASHTRIE_FLAG_*).
     *
     * rgHashTrieInit is exactly rgHashTrieInitEx(_trie, RGM_HASHTRIE_FLAG_NONE).
     * Pass RGM_HASHTRIE_FLAG_NOCOPY to build a trie that references caller-
     * owned keys instead of copying them (read its contract before use).
     *
     * @param[out] _trie  - HashTrie to clear. Must be non-0.
     * @param[in]  _flags - Bitwise OR of RGM_HASHTRIE_FLAG_* values.
     * @returns 0 on success, -1 if _trie is 0.
     */
    int32_t rgHashTrieInitEx(HashTrie* _trie, uint32_t _flags);

    /* Insert or overwrite a key -> value mapping.
     *
     * The writer's arena is passed in explicitly so multiple writer
     * threads can each contribute nodes from their own arena to the
     * same trie. The arena must be live and remain live for the
     * lifetime of every node it allocates here (i.e. for as long as
     * any Get may reach those nodes).
     *
     * Insertion publishes the new node via release-CAS; concurrent
     * Get calls see either the old or new state, never a partially-built
     * node. Overwriting an existing key atomic-stores the new value, so
     * concurrent readers see old-or-new (never torn).
     *
     * Multiple Put threads racing on the same empty slot retry until one
     * wins. Losers' speculatively-allocated nodes and key bytes stay in
     * the loser's arena and become unreachable (no leak in the GC sense,
     * but the arena's high-water mark advances).
     *
     * @param[in] _trie    - Trie to insert into. Must be non-0.
     * @param[in] _arena   - Live arena to allocate the new node from on
     *                       miss. May be ignored on a key-match overwrite,
     *                       but is still required (must not be 0).
     * @returns 0 on success, -1 on bad arguments, -3 if _arena is
     *          exhausted.
     */
    int32_t rgHashTriePut(HashTrie* _trie, Arena* _arena,
                          const void* _key, uint64_t _keyLen, uint64_t _value);

    /* Lookup a value by key, atomically.
     *
     * Get does not allocate; it walks slot pointers directly so it never
     * touches an arena.
     *
     * @returns 0 on hit, -1 on bad arguments or key-not-found.
     */
    int32_t rgHashTrieGet(HashTrie* _trie, const void* _key, uint64_t _keyLen, uint64_t* _outValue);

    /* Insert or overwrite, specialised for 8-byte uint64 keys.
     *
     * Same atomicity contract as rgHashTriePut, with the hash computed
     * inline and the key match a single uint64 ==. ~10-15% faster than
     * the generic Put on uint64-keyed workloads. */
    int32_t rgHashTriePutU64(HashTrie* _trie, Arena* _arena,
                             uint64_t _key, uint64_t _value);

    /* Lookup, specialised for 8-byte uint64 keys. Same atomicity contract
     * as rgHashTrieGet. */
    int32_t rgHashTrieGetU64(HashTrie* _trie, uint64_t _key, uint64_t* _outValue);

    /* Callback type for rgHashTrieForEach. Return 0 to continue, non-zero
     * to stop early. */
    typedef int (*rgHashTrieForEachFn)(const void* _key, uint64_t _keyLen,
                                       uint64_t _value, void* _userData);

    /* Visit every entry in the trie exactly once. Iteration is "snapshot-
     * like": entries published before the call started are guaranteed to
     * be visited; entries inserted during the iteration may or may not be
     * visited; values overwritten concurrently are seen as old-or-new
     * (never torn) per the standard HashTrie value-update contract.
     *
     * Traversal uses a fixed 64-frame stack; chains deeper than 64 levels
     * (engineered-collision territory) fall back to plain recursion so no
     * entry is ever silently dropped (debug builds also trap for visibility).
     *
     * @returns Number of entries visited.
     */
    uint64_t rgHashTrieForEach(HashTrie* _trie, rgHashTrieForEachFn _fn, void* _userData);

    /* Bulk insert / overwrite. Each entry uses the writer's arena _arena
     * (same single-writer-per-arena rule as rgHashTriePut). Returns the
     * first error encountered (e.g. arena exhausted), in which case
     * earlier entries were inserted. Returns OK on full success. */
    int32_t rgHashTriePutBatchU64(HashTrie* _trie, Arena* _arena,
                                  const uint64_t* _keys, const uint64_t* _values,
                                  uint32_t _count);

    /* Bulk concurrent lookup with software-pipelined walks. Same semantics
     * as rgHashMapGetBatchU64 plus the standard HashTrie atomicity
     * contract (atomic-acquire-load on each slot; values are old-or-new). */
    int32_t rgHashTrieGetBatchU64(HashTrie* _trie,
                                  const uint64_t* _keys, uint64_t* _outValues,
                                  int* _outFound, uint32_t _count);

/*--------------------------------------------------------------------------
 * HashIndex API (concurrent, lock-free, hash-keyed)
 *
 * Stores per-entry { hash1, hash2, value } but NOT the original key bytes.
 * Two statistically independent 64-bit hashes (wyhash with two different
 * seeds) give ~N^2 / 2^128 false match probability -- ~3e-26 for 1 M
 * entries, ~3e-20 for 1 B. Effectively zero for any non-adversarial input.
 *
 * Use when keys are long byte buffers (paths, strings, blobs) and you
 * don't need to recover the original key on iteration. Memory per entry
 * is constant (one cache line) regardless of key length, vs HashTrie's
 * keyLen-proportional cost.
 *
 * Concurrency model identical to HashTrie: lock-free Get/Put using strong
 * CAS with release/acquire ordering, multi-writer via one Arena per
 * writer thread. Single-threaded usage is fine -- atomic ops are free on
 * x86/x64 outside contention.
 *
 * NOT a drop-in replacement for HashMap/HashTrie:
 *   - Iteration cannot recover the original key (callback receives the
 *     two hashes).
 *   - Both digests come from wyhash with different seeds -- statistically
 *     independent for non-adversarial inputs, but an attacker who can
 *     defeat wyhash defeats both at once. Layer SipHash (or similar) on
 *     top if your input is attacker-controlled.
 *------------------------------------------------------------------------*/

    /* Hash-keyed concurrent lookup table.
     *
     * Zero-initialised HashIndex is a valid empty index. rgHashIndexInit
     * exists for symmetry with HashTrie and to clear an in-use index back
     * to empty. The lifetime of every arena that contributed a node must
     * extend past the last Get on the index.
     */
    typedef struct HashIndex
    {
#if RG_HASH_USE_TOP_INDEX
        uint64_t      m_top[RG_HASH_TOP_SIZE];  /* direct-mapped top-level roots, 0 = empty. */
#else
        uint64_t      m_root;                   /* absolute pointer to root node, 0 = empty. */
#endif

    } HashIndex;

    /* Clear a HashIndex to the empty state. */
    int32_t rgHashIndexInit(HashIndex* _idx);

    /* Insert or overwrite (byte-key API).
     *
     * Library computes both hashes from the key bytes. The byte buffer
     * itself is not retained -- after Put returns the caller can free /
     * reuse it. */
    int32_t rgHashIndexPut(HashIndex* _idx, Arena* _arena,
                           const void* _key, uint64_t _keyLen, uint64_t _value);

    /* Lookup by bytes. Same hashing as Put. */
    int32_t rgHashIndexGet(HashIndex* _idx,
                           const void* _key, uint64_t _keyLen, uint64_t* _outValue);

    /* Insert or overwrite (pre-hashed API).
     *
     * Use when the caller has already computed two independent hashes of
     * the underlying identifier -- e.g. a UUID, two prefixes of a SHA, or
     * any hash pair the caller knows are independent. Caller is
     * responsible for hash quality: if _h1 and _h2 are correlated, the
     * effective collision space collapses toward 64-bit. */
    int32_t rgHashIndexPutH(HashIndex* _idx, Arena* _arena,
                            uint64_t _h1, uint64_t _h2, uint64_t _value);

    /* Lookup by pre-computed hash pair. */
    int32_t rgHashIndexGetH(HashIndex* _idx,
                            uint64_t _h1, uint64_t _h2, uint64_t* _outValue);

    /* Bulk insert (pre-hashed). Stops on first error and returns it. */
    int32_t rgHashIndexPutBatchH(HashIndex* _idx, Arena* _arena,
                                 const uint64_t* _h1s, const uint64_t* _h2s,
                                 const uint64_t* _values, uint32_t _count);

    /* Bulk lookup (pre-hashed) with software-pipelined trie walks. Same
     * pipelining and outFound semantics as rgHashTrieGetBatchU64. */
    int32_t rgHashIndexGetBatchH(HashIndex* _idx,
                                 const uint64_t* _h1s, const uint64_t* _h2s,
                                 uint64_t* _outValues, int* _outFound,
                                 uint32_t _count);

    /* Callback type for rgHashIndexForEach. Receives the two hashes that
     * identify the entry plus its value. Return 0 to continue, non-zero
     * to stop. The original key bytes are not available -- this is the
     * trade-off for skipping key storage. */
    typedef int (*rgHashIndexForEachFn)(uint64_t _h1, uint64_t _h2,
                                        uint64_t _value, void* _userData);

    /* Visit every entry. Same snapshot-like semantics as
     * rgHashTrieForEach: entries published before the call are visited,
     * concurrent inserts may be missed, value updates are old-or-new.
     * Same fixed 64-frame traversal stack as rgHashMapForEach (on depth
     * overflow: recursive fallback so entries are never dropped; debug trap). */
    uint64_t rgHashIndexForEach(HashIndex* _idx, rgHashIndexForEachFn _fn,
                                void* _userData);

/*--------------------------------------------------------------------------
 * BloomFilter API (lock-free probabilistic set membership)
 *
 * A bit-array-backed approximate set: Test returns "definitely not in set"
 * (0) or "possibly in set" (1). No deletion. Sized by bit count and hash
 * count (k); the library rounds the bit count up to the next power of two
 * for fast `h & mask` indexing.
 *
 * Sizing guidance (n = expected items, p = target false-positive rate):
 *   bits / item   k       FP rate
 *   ----------    ----    --------
 *       4           3       14.7%
 *       8           5        2.2%
 *      10           7        0.82%
 *      12           8        0.31%
 *      16          11        0.05%
 *
 * Pick `bits = n * bits_per_item` and `k` from the table above. The
 * library rounds bits up to the next power of two, so the actual fill
 * ratio (and effective FP rate) is at most 2x better than requested.
 *
 * Hashing: BLOCKED double-hashing (Kirsch & Mitzenmacher, 2006, on a
 * cache-line block). The byte-key Add / Test path computes (h1, h2) as the
 * two halves of one 128-bit wyhash multiply; h1 picks the 512-bit block,
 * and the k in-block bit positions are (h2.bits0-8 + i*(h2.bits32-40|1))
 * mod 512 for i in [0, k) - one cache line touched per Add / Test. Callers
 * with pre-hashed identifiers (UUIDs, content hashes) skip the hashing via
 * the H variants; note only h1's block-selection bits and h2's bits 0-8 /
 * 32-40 are consumed, so supply well-mixed values in those positions.
 *------------------------------------------------------------------------*/

    /* Compute the buffer size rgBloomFilterCreateFromMemory would require.
     *
     * Returns the effective backing size (after rounding bits up to the
     * next power of two and the resulting bit array's byte size).
     *
     * @param[in] _bits - Requested bit count (rounded up to a whole
     *                    power-of-two number of 512-bit blocks; minimum
     *                    one block = 512 bits).
     * @returns Required buffer size in bytes, or 0 on overflow / zero input.
     */
    uint64_t rgBloomFilterBufferSize(uint64_t _bits);

    /* Create a Bloom filter with its bit array backed by the given arena.
     *
     * Populates *_bf in place. The bit array is allocated from _arena;
     * the BloomFilter struct itself is caller-owned and needs no explicit
     * destroy. rgArenaClear / rgArenaDestroy reclaim the bit array.
     *
     * The bit count is rounded up to a whole power-of-two number of
     * 512-bit blocks (minimum one block = 512 bits), so the actual fill
     * ratio is at most 2x better than requested.
     *
     * @param[in]  _arena     - Arena to allocate the bit array from.
     * @param[out] _bf        - BloomFilter to initialise. Must be non-0.
     * @param[in]  _bits      - Requested bit count.
     * @param[in]  _hashCount - Number of hash indices per Add / Test (k).
     * @returns 0 on success, or a negative error code:
     *           -1 invalid argument (0 _bf, 0 / uninit arena, zero
     *              _bits, zero _hashCount),
     *           -2 size overflow,
     *           -3 arena exhausted.
     *          On failure *_bf is left unchanged.
     */
    int32_t rgBloomFilterCreate(Arena* _arena, BloomFilter* _bf,
                                uint64_t _bits, uint32_t _hashCount);

    /* Create a Bloom filter using a caller-owned memory buffer.
     *
     * Same semantics as rgBloomFilterCreate but the bit array is supplied
     * by the caller. The buffer must outlive the BloomFilter; nothing in
     * this library frees or otherwise touches _buffer after this call.
     *
     * _buffer must be 64-byte (cache-line) aligned so each 512-bit block
     * occupies exactly one cache line -- this is what preserves the
     * one-line-per-Add/Test guarantee, and it also satisfies the natural
     * alignment the atomic OR / load operations require. Misaligned
     * buffers trigger an assertion in debug builds.
     * _bufferSize must be at least rgBloomFilterBufferSize(_bits).
     *
     * @returns 0 on success, or a negative error code:
     *           -1 invalid argument,
     *           -2 size overflow,
     *           -4 _bufferSize is too small.
     */
    int32_t rgBloomFilterCreateFromMemory(void* _buffer, uint64_t _bufferSize,
                                          BloomFilter* _bf,
                                          uint64_t _bits, uint32_t _hashCount);

    /* Add (byte-key). Computes wyhash + derived h2, sets k bits.
     * No-op on a 0 / uninitialised filter, or when _key is 0 with a
     * non-zero _keyLen. A zero-length key is valid: it hashes as the
     * empty input and occupies its own k bits like any other key. */
    void rgBloomFilterAdd(BloomFilter* _bf, const void* _key, uint64_t _keyLen);

    /* Add (uint64-key specialisation). Inlined hash, no length dispatch. */
    void rgBloomFilterAddU64(BloomFilter* _bf, uint64_t _key);

    /* Add (pre-hashed). Use when the caller already has two independent
     * 64-bit hashes -- e.g. when sharing them with a HashIndex front. */
    void rgBloomFilterAddH(BloomFilter* _bf, uint64_t _h1, uint64_t _h2);

    /* Test (byte-key). Returns 1 if all k bits are set ("possibly in
     * set"), 0 if any bit is missing ("definitely not in set"). Returns
     * 0 for a 0 / uninitialised filter. */
    int rgBloomFilterTest(BloomFilter* _bf, const void* _key, uint64_t _keyLen);

    /* Test (uint64-key specialisation). */
    int rgBloomFilterTestU64(BloomFilter* _bf, uint64_t _key);

    /* Test (pre-hashed). */
    int rgBloomFilterTestH(BloomFilter* _bf, uint64_t _h1, uint64_t _h2);

    /* Zero the bit array in O(m/64). NOT concurrent-safe -- ensure no
     * other thread is touching the filter when Clear is called. */
    void rgBloomFilterClear(BloomFilter* _bf);

    /* Effective bit count after the power-of-two round-up. */
    uint64_t rgBloomFilterBitCount(BloomFilter* _bf);

    /* Hash count (k) passed to Create. */
    uint32_t rgBloomFilterHashCount(BloomFilter* _bf);

    /* Number of bits currently set (population count). Useful for
     * estimating the realised false-positive rate: with `s` bits set
     * out of `m`, the FP rate of a Test is approximately (s/m)^k. */
    uint64_t rgBloomFilterPopCount(BloomFilter* _bf);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* RG_MEMORY_H_HEADER_GUARD */
