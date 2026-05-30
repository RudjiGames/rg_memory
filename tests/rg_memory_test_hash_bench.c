/*
 * Imagus Engine -  Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Throughput benchmarks for HashMap and HashTrie.
 *
 * Not a pass/fail test in the usual sense: the test functions always
 * "pass" via a couple of sanity asserts at the end, but the meaningful
 * output is the Mops/sec numbers printed to stdout. Use these to spot
 * regressions across changes, not as absolute performance promises --
 * results vary heavily by machine, compiler, and key distribution.
 *
 * Single-threaded workload:
 *   - N = 1,000,000 unique 8-byte integer keys.
 *   - Put pass:        inserts all N keys.
 *   - Get (seq) pass:  looks them up in insertion order.
 *   - Get (rand) pass: looks them up via a pre-computed permutation
 *                      (LCG-shuffled), to capture cache-cold behaviour
 *                      that sequential access masks.
 *
 * Multi-threaded HashTrie workload:
 *   - One arena per thread (mandatory: rgArenaAlloc is single-threaded
 *     by contract).
 *   - Thread count = portable CPU detection (GetSystemInfo on Windows,
 *     sysconf(_SC_NPROCESSORS_ONLN) on POSIX).
 *   - Each thread inserts a private slice of N keys; the trie is
 *     shared. Aggregate Mops/sec gives multi-writer Put throughput.
 *   - After joins, the main thread reads every key back to verify
 *     correctness end-to-end.
 *
 * Cross-platform high-resolution timing:
 *   - Windows: QueryPerformanceCounter
 *   - POSIX:   clock_gettime(CLOCK_MONOTONIC, ...)
 */

#include <rg_memory_test_pch.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#if defined(_WIN32) || defined(_WIN64)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#else
#   include <time.h>
#   include <unistd.h>   /* sysconf(_SC_NPROCESSORS_ONLN) */
#   include <pthread.h>  /* pthread_create / pthread_join */
#endif

/* -------------------------------------------------------------------------
 * Timing
 * ------------------------------------------------------------------------- */

