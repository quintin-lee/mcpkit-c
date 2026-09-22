/**
 * @file http.c
 *
 * Buffer-level HTTP/1.1 request/response parsing. Limits: 64 headers,
 * 8KB per line, 4MB body.
 *
 * Ownership asymmetry: the parsed request OWNS its header strings
 * (strdup'd, freed on destroy) but BORROWS the body pointer from the
 * caller's input buffer — the caller must keep that buffer alive for
 * the request's lifetime. mcp_http_response_set_body stores the
 * caller's pointer (does not copy); the caller must keep the body
 * buffer alive for the response's lifetime. mcp_http_response_serialize()
 * returns a single owned snapshot valid until the next mutation or
 * destroy.
 */
#include "mcpkit/transport/http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"

#define HTTP_MAX_RESP_HEADERS 16

typedef struct {
    char *name;
    char *value;
} http_header_t;

struct mcp_http_request {
    mcp_http_method_t method;
    char *target;
    http_header_t headers[MCP_HTTP_MAX_HEADERS];
    size_t nheaders;
    const char *body;
    size_t body_len;
};

struct mcp_http_response {
    int status;
    char *reason;
    http_header_t headers[HTTP_MAX_RESP_HEADERS];
    size_t nheaders;
    const char *body;
    size_t body_len;
    char *owned_serialized;
};

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

static char *dup_n(mcp_context_t *ctx, const char *s, size_t n) {
    char *out = h_malloc(ctx, n + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static const char *find_crlf(const char *p, const char *end) {
    for (const char *q = p; q + 1 < end; q++) {
        if (q[0] == '\r' && q[1] == '\n') {
            return q;
        }
    }
    return NULL;
}

static const char *find_headers_end(const char *data, size_t len) {
    if (len < 4) {
        return NULL;
    }
    for (size_t i = 0; i + 4 <= len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r'
            && data[i + 3] == '\n') {
            return data + i;
        }
    }
    return NULL;
}

static int header_name_eq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static const char *trim_l(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

static const char *trim_r(const char *p, const char *start) {
    while (p > start && (p[-1] == ' ' || p[-1] == '\t')) {
        p--;
    }
    return p;
}

mcp_http_request_t *mcp_http_parse_request(mcp_context_t *ctx, const char *data, size_t len) {
    if (data == NULL || len == 0) {
        return NULL;
    }
    const char *end = data + len;
    const char *hs_end = find_headers_end(data, len);
    if (hs_end == NULL) {
        return NULL;
    }
    const char *line_end = find_crlf(data, end);
    if (line_end == NULL || line_end > hs_end) {
        return NULL;
    }
    const char *sp1 = memchr(data, ' ', (size_t)(line_end - data));
    if (sp1 == NULL) {
        return NULL;
    }
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - (sp1 + 1)));
    if (sp2 == NULL) {
        return NULL;
    }
    size_t method_len = (size_t)(sp1 - data);
    size_t target_len = (size_t)(sp2 - (sp1 + 1));
    size_t ver_len = (size_t)(line_end - (sp2 + 1));
    if (method_len == 0 || target_len == 0 || ver_len != 8
        || strncmp(sp2 + 1, "HTTP/1.1", 8) != 0) {
        return NULL;
    }
    mcp_http_method_t method = MCP_HTTP_UNKNOWN;
    if (method_len == 3 && strncmp(data, "GET", 3) == 0) {
        method = MCP_HTTP_GET;
    } else if (method_len == 4 && strncmp(data, "POST", 4) == 0) {
        method = MCP_HTTP_POST;
    } else if (method_len == 6 && strncmp(data, "DELETE", 6) == 0) {
        method = MCP_HTTP_DELETE;
    }
    mcp_http_request_t *req = h_malloc(ctx, sizeof(*req));
    if (req == NULL) {
        return NULL;
    }
    memset(req, 0, sizeof(*req));
    req->method = method;
    req->target = dup_n(ctx, sp1 + 1, target_len);
    if (req->target == NULL) {
        h_free(ctx, req);
        return NULL;
    }
    const char *p = line_end + 2;
    while (p < hs_end) {
        const char *eol = find_crlf(p, hs_end + 2);
        if (eol == NULL || eol > hs_end) {
            goto fail;
        }
        if ((size_t)(eol - p) > MCP_HTTP_MAX_LINE) {
            goto fail;
        }
        const char *colon = memchr(p, ':', (size_t)(eol - p));
        if (colon == NULL) {
            goto fail;
        }
        const char *ns = trim_l(p, colon);
        const char *ne = trim_r(colon, ns);
        const char *vs = trim_l(colon + 1, eol);
        const char *ve = trim_r(eol, vs);
        if (ne <= ns) {
            goto fail;
        }
        if (req->nheaders >= MCP_HTTP_MAX_HEADERS) {
            goto fail;
        }
        char *name = dup_n(ctx, ns, (size_t)(ne - ns));
        char *value = dup_n(ctx, vs, (size_t)(ve - vs));
        if (name == NULL || value == NULL) {
            if (name != NULL) {
                h_free(ctx, name);
            }
            goto fail;
        }
        req->headers[req->nheaders].name = name;
        req->headers[req->nheaders].value = value;
        req->nheaders++;
        p = eol + 2;
    }
    const char *cl = NULL;
    for (size_t i = 0; i < req->nheaders; i++) {
        if (header_name_eq(req->headers[i].name, "Content-Length")) {
            cl = req->headers[i].value;
            break;
        }
    }
    const char *body = hs_end + 4;
    size_t avail = (size_t)(end - body);
    if (cl == NULL) {
        req->body = NULL;
        req->body_len = 0;
        return req;
    }
    char *endp = NULL;
    unsigned long v = strtoul(cl, &endp, 10);
    if (endp == cl || *endp != '\0') {
        goto fail;
    }
    size_t want = (size_t)v;
    if ((unsigned long)want != v || want > MCP_HTTP_MAX_BODY || want > avail) {
        goto fail;
    }
    req->body = want > 0 ? body : NULL;
    req->body_len = want;
    return req;

fail:
    mcp_http_request_destroy(ctx, req);
    return NULL;
}

