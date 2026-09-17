#ifndef MCPKIT_CORE_CONTEXT_H
#define MCPKIT_CORE_CONTEXT_H

#include "mcpkit/core/types.h"
#include "mcpkit/logging/logger.h"

typedef struct mcp_json_backend_ops mcp_json_backend_ops_t;

/**
 * @brief Configuration for `mcp_context_create`.
 *
 * All fields are optional; any NULL field is resolved to a built-in
 * default (libc allocator, stderr logger, built-in JSON backend).
 *
 * @note `logger` - if set, the context does NOT own it; the caller
 *       must keep it alive at least as long as the context.
 * @note `json_backend` - NULL means use the built-in DOM.
 */
typedef struct {
    const mcp_allocator_t *allocator;
    mcp_logger_t *logger;
    const mcp_json_backend_ops_t *json_backend;
} mcp_context_config_t;

/**
 * @brief Creates a context.
 *
 * On success the caller owns it and must release it with
 * `mcp_context_destroy`.
 *
 * @param config  Configuration; may be NULL to use all defaults.
 * @return Owned context on success; NULL on allocation failure.
 */
mcp_context_t *mcp_context_create(const mcp_context_config_t *config);

/**
 * @brief Destroys a context. NULL-safe.
 *
 * @param ctx  Context to destroy, or NULL.
 */
void mcp_context_destroy(mcp_context_t *ctx);

/**
 * @brief Returns the allocator bound to `ctx`.
 *
 * @param ctx  Context; NULL routes to the default libc allocator.
 * @return Borrowed allocator pointer; caller must not free.
 */
const mcp_allocator_t *mcp_context_allocator(mcp_context_t *ctx);

/**
 * @brief Returns the logger bound to `ctx`.
 *
 * The context does not necessarily own the logger; callers must not
 * free the returned pointer.
 *
 * @param ctx  Context; NULL returns NULL.
 * @return Borrowed logger, or NULL if unset.
 */
mcp_logger_t *mcp_context_logger(mcp_context_t *ctx);

/**
 * @brief Returns the JSON backend bound to `ctx`.
 *
 * @param ctx  Context; NULL routes to the built-in backend.
 * @return Borrowed backend ops table, or NULL for built-in.
 */
const mcp_json_backend_ops_t *mcp_context_json_backend(mcp_context_t *ctx);

/* INTERNAL: json layer only. Use mcp_json_set_backend() instead. */
void mcp_context_set_json_backend(mcp_context_t *ctx, const void *ops);

#endif
