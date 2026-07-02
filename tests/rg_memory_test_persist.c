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

/* -------------------------------------------------------------------------
 * Arena reuse: a map re-Init'd on a Cleared arena must not let Save trust
 * the previous session's persist header (rgArenaClear keeps committed
 * pages, so the stale header bytes survive at offset 0)
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapArenaReuseAfterClear(void)
{
    remove(RGM_PERSIST_PATH2);

    Arena a;
    TEST_ASSERT_EQUAL_INT(0, rgArenaCreateShared(&a, RGM_PERSIST_PATH2, 8 * 1024 * 1024));

    /* Session A: build + save a first map, then discard it with Clear. */
    {
        HashMap m;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapInit(&m, &a));
        for (uint32_t i = 0; i < 512; ++i)
        {
            uint64_t* v = rgHashMapPutU64(&m, 0xA0000000ull + i);
            TEST_ASSERT_NOT_NULL(v);
            *v = i;
        }
        TEST_ASSERT_EQUAL_INT(0, rgHashMapSave(&m));
    }

    /* Session B: reuse the arena for a brand-new map. A Save that trusted
     * the stale header would park the index at session A's offset -- inside
     * the region session B's nodes grow into -- and the second Save below
     * would overwrite live nodes. */
    rgArenaClear(&a);
    {
        HashMap m;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapInit(&m, &a));
        for (uint32_t i = 0; i < 2048; ++i)
        {
            uint64_t* v = rgHashMapPutU64(&m, 0xB0000000ull + i);
            TEST_ASSERT_NOT_NULL(v);
            *v = (uint64_t)i * 7u + 3u;
        }
        TEST_ASSERT_EQUAL_INT(0, rgHashMapSave(&m));

        for (uint32_t i = 2048; i < 4096; ++i)
        {
            uint64_t* v = rgHashMapPutU64(&m, 0xB0000000ull + i);
            TEST_ASSERT_NOT_NULL(v);
            *v = (uint64_t)i * 7u + 3u;
        }
        TEST_ASSERT_EQUAL_INT(0, rgHashMapSave(&m));

        /* The live map must be intact after the re-save. */
        for (uint32_t i = 0; i < 4096; ++i)
        {
            uint64_t* v = rgHashMapGetU64(&m, 0xB0000000ull + i);
            TEST_ASSERT_NOT_NULL(v);
            TEST_ASSERT_EQUAL_UINT64((uint64_t)i * 7u + 3u, *v);
        }
    }
    rgArenaFlush(&a);
    rgArenaDestroy(&a);

    /* Reopen: only the second map's entries exist, all values correct. */
    {
        Arena a2;
        TEST_ASSERT_EQUAL_INT(0, rgArenaOpenShared(&a2, RGM_PERSIST_PATH2, 0));
        HashMap m;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapOpen(&m, &a2));
        for (uint32_t i = 0; i < 4096; ++i)
        {
            uint64_t* v = rgHashMapGetU64(&m, 0xB0000000ull + i);
            TEST_ASSERT_NOT_NULL(v);
            TEST_ASSERT_EQUAL_UINT64((uint64_t)i * 7u + 3u, *v);
        }
        TEST_ASSERT_NULL(rgHashMapGetU64(&m, 0xA0000000ull));
        rgArenaDestroy(&a2);
    }

    remove(RGM_PERSIST_PATH2);
}

/* -------------------------------------------------------------------------
 * Open must reject a header whose index offset would wrap the bounds check
 * ------------------------------------------------------------------------- */

void rgMemoryTest_hashMapOpenRejectsCorruptIndexOffset(void)
{
    remove(RGM_PERSIST_PATH2);

    /* Build a valid persisted map first. */
    {
        Arena a;
        TEST_ASSERT_EQUAL_INT(0, rgArenaCreateShared(&a, RGM_PERSIST_PATH2, 1024 * 1024));
        HashMap m;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapInit(&m, &a));
        uint64_t* v = rgHashMapPutU64(&m, 1234u);
        TEST_ASSERT_NOT_NULL(v);
        *v = 5678u;
        TEST_ASSERT_EQUAL_INT(0, rgHashMapSave(&m));
        rgArenaFlush(&a);
        rgArenaDestroy(&a);
    }

    /* Corrupt the header's m_indexOffset (bytes 24..31: after the 8-byte
     * magic, two 4-byte config fields and the 8-byte high-water mark) with
     * a value that would wrap a naive `offset + indexBytes > cap` check. */
    {
        FILE* f = fopen(RGM_PERSIST_PATH2, "r+b");
        TEST_ASSERT_NOT_NULL(f);
        uint64_t evil = 0xFFFFFFFFFFFFC000ull;
        TEST_ASSERT_EQUAL_INT(0, fseek(f, 24, SEEK_SET));
        TEST_ASSERT_EQUAL_size_t(1, fwrite(&evil, sizeof(evil), 1, f));
        fclose(f);
    }

    /* Open must fail cleanly with a format error, not crash on a wild read. */
    {
        Arena a;
        TEST_ASSERT_EQUAL_INT(0, rgArenaOpenShared(&a, RGM_PERSIST_PATH2, 0));
        HashMap m;
        TEST_ASSERT_EQUAL_INT(RGM_ERROR_ERR_FORMAT, rgHashMapOpen(&m, &a));
        rgArenaDestroy(&a);
    }

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
    RUN_TEST(rgMemoryTest_hashMapArenaReuseAfterClear);
    RUN_TEST(rgMemoryTest_hashMapOpenRejectsCorruptIndexOffset);
    RUN_TEST(rgMemoryTest_hashMapSaveNullSafe);
}
