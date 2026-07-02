/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Private header: platform / compiler detection and the small shared
 * primitives every .c in this library wants (debug break, soft assert,
 * cache-line alignment). Not part of the public API -- never include
 * from a header in include/.
 */

#ifndef RG_MEMORY_PLATFORM_H_HEADER_GUARD
#define RG_MEMORY_PLATFORM_H_HEADER_GUARD

#include <stdint.h> /* int32_t / int64_t for atomic primitives below */

/* -------------------------------------------------------------------------
 * Platform detection
 *
 * Exactly one of RGM_PLATFORM_WINDOWS / RGM_PLATFORM_POSIX is set to 1;
 * the other is 0. Unsupported platforms fail to compile via #error.
 * ------------------------------------------------------------------------- */

#define RGM_PLATFORM_WINDOWS        0
#define RGM_PLATFORM_POSIX          0

#if defined(_WIN32) || defined(_WIN64) || defined(__WINDOWS__)
#   if !defined(WINAPI_FAMILY) || (WINAPI_FAMILY == WINAPI_FAMILY_DESKTOP_APP)
#       undef  RGM_PLATFORM_WINDOWS
#       define RGM_PLATFORM_WINDOWS     1
#   endif
#elif   defined(__ANDROID__) || defined(__linux__) || defined(linux) || defined(__APPLE__)
#   undef   RGM_PLATFORM_POSIX
#   define RGM_PLATFORM_POSIX           1
#else
#   error "Platform not supported!"
#endif

/* -------------------------------------------------------------------------
 * Platform headers and defines
 * ------------------------------------------------------------------------- */
#if RGM_PLATFORM_WINDOWS
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   if defined(_MSC_VER)
#       include <intrin.h>
#   endif
#elif RGM_PLATFORM_POSIX
#   include <sys/mman.h>
#   include <sys/stat.h>
#   include <unistd.h>
#   include <fcntl.h>
#   if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#       define MAP_ANONYMOUS MAP_ANON
#   endif
    /* MAP_NORESERVE is Linux's way of saying "don't pre-charge swap for this
     * mapping". macOS / *BSD either define it as a no-op or omit it entirely;
     * fall back to 0 so the bitwise-OR in rg_arena.c stays portable. */
#   if !defined(MAP_NORESERVE)
#       define MAP_NORESERVE 0
#   endif
#else
#   error "Platform not supported!"
#endif


/* Catches the Windows + non-DESKTOP WINAPI_FAMILY case (e.g. UWP / WinRT):
 * the outer block matched Windows but the inner family check rejected it,
 * leaving both platform macros at 0. Without this guard the failure would
 * only surface later from arena.c's platform-include block. */
#if !RGM_PLATFORM_WINDOWS && !RGM_PLATFORM_POSIX
#   error "Platform not supported (no platform macro selected)!"
#endif

/* -------------------------------------------------------------------------
 * Debug break / soft assert
 *
 * RGM_ASSERT(cond) traps in debug builds when cond is false and is a
 * no-op in release. Use for caller-bug detection on hot paths where a
 * real runtime check would be unwelcome.
 * ------------------------------------------------------------------------- */

#if defined(_DEBUG) || defined(DEBUG)
#   if defined(_MSC_VER)
#       define RGM_DEBUG_BREAK() __debugbreak()
#   elif defined(__GNUC__) || defined(__clang__)
#       define RGM_DEBUG_BREAK() __builtin_trap()
#   else
#       define RGM_DEBUG_BREAK() ((void)0)
#   endif
#   define RGM_ASSERT(_cond) do { if (!(_cond)) { RGM_DEBUG_BREAK(); } } while (0)
    /* RGM_FAIL is the unconditional-failure form. Prefer it over
     * RGM_ASSERT(0 && "msg"): the constant condition trips MSVC's C4127
     * at every call site, this form sidesteps it. The _msg is for
     * source-level documentation only -- the trap doesn't display it. */
#   define RGM_FAIL(_msg) do { (void)(_msg); RGM_DEBUG_BREAK(); } while (0)
#else
#   define RGM_ASSERT(_cond) ((void)0)
#   define RGM_FAIL(_msg)    ((void)0)
#endif

/* -------------------------------------------------------------------------
 * Cache-line alignment
 *
 * Apply RGM_ALIGN_TO_CACHE_LINE to a struct definition or array to keep
 * neighbouring instances on separate cache lines. RGM_CACHE_LINE is the
 * assumed line width in bytes (64 on every platform we currently target).
 * ------------------------------------------------------------------------- */

