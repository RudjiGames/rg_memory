/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Private header: shared hash functions and unaligned-load helpers used
 * by every hash-keyed container in this library (HashMap, HashTrie,
 * HashIndex).
 *
 * Split out from rg_hash_node.h so that consumers which only need the
 * hash (e.g. rg_hash_index.c) don't pull in HashNode-specific helpers
 * (key compare, key copy, node create) they would never call. Keeps the
 * -Wunused-function warning level clean.
 *
 * Not part of the public API. Never include from a header in include/.
 */

#ifndef RG_HASH_FUNC_H_HEADER_GUARD
#define RG_HASH_FUNC_H_HEADER_GUARD

#include "rg_memory_platform.h"

/* -------------------------------------------------------------------------
 * wyhash (Wang Yi, public domain) -- adapted for CRT-free C99.
 *
 * Reference: https://github.com/wangyi-fudan/wyhash
 *
 * Faster than FNV-1a on all key lengths and far better distributed in
 * the top bits (which the tries consume 2 at a time for descent). The
 * core operation is wymum: a 64x64->128 multiply that returns low XOR
 * high. On x86/x64 it folds into a single mulq + xorq.
 * ------------------------------------------------------------------------- */

/* Detect a usable 128-bit multiply path. __SIZEOF_INT128__ covers
 * GCC/Clang/ICC on 64-bit targets. _umul128 is the MSVC x64 intrinsic.
 * The software fallback works everywhere but is ~5x slower. */
#if defined(__SIZEOF_INT128__)
#   define RGM_HASH_MUL128_INT128 1
#else
#   define RGM_HASH_MUL128_INT128 0
#endif
#if !RGM_HASH_MUL128_INT128 && defined(_MSC_VER) && defined(_M_X64)
#   define RGM_HASH_MUL128_MSVC 1
#else
#   define RGM_HASH_MUL128_MSVC 0
#endif

/* Unaligned 8-byte / 4-byte reads. RGM_UNALIGNED (from rg_memory_platform.h)
 * expands to MSVC's __unaligned keyword where supported, and to nothing on
 * x86/clang. No CRT involvement on any branch:
 *
 *   - MSVC (cl.exe): __unaligned is a compiler keyword (Microsoft
 *     extension, not a libc symbol) that tells the compiler the access may
 *     be misaligned. On x64 it emits a single mov; on ARM64 it emits the
 *     safe unaligned-load instruction sequence. MSVC does NOT reliably fold
 *     the byte-OR ladder into a single load -- profiling on MSVC showed
 *     this was the dominant cost in wyhash's small-key path. The keyword is
 *     NOT supported on x86 (_M_IX86 -> C4235), where RGM_UNALIGNED is empty
 *     and the plain dereference is correct since x86 permits unaligned loads.
 *
 *   - Clang (incl. clang-cl) / GCC: the byte-OR ladder is recognised by the
 *     peephole pass and emitted as a single mov on x86/x64. The same code
 *     remains correct on strict-alignment targets, where the compiler keeps
 *     the byte loads. Clang's parser also rejects __unaligned in this
 *     position, so clang takes the ladder even when _MSC_VER is defined.
 */
static RGM_FORCEINLINE uint64_t rgm_hash_load_u64(const uint8_t* _p)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return *(const uint64_t RGM_UNALIGNED*)_p;
#else
    return (uint64_t)_p[0]
         | ((uint64_t)_p[1] <<  8)
         | ((uint64_t)_p[2] << 16)
         | ((uint64_t)_p[3] << 24)
         | ((uint64_t)_p[4] << 32)
         | ((uint64_t)_p[5] << 40)
         | ((uint64_t)_p[6] << 48)
         | ((uint64_t)_p[7] << 56);
#endif
}

static RGM_FORCEINLINE uint32_t rgm_hash_load_u32(const uint8_t* _p)
{
#if defined(_MSC_VER) && !defined(__clang__)
    return *(const uint32_t RGM_UNALIGNED*)_p;
#else
    return (uint32_t)_p[0]
         | ((uint32_t)_p[1] <<  8)
         | ((uint32_t)_p[2] << 16)
         | ((uint32_t)_p[3] << 24);
#endif
}

/* Pack 1, 2, or 3 bytes into the bottom of a uint64. */
static RGM_FORCEINLINE uint64_t rgm_hash_load_u3(const uint8_t* _p, uint64_t _k)
{
    return ((uint64_t)_p[0]         << 16)
         | ((uint64_t)_p[_k >> 1]   <<  8)
         |  (uint64_t)_p[_k - 1];
}

