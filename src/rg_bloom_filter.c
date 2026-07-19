/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Cache-line blocked Bloom filter.
 *
 * Bit array partitioned into power-of-two many 512-bit blocks (one cache
 * line each, 8 uint64 words). Every Add / Test:
 *   1. picks one block from h1 (h1 & m_blockMask) -- 1 cache-line load,
 *   2. derives all k bit positions inside that one block from h2 +
 *      Kirsch-Mitzenmacher double-hashing,
 *   3. ORs / loads the k bits within the line.
 *
 * Compared to the classic unblocked Bloom this trades ~30% memory for
 * the same false-positive rate against k-fold fewer cache-line loads
 * per query (1 line vs k lines). Net is a 3-7x latency win once the
 * filter doesn't fit in L1 / L2.
 *
 * Insertion is monotone (bits only ever flip 0 -> 1, never back), so
 * the structure is naturally lock-free:
 *   - Add publishes via release-ordered atomic OR; concurrent readers
 *     see the effect via acquire-ordered loads in Test.
 *   - Test uses acquire-ordered atomic loads; no CAS, no allocation.
 *   - Clear bulk-zeroes the array and is NOT concurrent-safe (no locks
 *     wrap it; the caller must arrange no other thread is touching the
 *     filter).
 *
 * Block-internal double hashing: bit positions are
 *   pos_i = (start + i * delta) & 0x1FF       (mod 512 within the block)
 * where `start` = low 9 bits of h2 and `delta` = high 9 bits of h2
 * OR'd with 1 (forcing odd so the step is coprime with 512 -- gives
 * a full permutation of bit positions for any k <= 512).
 *
 * h2 is *not* a second hash pass. wyhash's final 64x64 -> 128 multiply
 * already produces a 128-bit product whose halves the canonical output
 * XORs together; the byte-key and u64-key paths in this filter call the
 * `_128` variants (see rg_hash_func.h) which expose both halves and use
 * the upper one directly as h2. That saves one full wymum per query
 * compared to deriving h2 = wymum(h1, constant) -- "two hashes for the
 * price of one".
 */

#include "../include/rg_memory/rg_memory.h"
#include "rg_memory_platform.h"
#include "rg_hash_func.h"

#define RGM_BLOOM_OK              0
#define RGM_BLOOM_ERR_INVALID    -1
#define RGM_BLOOM_ERR_OVERFLOW   -2
#define RGM_BLOOM_ERR_NO_MEMORY  -3
#define RGM_BLOOM_ERR_TOO_SMALL  -4

/* h2 is no longer derived with a second wymum -- it falls out of the
 * same 64x64 -> 128 multiply that produced h1. Every supported target
 * computes both halves of that product anyway (umul128 on MSVC x64,
 * __int128 on GCC/Clang, schoolbook 32x32 partials elsewhere), so the
 * upper half is genuinely free. See rgm_hash_wymum128 in rg_hash_func.h. */

/* Block size: 512 bits = 8 uint64 words = 64 bytes = one cache line. */
#define RGM_BLOOM_BLOCK_BITS     512u
#define RGM_BLOOM_BLOCK_WORDS    8u       /* 512 / 64 */
#define RGM_BLOOM_BLOCK_BYTES    64u
#define RGM_BLOOM_BLOCK_BIT_MASK 0x1FFull /* 0..511 within a block */

/* True when the filter has been Created and holds a live bit array. */
static int rgm_bloom_is_live(const BloomFilter* _bf)
{
    return _bf != 0 && _bf->m_bits != 0;
}

/* Maximum block count this function will return. Constrained by the fact
 * that rgm_bloom_install / Clear / PopCount compute
 *
 *     uint32_t wordCount = blockCount * RGM_BLOOM_BLOCK_WORDS;
 *
 * to size the bit array. RGM_BLOOM_BLOCK_WORDS is 8 (== 2^3), so the
 * largest power of two whose product with 8 still fits in uint32_t is
 * 2^28 (gives wordCount = 2^31; one more doubling would wrap to 0 and
 * leave the bit array uninitialised). That's 2^28 * 512 bits = 16 GiB
 * of bit array -- larger than any realistic Bloom filter. */
#define RGM_BLOOM_MAX_BLOCKS (1u << 28)

/* Compute the block count from a requested bit budget. Rounds the bit
 * count up to whole 512-bit blocks, then up to the next power of two
 * (minimum 1 block). Returns 0 on overflow / zero input. */
