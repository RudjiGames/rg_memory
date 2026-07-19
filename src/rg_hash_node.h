/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Private header: shared node format and helpers for HashMap and HashTrie.
 *
 * Both structures use the same 4-ary trie node (Wellons, nullprogram
 * 2023-09-30). Inter-node references are full 64-bit absolute pointers,
 * stored as uint64_t so they can be the target of atomic CAS in HashTrie.
 *
 * Key bytes live inline immediately after the HashNode -- the whole
 * (node + key) pair is one arena allocation. This lets several arenas
 * cooperate on the same trie (multi-writer, per-thread arenas) without
 * any shared base pointer: each slot is self-describing.
 *
 * Not part of the public API. Never include from a header in include/.
 */

#ifndef RG_HASH_NODE_H_HEADER_GUARD
#define RG_HASH_NODE_H_HEADER_GUARD

#include "../include/rg_memory/rg_memory.h"
#include "rg_hash_func.h"  /* wyhash + unaligned loads + atomics */

/* 56 bytes, naturally 8-byte aligned. The trie indexes child[h >> 62] at
 * each level, consuming 2 hash bits per descent (h <<= 2). Slots hold
 * absolute HashNode pointers as uint64_t; 0 means "no node".
 *
 * Stores the full wyhash digest in m_hash so the walk can fast-reject
 * on a single uint64 compare and only fall through to the byte-by-byte
 * key compare on hash match. The key bytes are still stored inline (at
 * (node + 1)) as the safety net for hash collisions -- wyhash is fast
 * and well-distributed but not collision-resistant. For an 8-byte key
 * the entire node + key fits in one 64-byte cache line; for larger
 * keys the key spills onto a second line, but the fast-reject pays for
 * itself when keys are long enough that the byte loop dominates a
 * no-hash walk. */
typedef struct HashNode
{
    uint64_t  m_child[4];    /* absolute pointers to children, 0 = 0.        */
    uint64_t  m_hash;        /* full wyhash digest, for fast-reject in walks.   */
    uint64_t  m_value;       /* opaque 64-bit value (cast pointers via uintptr).*/
    uint32_t  m_keyLen;      /* length of the key in bytes.                     */
    /* The compiler appends 4 bytes of trailing padding to bring sizeof to
     * 56 (a multiple of the 8-byte struct alignment). Key bytes follow
     * after that, i.e. at (node + 1), length m_keyLen. */

} HashNode;

/* Pointer to the inline key bytes for a node. 0 key bytes for a
 * zero-length key are not allocated; m_keyLen == 0 is the marker. */
static RGM_FORCEINLINE const uint8_t* rgm_hash_node_key(const HashNode* _node)
{
    return (const uint8_t*)(_node + 1);
}

/* Key bytes for a node, honouring a trie's copy / nocopy mode. In copy mode
 * (_nocopy == 0) the key bytes follow the node inline, exactly like
 * rgm_hash_node_key. In nocopy mode the region at (node + 1) instead holds a
 * pointer to the caller-owned key, so dereference it. The branch is on a
 * value the caller (the trie) holds in a register and is only reached on a
 * hash-match candidate, so it stays off the bulk of the walk. */
static RGM_FORCEINLINE const uint8_t* rgm_hash_node_key_ext(const HashNode* _node, int _nocopy)
{
    if (_nocopy)
    {
        return (const uint8_t*)*(const void* const*)(const void*)(_node + 1);
    }
    return (const uint8_t*)(_node + 1);
}

/* Equality test for keys. Runs only after the m_hash + m_keyLen fast-reject,
 * i.e. ~once per lookup HIT, and hits dominate symbol-intern / string-table
 * traffic -- so it pays to compare 8 bytes at a time instead of one. Reads
 * stay strictly within [0,_len) on BOTH operands (block loop + byte tail), so
 * there is no over-read past either buffer's end. CRT-free, matching the
 * file's style. */
