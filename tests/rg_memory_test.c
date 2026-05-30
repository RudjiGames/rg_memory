/*
 * Imagus Engine -  Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 */

#include <rg_memory_test_pch.h>

#include <stdio.h>

/* Unity hooks. No per-test setup or teardown needed here. */
void setUp(void)    {}
void tearDown(void) {}

extern void rgMemoryTest_Arena(void);
extern void rgMemoryTest_FreeList(void);
extern void rgMemoryTest_DenseList(void);
extern void rgMemoryTest_HashMap(void);
extern void rgMemoryTest_HashTrie(void);
extern void rgMemoryTest_HashTrieMT(void);
extern void rgMemoryTest_HashIndex(void);
extern void rgMemoryTest_BloomFilter(void);
extern void rgMemoryTest_Persist(void);
extern void rgMemoryTest_HashBench(void);

int main(int _argc, char* _argv[])
{
	(void)_argc;
	(void)_argv;

	printf("Running tests for: rg_memory\n");

	UNITY_BEGIN();
	RUN_TEST(rgMemoryTest_Arena);
	RUN_TEST(rgMemoryTest_FreeList);
	RUN_TEST(rgMemoryTest_DenseList);
	RUN_TEST(rgMemoryTest_HashMap);
	RUN_TEST(rgMemoryTest_HashTrie);
	RUN_TEST(rgMemoryTest_HashTrieMT);
	RUN_TEST(rgMemoryTest_HashIndex);
	RUN_TEST(rgMemoryTest_BloomFilter);
	RUN_TEST(rgMemoryTest_Persist);
	RUN_TEST(rgMemoryTest_HashBench);
	return UNITY_END();
}