static uint32_t rgm_bloom_round_blocks(uint64_t _bits)
{
    if (_bits == 0)
    {
        return 0;
    }
    /* A _bits within 511 of UINT64_MAX would wrap the round-up below and
     * yield a tiny block count instead of an overflow rejection. */
    if (_bits > UINT64_MAX - ((uint64_t)RGM_BLOOM_BLOCK_BITS - 1ull))
    {
        return 0;
    }
    /* Block count needed before pow-2 rounding. */
    uint64_t blocks = (_bits + (uint64_t)RGM_BLOOM_BLOCK_BITS - 1ull)
                    /  (uint64_t)RGM_BLOOM_BLOCK_BITS;
    /* Cap before the pow-2 round-up. Any `blocks` in (2^28, 2^29] would
     * round up to 2^29 -- which then overflows the uint32_t wordCount
     * computation in rgm_bloom_install (2^29 * 8 = 2^32 -> 0). */
    if (blocks > (uint64_t)RGM_BLOOM_MAX_BLOCKS)
    {
        return 0;
    }
    /* Round up to next power of 2. */
    if (blocks <= 1ull)
    {
        return 1u;
    }
    uint32_t b = (uint32_t)(blocks - 1ull);
    b |= b >> 1;
    b |= b >> 2;
    b |= b >> 4;
    b |= b >> 8;
    b |= b >> 16;
    /* b is 2^k - 1; the next power of two is b + 1. With the cap above,
     * b is at most 2^28 - 1 and b + 1 == 2^28 fits in uint32_t. */
    return b + 1u;
}

/* Wire up *_bf and zero the bit array. Caller has already validated. */
static void rgm_bloom_install(BloomFilter* _bf, uint64_t* _bits,
                              uint32_t _blockCount, uint32_t _hashCount)
{
    uint32_t wordCount = _blockCount * RGM_BLOOM_BLOCK_WORDS;
    uint32_t i;
    for (i = 0; i < wordCount; ++i)
    {
        _bits[i] = 0;
    }
    _bf->m_blockMask  = (uint64_t)(_blockCount - 1u);
    _bf->m_blockCount = _blockCount;
    _bf->m_hashCount  = _hashCount;
    _bf->m_bits       = _bits;
}

/* ------------------------------------------------------------------------- */

uint64_t rgBloomFilterBufferSize(uint64_t _bits)
{
    uint32_t blocks = rgm_bloom_round_blocks(_bits);
    if (blocks == 0)
    {
        return 0;
    }
    return (uint64_t)blocks * (uint64_t)RGM_BLOOM_BLOCK_BYTES;
}

int32_t rgBloomFilterCreate(Arena* _arena, BloomFilter* _bf,
                            uint64_t _bits, uint32_t _hashCount)
{
    if (_bf == 0 || _bits == 0 || _hashCount == 0
     || _hashCount > RGM_BLOOM_BLOCK_BYTES * 8u)   /* k > 512 cannot yield new positions in a 512-bit block: it only
                                                    * saturates blocks (FP rate -> 1) and makes Add/Test O(k) spins */
    {
        return RGM_BLOOM_ERR_INVALID;
    }
    uint32_t blocks = rgm_bloom_round_blocks(_bits);
    if (blocks == 0)
    {
        return RGM_BLOOM_ERR_OVERFLOW;
    }
    uint64_t bytes = (uint64_t)blocks * (uint64_t)RGM_BLOOM_BLOCK_BYTES;

    /* Cache-line align the bit array so the per-block guarantee
     * ("one line per Add / Test") holds for every block. */
    uint64_t* buf = (uint64_t*)rgArenaAllocAligned(_arena, bytes, RGM_CACHE_LINE);
    if (buf == 0)
    {
        return rgArenaIsValid(_arena) ? RGM_BLOOM_ERR_NO_MEMORY
                                      : RGM_BLOOM_ERR_INVALID;
    }
    rgm_bloom_install(_bf, buf, blocks, _hashCount);
    return RGM_BLOOM_OK;
}

