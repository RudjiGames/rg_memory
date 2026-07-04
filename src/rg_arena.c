/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Arena allocator.
 *
 * Design notes:
 *   - Each arena reserves a contiguous range of virtual address space up
 *     front (VirtualAlloc MEM_RESERVE on Windows, mmap PROT_NONE on POSIX)
 *     and commits physical pages lazily as the bump pointer advances. A
 *     1 GiB reservation therefore costs only address space, not RAM, until
 *     it is touched.
 *   - Allocations carry no per-block header. The arena stores nothing but
 *     a bump pointer; rgArenaPop rewinds it by a caller-supplied size, so
 *     freeing by pointer / reallocing in place are not supported.
 *   - Bulk reclamation happens via rgArenaClear / rgArenaDestroy /
 *     rgArenaRestore.
 *   - The Arena struct is caller-owned (see rg_memory.h). A zero-initialised
 *     Arena is inert: every API call on it is a safe no-op. rgArenaCreate
 *     populates the struct in place; rgArenaDestroy releases the VM and
 *     puts the struct back into the inert state. Use rgArenaSave /
 *     rgArenaRestore for scratch-style temporary scopes.
 *   - Single-threaded by convention. Callers needing cross-thread sharing
 *     must serialize their own access.
 */

#include "../include/rg_memory/rg_memory.h"
#include "rg_memory_platform.h"

/* -------------------------------------------------------------------------
 * Small inline helpers (no CRT calls).
 * ------------------------------------------------------------------------- */

/* Round _value up to a multiple of _align (which must be a power of two). */
static uint64_t rgm_align_up(uint64_t _value, uint64_t _align)
{
    return (_value + (_align - 1)) & ~(_align - 1);
}

/* Returns the smallest non-negative padding such that (_addr + padding) % _align == 0.
 * _align must be a power of two. */
static uint64_t rgm_align_padding(uintptr_t _addr, uint64_t _align)
{
    return (uint64_t)((~_addr + 1) & (_align - 1));
}

/* -------------------------------------------------------------------------
 * Virtual memory primitives.
 * ------------------------------------------------------------------------- */

static uint64_t s_pageSize = 0;

/* Query the OS page size; cached after the first call. */
static uint64_t rgm_page_size(void)
{
    if (s_pageSize == 0)
    {
#if RGM_PLATFORM_WINDOWS
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        s_pageSize = (uint64_t)si.dwPageSize;
#else
        /* sysconf(_SC_PAGESIZE) supersedes the XSI-deprecated getpagesize();
         * keep the result in a signed type so a sysconf failure (-1) is
         * caught by the > 0 check below before we cast to unsigned. 4 KiB
         * is the safe floor on every Linux/BSD/Darwin target. */
        long pageSize = sysconf(_SC_PAGESIZE);
        s_pageSize = pageSize > 0 ? (uint64_t)pageSize : 4096;
#endif
    }
    return s_pageSize;
}

/* Result from rgm_vm_reserve: base, size actually reserved (may exceed
 * the request after page round-up), and bytes already committed (non-
 * zero only on the eager-commit huge-pages path on Windows). */
typedef struct rgm_vm_result
{
    void*    base;
    uint64_t reserved;
    uint64_t committed;
} rgm_vm_result;

/* Reserve _bytes of virtual address space without committing any pages.
 *
 * On POSIX, MAP_NORESERVE keeps the reservation out of the kernel's commit
 * accounting under strict overcommit (vm.overcommit_memory=2). Without it
 * a multi-GiB PROT_NONE reservation can be rejected on servers that disable
 * overcommit, even though we won't touch most of those pages.
 *
 * When RGM_ARENA_FLAG_HUGE_PAGES is set the function tries to back the
 * reservation with large / huge pages. The flag is advisory: failure to
 * obtain huge pages (privilege missing, OS support absent, contiguous
 * physical block unavailable) silently degrades to the normal-pages path.
 *
 * The returned struct has .base == 0 on hard failure (no normal-pages
 * fallback succeeded either). */
