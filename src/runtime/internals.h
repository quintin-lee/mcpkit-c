/**
 * @file internals.h
 *
 * Internal layout of the runtime executor: the executor ops table and the
 * backend handle it wraps. Shared across executor backends so that sync
 * and threadpool can swap implementations without exposing their private
 * state.
 *
 * - rt_malloc/rt_free: ctx-routed allocation helpers; NULL ctx falls
 *   back to the default allocator.
 */
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
