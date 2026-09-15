#include "mcpkit/apps/csp.h"

#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"

struct mcp_csp {
    char *sources[4];
};

static const char *kDirectives[4] = {"default-src", "script-src", "style-src", "connect-src"};

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static char *dup_of(mcp_context_t *ctx, const char *s) {
    const mcp_allocator_t *a = alloc_of(ctx);
    size_t n = strlen(s) + 1;
    char *p = a->malloc_fn(n, a->userdata);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

static void free_of(mcp_context_t *ctx, void *p) {
    if (p == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    a->free_fn(p, a->userdata);
}

mcp_csp_t *mcp_csp_default_deny_new(mcp_context_t *ctx) {
    const mcp_allocator_t *a = alloc_of(ctx);
    mcp_csp_t *c = a->malloc_fn(sizeof(*c), a->userdata);
    if (c == NULL) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->sources[0] = dup_of(ctx, "'none'");
    if (c->sources[0] == NULL) {
        free_of(ctx, c);
        return NULL;
    }
    return c;
}

void mcp_csp_destroy(mcp_context_t *ctx, mcp_csp_t *csp) {
    if (csp == NULL) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        free_of(ctx, csp->sources[i]);
    }
    free_of(ctx, csp);
}

mcp_status_t mcp_csp_set(mcp_context_t *ctx, mcp_csp_t *csp, const char *directive,
                         const char *sources_or_null) {
    if (csp == NULL || directive == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    int idx = -1;
    for (int i = 0; i < 4; i++) {
        if (strcmp(directive, kDirectives[i]) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (sources_or_null == NULL || sources_or_null[0] == '\0') {
        free_of(ctx, csp->sources[idx]);
        csp->sources[idx] = NULL;
        return MCP_OK;
    }
    char *copy = dup_of(ctx, sources_or_null);
    if (copy == NULL) {
        return MCP_ERR_NOMEM;
    }
    free_of(ctx, csp->sources[idx]);
    csp->sources[idx] = copy;
    return MCP_OK;
}

mcp_status_t mcp_csp_serialize(mcp_context_t *ctx, const mcp_csp_t *csp, char **out) {
    if (csp == NULL || out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    size_t total = 1;
    int count = 0;
    for (int i = 0; i < 4; i++) {
        if (csp->sources[i] != NULL) {
            total += strlen(kDirectives[i]) + 1 + strlen(csp->sources[i]) + 2;
            count++;
        }
    }
    if (count > 0) {
        total -= 2;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    char *buf = a->malloc_fn(total, a->userdata);
    if (buf == NULL) {
        return MCP_ERR_NOMEM;
    }
    size_t pos = 0;
    for (int i = 0; i < 4; i++) {
        if (csp->sources[i] == NULL) {
            continue;
        }
        if (pos > 0) {
            memcpy(buf + pos, "; ", 2);
            pos += 2;
        }
        size_t dl = strlen(kDirectives[i]);
        memcpy(buf + pos, kDirectives[i], dl);
        pos += dl;
        buf[pos++] = ' ';
        size_t sl = strlen(csp->sources[i]);
        memcpy(buf + pos, csp->sources[i], sl);
        pos += sl;
    }
    buf[pos] = '\0';
    *out = buf;
    return MCP_OK;
}
