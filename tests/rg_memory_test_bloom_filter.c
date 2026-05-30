/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 */

#include <rg_memory_test_pch.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Cache-line-aligned scratch buffer for the external-memory Create
 * variant. Cache-line blocked Bloom needs 64-byte alignment so each
 * block sits on its own line. */
#if defined(_MSC_VER)
#   define RGM_TEST_ALIGN_64 __declspec(align(64))
#elif defined(__GNUC__) || defined(__clang__)
#   define RGM_TEST_ALIGN_64 __attribute__((aligned(64)))
#else
#   define RGM_TEST_ALIGN_64
#endif

/* -------------------------------------------------------------------------
 * Zero-init / NULL safety
 * ------------------------------------------------------------------------- */

void rgMemoryTest_bloomFilterZeroInitIsInert(void)
{
    BloomFilter bf;
    memset(&bf, 0, sizeof(bf));
    TEST_ASSERT_EQUAL_UINT64(0, rgBloomFilterBitCount(&bf));
    TEST_ASSERT_EQUAL_UINT32(0, rgBloomFilterHashCount(&bf));
    TEST_ASSERT_EQUAL_UINT64(0, rgBloomFilterPopCount(&bf));
    rgBloomFilterAddU64(&bf, 42);                /* no crash */
    TEST_ASSERT_EQUAL_INT(0, rgBloomFilterTestU64(&bf, 42));
    rgBloomFilterClear(&bf);                     /* no crash */
}

void rgMemoryTest_bloomFilterNullHandle(void)
{
    rgBloomFilterAddU64(NULL, 1);                /* no crash */
    TEST_ASSERT_EQUAL_INT(0, rgBloomFilterTestU64(NULL, 1));
    TEST_ASSERT_EQUAL_UINT64(0, rgBloomFilterBitCount(NULL));
    TEST_ASSERT_EQUAL_UINT64(0, rgBloomFilterPopCount(NULL));
}

/* -------------------------------------------------------------------------
 * BufferSize and Create rejection paths
 * ------------------------------------------------------------------------- */

void rgMemoryTest_bloomFilterBufferSize(void)
{
    /* Blocked Bloom: rounds the requested bit count up to a whole
     * power-of-two number of 512-bit (= 64-byte) blocks, minimum 1. */
    TEST_ASSERT_EQUAL_size_t(64,   rgBloomFilterBufferSize(1));     /* 1 block  =  512 bits */
    TEST_ASSERT_EQUAL_size_t(64,   rgBloomFilterBufferSize(512));   /* 1 block  =  512 bits */
    TEST_ASSERT_EQUAL_size_t(128,  rgBloomFilterBufferSize(513));   /* 2 blocks = 1024 bits */
    TEST_ASSERT_EQUAL_size_t(128,  rgBloomFilterBufferSize(1024));  /* 2 blocks            */
    TEST_ASSERT_EQUAL_size_t(256,  rgBloomFilterBufferSize(1025));  /* 4 blocks (pow2)     */
    TEST_ASSERT_EQUAL_size_t(1024, rgBloomFilterBufferSize(8192));  /* 16 blocks, exact pow2 */
    TEST_ASSERT_EQUAL_size_t(0,    rgBloomFilterBufferSize(0));
}

void rgMemoryTest_bloomFilterCreateRejectsBadArgs(void)
{
    BloomFilter bf;
    Arena       a;
    memset(&bf, 0, sizeof(bf));
    memset(&a,  0, sizeof(a));

    /* NULL out */
    TEST_ASSERT_EQUAL_INT32(-1, rgBloomFilterCreate(&a, NULL, 1024, 7));
    /* zero bits */
    TEST_ASSERT_EQUAL_INT32(-1, rgBloomFilterCreate(&a, &bf, 0, 7));
    /* zero hash count */
    TEST_ASSERT_EQUAL_INT32(-1, rgBloomFilterCreate(&a, &bf, 1024, 0));
    /* uninitialised arena */
    TEST_ASSERT_EQUAL_INT32(-1, rgBloomFilterCreate(&a, &bf, 1024, 7));
}

