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

/* 16-byte-aligned scratch buffer for the external-memory Create variant. */
#if defined(_MSC_VER)
#   define RGM_TEST_ALIGN_16 __declspec(align(16))
#elif defined(__GNUC__) || defined(__clang__)
#   define RGM_TEST_ALIGN_16 __attribute__((aligned(16)))
#else
#   define RGM_TEST_ALIGN_16
#endif

/* -------------------------------------------------------------------------
 * Zero-init / NULL safety
 *
 * A zero-initialised FreeList -- and a NULL FreeList* -- must be inert:
 * every API call is a safe no-op, queries return 0, Alloc returns NULL.
 * ------------------------------------------------------------------------- */

void rgMemoryTest_freeListZeroInitIsInert(void)
{
    FreeList fl;
    memset(&fl, 0, sizeof(fl));
    TEST_ASSERT_NULL(rgFreeListAlloc(&fl));
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListMaxBlocks(&fl));
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListBlockSize(&fl));
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListBlocksFree(&fl));
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCheckPtr(&fl, &fl));
    uint32_t scratch = 0;
    rgFreeListFree(&fl, &scratch);
}

void rgMemoryTest_freeListNullPointerIsSafe(void)
{
    uint32_t scratch = 0;
    TEST_ASSERT_NULL(rgFreeListAlloc(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListMaxBlocks(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListBlockSize(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListBlocksFree(NULL));
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCheckPtr(NULL, &scratch));
    rgFreeListFree(NULL, &scratch);
}

/* -------------------------------------------------------------------------
 * Create
 * ------------------------------------------------------------------------- */

void rgMemoryTest_freeListCreateBasic(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 16));
    TEST_ASSERT_EQUAL_UINT32(16, rgFreeListMaxBlocks(&fl));
    TEST_ASSERT_EQUAL_UINT32(32, rgFreeListBlockSize(&fl));
    TEST_ASSERT_EQUAL_UINT32(16, rgFreeListBlocksFree(&fl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCreateNullOutFails(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    TEST_ASSERT_EQUAL_INT(-1, rgFreeListCreate(&a, NULL, 32, 16));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCreateZeroMaxBlocksFails(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    memset(&fl, 0, sizeof(fl));
    TEST_ASSERT_EQUAL_INT(-1, rgFreeListCreate(&a, &fl, 32, 0));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCreateInvalidArenaFails(void)
{
    FreeList fl;
    memset(&fl, 0, sizeof(fl));
    TEST_ASSERT_EQUAL_INT(-1, rgFreeListCreate(NULL, &fl, 32, 16));
}

void rgMemoryTest_freeListCreateClampsSmallBlockSize(void)
{
    /* _blockSize < sizeof(uint32_t) is clamped, then rounded up to 16. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 1, 8));
    TEST_ASSERT_EQUAL_UINT32(16, rgFreeListBlockSize(&fl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCreateRoundsBlockSizeTo16(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl24, fl33;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl24, 24, 8));
    TEST_ASSERT_EQUAL_UINT32(32, rgFreeListBlockSize(&fl24));
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl33, 33, 8));
    TEST_ASSERT_EQUAL_UINT32(48, rgFreeListBlockSize(&fl33));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCreateExhaustionReturnsNoMemory(void)
{
    /* Arena alloc fails -> arena pos must be unchanged, and *_freelist must
     * be left untouched (so the caller can distinguish failure from success
     * without inspecting the return value). */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    uint64_t before = rgArenaUsed(&a);
    FreeList fl;
    memset(&fl, 0xAB, sizeof(fl));
    TEST_ASSERT_EQUAL_INT(-3, rgFreeListCreate(&a, &fl, 64, 1000000));
    TEST_ASSERT_EQUAL_UINT64(before, rgArenaUsed(&a));
    TEST_ASSERT_EQUAL_UINT8(0xAB, ((uint8_t*)&fl)[0]);
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCreateAfterFailureSucceeds(void)
{
    /* A failed Create must leave the arena fully usable for a subsequent one. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList bad, good;
    TEST_ASSERT_EQUAL_INT(-3, rgFreeListCreate(&a, &bad,  64, 1000000));
    TEST_ASSERT_EQUAL_INT(0,  rgFreeListCreate(&a, &good, 32, 16));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCreateOverflowFails(void)
{
    /* Drive bs * _maxBlocks past SIZE_MAX (or bs past the uint32_t cap on 64-bit). */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    memset(&fl, 0, sizeof(fl));
    size_t huge = ((size_t)-1) / 4;
    TEST_ASSERT_EQUAL_INT(-2, rgFreeListCreate(&a, &fl, huge, 8));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * CreateFromMemory / BufferSize
 * ------------------------------------------------------------------------- */

void rgMemoryTest_freeListBufferSizeRoundsUp(void)
{
    /* Block 24 rounds up to 32; 8 * 32 = 256. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(8 * 32), (uint64_t)rgFreeListBufferSize(24, 8));
    /* Block 1 is clamped to sizeof(uint32_t)=4, then rounded to 16. */
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(8 * 16), (uint64_t)rgFreeListBufferSize(1, 8));
}

void rgMemoryTest_freeListBufferSizeZeroInputs(void)
{
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t)rgFreeListBufferSize(32, 0));
}

void rgMemoryTest_freeListCreateFromMemoryBasic(void)
{
    static RGM_TEST_ALIGN_16 uint8_t buf[8 * 32];
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreateFromMemory(buf, sizeof(buf), &fl, 32, 8));
    TEST_ASSERT_EQUAL_UINT32(8,  rgFreeListMaxBlocks(&fl));
    TEST_ASSERT_EQUAL_UINT32(32, rgFreeListBlockSize(&fl));
    TEST_ASSERT_EQUAL_UINT32(8,  rgFreeListBlocksFree(&fl));
    void* p = rgFreeListAlloc(&fl);
    TEST_ASSERT_EQUAL_PTR(buf, p);
}

void rgMemoryTest_freeListCreateFromMemoryNullBufferFails(void)
{
    FreeList fl;
    memset(&fl, 0, sizeof(fl));
    TEST_ASSERT_EQUAL_INT(-1, rgFreeListCreateFromMemory(NULL, 256, &fl, 32, 8));
}

void rgMemoryTest_freeListCreateFromMemoryNullOutFails(void)
{
    static RGM_TEST_ALIGN_16 uint8_t buf[256];
    TEST_ASSERT_EQUAL_INT(-1, rgFreeListCreateFromMemory(buf, sizeof(buf), NULL, 32, 8));
}

void rgMemoryTest_freeListCreateFromMemoryZeroMaxBlocksFails(void)
{
    static RGM_TEST_ALIGN_16 uint8_t buf[256];
    FreeList fl;
    memset(&fl, 0, sizeof(fl));
    TEST_ASSERT_EQUAL_INT(-1, rgFreeListCreateFromMemory(buf, sizeof(buf), &fl, 32, 0));
}

void rgMemoryTest_freeListCreateFromMemoryTooSmallFails(void)
{
    /* Need 8 * 32 = 256 bytes; give only 128. */
    static RGM_TEST_ALIGN_16 uint8_t buf[128];
    FreeList fl;
    memset(&fl, 0, sizeof(fl));
    TEST_ASSERT_EQUAL_INT(-4, rgFreeListCreateFromMemory(buf, sizeof(buf), &fl, 32, 8));
}

void rgMemoryTest_freeListCreateFromMemoryDoesNotTouchBufferAfterReturn(void)
{
    /* There's no Destroy, but the library does not touch _buffer outside of Alloc/Free calls.
     * NOTE the real ownership contract: the caller may write only through blocks currently
     * ALLOCATED to it - a free block's first bytes hold the in-band free-chain link (a
     * pointer) and belong to the library (scribbling them corrupts the chain). So allocate a block first,
     * write through it, and verify the write sticks and the list still round-trips. */
    static RGM_TEST_ALIGN_16 uint8_t buf[8 * 16];
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = 0xCD;
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreateFromMemory(buf, sizeof(buf), &fl, 16, 8));
    uint8_t* block = (uint8_t*)rgFreeListAlloc(&fl);
    TEST_ASSERT_NOT_NULL(block);
    block[0] = 0xAB;
    TEST_ASSERT_EQUAL_UINT8(0xAB, block[0]);
    rgFreeListFree(&fl, block);
    /* The chain is intact: all 8 blocks still allocate. */
    for (int i = 0; i < 8; ++i) TEST_ASSERT_NOT_NULL(rgFreeListAlloc(&fl));
    TEST_ASSERT_NULL(rgFreeListAlloc(&fl));
}

void rgMemoryTest_freeListCreateFromMemoryAllocFreeRoundTrip(void)
{
    /* End-to-end: alloc all, free all, alloc all - over an external buffer. */
    static RGM_TEST_ALIGN_16 uint8_t buf[16 * 16];
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreateFromMemory(buf, sizeof(buf), &fl, 16, 16));
    void* ptrs[16];
    for (int i = 0; i < 16; ++i)
    {
        ptrs[i] = rgFreeListAlloc(&fl);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
        TEST_ASSERT_EQUAL_INT(1, rgFreeListCheckPtr(&fl, ptrs[i]));
    }
    TEST_ASSERT_NULL(rgFreeListAlloc(&fl));
    for (int i = 0; i < 16; ++i) rgFreeListFree(&fl, ptrs[i]);
    TEST_ASSERT_EQUAL_UINT32(16, rgFreeListBlocksFree(&fl));
    for (int i = 0; i < 16; ++i)
    {
        TEST_ASSERT_NOT_NULL(rgFreeListAlloc(&fl));
    }
}

/* -------------------------------------------------------------------------
 * Alloc
 * ------------------------------------------------------------------------- */

void rgMemoryTest_freeListAllocBasic(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    void* p = rgFreeListAlloc(&fl);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(7, rgFreeListBlocksFree(&fl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListAllocReturnsDistinctAddresses(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    void* p1 = rgFreeListAlloc(&fl);
    void* p2 = rgFreeListAlloc(&fl);
    void* p3 = rgFreeListAlloc(&fl);
    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_NOT_NULL(p3);
    TEST_ASSERT_TRUE(p1 != p2);
    TEST_ASSERT_TRUE(p2 != p3);
    TEST_ASSERT_TRUE(p1 != p3);
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListAllocAllBlocks(void)
{
    const uint32_t N = 16;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, N));
    void* ptrs[16];
    for (uint32_t i = 0; i < N; ++i)
    {
        ptrs[i] = rgFreeListAlloc(&fl);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListBlocksFree(&fl));
    TEST_ASSERT_NULL(rgFreeListAlloc(&fl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListAllocBlocksAre16Aligned(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 16, 8));
    for (int i = 0; i < 8; ++i)
    {
        void* p = rgFreeListAlloc(&fl);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_TRUE(is_aligned(p, 16));
    }
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListAllocPointerInBuffer(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    void* p = rgFreeListAlloc(&fl);
    TEST_ASSERT_EQUAL_INT(1, rgFreeListCheckPtr(&fl, p));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListAllocBlocksAreContiguous(void)
{
    const uint32_t N = 4;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, N));
    uint32_t stride = rgFreeListBlockSize(&fl);
    uint8_t* first = (uint8_t*)rgFreeListAlloc(&fl);
    for (uint32_t i = 1; i < N; ++i)
    {
        uint8_t* p = (uint8_t*)rgFreeListAlloc(&fl);
        TEST_ASSERT_EQUAL_PTR(first + (size_t)i * stride, p);
    }
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Free
 * ------------------------------------------------------------------------- */

void rgMemoryTest_freeListFreeIncrementsBlocksFree(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    void* p = rgFreeListAlloc(&fl);
    TEST_ASSERT_EQUAL_UINT32(7, rgFreeListBlocksFree(&fl));
    rgFreeListFree(&fl, p);
    TEST_ASSERT_EQUAL_UINT32(8, rgFreeListBlocksFree(&fl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListFreeReusesAddress(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    void* p1 = rgFreeListAlloc(&fl);
    rgFreeListFree(&fl, p1);
    void* p2 = rgFreeListAlloc(&fl);
    TEST_ASSERT_EQUAL_PTR(p1, p2);
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListFreeAllThenReallocAll(void)
{
    const uint32_t N = 8;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, N));
    void* ptrs[8];
    for (uint32_t i = 0; i < N; ++i) ptrs[i] = rgFreeListAlloc(&fl);
    for (uint32_t i = 0; i < N; ++i) rgFreeListFree(&fl, ptrs[i]);
    TEST_ASSERT_EQUAL_UINT32(N, rgFreeListBlocksFree(&fl));
    for (uint32_t i = 0; i < N; ++i)
    {
        void* p = rgFreeListAlloc(&fl);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL_INT(1, rgFreeListCheckPtr(&fl, p));
    }
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListBlocksFree(&fl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListFreeOntoEmptyChain(void)
{
    /* Drain the pool, then verify Free puts a block back and Alloc returns it. */
    const uint32_t N = 4;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, N));
    void* ptrs[4];
    for (uint32_t i = 0; i < N; ++i) ptrs[i] = rgFreeListAlloc(&fl);
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListBlocksFree(&fl));
    TEST_ASSERT_NULL(rgFreeListAlloc(&fl));
    rgFreeListFree(&fl, ptrs[2]);
    TEST_ASSERT_EQUAL_UINT32(1, rgFreeListBlocksFree(&fl));
    TEST_ASSERT_EQUAL_PTR(ptrs[2], rgFreeListAlloc(&fl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListLifoFreeOrder(void)
{
    const uint32_t N = 4;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, N));
    void* ptrs[4];
    for (uint32_t i = 0; i < N; ++i) ptrs[i] = rgFreeListAlloc(&fl);
    for (int i = (int)N - 1; i >= 0; --i) rgFreeListFree(&fl, ptrs[i]);
    for (uint32_t i = 0; i < N; ++i)
    {
        TEST_ASSERT_EQUAL_PTR(ptrs[i], rgFreeListAlloc(&fl));
    }
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * CheckPtr
 * ------------------------------------------------------------------------- */

void rgMemoryTest_freeListCheckPtrInRange(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    void* p = rgFreeListAlloc(&fl);
    TEST_ASSERT_EQUAL_INT(1, rgFreeListCheckPtr(&fl, p));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCheckPtrLastByte(void)
{
    /* Walk to the buffer end by allocating contiguously. */
    const uint32_t N = 8;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, N));
    uint8_t* first = (uint8_t*)rgFreeListAlloc(&fl);
    for (uint32_t i = 1; i < N; ++i) rgFreeListAlloc(&fl);
    size_t buffer_bytes = (size_t)rgFreeListMaxBlocks(&fl) * rgFreeListBlockSize(&fl);
    TEST_ASSERT_EQUAL_INT(1, rgFreeListCheckPtr(&fl, first + buffer_bytes - 1));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCheckPtrPastEnd(void)
{
    const uint32_t N = 8;
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, N));
    uint8_t* first = (uint8_t*)rgFreeListAlloc(&fl);
    size_t buffer_bytes = (size_t)rgFreeListMaxBlocks(&fl) * rgFreeListBlockSize(&fl);
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCheckPtr(&fl, first + buffer_bytes));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCheckPtrBeforeBuffer(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    uint8_t* first = (uint8_t*)rgFreeListAlloc(&fl);
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCheckPtr(&fl, first - 1));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListCheckPtrNull(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCheckPtr(&fl, NULL));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Integration
 * ------------------------------------------------------------------------- */

void rgMemoryTest_freeListWriteReadSurvivesNeighbourFree(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 64, 4));
    uint8_t* p0 = (uint8_t*)rgFreeListAlloc(&fl);
    uint8_t* p1 = (uint8_t*)rgFreeListAlloc(&fl);
    uint8_t* p2 = (uint8_t*)rgFreeListAlloc(&fl);
    for (int i = 0; i < 64; ++i) { p0[i] = 0xAA; p1[i] = 0xBB; p2[i] = 0xCC; }
    rgFreeListFree(&fl, p1);
    for (int i = 0; i < 64; ++i)
    {
        TEST_ASSERT_EQUAL_UINT8(0xAA, p0[i]);
        TEST_ASSERT_EQUAL_UINT8(0xCC, p2[i]);
    }
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListAllocFreeCycleReusesAddress(void)
{
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    void* first = rgFreeListAlloc(&fl);
    rgFreeListFree(&fl, first);
    for (int iter = 0; iter < 256; ++iter)
    {
        void* p = rgFreeListAlloc(&fl);
        TEST_ASSERT_EQUAL_PTR(first, p);
        rgFreeListFree(&fl, p);
    }
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListLargePool(void)
{
    const uint32_t N = 10000;
    Arena a; rgArenaCreate(&a, 2 * 1024 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 64, N));
    TEST_ASSERT_EQUAL_UINT32(N, rgFreeListBlocksFree(&fl));
    for (uint32_t i = 0; i < N; ++i) TEST_ASSERT_NOT_NULL(rgFreeListAlloc(&fl));
    TEST_ASSERT_EQUAL_UINT32(0, rgFreeListBlocksFree(&fl));
    TEST_ASSERT_NULL(rgFreeListAlloc(&fl));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListMultipleListsCoexist(void)
{
    Arena a; rgArenaCreate(&a, 128 * 1024);
    FreeList fl1, fl2;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl1, 32, 8));
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl2, 64, 8));
    void* p1 = rgFreeListAlloc(&fl1);
    void* p2 = rgFreeListAlloc(&fl2);
    TEST_ASSERT_EQUAL_INT(1, rgFreeListCheckPtr(&fl1, p1));
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCheckPtr(&fl1, p2));
    TEST_ASSERT_EQUAL_INT(1, rgFreeListCheckPtr(&fl2, p2));
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCheckPtr(&fl2, p1));
    rgArenaDestroy(&a);
}

