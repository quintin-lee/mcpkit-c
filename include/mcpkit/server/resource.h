/**
 * @brief Resource descriptor: URI, name, MIME type, a read handler, and an
 *        optional cleanup callback.
 *
 * Ownership:
 * - mcp_resource_new(): caller owns the returned resource.
 * - mcp_resource_destroy(): destroys the resource; if a cleanup callback
 *   was set via mcp_resource_set_cleanup() it is invoked first.
 * - If the resource is already added to a server, remove it from the
 *   server first (mcp_server_remove_resource).
 */

#ifndef MCPKIT_SERVER_RESOURCE_H
#define MCPKIT_SERVER_RESOURCE_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_resource mcp_resource_t;

/**
 * @brief Resource read-handler callback signature.
 *
 * @param ctx Context for allocation.
 * @param session Current session (borrowed; not owned).
 * @param uri The resource URI from the request (borrowed).
 * @param user_data Opaque pointer passed to mcp_resource_new.
 * @param contents_out On MCP_OK, must be set to an owned JSON array. The
 *                     dispatcher destroys it after building the response.
 * @return MCP_OK on success; any other MCP_ERR_* on handler failure.
 */
typedef mcp_status_t (*mcp_resource_read_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                             const char *uri, void *user_data,
                                             mcp_json_value_t **contents_out);

/**
 * @brief Cleanup callback signature.
 *
 * Invoked exactly once, at the start of mcp_resource_destroy(), before the
 * resource struct itself is freed.
 *
 * @param ctx Context; may be NULL.
 * @param user_data Opaque pointer passed to mcp_resource_new.
 */
typedef void (*mcp_resource_cleanup_fn)(mcp_context_t *ctx, void *user_data);

/**
 * @brief Creates a resource descriptor.
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param uri Resource URI; strdup'd internally; caller may free.
 * @param name Optional display name; strdup'd internally; may be NULL.
 * @param mime_type_or_null Optional MIME type; strdup'd internally; may be NULL.
 * @param on_read Read handler.
 * @param user_data Opaque pointer forwarded to the handler.
 * @return Owned mcp_resource_t, or NULL on OOM.
 *         Caller destroys with mcp_resource_destroy (or
 *         mcp_server_remove_resource if already registered).
 */
mcp_resource_t *mcp_resource_new(mcp_context_t *ctx, const char *uri, const char *name,
                                 const char *mime_type_or_null, mcp_resource_read_fn on_read,
                                 void *user_data);

/**
 * @brief Destroys a resource, invoking its cleanup callback first (if set).
 * @param ctx Context; may be NULL.
 * @param res Resource to destroy; NULL is a no-op.
 */
void mcp_resource_destroy(mcp_context_t *ctx, mcp_resource_t *res);

/**
 * @brief Sets (or clears, with fn_or_null == NULL) the resource's cleanup
 *        callback.
 * @param ctx Context; may be NULL.
 * @param res Target resource.
 * @param fn_or_null New cleanup callback, or NULL to clear.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if ctx is NULL or
 *         res is NULL.
 */
mcp_status_t mcp_resource_set_cleanup(mcp_context_t *ctx, mcp_resource_t *res,
                                      mcp_resource_cleanup_fn fn_or_null);

#endif
