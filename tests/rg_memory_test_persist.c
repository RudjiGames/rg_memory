/*
 * Imagus Engine -  Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * File-backed (shared-memory) arena + HashMap persistence.
 *
 * Exercises the round trip that makes a HashMap durable: build it in a
 * file-backed arena, rgHashMapSave the top-level index into the file,
 * unmap, then rgArenaOpenShared + rgHashMapOpen at a fresh mapping (very
 * likely a different base address) and confirm every entry still resolves.
 * The node graph is position-independent because all inter-node links are
 * arena-relative offsets; this test is what proves that end to end.
 *
 * The tests write a small scratch file in the current working directory and
 * remove it afterwards.
 */

#include <rg_memory_test_pch.h>

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define RGM_PERSIST_PATH "rg_memory_persist_test.bin"
#define RGM_PERSIST_PATH2 "rg_memory_persist_test2.bin"

/* -------------------------------------------------------------------------
 * Shared (file-backed) arena behaves like a normal arena
 * ------------------------------------------------------------------------- */

void rgMemoryTest_sharedArenaBasicAlloc(void)
{
    remove(RGM_PERSIST_PATH);

    Arena a;
    TEST_ASSERT_EQUAL_INT(0, rgArenaCreateShared(&a, RGM_PERSIST_PATH, 1 * 1024 * 1024));
    TEST_ASSERT_TRUE(rgArenaIsValid(&a));

    /* Allocations work exactly like the anonymous arena. */
    uint32_t* p = (uint32_t*)rgArenaAlloc(&a, 256 * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(p);
    for (uint32_t i = 0; i < 256; ++i) p[i] = i * 3u + 1u;
    for (uint32_t i = 0; i < 256; ++i) TEST_ASSERT_EQUAL_UINT32(i * 3u + 1u, p[i]);

    /* Shrink is a documented no-op on file-backed arenas (must not corrupt). */
    rgArenaShrink(&a);
    TEST_ASSERT_EQUAL_UINT32(0u, p[0] - 1u); /* p still valid after shrink. */

    rgArenaFlush(&a); /* must be safe to call. */
    rgArenaDestroy(&a);
    TEST_ASSERT_FALSE(rgArenaIsValid(&a));

    remove(RGM_PERSIST_PATH);
}

/* -------------------------------------------------------------------------
 * Full HashMap persistence round trip
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapPersistRoundTrip(void)
{
    remove(RGM_PERSIST_PATH);

    const uint32_t N = 4000;

    /* Session 1: build + save + destroy. */
    {
        Arena a;
        TEST_ASSERT_EQUAL_INT(0, rgArenaCreateShared(&a, RGM_PERSIST_PATH, 8 * 1024 * 1024));
        HashMap m;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapInit(&m, &a));

        for (uint32_t i = 0; i < N; ++i)
        {
            uint64_t* v = rgHashMapPutU64(&m, (uint64_t)i * 1000u + 7u);
            TEST_ASSERT_NOT_NULL(v);
            *v = (uint64_t)i * 3u + 11u;
        }
        /* A handful of string keys too. */
        char buf[32];
        for (int i = 0; i < 64; ++i)
        {
            int n = snprintf(buf, sizeof(buf), "asset/%d.bin", i);
            uint64_t* v = rgHashMapPut(&m, buf, (size_t)n);
            TEST_ASSERT_NOT_NULL(v);
            *v = (uint64_t)(100000 + i);
        }

        TEST_ASSERT_EQUAL_INT(0, rgHashMapSave(&m));
        rgArenaFlush(&a);
        rgArenaDestroy(&a);
    }

    /* Session 2: reopen (fresh mapping), verify, append, re-save. */
    {
        Arena a;
        TEST_ASSERT_EQUAL_INT(0, rgArenaOpenShared(&a, RGM_PERSIST_PATH, 0));
        HashMap m;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapOpen(&m, &a));

        for (uint32_t i = 0; i < N; ++i)
        {
            uint64_t* v = rgHashMapGetU64(&m, (uint64_t)i * 1000u + 7u);
            TEST_ASSERT_NOT_NULL(v);
            TEST_ASSERT_EQUAL_UINT64((uint64_t)i * 3u + 11u, *v);
        }
        char buf[32];
        for (int i = 0; i < 64; ++i)
        {
            int n = snprintf(buf, sizeof(buf), "asset/%d.bin", i);
            uint64_t* v = rgHashMapGet(&m, buf, (size_t)n);
            TEST_ASSERT_NOT_NULL(v);
            TEST_ASSERT_EQUAL_UINT64((uint64_t)(100000 + i), *v);
        }
        /* A key that was never inserted must still miss. */
        TEST_ASSERT_NULL(rgHashMapGetU64(&m, 0xFFFFFFFFull));

        /* Append a new entry after reopening, then checkpoint again. */
        uint64_t* nv = rgHashMapPutU64(&m, 424242ull);
        TEST_ASSERT_NOT_NULL(nv);
        *nv = 777u;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapSave(&m));
        rgArenaDestroy(&a);
    }

    /* Session 3: read-only reopen sees the appended entry. */
    {
        Arena a;
        TEST_ASSERT_EQUAL_INT(0, rgArenaOpenShared(&a, RGM_PERSIST_PATH, 1));
        HashMap m;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapOpen(&m, &a));

        uint64_t* v = rgHashMapGetU64(&m, 424242ull);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_EQUAL_UINT64(777u, *v);

        /* An original entry survives across all three sessions. */
        uint64_t* v0 = rgHashMapGetU64(&m, 7u);
        TEST_ASSERT_NOT_NULL(v0);
        TEST_ASSERT_EQUAL_UINT64(11u, *v0);

        rgArenaDestroy(&a);
    }

    remove(RGM_PERSIST_PATH);
}

/* -------------------------------------------------------------------------
 * Open rejects an arena that holds no compatible persisted map
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapOpenRejectsGarbage(void)
{
    remove(RGM_PERSIST_PATH2);

    Arena a;
    TEST_ASSERT_EQUAL_INT(0, rgArenaCreateShared(&a, RGM_PERSIST_PATH2, 64 * 1024));
    /* Nothing was ever saved into this arena -> header magic absent. */
    HashMap m;
    TEST_ASSERT_EQUAL_INT(RGM_ERROR_ERR_FORMAT, rgHashMapOpen(&m, &a));
    rgArenaDestroy(&a);

    remove(RGM_PERSIST_PATH2);
}

void rgMemoryTest_hashMapSaveNullSafe(void)
{
    TEST_ASSERT_EQUAL_INT(RGM_ERROR_ERR_INVALID, rgHashMapSave(NULL));
    TEST_ASSERT_EQUAL_INT(RGM_ERROR_ERR_INVALID, rgHashMapOpen(NULL, NULL));
}

/* -------------------------------------------------------------------------
 * Entry point invoked from rg_memory_test.c
 * ------------------------------------------------------------------------- */

void rgMemoryTest_Persist(void)
{
    RUN_TEST(rgMemoryTest_sharedArenaBasicAlloc);
    RUN_TEST(rgMemoryTest_hashMapPersistRoundTrip);
    RUN_TEST(rgMemoryTest_hashMapOpenRejectsGarbage);
    RUN_TEST(rgMemoryTest_hashMapSaveNullSafe);
}
