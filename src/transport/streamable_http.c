/**
 * @file streamable_http.c
 *
 * Streamable-HTTP serve loop over an opaque I/O vtable. Sessions are
 * loop-local: a fixed 16-slot table keyed by Mcp-Session-Id, all
 * destroyed when the loop returns. The carry buffer accumulates
 * partial bodies across reads and is capped at CARRY_MAX; SSE
 * strings are released with mcp_json_free_string.
 */
#include "mcpkit/transport/streamable_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/value.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/server.h"
#include "mcpkit/transport/http.h"

#define SESSION_ID_CAP 32
#define CARRY_MAX (MCP_HTTP_MAX_BODY + 65536u)

typedef struct {
    bool used;
    char id[SESSION_ID_CAP];
    mcp_session_t *sess;
} http_session_slot_t;

typedef struct {
    http_session_slot_t slots[MCP_HTTP_MAX_SESSIONS];
    unsigned next_id;
} http_session_table_t;

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static void *h_malloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = alloc_of(ctx);
    return a->malloc_fn(n, a->userdata);
}

static void h_free(mcp_context_t *ctx, void *p) {
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(p, a->userdata);
}

char *mcp_sse_wrap(mcp_context_t *ctx, const char *json_text) {
    if (json_text == NULL) {
        return NULL;
    }
    size_t jlen = strlen(json_text);
    char *out = h_malloc(ctx, 6 + jlen + 2 + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, "data: ", 6);
    memcpy(out + 6, json_text, jlen);
    memcpy(out + 6 + jlen, "\n\n", 3);
    return out;
}

static http_session_slot_t *table_find(http_session_table_t *t, const char *sid) {
    for (size_t i = 0; i < MCP_HTTP_MAX_SESSIONS; i++) {
        if (t->slots[i].used && strcmp(t->slots[i].id, sid) == 0) {
            return &t->slots[i];
        }
    }
    return NULL;
}

