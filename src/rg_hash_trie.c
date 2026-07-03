/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Concurrent (lock-free) hash trie: same 4-ary node layout as HashMap,
 * but slot updates are strong CAS with release-on-success / acquire-on-
 * failure ordering. The algorithm follows Wellons' nullprogram piece
 * (2023-09-30) -- in particular the invariant that a published pointer
 * is never overwritten, which eliminates ABA without a tag/generation.
 *
 * Slots are 64-bit absolute HashNode pointers (stored as uint64_t so
 * they can be the operand of rgm_atomic_cas_i64). Multiple arenas can
 * therefore contribute nodes to the same trie -- each Put takes the
 * writer's arena explicitly, and the published node carries no arena
 * identity beyond its absolute address.
 *
 * Concurrency contract:
 *   - Multiple Get / multiple Get+Put: safe (atomic loads + CAS).
 *   - Multi-writer Put: give each writer thread its own arena. The
 *     arena's bump pointer is single-threaded, so writers must not
 *     share an arena -- but they can share the trie.
 *   - Lost CAS races leak the loser's allocated node + key bytes in
 *     the loser's arena (never reachable, just consumes high-water).
 *     Bounded by retry rate.
 *   - No deletion; values can be atomically overwritten by later Puts.
 *
 * The slot type punning -- treating uint64_t* as rgm_atomic_i64* -- is
 * legal because rgm_atomic_i64 is a same-width signed integer (volatile
 * long long on MSVC, int64_t on GCC/Clang) and C99 lets signed/unsigned
 * variants of the same type alias each other.
 */

#include "../include/rg_memory/rg_memory.h"
#include "rg_hash_node.h"  /* pulls in rg_memory_platform.h transitively */

/* Walk-entry helpers. With the top-level index enabled, consume the top
 * RG_HASH_TOP_BITS of the digest to pick a top slot and shift the working
 * register past them; without it, the entry slot is m_root and the working
 * register is the digest itself. Body code that uses these macros is
 * identical under both modes. */
#if RG_HASH_USE_TOP_INDEX
#  define RGM_HASH_TRIE_DESCEND(_h)   ((_h) << RG_HASH_TOP_BITS)
#  define RGM_HASH_TRIE_ROOT(_t, _h)  (&(_t)->m_top[(uint64_t)((_h) >> (64u - RG_HASH_TOP_BITS))])
#else
#  define RGM_HASH_TRIE_DESCEND(_h)   (_h)
#  define RGM_HASH_TRIE_ROOT(_t, _h)  (&(_t)->m_root)
#endif

/* Clear a HashTrie to the empty state and set creation flags. Zero-init is
 * already valid (flags 0 == copy mode); this exists for symmetry with
 * HashMap, to re-empty an in-use trie, and to opt into NOCOPY. */
int32_t rgHashTrieInitEx(HashTrie* _trie, uint32_t _flags)
{
    if (_trie == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }
#if RG_HASH_USE_TOP_INDEX
    /* Zero the 32 KiB top-level index. The compiler emits this as memset /
     * rep stosq; it dwarfs the rest of Init but is a one-time cost. */
    uint64_t i;
    for (i = 0; i < RG_HASH_TOP_SIZE; ++i)
    {
        _trie->m_top[i] = 0;
    }
#else
    _trie->m_root = 0;
#endif
    _trie->m_flags = _flags;
    return RGM_ERROR_OK;
}

/* Clear a HashTrie to the empty (copy-mode) state. */
int32_t rgHashTrieInit(HashTrie* _trie)
{
    return rgHashTrieInitEx(_trie, RGM_HASHTRIE_FLAG_NONE);
}

/* Insert or overwrite. Walks the trie consuming 2 hash bits per level.
 * On an empty slot, allocate a new node and CAS-publish it (release on
 * success synchronises with concurrent Get's acquire-load). On a key
 * match, atomic-store the new value. */
