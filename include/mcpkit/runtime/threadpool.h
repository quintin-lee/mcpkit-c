/**
 * @file threadpool.h
 * @brief C11 fixed-size thread pool executor.
 * @ingroup mcpkit-runtime
 *
 * - thread_count == 0 returns NULL (explicit rejection, not clamping).
 * - Tasks run with the submit-time ctx; the ctx, fn, and arg pointers
 *   must outlive the mcp_executor_wait call that drains them.
 * - mcp_executor_destroy discards still-pending tasks without running
 *   them. Call mcp_executor_wait first if every task must complete.
 */

#ifndef MCPKIT_RUNTIME_THREADPOOL_H
#define MCPKIT_RUNTIME_THREADPOOL_H

#include <stddef.h>


typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

/**
 * @brief Creates a fixed-size C11 thread-pool executor.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param thread_count Number of worker threads. 0 yields NULL
 *                     (explicit rejection).
 * @return Owned mcp_executor_t, or NULL on OOM or thread_count == 0.
 */
mcp_executor_t *mcp_threadpool_create(mcp_context_t *ctx, size_t thread_count);

#endif
