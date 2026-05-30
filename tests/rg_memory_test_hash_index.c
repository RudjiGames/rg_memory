/*
 * Imagus Engine -  Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 */

#include <rg_memory_test_pch.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Zero-init / NULL safety
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashIndexZeroInitIsEmpty(void)
{
    HashIndex idx;
    memset(&idx, 0, sizeof(idx));
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexGet(&idx, "k", 1, &v));
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexGetH(&idx, 0xdeadbeef, 0xcafef00d, &v));
}

void rgMemoryTest_hashIndexNullPointerIsSafe(void)
{
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexInit(NULL));
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexPut(NULL, NULL, "k", 1, 1));
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexGet(NULL, "k", 1, &v));
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexPutH(NULL, NULL, 1, 2, 3));
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexGetH(NULL, 1, 2, &v));
}

void rgMemoryTest_hashIndexPutNullArenaFails(void)
{
    HashIndex idx;
    rgHashIndexInit(&idx);
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexPut(&idx, NULL, "k", 1, 1));
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexPutH(&idx, NULL, 1, 2, 3));
}

/* -------------------------------------------------------------------------
 * Byte-key Put/Get
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashIndexPutGetBasic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    TEST_ASSERT_EQUAL_INT(0, rgHashIndexPut(&idx, &a, "hello", 5, 42));
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashIndexGet(&idx, "hello", 5, &v));
    TEST_ASSERT_EQUAL_UINT64(42, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexGetMissReturnsError(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    uint64_t v = 0xdeadbeef;
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexGet(&idx, "missing", 7, &v));
    TEST_ASSERT_EQUAL_UINT64(0xdeadbeef, v); /* untouched on miss */

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexPutOverwrites(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    rgHashIndexPut(&idx, &a, "k", 1, 10);
    rgHashIndexPut(&idx, &a, "k", 1, 20);

    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashIndexGet(&idx, "k", 1, &v));
    TEST_ASSERT_EQUAL_UINT64(20, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexDifferentLengthsDontCollide(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    rgHashIndexPut(&idx, &a, "abc",  3, 111);
    rgHashIndexPut(&idx, &a, "abcd", 4, 222);

    uint64_t v = 0;
    rgHashIndexGet(&idx, "abc",  3, &v); TEST_ASSERT_EQUAL_UINT64(111, v);
    rgHashIndexGet(&idx, "abcd", 4, &v); TEST_ASSERT_EQUAL_UINT64(222, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexZeroLengthKey(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    TEST_ASSERT_EQUAL_INT(0, rgHashIndexPut(&idx, &a, "", 0, 99));
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashIndexGet(&idx, "", 0, &v));
    TEST_ASSERT_EQUAL_UINT64(99, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexLongStringKeys(void)
{
    /* Big string keys: HashIndex's main use case. Memory per entry stays
     * constant regardless of key length. */
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    char buf[256];
    const int N = 100;
    for (int i = 0; i < N; ++i)
    {
        int len = snprintf(buf, sizeof(buf),
                           "this/is/a/sufficiently/long/asset/path/entry-%04d.png", i);
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexPut(&idx, &a, buf, (size_t)len, (uint64_t)(i + 1000)));
    }
    for (int i = 0; i < N; i += 7)
    {
        int len = snprintf(buf, sizeof(buf),
                           "this/is/a/sufficiently/long/asset/path/entry-%04d.png", i);
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexGet(&idx, buf, (size_t)len, &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)(i + 1000), v);
    }

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexManyKeys(void)
{
    Arena a;
    rgArenaCreate(&a, 4 * 1024 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    const int N = 5000;
    char buf[24];
    for (int i = 0; i < N; ++i)
    {
        int len = snprintf(buf, sizeof(buf), "key-%d", i);
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexPut(&idx, &a, buf, (size_t)len, (uint64_t)i));
    }
    /* Spot-check a sweep. */
    for (int i = 0; i < N; i += 13)
    {
        int len = snprintf(buf, sizeof(buf), "key-%d", i);
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexGet(&idx, buf, (size_t)len, &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i, v);
    }

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Pre-hashed API
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashIndexPutHGetHBasic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    TEST_ASSERT_EQUAL_INT(0, rgHashIndexPutH(&idx, &a, 0xAAAA, 0xBBBB, 42));
    TEST_ASSERT_EQUAL_INT(0, rgHashIndexPutH(&idx, &a, 0xCCCC, 0xDDDD, 99));

    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashIndexGetH(&idx, 0xAAAA, 0xBBBB, &v));
    TEST_ASSERT_EQUAL_UINT64(42, v);

    TEST_ASSERT_EQUAL_INT(0, rgHashIndexGetH(&idx, 0xCCCC, 0xDDDD, &v));
    TEST_ASSERT_EQUAL_UINT64(99, v);

    /* h1 match but h2 mismatch must NOT yield the entry. */
    v = 0xdeadbeef;
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexGetH(&idx, 0xAAAA, 0xFFFF, &v));
    TEST_ASSERT_EQUAL_UINT64(0xdeadbeef, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexPutHOverwrites(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    rgHashIndexPutH(&idx, &a, 100, 200, 1);
    rgHashIndexPutH(&idx, &a, 100, 200, 2);
    rgHashIndexPutH(&idx, &a, 100, 200, 3);

    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashIndexGetH(&idx, 100, 200, &v));
    TEST_ASSERT_EQUAL_UINT64(3, v);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Bulk pre-hashed API
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashIndexBatchHBasic(void)
{
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    const uint32_t N = 100;
    uint64_t h1s[100], h2s[100], vals[100];
    for (uint32_t i = 0; i < N; ++i)
    {
        h1s[i]  = i * 7u + 3u;
        h2s[i]  = i * 11u + 17u;
        vals[i] = i * 13u + 29u;
    }

    TEST_ASSERT_EQUAL_INT(0, rgHashIndexPutBatchH(&idx, &a, h1s, h2s, vals, N));

    uint64_t out[100];
    int      found[100];
    int32_t  hits = rgHashIndexGetBatchH(&idx, h1s, h2s, out, found, N);
    TEST_ASSERT_EQUAL_INT((int32_t)N, hits);
    for (uint32_t i = 0; i < N; ++i)
    {
        TEST_ASSERT_EQUAL_INT(1, found[i]);
        TEST_ASSERT_EQUAL_UINT64(vals[i], out[i]);
    }

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexBatchHHandlesMisses(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    /* Insert 50, look up 100 -- the second half should miss. */
    uint64_t h1s_in[50], h2s_in[50], vals[50];
    for (uint32_t i = 0; i < 50; ++i)
    {
        h1s_in[i] = i * 1000u + 1u;
        h2s_in[i] = i * 1000u + 2u;
        vals[i]   = i + 100u;
    }
    rgHashIndexPutBatchH(&idx, &a, h1s_in, h2s_in, vals, 50);

    uint64_t h1q[100], h2q[100], out[100];
    int      found[100];
    for (uint32_t i = 0; i < 100; ++i)
    {
        h1q[i] = i * 1000u + 1u;
        h2q[i] = i * 1000u + 2u;
        out[i] = 0xdeadbeef;
    }
    int32_t hits = rgHashIndexGetBatchH(&idx, h1q, h2q, out, found, 100);
    TEST_ASSERT_EQUAL_INT(50, hits);
    for (uint32_t i = 0; i < 50; ++i)
    {
        TEST_ASSERT_EQUAL_INT(1, found[i]);
        TEST_ASSERT_EQUAL_UINT64(i + 100u, out[i]);
    }
    for (uint32_t i = 50; i < 100; ++i)
    {
        TEST_ASSERT_EQUAL_INT(0, found[i]);
        TEST_ASSERT_EQUAL_UINT64(0xdeadbeef, out[i]);
    }

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * ForEach
 * ------------------------------------------------------------------------- */

typedef struct index_foreach_ctx
{
    int      count;
    uint64_t h1_xor;
    uint64_t h2_xor;
    uint64_t value_sum;
} index_foreach_ctx;

static int index_foreach_collect(uint64_t h1, uint64_t h2, uint64_t value, void* ud)
{
    index_foreach_ctx* c = (index_foreach_ctx*)ud;
    c->h1_xor    ^= h1;
    c->h2_xor    ^= h2;
    c->value_sum += value;
    c->count++;
    return 0;
}

void rgMemoryTest_hashIndexForEachVisitsEveryEntry(void)
{
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    const int N = 200;
    uint64_t expected_h1 = 0, expected_h2 = 0, expected_sum = 0;
    for (int i = 0; i < N; ++i)
    {
        uint64_t h1 = (uint64_t)(i * 7u + 3u);
        uint64_t h2 = (uint64_t)(i * 11u + 17u);
        uint64_t v  = (uint64_t)(i * 13u + 29u);
        rgHashIndexPutH(&idx, &a, h1, h2, v);
        expected_h1  ^= h1;
        expected_h2  ^= h2;
        expected_sum += v;
    }

    index_foreach_ctx c = {0, 0, 0, 0};
    uint64_t visited = rgHashIndexForEach(&idx, index_foreach_collect, &c);

    TEST_ASSERT_EQUAL_UINT64((uint64_t)N, visited);
    TEST_ASSERT_EQUAL_INT(N, c.count);
    TEST_ASSERT_EQUAL_UINT64(expected_h1,  c.h1_xor);
    TEST_ASSERT_EQUAL_UINT64(expected_h2,  c.h2_xor);
    TEST_ASSERT_EQUAL_UINT64(expected_sum, c.value_sum);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashIndexForEachEmpty(void)
{
    HashIndex idx;
    rgHashIndexInit(&idx);
    index_foreach_ctx c = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT64(0, rgHashIndexForEach(&idx, index_foreach_collect, &c));
    TEST_ASSERT_EQUAL_INT(0, c.count);
}

/* -------------------------------------------------------------------------
 * Multi-arena coexistence (proves nodes from different arenas link via
 * absolute pointers, same as HashTrie).
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashIndexMultiArenaCoexistence(void)
{
    Arena a1, a2;
    rgArenaCreate(&a1, 64 * 1024);
    rgArenaCreate(&a2, 64 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    for (int i = 0; i < 50; ++i)
    {
        char key[16];
        int  len = snprintf(key, sizeof(key), "a-%d", i);
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexPut(&idx, &a1, key, (size_t)len, (uint64_t)i));

        len = snprintf(key, sizeof(key), "b-%d", i);
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexPut(&idx, &a2, key, (size_t)len,
                                                (uint64_t)(i + 1000)));
    }
    for (int i = 0; i < 50; ++i)
    {
        char key[16];
        int  len = snprintf(key, sizeof(key), "a-%d", i);
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexGet(&idx, key, (size_t)len, &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i, v);

        len = snprintf(key, sizeof(key), "b-%d", i);
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexGet(&idx, key, (size_t)len, &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)(i + 1000), v);
    }

    rgArenaDestroy(&a1);
    rgArenaDestroy(&a2);
}

/* -------------------------------------------------------------------------
 * Digest-collision resilience (descent reseed).
 *
 * Insert many entries that all share h1 -- which alone drives descent -- so
 * without the bit-exhaustion reseed they would form a single linear child[0]
 * chain as deep as the entry count. The reseed remixes h2 once the h1 bits
 * run out, dispersing them back into a shallow ~log4 trie.
 *
 * Two observable guarantees:
 *   - Every entry round-trips via GetH (correct under any depth).
 *   - ForEach visits ALL of them. ForEach is a depth-64-capped DFS, so a
 *     chain deeper than 64 would silently drop entries; visiting all N is
 *     the proof the reseed kept the structure shallow.
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashIndexSharedH1StaysShallow(void)
{
    Arena a;
    rgArenaCreate(&a, 4 * 1024 * 1024);
    HashIndex idx;
    rgHashIndexInit(&idx);

    const uint32_t N  = 1000;
    const uint64_t H1 = 0x0123456789abcdefull; /* identical for every entry. */

    for (uint32_t i = 0; i < N; ++i)
    {
        /* h2 distinct and non-zero; value derived so the verifier is exact. */
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexPutH(&idx, &a, H1, (uint64_t)i + 1u,
                                                 (uint64_t)i * 3u + 7u));
    }

    for (uint32_t i = 0; i < N; ++i)
    {
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashIndexGetH(&idx, H1, (uint64_t)i + 1u, &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i * 3u + 7u, v);
    }

    /* A shared-h1 lookup with an unused h2 must still miss -- descent must
     * not stop early just because h1 matches a node along the chain. */
    uint64_t v = 0xdeadbeef;
    TEST_ASSERT_EQUAL_INT(-1, rgHashIndexGetH(&idx, H1, (uint64_t)N + 1000u, &v));
    TEST_ASSERT_EQUAL_UINT64(0xdeadbeef, v);

    index_foreach_ctx c = {0, 0, 0, 0};
    uint64_t visited = rgHashIndexForEach(&idx, index_foreach_collect, &c);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)N, visited);
    TEST_ASSERT_EQUAL_INT((int)N, c.count);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_HashIndex(void)
{
    RUN_TEST(rgMemoryTest_hashIndexZeroInitIsEmpty);
    RUN_TEST(rgMemoryTest_hashIndexNullPointerIsSafe);
    RUN_TEST(rgMemoryTest_hashIndexPutNullArenaFails);

    RUN_TEST(rgMemoryTest_hashIndexPutGetBasic);
    RUN_TEST(rgMemoryTest_hashIndexGetMissReturnsError);
    RUN_TEST(rgMemoryTest_hashIndexPutOverwrites);
    RUN_TEST(rgMemoryTest_hashIndexDifferentLengthsDontCollide);
    RUN_TEST(rgMemoryTest_hashIndexZeroLengthKey);
    RUN_TEST(rgMemoryTest_hashIndexLongStringKeys);
    RUN_TEST(rgMemoryTest_hashIndexManyKeys);

    RUN_TEST(rgMemoryTest_hashIndexPutHGetHBasic);
    RUN_TEST(rgMemoryTest_hashIndexPutHOverwrites);

    RUN_TEST(rgMemoryTest_hashIndexBatchHBasic);
    RUN_TEST(rgMemoryTest_hashIndexBatchHHandlesMisses);

    RUN_TEST(rgMemoryTest_hashIndexForEachVisitsEveryEntry);
    RUN_TEST(rgMemoryTest_hashIndexForEachEmpty);

    RUN_TEST(rgMemoryTest_hashIndexMultiArenaCoexistence);

    RUN_TEST(rgMemoryTest_hashIndexSharedH1StaysShallow);
}
