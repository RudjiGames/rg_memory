/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * HashIndex: concurrent lock-free hash-keyed lookup table that stores
 * only two 64-bit hashes per entry, never the original key bytes.
 *
 * Each node holds:
 *   - 4 child slots (absolute 64-bit pointers, same trie shape as
 *     HashTrie / HashMap)
 *   - m_h1: wyhash digest of the key, seeded with RGM_HASH_WYP0. Drives
 *     descent (h_shift >> 62) and is the fast-reject compare during walk.
 *   - m_h2: wyhash digest of the key, seeded with RGM_HASH_INDEX_H2_SEED
 *     (a different one of the wyhash secret constants). Pure equality
 *     check at the terminal match site -- the "safety net" against the
 *     rare event that h1 collides between two different keys. Two
 *     different seeds drive the same recurrence to statistically
 *     independent outputs, giving ~2^128 effective collision space
 *     against non-adversarial input distributions.
 *   - m_value
 *
 * Compared to HashTrie + inline key storage:
 *   + Constant per-entry size (56 B before cache-line padding) regardless
 *     of key length -- big memory win for long string / path keys.
 *   + Terminal compare is one uint64 == instead of a byte loop.
 *   + The key buffer passed to Put is not retained, so the caller can
 *     free / mutate it immediately after Put returns.
 *   - Cannot recover the original key during iteration.
 *   - Theoretical adversarial-collision exposure -- not for inputs an
 *     attacker controls without ruling out the case. Both digests come
 *     from the same hash family (wyhash with two seeds), so an attack
 *     that defeats wyhash defeats both at once. If you need diversity-
 *     against-attacks, layer an unrelated MAC (e.g. SipHash) on top.
 *
 * Concurrency: identical contract to HashTrie. Lock-free Get/Put via
 * release-CAS publish + acquire-load reads. Multi-writer with one Arena
 * per writer thread (rgArenaAlloc remains single-threaded by convention).
 */

#include "../include/rg_memory/rg_memory.h"
#include "rg_hash_func.h"  /* wyhash + unaligned loads + atomics + cache line + restrict. */

/* Alternate seed for the m_h2 hash. Picked from the wyhash v4 secret
 * pool (WYP2). Different from the default WYP0 seed used by m_h1, so
 * calling the same recurrence with these two seeds produces two
 * statistically independent digests of any input. */
#define RGM_HASH_INDEX_H2_SEED RGM_HASH_WYP2

/* -------------------------------------------------------------------------
 * Node and helpers
 * ------------------------------------------------------------------------- */

/* 56 bytes; cache-line aligned at allocation so each node owns one line. */
typedef struct HashIndexNode
{
    uint64_t  m_child[4];  /* absolute child pointers, 0 = 0. */
    uint64_t  m_h1;        /* wyhash(key, WYP0) -- descent + fast reject. */
    uint64_t  m_h2;        /* wyhash(key, WYP2) -- terminal equality check. */
    uint64_t  m_value;
} HashIndexNode;

