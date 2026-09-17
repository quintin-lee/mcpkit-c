#ifndef MCPKIT_CLIENT_CLIENT_H
#define MCPKIT_CLIENT_CLIENT_H

#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

/**
 * @file client.h
 * High-level JSON-RPC client over a transport.
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

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

typedef struct mcp_client mcp_client_t;

mcp_client_t *mcp_client_create(mcp_context_t *ctx, mcp_transport_t *transport);
void mcp_client_destroy(mcp_context_t *ctx, mcp_client_t *client);

/**
 * Sends transport start. Returns the transport's status code.
 */
mcp_status_t mcp_client_connect(mcp_context_t *ctx, mcp_client_t *client);
mcp_status_t mcp_client_disconnect(mcp_context_t *ctx, mcp_client_t *client);

/**
 * Full MCP handshake: sends initialize, validates response id, stores
 * the negotiated protocol version, and fires notifications/initialized
 * (fire-and-forget; send status is returned but the notification
 * itself has no response).
 *
 * client_name and client_version are copied; the caller retains them.
 * server_info_out is optional (NULL to skip).
 */
mcp_status_t mcp_client_initialize(mcp_context_t *ctx, mcp_client_t *client,
                                    const char *client_name, const char *client_version,
                                    mcp_json_value_t **server_info_out);

/**
 * Returns the negotiated protocol version string (BORROWED), or NULL if
 * mcp_client_initialize has not yet succeeded.
 */
const char *mcp_client_protocol_version(mcp_context_t *ctx, const mcp_client_t *client);

/**
 * Generic JSON-RPC request/response round-trip.
 *
 * params is consumed on both success and failure (the client destroys it
 * after building the request). result_out may be NULL to discard the
 * result. For error responses the mcp_status_t is mapped from the RPC
 * error code via mcp_rpc_code_to_status.
 */
mcp_status_t mcp_client_request(mcp_context_t *ctx, mcp_client_t *client,
                                const char *method, mcp_json_value_t *params,
                                mcp_json_value_t **result_out);

/**
 * Convenience wrappers. Each consumes its args (JSON value) on all paths
 * and sets *result_out to a caller-owned clone on MCP_OK.
 */
mcp_status_t mcp_client_ping(mcp_context_t *ctx, mcp_client_t *client);
mcp_status_t mcp_client_list_tools(mcp_context_t *ctx, mcp_client_t *client,
                                    mcp_json_value_t **tools_out);
mcp_status_t mcp_client_call_tool(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *name, mcp_json_value_t *args,
                                   mcp_json_value_t **result_out);
mcp_status_t mcp_client_read_resource(mcp_context_t *ctx, mcp_client_t *client,
                                       const char *uri, mcp_json_value_t **result_out);
mcp_status_t mcp_client_get_prompt(mcp_context_t *ctx, mcp_client_t *client,
                                    const char *name, mcp_json_value_t *args,
                                    mcp_json_value_t **result_out);

/**
 * completion/complete. ref is a string such as "prompt/argName".
 * args is the "argument" object (consumed on all paths).
 * *result_out receives a cloned completions array on MCP_OK.
 */
mcp_status_t mcp_client_complete(mcp_context_t *ctx, mcp_client_t *client,
                                  const char *ref, mcp_json_value_t *args,
                                  mcp_json_value_t **result_out);

#endif