#define RGM_CACHE_LINE              64

#if defined(_MSC_VER)
#   define RGM_ALIGN_TO_CACHE_LINE __declspec(align(RGM_CACHE_LINE))
#elif defined(__GNUC__) || defined(__clang__)
#   define RGM_ALIGN_TO_CACHE_LINE __attribute__((aligned(RGM_CACHE_LINE)))
#else
#   define RGM_ALIGN_TO_CACHE_LINE
#endif

/* -------------------------------------------------------------------------
 * Force-inline
 *
 * Use sparingly: only on very small helpers where the call overhead is a
 * significant fraction of the body and the compiler's auto-inline
 * heuristic is too conservative. MSVC's per-function-size threshold is
 * tighter than Clang/GCC's, so this is most useful on MSVC; on the GCC
 * family the always_inline attribute still helps in cases where the
 * inliner has given up because of cost limits.
 * ------------------------------------------------------------------------- */

#if defined(_MSC_VER)
#   define RGM_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#   define RGM_FORCEINLINE inline __attribute__((always_inline))
#else
#   define RGM_FORCEINLINE inline
#endif

/* -------------------------------------------------------------------------
 * Unaligned-access qualifier for the wyhash small-key loads (rg_hash_func.h).
 *
 * __unaligned is an MSVC keyword (compiler extension, not a libc symbol) valid
 * on x64 / ARM64; on x86 it is rejected (C4235) and unaligned loads are allowed
 * anyway, so it is empty there. clang (incl. clang-cl) and GCC take the byte-OR
 * ladder instead and never reference this macro.
 * ------------------------------------------------------------------------- */

#if defined(_MSC_VER) && !defined(__clang__) && !defined(_M_IX86)
#   define RGM_UNALIGNED __unaligned
#else
#   define RGM_UNALIGNED
#endif

/* -------------------------------------------------------------------------
 * Software prefetch
 *
 * Read-side, high temporal locality. Hints the CPU to start a load for the
 * given cache line so its latency overlaps with subsequent unrelated work.
 * Most useful in pipelined batch lookups where multiple independent walks
 * are in flight at once; harmless elsewhere (just a no-op hint).
 * ------------------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#   define RGM_PREFETCH_READ(_p) __builtin_prefetch((const void*)(_p), 0, 3)
#elif defined(_MSC_VER)
#   include <intrin.h>
#   define RGM_PREFETCH_READ(_p) _mm_prefetch((const char*)(_p), _MM_HINT_T0)
#else
#   define RGM_PREFETCH_READ(_p) ((void)(_p))
#endif

/* -------------------------------------------------------------------------
 * Atomic primitives (C99-compatible, no <stdatomic.h>)
 *
 * Thin static-inline wrappers over compiler intrinsics, covering the set
 * of operations a lock-free hash trie or arena map needs: pointer-/
 * integer-width acquire loads, release stores, strong compare-and-swap,
 * and fetch-add. Provided widths: i32, i64, and pointer (i64 will fall
 * back to a locked path on 32-bit targets, which is fine for the rare
 * 64-bit counter and never on the hot path).
 *
 * Memory ordering (matches the publish / consume pattern Chris Wellons'
 * concurrent hash trie uses -- nullprogram 2023-09-30):
 *   rgm_atomic_load_*  -> acquire
 *   rgm_atomic_store_* -> release
 *   rgm_atomic_cas_*   -> release on success, acquire on failure
 *                         (strong CAS -- no spurious failures)
 *   rgm_atomic_fadd_*  -> sequentially consistent
 *   rgm_atomic_full_barrier() -> process-wide seq_cst fence
 *
 * The MSVC _Interlocked* intrinsics are always full barriers (seq_cst),
 * which is strictly stronger than the orderings above -- correct but
 * slightly heavier on ARM64. Callers needing weaker / relaxed semantics
 * can drop to the underlying intrinsics directly; this header keeps the
 * surface narrow on purpose.
 *
 * Alignment: every operand must be naturally aligned for its type
 * (4 bytes for i32, 8 bytes for i64 / pointer-on-64-bit). Misaligned
 * atomic accesses are UB on every target we support.
 * ------------------------------------------------------------------------- */

