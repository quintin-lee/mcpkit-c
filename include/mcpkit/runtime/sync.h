/**
 * @file sync.h
 * @brief Synchronous (inline) executor factory.
 * @ingroup mcpkit-runtime
 *
 * Tasks are executed immediately on the calling thread when submitted.
 * wait() is a no-op.
 */

#ifndef MCPKIT_RUNTIME_SYNC_H
#define MCPKIT_RUNTIME_SYNC_H

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

/**
 * @brief Creates a synchronous (inline) executor.
 *
 * Tasks are executed immediately on the calling thread when submitted.
 * wait() is a no-op.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @return Owned mcp_executor_t, or NULL on OOM.
 */
mcp_executor_t *mcp_sync_executor_create(mcp_context_t *ctx);

#endif
