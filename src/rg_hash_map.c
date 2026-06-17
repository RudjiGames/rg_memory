/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Single-threaded hash map: a 4-ary hash trie backed by an arena.
 * Inter-node references are 32-bit offsets into the host arena, not
 * 64-bit absolute pointers -- HashMap is single-arena (rgHashMapInit
 * binds it to one Arena and keeps it), so a 4-byte slot is enough.
 * That halves the m_top array (32 KiB -> 16 KiB) and shrinks every
 * node's children from 32 B to 16 B, freeing 16 B of headroom inside
 * each cache-line-aligned node allocation.
 *
 * Design (Wellons, nullprogram 2023-09-30):
 *   - Each node holds 4 child slots and an inline key/value pair.
 *   - The walk consumes 2 hash bits per level via (h >> 62) and shifts
 *     h <<= 2 to advance. Average depth is log4(N). When the digest's bits
 *     are exhausted the descent register reseeds from a fresh hash of the
 *     key (see rgm_hash_reseed_bytes in rg_hash_func.h), so even a set of
 *     keys that share a full 64-bit digest stays ~log4-deep instead of
 *     degenerating into a linear child[0] chain.
 *   - The trie never deletes; the only mutations are slot publish and
 *     caller-driven writes through the returned value pointer.
 *
 * Put/Get return an absolute `uint64_t*` pointing at the matched node's
 * value field, or 0 on failure (bad args / not found / arena
 * exhausted). Only the inter-node child references stay as 32-bit offsets
 * to keep node size small; the value pointer handed back to the caller is
 * a plain absolute pointer valid until the host arena is cleared or
 * destroyed. Put is an upsert: a freshly inserted entry's value is
 * zero-initialised and the caller writes the real value through the
 * returned pointer; an existing entry's value is returned untouched.
 *
 * Offset encoding: every slot is `uint32_t offset` such that
 *   node_address == arena->m_base + offset.
 * Offset 0 reliably means "empty slot" because Init reserves the first
 * cache line of a fresh arena as a sentinel; the first HashMap node
 * therefore always lands at offset >= 64.
 *
 * The concurrent variant (HashTrie) keeps 64-bit absolute pointers
 * because each writer brings its own Arena -- a 32-bit offset has no
 * way to identify which arena a node lives in.
 *
 * A zero-initialised HashMap (m_arena == 0) is inert: every API
 * call on it is a safe no-op or returns the appropriate error code.
 */

#include "../include/rg_memory/rg_memory.h"
#include "rg_hash_node.h"  /* pulls in rg_memory_platform.h transitively */

/* HashMap-specific node layout. Same field layout as HashNode but with
 * 32-bit child offsets instead of 64-bit absolute pointers. 40 bytes
 * fixed part + inline key bytes. Cache-line-aligned at allocation so a
 * walk step reads exactly one line; the 16 B saved vs HashNode become
 * trailing pad inside the cache line. */
typedef struct HashMapNode
{
    uint32_t  m_child[4];   /* offsets into m_arena->m_base, 0 = 0. */
    uint64_t  m_hash;       /* full wyhash digest, for fast-reject in walks. */
    uint64_t  m_value;      /* opaque 64-bit value (cast pointers via uintptr).*/
    uint32_t  m_keyLen;     /* length of the key in bytes. */
    /* 4 bytes pad; key bytes follow at (node + 1). */
} HashMapNode;

/* Pointer to the inline key bytes for a HashMapNode. */
static RGM_FORCEINLINE const uint8_t* rgm_hash_map_node_key(const HashMapNode* _node)
{
    return (const uint8_t*)(_node + 1);
}

/* Allocate a HashMapNode + inline key bytes in one arena allocation,
 * fill in the fields, return the node pointer and write its arena
 * offset into *_outOffset. The value field is zero-initialised; the
 * caller (an upsert Put) writes the real value through the value pointer
 * it hands back.
 *
 * Aligned to 64 bytes (one cache line) so the 8-byte-key common case
 * fits inside one line; trie walks therefore read exactly one line
 * per descent. Returns 0 on arena exhaustion or 4 GiB overflow. */
