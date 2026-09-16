#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/transport/transport.h"

#include "internals.h"

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

mcp_client_t *mcp_client_create(mcp_context_t *ctx, mcp_transport_t *transport) {
    if (transport == NULL) {
        return NULL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    mcp_client_t *c = a->malloc_fn(sizeof(*c), a->userdata);
    if (c == NULL) {
        return NULL;
    }
    c->ctx = ctx;
    c->t = transport;
    c->next_id = 1.0;
    c->version = NULL;
    return c;
}

void mcp_client_destroy(mcp_context_t *ctx, mcp_client_t *client) {
    if (client == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    if (client->version != NULL) {
        a->free_fn(client->version, a->userdata);
    }
    a->free_fn(client, a->userdata);
}

mcp_status_t mcp_client_connect(mcp_context_t *ctx, mcp_client_t *client) {
    if (client == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return mcp_transport_start(ctx, client->t);
}

mcp_status_t mcp_client_disconnect(mcp_context_t *ctx, mcp_client_t *client) {
    if (client == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return mcp_transport_stop(ctx, client->t);
}

const char *mcp_client_protocol_version(mcp_context_t *ctx, const mcp_client_t *client) {
    (void)ctx;
    return client != NULL ? client->version : NULL;
}

// One request/response exchange. Returns the parsed response (owned, caller
// destroys) or NULL with a protocol status on the way out.
static mcp_status_t roundtrip(mcp_context_t *ctx, mcp_client_t *c, const char *method,
                              mcp_json_value_t *params, mcp_message_t **resp_out) {
    double id = c->next_id;
    c->next_id += 1.0;
    // Client takes params even on failure: no leak on any path below.
    mcp_message_t *req = mcp_request_new_number_id(ctx, id, method, params);
    if (req == NULL) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    char *out = mcp_message_serialize(ctx, req);
    mcp_message_destroy(ctx, req);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_transport_send(ctx, c->t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    if (st != MCP_OK) {
        return st;
    }
    char *line = NULL;
    st = mcp_transport_recv(ctx, c->t, &line);
    if (st != MCP_OK) {
        return st;
    }
    mcp_message_t *resp = mcp_message_parse(ctx, line, strlen(line));
    mcp_json_free_string(ctx, line);
    if (resp == NULL) {
        return MCP_ERR_PROTOCOL;
    }
    if (mcp_message_kind(ctx, resp) != MCP_MSG_RESPONSE) {
        mcp_message_destroy(ctx, resp);
        return MCP_ERR_PROTOCOL;
    }
    double got = 0.0;
    if (mcp_message_id_number(ctx, resp, &got) != MCP_OK || got != id) {
        mcp_message_destroy(ctx, resp);
        return MCP_ERR_PROTOCOL;
    }
    *resp_out = resp;
    return MCP_OK;
}

mcp_status_t mcp_client_request(mcp_context_t *ctx, mcp_client_t *client,
                                const char *method, mcp_json_value_t *params,
                                mcp_json_value_t **result_out) {
    if (client == NULL || method == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (result_out != NULL) {
        *result_out = NULL;
    }
    mcp_message_t *resp = NULL;
    mcp_status_t st = roundtrip(ctx, client, method, params, &resp);
    if (st != MCP_OK) {
        return st;
    }
    int code = 0;
    if (mcp_message_error_code(ctx, resp, &code) == MCP_OK) {
        mcp_message_destroy(ctx, resp);
        return mcp_rpc_code_to_status(code);
    }
    if (result_out != NULL) {
        const mcp_json_value_t *result = mcp_message_result(ctx, resp);
        mcp_json_value_t *clone = mcp_json_clone(ctx, result);
        mcp_message_destroy(ctx, resp);
        if (clone == NULL) {
            return MCP_ERR_NOMEM;
        }
        *result_out = clone;
        return MCP_OK;
    }
    mcp_message_destroy(ctx, resp);
    return MCP_OK;
}

mcp_status_t mcp_client_initialize(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *client_name, const char *client_version,
                                   mcp_json_value_t **server_info_out) {
    if (client == NULL || client_name == NULL || client_version == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (server_info_out != NULL) {
        *server_info_out = NULL;
    }
    mcp_json_value_t *params =
        mcp_initialize_params_new_v(ctx, MCP_PROTOCOL_VERSION_LATEST, client_name, client_version);
    if (params == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_message_t *resp = NULL;
    mcp_status_t st = roundtrip(ctx, client, "initialize", params, &resp);
    if (st != MCP_OK) {
        return st;
    }
    int code = 0;
    if (mcp_message_error_code(ctx, resp, &code) == MCP_OK) {
        mcp_message_destroy(ctx, resp);
        return mcp_rpc_code_to_status(code);
    }
    const mcp_json_value_t *result = mcp_message_result(ctx, resp);
    const mcp_json_value_t *pv =
        result != NULL ? mcp_json_object_get(ctx, result, "protocolVersion") : NULL;
    const char *version = NULL;
    if (pv == NULL || mcp_json_string_value(ctx, pv, &version) != MCP_OK || version == NULL) {
        mcp_message_destroy(ctx, resp);
        return MCP_ERR_PROTOCOL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    size_t n = strlen(version) + 1;
    char *copy = a->malloc_fn(n, a->userdata);
    if (copy == NULL) {
        mcp_message_destroy(ctx, resp);
        return MCP_ERR_NOMEM;
    }
    memcpy(copy, version, n);
    if (server_info_out != NULL) {
        mcp_json_value_t *info = mcp_json_clone(ctx, mcp_json_object_get(ctx, result, "serverInfo"));
        if (info == NULL) {
            a->free_fn(copy, a->userdata);
            mcp_message_destroy(ctx, resp);
            return MCP_ERR_NOMEM;
        }
        *server_info_out = info;
    }
    if (client->version != NULL) {
        a->free_fn(client->version, a->userdata);
    }
    client->version = copy;
    mcp_message_destroy(ctx, resp);
    mcp_message_t *ntf = mcp_initialized_notification_new(ctx);
    if (ntf == NULL) {
        return MCP_ERR_NOMEM;
    }
    char *out = mcp_message_serialize(ctx, ntf);
    mcp_message_destroy(ctx, ntf);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    st = mcp_transport_send(ctx, client->t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

mcp_status_t mcp_client_ping(mcp_context_t *ctx, mcp_client_t *client) {
    if (client == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return mcp_client_request(ctx, client, "ping", NULL, NULL);
}

mcp_status_t mcp_client_list_tools(mcp_context_t *ctx, mcp_client_t *client,
                                   mcp_json_value_t **tools_out) {
    if (client == NULL || tools_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return mcp_client_request(ctx, client, "tools/list", NULL, tools_out);
}

mcp_status_t mcp_client_call_tool(mcp_context_t *ctx, mcp_client_t *client,
                                  const char *name, mcp_json_value_t *args,
                                  mcp_json_value_t **result_out) {
    if (client == NULL || name == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *nv = params != NULL ? mcp_json_string_new(ctx, name) : NULL;
    if (params == NULL || nv == NULL) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set(ctx, params, "name", nv) != MCP_OK) {
        mcp_json_destroy(ctx, nv);
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    if (args != NULL && mcp_json_object_set(ctx, params, "arguments", args) != MCP_OK) {
        mcp_json_destroy(ctx, args);
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "tools/call", params, result_out);
}

mcp_status_t mcp_client_read_resource(mcp_context_t *ctx, mcp_client_t *client,
                                      const char *uri, mcp_json_value_t **result_out) {
    if (client == NULL || uri == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *uv = params != NULL ? mcp_json_string_new(ctx, uri) : NULL;
    if (params == NULL || uv == NULL) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set(ctx, params, "uri", uv) != MCP_OK) {
        mcp_json_destroy(ctx, uv);
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "resources/read", params, result_out);
}

mcp_status_t mcp_client_get_prompt(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *name, mcp_json_value_t *args,
                                   mcp_json_value_t **result_out) {
    if (client == NULL || name == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *nv = params != NULL ? mcp_json_string_new(ctx, name) : NULL;
    if (params == NULL || nv == NULL) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set(ctx, params, "name", nv) != MCP_OK) {
        mcp_json_destroy(ctx, nv);
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    if (args != NULL && mcp_json_object_set(ctx, params, "arguments", args) != MCP_OK) {
        mcp_json_destroy(ctx, args);
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "prompts/get", params, result_out);
}

mcp_status_t mcp_client_complete(mcp_context_t *ctx, mcp_client_t *client,
                                 const char *ref, mcp_json_value_t *args,
                                 mcp_json_value_t **result_out) {
    if (client == NULL || ref == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    // ref is a JSON object {"type":"ref","value":ref}; args is a JSON object.
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *ref_obj = params != NULL ? mcp_json_object_new(ctx) : NULL;
    if (params == NULL || ref_obj == NULL) {
        mcp_json_destroy(ctx, params);
        mcp_json_destroy(ctx, ref_obj);
        return MCP_ERR_NOMEM;
    }
    mcp_json_value_t *type_v = mcp_json_string_new(ctx, "ref");
    mcp_json_value_t *val_v = mcp_json_string_new(ctx, ref);
    if (type_v == NULL || val_v == NULL) {
        mcp_json_destroy(ctx, type_v);
        mcp_json_destroy(ctx, val_v);
        mcp_json_destroy(ctx, params);
        mcp_json_destroy(ctx, ref_obj);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set(ctx, ref_obj, "type", type_v) != MCP_OK ||
        mcp_json_object_set(ctx, ref_obj, "value", val_v) != MCP_OK) {
        mcp_json_destroy(ctx, type_v);
        mcp_json_destroy(ctx, val_v);
        mcp_json_destroy(ctx, params);
        mcp_json_destroy(ctx, ref_obj);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set(ctx, params, "ref", ref_obj) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        mcp_json_destroy(ctx, ref_obj);
        return MCP_ERR_NOMEM;
    }
    if (args != NULL && mcp_json_object_set(ctx, params, "argument", args) != MCP_OK) {
        mcp_json_destroy(ctx, args);
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "completion/complete", params, result_out);
}
