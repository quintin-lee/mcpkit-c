/**
 * @file builtin.c
 *
 * Built-in JSON backend: strict-JSON DOM, parser, serializer, and
 * UTF-8 validation (Hoehrmann DFA).
 *
 * This translation unit implements mcp_json_backend_ops_t and
 * registers it as the process-wide built-in. It is the reference
 * backend; user code can swap it per-context via
 * mcp_json_set_backend.
 *
 * Concurrency: this module is allocation-only and holds no global
 * mutable state beyond the static ops table, so it is safe to use
 * from multiple threads simultaneously.
 */
#include "mcpkit/json/json.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/core/context.h"

/**
 * Private JSON DOM node. Opaque to public API consumers.
 *
 * - Scalar types (NULL, BOOL, NUMBER, STRING) use the corresponding
 *   union member.
 * - ARRAY and OBJECT carry growable backing arrays; `cap` doubles
 *   on overflow. The object variant stores parallel keys[] and vals[]
 *   arrays (insertion order preserved).
 */
struct mcp_json_value {
    mcp_json_type_t type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            mcp_json_value_t **items;
            size_t len;
            size_t cap;
        } array;
        struct {
            char **keys;
            mcp_json_value_t **vals;
            size_t len;
            size_t cap;
        } object;
    } u;
};

// --- Allocator routing ---

/**
 * Resolves the allocator for a context: the ctx's bound allocator,
 * or the default libc allocator when ctx is NULL. This is the only
 * path through which builtin.c allocates or frees memory, so the
 * "free with the same ctx that allocated" rule is enforced here.
 */
static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static void *xmalloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = alloc_of(ctx);
    return a->malloc_fn(n, a->userdata);
}

