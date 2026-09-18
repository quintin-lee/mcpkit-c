/**
 * @file timer.h
 * @brief Sorted singly-linked timer list with CLOCK_MONOTONIC deadlines.
 *
 * - mcp_timer_schedule(): delay_ms == 0 fires on the next poll.
 *   FIFO ordering is preserved for equal deadlines.
 * - mcp_timer_poll(): runs all due callbacks INLINE (synchronously)
 *   before returning; reentrancy-safe (entries are unlinked before
 *   the callback is invoked).
 * - mcp_timer_cancel(): removes all pending entries matching (fn, arg);
 *   returns the count cancelled.
 */

#ifndef MCPKIT_RUNTIME_TIMER_H
#define MCPKIT_RUNTIME_TIMER_H

#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/runtime/task.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_timer mcp_timer_t;

/**
 * @brief Creates an empty timer list.
 * @param ctx Context; may be NULL (default allocator).
 * @return Owned mcp_timer_t, or NULL on OOM.
 */
mcp_timer_t *mcp_timer_create(mcp_context_t *ctx);

/**
 * @brief Destroys a timer list and all its pending entries.
 * @param ctx Context; may be NULL.
 * @param t Timer to destroy; NULL is a no-op.
 */
void mcp_timer_destroy(mcp_context_t *ctx, mcp_timer_t *t);

/**
 * @brief Schedules fn to fire after delay_ms (CLOCK_MONOTONIC).
 *
 * delay_ms == 0 fires on the next poll. FIFO ordering is preserved
 * for equal deadlines.
 *
 * @param ctx Context; may be NULL.
 * @param t Target timer.
 * @param delay_ms Delay in milliseconds.
 * @param fn Task callback to invoke when due.
 * @param arg Opaque pointer forwarded to fn.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if t, fn, or
 *         arg are NULL (arg may be NULL — only fn is required);
 *         MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_timer_schedule(mcp_context_t *ctx, mcp_timer_t *t,
                                uint64_t delay_ms, mcp_task_fn fn, void *arg);

/**
 * @brief Cancels all pending entries for (fn, arg).
 *
 * @param ctx Context; may be NULL.
 * @param t Target timer.
 * @param fn Task callback to match.
 * @param arg Opaque pointer to match.
 * @return Number of entries cancelled.
 */
size_t mcp_timer_cancel(mcp_context_t *ctx, mcp_timer_t *t, mcp_task_fn fn, void *arg);

/**
 * @brief Runs all due callbacks inline.
 *
 * Reentrancy-safe: entries are unlinked before the callback is invoked,
 * so a callback may schedule new timers on the same list.
 *
 * @param ctx Context; may be NULL.
 * @param t Target timer.
 * @param fired_out Optional; receives the count of callbacks that fired.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_timer_poll(mcp_context_t *ctx, mcp_timer_t *t, size_t *fired_out);

#endif