#if defined(_MSC_VER)

    /* MSVC: the _Interlocked* intrinsics are documented as full barriers
     * (sequentially consistent) on x86, x64 and ARM64; declared in <intrin.h>
     * which the Windows platform block above already includes. */

    typedef volatile long           rgm_atomic_i32;
    typedef volatile long long      rgm_atomic_i64;
    typedef void* volatile          rgm_atomic_ptr;

    /* Acquire-load primitives.
     *
     * Do NOT use _InterlockedOr / _InterlockedCompareExchangePointer here
     * to fake a load -- those emit "lock or [mem], 0" / "lock cmpxchg",
     * which take the cache line in M state and invalidate it on every
     * other core, even when the value is unchanged. Read-heavy walks
     * (hash trie traversal) collapse into cache-line ping-pong.
     *
     * On x86/x64 a properly-aligned mov is atomic and TSO gives acquire
     * ordering for free -- only a compiler-side ordering barrier is
     * needed. On ARM64 plain loads are not acquire under the weak memory
     * model; MSVC's __load_acquireN intrinsics emit the ldar instruction. */
#   if defined(_M_ARM64)
        static __forceinline int32_t rgm_atomic_load_i32(const rgm_atomic_i32* _p)
        {
            return (int32_t)__load_acquire32((const volatile unsigned __int32*)_p);
        }
        static __forceinline int64_t rgm_atomic_load_i64(const rgm_atomic_i64* _p)
        {
            return (int64_t)__load_acquire64((const volatile unsigned __int64*)_p);
        }
#   else /* x86 / x64 */
        static __forceinline int32_t rgm_atomic_load_i32(const rgm_atomic_i32* _p)
        {
            int32_t v = (int32_t)__iso_volatile_load32((const volatile __int32*)_p);
            _ReadWriteBarrier();
            return v;
        }
        static __forceinline int64_t rgm_atomic_load_i64(const rgm_atomic_i64* _p)
        {
#       if defined(_M_IX86)
            /* 32-bit x86: __iso_volatile_load64 compiles to two 32-bit moves,
             * which can tear against a concurrent 64-bit CAS. movq is atomic
             * for an aligned 8-byte access on every SSE2-capable CPU (Intel
             * SDM vol. 3A, 8.1.1), so bounce through an XMM register. */
            __m128i xv = _mm_loadl_epi64((const __m128i*)_p);
            __int64 v;
            _mm_storel_epi64((__m128i*)&v, xv);
            _ReadWriteBarrier();
            return (int64_t)v;
#       else
            int64_t v = (int64_t)__iso_volatile_load64((const volatile __int64*)_p);
            _ReadWriteBarrier();
            return v;
#       endif
        }
#   endif

    static __forceinline void* rgm_atomic_load_ptr(const rgm_atomic_ptr* _p)
    {
        /* Dispatch through the integer-width load so the arch-specific
         * acquire primitive is defined in exactly one place. */
#   if defined(_WIN64)
        return (void*)(intptr_t)rgm_atomic_load_i64((const rgm_atomic_i64*)_p);
#   else
        return (void*)(intptr_t)rgm_atomic_load_i32((const rgm_atomic_i32*)_p);
#   endif
    }

    /* Release-store primitives.
     *
     * Mirror of the acquire-load split above: do NOT use _InterlockedExchange
     * to fake a store -- it emits "lock xchg" which takes the cache line in
     * M state on every write. Value-overwrite paths (HashTrie / HashIndex Put
     * on existing key) bounce the line through coherence traffic without need.
     *
     * On x86/x64 a properly-aligned mov is atomic and TSO gives release
     * ordering for free -- only a compiler-side ordering barrier is needed.
     * On ARM64 plain stores are not release under the weak memory model;
     * MSVC's __stlrN intrinsics emit the stlr instruction. */
#   if defined(_M_ARM64)
        static __forceinline void rgm_atomic_store_i32(rgm_atomic_i32* _p, int32_t _v)
        {
            __stlr32((unsigned __int32 volatile*)_p, (unsigned __int32)_v);
        }
        static __forceinline void rgm_atomic_store_i64(rgm_atomic_i64* _p, int64_t _v)
        {
            __stlr64((unsigned __int64 volatile*)_p, (unsigned __int64)_v);
        }
