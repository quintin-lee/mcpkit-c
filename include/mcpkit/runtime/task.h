/**
 * @brief Task callback signature.
 *
 * @param ctx Context captured at submit time; forwarded to the task body.
 * @param arg Opaque pointer passed at submit time.
 */

/**
 * @brief Task body signature shared by every executor backend.
 *
 * @p fn runs with the submit-time context. fn/arg must outlive the
 * executor's wait/destroy that drains pending work; the executor does not
 * free them.
 */
#ifndef MCPKIT_RUNTIME_TASK_H
#define MCPKIT_RUNTIME_TASK_H

typedef struct mcp_context mcp_context_t;

typedef void (*mcp_task_fn)(mcp_context_t *ctx, void *arg);

#endif