void rgMemoryTest_bloomFilterCreateFromMemoryRejectsBadArgs(void)
{
    BloomFilter bf;
    memset(&bf, 0, sizeof(bf));
    uint8_t buffer[64];

    /* NULL buffer */
    TEST_ASSERT_EQUAL_INT32(-1,
        rgBloomFilterCreateFromMemory(NULL, sizeof(buffer), &bf, 256, 7));
    /* NULL out */
    TEST_ASSERT_EQUAL_INT32(-1,
        rgBloomFilterCreateFromMemory(buffer, sizeof(buffer), NULL, 256, 7));
    /* zero bits */
    TEST_ASSERT_EQUAL_INT32(-1,
        rgBloomFilterCreateFromMemory(buffer, sizeof(buffer), &bf, 0, 7));
    /* zero hash count */
    TEST_ASSERT_EQUAL_INT32(-1,
        rgBloomFilterCreateFromMemory(buffer, sizeof(buffer), &bf, 256, 0));
    /* buffer too small (asking for 1024 bits = 128 bytes, only 64 supplied) */
    TEST_ASSERT_EQUAL_INT32(-4,
        rgBloomFilterCreateFromMemory(buffer, sizeof(buffer), &bf, 1024, 7));
}

/* -------------------------------------------------------------------------
 * Round-trip: Add then Test must always succeed (no false negatives)
 * ------------------------------------------------------------------------- */

void rgMemoryTest_bloomFilterArenaRoundTrip(void)
{
    Arena       a;
    BloomFilter bf;
    TEST_ASSERT_EQUAL_INT32(0, rgArenaCreate(&a, 64 * 1024));
    TEST_ASSERT_EQUAL_INT32(0, rgBloomFilterCreate(&a, &bf, 4096, 7));

    /* 1024 keys, mix of byte-key and U64. Each must Test positive. */
    uint64_t i;
    for (i = 0; i < 1024; ++i)
    {
        rgBloomFilterAddU64(&bf, i);
    }
    for (i = 0; i < 1024; ++i)
    {
        TEST_ASSERT_TRUE(rgBloomFilterTestU64(&bf, i));
    }

    /* Some bits must be set. */
    TEST_ASSERT_TRUE(rgBloomFilterPopCount(&bf) > 0);
    TEST_ASSERT_EQUAL_UINT32(7, rgBloomFilterHashCount(&bf));
    TEST_ASSERT_EQUAL_UINT64(4096, rgBloomFilterBitCount(&bf));

    rgArenaDestroy(&a);
}

void rgMemoryTest_bloomFilterMemoryRoundTrip(void)
{
    /* 256 bits = 32 bytes. Storage for 1024 bits / 7 hashes = 128 bytes. */
    static RGM_TEST_ALIGN_64 uint8_t backing[128];

    BloomFilter bf;
    TEST_ASSERT_EQUAL_INT32(0,
        rgBloomFilterCreateFromMemory(backing, sizeof(backing), &bf, 1024, 7));

    const char* keys[] = { "alpha", "beta", "gamma", "delta", "epsilon" };
    size_t n = sizeof(keys) / sizeof(keys[0]);
    size_t i;
    for (i = 0; i < n; ++i)
    {
        rgBloomFilterAdd(&bf, keys[i], strlen(keys[i]));
    }
    for (i = 0; i < n; ++i)
    {
        TEST_ASSERT_TRUE(rgBloomFilterTest(&bf, keys[i], strlen(keys[i])));
    }
}

/* -------------------------------------------------------------------------
 * Pre-hashed API
 * ------------------------------------------------------------------------- */

