/**
 * @file queue.c
 * @brief Per-session FIFO message queue and single-step processing.
 */
#define _DEFAULT_SOURCE

#include <string.h>

#include "mcpkit/server/dispatcher.h"
#include "mcpkit/core/context.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/server.h"
#include "internals.h"

struct mcp_queue {
    mcp_session_t **sessions;
    mcp_message_t **msgs;
    size_t head;
    size_t len;
    size_t cap;
};

mcp_queue_t *mcp_queue_create(mcp_context_t *ctx) {
    mcp_queue_t *q = srv_malloc(ctx, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    memset(q, 0, sizeof(*q));
    return q;
}

void mcp_queue_destroy(mcp_context_t *ctx, mcp_queue_t *q) {
    if (q == NULL) {
        return;
    }
    for (size_t i = 0; i < q->len; i++) {
        mcp_message_destroy(ctx, q->msgs[(q->head + i) % q->cap]);
    }
    srv_free(ctx, q->sessions);
    srv_free(ctx, q->msgs);
    srv_free(ctx, q);
}

static mcp_status_t queue_grow(mcp_context_t *ctx, mcp_queue_t *q) {
    if (q->len < q->cap && q->head == 0) {
        return MCP_OK;
    }
    if (q->head > 0) {
        for (size_t i = 0; i < q->len; i++) {
            size_t src = (q->head + i) % q->cap;
            if (src != i) {
                q->sessions[i] = q->sessions[src];
                q->msgs[i] = q->msgs[src];
            }
        }
        q->head = 0;
        if (q->len < q->cap) {
            return MCP_OK;
        }
    }
    size_t ncap = q->cap == 0 ? 8 : q->cap * 2;
    mcp_message_t **om = q->msgs;
    struct mcp_session **os = q->sessions;
    mcp_message_t **nm = srv_realloc(ctx, om, ncap * sizeof(*nm));
    if (nm == NULL) {
        return MCP_ERR_NOMEM;
    }
    /* Commit msgs before touching sessions: if the sessions realloc fails,
     * q->msgs already points to a valid (possibly larger) buffer and the
     * ring logic still operates within the old cap, so no dangling pointer. */
    q->msgs = nm;
    struct mcp_session **ns = srv_realloc(ctx, os, ncap * sizeof(*ns));
    if (ns == NULL) {
        return MCP_ERR_NOMEM;
    }
    q->sessions = ns;
    q->cap = ncap;
    return MCP_OK;
}

mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session,
                            mcp_message_t *msg) {
    if (q == NULL || session == NULL || msg == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (q->len >= MCP_QUEUE_MAX_LEN) {
        return MCP_ERR_NOMEM;
    }
    if (queue_grow(ctx, q) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    size_t slot = (q->head + q->len) % q->cap;
    q->sessions[slot] = session;
    q->msgs[slot] = msg;
    q->len++;
    return MCP_OK;
}

mcp_status_t mcp_queue_pop(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t **session_out,
                           mcp_message_t **msg_out) {
    (void)ctx;
    if (q == NULL || session_out == NULL || msg_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (q->len == 0) {
        return MCP_ERR_NOT_FOUND;
    }
    *session_out = q->sessions[q->head];
    *msg_out = q->msgs[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return MCP_OK;
}

size_t mcp_queue_size(mcp_context_t *ctx, const mcp_queue_t *q) {
    (void)ctx;
    return q == NULL ? 0 : q->len;
}

mcp_status_t mcp_server_process_one(mcp_context_t *ctx, mcp_server_t *srv, mcp_queue_t *q,
                                    mcp_message_t **resp_out) {
    if (srv == NULL || q == NULL || resp_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *resp_out = NULL;
    mcp_session_t *session = NULL;
    mcp_message_t *msg = NULL;
    if (mcp_queue_pop(ctx, q, &session, &msg) != MCP_OK) {
        return MCP_ERR_NOT_FOUND;
    }
    if (mcp_message_kind(ctx, msg) == MCP_MSG_NOTIFICATION) {
        mcp_status_t st = mcp_server_notify(ctx, srv, session, msg);
        mcp_message_destroy(ctx, msg);
        return st;
    }
    mcp_status_t st = mcp_server_dispatch(ctx, srv, session, msg, resp_out);
    mcp_message_destroy(ctx, msg);
    return st;
}
