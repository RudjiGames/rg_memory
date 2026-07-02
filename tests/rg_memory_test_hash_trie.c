/*
 * Imagus Engine -  Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Tests are single-threaded only at this layer: they verify that the
 * HashTrie's structural behaviour matches HashMap when accessed from one
 * thread. Real concurrent stress (multi-writer Put, racing CAS) needs a
 * threading wrapper we don't have yet; add a separate harness when that
 * lands.
 */

#include <rg_memory_test_pch.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Zero-init / NULL safety
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashTrieZeroInitIsEmpty(void)
{
    /* With absolute-pointer slots there is nothing for Init to set up
     * beyond zeroing the top-level index array, so a memset'd HashTrie is
     * already a valid empty trie. Get must miss; Put with a NULL arena
     * must reject. */
    HashTrie t;
    memset(&t, 0, sizeof(t));
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, rgHashTriePut(&t, NULL, "k", 1, 42));
    TEST_ASSERT_EQUAL_INT(-1, rgHashTrieGet(&t, "k", 1, &v));
}

void rgMemoryTest_hashTrieNullPointerIsSafe(void)
{
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, rgHashTrieInit(NULL));
    TEST_ASSERT_EQUAL_INT(-1, rgHashTriePut(NULL, NULL, "k", 1, 42));
    TEST_ASSERT_EQUAL_INT(-1, rgHashTrieGet(NULL, "k", 1, &v));
}