static double bench_seconds(void)
{
#if defined(_WIN32) || defined(_WIN64)
    static LARGE_INTEGER s_freq = {{0}};
    LARGE_INTEGER        now;
    if (s_freq.QuadPart == 0)
    {
        QueryPerformanceFrequency(&s_freq);
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)s_freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* Estimated cache lines touched per op based on average 4-ary trie depth
 * (ceil(log4(_n))). Each step touches one 64-byte cache line (HashNode
 * with inline key fits in one line). A Put-miss additionally writes one
 * freshly allocated line for the new node. */
static uint64_t bench_bytes_per_op(uint32_t _n, int _put_miss)
{
    uint32_t depth = 0;
    uint32_t v     = _n;
    while (v > 1)
    {
        v >>= 2;
        depth++;
    }
    return ((uint64_t)depth + (_put_miss ? 1u : 0u)) * 64ull;
}

static void bench_print_row(const char* _name, uint32_t _n, double _seconds,
                            uint64_t _bytesPerOp)
{
    /* Defensive: divide-by-zero guard for absurdly fast timers. */
    double seconds = (_seconds > 1e-9) ? _seconds : 1e-9;
    double mops    = ((double)_n / seconds) * 1e-6;
    double gbs     = ((double)_n * (double)_bytesPerOp / seconds) * 1e-9;
    printf("  %-22s %7.3fM ops in %7.3fs = %7.2f Mops/sec  (%6.2f GB/s, ~%llu B/op)\n",
           _name, (double)_n * 1e-6, seconds, mops, gbs,
           (unsigned long long)_bytesPerOp);
}

/* -------------------------------------------------------------------------
 * CPU count (portable, C99 + platform headers above)
 * ------------------------------------------------------------------------- */

static uint32_t bench_cpu_count(void)
{
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (uint32_t)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 1u;
#endif
}

/* -------------------------------------------------------------------------
 * Shared workload helpers
 * ------------------------------------------------------------------------- */

#define BENCH_N        1000000   /* keys per pass (single-threaded 1M bench)  */
#define BENCH_N_10M    10000000  /* keys per pass (single-threaded 10M bench) */
#define BENCH_MT_TOTAL 1000000   /* total keys across all threads (MT 1M)     */
#define BENCH_MT_TOTAL_10M 10000000 /* total keys across all threads (MT 10M) */
#define BENCH_MT_MAX_THREADS 64u

/* LCG (Numerical Recipes constants) -- fast, deterministic, plenty of
 * spread for shuffle. Not cryptographic. */
static uint64_t bench_lcg(uint64_t* _state)
{
    *_state = (*_state) * 6364136223846793005ull + 1442695040888963407ull;
    return *_state;
}

/* Fisher-Yates shuffle of [0, n). _scratch must hold n uint32_t. */
static void bench_make_permutation(uint32_t* _scratch, uint32_t _n, uint64_t _seed)
{
    uint32_t i;
    for (i = 0; i < _n; ++i) _scratch[i] = i;
    for (i = _n - 1; i > 0; --i)
    {
        uint32_t j   = (uint32_t)(bench_lcg(&_seed) >> 32) % (i + 1);
        uint32_t tmp = _scratch[i];
        _scratch[i]  = _scratch[j];
        _scratch[j]  = tmp;
    }
}

static uint64_t bench_make_key(uint32_t _i)
{
    /* Treat the 8 bytes of the uint64 as the key payload. wyhash does
     * the spreading; sequential ints still give well-distributed trie
     * depth because the 64x64 wymum multiply propagates any one-byte
     * difference across the full 64-bit state. */
    return (uint64_t)_i;
}

static uint64_t bench_make_value(uint32_t _i)
{
    /* Make value distinguishable from key index so a "right value for the
     * wrong key" bug shows up as a sanity-assert failure. */
    return ((uint64_t)_i << 32) | (uint64_t)(0xCAFEBABEu ^ _i);
}

/* -------------------------------------------------------------------------
 * HashMap benchmark
 * ------------------------------------------------------------------------- */

/* Parameterised HashMap benchmark body. _arenaSize covers the 64 B / entry
 * cache-line-aligned node plus inline key, plus the permutation array,
 * plus headroom for alignment slack. */
static void bench_hash_map_run(uint32_t _n, uint64_t _arenaSize)
{
    Arena arena;
    rgArenaCreate(&arena, _arenaSize);

    /* Permutation lives in the same arena -- no separate allocator needed.
     * Allocated FIRST so its bytes don't get pushed around by Puts later. */
    uint32_t* perm = (uint32_t*)rgArenaAllocAligned(
        &arena, (size_t)_n * sizeof(uint32_t), 16);
    bench_make_permutation(perm, _n, 0x9E3779B97F4A7C15ull);

    HashMap map;
    rgHashMapInit(&map, &arena);

    printf("HashMap (N = %u):\n", (unsigned)_n);

    /* --- Put pass ------------------------------------------------------ */
    double t0 = bench_seconds();
    {
        uint32_t i;
        for (i = 0; i < _n; ++i)
        {
            uint64_t k = bench_make_key(i);
            uint64_t* v = rgHashMapPut(&map, &k, sizeof(k));
            if (v != NULL) *v = bench_make_value(i);
        }
    }
    double t1 = bench_seconds();
    bench_print_row("Put:",         _n, t1 - t0, bench_bytes_per_op(_n, 1));

    /* --- Get (sequential) ---------------------------------------------- */
    uint64_t checksum_seq = 0;
    double   t2 = bench_seconds();
    {
        uint32_t i;
        for (i = 0; i < _n; ++i)
        {
            uint64_t k = bench_make_key(i);
            uint64_t* v = rgHashMapGet(&map, &k, sizeof(k));
            checksum_seq ^= (v != NULL ? *v : 0);  /* prevent the compiler optimising the loop away */
        }
    }
    double t3 = bench_seconds();
    bench_print_row("Get (seq):",   _n, t3 - t2, bench_bytes_per_op(_n, 0));

    /* --- Get (random) -------------------------------------------------- */
    uint64_t checksum_rand = 0;
    double   t4 = bench_seconds();
    {
        uint32_t i;
        for (i = 0; i < _n; ++i)
        {
            uint32_t idx = perm[i];
            uint64_t k   = bench_make_key(idx);
            uint64_t* v  = rgHashMapGet(&map, &k, sizeof(k));
            checksum_rand ^= (v != NULL ? *v : 0);
        }
    }
    double t5 = bench_seconds();
    bench_print_row("Get (rand):",  _n, t5 - t4, bench_bytes_per_op(_n, 0));

    /* Sanity: both Get passes touched the same entries, so XOR sums match. */
    TEST_ASSERT_EQUAL_UINT64(checksum_seq, checksum_rand);

    /* Spot-check a few values to confirm the map actually populated. */
    {
        uint64_t k = bench_make_key(0);
        uint64_t* v = rgHashMapGet(&map, &k, sizeof(k));
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_EQUAL_UINT64(bench_make_value(0), *v);

        k = bench_make_key(_n - 1);
        v = rgHashMapGet(&map, &k, sizeof(k));
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_EQUAL_UINT64(bench_make_value(_n - 1), *v);
    }

    rgArenaDestroy(&arena);
}

void rgMemoryTest_hashMapBenchmark(void)
{
    bench_hash_map_run(BENCH_N, 128ull * 1024ull * 1024ull);
}

void rgMemoryTest_hashMapBenchmark10M(void)
{
    /* 10M * 64 B/entry ~= 640 MiB; plus 40 MiB perm; round up to 1 GiB. */
    bench_hash_map_run(BENCH_N_10M, 1024ull * 1024ull * 1024ull);
}

/* -------------------------------------------------------------------------
 * HashTrie benchmark (single-threaded; same workload, atomic-op path)
 * ------------------------------------------------------------------------- */

/* Parameterised HashTrie single-threaded benchmark body. */
static void bench_hash_trie_run(uint32_t _n, uint64_t _arenaSize)
{
    Arena arena;
    rgArenaCreate(&arena, _arenaSize);

    uint32_t* perm = (uint32_t*)rgArenaAllocAligned(
        &arena, (size_t)_n * sizeof(uint32_t), 16);
    bench_make_permutation(perm, _n, 0x9E3779B97F4A7C15ull);

    HashTrie trie;
    rgHashTrieInit(&trie);

    printf("HashTrie (N = %u, single-threaded; atomic ops always run):\n",
           (unsigned)_n);

    /* --- Put pass ------------------------------------------------------ */
    double t0 = bench_seconds();
    {
        uint32_t i;
        for (i = 0; i < _n; ++i)
        {
            uint64_t k = bench_make_key(i);
            (void)rgHashTriePut(&trie, &arena, &k, sizeof(k), bench_make_value(i));
        }
    }
    double t1 = bench_seconds();
    bench_print_row("Put:",         _n, t1 - t0, bench_bytes_per_op(_n, 1));

    /* --- Get (sequential) ---------------------------------------------- */
    uint64_t checksum_seq = 0;
    double   t2 = bench_seconds();
    {
        uint32_t i;
        for (i = 0; i < _n; ++i)
        {
            uint64_t k = bench_make_key(i);
            uint64_t v = 0;
            (void)rgHashTrieGet(&trie, &k, sizeof(k), &v);
            checksum_seq ^= v;
        }
    }
    double t3 = bench_seconds();
    bench_print_row("Get (seq):",   _n, t3 - t2, bench_bytes_per_op(_n, 0));

    /* --- Get (random) -------------------------------------------------- */
    uint64_t checksum_rand = 0;
    double   t4 = bench_seconds();
    {
        uint32_t i;
        for (i = 0; i < _n; ++i)
        {
            uint32_t idx = perm[i];
            uint64_t k   = bench_make_key(idx);
            uint64_t v   = 0;
            (void)rgHashTrieGet(&trie, &k, sizeof(k), &v);
            checksum_rand ^= v;
        }
    }
    double t5 = bench_seconds();
    bench_print_row("Get (rand):",  _n, t5 - t4, bench_bytes_per_op(_n, 0));

    /* --- Get (random, batched U64) ------------------------------------- */
    /* Same shuffled key stream as the Get (rand) pass above, but routed
     * through rgHashTrieGetBatchU64 so the pipelined walks can issue
     * independent cache-line loads in parallel. The expected lift over
     * Get (rand) is the K-fold latency hiding from software pipelining. */
    uint64_t checksum_batch = 0;
    double   t6 = bench_seconds();
    {
        enum { BATCH = 1024 };
        uint64_t kb[BATCH];
        uint64_t vb[BATCH];
        uint32_t i;
        for (i = 0; i < _n; i += BATCH)
        {
            uint32_t n = (_n - i) < BATCH ? (_n - i) : BATCH;
            uint32_t j;
            for (j = 0; j < n; ++j) kb[j] = bench_make_key(perm[i + j]);
            (void)rgHashTrieGetBatchU64(&trie, kb, vb, NULL, n);
            for (j = 0; j < n; ++j) checksum_batch ^= vb[j];
        }
    }
    double t7 = bench_seconds();
    bench_print_row("Get (rand, batched):", _n, t7 - t6, bench_bytes_per_op(_n, 0));

    TEST_ASSERT_EQUAL_UINT64(checksum_seq, checksum_rand);
    TEST_ASSERT_EQUAL_UINT64(checksum_seq, checksum_batch);

    {
        uint64_t k = bench_make_key(0);
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&trie, &k, sizeof(k), &v));
        TEST_ASSERT_EQUAL_UINT64(bench_make_value(0), v);

        k = bench_make_key(_n - 1);
        TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&trie, &k, sizeof(k), &v));
        TEST_ASSERT_EQUAL_UINT64(bench_make_value(_n - 1), v);
    }

    rgArenaDestroy(&arena);
}

