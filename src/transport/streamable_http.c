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

/* Case-insensitive match of the first `len` chars of `s` against a
 * lowercase-only pattern; mirrors the hand-rolled tolower approach used
 * by mcp_http_request_wants_close() in http.c (strncasecmp is POSIX,
 * unavailable under this project's C23 + -Werror baseline). */
static bool ci_prefix_eq(const char *s, size_t len, const char *pattern) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != pattern[i]) {
            return false;
        }
    }
    return true;
}

static mcp_status_t send_unauthorized(mcp_context_t *ctx, mcp_http_io_t *io) {
    mcp_http_response_t *resp = mcp_http_response_new(ctx, 401, "Unauthorized");
    if (resp == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_http_response_set_header(ctx, resp, "WWW-Authenticate",
                                                   "Bearer");
    if (st == MCP_OK) {
        st = mcp_http_response_set_body(ctx, resp, "", 0);
    }
    if (st == MCP_OK) {
        st = mcp_http_response_set_header(ctx, resp, "Content-Length", "0");
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

static mcp_status_t send_rpc_error(mcp_context_t *ctx, mcp_http_io_t *io,
                                   const mcp_message_t *req, int code,
                                   const char *message) {
    mcp_message_t *err_msg = mcp_response_err_new(ctx, req, code, message, NULL);
    if (err_msg == NULL) {
        return send_bytes(ctx, io, 400, "Bad Request", NULL, NULL, 0, NULL, NULL);
    }
    char *json = mcp_message_serialize(ctx, err_msg);
    mcp_message_destroy(ctx, err_msg);
    if (json == NULL) {
        return send_bytes(ctx, io, 400, "Bad Request", NULL, NULL, 0, NULL, NULL);
    }
    mcp_status_t st = send_bytes(ctx, io, 400, "Bad Request", "application/json",
                                 json, strlen(json), NULL, NULL);
    mcp_json_free_string(ctx, json);
    return st;
}

static bool is_supported_protocol_version(const char *ver) {
    if (ver == NULL) {
        return false;
    }
    static const char *const k_supported_versions[] = {
        "2026-07-28",
        "2025-11-25",
        "2025-06-18",
        "2024-11-05",
    };
    for (size_t i = 0; i < sizeof(k_supported_versions) / sizeof(k_supported_versions[0]); i++) {
        if (strcmp(ver, k_supported_versions[i]) == 0) {
            return true;
        }
    }
    return false;
}

static mcp_status_t validate_http_metadata(mcp_context_t *ctx, mcp_http_request_t *req,
                                           const mcp_message_t *msg, mcp_http_io_t *io,
                                           bool *rejected) {
    *rejected = false;
    const char *proto_hdr = mcp_http_header(ctx, req, "MCP-Protocol-Version");
    const char *method_hdr = mcp_http_header(ctx, req, "Mcp-Method");
    const char *name_hdr = mcp_http_header(ctx, req, "Mcp-Name");

    if (proto_hdr != NULL) {
        if (!is_supported_protocol_version(proto_hdr)) {
            *rejected = true;
            return send_rpc_error(ctx, io, msg, MCP_RPC_UNSUPPORTED_PROTOCOL_VERSION,
                                  "unsupported protocol version");
        }
    }

    const mcp_json_value_t *meta = mcp_message_meta(ctx, msg);
    const char *meta_ver = NULL;
    if (meta != NULL) {
        const mcp_json_value_t *pv = mcp_json_object_get(ctx, meta, "io.modelcontextprotocol/protocolVersion");
        if (pv == NULL) {
            pv = mcp_json_object_get(ctx, meta, "protocolVersion");
        }
        if (pv != NULL) {
            mcp_json_string_value(ctx, pv, &meta_ver);
        }
    }

    if (proto_hdr != NULL && meta_ver != NULL) {
        if (strcmp(proto_hdr, meta_ver) != 0) {
            *rejected = true;
            return send_rpc_error(ctx, io, msg, MCP_RPC_HEADER_MISMATCH,
                                  "MCP-Protocol-Version header does not match _meta");
        }
    }

    bool is_2026 = (proto_hdr != NULL && strcmp(proto_hdr, "2026-07-28") == 0);
    const char *msg_method = mcp_message_method(ctx, msg);
    mcp_msg_kind_t kind = mcp_message_kind(ctx, msg);

    if (is_2026 && kind == MCP_MSG_REQUEST) {
        if (method_hdr == NULL) {
            *rejected = true;
            return send_rpc_error(ctx, io, msg, MCP_RPC_HEADER_MISMATCH,
                                  "missing required Mcp-Method header");
        }
    }

    if (method_hdr != NULL && msg_method != NULL) {
        if (strcmp(method_hdr, msg_method) != 0) {
            *rejected = true;
            return send_rpc_error(ctx, io, msg, MCP_RPC_HEADER_MISMATCH,
                                  "Mcp-Method header does not match request method");
        }
    }

    bool needs_name = (msg_method != NULL &&
                       (strcmp(msg_method, "tools/call") == 0 ||
                        strcmp(msg_method, "prompts/get") == 0 ||
                        strcmp(msg_method, "resources/read") == 0));

    if (is_2026 && needs_name && name_hdr == NULL) {
        *rejected = true;
        return send_rpc_error(ctx, io, msg, MCP_RPC_HEADER_MISMATCH,
                              "missing required Mcp-Name header");
    }

    if (name_hdr != NULL && msg_method != NULL) {
        const mcp_json_value_t *params = mcp_message_params(ctx, msg);
        if (strcmp(msg_method, "tools/call") == 0 || strcmp(msg_method, "prompts/get") == 0) {
            const mcp_json_value_t *nv = params != NULL ? mcp_json_object_get(ctx, params, "name") : NULL;
            const char *pname = NULL;
            if (nv != NULL) {
                mcp_json_string_value(ctx, nv, &pname);
            }
            if (pname == NULL || strcmp(name_hdr, pname) != 0) {
                *rejected = true;
                return send_rpc_error(ctx, io, msg, MCP_RPC_HEADER_MISMATCH,
                                      "Mcp-Name header does not match params.name");
            }
        } else if (strcmp(msg_method, "resources/read") == 0) {
            const mcp_json_value_t *uv = params != NULL ? mcp_json_object_get(ctx, params, "uri") : NULL;
            const char *puri = NULL;
            if (uv != NULL) {
                mcp_json_string_value(ctx, uv, &puri);
            }
            if (puri == NULL || strcmp(name_hdr, puri) != 0) {
                *rejected = true;
                return send_rpc_error(ctx, io, msg, MCP_RPC_HEADER_MISMATCH,
                                      "Mcp-Name header does not match params.uri");
            }
        }
    }

    return MCP_OK;
}

static mcp_status_t handle_post(mcp_context_t *ctx, mcp_server_t *server,
                                http_session_table_t *table, mcp_http_io_t *io,
                                mcp_http_request_t *req,
                                mcp_http_auth_fn auth_fn, void *auth_ud) {
    if (auth_fn != NULL) {
        const char *auth_hdr = mcp_http_header(ctx, req, "Authorization");
        if (auth_hdr == NULL || !ci_prefix_eq(auth_hdr, 7, "bearer ")) {
            return send_unauthorized(ctx, io);
        }
        const char *token = auth_hdr + 7;
        if (!auth_fn(ctx, token, auth_ud)) {
            return send_unauthorized(ctx, io);
        }
    }
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
    bool rejected = false;
    mcp_status_t val_st = validate_http_metadata(ctx, req, msg, io, &rejected);
    if (rejected) {
        mcp_message_destroy(ctx, msg);
        return val_st;
    }
    const char *sid = mcp_http_header(ctx, req, "Mcp-Session-Id");
    const char *method = kind == MCP_MSG_REQUEST ? mcp_message_method(ctx, msg) : NULL;
    bool is_init = method != NULL && strcmp(method, "initialize") == 0;
    bool is_discover = method != NULL && strcmp(method, "server/discover") == 0;
    bool is_stateless = mcp_message_meta(ctx, msg) != NULL || is_discover;
    bool is_new = false;
    http_session_slot_t *slot = NULL;
    mcp_session_t *sess = NULL;

    if (sid == NULL) {
        if (is_stateless) {
            sess = mcp_server_create_session(ctx, server);
            if (sess == NULL) {
                mcp_message_destroy(ctx, msg);
                return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL, NULL);
            }
        } else if (!is_init) {
            mcp_message_destroy(ctx, msg);
            return send_bytes(ctx, io, 400, "Bad Request", NULL, NULL, 0, NULL, NULL);
        } else {
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
            sess = slot->sess;
        }
    } else {
        slot = table_find(table, sid);
        if (slot == NULL) {
            mcp_message_destroy(ctx, msg);
            return send_bytes(ctx, io, 404, "Not Found", NULL, NULL, 0, NULL, NULL);
        }
        sess = slot->sess;
    }
    if (kind == MCP_MSG_NOTIFICATION) {
        mcp_server_notify(ctx, server, sess, msg);
        mcp_message_destroy(ctx, msg);
        if (is_stateless) {
            mcp_server_destroy_session(ctx, server, sess);
        }
        return send_bytes(ctx, io, 202, "Accepted", NULL, NULL, 0, NULL, NULL);
    }
    mcp_message_t *resp = NULL;
    mcp_status_t st = mcp_server_dispatch(ctx, server, sess, msg, &resp);
    mcp_message_destroy(ctx, msg);
    if (is_stateless) {
        mcp_server_destroy_session(ctx, server, sess);
    }
    if (st != MCP_OK || resp == NULL) {
        return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL, NULL);
    }
    char *json = mcp_message_serialize(ctx, resp);
    mcp_message_destroy(ctx, resp);
    if (json == NULL) {
        return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL, NULL);
    }
    st = send_bytes(ctx, io, 200, "OK", "application/json", json, strlen(json),
                    is_new ? "Mcp-Session-Id" : NULL, slot ? slot->id : NULL);
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

static mcp_status_t http_serve_impl(mcp_context_t *ctx, mcp_server_t *server,
                                     mcp_http_io_t *io, mcp_http_auth_fn auth_fn,
                                     void *auth_ud) {
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
            st = handle_post(ctx, server, &table, io, req, auth_fn, auth_ud);
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

mcp_status_t mcp_http_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_http_io_t *io) {
    if (server == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return http_serve_impl(ctx, server, io, NULL, NULL);
}

mcp_status_t mcp_http_serve_with_auth(mcp_context_t *ctx, mcp_server_t *server,
                                       mcp_http_io_t *io, mcp_http_auth_fn auth_fn,
                                       void *auth_user_data) {
    if (server == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return http_serve_impl(ctx, server, io, auth_fn, auth_user_data);
}