#   else /* x86 / x64 */
        static __forceinline void rgm_atomic_store_i32(rgm_atomic_i32* _p, int32_t _v)
        {
            _ReadWriteBarrier();
            __iso_volatile_store32((volatile __int32*)_p, (__int32)_v);
        }
        static __forceinline void rgm_atomic_store_i64(rgm_atomic_i64* _p, int64_t _v)
        {
#       if defined(_M_IX86)
            /* 32-bit x86: mirror of the load above -- a plain 64-bit store
             * is two 32-bit moves and can tear; movq keeps it atomic. */
            __int64 v = (__int64)_v;
            _ReadWriteBarrier();
            _mm_storel_epi64((__m128i*)_p, _mm_loadl_epi64((const __m128i*)&v));
#       else
            _ReadWriteBarrier();
            __iso_volatile_store64((volatile __int64*)_p, (__int64)_v);
#       endif
        }
#   endif

    static __forceinline void rgm_atomic_store_ptr(rgm_atomic_ptr* _p, void* _v)
    {
        /* Dispatch through the integer-width store so the arch-specific
         * release primitive is defined in exactly one place. */
#   if defined(_WIN64)
        rgm_atomic_store_i64((rgm_atomic_i64*)_p, (int64_t)(intptr_t)_v);
#   else
        rgm_atomic_store_i32((rgm_atomic_i32*)_p, (int32_t)(intptr_t)_v);
#   endif
    }

    /* Strong CAS: returns 1 on success, 0 on failure. */
    static __forceinline int rgm_atomic_cas_i32(rgm_atomic_i32* _p,
                                                int32_t _expected, int32_t _desired)
    {
        return _InterlockedCompareExchange(_p, (long)_desired, (long)_expected)
            == (long)_expected;
    }
    static __forceinline int rgm_atomic_cas_i64(rgm_atomic_i64* _p,
                                                int64_t _expected, int64_t _desired)
    {
        return _InterlockedCompareExchange64(_p, (long long)_desired, (long long)_expected)
            == (long long)_expected;
    }
    static __forceinline int rgm_atomic_cas_ptr(rgm_atomic_ptr* _p,
                                                void* _expected, void* _desired)
    {
        return _InterlockedCompareExchangePointer(_p, _desired, _expected) == _expected;
    }

    /* Value-returning CAS variant: returns the slot's prior value (==_expected
     * on success, the racing winner's published value on failure). Used by
     * the trie's CAS-retry path to skip the follow-up acquire-load on failure
     * -- the prior value the CAS already delivered is the same one another
     * round-trip to the slot would have produced (the trie's "published
     * pointers are never overwritten" invariant guarantees stability). */
    static __forceinline int64_t rgm_atomic_cas_val_i64(rgm_atomic_i64* _p,
                                                       int64_t _expected, int64_t _desired)
    {
        return (int64_t)_InterlockedCompareExchange64(_p, (long long)_desired,
                                                          (long long)_expected);
    }

    /* Returns the value held before the add. */
    static __forceinline int32_t rgm_atomic_fadd_i32(rgm_atomic_i32* _p, int32_t _v)
    {
        return (int32_t)_InterlockedExchangeAdd(_p, (long)_v);
    }
    static __forceinline int64_t rgm_atomic_fadd_i64(rgm_atomic_i64* _p, int64_t _v)
    {
#if defined(_M_IX86)
        /* x86 has no _InterlockedExchangeAdd64 intrinsic; emulate with a CAS
         * loop on _InterlockedCompareExchange64 (cmpxchg8b). */
        __int64 _old, _want;
        do {
            _old  = *(volatile __int64*)_p;
            _want = _old + (__int64)_v;
        } while (_InterlockedCompareExchange64((volatile __int64*)_p,
                                               _want, _old) != _old);
        return (int64_t)_old;
#else
        return (int64_t)_InterlockedExchangeAdd64(_p, (long long)_v);
#endif
    }

    /* Atomic bitwise OR with release ordering. Used by BloomFilter Add to
     * publish bit-sets so concurrent Test readers see them via an acquire
     * load. _InterlockedOr64 is intrinsically locked + seq_cst on MSVC. */
    static __forceinline void rgm_atomic_or_i64(rgm_atomic_i64* _p, int64_t _v)
    {
#if defined(_M_IX86)
        /* x86 has no _InterlockedOr64 intrinsic; emulate with a CAS loop on
         * _InterlockedCompareExchange64 (cmpxchg8b). Still locked + seq_cst:
         * each CAS is a full barrier, so the release semantics the BloomFilter
         * Add path relies on are preserved. */
        __int64 _old, _want;
        do {
            _old  = *(volatile __int64*)_p;
            _want = _old | (__int64)_v;
        } while (_InterlockedCompareExchange64((volatile __int64*)_p,
                                               _want, _old) != _old);
#else
        (void)_InterlockedOr64(_p, (long long)_v);
#endif
    }

    static __forceinline void rgm_atomic_full_barrier(void)
    {
        MemoryBarrier();
    }