int32_t rgBloomFilterCreateFromMemory(void* _buffer, uint64_t _bufferSize,
                                      BloomFilter* _bf,
                                      uint64_t _bits, uint32_t _hashCount)
{
    if (_buffer == 0 || _bf == 0 || _bits == 0 || _hashCount == 0
     || _hashCount > RGM_BLOOM_BLOCK_BYTES * 8u)   /* see rgBloomFilterCreate */
    {
        return RGM_BLOOM_ERR_INVALID;
    }
    uint32_t blocks = rgm_bloom_round_blocks(_bits);
    if (blocks == 0)
    {
        return RGM_BLOOM_ERR_OVERFLOW;
    }
    uint64_t bytes = (uint64_t)blocks * (uint64_t)RGM_BLOOM_BLOCK_BYTES;
    if (_bufferSize < bytes)
    {
        return RGM_BLOOM_ERR_TOO_SMALL;
    }
    /* Each block is a cache line; the caller's buffer needs cache-line
     * alignment so block N inherits the alignment for every N. */
    RGM_ASSERT(((uintptr_t)_buffer & (uintptr_t)(RGM_CACHE_LINE - 1u)) == 0
               && "rgBloomFilterCreateFromMemory: buffer must be cache-line aligned");
    rgm_bloom_install(_bf, (uint64_t*)_buffer, blocks, _hashCount);
    return RGM_BLOOM_OK;
}

void rgBloomFilterClear(BloomFilter* _bf)
{
    if (!rgm_bloom_is_live(_bf))
    {
        return;
    }
    uint32_t wordCount = _bf->m_blockCount * RGM_BLOOM_BLOCK_WORDS;
    uint32_t i;
    for (i = 0; i < wordCount; ++i)
    {
        _bf->m_bits[i] = 0;
    }
}

/* ------------------------------------------------------------------------- */

/* Add k bits derived from (h1, h2) inside one 512-bit block. h1 selects
 * the block; h2 seeds the in-block double-hashing. Release-ordered atomic
 * OR so concurrent Test readers see the bits via acquire load. */
static RGM_FORCEINLINE void rgm_bloom_add_h(BloomFilter* _bf, uint64_t _h1, uint64_t _h2)
{
    uint64_t  block_idx = _h1 & _bf->m_blockMask;
    uint64_t* block     = _bf->m_bits + (uint64_t)block_idx * RGM_BLOOM_BLOCK_WORDS;

    /* In-block double hashing: K bit positions = start + i * delta (mod 512).
     * `delta` is forced odd so it's coprime with 512 -- this guarantees the
     * K positions are distinct for any K <= 512 (no degenerate "all bits
     * land on the same position" case when h2 happens to have zeros). */
    uint32_t pos   = (uint32_t)(_h2          & RGM_BLOOM_BLOCK_BIT_MASK);
    uint32_t delta = (uint32_t)((_h2 >> 32)  & RGM_BLOOM_BLOCK_BIT_MASK) | 1u;

    uint32_t k = _bf->m_hashCount;
    uint32_t i;

    /* All k bits land in this one 512-bit block (8 words / one cache line).
     * Fold them into a per-word mask first, then issue at most 8 locked ORs
     * (one per touched word) instead of one locked RMW per bit -- a strict
     * reduction whenever two positions share a word, and a guaranteed
     * k -> <=8 reduction for k > 8. On 32-bit x86 each rgm_atomic_or_i64 is a
     * cmpxchg8b retry loop, so collapsing the count matters more there.
     * Monotone-OR semantics and per-word release ordering are preserved. */
    uint64_t wmask[RGM_BLOOM_BLOCK_WORDS];
    for (i = 0; i < RGM_BLOOM_BLOCK_WORDS; ++i)
    {
        wmask[i] = 0;
    }
    for (i = 0; i < k; ++i)
    {
        wmask[pos >> 6] |= 1ull << (pos & 63u);
        pos = (pos + delta) & (uint32_t)RGM_BLOOM_BLOCK_BIT_MASK;
    }
    for (i = 0; i < RGM_BLOOM_BLOCK_WORDS; ++i)
    {
        if (wmask[i] != 0)
        {
            rgm_atomic_or_i64((rgm_atomic_i64*)&block[i], (int64_t)wmask[i]);
        }
    }
}

