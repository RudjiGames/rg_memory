/*
 * Imagus Engine -  Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Multi-threaded stress test for HashTrie.
 *
 * Uses platform-native threading primitives directly (Win32 CreateThread
 * on Windows, pthread on POSIX) so rg_memory itself stays free of any
 * threading wrapper. The thin layer in this file is test-only.
 *
 * Pattern: one writer + N readers. The writer inserts a fixed set of
 * key/value pairs; readers concurrently query every key in a loop until
 * the writer signals "done" plus one extra pass to ensure each reader
 * observes the fully-populated trie. The test fails if any reader ever
 * sees a Get-success with a value that doesn't match what the writer
 * stored -- the only way that can happen is if the trie's release-CAS
 * publish or atomic_store on the value slot is broken.
 *
 * Multi-writer is intentionally NOT exercised here: rgArenaAlloc is
 * single-threaded by convention in this library, so the documented safe
 * concurrent usage is single-writer / many-reader. Genuine multi-writer
 * would need either a per-thread arena (Wellons' original model) or
 * external locking around Put.
 */

#include <rg_memory_test_pch.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Test-local thin threading layer (intentionally NOT in rg_memory).
 * ------------------------------------------------------------------------- */

#if defined(_WIN32) || defined(_WIN64)
#   define RGM_TEST_WINDOWS 1
#else
#   define RGM_TEST_WINDOWS 0
#endif

#if RGM_TEST_WINDOWS
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#else
#   include <pthread.h>
#endif

typedef void (*rgm_test_worker)(void*);

typedef struct rgm_test_tramp
{
    rgm_test_worker fn;
    void*           arg;
} rgm_test_tramp;

#if RGM_TEST_WINDOWS

typedef HANDLE rgm_test_thread;

static DWORD WINAPI rgm_test_win_tramp(LPVOID _p)
{
    rgm_test_tramp* t = (rgm_test_tramp*)_p;
    t->fn(t->arg);
    return 0;
}

static rgm_test_thread rgm_test_thread_spawn(rgm_test_tramp* _t,
                                             rgm_test_worker _fn, void* _arg)
{
    _t->fn  = _fn;
    _t->arg = _arg;
    return CreateThread(NULL, 0, rgm_test_win_tramp, _t, 0, NULL);
}

static void rgm_test_thread_join(rgm_test_thread _h)
{
    WaitForSingleObject(_h, INFINITE);
    CloseHandle(_h);
}

static void rgm_test_atomic_store_i32(volatile int32_t* _p, int32_t _v)
{
    InterlockedExchange((volatile LONG*)_p, (LONG)_v);
}

static int32_t rgm_test_atomic_load_i32(const volatile int32_t* _p)
{
    return (int32_t)InterlockedOr((volatile LONG*)_p, 0);
}

#else /* POSIX */

typedef pthread_t rgm_test_thread;

static void* rgm_test_posix_tramp(void* _p)
{
    rgm_test_tramp* t = (rgm_test_tramp*)_p;
    t->fn(t->arg);
    return NULL;
}

static rgm_test_thread rgm_test_thread_spawn(rgm_test_tramp* _t,
                                             rgm_test_worker _fn, void* _arg)
{
    _t->fn  = _fn;
    _t->arg = _arg;
    pthread_t th;
    /* pthread_create essentially never fails on modern systems unless
     * resources are exhausted; if it does, downstream Join will hang or
     * the worker simply never runs. A failed test will show the symptom. */
    (void)pthread_create(&th, NULL, rgm_test_posix_tramp, _t);
    return th;
}

static void rgm_test_thread_join(rgm_test_thread _h)
{
    pthread_join(_h, NULL);
}

static void rgm_test_atomic_store_i32(volatile int32_t* _p, int32_t _v)
{
    __atomic_store_n(_p, _v, __ATOMIC_SEQ_CST);
}

static int32_t rgm_test_atomic_load_i32(const volatile int32_t* _p)
{
    return __atomic_load_n(_p, __ATOMIC_SEQ_CST);
}

#endif

/* -------------------------------------------------------------------------
 * Stress test parameters
 * ------------------------------------------------------------------------- */

/* 1024 keys gives ~5 levels of trie depth on average (log4) -- enough that
 * child[] indexing actually happens across the run. 4 readers is a good
 * middle ground: enough to interleave with the writer on most machines,
 * not so many that contention is theatre. */
#define RGM_STRESS_NUM_KEYS    1024
#define RGM_STRESS_NUM_READERS 4

typedef struct stress_shared
{
    HashTrie*        trie;
    Arena*           writer_arena;
    volatile int32_t writer_done;
} stress_shared;

typedef struct reader_ctx
{
    stress_shared* shared;
    int            errors;      /* non-zero => reader saw a wrong value. */
    int            iterations;  /* full key-set sweeps completed.        */
} reader_ctx;

