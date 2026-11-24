/**
 * @file resource.h
 * @brief Resource descriptor: URI, name, MIME type, a read handler, and an
 *        optional cleanup callback.
 * @ingroup mcpkit-server
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

#include <stdbool.h>
#include <stdint.h>
#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_resource_template mcp_resource_template_t;

/**
 * @brief Resource template read-handler callback signature.
 *
 * Invoked when an incoming resources/read URI matches the template pattern.
 *
 * @param ctx Context for allocation.
 * @param session Current session (borrowed; not owned).
 * @param uri The concrete request URI (borrowed).
 * @param vars Borrowed JSON object containing extracted variables (e.g. {"path": "docs/readme.md"}).
 * @param user_data Opaque pointer passed to mcp_resource_template_new.
 * @param contents_out On MCP_OK, must be set to an owned JSON array. The
 *                     dispatcher destroys it after building the response.
 * @return MCP_OK on success; any other MCP_ERR_* on handler failure.
 */
typedef mcp_status_t (*mcp_resource_template_read_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                                      const char *uri,
                                                      const mcp_json_value_t *vars,
                                                      void *user_data,
                                                      mcp_json_value_t **contents_out);

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

/**
 * @brief Creates a resource template descriptor.
 *
 * @param ctx Context; may be NULL.
 * @param uri_template RFC 6570 URI template (e.g. "file:///{path}"); strdup'd internally.
 * @param name Display name; strdup'd internally; must not be NULL.
 * @param description_or_null Optional description; strdup'd internally; may be NULL.
 * @param mime_type_or_null Optional MIME type; strdup'd internally; may be NULL.
 * @param on_read Read callback invoked when a concrete URI matches uri_template.
 * @param user_data Opaque pointer forwarded to handler.
 * @return Owned mcp_resource_template_t, or NULL on error/OOM.
 */
mcp_resource_template_t *mcp_resource_template_new(mcp_context_t *ctx,
                                                   const char *uri_template,
                                                   const char *name,
                                                   const char *description_or_null,
                                                   const char *mime_type_or_null,
                                                   mcp_resource_template_read_fn on_read,
                                                   void *user_data);

/**
 * @brief Destroys a resource template.
 * @param ctx Context; may be NULL.
 * @param tmpl Resource template to destroy; NULL is a no-op.
 */
void mcp_resource_template_destroy(mcp_context_t *ctx, mcp_resource_template_t *tmpl);

/**
 * @brief Sets the description of a resource.
 *
 * @param ctx Context; may be NULL.
 * @param res Target resource.
 * @param description_or_null Description text (strdup'd internally; may be NULL to clear).
 * @return MCP_OK on success; error code on failure.
 */
mcp_status_t mcp_resource_set_description(mcp_context_t *ctx, mcp_resource_t *res,
                                          const char *description_or_null);

/**
 * @brief Gets the description of a resource.
 */
const char *mcp_resource_get_description(const mcp_resource_t *res);

/**
 * @brief Sets the size (in bytes) of a resource.
 *
 * @param res Target resource.
 * @param size Size in bytes.
 * @return MCP_OK on success; error code on failure.
 */
mcp_status_t mcp_resource_set_size(mcp_resource_t *res, int64_t size);

/**
 * @brief Gets the size of a resource if set.
 *
 * @param res Target resource.
 * @param size_out Pointer to receive size in bytes.
 * @return true if size was set, false otherwise.
 */
bool mcp_resource_get_size(const mcp_resource_t *res, int64_t *size_out);

/**
 * @brief Sets the annotations object of a resource.
 *
 * @param ctx Context; may be NULL.
 * @param res Target resource.
 * @param annotations JSON object containing annotations (cloned internally; NULL to clear).
 * @return MCP_OK on success; error code on failure.
 */
mcp_status_t mcp_resource_set_annotations(mcp_context_t *ctx, mcp_resource_t *res,
                                          const mcp_json_value_t *annotations);

/**
 * @brief Gets the annotations object of a resource (borrowed).
 */
const mcp_json_value_t *mcp_resource_get_annotations(const mcp_resource_t *res);

/**
 * @brief Sets the annotations object of a resource template.
 *
 * @param ctx Context; may be NULL.
 * @param tmpl Target resource template.
 * @param annotations JSON object containing annotations (cloned internally; NULL to clear).
 * @return MCP_OK on success; error code on failure.
 */
mcp_status_t mcp_resource_template_set_annotations(mcp_context_t *ctx,
                                                   mcp_resource_template_t *tmpl,
                                                   const mcp_json_value_t *annotations);

/**
 * @brief Gets the annotations object of a resource template (borrowed).
 */
const mcp_json_value_t *mcp_resource_template_get_annotations(const mcp_resource_template_t *tmpl);

#endif