int32_t rgHashTriePut(HashTrie* restrict _trie, Arena* restrict _arena,
                      const void* restrict _key, uint64_t _keyLen, uint64_t _value)
{
    /* _keyLen > UINT32_MAX is rejected because the node format stores key
     * length as uint32_t; storing such a key would truncate the length
     * ForEach reports. */
    if (_trie == 0 || _arena == 0 || _arena->m_base == 0
     || (_key == 0 && _keyLen != 0) || _keyLen > UINT32_MAX)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t        hfull  = rgm_hash_wyhash(_key, _keyLen);
    uint64_t        h      = RGM_HASH_TRIE_DESCEND(hfull);
    rgm_atomic_i64* slot   = (rgm_atomic_i64*)RGM_HASH_TRIE_ROOT(_trie, hfull);
    uint64_t        cur    = (uint64_t)rgm_atomic_load_i64(slot);
    uint32_t        round  = 0;
    const int       nocopy = (_trie->m_flags & RGM_HASHTRIE_FLAG_NOCOPY) != 0;

    for (;;)
    {
        if (cur == 0)
        {
            /* Empty slot -- speculatively allocate a node and CAS it in.
             * If we lose the race the node + key bytes stay in our arena
             * but become unreachable; the algorithm tolerates this. */
            HashNode* node = nocopy
                ? rgm_hash_node_create_nocopy(_arena, hfull, _key, _keyLen, _value)
                : rgm_hash_node_create(_arena, hfull, _key, _keyLen, _value);
            if (node == 0)
            {
                return RGM_ERROR_ERR_NO_MEMORY;
            }
            uint64_t desired = (uint64_t)(uintptr_t)node;

            /* CAS-with-prior: on success returns 0 (our expected); on
             * failure returns the winner's published pointer. Folding the
             * retry through the returned value skips the follow-up
             * acquire-load that a bool-CAS + `continue` would issue. The
             * trie's no-overwrite invariant means another load would have
             * returned the same prior value anyway. */
            int64_t prior = rgm_atomic_cas_val_i64(slot, 0, (int64_t)desired);
            if (prior == 0)
            {
                return RGM_ERROR_OK;
            }
            cur = (uint64_t)prior;
            /* Fall through to the match-or-descend path with `cur`
             * already populated (no re-load needed). */
        }

        /* Fast reject: full hash compare in one uint64 ==. Only fall
         * through to the byte loop on hash match (then verify keylen +
         * bytes; wyhash is not collision-resistant so the byte check
         * remains the source of truth). */
        HashNode* n = (HashNode*)(uintptr_t)cur;
        if (n->m_hash == hfull
         && n->m_keyLen == (uint32_t)_keyLen
         && (_keyLen == 0
          || rgm_hash_keys_equal(rgm_hash_node_key_ext(n, nocopy), _key, _keyLen)))
        {
            /* Key matched -- atomic-store the new value. Concurrent
             * readers see either the old or new value, never a torn
             * read (m_value is 64-bit aligned). */
            rgm_atomic_store_i64((rgm_atomic_i64*)&n->m_value, (int64_t)_value);
            return RGM_ERROR_OK;
        }

        slot = (rgm_atomic_i64*)&n->m_child[h >> 62];
        h  <<= 2;
        if (h == 0) { h = rgm_hash_reseed_bytes(_key, _keyLen, ++round); }
        cur = (uint64_t)rgm_atomic_load_i64(slot);
    }
}

/* Lookup. Atomic-acquire-load each slot; the matching CAS on the writer
 * side carries the new node's writes into our view. */
