/**
 * @file initialize.h
 * Initialize handshake helpers and protocol negotiation.
 *
 * The MCP protocol requires an initialize / initialized notification
 * exchange before any other method is accepted. This module provides
 * builders for the "initialize" request params and result, a validator
 * for client-supplied params, and a version-negotiation table.
 *
 * @ingroup mcpkit-protocol
 * @see mcpkit-server (`mcp_server_dispatch` gates on the initialized flag)
 */

#ifndef MCPKIT_PROTOCOL_INITIALIZE_H
#define MCPKIT_PROTOCOL_INITIALIZE_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;

/**
 * @brief The highest protocol version this library supports.
 *
 * Used as the default when a client does not specify a version in
 * initialize.
 */
#define MCP_PROTOCOL_VERSION_LATEST "2025-06-18"

/**
 * @brief Builds the "params" JSON object for an initialize request using
 * the LATEST protocol version.
 *
 * @param ctx             Context; NULL uses the default allocator.
 * @param client_name     Client name; may be NULL (omitted from result).
 * @param client_version  Client version; may be NULL (omitted from result).
 * @return Caller-owned params value; free with mcp_json_destroy(ctx, params).
 */
mcp_json_value_t *mcp_initialize_params_new(mcp_context_t *ctx, const char *client_name,
                                              const char *client_version);

/**
 * @brief Builds initialize "params" with an explicit `protocol_version` string.
 *
 * @param ctx             Context; NULL uses the default allocator.
 * @param protocol_version  The protocol version to advertise.
 * @param client_name     Client name; may be NULL.
 * @param client_version  Client version; may be NULL.
 * @return Caller-owned params value, or NULL on NOMEM.
 */
mcp_json_value_t *mcp_initialize_params_new_v(mcp_context_t *ctx,
                                               const char *protocol_version,
                                               const char *client_name,
                                               const char *client_version);

/**
 * @brief Validates a client-supplied initialize "params" JSON object.
 *
 * Checks that all required fields are present and of the correct type.
 *
 * @param ctx     Context.
 * @param params  Params object to validate.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT on failure.
 */
mcp_status_t mcp_initialize_params_validate(mcp_context_t *ctx,
                                             const mcp_json_value_t *params);

/**
 * @brief Returns the protocol version string to advertise in the
 * initialize result.
 *
 * Given a `client_version` string (may be NULL), returns `client_version`
 * if it is a known supported version, otherwise MCP_PROTOCOL_VERSION_LATEST.
 *
 * @param ctx             Context (unused; kept for API consistency).
 * @param client_version  Client-supplied version string, or NULL.
 * @return Borrowed static string pointer (no allocation, no ownership
 *         transfer); NULL if `client_version` is NULL and no default.
 */
const char *mcp_protocol_negotiate(mcp_context_t *ctx, const char *client_version);

/**
 * @brief Builds the "result" JSON object for an initialize response.
 *
 * @param ctx             Context; NULL uses the default allocator.
 * @param server_name     Server name (must be non-NULL).
 * @param server_version  Server version (must be non-NULL).
 * @return Caller-owned result value; free with mcp_json_destroy.
 */
mcp_json_value_t *mcp_initialize_result_new(mcp_context_t *ctx, const char *server_name,
                                              const char *server_version);

/**
 * @brief Creates the "notifications/initialized" notification message.
 *
 * This is the client's acknowledgment that initialization is complete.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned message; free with mcp_message_destroy(ctx, msg).
 */
mcp_message_t *mcp_initialized_notification_new(mcp_context_t *ctx);

#endif