static void xfree(mcp_context_t *ctx, void *p) {
    if (p == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(p, a->userdata);
}

static void *xrealloc(mcp_context_t *ctx, void *p, size_t n) {
    const mcp_allocator_t *a = alloc_of(ctx);
    return a->realloc_fn(p, n, a->userdata);
}

// --- Hoehrmann UTF-8 DFA ---
// State table (little-endian) for validating each byte of a UTF-8
// sequence. The DFA is derived from the public-domain implementation
// at http://brynh_hash.github.io/utf8/; entries are (type, next-state)
// pairs: byte types 2–6 are the 1–5 byte sequence start/continue
// classes, 0/1 are ASCII/invalid, 7–9 are UTF-16 surrogate halves
// that appear as bytes inside well-formed 3/4-byte sequences.

static const uint8_t k_utf8d[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    8, 8, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    0xa, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x3, 0x4, 0x3, 0x3,
    0xb, 0x6, 0x6, 0x6, 0x5, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0x8,
    0x0, 0x1, 0x2, 0x3, 0x5, 0x8, 0x7, 0x1, 0x1, 0x1, 0x4, 0x6, 0x1, 0x1, 0x1, 0x1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1,
    1, 2, 1, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 1, 1, 1,
    1, 3, 1, 1, 1, 1, 1, 3, 1, 3, 1, 1, 1, 1, 1, 1, 1, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

#define UTF8_ACCEPT 0u
#define UTF8_REJECT 1u

static uint32_t utf8_step(uint32_t *state, uint8_t byte) {
    uint32_t type = k_utf8d[byte];
    *state = k_utf8d[256 + *state * 16 + type];
    return *state;
}

static bool utf8_valid(const char *s, size_t n) {
    uint32_t state = UTF8_ACCEPT;
    for (size_t i = 0; i < n; i++) {
        if (utf8_step(&state, (uint8_t)s[i]) == UTF8_REJECT) {
            return false;
        }
    }
    return state == UTF8_ACCEPT;
}

static size_t utf8_encode(char out[4], uint32_t cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// --- DOM core ---

static mcp_json_value_t *value_new(mcp_context_t *ctx, mcp_json_type_t type) {
    mcp_json_value_t *v = xmalloc(ctx, sizeof(*v));
    if (v == NULL) {
        return NULL;
    }
    v->type = type;
    return v;
}

static void builtin_destroy(mcp_context_t *ctx, mcp_json_value_t *v) {
    if (v == NULL) {
        return;
    }
    size_t i;
    switch (v->type) {
        case MCP_JSON_STRING:
            xfree(ctx, v->u.string);
            break;
        case MCP_JSON_ARRAY:
            for (i = 0; i < v->u.array.len; i++) {
                builtin_destroy(ctx, v->u.array.items[i]);
            }
            xfree(ctx, v->u.array.items);
            break;
        case MCP_JSON_OBJECT:
            for (i = 0; i < v->u.object.len; i++) {
                xfree(ctx, v->u.object.keys[i]);
                builtin_destroy(ctx, v->u.object.vals[i]);
            }
            xfree(ctx, v->u.object.keys);
            xfree(ctx, v->u.object.vals);
            break;
        default:
            break;
    }
    xfree(ctx, v);
}

static mcp_json_type_t builtin_type_of(mcp_context_t *ctx, const mcp_json_value_t *v) {
    (void)ctx;
    return v != NULL ? v->type : MCP_JSON_NULL;
}

static mcp_status_t builtin_get_bool(mcp_context_t *ctx, const mcp_json_value_t *v, bool *out) {
    (void)ctx;
    if (v == NULL || out == NULL || v->type != MCP_JSON_BOOL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = v->u.boolean;
    return MCP_OK;
}

static mcp_status_t builtin_get_number(mcp_context_t *ctx, const mcp_json_value_t *v, double *out) {
    (void)ctx;
    if (v == NULL || out == NULL || v->type != MCP_JSON_NUMBER) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = v->u.number;
    return MCP_OK;
}

static mcp_status_t builtin_get_string(mcp_context_t *ctx, const mcp_json_value_t *v,
                                       const char **out) {
    (void)ctx;
    if (v == NULL || out == NULL || v->type != MCP_JSON_STRING) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = v->u.string;
    return MCP_OK;
}

static mcp_json_value_t *builtin_new_null(mcp_context_t *ctx) {
    return value_new(ctx, MCP_JSON_NULL);
}

static mcp_json_value_t *builtin_new_bool(mcp_context_t *ctx, bool b) {
    mcp_json_value_t *v = value_new(ctx, MCP_JSON_BOOL);
    if (v != NULL) {
        v->u.boolean = b;
    }
    return v;
}

static mcp_json_value_t *builtin_new_number(mcp_context_t *ctx, double d) {
    if (!isfinite(d)) {
        return NULL;
    }
    mcp_json_value_t *v = value_new(ctx, MCP_JSON_NUMBER);
    if (v != NULL) {
        v->u.number = d;
    }
    return v;
}

static mcp_json_value_t *builtin_new_string_n(mcp_context_t *ctx, const char *s, size_t n) {
    if (s == NULL || memchr(s, '\0', n) != NULL || !utf8_valid(s, n)) {
        return NULL;
    }
    mcp_json_value_t *v = value_new(ctx, MCP_JSON_STRING);
    if (v == NULL) {
        return NULL;
    }
    v->u.string = xmalloc(ctx, n + 1);
    if (v->u.string == NULL) {
        xfree(ctx, v);
        return NULL;
    }
    memcpy(v->u.string, s, n);
    v->u.string[n] = '\0';
    return v;
}

static mcp_json_value_t *builtin_new_array(mcp_context_t *ctx) {
    mcp_json_value_t *v = value_new(ctx, MCP_JSON_ARRAY);
    if (v != NULL) {
        v->u.array.items = NULL;
        v->u.array.len = 0;
        v->u.array.cap = 0;
    }
    return v;
}

static mcp_json_value_t *builtin_new_object(mcp_context_t *ctx) {
    mcp_json_value_t *v = value_new(ctx, MCP_JSON_OBJECT);
    if (v != NULL) {
        v->u.object.keys = NULL;
        v->u.object.vals = NULL;
        v->u.object.len = 0;
        v->u.object.cap = 0;
    }
    return v;
}

static mcp_status_t builtin_object_set(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key,
                                       mcp_json_value_t *val) {
    if (obj == NULL || obj->type != MCP_JSON_OBJECT || key == NULL || val == NULL ||
        !utf8_valid(key, strlen(key))) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < obj->u.object.len; i++) {
        if (strcmp(obj->u.object.keys[i], key) == 0) {
            builtin_destroy(ctx, obj->u.object.vals[i]);
            obj->u.object.vals[i] = val;
            return MCP_OK;
        }
    }
    if (obj->u.object.len == obj->u.object.cap) {
        size_t ncap = obj->u.object.cap != 0 ? obj->u.object.cap * 2 : 4;
        char **nkeys = xrealloc(ctx, obj->u.object.keys, ncap * sizeof(*nkeys));
        if (nkeys == NULL) {
            return MCP_ERR_NOMEM;
        }
        obj->u.object.keys = nkeys;
        mcp_json_value_t **nvals = xrealloc(ctx, obj->u.object.vals, ncap * sizeof(*nvals));
        if (nvals == NULL) {
            return MCP_ERR_NOMEM;
        }
        obj->u.object.vals = nvals;
        obj->u.object.cap = ncap;
    }
    size_t klen = strlen(key);
    char *kcopy = xmalloc(ctx, klen + 1);
    if (kcopy == NULL) {
        return MCP_ERR_NOMEM;
    }
    memcpy(kcopy, key, klen + 1);
    obj->u.object.keys[obj->u.object.len] = kcopy;
    obj->u.object.vals[obj->u.object.len] = val;
    obj->u.object.len++;
    return MCP_OK;
}

static const mcp_json_value_t *builtin_object_get(mcp_context_t *ctx, const mcp_json_value_t *obj,
                                                 const char *key) {
    (void)ctx;
    if (obj == NULL || obj->type != MCP_JSON_OBJECT || key == NULL ||
        !utf8_valid(key, strlen(key))) {
        return NULL;
    }
    for (size_t i = 0; i < obj->u.object.len; i++) {
        if (strcmp(obj->u.object.keys[i], key) == 0) {
            return obj->u.object.vals[i];
        }
    }
    return NULL;
}

static bool builtin_object_has(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key) {
    return builtin_object_get(ctx, obj, key) != NULL;
}

static size_t builtin_object_size(mcp_context_t *ctx, const mcp_json_value_t *obj) {
    (void)ctx;
    if (obj == NULL || obj->type != MCP_JSON_OBJECT) {
        return 0;
    }
    return obj->u.object.len;
}

static const char *builtin_object_key_at(mcp_context_t *ctx, const mcp_json_value_t *obj,
                                         size_t i) {
    (void)ctx;
    if (obj == NULL || obj->type != MCP_JSON_OBJECT || i >= obj->u.object.len) {
        return NULL;
    }
    return obj->u.object.keys[i];
}

static mcp_status_t builtin_array_append(mcp_context_t *ctx, mcp_json_value_t *arr,
                                         mcp_json_value_t *val) {
    if (arr == NULL || arr->type != MCP_JSON_ARRAY || val == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (arr->u.array.len == arr->u.array.cap) {
        size_t ncap = arr->u.array.cap != 0 ? arr->u.array.cap * 2 : 4;
        mcp_json_value_t **nitems = xrealloc(ctx, arr->u.array.items, ncap * sizeof(*nitems));
        if (nitems == NULL) {
            return MCP_ERR_NOMEM;
        }
        arr->u.array.items = nitems;
        arr->u.array.cap = ncap;
    }
    arr->u.array.items[arr->u.array.len++] = val;
    return MCP_OK;
}

static const mcp_json_value_t *builtin_array_get(mcp_context_t *ctx, const mcp_json_value_t *arr,
                                                size_t i) {
    (void)ctx;
    if (arr == NULL || arr->type != MCP_JSON_ARRAY || i >= arr->u.array.len) {
        return NULL;
    }
    return arr->u.array.items[i];
}

static size_t builtin_array_size(mcp_context_t *ctx, const mcp_json_value_t *arr) {
    (void)ctx;
    if (arr == NULL || arr->type != MCP_JSON_ARRAY) {
        return 0;
    }
    return arr->u.array.len;
}

// --- Parser ---

typedef struct {
    const char *p;
    const char *end;
    mcp_context_t *ctx;
    int depth;
} parser_t;

static void skip_ws(parser_t *ps) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        ps->p++;
    }
}

static mcp_json_value_t *parse_value(parser_t *ps);

static bool match_lit(parser_t *ps, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) {
        return false;
    }
    ps->p += n;
    return true;
}

static bool scan_number(const char *p, const char *end, size_t *out_len) {
    const char *s = p;
    if (s < end && *s == '-') {
        s++;
    }
    if (s >= end) {
        return false;
    }
    if (*s == '0') {
        s++;
    } else if (*s >= '1' && *s <= '9') {
        while (s < end && *s >= '0' && *s <= '9') {
            s++;
        }
    } else {
        return false;
    }
    if (s < end && *s == '.') {
        s++;
        const char *f = s;
        while (s < end && *s >= '0' && *s <= '9') {
            s++;
        }
        if (s == f) {
            return false;
        }
    }
    if (s < end && (*s == 'e' || *s == 'E')) {
        s++;
        if (s < end && (*s == '+' || *s == '-')) {
            s++;
        }
        const char *e = s;
        while (s < end && *s >= '0' && *s <= '9') {
            s++;
        }
        if (s == e) {
            return false;
        }
    }
    *out_len = (size_t)(s - p);
    return true;
}

static mcp_json_value_t *parse_number(parser_t *ps) {
    size_t len = 0;
    if (!scan_number(ps->p, ps->end, &len)) {
        return NULL;
    }
    char *tmp = xmalloc(ps->ctx, len + 1);
    if (tmp == NULL) {
        return NULL;
    }
    memcpy(tmp, ps->p, len);
    tmp[len] = '\0';
    char *eptr = NULL;
    double d = strtod(tmp, &eptr);
    bool ok = eptr != NULL && *eptr == '\0' && isfinite(d);
    xfree(ps->ctx, tmp);
    if (!ok) {
        return NULL;
    }
    ps->p += len;
    return builtin_new_number(ps->ctx, d);
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    mcp_context_t *ctx;
    bool ok;
} growbuf_t;

static void grow_append(growbuf_t *b, const char *s, size_t n) {
    if (!b->ok) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap != 0 ? b->cap : 32;
        while (ncap < b->len + n + 1) {
            ncap *= 2;
        }
        char *nd = xrealloc(b->ctx, b->data, ncap);
        if (nd == NULL) {
            b->ok = false;
            return;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static bool parse_hex4(parser_t *ps, uint32_t *out) {
    if (ps->end - ps->p < 4) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    ps->p += 4;
    *out = v;
    return true;
}

static bool parse_unicode_escape(parser_t *ps, growbuf_t *b) {
    uint32_t hi = 0;
    if (!parse_hex4(ps, &hi)) {
        return false;
    }
    uint32_t cp = hi;
    if (hi >= 0xD800 && hi <= 0xDBFF) {
        if (ps->end - ps->p < 6 || ps->p[0] != '\\' || ps->p[1] != 'u') {
            return false;
        }
        ps->p += 2;
        uint32_t lo = 0;
        if (!parse_hex4(ps, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
            return false;
        }
        cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
    } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
        return false;
    }
    if (cp > 0x10FFFF) {
        return false;
    }
    char enc[4];
    grow_append(b, enc, utf8_encode(enc, cp));
    return b->ok;
}

static char *parse_string_raw(parser_t *ps) {
    // ps->p points at opening quote. Returns owned NUL-terminated string or NULL.
    ps->p++;
    growbuf_t b = {.data = NULL, .len = 0, .cap = 0, .ctx = ps->ctx, .ok = true};
    bool closed = false;
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            closed = true;
            ps->p++;
            break;
        }
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) {
                break;
            }
            char e = *ps->p++;
            switch (e) {
                case '"':
                    grow_append(&b, "\"", 1);
                    break;
                case '\\':
                    grow_append(&b, "\\", 1);
                    break;
                case '/':
                    grow_append(&b, "/", 1);
                    break;
                case 'b':
                    grow_append(&b, "\b", 1);
                    break;
                case 'f':
                    grow_append(&b, "\f", 1);
                    break;
                case 'n':
                    grow_append(&b, "\n", 1);
                    break;
                case 'r':
                    grow_append(&b, "\r", 1);
                    break;
                case 't':
                    grow_append(&b, "\t", 1);
                    break;
                case 'u':
                    if (!parse_unicode_escape(ps, &b)) {
                        closed = false;
                        goto done;
                    }
                    break;
                default:
                    goto done;
            }
            if (!b.ok) {
                goto done;
            }
        } else if (c < 0x20) {
            goto done;
        } else {
            grow_append(&b, ps->p, 1);
            ps->p++;
        }
    }