void mcp_http_request_destroy(mcp_context_t *ctx, mcp_http_request_t *req) {
    if (req == NULL) {
        return;
    }
    if (req->target != NULL) {
        h_free(ctx, req->target);
    }
    for (size_t i = 0; i < req->nheaders; i++) {
        h_free(ctx, req->headers[i].name);
        h_free(ctx, req->headers[i].value);
    }
    h_free(ctx, req);
}

mcp_http_method_t mcp_http_request_method(mcp_context_t *ctx, const mcp_http_request_t *req) {
    (void)ctx;
    return req != NULL ? req->method : MCP_HTTP_UNKNOWN;
}

const char *mcp_http_request_target(mcp_context_t *ctx, const mcp_http_request_t *req) {
    (void)ctx;
    return req != NULL ? req->target : NULL;
}

const char *mcp_http_header(mcp_context_t *ctx, const mcp_http_request_t *req, const char *name) {
    (void)ctx;
    if (req == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < req->nheaders; i++) {
        if (header_name_eq(req->headers[i].name, name)) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

const char *mcp_http_request_body(mcp_context_t *ctx, const mcp_http_request_t *req,
                                  size_t *len_out) {
    (void)ctx;
    if (len_out != NULL) {
        *len_out = req != NULL ? req->body_len : 0;
    }
    return req != NULL ? req->body : NULL;
}

bool mcp_http_request_wants_close(mcp_context_t *ctx,
                                  const mcp_http_request_t *req) {
    (void)ctx;
    if (req == NULL) {
        return false;
    }
    const char *v = mcp_http_header(ctx, req, "Connection");
    if (v == NULL) {
        return false;
    }
    for (size_t i = 0; i < 5; i++) {
        if (tolower((unsigned char)v[i]) != "close"[i]) {
            return false;
        }
    }
    /* RFC 7230: Connection is a comma-separated list; must not
     * partially match a longer token like "close-foo". */
    return v[5] == '\0' || v[5] == ',';
}

mcp_http_response_t *mcp_http_response_new(mcp_context_t *ctx, int status, const char *reason) {
    if (reason == NULL) {
        return NULL;
    }
    mcp_http_response_t *resp = h_malloc(ctx, sizeof(*resp));
    if (resp == NULL) {
        return NULL;
    }
    memset(resp, 0, sizeof(*resp));
    resp->status = status;
    resp->reason = dup_n(ctx, reason, strlen(reason));
    if (resp->reason == NULL) {
        h_free(ctx, resp);
        return NULL;
    }
    return resp;
}

mcp_status_t mcp_http_response_set_header(mcp_context_t *ctx, mcp_http_response_t *resp,
                                          const char *name, const char *value) {
    if (resp == NULL || name == NULL || value == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (resp->nheaders >= HTTP_MAX_RESP_HEADERS) {
        return MCP_ERR_NOMEM;
    }
    char *n = dup_n(ctx, name, strlen(name));
    char *v = dup_n(ctx, value, strlen(value));
    if (n == NULL || v == NULL) {
        if (n != NULL) {
            h_free(ctx, n);
        }
        return MCP_ERR_NOMEM;
    }
    resp->headers[resp->nheaders].name = n;
    resp->headers[resp->nheaders].value = v;
    resp->nheaders++;
    return MCP_OK;
}

mcp_status_t mcp_http_response_set_body(mcp_context_t *ctx, mcp_http_response_t *resp,
                                        const char *body, size_t len) {
    (void)ctx;
    if (resp == NULL || (body == NULL && len > 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    resp->body = body;
    resp->body_len = len;
    return MCP_OK;
}

const char *mcp_http_response_serialize(mcp_context_t *ctx, mcp_http_response_t *resp) {
    if (resp == NULL) {
        return NULL;
    }
    size_t cap = 128 + strlen(resp->reason) + resp->body_len + 32;
    for (size_t i = 0; i < resp->nheaders; i++) {
        cap += strlen(resp->headers[i].name) + strlen(resp->headers[i].value) + 4;
    }
    char digits[32];
    int nd = snprintf(digits, sizeof(digits), "%zu", resp->body_len);
    if (nd < 0) {
        return NULL;
    }
    cap += (size_t)nd;
    char *out = h_malloc(ctx, cap + 1);
    if (out == NULL) {
        return NULL;
    }
    int n = snprintf(out, cap + 1, "HTTP/1.1 %d %s\r\n", resp->status, resp->reason);
    if (n < 0) {
        h_free(ctx, out);
        return NULL;
    }
    size_t pos = (size_t)n;
    for (size_t i = 0; i < resp->nheaders; i++) {
        n = snprintf(out + pos, cap + 1 - pos, "%s: %s\r\n", resp->headers[i].name,
                     resp->headers[i].value);
        if (n < 0) {
            h_free(ctx, out);
            return NULL;
        }
        pos += (size_t)n;
    }
    n = snprintf(out + pos, cap + 1 - pos, "Content-Length: %s\r\n\r\n", digits);
    if (n < 0) {
        h_free(ctx, out);
        return NULL;
    }
    pos += (size_t)n;
    if (resp->body != NULL && resp->body_len > 0) {
        memcpy(out + pos, resp->body, resp->body_len);
        pos += resp->body_len;
    }
    out[pos] = '\0';
    if (resp->owned_serialized != NULL) {
        h_free(ctx, resp->owned_serialized);
    }
    resp->owned_serialized = out;
    return out;
}

void mcp_http_response_destroy(mcp_context_t *ctx, mcp_http_response_t *resp) {
    if (resp == NULL) {
        return;
    }
    h_free(ctx, resp->reason);
    for (size_t i = 0; i < resp->nheaders; i++) {
        h_free(ctx, resp->headers[i].name);
        h_free(ctx, resp->headers[i].value);
    }
    if (resp->owned_serialized != NULL) {
        h_free(ctx, resp->owned_serialized);
    }
    h_free(ctx, resp);
}
