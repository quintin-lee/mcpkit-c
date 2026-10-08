/**
 * @file uri_template.c
 * @brief RFC 6570 URI Template matching and expansion (Level 1 & 2).
 */

#include "mcpkit/protocol/uri_template.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"

#define MAX_RAW_CAPTURES 64

typedef struct {
    const char *name_start;
    size_t name_len;
    const char *val_start;
    size_t val_len;
    bool is_reserved;
} raw_capture_t;

typedef struct {
    raw_capture_t items[MAX_RAW_CAPTURES];
    size_t count;
} raw_captures_t;

static inline bool is_unreserved(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

static inline bool is_reserved_char(char c) {
    switch (c) {
        case ':': case '/': case '?': case '#': case '[': case ']': case '@':
        case '!': case '$': case '&': case '\'': case '(': case ')':
        case '*': case '+': case ',': case ';': case '=':
            return true;
        default:
            return false;
    }
}

static inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *url_decode(mcp_context_t *ctx, const char *src, size_t len) {
    const mcp_allocator_t *alloc = mcp_context_allocator(ctx);
    char *out = alloc->malloc_fn(len + 1, alloc->userdata);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len) {
            int h1 = hex_digit(src[i + 1]);
            int h2 = hex_digit(src[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                out[o++] = (char)((h1 << 4) | h2);
                i += 2;
                continue;
            }
        }
        out[o++] = src[i];
    }
    out[o] = '\0';
    return out;
}

typedef struct {
    char op;            /* 0, '+', '#', '/', '.', '?' */
    const char *name;
    size_t name_len;
    bool is_reserved;
    const char *prefix;
    bool is_query;
} parsed_var_expr_t;

static bool parse_var_expr(const char *start, const char *end, parsed_var_expr_t *out) {
    if (start >= end) return false;
    char first = *start;
    if (first == '+' || first == '#' || first == '/' || first == '.' || first == '?') {
        out->op = first;
        out->name = start + 1;
        out->name_len = (size_t)(end - (start + 1));
        if (out->name_len == 0) return false;
        out->is_reserved = (first == '+' || first == '#');
        out->is_query = (first == '?');
        if (first == '+') out->prefix = "";
        else if (first == '#') out->prefix = "#";
        else if (first == '/') out->prefix = "/";
        else if (first == '.') out->prefix = ".";
        else if (first == '?') out->prefix = "?";
        else out->prefix = "";
    } else {
        out->op = 0;
        out->name = start;
        out->name_len = (size_t)(end - start);
        if (out->name_len == 0) return false;
        out->is_reserved = false;
        out->is_query = false;
        out->prefix = "";
    }
    return true;
}

static bool match_step(const char *p, const char *u, raw_captures_t *caps) {
    while (*p != '\0' && *p != '{') {
        if (*p != *u) {
            return false;
        }
        p++;
        u++;
    }

    if (*p == '\0') {
        return *u == '\0';
    }

    // *p == '{'
    const char *end = strchr(p, '}');
    if (end == NULL) {
        return false;
    }

    parsed_var_expr_t expr;
    if (!parse_var_expr(p + 1, end, &expr)) {
        return false;
    }

    const char *next_p = end + 1;
    const char *u_val = u;

    if (expr.op == '#') {
        if (*u_val != '#') return false;
        u_val++;
    } else if (expr.op == '/') {
        if (*u_val != '/') return false;
        u_val++;
    } else if (expr.op == '.') {
        if (*u_val != '.') return false;
        u_val++;
    } else if (expr.op == '?') {
        if (*u_val != '?') return false;
        u_val++;
        if (strncmp(u_val, expr.name, expr.name_len) != 0 || u_val[expr.name_len] != '=') {
            return false;
        }
        u_val += expr.name_len + 1;
    }

    // If next_p == '\0', variable consumes rest of u_val
    if (*next_p == '\0') {
        if (!expr.is_reserved && strchr(u_val, '/') != NULL) {
            return false;
        }
        if (expr.op == '?' && strchr(u_val, '&') != NULL) {
            return false;
        }
        if (caps->count < MAX_RAW_CAPTURES) {
            caps->items[caps->count++] = (raw_capture_t){
                .name_start = expr.name,
                .name_len = expr.name_len,
                .val_start = u_val,
                .val_len = strlen(u_val),
                .is_reserved = expr.is_reserved
            };
            return true;
        }
        return false;
    }

    // next_p != '\0': try matching different lengths of u_val
    size_t u_len = strlen(u_val);
    for (size_t len = 0; len <= u_len; len++) {
        if (!expr.is_reserved && len > 0 && u_val[len - 1] == '/') {
            break;
        }
        if (expr.op == '?' && len > 0 && (u_val[len - 1] == '&' || u_val[len - 1] == '#')) {
            break;
        }
        size_t saved_count = caps->count;
        if (saved_count < MAX_RAW_CAPTURES) {
            caps->items[caps->count++] = (raw_capture_t){
                .name_start = expr.name,
                .name_len = expr.name_len,
                .val_start = u_val,
                .val_len = len,
                .is_reserved = expr.is_reserved
            };
            if (match_step(next_p, u_val + len, caps)) {
                return true;
            }
            caps->count = saved_count;
        }
    }

    return false;
}