done:
    if (!closed || !b.ok || !utf8_valid(b.data != NULL ? b.data : "", b.len)) {
        xfree(ps->ctx, b.data);
        return NULL;
    }
    if (b.data == NULL) {
        b.data = xmalloc(ps->ctx, 1);
        if (b.data == NULL) {
            return NULL;
        }
        b.data[0] = '\0';
    }
    return b.data;
}

static mcp_json_value_t *parse_array(parser_t *ps) {
    if (++ps->depth > MCP_JSON_MAX_DEPTH) {
        ps->depth--;
        return NULL;
    }
    ps->p++;
    mcp_json_value_t *arr = builtin_new_array(ps->ctx);
    if (arr == NULL) {
        ps->depth--;
        return NULL;
    }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        ps->depth--;
        return arr;
    }
    for (;;) {
        skip_ws(ps);
        mcp_json_value_t *item = parse_value(ps);
        if (item == NULL) {
            builtin_destroy(ps->ctx, arr);
            ps->depth--;
            return NULL;
        }
        if (builtin_array_append(ps->ctx, arr, item) != MCP_OK) {
            builtin_destroy(ps->ctx, item);
            builtin_destroy(ps->ctx, arr);
            ps->depth--;
            return NULL;
        }
        skip_ws(ps);
        if (ps->p >= ps->end) {
            builtin_destroy(ps->ctx, arr);
            ps->depth--;
            return NULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            ps->depth--;
            return arr;
        }
        builtin_destroy(ps->ctx, arr);
        ps->depth--;
        return NULL;
    }
}