void rgMemoryTest_hashTrieBenchmark(void)
{
    bench_hash_trie_run(BENCH_N, 128ull * 1024ull * 1024ull);
}

void rgMemoryTest_hashTrieBenchmark10M(void)
{
    bench_hash_trie_run(BENCH_N_10M, 1024ull * 1024ull * 1024ull);
}

/* -------------------------------------------------------------------------
 * Multi-threaded HashTrie benchmark
 *
 * Each thread owns a private arena and inserts a contiguous slice of the
 * global key space [0, BENCH_MT_TOTAL). The trie is shared. Walltime
 * spans "first thread starts" to "last thread joins"; throughput is
 * (BENCH_MT_TOTAL / walltime). Single-thread baseline above is the
 * apples-to-apples comparison.
 *
 * Self-contained threading layer (same pattern as the MT correctness
 * test): rg_memory itself stays free of any threading wrapper.
 * ------------------------------------------------------------------------- */

#if defined(_WIN32) || defined(_WIN64)
#   define BENCH_MT_WINDOWS 1
#else
#   define BENCH_MT_WINDOWS 0
#endif

typedef void (*bench_worker_fn)(void*);

typedef struct bench_tramp
{
    bench_worker_fn fn;
    void*           arg;
} bench_tramp;

#if BENCH_MT_WINDOWS

