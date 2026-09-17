/**
 * @file sync.h
 * Inline executor backend.
 *
 * submit runs the task immediately on the caller thread; wait is a no-op
 * that returns MCP_OK. Never returns NULL on success.
 */
#ifndef MCPKIT_RUNTIME_SYNC_H
#define MCPKIT_RUNTIME_SYNC_H

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

mcp_executor_t *mcp_sync_executor_create(mcp_context_t *ctx);

#endif