static HashMapNode* rgm_hash_map_node_create(Arena* _arena, uint64_t _hash,
                                             const void* _key, uint64_t _keyLen,
                                             uint32_t* _outOffset)
{
    uint64_t total = sizeof(HashMapNode) + _keyLen;
    HashMapNode* node = (HashMapNode*)rgArenaAllocAligned(_arena, total, RGM_CACHE_LINE);
    if (node == 0)
    {
        return 0;
    }
    /* Compute the offset; bail if it would overflow uint32_t (arena
     * is bigger than 4 GiB). The Init-time sentinel allocation ensures
     * the offset is always >= 64, so 0 in *_outOffset cannot collide
     * with "empty slot". */
    uint64_t off = (uint64_t)((uint8_t*)node - _arena->m_base);
    if (off > UINT32_MAX)
    {
        return 0;
    }
    *_outOffset = (uint32_t)off;

    node->m_child[0] = 0;
    node->m_child[1] = 0;
    node->m_child[2] = 0;
    node->m_child[3] = 0;
    node->m_hash     = _hash;
    node->m_value    = 0;        /* caller writes the real value via the returned pointer. */
    node->m_keyLen   = (uint32_t)_keyLen;

    if (_keyLen != 0)
    {
        rgm_hash_copy_bytes((uint8_t*)(node + 1), _key, _keyLen);
    }
    return node;
}

/* Walk-entry helpers. With the top-level index enabled, consume the top
 * RG_HASH_TOP_BITS of the digest to pick a top slot and shift the working
 * register past them; without it, the entry slot is m_root and the working
 * register is the digest itself. Body code that uses these macros is
 * identical under both modes. */
#if RG_HASH_USE_TOP_INDEX
#  define RGM_HASH_MAP_DESCEND(_h)   ((_h) << RG_HASH_TOP_BITS)
#  define RGM_HASH_MAP_ROOT(_m, _h)  (&(_m)->m_top[(uint64_t)((_h) >> (64u - RG_HASH_TOP_BITS))])
#else
#  define RGM_HASH_MAP_DESCEND(_h)   (_h)
#  define RGM_HASH_MAP_ROOT(_m, _h)  (&(_m)->m_root)
#endif

/* Resolve an arena offset to a HashMapNode pointer. */
#define RGM_HASH_MAP_NODE_AT(_base, _off) ((HashMapNode*)((_base) + (_off)))

/* A HashMap is "live" once Init has bound it to a live arena. */
static RGM_FORCEINLINE int rgm_map_is_live(const HashMap* _map)
{
    return _map != 0
        && _map->m_arena != 0
        && _map->m_arena->m_base != 0;
}

/* Bind a HashMap to an arena and clear it. On a fresh arena (used == 0),
 * burn the first cache line as a sentinel so no real HashMap node ever
 * lives at arena offset 0 -- that's what lets every slot use 0 to mean
 * "empty". With the top-level index enabled, zero the 16 KiB top array
 * (compiler emits memset / rep stosq). */
int32_t rgHashMapInit(HashMap* _map, Arena* _arena)
{
    if (_map == 0 || _arena == 0 || _arena->m_base == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }
    _map->m_arena = _arena;

    if (rgArenaUsed(_arena) == 0)
    {
        /* Reserve the first cache line so the first node we ever
         * allocate lands at offset 64, not 0. The sentinel allocation
         * itself is never dereferenced; it just keeps offset 0 free
         * to serve as the "empty slot" marker. */
        (void)rgArenaAllocAligned(_arena, RGM_CACHE_LINE, RGM_CACHE_LINE);
    }

#if RG_HASH_USE_TOP_INDEX
    uint64_t i;
    for (i = 0; i < RG_HASH_TOP_SIZE; ++i)
    {
        _map->m_top[i] = 0;
    }
#else
    _map->m_root = 0;
#endif
    return RGM_ERROR_OK;
}

/* -------------------------------------------------------------------------
 * Persistence (rgHashMapSave / rgHashMapOpen).
 *
 * The node graph is already position-independent -- every inter-node link is
 * a 32-bit arena offset, and Get/Put recompute absolute pointers as
 * base + offset on each call, so they are valid at whatever address the
 * arena is mapped. The ONLY state outside the arena is the top-level index
 * (inline in the HashMap struct). Save mirrors it into the arena behind a
 * small header parked in the offset-0 sentinel cache line that Init reserves;
 * Open validates that header and copies the index back out. The hot Put/Get
 * paths are untouched -- they keep reading the inline index.
 * ------------------------------------------------------------------------- */

#define RGM_HASHMAP_MAGIC           0x52474d484d415031ull /* "RGMHMAP1" */
#define RGM_HASHMAP_PERSIST_VERSION 1u

