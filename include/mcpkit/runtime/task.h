#ifndef MCPKIT_RUNTIME_TASK_H
#define MCPKIT_RUNTIME_TASK_H

typedef struct mcp_context mcp_context_t;

// Task body. fn/arg must outlive the executor wait/destroy that drains them.
typedef void (*mcp_task_fn)(mcp_context_t *ctx, void *arg);

#endif
