/**
 * @file arena.h
 * @brief Chunked monotonic arena allocator with realloc support.
 *
 * Implements a region-based bump allocator suitable for request lifecycles.
 * Supports arbitrary alignments, O(1) bulk reset without releasing chunk
 * allocations to the OS, and bridging directly to `mcp_allocator_t`.
 *
 * @ingroup mcpkit-core
 */

#ifndef MCPKIT_CORE_ARENA_H
#define MCPKIT_CORE_ARENA_H

#include <stddef.h>
#include "mcpkit/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_arena mcp_arena_t;

/**
 * @brief Creates a chunked arena allocator.
 *
 * @param upstream Upstream allocator for chunk management; NULL for default libc.
 * @param default_chunk_size Size in bytes for standard chunks (e.g. 32768). 0 uses default (32KB).
 * @return Owned arena instance, or NULL on allocation failure.
 */
mcp_arena_t *mcp_arena_create(const mcp_allocator_t *upstream, size_t default_chunk_size);

/**
 * @brief Allocates `size` bytes from the arena with default pointer alignment (max_align_t / 8 bytes).
 *
 * @param arena Arena handle; must not be NULL.
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 */
void *mcp_arena_alloc(mcp_arena_t *arena, size_t size);

/**
 * @brief Allocates `size` bytes from the arena with specified alignment.
 *
 * @param arena Arena handle; must not be NULL.
 * @param size Number of bytes to allocate.
 * @param alignment Required alignment (must be a power of 2, minimum sizeof(void*)).
 * @return Pointer to aligned memory, or NULL on failure.
 */
void *mcp_arena_alloc_aligned(mcp_arena_t *arena, size_t size, size_t alignment);

/**
 * @brief Reallocates `ptr` to `new_size` bytes within the arena.
 *
 * If `ptr` was the most recent allocation in the active chunk and capacity allows,
 * it expands in-place. Otherwise, a new block is allocated and previous data is copied.
 *
 * @param arena Arena handle; must not be NULL.
 * @param ptr Pointer previously returned by `mcp_arena_alloc*`, or NULL (acts as alloc).
 * @param new_size New size in bytes. If 0, returns NULL (does not free individual block).
 * @return Pointer to reallocated memory, or NULL on failure.
 */
void *mcp_arena_realloc(mcp_arena_t *arena, void *ptr, size_t new_size);

/**
 * @brief Duplicates a NULL-terminated string using memory allocated from the arena.
 *
 * @param arena Arena handle; must not be NULL.
 * @param s String to duplicate; NULL returns NULL.
 * @return Duplicated string in arena, or NULL on failure.
 */
char *mcp_arena_strdup(mcp_arena_t *arena, const char *s);

/**
 * @brief Resets all allocation offsets in the arena to zero.
 *
 * Retains previously allocated chunks for subsequent allocations without OS overhead.
 *
 * @param arena Arena handle; NULL is a no-op.
 */
void mcp_arena_reset(mcp_arena_t *arena);

/**
 * @brief Destroys the arena and releases all memory chunks back to the upstream allocator.
 *
 * @param arena Arena handle; NULL is a no-op.
 */
void mcp_arena_destroy(mcp_arena_t *arena);

/**
 * @brief Constructs an `mcp_allocator_t` adapter backed by this arena.
 *
 * `malloc_fn` calls `mcp_arena_alloc`.
 * `realloc_fn` calls `mcp_arena_realloc`.
 * `free_fn` is a no-op.
 * `calloc_fn` allocates and zeroes memory.
 *
 * @param arena Arena handle; must remain alive for the lifetime of using the allocator.
 * @return Populated `mcp_allocator_t` struct with userdata pointing to `arena`.
 */
mcp_allocator_t mcp_arena_as_allocator(mcp_arena_t *arena);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_CORE_ARENA_H */
