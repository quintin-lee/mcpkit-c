#ifndef MCPKIT_CORE_TYPES_H
#define MCPKIT_CORE_TYPES_H

#include <stddef.h>

// Opaque forward declarations (ABI stability).
// Users only ever hold pointers; internals can move from
// pthread to libuv without touching user code.
typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_prompt mcp_prompt_t;

// Pluggable allocator. Any NULL function pointer falls back to libc
// at context creation time. userdata is passed through to each call.
typedef struct mcp_allocator {
    void *(*malloc_fn)(size_t size, void *userdata);
    void (*free_fn)(void *ptr, void *userdata);
    void *(*calloc_fn)(size_t nmemb, size_t size, void *userdata);
    void *(*realloc_fn)(void *ptr, size_t size, void *userdata);
    void *userdata;
} mcp_allocator_t;

const mcp_allocator_t *mcp_default_allocator(void);

#endif
