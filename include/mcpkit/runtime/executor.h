/**
 * @brief Executor backend operation table.
 *
 * All three slots are required; a NULL ops pointer or any NULL op
 * callback is rejected by the dispatcher.
 *
 * - submit: enqueues (or runs) a task. fn/arg are borrowed until
 *   wait/destroy drains pending work. NULL fn yields INVALID_ARGUMENT.
 * - wait: blocks until all submitted tasks have finished. Idempotent.
 * - destroy_backend: backend teardown, called once by mcp_executor_destroy
 *   before the shell is freed. The executor has no start/stop to
 *   piggyback on (unlike transport), so teardown needs its own slot.
 *   (sync backend: no-op; threadpool: stop-workers + join + free pool.)
 */

/**
 * @brief Executor shell + backend ops table (sync and threadpool both fit).
 *
 * The shell stores an opaque backend pointer plus the ops table. NULL ops
 * or any NULL op yields an UNSUPPORTED dispatcher return; NULL args to
 * submit are INVALID_ARGUMENT.
 */
#ifndef MCPKIT_RUNTIME_EXECUTOR_H
#define MCPKIT_RUNTIME_EXECUTOR_H

#include "mcpkit/core/error.h"
#include "mcpkit/runtime/task.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

typedef struct mcp_executor_ops {
    mcp_status_t (*submit)(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn, void *arg);
    mcp_status_t (*wait)(mcp_context_t *ctx, mcp_executor_t *ex);
    void (*destroy_backend)(mcp_context_t *ctx, mcp_executor_t *ex);
} mcp_executor_ops_t;

/**
 * @brief Creates an executor wrapper around an ops table and a backend
 *        pointer.
 * @param ctx Context; may be NULL (default allocator).
 * @param ops Required ops table; NULL or incomplete ops yields NULL.
 * @param backend Opaque backend pointer; stored, not owned.
 * @return Owned mcp_executor_t, or NULL on OOM or NULL/incomplete ops.
 */
mcp_executor_t *mcp_executor_create(mcp_context_t *ctx, const mcp_executor_ops_t *ops,
                                    void *backend);

/**
 * @brief Destroys an executor, invoking destroy_backend first.
 * @param ctx Context; may be NULL.
 * @param ex Executor to destroy; NULL is a no-op.
 */
void mcp_executor_destroy(mcp_context_t *ctx, mcp_executor_t *ex);

/**
 * @brief Returns the backend pointer passed to mcp_executor_create.
 *
 * Used by ops callbacks to access their own state.
 *
 * @param ctx Context; may be NULL.
 * @param ex Executor handle; NULL yields NULL.
 * @return The stored backend pointer.
 */
void *mcp_executor_backend(mcp_context_t *ctx, const mcp_executor_t *ex);

/**
 * @brief Submits a task to the executor.
 *
 * fn/arg are borrowed until mcp_executor_wait or mcp_executor_destroy
 * drains pending work; the executor does not free them.
 *
 * @param ctx Context; may be NULL.
 * @param ex Target executor.
 * @param fn Task callback; NULL yields MCP_ERR_INVALID_ARGUMENT.
 * @param arg Opaque pointer forwarded to fn.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if fn is NULL;
 *         MCP_ERR_UNSUPPORTED if the backend's submit op is NULL.
 */
mcp_status_t mcp_executor_submit(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn,
                                 void *arg);

/**
 * @brief Blocks until all submitted tasks have finished.
 *
 * Idempotent: calling wait multiple times is safe.
 *
 * @param ctx Context; may be NULL.
 * @param ex Target executor.
 * @return MCP_OK on success; MCP_ERR_UNSUPPORTED if the backend's
 *         wait op is NULL.
 */
mcp_status_t mcp_executor_wait(mcp_context_t *ctx, mcp_executor_t *ex);

#endif
