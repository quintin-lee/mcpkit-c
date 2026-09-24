/**
 * @file sse.c
 * @brief W3C Server-Sent Events (SSE) streaming frame parser.
 */

#include "mcpkit/transport/sse.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mcp_sse_parser {
    mcp_sse_event_cb on_event;
    void *userdata;

    /* Line accumulation */
    char *line_buf;
    size_t line_len;
    size_t line_cap;
    bool pending_lf;

    /* Current event accumulation */
    char *data_buf;
    size_t data_len;
    size_t data_cap;
    bool has_data;

    char *event_buf;
    size_t event_len;
    size_t event_cap;

    char *id_buf;
    size_t id_len;
    size_t id_cap;
    bool has_id;

    int64_t retry_ms;

    /* Persistent state across events */
    char *last_event_id;
    size_t last_id_len;
    size_t last_id_cap;
};

static void buf_clear(char *buf, size_t *len) {
    *len = 0;
    if (buf != NULL) {
        buf[0] = '\0';
    }
}

static void buf_free(mcp_context_t *ctx, char **buf, size_t *cap) {
    if (*buf != NULL) {
        const mcp_allocator_t *alloc = mcp_context_allocator(ctx);
        alloc->free_fn(*buf, alloc->userdata);
        *buf = NULL;
        *cap = 0;
    }
}

static mcp_status_t buf_append(mcp_context_t *ctx, char **buf, size_t *len, size_t *cap,
                               const char *str, size_t str_len) {
    const mcp_allocator_t *alloc = mcp_context_allocator(ctx);
    if (*buf == NULL || *len + str_len + 1 > *cap) {
        size_t new_cap = (*cap == 0) ? 128 : *cap * 2;
        while (new_cap < *len + str_len + 1) {
            new_cap *= 2;
        }
        char *new_buf = alloc->realloc_fn(*buf, new_cap, alloc->userdata);
        if (new_buf == NULL) {
            return MCP_ERR_NOMEM;
        }
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, str, str_len);
    *len += str_len;
    (*buf)[*len] = '\0';
    return MCP_OK;
}

static mcp_status_t buf_set(mcp_context_t *ctx, char **buf, size_t *len, size_t *cap,
                            const char *str, size_t str_len) {
    *len = 0;
    return buf_append(ctx, buf, len, cap, str, str_len);
}

static mcp_status_t process_line(mcp_context_t *ctx, mcp_sse_parser_t *p) {
    if (p->line_len == 0) {
        if (p->has_data) {
            mcp_sse_event_t ev;
            ev.event = (p->event_len > 0) ? p->event_buf : "message";
            ev.data = (p->data_buf != NULL) ? p->data_buf : "";
            ev.id = p->has_id ? p->id_buf : NULL;
            ev.retry_ms = p->retry_ms;
            p->on_event(&ev, p->userdata);
        }
        p->has_data = false;
        buf_clear(p->data_buf, &p->data_len);
        buf_clear(p->event_buf, &p->event_len);
        buf_clear(p->id_buf, &p->id_len);
        p->has_id = false;
        return MCP_OK;
    }

    if (p->line_buf[0] == ':') {
        /* Comment, ignore */
        return MCP_OK;
    }

    char *colon = strchr(p->line_buf, ':');
    const char *field = p->line_buf;
    const char *value = "";
    if (colon != NULL) {
        *colon = '\0';
        value = colon + 1;
        if (*value == ' ') {
            value++;
        }
    }

    if (strcmp(field, "event") == 0) {
        return buf_set(ctx, &p->event_buf, &p->event_len, &p->event_cap, value, strlen(value));
    } else if (strcmp(field, "data") == 0) {
        if (p->has_data) {
            mcp_status_t st = buf_append(ctx, &p->data_buf, &p->data_len, &p->data_cap, "\n", 1);
            if (st != MCP_OK) {
                return st;
            }
        }
        p->has_data = true;
        return buf_append(ctx, &p->data_buf, &p->data_len, &p->data_cap, value, strlen(value));
    } else if (strcmp(field, "id") == 0) {
        p->has_id = true;
        mcp_status_t st = buf_set(ctx, &p->id_buf, &p->id_len, &p->id_cap, value, strlen(value));
        if (st != MCP_OK) {
            return st;
        }
        return buf_set(ctx, &p->last_event_id, &p->last_id_len, &p->last_id_cap, value, strlen(value));
    } else if (strcmp(field, "retry") == 0) {
        bool all_digits = (value[0] != '\0');
        for (const char *s = value; *s != '\0'; s++) {
            if (!isdigit((unsigned char)*s)) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            p->retry_ms = (int64_t)strtoll(value, NULL, 10);
        }
    }

    return MCP_OK;
}

mcp_sse_parser_t *mcp_sse_parser_create(mcp_context_t *ctx,
                                        mcp_sse_event_cb on_event,
                                        void *userdata) {
    if (on_event == NULL) {
        return NULL;
    }
    const mcp_allocator_t *alloc = mcp_context_allocator(ctx);
    mcp_sse_parser_t *p = alloc->calloc_fn(1, sizeof(*p), alloc->userdata);
    if (p == NULL) {
        return NULL;
    }
    p->on_event = on_event;
    p->userdata = userdata;
    p->retry_ms = -1;
    return p;
}

mcp_status_t mcp_sse_parser_feed(mcp_context_t *ctx,
                                 mcp_sse_parser_t *p,
                                 const char *chunk,
                                 size_t len) {
    if (p == NULL || (chunk == NULL && len > 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (len == 0) {
        return MCP_OK;
    }

    size_t i = 0;
    while (i < len) {
        if (p->pending_lf) {
            p->pending_lf = false;
            if (chunk[i] == '\n') {
                i++;
                continue;
            }
        }

        size_t start = i;
        while (i < len && chunk[i] != '\r' && chunk[i] != '\n') {
            i++;
        }

        if (i > start) {
            mcp_status_t st = buf_append(ctx, &p->line_buf, &p->line_len, &p->line_cap,
                                         chunk + start, i - start);
            if (st != MCP_OK) {
                return st;
            }
        }

        if (i < len) {
            char c = chunk[i];
            if (c == '\r') {
                p->pending_lf = true;
            }
            i++;
            mcp_status_t st = process_line(ctx, p);
            buf_clear(p->line_buf, &p->line_len);
            if (st != MCP_OK) {
                return st;
            }
        }
    }

    return MCP_OK;
}

const char *mcp_sse_parser_last_event_id(mcp_context_t *ctx,
                                        const mcp_sse_parser_t *parser) {
    (void)ctx;
    if (parser == NULL || parser->last_id_len == 0) {
        return NULL;
    }
    return parser->last_event_id;
}

void mcp_sse_parser_destroy(mcp_context_t *ctx, mcp_sse_parser_t *p) {
    if (p == NULL) {
        return;
    }
    const mcp_allocator_t *alloc = mcp_context_allocator(ctx);
    buf_free(ctx, &p->line_buf, &p->line_cap);
    buf_free(ctx, &p->data_buf, &p->data_cap);
    buf_free(ctx, &p->event_buf, &p->event_cap);
    buf_free(ctx, &p->id_buf, &p->id_cap);
    buf_free(ctx, &p->last_event_id, &p->last_id_cap);
    alloc->free_fn(p, alloc->userdata);
}
