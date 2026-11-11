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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/core/auth.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/value.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/policy.h"
#include "mcpkit/protocol/version.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"
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

static mcp_status_t send_unauthorized(mcp_context_t *ctx, mcp_http_io_t *io,
                                      const char *scope, const char *resource_metadata,
                                      const char *error_description) {
    mcp_http_response_t *resp = mcp_http_response_new(ctx, 401, "Unauthorized");
    if (resp == NULL) {
        return MCP_ERR_NOMEM;
    }
    /* RFC 6750 + SEP-835 (scope) + RFC 9728 §3.2 (resource_metadata) composed
       challenge. Each optional fragment is omitted when its argument is NULL,
       so the bare pre-P3 form is `Bearer` alone. */
    size_t cap = 8 +
                (scope != NULL ? strlen(scope) + 10 : 0) +
                (resource_metadata != NULL ? strlen(resource_metadata) + 24 : 0) +
                (error_description != NULL ? strlen(error_description) + 20 : 0) +
                64;
    const mcp_allocator_t *a = alloc_of(ctx);
    char *challenge = a->malloc_fn(cap, a->userdata);
    if (challenge == NULL) {
        mcp_http_response_destroy(ctx, resp);
        return MCP_ERR_NOMEM;
    }
    size_t off = (size_t)snprintf(challenge, cap, "Bearer");
    if (error_description != NULL) {
        off += (size_t)snprintf(challenge + off, cap - off,
                                ", error_description=\"%s\"", error_description);
    }
    if (scope != NULL) {
        off += (size_t)snprintf(challenge + off, cap - off, ", scope=\"%s\"", scope);
    }
    if (resource_metadata != NULL) {
        off += (size_t)snprintf(challenge + off, cap - off,
                                ", resource_metadata=\"%s\"", resource_metadata);
    }

    mcp_status_t st = mcp_http_response_set_header(ctx, resp, "WWW-Authenticate", challenge);
    h_free(ctx, challenge);
    if (st == MCP_OK) {
        st = mcp_http_response_set_body(ctx, resp, "", 0);
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
    return mcp_proto_version_parse(ver) != MCP_PROTO_NONE;
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

/* -------------------------------------------------------------------------
 * P4-a T3: SEP-2243 Mcp-Param-* consistency check.
 *
 * Walks the tool's inputSchema for `x-mcp-header` annotations, reads the
 * corresponding `Mcp-Param-{Name}` HTTP headers, decodes base64 sentinels,
 * and compares with the body parameter value.
 * Mismatch, missing header (when body has a value), or decode failure →
 * returns false (caller sends 400 + -32001).
 * ---------------------------------------------------------------------- */

#define XMCP_MAX_ANNOTATIONS 32
typedef struct {
    char param_name[64];    /* JSON property name (body key) */
    char header_name[64];   /* x-mcp-header annotation value (header suffix) */
    int cmp_mode;           /* 0=string, 1=numeric, 2=boolean */
} xmcp_anno_t;

/* Classify the comparison mode from a property schema's "type" constraint.
   The SEP-2243 registry (S1) guarantees the annotation sits on a property
   whose type allows string/integer/boolean, so the classification is:
   - exactly "integer" or a union containing integer -> numeric compare
   - exactly "boolean" or a union containing boolean -> boolean compare
   - otherwise -> string compare */
static int xmcp_classify_type(mcp_context_t *ctx, const mcp_json_value_t *schema) {
    const mcp_json_value_t *type_v = mcp_json_object_get(ctx, schema, "type");
    if (type_v == NULL) {
        return 0; /* unspecified type -> string compare */
    }
    int t = mcp_json_type(ctx, type_v);
    bool has_int = false, has_bool = false, has_other = false;
    if (t == MCP_JSON_STRING) {
        const char *s = NULL;
        mcp_json_string_value(ctx, type_v, &s);
        if (s != NULL) {
            if (strcmp(s, "integer") == 0) has_int = true;
            else if (strcmp(s, "boolean") == 0) has_bool = true;
            else has_other = true;
        }
    } else if (t == MCP_JSON_ARRAY) {
        size_t n = mcp_json_array_size(ctx, type_v);
        for (size_t i = 0; i < n; i++) {
            const char *s = NULL;
            mcp_json_string_value(ctx, mcp_json_array_get(ctx, type_v, i), &s);
            if (s == NULL) {
                continue;
            }
            if (strcmp(s, "integer") == 0) has_int = true;
            else if (strcmp(s, "boolean") == 0) has_bool = true;
            else has_other = true;
        }
    }
    if (has_int && !has_bool && !has_other) {
        return 1;
    }
    if (has_bool && !has_int && !has_other) {
        return 2;
    }
    return 0;
}

/* Key-aware recursive walk: when a node's "x-mcp-header" key is found
   AND its parent context is a properties-object, record (param_name, header_name).
   We track the current property name via a stack passed as `cur_prop_name`.
   At the top level the caller passes NULL. */
static int xmcp_collect_anno(mcp_context_t *ctx, const mcp_json_value_t *node,
                             const char *cur_prop_name,
                             xmcp_anno_t *out, int *count, int cap) {
    if (node == NULL || *count >= cap) {
        return *count;
    }
    int t = mcp_json_type(ctx, node);
    if (t == MCP_JSON_ARRAY) {
        size_t n = mcp_json_array_size(ctx, node);
        for (size_t i = 0; i < n && *count < cap; i++) {
            xmcp_collect_anno(ctx, mcp_json_array_get(ctx, node, i),
                              cur_prop_name, out, count, cap);
        }
        return *count;
    }
    if (t != MCP_JSON_OBJECT) {
        return *count;
    }

    /* Check if this object itself carries an x-mcp-header annotation. */
    if (mcp_json_object_has(ctx, node, "x-mcp-header") && cur_prop_name != NULL) {
        const mcp_json_value_t *hdr_v = mcp_json_object_get(ctx, node, "x-mcp-header");
        const char *hdr = NULL;
        if (mcp_json_string_value(ctx, hdr_v, &hdr) == MCP_OK && hdr != NULL && hdr[0] != '\0') {
            xmcp_anno_t *e = &out[*count];
            snprintf(e->param_name, sizeof(e->param_name), "%s", cur_prop_name);
            snprintf(e->header_name, sizeof(e->header_name), "%s", hdr);
            e->cmp_mode = xmcp_classify_type(ctx, node);
            (*count)++;
        }
    }

    /* Recurse into all object members, passing each key as the property name. */
    size_t n_members = mcp_json_object_size(ctx, node);
    for (size_t i = 0; i < n_members && *count < cap; i++) {
        const char *key = mcp_json_object_key_at(ctx, node, i);
        if (key == NULL) {
            continue;
        }
        const mcp_json_value_t *val = mcp_json_object_get(ctx, node, key);
        /* When the key is "properties", its children are property schemas.
           Each child's key is the parameter name, so recurse with the child's
           key as cur_prop_name. */
        if (strcmp(key, "properties") == 0 && mcp_json_type(ctx, val) == MCP_JSON_OBJECT) {
            size_t n_props = mcp_json_object_size(ctx, val);
            for (size_t j = 0; j < n_props && *count < cap; j++) {
                const char *pname = mcp_json_object_key_at(ctx, val, j);
                if (pname == NULL) {
                    continue;
                }
                xmcp_collect_anno(ctx, mcp_json_object_get(ctx, val, pname),
                                  pname, out, count, cap);
            }
        } else if (strcmp(key, "items") == 0) {
            /* items may be an object schema or an array of schemas */
            xmcp_collect_anno(ctx, val, cur_prop_name, out, count, cap);
        } else {
            xmcp_collect_anno(ctx, val, NULL, out, count, cap);
        }
    }
    return *count;
}

/* Collect all x-mcp-header annotations from a tool's inputSchema.
   Returns the number collected (0 if none or schema is NULL). */
static int xmcp_collect_annotations(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                   xmcp_anno_t *out, int cap) {
    int count = 0;
    if (schema == NULL) {
        return 0;
    }
    xmcp_collect_anno(ctx, schema, NULL, out, &count, cap);
    return count;
}

/* Compare a decoded header value against the body parameter value.
   Numeric comparison for integer-typed annotations, boolean textual compare
   for boolean-typed, string equality otherwise. */
static bool xmcp_values_match(const xmcp_anno_t *anno,
                              const char *decoded_header,
                              mcp_context_t *ctx,
                              const mcp_json_value_t *body_val) {
    if (body_val == NULL || decoded_header == NULL) {
        return false;
    }
    if (anno->cmp_mode == 1) {
        /* Numeric comparison: parse body number and header string as int64 */
        double body_num = 0;
        int rc = mcp_json_number_value(ctx, body_val, &body_num);
        if (rc == MCP_OK) {
            char *end = NULL;
            int64_t hdr_int = strtoll(decoded_header, &end, 10);
            return end != NULL && *end == '\0' && (double)hdr_int == body_num;
        }
        return false; /* body value is not a number */
    }
    if (anno->cmp_mode == 2) {
        bool bval = false;
        int rc = mcp_json_bool_value(ctx, body_val, &bval);
        if (rc == MCP_OK) {
            return (bval ? strcmp(decoded_header, "true") == 0 : strcmp(decoded_header, "false") == 0);
        }
        return false; /* body value is not a boolean */
    }
    /* Default: string equality */
    const char *body_str = NULL;
    if (mcp_json_string_value(ctx, body_val, &body_str) == MCP_OK && body_str != NULL) {
        return strcmp(decoded_header, body_str) == 0;
    }
    return false;
}

/* Returns true if the check passes (headers consistent or no annotations).
   On failure copies the failing parameter name into *err_param (buf of at
   least 64 bytes); leaves it as empty string on success. */
static bool xmcp_check_headers(mcp_context_t *ctx, mcp_server_t *server,
                               mcp_http_request_t *req,
                               const mcp_json_value_t *params_obj,
                               const char *tool_name,
                               char *err_param, size_t err_cap) {
    if (err_param != NULL && err_cap > 0) {
        err_param[0] = '\0';
    }

    /* Get the tool's inputSchema */
    const mcp_json_value_t *schema = mcp_server_get_tool_schema(ctx, server, tool_name);
    if (schema == NULL) {
        return true; /* tool not in registry or no schema → skip check */
    }

    /* Collect x-mcp-header annotations */
    xmcp_anno_t annos[XMCP_MAX_ANNOTATIONS];
    int n_annos = xmcp_collect_annotations(ctx, schema, annos, XMCP_MAX_ANNOTATIONS);
    if (n_annos == 0) {
        return true; /* no annotations → nothing to check */
    }

    for (int i = 0; i < n_annos; i++) {
        const xmcp_anno_t *anno = &annos[i];

        /* Read the body parameter value */
        const mcp_json_value_t *body_val =
            (params_obj != NULL) ? mcp_json_object_get(ctx, params_obj, anno->param_name) : NULL;

        /* Construct the header name: Mcp-Param-{HeaderName} */
        char hdr_name_buf[128];
        int n = snprintf(hdr_name_buf, sizeof(hdr_name_buf), "Mcp-Param-%s", anno->header_name);
        if (n <= 0 || (size_t)n >= sizeof(hdr_name_buf)) {
            if (err_param != NULL && err_cap > 0) {
                snprintf(err_param, err_cap, "%s", anno->param_name);
            }
            return false; /* header name too long to form */
        }
        const char *hdr_val = mcp_http_header(ctx, req, hdr_name_buf);

        /* Body has a value but header is missing → fail */
        if (body_val != NULL && hdr_val == NULL) {
            if (err_param != NULL && err_cap > 0) {
                snprintf(err_param, err_cap, "%s", anno->param_name);
            }
            return false;
        }

        /* Header present: decode sentinel and compare */
        if (hdr_val != NULL) {
            char *decoded = NULL;
            mcp_status_t st = mcp_base64_sentinel_decode(ctx, hdr_val, &decoded);
            if (st != MCP_OK) {
                mcp_oauth_free_string(ctx, decoded); /* may be NULL on error */
                if (err_param != NULL && err_cap > 0) {
                    snprintf(err_param, err_cap, "%s", anno->param_name);
                }
                return false;
            }
            if (!xmcp_values_match(anno, decoded, ctx, body_val)) {
                mcp_oauth_free_string(ctx, decoded);
                if (err_param != NULL && err_cap > 0) {
                    snprintf(err_param, err_cap, "%s", anno->param_name);
                }
                return false;
            }
            mcp_oauth_free_string(ctx, decoded);
        }
    }
    return true;
}

static mcp_status_t handle_post(mcp_context_t *ctx, mcp_server_t *server,
                                http_session_table_t *table, mcp_http_io_t *io,
                                mcp_http_request_t *req,
                                mcp_http_auth_fn auth_fn, void *auth_ud,
                                mcp_http_auth_result_v2_fn auth_fn_v2) {
    /* P3 T3: 401 challenges carry the resource_metadata fragment when the
       request's protocol version (MCP-Protocol-Version header, or the
       _meta version once parsed below — the header is the conservative
       gate) is at or above 2025-11-25 AND the host has injected RFC 9728
       protected-resource metadata. Pre-P3 requests keep the bare `Bearer`
       challenge (zero regression). */
    const char *proto_hdr_chk = mcp_http_header(ctx, req, "MCP-Protocol-Version");
    mcp_proto_version_t chk_ver = mcp_proto_version_parse(proto_hdr_chk);
    bool p3_active = mcp_proto_at_least(chk_ver, MCP_PROTO_2025_11_25);
    const mcp_protected_resource_metadata_t *pr =
        mcp_server_get_protected_resource_metadata(ctx, server);
    const char *pr_resource = (p3_active && pr != NULL) ? pr->resource : NULL;

    if (auth_fn != NULL) {
        const char *auth_hdr = mcp_http_header(ctx, req, "Authorization");
        if (auth_hdr == NULL || !ci_prefix_eq(auth_hdr, 7, "bearer ")) {
            return send_unauthorized(ctx, io, NULL, pr_resource, NULL);
        }
        const char *token = auth_hdr + 7;
        if (!auth_fn(ctx, token, auth_ud)) {
            return send_unauthorized(ctx, io, NULL, pr_resource, NULL);
        }
    } else if (auth_fn_v2 != NULL) {
        const char *auth_hdr = mcp_http_header(ctx, req, "Authorization");
        if (auth_hdr == NULL || !ci_prefix_eq(auth_hdr, 7, "bearer ")) {
            return send_unauthorized(ctx, io, NULL, pr_resource, NULL);
        }
        const char *token = auth_hdr + 7;
        mcp_http_auth_result_t res;
        res.code = MCP_HTTP_AUTH_OK;
        res.scope = NULL;
        res.resource_metadata = NULL;
        res.error_description = NULL;
        auth_fn_v2(ctx, token, auth_ud, &res);
        switch (res.code) {
        case MCP_HTTP_AUTH_OK:
            break; /* proceed */
        case MCP_HTTP_AUTH_DENY:
            /* Bare 401 (or resource_metadata only, per P3 T3 gate). */
            return send_unauthorized(ctx, io, NULL, pr_resource, NULL);
        case MCP_HTTP_AUTH_SCOPE_INSUFFICIENT:
            return send_unauthorized(ctx, io, res.scope,
                                    res.resource_metadata != NULL ? res.resource_metadata : pr_resource,
                                    res.error_description);
        default:
            /* Unknown code: treat as DENY so a future enum extension cannot
               ever silently allow a request. */
            return send_unauthorized(ctx, io, NULL, pr_resource, NULL);
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
    const char *proto_hdr = mcp_http_header(ctx, req, "MCP-Protocol-Version");
    const mcp_json_value_t *req_meta = mcp_message_meta(ctx, msg);
    const char *meta_ver = NULL;
    if (req_meta != NULL) {
        const mcp_json_value_t *pv = mcp_json_object_get(ctx, req_meta, "io.modelcontextprotocol/protocolVersion");
        if (pv == NULL) {
            pv = mcp_json_object_get(ctx, req_meta, "protocolVersion");
        }
        if (pv != NULL) {
            mcp_json_string_value(ctx, pv, &meta_ver);
        }
    }
    const char *active_ver = (proto_hdr != NULL) ? proto_hdr : meta_ver;
    bool is_2026 = (active_ver != NULL && strcmp(active_ver, "2026-07-28") == 0);
    if (is_2026) {
        /* P2-T13: 2026 requests ignore Mcp-Session-Id header, do not associate with old session */
        sid = NULL;
    }
    bool is_stateless = (req_meta != NULL) || is_discover || is_2026;
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
            if (active_ver != NULL) {
                mcp_session_set_negotiated(ctx, sess, mcp_proto_version_parse(active_ver));
            }
            if (req_meta != NULL) {
                mcp_session_set_client_meta(ctx, sess, req_meta);
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
    /* P4-a T3: SEP-2243 Mcp-Param-* consistency check (2026 only, session-gated).
       Runs AFTER session resolution (negotiated version is written) and BEFORE
       dispatch. Only fires when sep2243_active is true; all other versions skip
       entirely (zero regression). */
    if (is_2026 && mcp_proto_policy_sep2243_active(ctx, sess)) {
        const char *tool_name = mcp_http_header(ctx, req, "Mcp-Name");
        const char *msg_method = mcp_message_method(ctx, msg);
        if (tool_name != NULL && msg_method != NULL
            && strcmp(msg_method, "tools/call") == 0) {
            /* tools/call params = { name, arguments } — the body parameter
               values live under `params.arguments`, not at the params root. */
            const mcp_json_value_t *params_root = mcp_message_params(ctx, msg);
            const mcp_json_value_t *arguments_obj =
                (params_root != NULL) ? mcp_json_object_get(ctx, params_root, "arguments") : NULL;
            char err_param[64] = {0};
            if (!xmcp_check_headers(ctx, server, req, arguments_obj, tool_name,
                                    err_param, sizeof(err_param))) {
                /* err_param names the failing body parameter; embed it in the
                   error message when available for host diagnostics. */
                char detail[200];
                if (err_param[0] != '\0') {
                    int dn = snprintf(detail, sizeof(detail),
                                      "Mcp-Param header for '%s' does not match body parameter", err_param);
                    if (dn < 0 || (size_t)dn >= sizeof(detail)) {
                        snprintf(detail, sizeof(detail),
                                 "Mcp-Param-* header does not match body parameter");
                    }
                } else {
                    snprintf(detail, sizeof(detail),
                             "Mcp-Param-* header does not match body parameter");
                }
                mcp_status_t er = send_rpc_error(ctx, io, msg, MCP_RPC_XMCP_PARAM_MISMATCH,
                                                 detail);
                mcp_message_destroy(ctx, msg);
                return er;
            }
        }
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

static mcp_status_t handle_get(mcp_context_t *ctx, mcp_server_t *server,
                               mcp_http_io_t *io, mcp_http_request_t *req) {
    /* P3 T3: RFC 9728 protected-resource-metadata + OIDC discovery routes
       are public (no auth) and take precedence over the SSE-ping default. */
    const char *target = mcp_http_request_target(ctx, req);
    if (target != NULL && strcmp(target, "/.well-known/oauth-protected-resource") == 0) {
        const mcp_protected_resource_metadata_t *pr =
            mcp_server_get_protected_resource_metadata(ctx, server);
        if (pr == NULL) {
            return send_bytes(ctx, io, 404, "Not Found", NULL, NULL, 0, NULL, NULL);
        }
        char *json = mcp_oauth_protected_resource_serialize(ctx, pr);
        if (json == NULL) {
            return send_bytes(ctx, io, 500, "Internal Server Error", NULL, NULL, 0, NULL, NULL);
        }
        mcp_status_t st = send_bytes(ctx, io, 200, "OK", "application/json", json,
                                     strlen(json), NULL, NULL);
        mcp_oauth_free_string(ctx, json);
        return st;
    }

    /* OIDC Discovery 1.0 document reuses mcp_oauth_metadata_parse/serialize
       (T5). No route for /.well-known/cim/... yet: that document is client-
       authored and served by the client host, not by this library (P3 spec
       non-goal, issuer auto-discovery deferred to P4). */

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
                                     void *auth_ud,
                                     mcp_http_auth_result_v2_fn auth_fn_v2) {
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
            st = handle_post(ctx, server, &table, io, req, auth_fn, auth_ud, auth_fn_v2);
        } else if (m == MCP_HTTP_GET) {
            st = handle_get(ctx, server, io, req);
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
    return http_serve_impl(ctx, server, io, NULL, NULL, NULL);
}

mcp_status_t mcp_http_serve_with_auth(mcp_context_t *ctx, mcp_server_t *server,
                                       mcp_http_io_t *io, mcp_http_auth_fn auth_fn,
                                       void *auth_user_data) {
    if (server == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return http_serve_impl(ctx, server, io, auth_fn, auth_user_data, NULL);
}

mcp_status_t mcp_http_serve_with_auth_v2(mcp_context_t *ctx, mcp_server_t *server,
                                          mcp_http_io_t *io,
                                          mcp_http_auth_result_v2_fn auth_fn_v2,
                                          void *auth_user_data) {
    if (server == NULL || io == NULL || io->read == NULL || io->write == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return http_serve_impl(ctx, server, io, NULL, auth_user_data, auth_fn_v2);
}
