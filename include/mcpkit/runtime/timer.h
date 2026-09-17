#ifndef MCPKIT_RUNTIME_TIMER_H
#define MCPKIT_RUNTIME_TIMER_H

#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/runtime/task.h"

/**
 * @file timer.h
 * Sorted singly-linked timer list with CLOCK_MONOTONIC deadlines.
 *
 * - mcp_timer_schedule(): delay_ms == 0 fires on the next poll.
 *   FIFO ordering is preserved for equal deadlines.
 * - mcp_timer_poll(): runs all due callbacks INLINE (synchronously)
 *   before returning; reentrancy-safe (entries are unlinked before
 *   the callback is invoked).
 * - mcp_timer_cancel(): removes all pending entries matching (fn, arg);
 *   returns the count cancelled.
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_timer mcp_timer_t;

mcp_timer_t *mcp_timer_create(mcp_context_t *ctx);
void mcp_timer_destroy(mcp_context_t *ctx, mcp_timer_t *t);

/**
 * Schedules fn to fire after delay_ms (CLOCK_MONOTONIC).
 * MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if args are NULL;
 * MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_timer_schedule(mcp_context_t *ctx, mcp_timer_t *t,
                                 uint64_t delay_ms, mcp_task_fn fn, void *arg);

/**
 * Cancels all pending entries for (fn, arg). Returns the count cancelled.
 */
size_t mcp_timer_cancel(mcp_context_t *ctx, mcp_timer_t *t, mcp_task_fn fn, void *arg);

/**
 * Runs all due callbacks inline. *fired_out (optional) receives the
 * count of callbacks that fired.
 */
mcp_status_t mcp_timer_poll(mcp_context_t *ctx, mcp_timer_t *t, size_t *fired_out);

#endif
