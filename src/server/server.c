#include "mcpkit/server/server.h"

#include <string.h>

#include "internals.h"
#include "mcpkit/server/dispatcher.h"
#include "mcpkit/server/session.h"

void *srv_malloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->malloc_fn(n, a->userdata);
}

void *srv_realloc(mcp_context_t *ctx, void *ptr, size_t n) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->realloc_fn(ptr, n, a->userdata);
}

void srv_free(mcp_context_t *ctx, void *ptr) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    a->free_fn(ptr, a->userdata);
}

char *srv_strdup(mcp_context_t *ctx, const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *out = srv_malloc(ctx, n);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n);
    return out;
}

mcp_server_t *mcp_server_create(mcp_context_t *ctx, const char *name, const char *version) {
    if (name == NULL || version == NULL) {
        return NULL;
    }
    mcp_server_t *srv = srv_malloc(ctx, sizeof(*srv));
    if (srv == NULL) {
        return NULL;
    }
    memset(srv, 0, sizeof(*srv));
    srv->name = srv_strdup(ctx, name);
    srv->version = srv_strdup(ctx, version);
    if (srv->name == NULL || srv->version == NULL) {
        srv_free(ctx, srv->name);
        srv_free(ctx, srv->version);
        srv_free(ctx, srv);
        return NULL;
    }
    return srv;
}

static void free_all_tools(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_tools; i++) {
        mcp_tool_destroy(ctx, srv->tools[i]);
    }
    srv_free(ctx, srv->tools);
}

static void free_all_resources(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_resources; i++) {
        mcp_resource_destroy(ctx, srv->resources[i]);
    }
    srv_free(ctx, srv->resources);
}

static void free_all_prompts(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_prompts; i++) {
        mcp_prompt_destroy(ctx, srv->prompts[i]);
    }
    srv_free(ctx, srv->prompts);
}

static void free_all_sessions(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_sessions; i++) {
        session_free(ctx, srv->sessions[i]);
    }
    srv_free(ctx, srv->sessions);
}

void mcp_server_destroy(mcp_context_t *ctx, mcp_server_t *srv) {
    if (srv == NULL) {
        return;
    }
    free_all_sessions(ctx, srv);
    free_all_tools(ctx, srv);
    free_all_resources(ctx, srv);
    free_all_prompts(ctx, srv);
    srv_free(ctx, srv->name);
    srv_free(ctx, srv->version);
    srv_free(ctx, srv);
}

static mcp_status_t append_ptr(mcp_context_t *ctx, void ***arr, size_t *n, size_t *cap, void *p) {
    if (*n == *cap) {
        size_t ncap = *cap == 0 ? 4 : *cap * 2;
        void **narr = srv_realloc(ctx, *arr, ncap * sizeof(*narr));
        if (narr == NULL) {
            return MCP_ERR_NOMEM;
        }
        *arr = narr;
        *cap = ncap;
    }
    (*arr)[(*n)++] = p;
    return MCP_OK;
}

static const mcp_tool_t *find_tool(const mcp_server_t *srv, const char *name) {
    for (size_t i = 0; i < srv->n_tools; i++) {
        if (strcmp(srv->tools[i]->name, name) == 0) {
            return srv->tools[i];
        }
    }
    return NULL;
}

static const mcp_resource_t *find_resource(const mcp_server_t *srv, const char *uri) {
    for (size_t i = 0; i < srv->n_resources; i++) {
        if (strcmp(srv->resources[i]->uri, uri) == 0) {
            return srv->resources[i];
        }
    }
    return NULL;
}

static const mcp_prompt_t *find_prompt(const mcp_server_t *srv, const char *name) {
    for (size_t i = 0; i < srv->n_prompts; i++) {
        if (strcmp(srv->prompts[i]->name, name) == 0) {
            return srv->prompts[i];
        }
    }
    return NULL;
}

mcp_status_t mcp_server_add_tool(mcp_context_t *ctx, mcp_server_t *srv, mcp_tool_t *tool) {
    if (srv == NULL || tool == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (find_tool(srv, tool->name) != NULL) {
        return MCP_ERR_ALREADY_EXISTS;
    }
    return append_ptr(ctx, (void ***)&srv->tools, &srv->n_tools, &srv->cap_tools, tool);
}

mcp_status_t mcp_server_add_resource(mcp_context_t *ctx, mcp_server_t *srv, mcp_resource_t *res) {
    if (srv == NULL || res == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (find_resource(srv, res->uri) != NULL) {
        return MCP_ERR_ALREADY_EXISTS;
    }
    return append_ptr(ctx, (void ***)&srv->resources, &srv->n_resources, &srv->cap_resources, res);
}

mcp_status_t mcp_server_add_prompt(mcp_context_t *ctx, mcp_server_t *srv, mcp_prompt_t *prompt) {
    if (srv == NULL || prompt == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (find_prompt(srv, prompt->name) != NULL) {
        return MCP_ERR_ALREADY_EXISTS;
    }
    return append_ptr(ctx, (void ***)&srv->prompts, &srv->n_prompts, &srv->cap_prompts, prompt);
}

mcp_status_t mcp_server_remove_tool(mcp_context_t *ctx, mcp_server_t *srv, const char *name) {
    if (srv == NULL || name == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < srv->n_tools; i++) {
        if (strcmp(srv->tools[i]->name, name) == 0) {
            mcp_tool_destroy(ctx, srv->tools[i]);
            srv->tools[i] = srv->tools[--srv->n_tools];
            return MCP_OK;
        }
    }
    return MCP_ERR_NOT_FOUND;
}

mcp_status_t mcp_server_remove_resource(mcp_context_t *ctx, mcp_server_t *srv, const char *uri) {
    if (srv == NULL || uri == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < srv->n_resources; i++) {
        if (strcmp(srv->resources[i]->uri, uri) == 0) {
            mcp_resource_destroy(ctx, srv->resources[i]);
            srv->resources[i] = srv->resources[--srv->n_resources];
            return MCP_OK;
        }
    }
    return MCP_ERR_NOT_FOUND;
}

mcp_status_t mcp_server_remove_prompt(mcp_context_t *ctx, mcp_server_t *srv, const char *name) {
    if (srv == NULL || name == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < srv->n_prompts; i++) {
        if (strcmp(srv->prompts[i]->name, name) == 0) {
            mcp_prompt_destroy(ctx, srv->prompts[i]);
            srv->prompts[i] = srv->prompts[--srv->n_prompts];
            return MCP_OK;
        }
    }
    return MCP_ERR_NOT_FOUND;
}
