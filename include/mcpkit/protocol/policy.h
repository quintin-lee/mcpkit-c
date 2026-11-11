/**
 * @file policy.h
 * @brief Version-dependent protocol policy queries for server routes.
 *
 * These helpers centralize the 2026-07-28 behavior gates that would
 * otherwise scatter through route handlers and transport code:
 * deprecated method detection, deterministic list sorting, and default
 * cache hint injection.
 *
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_POLICY_H
#define MCPKIT_PROTOCOL_POLICY_H

#include <stdbool.h>

#include "mcpkit/core/error.h"
#include "mcpkit/protocol/version.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;

/**
 * @brief Returns true when the session's negotiated protocol treats @p method
 *        as deprecated.
 *
 * For 2026-07-28, `ping`, `logging/setLevel`, `resources/subscribe`, and
 * `resources/unsubscribe` are removed from the method surface.
 *
 * @param ctx     Context; may be NULL.
 * @param session Session whose negotiated version is used.
 * @param method  Borrowed method name; NULL returns false.
 * @return true if the method is deprecated for this session.
 */
bool mcp_proto_policy_method_deprecated(mcp_context_t *ctx,
                                        const mcp_session_t *session,
                                        const char *method);

/**
 * @brief Returns true when default cache hints should be injected on list
 *        responses for sessions that have not been explicitly configured.
 *
 * This is the 2026-07-28 cache default behavior: when no explicit
 * `list_ttl_ms` / `list_cache_scope` is set, the server injects
 * `ttlMs: 0` and `cacheScope: "private"`.
 *
 * @param ctx     Context; may be NULL.
 * @param session Session whose negotiated version is used.
 * @return true if default cache hints should be applied for this session.
 */
bool mcp_proto_policy_cache_defaults_applied(mcp_context_t *ctx,
                                              const mcp_session_t *session);

/**
 * @brief Returns true when list results should be sorted by name.
 *
 * Deterministic sorting is a 2026-07-28 requirement.
 *
 * @param ctx     Context; may be NULL.
 * @param session Session whose negotiated version is used.
 * @return true if list handlers should sort alphabetically.
 */
bool mcp_proto_policy_sort_lists_by_name(mcp_context_t *ctx,
                                          const mcp_session_t *session);

/**
 * @brief Returns true when P3 authorization hardening is active for the
 *        session's negotiated protocol version.
 *
 * P3 hardening (RFC 9207 issuer gate, RFC 9728 protected-resource
 * metadata, CIMD, SEP-835 incremental scope) is gated to sessions that
 * negotiated 2025-11-25 or later. Below that version the gates are
 * inactive and 401 responses retain their legacy shape.
 *
 * @param ctx     Context; may be NULL.
 * @param session Session whose negotiated version is used.
 * @return true if P3 authorization hardening applies to this session.
 */
bool mcp_proto_policy_auth_hardening_active(mcp_context_t *ctx,
                                             const mcp_session_t *session);

/**
 * @brief Returns true when the session prefers Client ID Metadata Documents
 *        (SEP-991 / CIMD) over the deprecated RFC 7591 Dynamic Client
 *        Registration (DCR) endpoint.
 *
 * Active at 2025-11-25 and later. DCR responses in this regime carry the
 * `Deprecation: true` + `Sunset` header pair (see
 * mcp_oauth_build_dcr_deprecation_headers).
 *
 * @param ctx     Context; may be NULL.
 * @param session Session whose negotiated version is used.
 * @return true if CIMD is the preferred registration mechanism.
 */
bool mcp_proto_policy_cimd_preferred(mcp_context_t *ctx,
                                      const mcp_session_t *session);

/**
 * @brief Returns true when SEP-2243 custom Mcp-Param-* header behavior is
 *        active for the session's negotiated protocol version.
 *
 * SEP-2243 x-mcp-header / Mcp-Param-* consistency checks are mandatory
 * (MUST-grade, missing header or mismatch -> HTTP 400 + JSON-RPC -32001)
 * at 2026-07-28. Below that version the x-mcp-header annotation may be
 * present but custom headers are not validated (SHOULD-grade, zero
 * regression on 2025-11-25 and earlier).
 *
 * @param ctx     Context; may be NULL.
 * @param session Session whose negotiated version is used; NULL ->
 *                conservative baseline (not active).
 * @return true if SEP-2243 custom header behavior applies to this session.
 */
bool mcp_proto_policy_sep2243_active(mcp_context_t *ctx,
                                      const mcp_session_t *session);

#endif