mcp_status_t mcp_uri_template_match(mcp_context_t *ctx,
                                    const char *pattern,
                                    const char *uri,
                                    mcp_json_value_t **out_variables) {
    if (pattern == NULL || uri == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (out_variables != NULL) {
        *out_variables = NULL;
    }

    // Validate pattern syntax
    const char *scan = pattern;
    while (*scan != '\0') {
        if (*scan == '{') {
            const char *close = strchr(scan, '}');
            if (close == NULL) {
                return MCP_ERR_INVALID_ARGUMENT;
            }
            if (close == scan + 1) {
                return MCP_ERR_INVALID_ARGUMENT;
            }
            scan = close + 1;
        } else if (*scan == '}') {
            return MCP_ERR_INVALID_ARGUMENT;
        } else {
            scan++;
        }
    }

    raw_captures_t caps;
    caps.count = 0;

    if (!match_step(pattern, uri, &caps)) {
        return MCP_ERR_NOT_FOUND;
    }

    if (out_variables == NULL) {
        return MCP_OK;
    }

    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        return MCP_ERR_NOMEM;
    }

    const mcp_allocator_t *alloc = mcp_context_allocator(ctx);

    for (size_t i = 0; i < caps.count; i++) {
        char name[128];
        size_t nlen = caps.items[i].name_len;
        if (nlen >= sizeof(name)) {
            nlen = sizeof(name) - 1;
        }
        memcpy(name, caps.items[i].name_start, nlen);
        name[nlen] = '\0';

        char *decoded = url_decode(ctx, caps.items[i].val_start, caps.items[i].val_len);
        if (decoded == NULL) {
            mcp_json_destroy(ctx, obj);
            return MCP_ERR_NOMEM;
        }

        mcp_json_value_t *val_v = mcp_json_string_new(ctx, decoded);
        alloc->free_fn(decoded, alloc->userdata);
        if (val_v == NULL || mcp_json_object_set_take(ctx, obj, name, val_v) != MCP_OK) {
            mcp_json_destroy(ctx, val_v);
            mcp_json_destroy(ctx, obj);
            return MCP_ERR_NOMEM;
        }
    }

    *out_variables = obj;
    return MCP_OK;
}

