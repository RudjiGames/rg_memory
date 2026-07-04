/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Free list allocator, arena embedded.
 *
 * Design:
 *   - Callers own the FreeList struct (see rg_memory.h). Create populates
 *     it in place; there is no internal pool, no handles, and no matching
 *     Destroy because the library owns no resources tied to the struct.
 *     A zero-initialised FreeList is treated as uninitialised: every
 *     operation is a safe no-op until Create has been called on it.
 *   - The block buffer is allocated from a caller-supplied arena. Its
 *     lifetime is tied to that arena -- rgArenaClear / rgArenaDestroy
 *     reclaim it. The FreeList struct itself lives wherever the caller
 *     puts it and outlives nothing in particular.
 *   - The free chain is stored inside the free blocks themselves: each
 *     free block holds the index of the next free block (or m_maxBlocks
 *     as the end-of-chain sentinel). Block size is clamped up to
 *     sizeof(uint32_t) and rounded up to 16 so block N starts on a
 *     16-byte boundary (matching the arena's default alignment).
 *   - Create eagerly links every block into the chain. A single linear
 *     pass over the buffer at create time lets Alloc skip any lazy-init
 *     branch on the hot path.
 *
 * Single-threaded by convention. Callers needing cross-thread sharing
 * must serialise their own access; concurrent operations on the same
 * FreeList have undefined results.
 */

#include "../include/rg_memory/rg_memory.h"
#include "rg_memory_platform.h"

/* Error codes (RGM_ERROR_*) are declared in rg_memory.h and shared with DenseList. */

/* A FreeList with m_buffer == 0 is treated as uninitialised: queries
 * report zero, Alloc returns 0, Free / CheckPtr do nothing. */
static int rgm_freelist_is_live(const FreeList* _fl)
{
    return _fl != 0 && _fl->m_buffer != 0;
}

/* Apply the clamp + 16-byte round-up that both Create variants share, and
 * also enforce that the result fits in a uint32_t (since m_blockSize is one).
 * Returns 0 on overflow / bad input. */
static uint64_t rgm_freelist_effective_block_size(uint64_t _blockSize)
{
    if (_blockSize < sizeof(uint32_t))
    {
        _blockSize = sizeof(uint32_t);
    }
    _blockSize = (_blockSize + 15u) & ~(uint64_t)15u;
    if (_blockSize == 0 || _blockSize > 0xFFFFFFFFu)
    {
        return 0;
    }
    return _blockSize;
}

/* Wire up *_fl, eagerly linking every block into the free chain. Caller has
 * already validated the inputs: _buffer non-0, _blockSize the clamped
 * effective size, _maxBlocks non-zero. */
static void rgm_freelist_install(FreeList* _fl, uint8_t* _buffer,
                                 uint32_t _blockSize, uint32_t _maxBlocks)
{
    for (uint32_t i = 0; i + 1u < _maxBlocks; ++i)
    {
        *(uint32_t*)(_buffer + (uint64_t)i * _blockSize) = i + 1u;
    }
    *(uint32_t*)(_buffer + (uint64_t)(_maxBlocks - 1u) * _blockSize) = _maxBlocks;

    _fl->m_maxBlocks  = _maxBlocks;
    _fl->m_blockSize  = _blockSize;
    _fl->m_blocksFree = _maxBlocks;
    _fl->m_initHigh   = _maxBlocks;   /* all blocks pre-linked -> bump path never taken */
    _fl->m_buffer     = _buffer;
    _fl->m_next       = _buffer;
}

/* Lazy install: same field setup as rgm_freelist_install but WITHOUT the
 * O(_maxBlocks) chain-linking pass. The free chain starts empty (m_next == 0)
 * and every block is initially "un-initialised": rgFreeListAlloc hands them out
 * by bumping m_initHigh from 0 upward, so a block's link word is only written
 * once it is actually freed. Nothing here touches the buffer, so its pages stay
 * unfaulted until real use. */
static void rgm_freelist_install_lazy(FreeList* _fl, uint8_t* _buffer,
                                      uint32_t _blockSize, uint32_t _maxBlocks)
{
    _fl->m_maxBlocks  = _maxBlocks;
    _fl->m_blockSize  = _blockSize;
    _fl->m_blocksFree = _maxBlocks;
    _fl->m_initHigh   = 0;             /* nothing bumped yet; Alloc fills 0,1,2,... */
    _fl->m_buffer     = _buffer;
    _fl->m_next       = 0;             /* empty returned-free chain */
}

/* ------------------------------------------------------------------------- */

/* Return the post-round-up buffer requirement, or 0 on overflow / zero input. */
uint64_t rgFreeListBufferSize(uint64_t _blockSize, uint32_t _maxBlocks)
{
    if (_maxBlocks == 0)
    {
        return 0;
    }
    uint64_t bs = rgm_freelist_effective_block_size(_blockSize);
    if (bs == 0 || bs > SIZE_MAX / _maxBlocks)
    {
        return 0;
    }
    return bs * _maxBlocks;
}

/* Populate *_list with an arena-backed block buffer. */
int32_t rgFreeListCreate(Arena* _arena, FreeList* _list,
                         uint64_t _blockSize, uint32_t _maxBlocks)
{
    if (_list == 0 || _maxBlocks == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t bs = rgm_freelist_effective_block_size(_blockSize);
    if (bs == 0 || bs > SIZE_MAX / _maxBlocks)
    {
        return RGM_ERROR_ERR_OVERFLOW;
    }

    /* An invalid arena surfaces as a 0 alloc; distinguish it from genuine
     * arena exhaustion so callers can tell a programming error from an OOM. */
    uint8_t* buffer = (uint8_t*)rgArenaAlloc(_arena, bs * _maxBlocks);
    if (buffer == 0)
    {
        return rgArenaIsValid(_arena) ? RGM_ERROR_ERR_NO_MEMORY
                                      : RGM_ERROR_ERR_INVALID;
    }

    rgm_freelist_install(_list, buffer, (uint32_t)bs, _maxBlocks);
    return RGM_ERROR_OK;
}

/* Lazy variant of rgFreeListCreate: identical validation and allocation, but the
 * block chain is bump-filled on demand instead of linked up front (see
 * rgm_freelist_install_lazy). O(1) setup, and the buffer's pages stay unfaulted
 * until blocks are actually used. */
int32_t rgFreeListCreateLazy(Arena* _arena, FreeList* _list,
                             uint64_t _blockSize, uint32_t _maxBlocks)
{
    if (_list == 0 || _maxBlocks == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t bs = rgm_freelist_effective_block_size(_blockSize);
    if (bs == 0 || bs > SIZE_MAX / _maxBlocks)
    {
        return RGM_ERROR_ERR_OVERFLOW;
    }

    uint8_t* buffer = (uint8_t*)rgArenaAlloc(_arena, bs * _maxBlocks);
    if (buffer == 0)
    {
        return rgArenaIsValid(_arena) ? RGM_ERROR_ERR_NO_MEMORY
                                      : RGM_ERROR_ERR_INVALID;
    }

    rgm_freelist_install_lazy(_list, buffer, (uint32_t)bs, _maxBlocks);
    return RGM_ERROR_OK;
}

/* Populate *_list using caller-owned _buffer; caller retains ownership. */
int32_t rgFreeListCreateFromMemory(void* _buffer, uint64_t _bufferSize,
                                   FreeList* _list,
                                   uint64_t _blockSize, uint32_t _maxBlocks)
{
    if (_list == 0 || _buffer == 0 || _maxBlocks == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t bs = rgm_freelist_effective_block_size(_blockSize);
    if (bs == 0 || bs > SIZE_MAX / _maxBlocks)
    {
        return RGM_ERROR_ERR_OVERFLOW;
    }
    if (_bufferSize < bs * _maxBlocks)
    {
        return RGM_ERROR_ERR_TOO_SMALL;
    }

    /* 16-byte alignment is required so block N inherits it for every N. */
    RGM_ASSERT(((uintptr_t)_buffer & 15u) == 0
               && "rgFreeListCreateFromMemory: buffer must be 16-byte aligned");

    rgm_freelist_install(_list, (uint8_t*)_buffer, (uint32_t)bs, _maxBlocks);
    return RGM_ERROR_OK;
}

/* Pop one block off the free chain, or return 0 when empty / inert. */
void* rgFreeListAlloc(FreeList* _list)
{
    if (!rgm_freelist_is_live(_list) || _list->m_blocksFree == 0)
    {
        return 0;
    }

    --_list->m_blocksFree;

    /* Prefer the returned-free chain (blocks freed after use). */
    if (_list->m_next)
    {
        void*    ret      = _list->m_next;
        uint32_t next_idx = *(uint32_t*)_list->m_next;

        /* m_maxBlocks doubles as the end-of-chain sentinel; every link in the
         * chain is valid because it was written by Free (a real index, or the
         * sentinel when pushing onto an empty chain) or by an eager Create. */
        _list->m_next = (next_idx == _list->m_maxBlocks)
                          ? 0
                          : _list->m_buffer + (uint64_t)next_idx * _list->m_blockSize;
        return ret;
    }

    /* Chain empty -> hand out the next never-initialised block by bumping the
     * watermark (lazy lists only; an eager list has m_initHigh == m_maxBlocks,
     * so this is unreachable there and its behaviour is unchanged). blocksFree
     * was just verified > 0 with an empty chain, which means m_initHigh <
     * m_maxBlocks, so the index is always in range. This yields 0,1,2,... in
     * exactly the order an eager pre-linked chain would. */
    return _list->m_buffer + (uint64_t)(_list->m_initHigh++) * _list->m_blockSize;
}

/* Push _ptr back onto the head of the free chain. No-op when the list is inert. */
void rgFreeListFree(FreeList* _list, void* _ptr)
{
    if (!rgm_freelist_is_live(_list))
    {
        return;
    }

    /* Cheap double-free detector: catches "free the same block twice in a
     * row". Doesn't catch deeper duplicates. */
    RGM_ASSERT(_ptr != _list->m_next && "rgFreeListFree: double free");

    if (_list->m_next)
    {
        uint32_t index   = (uint32_t)((_list->m_next - _list->m_buffer)
                                      / _list->m_blockSize);
        *(uint32_t*)_ptr  = index;
        _list->m_next = (uint8_t*)_ptr;
    }
    else
    {
        *(uint32_t*)_ptr  = _list->m_maxBlocks;
        _list->m_next = (uint8_t*)_ptr;
    }
    ++_list->m_blocksFree;
}

/* True if _ptr lies inside the list's block buffer (range check only, no liveness check). */
int rgFreeListCheckPtr(FreeList* _list, void* _ptr)
{
    if (!rgm_freelist_is_live(_list))
    {
        return 0;
    }
    /* Compare as integers, not via pointer subtraction: _ptr may point outside the buffer object, and
     * subtracting pointers into different objects is undefined behaviour the optimizer can exploit. */
    uintptr_t buffer_bytes = (uintptr_t)_list->m_maxBlocks * (uintptr_t)_list->m_blockSize;
    uintptr_t p            = (uintptr_t)_ptr;
    uintptr_t buf          = (uintptr_t)_list->m_buffer;
    if (p < buf)
    {
        return 0;
    }
    return buffer_bytes > (p - buf) ? 1 : 0;
}

/* Configured block count, or 0 when the list is inert. */
uint32_t rgFreeListMaxBlocks(FreeList* _list)
{
    return rgm_freelist_is_live(_list) ? _list->m_maxBlocks : 0;
}

/* Effective per-block size after the clamp + 16-byte round-up. */
uint32_t rgFreeListBlockSize(FreeList* _list)
{
    return rgm_freelist_is_live(_list) ? _list->m_blockSize : 0;
}

/* Number of blocks currently available for allocation. */
uint32_t rgFreeListBlocksFree(FreeList* _list)
{
    return rgm_freelist_is_live(_list) ? _list->m_blocksFree : 0;
}
