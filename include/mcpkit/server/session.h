#ifndef MCPKIT_SERVER_SESSION_H
#define MCPKIT_SERVER_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "mcpkit/core/error.h"

/**
 * @file session.h
 * Per-session state: initialization flag, MCP Apps host flag, and a
 * 32-bit permission mask.
 *
 * Sessions are created by mcp_server_create_session() and destroyed by
 * mcp_server_destroy_session(). All functions take ctx first (NULL
 * allowed — the session itself was created with some ctx; the ctx here
 * is only used for internal bookkeeping and is not used for
 * allocation on these particular paths).
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;

/**
 * Returns true after the session has received a valid initialize request
 * and the client has sent the "notifications/initialized" notification.
 * Before that, most dispatches are gated with MCP_RPC_INVALID_REQUEST
 * (code -32600).
 */
bool mcp_session_is_initialized(mcp_context_t *ctx, const mcp_session_t *session);

/**
 * Sets the MCP Apps host flag. When true, the dispatcher filters tools
 * by visibility: tools marked MCP_TOOL_VIS_MODEL are hidden from this
 * session's tools/list and tools/call results.
 */
mcp_status_t mcp_session_set_apps_host(mcp_context_t *ctx, mcp_session_t *session,
                                       bool apps_host);
bool mcp_session_is_apps_host(mcp_context_t *ctx, const mcp_session_t *session);

/**
 * Grants permission bits to the session. The mask uses the same bit
 * layout as mcp_tool_require_perms() (see mcpkit/apps/ui.h).
 * grant(0) is a no-op.
 */
mcp_status_t mcp_session_grant(mcp_context_t *ctx, mcp_session_t *session,
                               uint32_t perm_mask);
mcp_status_t mcp_session_revoke(mcp_context_t *ctx, mcp_session_t *session,
                                uint32_t perm_mask);
/**
 * Returns true if ALL bits in perm_mask are currently granted.
 * A zero mask always returns true (no requirement).
 */
bool mcp_session_grants(mcp_context_t *ctx, const mcp_session_t *session,
                        uint32_t perm_mask);

#endif