mcp_status_t mcp_uri_template_expand(mcp_context_t *ctx,
                                     const char *pattern,
                                     const mcp_json_value_t *variables,
                                     char **out_uri) {
    if (pattern == NULL || out_uri == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out_uri = NULL;

    const mcp_allocator_t *alloc = mcp_context_allocator(ctx);
    size_t cap = 256;
    char *buf = alloc->malloc_fn(cap, alloc->userdata);
    if (buf == NULL) {
        return MCP_ERR_NOMEM;
    }
    size_t len = 0;
    buf[0] = '\0';

    const char *p = pattern;
    while (*p != '\0') {
        if (*p != '{') {
            if (len + 1 >= cap) {
                size_t new_cap = cap * 2;
                char *new_buf = alloc->realloc_fn(buf, new_cap, alloc->userdata);
                if (new_buf == NULL) {
                    alloc->free_fn(buf, alloc->userdata);
                    return MCP_ERR_NOMEM;
                }
                buf = new_buf;
                cap = new_cap;
            }
            buf[len++] = *p++;
            buf[len] = '\0';
            continue;
        }

        // *p == '{'
        const char *end = strchr(p, '}');
        if (end == NULL) {
            alloc->free_fn(buf, alloc->userdata);
            return MCP_ERR_INVALID_ARGUMENT;
        }

        parsed_var_expr_t expr;
        if (!parse_var_expr(p + 1, end, &expr)) {
            alloc->free_fn(buf, alloc->userdata);
            return MCP_ERR_INVALID_ARGUMENT;
        }

        char name[128];
        if (expr.name_len >= sizeof(name)) {
            expr.name_len = sizeof(name) - 1;
        }
        memcpy(name, expr.name, expr.name_len);
        name[expr.name_len] = '\0';

        p = end + 1;

        const char *val_str = NULL;
        char num_buf[64];
        if (variables != NULL) {
            const mcp_json_value_t *v = mcp_json_object_get(ctx, variables, name);
            if (v != NULL) {
                if (mcp_json_string_value(ctx, v, &val_str) != MCP_OK || val_str == NULL) {
                    if (mcp_json_type(ctx, v) == MCP_JSON_NUMBER) {
                        double d = 0;
                        if (mcp_json_number_value(ctx, v, &d) == MCP_OK) {
                            snprintf(num_buf, sizeof(num_buf), "%g", d);
                            val_str = num_buf;
                        }
                    }
                }
            }
        }

        if (val_str == NULL) {
            // undefined variable expands to empty string
            continue;
        }

        if (expr.prefix[0] != '\0') {
            size_t plen = strlen(expr.prefix);
            while (len + plen >= cap) {
                size_t new_cap = cap * 2;
                char *new_buf = alloc->realloc_fn(buf, new_cap, alloc->userdata);
                if (new_buf == NULL) {
                    alloc->free_fn(buf, alloc->userdata);
                    return MCP_ERR_NOMEM;
                }
                buf = new_buf;
                cap = new_cap;
            }
            memcpy(buf + len, expr.prefix, plen);
            len += plen;
            buf[len] = '\0';
        }

        if (expr.is_query) {
            size_t nlen = strlen(name);
            while (len + nlen + 1 >= cap) {
                size_t new_cap = cap * 2;
                char *new_buf = alloc->realloc_fn(buf, new_cap, alloc->userdata);
                if (new_buf == NULL) {
                    alloc->free_fn(buf, alloc->userdata);
                    return MCP_ERR_NOMEM;
                }
                buf = new_buf;
                cap = new_cap;
            }
            memcpy(buf + len, name, nlen);
            len += nlen;
            buf[len++] = '=';
            buf[len] = '\0';
        }

        for (const char *s = val_str; *s != '\0'; s++) {
            char c = *s;
            bool allowed = is_unreserved(c) || (expr.is_reserved && is_reserved_char(c));
            if (allowed) {
                if (len + 1 >= cap) {
                    size_t new_cap = cap * 2;
                    char *new_buf = alloc->realloc_fn(buf, new_cap, alloc->userdata);
                    if (new_buf == NULL) {
                        alloc->free_fn(buf, alloc->userdata);
                        return MCP_ERR_NOMEM;
                    }
                    buf = new_buf;
                    cap = new_cap;
                }
                buf[len++] = c;
            } else {
                if (len + 3 >= cap) {
                    size_t new_cap = cap * 2 + 16;
                    char *new_buf = alloc->realloc_fn(buf, new_cap, alloc->userdata);
                    if (new_buf == NULL) {
                        alloc->free_fn(buf, alloc->userdata);
                        return MCP_ERR_NOMEM;
                    }
                    buf = new_buf;
                    cap = new_cap;
                }
                static const char hex[] = "0123456789ABCDEF";
                buf[len++] = '%';
                buf[len++] = hex[(unsigned char)c >> 4];
                buf[len++] = hex[(unsigned char)c & 0x0F];
            }
            buf[len] = '\0';
        }
    }

    *out_uri = buf;
    return MCP_OK;
}

void mcp_uri_template_free_string(mcp_context_t *ctx, char *uri) {
    if (uri == NULL) {
        return;
    }
    const mcp_allocator_t *alloc = mcp_context_allocator(ctx);
    alloc->free_fn(uri, alloc->userdata);
}
