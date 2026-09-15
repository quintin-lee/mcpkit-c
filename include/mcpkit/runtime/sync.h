#ifndef MCPKIT_RUNTIME_SYNC_H
#define MCPKIT_RUNTIME_SYNC_H

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

// Inline executor: submit runs fn immediately on the caller thread,
// wait is a no-op returning MCP_OK. Never NULL on success.
mcp_executor_t *mcp_sync_executor_create(mcp_context_t *ctx);

#endif