/* Test k bits inside the chosen block. Returns 0 on the first missing bit. */
static RGM_FORCEINLINE int rgm_bloom_test_h(const BloomFilter* _bf, uint64_t _h1, uint64_t _h2)
{
    uint64_t  block_idx = _h1 & _bf->m_blockMask;
    uint64_t* block     = _bf->m_bits + (uint64_t)block_idx * RGM_BLOOM_BLOCK_WORDS;

    uint32_t pos   = (uint32_t)(_h2          & RGM_BLOOM_BLOCK_BIT_MASK);
    uint32_t delta = (uint32_t)((_h2 >> 32)  & RGM_BLOOM_BLOCK_BIT_MASK) | 1u;

    uint32_t k = _bf->m_hashCount;
    uint32_t i;
    for (i = 0; i < k; ++i)
    {
        uint32_t word   = pos >> 6;
        uint64_t m      = 1ull << (pos & 63u);
        uint64_t loaded = (uint64_t)rgm_atomic_load_i64(
                            (const rgm_atomic_i64*)&block[word]);
        if ((loaded & m) == 0)
        {
            return 0;
        }
        pos = (pos + delta) & (uint32_t)RGM_BLOOM_BLOCK_BIT_MASK;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */

void rgBloomFilterAddH(BloomFilter* _bf, uint64_t _h1, uint64_t _h2)
{
    if (!rgm_bloom_is_live(_bf))
    {
        return;
    }
    rgm_bloom_add_h(_bf, _h1, _h2);
}

void rgBloomFilterAddU64(BloomFilter* _bf, uint64_t _key)
{
    if (!rgm_bloom_is_live(_bf))
    {
        return;
    }
    uint64_t h1, h2;
    rgm_hash_u64_128(_key, &h1, &h2);
    rgm_bloom_add_h(_bf, h1, h2);
}

void rgBloomFilterAdd(BloomFilter* _bf, const void* _key, uint64_t _keyLen)
{
    if (!rgm_bloom_is_live(_bf) || (_key == 0 && _keyLen != 0))
    {
        return;
    }
    uint64_t h1, h2;
    rgm_hash_wyhash_128(_key, _keyLen, &h1, &h2);
    rgm_bloom_add_h(_bf, h1, h2);
}

int rgBloomFilterTestH(BloomFilter* _bf, uint64_t _h1, uint64_t _h2)
{
    if (!rgm_bloom_is_live(_bf))
    {
        return 0;
    }
    return rgm_bloom_test_h(_bf, _h1, _h2);
}

int rgBloomFilterTestU64(BloomFilter* _bf, uint64_t _key)
{
    if (!rgm_bloom_is_live(_bf))
    {
        return 0;
    }
    uint64_t h1, h2;
    rgm_hash_u64_128(_key, &h1, &h2);
    return rgm_bloom_test_h(_bf, h1, h2);
}

int rgBloomFilterTest(BloomFilter* _bf, const void* _key, uint64_t _keyLen)
{
    if (!rgm_bloom_is_live(_bf) || (_key == 0 && _keyLen != 0))
    {
        return 0;
    }
    uint64_t h1, h2;
    rgm_hash_wyhash_128(_key, _keyLen, &h1, &h2);
    return rgm_bloom_test_h(_bf, h1, h2);
}

/* ------------------------------------------------------------------------- */

uint64_t rgBloomFilterBitCount(BloomFilter* _bf)
{
    return rgm_bloom_is_live(_bf)
         ? (uint64_t)_bf->m_blockCount * (uint64_t)RGM_BLOOM_BLOCK_BITS
         : 0;
}

uint32_t rgBloomFilterHashCount(BloomFilter* _bf)
{
    return rgm_bloom_is_live(_bf) ? _bf->m_hashCount : 0;
}

/* Count of set bits across the whole array. Uses the compiler popcount
 * intrinsic on every target we support; falls back to a portable byte
 * loop on the off chance one isn't available. */
static RGM_FORCEINLINE uint32_t rgm_bloom_popcount64(uint64_t _v)
{
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_popcountll(_v);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    return (uint32_t)__popcnt64(_v);
#else
    /* MSVC x86 has no 64-bit popcount intrinsic (__popcnt64 is x64/ARM64
     * only), so fall through to the portable SWAR implementation below. */
    /* SWAR popcount. ~10 instructions vs single popcntq, but portable. */
    _v -= (_v >> 1) & 0x5555555555555555ull;
    _v  = (_v & 0x3333333333333333ull) + ((_v >> 2) & 0x3333333333333333ull);
    _v  = (_v + (_v >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (uint32_t)((_v * 0x0101010101010101ull) >> 56);
#endif
}

uint64_t rgBloomFilterPopCount(BloomFilter* _bf)
{
    if (!rgm_bloom_is_live(_bf))
    {
        return 0;
    }
    uint64_t set = 0;
    uint32_t wordCount = _bf->m_blockCount * RGM_BLOOM_BLOCK_WORDS;
    uint32_t i;
    for (i = 0; i < wordCount; ++i)
    {
        /* Plain load is sufficient for a stats query; tearing across
         * concurrent Adds gives an approximate count, which is the
         * point of PopCount anyway. */
        set += rgm_bloom_popcount64(_bf->m_bits[i]);
    }
    return set;
}
