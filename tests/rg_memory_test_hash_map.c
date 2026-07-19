/*
 * Imagus Engine -  Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 */

#include <rg_memory_test_pch.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Zero-init / NULL safety
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapZeroInitIsInert(void)
{
    HashMap m;
    memset(&m, 0, sizeof(m));
    TEST_ASSERT_NULL(rgHashMapPut(&m, "k", 1));
    TEST_ASSERT_NULL(rgHashMapGet(&m, "k", 1));
}

void rgMemoryTest_hashMapNullPointerIsSafe(void)
{
    TEST_ASSERT_EQUAL_INT(-1, rgHashMapInit(NULL, NULL));
    TEST_ASSERT_NULL(rgHashMapPut(NULL, "k", 1));
    TEST_ASSERT_NULL(rgHashMapGet(NULL, "k", 1));
}

/* -------------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapInitBasic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    TEST_ASSERT_EQUAL_INT(0, rgHashMapInit(&m, &a));
    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapInitNullArenaFails(void)
{
    HashMap m;
    TEST_ASSERT_EQUAL_INT(-1, rgHashMapInit(&m, NULL));
}

void rgMemoryTest_hashMapInitUninitArenaFails(void)
{
    Arena a;
    memset(&a, 0, sizeof(a));
    HashMap m;
    TEST_ASSERT_EQUAL_INT(-1, rgHashMapInit(&m, &a));
}

void rgMemoryTest_hashMapRejectsOversizedKeyLen(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    TEST_ASSERT_EQUAL_INT(0, rgHashMapInit(&m, &a));

    /* Nodes store key length as uint32_t: lengths above UINT32_MAX must be
     * rejected up front, before the key bytes are ever dereferenced. */
    char key = 'x';
    TEST_ASSERT_NULL(rgHashMapPut(&m, &key, (uint64_t)UINT32_MAX + 1ull));
    TEST_ASSERT_NULL(rgHashMapGet(&m, &key, (uint64_t)UINT32_MAX + 1ull));

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Put / Get
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapPutGetBasic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    uint64_t* p = rgHashMapPut(&m, "hello", 5);
    TEST_ASSERT_NOT_NULL(p);
    *p = 42;
    uint64_t* v = rgHashMapGet(&m, "hello", 5);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(42, *v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapPutZeroInitsNewValue(void)
{
    /* A freshly inserted entry's value slot starts zeroed; the caller is
     * responsible for writing the real value through the returned pointer. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    uint64_t* p = rgHashMapPut(&m, "fresh", 5);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT64(0, *p);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapGetMissReturnsNull(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    TEST_ASSERT_NULL(rgHashMapGet(&m, "missing", 7));

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapPutReturnsStableSlotAndOverwrites(void)
{
    /* Put is an upsert: a second Put with the same key returns a pointer
     * to the SAME existing value slot (left untouched), and writing
     * through it overwrites the value a Get observes. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    uint64_t* p1 = rgHashMapPut(&m, "k", 1);
    TEST_ASSERT_NOT_NULL(p1);
    *p1 = 10;

    uint64_t* p2 = rgHashMapPut(&m, "k", 1);
    TEST_ASSERT_EQUAL_PTR(p1, p2);
    TEST_ASSERT_EQUAL_UINT64(10, *p2); /* existing value untouched by Put */
    *p2 = 20;

    uint64_t* v = rgHashMapGet(&m, "k", 1);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(20, *v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapDifferentLengthsDontCollide(void)
{
    /* "abc" and "abcd" share a prefix; keys must compare by length too. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    *rgHashMapPut(&m, "abc",  3) = 111;
    *rgHashMapPut(&m, "abcd", 4) = 222;

    uint64_t* v = rgHashMapGet(&m, "abc",  3);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(111, *v);
    v = rgHashMapGet(&m, "abcd", 4);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(222, *v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapSameLengthDifferentBytesDontCollide(void)
{
    /* "abc" vs "abd": same length, differ in last byte. Both must round-trip. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    *rgHashMapPut(&m, "abc", 3) = 1;
    *rgHashMapPut(&m, "abd", 3) = 2;

    TEST_ASSERT_EQUAL_UINT64(1, *rgHashMapGet(&m, "abc", 3));
    TEST_ASSERT_EQUAL_UINT64(2, *rgHashMapGet(&m, "abd", 3));

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapZeroLengthKey(void)
{
    /* A zero-length key is a valid degenerate case: only one such entry
     * exists, sitting at the root by virtue of hash(""). */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    uint64_t* p = rgHashMapPut(&m, "", 0);
    TEST_ASSERT_NOT_NULL(p);
    *p = 99;
    uint64_t* v = rgHashMapGet(&m, "", 0);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(99, *v);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapGetProbesMembership(void)
{
    /* Get returns a non-NULL pointer for a present key and NULL for an
     * absent one, so it doubles as a membership probe. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    *rgHashMapPut(&m, "k", 1) = 7;

    TEST_ASSERT_NOT_NULL(rgHashMapGet(&m, "k", 1));
    TEST_ASSERT_NULL(rgHashMapGet(&m, "x", 1));

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapManyKeys(void)
{
    /* Insert many distinct keys; spot-check a handful round-trip. Drives
     * trie depth past the first few levels so child[] indexing is real. */
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    const int N = 1000;
    for (int i = 0; i < N; ++i)
    {
        char key[16];
        int len = 0;
        int n = i;
        do { key[len++] = (char)('0' + (n % 10)); n /= 10; } while (n != 0);
        uint64_t* p = rgHashMapPut(&m, key, (size_t)len);
        TEST_ASSERT_NOT_NULL(p);
        *p = (uint64_t)i;
    }
    /* Pull a few back. */
    for (int i = 0; i < N; i += 137)
    {
        char key[16];
        int len = 0;
        int n = i;
        do { key[len++] = (char)('0' + (n % 10)); n /= 10; } while (n != 0);
        uint64_t* v = rgHashMapGet(&m, key, (size_t)len);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i, *v);
    }

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapPointerValueRoundTrip(void)
{
    /* Demonstrate the (uint64_t)(uintptr_t) cast pattern documented in
     * the header -- the trie's value slot doubles as a pointer holder. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    int target = 12345;
    *rgHashMapPut(&m, "ptr", 3) = (uint64_t)(uintptr_t)&target;

    uint64_t* v = rgHashMapGet(&m, "ptr", 3);
    TEST_ASSERT_NOT_NULL(v);
    int* recovered = (int*)(uintptr_t)*v;
    TEST_ASSERT_EQUAL_PTR(&target, recovered);
    TEST_ASSERT_EQUAL_INT(12345, *recovered);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * U64-specialised API
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapU64Basic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    uint64_t* p = rgHashMapPutU64(&m, 42);
    TEST_ASSERT_NOT_NULL(p);
    *p = 100;
    p = rgHashMapPutU64(&m, 99);
    TEST_ASSERT_NOT_NULL(p);
    *p = 200;

    uint64_t* v = rgHashMapGetU64(&m, 42);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(100, *v);
    v = rgHashMapGetU64(&m, 99);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(200, *v);

    /* Miss returns NULL. */
    TEST_ASSERT_NULL(rgHashMapGetU64(&m, 7));

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapU64InteropsWithGeneric(void)
{
    /* A key put via U64 must be visible via generic Get and vice-versa --
     * the U64 path stores the key as 8 bytes interpreted as a uint64,
     * which is exactly what the generic byte path produces. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    *rgHashMapPutU64(&m, 12345) = 67890;

    uint64_t key64 = 12345;
    uint64_t* v = rgHashMapGet(&m, &key64, sizeof(key64));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(67890, *v);

    /* Reverse direction. */
    key64 = 9999;
    *rgHashMapPut(&m, &key64, sizeof(key64)) = 11111;
    v = rgHashMapGetU64(&m, 9999);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT64(11111, *v);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * ForEach iteration
 * ------------------------------------------------------------------------- */

typedef struct foreach_ctx
{
    int      count;
    uint64_t key_xor;
    uint64_t value_sum;
    int      stop_at;
} foreach_ctx;

static int foreach_collect(const void* key, uint64_t keyLen, uint64_t value, void* ud)
{
    foreach_ctx* c = (foreach_ctx*)ud;
    TEST_ASSERT_EQUAL_size_t(8, keyLen);
    uint64_t k = 0;
    /* keyLen == 8 in this test; copy out the raw bytes. */
    const uint8_t* p = (const uint8_t*)key;
    for (size_t i = 0; i < 8; ++i) k |= (uint64_t)p[i] << (i * 8);
    c->key_xor   ^= k;
    c->value_sum += value;
    c->count++;
    if (c->stop_at > 0 && c->count >= c->stop_at) return 1;
    return 0;
}

void rgMemoryTest_hashMapForEachVisitsEveryEntry(void)
{
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    const int N = 200;
    uint64_t expected_xor = 0;
    uint64_t expected_sum = 0;
    for (int i = 0; i < N; ++i)
    {
        *rgHashMapPutU64(&m, (uint64_t)i) = (uint64_t)(i * 7 + 13);
        expected_xor ^= (uint64_t)i;
        expected_sum += (uint64_t)(i * 7 + 13);
    }

    foreach_ctx c = {0, 0, 0, 0};
    uint64_t visited = rgHashMapForEach(&m, foreach_collect, &c);

    TEST_ASSERT_EQUAL_UINT64((uint64_t)N, visited);
    TEST_ASSERT_EQUAL_INT(N, c.count);
    TEST_ASSERT_EQUAL_UINT64(expected_xor, c.key_xor);
    TEST_ASSERT_EQUAL_UINT64(expected_sum, c.value_sum);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapForEachEarlyStop(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    for (int i = 0; i < 50; ++i) *rgHashMapPutU64(&m, (uint64_t)i) = (uint64_t)i;

    foreach_ctx c = {0, 0, 0, 10};
    uint64_t visited = rgHashMapForEach(&m, foreach_collect, &c);

    TEST_ASSERT_EQUAL_UINT64(10, visited);
    TEST_ASSERT_EQUAL_INT(10, c.count);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapForEachEmpty(void)
{
    Arena a;
    rgArenaCreate(&a, 4 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    foreach_ctx c = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT64(0, rgHashMapForEach(&m, foreach_collect, &c));
    TEST_ASSERT_EQUAL_INT(0, c.count);

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapForEachNullCallbackIsSafe(void)
{
    Arena a;
    rgArenaCreate(&a, 4 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);
    *rgHashMapPutU64(&m, 1) = 2;
    TEST_ASSERT_EQUAL_UINT64(0, rgHashMapForEach(&m, NULL, NULL));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Bulk uint64 API
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapPutGetBatchU64Basic(void)
{
    Arena a;
    rgArenaCreate(&a, 1 * 1024 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    const uint32_t N = 100;
    uint64_t keys[100], vals[100];
    for (uint32_t i = 0; i < N; ++i) { keys[i] = i * 7u + 3u; vals[i] = i * 11u + 17u; }

    TEST_ASSERT_EQUAL_INT(0, rgHashMapPutBatchU64(&m, keys, vals, N));

    uint64_t out[100];
    int      found[100];
    int32_t  hits = rgHashMapGetBatchU64(&m, keys, out, found, N);
    TEST_ASSERT_EQUAL_INT((int32_t)N, hits);
    for (uint32_t i = 0; i < N; ++i)
    {
        TEST_ASSERT_EQUAL_INT(1, found[i]);
        TEST_ASSERT_EQUAL_UINT64(vals[i], out[i]);
    }

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapGetBatchU64HandlesMisses(void)
{
    /* Half of the queried keys are inserted; the other half must miss
     * cleanly without touching out values. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    for (uint32_t i = 0; i < 50; ++i) *rgHashMapPutU64(&m, i) = i * 2u;

    uint64_t keys[100], out[100];
    int      found[100];
    for (uint32_t i = 0; i < 100; ++i) { keys[i] = i; out[i] = 0xdeadbeefu; }

    int32_t hits = rgHashMapGetBatchU64(&m, keys, out, found, 100);
    TEST_ASSERT_EQUAL_INT(50, hits);

    for (uint32_t i = 0; i < 50; ++i)
    {
        TEST_ASSERT_EQUAL_INT(1, found[i]);
        TEST_ASSERT_EQUAL_UINT64(i * 2u, out[i]);
    }
    for (uint32_t i = 50; i < 100; ++i)
    {
        TEST_ASSERT_EQUAL_INT(0, found[i]);
        TEST_ASSERT_EQUAL_UINT64(0xdeadbeefu, out[i]); /* untouched on miss */
    }

    rgArenaDestroy(&a);
}

void rgMemoryTest_hashMapGetBatchU64NullOutFound(void)
{
    /* outFound is optional. With it NULL, the function still returns
     * the correct hit count and writes outValues on hits. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    HashMap m;
    rgHashMapInit(&m, &a);

    for (uint32_t i = 0; i < 20; ++i) *rgHashMapPutU64(&m, i) = i + 100u;

    uint64_t keys[20], out[20];
    for (uint32_t i = 0; i < 20; ++i) { keys[i] = i; out[i] = 0; }

    int32_t hits = rgHashMapGetBatchU64(&m, keys, out, NULL, 20);
    TEST_ASSERT_EQUAL_INT(20, hits);
    for (uint32_t i = 0; i < 20; ++i) TEST_ASSERT_EQUAL_UINT64(i + 100u, out[i]);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_HashMap(void)
{
    RUN_TEST(rgMemoryTest_hashMapZeroInitIsInert);
    RUN_TEST(rgMemoryTest_hashMapNullPointerIsSafe);

    RUN_TEST(rgMemoryTest_hashMapInitBasic);
    RUN_TEST(rgMemoryTest_hashMapInitNullArenaFails);
    RUN_TEST(rgMemoryTest_hashMapInitUninitArenaFails);
    RUN_TEST(rgMemoryTest_hashMapRejectsOversizedKeyLen);

    RUN_TEST(rgMemoryTest_hashMapPutGetBasic);
    RUN_TEST(rgMemoryTest_hashMapPutZeroInitsNewValue);
    RUN_TEST(rgMemoryTest_hashMapGetMissReturnsNull);
    RUN_TEST(rgMemoryTest_hashMapPutReturnsStableSlotAndOverwrites);
    RUN_TEST(rgMemoryTest_hashMapDifferentLengthsDontCollide);
    RUN_TEST(rgMemoryTest_hashMapSameLengthDifferentBytesDontCollide);
    RUN_TEST(rgMemoryTest_hashMapZeroLengthKey);
    RUN_TEST(rgMemoryTest_hashMapGetProbesMembership);
    RUN_TEST(rgMemoryTest_hashMapManyKeys);
    RUN_TEST(rgMemoryTest_hashMapPointerValueRoundTrip);

    RUN_TEST(rgMemoryTest_hashMapU64Basic);
    RUN_TEST(rgMemoryTest_hashMapU64InteropsWithGeneric);

    RUN_TEST(rgMemoryTest_hashMapForEachVisitsEveryEntry);
    RUN_TEST(rgMemoryTest_hashMapForEachEarlyStop);
    RUN_TEST(rgMemoryTest_hashMapForEachEmpty);
    RUN_TEST(rgMemoryTest_hashMapForEachNullCallbackIsSafe);

    RUN_TEST(rgMemoryTest_hashMapPutGetBatchU64Basic);
    RUN_TEST(rgMemoryTest_hashMapGetBatchU64HandlesMisses);
    RUN_TEST(rgMemoryTest_hashMapGetBatchU64NullOutFound);
}