/* 64x64 -> 128 multiply, exposing both halves of the product. Every
 * supported target produces both halves anyway -- _umul128 on MSVC x64,
 * native __int128 on GCC/Clang, schoolbook 32x32 partials elsewhere -- so
 * returning both costs nothing relative to throwing away `hi`. Callers
 * that want the canonical wyhash digest XOR them together (`rgm_hash_wymum`);
 * callers that want a "free" second independent value (Bloom filter
 * double hashing) keep `hi` directly. */
static RGM_FORCEINLINE void rgm_hash_wymum128(uint64_t _a, uint64_t _b,
                                              uint64_t* _lo, uint64_t* _hi)
{
#if RGM_HASH_MUL128_INT128
    unsigned __int128 r = (unsigned __int128)_a * _b;
    *_lo = (uint64_t)r;
    *_hi = (uint64_t)(r >> 64);
#elif RGM_HASH_MUL128_MSVC
    uint64_t hi;
    *_lo = _umul128(_a, _b, &hi);
    *_hi = hi;
#else
    /* Schoolbook 128-bit multiply via 32-bit halves. */
    uint64_t al = (uint32_t)_a, ah = _a >> 32;
    uint64_t bl = (uint32_t)_b, bh = _b >> 32;
    uint64_t ll = al * bl;
    uint64_t lh = al * bh;
    uint64_t hl = ah * bl;
    uint64_t hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    *_lo = (mid << 32) | (uint32_t)ll;
    *_hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}

/* 64x64 -> 128 multiply, return low XOR high. Core of wyhash. Calls
 * wymum128 and folds the halves -- both functions are RGM_FORCEINLINE so
 * after SROA the locals vanish and the emitted code matches the original
 * single-expression wymum byte for byte. */
static RGM_FORCEINLINE uint64_t rgm_hash_wymum(uint64_t _a, uint64_t _b)
{
    uint64_t lo, hi;
    rgm_hash_wymum128(_a, _b, &lo, &hi);
    return lo ^ hi;
}

static RGM_FORCEINLINE uint64_t rgm_hash_wymix(uint64_t _a, uint64_t _b, uint64_t _s1, uint64_t _s2)
{
    return rgm_hash_wymum(_a ^ _s1, _b ^ _s2);
}

/* 128-bit analogue of rgm_hash_wymix. *_h1 is the canonical wyhash output
 * (lo ^ hi); *_h2 is the upper half of the same multiply. The pair is
 * statistically near-independent for non-adversarial input -- the
 * bijection (h1, h2) = (lo XOR hi, hi) over uniform (lo, hi) is itself
 * uniform over the 128-bit space. Used wherever the consumer needs two
 * hashes (Bloom filter, double hashing). */
static RGM_FORCEINLINE void rgm_hash_wymix128(uint64_t _a, uint64_t _b,
                                              uint64_t _s1, uint64_t _s2,
                                              uint64_t* _h1, uint64_t* _h2)
{
    uint64_t lo, hi;
    rgm_hash_wymum128(_a ^ _s1, _b ^ _s2, &lo, &hi);
    *_h1 = lo ^ hi;
    *_h2 = hi;
}

/* wyhash secret constants (v4 final). */
#define RGM_HASH_WYP0 0x2d358dccaa6c78a5ull
#define RGM_HASH_WYP1 0x8bb84b93962eacc9ull
#define RGM_HASH_WYP2 0x4b33a62ed433d4a3ull
#define RGM_HASH_WYP3 0x4d5a2da51de1aa47ull

/* wyhash 64-bit (seeded). The seed parameter becomes the initial state
 * of the mixing recurrence, so two calls with different seeds produce
 * statistically independent digests for the same input -- this is what
 * HashIndex relies on when deriving its two collision-safety hashes
 * from one byte key.
 *
 * Fast path at the top covers the 8-byte key case (uint64 keys, by far
 * the most common in the tries). The branch is a single compare and
 * predicts perfectly for any workload with a dominant key size. The
 * fast path produces the *same hash output* as the general path on
 * little-endian targets (every platform this library supports), so
 * mixed-size inputs are still wyhash-compliant.
 *
 * Marked `inline` (compiler's choice, not forced). MSVC measurably
 * regressed when this was RGM_FORCEINLINE: the full wyhash body
 * expanded at every Put/Get call site bloated the hot loop enough to
 * push it past L1 i-cache effective capacity. Leaving the decision to
 * the inliner gives the right call-vs-inline balance for both ST and
 * MT paths on the workloads measured. */
