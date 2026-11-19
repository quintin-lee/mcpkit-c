#include "mcpkit/core/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MCP_ARENA_DEFAULT_CHUNK_SIZE (32768)
#define MCP_ARENA_DEFAULT_ALIGNMENT (sizeof(void *) * 2)

typedef struct mcp_arena_alloc_hdr {
    size_t size;
} mcp_arena_alloc_hdr_t;

typedef struct mcp_arena_chunk {
    struct mcp_arena_chunk *next;
    size_t capacity;
    size_t offset;
    uint8_t data[];
} mcp_arena_chunk_t;

struct mcp_arena {
    mcp_arena_chunk_t *first;
    mcp_arena_chunk_t *current;
    size_t default_chunk_size;
    const mcp_allocator_t *upstream;
};

static void *upstream_malloc(const mcp_allocator_t *alloc, size_t size) {
    if (alloc != NULL && alloc->malloc_fn != NULL) {
        return alloc->malloc_fn(size, alloc->userdata);
    }
    return malloc(size);
}

static void upstream_free(const mcp_allocator_t *alloc, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (alloc != NULL && alloc->free_fn != NULL) {
        alloc->free_fn(ptr, alloc->userdata);
    } else {
        free(ptr);
    }
}

static mcp_arena_chunk_t *alloc_chunk(const mcp_allocator_t *upstream, size_t capacity) {
    size_t total_size = sizeof(mcp_arena_chunk_t) + capacity;
    mcp_arena_chunk_t *chunk = (mcp_arena_chunk_t *)upstream_malloc(upstream, total_size);
    if (chunk == NULL) {
        return NULL;
    }
    chunk->next = NULL;
    chunk->capacity = capacity;
    chunk->offset = 0;
    return chunk;
}

mcp_arena_t *mcp_arena_create(const mcp_allocator_t *upstream, size_t default_chunk_size) {
    mcp_arena_t *arena = (mcp_arena_t *)upstream_malloc(upstream, sizeof(mcp_arena_t));
    if (arena == NULL) {
        return NULL;
    }
    arena->upstream = upstream;
    arena->default_chunk_size =
        (default_chunk_size > 0) ? default_chunk_size : MCP_ARENA_DEFAULT_CHUNK_SIZE;

    arena->first = alloc_chunk(upstream, arena->default_chunk_size);
    if (arena->first == NULL) {
        upstream_free(upstream, arena);
        return NULL;
    }
    arena->current = arena->first;
    return arena;
}

