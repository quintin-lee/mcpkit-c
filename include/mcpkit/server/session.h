/**
 * @file session.h
 * @brief Per-session state: initialization flag, MCP Apps host flag, and a
 *        32-bit permission mask.
 *
 * Sessions are created by mcp_server_create_session() and destroyed by
 * mcp_server_destroy_session(). All functions take ctx first (NULL
 * allowed — the session itself was created with some ctx; the ctx here
 * is only used for internal bookkeeping and is not used for
 * allocation on these particular paths).
 */

#ifndef MCPKIT_SERVER_SESSION_H
#define MCPKIT_SERVER_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "mcpkit/core/error.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;

/**
 * @brief Checks whether the session has completed initialization.
 *
 * Returns true after the session has received a valid initialize request
 * and the client has sent the "notifications/initialized" notification.
 * Before that, most dispatches are gated with MCP_RPC_INVALID_REQUEST
 * (code -32600).
 *
 * @param ctx Context; may be NULL.
 * @param session Session to check; NULL returns false.
 * @return true if initialized, false otherwise.
 */
bool mcp_session_is_initialized(mcp_context_t *ctx, const mcp_session_t *session);

/**
 * @brief Sets the MCP Apps host flag for the session.
 *
 * When true, the dispatcher filters tools by visibility: tools marked
 * MCP_TOOL_VIS_MODEL are hidden from this session's tools/list and
 * tools/call results.
 *
 * @param ctx Context; may be NULL.
 * @param session Target session.
 * @param apps_host true to mark as an Apps host, false otherwise.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if session is NULL.
 */
mcp_status_t mcp_session_set_apps_host(mcp_context_t *ctx, mcp_session_t *session,
                                       bool apps_host);

/**
 * @brief Checks whether the session is marked as an MCP Apps host.
 * @param ctx Context; may be NULL.
 * @param session Session to check; NULL returns false.
 * @return true if the session is an Apps host.
 */
bool mcp_session_is_apps_host(mcp_context_t *ctx, const mcp_session_t *session);

/**
 * @brief Grants permission bits to the session.
 *
 * The mask uses the same bit layout as mcp_tool_require_perms()
 * (see mcpkit/apps/ui.h). grant(0) is a no-op.
 *
 * @param ctx Context; may be NULL.
 * @param session Target session.
 * @param perm_mask Bits to grant.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if session is NULL.
 */
mcp_status_t mcp_session_grant(mcp_context_t *ctx, mcp_session_t *session,
                               uint32_t perm_mask);

/**
 * @brief Revokes permission bits from the session.
 * @param ctx Context; may be NULL.
 * @param session Target session.
 * @param perm_mask Bits to revoke.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if session is NULL.
 */
mcp_status_t mcp_session_revoke(mcp_context_t *ctx, mcp_session_t *session,
                                uint32_t perm_mask);

/**
 * @brief Checks whether ALL bits in perm_mask are currently granted.
 * A zero mask always returns true (no requirement).
 *
 * @param ctx Context; may be NULL.
 * @param session Session to check; NULL returns true for zero mask, false
 *                otherwise.
 * @param perm_mask Bit mask to test.
 * @return true if all bits are granted.
 */
bool mcp_session_grants(mcp_context_t *ctx, const mcp_session_t *session,
                        uint32_t perm_mask);

#endif
