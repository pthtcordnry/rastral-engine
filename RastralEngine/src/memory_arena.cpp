#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define GAME_ARENA_SIZE (1024 * 1024)
#define HEADER_SIZE (sizeof(size_t))

typedef struct BlockHeader
{
    size_t size;
    struct BlockHeader* next;
} BlockHeader;

typedef struct MemoryArena
{
    size_t size;
    unsigned char* base;
    BlockHeader* freeList;
} MemoryArena;

MemoryArena engineMemArena;

size_t align8(size_t size)
{
    return (size + 7U) & ~7U;
}

void arena_init(MemoryArena* arena, size_t size)
{
    if (!arena)
        return;
    arena->size = size;
    arena->base = (unsigned char*)malloc(size);
    if (!arena->base)
    {
        fprintf(stderr, "MemoryArena: allocation of %zu bytes failed\n", size);
        exit(1);
    }
    // Initialize the free list to encompass the entire block.
    arena->freeList = (BlockHeader*)arena->base;
    arena->freeList->size = size;
    arena->freeList->next = NULL;
}

void arena_reset(MemoryArena* arena)
{
    if (!arena || !arena->base)
        return;
    arena->freeList = (BlockHeader*)arena->base;
    arena->freeList->size = arena->size;
    arena->freeList->next = NULL;
}

void arena_destroy(MemoryArena* arena)
{
    if (!arena)
        return;
    free(arena->base);
    arena->base = NULL;
    arena->freeList = NULL;
    arena->size = 0;
}

void* arena_alloc(MemoryArena* arena, size_t size)
{
    if (!arena)
        return NULL;
    size_t totalSize = align8(size + HEADER_SIZE);
    BlockHeader* prev = NULL;
    BlockHeader* curr = arena->freeList;

    while (curr)
    {
        if (curr->size >= totalSize)
        {
            if (curr->size - totalSize >= (sizeof(BlockHeader) + 8))
            {
                // Split the block.
                BlockHeader* newBlock = (BlockHeader*)((unsigned char*)curr + totalSize);
                newBlock->size = curr->size - totalSize;
                newBlock->next = curr->next;
                if (prev)
                    prev->next = newBlock;
                else
                    arena->freeList = newBlock;
                curr->size = totalSize;
            }
            else
            {
                // Use the entire block.
                if (prev)
                    prev->next = curr->next;
                else
                    arena->freeList = curr->next;
            }
            // Store the allocated block size.
            *((size_t*)curr) = curr->size;
            return (void*)((unsigned char*)curr + HEADER_SIZE);
        }
        prev = curr;
        curr = curr->next;
    }

    fprintf(stderr, "Arena out of memory in arena_alloc\n");
    return NULL;
}

void arena_free(MemoryArena* arena, void* ptr)
{
    if (!arena || !ptr)
        return;
    BlockHeader* block = (BlockHeader*)((unsigned char*)ptr - HEADER_SIZE);
    // Insert block into the free list in address order.
    BlockHeader* prev = NULL;
    BlockHeader* curr = arena->freeList;
    while (curr && curr < block)
    {
        prev = curr;
        curr = curr->next;
    }
    block->next = curr;
    if (prev)
        prev->next = block;
    else
        arena->freeList = block;
    // Coalesce with next block.
    if (block->next &&
        ((unsigned char*)block + block->size == (unsigned char*)block->next))
    {
        block->size += block->next->size;
        block->next = block->next->next;
    }
    // Coalesce with previous block.
    if (prev &&
        ((unsigned char*)prev + prev->size == (unsigned char*)block))
    {
        prev->size += block->size;
        prev->next = block->next;
    }
}

void* arena_realloc(MemoryArena* arena, void* ptr, size_t new_size)
{
    if (!arena)
        return NULL;
    if (!ptr)
        return arena_alloc(arena, new_size);
    if (new_size == 0)
    {
        arena_free(arena, ptr);
        return NULL;
    }

    BlockHeader* oldHeader = (BlockHeader*)((unsigned char*)ptr - HEADER_SIZE);
    size_t oldTotalSize = oldHeader->size;
    size_t oldUserSize = oldTotalSize - HEADER_SIZE;
    size_t newTotalSize = align8(new_size + HEADER_SIZE);

    // Allocate new block.
    void* newPtr = arena_alloc(arena, new_size);
    if (!newPtr)
    {
        return NULL;
    }

    size_t copySize = (oldUserSize < new_size) ? oldUserSize : new_size;
    memcpy(newPtr, ptr, copySize);
    if (new_size > copySize)
    {
        memset((unsigned char*)newPtr + copySize, 0, new_size - copySize);
    }

    arena_free(arena, ptr);
    return newPtr;
}