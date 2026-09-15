#ifndef MCPKIT_SERVER_SESSION_H
#define MCPKIT_SERVER_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;

bool mcp_session_is_initialized(mcp_context_t *ctx, const mcp_session_t *session);

mcp_status_t mcp_session_set_apps_host(mcp_context_t *ctx, mcp_session_t *session,
                                       bool apps_host);
bool mcp_session_is_apps_host(mcp_context_t *ctx, const mcp_session_t *session);
mcp_status_t mcp_session_grant(mcp_context_t *ctx, mcp_session_t *session, uint32_t perm_mask);
mcp_status_t mcp_session_revoke(mcp_context_t *ctx, mcp_session_t *session, uint32_t perm_mask);
bool mcp_session_grants(mcp_context_t *ctx, const mcp_session_t *session, uint32_t perm_mask);

#endif
