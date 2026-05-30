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
 * Helpers
 * ------------------------------------------------------------------------- */

static int is_aligned(const void* _ptr, size_t _align)
{
    return ((uintptr_t)_ptr & ((uintptr_t)_align - 1)) == 0;
}

/* -------------------------------------------------------------------------
 * Zero-init / NULL safety
 *
 * A zero-initialised Arena, and a NULL Arena*, must be inert: every API
 * call is a safe no-op, queries return 0, Alloc returns NULL.
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaZeroInitIsInert(void)
{
    Arena a;
    memset(&a, 0, sizeof(a));
    TEST_ASSERT_EQUAL_INT(0, rgArenaIsValid(&a));
    TEST_ASSERT_NULL(rgArenaAlloc(&a, 100));
    TEST_ASSERT_EQUAL_UINT64(0, rgArenaUsed(&a));
    TEST_ASSERT_EQUAL_UINT64(0, rgArenaCapacity(&a));
    TEST_ASSERT_EQUAL_UINT64(RGM_ARENA_INVALID_SAVE_POINT, rgArenaSave(&a));
    /* Destroy / Clear / Pop / Restore on an inert arena must not crash. */
    rgArenaClear(&a);
    rgArenaPop(&a, 100);
    rgArenaRestore(&a, 0);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaNullPointerIsSafe(void)
{
    TEST_ASSERT_EQUAL_INT(0, rgArenaIsValid(NULL));
    TEST_ASSERT_NULL(rgArenaAlloc(NULL, 100));
    TEST_ASSERT_NULL(rgArenaAllocAligned(NULL, 100, 16));
    TEST_ASSERT_EQUAL_UINT64(0, rgArenaUsed(NULL));
    TEST_ASSERT_EQUAL_UINT64(0, rgArenaCapacity(NULL));
    TEST_ASSERT_EQUAL_UINT64(RGM_ARENA_INVALID_SAVE_POINT, rgArenaSave(NULL));
    rgArenaClear(NULL);
    rgArenaPop(NULL, 100);
    rgArenaRestore(NULL, 0);
    rgArenaDestroy(NULL);
}

/* -------------------------------------------------------------------------
 * Create / Destroy
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaCreateBasic(void)
{
    Arena a;
    TEST_ASSERT_EQUAL_INT(0, rgArenaCreate(&a, 64 * 1024));
    TEST_ASSERT_EQUAL_INT(1, rgArenaIsValid(&a));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaCreateNullOutFails(void)
{
    TEST_ASSERT_EQUAL_INT(-1, rgArenaCreate(NULL, 64 * 1024));
}

void rgMemoryTest_arenaCreateZeroSizeFails(void)
{
    Arena a;
    memset(&a, 0, sizeof(a));
    TEST_ASSERT_EQUAL_INT(-1, rgArenaCreate(&a, 0));
    TEST_ASSERT_EQUAL_INT(0, rgArenaIsValid(&a));
}

void rgMemoryTest_arenaDestroyInvalidates(void)
{
    Arena a;
    TEST_ASSERT_EQUAL_INT(0, rgArenaCreate(&a, 64 * 1024));
    TEST_ASSERT_EQUAL_INT(1, rgArenaIsValid(&a));
    rgArenaDestroy(&a);
    TEST_ASSERT_EQUAL_INT(0, rgArenaIsValid(&a));
}

void rgMemoryTest_arenaDestroyIsIdempotent(void)
{
    /* Second Destroy on the same Arena must be a safe no-op. */
    Arena a;
    TEST_ASSERT_EQUAL_INT(0, rgArenaCreate(&a, 64 * 1024));
    rgArenaDestroy(&a);
    rgArenaDestroy(&a);
    TEST_ASSERT_EQUAL_INT(0, rgArenaIsValid(&a));
}

void rgMemoryTest_arenaCreateMultiple(void)
{
    Arena a, b;
    TEST_ASSERT_EQUAL_INT(0, rgArenaCreate(&a, 64 * 1024));
    TEST_ASSERT_EQUAL_INT(0, rgArenaCreate(&b, 64 * 1024));
    TEST_ASSERT_EQUAL_INT(1, rgArenaIsValid(&a));
    TEST_ASSERT_EQUAL_INT(1, rgArenaIsValid(&b));
    /* Distinct reservations must occupy distinct VM ranges. */
    void* pa = rgArenaAlloc(&a, 100);
    void* pb = rgArenaAlloc(&b, 100);
    TEST_ASSERT_TRUE(pa != pb);
    rgArenaDestroy(&a);
    rgArenaDestroy(&b);
}