static rgm_vm_result rgm_vm_reserve(uint64_t _bytes, uint32_t _flags)
{
    rgm_vm_result r;
    r.base      = 0;
    r.reserved  = 0;
    r.committed = 0;

#if RGM_PLATFORM_WINDOWS
    /* Try huge pages first. They require eager commit on Windows -- the
     * reservation is fully physically backed at create time. Falls back
     * silently to the lazy reserve path on failure (no SeLockMemoryPrivilege,
     * no contiguous large-page-aligned block, etc.). */
    if (_flags & RGM_ARENA_FLAG_HUGE_PAGES)
    {
        SIZE_T lp = GetLargePageMinimum();
        if (lp != 0)
        {
            uint64_t hugeBytes = rgm_align_up(_bytes, (uint64_t)lp);
            void*    p = VirtualAlloc(0, (SIZE_T)hugeBytes,
                                      MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                      PAGE_READWRITE);
            if (p != 0)
            {
                r.base      = p;
                r.reserved  = hugeBytes;
                r.committed = hugeBytes;
                return r;
            }
            /* Fall through to normal lazy reserve. */
        }
    }

    {
        void* p = VirtualAlloc(0, (SIZE_T)_bytes, MEM_RESERVE, PAGE_NOACCESS);
        if (p != 0)
        {
            r.base     = p;
            r.reserved = _bytes;
        }
    }
#else
    {
        void* p = mmap(0, (uint64_t)_bytes, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED)
        {
            r.base     = p;
            r.reserved = _bytes;

#   if defined(MADV_HUGEPAGE)
            /* Linux: hint the kernel to back this region with transparent
             * huge pages. Lazy commit semantics are unchanged -- mprotect
             * still drives the actual fault-in -- the kernel just picks
             * 2 MiB pages instead of 4 KiB when defrag allows. macOS lacks
             * MADV_HUGEPAGE and silently falls through with normal pages. */
            if (_flags & RGM_ARENA_FLAG_HUGE_PAGES)
            {
                (void)madvise(p, (uint64_t)_bytes, MADV_HUGEPAGE);
            }
#   else
            (void)_flags;
#   endif
        }
    }
#endif
    return r;
}