/* Predictable key + value so the verifier can recompute expectations from
 * the index alone. Encoding a recognisable pattern into the value helps a
 * dump distinguish "right value for the wrong key" from "torn value". */

static void rgm_stress_make_key(int _i, char* _buf, int* _outLen)
{
    int n = snprintf(_buf, 32, "key-%08d", _i);
    *_outLen = (n > 0 ? n : 0);
}

static uint64_t rgm_stress_expected_value(int _i)
{
    return ((uint64_t)(uint32_t)_i << 32) | (uint64_t)(0xCAFEBABEu ^ (uint32_t)_i);
}

/* -------------------------------------------------------------------------
 * Worker functions
 * ------------------------------------------------------------------------- */

static void rgm_stress_writer(void* _arg)
{
    stress_shared* s = (stress_shared*)_arg;
    for (int i = 0; i < RGM_STRESS_NUM_KEYS; ++i)
    {
        char key[32];
        int  klen = 0;
        rgm_stress_make_key(i, key, &klen);
        /* Single-threaded writer, so the arena's rgArenaAlloc contract is
         * satisfied. The trie's CAS happens via rgm_atomic_cas_i64 -- it
         * doesn't matter for correctness here (no contention) but the same
         * code path exercises the readers' acquire-loads. */
        rgHashTriePut(s->trie, s->writer_arena, key, (size_t)klen,
                      rgm_stress_expected_value(i));
    }
    rgm_test_atomic_store_i32(&s->writer_done, 1);
}

static void rgm_stress_reader(void* _arg)
{
    reader_ctx*    r = (reader_ctx*)_arg;
    stress_shared* s = r->shared;

    /* Loop until the writer signals done, then make ONE more pass so the
     * reader sees the fully-populated trie and verifies every key. */
    int extra_pass = 0;
    for (;;)
    {
        for (int i = 0; i < RGM_STRESS_NUM_KEYS; ++i)
        {
            char key[32];
            int  klen = 0;
            rgm_stress_make_key(i, key, &klen);
            uint64_t v = 0;
            if (rgHashTrieGet(s->trie, key, (size_t)klen, &v) == 0)
            {
                if (v != rgm_stress_expected_value(i))
                {
                    /* Per-reader counter -- no inter-thread contention. */
                    r->errors++;
                }
            }
        }
        r->iterations++;

        if (extra_pass) break;
        if (rgm_test_atomic_load_i32(&s->writer_done)) extra_pass = 1;
    }
}

/* -------------------------------------------------------------------------
 * Test
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashTrieConcurrentSingleWriterManyReaders(void)
{
    /* 16 MiB arena: comfortable headroom for 1024 entries + key bytes. */
    Arena arena;
    rgArenaCreate(&arena, 16 * 1024 * 1024);

    HashTrie trie;
    rgHashTrieInit(&trie);

    stress_shared shared;
    shared.trie         = &trie;
    shared.writer_arena = &arena;
    shared.writer_done  = 0;

    reader_ctx     rctx[RGM_STRESS_NUM_READERS];
    rgm_test_tramp reader_tramps[RGM_STRESS_NUM_READERS];
    rgm_test_tramp writer_tramp;

    for (int i = 0; i < RGM_STRESS_NUM_READERS; ++i)
    {
        rctx[i].shared     = &shared;
        rctx[i].errors     = 0;
        rctx[i].iterations = 0;
    }

    /* Spawn readers first so they're polling an empty trie when the
     * writer starts -- maximises the window for inter-thread overlap. */
    rgm_test_thread readers[RGM_STRESS_NUM_READERS];
    for (int i = 0; i < RGM_STRESS_NUM_READERS; ++i)
    {
        readers[i] = rgm_test_thread_spawn(&reader_tramps[i],
                                           rgm_stress_reader, &rctx[i]);
    }
    rgm_test_thread writer = rgm_test_thread_spawn(&writer_tramp,
                                                   rgm_stress_writer, &shared);

    rgm_test_thread_join(writer);
    for (int i = 0; i < RGM_STRESS_NUM_READERS; ++i)
    {
        rgm_test_thread_join(readers[i]);
    }

    /* No reader may have observed a Get-hit with the wrong value. */
    for (int i = 0; i < RGM_STRESS_NUM_READERS; ++i)
    {
        TEST_ASSERT_EQUAL_INT(0, rctx[i].errors);
        TEST_ASSERT_TRUE(rctx[i].iterations > 0);
    }

    /* Final state must have every key readable with the right value. */
    for (int i = 0; i < RGM_STRESS_NUM_KEYS; ++i)
    {
        char key[32];
        int  klen = 0;
        rgm_stress_make_key(i, key, &klen);
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&trie, key, (size_t)klen, &v));
        TEST_ASSERT_EQUAL_UINT64(rgm_stress_expected_value(i), v);
    }

    rgArenaDestroy(&arena);
}

