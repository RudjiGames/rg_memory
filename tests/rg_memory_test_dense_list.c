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

#if defined(_MSC_VER)
#   define RGM_TEST_ALIGN_16 __declspec(align(16))
#elif defined(__GNUC__) || defined(__clang__)
#   define RGM_TEST_ALIGN_16 __attribute__((aligned(16)))
#else
#   define RGM_TEST_ALIGN_16
#endif

/* -------------------------------------------------------------------------
 * Zero-init / NULL safety
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListZeroInitIsInert(void)
{
    DenseList dl;
    memset(&dl, 0, sizeof(dl));
    TEST_ASSERT_NULL(rgDenseListAlloc(&dl));
    TEST_ASSERT_NULL(rgDenseListData(&dl));
    TEST_ASSERT_NULL(rgDenseListAt(&dl, 0));
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListMaxBlocks(&dl));
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListBlockSize(&dl));
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListCount(&dl));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rgDenseListIndexOf(&dl, &dl));
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCheckPtr(&dl, &dl));
    /* Clear / Free on an inert list must not crash. */
    rgDenseListClear(&dl);
    uint32_t scratch = 0;
    rgDenseListFree(&dl, &scratch);
}

void rgMemoryTest_denseListNullPointerIsSafe(void)
{
    uint32_t scratch = 0;
    TEST_ASSERT_NULL(rgDenseListAlloc(NULL));
    TEST_ASSERT_NULL(rgDenseListData(NULL));
    TEST_ASSERT_NULL(rgDenseListAt(NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListMaxBlocks(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListBlockSize(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListCount(NULL));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rgDenseListIndexOf(NULL, &scratch));
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCheckPtr(NULL, &scratch));
    rgDenseListClear(NULL);
    rgDenseListFree(NULL, &scratch);
}

/* -------------------------------------------------------------------------
 * Create
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListCreateBasic(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 16));
    TEST_ASSERT_EQUAL_UINT32(16, rgDenseListMaxBlocks(&dl));
    TEST_ASSERT_EQUAL_UINT32(32, rgDenseListBlockSize(&dl));
    TEST_ASSERT_EQUAL_UINT32(0,  rgDenseListCount(&dl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCreateNullOutFails(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    TEST_ASSERT_EQUAL_INT(-1, rgDenseListCreate(&a, NULL, 32, 16));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCreateZeroMaxBlocksFails(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    memset(&dl, 0, sizeof(dl));
    TEST_ASSERT_EQUAL_INT(-1, rgDenseListCreate(&a, &dl, 32, 0));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCreateInvalidArenaFails(void)
{
    DenseList dl;
    memset(&dl, 0, sizeof(dl));
    TEST_ASSERT_EQUAL_INT(-1, rgDenseListCreate(NULL, &dl, 32, 16));
}

void rgMemoryTest_denseListCreateRoundsBlockSizeTo16(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl24, dl33;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl24, 24, 8));
    TEST_ASSERT_EQUAL_UINT32(32, rgDenseListBlockSize(&dl24));
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl33, 33, 8));
    TEST_ASSERT_EQUAL_UINT32(48, rgDenseListBlockSize(&dl33));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCreateTinyBlockSize(void)
{
    /* Unlike FreeList there is no sizeof(uint32_t) minimum; a 1-byte
     * block still rounds up to 16 but doesn't get pre-clamped. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 1, 8));
    TEST_ASSERT_EQUAL_UINT32(16, rgDenseListBlockSize(&dl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCreateExhaustionReturnsNoMemory(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    uint64_t before = rgArenaUsed(&a);
    DenseList dl;
    memset(&dl, 0xAB, sizeof(dl));
    TEST_ASSERT_EQUAL_INT(-3, rgDenseListCreate(&a, &dl, 64, 1000000));
    TEST_ASSERT_EQUAL_UINT64(before, rgArenaUsed(&a));
    TEST_ASSERT_EQUAL_UINT8(0xAB, ((uint8_t*)&dl)[0]);
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCreateAfterFailureSucceeds(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList bad, good;
    TEST_ASSERT_EQUAL_INT(-3, rgDenseListCreate(&a, &bad,  64, 1000000));
    TEST_ASSERT_EQUAL_INT(0,  rgDenseListCreate(&a, &good, 32, 16));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCreateOverflowFails(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    memset(&dl, 0, sizeof(dl));
    size_t huge = ((size_t)-1) / 4;
    TEST_ASSERT_EQUAL_INT(-2, rgDenseListCreate(&a, &dl, huge, 8));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * CreateFromMemory / BufferSize
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListBufferSizeRoundsUp(void)
{
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(8 * 32), (uint64_t)rgDenseListBufferSize(24, 8));
    /* No sizeof(uint32_t) clamp -- 1 byte rounds straight to 16. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(8 * 16), (uint64_t)rgDenseListBufferSize(1, 8));
}

void rgMemoryTest_denseListBufferSizeZeroInputs(void)
{
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)rgDenseListBufferSize(32, 0));
}

void rgMemoryTest_denseListCreateFromMemoryBasic(void)
{
    static RGM_TEST_ALIGN_16 uint8_t buf[8 * 32];
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreateFromMemory(buf, sizeof(buf), &dl, 32, 8));
    TEST_ASSERT_EQUAL_UINT32(8,  rgDenseListMaxBlocks(&dl));
    TEST_ASSERT_EQUAL_UINT32(32, rgDenseListBlockSize(&dl));
    TEST_ASSERT_EQUAL_UINT32(0,  rgDenseListCount(&dl));
    void* p = rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_PTR(buf, p);
}

void rgMemoryTest_denseListCreateFromMemoryNullBufferFails(void)
{
    DenseList dl;
    memset(&dl, 0, sizeof(dl));
    TEST_ASSERT_EQUAL_INT(-1, rgDenseListCreateFromMemory(NULL, 256, &dl, 32, 8));
}

void rgMemoryTest_denseListCreateFromMemoryNullOutFails(void)
{
    static RGM_TEST_ALIGN_16 uint8_t buf[256];
    TEST_ASSERT_EQUAL_INT(-1, rgDenseListCreateFromMemory(buf, sizeof(buf), NULL, 32, 8));
}

void rgMemoryTest_denseListCreateFromMemoryZeroMaxBlocksFails(void)
{
    static RGM_TEST_ALIGN_16 uint8_t buf[256];
    DenseList dl;
    memset(&dl, 0, sizeof(dl));
    TEST_ASSERT_EQUAL_INT(-1, rgDenseListCreateFromMemory(buf, sizeof(buf), &dl, 32, 0));
}

void rgMemoryTest_denseListCreateFromMemoryTooSmallFails(void)
{
    static RGM_TEST_ALIGN_16 uint8_t buf[128];
    DenseList dl;
    memset(&dl, 0, sizeof(dl));
    TEST_ASSERT_EQUAL_INT(-4, rgDenseListCreateFromMemory(buf, sizeof(buf), &dl, 32, 8));
}

/* -------------------------------------------------------------------------
 * Alloc
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListAllocStartsAtBase(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    void* p = rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_PTR(rgDenseListData(&dl), p);
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListAllocIncrementsCount(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListCount(&dl));
    rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_UINT32(1, rgDenseListCount(&dl));
    rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_UINT32(2, rgDenseListCount(&dl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListAllocAllBlocks(void)
{
    const uint32_t N = 16;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, N));
    for (uint32_t i = 0; i < N; ++i)
    {
        TEST_ASSERT_NOT_NULL(rgDenseListAlloc(&dl));
    }
    TEST_ASSERT_EQUAL_UINT32(N, rgDenseListCount(&dl));
    TEST_ASSERT_NULL(rgDenseListAlloc(&dl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListAllocBlocksAre16Aligned(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 16, 8));
    for (int i = 0; i < 8; ++i)
    {
        void* p = rgDenseListAlloc(&dl);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_TRUE(is_aligned(p, 16));
    }
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListAllocReturnsContiguousSlots(void)
{
    /* Each Alloc must land at the slot immediately after the previous one. */
    const uint32_t N = 4;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, N));
    uint32_t stride = rgDenseListBlockSize(&dl);
    uint8_t* first = (uint8_t*)rgDenseListAlloc(&dl);
    for (uint32_t i = 1; i < N; ++i)
    {
        uint8_t* p = (uint8_t*)rgDenseListAlloc(&dl);
        TEST_ASSERT_EQUAL_PTR(first + (size_t)i * stride, p);
    }
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Free  (swap-back semantics)
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListFreeDecrementsCount(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    void* p = rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_UINT32(1, rgDenseListCount(&dl));
    rgDenseListFree(&dl, p);
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListCount(&dl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListFreeLastIsZeroCopy(void)
{
    /* Freeing the last live block must not memcpy onto itself; verify by
     * checking that the byte pattern in the last slot is unchanged. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 4));
    uint8_t* p0 = (uint8_t*)rgDenseListAlloc(&dl);
    uint8_t* p1 = (uint8_t*)rgDenseListAlloc(&dl);
    memset(p0, 0xAA, 32);
    memset(p1, 0xBB, 32);
    rgDenseListFree(&dl, p1);
    TEST_ASSERT_EQUAL_UINT32(1, rgDenseListCount(&dl));
    /* p1's slot still holds the 0xBB pattern; nothing scribbled over it. */
    for (int i = 0; i < 32; ++i) TEST_ASSERT_EQUAL_UINT8(0xBB, p1[i]);
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListFreeMiddleSwapsBackContents(void)
{
    /* Freeing a middle block must overwrite its contents with the last
     * block's contents. The slot pointer stays the same, but what's in
     * it changes. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 4));
    uint8_t* p0 = (uint8_t*)rgDenseListAlloc(&dl);
    uint8_t* p1 = (uint8_t*)rgDenseListAlloc(&dl);
    uint8_t* p2 = (uint8_t*)rgDenseListAlloc(&dl);
    memset(p0, 0xAA, 32);
    memset(p1, 0xBB, 32);
    memset(p2, 0xCC, 32);
    rgDenseListFree(&dl, p1);
    TEST_ASSERT_EQUAL_UINT32(2, rgDenseListCount(&dl));
    /* p0 untouched, p1's slot now holds what was in p2 (0xCC). */
    for (int i = 0; i < 32; ++i) TEST_ASSERT_EQUAL_UINT8(0xAA, p0[i]);
    for (int i = 0; i < 32; ++i) TEST_ASSERT_EQUAL_UINT8(0xCC, p1[i]);
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListFreeFirstSwapsBackContents(void)
{
    /* Freeing slot 0 with a non-empty list moves the last block into slot 0. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 4));
    uint8_t* p0 = (uint8_t*)rgDenseListAlloc(&dl);
    uint8_t* p1 = (uint8_t*)rgDenseListAlloc(&dl);
    memset(p0, 0xAA, 32);
    memset(p1, 0xBB, 32);
    rgDenseListFree(&dl, p0);
    TEST_ASSERT_EQUAL_UINT32(1, rgDenseListCount(&dl));
    for (int i = 0; i < 32; ++i) TEST_ASSERT_EQUAL_UINT8(0xBB, p0[i]);
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListFreeAllOneByOne(void)
{
    /* Free every block in alloc order; count must reach 0 and Alloc must
     * resume from slot 0 afterwards. */
    const uint32_t N = 8;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, N));
    for (uint32_t i = 0; i < N; ++i) rgDenseListAlloc(&dl);
    for (uint32_t i = 0; i < N; ++i)
    {
        /* Always free slot 0 -- forces N-1 swap-back copies in a row. */
        void* p = rgDenseListAt(&dl, 0);
        TEST_ASSERT_NOT_NULL(p);
        rgDenseListFree(&dl, p);
    }
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListCount(&dl));
    void* p = rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_PTR(rgDenseListData(&dl), p);
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListFreeAndRefillKeepsCount(void)
{
    /* Free a middle block, then Alloc -- the freed slot's content
     * changed (swap-back) but the new Alloc still lands at the new
     * m_count, not at the just-freed pointer. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 4));
    void* p0 = rgDenseListAlloc(&dl);
    void* p1 = rgDenseListAlloc(&dl);
    void* p2 = rgDenseListAlloc(&dl);
    (void)p0;
    rgDenseListFree(&dl, p1);  /* count: 3 -> 2; p2's data now lives at p1's slot */
    void* p3 = rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_PTR(p2, p3); /* new block lands at the old p2 slot (now index 2) */
    TEST_ASSERT_EQUAL_UINT32(3, rgDenseListCount(&dl));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Data / At / IndexOf
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListDataReturnsBuffer(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    void* base = rgDenseListData(&dl);
    TEST_ASSERT_NOT_NULL(base);
    /* First Alloc must return Data(). */
    TEST_ASSERT_EQUAL_PTR(base, rgDenseListAlloc(&dl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListAtMatchesAllocOrder(void)
{
    const uint32_t N = 4;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, N));
    void* ptrs[4];
    for (uint32_t i = 0; i < N; ++i) ptrs[i] = rgDenseListAlloc(&dl);
    for (uint32_t i = 0; i < N; ++i)
    {
        TEST_ASSERT_EQUAL_PTR(ptrs[i], rgDenseListAt(&dl, i));
    }
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListAtPastCountReturnsNull(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    rgDenseListAlloc(&dl);
    TEST_ASSERT_NOT_NULL(rgDenseListAt(&dl, 0));
    TEST_ASSERT_NULL(rgDenseListAt(&dl, 1));
    TEST_ASSERT_NULL(rgDenseListAt(&dl, 7));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListIndexOfAllocReturn(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    void* p0 = rgDenseListAlloc(&dl);
    void* p1 = rgDenseListAlloc(&dl);
    void* p2 = rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListIndexOf(&dl, p0));
    TEST_ASSERT_EQUAL_UINT32(1, rgDenseListIndexOf(&dl, p1));
    TEST_ASSERT_EQUAL_UINT32(2, rgDenseListIndexOf(&dl, p2));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListIndexOfOutOfBufferReturnsMax(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    uint32_t stranger = 0;
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rgDenseListIndexOf(&dl, &stranger));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, rgDenseListIndexOf(&dl, NULL));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Clear
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListClearResetsCount(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    for (int i = 0; i < 5; ++i) rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_UINT32(5, rgDenseListCount(&dl));
    rgDenseListClear(&dl);
    TEST_ASSERT_EQUAL_UINT32(0, rgDenseListCount(&dl));
    /* Buffer is still live; max blocks / block size unchanged. */
    TEST_ASSERT_EQUAL_UINT32(8,  rgDenseListMaxBlocks(&dl));
    TEST_ASSERT_EQUAL_UINT32(32, rgDenseListBlockSize(&dl));
    /* Re-Alloc starts at slot 0 again. */
    TEST_ASSERT_EQUAL_PTR(rgDenseListData(&dl), rgDenseListAlloc(&dl));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * CheckPtr
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListCheckPtrInRange(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    void* p = rgDenseListAlloc(&dl);
    TEST_ASSERT_EQUAL_INT(1, rgDenseListCheckPtr(&dl, p));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCheckPtrInUnusedSlot(void)
{
    /* CheckPtr is a buffer-range probe, not a liveness check; slots past
     * m_count still belong to the buffer and must report true. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    /* count is still 0; the byte at slot 3 is inside the buffer. */
    uint8_t* slot3 = (uint8_t*)rgDenseListData(&dl) + 3 * 32;
    TEST_ASSERT_EQUAL_INT(1, rgDenseListCheckPtr(&dl, slot3));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCheckPtrPastEnd(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    uint8_t* base = (uint8_t*)rgDenseListData(&dl);
    size_t buffer_bytes = (size_t)rgDenseListMaxBlocks(&dl) * rgDenseListBlockSize(&dl);
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCheckPtr(&dl, base + buffer_bytes));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListCheckPtrBeforeBuffer(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    uint8_t* base = (uint8_t*)rgDenseListData(&dl);
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCheckPtr(&dl, base - 1));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Integration
 * ------------------------------------------------------------------------- */

void rgMemoryTest_denseListIterationVisitsEveryLive(void)
{
    /* Write a unique tag into each block, iterate, sum them up. */
    const uint32_t N = 16;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, sizeof(uint32_t), N));
    for (uint32_t i = 0; i < N; ++i)
    {
        uint32_t* p = (uint32_t*)rgDenseListAlloc(&dl);
        TEST_ASSERT_NOT_NULL(p);
        *p = i + 1;
    }

    uint8_t* base   = (uint8_t*)rgDenseListData(&dl);
    uint32_t count  = rgDenseListCount(&dl);
    uint32_t stride = rgDenseListBlockSize(&dl);
    uint32_t sum    = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        sum += *(uint32_t*)(base + (size_t)i * stride);
    }
    TEST_ASSERT_EQUAL_UINT32(N * (N + 1) / 2, sum);
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListBackwardFreeDuringIteration(void)
{
    /* The doc-recommended pattern: walk backwards, free even values.
     * Survivors must be exactly the odd ones. */
    const uint32_t N = 10;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, sizeof(uint32_t), N));
    for (uint32_t i = 0; i < N; ++i)
    {
        *(uint32_t*)rgDenseListAlloc(&dl) = i;
    }
    for (uint32_t i = rgDenseListCount(&dl); i-- > 0; )
    {
        uint32_t* p = (uint32_t*)rgDenseListAt(&dl, i);
        if ((*p % 2u) == 0u)
        {
            rgDenseListFree(&dl, p);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(5, rgDenseListCount(&dl));
    /* Survivors are some permutation of {1,3,5,7,9} -- swap-back doesn't
     * preserve order, so just check the set via OR. */
    uint32_t mask = 0;
    for (uint32_t i = 0; i < rgDenseListCount(&dl); ++i)
    {
        mask |= 1u << *(uint32_t*)rgDenseListAt(&dl, i);
    }
    TEST_ASSERT_EQUAL_UINT32(0x2AAu, mask); /* bits 1,3,5,7,9 */
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListLargePool(void)
{
    const uint32_t N = 10000;
    Arena a; rgArenaCreate(&a, 2 * 1024 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 64, N));
    for (uint32_t i = 0; i < N; ++i) TEST_ASSERT_NOT_NULL(rgDenseListAlloc(&dl));
    TEST_ASSERT_EQUAL_UINT32(N, rgDenseListCount(&dl));
    TEST_ASSERT_NULL(rgDenseListAlloc(&dl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_denseListUsableAfterArenaClear(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    DenseList dl;
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 32, 8));
    TEST_ASSERT_NOT_NULL(rgDenseListAlloc(&dl));
    rgArenaClear(&a);
    TEST_ASSERT_EQUAL_INT(0, rgDenseListCreate(&a, &dl, 64, 4));
    TEST_ASSERT_NOT_NULL(rgDenseListAlloc(&dl));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_DenseList(void)
{
    RUN_TEST(rgMemoryTest_denseListZeroInitIsInert);
    RUN_TEST(rgMemoryTest_denseListNullPointerIsSafe);

    RUN_TEST(rgMemoryTest_denseListCreateBasic);
    RUN_TEST(rgMemoryTest_denseListCreateNullOutFails);
    RUN_TEST(rgMemoryTest_denseListCreateZeroMaxBlocksFails);
    RUN_TEST(rgMemoryTest_denseListCreateInvalidArenaFails);
    RUN_TEST(rgMemoryTest_denseListCreateRoundsBlockSizeTo16);
    RUN_TEST(rgMemoryTest_denseListCreateTinyBlockSize);
    RUN_TEST(rgMemoryTest_denseListCreateExhaustionReturnsNoMemory);
    RUN_TEST(rgMemoryTest_denseListCreateAfterFailureSucceeds);
    RUN_TEST(rgMemoryTest_denseListCreateOverflowFails);

    RUN_TEST(rgMemoryTest_denseListBufferSizeRoundsUp);
    RUN_TEST(rgMemoryTest_denseListBufferSizeZeroInputs);
    RUN_TEST(rgMemoryTest_denseListCreateFromMemoryBasic);
    RUN_TEST(rgMemoryTest_denseListCreateFromMemoryNullBufferFails);
    RUN_TEST(rgMemoryTest_denseListCreateFromMemoryNullOutFails);
    RUN_TEST(rgMemoryTest_denseListCreateFromMemoryZeroMaxBlocksFails);
    RUN_TEST(rgMemoryTest_denseListCreateFromMemoryTooSmallFails);

    RUN_TEST(rgMemoryTest_denseListAllocStartsAtBase);
    RUN_TEST(rgMemoryTest_denseListAllocIncrementsCount);
    RUN_TEST(rgMemoryTest_denseListAllocAllBlocks);
    RUN_TEST(rgMemoryTest_denseListAllocBlocksAre16Aligned);
    RUN_TEST(rgMemoryTest_denseListAllocReturnsContiguousSlots);

    RUN_TEST(rgMemoryTest_denseListFreeDecrementsCount);
    RUN_TEST(rgMemoryTest_denseListFreeLastIsZeroCopy);
    RUN_TEST(rgMemoryTest_denseListFreeMiddleSwapsBackContents);
    RUN_TEST(rgMemoryTest_denseListFreeFirstSwapsBackContents);
    RUN_TEST(rgMemoryTest_denseListFreeAllOneByOne);
    RUN_TEST(rgMemoryTest_denseListFreeAndRefillKeepsCount);

    RUN_TEST(rgMemoryTest_denseListDataReturnsBuffer);
    RUN_TEST(rgMemoryTest_denseListAtMatchesAllocOrder);
    RUN_TEST(rgMemoryTest_denseListAtPastCountReturnsNull);
    RUN_TEST(rgMemoryTest_denseListIndexOfAllocReturn);
    RUN_TEST(rgMemoryTest_denseListIndexOfOutOfBufferReturnsMax);

    RUN_TEST(rgMemoryTest_denseListClearResetsCount);

    RUN_TEST(rgMemoryTest_denseListCheckPtrInRange);
    RUN_TEST(rgMemoryTest_denseListCheckPtrInUnusedSlot);
    RUN_TEST(rgMemoryTest_denseListCheckPtrPastEnd);
    RUN_TEST(rgMemoryTest_denseListCheckPtrBeforeBuffer);

    RUN_TEST(rgMemoryTest_denseListIterationVisitsEveryLive);
    RUN_TEST(rgMemoryTest_denseListBackwardFreeDuringIteration);
    RUN_TEST(rgMemoryTest_denseListLargePool);
    RUN_TEST(rgMemoryTest_denseListUsableAfterArenaClear);
}
