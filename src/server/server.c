#define _DEFAULT_SOURCE

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

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "internals.h"
#include "mcpkit/json/json.h"
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
    srv->ctx = ctx;
    atomic_init(&srv->c_requests_total, 0);
    atomic_init(&srv->c_requests_error, 0);
    atomic_init(&srv->c_notifications_total, 0);
    atomic_init(&srv->c_tools_called, 0);
    atomic_init(&srv->log_floor, MCP_LOG_DEBUG);
    pthread_mutex_init(&srv->subscribed_lock, NULL);
    pthread_mutex_init(&srv->sessions_lock, NULL);
    pthread_mutex_init(&srv->outbox_lock, NULL);
    pthread_mutex_init(&srv->pending_lock, NULL);
    srv->pending_requests = NULL;
    atomic_init(&srv->next_server_id, 1000ULL);
    srv->list_ttl_ms = 0;
    srv->list_cache_scope = NULL;
    srv->response_meta = NULL;
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

mcp_status_t mcp_server_set_protected_resource_metadata(
    mcp_context_t *ctx, mcp_server_t *srv,
    const mcp_protected_resource_metadata_t *pr_meta) {
    (void)ctx;
    if (srv == NULL || pr_meta == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    srv->pr_meta = *pr_meta;
    srv->has_pr_meta = true;
    return MCP_OK;
}

const mcp_protected_resource_metadata_t *
mcp_server_get_protected_resource_metadata(mcp_context_t *ctx, mcp_server_t *srv) {
    (void)ctx;
    if (srv == NULL || !srv->has_pr_meta) {
        return NULL;
    }
    return &srv->pr_meta;
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

static void free_all_resource_templates(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_resource_templates; i++) {
        mcp_resource_template_destroy(ctx, srv->resource_templates[i]);
    }
    srv_free(ctx, srv->resource_templates);
}

static void free_all_prompts(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_prompts; i++) {
        mcp_prompt_destroy(ctx, srv->prompts[i]);
    }
    srv_free(ctx, srv->prompts);
}

static void free_all_sessions(mcp_context_t *ctx, mcp_server_t *srv) {
    pthread_mutex_lock(&srv->sessions_lock);
    for (size_t i = 0; i < srv->n_sessions; i++) {
        session_free(ctx, srv->sessions[i]);
    }
    srv_free(ctx, srv->sessions);
    srv->sessions = NULL;
    srv->n_sessions = 0;
    srv->cap_sessions = 0;
    pthread_mutex_unlock(&srv->sessions_lock);
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
    pthread_mutex_lock(&srv->outbox_lock);
    for (size_t i = 0; i < srv->n_outbox; i++) {
        mcp_message_destroy(ctx, srv->outbox[i]);
    }
    srv_free(ctx, srv->outbox);
    srv->n_outbox = 0;
    srv->cap_outbox = 0;
    pthread_mutex_unlock(&srv->outbox_lock);
}

// Pending request state table: freed in bulk at teardown.
static void free_all_pending_requests(mcp_context_t *ctx, mcp_server_t *srv) {
    pthread_mutex_lock(&srv->pending_lock);
    mcp_pending_req_t *pnode = srv->pending_requests;
    srv->pending_requests = NULL;
    pthread_mutex_unlock(&srv->pending_lock);
    while (pnode != NULL) {
        mcp_pending_req_t *next = pnode->next;
        if (pnode->str_id != NULL) {
            srv_free(ctx, pnode->str_id);
        }
        srv_free(ctx, pnode);
        pnode = next;
    }
}

// Resource-subscription set: each entry is a srv_strdup'd uri owned by
// the server; freed in bulk at teardown.
static void free_subscribed(mcp_context_t *ctx, mcp_server_t *srv) {
    for (size_t i = 0; i < srv->n_subscribed; i++) {
        srv_free(ctx, srv->subscribed_uris[i]);
    }
    srv_free(ctx, srv->subscribed_uris);
    srv->n_subscribed = 0;
    srv->cap_subscribed = 0;
}

void mcp_server_destroy(mcp_context_t *ctx, mcp_server_t *srv) {
    if (srv == NULL) {
        return;
    }
    if (ctx == NULL) {
        ctx = srv->ctx;
    }
    free_all_sessions(ctx, srv);
    free_all_tools(ctx, srv);
    free_all_resources(ctx, srv);
    free_all_resource_templates(ctx, srv);
    free_all_prompts(ctx, srv);
    free_all_completions(ctx, srv);
    free_outbox(ctx, srv);
    free_all_pending_requests(ctx, srv);
    free_subscribed(ctx, srv);
    if (srv->task_mgr != NULL) {
        mcp_task_mgr_free(ctx, srv->task_mgr);
        srv->task_mgr = NULL;
    }
    if (srv->skill_reg != NULL) {
        mcp_skill_registry_free(ctx, srv->skill_reg);
        srv->skill_reg = NULL;
    }
    mcp_json_destroy(ctx, srv->response_meta);
    mcp_json_destroy(ctx, srv->custom_capabilities);
    srv_free(ctx, srv->list_cache_scope);
    pthread_mutex_destroy(&srv->subscribed_lock);
    pthread_mutex_destroy(&srv->sessions_lock);
    pthread_mutex_destroy(&srv->outbox_lock);
    pthread_mutex_destroy(&srv->pending_lock);
    srv_free(ctx, srv->name);
    srv_free(ctx, srv->version);
    srv_free(ctx, srv);
}