typedef HANDLE bench_thread;

static DWORD WINAPI bench_win_tramp(LPVOID _p)
{
    bench_tramp* t = (bench_tramp*)_p;
    t->fn(t->arg);
    return 0;
}

static bench_thread bench_thread_spawn(bench_tramp* _t,
                                       bench_worker_fn _fn, void* _arg)
{
    _t->fn  = _fn;
    _t->arg = _arg;
    return CreateThread(NULL, 0, bench_win_tramp, _t, 0, NULL);
}

static void bench_thread_join(bench_thread _h)
{
    WaitForSingleObject(_h, INFINITE);
    CloseHandle(_h);
}

#else

typedef pthread_t bench_thread;

static void* bench_posix_tramp(void* _p)
{
    bench_tramp* t = (bench_tramp*)_p;
    t->fn(t->arg);
    return NULL;
}

static bench_thread bench_thread_spawn(bench_tramp* _t,
                                       bench_worker_fn _fn, void* _arg)
{
    _t->fn  = _fn;
    _t->arg = _arg;
    pthread_t th;
    (void)pthread_create(&th, NULL, bench_posix_tramp, _t);
    return th;
}

static void bench_thread_join(bench_thread _h)
{
    pthread_join(_h, NULL);
}

#endif

/* -------------------------------------------------------------------------
 * Huge-pages privilege bootstrap
 *
 * Windows MEM_LARGE_PAGES requires SeLockMemoryPrivilege on the calling
 * process token. The privilege has to be (a) granted to the user account
 * via Local Security Policy -> Local Policies -> User Rights Assignment
 * -> "Lock pages in memory", and (b) enabled on the process token at
 * runtime. (a) is operator setup we can't do from inside the test; (b)
 * is one AdjustTokenPrivileges call which we do here.
 *
 * On POSIX, MADV_HUGEPAGE is unprivileged -- this is a no-op.
 * ------------------------------------------------------------------------- */