void *mcp_arena_alloc_aligned(mcp_arena_t *arena, size_t size, size_t alignment) {
    if (arena == NULL || size == 0) {
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    /* Ensure alignment is a power of two */
    if ((alignment & (alignment - 1)) != 0) {
        return NULL;
    }

    mcp_arena_chunk_t *chunk = arena->current;
    while (chunk != NULL) {
        uintptr_t curr_addr = (uintptr_t)(chunk->data + chunk->offset);
        uintptr_t min_hdr_addr = curr_addr;
        uintptr_t user_addr = (min_hdr_addr + sizeof(mcp_arena_alloc_hdr_t) + (alignment - 1)) &
                              ~(uintptr_t)(alignment - 1);
        uintptr_t hdr_addr = user_addr - sizeof(mcp_arena_alloc_hdr_t);

        if (user_addr + size <= (uintptr_t)(chunk->data + chunk->capacity)) {
            mcp_arena_alloc_hdr_t *hdr = (mcp_arena_alloc_hdr_t *)hdr_addr;
            hdr->size = size;
            chunk->offset = (size_t)((user_addr + size) - (uintptr_t)chunk->data);
            arena->current = chunk;
            return (void *)user_addr;
        }

        /* Check next existing chunk if available */
        if (chunk->next != NULL) {
            chunk = chunk->next;
        } else {
            break;
        }
    }

    /* Allocate a new chunk */
    size_t needed = size + sizeof(mcp_arena_alloc_hdr_t) + alignment;
    size_t chunk_cap = (needed > arena->default_chunk_size) ? needed : arena->default_chunk_size;
    mcp_arena_chunk_t *new_chunk = alloc_chunk(arena->upstream, chunk_cap);
    if (new_chunk == NULL) {
        return NULL;
    }

    /* Append to chain */
    if (chunk != NULL) {
        chunk->next = new_chunk;
    } else {
        arena->first = new_chunk;
    }
    arena->current = new_chunk;

    uintptr_t base_addr = (uintptr_t)new_chunk->data;
    uintptr_t user_addr =
        (base_addr + sizeof(mcp_arena_alloc_hdr_t) + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
    uintptr_t hdr_addr = user_addr - sizeof(mcp_arena_alloc_hdr_t);
    mcp_arena_alloc_hdr_t *hdr = (mcp_arena_alloc_hdr_t *)hdr_addr;
    hdr->size = size;
    new_chunk->offset = (size_t)((user_addr + size) - base_addr);
    return (void *)user_addr;
}

void *mcp_arena_alloc(mcp_arena_t *arena, size_t size) {
    return mcp_arena_alloc_aligned(arena, size, MCP_ARENA_DEFAULT_ALIGNMENT);
}

void *mcp_arena_realloc(mcp_arena_t *arena, void *ptr, size_t new_size) {
    if (arena == NULL) {
        return NULL;
    }
    if (ptr == NULL) {
        return mcp_arena_alloc(arena, new_size);
    }
    if (new_size == 0) {
        return NULL;
    }

    mcp_arena_alloc_hdr_t *hdr =
        (mcp_arena_alloc_hdr_t *)((uint8_t *)ptr - sizeof(mcp_arena_alloc_hdr_t));
    size_t old_size = hdr->size;

    /* Check if ptr is the top allocation in the current chunk */
    mcp_arena_chunk_t *chunk = arena->current;
    if (chunk != NULL && (uint8_t *)ptr >= chunk->data &&
        (uint8_t *)ptr < chunk->data + chunk->capacity) {
        uintptr_t ptr_end = (uintptr_t)ptr + old_size;
        uintptr_t chunk_top = (uintptr_t)(chunk->data + chunk->offset);

        if (ptr_end == chunk_top) {
            /* We are at the top of the bump pointer in current chunk */
            if ((uintptr_t)ptr + new_size <= (uintptr_t)(chunk->data + chunk->capacity)) {
                hdr->size = new_size;
                chunk->offset = (size_t)(((uintptr_t)ptr + new_size) - (uintptr_t)chunk->data);
                return ptr;
            }
        }
    }

    /* Otherwise allocate new and copy */
    void *new_ptr = mcp_arena_alloc(arena, new_size);
    if (new_ptr == NULL) {
        return NULL;
    }
    size_t copy_len = (old_size < new_size) ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_len);
    return new_ptr;
}

char *mcp_arena_strdup(mcp_arena_t *arena, const char *s) {
    if (arena == NULL || s == NULL) {
        return NULL;
    }
    size_t len = strlen(s);
    char *dup = (char *)mcp_arena_alloc(arena, len + 1);
    if (dup != NULL) {
        memcpy(dup, s, len + 1);
    }
    return dup;
}

void mcp_arena_reset(mcp_arena_t *arena) {
    if (arena == NULL) {
        return;
    }
    mcp_arena_chunk_t *curr = arena->first;
    while (curr != NULL) {
        curr->offset = 0;
        curr = curr->next;
    }
    arena->current = arena->first;
}

void mcp_arena_destroy(mcp_arena_t *arena) {
    if (arena == NULL) {
        return;
    }
    mcp_arena_chunk_t *curr = arena->first;
    while (curr != NULL) {
        mcp_arena_chunk_t *next = curr->next;
        upstream_free(arena->upstream, curr);
        curr = next;
    }
    upstream_free(arena->upstream, arena);
}

/* Allocator adapter callbacks */
static void *arena_adapter_malloc(size_t size, void *userdata) {
    return mcp_arena_alloc((mcp_arena_t *)userdata, size);
}

static void *arena_adapter_realloc(void *ptr, size_t size, void *userdata) {
    return mcp_arena_realloc((mcp_arena_t *)userdata, ptr, size);
}

static void arena_adapter_free(void *ptr, void *userdata) {
    (void)ptr;
    (void)userdata;
    /* Arena individual frees are no-ops */
}

static void *arena_adapter_calloc(size_t nmemb, size_t size, void *userdata) {
    size_t total = nmemb * size;
    void *p = mcp_arena_alloc((mcp_arena_t *)userdata, total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

mcp_allocator_t mcp_arena_as_allocator(mcp_arena_t *arena) {
    mcp_allocator_t alloc = {
        .malloc_fn = arena_adapter_malloc,
        .free_fn = arena_adapter_free,
        .calloc_fn = arena_adapter_calloc,
        .realloc_fn = arena_adapter_realloc,
        .userdata = arena
    };
    return alloc;
}