static inline uint64_t rgm_hash_wyhash_seeded(const void* _data, uint64_t _len, uint64_t _seed)
{
    const uint8_t* p    = (const uint8_t*)_data;
    uint64_t       seed = _seed;
    uint64_t       a, b;

    /* len == 8 fast path: one u64 load + a 32-bit rotation replaces the
     * general path's 4 u32 loads + 4 shifts + 4 ORs. Algebra:
     *   load_u64(p) on LE = [b7 b6 b5 b4 b3 b2 b1 b0]
     *                     = (load_u32(p+4) << 32) | load_u32(p)   = b_general
     *   (b << 32) | (b >> 32)
     *                     = (load_u32(p)   << 32) | load_u32(p+4) = a_general
     */
    if (_len == 8)
    {
        b = rgm_hash_load_u64(p);
        a = (b << 32) | (b >> 32);
        return rgm_hash_wymix(a, b ^ 8ull, RGM_HASH_WYP1, seed);
    }

    if (_len <= 16)
    {
        if (_len >= 4)
        {
            a = ((uint64_t)rgm_hash_load_u32(p) << 32)
              |  (uint64_t)rgm_hash_load_u32(p + ((_len >> 3) << 2));
            b = ((uint64_t)rgm_hash_load_u32(p + _len - 4) << 32)
              |  (uint64_t)rgm_hash_load_u32(p + _len - 4 - ((_len >> 3) << 2));
        }
        else if (_len > 0)
        {
            a = rgm_hash_load_u3(p, _len);
            b = 0;
        }
        else
        {
            a = 0;
            b = 0;
        }
    }
    else
    {
        uint64_t i = _len;
        if (i > 48)
        {
            uint64_t see1 = seed;
            uint64_t see2 = seed;
            do
            {
                seed = rgm_hash_wymix(rgm_hash_load_u64(p),      rgm_hash_load_u64(p +  8), RGM_HASH_WYP1, seed);
                see1 = rgm_hash_wymix(rgm_hash_load_u64(p + 16), rgm_hash_load_u64(p + 24), RGM_HASH_WYP2, see1);
                see2 = rgm_hash_wymix(rgm_hash_load_u64(p + 32), rgm_hash_load_u64(p + 40), RGM_HASH_WYP3, see2);
                p += 48;
                i -= 48;
            } while (i > 48);
            seed ^= see1 ^ see2;
        }
        while (i > 16)
        {
            seed = rgm_hash_wymix(rgm_hash_load_u64(p), rgm_hash_load_u64(p + 8), RGM_HASH_WYP1, seed);
            i -= 16;
            p += 16;
        }
        a = rgm_hash_load_u64(p + i - 16);
        b = rgm_hash_load_u64(p + i -  8);
    }

    return rgm_hash_wymix(a, b ^ (uint64_t)_len, RGM_HASH_WYP1, seed);
}

/* Default-seeded wyhash. Drop-in replacement for FNV-1a but faster on
 * every length and with much better avalanche. The initial state is
 * RGM_HASH_WYP0, matching the upstream wyhash v4 final convention. */
static inline uint64_t rgm_hash_wyhash(const void* _data, uint64_t _len)
{
    return rgm_hash_wyhash_seeded(_data, _len, RGM_HASH_WYP0);
}

/* Seeded inline wyhash for an 8-byte key already in a register. The seed
 * plays the same role as in rgm_hash_wyhash_seeded -- a different seed
 * yields a statistically independent digest of the same key. Produces the
 * exact same output as rgm_hash_wyhash_seeded(&_key, 8, _seed) on
 * little-endian targets, so the U64 and byte-key descent paths agree on
 * any reseed (see rgm_hash_reseed_u64). */
static RGM_FORCEINLINE uint64_t rgm_hash_u64_seeded(uint64_t _key, uint64_t _seed)
{
    uint64_t a = (_key << 32) | (_key >> 32);
    return rgm_hash_wymix(a, _key ^ 8ull, RGM_HASH_WYP1, _seed);
}

/* Inline wyhash for an 8-byte key already in a register. Same algebra
 * as the rgm_hash_wyhash len==8 fast path but skips the memory load and
 * subsequent length-dispatch branch. Used by every U64-specialised
 * Put/Get and by the bulk U64 paths. */
