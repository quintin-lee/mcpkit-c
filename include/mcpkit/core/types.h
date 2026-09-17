/**
 * @file types.h
 * Opaque forward declarations for all core handle types plus the
 * pluggable allocator struct.
 *
 * All handle types are opaque: user code holds only pointers. The
 * concrete struct definitions live in internal headers so the ABI
 * can evolve (e.g. switching the thread pool from std::thread to
 * libuv) without breaking downstream binaries.
 */

#ifndef MCPKIT_CORE_TYPES_H
#define MCPKIT_CORE_TYPES_H

#include <stddef.h>


/**
 * @brief Opaque forward declaration: execution context.
 *
 * Owns allocator, logger, and JSON backend selection.
 */
typedef struct mcp_context mcp_context_t;
/**
 * @brief Opaque forward declaration: server registry.
 *
 * Holds tools, resources, prompts, and active sessions.
 */
typedef struct mcp_server mcp_server_t;
/**
 * @brief Opaque forward declaration: a single MCP session.
 *
 * Represents one client connection and its state.
 */
typedef struct mcp_session mcp_session_t;
/**
 * @brief Opaque forward declaration: registered tool.
 */
typedef struct mcp_tool mcp_tool_t;
/**
 * @brief Opaque forward declaration: registered resource.
 */
typedef struct mcp_resource mcp_resource_t;
/**
 * @brief Opaque forward declaration: registered prompt.
 */
typedef struct mcp_prompt mcp_prompt_t;

/**
 * @brief Pluggable allocator.
 *
 * Each function pointer is optional: a NULL entry is resolved to the
 * corresponding libc function at `mcp_context_create` time. The
 * `userdata` pointer is passed through to every call, so the same
 * allocator instance can be shared across contexts that use
 * different backends.
 *
 * @note A context created with a given allocator must use that
 *       allocator for every free; do not mix allocators across calls.
 */
typedef struct mcp_allocator {
    void *(*malloc_fn)(size_t size, void *userdata);
    void (*free_fn)(void *ptr, void *userdata);
    void *(*calloc_fn)(size_t nmemb, size_t size, void *userdata);
    void *(*realloc_fn)(void *ptr, size_t size, void *userdata);
    void *userdata;
} mcp_allocator_t;

/**
 * @brief Returns a pointer to the process-wide default (libc-backed) allocator.
 *
 * The pointer is valid for the lifetime of the process and needs no
 * release.
 *
 * @return Borrowed default allocator, valid for the lifetime of the process.
 */
const mcp_allocator_t *mcp_default_allocator(void);

#endif