static mcp_status_t send_bytes(mcp_context_t *ctx, mcp_http_io_t *io, int status,
                               const char *reason, const char *ctype, const char *body,
                               size_t body_len, const char *xname, const char *xvalue) {
    mcp_http_response_t *resp = mcp_http_response_new(ctx, status, reason);
    if (resp == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = MCP_OK;
    if (ctype != NULL) {
        st = mcp_http_response_set_header(ctx, resp, "Content-Type", ctype);
    }
    if (st == MCP_OK && xname != NULL) {
        st = mcp_http_response_set_header(ctx, resp, xname, xvalue);
    }
    if (st == MCP_OK) {
        st = mcp_http_response_set_body(ctx, resp, body, body_len);
    }
    const char *bytes = NULL;
    if (st == MCP_OK) {
        bytes = mcp_http_response_serialize(ctx, resp);
        if (bytes == NULL) {
            st = MCP_ERR_NOMEM;
        }
    }
    if (st == MCP_OK) {
        st = io->write(ctx, io->user, bytes, strlen(bytes));
    }
    mcp_http_response_destroy(ctx, resp);
    return st;
}

static mcp_status_t handle_post(mcp_context_t *ctx, mcp_server_t *server,
                                http_session_table_t *table, mcp_http_io_t *io,
                                mcp_http_request_t *req) {
    size_t body_len = 0;
    const char *body = mcp_http_request_body(ctx, req, &body_len);
    if (body == NULL || body_len == 0) {
        return send_bytes(ctx, io, 400, "Bad Request", NULL, NULL, 0, NULL, NULL);
    }
    /* RFC 9110 10.1.5: in a real socket stream the client sends
     * Expect: 100-continue *before* the body; our buffer-level model
     * already has the body, so this is purely the protocol-level
     * acknowledgement the client expects before the final response. */
    const char *expect_hdr = mcp_http_header(ctx, req, "Expect");
    if (expect_hdr != NULL && strncmp(expect_hdr, "100", 3) == 0
        && (expect_hdr[3] == '-' || expect_hdr[3] == '\0')) {
        const char *interim = "HTTP/1.1 100 Continue\r\n\r\n";
        if (io->write(ctx, io->user, interim, strlen(interim)) != MCP_OK) {
            return MCP_ERR_IO;
        }
    }
    mcp_message_t *msg = mcp_message_parse(ctx, body, body_len);
    if (msg == NULL) {
        return send_bytes(ctx, io, 400, "Bad Request", NULL, NULL, 0, NULL, NULL);
    }
    mcp_msg_kind_t kind = mcp_message_kind(ctx, msg);
    if (kind != MCP_MSG_REQUEST && kind != MCP_MSG_NOTIFICATION) {
        mcp_message_destroy(ctx, msg);
        return send_bytes(ctx, io, 400, "Bad Request", NULL, NULL, 0, NULL, NULL);
    }
    const char *sid = mcp_http_header(ctx, req, "Mcp-Session-Id");
    const char *method = kind == MCP_MSG_REQUEST ? mcp_message_method(ctx, msg) : NULL;
    bool is_init = method != NULL && strcmp(method, "initialize") == 0;
    bool is_new = false;
    http_session_slot_t *slot = NULL;
    if (sid == NULL) {
        if (!is_init) {
            mcp_message_destroy(ctx, msg);
            return send_bytes(ctx, io, 400, "Bad Request", NULL, NULL, 0, NULL, NULL);
        }
        for (size_t i = 0; i < MCP_HTTP_MAX_SESSIONS; i++) {
            if (!table->slots[i].used) {
                slot = &table->slots[i];
                break;
            }
        }
        if (slot == NULL) {
            mcp_message_destroy(ctx, msg);
            return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL,
                              NULL);
        }
        slot->sess = mcp_server_create_session(ctx, server);
        if (slot->sess == NULL) {
            mcp_message_destroy(ctx, msg);
            return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL,
                              NULL);
        }
        snprintf(slot->id, sizeof(slot->id), "sess-%u", table->next_id++);
        slot->used = true;
        is_new = true;
    } else {
        slot = table_find(table, sid);
        if (slot == NULL) {
            mcp_message_destroy(ctx, msg);
            return send_bytes(ctx, io, 404, "Not Found", NULL, NULL, 0, NULL, NULL);
        }
    }
    if (kind == MCP_MSG_NOTIFICATION) {
        mcp_server_notify(ctx, server, slot->sess, msg);
        mcp_message_destroy(ctx, msg);
        return send_bytes(ctx, io, 202, "Accepted", NULL, NULL, 0, NULL, NULL);
    }
    mcp_message_t *resp = NULL;
    mcp_status_t st = mcp_server_dispatch(ctx, server, slot->sess, msg, &resp);
    mcp_message_destroy(ctx, msg);
    if (st != MCP_OK || resp == NULL) {
        return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL, NULL);
    }
    char *json = mcp_message_serialize(ctx, resp);
    mcp_message_destroy(ctx, resp);
    if (json == NULL) {
        return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL, NULL);
    }
    st = send_bytes(ctx, io, 200, "OK", "application/json", json, strlen(json),
                    is_new ? "Mcp-Session-Id" : NULL, slot->id);
    mcp_json_free_string(ctx, json);
    return st;
}

static mcp_status_t handle_get(mcp_context_t *ctx, mcp_http_io_t *io, mcp_http_request_t *req) {
    const char *accept = mcp_http_header(ctx, req, "Accept");
    if (accept == NULL || strstr(accept, "text/event-stream") == NULL) {
        return send_bytes(ctx, io, 405, "Method Not Allowed", NULL, NULL, 0, NULL, NULL);
    }
    char *ev = mcp_sse_wrap(ctx, "{\"ping\":\"ok\"}");
    if (ev == NULL) {
        return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL, NULL);
    }
    mcp_status_t st = send_bytes(ctx, io, 200, "OK", "text/event-stream", ev, strlen(ev),
                                 NULL, NULL);
    mcp_json_free_string(ctx, ev);
    return st;
}

