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
        /* Roll the allocation back so repeated Puts on a >4 GiB-full arena
         * don't keep advancing the high-water mark (alignment padding is
         * not recovered; see rgArenaPop). */
        rgArenaPop(_arena, total);
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
        uint8_t* sentinel = (uint8_t*)rgArenaAllocAligned(_arena, RGM_CACHE_LINE, RGM_CACHE_LINE);
        if (sentinel == 0)
        {
            /* Without the sentinel the first node would land at offset 0 = the "empty slot"
             * marker: its publish would be invisible (unreachable entry, re-Put allocates
             * forever). Init MUST fail rather than hand back a silently-broken map. */
            return RGM_ERROR_ERR_NO_MEMORY;
        }
        {
            /* Zero it: rgArenaClear keeps committed pages, so an arena being
             * reused for a fresh map may still hold a persist header from a
             * previous session at offset 0. A stale header would make
             * rgHashMapSave reuse a bogus index offset and silently corrupt
             * the map. */
            uint32_t i;
            for (i = 0; i < RGM_CACHE_LINE; ++i)
            {
                sentinel[i] = 0;
            }
        }
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
#define RGM_HASHMAP_PERSIST_VERSION 2u /* 2: protected-mix hash (digests changed). */

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

    /* Reuse the index region across re-saves; allocate it once otherwise. The header bytes may come from
     * an UNTRUSTED mapped file (OpenShared + Init instead of Open), so the reused offset must be fully
     * bounds-checked like Open does - without the cap check a hostile m_indexOffset makes the index copy
     * below a 16 KiB out-of-bounds write. A failed check just falls through to a fresh allocation. */
    uint64_t cap = rgArenaCapacity(arena);
    uint32_t indexOffset;
    if (hdr->m_magic == RGM_HASHMAP_MAGIC
     && hdr->m_version == RGM_HASHMAP_PERSIST_VERSION
     && hdr->m_topBits == RGM_HASH_MAP_INDEX_TOPBITS
     && hdr->m_indexOffset >= RGM_CACHE_LINE
     && hdr->m_indexOffset <= UINT32_MAX
     && hdr->m_indexOffset <= cap
     && RGM_HASH_MAP_INDEX_BYTES <= cap - hdr->m_indexOffset)
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
            /* Roll the allocation back so a retried Save doesn't keep
             * burning arena space (alignment padding is not recovered;
             * see rgArenaPop). */
            rgArenaPop(arena, RGM_HASH_MAP_INDEX_BYTES);
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

/* Maximum trie depth rgHashMapOpen accepts. Natural depth is ~log4(N) plus a
 * few levels; anything near this cap needs repeated full-digest collisions
 * across independent reseeds, so a deeper graph is treated as corrupt. */
#define RGM_HASH_MAP_VALIDATE_MAX_DEPTH 256u

/* Check that the node graph under one index root is safe to walk: every node
 * lies (with its inline key) inside [RGM_CACHE_LINE, _highWater), is 8-byte
 * aligned, and sits at a strictly higher offset than its parent. Nodes are
 * bump-allocated after their parent, so a genuine map always satisfies the
 * ordering, and it rules out cycles. *_budget caps the total node visits at
 * the number of nodes that could fit below _highWater, so a hostile graph
 * that shares children (a DAG) cannot force exponential work: it runs out
 * of budget and is rejected. Returns 1 when valid, 0 otherwise. */
static int rgm_hash_map_validate_subtree(const uint8_t* _base, uint64_t _highWater,
                                         uint32_t _root, uint64_t* _budget)
{
    struct { uint32_t off; uint32_t next; } stack[RGM_HASH_MAP_VALIDATE_MAX_DEPTH];
    uint32_t sp = 0;
    uint32_t off = _root;
    uint32_t parent = 0;

    for (;;)
    {
        /* Validate `off` and push it. */
        if (off < RGM_CACHE_LINE
         || off <= parent
         || (off & 7u) != 0
         || (uint64_t)off + sizeof(HashMapNode) > _highWater
         || *_budget == 0
         || sp == RGM_HASH_MAP_VALIDATE_MAX_DEPTH)
        {
            return 0;
        }
        const HashMapNode* n = (const HashMapNode*)(_base + off);
        if ((uint64_t)n->m_keyLen > _highWater - off - sizeof(HashMapNode))
        {
            return 0;
        }
        --*_budget;
        stack[sp].off  = off;
        stack[sp].next = 0;
        ++sp;

        /* Find the next child to descend into, popping finished frames. */
        off = 0;
        while (sp > 0)
        {
            const HashMapNode* top = (const HashMapNode*)(_base + stack[sp - 1].off);
            while (stack[sp - 1].next < 4 && off == 0)
            {
                off = top->m_child[stack[sp - 1].next++];
            }
            if (off != 0)
            {
                parent = stack[sp - 1].off;
                break;
            }
            --sp;
        }
        if (sp == 0)
        {
            return 1;
        }
    }
}

