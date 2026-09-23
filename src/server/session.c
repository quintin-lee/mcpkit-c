/**
 * @file session.c
 *
 * Session allocation and per-session duplicate-id tracking. session_new
 * zero-clears the struct then sets granted = 0xFFFFFFFFu (ALL granted)
 * as the default permission mask; memset alone would leave it NONE.
 */
#include "mcpkit/server/session.h"

#include <string.h>

#include "internals.h"
#include "mcpkit/server/server.h"

static mcp_session_t *session_new(mcp_context_t *ctx) {
    mcp_session_t *s = srv_malloc(ctx, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->granted = 0xFFFFFFFFu;
    s->ids = mcp_idset_create(ctx);
    if (s->ids == NULL) {
        srv_free(ctx, s);
        return NULL;
    }
    return s;
}

void session_free(mcp_context_t *ctx, mcp_session_t *s) {
    if (s == NULL) {
        return;
    }
    mcp_idset_destroy(ctx, s->ids);
    mcp_json_destroy(ctx, s->client_meta);
    srv_free(ctx, s->client_name);
    srv_free(ctx, s->client_version);
    srv_free(ctx, s->sub_id_str);
    if (s->sub_resource_uris != NULL) {
        for (size_t i = 0; i < s->n_sub_resource_uris; i++) {
            srv_free(ctx, s->sub_resource_uris[i]);
        }
        srv_free(ctx, s->sub_resource_uris);
    }
    srv_free(ctx, s);
}

bool mcp_session_is_initialized(mcp_context_t *ctx, const mcp_session_t *session) {
    (void)ctx;
    return session != NULL && session->initialized;
}

mcp_status_t mcp_session_set_apps_host(mcp_context_t *ctx, mcp_session_t *session,
                                       bool apps_host) {
    if (ctx == NULL || session == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    session->apps_host = apps_host;
    return MCP_OK;
}

bool mcp_session_is_apps_host(mcp_context_t *ctx, const mcp_session_t *session) {
    (void)ctx;
    return session != NULL && session->apps_host;
}

mcp_status_t mcp_session_grant(mcp_context_t *ctx, mcp_session_t *session, uint32_t perm_mask) {
    if (ctx == NULL || session == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    session->granted |= perm_mask;
    return MCP_OK;
}

mcp_status_t mcp_session_revoke(mcp_context_t *ctx, mcp_session_t *session, uint32_t perm_mask) {
    if (ctx == NULL || session == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    session->granted &= ~perm_mask;
    return MCP_OK;
}

bool mcp_session_grants(mcp_context_t *ctx, const mcp_session_t *session, uint32_t perm_mask) {
    (void)ctx;
    return session != NULL && (session->granted & perm_mask) == perm_mask;
}

const mcp_json_value_t *mcp_session_client_meta(mcp_context_t *ctx,
                                                 const mcp_session_t *session) {
    (void)ctx;
    if (session == NULL) {
        return NULL;
    }
    return session->client_meta;
}

mcp_session_t *mcp_server_create_session(mcp_context_t *ctx, mcp_server_t *srv) {
    if (srv == NULL) {
        return NULL;
    }
    mcp_session_t *s = session_new(ctx);
    if (s == NULL) {
        return NULL;
    }
    if (srv->n_sessions == srv->cap_sessions) {
        size_t ncap = srv->cap_sessions == 0 ? 4 : srv->cap_sessions * 2;
        mcp_session_t **narr = srv_realloc(ctx, srv->sessions, ncap * sizeof(*narr));
        if (narr == NULL) {
            session_free(ctx, s);
            return NULL;
        }
        srv->sessions = narr;
        srv->cap_sessions = ncap;
    }
    srv->sessions[srv->n_sessions++] = s;
    return s;
}

void mcp_server_destroy_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *session) {
    if (srv == NULL || session == NULL) {
        return;
    }
    for (size_t i = 0; i < srv->n_sessions; i++) {
        if (srv->sessions[i] == session) {
            srv->sessions[i] = srv->sessions[--srv->n_sessions];
            session_free(ctx, session);
            return;
        }
    }
}