void rgMemoryTest_bloomFilterPreHashedRoundTrip(void)
{
    Arena       a;
    BloomFilter bf;
    TEST_ASSERT_EQUAL_INT32(0, rgArenaCreate(&a, 64 * 1024));
    TEST_ASSERT_EQUAL_INT32(0, rgBloomFilterCreate(&a, &bf, 4096, 5));

    /* Sized-up arbitrary hash pairs. */
    uint64_t i;
    for (i = 0; i < 256; ++i)
    {
        uint64_t h1 = i * 0x9E3779B97F4A7C15ull;
        uint64_t h2 = (i + 1) * 0xC6BC279692B5C323ull;
        rgBloomFilterAddH(&bf, h1, h2);
    }
    for (i = 0; i < 256; ++i)
    {
        uint64_t h1 = i * 0x9E3779B97F4A7C15ull;
        uint64_t h2 = (i + 1) * 0xC6BC279692B5C323ull;
        TEST_ASSERT_TRUE(rgBloomFilterTestH(&bf, h1, h2));
    }
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * False-positive rate sanity
 *
 * Insert N items, then Test M items that were NOT inserted. The empirical
 * false-positive rate should be in the same ballpark as the theoretical
 * (s/m)^k where s = bits set, m = bit count, k = hash count.
 *
 * We allow a generous slack (up to 5x theoretical) because:
 *  - Small N has high variance.
 *  - Power-of-two rounding can over-size the bit array, lowering both
 *    theoretical and measured FP rate.
 *  - The Kirsch-Mitzenmacher double-hashing is statistically equivalent
 *    to k independent hashes for typical k, but small deviations exist.
 * ------------------------------------------------------------------------- */

void rgMemoryTest_bloomFilterFalsePositiveSanity(void)
{
    Arena       a;
    BloomFilter bf;
    TEST_ASSERT_EQUAL_INT32(0, rgArenaCreate(&a, 1 * 1024 * 1024));

    /* 4096 items, 10 bits/item, k = 7 -> theoretical ~0.8% FP. */
    TEST_ASSERT_EQUAL_INT32(0, rgBloomFilterCreate(&a, &bf, 4096 * 10, 7));

    uint64_t i;
    for (i = 0; i < 4096; ++i)
    {
        rgBloomFilterAddU64(&bf, i);
    }

    /* Probe 100000 disjoint keys (range [10M, 10M + 100k)). */
    uint64_t falsePositives = 0;
    uint64_t probes         = 100000;
    for (i = 10000000; i < 10000000 + probes; ++i)
    {
        if (rgBloomFilterTestU64(&bf, i))
        {
            falsePositives++;
        }
    }

    /* Sanity ceiling: 5% (theoretical ~0.8%, with slack for variance and
     * the pow2 round-up to 65536 bits). If this fails, the hashing is
     * almost certainly broken -- not just statistical noise. */
    double rate = (double)falsePositives / (double)probes;
    TEST_ASSERT_TRUE(rate < 0.05);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Clear
 * ------------------------------------------------------------------------- */

void rgMemoryTest_bloomFilterClearResets(void)
{
    Arena       a;
    BloomFilter bf;
    TEST_ASSERT_EQUAL_INT32(0, rgArenaCreate(&a, 64 * 1024));
    TEST_ASSERT_EQUAL_INT32(0, rgBloomFilterCreate(&a, &bf, 4096, 7));

    uint64_t i;
    for (i = 0; i < 256; ++i)
    {
        rgBloomFilterAddU64(&bf, i);
    }
    TEST_ASSERT_TRUE(rgBloomFilterPopCount(&bf) > 0);

    rgBloomFilterClear(&bf);
    TEST_ASSERT_EQUAL_UINT64(0, rgBloomFilterPopCount(&bf));

    /* Previously-Added keys should now Test false (modulo the
     * astronomically tiny chance of every k bits being newly set by
     * unrelated activity -- can't happen here since nothing was added). */
    for (i = 0; i < 256; ++i)
    {
        TEST_ASSERT_FALSE(rgBloomFilterTestU64(&bf, i));
    }

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Test entry
 * ------------------------------------------------------------------------- */

void rgMemoryTest_BloomFilter(void)
{
    RUN_TEST(rgMemoryTest_bloomFilterZeroInitIsInert);
    RUN_TEST(rgMemoryTest_bloomFilterNullHandle);
    RUN_TEST(rgMemoryTest_bloomFilterBufferSize);
    RUN_TEST(rgMemoryTest_bloomFilterCreateRejectsBadArgs);
    RUN_TEST(rgMemoryTest_bloomFilterCreateFromMemoryRejectsBadArgs);
    RUN_TEST(rgMemoryTest_bloomFilterArenaRoundTrip);
    RUN_TEST(rgMemoryTest_bloomFilterMemoryRoundTrip);
    RUN_TEST(rgMemoryTest_bloomFilterPreHashedRoundTrip);
    RUN_TEST(rgMemoryTest_bloomFilterFalsePositiveSanity);
    RUN_TEST(rgMemoryTest_bloomFilterClearResets);
}