int32_t rgHashMapOpen(HashMap* _map, Arena* _arena)
{
    if (_map == 0 || _arena == 0 || _arena->m_base == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    /* The header read below touches the first 32 bytes: on a reserve-only anonymous arena (nothing
     * committed yet) that dereference would FAULT before any validation ran. Reject uncommitted
     * arenas up front - a valid persisted map always has at least the sentinel line committed. */
    if (_arena->m_committed < sizeof(rgm_hash_map_persist_header))
    {
        return RGM_ERROR_ERR_FORMAT;
    }

    uint8_t* base = _arena->m_base;
    const rgm_hash_map_persist_header* hdr = (const rgm_hash_map_persist_header*)base;

    /* All header fields come from an untrusted file; the bounds checks are
     * phrased so a huge m_indexOffset cannot wrap the addition and slip
     * past. Save stores offsets as uint32_t, so anything above UINT32_MAX
     * is corrupt by definition. Consistency checks: the index region must sit
     * ABOVE the sentinel line (else it aliases this header) and the restored
     * high-water must cover it (else the next Put's node lands inside the
     * persisted header/index/nodes region - in-bounds but silently corrupting). */
    uint64_t cap = rgArenaCapacity(_arena);
    if (hdr->m_magic != RGM_HASHMAP_MAGIC
     || hdr->m_version != RGM_HASHMAP_PERSIST_VERSION
     || hdr->m_topBits != RGM_HASH_MAP_INDEX_TOPBITS
     || hdr->m_indexOffset < RGM_CACHE_LINE
     || hdr->m_indexOffset > UINT32_MAX
     || hdr->m_indexOffset > cap
     || RGM_HASH_MAP_INDEX_BYTES > cap - hdr->m_indexOffset
     || (hdr->m_indexOffset & 3u) != 0
     || hdr->m_highWater > cap
     || hdr->m_highWater > _arena->m_committed
     || hdr->m_highWater < hdr->m_indexOffset + RGM_HASH_MAP_INDEX_BYTES)
    {
        return RGM_ERROR_ERR_FORMAT;
    }

    /* The node graph is as untrusted as the header: every offset reachable
     * from the index must be validated before Get/Put/ForEach dereference it,
     * or a corrupt file turns into out-of-bounds reads and writes. Validate
     * from the file's copy of the index first, so a rejected file leaves
     * *_map untouched. */
    {
        const uint32_t* index  = (const uint32_t*)(base + hdr->m_indexOffset);
        uint64_t        nroots = RGM_HASH_MAP_INDEX_BYTES / sizeof(uint32_t);
        uint64_t        budget = hdr->m_highWater / sizeof(HashMapNode);
        uint64_t        r;
        for (r = 0; r < nroots; ++r)
        {
            if (index[r] != 0
             && !rgm_hash_map_validate_subtree(base, hdr->m_highWater, index[r], &budget))
            {
                return RGM_ERROR_ERR_FORMAT;
            }
        }
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
    /* _keyLen > UINT32_MAX is rejected because the node format stores key
     * length as uint32_t; such a key could never be stored or matched. */
    if (!rgm_map_is_live(_map) || (_key == 0 && _keyLen != 0) || _keyLen > UINT32_MAX)
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
    /* _keyLen > UINT32_MAX is rejected because the node format stores key
     * length as uint32_t; such a key could never be stored or matched. */
    if (!rgm_map_is_live(_map) || (_key == 0 && _keyLen != 0) || _keyLen > UINT32_MAX)
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
 * through here.
 *
 * Batched walks use asynchronous memory access chaining (AMAC): K lanes
 * each walk one key, and a lane that finishes (hit, miss) is refilled with
 * the next key immediately rather than waiting for the slowest lane of a
 * fixed group. Each lane prefetches its next node as soon as the offset is
 * known, so K independent cache misses are in flight while the loop visits
 * the other lanes. Measured vs the previous lockstep K=8 loop (no prefetch):
 * ~2.2x at 1M keys, ~1.6x at 10M (4 KiB pages); prefetch alone on the
 * lockstep loop only bought ~12%, because the group still stalled on its
 * deepest walk.
 * ------------------------------------------------------------------------- */

#define RGM_HASH_BATCH_K 32u

/* AMAC walk of _count uint64 keys. With _outValues != 0 this is the batched
 * Get (writes _outValues / _outFound, returns the hit count). With
 * _outValues == 0 it is a read-only warm-up pass for PutBatch: it walks each
 * key's path to its match or empty slot so the following serial Puts find
 * those lines in cache, and writes nothing. */
static uint64_t rgm_hash_map_batch_walk(HashMap* restrict _map,
                                        const uint64_t* restrict _keys,
                                        uint64_t* restrict _outValues,
                                        int* restrict _outFound,
                                        uint32_t _count)
{
    uint8_t* base = _map->m_arena->m_base;
    uint64_t hits = 0;

    uint64_t key [RGM_HASH_BATCH_K];
    uint64_t hful[RGM_HASH_BATCH_K];
    uint64_t h   [RGM_HASH_BATCH_K];
    uint32_t cur [RGM_HASH_BATCH_K];
    uint32_t idx [RGM_HASH_BATCH_K];   /* key index, UINT32_MAX = idle lane */
    uint32_t rnd [RGM_HASH_BATCH_K];
    uint32_t next = 0;
    uint32_t live = 0;
    uint32_t i;

    for (i = 0; i < RGM_HASH_BATCH_K; ++i)
    {
        idx[i] = UINT32_MAX;
        cur[i] = 0;
        if (next < _count)
        {
            uint32_t j = next++;
            idx[i]  = j;
            key[i]  = _keys[j];
            hful[i] = rgm_hash_u64(key[i]);
            h[i]    = RGM_HASH_MAP_DESCEND(hful[i]);
            cur[i]  = *RGM_HASH_MAP_ROOT(_map, hful[i]);
            rnd[i]  = 0;
            RGM_PREFETCH_READ(base + cur[i]);
            if (_outFound) _outFound[j] = 0;
            ++live;
        }
    }

    while (live != 0)
    {
        for (i = 0; i < RGM_HASH_BATCH_K; ++i)
        {
            if (idx[i] == UINT32_MAX) continue;

            uint32_t c = cur[i];
            if (c != 0)
            {
                const HashMapNode* n = RGM_HASH_MAP_NODE_AT(base, c);
                if (!(n->m_hash == hful[i]
                   && n->m_keyLen == 8u
                   && rgm_hash_load_u64(rgm_hash_map_node_key(n)) == key[i]))
                {
                    /* Descend one level and prefetch the child (base + 0
                     * for an empty slot is a harmless in-bounds hint). */
                    cur[i] = n->m_child[h[i] >> 62];
                    RGM_PREFETCH_READ(base + cur[i]);
                    h[i] <<= 2;
                    if (h[i] == 0) { h[i] = rgm_hash_reseed_u64(key[i], ++rnd[i]); }
                    continue;
                }
                if (_outValues)
                {
                    _outValues[idx[i]] = n->m_value;
                    if (_outFound) _outFound[idx[i]] = 1;
                }
                ++hits;
            }

            /* Lane finished (hit or empty slot): refill it. */
            if (next < _count)
            {
                uint32_t j = next++;
                idx[i]  = j;
                key[i]  = _keys[j];
                hful[i] = rgm_hash_u64(key[i]);
                h[i]    = RGM_HASH_MAP_DESCEND(hful[i]);
                cur[i]  = *RGM_HASH_MAP_ROOT(_map, hful[i]);
                rnd[i]  = 0;
                RGM_PREFETCH_READ(base + cur[i]);
                if (_outFound) _outFound[j] = 0;
            }
            else
            {
                idx[i] = UINT32_MAX;
                --live;
            }
        }
    }
    return hits;
}

/* Keys per PutBatch window: warm the window's paths with a parallel
 * read-only walk, then run the unchanged serial PutU64 loop over it, whose
 * dependent loads now hit cache. Measured ~1.4x at 1M inserts, ~2x at 10M. */
#define RGM_HASH_PUT_WINDOW 32u

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

    uint32_t done = 0;
    while (done < _count)
    {
        uint32_t n = (_count - done) < RGM_HASH_PUT_WINDOW
                   ? (_count - done) : RGM_HASH_PUT_WINDOW;
        (void)rgm_hash_map_batch_walk(_map, _keys + done, 0, 0, n);

        uint32_t i;
        for (i = done; i < done + n; ++i)
        {
            /* Args are validated above, so a 0 here means arena exhaustion. */
            uint64_t* slot = rgHashMapPutU64(_map, _keys[i]);
            if (slot == 0)
            {
                return RGM_ERROR_ERR_NO_MEMORY;
            }
            *slot = _values[i];
        }
        done += n;
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

    /* 64-bit accumulator: _count is u32, so > 2^31 hits in one call would overflow a plain int32
     * (UB, and a negative return colliding with the RGM_ERROR_* codes). Clamped here. */
    uint64_t hits = rgm_hash_map_batch_walk(_map, _keys, _outValues, _outFound, _count);
    return hits > (uint64_t)INT32_MAX ? INT32_MAX : (int32_t)hits;
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

/* Recursive fallback for the (engineered-collision-only) case where a chain
 * runs deeper than the inline explicit stack. Visits _node and its whole
 * subtree so entries are never silently dropped in release builds, where the
 * old RGM_FAIL was a no-op. Only ever entered past depth 64. */
static uint64_t rgm_hash_map_foreach_rec(uint8_t* _base, HashMapNode* _node,
                                         rgHashMapForEachFn _fn, void* _userData, int* _stop)
{
    uint64_t count = 1;
    int i;
    *_stop = _fn(rgm_hash_map_node_key(_node), _node->m_keyLen, _node->m_value, _userData);
    if (*_stop) return count;
    for (i = 0; i < 4; ++i)
    {
        uint32_t cur = _node->m_child[i];
        if (cur != 0)
        {
            count += rgm_hash_map_foreach_rec(_base, RGM_HASH_MAP_NODE_AT(_base, cur), _fn, _userData, _stop);
            if (*_stop) break;
        }
    }
    return count;
}

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
                /* Prefetch the children now: the DFS visits them next, and each
                 * step is otherwise one dependent cache miss. The callback runs
                 * while the lines arrive (~1.5-1.9x at 1M-10M nodes). Offset 0
                 * (empty) prefetches base, a harmless hint. */
                RGM_PREFETCH_READ(base + f->node->m_child[0]);
                RGM_PREFETCH_READ(base + f->node->m_child[1]);
                RGM_PREFETCH_READ(base + f->node->m_child[2]);
                RGM_PREFETCH_READ(base + f->node->m_child[3]);
                stop = _fn(rgm_hash_map_node_key(f->node), f->node->m_keyLen,
                           f->node->m_value, _userData);
                count++;
                if (stop) break;
            }
            if (f->nextChild < 4)
            {
                uint32_t cur = f->node->m_child[f->nextChild++];
                if (cur != 0)
                {
                    if (sp < RGM_HASH_ITER_STACK_DEPTH)
                    {
                        stack[sp].node      = RGM_HASH_MAP_NODE_AT(base, cur);
                        stack[sp].visited   = 0;
                        stack[sp].nextChild = 0;
                        sp++;
                        /* No software prefetch -- see Get path for the
                         * trade-off rationale. */
                    }
                    else
                    {
                        /* Depth > 64 requires engineered full-digest hash
                         * collisions. Trap in debug for the signal, then
                         * recurse so the subtree is still fully visited
                         * (release must not silently drop it). */
                        RGM_FAIL("rgHashMapForEach: iteration stack overflow; recursing");
                        count += rgm_hash_map_foreach_rec(base, RGM_HASH_MAP_NODE_AT(base, cur), _fn, _userData, &stop);
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
