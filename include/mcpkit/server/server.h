#ifndef MCPKIT_SERVER_SERVER_H
#define MCPKIT_SERVER_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"

/**
 * @file server.h
 * Server container: named tool/resource/prompt registries, session
 * lifecycle, per-session message dispatch, and a completion-provider
 * table.
 *
 * Ownership:
 * - mcp_server_create() returns an owned handle; caller destroys with
 *   mcp_server_destroy(ctx, server).
 * - mcp_server_add_tool/resource/prompt(): server TAKES ownership of the
 *   item on MCP_OK. On error the item is NOT destroyed; caller retains it
 *   and must free it with the matching _destroy.
 * - mcp_server_remove_*(): server destroys the removed item internally;
 *   the key (name/uri) is matched against the stored item, not the caller's
 *   pointer.
 * - mcp_server_create_session(): caller owns the session and destroys it
 *   with mcp_server_destroy_session.
 * - mcp_server_dispatch(): on MCP_OK the response is caller-owned; on error
 *   no response is built and *resp_out is left unchanged (set to NULL first
 *   for safety).
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_prompt mcp_prompt_t;
typedef struct mcp_json_value mcp_json_value_t;

/**
 * Creates a named server. Returns NULL only on OOM (ctx may be NULL, in
 * which case the default allocator is used).
 */
mcp_server_t *mcp_server_create(mcp_context_t *ctx, const char *name, const char *version);
void mcp_server_destroy(mcp_context_t *ctx, mcp_server_t *server);

/**
 * Adds an item to the registry. Server owns it on MCP_OK.
 * MCP_ERR_ALREADY_EXISTS if an item with the same key is already registered.
 * On any error the item is NOT destroyed; caller retains ownership.
 */
mcp_status_t mcp_server_add_tool(mcp_context_t *ctx, mcp_server_t *server, mcp_tool_t *tool);
mcp_status_t mcp_server_add_resource(mcp_context_t *ctx, mcp_server_t *server,
                                    mcp_resource_t *res);
mcp_status_t mcp_server_add_prompt(mcp_context_t *ctx, mcp_server_t *server,
                                   mcp_prompt_t *prompt);

/**
 * Removes and destroys the item identified by key. MCP_ERR_NOT_FOUND if
 * the key is not present. ctx may be NULL.
 */
mcp_status_t mcp_server_remove_tool(mcp_context_t *ctx, mcp_server_t *server,
                                    const char *name);
mcp_status_t mcp_server_remove_resource(mcp_context_t *ctx, mcp_server_t *server,
                                        const char *uri);
mcp_status_t mcp_server_remove_prompt(mcp_context_t *ctx, mcp_server_t *server,
                                      const char *name);

/*
 * Completion provider callback. The provider must return an owned JSON array
 * (created via ctx); the dispatcher destroys it after building the response.
 * Returns NULL on internal error, which the dispatcher surfaces as
 * MCP_RPC_INTERNAL_ERROR.
 */
typedef mcp_json_value_t *(*mcp_completion_provider_fn)(mcp_context_t *ctx,
                                                        mcp_session_t *session,
                                                        const mcp_json_value_t *ref,
                                                        void *user_data);

/**
 * Registers a completion provider keyed by ref_prefix. The dispatcher matches
 * a completion/complete request by checking whether the request's ref string
 * starts with ref_prefix. Multiple providers with different prefixes may
 * coexist. MCP_ERR_ALREADY_EXISTS if the same prefix is registered twice.
 */
mcp_status_t mcp_server_register_completion_provider(mcp_context_t *ctx,
                                                     mcp_server_t *server,
                                                     const char *ref_prefix,
                                                     mcp_completion_provider_fn fn,
                                                     void *user_data);
mcp_status_t mcp_server_remove_completion_provider(mcp_context_t *ctx,
                                                   mcp_server_t *server,
                                                   const char *ref_prefix);

/**
 * Creates a session. Caller destroys with mcp_server_destroy_session.
 * Sessions are independent; each has its own initialized state and
 * apps-host/permission flags.
 */
mcp_session_t *mcp_server_create_session(mcp_context_t *ctx, mcp_server_t *server);
void mcp_server_destroy_session(mcp_context_t *ctx, mcp_server_t *server, mcp_session_t *session);

/**
 * Routes a JSON-RPC request through the full pipeline (validate, gate,
 * dispatch). On MCP_OK *resp_out is set to a caller-owned response (or NULL
 * if the request was a notification). On any error *resp_out is set to NULL
 * and the status code is returned.
 */
mcp_status_t mcp_server_dispatch(mcp_context_t *ctx, mcp_server_t *server,
                                 mcp_session_t *session, const mcp_message_t *req,
                                 mcp_message_t **resp_out);

/**
 * Handles a notification. Sets the session's initialized flag when the
 * notification is "notifications/initialized". Returns MCP_OK for all
 * valid notifications; MCP_ERR_INVALID_ARGUMENT if the message is not a
 * notification.
 */
mcp_status_t mcp_server_notify(mcp_context_t *ctx, mcp_server_t *server,
                               mcp_session_t *session, const mcp_message_t *notif);

#endif