static RGM_FORCEINLINE uint64_t rgm_hash_u64(uint64_t _key)
{
    return rgm_hash_u64_seeded(_key, RGM_HASH_WYP0);
}

/* -------------------------------------------------------------------------
 * Descent reseed -- collision resilience for the 4-ary hash tries.
 *
 * Every trie (HashMap, HashTrie, HashIndex) consumes 2 hash bits per level
 * from a working "descent register" via (h >> 62), shifting h <<= 2 to
 * advance. After ~26 levels (with the top-level index, which pre-consumes
 * 12 bits) or ~32 (without) the register is shifted to zero and every
 * further step would select child[0]. Without intervention, a set of keys
 * that share a full 64-bit digest -- whether by chance (~2^-64 per pair) or
 * because an attacker engineered a wyhash collision (wyhash is fast and
 * well-distributed but NOT collision-resistant) -- piles into an unbounded
 * child[0] chain, degrading lookups from O(log4 N) to O(n).
 *
 * The fix (cf. Litwin trie-hashing / NetBSD thmap, which re-hash with an
 * incremented seed for deeper levels): when the descent register hits zero,
 * reseed it from a fresh, round-varying hash and keep descending. Two
 * properties make this correct and cheap:
 *
 *   1. Put and Get reseed at identical points -- both trigger on h == 0 and
 *      both recompute the reseed deterministically from the key (or the
 *      stored hash pair), so a key's Get descent always retraces its Put
 *      descent. The node match test still uses the ORIGINAL digest, so
 *      reseeding only ever changes which child a step takes, never which
 *      node counts as a match.
 *
 *   2. The reseed of two distinct keys differs even when their original
 *      digests collided (distinct key bytes -> distinct wyhash), so the
 *      reseed genuinely breaks the collision rather than merely re-balancing
 *      it. The round counter advances on each reseed so a chain long enough
 *      to exhaust a reseeded digest keeps drawing fresh entropy.
 *
 * Cost on the hot path is a single `if (h == 0)` per level. For any
 * realistic key set the trie is only ~log4(N) deep (≈10 levels at 1 M,
 * ≈12 at 10 M), so the branch is never taken, predicts perfectly, and hides
 * entirely under the dependent-load latency that already bounds the walk.
 * ------------------------------------------------------------------------- */

/* Round-varying reseed seed. WYP3 is odd, so WYP3 * _round is non-zero and
 * distinct for every _round in [1, 2^32), and the XOR keeps the result
 * different from WYP0 (the base digest seed) on every round >= 1. */
#define RGM_HASH_RESEED_SEED(_round) (RGM_HASH_WYP0 ^ (RGM_HASH_WYP3 * (uint64_t)(_round)))

/* Byte-keyed reseed (HashMap, HashTrie): re-hash the key with a fresh seed. */
static RGM_FORCEINLINE uint64_t rgm_hash_reseed_bytes(const void* _key, uint64_t _keyLen,
                                                      uint32_t _round)
{
    return rgm_hash_wyhash_seeded(_key, _keyLen, RGM_HASH_RESEED_SEED(_round));
}

/* uint64-keyed reseed; equals rgm_hash_reseed_bytes(&_key, 8, _round) on
 * little-endian targets, so the generic and U64 descent paths stay in sync
 * for an 8-byte key inserted via one API and looked up via the other. */
static RGM_FORCEINLINE uint64_t rgm_hash_reseed_u64(uint64_t _key, uint32_t _round)
{
    return rgm_hash_u64_seeded(_key, RGM_HASH_RESEED_SEED(_round));
}

/* Pre-hashed reseed (HashIndex, which stores no key bytes and so cannot
 * re-hash a key). Remix the two stored digests: descent is driven by h1
 * alone, so entries sharing h1 but differing in h2 -- distinct entries that
 * would otherwise chain -- diverge via h2's entropy here. Entries equal in
 * BOTH h1 and h2 are the same entry by HashIndex's definition, so there is
 * nothing to disambiguate. */
static RGM_FORCEINLINE uint64_t rgm_hash_reseed_h(uint64_t _h1, uint64_t _h2, uint32_t _round)
{
    return rgm_hash_wymix(_h1 ^ (RGM_HASH_WYP3 * (uint64_t)_round), _h2,
                          RGM_HASH_WYP1, RGM_HASH_WYP2);
}