static mcp_json_value_t *parse_object(parser_t *ps) {
    if (++ps->depth > MCP_JSON_MAX_DEPTH) {
        ps->depth--;
        return NULL;
    }
    ps->p++;
    mcp_json_value_t *obj = builtin_new_object(ps->ctx);
    if (obj == NULL) {
        ps->depth--;
        return NULL;
    }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        ps->depth--;
        return obj;
    }
    for (;;) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            builtin_destroy(ps->ctx, obj);
            ps->depth--;
            return NULL;
        }
        char *key = parse_string_raw(ps);
        if (key == NULL) {
            builtin_destroy(ps->ctx, obj);
            ps->depth--;
            return NULL;
        }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            xfree(ps->ctx, key);
            builtin_destroy(ps->ctx, obj);
            ps->depth--;
            return NULL;
        }
        ps->p++;
        skip_ws(ps);
        mcp_json_value_t *val = parse_value(ps);
        if (val == NULL) {
            xfree(ps->ctx, key);
            builtin_destroy(ps->ctx, obj);
            ps->depth--;
            return NULL;
        }
        mcp_status_t st = builtin_object_set(ps->ctx, obj, key, val);
        xfree(ps->ctx, key);
        if (st != MCP_OK) {
            builtin_destroy(ps->ctx, val);
            builtin_destroy(ps->ctx, obj);
            ps->depth--;
            return NULL;
        }
        skip_ws(ps);
        if (ps->p >= ps->end) {
            builtin_destroy(ps->ctx, obj);
            ps->depth--;
            return NULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            ps->depth--;
            return obj;
        }
        builtin_destroy(ps->ctx, obj);
        ps->depth--;
        return NULL;
    }
}