/* Push one owned message onto the outbox under outbox_lock.
 * Grows the array as needed; on NOMEM the message is destroyed and
 * MCP_ERR_NOMEM is returned. The caller retains no reference on success. */
static mcp_status_t outbox_push(mcp_context_t *ctx, mcp_server_t *srv,
                                mcp_message_t *msg) {
    pthread_mutex_lock(&srv->outbox_lock);
    if (srv->n_outbox == srv->cap_outbox) {
        size_t ncap = srv->cap_outbox == 0 ? 4 : srv->cap_outbox * 2;
        mcp_message_t **narr = srv_realloc(ctx, srv->outbox, ncap * sizeof(*narr));
        if (narr == NULL) {
            pthread_mutex_unlock(&srv->outbox_lock);
            mcp_message_destroy(ctx, msg);
            return MCP_ERR_NOMEM;
        }
        srv->outbox = narr;
        srv->cap_outbox = ncap;
    }
    srv->outbox[srv->n_outbox++] = msg;
    pthread_mutex_unlock(&srv->outbox_lock);
    return MCP_OK;
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

/* -------------------------------------------------------------------------
 * P4-a S1: x-mcp-header annotation validation in mcp_server_add_tool.
 *
 * Walks the tool's inputSchema JSON tree (any nesting depth). Whenever an
 * object contains an `x-mcp-header` key, that object is a property schema
 * and is validated per SEP-2243:
 *  - its `type` constraint MUST allow string/integer/boolean (number/object/
 *    array-only types forbidden; a union passes if ANY allowed type present,
 *    and an absent/other `type` is treated as unconstrained -> allowed)
 *  - the annotation value MUST be a non-empty string of RFC 9110 tchar
 *    (printable ASCII 0x21-0x7E, i.e. no CR/LF/control/empty)
 *  - annotation values MUST be unique case-insensitively within the tool
 * Violation -> MCP_ERR_INVALID_ARGUMENT (the tool is NOT registered).
 * ---------------------------------------------------------------------- */

static bool is_tchar(char c) {
    /* RFC 9110 tchar (printable-ASCII subset relevant here) */
    return (unsigned char)c >= 0x21 && (unsigned char)c <= 0x7E;
}

static bool xmcp_type_allowed(const char *type_str) {
    return strcmp(type_str, "string") == 0 ||
           strcmp(type_str, "integer") == 0 ||
           strcmp(type_str, "boolean") == 0;
}

/* Check the `type` constraint of a property object. Returns true when the
 * annotation is permitted (allowed type present, or unconstrained). */
static bool xmcp_property_type_ok(mcp_context_t *ctx, const mcp_json_value_t *prop) {
    const mcp_json_value_t *type_v = mcp_json_object_get(ctx, prop, "type");
    if (type_v == NULL) {
        return true; /* no type constraint -> allowed */
    }
    if (mcp_json_type(ctx, type_v) == MCP_JSON_STRING) {
        const char *ts;
        if (mcp_json_string_value(ctx, type_v, &ts) != MCP_OK) {
            return true;
        }
        return xmcp_type_allowed(ts);
    }
    if (mcp_json_type(ctx, type_v) == MCP_JSON_ARRAY) {
        size_t n = mcp_json_array_size(ctx, type_v);
        for (size_t i = 0; i < n; i++) {
            const mcp_json_value_t *elem = mcp_json_array_get(ctx, type_v, i);
            if (elem != NULL && mcp_json_type(ctx, elem) == MCP_JSON_STRING) {
                const char *ts;
                if (mcp_json_string_value(ctx, elem, &ts) == MCP_OK &&
                    xmcp_type_allowed(ts)) {
                    return true; /* at least one allowed type -> ok */
                }
            }
        }
        return false; /* union with no allowed type -> reject */
    }
    return true; /* other `type` value -> unconstrained, allow */
}

/* Single-pass recursive walk. When an object holds an x-mcp-header key,
 * validate that property's type, the header value, and uniqueness. */
static mcp_status_t xmcp_walk(mcp_context_t *ctx, const mcp_json_value_t *node,
                              char **seen, size_t *n_seen, size_t cap) {
    if (node == NULL) {
        return MCP_OK;
    }
    mcp_json_type_t t = mcp_json_type(ctx, node);
    if (t != MCP_JSON_OBJECT) {
        if (t == MCP_JSON_ARRAY) {
            size_t n = mcp_json_array_size(ctx, node);
            for (size_t i = 0; i < n; i++) {
                mcp_status_t st = xmcp_walk(ctx, mcp_json_array_get(ctx, node, i),
                                            seen, n_seen, cap);
                if (st != MCP_OK) {
                    return st;
                }
            }
        }
        return MCP_OK;
    }
    /* Is this an object that itself carries an x-mcp-header annotation? */
    if (mcp_json_object_has(ctx, node, "x-mcp-header")) {
        const mcp_json_value_t *hdr_v = mcp_json_object_get(ctx, node, "x-mcp-header");
        if (mcp_json_type(ctx, hdr_v) != MCP_JSON_STRING) {
            return MCP_ERR_INVALID_ARGUMENT;
        }
        const char *hdr;
        if (mcp_json_string_value(ctx, hdr_v, &hdr) != MCP_OK || hdr == NULL || hdr[0] == '\0') {
            return MCP_ERR_INVALID_ARGUMENT;
        }
        for (const char *p = hdr; *p; p++) {
            if (!is_tchar(*p)) {
                return MCP_ERR_INVALID_ARGUMENT;
            }
        }
        for (size_t j = 0; j < *n_seen; j++) {
            if (strcasecmp(seen[j], hdr) == 0) {
                return MCP_ERR_INVALID_ARGUMENT; /* duplicate (case-insensitive) */
            }
        }
        if (*n_seen < cap) {
            seen[*n_seen] = (char *)hdr;
            (*n_seen)++;
        }
        if (!xmcp_property_type_ok(ctx, node)) {
            return MCP_ERR_INVALID_ARGUMENT;
        }
    }
    /* Recurse into object members */
    size_t n = mcp_json_object_size(ctx, node);
    for (size_t i = 0; i < n; i++) {
        const char *key = mcp_json_object_key_at(ctx, node, i);
        if (key == NULL) {
            continue;
        }
        mcp_status_t st = xmcp_walk(ctx, mcp_json_object_get(ctx, node, key),
                                    seen, n_seen, cap);
        if (st != MCP_OK) {
            return st;
        }
    }
    return MCP_OK;
}

static mcp_status_t xmcp_validate_schema(mcp_context_t *ctx,
                                         const mcp_json_value_t *schema) {
    if (schema == NULL) {
        return MCP_OK; /* no schema -> no annotations to check */
    }
    char *seen[64];
    size_t n_seen = 0;
    return xmcp_walk(ctx, schema, seen, &n_seen, 64);
}

mcp_status_t mcp_server_add_tool(mcp_context_t *ctx, mcp_server_t *srv, mcp_tool_t *tool) {
    if (srv == NULL || tool == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (find_tool(srv, tool->name) != NULL) {
        return MCP_ERR_ALREADY_EXISTS;
    }
    /* P4-a S1: validate x-mcp-header annotations in the tool's inputSchema */
    mcp_status_t st = xmcp_validate_schema(ctx, tool->schema);
    if (st != MCP_OK) {
        return st;
    }
    return append_ptr(ctx, (void ***)&srv->tools, &srv->n_tools, &srv->cap_tools, tool);
}

/* P4-a T3: return a borrowed pointer to the tool's inputSchema by name.
   Used by streamable_http.c to walk the schema for x-mcp-header annotations. */
const mcp_json_value_t *mcp_server_get_tool_schema(mcp_context_t *ctx, mcp_server_t *srv,
                                                   const char *name) {
    (void)ctx;
    if (srv == NULL || name == NULL) {
        return NULL;
    }
    const mcp_tool_t *tool = find_tool(srv, name);
    return (tool != NULL) ? tool->schema : NULL;
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

mcp_status_t mcp_server_add_resource_template(mcp_context_t *ctx, mcp_server_t *srv,
                                              mcp_resource_template_t *tmpl) {
    if (srv == NULL || tmpl == NULL || tmpl->uri_template == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < srv->n_resource_templates; i++) {
        if (strcmp(srv->resource_templates[i]->uri_template, tmpl->uri_template) == 0) {
            return MCP_ERR_ALREADY_EXISTS;
        }
    }
    return append_ptr(ctx, (void ***)&srv->resource_templates,
                      &srv->n_resource_templates, &srv->cap_resource_templates, tmpl);
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

mcp_status_t mcp_server_remove_resource_template(mcp_context_t *ctx, mcp_server_t *srv,
                                                 const char *uri_template) {
    if (srv == NULL || uri_template == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_context_t *pctx = (srv->ctx != NULL) ? srv->ctx : ctx;
    for (size_t i = 0; i < srv->n_resource_templates; i++) {
        if (strcmp(srv->resource_templates[i]->uri_template, uri_template) == 0) {
            mcp_resource_template_destroy(pctx, srv->resource_templates[i]);
            srv->resource_templates[i] = srv->resource_templates[--srv->n_resource_templates];
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

mcp_status_t mcp_server_session_notify(mcp_context_t *ctx, mcp_server_t *srv,
                                       mcp_session_t *session, const char *method,
                                       mcp_json_value_t *params) {
    if (srv == NULL || method == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_message_t *notif = NULL;
    mcp_status_t st = mcp_session_build_notification(ctx, session, method, params, &notif);
    if (st != MCP_OK) {
        return st;
    }
    if (notif == NULL) {
        /* Filtered out cleanly */
        return MCP_OK;
    }
    return outbox_push(ctx, srv, notif);
}

mcp_status_t mcp_server_session_close_subscription(mcp_context_t *ctx, mcp_server_t *srv,
                                                   mcp_session_t *session,
                                                   mcp_message_t **resp_out) {
    if (srv == NULL || session == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_message_t *resp = NULL;
    mcp_status_t st = mcp_session_build_subscription_closure(ctx, session, &resp);
    if (st != MCP_OK) {
        return st;
    }
    if (resp_out != NULL) {
        *resp_out = resp;
        return MCP_OK;
    }
    return outbox_push(ctx, srv, resp);
}

mcp_status_t mcp_server_notify_client(mcp_context_t *ctx, mcp_server_t *srv,
                                      const char *method, mcp_json_value_t *params) {
    if (srv == NULL || method == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&srv->sessions_lock);
    bool fast_path = (srv->n_sessions == 1 &&
                      srv->sessions[0] != NULL &&
                      srv->sessions[0]->subscription_active);
    mcp_session_t *fast_sess = fast_path ? srv->sessions[0] : NULL;
    pthread_mutex_unlock(&srv->sessions_lock);
    if (fast_path) {
        return mcp_server_session_notify(ctx, srv, fast_sess, method, params);
    }
    // Server takes params on success only; the builder retains them on
    // failure, so the caller's value is never leaked on the error path.
    mcp_message_t *notif = mcp_notification_new(ctx, method, params);
    if (notif == NULL) {
        return MCP_ERR_NOMEM;
    }
    return outbox_push(ctx, srv, notif);
}

mcp_status_t mcp_server_outbox_pop(mcp_context_t *ctx, mcp_server_t *srv,
                                    mcp_message_t **out) {
    (void)ctx;
    if (srv == NULL || out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    pthread_mutex_lock(&srv->outbox_lock);
    if (srv->n_outbox == 0) {
        pthread_mutex_unlock(&srv->outbox_lock);
        return MCP_ERR_NOT_FOUND;
    }
    *out = srv->outbox[--srv->n_outbox];
    srv->outbox[srv->n_outbox] = NULL;
    pthread_mutex_unlock(&srv->outbox_lock);
    return MCP_OK;
}

mcp_status_t mcp_server_request_client_with_response(mcp_context_t *ctx,
                                                     mcp_server_t *srv,
                                                     mcp_session_t *session,
                                                     const char *method,
                                                     mcp_json_value_t *params,
                                                     uint64_t timeout_ms,
                                                     mcp_client_response_fn cb,
                                                     void *user_data,
                                                     uint64_t *assigned_id_out) {
    if (srv == NULL || method == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    uint64_t id = atomic_fetch_add(&srv->next_server_id, 1);
    if (assigned_id_out != NULL) {
        *assigned_id_out = id;
    }

    mcp_pending_req_t *entry = NULL;
    if (cb != NULL || timeout_ms > 0) {
        entry = (mcp_pending_req_t *)srv_malloc(ctx, sizeof(mcp_pending_req_t));
        if (entry == NULL) {
            return MCP_ERR_NOMEM;
        }
        entry->req_id = id;
        entry->str_id = NULL;
        entry->session = session;
        entry->callback = cb;
        entry->user_data = user_data;
        entry->next = NULL;
        if (timeout_ms > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            entry->expire_at_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 + timeout_ms;
        } else {
            entry->expire_at_ms = 0;
        }

        pthread_mutex_lock(&srv->pending_lock);
        entry->next = srv->pending_requests;
        srv->pending_requests = entry;
        pthread_mutex_unlock(&srv->pending_lock);
    }

    mcp_message_t *req = mcp_request_new_number_id(ctx, (double)id, method, params);
    if (req == NULL) {
        if (entry != NULL) {
            pthread_mutex_lock(&srv->pending_lock);
            mcp_pending_req_t **curr = &srv->pending_requests;
            while (*curr != NULL) {
                if (*curr == entry) {
                    *curr = entry->next;
                    break;
                }
                curr = &(*curr)->next;
            }
            pthread_mutex_unlock(&srv->pending_lock);
            srv_free(ctx, entry);
        }
        return MCP_ERR_NOMEM;
    }

    mcp_status_t st = mcp_server_deliver_notification(ctx, srv, session, req);
    if (st != MCP_OK) {
        if (entry != NULL) {
            pthread_mutex_lock(&srv->pending_lock);
            mcp_pending_req_t **curr = &srv->pending_requests;
            while (*curr != NULL) {
                if (*curr == entry) {
                    *curr = entry->next;
                    break;
                }
                curr = &(*curr)->next;
            }
            pthread_mutex_unlock(&srv->pending_lock);
            srv_free(ctx, entry);
        }
        return st;
    }

    return MCP_OK;
}

mcp_status_t mcp_server_request_client(mcp_context_t *ctx, mcp_server_t *srv,
                                       const char *method, mcp_json_value_t *params) {
    return mcp_server_request_client_with_response(ctx, srv, NULL, method, params, 0, NULL, NULL, NULL);
}

mcp_status_t mcp_server_request_sampling(mcp_context_t *ctx,
                                         mcp_server_t *server,
                                         mcp_session_t *session,
                                         mcp_json_value_t *params,
                                         uint64_t timeout_ms,
                                         mcp_client_response_fn cb,
                                         void *user_data,
                                         uint64_t *assigned_id_out) {
    return mcp_server_request_client_with_response(ctx, server, session,
                                                   "sampling/createMessage",
                                                   params, timeout_ms, cb,
                                                   user_data, assigned_id_out);
}

mcp_status_t mcp_server_request_roots(mcp_context_t *ctx,
                                      mcp_server_t *server,
                                      mcp_session_t *session,
                                      uint64_t timeout_ms,
                                      mcp_client_response_fn cb,
                                      void *user_data,
                                      uint64_t *assigned_id_out) {
    return mcp_server_request_client_with_response(ctx, server, session,
                                                   "roots/list",
                                                   NULL, timeout_ms, cb,
                                                   user_data, assigned_id_out);
}

mcp_status_t mcp_server_request_elicitation(mcp_context_t *ctx,
                                            mcp_server_t *server,
                                            mcp_session_t *session,
                                            mcp_json_value_t *params,
                                            uint64_t timeout_ms,
                                            mcp_client_response_fn cb,
                                            void *user_data,
                                            uint64_t *assigned_id_out) {
    return mcp_server_request_client_with_response(ctx, server, session,
                                                   "elicitation/create",
                                                   params, timeout_ms, cb,
                                                   user_data, assigned_id_out);
}

mcp_status_t mcp_server_dispatch_response(mcp_context_t *ctx, mcp_server_t *server,
                                          mcp_session_t *session, const mcp_message_t *resp) {
    if (server == NULL || resp == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_message_kind(ctx, resp) != MCP_MSG_RESPONSE) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_id_type_t id_type = mcp_message_id_type(ctx, resp);
    if (id_type == MCP_ID_NONE) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    uint64_t num_id = 0;
    bool has_num_id = false;
    const char *str_id = NULL;

    if (id_type == MCP_ID_NUMBER) {
        double d = 0;
        if (mcp_message_id_number(ctx, resp, &d) == MCP_OK) {
            num_id = (uint64_t)d;
            has_num_id = true;
        }
    } else if (id_type == MCP_ID_STRING) {
        str_id = mcp_message_id_string(ctx, resp);
        if (str_id != NULL) {
            char *end = NULL;
            unsigned long long parsed = strtoull(str_id, &end, 10);
            if (end != str_id && *end == '\0') {
                num_id = (uint64_t)parsed;
                has_num_id = true;
            }
        }
    }

    mcp_pending_req_t *matched = NULL;
    pthread_mutex_lock(&server->pending_lock);
    mcp_pending_req_t **curr = &server->pending_requests;
    while (*curr != NULL) {
        mcp_pending_req_t *node = *curr;
        bool match = false;
        if (node->str_id != NULL && str_id != NULL && strcmp(node->str_id, str_id) == 0) {
            match = true;
        } else if (has_num_id && node->req_id == num_id) {
            match = true;
        }
        if (match) {
            *curr = node->next;
            node->next = NULL;
            matched = node;
            break;
        }
        curr = &node->next;
    }
    pthread_mutex_unlock(&server->pending_lock);

    if (matched == NULL) {
        dlogf_srv(ctx, server, MCP_LOG_WARN, "event=unmatched_response");
        return MCP_ERR_NOT_FOUND;
    }

    if (matched->callback != NULL) {
        const mcp_json_value_t *result = mcp_message_result(ctx, resp);
        const mcp_json_value_t *error = mcp_message_error(ctx, resp);
        matched->callback(ctx, server, session != NULL ? session : matched->session,
                          matched->req_id, result, error, matched->user_data);
    }

    if (matched->str_id != NULL) {
        srv_free(ctx, matched->str_id);
    }
    srv_free(ctx, matched);
    return MCP_OK;
}

size_t mcp_server_sweep_pending_requests(mcp_context_t *ctx, mcp_server_t *server, uint64_t now_ms) {
    if (server == NULL) {
        return 0;
    }
    if (now_ms == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    }
    mcp_pending_req_t *expired_head = NULL;
    mcp_pending_req_t **expired_tail = &expired_head;
    size_t count = 0;

    pthread_mutex_lock(&server->pending_lock);
    mcp_pending_req_t **curr = &server->pending_requests;
    while (*curr != NULL) {
        mcp_pending_req_t *node = *curr;
        if (node->expire_at_ms > 0 && now_ms >= node->expire_at_ms) {
            *curr = node->next;
            node->next = NULL;
            *expired_tail = node;
            expired_tail = &node->next;
            count++;
        } else {
            curr = &node->next;
        }
    }
    pthread_mutex_unlock(&server->pending_lock);

    mcp_pending_req_t *node = expired_head;
    while (node != NULL) {
        mcp_pending_req_t *next = node->next;
        if (node->callback != NULL) {
            mcp_json_value_t *err_obj = mcp_json_object_new(ctx);
            if (err_obj != NULL) {
                mcp_json_object_set(ctx, err_obj, "code", mcp_json_number_new(ctx, (double)MCP_RPC_REQUEST_TIMEOUT));
                mcp_json_object_set(ctx, err_obj, "message", mcp_json_string_new(ctx, "Request timed out"));
            }
            node->callback(ctx, server, node->session, node->req_id, NULL, err_obj, node->user_data);
            if (err_obj != NULL) {
                mcp_json_destroy(ctx, err_obj);
            }
        }
        if (node->str_id != NULL) {
            srv_free(ctx, node->str_id);
        }
        srv_free(ctx, node);
        node = next;
    }
    return count;
}

mcp_status_t mcp_server_set_list_cache(mcp_context_t *ctx, mcp_server_t *srv,
                                        uint64_t ttl_ms, const char *cache_scope_or_null) {
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    srv->list_ttl_ms = ttl_ms;
    char *scope_copy = (cache_scope_or_null != NULL) ? srv_strdup(ctx, cache_scope_or_null) : NULL;
    if (cache_scope_or_null != NULL && scope_copy == NULL) {
        return MCP_ERR_NOMEM;
    }
    srv_free(ctx, srv->list_cache_scope);
    srv->list_cache_scope = scope_copy;
    return MCP_OK;
}

mcp_status_t mcp_server_set_response_meta(mcp_context_t *ctx, mcp_server_t *srv,
                                           mcp_json_value_t *meta_json) {
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *clone = (meta_json != NULL) ? mcp_json_clone(ctx, meta_json) : NULL;
    if (meta_json != NULL && clone == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_json_destroy(ctx, srv->response_meta);
    srv->response_meta = clone;
    return MCP_OK;
}

mcp_status_t mcp_server_set_capabilities(mcp_context_t *ctx, mcp_server_t *srv,
                                         const mcp_json_value_t *caps_json) {
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *clone = (caps_json != NULL) ? mcp_json_clone(ctx, caps_json) : NULL;
    if (caps_json != NULL && clone == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_json_destroy(ctx, srv->custom_capabilities);
    srv->custom_capabilities = clone;
    return MCP_OK;
}

mcp_status_t mcp_server_enable_tasks(mcp_context_t *ctx, mcp_server_t *srv) {
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (srv->task_mgr != NULL) {
        return MCP_OK;
    }
    srv->task_mgr = mcp_task_mgr_new(ctx);
    return srv->task_mgr != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

mcp_task_mgr_t *mcp_server_get_task_mgr(mcp_server_t *srv) {
    return srv != NULL ? srv->task_mgr : NULL;
}

mcp_status_t mcp_server_enable_skills(mcp_context_t *ctx, mcp_server_t *srv) {
    if (srv == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (srv->skill_reg != NULL) {
        return MCP_OK;
    }
    srv->skill_reg = mcp_skill_registry_new(ctx);
    return srv->skill_reg != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

mcp_skill_registry_t *mcp_server_get_skill_registry(mcp_server_t *srv) {
    return srv != NULL ? srv->skill_reg : NULL;
}

mcp_status_t mcp_server_deliver_notification(mcp_context_t *ctx, mcp_server_t *srv,
                                             mcp_session_t *sess, mcp_message_t *notif) {
    if (sess != NULL) {
        pthread_mutex_lock(&sess->direct_sub_lock);
        mcp_session_sink_fn sink = sess->notify_sink;
        void *ud = sess->notify_userdata;
        pthread_mutex_unlock(&sess->direct_sub_lock);
        if (sink != NULL) {
            char *text = mcp_message_serialize(ctx, notif);
            if (text != NULL) {
                sink(ctx, sess, text, strlen(text), ud);
                mcp_json_free_string(ctx, text);
            }
            mcp_message_destroy(ctx, notif);
            return MCP_OK;
        }
    }
    if (srv != NULL) {
        return outbox_push(ctx, srv, notif);
    }
    mcp_message_destroy(ctx, notif);
    return MCP_OK;
}

mcp_status_t mcp_server_notify_resource_updated(mcp_context_t *ctx, mcp_server_t *server,
                                                const char *uri) {
    if (server == NULL || uri == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&server->sessions_lock);
    for (size_t i = 0; i < server->n_sessions; i++) {
        mcp_session_t *sess = server->sessions[i];
        if (sess == NULL) continue;

        bool matches = false;
        if (sess->subscription_active) {
            mcp_json_value_t *tmp = mcp_json_object_new(ctx);
            if (tmp != NULL) {
                mcp_json_object_set(ctx, tmp, "uri", mcp_json_string_new(ctx, uri));
                matches = mcp_session_is_subscribed_to_notification(ctx, sess,
                                                                    "notifications/resources/updated", tmp);
                mcp_json_destroy(ctx, tmp);
            }
        } else {
            matches = mcp_session_is_subscribed_to_resource(sess, uri);
        }

        if (matches) {
            mcp_json_value_t *params = mcp_json_object_new(ctx);
            if (params != NULL) {
                mcp_json_object_set(ctx, params, "uri", mcp_json_string_new(ctx, uri));
            }
            mcp_message_t *notif = NULL;
            mcp_status_t st = mcp_session_build_notification(ctx, sess,
                                                            "notifications/resources/updated",
                                                            params, &notif);
            if (st == MCP_OK && notif != NULL) {
                mcp_server_deliver_notification(ctx, server, sess, notif);
            }
        }
    }
    pthread_mutex_unlock(&server->sessions_lock);
    return MCP_OK;
}

mcp_status_t mcp_server_notify_tools_changed(mcp_context_t *ctx, mcp_server_t *server) {
    if (server == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&server->sessions_lock);
    for (size_t i = 0; i < server->n_sessions; i++) {
        mcp_session_t *sess = server->sessions[i];
        if (sess == NULL) continue;

        bool matches = true;
        if (sess->subscription_active) {
            matches = mcp_session_is_subscribed_to_notification(ctx, sess,
                                                                "notifications/tools/list_changed", NULL);
        }

        if (matches) {
            mcp_message_t *notif = NULL;
            mcp_status_t st = mcp_session_build_notification(ctx, sess,
                                                            "notifications/tools/list_changed",
                                                            NULL, &notif);
            if (st == MCP_OK && notif != NULL) {
                mcp_server_deliver_notification(ctx, server, sess, notif);
            }
        }
    }
    pthread_mutex_unlock(&server->sessions_lock);
    return MCP_OK;
}

mcp_status_t mcp_server_notify_resources_changed(mcp_context_t *ctx, mcp_server_t *server) {
    if (server == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&server->sessions_lock);
    for (size_t i = 0; i < server->n_sessions; i++) {
        mcp_session_t *sess = server->sessions[i];
        if (sess == NULL) continue;

        bool matches = true;
        if (sess->subscription_active) {
            matches = mcp_session_is_subscribed_to_notification(ctx, sess,
                                                                "notifications/resources/list_changed", NULL);
        }

        if (matches) {
            mcp_message_t *notif = NULL;
            mcp_status_t st = mcp_session_build_notification(ctx, sess,
                                                            "notifications/resources/list_changed",
                                                            NULL, &notif);
            if (st == MCP_OK && notif != NULL) {
                mcp_server_deliver_notification(ctx, server, sess, notif);
            }
        }
    }
    pthread_mutex_unlock(&server->sessions_lock);
    return MCP_OK;
}

mcp_status_t mcp_server_notify_prompts_changed(mcp_context_t *ctx, mcp_server_t *server) {
    if (server == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&server->sessions_lock);
    for (size_t i = 0; i < server->n_sessions; i++) {
        mcp_session_t *sess = server->sessions[i];
        if (sess == NULL) continue;

        bool matches = true;
        if (sess->subscription_active) {
            matches = mcp_session_is_subscribed_to_notification(ctx, sess,
                                                                "notifications/prompts/list_changed", NULL);
        }

        if (matches) {
            mcp_message_t *notif = NULL;
            mcp_status_t st = mcp_session_build_notification(ctx, sess,
                                                            "notifications/prompts/list_changed",
                                                            NULL, &notif);
            if (st == MCP_OK && notif != NULL) {
                mcp_server_deliver_notification(ctx, server, sess, notif);
            }
        }
    }
    pthread_mutex_unlock(&server->sessions_lock);
    return MCP_OK;
}

static inline uint64_t fnv1a64_update(uint64_t hash, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t mcp_server_get_tools_revision(const mcp_server_t *server, mcp_tool_visibility_t vis) {

    if (server == NULL) {
        return 0;
    }
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < server->n_tools; i++) {
        const mcp_tool_t *t = server->tools[i];
        if (t == NULL) continue;
        bool visible = false;
        if (vis == MCP_TOOL_VIS_BOTH) {
            visible = true;
        } else if (vis == MCP_TOOL_VIS_APP) {
            visible = (t->vis == MCP_TOOL_VIS_APP || t->vis == MCP_TOOL_VIS_BOTH);
        } else {
            visible = (t->vis == MCP_TOOL_VIS_MODEL || t->vis == MCP_TOOL_VIS_BOTH);
        }
        if (!visible) continue;

        if (t->name != NULL) {
            hash = fnv1a64_update(hash, t->name, strlen(t->name));
        }
        uint8_t sep = 0x1f;
        hash = fnv1a64_update(hash, &sep, 1);
        if (t->description != NULL) {
            hash = fnv1a64_update(hash, t->description, strlen(t->description));
        }
        hash = fnv1a64_update(hash, &sep, 1);
        hash = fnv1a64_update(hash, &t->vis, sizeof(t->vis));
        hash = fnv1a64_update(hash, &t->required, sizeof(t->required));
    }
    return hash;
}

uint64_t mcp_server_get_resources_revision(const mcp_server_t *server) {
    if (server == NULL) {
        return 0;
    }
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < server->n_resources; i++) {
        const mcp_resource_t *r = server->resources[i];
        if (r == NULL) continue;
        if (r->uri != NULL) {
            hash = fnv1a64_update(hash, r->uri, strlen(r->uri));
        }
        uint8_t sep = 0x1f;
        hash = fnv1a64_update(hash, &sep, 1);
        if (r->name != NULL) {
            hash = fnv1a64_update(hash, r->name, strlen(r->name));
        }
        hash = fnv1a64_update(hash, &sep, 1);
        if (r->mime_type != NULL) {
            hash = fnv1a64_update(hash, r->mime_type, strlen(r->mime_type));
        }
        hash = fnv1a64_update(hash, &sep, 1);
    }
    return hash;
}

uint64_t mcp_server_get_prompts_revision(const mcp_server_t *server) {
    if (server == NULL) {
        return 0;
    }
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < server->n_prompts; i++) {
        const mcp_prompt_t *p = server->prompts[i];
        if (p == NULL) continue;
        if (p->name != NULL) {
            hash = fnv1a64_update(hash, p->name, strlen(p->name));
        }
        uint8_t sep = 0x1f;
        hash = fnv1a64_update(hash, &sep, 1);
        if (p->description != NULL) {
            hash = fnv1a64_update(hash, p->description, strlen(p->description));
        }
        hash = fnv1a64_update(hash, &sep, 1);
    }
    return hash;
}

mcp_status_t mcp_server_cancel_request(mcp_context_t *ctx, mcp_server_t *server,
                                       mcp_session_t *session,
                                       const char *req_id_str, double req_id_num,
                                       const char *reason) {
    if (server == NULL || session == NULL || (req_id_str == NULL && req_id_num == 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    if (params == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_json_value_t *id_val = req_id_str != NULL ? mcp_json_string_new(ctx, req_id_str)
                                                  : mcp_json_number_new(ctx, req_id_num);
    if (id_val == NULL) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    mcp_json_object_set_take(ctx, params, "requestId", id_val);

    if (reason != NULL) {
        mcp_json_value_t *r_val = mcp_json_string_new(ctx, reason);
        if (r_val != NULL) {
            mcp_json_object_set_take(ctx, params, "reason", r_val);
        }
    }

    mcp_message_t *notif = mcp_notification_new(ctx, "notifications/cancelled", params);
    if (notif == NULL) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_server_deliver_notification(ctx, server, session, notif);
}

static const char *log_level_to_mcp_string(mcp_log_level_t level) {
    switch (level) {
        case MCP_LOG_DEBUG: return "debug";
        case MCP_LOG_INFO:  return "info";
        case MCP_LOG_WARN:  return "warning";
        case MCP_LOG_ERROR: return "error";
        default:            return "info";
    }
}

mcp_status_t mcp_server_send_log(mcp_context_t *ctx, mcp_server_t *server,
                                 mcp_session_t *session,
                                 mcp_log_level_t level,
                                 const char *logger_name,
                                 const mcp_json_value_t *data) {
    if (server == NULL || data == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    if (session != NULL) {
        if (session->session_log_level_set && level < session->session_log_floor) {
            return MCP_OK;
        }
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        if (params == NULL) return MCP_ERR_NOMEM;
        mcp_json_object_set_take(ctx, params, "level", mcp_json_string_new(ctx, log_level_to_mcp_string(level)));
        if (logger_name != NULL) {
            mcp_json_object_set_take(ctx, params, "logger", mcp_json_string_new(ctx, logger_name));
        }
        mcp_json_value_t *data_clone = mcp_json_clone(ctx, data);
        if (data_clone == NULL) {
            mcp_json_destroy(ctx, params);
            return MCP_ERR_NOMEM;
        }
        mcp_json_object_set_take(ctx, params, "data", data_clone);

        mcp_message_t *notif = mcp_notification_new(ctx, "notifications/message", params);
        if (notif == NULL) {
            mcp_json_destroy(ctx, params);
            return MCP_ERR_NOMEM;
        }
        return mcp_server_deliver_notification(ctx, server, session, notif);
    }

    /* Broadcast to sessions matching filter */
    pthread_mutex_lock(&server->sessions_lock);
    for (size_t i = 0; i < server->n_sessions; i++) {
        mcp_session_t *sess = server->sessions[i];
        if (sess == NULL) continue;
        if (sess->session_log_level_set && level < sess->session_log_floor) {
            continue;
        }
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        if (params == NULL) continue;
        mcp_json_object_set_take(ctx, params, "level", mcp_json_string_new(ctx, log_level_to_mcp_string(level)));
        if (logger_name != NULL) {
            mcp_json_object_set_take(ctx, params, "logger", mcp_json_string_new(ctx, logger_name));
        }
        mcp_json_value_t *data_clone = mcp_json_clone(ctx, data);
        if (data_clone == NULL) {
            mcp_json_destroy(ctx, params);
            continue;
        }
        mcp_json_object_set_take(ctx, params, "data", data_clone);

        mcp_message_t *notif = mcp_notification_new(ctx, "notifications/message", params);
        if (notif != NULL) {
            mcp_server_deliver_notification(ctx, server, sess, notif);
        }
    }
    pthread_mutex_unlock(&server->sessions_lock);
    return MCP_OK;
}

mcp_status_t mcp_server_send_log_text(mcp_context_t *ctx, mcp_server_t *server,
                                      mcp_session_t *session,
                                      mcp_log_level_t level,
                                      const char *logger_name,
                                      const char *text_message) {
    if (server == NULL || text_message == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *data = mcp_json_string_new(ctx, text_message);
    if (data == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_server_send_log(ctx, server, session, level, logger_name, data);
    mcp_json_destroy(ctx, data);
    return st;
}