#if BENCH_MT_WINDOWS
#   pragma comment(lib, "advapi32.lib")  /* OpenProcessToken et al. */

static int bench_try_enable_lock_pages_privilege(void)
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
    {
        return 0;
    }

    LUID luid;
    if (!LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &luid))
    {
        CloseHandle(token);
        return 0;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL  ok  = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    DWORD err = GetLastError();
    CloseHandle(token);

    /* AdjustTokenPrivileges returns TRUE even if it couldn't actually
     * enable the privilege; ERROR_NOT_ALL_ASSIGNED is the signal that
     * the user account doesn't hold it (and needs the LSP grant + a
     * re-login). */
    return ok && err != ERROR_NOT_ALL_ASSIGNED;
}

#else

static int bench_try_enable_lock_pages_privilege(void)
{
    return 0;  /* POSIX: no privilege required; madvise hint is unprivileged. */
}

#endif

typedef struct bench_mt_ctx
{
    HashTrie* trie;
    Arena*    arena;
    uint32_t  first;     /* first key index inserted by this thread. */
    uint32_t  last;      /* one past last.                           */
    uint64_t  checksum;  /* reader output: XOR of retrieved values.  */
} bench_mt_ctx;

static void bench_mt_writer(void* _arg)
{
    /* Uses the uint64-specialised Put: hash computed inline, no length
     * dispatch, key match is one uint64 == instead of a byte loop. */
    bench_mt_ctx* c = (bench_mt_ctx*)_arg;
    uint32_t i;
    for (i = c->first; i < c->last; ++i)
    {
        (void)rgHashTriePutU64(c->trie, c->arena,
                               bench_make_key(i), bench_make_value(i));
    }
}

static void bench_mt_reader(void* _arg)
{
    /* Wait-free reads: no CAS, no allocation. Once the trie is built,
     * interior cache lines sit in S state across all reader cores and
     * stay there. The XOR checksum prevents DCE and lets the test verify
     * every reader saw the expected values for its slice. */
    bench_mt_ctx* c = (bench_mt_ctx*)_arg;
    uint64_t cs = 0;
    uint32_t i;
    for (i = c->first; i < c->last; ++i)
    {
        uint64_t v = 0;
        (void)rgHashTrieGetU64(c->trie, bench_make_key(i), &v);
        cs ^= v;
    }
    c->checksum = cs;
}

static void bench_mt_reader_batched(void* _arg)
{
    /* Same workload as bench_mt_reader, but funnelled through the
     * software-pipelined batch Get. Each batch lets the CPU issue K=8
     * independent cache-line loads per descent level rather than one --
     * the per-thread cost of each Get drops, so the aggregate over 32
     * threads has more headroom before DRAM bandwidth saturates. */
    bench_mt_ctx* c = (bench_mt_ctx*)_arg;
    enum { BATCH = 512 };
    uint64_t kb[BATCH];
    uint64_t vb[BATCH];
    uint64_t cs = 0;

    uint32_t i = c->first;
    while (i < c->last)
    {
        uint32_t n = (c->last - i) < (uint32_t)BATCH
                   ? (c->last - i) : (uint32_t)BATCH;
        uint32_t j;
        for (j = 0; j < n; ++j) kb[j] = bench_make_key(i + j);
        (void)rgHashTrieGetBatchU64(c->trie, kb, vb, NULL, n);
        for (j = 0; j < n; ++j) cs ^= vb[j];
        i += n;
    }
    c->checksum = cs;
}

