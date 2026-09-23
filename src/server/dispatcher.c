/**
 * @file dispatcher.c
 *
 * Request dispatch pipeline: validate (L1/L2/L3) -> session gate ->
 * per-session duplicate-id check -> route. Returns MCP_OK with a
 * response set for every protocol outcome (including validation and
 * route errors); INVALID_ARGUMENT only for malformed caller arguments
 * or a non-request message, NOMEM when the response cannot be built.
 *
 * process_one consumes (destroys) the incoming message on both the
 * notification and dispatch paths.
 */
#include "mcpkit/server/dispatcher.h"

#define _DEFAULT_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "internals.h"
#include "../method_table.h"
#include "mcpkit/core/context.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/schema.h"
#include "mcpkit/logging/logger.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/mrtr.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"

static mcp_message_t *err_resp(mcp_context_t *ctx, const mcp_message_t *req, int code,
                               const char *text) {
    return mcp_response_err_new(ctx, req, code, text, NULL);
}

// Flat k=v dispatch log; NULL-ctx safe (NULL logger discards).
static void dlogf(mcp_context_t *ctx, mcp_log_level_t level, const char *fmt, ...) {
    mcp_logger_t *lg = mcp_context_logger(ctx);
    if (lg == NULL) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mcp_logger_log(lg, level, buf);
}

// Dispatch-route log variant: consults the server's log floor set by
// logging/setLevel so an operator can quiet verbose routes at runtime.
// Serve-loop stations (loop.c/stdio.c) have no server and stay
// logger-level-filtered only.
static void dlogf_srv(mcp_context_t *ctx, mcp_server_t *srv, mcp_log_level_t level,
                       const char *fmt, ...) {
    if (level < (mcp_log_level_t)atomic_load(&srv->log_floor)) {
        return;
    }
    // Forward to the plain variant to keep a single formatting path.
    va_list ap;
    va_start(ap, fmt);
    mcp_logger_t *lg = mcp_context_logger(ctx);
    if (lg == NULL) {
        va_end(ap);
        return;
    }
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mcp_logger_log(lg, level, buf);
}

// Monotonic nanoseconds for tracer durations.
static uint64_t trace_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void trace_begin(mcp_context_t *ctx, mcp_server_t *srv, const char *method) {
    if (srv->tracer != NULL) {
        srv->tracer(ctx, MCP_TRACE_BEGIN, method, 0, 0, srv->tracer_ud);
    }
}

static void trace_end(mcp_context_t *ctx, mcp_server_t *srv, const char *method,
                      uint64_t t0, int status) {
    if (srv->tracer != NULL) {
        srv->tracer(ctx, MCP_TRACE_END, method, status, trace_now() - t0, srv->tracer_ud);
    }
}

#define MCP_LIST_PAGE_SIZE 100u

static size_t page_offset(mcp_context_t *ctx, const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const mcp_json_value_t *cur = params != NULL ? mcp_json_object_get(ctx, params, "cursor") : NULL;
    if (cur == NULL) {
        return 0;
    }
    const char *s = NULL;
    if (mcp_json_type(ctx, cur) != MCP_JSON_STRING ||
        mcp_json_string_value(ctx, cur, &s) != MCP_OK || s == NULL || *s == '\0') {
        return (size_t)-1;
    }
    size_t off = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return (size_t)-1;
        }
        off = off * 10u + (size_t)(*p - '0');
    }
    return off;
}

static mcp_status_t set_next_cursor(mcp_context_t *ctx, mcp_json_value_t *result, size_t end) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%zu", end);
    mcp_json_value_t *v = mcp_json_string_new(ctx, buf);
    if (v == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, result, "nextCursor", v) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