/* Commit physical backing for [_base, _base + _bytes). Returns 1 on success. */
static int rgm_vm_commit(void* _base, uint64_t _bytes)
{
#if RGM_PLATFORM_WINDOWS
    return VirtualAlloc(_base, (SIZE_T)_bytes, MEM_COMMIT, PAGE_READWRITE) != 0;
#else
    return mprotect(_base, (uint64_t)_bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

/* Arena backing kind, stored in Arena::m_backing. Picks the teardown path
 * (and disables decommit, which is meaningless for a file view). */
#define RGM_ARENA_BACKING_ANON 0u
#define RGM_ARENA_BACKING_FILE 1u

/* Release the reservation/mapping at _base. Anonymous arenas hand the VM
 * range back to the OS; file-backed arenas unmap the view (the underlying
 * file persists). */
static void rgm_vm_release(void* _base, uint64_t _bytes, uint32_t _backing)
{
#if RGM_PLATFORM_WINDOWS
    (void)_bytes;
    if (_backing == RGM_ARENA_BACKING_FILE)
    {
        UnmapViewOfFile(_base);
    }
    else
    {
        VirtualFree(_base, 0, MEM_RELEASE);
    }
#else
    /* munmap releases both anonymous and file-backed mappings; the file
     * itself is unaffected. */
    (void)_backing;
    munmap(_base, (uint64_t)_bytes);
#endif
}

/* Map a file as a shared, eagerly-backed arena region. Unlike the anonymous
 * reserve/commit path, the whole file is mapped and resident-on-demand from
 * disk, so m_committed == m_reserved == file size and no later commit step
 * runs. The OS file / mapping handles are closed immediately: on both
 * platforms the view stays valid after the handle is closed, so the Arena
 * carries no handle and teardown needs only the base pointer.
 *
 * _openExisting == 0 creates / truncates the file to _bytes; != 0 maps an
 * existing file at its current size (_bytes ignored).
 *
 * _deleteOnClose != 0 (create path only) makes the file a self-cleaning
 * temporary: Windows opens it FILE_FLAG_DELETE_ON_CLOSE (the live view holds it
 * until unmap / process death, then the OS deletes it); POSIX unlink()s the
 * path right after open so the inode is reclaimed when the mapping is released.
 * On Windows, if the create fails WITH the flag (some network filesystems
 * reject it) we retry without it and the file behaves as an ordinary one.
 *
 * Returns .base == 0 on any failure (open, size query, truncate, or map). */
static rgm_vm_result rgm_vm_map_file(const char* _path, uint64_t _bytes,
                                     int _openExisting, int _readOnly,
                                     int _deleteOnClose)
{
    rgm_vm_result r;
    r.base      = 0;
    r.reserved  = 0;
    r.committed = 0;

#if RGM_PLATFORM_WINDOWS
    {
        DWORD  access = _readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
        /* Always permit a concurrent writer (FILE_SHARE_WRITE), even for a read-only map: this lets us open + map a
         * file another process is actively writing (e.g. tailing an UnrealTraceServer store .utrace, or any live
         * capture). Harmless for files we own. The view is the size at map time; a concurrent append-only writer
         * never touches our mapped pages. */
        DWORD  share  = FILE_SHARE_READ | (DWORD)FILE_SHARE_WRITE;
        DWORD  disp   = _openExisting ? OPEN_EXISTING : CREATE_ALWAYS;
        DWORD  attr   = FILE_ATTRIBUTE_NORMAL;
        HANDLE f;
        if (_deleteOnClose && !_openExisting)
        {
            attr  |= FILE_FLAG_DELETE_ON_CLOSE;
            share |= FILE_SHARE_DELETE;	/* allow the delete disposition while mapped */
        }
        f = CreateFileA(_path, access, share, 0, disp, attr, 0);
        if (f == INVALID_HANDLE_VALUE && (attr & FILE_FLAG_DELETE_ON_CLOSE))
        {
            /* Filesystem rejected DELETE_ON_CLOSE -- fall back to a plain file
             * (the caller's explicit unlink path then handles removal). */
            attr  &= ~(DWORD)FILE_FLAG_DELETE_ON_CLOSE;
            share &= ~(DWORD)FILE_SHARE_DELETE;
            f = CreateFileA(_path, access, share, 0, disp, attr, 0);
        }
        if (f == INVALID_HANDLE_VALUE)
        {
            return r;
        }

        uint64_t size = _bytes;
        if (_openExisting)
        {
            LARGE_INTEGER li;
            if (!GetFileSizeEx(f, &li) || li.QuadPart <= 0)
            {
                CloseHandle(f);
                return r;
            }
            size = (uint64_t)li.QuadPart;
        }

        {
            DWORD  prot = _readOnly ? PAGE_READONLY : PAGE_READWRITE;
            /* On create, the max-size args grow the file to `size`; on open,
             * 0/0 maps the whole existing file. */
            HANDLE map = CreateFileMappingA(f, 0, prot,
                            _openExisting ? 0u : (DWORD)(size >> 32),
                            _openExisting ? 0u : (DWORD)(size & 0xffffffffu),
                            0);
            if (map == 0)
            {
                CloseHandle(f);
                return r;
            }
            {
                DWORD viewAccess = _readOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
                void* base = MapViewOfFile(map, viewAccess, 0, 0, (SIZE_T)size);
                /* Both handles can go now -- the view keeps the file mapped. */
                CloseHandle(map);
                CloseHandle(f);
                if (base == 0)
                {
                    return r;
                }
                r.base      = base;
                r.reserved  = size;
                r.committed = size;
            }
        }
    }
#else
    {
        int flags = _readOnly ? O_RDONLY
                              : (O_RDWR | (_openExisting ? 0 : (O_CREAT | O_TRUNC)));
        int fd = open(_path, flags, 0644);
        if (fd < 0)
        {
            return r;
        }

        /* Self-cleaning temp: drop the name now -- the open fd (and the mmap
         * below, which takes its own reference) keep the inode alive until the
         * mapping is released or the process dies, then the OS reclaims it. */
        if (_deleteOnClose && !_openExisting)
        {
            unlink(_path);
        }

        uint64_t size = _bytes;
        if (_openExisting)
        {
            struct stat st;
            if (fstat(fd, &st) != 0 || st.st_size <= 0)
            {
                close(fd);
                return r;
            }
            size = (uint64_t)st.st_size;
        }
        else if (ftruncate(fd, (off_t)size) != 0)
        {
            close(fd);
            return r;
        }

        {
            int   prot = PROT_READ | (_readOnly ? 0 : PROT_WRITE);
            void* base = mmap(0, (uint64_t)size, prot, MAP_SHARED, fd, 0);
            /* mmap keeps its own reference; closing fd does not unmap. */
            close(fd);
            if (base == MAP_FAILED)
            {
                return r;
            }
            r.base      = base;
            r.reserved  = size;
            r.committed = size;
        }
    }
#endif
    return r;
}

/* -------------------------------------------------------------------------
 * Internal helpers.
 * ------------------------------------------------------------------------- */

#define RGM_ARENA_DEFAULT_ALIGN 16

/* Minimum bytes to commit at once, to amortize the syscall cost. Picked at
 * 64 KiB to match Windows' allocation granularity; on POSIX mprotect costs
 * the same regardless of size, so any reasonable value works. */
#define RGM_COMMIT_MIN_CHUNK (64ull * 1024ull)

/* Cap on the geometric commit step. Pure doubling is ideal for small arenas (O(log N)
 * commit syscalls), but on a multi-GB arena the final 2x over-commits up to ~1 GB that is
 * never touched (e.g. a 3 GB op-store column doubles 2->4 GB). Past this cap the arena
 * grows LINEARLY by RGM_COMMIT_MAX_STEP, so committed stays within one step of the high
 * water mark. 64 MB keeps the extra commit syscalls negligible (a few dozen on a 3 GB
 * arena) while bounding wasted commit to <64 MB per arena. */
#define RGM_COMMIT_MAX_STEP  (64ull * 1024ull * 1024ull)

/* An Arena with m_base == 0 is treated as uninitialised: queries report
 * zero, Alloc returns 0, Destroy / Clear do nothing. */
static int rgm_arena_is_live(const Arena* _a)
{
    return _a != 0 && _a->m_base != 0;
}

/* Ensure pages covering [0 .. _required_pos) are committed. Grows the
 * committed region geometrically (at least 2x previous) and never below
 * RGM_COMMIT_MIN_CHUNK, so a stream of small allocations only triggers
 * O(log N) commit syscalls instead of one per page crossed. Returns 1
 * on success. */
static int rgm_arena_ensure_committed(Arena* _a, uint64_t _required_pos)
{
    if (_required_pos <= _a->m_committed)
    {
        return 1;
    }
    if (_required_pos > _a->m_reserved)
    {
        return 0;
    }

    uint64_t target = _a->m_committed * 2;
    if (target > _a->m_committed + RGM_COMMIT_MAX_STEP)	/* cap the doubling on large arenas (see RGM_COMMIT_MAX_STEP) */
    {
        target = _a->m_committed + RGM_COMMIT_MAX_STEP;
    }
    if (target < _required_pos)
    {
        target = _required_pos;
    }
    if (target < RGM_COMMIT_MIN_CHUNK)
    {
        target = RGM_COMMIT_MIN_CHUNK;
    }
    target = rgm_align_up(target, rgm_page_size());
    if (target > _a->m_reserved)
    {
        target = _a->m_reserved;
    }

    uint64_t extra = target - _a->m_committed;
    if (!rgm_vm_commit(_a->m_base + _a->m_committed, extra))
    {
        return 0;
    }
    _a->m_committed = target;
    return 1;
}

/* Core aligned bump-allocation. Advances pos and returns the user pointer.
 * Returns 0 on out-of-memory or invalid args. */
static void* rgm_arena_alloc_internal(Arena* _a, uint64_t _size, uint64_t _align)
{
    RGM_ASSERT(_align != 0 && (_align & (_align - 1)) == 0);

    if (_size == 0)
    {
        return 0;
    }
    if (_align < 8)
    {
        _align = 8;
    }

    /* Align pos up so the returned pointer satisfies _align. */
    uintptr_t user_target = (uintptr_t)_a->m_base + (uintptr_t)_a->m_pos;
    uint64_t  padding     = rgm_align_padding(user_target, _align);

    uint64_t user_off = _a->m_pos + padding;
    uint64_t new_pos  = user_off + _size;

    /* new_pos < user_off catches a uint64_t wrap when _size is pathologically
     * large; without it the wrapped value would compare under _a->m_reserved. */
    if (new_pos < user_off || new_pos > _a->m_reserved)
    {
        return 0;
    }
    if (!rgm_arena_ensure_committed(_a, new_pos))
    {
        return 0;
    }

    _a->m_pos = new_pos;
    return _a->m_base + user_off;
}

/* -------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */

/* Reserve _arenaSize bytes of virtual address space and populate *_arena. */
int32_t rgArenaCreate(Arena* _arena, uint64_t _arenaSize)
{
    return rgArenaCreateEx(_arena, _arenaSize, RGM_ARENA_FLAG_NONE);
}

/* Reserve _arenaSize bytes with caller-supplied flags. See rg_memory.h
 * for the RGM_ARENA_FLAG_* set. Hint-style flags (huge pages) never fail
 * the creation -- they degrade to the normal-pages path silently. */
int32_t rgArenaCreateEx(Arena* _arena, uint64_t _arenaSize, uint32_t _flags)
{
    if (_arena == 0 || _arenaSize == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t      page    = rgm_page_size();
    uint64_t      request = rgm_align_up(_arenaSize, page);
    rgm_vm_result r       = rgm_vm_reserve(request, _flags);
    if (r.base == 0)
    {
        return RGM_ERROR_ERR_NO_MEMORY;
    }

    _arena->m_base      = (uint8_t*)r.base;
    _arena->m_reserved  = r.reserved;
    _arena->m_committed = r.committed; /* non-zero on the eager-commit huge-pages path. */
    _arena->m_pos       = 0;
    _arena->m_backing   = RGM_ARENA_BACKING_ANON;
    return RGM_ERROR_OK;
}

/* Create an arena backed by a file mapping (shared memory / persistence).
 *
 * The whole file is created at _arenaSize bytes and mapped MAP_SHARED, so
 * allocations land directly in the file and survive process exit. Combined
 * with the offset-based HashMap (rgHashMapSave / rgHashMapOpen) this gives a
 * persistent, position-independent on-disk hash map. See rg_memory.h. */
int32_t rgArenaCreateShared(Arena* _arena, const char* _path, uint64_t _arenaSize)
{
    if (_arena == 0 || _path == 0 || _arenaSize == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t      size = rgm_align_up(_arenaSize, rgm_page_size());
    rgm_vm_result r    = rgm_vm_map_file(_path, size, 0 /*create*/, 0 /*rw*/, 0 /*keep file*/);
    if (r.base == 0)
    {
        return RGM_ERROR_ERR_IO;
    }

    _arena->m_base      = (uint8_t*)r.base;
    _arena->m_reserved  = r.reserved;
    _arena->m_committed = r.committed; /* whole file is backed; no later commit step. */
    _arena->m_pos       = 0;
    _arena->m_backing   = RGM_ARENA_BACKING_FILE;
    return RGM_ERROR_OK;
}

/* Create a shared file-backed arena whose file is an auto-deleting temporary
 * (see the header). Identical to rgArenaCreateShared except for the delete-on-
 * close request, so the file cannot outlive the process. */
int32_t rgArenaCreateSharedTemp(Arena* _arena, const char* _path, uint64_t _arenaSize)
{
    if (_arena == 0 || _path == 0 || _arenaSize == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t      size = rgm_align_up(_arenaSize, rgm_page_size());
    rgm_vm_result r    = rgm_vm_map_file(_path, size, 0 /*create*/, 0 /*rw*/, 1 /*delete on close*/);
    if (r.base == 0)
    {
        return RGM_ERROR_ERR_IO;
    }

    _arena->m_base      = (uint8_t*)r.base;
    _arena->m_reserved  = r.reserved;
    _arena->m_committed = r.committed;
    _arena->m_pos       = 0;
    _arena->m_backing   = RGM_ARENA_BACKING_FILE;
    return RGM_ERROR_OK;
}

/* Map an existing arena file (as written by a prior rgArenaCreateShared
 * session). The arena reports the whole file as used (m_pos == m_reserved);
 * rgHashMapOpen rewinds m_pos to the persisted high-water mark so a reopened
 * map can be appended to. Pass _readOnly != 0 for a read-only mapping. */
int32_t rgArenaOpenShared(Arena* _arena, const char* _path, int _readOnly)
{
    if (_arena == 0 || _path == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    rgm_vm_result r = rgm_vm_map_file(_path, 0, 1 /*open existing*/, _readOnly, 0 /*keep file*/);
    if (r.base == 0)
    {
        return RGM_ERROR_ERR_IO;
    }

    _arena->m_base      = (uint8_t*)r.base;
    _arena->m_reserved  = r.reserved;
    _arena->m_committed = r.committed;
    _arena->m_pos       = r.reserved; /* rgHashMapOpen resets to the stored high-water. */
    _arena->m_backing   = RGM_ARENA_BACKING_FILE;
    return RGM_ERROR_OK;
}

/* Flush a file-backed arena's dirty pages to disk. No-op on anonymous
 * arenas. Destroy also flushes via unmap; this is an explicit checkpoint. */
void rgArenaFlush(Arena* _arena)
{
    if (!rgm_arena_is_live(_arena) || _arena->m_backing != RGM_ARENA_BACKING_FILE)
    {
        return;
    }
#if RGM_PLATFORM_WINDOWS
    (void)FlushViewOfFile(_arena->m_base, (SIZE_T)_arena->m_committed);
#else
    (void)msync(_arena->m_base, (uint64_t)_arena->m_committed, MS_SYNC);
#endif
}

/* Test whether _arena is initialised and holds a live VM reservation. */
int rgArenaIsValid(Arena* _arena)
{
    return rgm_arena_is_live(_arena) ? 1 : 0;
}

/* Release the VM reservation and put *_arena back into the inert state.
 * Idempotent and 0-safe. */
void rgArenaDestroy(Arena* _arena)
{
    if (!rgm_arena_is_live(_arena))
    {
        return;
    }
    rgm_vm_release(_arena->m_base, _arena->m_reserved, _arena->m_backing);
    _arena->m_base      = 0;
    _arena->m_reserved  = 0;
    _arena->m_committed = 0;
    _arena->m_pos       = 0;
    _arena->m_backing   = RGM_ARENA_BACKING_ANON;
}

/* Reset the bump pointer to 0; keeps committed pages hot for reuse. */
void rgArenaClear(Arena* _arena)
{
    if (!rgm_arena_is_live(_arena))
    {
        return;
    }
    _arena->m_pos = 0;
    /* Committed pages are intentionally kept hot for reuse. */
}

/* Default-aligned (16-byte) bump allocation, with an inlined fast path for the common case. */
void* rgArenaAlloc(Arena* restrict _arena, uint64_t _size)
{
    if (!rgm_arena_is_live(_arena) || _size == 0)
    {
        return 0;
    }

    /* Fast path: the common case where the bump fits inside already-committed
     * pages. Avoids the function-call + reserved/commit checks of the cold path.
     * The new_pos >= user_off guard catches uint64_t wrap when _size is close
     * to SIZE_MAX -- without it the wrapped value would slip past the committed
     * check and we'd hand out an aliasing pointer while rewinding m_pos. */
    uint64_t  size     = (uint64_t)_size;
    uintptr_t target   = (uintptr_t)_arena->m_base + (uintptr_t)_arena->m_pos;
    uint64_t  pad      = rgm_align_padding(target, RGM_ARENA_DEFAULT_ALIGN);
    uint64_t  user_off = _arena->m_pos + pad;
    uint64_t  new_pos  = user_off + size;
    if (new_pos >= user_off && new_pos <= _arena->m_committed)
    {
        _arena->m_pos = new_pos;
        return _arena->m_base + user_off;
    }

    /* Slow path: needs more committed pages, hits the reserved cap, or
     * detected the overflow above and needs to bail out cleanly. */
    return rgm_arena_alloc_internal(_arena, size, RGM_ARENA_DEFAULT_ALIGN);
}

/* Bump allocation with a caller-specified power-of-two alignment. */
void* rgArenaAllocAligned(Arena* restrict _arena, uint64_t _size, uint64_t _alignment)
{
    if (!rgm_arena_is_live(_arena) || _size == 0)
    {
        return 0;
    }

    uint64_t align = (uint64_t)_alignment;
    if (align < 8)
    {
        align = 8;
    }
    /* Non-pow2 alignment is a caller bug; let the cold path reject it. */
    if ((align & (align - 1)) == 0)
    {
        uint64_t  size     = (uint64_t)_size;
        uintptr_t target   = (uintptr_t)_arena->m_base + (uintptr_t)_arena->m_pos;
        uint64_t  pad      = rgm_align_padding(target, align);
        uint64_t  user_off = _arena->m_pos + pad;
        uint64_t  new_pos  = user_off + size;
        /* See rgArenaAlloc for the new_pos >= user_off rationale. */
        if (new_pos >= user_off && new_pos <= _arena->m_committed)
        {
            _arena->m_pos = new_pos;
            return _arena->m_base + user_off;
        }
    }

    return rgm_arena_alloc_internal(_arena, (uint64_t)_size, (uint64_t)_alignment);
}

/* Rewind the bump pointer by _size bytes (LIFO; the value is unchecked). */
void rgArenaPop(Arena* _arena, uint64_t _size)
{
    if (!rgm_arena_is_live(_arena))
    {
        return;
    }
    if ((uint64_t)_size > _arena->m_pos)
    {
        RGM_FAIL("rgArenaPop: size exceeds current position");
        return;
    }
    _arena->m_pos -= (uint64_t)_size;
}

/* Like rgArenaPop, but debug-asserts the resulting position is _alignment-aligned. */
void rgArenaPopAligned(Arena* _arena, uint64_t _size, uint64_t _alignment)
{
    if (!rgm_arena_is_live(_arena))
    {
        return;
    }
    if ((uint64_t)_size > _arena->m_pos)
    {
        RGM_FAIL("rgArenaPopAligned: size exceeds current position");
        return;
    }
    _arena->m_pos -= (uint64_t)_size;

    if (_alignment > 1)
    {
        uintptr_t addr = (uintptr_t)_arena->m_base + (uintptr_t)_arena->m_pos;
        (void)addr;
        RGM_ASSERT((addr & ((uintptr_t)_alignment - 1)) == 0
                   && "rgArenaPopAligned: position not aligned to the given alignment");
    }
}

/* Capture the current high-water mark for a later rgArenaRestore.
 * Returns RGM_ARENA_INVALID_SAVE_POINT for a 0 or uninitialised arena so
 * callers can distinguish that failure from an empty arena (which legitimately
 * returns 0). */
ArenaSavePoint rgArenaSave(Arena* _arena)
{
    return rgm_arena_is_live(_arena) ? _arena->m_pos : RGM_ARENA_INVALID_SAVE_POINT;
}

/* Rewind to a previously captured save point.
 * The invalid-save-point sentinel is treated as a silent no-op so callers can
 * pair rgArenaSave / rgArenaRestore without an explicit validity check. */
void rgArenaRestore(Arena* _arena, ArenaSavePoint _save)
{
    if (_save == RGM_ARENA_INVALID_SAVE_POINT)
    {
        return;
    }
    if (!rgm_arena_is_live(_arena))
    {
        return;
    }
    if (_save > _arena->m_pos)
    {
        RGM_FAIL("rgArenaRestore: save point is ahead of current position");
        return;
    }
    _arena->m_pos = _save;
}

/* Current high-water mark in bytes, including any alignment padding. */
uint64_t rgArenaUsed(Arena* _arena)
{
    return rgm_arena_is_live(_arena) ? _arena->m_pos : 0;
}

/* Total reserved bytes (may exceed the create-time request after page round-up). */
uint64_t rgArenaCapacity(Arena* _arena)
{
    return rgm_arena_is_live(_arena) ? _arena->m_reserved : 0;
}

/* Shared shrink core: decommit pages above the high-water mark plus _slackBytes
 * of retained slack. rgArenaShrink is exactly _slackBytes == 0, so its behaviour
 * is byte-for-byte unchanged. */
static void rgm_arena_shrink_keep(Arena* _arena, uint64_t _slackBytes)
{
    if (!rgm_arena_is_live(_arena))
    {
        return;
    }
    /* A file view is fully backed by the file; partial decommit does not
     * apply and would corrupt the mapping. Leave it intact. */
    if (_arena->m_backing == RGM_ARENA_BACKING_FILE)
    {
        return;
    }

    uint64_t page = rgm_page_size();
    /* Retain the high-water mark plus the requested slack, page-aligned. The two
     * overflow guards let a caller pass an arbitrarily large slack (up to
     * SIZE_MAX) to mean "keep everything" without wrapping into an over-decommit. */
    uint64_t keep = _arena->m_pos;
    if (_slackBytes > (uint64_t)-1 - keep)
    {
        return;                     /* keep + slack would overflow -> retain all */
    }
    uint64_t newCommitted = rgm_align_up(keep + _slackBytes, page);
    if (newCommitted < keep)
    {
        return;                     /* align_up wrapped near the top -> retain all */
    }
    if (newCommitted >= _arena->m_committed)
    {
        return; /* nothing above the retained window is committed. */
    }

    uint8_t* decommitBase = _arena->m_base + newCommitted;
    uint64_t decommitLen  = _arena->m_committed - newCommitted;

#if RGM_PLATFORM_WINDOWS
    /* MEM_DECOMMIT returns pages to the reservation pool. Fails on
     * arenas backed by eager-commit huge pages (cannot be partially
     * decommitted); in that case we just leave the commitment as-is. */
    if (!VirtualFree(decommitBase, (SIZE_T)decommitLen, MEM_DECOMMIT))
    {
        return;
    }
#else
    /* madvise(DONTNEED) hints the kernel to discard the physical pages
     * immediately (Linux) or lazily (macOS). mprotect back to PROT_NONE
     * matches the initial reserve state -- any stray access traps
     * instead of silently faulting in zeroed pages. */
    (void)madvise(decommitBase, (uint64_t)decommitLen, MADV_DONTNEED);
    if (mprotect(decommitBase, (uint64_t)decommitLen, PROT_NONE) != 0)
    {
        return;
    }
#endif

    _arena->m_committed = newCommitted;
}

/* Decommit pages above the high-water mark. */
void rgArenaShrink(Arena* _arena)
{
    rgm_arena_shrink_keep(_arena, 0);
}

/* Decommit pages above the high-water mark, retaining _slackBytes of committed
 * slack as hysteresis for oscillating / streaming workloads. */
void rgArenaShrinkKeep(Arena* _arena, uint64_t _slackBytes)
{
    rgm_arena_shrink_keep(_arena, _slackBytes);
}

void rgArenaDecommitFront(Arena* _arena, uint64_t _upToOffset)
{
    if (!rgm_arena_is_live(_arena))
    {
        return;
    }
    /* File views are fully backed by the file; partial decommit would corrupt the mapping. */
    if (_arena->m_backing == RGM_ARENA_BACKING_FILE)
    {
        return;
    }

    uint64_t page = rgm_page_size();
    /* Only release pages that are *entirely* below _upToOffset (round the boundary DOWN). */
    uint64_t end  = _upToOffset & ~(page - 1);
    if (end > _arena->m_committed)
    {
        end = _arena->m_committed & ~(page - 1);
    }
    if (end == 0)
    {
        return; /* nothing fully consumed yet */
    }

    /* Releases [m_base, m_base + end). This deliberately leaves a decommitted hole at the front and
     * does NOT maintain the contiguous-commit invariant: after a front-decommit the arena is valid
     * only to keep *reading past* _upToOffset until rgArenaDestroy (which releases the whole
     * reservation regardless of commit state). Intended for streaming filter-compaction, where the
     * source is consumed front-to-back and torn down once done - keeping peak commit ~= one arena. */
#if RGM_PLATFORM_WINDOWS
    /* MEM_DECOMMIT already makes the front pages inaccessible (a decommitted page traps on access). */
    (void)VirtualFree(_arena->m_base, (SIZE_T)end, MEM_DECOMMIT);
#else
    /* madvise(DONTNEED) only discards the physical pages; the mapping stays readable/writable and a stray
     * access would silently refault zeroed pages. mprotect(PROT_NONE) makes the hole actually trap, matching
     * the "valid only past _upToOffset" contract (and mirroring rgArenaShrink's decommit). */
    (void)madvise(_arena->m_base, (uint64_t)end, MADV_DONTNEED);
    (void)mprotect(_arena->m_base, (uint64_t)end, PROT_NONE);
#endif
}