static mcp_json_value_t *parse_value(parser_t *ps) {
    if (ps->p >= ps->end) {
        return NULL;
    }
    switch (*ps->p) {
        case 'n':
            return match_lit(ps, "null") ? builtin_new_null(ps->ctx) : NULL;
        case 't':
            return match_lit(ps, "true") ? builtin_new_bool(ps->ctx, true) : NULL;
        case 'f':
            return match_lit(ps, "false") ? builtin_new_bool(ps->ctx, false) : NULL;
        case '"': {
            char *s = parse_string_raw(ps);
            if (s == NULL) {
                return NULL;
            }
            mcp_json_value_t *v = builtin_new_string_n(ps->ctx, s, strlen(s));
            xfree(ps->ctx, s);
            return v;
        }
        case '[':
            return parse_array(ps);
        case '{':
            return parse_object(ps);
        default:
            return parse_number(ps);
    }
}

static mcp_json_value_t *builtin_parse(mcp_context_t *ctx, const char *text, size_t len) {
    if (text == NULL || !utf8_valid(text, len)) {
        return NULL;
    }
    parser_t ps = {.p = text, .end = text + len, .ctx = ctx, .depth = 0};
    skip_ws(&ps);
    mcp_json_value_t *v = parse_value(&ps);
    if (v == NULL) {
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        builtin_destroy(ctx, v);
        return NULL;
    }
    return v;
}

// --- Serializer ---