/* ---------- 128-bit variants (two hashes for the price of one) -----------
 *
 * Same wyhash mixing chain, but the *final* wymum's two halves are exposed
 * instead of XORed together. *_h1 matches the canonical wyhash output
 * exactly (so a 64-bit consumer is unaffected by this layering); *_h2 is
 * the upper half of that same multiply -- statistically independent
 * enough of *_h1 for double-hashing schemes (Bloom filter, cuckoo, etc.).
 *
 * The savings: one *extra* wymum per call disappears. The classic pattern
 *
 *   h1 = wyhash(key);
 *   h2 = wymum(h1, GOLDEN);   <- this multiply is now free
 *
 * collapses into a single hash, because wymum already produced `hi` on
 * its way to producing the published `lo ^ hi`.
 */

/* 128-bit wyhash for an 8-byte key already in a register. h1 matches
 * rgm_hash_u64 byte for byte; h2 is the upper half of the same wymum. */
static RGM_FORCEINLINE void rgm_hash_u64_128(uint64_t _key,
                                             uint64_t* _h1, uint64_t* _h2)
{
    uint64_t a = (_key << 32) | (_key >> 32);
    rgm_hash_wymix128(a, _key ^ 8ull, RGM_HASH_WYP1, RGM_HASH_WYP0, _h1, _h2);
}

/* 128-bit seeded wyhash. Body is the byte-for-byte twin of rgm_hash_wyhash_seeded
 * up to (and including) the final mix, which is wymix128 instead of wymix.
 * Duplicated deliberately rather than wrapping wyhash_seeded: this function
 * has a single (Bloom filter) caller, so the marginal cost of duplicating
 * 30 lines once is worth not perturbing the inlining decisions the
 * upstream wyhash_seeded relies on for its many trie/index call sites. */
static inline void rgm_hash_wyhash_128_seeded(const void* _data, uint64_t _len, uint64_t _seed,
                                              uint64_t* _h1, uint64_t* _h2)
{
    const uint8_t* p    = (const uint8_t*)_data;
    uint64_t       seed = _seed;
    uint64_t       a, b;

    if (_len == 8)
    {
        b = rgm_hash_load_u64(p);
        a = (b << 32) | (b >> 32);
        rgm_hash_wymix128(a, b ^ 8ull, RGM_HASH_WYP1, seed, _h1, _h2);
        return;
    }

    if (_len <= 16)
    {
        if (_len >= 4)
        {
            a = ((uint64_t)rgm_hash_load_u32(p) << 32)
              |  (uint64_t)rgm_hash_load_u32(p + ((_len >> 3) << 2));
            b = ((uint64_t)rgm_hash_load_u32(p + _len - 4) << 32)
              |  (uint64_t)rgm_hash_load_u32(p + _len - 4 - ((_len >> 3) << 2));
        }
        else if (_len > 0)
        {
            a = rgm_hash_load_u3(p, _len);
            b = 0;
        }
        else
        {
            a = 0;
            b = 0;
        }
    }
    else
    {
        uint64_t i = _len;
        if (i > 48)
        {
            uint64_t see1 = seed;
            uint64_t see2 = seed;
            do
            {
                seed = rgm_hash_wymix(rgm_hash_load_u64(p),      rgm_hash_load_u64(p +  8), RGM_HASH_WYP1, seed);
                see1 = rgm_hash_wymix(rgm_hash_load_u64(p + 16), rgm_hash_load_u64(p + 24), RGM_HASH_WYP2, see1);
                see2 = rgm_hash_wymix(rgm_hash_load_u64(p + 32), rgm_hash_load_u64(p + 40), RGM_HASH_WYP3, see2);
                p += 48;
                i -= 48;
            } while (i > 48);
            seed ^= see1 ^ see2;
        }
        while (i > 16)
        {
            seed = rgm_hash_wymix(rgm_hash_load_u64(p), rgm_hash_load_u64(p + 8), RGM_HASH_WYP1, seed);
            i -= 16;
            p += 16;
        }
        a = rgm_hash_load_u64(p + i - 16);
        b = rgm_hash_load_u64(p + i -  8);
    }

    rgm_hash_wymix128(a, b ^ (uint64_t)_len, RGM_HASH_WYP1, seed, _h1, _h2);
}

/* Default-seeded 128-bit wyhash. h1 matches rgm_hash_wyhash byte for byte. */
static inline void rgm_hash_wyhash_128(const void* _data, uint64_t _len,
                                       uint64_t* _h1, uint64_t* _h2)
{
    rgm_hash_wyhash_128_seeded(_data, _len, RGM_HASH_WYP0, _h1, _h2);
}

#endif /* RG_HASH_FUNC_H_HEADER_GUARD */