int32_t rgHashTrieGet(HashTrie* restrict _trie, const void* restrict _key,
                      uint64_t _keyLen, uint64_t* restrict _outValue)
{
    /* Keys longer than UINT32_MAX can never be stored (see rgHashTriePut),
     * so reject them here too instead of walking with a truncated compare. */
    if (_trie == 0 || (_key == 0 && _keyLen != 0) || _keyLen > UINT32_MAX)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t              hfull  = rgm_hash_wyhash(_key, _keyLen);
    uint64_t              h      = RGM_HASH_TRIE_DESCEND(hfull);
    const rgm_atomic_i64* slot   = (const rgm_atomic_i64*)RGM_HASH_TRIE_ROOT(_trie, hfull);
    uint32_t              round  = 0;
    const int             nocopy = (_trie->m_flags & RGM_HASHTRIE_FLAG_NOCOPY) != 0;

    for (;;)
    {
        uint64_t cur = (uint64_t)rgm_atomic_load_i64(slot);
        if (cur == 0)
        {
            return RGM_ERROR_ERR_INVALID; /* not found */
        }

        HashNode* n = (HashNode*)(uintptr_t)cur;
        if (n->m_hash == hfull
         && n->m_keyLen == (uint32_t)_keyLen
         && (_keyLen == 0
          || rgm_hash_keys_equal(rgm_hash_node_key_ext(n, nocopy), _key, _keyLen)))
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
        if (h == 0) { h = rgm_hash_reseed_bytes(_key, _keyLen, ++round); }
    }
}

/* uint64-key specialised Put. Same atomicity contract as rgHashTriePut,
 * with the hash and key compare both specialised to one instruction each. */