void rgMemoryTest_freeListUsableAfterArenaClear(void)
{
    /* After clearing the arena, the original FreeList's buffer pointer is
     * dangling -- the caller must not use the old struct. Re-creating into
     * the same storage must work and produce a fully usable list. */
    Arena a; rgArenaCreate(&a, 64 * 1024);
    FreeList fl;
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 32, 8));
    TEST_ASSERT_NOT_NULL(rgFreeListAlloc(&fl));
    rgArenaClear(&a);
    TEST_ASSERT_EQUAL_INT(0, rgFreeListCreate(&a, &fl, 64, 4));
    TEST_ASSERT_NOT_NULL(rgFreeListAlloc(&fl));
    rgArenaDestroy(&a);
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_FreeList(void)
{
    RUN_TEST(rgMemoryTest_freeListZeroInitIsInert);
    RUN_TEST(rgMemoryTest_freeListNullPointerIsSafe);

    RUN_TEST(rgMemoryTest_freeListCreateBasic);
    RUN_TEST(rgMemoryTest_freeListCreateNullOutFails);
    RUN_TEST(rgMemoryTest_freeListCreateZeroMaxBlocksFails);
    RUN_TEST(rgMemoryTest_freeListCreateInvalidArenaFails);
    RUN_TEST(rgMemoryTest_freeListCreateClampsSmallBlockSize);
    RUN_TEST(rgMemoryTest_freeListCreateRoundsBlockSizeTo16);
    RUN_TEST(rgMemoryTest_freeListCreateExhaustionReturnsNoMemory);
    RUN_TEST(rgMemoryTest_freeListCreateAfterFailureSucceeds);
    RUN_TEST(rgMemoryTest_freeListCreateOverflowFails);

    RUN_TEST(rgMemoryTest_freeListBufferSizeRoundsUp);
    RUN_TEST(rgMemoryTest_freeListBufferSizeZeroInputs);
    RUN_TEST(rgMemoryTest_freeListCreateFromMemoryBasic);
    RUN_TEST(rgMemoryTest_freeListCreateFromMemoryNullBufferFails);
    RUN_TEST(rgMemoryTest_freeListCreateFromMemoryNullOutFails);
    RUN_TEST(rgMemoryTest_freeListCreateFromMemoryZeroMaxBlocksFails);
    RUN_TEST(rgMemoryTest_freeListCreateFromMemoryTooSmallFails);
    RUN_TEST(rgMemoryTest_freeListCreateFromMemoryDoesNotTouchBufferAfterReturn);
    RUN_TEST(rgMemoryTest_freeListCreateFromMemoryAllocFreeRoundTrip);

    RUN_TEST(rgMemoryTest_freeListAllocBasic);
    RUN_TEST(rgMemoryTest_freeListAllocReturnsDistinctAddresses);
    RUN_TEST(rgMemoryTest_freeListAllocAllBlocks);
    RUN_TEST(rgMemoryTest_freeListAllocBlocksAre16Aligned);
    RUN_TEST(rgMemoryTest_freeListAllocPointerInBuffer);
    RUN_TEST(rgMemoryTest_freeListAllocBlocksAreContiguous);

    RUN_TEST(rgMemoryTest_freeListFreeIncrementsBlocksFree);
    RUN_TEST(rgMemoryTest_freeListFreeReusesAddress);
    RUN_TEST(rgMemoryTest_freeListFreeAllThenReallocAll);
    RUN_TEST(rgMemoryTest_freeListFreeOntoEmptyChain);
    RUN_TEST(rgMemoryTest_freeListLifoFreeOrder);

    RUN_TEST(rgMemoryTest_freeListCheckPtrInRange);
    RUN_TEST(rgMemoryTest_freeListCheckPtrLastByte);
    RUN_TEST(rgMemoryTest_freeListCheckPtrPastEnd);
    RUN_TEST(rgMemoryTest_freeListCheckPtrBeforeBuffer);
    RUN_TEST(rgMemoryTest_freeListCheckPtrNull);

    RUN_TEST(rgMemoryTest_freeListWriteReadSurvivesNeighbourFree);
    RUN_TEST(rgMemoryTest_freeListAllocFreeCycleReusesAddress);
    RUN_TEST(rgMemoryTest_freeListLargePool);
    RUN_TEST(rgMemoryTest_freeListMultipleListsCoexist);
    RUN_TEST(rgMemoryTest_freeListUsableAfterArenaClear);
}
