#include "mcpkit/server/dispatcher.h"

#include <string.h>

#include "internals.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/schema.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"

static mcp_message_t *err_resp(mcp_context_t *ctx, const mcp_message_t *req, int code,
                               const char *text) {
    return mcp_response_err_new(ctx, req, code, text, NULL);
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
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_tools_list(mcp_context_t *ctx, mcp_server_t *srv,
                                       const mcp_message_t *req) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    for (size_t i = 0; i < srv->n_tools; i++) {
        const mcp_tool_t *t = srv->tools[i];
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
            if (copy == NULL || mcp_json_object_set(ctx, entry, "inputSchema", copy) != MCP_OK) {
                mcp_json_destroy(ctx, copy);
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
    if (mcp_json_object_set(ctx, result, "tools", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
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
    if (result == NULL || content == NULL || item == NULL ||
        set_string(ctx, item, "type", "text") != MCP_OK ||
        set_string(ctx, item, "text", mcp_status_string(st)) != MCP_OK ||
        mcp_json_array_append(ctx, content, item) != MCP_OK ||
        mcp_json_object_set(ctx, result, "content", content) != MCP_OK ||
        mcp_json_object_set(ctx, result, "isError", mcp_json_bool_new(ctx, true)) != MCP_OK) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
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
    const mcp_json_value_t *args = mcp_json_object_get(ctx, params, "arguments");
    if (tool->schema != NULL) {
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
    mcp_json_value_t *result = NULL;
    mcp_status_t st = tool->handler(ctx, s, args, tool->user_data, &result);
    if (st != MCP_OK) {
        return tool_error_result(ctx, req, st);
    }
    if (result == NULL) {
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "tools/call: empty result");
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_resources_list(mcp_context_t *ctx, mcp_server_t *srv,
                                           const mcp_message_t *req) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    for (size_t i = 0; i < srv->n_resources; i++) {
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
    if (mcp_json_object_set(ctx, result, "resources", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
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
    if (result == NULL || mcp_json_object_set(ctx, result, "contents", contents) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, contents);
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
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    for (size_t i = 0; i < srv->n_prompts; i++) {
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
    if (mcp_json_object_set(ctx, result, "prompts", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
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
    if (result == NULL || mcp_json_object_set(ctx, result, "messages", messages) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, messages);
        return NULL;
    }
    if (prompt->description != NULL && set_string(ctx, result, "description", prompt->description) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_request(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                    const mcp_message_t *req, const char *method) {
    if (strcmp(method, "initialize") == 0) {
        return route_initialize(ctx, srv, s, req);
    }
    if (strcmp(method, "ping") == 0) {
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        if (result == NULL) {
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    if (strcmp(method, "tools/list") == 0) {
        return route_tools_list(ctx, srv, req);
    }
    if (strcmp(method, "tools/call") == 0) {
        return route_tools_call(ctx, srv, s, req);
    }
    if (strcmp(method, "resources/list") == 0) {
        return route_resources_list(ctx, srv, req);
    }
    if (strcmp(method, "resources/read") == 0) {
        return route_resources_read(ctx, srv, s, req);
    }
    if (strcmp(method, "prompts/list") == 0) {
        return route_prompts_list(ctx, srv, req);
    }
    if (strcmp(method, "prompts/get") == 0) {
        return route_prompts_get(ctx, srv, s, req);
    }
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
    int code = 0;
    if (mcp_message_validate(ctx, req, &code) != MCP_OK) {
        *resp_out = err_resp(ctx, req, code, "invalid request");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    const char *method = mcp_message_method(ctx, req);
    if (method == NULL) {
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "missing method");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    if (!session->initialized && strcmp(method, "initialize") != 0) {
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "session not initialized");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    mcp_id_type_t id_type = mcp_message_id_type(ctx, req);
    if (id_type != MCP_ID_NONE) {
        const char *id_str = mcp_message_id_string(ctx, req);
        double id_num = 0;
        mcp_message_id_number(ctx, req, &id_num);
        if (mcp_idset_contains(ctx, session->ids, id_type, id_str, id_num)) {
            *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "duplicate request id");
            return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
        }
        if (mcp_idset_add(ctx, session->ids, id_type, id_str, id_num) != MCP_OK) {
            *resp_out = err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "id tracking failed");
            return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
        }
    }
    mcp_message_t *resp = route_request(ctx, srv, session, req, method);
    if (resp == NULL) {
        *resp_out = err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "internal error");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
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
    if (method != NULL && strcmp(method, "notifications/initialized") == 0) {
        session->initialized = true;
    }
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
    // Allocate the message slots first: sessions realloc must not run
    // before we know both allocations succeed, or q->sessions dangles.
    mcp_message_t **nm = srv_realloc(ctx, q->msgs, ncap * sizeof(*nm));
    if (nm == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_session_t **ns = srv_realloc(ctx, q->sessions, ncap * sizeof(*ns));
    if (ns == NULL) {
        return MCP_ERR_NOMEM;
    }
    q->sessions = ns;
    q->msgs = nm;
    q->cap = ncap;
    return MCP_OK;
}

mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session,
                            mcp_message_t *msg) {
    if (q == NULL || session == NULL || msg == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
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
