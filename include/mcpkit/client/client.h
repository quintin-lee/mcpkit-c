/**
 * @defgroup mcpkit-client MCP client
 *
 * High-level request/response client. Results are deep-cloned to the
 * caller; mcp_client_list_tools auto-paginates tools/list responses.
 *
 * @{
 */
/**
 * @file client.h
 * @brief High-level JSON-RPC client over a transport.
 * @ingroup mcpkit-client
 *
 * Ownership:
 * - mcp_client_create(): caller owns the client; destroy with
 *   mcp_client_destroy(ctx, client).
 * - mcp_client_request / mcp_client_call_tool / mcp_client_get_prompt /
 *   mcp_client_complete: on MCP_OK the params JSON value is TAKEN into
 *   the request and destroyed after the round-trip; *result_out (if
 *   non-NULL) receives a CLONED result owned by the caller. On error the
 *   params value is still consumed (destroyed) and *result_out is set to
 *   NULL.
 * - mcp_client_initialize: *server_info_out (if non-NULL) receives a
 *   cloned serverInfo value owned by the caller; NULL if absent.
 * - mcp_client_protocol_version(): returns a BORROWED string; valid only
 *   while the client is alive.
 */

#ifndef MCPKIT_CLIENT_CLIENT_H
#define MCPKIT_CLIENT_CLIENT_H

#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_message mcp_message_t;

typedef struct mcp_client mcp_client_t;

/**
 * @brief Creates a client bound to the given transport.
 * @param ctx Context; may be NULL (default allocator).
 * @param transport Transport to use; must outlive the client.
 * @return Owned mcp_client_t, or NULL on OOM.
 */
mcp_client_t *mcp_client_create(mcp_context_t *ctx, mcp_transport_t *transport);

/**
 * @brief Destroys a client.
 * @param ctx Context; may be NULL.
 * @param client Client to destroy; NULL is a no-op.
 */
void mcp_client_destroy(mcp_context_t *ctx, mcp_client_t *client);

/**
 * @brief Starts the underlying transport.
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @return The transport's status code.
 */
mcp_status_t mcp_client_connect(mcp_context_t *ctx, mcp_client_t *client);

/**
 * @brief Stops the underlying transport.
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @return The transport's status code.
 */
mcp_status_t mcp_client_disconnect(mcp_context_t *ctx, mcp_client_t *client);

/**
 * @brief Performs the full MCP initialize handshake.
 *
 * Sends the initialize request, validates the response id, stores the
 * negotiated protocol version, and fires notifications/initialized
 * (fire-and-forget; send status is returned but the notification has
 * no response).
 *
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @param client_name Copied internally; caller may free.
 * @param client_version Copied internally; caller may free.
 * @param server_info_out Optional; receives a caller-owned cloned
 *                         serverInfo JSON value, or NULL if absent.
 * @return MCP_OK on success; MCP_ERR_PROTOCOL on id mismatch.
 */
mcp_status_t mcp_client_initialize(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *client_name, const char *client_version,
                                   mcp_json_value_t **server_info_out);

/**
 * @brief Returns the negotiated protocol version string.
 *
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @return BORROWED string valid only while the client is alive, or NULL
 *         if mcp_client_initialize has not yet succeeded.
 */
const char *mcp_client_protocol_version(mcp_context_t *ctx, const mcp_client_t *client);

/**
 * @brief Generic JSON-RPC request/response round-trip.
 *
 * params is consumed on both success and failure (destroyed after
 * building the request). result_out may be NULL to discard the result.
 * For error responses the status is mapped from the RPC error code via
 * mcp_rpc_code_to_status.
 *
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @param method JSON-RPC method name (e.g. "tools/list"); not owned.
 * @param params Optional params object; consumed (destroyed) on all paths.
 * @param result_out On MCP_OK receives a caller-owned cloned result;
 *                   may be NULL to discard.
 * @return MCP_OK on success; mapped MCP_ERR_* on protocol error.
 */
mcp_status_t mcp_client_request(mcp_context_t *ctx, mcp_client_t *client,
                                const char *method, mcp_json_value_t *params,
                                mcp_json_value_t **result_out);

/**
 * @brief Sends ping (no params, no result).
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_client_ping(mcp_context_t *ctx, mcp_client_t *client);

/**
 * @brief Sends tools/list.
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @param tools_out Receives a caller-owned cloned tools array.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_client_list_tools(mcp_context_t *ctx, mcp_client_t *client,
                                   mcp_json_value_t **tools_out);

/**
 * @brief Sends tools/call with the named tool and args.
 *
 * args is consumed on all paths.
 *
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @param name Tool name; not owned.
 * @param args Optional params object; consumed on all paths.
 * @param result_out Receives a caller-owned cloned result on MCP_OK.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_client_call_tool(mcp_context_t *ctx, mcp_client_t *client,
                                  const char *name, mcp_json_value_t *args,
                                  mcp_json_value_t **result_out);

/**
 * @brief Sends resources/read for the given URI.
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @param uri Resource URI; not owned.
 * @param result_out Receives a caller-owned cloned result on MCP_OK.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_client_read_resource(mcp_context_t *ctx, mcp_client_t *client,
                                      const char *uri, mcp_json_value_t **result_out);

/**
 * @brief Sends prompts/get with the named prompt and args.
 *
 * args is consumed on all paths.
 *
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @param name Prompt name; not owned.
 * @param args Optional params object; consumed on all paths.
 * @param result_out Receives a caller-owned cloned result on MCP_OK.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_client_get_prompt(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *name, mcp_json_value_t *args,
                                   mcp_json_value_t **result_out);

/**
 * @brief Sends completion/complete.
 *
 * ref is a string such as "prompt/argName"; args is the "argument"
 * object, consumed on all paths. *result_out receives a cloned
 * completions array on MCP_OK.
 *
 * @param ctx Context; may be NULL.
 * @param client Target client.
 * @param ref Completion reference string; not owned.
 * @param args Optional argument object; consumed on all paths.
 * @param result_out Receives a caller-owned cloned completions array.
 * @return MCP_OK on success.
 */
