/**
 * @file executor.c
 *
 * Generic executor wrapper over a user-supplied ops table.
 * mcp_executor_destroy only frees the executor struct itself; if the
 * ops table provides a non-NULL destroy_backend slot it is invoked
 * with the executor so backends can tear down their own state.
 *
 * Dispatch error convention: a NULL fn argument is INVALID_ARGUMENT
 * (bad caller input), whereas a missing ops slot is UNSUPPORTED
 * (the backend chose not to implement that operation).
 */
#include "mcpkit/runtime/executor.h"

#include "internals.h"

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

void *rt_malloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = alloc_of(ctx);
    return a->malloc_fn(n, a->userdata);
}

void rt_free(mcp_context_t *ctx, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(ptr, a->userdata);
}

mcp_executor_t *mcp_executor_create(mcp_context_t *ctx, const mcp_executor_ops_t *ops,
                                    void *backend) {
    if (ops == NULL) {
        return NULL;
    }
    mcp_executor_t *ex = rt_malloc(ctx, sizeof(*ex));
    if (ex == NULL) {
        return NULL;
    }
    ex->ops = *ops;
    ex->backend = backend;
    return ex;
}

void mcp_executor_destroy(mcp_context_t *ctx, mcp_executor_t *ex) {
    if (ex == NULL) {
        return;
    }
    if (ex->ops.destroy_backend != NULL) {
        ex->ops.destroy_backend(ctx, ex);
    }
    rt_free(ctx, ex);
}

void *mcp_executor_backend(mcp_context_t *ctx, const mcp_executor_t *ex) {
    (void)ctx;
    return ex != NULL ? ex->backend : NULL;
}

mcp_status_t mcp_executor_submit(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn,
                                 void *arg) {
    if (ex == NULL || fn == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (ex->ops.submit == NULL) {
        return MCP_ERR_UNSUPPORTED;
    }
    return ex->ops.submit(ctx, ex, fn, arg);
}

mcp_status_t mcp_executor_wait(mcp_context_t *ctx, mcp_executor_t *ex) {
    if (ex == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (ex->ops.wait == NULL) {
        return MCP_ERR_UNSUPPORTED;
    }
    return ex->ops.wait(ctx, ex);
}