/* Parked at arena offset 0 (inside Init's sentinel line, so it never aliases
 * a real node). 32 bytes -- comfortably within the 64-byte sentinel. */
typedef struct rgm_hash_map_persist_header
{
    uint64_t m_magic;       /* RGM_HASHMAP_MAGIC.                              */
    uint32_t m_version;     /* RGM_HASHMAP_PERSIST_VERSION.                    */
    uint32_t m_topBits;     /* index width this file was built with.          */
    uint64_t m_highWater;   /* arena m_pos at save time.                      */
    uint64_t m_indexOffset; /* arena offset of the persisted index copy.      */
} rgm_hash_map_persist_header;

/* The "index" is the inline top array (top-index on) or the single root slot
 * (top-index off). One pair of macros so Save/Open are mode-agnostic. */
#if RG_HASH_USE_TOP_INDEX
#  define RGM_HASH_MAP_INDEX_PTR(_m)   ((void*)(_m)->m_top)
#  define RGM_HASH_MAP_INDEX_BYTES     ((uint64_t)RG_HASH_TOP_SIZE * sizeof(uint32_t))
#  define RGM_HASH_MAP_INDEX_TOPBITS   RG_HASH_TOP_BITS
#else
#  define RGM_HASH_MAP_INDEX_PTR(_m)   ((void*)&(_m)->m_root)
#  define RGM_HASH_MAP_INDEX_BYTES     (sizeof(uint32_t))
#  define RGM_HASH_MAP_INDEX_TOPBITS   0u
#endif

int32_t rgHashMapSave(HashMap* _map)
{
    if (!rgm_map_is_live(_map))
    {
        return RGM_ERROR_ERR_INVALID;
    }

    Arena*   arena = _map->m_arena;
    uint8_t* base  = arena->m_base;

    /* The map must own the offset-0 sentinel (i.e. it was Init'd on a fresh
     * arena). If nothing has been allocated, there is no sentinel to park
     * the header in. */
    if (rgArenaUsed(arena) < RGM_CACHE_LINE)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    rgm_hash_map_persist_header* hdr = (rgm_hash_map_persist_header*)base;

    /* Reuse the index region across re-saves; allocate it once otherwise. */
    uint32_t indexOffset;
    if (hdr->m_magic == RGM_HASHMAP_MAGIC
     && hdr->m_version == RGM_HASHMAP_PERSIST_VERSION
     && hdr->m_topBits == RGM_HASH_MAP_INDEX_TOPBITS
     && hdr->m_indexOffset != 0)
    {
        indexOffset = (uint32_t)hdr->m_indexOffset;
    }
    else
    {
        void* idx = rgArenaAllocAligned(arena, RGM_HASH_MAP_INDEX_BYTES, 16);
        if (idx == 0)
        {
            return RGM_ERROR_ERR_NO_MEMORY;
        }
        uint64_t off = (uint64_t)((uint8_t*)idx - base);
        if (off > UINT32_MAX)
        {
            return RGM_ERROR_ERR_NO_MEMORY;
        }
        indexOffset = (uint32_t)off;
    }

    rgm_hash_copy_bytes(base + indexOffset, RGM_HASH_MAP_INDEX_PTR(_map),
                        RGM_HASH_MAP_INDEX_BYTES);

    /* High-water is captured AFTER any index allocation so reopening restores
     * a position past the index region. */
    hdr->m_magic       = RGM_HASHMAP_MAGIC;
    hdr->m_version     = RGM_HASHMAP_PERSIST_VERSION;
    hdr->m_topBits     = RGM_HASH_MAP_INDEX_TOPBITS;
    hdr->m_highWater   = rgArenaUsed(arena);
    hdr->m_indexOffset = indexOffset;
    return RGM_ERROR_OK;
}

