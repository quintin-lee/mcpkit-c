/**
 * @file session.h
 * @brief Per-session state: initialization flag, MCP Apps host flag, and a
 *        32-bit permission mask.
 * @ingroup mcpkit-server
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

typedef struct mcp_json_value mcp_json_value_t;
typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_message mcp_message_t;

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

/**
 * @brief Returns the `_meta` object stored on the session from the
 *        initialize request, or NULL if the client did not send one.
 *
 * The returned pointer is BORROWED; valid while the session is alive.
 *
 * @param ctx     Context.
 * @param session Session to inspect; NULL returns NULL.
 * @return Borrowed `_meta` value, or NULL.
 */
const mcp_json_value_t *mcp_session_client_meta(mcp_context_t *ctx,
                                                 const mcp_session_t *session);

/**
 * @brief Checks whether the session has an active subscription stream.
 *
 * @param ctx     Context; may be NULL.
 * @param session Session to check; NULL returns false.
 * @return true if an active subscription stream is open, false otherwise.
 */
bool mcp_session_has_active_subscription(mcp_context_t *ctx, const mcp_session_t *session);

/**
 * @brief Checks whether a notification matches the session's active subscription filter.
 *
 * If the session has NO active subscription, returns true (unfiltered).
 * If the session HAS an active subscription, checks against:
 *   - toolsListChanged for "notifications/tools/list_changed"
 *   - promptsListChanged for "notifications/prompts/list_changed"
 *   - resourcesListChanged for "notifications/resources/list_changed"
 *   - resourceSubscriptions for "notifications/resources/updated" (matches uri in params)
 *   - other notification methods return true.
 *
 * @param ctx      Context; may be NULL.
 * @param session  Session to check.
 * @param method   Notification method string (non-NULL).
 * @param params   Notification params JSON object (borrowed, may be NULL).
 * @return true if the notification should be delivered to this session; false if filtered out.
 */
bool mcp_session_is_subscribed_to_notification(mcp_context_t *ctx, const mcp_session_t *session,
                                              const char *method, const mcp_json_value_t *params);

/**
 * @brief Builds a notification message for a session, automatically applying subscription
 *        filtering and injecting `_meta["io.modelcontextprotocol/subscriptionId"]` if active.
 *
 * Ownership: On MCP_OK, if `*notif_out != NULL`, `params` ownership was transferred to `*notif_out`.
 *            If `*notif_out == NULL` (because the notification was filtered out), `params` was destroyed.
 *            On error, the caller retains ownership of `params` (unless already consumed).
 *
 * @param ctx        Context; may be NULL.
 * @param session    Target session; may be NULL (treated as no active subscription).
 * @param method     Notification method string (non-NULL).
 * @param params     Owned params object; may be NULL.
 * @param notif_out  Receives the built notification message, or NULL if filtered out.
 * @return MCP_OK on success (with *notif_out set or NULL); MCP_ERR_* on error.
 */
mcp_status_t mcp_session_build_notification(mcp_context_t *ctx, const mcp_session_t *session,
                                            const char *method, mcp_json_value_t *params,
                                            mcp_message_t **notif_out);

/**
 * @brief Builds a graceful closure response for an active subscription stream.
 *
 * Emits the JSON-RPC response completing the original `subscriptions/listen` request:
 * {"jsonrpc":"2.0","id":<sub_id>,"result":{"resultType":"complete","_meta":{"io.modelcontextprotocol/subscriptionId":<sub_id>}}}
 * Resets the session's active subscription state.
 *
 * @param ctx       Context; may be NULL.
 * @param session   Target session; must have an active subscription.
 * @param resp_out  Receives the caller-owned completion response message.
 * @return MCP_OK on success; MCP_ERR_NOT_FOUND or MCP_ERR_INVALID_ARGUMENT if no active subscription.
 */
mcp_status_t mcp_session_build_subscription_closure(mcp_context_t *ctx, mcp_session_t *session,
                                                    mcp_message_t **resp_out);

#endif
