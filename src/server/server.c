/**
 * @file server.c
 *
 * Server registry: add/remove for tools, resources, prompts, and
 * sessions. add_* takes ownership of the item on success and returns
 * ALREADY_EXISTS on a duplicate name; remove_* swap-removes and
 * destroys the item. mcp_server_destroy frees sessions, then
 * tools/resources/prompts, then name/version, then the struct itself.
 *
 * srv_malloc/srv_realloc/srv_free/srv_strdup are the shared
 * allocator helpers: a NULL ctx routes to the default (libc)
 * allocator so all server APIs are NULL-ctx-safe.
 */
#include "mcpkit/server/server.h"

#include <string.h>

#include "internals.h"
#include "mcpkit/protocol/message.h"
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
    atomic_init(&srv->c_requests_total, 0);
    atomic_init(&srv->c_requests_error, 0);
    atomic_init(&srv->c_notifications_total, 0);
    atomic_init(&srv->c_tools_called, 0);
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

static void free_all_completions(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_completions; i++) {
        srv_free(ctx, srv->completions[i].ref_prefix);
    }
    srv_free(ctx, srv->completions);
}

// Outbox entries are caller-built notifications; destroy any that were
// pushed but never drained by a transport before the server tears down.
static void free_outbox(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_outbox; i++) {
        mcp_message_destroy(ctx, srv->outbox[i]);
    }
    srv_free(ctx, srv->outbox);
    srv->n_outbox = 0;
    srv->cap_outbox = 0;
}

void mcp_server_destroy(mcp_context_t *ctx, mcp_server_t *srv) {
    if (srv == NULL) {
        return;
    }
    free_all_sessions(ctx, srv);
    free_all_tools(ctx, srv);
    free_all_resources(ctx, srv);
    free_all_prompts(ctx, srv);
    free_all_completions(ctx, srv);
    free_outbox(ctx, srv);
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

mcp_status_t mcp_server_register_completion_provider(mcp_context_t *ctx, mcp_server_t *srv,
                                                     const char *ref_prefix,
                                                     mcp_completion_provider_fn fn,
                                                     void *user_data) {
    if (srv == NULL || ref_prefix == NULL || fn == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    // Reject duplicate prefixes.
    for (size_t i = 0; i < srv->n_completions; i++) {
        if (strcmp(srv->completions[i].ref_prefix, ref_prefix) == 0) {
            return MCP_ERR_ALREADY_EXISTS;
        }
    }
    if (srv->n_completions == srv->cap_completions) {
        size_t ncap = srv->cap_completions == 0 ? 4 : srv->cap_completions * 2;
        mcp_completion_entry_t *nc = srv_realloc(ctx, srv->completions, ncap * sizeof(*nc));
        if (nc == NULL) {
            return MCP_ERR_NOMEM;
        }
        srv->completions = nc;
        srv->cap_completions = ncap;
    }
    char *copy = srv_strdup(ctx, ref_prefix);
    if (copy == NULL) {
        return MCP_ERR_NOMEM;
    }
    srv->completions[srv->n_completions].ref_prefix = copy;
    srv->completions[srv->n_completions].fn = fn;
    srv->completions[srv->n_completions].user_data = user_data;
    srv->n_completions++;
    return MCP_OK;
}

mcp_status_t mcp_server_remove_completion_provider(mcp_context_t *ctx, mcp_server_t *srv,
                                                   const char *ref_prefix) {
    if (srv == NULL || ref_prefix == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < srv->n_completions; i++) {
        if (strcmp(srv->completions[i].ref_prefix, ref_prefix) == 0) {
            srv_free(ctx, srv->completions[i].ref_prefix);
            srv->completions[i] = srv->completions[--srv->n_completions];
            return MCP_OK;
        }
    }
    return MCP_ERR_NOT_FOUND;
}

mcp_status_t mcp_server_counters(mcp_context_t *ctx, const mcp_server_t *srv,
                                 mcp_server_counters_t *out) {
    (void)ctx;
    if (srv == NULL || out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    out->requests_total = atomic_load(&srv->c_requests_total);
    out->requests_error = atomic_load(&srv->c_requests_error);
    out->notifications_total = atomic_load(&srv->c_notifications_total);
    out->tools_called = atomic_load(&srv->c_tools_called);
    return MCP_OK;
}

mcp_status_t mcp_server_set_tracer(mcp_context_t *ctx, mcp_server_t *srv,
                                   mcp_trace_fn fn_or_null, void *userdata) {
    (void)ctx;
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    srv->tracer = fn_or_null;
    srv->tracer_ud = userdata;
    return MCP_OK;
}

mcp_status_t mcp_server_notify_client(mcp_context_t *ctx, mcp_server_t *srv,
                                      const char *method, mcp_json_value_t *params) {
    if (srv == NULL || method == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    // Server takes params on success only; the builder retains them on
    // failure, so the caller's value is never leaked on the error path.
    mcp_message_t *notif = mcp_notification_new(ctx, method, params);
    if (notif == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (srv->n_outbox == srv->cap_outbox) {
        size_t ncap = srv->cap_outbox == 0 ? 4 : srv->cap_outbox * 2;
        mcp_message_t **narr = srv_realloc(ctx, srv->outbox, ncap * sizeof(*narr));
        if (narr == NULL) {
            mcp_message_destroy(ctx, notif);
            return MCP_ERR_NOMEM;
        }
        srv->outbox = narr;
        srv->cap_outbox = ncap;
    }
    srv->outbox[srv->n_outbox++] = notif;
    return MCP_OK;
}

mcp_status_t mcp_server_outbox_pop(mcp_context_t *ctx, mcp_server_t *srv,
                                   mcp_message_t **out) {
    if (srv == NULL || out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    if (srv->n_outbox == 0) {
        return MCP_ERR_NOT_FOUND;
    }
    *out = srv->outbox[--srv->n_outbox];
    srv->outbox[srv->n_outbox] = NULL;
    return MCP_OK;
}
