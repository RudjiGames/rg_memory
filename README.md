# rg_memory

[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue.svg)](LICENSE)
![Language: C](https://img.shields.io/badge/Language-C-orange.svg)
[![Platforms](https://img.shields.io/badge/Platforms-5-green.svg)](#supported-platforms)

**rg_memory** is a minimal, freestanding memory management library written in C. It provides a virtual-memory-backed bump arena, a fixed-block free list, a dense-packed pool, and arena-backed hash structures (single-threaded `HashMap` and concurrent lock-free `HashTrie`), and serves as part of the Imagus Engine and other [Rudji Games](https://github.com/RudjiGames) projects.

It is designed to drop cleanly into any codebase that wants explicit, allocator-shaped memory ownership without dragging in `malloc` or any other C runtime allocator.

---

## Highlights

- **Zero CRT dependencies** — no `libc`, no `malloc`/`free`, no surprises.
- **Caller-owned structs** — `Arena`, `FreeList`, `DenseList` are all plain structs the caller places on the stack, in `.data`, or wherever they like. A zero-initialised value is inert and safe to operate on. No internal pools, no handles, no slot exhaustion.
- **Virtual-memory backed arenas** — reserve address space up front, commit pages lazily as the bump pointer advances. A 1 GiB reservation costs only address space, not RAM, until touched.
- **File-backed / shared-memory arenas** — `rgArenaCreateShared` maps a file instead of anonymous memory, so an arena's contents persist across runs and can be shared between processes. Because `HashMap` stores every link as an arena-relative offset, `rgHashMapSave` / `rgHashMapOpen` give you a **persistent, position-independent on-disk hash map** that re-maps correctly at any address.
- **Optional huge / large pages** — opt-in via `rgArenaCreateEx(..., RGM_ARENA_FLAG_HUGE_PAGES)` for big arenas that get a lot of random access. The library hints the OS and silently falls back to normal pages when the hint isn't honoured.
- **No per-allocation headers** — allocations are raw bumps; `rgArenaPop` rewinds by a caller-supplied size.
- **Save / Restore as scratch** — `rgArenaSave` / `rgArenaRestore` give you LIFO scoped allocations without per-allocation bookkeeping; pairs nest naturally.
- **Fixed-block free list** — arena- or external-memory-backed, eager-init free chain stored in the blocks themselves, O(1) alloc/free with stable pointers.
- **Dense-packed pool** — sibling type using swap-back Free, so live blocks always occupy a contiguous prefix. Trades stable pointers for cache-friendly iteration over particles / draw lists / ECS components.
- **Arena-backed hash structures** — single-threaded `HashMap`, lock-free concurrent `HashTrie`, and key-less concurrent `HashIndex`. All three are 4-ary hash tries with 64-bit absolute pointers in each slot, prefixed by a 32 KiB direct-mapped top-level index (L1-resident) that skips ~6 trie levels per query. Nodes are cache-line aligned so each step of the walk reads exactly one line; each node caches the full hash digest so the walk fast-rejects on a single `uint64 ==` before falling through to the byte-level key compare. Hash function is wyhash (Wang Yi, public domain). `HashTrie` and `HashIndex` support lock-free **multi-writer** Put: each writer thread brings its own arena and the trie stitches per-arena nodes together via absolute pointers.
- **Collision-resilient descent** — when a walk exhausts the digest's bits (a full 64-bit hash collision, whether by chance or an adversarially-crafted wyhash collision), the descent register **reseeds** from a fresh hash of the key and keeps going, so a degenerate key set stays `~log₄(N)` deep instead of collapsing into a linear chain. One never-taken `if` on the hot path; no effect on realistic workloads.
- **Optional NOCOPY keys** — `rgHashTrieInitEx(t, RGM_HASHTRIE_FLAG_NOCOPY)` makes `HashTrie` reference the caller's key bytes instead of copying them inline. For long, stable keys (interned strings, asset paths) the node plus its key reference fit in one cache line regardless of key length — a memory win when the keys already live elsewhere.
- **Lock-free, cache-line-blocked Bloom filter** — `BloomFilter` partitions its bit array into 512-bit blocks (one cache line each); every Add / Test selects one block from the key's first hash and walks all k bit positions inside that single block. One cache-line load per query regardless of k — a 3-7× latency win over an unblocked Bloom once the filter spills past L1. Add uses release-ordered atomic OR; Test uses acquire-ordered loads. Naturally lock-free because bits only ever flip 0 → 1 (no deletion, no CAS retry, no ABA). Useful as a negative-lookup front for `HashTrie` / `HashIndex` — skip the walk entirely when Test returns 0.
- **Header-light public API** — one umbrella include: `<rg_memory/rg_memory.h>`.
- **Unit tested** — every module ships with a self-contained test.

---

## Supported Platforms

| Family  | Platforms              |
| ------- | ---------------------- |
| Desktop | Windows, Linux, macOS  |
| Mobile  | iOS, Android           |

### Compilers

MSVC · GCC · Clang

### CPU architectures

x86 / x64 · ARM / ARM64

The arena uses `VirtualAlloc` on Windows and `mmap` + `mprotect` on POSIX; any platform with one of those backends is straightforward to add.

---

## Modules

Everything is exposed via the umbrella header `<rg_memory/rg_memory.h>`.

| Source              | Purpose                                                          |
| ------------------- | ---------------------------------------------------------------- |
| `rg_arena.c`        | Virtual-memory-backed bump arena with save / restore             |
| `rg_free_list.c`    | Fixed-block free list (stable pointers), arena or external mem   |
| `rg_dense_list.c`   | Dense-packed pool (swap-back Free), arena or external mem        |
| `rg_hash_map.c`     | Single-threaded 4-ary hash trie + top-level index                |
| `rg_hash_trie.c`    | Concurrent lock-free hash trie (same layout, atomic CAS slots)   |
| `rg_hash_index.c`   | Concurrent hash-keyed index (stores only hash digests, not keys) |
| `rg_bloom_filter.c` | Lock-free Bloom filter (atomic-OR Add, atomic-load Test)         |

### Arena API

`Arena` is caller-owned: place the struct wherever you like, pass `&arena` into every call. `rgArenaCreate` populates the struct in place and returns 0 on success or a negative error code on failure. A zero-initialised `Arena` is inert — every API call on it is a safe no-op.

```c
int32_t        rgArenaCreate(Arena* a, uint64_t size);
int32_t        rgArenaCreateEx(Arena* a, uint64_t size, uint32_t flags);
int32_t        rgArenaCreateShared(Arena* a, const char* path, uint64_t size);
int32_t        rgArenaOpenShared(Arena* a, const char* path, int readOnly);
void           rgArenaFlush(Arena* a);
void           rgArenaDestroy(Arena* a);
void           rgArenaClear(Arena* a);
int            rgArenaIsValid(Arena* a);

void*          rgArenaAlloc(Arena* a, size_t size);
void*          rgArenaAllocAligned(Arena* a, size_t size, size_t align);
void           rgArenaPop(Arena* a, size_t size);
void           rgArenaPopAligned(Arena* a, size_t size, size_t align);

ArenaSavePoint rgArenaSave(Arena* a);
void           rgArenaRestore(Arena* a, ArenaSavePoint sp);

uint64_t       rgArenaUsed(Arena* a);
uint64_t       rgArenaCapacity(Arena* a);
```

`rgArenaCreateEx` accepts a bitwise OR of `RGM_ARENA_FLAG_*` values:

| Flag                        | Effect                                                                                                       |
| --------------------------- | ------------------------------------------------------------------------------------------------------------ |
| `RGM_ARENA_FLAG_NONE`       | Same as `rgArenaCreate`.                                                                                     |
| `RGM_ARENA_FLAG_HUGE_PAGES` | Hint the OS to back the reservation with huge / large pages. Silently degrades to normal pages on failure.   |

The huge-pages hint is most useful for large arenas (tens of MiB and up) under random-access workloads like the hash trie, where TLB pressure is real. Platform behaviour:

- **Linux / Android / *BSD**: `mmap` + `madvise(MADV_HUGEPAGE)`. The kernel decides whether to actually use 2 MiB pages; lazy commit is unchanged.
- **macOS**: no effect (no portable `MADV_HUGEPAGE`); the arena uses normal pages.
- **Windows**: `VirtualAlloc` with `MEM_LARGE_PAGES`. Requires `SeLockMemoryPrivilege` ("Lock pages in memory" under Local Security Policy). Large pages on Windows must be committed eagerly, so when the privilege is granted the entire reservation is physically backed at create time. Without the privilege the call silently falls back to lazy normal pages.

### Free list API

`FreeList` is caller-owned; both Create variants populate the struct in place and return 0 on success or a negative error code on failure. There is no matching `Destroy` — the buffer is tied to the host arena (or to the caller's memory), and a zero-initialised `FreeList` is treated as uninitialised.

```c
int32_t  rgFreeListCreate(Arena* a, FreeList* out,
                          size_t blockSize, uint32_t maxBlocks);
int32_t  rgFreeListCreateFromMemory(void* buf, size_t bufSize, FreeList* out,
                                    size_t blockSize, uint32_t maxBlocks);
size_t   rgFreeListBufferSize(size_t blockSize, uint32_t maxBlocks);

void*    rgFreeListAlloc(FreeList* fl);
void     rgFreeListFree(FreeList* fl, void* ptr);
int      rgFreeListCheckPtr(FreeList* fl, void* ptr);

uint32_t rgFreeListMaxBlocks(FreeList* fl);
uint32_t rgFreeListBlockSize(FreeList* fl);
uint32_t rgFreeListBlocksFree(FreeList* fl);
```

### Dense list API

Same caller-owned, return-code-on-Create story as the free list. Live blocks always occupy `[0, count)`; `rgDenseListFree(ptr)` overwrites `ptr`'s slot with the contents of the last live block — pointers returned by `rgDenseListAlloc` are invalidated by any subsequent `Free`.

```c
int32_t  rgDenseListCreate(Arena* a, DenseList* out,
                           size_t blockSize, uint32_t maxBlocks);
int32_t  rgDenseListCreateFromMemory(void* buf, size_t bufSize, DenseList* out,
                                     size_t blockSize, uint32_t maxBlocks);
size_t   rgDenseListBufferSize(size_t blockSize, uint32_t maxBlocks);
void     rgDenseListClear(DenseList* dl);

void*    rgDenseListAlloc(DenseList* dl);
void     rgDenseListFree(DenseList* dl, void* ptr);   /* swap-back */

void*    rgDenseListData(DenseList* dl);
void*    rgDenseListAt(DenseList* dl, uint32_t index);
uint32_t rgDenseListIndexOf(DenseList* dl, void* ptr);

uint32_t rgDenseListMaxBlocks(DenseList* dl);
uint32_t rgDenseListBlockSize(DenseList* dl);
uint32_t rgDenseListCount(DenseList* dl);
int      rgDenseListCheckPtr(DenseList* dl, void* ptr);
```

### Hash map API (single-threaded)

`HashMap` is a 4-ary hash trie keyed by arbitrary byte sequences, holding 64-bit values. Inter-node references are 32-bit offsets into the host arena (HashMap is single-arena, so a 4-byte slot is enough). Caps the host arena at 4 GiB; halves the size of every slot vs the 64-bit-pointer scheme used by `HashTrie` / `HashIndex` — the top-level index drops from 32 KiB to 16 KiB and each node's 4 children drop from 32 B to 16 B. Init burns the first cache line of a fresh arena as a sentinel so offset 0 reliably means "empty slot" everywhere.

`Put` is an upsert: it returns a pointer to the entry's value slot (`NULL` on bad arguments or arena exhaustion) and the caller writes the value through it. A fresh insert's value is zero-initialised; an existing key's value is returned untouched. `Get` returns a pointer to the value (`NULL` on miss). The returned pointer is an absolute pointer into the host arena — only the trie's inter-node child references are kept as 32-bit offsets to keep nodes small — and stays valid until the arena is cleared or destroyed.

```c
int32_t   rgHashMapInit(HashMap* m, Arena* arena);

uint64_t* rgHashMapPut(HashMap* m, const void* key, size_t keyLen);
uint64_t* rgHashMapGet(HashMap* m, const void* key, size_t keyLen);

uint64_t* rgHashMapPutU64(HashMap* m, uint64_t key);
uint64_t* rgHashMapGetU64(HashMap* m, uint64_t key);

/* Persistence: pair with a file-backed arena (rgArenaCreateShared). */
int32_t   rgHashMapSave(HashMap* m);              /* checkpoint the index into the arena */
int32_t   rgHashMapOpen(HashMap* m, Arena* mapped);/* reconstruct over a re-mapped arena  */
```

### Persistence & shared memory

`HashMap` keeps every inter-node link as a 32-bit arena-relative offset, so the node graph is position-independent: map it at any address and it still resolves. The only state that lives outside the arena is the 16 KiB top-level index (inline in the `HashMap` struct). Persisting a map is therefore: build it in a **file-backed arena**, `rgHashMapSave` (which copies the index into the arena behind a small header parked in the offset-0 sentinel and records the high-water mark), flush, and close. Reopen by mapping the file and calling `rgHashMapOpen`, which validates the header, restores the index, and rewinds the arena so the map can still be appended to. The hot `Put`/`Get` paths are untouched — they keep reading the inline index.

```c
/* Build + persist */
Arena a;   rgArenaCreateShared(&a, "names.map", 64u << 20);
HashMap m; rgHashMapInit(&m, &a);
*rgHashMapPut(&m, "alice", 5) = 100;
rgHashMapSave(&m);                 /* checkpoint the index */
rgArenaDestroy(&a);                /* unmap also flushes   */

/* Reopen later (possibly in another process, at a different address) */
Arena b;   rgArenaOpenShared(&b, "names.map", /*readOnly*/ 1);
HashMap n; rgHashMapOpen(&n, &b);
uint64_t* v = rgHashMapGet(&n, "alice", 5);   /* *v == 100 */
rgArenaDestroy(&b);
```

The on-disk image is **build-specific** — host endianness and the `RG_HASH_*` configuration are baked in, and `rgHashMapOpen` rejects a mismatched file (`RGM_ERROR_ERR_FORMAT`). It is not a portable interchange format. `Save` is an explicit checkpoint, like `fsync`: index changes made by `Put`s after a `Save` are not on disk until the next `Save`. `HashTrie` / `HashIndex` use absolute pointers (for multi-arena stitching) and are therefore **not** persistable this way — use `HashMap` when you need on-disk durability.

### Hash trie API (concurrent, lock-free)

Same trie shape and same hash function as `HashMap`, but slots are **absolute 64-bit pointers** — each `rgHashTriePut` takes the writer's arena explicitly, so multi-writer setups give each thread its own arena and nodes from different arenas coexist in the same trie (an offset-based encoding would have nowhere to record which arena a node lives in). Every slot read is an atomic-acquire load and every slot publish is a strong CAS with release ordering, making Get/Put thread-safe.

```c
int32_t  rgHashTrieInit(HashTrie* t);
int32_t  rgHashTrieInitEx(HashTrie* t, uint32_t flags);  /* RGM_HASHTRIE_FLAG_NOCOPY */

int32_t  rgHashTriePut(HashTrie* t, Arena* writerArena,
                       const void* key, size_t keyLen, uint64_t value);
int32_t  rgHashTrieGet(HashTrie* t, const void* key, size_t keyLen, uint64_t* outValue);
```

Pass `RGM_HASHTRIE_FLAG_NOCOPY` to `rgHashTrieInitEx` to reference caller-owned keys instead of copying them inline. The caller then guarantees every key passed to `Put` stays valid and immutable for as long as the trie may read it, and uses **only** the generic byte-key API on that trie (the `U64` / batch specialisations assume inline keys). `ForEach` hands back the caller's original key pointer.

### Bloom filter API (concurrent, lock-free)

Caller-owned like `FreeList` / `DenseList`; Create populates `*_bf` in place and returns 0 on success or a negative error code on failure. The bit array is partitioned into 512-bit (one cache line) blocks; the bit count is rounded up to a whole power-of-two number of such blocks, minimum 1 (= 64 bytes). Every Add / Test selects one block from h1 and walks all k bit positions inside that single block — so a query touches exactly one cache line regardless of k, instead of k lines for an unblocked filter. The external-memory buffer for `CreateFromMemory` must be cache-line (64-byte) aligned. Add is release-ordered atomic OR per bit; Test is acquire-ordered atomic load per bit. No deletion; bits monotonically go 0 → 1 until `Clear` resets the array (Clear is NOT concurrent-safe).

```c
size_t   rgBloomFilterBufferSize(uint64_t bits);

int32_t  rgBloomFilterCreate(Arena* a, BloomFilter* out,
                             uint64_t bits, uint32_t hashCount);
int32_t  rgBloomFilterCreateFromMemory(void* buf, size_t bufSize, BloomFilter* out,
                                       uint64_t bits, uint32_t hashCount);

void     rgBloomFilterAdd    (BloomFilter* bf, const void* key, size_t keyLen);
void     rgBloomFilterAddU64 (BloomFilter* bf, uint64_t key);
void     rgBloomFilterAddH   (BloomFilter* bf, uint64_t h1, uint64_t h2);

int      rgBloomFilterTest    (BloomFilter* bf, const void* key, size_t keyLen);
int      rgBloomFilterTestU64 (BloomFilter* bf, uint64_t key);
int      rgBloomFilterTestH   (BloomFilter* bf, uint64_t h1, uint64_t h2);

void     rgBloomFilterClear    (BloomFilter* bf);
uint64_t rgBloomFilterBitCount (BloomFilter* bf);
uint32_t rgBloomFilterHashCount(BloomFilter* bf);
uint64_t rgBloomFilterPopCount (BloomFilter* bf);   /* bits set; for fill ratio */
```

Sizing rule of thumb for the blocked filter (n = expected items). Rates here are noticeably higher than for an unblocked Bloom at the same bits / item — the cache-line locality is paid for in extra bits per element to hit the same FP target:

| bits / item | k  | blocked FP rate |
| ----------: | -- | --------------: |
|           4 | 3  |          ~24%   |
|           8 | 5  |          ~4.7%  |
|          10 | 7  |          ~1.7%  |
|          12 | 8  |          ~0.7%  |
|          16 | 11 |          ~0.13% |

To match a classic-Bloom 1% target, allocate roughly 30% more bits / item than the unblocked formulas suggest. So a 1 M-item filter at 13 bits / item with k = 8 wants `bits = 13_000_000` and `hashCount = 8` (the library will round up to 32 768 blocks = 16 777 216 bits = 2 MiB).

**Concurrency contract:**

<!-- markdownlint-disable MD060 -->

| Operation                              | Safe? | Notes |
| -------------------------------------- | :---: | ----- |
| Concurrent `Add` from any threads      |   ✅  | Each bit-set is a release-ordered atomic OR. Two `Add`s racing on the same word just OR their masks into it; bits are monotone (0 → 1) so there's no ABA and no retry. |
| Concurrent `Test` from any threads     |   ✅  | Acquire-ordered atomic load per bit. Wait-free. |
| Concurrent `Add` + `Test`              |   ✅  | Standard publish / consume. Any bit set by an `Add` that returned before a `Test` started is visible to that `Test`. |
| `PopCount` during concurrent `Add`     |   ⚠️  | Plain loads — count is an approximate snapshot, useful for fill-ratio estimation. Never observes a torn value (8-byte loads are atomic on the targets we support); just no global instant. |
| `Clear` during any other operation     |   ❌  | Bulk non-atomic zero. Quiesce all other threads first, or use the filter across well-defined phases (frame boundary, level load, pipeline-phase end). |
| `Init` / `Create` / `CreateFromMemory` |   ❌  | Constructors. Same rule as every other type in this library: complete construction before publishing the struct. |

<!-- markdownlint-enable MD060 -->

If you need `Clear` to run alongside live `Add` / `Test` traffic, that's a different data structure — the natural choices are double-buffered (RCU-style) filters with a generation counter, or per-thread sub-filters that the readers OR together. Both change the API surface and the per-op cost; not in scope for the simple `BloomFilter` here.

---

## Getting the Source

```sh
git clone https://github.com/RudjiGames/rg_memory.git
```

---

## Building

`rg_memory` builds with [GENie](https://github.com/bkaradzic/GENie) project files, driven by the [zidar](https://github.com/RudjiGames/zidar) build system.

Set `RG_ZIDAR_DIR` to your local `zidar` checkout, or place it in a parent directory and let the makefile auto-discover it.

```sh
# Generate project files for all supported toolchains
make projgen

# Available targets
make help

# Remove generated intermediates
make clean
```

This produces:

- **Visual Studio 2022** solutions (Windows)
- **GNU Make** projects (Linux GCC / Clang, Android NDK)
- **Xcode** projects (macOS, iOS)

---

## Using it in your project

### Bump arena

```c
#include <rg_memory/rg_memory.h>

Arena a;
rgArenaCreate(&a, 1ull << 30); /* reserve 1 GiB, costs only VA */

/* Default-aligned (16-byte) allocation. */
char* buf = (char*)rgArenaAlloc(&a, 4096);

/* Aligned allocation; alignment must be a power of two. */
float* verts = (float*)rgArenaAllocAligned(&a, 64 * sizeof(float), 64);

/* Scratch sub-lifetime via Save / Restore: every allocation made in
 * between is dropped in one shot when we rewind. Pairs nest naturally. */
{
    ArenaSavePoint sp = rgArenaSave(&a);
    char* tmp = (char*)rgArenaAlloc(&a, 16 * 1024);
    /* ... use tmp ... */
    rgArenaRestore(&a, sp);
}

rgArenaDestroy(&a);
```

For large, randomly-accessed arenas (hash tables, scene graphs, asset
caches), request huge pages — the library degrades silently when the
OS refuses, so it's safe to ask unconditionally:

```c
Arena big;
rgArenaCreateEx(&big, 256ull << 20, RGM_ARENA_FLAG_HUGE_PAGES);
/* ... same API as before ... */
rgArenaDestroy(&big);
```

### Fixed-block free list

```c
#include <rg_memory/rg_memory.h>

typedef struct Node { int data; } Node;

Arena arena;
rgArenaCreate(&arena, 64 * 1024);

FreeList nodes;
rgFreeListCreate(&arena, &nodes, sizeof(Node), 1024);

Node* a = (Node*)rgFreeListAlloc(&nodes);
Node* b = (Node*)rgFreeListAlloc(&nodes);
rgFreeListFree(&nodes, a);

/* No Destroy: the FreeList struct lives wherever you put it, and the
 * block buffer is reclaimed when the host arena is cleared/destroyed. */
rgArenaDestroy(&arena);
```

Or with caller-owned memory:

```c
static _Alignas(16) uint8_t backing[1024 * 16];

FreeList nodes;
rgFreeListCreateFromMemory(backing, sizeof(backing), &nodes,
                           sizeof(Node), 1024);
```

### Dense-packed pool

Same caller-owned shape as the free list. Use this when you walk the live set more often than you hold references into it.

```c
#include <rg_memory/rg_memory.h>

typedef struct Particle { float x, y, vx, vy, life; } Particle;

Arena arena;
rgArenaCreate(&arena, 64 * 1024);

DenseList particles;
rgDenseListCreate(&arena, &particles, sizeof(Particle), 1024);

/* Spawn -- new block lands at the next dense slot. */
Particle* p = (Particle*)rgDenseListAlloc(&particles);
p->x = p->y = 0.0f; p->vx = 1.0f; p->vy = 0.0f; p->life = 1.0f;

/* Tight iteration over the live range. */
uint8_t* base   = (uint8_t*)rgDenseListData(&particles);
uint32_t count  = rgDenseListCount(&particles);
uint32_t stride = rgDenseListBlockSize(&particles);
for (uint32_t i = 0; i < count; ++i) {
    Particle* x = (Particle*)(base + (size_t)i * stride);
    x->x += x->vx;
    x->y += x->vy;
    x->life -= 0.016f;
}

/* Remove dead particles. Walk backwards so the swapped-in element
 * ends up at an already-visited index. */
for (uint32_t i = rgDenseListCount(&particles); i-- > 0; ) {
    Particle* x = (Particle*)rgDenseListAt(&particles, i);
    if (x->life <= 0.0f) rgDenseListFree(&particles, x);
}

rgArenaDestroy(&arena);
```

### Hash map / hash trie

```c
#include <rg_memory/rg_memory.h>

Arena arena;
rgArenaCreate(&arena, 1 * 1024 * 1024);

HashMap names;
rgHashMapInit(&names, &arena);

*rgHashMapPut(&names, "alice", 5) = 100;
*rgHashMapPut(&names, "bob",   3) = 200;

uint64_t* score = rgHashMapGet(&names, "alice", 5);
if (score != NULL) {
    /* *score == 100 */
}

/* Values can stand in for pointers via uintptr_t casts. */
int target = 42;
*rgHashMapPut(&names, "ptr", 3) = (uint64_t)(uintptr_t)&target;
uint64_t* slot = rgHashMapGet(&names, "ptr", 3);
int* recovered = (int*)(uintptr_t)*slot;
/* recovered == &target */

rgArenaDestroy(&arena);
```

`HashTrie` has the same trie shape and walk, but its Get/Put still take a `uint64_t value` / `uint64_t* outValue` and return a status code rather than a value pointer — returning a bare pointer into a lock-free structure shared across writers would not be safe. Reach for it when you need concurrent Get/Put.

---

## Design Notes

### Arena

- The `Arena` struct is caller-owned; there is no internal pool, no opaque handle. A zero-initialised `Arena` is treated as uninitialised and every API call on it is a safe no-op. Each live `Arena` owns one VM reservation — don't copy or `memcpy` a live struct, since both copies would point at the same reservation and a double-destroy would corrupt the address space.
- The bump pointer carries no per-allocation metadata; freeing is by LIFO `rgArenaPop`, bulk reclaim is via `rgArenaClear` / `rgArenaDestroy` / `rgArenaRestore`.
- The commit policy grows geometrically (at least 2x, floor of 64 KiB) so a stream of small allocations triggers O(log N) commit syscalls rather than one per page boundary. The eager-commit huge-pages path on Windows is the only exception — when `MEM_LARGE_PAGES` succeeds, the full reservation is physically backed at create time and the geometric grow loop becomes a no-op.
- Use `rgArenaSave` / `rgArenaRestore` for scratch-style temporary scopes — pairs nest naturally as long as restores happen in LIFO order. `ArenaSavePoint` is just `uint64_t`; the caller pairs it with the arena it was taken from. `RGM_ARENA_INVALID_SAVE_POINT` is returned by Save on an uninitialised arena, and is a silent no-op on Restore, so Save / Restore pairs survive an invalid arena without an explicit validity check at the call site.

### Free list

- The `FreeList` struct is caller-owned; there is no internal pool, no opaque handle, no matching `Destroy`. A zero-initialised `FreeList` is treated as uninitialised and every API call on it is a safe no-op.
- Block size is clamped up to `sizeof(uint32_t)` (so the in-place free chain can stash next-index links) and rounded up to 16 bytes (so block N inherits 16-byte alignment for every N).
- The free chain is initialised eagerly at create time in a single linear pass over the buffer, so `rgFreeListAlloc` doesn't need a lazy-init branch on the hot path.
- Pointers returned by `rgFreeListAlloc` are stable until the matching `rgFreeListFree` (or until the host arena is cleared / destroyed).
- `rgFreeListBufferSize` exposes the post-rounding size requirement, so callers can size external buffers exactly without duplicating the rules.

### Dense list

- Same caller-owned struct + same arena-or-external buffer story as the free list. Inert when zero-initialised.
- Live blocks always occupy the prefix `[0, m_count)` of the buffer; `rgDenseListAlloc` returns slot `m_count` and bumps the counter, `rgDenseListFree(ptr)` overwrites the freed slot with the contents of the last live block and decrements the counter ("swap-back").
- Trade-off vs the free list: **pointers are not stable** — every `Free` may relocate the data of one other block. Iteration over the live range is correspondingly cheap (one tight contiguous loop, no holes).
- No `sizeof(uint32_t)` clamp on block size since there is no per-block chain link; the only constraint is the 16-byte alignment round-up.
- `rgDenseListClear` resets the count to zero in O(1) without touching the buffer; old contents are overwritten by subsequent allocs.

### Hash structures

- Both `HashMap` and `HashTrie` are 4-ary hash tries (Wellons, [nullprogram 2023-09-30](https://nullprogram.com/blog/2023/09/30/)). The walk consumes 2 hash bits per level via `h >> 62` and shifts `h <<= 2`; depth is `~log4(N)` on average.
- **Collision-resilient descent.** A fixed 64-bit digest yields only ~26 descent levels (after the 12-bit top index) before its bits are exhausted. Without intervention, keys that share a full 64-bit digest — possible by chance (~2⁻⁶⁴ per pair) or by an adversarially-crafted wyhash collision (wyhash is fast and well-distributed but not collision-resistant) — would pile into an unbounded `child[0]` chain, turning lookups O(N). Instead, when the descent register hits zero the walk **reseeds** it from a fresh, round-varying hash (re-hashing the key for `HashMap`/`HashTrie`; remixing the stored `h1`/`h2` for `HashIndex`, which keeps no key bytes) and keeps descending. `Put` and `Get` reseed at identical points, and the node match still tests the *original* digest, so the only thing reseeding changes is which child a step takes — distinct keys that collided in the first digest re-hash differently and diverge, keeping the structure `~log4`-deep. The cost is one branch per level that is never taken at realistic depths (≈10–12 levels at 1–10 M keys) and hides under the walk's dependent-load latency. NetBSD's `thmap` uses the same idea (re-hash with an incremented seed for deeper levels).
- **Slot encoding diverges by container.** `HashTrie` and `HashIndex` keep absolute 64-bit pointers so the multi-writer model can stitch nodes from per-thread arenas together (each pointer self-resolves; no shared base needed). `HashMap` is single-arena, so it switches to **32-bit offsets into the host arena's `m_base`** instead: the top-level index drops from 32 KiB to 16 KiB, each node's 4 children drop from 32 B to 16 B, and the host arena is capped at 4 GiB. HashMap Init burns the first cache line of a fresh arena as a sentinel so offset 0 unambiguously means "empty slot" everywhere.
- **Direct-mapped top-level index (default on).** Each hash structure (`HashMap`, `HashTrie`, `HashIndex`) begins with a flat 4096-slot top array indexed by the top 12 bits of the wyhash digest (16 KiB on HashMap, 32 KiB on the others). The first walk step becomes an L1-resident load that skips ~6 trie levels of L2/L3/DRAM traffic; the 4-ary descent then picks up with the remaining 52 hash bits. Multi-writer Put benefits doubly because writers now scatter across 4096 independent top slots instead of contending on a single root. Toggle via `RG_HASH_USE_TOP_INDEX` in the public header (`0` reverts to the single-`m_root` layout, useful for size-constrained embedded targets or A/B benchmarking).
- HashTrie / HashIndex node layout is 56 bytes (4 × 8-byte child pointers + 8-byte stored hash + 8-byte value + 4-byte key length + 4-byte pad). HashMap's node is 40 bytes for the same field set with 4-byte child offsets. Each node and its key bytes are allocated together in a single arena allocation **aligned to a 64-byte cache line**, so an 8-byte key + node fits inside exactly one line and a traversal step is one line load. The cache-line alignment costs ≤ 8 bytes of padding per small-key node (24 B for HashMap's smaller node) but eliminates the ~50% of nodes that would otherwise straddle two lines — a meaningful win under multi-writer Put where the second line ping-pongs separately.
- Each node caches the full hash in `m_hash`. The walk fast-rejects on `n->m_hash == h` (one `uint64 ==`) before falling through to the keylen + byte-by-byte key compare on hash match. The byte compare remains the source of truth — wyhash is fast and well-distributed but not collision-resistant — so a hash match still verifies the actual key.
- Hash function is **wyhash** (Wang Yi, public domain) — a 64×64→128 multiply mixer that's both faster and better-distributed than FNV-1a, especially in the top bits the trie consumes 2 at a time. Unaligned reads use `__unaligned` on MSVC (compiler keyword, not a libc symbol) and the byte-OR pattern on Clang/GCC (recognised as a single mov on x86/x64).
- `HashMap` is single-threaded. `HashTrie` shares the exact node layout but every slot read is an atomic-acquire load and every slot publish is a strong CAS with release ordering — the canonical publish/consume pattern from the article. Once a slot is set, it never changes (no deletion), which is what eliminates ABA concerns without a tag or generation counter.
- The MSVC atomic-load primitives route through `__iso_volatile_load*` + `_ReadWriteBarrier()` (or `__load_acquire*` on ARM64), **not** through `_InterlockedOr` / `_InterlockedCompareExchange*` with a no-op operand. The latter idiom emits a locked read-modify-write that takes the cache line in M state on every read, so a read-heavy trie walk collapses into cache-line ping-pong across cores. On x86/x64 a properly-aligned mov is already atomic with acquire semantics under TSO, so only a compiler-side ordering barrier is needed. The matching release-store side uses `__iso_volatile_store*` + `_ReadWriteBarrier()` (or `__stlr*` on ARM64) for the same reason — `_InterlockedExchange*` would emit a locked `xchg` that bounces the line through coherence on every value overwrite.
- Values are 64-bit. Concurrent updates use `atomic_store` / `atomic_load`, so readers always see either the old or new value but never a torn read.
- Multi-writer `HashTrie` is supported by giving each writer thread its own arena and passing it to `rgHashTriePut`. The single-threaded `rgArenaAlloc` contract stays satisfied (each arena only ever sees one writer), and the trie's slots stitch the per-arena nodes together via absolute pointers. Lost CAS races leave the loser's speculatively-allocated node / key bytes in the loser's arena (never reachable, no UB, but the arena's high-water advances). The empty-slot Put path uses a **CAS-with-prior** variant that returns the slot's prior value on failure — the lock-free no-overwrite invariant guarantees a follow-up acquire-load would return the same value, so the retry path can skip one round-trip and feed the prior value straight into the match-or-descend path.
- Get is **wait-free**: no CAS, no allocation. Once the trie is built every interior cache line settles into S state across reader cores and stays there, so concurrent Gets scale near-linearly until memory bandwidth runs out.
- **`HashIndex` keeps two 64-bit digests per entry** (`m_h1`, `m_h2`) and never stores the original key bytes. Both digests are wyhash run over the same key with two different seeds (`RGM_HASH_WYP0` for `m_h1`, `RGM_HASH_WYP2` for `m_h2`), giving two statistically independent outputs for non-adversarial input distributions and ~N²/2¹²⁸ false-match probability at the terminal compare. Trade-off: both digests come from the same hash family, so a workload that defeats wyhash defeats both at once — wrap an unrelated MAC (e.g. SipHash) around HashIndex if your inputs are attacker-controlled.

### Threading

The library is single-threaded by convention for the lifecycle and bump-allocator surface. The one exception is `HashTrie` Get/Put, which are lock-free across threads — and because `rgHashTriePut` takes the writer's arena explicitly, multi-writer setups just hand each thread its own arena. No shared allocator, no external lock, no single-writer pattern required. The atomic primitives that the trie uses (acquire-load, release-CAS, sequentially-consistent fetch-add) are wrapped in the private `rg_memory_platform.h` and route through `_Interlocked*` (MSVC) or `__atomic_*` (GCC / Clang) intrinsics — no `<stdatomic.h>` required, so the library stays C99.

---

## Benchmarks

All numbers below are throughput in **millions of operations per second (Mops/sec)**, higher is better. Two decimal places of precision; figures are median of 3 runs.

**Test machine:**

- AMD Ryzen 9 7950X3D — 16 cores / 32 threads, max 4.2 GHz, 128 MiB L3
- 128 GiB DDR5 @ 4800 MT/s (dual-channel; ~76.8 GB/sec theoretical peak, ~60 GB/sec realistic)
- Windows 11 Pro
- MSVC, optimised build (`/O2 /Ob3`-class settings)
- `SeLockMemoryPrivilege` not granted (huge-pages request silently falls back to normal pages)

**Workload:** 8-byte `uint64` unique keys (sequential at insert). Get passes touch every key, once sequentially and once via an LCG-shuffled permutation. The shuffled pass is the cache-cold case. The MT bench partitions the key space across `cpu_count()` threads, each with its own arena; aggregate throughput is `total_keys / walltime` across all writer (or reader) threads.

Each op walks the trie roughly `log₄(N)` levels deep, touching one cache line per level (the HashNode and its inline key sit on one 64-byte line by design). Put on an empty slot additionally allocates one cache line for the new node. The bench prints both Mops/sec and the resulting effective memory bandwidth.

> **What "GB/sec" means here — it is *effective* bandwidth, not DRAM bus bandwidth.** The figure is `bytes-touched-per-op × ops/sec`, i.e. the rate at which the trie walk consumes cache lines from *wherever they currently live in the memory hierarchy*. The overwhelming majority of those lines are served from on-chip cache (the top-level index from L1, the upper subtrie levels from L2/L3), **not** from the DRAM bus. That is precisely why several MT figures below sit well above this DDR5 kit's ~77 GB/sec theoretical peak (and the ~60 GB/sec realistic figure): the numbers are not measuring the memory controller, they are measuring how fast cores can pull cached lines through the trie. A figure exceeding the DRAM ceiling is the *expected* result of good cache utilisation, not a measurement error. Only the cache-cold leaf accesses at 10M (working set ~5× L3) actually touch DRAM in volume.

The reported `B/op` figures use the *full-walk* model: `log₄(N) × 64 B`, plus one extra line on Put for the new node. With the direct-mapped top-level index enabled (the default), the first 6 of those levels collapse into a single L1-resident load, so the actual touched-bytes-per-op is roughly `(log₄(N) − 6) × 64 B + 64 B` — almost half the printed B/op at 1M and about 60% at 10M. The Mops/sec figures are exact; the printed GB/sec is the conservative upper bound implied by the displayed B/op.

### 1 000 000 keys (working set ~64 MiB nodes — fits in this CPU's 128 MiB L3)

| Operation                           | HashMap |   HashTrie | HashTrie MT (32 T) |
| ----------------------------------- | ------: | ---------: | -----------------: |
| Put (Mops/sec)                      |   26.41 |      22.74 |  177.38 (aggregate)|
| Put (GB/sec)                        |   18.60 |      16.01 |             124.87 |
| Get sequential (Mops/sec)           |   23.38 |      26.22 |                 -- |
| Get sequential (GB/sec)             |   14.96 |      16.78 |                 -- |
| Get random (Mops/sec)               |   12.78 |      12.55 |  346.18 (aggregate)|
| Get random (GB/sec)                 |    8.18 |       8.03 |             221.55 |
| Get random, batched K=8 (Mops/sec)  |      -- |      22.08 |  441.31 (aggregate)|
| Get random, batched K=8 (GB/sec)    |      -- |      14.13 |             282.44 |
| Get verify, 1T over MT trie (Mops)  |      -- |         -- |              16.03 |

### 10 000 000 keys (working set ~640 MiB nodes — past L3, hits DRAM)

| Operation                           | HashMap |   HashTrie | HashTrie MT (32 T) |
| ----------------------------------- | ------: | ---------: | -----------------: |
| Put (Mops/sec)                      |   10.43 |       9.14 |  104.65 (aggregate)|
| Put (GB/sec)                        |    8.68 |       7.60 |              87.07 |
| Get sequential (Mops/sec)           |    7.01 |       8.39 |                 -- |
| Get sequential (GB/sec)             |    5.39 |       6.44 |                 -- |
| Get random (Mops/sec)               |    6.20 |       5.50 |  199.49 (aggregate)|
| Get random (GB/sec)                 |    4.76 |       4.22 |             153.21 |
| Get random, batched K=8 (Mops/sec)  |      -- |      13.26 |  270.89 (aggregate)|
| Get random, batched K=8 (GB/sec)    |      -- |      10.18 |             208.05 |
| Get verify, 1T over MT trie (Mops)  |      -- |         -- |               6.32 |

### What the shapes mean

- **Top-level index is the recent structural win.** Single-thread random Get went from ~9 to ~12.5 Mops/sec at 1M (~36% gain), MT Get aggregate from ~291 to ~346 Mops/sec, MT Put aggregate from ~161 to ~177 — all in the same run-shape, no algorithm change beyond replacing the single `m_root` with a 4096-slot array indexed by the top 12 hash bits. Larger gains on Get than on Put because Put's bottleneck is arena allocation, not walk depth.
- **HashTrie single-thread is essentially equivalent to HashMap** across most ops (sequential Get: 26.22 vs 23.38; random Get: 12.55 vs 12.78; Put: 22.74 vs 26.41). On x86/x64 the atomic acquire-loads are plain `mov`s with a compiler-only barrier; the CAS at publish only fires on the rare empty-slot path. HashTrie's atomic plumbing is essentially free in the single-threaded case.
- **MT Put scales ~8×** on 32 threads (177.38 Mops/sec vs 22.74 ST). Better than before the top-level index, since the previous root-slot bottleneck is now spread across 4096 independent top slots — writers for different keys almost never hit the same one.
- **MT Get scales ~13× vs single-thread sequential and ~28× vs cache-cold random** (346.18 Mops/sec vs 26.22 / 12.55 ST). Get is wait-free; the only ceiling is L3 bandwidth. At 222 GB/sec we're at Zen 4's measured L3 read limit.
- **Batched MT Get at 1M (441.31 Mops/sec, 282 GB/sec upper bound) is the absolute peak.** Software-pipelined K=8 walks let each thread keep multiple cache-line loads in flight simultaneously, so 32 threads collectively *exceed* L3 read bandwidth — the measured number includes effective sharing where the top-level array and the first few subtrie levels stay L1/L2-resident across all walkers, and the same cache lines satisfy reads from many lanes at once. **35× over single-thread cache-cold random Get.**
- **10M shows the cache cliff** — working set is ~5× L3 (640 MiB vs 128 MiB), so most leaf-level lines are now DRAM-resident. Per-op throughput drops ~2-3× across the board. But:
  - MT aggregate `Get (random)` at 153 GB/sec **exceeds** DDR5-4800 dual-channel's ~77 GB/sec theoretical peak. The figure is L3 + DRAM combined: the top-level array stays L1-resident across every walker, the first ~3 subtrie levels stay in L3, only the leaves actually hit DRAM.
  - Batched MT Get at 10M (208 GB/sec) is *higher* than serial MT Get at 10M (153 GB/sec) because the pipelined version keeps more memory requests in flight per thread, hiding more per-load DRAM latency. The K=8 lanes also let one outstanding DRAM miss in a thread proceed independently of the others.
- **HashMap and HashTrie are nearly indistinguishable at large N**: 10M random Get is 6.20 vs 5.50 Mops/sec — HashTrie's atomic loads become small noise against DRAM latency. The slight HashMap edge on the random path comes from skipping the atomic compiler barriers on every slot load.
- **Single-thread is latency-bound, not bandwidth-bound.** 8-19 GB/sec at ST is well below DRAM/L3 ceilings — each op is a sequential dependent-load chain through the trie that no amount of bandwidth can speed up. The top-level index trims that chain by ~6 dependent loads, but the residual depth (4-6 levels at 1M, 6-8 at 10M) still serialises on memory latency. Further reductions need fewer levels (we measured 16-ary, regressed due to bigger nodes) or pipelining (which the batched Get does, recovering ~2× ST throughput).

---

## Repository Layout

```text
rg_memory/
├── include/       # Public headers (rg_memory/rg_memory.h)
├── scripts/       # GENie / build scripts
├── src/           # Library implementation
├── tests/         # Per-module unit tests
├── LICENSE
└── makefile
```

---

## Caveats

- **No NULL-value ambiguity.** `HashMap` returns a *pointer* to the value slot (`NULL` only means not-found / failure) and `HashTrie` / `HashIndex` return a status code with the value written through an out-param. Storing a value of `0` is therefore unambiguous — unlike map APIs that return the value directly and cannot distinguish "absent" from "present and zero".
- **No deletion.** The tries only ever publish slots, never clear them — that "a published pointer never changes" invariant is what makes `HashTrie` lock-free without ABA tags or reclamation. Drop a whole map by clearing/destroying its arena; there is no per-key erase.
- **Arena lifetime owns the data.** A `HashTrie` / `HashIndex` holds absolute pointers into the arena(s) that supplied its nodes; every such arena must outlive the last `Get`. Don't `memcpy` or move a live `Arena` struct — both copies would alias one reservation and a double-destroy would corrupt the address space.
- **Multi-writer Put.** Safe when each writer thread brings its own arena (the bump allocator is single-threaded by convention). A lost CAS race leaves the loser's speculatively-allocated node in the loser's arena, unreachable — no leak in the GC sense, but the arena's high-water advances; bounded by the retry rate.
- **NOCOPY keys.** A `RGM_HASHTRIE_FLAG_NOCOPY` trie references caller-owned key memory: the caller must keep every key valid and immutable for the node's lifetime, and must use only the generic byte-key API on that trie.
- **Persistence is build-specific.** `rgHashMapSave` images bake in host endianness and the `RG_HASH_*` configuration; `rgHashMapOpen` rejects a mismatch but the format is not a portable interchange format. `Save` is an explicit checkpoint (changes after it are not on disk until the next `Save`). Only `HashMap` is persistable; `HashTrie` / `HashIndex` use absolute pointers and are not.
- **Adversarial keys.** wyhash is fast and well-distributed but not collision-resistant. The descent reseed keeps even a full-digest-collision key set `~log4`-deep, and `HashMap` / `HashTrie` still verify the actual key bytes on a hash match — but `HashIndex` stores only digests, so for attacker-controlled input wrap an unrelated MAC (e.g. SipHash) around it.

---

## License

`rg_memory` is released under the **BSD 2-Clause License**. See [LICENSE](LICENSE) for full text.

Copyright © 2025-2026 Milos Tosic, Rudji Games.
