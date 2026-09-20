/**
 * @file task.h
 * @brief Task callback types shared by executor backends.
 * @ingroup mcpkit-runtime
 */

#ifndef MCPKIT_RUNTIME_TASK_H
#define MCPKIT_RUNTIME_TASK_H

typedef struct mcp_context mcp_context_t;

/**
 * @brief Task callback signature.
 *
 * @param ctx Context captured at submit time; forwarded to the task body.
 * @param arg Opaque pointer passed at submit time.
 */
typedef void (*mcp_task_fn)(mcp_context_t *ctx, void *arg);

#endif
