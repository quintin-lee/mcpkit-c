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

#endif
