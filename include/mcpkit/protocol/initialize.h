// initialize.h — initialize handshake helpers and protocol negotiation.
//
// The MCP protocol requires an initialize / initialized notification
// exchange before any other method is accepted. This module provides
// builders for the "initialize" request params and result, a validator
// for client-supplied params, and a version-negotiation table.

#ifndef MCPKIT_PROTOCOL_INITIALIZE_H
#define MCPKIT_PROTOCOL_INITIALIZE_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;

/**
 * The highest protocol version this library supports. Used as the
 * default when a client does not specify a version in initialize.
 */
#define MCP_PROTOCOL_VERSION_LATEST "2025-06-18"

/**
 * Builds the "params" JSON object for an initialize request using the
 * LATEST protocol version. `client_name` and `client_version` may be
 * NULL (omitted from the result). The returned value is owned by the
 * caller; free with mcp_json_destroy(ctx, params).
 */
mcp_json_value_t *mcp_initialize_params_new(mcp_context_t *ctx, const char *client_name,
                                             const char *client_version);

/**
 * Builds initialize "params" with an explicit `protocol_version` string.
 * `client_name` / `client_version` may be NULL. Returns NULL on NOMEM.
 */
mcp_json_value_t *mcp_initialize_params_new_v(mcp_context_t *ctx,
                                              const char *protocol_version,
                                              const char *client_name,
                                              const char *client_version);

/**
 * Validates a client-supplied initialize "params" JSON object.
 * Checks that all required fields are present and of the correct type.
 * Returns MCP_OK on success, MCP_ERR_INVALID_ARGUMENT on failure.
 */
mcp_status_t mcp_initialize_params_validate(mcp_context_t *ctx,
                                            const mcp_json_value_t *params);

/**
 * Returns the protocol version string to advertise in the initialize
 * result. Given a `client_version` string (may be NULL), returns
 * `client_version` if it is a known supported version, otherwise
 * MCP_PROTOCOL_VERSION_LATEST. The returned pointer is to a static
 * string (no ownership transfer, no allocation).
 */
const char *mcp_protocol_negotiate(mcp_context_t *ctx, const char *client_version);

/**
 * Builds the "result" JSON object for an initialize response.
 * `server_name` and `server_version` are required (non-NULL).
 * Returns an owned JSON value; free with mcp_json_destroy.
 */
mcp_json_value_t *mcp_initialize_result_new(mcp_context_t *ctx, const char *server_name,
                                             const char *server_version);

/**
 * Creates the "notifications/initialized" notification message.
 * This is the client's acknowledgment that initialization is complete.
 * The returned message is owned by the caller; free with
 * mcp_message_destroy(ctx, msg).
 */
mcp_message_t *mcp_initialized_notification_new(mcp_context_t *ctx);

#endif