/* -------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashTrieInitBasic(void)
{
    HashTrie t;
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieInit(&t));
}

void rgMemoryTest_hashTriePutNullArenaFails(void)
{
    HashTrie t;
    rgHashTrieInit(&t);
    TEST_ASSERT_EQUAL_INT(-1, rgHashTriePut(&t, NULL, "k", 1, 42));
}

void rgMemoryTest_hashTriePutUninitArenaFails(void)
{
    Arena a;
    memset(&a, 0, sizeof(a));
    HashTrie t;
    rgHashTrieInit(&t);
    TEST_ASSERT_EQUAL_INT(-1, rgHashTriePut(&t, &a, "k", 1, 42));
}

void rgMemoryTest_hashTrieRejectsOversizedKeyLen(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    /* Nodes store key length as uint32_t: lengths above UINT32_MAX must be
     * rejected up front, before the key bytes are ever dereferenced. */
    char key = 'x';
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(-1, rgHashTriePut(&t, &a, &key, (uint64_t)UINT32_MAX + 1ull, 42));
    TEST_ASSERT_EQUAL_INT(-1, rgHashTrieGet(&t, &key, (uint64_t)UINT32_MAX + 1ull, &v));

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Put / Get
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashTriePutGetBasic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    TEST_ASSERT_EQUAL_INT(0, rgHashTriePut(&t, &a, "hello", 5, 42));
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, "hello", 5, &v));
    TEST_ASSERT_EQUAL_UINT64(42, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTrieGetMissReturnsError(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    uint64_t v = 0xdeadbeef;
    TEST_ASSERT_EQUAL_INT(-1, rgHashTrieGet(&t, "missing", 7, &v));
    TEST_ASSERT_EQUAL_UINT64(0xdeadbeef, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTriePutOverwrites(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    rgHashTriePut(&t, &a, "k", 1, 10);
    rgHashTriePut(&t, &a, "k", 1, 20);

    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, "k", 1, &v));
    TEST_ASSERT_EQUAL_UINT64(20, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTrieDifferentLengthsDontCollide(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    rgHashTriePut(&t, &a, "abc",  3, 111);
    rgHashTriePut(&t, &a, "abcd", 4, 222);

    uint64_t v = 0;
    rgHashTrieGet(&t, "abc",  3, &v); TEST_ASSERT_EQUAL_UINT64(111, v);
    rgHashTrieGet(&t, "abcd", 4, &v); TEST_ASSERT_EQUAL_UINT64(222, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTrieZeroLengthKey(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    TEST_ASSERT_EQUAL_INT(0, rgHashTriePut(&t, &a, "", 0, 99));
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, "", 0, &v));
    TEST_ASSERT_EQUAL_UINT64(99, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTrieManyKeys(void)
{
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    const int N = 1000;
    for (int i = 0; i < N; ++i)
    {
        char key[16];
        int len = 0;
        int n = i;
        do { key[len++] = (char)('0' + (n % 10)); n /= 10; } while (n != 0);
        TEST_ASSERT_EQUAL_INT(0, rgHashTriePut(&t, &a, key, (size_t)len, (uint64_t)i));
    }
    for (int i = 0; i < N; i += 137)
    {
        char key[16];
        int len = 0;
        int n = i;
        do { key[len++] = (char)('0' + (n % 10)); n /= 10; } while (n != 0);
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, key, (size_t)len, &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i, v);
    }

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTrieMultiArenaCoexistence(void)
{
    /* Two arenas contribute to the same trie. Every key must round-trip
     * via Get even though the nodes live in different memory regions --
     * this is the key property the 64-bit-absolute-slot design enables. */
    Arena a1, a2;
    rgArenaCreate(&a1, 64 * 1024);
    rgArenaCreate(&a2, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    /* Interleave Puts so neither arena ends up "early" or "late" in the
     * trie -- exercises descent crossing arena boundaries. */
    for (int i = 0; i < 50; ++i)
    {
        char key[16];
        int  len = snprintf(key, sizeof(key), "a-%d", i);
        TEST_ASSERT_EQUAL_INT(0, rgHashTriePut(&t, &a1, key, (size_t)len, (uint64_t)i));

        len = snprintf(key, sizeof(key), "b-%d", i);
        TEST_ASSERT_EQUAL_INT(0, rgHashTriePut(&t, &a2, key, (size_t)len,
                                               (uint64_t)(i + 1000)));
    }
    for (int i = 0; i < 50; ++i)
    {
        char key[16];
        int  len = snprintf(key, sizeof(key), "a-%d", i);
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, key, (size_t)len, &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i, v);

        len = snprintf(key, sizeof(key), "b-%d", i);
        TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, key, (size_t)len, &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)(i + 1000), v);
    }

    rgArenaDestroy(&a1);
    rgArenaDestroy(&a2);
}

/* -------------------------------------------------------------------------
 * U64-specialised API
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashTrieU64Basic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    TEST_ASSERT_EQUAL_INT(0, rgHashTriePutU64(&t, &a, 42, 100));
    TEST_ASSERT_EQUAL_INT(0, rgHashTriePutU64(&t, &a, 99, 200));

    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieGetU64(&t, 42, &v));
    TEST_ASSERT_EQUAL_UINT64(100, v);
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieGetU64(&t, 99, &v));
    TEST_ASSERT_EQUAL_UINT64(200, v);

    v = 0xdeadbeef;
    TEST_ASSERT_EQUAL_INT(-1, rgHashTrieGetU64(&t, 7, &v));
    TEST_ASSERT_EQUAL_UINT64(0xdeadbeef, v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTrieU64InteropsWithGeneric(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    rgHashTriePutU64(&t, &a, 12345, 67890);

    uint64_t key64 = 12345;
    uint64_t v = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, &key64, sizeof(key64), &v));
    TEST_ASSERT_EQUAL_UINT64(67890, v);

    key64 = 9999;
    rgHashTriePut(&t, &a, &key64, sizeof(key64), 11111);
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieGetU64(&t, 9999, &v));
    TEST_ASSERT_EQUAL_UINT64(11111, v);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * ForEach iteration
 * ------------------------------------------------------------------------- */

typedef struct trie_foreach_ctx
{
    int      count;
    uint64_t key_xor;
    uint64_t value_sum;
} trie_foreach_ctx;

static int trie_foreach_collect(const void* key, size_t keyLen, uint64_t value, void* ud)
{
    trie_foreach_ctx* c = (trie_foreach_ctx*)ud;
    TEST_ASSERT_EQUAL_size_t(8, keyLen);
    uint64_t k = 0;
    const uint8_t* p = (const uint8_t*)key;
    for (size_t i = 0; i < 8; ++i) k |= (uint64_t)p[i] << (i * 8);
    c->key_xor   ^= k;
    c->value_sum += value;
    c->count++;
    return 0;
}

void rgMemoryTest_hashTrieForEachVisitsEveryEntry(void)
{
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    const int N = 200;
    uint64_t expected_xor = 0;
    uint64_t expected_sum = 0;
    for (int i = 0; i < N; ++i)
    {
        rgHashTriePutU64(&t, &a, (uint64_t)i, (uint64_t)(i * 7 + 13));
        expected_xor ^= (uint64_t)i;
        expected_sum += (uint64_t)(i * 7 + 13);
    }

    trie_foreach_ctx c = {0, 0, 0};
    uint64_t visited = rgHashTrieForEach(&t, trie_foreach_collect, &c);

    TEST_ASSERT_EQUAL_UINT64((uint64_t)N, visited);
    TEST_ASSERT_EQUAL_INT(N, c.count);
    TEST_ASSERT_EQUAL_UINT64(expected_xor, c.key_xor);
    TEST_ASSERT_EQUAL_UINT64(expected_sum, c.value_sum);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTrieForEachEmpty(void)
{
    HashTrie t;
    rgHashTrieInit(&t);
    trie_foreach_ctx c = {0, 0, 0};
    TEST_ASSERT_EQUAL_UINT64(0, rgHashTrieForEach(&t, trie_foreach_collect, &c));
    TEST_ASSERT_EQUAL_INT(0, c.count);
}

/* -------------------------------------------------------------------------
 * Bulk uint64 API
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashTriePutGetBatchU64Basic(void)
{
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    const uint32_t N = 100;
    uint64_t keys[100], vals[100];
    for (uint32_t i = 0; i < N; ++i) { keys[i] = i * 7u + 3u; vals[i] = i * 11u + 17u; }

    TEST_ASSERT_EQUAL_INT(0, rgHashTriePutBatchU64(&t, &a, keys, vals, N));

    uint64_t out[100];
    int      found[100];
    int32_t  hits = rgHashTrieGetBatchU64(&t, keys, out, found, N);
    TEST_ASSERT_EQUAL_INT((int32_t)N, hits);
    for (uint32_t i = 0; i < N; ++i)
    {
        TEST_ASSERT_EQUAL_INT(1, found[i]);
        TEST_ASSERT_EQUAL_UINT64(vals[i], out[i]);
    }

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashTrieGetBatchU64HandlesMisses(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashTrie t;
    rgHashTrieInit(&t);

    for (uint32_t i = 0; i < 50; ++i) rgHashTriePutU64(&t, &a, i, i * 2u);

    uint64_t keys[100], out[100];
    int      found[100];
    for (uint32_t i = 0; i < 100; ++i) { keys[i] = i; out[i] = 0xdeadbeefu; }

    int32_t hits = rgHashTrieGetBatchU64(&t, keys, out, found, 100);
    TEST_ASSERT_EQUAL_INT(50, hits);

    for (uint32_t i = 0; i < 50; ++i)
    {
        TEST_ASSERT_EQUAL_INT(1, found[i]);
        TEST_ASSERT_EQUAL_UINT64(i * 2u, out[i]);
    }
    for (uint32_t i = 50; i < 100; ++i)
    {
        TEST_ASSERT_EQUAL_INT(0, found[i]);
        TEST_ASSERT_EQUAL_UINT64(0xdeadbeefu, out[i]);
    }

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * NOCOPY mode: keys are referenced, not copied.
 * ------------------------------------------------------------------------- */

static const char* g_nocopyKeys[5] = {
    "alpha", "bravo", "charlie", "delta",
    "a-deliberately-long-and-stable-asset-path/textures/diffuse.png"
};

typedef struct nocopy_ctx
{
    int count;
    int identity[5]; /* set when the callback key pointer == the caller's. */
} nocopy_ctx;

static int nocopy_collect(const void* key, size_t keyLen, uint64_t value, void* ud)
{
    nocopy_ctx* c = (nocopy_ctx*)ud;
    (void)keyLen; (void)value;
    c->count++;
    /* NOCOPY must report the ORIGINAL caller pointer, not an arena copy. */
    for (int i = 0; i < 5; ++i)
    {
        if (key == (const void*)g_nocopyKeys[i]) c->identity[i] = 1;
    }
    return 0;
}

void rgMemoryTest_hashTrieNoCopyReferencesKeys(void)
{
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashTrie t;
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieInitEx(&t, RGM_HASHTRIE_FLAG_NOCOPY));

    for (int i = 0; i < 5; ++i)
    {
        TEST_ASSERT_EQUAL_INT(0, rgHashTriePut(&t, &a, g_nocopyKeys[i],
                                               strlen(g_nocopyKeys[i]), (uint64_t)i));
    }

    /* Round-trip. */
    for (int i = 0; i < 5; ++i)
    {
        uint64_t v = 0;
        TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, g_nocopyKeys[i],
                                               strlen(g_nocopyKeys[i]), &v));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i, v);
    }

    /* Miss + overwrite-keeps-one-entry behave like copy mode. */
    uint64_t miss = 123;
    TEST_ASSERT_EQUAL_INT(-1, rgHashTrieGet(&t, "absent", 6, &miss));
    TEST_ASSERT_EQUAL_INT(0, rgHashTriePut(&t, &a, g_nocopyKeys[1],
                                           strlen(g_nocopyKeys[1]), 555));
    uint64_t ov = 0;
    TEST_ASSERT_EQUAL_INT(0, rgHashTrieGet(&t, g_nocopyKeys[1], strlen(g_nocopyKeys[1]), &ov));
    TEST_ASSERT_EQUAL_UINT64(555, ov);

    /* ForEach proves no copy was made: the reported key pointers are the
     * caller's own pointers, and there are exactly 5 distinct entries. */
    nocopy_ctx c = {0, {0, 0, 0, 0, 0}};
    uint64_t visited = rgHashTrieForEach(&t, nocopy_collect, &c);
    TEST_ASSERT_EQUAL_UINT64(5, visited);
    TEST_ASSERT_EQUAL_INT(5, c.count);
    for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_INT(1, c.identity[i]);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_HashTrie(void)
{
    RUN_TEST(rgMemoryTest_hashTrieZeroInitIsEmpty);
    RUN_TEST(rgMemoryTest_hashTrieNullPointerIsSafe);

    RUN_TEST(rgMemoryTest_hashTrieInitBasic);
    RUN_TEST(rgMemoryTest_hashTriePutNullArenaFails);
    RUN_TEST(rgMemoryTest_hashTriePutUninitArenaFails);
    RUN_TEST(rgMemoryTest_hashTrieRejectsOversizedKeyLen);

    RUN_TEST(rgMemoryTest_hashTriePutGetBasic);
    RUN_TEST(rgMemoryTest_hashTrieGetMissReturnsError);
    RUN_TEST(rgMemoryTest_hashTriePutOverwrites);
    RUN_TEST(rgMemoryTest_hashTrieDifferentLengthsDontCollide);
    RUN_TEST(rgMemoryTest_hashTrieZeroLengthKey);
    RUN_TEST(rgMemoryTest_hashTrieManyKeys);
    RUN_TEST(rgMemoryTest_hashTrieMultiArenaCoexistence);

    RUN_TEST(rgMemoryTest_hashTrieU64Basic);
    RUN_TEST(rgMemoryTest_hashTrieU64InteropsWithGeneric);

    RUN_TEST(rgMemoryTest_hashTrieForEachVisitsEveryEntry);
    RUN_TEST(rgMemoryTest_hashTrieForEachEmpty);

    RUN_TEST(rgMemoryTest_hashTriePutGetBatchU64Basic);
    RUN_TEST(rgMemoryTest_hashTrieGetBatchU64HandlesMisses);

    RUN_TEST(rgMemoryTest_hashTrieNoCopyReferencesKeys);
}