static RGM_FORCEINLINE int rgm_hash_keys_equal(const void* _a, const void* _b, uint64_t _len)
{
    const uint8_t* pa = (const uint8_t*)_a;
    const uint8_t* pb = (const uint8_t*)_b;
    uint64_t i = 0;
    for (; i + 8u <= _len; i += 8u)
    {
        if (rgm_hash_load_u64(pa + i) != rgm_hash_load_u64(pb + i))
        {
            return 0;
        }
    }
    for (; i < _len; ++i)
    {
        if (pa[i] != pb[i])
        {
            return 0;
        }
    }
    return 1;
}

/* Copy _len bytes from _src to _dst. CRT-free, byte loop; the compiler
 * vectorises it for typical key sizes. */
static inline void rgm_hash_copy_bytes(void* _dst, const void* _src, uint64_t _len)
{
    uint8_t*       d = (uint8_t*)_dst;
    const uint8_t* s = (const uint8_t*)_src;
    uint64_t i;
    for (i = 0; i < _len; ++i)
    {
        d[i] = s[i];
    }
}

/* Allocate a HashNode + inline key bytes in one arena allocation, fill
 * in the fields, zero the child array, set the hash + value. Returns
 * 0 if the arena is exhausted. _keyLen may be zero, in which case
 * only the HashNode itself is allocated.
 *
 * Single-allocation layout means the node carries no offset/handle to
 * its key bytes -- you find them at (node + 1). That keeps node slots
 * self-contained across arenas.
 *
 * Aligned to 64 bytes (one cache line). The 56-byte fixed part plus an
 * 8-byte key fits inside a single line, so a traversal step reads
 * exactly one line. With the default 8-byte alignment ~50% of nodes
 * straddled two lines (56 B doesn't tile 64), doubling the line load
 * count and the inter-core invalidation traffic under multi-writer Put.
 * Cost is at most 8 B of trailing padding per 0-key node. */
static inline HashNode* rgm_hash_node_create(Arena* _arena, uint64_t _hash,
                                             const void* _key, uint64_t _keyLen,
                                             uint64_t _value)
{
    uint64_t total = sizeof(HashNode) + _keyLen;
    HashNode* node = (HashNode*)rgArenaAllocAligned(_arena, total, RGM_CACHE_LINE);
    if (node == 0)
    {
        return 0;
    }

    node->m_child[0] = 0;
    node->m_child[1] = 0;
    node->m_child[2] = 0;
    node->m_child[3] = 0;
    node->m_hash     = _hash;
    node->m_value    = _value;
    node->m_keyLen   = (uint32_t)_keyLen;

    if (_keyLen != 0)
    {
        rgm_hash_copy_bytes((uint8_t*)(node + 1), _key, _keyLen);
    }
    return node;
}

/* NOCOPY node create: store a POINTER to the caller's key at (node + 1)
 * instead of copying the bytes. The fixed HashNode part is unchanged, so the
 * node never grows; only a single pointer's worth of inline space is used
 * regardless of key length. The caller guarantees the key bytes outlive the
 * node and never change (this is the contract of a NOCOPY HashTrie). For
 * keys longer than one pointer this is a net memory win -- the node + its
 * key reference fit in one cache line whatever the key length. */
static inline HashNode* rgm_hash_node_create_nocopy(Arena* _arena, uint64_t _hash,
                                                    const void* _key, uint64_t _keyLen,
                                                    uint64_t _value)
{
    HashNode* node = (HashNode*)rgArenaAllocAligned(
        _arena, sizeof(HashNode) + sizeof(const void*), RGM_CACHE_LINE);
    if (node == 0)
    {
        return 0;
    }

    node->m_child[0] = 0;
    node->m_child[1] = 0;
    node->m_child[2] = 0;
    node->m_child[3] = 0;
    node->m_hash     = _hash;
    node->m_value    = _value;
    node->m_keyLen   = (uint32_t)_keyLen;

    *(const void**)(void*)(node + 1) = _key;
    return node;
}

#endif /* RG_HASH_NODE_H_HEADER_GUARD */