static mcp_status_t handle_delete(mcp_context_t *ctx, mcp_server_t *server,
                                  http_session_table_t *table, mcp_http_io_t *io,
                                  mcp_http_request_t *req) {
    const char *sid = mcp_http_header(ctx, req, "Mcp-Session-Id");
    http_session_slot_t *slot = sid != NULL ? table_find(table, sid) : NULL;
    if (slot == NULL) {
        return send_bytes(ctx, io, 404, "Not Found", NULL, NULL, 0, NULL, NULL);
    }
    mcp_server_destroy_session(ctx, server, slot->sess);
    slot->used = false;
    slot->sess = NULL;
    return send_bytes(ctx, io, 200, "OK", NULL, NULL, 0, NULL, NULL);
}

mcp_status_t mcp_http_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_http_io_t *io) {
    if (server == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    http_session_table_t table;
    memset(&table, 0, sizeof(table));
    table.next_id = 1;
    char *carry = NULL;
    size_t clen = 0;
    size_t ccap = 0;
    mcp_status_t status = MCP_OK;
    for (;;) {
        mcp_http_request_t *req = NULL;
        size_t consumed = 0;
        for (;;) {
            if (clen >= 4) {
                const char *hs = NULL;
                for (size_t i = 0; i + 4 <= clen; i++) {
                    if (carry[i] == '\r' && carry[i + 1] == '\n' && carry[i + 2] == '\r'
                        && carry[i + 3] == '\n') {
                        hs = carry + i;
                        break;
                    }
                }
                if (hs != NULL) {
                    req = mcp_http_parse_request(ctx, carry, clen);
                    if (req == NULL) {
                        status = send_bytes(ctx, io, 400, "Bad Request", NULL, NULL, 0,
                                            NULL, NULL);
                        if (status != MCP_OK) {
                            goto done;
                        }
                        clen = 0;
                        break;
                    }
                    size_t body_len = 0;
                    const char *body = mcp_http_request_body(ctx, req, &body_len);
                    (void)body;
                    consumed = (size_t)(hs - carry) + 4 + body_len;
                    if (consumed > clen) {
                        mcp_http_request_destroy(ctx, req);
                        req = NULL;
                        consumed = 0;
                    } else {
                        break;
                    }
                }
            }
            if (clen >= CARRY_MAX) {
                status = MCP_ERR_PROTOCOL;
                goto done;
            }
            if (ccap - clen < 4096) {
                size_t ncap = ccap == 0 ? 8192 : ccap * 2;
                char *nbuf = h_malloc(ctx, ncap);
                if (nbuf == NULL) {
                    status = MCP_ERR_NOMEM;
                    goto done;
                }
                if (clen > 0) {
                    memcpy(nbuf, carry, clen);
                }
                h_free(ctx, carry);
                carry = nbuf;
                ccap = ncap;
            }
            size_t n = 0;
            mcp_status_t st = io->read(ctx, io->user, carry + clen, ccap - clen, &n);
            if (st != MCP_OK) {
                status = st;
                goto done;
            }
            if (n == 0) {
                if (clen == 0) {
                    goto done;
                }
                status = MCP_ERR_IO;
                goto done;
            }
            clen += n;
        }
        if (req == NULL) {
            continue;
        }
        mcp_http_method_t m = mcp_http_request_method(ctx, req);
        mcp_status_t st;
        if (m == MCP_HTTP_POST) {
            st = handle_post(ctx, server, &table, io, req);
        } else if (m == MCP_HTTP_GET) {
            st = handle_get(ctx, io, req);
        } else if (m == MCP_HTTP_DELETE) {
            st = handle_delete(ctx, server, &table, io, req);
        } else {
            st = send_bytes(ctx, io, 405, "Method Not Allowed", NULL, NULL, 0, NULL, NULL);
        }
        mcp_http_request_destroy(ctx, req);
        if (consumed < clen) {
            memmove(carry, carry + consumed, clen - consumed);
        }
        clen -= consumed;
        if (st != MCP_OK) {
            status = st;
            goto done;
        }
    }
done:
    for (size_t i = 0; i < MCP_HTTP_MAX_SESSIONS; i++) {
        if (table.slots[i].used) {
            mcp_server_destroy_session(ctx, server, table.slots[i].sess);
            table.slots[i].used = false;
        }
    }
    h_free(ctx, carry);
    return status;
}