static void bench_hash_trie_mt_run(uint32_t _target)
{
    uint32_t cpu = bench_cpu_count();
    if (cpu < 2u)              cpu = 2u;     /* still exercise MT path. */
    if (cpu > BENCH_MT_MAX_THREADS) cpu = BENCH_MT_MAX_THREADS;

    uint32_t per_thread = _target / cpu;
    uint32_t total      = per_thread * cpu;  /* may shave a few keys; fine. */

    /* Try to enable the privilege MEM_LARGE_PAGES needs on Windows so the
     * RGM_ARENA_FLAG_HUGE_PAGES hint below actually takes effect. The
     * arena create will silently fall back to normal pages either way --
     * this just lifts the gate when the operator has set things up. */
    int huge_pages_priv = bench_try_enable_lock_pages_privilege();
#if BENCH_MT_WINDOWS
    const char* huge_pages_status = huge_pages_priv
        ? "SeLockMemoryPrivilege enabled"
        : "SeLockMemoryPrivilege NOT granted (using normal pages)";
#else
    (void)huge_pages_priv;
    const char* huge_pages_status = "MADV_HUGEPAGE hinted";
#endif

    printf("HashTrie MT (threads = %u, keys = %u, per thread = %u, %s):\n",
           (unsigned)cpu, (unsigned)total, (unsigned)per_thread,
           huge_pages_status);

    /* One arena per writer. Per-entry footprint for the cache-line-aligned
     * 4-ary trie: sizeof(HashNode) = 48 B + small key + alignment padding
     * rounds up to 64 B per entry. Headroom for CAS-race losers and
     * alignment slack on top of that. Round up to 4 MiB minimum. */
    uint64_t per_arena = (uint64_t)per_thread * 96u;
    if (per_arena < 4u * 1024u * 1024u) per_arena = 4u * 1024u * 1024u;

    HashTrie     trie;
    rgHashTrieInit(&trie);

    Arena        arenas[BENCH_MT_MAX_THREADS];
    bench_mt_ctx ctxs  [BENCH_MT_MAX_THREADS];
    bench_tramp  tramps[BENCH_MT_MAX_THREADS];
    bench_thread ths   [BENCH_MT_MAX_THREADS];

    uint32_t i;
    for (i = 0; i < cpu; ++i)
    {
        /* Try huge pages -- silently degrades to normal pages on systems
         * without the privilege / kernel support. The TLB win on random
         * Get is the reason this benchmark requests them. */
        rgArenaCreateEx(&arenas[i], per_arena, RGM_ARENA_FLAG_HUGE_PAGES);
        ctxs[i].trie     = &trie;
        ctxs[i].arena    = &arenas[i];
        ctxs[i].first    = i * per_thread;
        ctxs[i].last     = ctxs[i].first + per_thread;
        ctxs[i].checksum = 0;
    }

    double t0 = bench_seconds();
    for (i = 0; i < cpu; ++i)
    {
        ths[i] = bench_thread_spawn(&tramps[i], bench_mt_writer, &ctxs[i]);
    }
    for (i = 0; i < cpu; ++i)
    {
        bench_thread_join(ths[i]);
    }
    double t1 = bench_seconds();

    bench_print_row("Put (aggregate):", total, t1 - t0, bench_bytes_per_op(total, 1));

    /* Verify every key the writers were supposed to insert is present
     * with the right value. Single-threaded read pass; if any thread
     * lost its CAS race and didn't retry correctly this would catch it
     * (it won't, but the check is cheap relative to the Put cost). */
    double tv0 = bench_seconds();
    for (i = 0; i < total; ++i)
    {
        uint64_t k = bench_make_key(i);
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&trie, &k, sizeof(k), &v));
        TEST_ASSERT_EQUAL_UINT64(bench_make_value(i), v);
    }
    double tv1 = bench_seconds();
    bench_print_row("Get (verify, 1T):", total, tv1 - tv0, bench_bytes_per_op(total, 0));

    /* --- MT Get pass --------------------------------------------------- */
    /* Concurrent wait-free reads. Each thread Gets its own slice (same
     * partition as the writer pass). The trie's interior cache lines
     * settle into S state across cores and stay there -- no contention
     * traffic, so this should scale near-linearly with thread count
     * until memory bandwidth saturates. */
    double tg0 = bench_seconds();
    for (i = 0; i < cpu; ++i)
    {
        ths[i] = bench_thread_spawn(&tramps[i], bench_mt_reader, &ctxs[i]);
    }
    for (i = 0; i < cpu; ++i)
    {
        bench_thread_join(ths[i]);
    }
    double tg1 = bench_seconds();
    bench_print_row("Get (aggregate):", total, tg1 - tg0, bench_bytes_per_op(total, 0));

    /* Verify the readers actually saw the right values: XOR all the
     * per-thread checksums and compare against the expected sum.
     * Mismatch => a reader missed a key or got the wrong value. */
    uint64_t expected_cs = 0;
    uint64_t got_cs      = 0;
    for (i = 0; i < total; ++i) expected_cs ^= bench_make_value(i);
    for (i = 0; i < cpu;   ++i) got_cs      ^= ctxs[i].checksum;
    TEST_ASSERT_EQUAL_UINT64(expected_cs, got_cs);

    /* --- MT Get pass (software-pipelined batched) --------------------- */
    /* Same parallelism but each thread routes its slice through
     * rgHashTrieGetBatchU64. The K-fold load parallelism per thread
     * means each thread's per-Get latency drops -- aggregate goes up
     * until DRAM bandwidth saturates. */
    for (i = 0; i < cpu; ++i) ctxs[i].checksum = 0;
    double tb0 = bench_seconds();
    for (i = 0; i < cpu; ++i)
    {
        ths[i] = bench_thread_spawn(&tramps[i], bench_mt_reader_batched, &ctxs[i]);
    }
    for (i = 0; i < cpu; ++i)
    {
        bench_thread_join(ths[i]);
    }
    double tb1 = bench_seconds();
    bench_print_row("Get (aggregate, batched):", total, tb1 - tb0, bench_bytes_per_op(total, 0));

    got_cs = 0;
    for (i = 0; i < cpu; ++i) got_cs ^= ctxs[i].checksum;
    TEST_ASSERT_EQUAL_UINT64(expected_cs, got_cs);

    for (i = 0; i < cpu; ++i)
    {
        rgArenaDestroy(&arenas[i]);
    }
}

void rgMemoryTest_hashTrieBenchmarkMT(void)
{
    bench_hash_trie_mt_run(BENCH_MT_TOTAL);
}

void rgMemoryTest_hashTrieBenchmarkMT10M(void)
{
    bench_hash_trie_mt_run(BENCH_MT_TOTAL_10M);
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_HashBench(void)
{
    /* 1M baseline -- quick enough for routine CI, exercises every code path. */
    RUN_TEST(rgMemoryTest_hashMapBenchmark);
    RUN_TEST(rgMemoryTest_hashTrieBenchmark);
    RUN_TEST(rgMemoryTest_hashTrieBenchmarkMT);

    /* 10M -- pushes past L3 capacity on most CPUs so the numbers reflect
     * DRAM-bandwidth-bound behaviour. Takes a few seconds; gated by the
     * same harness invocation as the 1M pass. */
    RUN_TEST(rgMemoryTest_hashMapBenchmark10M);
    RUN_TEST(rgMemoryTest_hashTrieBenchmark10M);
    RUN_TEST(rgMemoryTest_hashTrieBenchmarkMT10M);
}