static mcp_status_t set_string(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key,
                               const char *val) {
    mcp_json_value_t *v = mcp_json_string_new(ctx, val);
    if (v == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set(ctx, obj, key, v) != MCP_OK) {
        mcp_json_destroy(ctx, v);
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

static mcp_status_t get_string(mcp_context_t *ctx, const mcp_json_value_t *obj,
                               const char *key, const char **out) {
    const mcp_json_value_t *v = mcp_json_object_get(ctx, obj, key);
    if (v == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    return mcp_json_string_value(ctx, v, out);
}

static mcp_tool_t *find_tool(mcp_server_t *srv, const char *name) {
    for (size_t i = 0; i < srv->n_tools; i++) {
        if (strcmp(srv->tools[i]->name, name) == 0) {
            return srv->tools[i];
        }
    }
    return NULL;
}

static mcp_resource_t *find_resource(mcp_server_t *srv, const char *uri) {
    for (size_t i = 0; i < srv->n_resources; i++) {
        if (strcmp(srv->resources[i]->uri, uri) == 0) {
            return srv->resources[i];
        }
    }
    return NULL;
}

static mcp_prompt_t *find_prompt(mcp_server_t *srv, const char *name) {
    for (size_t i = 0; i < srv->n_prompts; i++) {
        if (strcmp(srv->prompts[i]->name, name) == 0) {
            return srv->prompts[i];
        }
    }
    return NULL;
}

// Internal helper: decorates a result object before mcp_response_ok_new.
// - is_list=true:  inject resultType + ttlMs + cacheScope + _meta
// - is_list=false: inject resultType only
// NOMEM: caller must destroy result and return NULL from the route.
static mcp_status_t decorate_result(mcp_context_t *ctx, mcp_server_t *srv,
                                    mcp_json_value_t *result, bool is_list) {
    if (mcp_result_inject_result_type(ctx, result) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    if (!is_list) {
        return MCP_OK;
    }
    if (srv->list_ttl_ms != 0) {
        mcp_json_value_t *ttl = mcp_json_number_new(ctx, (double)srv->list_ttl_ms);
        if (ttl == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, result, "ttlMs", ttl) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    }
    if (srv->list_cache_scope != NULL) {
        mcp_json_value_t *scope = mcp_json_string_new(ctx, srv->list_cache_scope);
        if (scope == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, result, "cacheScope", scope) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    }
    if (srv->response_meta != NULL) {
        mcp_json_value_t *meta_clone = mcp_json_clone(ctx, srv->response_meta);
        if (meta_clone == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_result_inject_meta(ctx, result, meta_clone) != MCP_OK) {
            // set_take consumed meta_clone on failure; do not free again.
            return MCP_ERR_NOMEM;
        }
    }
    return MCP_OK;
}

static mcp_message_t *route_server_discover(mcp_context_t *ctx, mcp_server_t *srv,
                                              const mcp_message_t *req) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return NULL;
    }
    mcp_json_value_t *v;
    if ((v = mcp_json_string_new(ctx, srv->name)) == NULL ||
        mcp_json_object_set_take(ctx, result, "serverName", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_string_new(ctx, srv->version)) == NULL ||
        mcp_json_object_set_take(ctx, result, "serverVersion", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_string_new(ctx, MCP_PROTOCOL_VERSION_LATEST)) == NULL ||
        mcp_json_object_set_take(ctx, result, "protocolVersion", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_number_new(ctx, (double)srv->n_tools)) == NULL ||
        mcp_json_object_set_take(ctx, result, "toolsCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_number_new(ctx, (double)srv->n_resources)) == NULL ||
        mcp_json_object_set_take(ctx, result, "resourcesCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_number_new(ctx, (double)srv->n_prompts)) == NULL ||
        mcp_json_object_set_take(ctx, result, "promptsCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (srv->list_ttl_ms != 0) {
        if ((v = mcp_json_number_new(ctx, (double)srv->list_ttl_ms)) == NULL ||
            mcp_json_object_set_take(ctx, result, "listTtlMs", v) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    if (srv->list_cache_scope != NULL) {
        if ((v = mcp_json_string_new(ctx, srv->list_cache_scope)) == NULL ||
            mcp_json_object_set_take(ctx, result, "listCacheScope", v) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    if ((v = mcp_json_bool_new(ctx, true)) == NULL ||
        mcp_json_object_set_take(ctx, result, "supportsStateless", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_bool_new(ctx, true)) == NULL ||
        mcp_json_object_set_take(ctx, result, "supportsMeta", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_initialize(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                       const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *ver = NULL;
    if (get_string(ctx, params, "protocolVersion", &ver) != MCP_OK || ver == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "initialize: missing protocolVersion");
    }
    if (mcp_protocol_negotiate(ctx, ver) == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "initialize: unsupported version");
    }
    const mcp_json_value_t *info = mcp_json_object_get(ctx, params, "clientInfo");
    if (info != NULL) {
        const char *name = NULL;
        const char *cver = NULL;
        if (get_string(ctx, info, "name", &name) == MCP_OK && name != NULL) {
            char *copy = srv_strdup(ctx, name);
            if (copy == NULL) {
                return NULL;
            }
            srv_free(ctx, s->client_name);
            s->client_name = copy;
        }
        if (get_string(ctx, info, "version", &cver) == MCP_OK && cver != NULL) {
            char *copy = srv_strdup(ctx, cver);
            if (copy == NULL) {
                return NULL;
            }
            srv_free(ctx, s->client_version);
            s->client_version = copy;
        }
    }
    mcp_json_value_t *result = mcp_initialize_result_new(ctx, srv->name, srv->version);
    if (result == NULL) {
        return NULL;
    }
    const mcp_json_value_t *meta = mcp_message_meta(ctx, req);
    if (meta != NULL) {
        mcp_json_value_t *clone = mcp_json_clone(ctx, meta);
        if (clone == NULL) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_json_destroy(ctx, s->client_meta);
        s->client_meta = clone;
    }
    if (decorate_result(ctx, srv, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_tools_list(mcp_context_t *ctx, mcp_server_t *srv,
                                       mcp_session_t *s, const mcp_message_t *req) {
    size_t offset = page_offset(ctx, req);
    if (offset == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/list: bad cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    size_t visible = 0;
    for (size_t i = 0; i < srv->n_tools; i++) {
        const mcp_tool_t *t = srv->tools[i];
        if (t->vis != MCP_TOOL_VIS_BOTH && (t->vis == MCP_TOOL_VIS_APP) != s->apps_host) {
            continue;
        }
        size_t idx = visible++;
        if (idx < offset || idx >= offset + MCP_LIST_PAGE_SIZE) {
            continue;
        }
        mcp_json_value_t *entry = mcp_json_object_new(ctx);
        if (entry == NULL || set_string(ctx, entry, "name", t->name) != MCP_OK ||
            (t->description != NULL && set_string(ctx, entry, "description", t->description) != MCP_OK)) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }
        if (t->schema != NULL) {
            mcp_json_value_t *copy = mcp_json_clone(ctx, t->schema);
            if (copy == NULL ||
                mcp_json_object_set_take(ctx, entry, "inputSchema", copy) != MCP_OK) {
                mcp_json_destroy(ctx, entry);
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                return NULL;
            }
        }
        if (mcp_json_array_append(ctx, arr, entry) != MCP_OK) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }
    }
    if (mcp_json_object_set_take(ctx, result, "tools", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (offset + MCP_LIST_PAGE_SIZE < visible &&
        set_next_cursor(ctx, result, offset + MCP_LIST_PAGE_SIZE) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *tool_error_result(mcp_context_t *ctx, const mcp_message_t *req,
                                        mcp_status_t st) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    if (result == NULL || content == NULL || item == NULL) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (set_string(ctx, item, "type", "text") != MCP_OK ||
        set_string(ctx, item, "text", mcp_status_string(st)) != MCP_OK ||
        mcp_json_array_append(ctx, content, item) != MCP_OK) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    /* item now owned by content; take consumes content/flag on all paths */
    if (mcp_json_object_set_take(ctx, result, "content", content) != MCP_OK ||
        mcp_json_object_set_take(ctx, result, "isError", mcp_json_bool_new(ctx, true)) !=
            MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_tools_call(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                       const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *name = NULL;
    if (params == NULL || get_string(ctx, params, "name", &name) != MCP_OK || name == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/call: missing name");
    }
    mcp_tool_t *tool = find_tool(srv, name);
    if (tool == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/call: unknown tool");
    }
    if (tool->vis != MCP_TOOL_VIS_BOTH && (tool->vis == MCP_TOOL_VIS_APP) != s->apps_host) {
        return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "tools/call: unknown tool");
    }
    const mcp_json_value_t *args = mcp_json_object_get(ctx, params, "arguments");
    const mcp_json_value_t *rs_val = mcp_json_object_get(ctx, params, "requestState");
    bool is_mrtr_retry = (tool->handler_v2 != NULL && rs_val != NULL);

    if (tool->schema != NULL && !is_mrtr_retry) {
        mcp_status_t vst;
        if (args != NULL) {
            vst = mcp_schema_validate(ctx, tool->schema, args);
        } else {
            mcp_json_value_t *null = mcp_json_null_new(ctx);
            vst = null == NULL ? MCP_ERR_NOMEM : mcp_schema_validate(ctx, tool->schema, null);
            mcp_json_destroy(ctx, null);
        }
        if (vst != MCP_OK) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/call: invalid arguments");
        }
    }
    if ((tool->required & ~s->granted) != 0) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/call: permission denied");
    }
    mcp_json_value_t *result = NULL;
    mcp_status_t st;
    if (tool->handler_v2 != NULL) {
        /* V2 path: extract MRTR fields and populate call context */
        mcp_tool_call_ctx_t call_ctx;
        call_ctx.args            = args;
        call_ctx.input_responses = mcp_json_object_get(ctx, params, "inputResponses");
        const char *rs_str = NULL;
        if (rs_val != NULL && mcp_json_type(ctx, rs_val) == MCP_JSON_STRING) {
            mcp_json_string_value(ctx, rs_val, &rs_str);
        }
        call_ctx.request_state = rs_str;
        st = tool->handler_v2(ctx, s, &call_ctx, tool->user_data, &result);
    } else {
        /* V1 path: unchanged */
        st = tool->handler(ctx, s, args, tool->user_data, &result);
    }
    atomic_fetch_add(&srv->c_tools_called, 1);
    if (st != MCP_OK) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=tool_error tool=%s status=%s", name,
              mcp_status_string(st));
        return tool_error_result(ctx, req, st);
    }
    if (result == NULL) {
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "tools/call: empty result");
    }
    /* For InputRequiredResult, skip the "complete" decoration and pass
     * the result through as-is so the client gets the full MRTR payload. */
    if (!mcp_mrtr_is_input_required(ctx, result)) {
        if (decorate_result(ctx, srv, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_resources_list(mcp_context_t *ctx, mcp_server_t *srv,
                                           const mcp_message_t *req) {
    size_t offset = page_offset(ctx, req);
    if (offset == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "resources/list: bad cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    size_t end = offset + MCP_LIST_PAGE_SIZE < srv->n_resources ? offset + MCP_LIST_PAGE_SIZE
                                                                : srv->n_resources;
    for (size_t i = offset; i < end; i++) {
        const mcp_resource_t *r = srv->resources[i];
        mcp_json_value_t *entry = mcp_json_object_new(ctx);
        if (entry == NULL || set_string(ctx, entry, "uri", r->uri) != MCP_OK ||
            set_string(ctx, entry, "name", r->name) != MCP_OK ||
            (r->mime_type != NULL && set_string(ctx, entry, "mimeType", r->mime_type) != MCP_OK) ||
            mcp_json_array_append(ctx, arr, entry) != MCP_OK) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }
    }
    if (mcp_json_object_set_take(ctx, result, "resources", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (end < srv->n_resources && set_next_cursor(ctx, result, end) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_resources_read(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                           const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *uri = NULL;
    if (params == NULL || get_string(ctx, params, "uri", &uri) != MCP_OK || uri == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "resources/read: missing uri");
    }
    mcp_resource_t *res = find_resource(srv, uri);
    if (res == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "resources/read: unknown resource");
    }
    if (res->on_read == NULL) {
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "resources/read: no reader");
    }
    mcp_json_value_t *contents = NULL;
    mcp_status_t st = res->on_read(ctx, s, uri, res->user_data, &contents);
    if (st != MCP_OK) {
        return err_resp(ctx, req, mcp_status_to_rpc_code(st), "resources/read: handler failed");
    }
    if (contents == NULL || mcp_json_type(ctx, contents) != MCP_JSON_ARRAY) {
        mcp_json_destroy(ctx, contents);
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "resources/read: bad contents");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        mcp_json_destroy(ctx, contents);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, result, "contents", contents) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_prompts_list(mcp_context_t *ctx, mcp_server_t *srv,
                                         const mcp_message_t *req) {
    size_t offset = page_offset(ctx, req);
    if (offset == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "prompts/list: bad cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    size_t end = offset + MCP_LIST_PAGE_SIZE < srv->n_prompts ? offset + MCP_LIST_PAGE_SIZE
                                                              : srv->n_prompts;
    for (size_t i = offset; i < end; i++) {
        const mcp_prompt_t *p = srv->prompts[i];
        mcp_json_value_t *entry = mcp_json_object_new(ctx);
        if (entry == NULL || set_string(ctx, entry, "name", p->name) != MCP_OK ||
            (p->description != NULL &&
             set_string(ctx, entry, "description", p->description) != MCP_OK) ||
            mcp_json_array_append(ctx, arr, entry) != MCP_OK) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }
    }
    if (mcp_json_object_set_take(ctx, result, "prompts", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (end < srv->n_prompts && set_next_cursor(ctx, result, end) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_prompts_get(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                        const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *name = NULL;
    if (params == NULL || get_string(ctx, params, "name", &name) != MCP_OK || name == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "prompts/get: missing name");
    }
    mcp_prompt_t *prompt = find_prompt(srv, name);
    if (prompt == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "prompts/get: unknown prompt");
    }
    if (prompt->on_get == NULL) {
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "prompts/get: no handler");
    }
    const mcp_json_value_t *args = mcp_json_object_get(ctx, params, "arguments");
    mcp_json_value_t *messages = NULL;
    mcp_status_t st = prompt->on_get(ctx, s, args, prompt->user_data, &messages);
    if (st != MCP_OK) {
        return err_resp(ctx, req, mcp_status_to_rpc_code(st), "prompts/get: handler failed");
    }
    if (messages == NULL || mcp_json_type(ctx, messages) != MCP_JSON_ARRAY) {
        mcp_json_destroy(ctx, messages);
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "prompts/get: bad messages");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        mcp_json_destroy(ctx, messages);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, result, "messages", messages) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (prompt->description != NULL && set_string(ctx, result, "description", prompt->description) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_completion_list(mcp_context_t *ctx, mcp_server_t *srv,
                                            const mcp_message_t *req) {
    // Return an empty array – completion/list has no meaningful payload.
    // The cursor is still validated so a bad cursor fails consistently
    // across all list methods.
    if (page_offset(ctx, req) == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "completion/list: bad cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *comps = mcp_json_array_new(ctx);
    if (result == NULL || comps == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, comps);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, result, "completions", comps) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_completion_complete(mcp_context_t *ctx, mcp_server_t *srv,
                                                mcp_session_t *s, const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const mcp_json_value_t *ref_obj = params != NULL ? mcp_json_object_get(ctx, params, "ref") : NULL;
    if (ref_obj == NULL || mcp_json_type(ctx, ref_obj) != MCP_JSON_OBJECT) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS,
                        "completion/complete: missing ref object");
    }
    const char *ref_str = NULL;
    if (get_string(ctx, ref_obj, "value", &ref_str) != MCP_OK || ref_str == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS,
                        "completion/complete: ref.value missing or not a string");
    }
    // Find the first registered provider whose ref_prefix matches the reference.
    for (size_t i = 0; i < srv->n_completions; i++) {
        size_t plen = strlen(srv->completions[i].ref_prefix);
        if (strncmp(ref_str, srv->completions[i].ref_prefix, plen) == 0) {
            const mcp_json_value_t *args_ref = mcp_json_object_get(ctx, params, "argument");
            // Clone so the provider never sees a dangling pointer into the request
            // DOM (which the caller destroys after mcp_server_dispatch returns).
            mcp_json_value_t *args;
            if (args_ref != NULL) {
                args = mcp_json_clone(ctx, args_ref);
                if (args == NULL) {
                    return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR,
                                    "completion/complete: arg clone failed");
                }
            } else {
                args = mcp_json_null_new(ctx);
                if (args == NULL) {
                    return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR,
                                    "completion/complete: arg clone failed");
                }
            }
            mcp_json_value_t *comps = srv->completions[i].fn(ctx, s, args,
                                                               srv->completions[i].user_data);
            mcp_json_destroy(ctx, args);
            if (comps == NULL) {
                return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR,
                                "completion/complete: provider returned NULL");
            }
            if (mcp_json_type(ctx, comps) != MCP_JSON_ARRAY) {
                mcp_json_destroy(ctx, comps);
                return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR,
                                "completion/complete: provider returned non-array");
            }
            mcp_json_value_t *result = mcp_json_object_new(ctx);
            if (result == NULL) {
                mcp_json_destroy(ctx, comps);
                return NULL;
            }
            if (mcp_json_object_set_take(ctx, result, "completions", comps) != MCP_OK) {
                mcp_json_destroy(ctx, result);
                return NULL;
            }
            if (decorate_result(ctx, srv, result, false) != MCP_OK) {
                mcp_json_destroy(ctx, result);
                return NULL;
            }
            mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
            if (resp == NULL) {
                mcp_json_destroy(ctx, result);
            }
            return resp;
        }
    }
    // No matching provider registered – return empty completions.
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *comps = mcp_json_array_new(ctx);
    if (result == NULL || comps == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, comps);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, result, "completions", comps) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_advanced(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                     const mcp_message_t *req, const char *method) {
    (void)s;
    // Returns NULL when method is not one of the advanced routes so the
    // caller falls through to the unknown_method handler.
    if (strcmp(method, "logging/setLevel") == 0) {
        const mcp_json_value_t *params = mcp_message_params(ctx, req);
        const char *lvl = NULL;
        if (params == NULL || get_string(ctx, params, "level", &lvl) != MCP_OK || lvl == NULL) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "logging/setLevel: missing level");
        }
        mcp_log_level_t mapped;
        if (strcmp(lvl, "debug") == 0) {
            mapped = MCP_LOG_DEBUG;
        } else if (strcmp(lvl, "info") == 0 || strcmp(lvl, "notice") == 0) {
            mapped = MCP_LOG_INFO;
        } else if (strcmp(lvl, "warning") == 0 || strcmp(lvl, "warn") == 0) {
            mapped = MCP_LOG_WARN;
        } else if (strcmp(lvl, "error") == 0) {
            mapped = MCP_LOG_ERROR;
        } else {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "logging/setLevel: bad level");
        }
        atomic_store(&srv->log_floor, (int)mapped);
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        if (result == NULL) {
            return NULL;
        }
        if (decorate_result(ctx, srv, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    if (strcmp(method, "resources/subscribe") == 0 ||
        strcmp(method, "resources/unsubscribe") == 0) {
        const mcp_json_value_t *params = mcp_message_params(ctx, req);
        const char *uri = NULL;
        if (params == NULL || get_string(ctx, params, "uri", &uri) != MCP_OK || uri == NULL) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "missing uri");
        }
        bool is_sub = strcmp(method, "resources/subscribe") == 0;
        pthread_mutex_lock(&srv->subscribed_lock);
        {
            if (is_sub) {
                bool seen = false;
                for (size_t i = 0; i < srv->n_subscribed; i++) {
                    if (strcmp(srv->subscribed_uris[i], uri) == 0) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    if (srv->n_subscribed == srv->cap_subscribed) {
                        size_t ncap = srv->cap_subscribed == 0 ? 4 : srv->cap_subscribed * 2;
                        char **nu = srv_realloc(ctx, srv->subscribed_uris, ncap * sizeof(*nu));
                        if (nu == NULL) {
                            pthread_mutex_unlock(&srv->subscribed_lock);
                            return NULL;
                        }
                        srv->subscribed_uris = nu;
                        srv->cap_subscribed = ncap;
                    }
                    char *copy = srv_strdup(ctx, uri);
                    if (copy == NULL) {
                        pthread_mutex_unlock(&srv->subscribed_lock);
                        return NULL;
                    }
                    srv->subscribed_uris[srv->n_subscribed++] = copy;
                }
            } else {
                for (size_t i = 0; i < srv->n_subscribed; i++) {
                    if (strcmp(srv->subscribed_uris[i], uri) == 0) {
                        srv_free(ctx, srv->subscribed_uris[i]);
                        srv->subscribed_uris[i] = srv->subscribed_uris[--srv->n_subscribed];
                        break;
                    }
                }
            }
        }
        pthread_mutex_unlock(&srv->subscribed_lock);
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        if (result == NULL) {
            return NULL;
        }
        if (decorate_result(ctx, srv, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    if (strcmp(method, "resources/templates/list") == 0) {
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        if (result == NULL || arr == NULL) {
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }
        if (mcp_json_object_set_take(ctx, result, "templates", arr) != MCP_OK) {
            // set_take owns (and freed) arr on failure; result is still ours.
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        if (decorate_result(ctx, srv, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    return NULL;
}

static mcp_message_t *route_request(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                     const mcp_message_t *req, const char *method) {
    // Method names come from method_table.h so the route table and the
    // validate.c L2 table can never drift apart.
    if (strcmp(method, k_mcp_server_methods[0]) == 0) {
        return route_initialize(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[1]) == 0) {
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        if (result == NULL) {
            return NULL;
        }
        if (decorate_result(ctx, srv, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    if (strcmp(method, k_mcp_server_methods[2]) == 0) {
        return route_tools_list(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[3]) == 0) {
        return route_tools_call(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[4]) == 0) {
        return route_resources_list(ctx, srv, req);
    }
    if (strcmp(method, k_mcp_server_methods[5]) == 0) {
        return route_resources_read(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[6]) == 0) {
        return route_prompts_list(ctx, srv, req);
    }
    if (strcmp(method, k_mcp_server_methods[7]) == 0) {
        return route_prompts_get(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[8]) == 0) {
        return route_completion_list(ctx, srv, req);
    }
    if (strcmp(method, k_mcp_server_methods[9]) == 0) {
        return route_completion_complete(ctx, srv, s, req);
    }
    mcp_message_t *adv = route_advanced(ctx, srv, s, req, method);
    if (adv != NULL) {
        return adv;
    }
    if (strcmp(method, k_mcp_server_methods[15]) == 0) {
        return route_server_discover(ctx, srv, req);
    }
    dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=unknown_method method=%s", method);
    // No counter or trace here: dispatch counts error responses and
    // fires END centrally after route_request returns; doing it here
    // would double both.
    return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "unknown method");
}

mcp_status_t mcp_server_dispatch(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *session,
                                 const mcp_message_t *req, mcp_message_t **resp_out) {
    if (srv == NULL || session == NULL || req == NULL || resp_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *resp_out = NULL;
    if (mcp_message_kind(ctx, req) != MCP_MSG_REQUEST) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    atomic_fetch_add(&srv->c_requests_total, 1);
    int code = 0;
    if (mcp_message_validate(ctx, req, &code) != MCP_OK) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=invalid_request code=%d", code);
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, code, "invalid request");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    const char *method = mcp_message_method(ctx, req);
    if (method == NULL) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=missing_method");
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "missing method");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    bool is_stateless = (mcp_message_meta(ctx, req) != NULL);
    bool is_discover = (strcmp(method, "server/discover") == 0);
    if (!session->initialized && strcmp(method, "initialize") != 0 && !is_discover && !is_stateless) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=uninitialized method=%s", method);
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "session not initialized");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    mcp_id_type_t id_type = mcp_message_id_type(ctx, req);
    if (id_type != MCP_ID_NONE) {
        const char *id_str = mcp_message_id_string(ctx, req);
        double id_num = 0;
        mcp_message_id_number(ctx, req, &id_num);
        if (mcp_idset_contains(ctx, session->ids, id_type, id_str, id_num)) {
            dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=duplicate_id method=%s", method);
            atomic_fetch_add(&srv->c_requests_error, 1);
            *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "duplicate request id");
            return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
        }
        if (mcp_idset_add(ctx, session->ids, id_type, id_str, id_num) != MCP_OK) {
            dlogf_srv(ctx, srv, MCP_LOG_ERROR, "event=id_tracking_failed method=%s", method);
            atomic_fetch_add(&srv->c_requests_error, 1);
            *resp_out = err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "id tracking failed");
            return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
        }
    }
    uint64_t t0 = srv->tracer != NULL ? trace_now() : 0;
    trace_begin(ctx, srv, method);
    mcp_message_t *resp = route_request(ctx, srv, session, req, method);
    if (resp == NULL) {
        dlogf_srv(ctx, srv, MCP_LOG_ERROR, "event=route_failed method=%s", method);
        atomic_fetch_add(&srv->c_requests_error, 1);
        trace_end(ctx, srv, method, t0, MCP_RPC_INTERNAL_ERROR);
        *resp_out = err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "internal error");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    int ecode = 0;
    int end_status = mcp_message_error_code(ctx, resp, &ecode) == MCP_OK ? ecode : 0;
    if (end_status != 0) {
        atomic_fetch_add(&srv->c_requests_error, 1);
    }
    trace_end(ctx, srv, method, t0, end_status);
    *resp_out = resp;
    return MCP_OK;
}

mcp_status_t mcp_server_notify(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *session,
                               const mcp_message_t *notif) {
    if (srv == NULL || session == NULL || notif == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_message_kind(ctx, notif) != MCP_MSG_NOTIFICATION) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const char *method = mcp_message_method(ctx, notif);
    if (method == NULL) {
        return MCP_OK;
    }
    if (strcmp(method, "notifications/initialized") == 0) {
        session->initialized = true;
    }
    bool advanced = false;
    for (int i = 0; i < (int)MCP_SERVER_NOTIFICATION_COUNT; i++) {
        if (strcmp(method, k_mcp_server_notifications[i]) == 0) {
            advanced = true;
            break;
        }
    }
    if (advanced) {
        dlogf_srv(ctx, srv, MCP_LOG_DEBUG, "event=notification method=%s", method);
    }
    atomic_fetch_add(&srv->c_notifications_total, 1);
    return MCP_OK;
}

mcp_queue_t *mcp_queue_create(mcp_context_t *ctx) {
    mcp_queue_t *q = srv_malloc(ctx, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }
    memset(q, 0, sizeof(*q));
    return q;
}

void mcp_queue_destroy(mcp_context_t *ctx, mcp_queue_t *q) {
    if (q == NULL) {
        return;
    }
    for (size_t i = 0; i < q->len; i++) {
        mcp_message_destroy(ctx, q->msgs[(q->head + i) % q->cap]);
    }
    srv_free(ctx, q->sessions);
    srv_free(ctx, q->msgs);
    srv_free(ctx, q);
}

static mcp_status_t queue_grow(mcp_context_t *ctx, mcp_queue_t *q) {
    if (q->len < q->cap && q->head == 0) {
        return MCP_OK;
    }
    if (q->head > 0) {
        for (size_t i = 0; i < q->len; i++) {
            size_t src = (q->head + i) % q->cap;
            if (src != i) {
                q->sessions[i] = q->sessions[src];
                q->msgs[i] = q->msgs[src];
            }
        }
        q->head = 0;
        if (q->len < q->cap) {
            return MCP_OK;
        }
    }
    size_t ncap = q->cap == 0 ? 8 : q->cap * 2;
    mcp_message_t **om = q->msgs;
    struct mcp_session **os = q->sessions;
    mcp_message_t **nm = srv_realloc(ctx, om, ncap * sizeof(*nm));
    if (nm == NULL) {
        return MCP_ERR_NOMEM;
    }
    /* Commit msgs before touching sessions: if the sessions realloc fails,
     * q->msgs already points to a valid (possibly larger) buffer and the
     * ring logic still operates within the old cap, so no dangling pointer. */
    q->msgs = nm;
    struct mcp_session **ns = srv_realloc(ctx, os, ncap * sizeof(*ns));
    if (ns == NULL) {
        return MCP_ERR_NOMEM;
    }
    q->sessions = ns;
    q->cap = ncap;
    return MCP_OK;
}

mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session,
                            mcp_message_t *msg) {
    if (q == NULL || session == NULL || msg == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (q->len >= MCP_QUEUE_MAX_LEN) {
        return MCP_ERR_NOMEM;
    }
    if (queue_grow(ctx, q) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    size_t slot = (q->head + q->len) % q->cap;
    q->sessions[slot] = session;
    q->msgs[slot] = msg;
    q->len++;
    return MCP_OK;
}

mcp_status_t mcp_queue_pop(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t **session_out,
                           mcp_message_t **msg_out) {
    (void)ctx;
    if (q == NULL || session_out == NULL || msg_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (q->len == 0) {
        return MCP_ERR_NOT_FOUND;
    }
    *session_out = q->sessions[q->head];
    *msg_out = q->msgs[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return MCP_OK;
}

size_t mcp_queue_size(mcp_context_t *ctx, const mcp_queue_t *q) {
    (void)ctx;
    return q == NULL ? 0 : q->len;
}

mcp_status_t mcp_server_process_one(mcp_context_t *ctx, mcp_server_t *srv, mcp_queue_t *q,
                                    mcp_message_t **resp_out) {
    if (srv == NULL || q == NULL || resp_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *resp_out = NULL;
    mcp_session_t *session = NULL;
    mcp_message_t *msg = NULL;
    if (mcp_queue_pop(ctx, q, &session, &msg) != MCP_OK) {
        return MCP_ERR_NOT_FOUND;
    }
    if (mcp_message_kind(ctx, msg) == MCP_MSG_NOTIFICATION) {
        mcp_status_t st = mcp_server_notify(ctx, srv, session, msg);
        mcp_message_destroy(ctx, msg);
        return st;
    }
    mcp_status_t st = mcp_server_dispatch(ctx, srv, session, msg, resp_out);
    mcp_message_destroy(ctx, msg);
    return st;
}
