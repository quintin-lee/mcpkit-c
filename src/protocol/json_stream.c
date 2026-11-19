#include "mcpkit/protocol/json_stream.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_STREAM_INITIAL_CAP (256)

void mcp_json_stream_init_arena(mcp_json_stream_t *s, mcp_arena_t *arena) {
    if (s == NULL) {
        return;
    }
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
    s->arena = arena;
    s->allocator = NULL;
}

void mcp_json_stream_init_allocator(mcp_json_stream_t *s, const mcp_allocator_t *allocator) {
    if (s == NULL) {
        return;
    }
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
    s->arena = NULL;
    s->allocator = allocator;
}

void mcp_json_stream_free(mcp_json_stream_t *s) {
    if (s == NULL) {
        return;
    }
    if (s->arena == NULL && s->buf != NULL) {
        if (s->allocator != NULL && s->allocator->free_fn != NULL) {
            s->allocator->free_fn(s->buf, s->allocator->userdata);
        } else {
            free(s->buf);
        }
    }
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
}

static bool ensure_capacity(mcp_json_stream_t *s, size_t needed) {
    if (s == NULL) {
        return false;
    }
    if (s->len + needed + 1 <= s->cap) {
        return true;
    }
    size_t new_cap = (s->cap > 0) ? s->cap * 2 : JSON_STREAM_INITIAL_CAP;
    while (new_cap < s->len + needed + 1) {
        new_cap *= 2;
    }

    char *new_buf = NULL;
    if (s->arena != NULL) {
        new_buf = (char *)mcp_arena_realloc(s->arena, s->buf, new_cap);
    } else if (s->allocator != NULL && s->allocator->realloc_fn != NULL) {
        new_buf = (char *)s->allocator->realloc_fn(s->buf, new_cap, s->allocator->userdata);
    } else if (s->allocator != NULL && s->allocator->malloc_fn != NULL) {
        new_buf = (char *)s->allocator->malloc_fn(new_cap, s->allocator->userdata);
        if (new_buf != NULL && s->buf != NULL) {
            memcpy(new_buf, s->buf, s->len);
            if (s->allocator->free_fn != NULL) {
                s->allocator->free_fn(s->buf, s->allocator->userdata);
            }
        }
    } else {
        new_buf = (char *)realloc(s->buf, new_cap);
    }

    if (new_buf == NULL) {
        return false;
    }
    s->buf = new_buf;
    s->cap = new_cap;
    return true;
}

void mcp_json_stream_write_raw(mcp_json_stream_t *s, const char *str, size_t len) {
    if (s == NULL || str == NULL || len == 0) {
        return;
    }
    if (!ensure_capacity(s, len)) {
        return;
    }
    memcpy(s->buf + s->len, str, len);
    s->len += len;
    s->buf[s->len] = '\0';
}

void mcp_json_stream_write_string(mcp_json_stream_t *s, const char *str) {
    if (s == NULL) {
        return;
    }
    if (str == NULL) {
        mcp_json_stream_write_null(s);
        return;
    }

    mcp_json_stream_write_raw(s, "\"", 1);

    const char *p = str;
    const char *start = p;

    while (*p != '\0') {
        unsigned char c = (unsigned char)*p;
        const char *escape = NULL;
        char hex_buf[8];

        switch (c) {
        case '\"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (c < 0x20) {
                snprintf(hex_buf, sizeof(hex_buf), "\\u%04x", c);
                escape = hex_buf;
            }
            break;
        }

        if (escape != NULL) {
            if (p > start) {
                mcp_json_stream_write_raw(s, start, (size_t)(p - start));
            }
            mcp_json_stream_write_raw(s, escape, strlen(escape));
            start = p + 1;
        }
        p++;
    }

    if (p > start) {
        mcp_json_stream_write_raw(s, start, (size_t)(p - start));
    }

    mcp_json_stream_write_raw(s, "\"", 1);
}

void mcp_json_stream_write_int64(mcp_json_stream_t *s, int64_t val) {
    char num_buf[32];
    int n = snprintf(num_buf, sizeof(num_buf), "%" PRId64, val);
    if (n > 0) {
        mcp_json_stream_write_raw(s, num_buf, (size_t)n);
    }
}

void mcp_json_stream_write_uint64(mcp_json_stream_t *s, uint64_t val) {
    char num_buf[32];
    int n = snprintf(num_buf, sizeof(num_buf), "%" PRIu64, val);
    if (n > 0) {
        mcp_json_stream_write_raw(s, num_buf, (size_t)n);
    }
}

void mcp_json_stream_write_bool(mcp_json_stream_t *s, bool val) {
    if (val) {
        mcp_json_stream_write_raw(s, "true", 4);
    } else {
        mcp_json_stream_write_raw(s, "false", 5);
    }
}

void mcp_json_stream_write_null(mcp_json_stream_t *s) {
    mcp_json_stream_write_raw(s, "null", 4);
}

static void write_rpc_id(mcp_json_stream_t *s, const mcp_json_rpc_id_t *id) {
    if (id == NULL || id->type == MCP_ID_NONE) {
        mcp_json_stream_write_null(s);
    } else if (id->type == MCP_ID_STRING) {
        mcp_json_stream_write_string(s, id->val.str);
    } else if (id->type == MCP_ID_NUMBER) {
        mcp_json_stream_write_int64(s, id->val.num);
    } else {
        mcp_json_stream_write_null(s);
    }
}

mcp_status_t mcp_serialize_response_stream(mcp_json_stream_t *stream,
                                           const mcp_json_rpc_id_t *id,
                                           const char *result_json_raw) {
    if (stream == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const char *raw = (result_json_raw != NULL) ? result_json_raw : "null";

    mcp_json_stream_write_raw(stream, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    write_rpc_id(stream, id);
    mcp_json_stream_write_raw(stream, ",\"result\":", 10);
    mcp_json_stream_write_raw(stream, raw, strlen(raw));
    mcp_json_stream_write_raw(stream, "}", 1);

    return (stream->buf != NULL) ? MCP_OK : MCP_ERR_NOMEM;
}

mcp_status_t mcp_serialize_error_stream(mcp_json_stream_t *stream,
                                        const mcp_json_rpc_id_t *id,
                                        int code,
                                        const char *message) {
    if (stream == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const char *msg = (message != NULL) ? message : "Internal Error";

    mcp_json_stream_write_raw(stream, "{\"jsonrpc\":\"2.0\",\"id\":", 22);
    write_rpc_id(stream, id);
    mcp_json_stream_write_raw(stream, ",\"error\":{\"code\":", 17);
    mcp_json_stream_write_int64(stream, (int64_t)code);
    mcp_json_stream_write_raw(stream, ",\"message\":", 11);
    mcp_json_stream_write_string(stream, msg);
    mcp_json_stream_write_raw(stream, "}}", 2);

    return (stream->buf != NULL) ? MCP_OK : MCP_ERR_NOMEM;
}