/* Allocate a node, cache-line aligned, fully initialised. */
static HashIndexNode* rgm_hash_index_node_create(Arena* _arena,
                                                 uint64_t _h1, uint64_t _h2,
                                                 uint64_t _value)
{
    HashIndexNode* node = (HashIndexNode*)rgArenaAllocAligned(
        _arena, sizeof(HashIndexNode), RGM_CACHE_LINE);
    if (node == 0)
    {
        return 0;
    }
    node->m_child[0] = 0;
    node->m_child[1] = 0;
    node->m_child[2] = 0;
    node->m_child[3] = 0;
    node->m_h1       = _h1;
    node->m_h2       = _h2;
    node->m_value    = _value;
    return node;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Walk-entry helpers. With the top-level index enabled, consume the top
 * RG_HASH_TOP_BITS of h1 to pick a top slot and shift the working register
 * past them; without it, the entry slot is m_root and the working register
 * is h1 itself. Body code that uses these macros is identical under both
 * modes. */
#if RG_HASH_USE_TOP_INDEX
#  define RGM_HASH_INDEX_DESCEND(_h)   ((_h) << RG_HASH_TOP_BITS)
#  define RGM_HASH_INDEX_ROOT(_i, _h)  (&(_i)->m_top[(uint64_t)((_h) >> (64u - RG_HASH_TOP_BITS))])
#else
#  define RGM_HASH_INDEX_DESCEND(_h)   (_h)
#  define RGM_HASH_INDEX_ROOT(_i, _h)  (&(_i)->m_root)
#endif

int32_t rgHashIndexInit(HashIndex* _idx)
{
    if (_idx == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }
#if RG_HASH_USE_TOP_INDEX
    /* Zero the 32 KiB top-level index. */
    uint64_t i;
    for (i = 0; i < RG_HASH_TOP_SIZE; ++i)
    {
        _idx->m_top[i] = 0;
    }
#else
    _idx->m_root = 0;
#endif
    return RGM_ERROR_OK;
}

/* Core pre-hashed Put. The byte-key Put computes hashes then forwards here. */
int32_t rgHashIndexPutH(HashIndex* restrict _idx, Arena* restrict _arena,
                        uint64_t _h1, uint64_t _h2, uint64_t _value)
{
    if (_idx == 0 || _arena == 0 || _arena->m_base == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t        h     = RGM_HASH_INDEX_DESCEND(_h1);  /* shifting copy used for descent. */
    rgm_atomic_i64* slot  = (rgm_atomic_i64*)RGM_HASH_INDEX_ROOT(_idx, _h1);
    uint64_t        cur   = (uint64_t)rgm_atomic_load_i64(slot);
    uint32_t        round = 0;

    for (;;)
    {
        if (cur == 0)
        {
            HashIndexNode* node = rgm_hash_index_node_create(_arena, _h1, _h2, _value);
            if (node == 0)
            {
                return RGM_ERROR_ERR_NO_MEMORY;
            }
            uint64_t desired = (uint64_t)(uintptr_t)node;

            /* CAS-with-prior: on success returns 0 (our expected); on
             * failure returns the winner's published pointer. The trie's
             * no-overwrite invariant means another load would have
             * returned the same prior value anyway -- skip it. */
            int64_t prior = rgm_atomic_cas_val_i64(slot, 0, (int64_t)desired);
            if (prior == 0)
            {
                return RGM_ERROR_OK;
            }
            cur = (uint64_t)prior;
            /* Fall through to match-or-descend with `cur` populated. */
        }

        HashIndexNode* n = (HashIndexNode*)(uintptr_t)cur;
        if (n->m_h1 == _h1 && n->m_h2 == _h2)
        {
            /* Match -- overwrite value atomically. */
            rgm_atomic_store_i64((rgm_atomic_i64*)&n->m_value, (int64_t)_value);
            return RGM_ERROR_OK;
        }

        slot = (rgm_atomic_i64*)&n->m_child[h >> 62];
        h  <<= 2;
        if (h == 0) { h = rgm_hash_reseed_h(_h1, _h2, ++round); }
        cur = (uint64_t)rgm_atomic_load_i64(slot);
    }
}

/* Core pre-hashed Get. */
int32_t rgHashIndexGetH(HashIndex* restrict _idx,
                        uint64_t _h1, uint64_t _h2, uint64_t* restrict _outValue)
{
    if (_idx == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t              h     = RGM_HASH_INDEX_DESCEND(_h1);
    const rgm_atomic_i64* slot  = (const rgm_atomic_i64*)RGM_HASH_INDEX_ROOT(_idx, _h1);
    uint32_t              round = 0;

    for (;;)
    {
        uint64_t cur = (uint64_t)rgm_atomic_load_i64(slot);
        if (cur == 0)
        {
            return RGM_ERROR_ERR_INVALID; /* not found */
        }

        HashIndexNode* n = (HashIndexNode*)(uintptr_t)cur;
        if (n->m_h1 == _h1 && n->m_h2 == _h2)
        {
            if (_outValue != 0)
            {
                *_outValue = (uint64_t)rgm_atomic_load_i64(
                                (const rgm_atomic_i64*)&n->m_value);
            }
            return RGM_ERROR_OK;
        }

        slot = (const rgm_atomic_i64*)&n->m_child[h >> 62];
        h  <<= 2;
        if (h == 0) { h = rgm_hash_reseed_h(_h1, _h2, ++round); }
    }
}

/* Byte-key Put: hash key with both functions then dispatch to PutH. */
int32_t rgHashIndexPut(HashIndex* _idx, Arena* _arena,
                       const void* _key, uint64_t _keyLen, uint64_t _value)
{
    if (_idx == 0 || _arena == 0 || _arena->m_base == 0
     || (_key == 0 && _keyLen != 0))
    {
        return RGM_ERROR_ERR_INVALID;
    }
    /* One pass over the key for both digests (byte-identical to the two separate calls,
     * so m_h2's documented RGM_HASH_INDEX_H2_SEED contract is unchanged). */
    uint64_t h1, h2;
    rgm_hash_wyhash_dual_seeded(_key, _keyLen, RGM_HASH_WYP0, RGM_HASH_INDEX_H2_SEED, &h1, &h2);
    return rgHashIndexPutH(_idx, _arena, h1, h2, _value);
}

/* Byte-key Get. */
int32_t rgHashIndexGet(HashIndex* _idx,
                       const void* _key, uint64_t _keyLen, uint64_t* _outValue)
{
    if (_idx == 0 || (_key == 0 && _keyLen != 0))
    {
        return RGM_ERROR_ERR_INVALID;
    }
    uint64_t h1, h2;
    rgm_hash_wyhash_dual_seeded(_key, _keyLen, RGM_HASH_WYP0, RGM_HASH_INDEX_H2_SEED, &h1, &h2);
    return rgHashIndexGetH(_idx, h1, h2, _outValue);
}

/* -------------------------------------------------------------------------
 * Bulk pre-hashed API. Puts share no walk state across keys (a Put may
 * publish a node a later Put traverses), so PutBatchH is a loop. GetBatchH
 * uses the standard K=8 software-pipelined walk pattern.
 * ------------------------------------------------------------------------- */

int32_t rgHashIndexPutBatchH(HashIndex* restrict _idx, Arena* restrict _arena,
                             const uint64_t* restrict _h1s,
                             const uint64_t* restrict _h2s,
                             const uint64_t* restrict _values,
                             uint32_t _count)
{
    if (_idx == 0 || _arena == 0 || _arena->m_base == 0
     || _h1s == 0 || _h2s == 0 || _values == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }
    uint32_t i;
    for (i = 0; i < _count; ++i)
    {
        int32_t rc = rgHashIndexPutH(_idx, _arena, _h1s[i], _h2s[i], _values[i]);
        if (rc != RGM_ERROR_OK) return rc;
    }
    return RGM_ERROR_OK;
}

#define RGM_HASH_INDEX_BATCH_K 8u

int32_t rgHashIndexGetBatchH(HashIndex* restrict _idx,
                             const uint64_t* restrict _h1s,
                             const uint64_t* restrict _h2s,
                             uint64_t* restrict _outValues,
                             int* restrict _outFound,
                             uint32_t _count)
{
    if (_idx == 0 || _h1s == 0 || _h2s == 0 || _outValues == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    /* 64-bit accumulator: _count is u32, so > 2^31 hits would overflow int32 (negative return
     * colliding with RGM_ERROR_*). Clamped at return. */
    uint64_t hits = 0;

    uint64_t              h    [RGM_HASH_INDEX_BATCH_K];
    const rgm_atomic_i64* slot [RGM_HASH_INDEX_BATCH_K];
    uint8_t               active[RGM_HASH_INDEX_BATCH_K];
    uint32_t              round [RGM_HASH_INDEX_BATCH_K];

    uint32_t base;
    for (base = 0; base < _count; base += RGM_HASH_INDEX_BATCH_K)
    {
        uint32_t n = (_count - base) < RGM_HASH_INDEX_BATCH_K
                   ? (_count - base) : RGM_HASH_INDEX_BATCH_K;

        uint32_t i;
        for (i = 0; i < n; ++i)
        {
            uint64_t h1 = _h1s[base + i];
            h[i]      = RGM_HASH_INDEX_DESCEND(h1);
            slot[i]   = (const rgm_atomic_i64*)RGM_HASH_INDEX_ROOT(_idx, h1);
            active[i] = 1;
            round[i]  = 0;
            if (_outFound) _outFound[base + i] = 0;
        }

        int any = 1;
        while (any)
        {
            any = 0;
            for (i = 0; i < n; ++i)
            {
                if (!active[i]) continue;

                uint64_t cur = (uint64_t)rgm_atomic_load_i64(slot[i]);
                if (cur == 0)
                {
                    active[i] = 0;
                    continue;
                }

                HashIndexNode* node = (HashIndexNode*)(uintptr_t)cur;
                if (node->m_h1 == _h1s[base + i] && node->m_h2 == _h2s[base + i])
                {
                    _outValues[base + i] = (uint64_t)rgm_atomic_load_i64(
                                                (const rgm_atomic_i64*)&node->m_value);
                    if (_outFound) _outFound[base + i] = 1;
                    active[i] = 0;
                    hits++;
                    continue;
                }

                slot[i] = (const rgm_atomic_i64*)&node->m_child[h[i] >> 62];
                h[i]  <<= 2;
                if (h[i] == 0) { h[i] = rgm_hash_reseed_h(_h1s[base + i], _h2s[base + i], ++round[i]); }
                any = 1;
            }
        }
    }

    return hits > (uint64_t)INT32_MAX ? INT32_MAX : (int32_t)hits;
}

/* -------------------------------------------------------------------------
 * Iteration. Same iterative pre-order DFS as HashTrieForEach with atomic
 * acquire-loads, but the callback receives (h1, h2, value) -- no key
 * bytes to pass since they were never stored.
 * ------------------------------------------------------------------------- */

typedef struct rgm_hash_index_iter_frame
{
    HashIndexNode* node;
    uint32_t       visited;
    uint32_t       nextChild;
} rgm_hash_index_iter_frame;

#define RGM_HASH_INDEX_ITER_STACK_DEPTH 64

/* Recursive fallback for chains deeper than the inline explicit stack. Unlike
 * HashMap/HashTrie (which hash their keys internally), HashIndex takes the
 * caller's pre-hashed (h1,h2), so depth is fully caller-controlled and deep
 * chains are easy to construct -- this path must fully visit the subtree
 * instead of the old release no-op that silently dropped it. */
static uint64_t rgm_hash_index_foreach_rec(HashIndexNode* _node,
                                           rgHashIndexForEachFn _fn, void* _userData, int* _stop)
{
    uint64_t count = 1;
    int i;
    uint64_t v = (uint64_t)rgm_atomic_load_i64((const rgm_atomic_i64*)&_node->m_value);
    *_stop = _fn(_node->m_h1, _node->m_h2, v, _userData);
    if (*_stop) return count;
    for (i = 0; i < 4; ++i)
    {
        uint64_t cur = (uint64_t)rgm_atomic_load_i64((const rgm_atomic_i64*)&_node->m_child[i]);
        if (cur != 0)
        {
            count += rgm_hash_index_foreach_rec((HashIndexNode*)(uintptr_t)cur, _fn, _userData, _stop);
            if (*_stop) break;
        }
    }
    return count;
}

uint64_t rgHashIndexForEach(HashIndex* _idx, rgHashIndexForEachFn _fn, void* _userData)
{
    if (_idx == 0 || _fn == 0)
    {
        return 0;
    }

    rgm_hash_index_iter_frame stack[RGM_HASH_INDEX_ITER_STACK_DEPTH];
    uint64_t count = 0;
    int      stop  = 0;

#if RG_HASH_USE_TOP_INDEX
    /* Walk every populated top-level subtrie in index order. */
    uint64_t top;
    for (top = 0; top < RG_HASH_TOP_SIZE && !stop; ++top)
    {
        uint64_t rootCur = (uint64_t)rgm_atomic_load_i64(
                                (const rgm_atomic_i64*)&_idx->m_top[top]);
        if (rootCur == 0)
        {
            continue;
        }
#else
    {
        uint64_t rootCur = (uint64_t)rgm_atomic_load_i64(
                                (const rgm_atomic_i64*)&_idx->m_root);
        if (rootCur == 0)
        {
            return 0;
        }
#endif

        uint32_t sp        = 0;
        stack[sp].node     = (HashIndexNode*)(uintptr_t)rootCur;
        stack[sp].visited  = 0;
        stack[sp].nextChild = 0;
        sp++;

        while (sp > 0 && !stop)
        {
            rgm_hash_index_iter_frame* f = &stack[sp - 1];
            if (!f->visited)
            {
                f->visited = 1;
                uint64_t v = (uint64_t)rgm_atomic_load_i64(
                                (const rgm_atomic_i64*)&f->node->m_value);
                stop = _fn(f->node->m_h1, f->node->m_h2, v, _userData);
                count++;
                if (stop) break;
            }
            if (f->nextChild < 4)
            {
                uint64_t cur = (uint64_t)rgm_atomic_load_i64(
                                    (const rgm_atomic_i64*)&f->node->m_child[f->nextChild]);
                f->nextChild++;
                if (cur != 0)
                {
                    if (sp < RGM_HASH_INDEX_ITER_STACK_DEPTH)
                    {
                        stack[sp].node      = (HashIndexNode*)(uintptr_t)cur;
                        stack[sp].visited   = 0;
                        stack[sp].nextChild = 0;
                        sp++;
                    }
                    else
                    {
                        /* Caller-controlled hashes make deep chains easy; trap
                         * in debug for the signal, then recurse so the subtree
                         * is still fully visited (release must not silently
                         * drop it). */
                        RGM_FAIL("rgHashIndexForEach: iteration stack overflow; recursing");
                        count += rgm_hash_index_foreach_rec((HashIndexNode*)(uintptr_t)cur, _fn, _userData, &stop);
                        if (stop) break;
                    }
                }
            }
            else
            {
                sp--;
            }
        }
    }

    return count;
}
