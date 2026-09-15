#ifndef MCPKIT_RUNTIME_EXECUTOR_H
#define MCPKIT_RUNTIME_EXECUTOR_H

#include "mcpkit/core/error.h"
#include "mcpkit/runtime/task.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

typedef struct mcp_executor_ops {
    mcp_status_t (*submit)(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn, void *arg);
    mcp_status_t (*wait)(mcp_context_t *ctx, mcp_executor_t *ex);
    // Backend teardown, called once by mcp_executor_destroy before the shell
    // is freed. Transport has no such op because stdio piggybacks on stop();
    // the executor has no start/stop, so teardown needs its own slot.
    // (sync backend: no-op; threadpool: stop-workers + join + free pool.)
    void (*destroy_backend)(mcp_context_t *ctx, mcp_executor_t *ex);
} mcp_executor_ops_t;

mcp_executor_t *mcp_executor_create(mcp_context_t *ctx, const mcp_executor_ops_t *ops,
                                    void *backend);
void mcp_executor_destroy(mcp_context_t *ctx, mcp_executor_t *ex);
// Backend pointer from create, for ops callbacks; NULL executor yields NULL.
void *mcp_executor_backend(mcp_context_t *ctx, const mcp_executor_t *ex);
// submit: fn/arg borrowed until wait/destroy drains them. NULL fn -> INVALID_ARGUMENT.
// wait: blocks until all submitted tasks finished. Idempotent.
mcp_status_t mcp_executor_submit(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn,
                                 void *arg);
mcp_status_t mcp_executor_wait(mcp_context_t *ctx, mcp_executor_t *ex);

#endif
