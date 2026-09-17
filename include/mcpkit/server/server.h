/**
 * @brief Server container: named tool/resource/prompt registries, session
 *        lifecycle, per-session message dispatch, and a completion-provider
 *        table.
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

#ifndef MCPKIT_SERVER_SERVER_H
#define MCPKIT_SERVER_SERVER_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_prompt mcp_prompt_t;
typedef struct mcp_json_value mcp_json_value_t;

/**
 * @brief Creates a named server.
 *
 * @param ctx Context used for allocation; may be NULL (uses default allocator).
 * @param name Server name; may be NULL.
 * @param version Version string; may be NULL.
 * @return Owned mcp_server_t handle, or NULL on OOM.
 */
mcp_server_t *mcp_server_create(mcp_context_t *ctx, const char *name, const char *version);

/**
 * @brief Destroys a server and all resources it owns (tools, resources,
 * prompts, sessions, completion providers, and its own struct).
 * @param ctx Context; may be NULL.
 * @param server Handle to destroy; NULL is a no-op.
 */
void mcp_server_destroy(mcp_context_t *ctx, mcp_server_t *server);

/**
 * @brief Adds a tool to the server's registry.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param tool Tool to register; server TAKES ownership on MCP_OK.
 *             On any error the caller retains ownership.
 * @return MCP_OK on success; MCP_ERR_ALREADY_EXISTS if a tool with the
 *         same name is already registered.
 */
mcp_status_t mcp_server_add_tool(mcp_context_t *ctx, mcp_server_t *server, mcp_tool_t *tool);

/**
 * @brief Adds a resource to the server's registry.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param res Resource to register; server TAKES ownership on MCP_OK.
 *            On any error the caller retains ownership.
 * @return MCP_OK on success; MCP_ERR_ALREADY_EXISTS if a resource with
 *         the same URI is already registered.
 */
mcp_status_t mcp_server_add_resource(mcp_context_t *ctx, mcp_server_t *server,
                                     mcp_resource_t *res);

/**
 * @brief Adds a prompt to the server's registry.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param prompt Prompt to register; server TAKES ownership on MCP_OK.
 *               On any error the caller retains ownership.
 * @return MCP_OK on success; MCP_ERR_ALREADY_EXISTS if a prompt with the
 *         same name is already registered.
 */
mcp_status_t mcp_server_add_prompt(mcp_context_t *ctx, mcp_server_t *server,
                                   mcp_prompt_t *prompt);

/**
 * @brief Removes and destroys the tool identified by name.
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param name Tool name to remove.
 * @return MCP_OK on success; MCP_ERR_NOT_FOUND if no tool has that name.
 */
mcp_status_t mcp_server_remove_tool(mcp_context_t *ctx, mcp_server_t *server,
                                    const char *name);

/**
 * @brief Removes and destroys the resource identified by URI.
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param uri Resource URI to remove.
 * @return MCP_OK on success; MCP_ERR_NOT_FOUND if no resource has that URI.
 */
mcp_status_t mcp_server_remove_resource(mcp_context_t *ctx, mcp_server_t *server,
                                        const char *uri);

/**
 * @brief Removes and destroys the prompt identified by name.
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param name Prompt name to remove.
 * @return MCP_OK on success; MCP_ERR_NOT_FOUND if no prompt has that name.
 */
mcp_status_t mcp_server_remove_prompt(mcp_context_t *ctx, mcp_server_t *server,
                                      const char *name);

/**
 * @brief Completion-provider callback signature.
 *
 * The provider must return an owned JSON array created via ctx.
 * The dispatcher destroys the array after building the response.
 *
 * @param ctx Context for allocation.
 * @param session Current session (borrowed; not owned).
 * @param ref The completion reference object from the request (borrowed).
 * @param user_data Opaque pointer passed at registration time.
 * @return Owned JSON array of completion strings, or NULL on internal
 *         error (the dispatcher surfaces this as MCP_RPC_INTERNAL_ERROR).
 */
typedef mcp_json_value_t *(*mcp_completion_provider_fn)(mcp_context_t *ctx,
                                                        mcp_session_t *session,
                                                        const mcp_json_value_t *ref,
                                                        void *user_data);

/**
 * @brief Registers a completion provider keyed by ref_prefix.
 *
 * The dispatcher matches a completion/complete request by checking whether
 * the request's ref string starts with ref_prefix. Multiple providers with
 * different prefixes may coexist.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param ref_prefix Prefix string; server strdup's it; caller may free
 *                   immediately.
 * @param fn Provider callback.
 * @param user_data Opaque pointer forwarded to the provider.
 * @return MCP_OK on success; MCP_ERR_ALREADY_EXISTS if the same prefix
 *         is already registered.
 */
mcp_status_t mcp_server_register_completion_provider(mcp_context_t *ctx,
                                                     mcp_server_t *server,
                                                     const char *ref_prefix,
                                                     mcp_completion_provider_fn fn,
                                                     void *user_data);

/**
 * @brief Removes a completion provider by prefix.
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param ref_prefix The exact prefix string used at registration.
 * @return MCP_OK on success; MCP_ERR_NOT_FOUND if no provider has that
 *         prefix.
 */
mcp_status_t mcp_server_remove_completion_provider(mcp_context_t *ctx,
                                                   mcp_server_t *server,
                                                   const char *ref_prefix);

/**
 * @brief Creates an independent session for the server.
 *
 * Each session has its own initialized state, apps-host flag, and
 * permission mask. Sessions do not share state with each other.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @return Owned mcp_session_t, or NULL on OOM.
 *         Caller destroys with mcp_server_destroy_session.
 */
mcp_session_t *mcp_server_create_session(mcp_context_t *ctx, mcp_server_t *server);

/**
 * @brief Destroys a session previously created by mcp_server_create_session.
 * @param ctx Context; may be NULL.
 * @param server Owning server.
 * @param session Session to destroy; NULL is a no-op.
 */
void mcp_server_destroy_session(mcp_context_t *ctx, mcp_server_t *server, mcp_session_t *session);

/**
 * @brief Routes a JSON-RPC request through the full pipeline
 *        (validate, gate, dispatch).
 *
 * On MCP_OK, *resp_out is set to a caller-owned response, or NULL if the
 * request was a notification. On any error *resp_out is set to NULL.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param session Active session.
 * @param req Borrowed request message (not consumed).
 * @param resp_out On MCP_OK, receives a caller-owned response; set to
 *                 NULL before calling for safety.
 * @return MCP_OK for all protocol outcomes (including validation errors,
 *         which produce an error response); MCP_ERR_INVALID_ARGUMENT for
 *         bad caller arguments; MCP_ERR_NOMEM if the response cannot be
 *         built.
 */
mcp_status_t mcp_server_dispatch(mcp_context_t *ctx, mcp_server_t *server,
                                 mcp_session_t *session, const mcp_message_t *req,
                                 mcp_message_t **resp_out);

/**
 * @brief Handles a notification.
 *
 * Sets the session's initialized flag when the notification is
 * "notifications/initialized".
 *
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param session Active session.
 * @param notif Borrowed notification message.
 * @return MCP_OK for all valid notifications; MCP_ERR_INVALID_ARGUMENT if
 *         the message is not a notification.
 */
mcp_status_t mcp_server_notify(mcp_context_t *ctx, mcp_server_t *server,
                               mcp_session_t *session, const mcp_message_t *notif);

#endif
