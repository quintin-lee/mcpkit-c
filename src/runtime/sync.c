/**
 * @file sync.c
 *
 * Synchronous (inline) executor backend. submit() runs the task on the
 * caller thread and wait() is a no-op. Wraps the ops table with
 * mcp_executor_create so the public handle routes through the standard
 * executor lifecycle.
 */
#include "mcpkit/runtime/sync.h"

#include "internals.h"
#include "mcpkit/runtime/executor.h"

static mcp_status_t sync_submit(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn,
                                void *arg) {
    (void)ex;
    fn(ctx, arg);
    return MCP_OK;
}

static mcp_status_t sync_wait(mcp_context_t *ctx, mcp_executor_t *ex) {
    (void)ctx;
    (void)ex;
    return MCP_OK;
}

static void sync_destroy_backend(mcp_context_t *ctx, mcp_executor_t *ex) {
    (void)ctx;
    (void)ex;
}

static const mcp_executor_ops_t sync_ops = {
    .submit = sync_submit,
    .wait = sync_wait,
    .destroy_backend = sync_destroy_backend,
};

mcp_executor_t *mcp_sync_executor_create(mcp_context_t *ctx) {
    return mcp_executor_create(ctx, &sync_ops, NULL);
}