int32_t rgHashMapOpen(HashMap* _map, Arena* _arena)
{
    if (_map == 0 || _arena == 0 || _arena->m_base == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint8_t* base = _arena->m_base;
    const rgm_hash_map_persist_header* hdr = (const rgm_hash_map_persist_header*)base;

    uint64_t cap = rgArenaCapacity(_arena);
    if (hdr->m_magic != RGM_HASHMAP_MAGIC
     || hdr->m_version != RGM_HASHMAP_PERSIST_VERSION
     || hdr->m_topBits != RGM_HASH_MAP_INDEX_TOPBITS
     || hdr->m_indexOffset == 0
     || hdr->m_indexOffset + RGM_HASH_MAP_INDEX_BYTES > cap
     || hdr->m_highWater > cap)
    {
        return RGM_ERROR_ERR_FORMAT;
    }

    _map->m_arena = _arena;
    rgm_hash_copy_bytes(RGM_HASH_MAP_INDEX_PTR(_map), base + hdr->m_indexOffset,
                        RGM_HASH_MAP_INDEX_BYTES);
    _arena->m_pos = hdr->m_highWater; /* allow appends past the persisted nodes. */
    return RGM_ERROR_OK;
}

/* Upsert. Walks the trie consuming 2 hash bits per level. On a slot
 * match, returns a pointer to the existing value (untouched). On a slot
 * miss (current == 0), allocates a new node, publishes its offset, and
 * returns a pointer to its zero-initialised value. Returns 0 on bad
 * arguments or arena exhaustion. The caller writes the value through the
 * returned pointer. */
uint64_t* rgHashMapPut(HashMap* restrict _map, const void* restrict _key,
                       uint64_t _keyLen)
{
    if (!rgm_map_is_live(_map) || (_key == 0 && _keyLen != 0))
    {
        return 0;
    }

    Arena*    arena  = _map->m_arena;
    uint8_t*  base   = arena->m_base;
    uint64_t  hfull  = rgm_hash_wyhash(_key, _keyLen);
    uint64_t  h      = RGM_HASH_MAP_DESCEND(hfull);
    uint32_t* slot   = RGM_HASH_MAP_ROOT(_map, hfull);
    uint32_t  round  = 0;

    for (;;)
    {
        uint32_t cur = *slot;
        if (cur == 0)
        {
            /* Empty slot -- materialise the node and publish its offset. */
            uint32_t offset;
            HashMapNode* node = rgm_hash_map_node_create(arena, hfull, _key, _keyLen,
                                                         &offset);
            if (node == 0)
            {
                return 0;
            }
            *slot = offset;
            return &node->m_value;
        }

        HashMapNode* n = RGM_HASH_MAP_NODE_AT(base, cur);
        /* Fast reject: full hash compare in one uint64 ==. Only fall
         * through to the byte loop on hash match (then verify keylen +
         * bytes; wyhash is not collision-resistant so the byte check
         * remains the source of truth). */
        if (n->m_hash == hfull
         && n->m_keyLen == (uint32_t)_keyLen
         && (_keyLen == 0 || rgm_hash_keys_equal(rgm_hash_map_node_key(n), _key, _keyLen)))
        {
            return &n->m_value; /* existing entry, value left untouched */
        }

        slot = &n->m_child[h >> 62];
        h  <<= 2;
        if (h == 0) { h = rgm_hash_reseed_bytes(_key, _keyLen, ++round); }
    }
}

/* Lookup. Same walk; returns a pointer to the value on hit, 0 on
 * miss (or bad arguments). */
uint64_t* rgHashMapGet(HashMap* restrict _map, const void* restrict _key,
                       uint64_t _keyLen)
{
    if (!rgm_map_is_live(_map) || (_key == 0 && _keyLen != 0))
    {
        return 0;
    }

    uint8_t* base  = _map->m_arena->m_base;
    uint64_t hfull = rgm_hash_wyhash(_key, _keyLen);
    uint64_t h     = RGM_HASH_MAP_DESCEND(hfull);
    uint32_t cur   = *RGM_HASH_MAP_ROOT(_map, hfull);
    uint32_t round = 0;

    while (cur != 0)
    {
        HashMapNode* n = RGM_HASH_MAP_NODE_AT(base, cur);
        if (n->m_hash == hfull
         && n->m_keyLen == (uint32_t)_keyLen
         && (_keyLen == 0 || rgm_hash_keys_equal(rgm_hash_map_node_key(n), _key, _keyLen)))
        {
            return &n->m_value;
        }
        cur = n->m_child[h >> 62];
        h <<= 2;
        if (h == 0) { h = rgm_hash_reseed_bytes(_key, _keyLen, ++round); }
    }
    return 0;
}

/* uint64-key specialised upsert. Inlined hash + single uint64 == key
 * match replaces the byte loop the generic path uses. Returns a pointer
 * to the value (zero-initialised on insert, existing value untouched on
 * match), or 0 on bad arguments / arena exhaustion. */
uint64_t* rgHashMapPutU64(HashMap* restrict _map, uint64_t _key)
{
    if (!rgm_map_is_live(_map))
    {
        return 0;
    }

    Arena*   arena = _map->m_arena;
    uint8_t* base  = arena->m_base;
    uint64_t hfull = rgm_hash_u64(_key);
    uint64_t h     = RGM_HASH_MAP_DESCEND(hfull);
    uint32_t* restrict slot = RGM_HASH_MAP_ROOT(_map, hfull);
    uint32_t  round = 0;

    for (;;)
    {
        uint32_t cur = *slot;
        if (cur == 0)
        {
            uint32_t offset;
            HashMapNode* node = rgm_hash_map_node_create(arena, hfull, &_key, sizeof(_key),
                                                         &offset);
            if (node == 0)
            {
                return 0;
            }
            *slot = offset;
            return &node->m_value;
        }

        HashMapNode* n = RGM_HASH_MAP_NODE_AT(base, cur);
        if (n->m_hash == hfull
         && n->m_keyLen == 8u
         && rgm_hash_load_u64(rgm_hash_map_node_key(n)) == _key)
        {
            return &n->m_value;
        }

        slot = &n->m_child[h >> 62];
        h  <<= 2;
        if (h == 0) { h = rgm_hash_reseed_u64(_key, ++round); }
    }
}

/* uint64-key specialised Get. Returns a pointer to the value on hit,
 * 0 on miss (or bad arguments). */
uint64_t* rgHashMapGetU64(HashMap* restrict _map, uint64_t _key)
{
    if (!rgm_map_is_live(_map))
    {
        return 0;
    }

    uint8_t* base  = _map->m_arena->m_base;
    uint64_t hfull = rgm_hash_u64(_key);
    uint64_t h     = RGM_HASH_MAP_DESCEND(hfull);
    uint32_t cur   = *RGM_HASH_MAP_ROOT(_map, hfull);
    uint32_t round = 0;

    while (cur != 0)
    {
        HashMapNode* n = RGM_HASH_MAP_NODE_AT(base, cur);
        if (n->m_hash == hfull
         && n->m_keyLen == 8u
         && rgm_hash_load_u64(rgm_hash_map_node_key(n)) == _key)
        {
            return &n->m_value;
        }
        cur = n->m_child[h >> 62];
        h <<= 2;
        if (h == 0) { h = rgm_hash_reseed_u64(_key, ++round); }
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Bulk uint64 API. Hot path -- the benchmark's batched Get pass routes
 * through here. Put is a simple loop (per-Put walks aren't independent
 * across keys, so pipelining buys nothing); Get is software-pipelined.
 *
 * Note: software prefetch hints were measured to *hurt* throughput on
 * this code path. With K=8 lanes already issuing independent loads per
 * step, the CPU's LSU is already saturated; adding prefetch instructions
 * just competed for load slots without adding parallelism.
 * ------------------------------------------------------------------------- */

#define RGM_HASH_BATCH_K 8u

int32_t rgHashMapPutBatchU64(HashMap* restrict _map,
                             const uint64_t* restrict _keys,
                             const uint64_t* restrict _values,
                             uint32_t _count)
{
    if (_map == 0 || _map->m_arena == 0 || _map->m_arena->m_base == 0
     || _keys == 0 || _values == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint32_t i;
    for (i = 0; i < _count; ++i)
    {
        /* Args are validated above, so a 0 here means arena exhaustion. */
        uint64_t* slot = rgHashMapPutU64(_map, _keys[i]);
        if (slot == 0)
        {
            return RGM_ERROR_ERR_NO_MEMORY;
        }
        *slot = _values[i];
    }
    return RGM_ERROR_OK;
}

int32_t rgHashMapGetBatchU64(HashMap* restrict _map,
                             const uint64_t* restrict _keys,
                             uint64_t* restrict _outValues,
                             int* restrict _outFound,
                             uint32_t _count)
{
    if (_map == 0 || _map->m_arena == 0 || _map->m_arena->m_base == 0
     || _keys == 0 || _outValues == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint8_t* base = _map->m_arena->m_base;
    int32_t  hits = 0;

    uint64_t        hfull[RGM_HASH_BATCH_K];
    uint64_t        h    [RGM_HASH_BATCH_K];
    const uint32_t* slot [RGM_HASH_BATCH_K];
    uint8_t         active[RGM_HASH_BATCH_K];
    uint32_t        round [RGM_HASH_BATCH_K];

    uint32_t batchBase;
    for (batchBase = 0; batchBase < _count; batchBase += RGM_HASH_BATCH_K)
    {
        uint32_t n = (_count - batchBase) < RGM_HASH_BATCH_K
                   ? (_count - batchBase) : RGM_HASH_BATCH_K;

        uint32_t i;
        for (i = 0; i < n; ++i)
        {
            hfull[i]  = rgm_hash_u64(_keys[batchBase + i]);
            h[i]      = RGM_HASH_MAP_DESCEND(hfull[i]);
            slot[i]   = RGM_HASH_MAP_ROOT(_map, hfull[i]);
            active[i] = 1;
            round[i]  = 0;
            if (_outFound) _outFound[batchBase + i] = 0;
        }

        int any = 1;
        while (any)
        {
            any = 0;
            for (i = 0; i < n; ++i)
            {
                if (!active[i]) continue;

                uint32_t cur = *slot[i];
                if (cur == 0)
                {
                    active[i] = 0;
                    continue;
                }

                HashMapNode* node = RGM_HASH_MAP_NODE_AT(base, cur);
                if (node->m_hash == hfull[i]
                 && node->m_keyLen == 8u
                 && rgm_hash_load_u64(rgm_hash_map_node_key(node)) == _keys[batchBase + i])
                {
                    _outValues[batchBase + i] = node->m_value;
                    if (_outFound) _outFound[batchBase + i] = 1;
                    active[i] = 0;
                    hits++;
                    continue;
                }

                slot[i] = &node->m_child[h[i] >> 62];
                h[i]  <<= 2;
                if (h[i] == 0) { h[i] = rgm_hash_reseed_u64(_keys[batchBase + i], ++round[i]); }
                any = 1;
            }
        }
    }

    return hits;
}

/* -------------------------------------------------------------------------
 * COLD: ForEach iteration. Placed at the bottom of the TU intentionally
 * so the linker positions it after the hot Put/Get/Batch code -- keeping
 * the hot region adjacent in the i-cache and not punctuated by iteration
 * code that runs at most once per use site (and often never).
 *
 * Pre-order trie traversal with an explicit stack to avoid unbounded
 * recursion on degenerate paths. Stack depth caps at 64.
 * ------------------------------------------------------------------------- */

typedef struct rgm_hash_iter_frame
{
    HashMapNode* node;
    uint32_t     visited;   /* 1 once the callback has fired for this node. */
    uint32_t     nextChild; /* 0..4; next child slot to descend into.       */
} rgm_hash_iter_frame;

#define RGM_HASH_ITER_STACK_DEPTH 64

uint64_t rgHashMapForEach(HashMap* _map, rgHashMapForEachFn _fn, void* _userData)
{
    if (_map == 0 || _map->m_arena == 0 || _map->m_arena->m_base == 0
     || _fn == 0)
    {
        return 0;
    }

    uint8_t* base = _map->m_arena->m_base;
    rgm_hash_iter_frame stack[RGM_HASH_ITER_STACK_DEPTH];
    uint64_t count = 0;
    int      stop  = 0;

#if RG_HASH_USE_TOP_INDEX
    /* Walk every populated top-level subtrie in index order. */
    uint64_t top;
    for (top = 0; top < RG_HASH_TOP_SIZE && !stop; ++top)
    {
        uint32_t rootCur = _map->m_top[top];
        if (rootCur == 0)
        {
            continue;
        }
#else
    {
        uint32_t rootCur = _map->m_root;
        if (rootCur == 0)
        {
            return 0;
        }
#endif

        uint32_t sp        = 0;
        stack[sp].node     = RGM_HASH_MAP_NODE_AT(base, rootCur);
        stack[sp].visited  = 0;
        stack[sp].nextChild = 0;
        sp++;

        while (sp > 0 && !stop)
        {
            rgm_hash_iter_frame* f = &stack[sp - 1];
            if (!f->visited)
            {
                f->visited = 1;
                stop = _fn(rgm_hash_map_node_key(f->node), f->node->m_keyLen,
                           f->node->m_value, _userData);
                count++;
                if (stop) break;
            }
            if (f->nextChild < 4)
            {
                uint32_t cur = f->node->m_child[f->nextChild++];
                if (cur != 0 && sp < RGM_HASH_ITER_STACK_DEPTH)
                {
                    stack[sp].node      = RGM_HASH_MAP_NODE_AT(base, cur);
                    stack[sp].visited   = 0;
                    stack[sp].nextChild = 0;
                    sp++;
                    /* No software prefetch -- see Get path for the
                     * trade-off rationale. */
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
