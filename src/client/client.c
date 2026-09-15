#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
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