/* -------------------------------------------------------------------------
 * Multi-writer stress: N writer threads, each with its OWN arena, all
 * inserting disjoint key ranges into ONE shared HashTrie.
 *
 * This is the path the library documents but the single-writer test above
 * does not cover: concurrent Puts racing on shared trie slots, and nodes
 * from several per-thread arenas stitched into one trie via absolute
 * pointers. Writers contend whenever their keys descend through the same
 * subtrie; the lock-free CAS publish must let exactly one win each race
 * while every key still ends up present. After the writers join, a single
 * thread verifies that every key from every range resolves to the value its
 * writer stored -- a lost update, a torn publish, or a cross-arena pointer
 * mistake would all surface here.
 * ------------------------------------------------------------------------- */

#define RGM_MW_NUM_WRITERS     4
#define RGM_MW_KEYS_PER_WRITER 4000

/* Globally-unique key for (writer, index); value recomputable from the key. */
static uint64_t rgm_mw_key(int _writer, int _i)
{
    return (uint64_t)_writer * 1000000u + (uint64_t)_i;
}
static uint64_t rgm_mw_value(uint64_t _key)
{
    return (_key << 1) ^ 0x9E3779B97F4A7C15ull;
}

typedef struct mw_writer_ctx
{
    HashTrie* trie;
    Arena     arena;   /* this writer's private arena (single-threaded use). */
    int       id;
} mw_writer_ctx;

static int rgm_mw_count_cb(const void* k, uint64_t kl, uint64_t v, void* ud)
{
    (void)k; (void)kl; (void)v;
    (*(uint64_t*)ud)++;
    return 0;
}

static void rgm_mw_writer(void* _arg)
{
    mw_writer_ctx* w = (mw_writer_ctx*)_arg;
    for (int i = 0; i < RGM_MW_KEYS_PER_WRITER; ++i)
    {
        uint64_t k = rgm_mw_key(w->id, i);
        rgHashTriePutU64(w->trie, &w->arena, k, rgm_mw_value(k));
    }
}

void rgMemoryTest_hashTrieConcurrentMultiWriter(void)
{
    HashTrie trie;
    rgHashTrieInit(&trie);

    mw_writer_ctx  wctx[RGM_MW_NUM_WRITERS];
    rgm_test_tramp tramps[RGM_MW_NUM_WRITERS];
    rgm_test_thread threads[RGM_MW_NUM_WRITERS];

    for (int w = 0; w < RGM_MW_NUM_WRITERS; ++w)
    {
        wctx[w].trie = &trie;
        wctx[w].id   = w;
        /* Generous per-writer arena: live nodes plus any nodes lost to CAS
         * races (the loser's speculative node stays in its arena). */
        TEST_ASSERT_EQUAL_INT(0, rgArenaCreate(&wctx[w].arena, 16 * 1024 * 1024));
    }

    for (int w = 0; w < RGM_MW_NUM_WRITERS; ++w)
    {
        threads[w] = rgm_test_thread_spawn(&tramps[w], rgm_mw_writer, &wctx[w]);
    }
    for (int w = 0; w < RGM_MW_NUM_WRITERS; ++w)
    {
        rgm_test_thread_join(threads[w]);
    }

    /* Every key from every writer must be present with the right value. */
    for (int w = 0; w < RGM_MW_NUM_WRITERS; ++w)
    {
        for (int i = 0; i < RGM_MW_KEYS_PER_WRITER; ++i)
        {
            uint64_t k = rgm_mw_key(w, i);
            uint64_t v = 0;
            TEST_ASSERT_EQUAL_INT(0, rgHashTrieGetU64(&trie, k, &v));
            TEST_ASSERT_EQUAL_UINT64(rgm_mw_value(k), v);
        }
    }

    /* ForEach must see exactly the full set (no duplicates, no drops). */
    uint64_t iterated = 0;
    uint64_t visited  = rgHashTrieForEach(&trie, rgm_mw_count_cb, &iterated);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)RGM_MW_NUM_WRITERS * RGM_MW_KEYS_PER_WRITER, visited);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)RGM_MW_NUM_WRITERS * RGM_MW_KEYS_PER_WRITER, iterated);

    /* Arenas must outlive every Get above (the trie points into them). */
    for (int w = 0; w < RGM_MW_NUM_WRITERS; ++w)
    {
        rgArenaDestroy(&wctx[w].arena);
    }
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_HashTrieMT(void)
{
    RUN_TEST(rgMemoryTest_hashTrieConcurrentSingleWriterManyReaders);
    RUN_TEST(rgMemoryTest_hashTrieConcurrentMultiWriter);
}
