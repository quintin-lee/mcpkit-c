/**
 * @file policy.c
 * @brief Version-dependent protocol policy queries.
 *
 * Centralizes 2026-07-28 behavior gates (deprecated method detection,
 * deterministic list sorting) that would otherwise scatter through route
 * handlers and transport code.
 */

#include "mcpkit/protocol/policy.h"
#include "mcpkit/protocol/version.h"
#include "mcpkit/server/session.h"

#include <string.h>

bool mcp_proto_policy_method_deprecated(mcp_context_t *ctx,
                                       const mcp_session_t *session,
                                       const char *method) {
    if (method == NULL ||
        mcp_proto_at_least(mcp_session_negotiated(ctx, session), MCP_PROTO_2026_07_28) == false) {
        return false;
    }
    if (strcmp(method, "ping") == 0 || strcmp(method, "logging/setLevel") == 0) {
        return true;
    }
    return strcmp(method, "resources/subscribe") == 0 ||
           strcmp(method, "resources/unsubscribe") == 0;
}

bool mcp_proto_policy_cache_defaults_applied(mcp_context_t *ctx,
                                              const mcp_session_t *session) {
    return mcp_proto_at_least(mcp_session_negotiated(ctx, session), MCP_PROTO_2026_07_28);
}

bool mcp_proto_policy_sort_lists_by_name(mcp_context_t *ctx,
                                          const mcp_session_t *session) {
    return mcp_proto_at_least(mcp_session_negotiated(ctx, session), MCP_PROTO_2026_07_28);
}

bool mcp_proto_policy_auth_hardening_active(mcp_context_t *ctx,
                                             const mcp_session_t *session) {
    return mcp_proto_at_least(mcp_session_negotiated(ctx, session), MCP_PROTO_2025_11_25);
}

bool mcp_proto_policy_cimd_preferred(mcp_context_t *ctx,
                                      const mcp_session_t *session) {
    return mcp_proto_at_least(mcp_session_negotiated(ctx, session), MCP_PROTO_2025_11_25);
}

bool mcp_proto_policy_sep2243_active(mcp_context_t *ctx,
                                      const mcp_session_t *session) {
    /* SEP-2243 custom Mcp-Param-* header behavior is a 2026-07-28 MUST.
       Below that version custom headers are not validated (SHOULD-grade),
       so the gate is false. NULL session -> conservative baseline. */
    return mcp_proto_at_least(mcp_session_negotiated(ctx, session), MCP_PROTO_2026_07_28);
}
