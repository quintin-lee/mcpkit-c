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
    srv_free(ctx, s->client_name);
    srv_free(ctx, s->client_version);
    srv_free(ctx, s);
}

bool mcp_session_is_initialized(mcp_context_t *ctx, const mcp_session_t *session) {
    (void)ctx;
    return session != NULL && session->initialized;
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
