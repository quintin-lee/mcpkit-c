#ifndef MCPKIT_RUNTIME_THREADPOOL_H
#define MCPKIT_RUNTIME_THREADPOOL_H

#include <stddef.h>

typedef struct mcp_context mcp_context_t;
typedef struct mcp_executor mcp_executor_t;

// Fixed worker pool (C11 threads, no pthread types in the ABI).
// thread_count==0 returns NULL (explicitness over clamping). Tasks run with
// the submit-time ctx; ctx/fn/arg must outlive the wait that drains them.
// destroy discards still-pending tasks without running them: call
// mcp_executor_wait first when every task must run.
mcp_executor_t *mcp_threadpool_create(mcp_context_t *ctx, size_t thread_count);

#endif