#elif defined(__GNUC__) || defined(__clang__)

    /* GCC / Clang: __atomic_* builtins take memory_order explicitly. The
     * builtins are GCC 4.7+ and Clang 3.1+; both far older than anything
     * we target. */

    typedef int32_t                 rgm_atomic_i32;
    typedef int64_t                 rgm_atomic_i64;
    typedef void*                   rgm_atomic_ptr;

    static inline int32_t rgm_atomic_load_i32(const rgm_atomic_i32* _p)
    {
        return __atomic_load_n(_p, __ATOMIC_ACQUIRE);
    }
    static inline int64_t rgm_atomic_load_i64(const rgm_atomic_i64* _p)
    {
        return __atomic_load_n(_p, __ATOMIC_ACQUIRE);
    }
    static inline void* rgm_atomic_load_ptr(const rgm_atomic_ptr* _p)
    {
        return __atomic_load_n(_p, __ATOMIC_ACQUIRE);
    }

    static inline void rgm_atomic_store_i32(rgm_atomic_i32* _p, int32_t _v)
    {
        __atomic_store_n(_p, _v, __ATOMIC_RELEASE);
    }
    static inline void rgm_atomic_store_i64(rgm_atomic_i64* _p, int64_t _v)
    {
        __atomic_store_n(_p, _v, __ATOMIC_RELEASE);
    }
    static inline void rgm_atomic_store_ptr(rgm_atomic_ptr* _p, void* _v)
    {
        __atomic_store_n(_p, _v, __ATOMIC_RELEASE);
    }

    /* Strong CAS with release-on-success / acquire-on-failure: the standard
     * publish-pointer pattern. Success publishes the new value to other
     * threads; failure pairs with a concurrent store's release. 4th arg = 0
     * selects the strong form (no spurious failures). */
    static inline int rgm_atomic_cas_i32(rgm_atomic_i32* _p,
                                         int32_t _expected, int32_t _desired)
    {
        return __atomic_compare_exchange_n(_p, &_expected, _desired, 0,
                                           __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
    }
    static inline int rgm_atomic_cas_i64(rgm_atomic_i64* _p,
                                         int64_t _expected, int64_t _desired)
    {
        return __atomic_compare_exchange_n(_p, &_expected, _desired, 0,
                                           __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
    }
    static inline int rgm_atomic_cas_ptr(rgm_atomic_ptr* _p,
                                         void* _expected, void* _desired)
    {
        return __atomic_compare_exchange_n(_p, &_expected, _desired, 0,
                                           __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
    }

    /* Value-returning CAS variant. __atomic_compare_exchange_n writes the
     * prior value into *expected on failure and leaves it unchanged on
     * success -- returning the local `expected` after the call therefore
     * yields _expected on success and the racing value on failure. */
    static inline int64_t rgm_atomic_cas_val_i64(rgm_atomic_i64* _p,
                                                int64_t _expected, int64_t _desired)
    {
        int64_t expected = _expected;
        (void)__atomic_compare_exchange_n(_p, &expected, _desired, 0,
                                          __ATOMIC_RELEASE, __ATOMIC_ACQUIRE);
        return expected;
    }

    static inline int32_t rgm_atomic_fadd_i32(rgm_atomic_i32* _p, int32_t _v)
    {
        return __atomic_fetch_add(_p, _v, __ATOMIC_SEQ_CST);
    }
    static inline int64_t rgm_atomic_fadd_i64(rgm_atomic_i64* _p, int64_t _v)
    {
        return __atomic_fetch_add(_p, _v, __ATOMIC_SEQ_CST);
    }

    /* Atomic bitwise OR with release ordering. See MSVC variant for usage. */
    static inline void rgm_atomic_or_i64(rgm_atomic_i64* _p, int64_t _v)
    {
        (void)__atomic_fetch_or(_p, _v, __ATOMIC_RELEASE);
    }

    static inline void rgm_atomic_full_barrier(void)
    {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

#else
#   error "rg_memory: atomic primitives not implemented for this compiler"
#endif

#endif /* RG_MEMORY_PLATFORM_H_HEADER_GUARD */
