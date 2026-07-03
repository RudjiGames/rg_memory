/*
 * Imagus Engine - Memory Management Library.
 * Copyright (c) 2025-2026 Milos Tosic, Rudji Games. All rights reserved.
 * License: https://github.com/RudjiGames/rg_memory/blob/master/LICENSE
 *
 * Dense-packed pool allocator, arena embedded.
 *
 * Design:
 *   - Callers own the DenseList struct (see rg_memory.h). Create
 *     populates it in place; there is no internal pool, no handles,
 *     no matching Destroy. A zero-initialised DenseList is treated
 *     as uninitialised: every API call is a safe no-op until Create
 *     has been called.
 *   - Live blocks always occupy the prefix [0, m_count) of the
 *     buffer. Alloc just returns slot m_count and bumps the counter;
 *     Free overwrites the freed slot with the contents of the last
 *     live block and decrements the counter ("swap-back").
 *   - No per-block chain link, so the only block-size constraint is
 *     the 16-byte alignment round-up (matching the arena's default).
 *     Tiny blocks (e.g. 4-byte IDs) round up to 16 bytes -- pay the
 *     cost for alignment, not for chain metadata.
 *   - Pointers returned by Alloc are invalidated by any subsequent
 *     Free on any block. Callers that need stable references should
 *     use FreeList (stable pointers, holes allowed) or a sparse-set
 *     layer on top.
 *
 * Single-threaded by convention. Concurrent operations on the same
 * DenseList have undefined results; serialise externally if needed.
 */

#include "../include/rg_memory/rg_memory.h"
#include "rg_memory_platform.h"

/* Error codes (RGM_ERROR_*) are declared in rg_memory.h and shared with FreeList. */

/* A DenseList with m_buffer == 0 is treated as uninitialised: queries
 * report zero, Alloc returns 0, Free / Clear do nothing. */
static int rgm_denselist_is_live(const DenseList* _dl)
{
    return _dl != 0 && _dl->m_buffer != 0;
}

/* Round up to a multiple of 16 so block N is 16-byte aligned for every
 * N. Unlike FreeList there is no minimum size beyond that round-up,
 * since DenseList stores no per-block metadata. Returns 0 on overflow
 * / bad input (also when the rounded result would not fit in uint32_t,
 * the type used for m_blockSize). */
static uint64_t rgm_denselist_effective_block_size(uint64_t _blockSize)
{
    if (_blockSize == 0)
    {
        _blockSize = 1;
    }
    _blockSize = (_blockSize + 15u) & ~(uint64_t)15u;
    if (_blockSize == 0 || _blockSize > 0xFFFFFFFFu)
    {
        return 0;
    }
    return _blockSize;
}

/* Wire up *_dl. Caller has already validated the inputs: _buffer
 * non-0, _blockSize the clamped effective size, _maxBlocks non-zero. */
static void rgm_denselist_install(DenseList* _dl, uint8_t* _buffer,
                                  uint32_t _blockSize, uint32_t _maxBlocks)
{
    _dl->m_maxBlocks = _maxBlocks;
    _dl->m_blockSize = _blockSize;
    _dl->m_count     = 0;
    _dl->m_buffer    = _buffer;
}

/* ------------------------------------------------------------------------- */

/* Return the post-round-up buffer requirement, or 0 on overflow / zero input. */
uint64_t rgDenseListBufferSize(uint64_t _blockSize, uint32_t _maxBlocks)
{
    if (_maxBlocks == 0)
    {
        return 0;
    }
    uint64_t bs = rgm_denselist_effective_block_size(_blockSize);
    if (bs == 0 || bs > SIZE_MAX / _maxBlocks)
    {
        return 0;
    }
    return bs * _maxBlocks;
}

/* Populate *_list with an arena-backed block buffer. */
int32_t rgDenseListCreate(Arena* _arena, DenseList* _list,
                          uint64_t _blockSize, uint32_t _maxBlocks)
{
    if (_list == 0 || _maxBlocks == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t bs = rgm_denselist_effective_block_size(_blockSize);
    if (bs == 0 || bs > SIZE_MAX / _maxBlocks)
    {
        return RGM_ERROR_ERR_OVERFLOW;
    }

    /* Same arena-vs-OOM disambiguation as rgFreeListCreate. */
    uint8_t* buffer = (uint8_t*)rgArenaAlloc(_arena, bs * _maxBlocks);
    if (buffer == 0)
    {
        return rgArenaIsValid(_arena) ? RGM_ERROR_ERR_NO_MEMORY
                                      : RGM_ERROR_ERR_INVALID;
    }

    rgm_denselist_install(_list, buffer, (uint32_t)bs, _maxBlocks);
    return RGM_ERROR_OK;
}

/* Populate *_list using caller-owned _buffer; caller retains ownership. */
int32_t rgDenseListCreateFromMemory(void* _buffer, uint64_t _bufferSize,
                                    DenseList* _list,
                                    uint64_t _blockSize, uint32_t _maxBlocks)
{
    if (_list == 0 || _buffer == 0 || _maxBlocks == 0)
    {
        return RGM_ERROR_ERR_INVALID;
    }

    uint64_t bs = rgm_denselist_effective_block_size(_blockSize);
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
               && "rgDenseListCreateFromMemory: buffer must be 16-byte aligned");

    rgm_denselist_install(_list, (uint8_t*)_buffer, (uint32_t)bs, _maxBlocks);
    return RGM_ERROR_OK;
}

