#ifndef MCPKIT_CORE_CONTEXT_H
#define MCPKIT_CORE_CONTEXT_H

#include "mcpkit/core/types.h"
#include "mcpkit/logging/logger.h"

typedef struct mcp_json_backend_ops mcp_json_backend_ops_t;

/**
 * Configuration for `mcp_context_create`.
 *
 * All fields are optional; any NULL field is resolved to a built-in
 * default (libc allocator, stderr logger, built-in JSON backend).
 *
 * - `allocator`   - if set, every mcpkit allocation made through a
 *   context built with this config goes through these functions.
 *   NULL entries inside the struct fall back to libc individually.
 * - `logger`      - if set, the context does NOT own it; the caller
 *   must keep it alive at least as long as the context.
 * - `json_backend`- NULL means use the built-in DOM.
 */
typedef struct {
    const mcp_allocator_t *allocator;
    mcp_logger_t *logger;
    const mcp_json_backend_ops_t *json_backend;
} mcp_context_config_t;

/**
 * Creates a context. On success the caller owns it and must release
 * it with `mcp_context_destroy`. Returns NULL on allocation failure.
 * `config` may be NULL (all defaults).
 */
mcp_context_t *mcp_context_create(const mcp_context_config_t *config);

/** Destroys a context; NULL-safe. */
void mcp_context_destroy(mcp_context_t *ctx);

/** Returns the allocator bound to `ctx`; NULL ctx → default libc. */
const mcp_allocator_t *mcp_context_allocator(mcp_context_t *ctx);

/**
 * Returns the logger bound to `ctx`. The context does not necessarily
 * own it; callers must not free the returned pointer.
 */
mcp_logger_t *mcp_context_logger(mcp_context_t *ctx);

/** Returns the JSON backend bound to `ctx`, or NULL for built-in. */
const mcp_json_backend_ops_t *mcp_context_json_backend(mcp_context_t *ctx);

// INTERNAL: json layer only. Use mcp_json_set_backend() instead.
void mcp_context_set_json_backend(mcp_context_t *ctx, const void *ops);

#endif