int32_t rgHashTriePutU64(HashTrie* restrict _trie, Arena* restrict _arena,
                         uint64_t _key, uint64_t _value)
{
    if (_trie == 0 || _arena == 0 || _arena->m_base == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t        hfull = rgm_hash_u64(_key);
    uint64_t        h     = RGM_HASH_TRIE_DESCEND(hfull);
    rgm_atomic_i64* slot  = (rgm_atomic_i64*)RGM_HASH_TRIE_ROOT(_trie, hfull);
    uint64_t        cur   = (uint64_t)rgm_atomic_load_i64(slot);
    uint32_t        round = 0;

    for (;;)
    {
        if (cur == 0)
        {
            HashNode* node = rgm_hash_node_create(_arena, hfull, &_key, sizeof(_key), _value);
            if (node == 0)
            {
                return RGM_ERROR_ERR_NO_MEMORY;
            }
            uint64_t desired = (uint64_t)(uintptr_t)node;

            /* CAS-with-prior: see rgHashTriePut for the rationale. */
            int64_t prior = rgm_atomic_cas_val_i64(slot, 0, (int64_t)desired);
            if (prior == 0)
            {
                return RGM_ERROR_OK;
            }
            cur = (uint64_t)prior;
        }

        HashNode* n = (HashNode*)(uintptr_t)cur;
        if (n->m_hash == hfull
         && n->m_keyLen == 8u
         && rgm_hash_load_u64(rgm_hash_node_key(n)) == _key)
        {
            rgm_atomic_store_i64((rgm_atomic_i64*)&n->m_value, (int64_t)_value);
            return RGM_ERROR_OK;
        }

        slot = (rgm_atomic_i64*)&n->m_child[h >> 62];
        h  <<= 2;
        if (h == 0) { h = rgm_hash_reseed_u64(_key, ++round); }
        cur = (uint64_t)rgm_atomic_load_i64(slot);
    }
}

/* uint64-key specialised Get. */
int32_t rgHashTrieGetU64(HashTrie* restrict _trie, uint64_t _key, uint64_t* restrict _outValue)
{
    if (_trie == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t              hfull = rgm_hash_u64(_key);
    uint64_t              h     = RGM_HASH_TRIE_DESCEND(hfull);
    const rgm_atomic_i64* slot  = (const rgm_atomic_i64*)RGM_HASH_TRIE_ROOT(_trie, hfull);
    uint32_t              round = 0;

    for (;;)
    {
        uint64_t cur = (uint64_t)rgm_atomic_load_i64(slot);
        if (cur == 0)
        {
            return RGM_ERROR_ERR_INVALID;
        }

        HashNode* n = (HashNode*)(uintptr_t)cur;
        if (n->m_hash == hfull
         && n->m_keyLen == 8u
         && rgm_hash_load_u64(rgm_hash_node_key(n)) == _key)
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
        if (h == 0) { h = rgm_hash_reseed_u64(_key, ++round); }
    }
}

/* -------------------------------------------------------------------------
 * Bulk uint64 API. Hot path -- both the single-thread "Get (rand, batched)"
 * and the MT "Get (aggregate, batched)" passes route through GetBatchU64.
 * Put is a simple loop (CAS retry semantics + per-Put walks aren't
 * independent across keys, so pipelining doesn't help); Get is software-
 * pipelined with K=8 parallel walks per batch.
 *
 * Note: software prefetch hints were measured to *hurt* throughput on
 * this code path -- same finding as HashIndex / HashMap. The K-fold
 * load parallelism is what wins; extra prefetches just compete for
 * LSU slots.
 * ------------------------------------------------------------------------- */

#define RGM_HASH_TRIE_BATCH_K 8u

int32_t rgHashTriePutBatchU64(HashTrie* restrict _trie, Arena* restrict _arena,
                              const uint64_t* restrict _keys,
                              const uint64_t* restrict _values,
                              uint32_t _count)
{
    if (_trie == 0 || _arena == 0 || _arena->m_base == 0
     || _keys == 0 || _values == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint32_t i;
    for (i = 0; i < _count; ++i)
    {
        int32_t rc = rgHashTriePutU64(_trie, _arena, _keys[i], _values[i]);
        if (rc != RGM_ERROR_OK)
        {
            return rc;
        }
    }
    return RGM_ERROR_OK;
}

int32_t rgHashTrieGetBatchU64(HashTrie* restrict _trie,
                              const uint64_t* restrict _keys,
                              uint64_t* restrict _outValues,
                              int* restrict _outFound,
                              uint32_t _count)
{
    if (_trie == 0 || _keys == 0 || _outValues == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    int32_t hits = 0;

    uint64_t              hfull[RGM_HASH_TRIE_BATCH_K];
    uint64_t              h    [RGM_HASH_TRIE_BATCH_K];
    const rgm_atomic_i64* slot [RGM_HASH_TRIE_BATCH_K];
    uint8_t               active[RGM_HASH_TRIE_BATCH_K];
    uint32_t              round [RGM_HASH_TRIE_BATCH_K];

    uint32_t base;
    for (base = 0; base < _count; base += RGM_HASH_TRIE_BATCH_K)
    {
        uint32_t n = (_count - base) < RGM_HASH_TRIE_BATCH_K
                   ? (_count - base) : RGM_HASH_TRIE_BATCH_K;

        uint32_t i;
        for (i = 0; i < n; ++i)
        {
            hfull[i]  = rgm_hash_u64(_keys[base + i]);
            h[i]      = RGM_HASH_TRIE_DESCEND(hfull[i]);
            slot[i]   = (const rgm_atomic_i64*)RGM_HASH_TRIE_ROOT(_trie, hfull[i]);
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

                HashNode* node = (HashNode*)(uintptr_t)cur;
                if (node->m_hash == hfull[i]
                 && node->m_keyLen == 8u
                 && rgm_hash_load_u64(rgm_hash_node_key(node)) == _keys[base + i])
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
                if (h[i] == 0) { h[i] = rgm_hash_reseed_u64(_keys[base + i], ++round[i]); }
                any = 1;
            }
        }
    }

    return hits;
}

/* -------------------------------------------------------------------------
 * COLD: ForEach iteration. Placed at the bottom of the TU intentionally
 * so the linker positions it after the hot Put/Get/PutU64/GetU64/Batch
 * code -- keeping the hot region adjacent in the i-cache and not
 * punctuated by iteration code that runs at most once per use site.
 *
 * Same iterative DFS pattern as HashMap's ForEach but reads slots via
 * atomic acquire-loads so it can run concurrently with writers. New
 * nodes published after a slot was visited may be missed; value updates
 * are old-or-new (atomic).
 * ------------------------------------------------------------------------- */

typedef struct rgm_hash_trie_iter_frame
{
    HashNode* node;
    uint32_t  visited;
    uint32_t  nextChild;
} rgm_hash_trie_iter_frame;

#define RGM_HASH_TRIE_ITER_STACK_DEPTH 64

/* Recursive fallback for chains deeper than the inline explicit stack (only
 * reachable via engineered full-digest collisions). Visits _node and its whole
 * subtree via the same atomic acquire-loads so release builds never silently
 * drop the subtree. Only ever entered past depth 64. */
static uint64_t rgm_hash_trie_foreach_rec(HashNode* _node, int _nocopy,
                                          rgHashTrieForEachFn _fn, void* _userData, int* _stop)
{
    uint64_t count = 1;
    int i;
    uint64_t v = (uint64_t)rgm_atomic_load_i64((const rgm_atomic_i64*)&_node->m_value);
    *_stop = _fn(rgm_hash_node_key_ext(_node, _nocopy), _node->m_keyLen, v, _userData);
    if (*_stop) return count;
    for (i = 0; i < 4; ++i)
    {
        uint64_t cur = (uint64_t)rgm_atomic_load_i64((const rgm_atomic_i64*)&_node->m_child[i]);
        if (cur != 0)
        {
            count += rgm_hash_trie_foreach_rec((HashNode*)(uintptr_t)cur, _nocopy, _fn, _userData, _stop);
            if (*_stop) break;
        }
    }
    return count;
}

uint64_t rgHashTrieForEach(HashTrie* _trie, rgHashTrieForEachFn _fn, void* _userData)
{
    if (_trie == 0 || _fn == 0)
    {
        return 0;
    }

    rgm_hash_trie_iter_frame stack[RGM_HASH_TRIE_ITER_STACK_DEPTH];
    uint64_t  count  = 0;
    int       stop   = 0;
    const int nocopy = (_trie->m_flags & RGM_HASHTRIE_FLAG_NOCOPY) != 0;

#if RG_HASH_USE_TOP_INDEX
    /* Walk every populated top-level subtrie in index order. Each
     * non-empty slot is the root of an independent 4-ary trie. */
    uint64_t top;
    for (top = 0; top < RG_HASH_TOP_SIZE && !stop; ++top)
    {
        uint64_t rootCur = (uint64_t)rgm_atomic_load_i64(
                                (const rgm_atomic_i64*)&_trie->m_top[top]);
        if (rootCur == 0)
        {
            continue;
        }
#else
    {
        uint64_t rootCur = (uint64_t)rgm_atomic_load_i64(
                                (const rgm_atomic_i64*)&_trie->m_root);
        if (rootCur == 0)
        {
            return 0;
        }
#endif

        uint32_t sp        = 0;
        stack[sp].node     = (HashNode*)(uintptr_t)rootCur;
        stack[sp].visited  = 0;
        stack[sp].nextChild = 0;
        sp++;

        while (sp > 0 && !stop)
        {
            rgm_hash_trie_iter_frame* f = &stack[sp - 1];
            if (!f->visited)
            {
                f->visited = 1;
                uint64_t v = (uint64_t)rgm_atomic_load_i64(
                                (const rgm_atomic_i64*)&f->node->m_value);
                stop = _fn(rgm_hash_node_key_ext(f->node, nocopy), f->node->m_keyLen, v, _userData);
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
                    if (sp < RGM_HASH_TRIE_ITER_STACK_DEPTH)
                    {
                        stack[sp].node      = (HashNode*)(uintptr_t)cur;
                        stack[sp].visited   = 0;
                        stack[sp].nextChild = 0;
                        sp++;
                        /* No software prefetch here -- same trade-off as
                         * HashMap's ForEach (see rg_hash_map.c). */
                    }
                    else
                    {
                        /* Depth > 64 requires engineered full-digest hash
                         * collisions. Trap in debug for the signal, then
                         * recurse so the subtree is still fully visited
                         * (release must not silently drop it). */
                        RGM_FAIL("rgHashTrieForEach: iteration stack overflow; recursing");
                        count += rgm_hash_trie_foreach_rec((HashNode*)(uintptr_t)cur, nocopy, _fn, _userData, &stop);
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