static void ser_value(growbuf_t *b, const mcp_json_value_t *v);

static void ser_string(growbuf_t *b, const char *s) {
    static const char *k_hex = "0123456789abcdef";
    grow_append(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        switch (*p) {
            case '"':
                grow_append(b, "\\\"", 2);
                break;
            case '\\':
                grow_append(b, "\\\\", 2);
                break;
            case '\b':
                grow_append(b, "\\b", 2);
                break;
            case '\f':
                grow_append(b, "\\f", 2);
                break;
            case '\n':
                grow_append(b, "\\n", 2);
                break;
            case '\r':
                grow_append(b, "\\r", 2);
                break;
            case '\t':
                grow_append(b, "\\t", 2);
                break;
            default:
                if (*p < 0x20) {
                    char esc[6] = {'\\', 'u', '0', '0', k_hex[*p >> 4], k_hex[*p & 0xF]};
                    grow_append(b, esc, 6);
                } else {
                    grow_append(b, (const char *)p, 1);
                }
                break;
        }
        if (!b->ok) {
            return;
        }
    }
    grow_append(b, "\"", 1);
}

static void ser_value(growbuf_t *b, const mcp_json_value_t *v) {
    if (!b->ok || v == NULL) {
        b->ok = false;
        return;
    }
    switch (v->type) {
        case MCP_JSON_NULL:
            grow_append(b, "null", 4);
            break;
        case MCP_JSON_BOOL:
            grow_append(b, v->u.boolean ? "true" : "false", v->u.boolean ? 4 : 5);
            break;
        case MCP_JSON_NUMBER: {
            char nb[32];
            snprintf(nb, sizeof(nb), "%.17g", v->u.number);
            grow_append(b, nb, strlen(nb));
            break;
        }
        case MCP_JSON_STRING:
            ser_string(b, v->u.string);
            break;
        case MCP_JSON_ARRAY:
            grow_append(b, "[", 1);
            for (size_t i = 0; i < v->u.array.len; i++) {
                if (i > 0) {
                    grow_append(b, ",", 1);
                }
                ser_value(b, v->u.array.items[i]);
            }
            grow_append(b, "]", 1);
            break;
        case MCP_JSON_OBJECT:
            grow_append(b, "{", 1);
            for (size_t i = 0; i < v->u.object.len; i++) {
                if (i > 0) {
                    grow_append(b, ",", 1);
                }
                ser_string(b, v->u.object.keys[i]);
                grow_append(b, ":", 1);
                ser_value(b, v->u.object.vals[i]);
            }
            grow_append(b, "}", 1);
            break;
    }
}

static char *builtin_serialize(mcp_context_t *ctx, const mcp_json_value_t *value) {
    if (value == NULL) {
        return NULL;
    }
    growbuf_t b = {.data = NULL, .len = 0, .cap = 0, .ctx = ctx, .ok = true};
    ser_value(&b, value);
    if (!b.ok) {
        xfree(ctx, b.data);
        return NULL;
    }
    if (b.data == NULL) {
        b.data = xmalloc(ctx, 1);
        if (b.data == NULL) {
            return NULL;
        }
        b.data[0] = '\0';
    }
    return b.data;
}

static void builtin_free_string(mcp_context_t *ctx, char *s) {
    xfree(ctx, s);
}

static const mcp_json_backend_ops_t k_builtin_backend = {
    .name = "builtin",
    .parse = builtin_parse,
    .serialize = builtin_serialize,
    .free_string = builtin_free_string,
    .destroy = builtin_destroy,
    .type_of = builtin_type_of,
    .get_bool = builtin_get_bool,
    .get_number = builtin_get_number,
    .get_string = builtin_get_string,
    .new_null = builtin_new_null,
    .new_bool = builtin_new_bool,
    .new_number = builtin_new_number,
    .new_string_n = builtin_new_string_n,
    .new_array = builtin_new_array,
    .new_object = builtin_new_object,
    .object_set = builtin_object_set,
    .object_get = builtin_object_get,
    .object_has = builtin_object_has,
    .object_size = builtin_object_size,
    .object_key_at = builtin_object_key_at,
    .array_append = builtin_array_append,
    .array_get = builtin_array_get,
    .array_size = builtin_array_size,
};

const mcp_json_backend_ops_t *mcp_json_builtin_backend(void) {
    return &k_builtin_backend;
}
