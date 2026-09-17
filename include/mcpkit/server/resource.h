#ifndef MCPKIT_SERVER_RESOURCE_H
#define MCPKIT_SERVER_RESOURCE_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

/**
 * @file resource.h
 * Resource descriptor: URI, name, MIME type, a read handler, and an
 * optional cleanup callback.
 *
 * Ownership:
 * - mcp_resource_new(): caller owns the returned resource.
 * - mcp_resource_destroy(): destroys the resource; if a cleanup callback
 *   was set via mcp_resource_set_cleanup() it is invoked first.
 * - If the resource is already added to a server, remove it from the
 *   server first (mcp_server_remove_resource).
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_resource mcp_resource_t;

/**
 * Read handler. Called during resources/read dispatch.
 * *contents_out must be set to an owned JSON array on MCP_OK; the
 * dispatcher destroys it after building the response.
 */
typedef mcp_status_t (*mcp_resource_read_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                              const char *uri, void *user_data,
                                              mcp_json_value_t **contents_out);

/**
 * Cleanup callback. Invoked exactly once, at the start of
 * mcp_resource_destroy(), before the resource struct itself is freed.
 * May be NULL to skip cleanup.
 */
typedef void (*mcp_resource_cleanup_fn)(mcp_context_t *ctx, void *user_data);

mcp_resource_t *mcp_resource_new(mcp_context_t *ctx, const char *uri, const char *name,
                                  const char *mime_type_or_null, mcp_resource_read_fn on_read,
                                  void *user_data);
void mcp_resource_destroy(mcp_context_t *ctx, mcp_resource_t *res);

/**
 * Sets (or clears, with fn_or_null == NULL) the resource's cleanup
 * callback. MCP_ERR_INVALID_ARGUMENT if ctx is NULL or res is NULL.
 */
mcp_status_t mcp_resource_set_cleanup(mcp_context_t *ctx, mcp_resource_t *res,
                                       mcp_resource_cleanup_fn fn_or_null);

#endif
