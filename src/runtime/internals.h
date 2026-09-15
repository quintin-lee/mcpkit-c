#ifndef MCPKIT_RUNTIME_INTERNALS_H
#define MCPKIT_RUNTIME_INTERNALS_H

#include <stddef.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/runtime/executor.h"

struct mcp_executor {
    mcp_executor_ops_t ops;
    void *backend;
};

// Shared allocation helpers. NULL ctx falls back to the default allocator;
// destroy with the same ctx used at creation when a counting allocator is
// in play.
void *rt_malloc(mcp_context_t *ctx, size_t n);
void rt_free(mcp_context_t *ctx, void *ptr);

#endif