/* -------------------------------------------------------------------------
 * Capacity / Used
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaCapacityAtLeastRequested(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    TEST_ASSERT_TRUE(rgArenaCapacity(&a) >= (uint64_t)(64 * 1024));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaUsedStartsZero(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    TEST_ASSERT_EQUAL_UINT64(0, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Allocation
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaAllocBasic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    void* p = rgArenaAlloc(&a, 100);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(rgArenaUsed(&a) >= 100);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocDefaultAlignment(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    void* p = rgArenaAlloc(&a, 17);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(is_aligned(p, 16));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocSeparateAddresses(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    void* p1 = rgArenaAlloc(&a, 100);
    void* p2 = rgArenaAlloc(&a, 100);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_TRUE(p2 > p1);
    TEST_ASSERT_TRUE((uint8_t*)p2 - (uint8_t*)p1 >= 100);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocZeroSize(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    TEST_ASSERT_NULL(rgArenaAlloc(&a, 0));
    TEST_ASSERT_EQUAL_UINT64(0, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocWriteRead(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    const size_t n = 256;
    uint8_t* p = (uint8_t*)rgArenaAlloc(&a, n);
    TEST_ASSERT_NOT_NULL(p);
    for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)(i & 0xff);
    for (size_t i = 0; i < n; ++i) TEST_ASSERT_EQUAL_UINT8((uint8_t)(i & 0xff), p[i]);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocUsedMonotonic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    uint64_t prev = rgArenaUsed(&a);
    for (int i = 0; i < 16; ++i)
    {
        TEST_ASSERT_NOT_NULL(rgArenaAlloc(&a, 32));
        uint64_t now = rgArenaUsed(&a);
        TEST_ASSERT_TRUE(now > prev);
        prev = now;
    }
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Aligned allocation
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaAllocAligned32(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 1); /* bump pos off alignment so padding is exercised */
    void* p = rgArenaAllocAligned(&a, 100, 32);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(is_aligned(p, 32));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocAligned64(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 1);
    void* p = rgArenaAllocAligned(&a, 100, 64);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(is_aligned(p, 64));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocAligned4096(void)
{
    Arena a;
    rgArenaCreate(&a, 1024 * 1024);
    rgArenaAlloc(&a, 1);
    void* p = rgArenaAllocAligned(&a, 100, 4096);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(is_aligned(p, 4096));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocAlignedPromotedToEight(void)
{
    /* Alignments < 8 are clamped internally to 8. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    void* p = rgArenaAllocAligned(&a, 64, 1);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(is_aligned(p, 8));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Commit / Reserve boundary
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaAllocLargerThanMinCommitChunk(void)
{
    /* Forces a commit larger than the 64 KiB minimum chunk. */
    Arena a;
    rgArenaCreate(&a, 1024 * 1024);
    void* p = rgArenaAlloc(&a, 128 * 1024);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(rgArenaUsed(&a) >= (uint64_t)(128 * 1024));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocBeyondReservedFails(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    uint64_t cap = rgArenaCapacity(&a);
    TEST_ASSERT_NULL(rgArenaAlloc(&a, (size_t)(cap + 1)));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocManyToTriggerMultipleCommits(void)
{
    /* Hammer the bump pointer past several geometric commit boundaries. */
    Arena a;
    rgArenaCreate(&a, 1024 * 1024);
    for (int i = 0; i < 1024; ++i) TEST_ASSERT_NOT_NULL(rgArenaAlloc(&a, 512));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Size overflow
 *
 * Regression: rgArenaAlloc / rgArenaAllocAligned compute new_pos = user_off
 * + size as uint64_t. For pathologically large _size (near SIZE_MAX), the
 * sum wraps below user_off. Without an explicit wrap check the wrapped
 * value slipped past committed/reserved comparisons; the fast path would
 * return an aliasing pointer while rewinding m_pos to a tiny value, and
 * subsequent allocs would alias previous ones.
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaAllocSizeMaxReturnsNull(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    uint64_t used_before = rgArenaUsed(&a);
    TEST_ASSERT_NULL(rgArenaAlloc(&a, (size_t)-1));
    TEST_ASSERT_EQUAL_UINT64(used_before, rgArenaUsed(&a));
    TEST_ASSERT_NOT_NULL(rgArenaAlloc(&a, 100));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocSizeMaxAfterNonZeroPos(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 64);
    uint64_t used_before = rgArenaUsed(&a);
    TEST_ASSERT_NULL(rgArenaAlloc(&a, (size_t)-1));
    TEST_ASSERT_EQUAL_UINT64(used_before, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaAllocAlignedSizeMaxReturnsNull(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    uint64_t used_before = rgArenaUsed(&a);
    TEST_ASSERT_NULL(rgArenaAllocAligned(&a, (size_t)-1, 64));
    TEST_ASSERT_EQUAL_UINT64(used_before, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Pop
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaPopReducesUsed(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 100);
    uint64_t after = rgArenaUsed(&a);
    rgArenaPop(&a, 100);
    TEST_ASSERT_TRUE(rgArenaUsed(&a) < after);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaPopReuseAddress(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    void* p1 = rgArenaAlloc(&a, 64);
    rgArenaPop(&a, 64);
    void* p2 = rgArenaAlloc(&a, 64);
    TEST_ASSERT_EQUAL_PTR(p1, p2);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaPopZeroIsNoop(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 100);
    uint64_t before = rgArenaUsed(&a);
    rgArenaPop(&a, 0);
    TEST_ASSERT_EQUAL_UINT64(before, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaPopAlignedReusesAddress(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    void* p1 = rgArenaAllocAligned(&a, 64, 16);
    rgArenaPopAligned(&a, 64, 16);
    void* p2 = rgArenaAllocAligned(&a, 64, 16);
    TEST_ASSERT_EQUAL_PTR(p1, p2);
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Clear
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaClearResetsUsed(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 100);
    rgArenaAlloc(&a, 100);
    TEST_ASSERT_TRUE(rgArenaUsed(&a) > 0);
    rgArenaClear(&a);
    TEST_ASSERT_EQUAL_UINT64(0, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaClearReuseFromStart(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    void* p1 = rgArenaAlloc(&a, 100);
    rgArenaClear(&a);
    void* p2 = rgArenaAlloc(&a, 100);
    TEST_ASSERT_EQUAL_PTR(p1, p2);
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Save / Restore (also serves as the "scratch" mechanism)
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaSaveRestoreBasic(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 100);
    ArenaSavePoint sp = rgArenaSave(&a);
    rgArenaAlloc(&a, 200);
    rgArenaAlloc(&a, 300);
    rgArenaRestore(&a, sp);
    TEST_ASSERT_EQUAL_UINT64(sp, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaSaveOnUninitReturnsSentinel(void)
{
    /* Regression: distinguishes an uninitialised arena from an empty live one. */
    Arena a;
    memset(&a, 0, sizeof(a));
    TEST_ASSERT_EQUAL_UINT64(RGM_ARENA_INVALID_SAVE_POINT, rgArenaSave(&a));
}

void rgMemoryTest_arenaSaveEmptyArenaReturnsZero(void)
{
    /* A live but empty arena returns 0, distinct from the invalid sentinel. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    ArenaSavePoint sp = rgArenaSave(&a);
    TEST_ASSERT_EQUAL_UINT64(0, sp);
    TEST_ASSERT_TRUE(sp != RGM_ARENA_INVALID_SAVE_POINT);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaRestoreSentinelIsNoop(void)
{
    /* Passing the sentinel to Restore must silently no-op in both debug and
     * release. Naively the sentinel value (UINT64_MAX) is "ahead of" any
     * valid pos and would otherwise trip the debug assert. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 100);
    uint64_t before = rgArenaUsed(&a);
    rgArenaRestore(&a, RGM_ARENA_INVALID_SAVE_POINT);
    TEST_ASSERT_EQUAL_UINT64(before, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaRestoreReclaimsSpace(void)
{
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    ArenaSavePoint sp = rgArenaSave(&a);
    void* p = rgArenaAlloc(&a, 1000);
    TEST_ASSERT_NOT_NULL(p);
    rgArenaRestore(&a, sp);
    void* q = rgArenaAlloc(&a, 1000);
    TEST_ASSERT_EQUAL_PTR(p, q);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaSaveRestorePreservesPriorAlloc(void)
{
    /* Save/Restore as scratch: bytes allocated BEFORE Save must survive
     * Restore unmodified. (Replaces the old scratch test of the same shape.) */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    uint8_t* p = (uint8_t*)rgArenaAlloc(&a, 256);
    TEST_ASSERT_NOT_NULL(p);
    for (size_t i = 0; i < 256; ++i) p[i] = 0xAA;

    ArenaSavePoint sp = rgArenaSave(&a);
    rgArenaAlloc(&a, 1000);
    rgArenaRestore(&a, sp);

    for (size_t i = 0; i < 256; ++i) TEST_ASSERT_EQUAL_UINT8(0xAA, p[i]);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaSaveRestoreNestedLIFO(void)
{
    /* Nested Save/Restore pairs work as long as restores happen LIFO --
     * replaces the old nested-scratch test of the same shape. */
    Arena a;
    rgArenaCreate(&a, 64 * 1024);
    rgArenaAlloc(&a, 100);
    uint64_t before = rgArenaUsed(&a);

    ArenaSavePoint outer = rgArenaSave(&a);
    rgArenaAlloc(&a, 100);
    ArenaSavePoint inner = rgArenaSave(&a);
    rgArenaAlloc(&a, 200);
    rgArenaRestore(&a, inner);
    rgArenaRestore(&a, outer);

    TEST_ASSERT_EQUAL_UINT64(before, rgArenaUsed(&a));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * rgArenaShrink
 * ------------------------------------------------------------------------- */

void rgMemoryTest_arenaShrinkNoOpOnEmpty(void)
{
    /* Shrink on a freshly-created arena (nothing committed) should be a
     * silent no-op. Subsequent allocations must still work. */
    Arena a;
    rgArenaCreate(&a, 4 * 1024 * 1024);
    rgArenaShrink(&a);
    void* p = rgArenaAlloc(&a, 16);
    TEST_ASSERT_NOT_NULL(p);
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaShrinkNoOpOnUninitialised(void)
{
    /* Inert struct: shrink is a no-op. */
    Arena a;
    memset(&a, 0, sizeof(a));
    rgArenaShrink(&a);
    rgArenaShrink(NULL);
    /* Reaching here without a crash is the test. */
    TEST_ASSERT_EQUAL_INT(0, rgArenaIsValid(&a));
}

void rgMemoryTest_arenaShrinkPreservesLiveAllocations(void)
{
    /* Allocate a chunk, write a pattern, allocate a bigger chunk to
     * commit more pages, free back to the first chunk, then shrink.
     * The first chunk's data must still be readable. */
    Arena a;
    rgArenaCreate(&a, 16 * 1024 * 1024);

    uint8_t* first = (uint8_t*)rgArenaAlloc(&a, 1024);
    TEST_ASSERT_NOT_NULL(first);
    for (int i = 0; i < 1024; ++i) first[i] = (uint8_t)(i & 0xFF);

    /* Force a bigger commit. */
    ArenaSavePoint sp = rgArenaSave(&a);
    void* big = rgArenaAlloc(&a, 4 * 1024 * 1024);
    TEST_ASSERT_NOT_NULL(big);
    (void)big;

    /* Rewind, then shrink to decommit the pages we just released. */
    rgArenaRestore(&a, sp);
    rgArenaShrink(&a);

    /* First chunk's data must survive. */
    for (int i = 0; i < 1024; ++i)
    {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i & 0xFF), first[i]);
    }
    rgArenaDestroy(&a);
}

void rgMemoryTest_arenaShrinkAllowsReuse(void)
{
    /* After shrink, subsequent allocations must succeed -- the commit
     * region must be able to grow back through the decommitted area. */
    Arena a;
    rgArenaCreate(&a, 16 * 1024 * 1024);

    /* Commit ~4 MiB. */
    (void)rgArenaAlloc(&a, 4 * 1024 * 1024);
    rgArenaClear(&a);

    /* Shrink: the just-committed 4 MiB should be decommittable. */
    rgArenaShrink(&a);

    /* Allocate beyond what we just decommitted; this re-commits the same
     * pages. The arena must still hand back working memory. */
    uint8_t* p = (uint8_t*)rgArenaAlloc(&a, 5 * 1024 * 1024);
    TEST_ASSERT_NOT_NULL(p);
    p[0] = 0xAA;
    p[5 * 1024 * 1024 - 1] = 0xBB;
    TEST_ASSERT_EQUAL_UINT8(0xAA, p[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, p[5 * 1024 * 1024 - 1]);

    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_Arena(void)
{
    RUN_TEST(rgMemoryTest_arenaZeroInitIsInert);
    RUN_TEST(rgMemoryTest_arenaNullPointerIsSafe);

    RUN_TEST(rgMemoryTest_arenaCreateBasic);
    RUN_TEST(rgMemoryTest_arenaCreateNullOutFails);
    RUN_TEST(rgMemoryTest_arenaCreateZeroSizeFails);
    RUN_TEST(rgMemoryTest_arenaDestroyInvalidates);
    RUN_TEST(rgMemoryTest_arenaDestroyIsIdempotent);
    RUN_TEST(rgMemoryTest_arenaCreateMultiple);

    RUN_TEST(rgMemoryTest_arenaCapacityAtLeastRequested);
    RUN_TEST(rgMemoryTest_arenaUsedStartsZero);

    RUN_TEST(rgMemoryTest_arenaAllocBasic);
    RUN_TEST(rgMemoryTest_arenaAllocDefaultAlignment);
    RUN_TEST(rgMemoryTest_arenaAllocSeparateAddresses);
    RUN_TEST(rgMemoryTest_arenaAllocZeroSize);
    RUN_TEST(rgMemoryTest_arenaAllocWriteRead);
    RUN_TEST(rgMemoryTest_arenaAllocUsedMonotonic);

    RUN_TEST(rgMemoryTest_arenaAllocAligned32);
    RUN_TEST(rgMemoryTest_arenaAllocAligned64);
    RUN_TEST(rgMemoryTest_arenaAllocAligned4096);
    RUN_TEST(rgMemoryTest_arenaAllocAlignedPromotedToEight);

    RUN_TEST(rgMemoryTest_arenaAllocLargerThanMinCommitChunk);
    RUN_TEST(rgMemoryTest_arenaAllocBeyondReservedFails);
    RUN_TEST(rgMemoryTest_arenaAllocManyToTriggerMultipleCommits);

    RUN_TEST(rgMemoryTest_arenaAllocSizeMaxReturnsNull);
    RUN_TEST(rgMemoryTest_arenaAllocSizeMaxAfterNonZeroPos);
    RUN_TEST(rgMemoryTest_arenaAllocAlignedSizeMaxReturnsNull);

    RUN_TEST(rgMemoryTest_arenaPopReducesUsed);
    RUN_TEST(rgMemoryTest_arenaPopReuseAddress);
    RUN_TEST(rgMemoryTest_arenaPopZeroIsNoop);
    RUN_TEST(rgMemoryTest_arenaPopAlignedReusesAddress);

    RUN_TEST(rgMemoryTest_arenaClearResetsUsed);
    RUN_TEST(rgMemoryTest_arenaClearReuseFromStart);

    RUN_TEST(rgMemoryTest_arenaSaveRestoreBasic);
    RUN_TEST(rgMemoryTest_arenaSaveOnUninitReturnsSentinel);
    RUN_TEST(rgMemoryTest_arenaSaveEmptyArenaReturnsZero);
    RUN_TEST(rgMemoryTest_arenaRestoreSentinelIsNoop);
    RUN_TEST(rgMemoryTest_arenaRestoreReclaimsSpace);
    RUN_TEST(rgMemoryTest_arenaSaveRestorePreservesPriorAlloc);
    RUN_TEST(rgMemoryTest_arenaSaveRestoreNestedLIFO);

    RUN_TEST(rgMemoryTest_arenaShrinkNoOpOnEmpty);
    RUN_TEST(rgMemoryTest_arenaShrinkNoOpOnUninitialised);
    RUN_TEST(rgMemoryTest_arenaShrinkPreservesLiveAllocations);
    RUN_TEST(rgMemoryTest_arenaShrinkAllowsReuse);
}