mcp_status_t mcp_client_complete(mcp_context_t *ctx, mcp_client_t *client,
                                 const char *ref, mcp_json_value_t *args,
                                 mcp_json_value_t **result_out);

/**
 * @brief Host-injected callback that responds to a roots/list request
 *        sent by the server to the client.
 *
 * The callback receives ctx and user_data.  It returns an owned JSON
 * array of Root objects ([{uri, name?}, …]) that the client will send
 * as the result of the roots/list request.  Return NULL if the host
 * does not support roots; the client will reply with a -32601 error.
 */
typedef mcp_json_value_t *(*mcp_client_roots_fn)(mcp_context_t *ctx, void *user_data);

/**
 * @brief Registers the host's roots provider on the client.
 *
 * fn is stored on the client; user_data is passed back to fn on each
 * invocation.  A NULL fn clears the provider (client replies -32601 to
 * any roots/list request).
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param c Target client; must not be NULL.
 * @param fn Provider callback, or NULL to clear.
 * @param user_data Opaque pointer passed to fn on each call.
 */
void mcp_client_set_roots_provider(mcp_context_t *ctx, mcp_client_t *c,
                                   mcp_client_roots_fn fn, void *user_data);

/**
 * @brief Host-injected callback that responds to a sampling/createMessage
 *        request sent by the server to the client.
 *
 * The callback receives the request parameters (messages, modelPreferences,
 * systemPrompt, maxTokens) as a single JSON object and returns an owned
 * result JSON object conforming to the spec's sampling response shape
 * ({role, content, model, stopReason}).  Return NULL if sampling is not
 * supported; the client will reply with a -32601 error.
 */
typedef mcp_json_value_t *(*mcp_client_sample_fn)(mcp_context_t *ctx,
                                                   const mcp_json_value_t *params,
                                                   void *user_data);

/**
 * @brief Registers the host's sampling provider on the client.
 *
 * fn is stored on the client; user_data is passed back to fn on each
 * invocation.  A NULL fn clears the provider (client replies -32601 to
 * any sampling/createMessage request).
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param c Target client; must not be NULL.
 * @param fn Provider callback, or NULL to clear.
 * @param user_data Opaque pointer passed to fn on each call.
 */
void mcp_client_set_sample_provider(mcp_context_t *ctx, mcp_client_t *c,
                                    mcp_client_sample_fn fn, void *user_data);

/**
 * @brief Host-injected callback that responds to an elicitation/create
 *        request sent by the server to the client.
 *
 * The callback receives the request parameters (message, requestedSchema)
 * as a single JSON object and returns an owned result JSON object
 * containing the user-supplied data.  Return NULL if elicitation is not
 * supported; the client will reply with a -32601 error.
 */
typedef mcp_json_value_t *(*mcp_client_elicitation_fn)(mcp_context_t *ctx,
                                                       const mcp_json_value_t *params,
                                                       void *user_data);

/**
 * @brief Registers the host's elicitation provider on the client.
 *
 * fn is stored on the client; user_data is passed back to fn on each
 * invocation.  A NULL fn clears the provider (client replies -32601 to
 * any elicitation/create request).
 *
 * @param ctx Context; may be NULL (default allocator).
 * @param c Target client; must not be NULL.
 * @param fn Provider callback, or NULL to clear.
 * @param user_data Opaque pointer passed to fn on each call.
 */
void mcp_client_set_elicitation_provider(mcp_context_t *ctx, mcp_client_t *c,
                                         mcp_client_elicitation_fn fn, void *user_data);

/**
 * @brief Routes an incoming server-originated JSON-RPC request to the
 *        registered provider and builds the response to send back.
 *
 * Recognized methods: roots/list, sampling/createMessage, elicitation/create.
 *
 * If no provider is registered (or the provider returns NULL), the
 * response is an mcp_response_err_new with MCP_RPC_METHOD_NOT_FOUND
 * (-32601).  For roots/list a NULL provider means "roots not supported";
 * the error message is chosen accordingly.
 *
 * The caller does NOT own `req`; it is only read.  On MCP_OK the caller
 * owns *resp_out (an mcp_message_t) and must serialize + send it, then
 * destroy it with mcp_message_destroy.
 *
 * @param ctx        Context; may be NULL.
 * @param c          Target client; NULL returns INVALID_ARGUMENT.
 * @param req        Parsed server-originated request (MCP_MSG_REQUEST kind).
 * @param resp_out   Receives the caller-owned response message.
 * @return MCP_OK on success (even when the response is a -32601 error
 *         — the host should still send it); INVALID_ARGUMENT if c or
 *         req is NULL; NOMEM on allocation failure.
 */
mcp_status_t mcp_client_handle_server_request(mcp_context_t *ctx, mcp_client_t *c,
                                              const mcp_message_t *req,
                                              mcp_message_t **resp_out);

/** @} */

#endif