/* Reset the live count to 0 in O(1); the buffer contents are left untouched. */
void rgDenseListClear(DenseList* _list)
{
    if (rgm_denselist_is_live(_list))
    {
        _list->m_count = 0;
    }
}

/* Return slot m_count and bump it; 0 when the list is full or inert. */
void* rgDenseListAlloc(DenseList* _list)
{
    if (!rgm_denselist_is_live(_list) || _list->m_count >= _list->m_maxBlocks)
    {
        return 0;
    }
    void* slot = _list->m_buffer + (uint64_t)_list->m_count * _list->m_blockSize;
    ++_list->m_count;
    return slot;
}

/* Swap-back: overwrite *_ptr with the contents of the last live block, then decrement m_count. */
void rgDenseListFree(DenseList* _list, void* _ptr)
{
    if (!rgm_denselist_is_live(_list) || _list->m_count == 0)
    {
        return;
    }

    uint8_t* last = _list->m_buffer
                  + (uint64_t)(_list->m_count - 1u) * _list->m_blockSize;

    /* Debug-only: validate _ptr lies on a block boundary inside the
     * live range. In release we trust the caller. */
    RGM_ASSERT((uint8_t*)_ptr >= _list->m_buffer
               && (uint8_t*)_ptr <= last
               && (((uint8_t*)_ptr - _list->m_buffer) % _list->m_blockSize) == 0
               && "rgDenseListFree: pointer out of live range or not block-aligned");

    /* Freeing the last live block doesn't need a copy, and skipping it
     * avoids a self-overlapping copy when _ptr == last. */
    if ((uint8_t*)_ptr != last)
    {
        /* Block-aligned, non-overlapping, single block: a straight byte
         * loop is good enough and keeps the file CRT-free (matches the
         * style of rg_arena.c). The compiler vectorises this for typical
         * block sizes. */
        uint8_t*       dst = (uint8_t*)_ptr;
        const uint8_t* src = last;
        uint32_t       n   = _list->m_blockSize;
        for (uint32_t i = 0; i < n; ++i)
        {
            dst[i] = src[i];
        }
    }
    --_list->m_count;
}

/* Base pointer of the live region; pair with Count and BlockSize for iteration. */
void* rgDenseListData(DenseList* _list)
{
    return rgm_denselist_is_live(_list) ? _list->m_buffer : 0;
}

/* Pointer to slot _index, or 0 when _index >= m_count / inert. */
void* rgDenseListAt(DenseList* _list, uint32_t _index)
{
    if (!rgm_denselist_is_live(_list) || _index >= _list->m_count)
    {
        return 0;
    }
    return _list->m_buffer + (uint64_t)_index * _list->m_blockSize;
}

/* Slot index of _ptr in [0, m_maxBlocks), or UINT32_MAX when out of the buffer. */
uint32_t rgDenseListIndexOf(DenseList* _list, void* _ptr)
{
    if (!rgm_denselist_is_live(_list) || _ptr == 0)
    {
        return UINT32_MAX;
    }
    /* Integer arithmetic, not pointer subtraction: _ptr may point outside the buffer object and
     * subtracting pointers into different objects is undefined behaviour the optimizer can exploit. */
    uintptr_t buffer_bytes = (uintptr_t)_list->m_maxBlocks * (uintptr_t)_list->m_blockSize;
    uintptr_t p            = (uintptr_t)_ptr;
    uintptr_t buf          = (uintptr_t)_list->m_buffer;
    uintptr_t offset;
    if (p < buf)
    {
        return UINT32_MAX;
    }
    offset = p - buf;
    if (offset >= buffer_bytes)
    {
        return UINT32_MAX;
    }
    RGM_ASSERT((offset % _list->m_blockSize) == 0
               && "rgDenseListIndexOf: pointer not block-aligned");
    return (uint32_t)(offset / _list->m_blockSize);
}

/* Configured block count, or 0 when the list is inert. */
uint32_t rgDenseListMaxBlocks(DenseList* _list)
{
    return rgm_denselist_is_live(_list) ? _list->m_maxBlocks : 0;
}

/* Effective per-block size after the 16-byte round-up. */
uint32_t rgDenseListBlockSize(DenseList* _list)
{
    return rgm_denselist_is_live(_list) ? _list->m_blockSize : 0;
}

/* Number of live blocks (size / length). */
uint32_t rgDenseListCount(DenseList* _list)
{
    return rgm_denselist_is_live(_list) ? _list->m_count : 0;
}

/* True if _ptr lies inside the buffer; does not check liveness. */
int rgDenseListCheckPtr(DenseList* _list, void* _ptr)
{
    if (!rgm_denselist_is_live(_list))
    {
        return 0;
    }
    /* Integer arithmetic, not pointer subtraction: _ptr may point outside the buffer object and
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
