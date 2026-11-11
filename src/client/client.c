/**
 * @file client.c
 *
 * High-level MCP client. All roundtrip results are deep-cloned
 * (mcp_json_clone) so the caller owns the result independently of
 * the transport buffer. The request id counter starts at 1.0.
 *
 * On both success and failure the client TAKES ownership of `args`;
 * the caller must not free `args` after calling.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/core/auth.h"
#include "mcpkit/core/context.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/mrtr.h"
#include "mcpkit/protocol/version.h"
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
    c->roots_fn = NULL;
    c->roots_ud = NULL;
    c->sample_fn = NULL;
    c->sample_ud = NULL;
    c->elicitation_fn = NULL;
    c->elicitation_ud = NULL;
    c->mrtr_elicit_fn = NULL;
    c->mrtr_elicit_ud = NULL;
    c->bearer_token = NULL;
    c->schema_cache.tool_names = NULL;
    c->schema_cache.schemas = NULL;
    c->schema_cache.n = 0;
    c->schema_cache.cap = 0;
    c->xmcp_params = NULL;
    c->n_xmcp_params = 0;
    c->cap_xmcp_params = 0;
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
    if (client->bearer_token != NULL) {
        a->free_fn(client->bearer_token, a->userdata);
    }
    /* P4-a T4: free schema cache */
    if (client->schema_cache.tool_names != NULL) {
        for (size_t i = 0; i < client->schema_cache.n; i++) {
            if (client->schema_cache.tool_names[i] != NULL) {
                a->free_fn(client->schema_cache.tool_names[i], a->userdata);
            }
            if (client->schema_cache.schemas[i] != NULL) {
                mcp_json_destroy(ctx, client->schema_cache.schemas[i]);
            }
        }
        a->free_fn(client->schema_cache.tool_names, a->userdata);
        a->free_fn(client->schema_cache.schemas, a->userdata);
    }
    /* P4-a T4: free pending Mcp-Param-* headers */
    if (client->xmcp_params != NULL) {
        for (size_t i = 0; i < client->n_xmcp_params; i++) {
            if (client->xmcp_params[i].name != NULL) {
                a->free_fn(client->xmcp_params[i].name, a->userdata);
            }
            if (client->xmcp_params[i].value != NULL) {
                a->free_fn(client->xmcp_params[i].value, a->userdata);
            }
        }
        a->free_fn(client->xmcp_params, a->userdata);
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

mcp_status_t mcp_client_discover(mcp_context_t *ctx, mcp_client_t *client,
                                 mcp_json_value_t **result_out) {
    if (client == NULL || result_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return mcp_client_request(ctx, client, "server/discover", NULL, result_out);
}

mcp_status_t mcp_client_list_tools(mcp_context_t *ctx, mcp_client_t *client,
                                   mcp_json_value_t **tools_out) {
    if (client == NULL || tools_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *tools_out = NULL;
    mcp_json_value_t *acc = mcp_json_array_new(ctx);
    if (acc == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_json_value_t *params = NULL;
    for (;;) {
        mcp_json_value_t *page = NULL;
        mcp_status_t st = mcp_client_request(ctx, client, "tools/list", params, &page);
        params = NULL;
        if (st != MCP_OK) {
            mcp_json_destroy(ctx, acc);
            return st;
        }
        const mcp_json_value_t *arr = mcp_json_object_get(ctx, page, "tools");
        if (arr != NULL && mcp_json_type(ctx, arr) == MCP_JSON_ARRAY) {
            size_t n = mcp_json_array_size(ctx, arr);
            for (size_t i = 0; i < n; i++) {
                mcp_json_value_t *copy = mcp_json_clone(ctx, mcp_json_array_get(ctx, arr, i));
                if (copy == NULL || mcp_json_array_append(ctx, acc, copy) != MCP_OK) {
                    mcp_json_destroy(ctx, copy);
                    mcp_json_destroy(ctx, page);
                    mcp_json_destroy(ctx, acc);
                    return MCP_ERR_NOMEM;
                }
            }
        }
        const mcp_json_value_t *nc = mcp_json_object_get(ctx, page, "nextCursor");
        const char *cursor = NULL;
        if (nc == NULL || mcp_json_type(ctx, nc) != MCP_JSON_STRING ||
            mcp_json_string_value(ctx, nc, &cursor) != MCP_OK || cursor == NULL) {
            mcp_json_destroy(ctx, page);
            break;
        }
        mcp_json_value_t *next = mcp_json_object_new(ctx);
        mcp_json_value_t *cv = next != NULL ? mcp_json_string_new(ctx, cursor) : NULL;
        if (next == NULL || cv == NULL ||
            mcp_json_object_set_take(ctx, next, "cursor", cv) != MCP_OK) {
            mcp_json_destroy(ctx, next);
            mcp_json_destroy(ctx, page);
            mcp_json_destroy(ctx, acc);
            return MCP_ERR_NOMEM;
        }
        mcp_json_destroy(ctx, page);
        params = next;
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL || mcp_json_object_set_take(ctx, result, "tools", acc) != MCP_OK) {
        /* set_take destroys acc on failure; only result may need freeing here. */
        if (result != NULL) {
            mcp_json_destroy(ctx, result);
        }
        return MCP_ERR_NOMEM;
    }
    *tools_out = result;
    return MCP_OK;
}

/* P4-a T4: forward declaration of the mirroring helper, defined at the
 * end of this file.  Called from mcp_client_call_tool before the request
 * is sent, so the transport can pick up the computed Mcp-Param-* headers. */
static mcp_status_t xmcp_mirror_headers(mcp_context_t *ctx, mcp_client_t *client,
                                        const char *tool_name,
                                        const mcp_json_value_t *args);

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
    if (mcp_json_object_set_take(ctx, params, "name", nv) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    if (args != NULL && mcp_json_object_set_take(ctx, params, "arguments", args) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    /* P4-a T4: compute Mcp-Param-* headers from cached schema before sending */
    mcp_status_t mirror_st = xmcp_mirror_headers(ctx, client, name, args);
    if (mirror_st != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return mirror_st;
    }
    return mcp_client_request(ctx, client, "tools/call", params, result_out);
}

void mcp_client_set_mrtr_elicit_handler(mcp_context_t *ctx, mcp_client_t *client,
                                        mcp_client_mrtr_elicit_fn fn, void *user_data) {
    (void)ctx;
    if (client == NULL) return;
    client->mrtr_elicit_fn = fn;
    client->mrtr_elicit_ud = user_data;
}

mcp_status_t mcp_client_call_tool_mrtr(mcp_context_t *ctx, mcp_client_t *client,
                                        const char *name, mcp_json_value_t *args,
                                        mcp_json_value_t **result_out) {
    if (client == NULL || name == NULL) {
        mcp_json_destroy(ctx, args);
        return MCP_ERR_INVALID_ARGUMENT;
    }

    /* Pending inputResponses and requestState for the next hop (owned). */
    mcp_json_value_t *pending_responses = NULL; /* owned JSON array  */
    char *pending_state = NULL;                 /* owned string copy */

    mcp_json_value_t *result = NULL;
    mcp_status_t st = MCP_OK;

    for (int hop = 0; hop <= MCP_MRTR_MAX_HOPS; hop++) {
        /* Build params for this hop */
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        mcp_json_value_t *nv = params != NULL ? mcp_json_string_new(ctx, name) : NULL;
        if (params == NULL || nv == NULL) {
            mcp_json_destroy(ctx, params);
            mcp_json_destroy(ctx, args);
            mcp_json_destroy(ctx, pending_responses);
            if (pending_state != NULL) {
                const mcp_allocator_t *a = ctx != NULL
                    ? mcp_context_allocator(ctx) : mcp_default_allocator();
                a->free_fn(pending_state, a->userdata);
            }
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, params, "name", nv) != MCP_OK) {
            mcp_json_destroy(ctx, params);
            mcp_json_destroy(ctx, args);
            mcp_json_destroy(ctx, pending_responses);
            if (pending_state != NULL) {
                const mcp_allocator_t *a = ctx != NULL
                    ? mcp_context_allocator(ctx) : mcp_default_allocator();
                a->free_fn(pending_state, a->userdata);
            }
            return MCP_ERR_NOMEM;
        }
        /* On first hop, attach the original args (caller-owned, consumed). */
        if (hop == 0 && args != NULL) {
            if (mcp_json_object_set_take(ctx, params, "arguments", args) != MCP_OK) {
                mcp_json_destroy(ctx, params);
                mcp_json_destroy(ctx, args);
                mcp_json_destroy(ctx, pending_responses);
                if (pending_state != NULL) {
                    const mcp_allocator_t *a = ctx != NULL
                        ? mcp_context_allocator(ctx) : mcp_default_allocator();
                    a->free_fn(pending_state, a->userdata);
                }
                return MCP_ERR_NOMEM;
            }
            args = NULL; /* consumed */
        }
        /* On retry hops, attach inputResponses (owned by us, consumed). */
        if (pending_responses != NULL) {
            if (mcp_json_object_set_take(ctx, params, "inputResponses",
                                         pending_responses) != MCP_OK) {
                mcp_json_destroy(ctx, params);
                mcp_json_destroy(ctx, pending_responses);
                if (pending_state != NULL) {
                    const mcp_allocator_t *a = ctx != NULL
                        ? mcp_context_allocator(ctx) : mcp_default_allocator();
                    a->free_fn(pending_state, a->userdata);
                }
                return MCP_ERR_NOMEM;
            }
            pending_responses = NULL; /* consumed */
        }
        /* On retry hops, attach requestState string. */
        if (pending_state != NULL) {
            mcp_json_value_t *sv = mcp_json_string_new(ctx, pending_state);
            const mcp_allocator_t *a = ctx != NULL
                ? mcp_context_allocator(ctx) : mcp_default_allocator();
            a->free_fn(pending_state, a->userdata);
            pending_state = NULL;
            if (sv == NULL) {
                mcp_json_destroy(ctx, params);
                return MCP_ERR_NOMEM;
            }
            if (mcp_json_object_set_take(ctx, params, "requestState", sv) != MCP_OK) {
                mcp_json_destroy(ctx, params);
                return MCP_ERR_NOMEM;
            }
        }

        result = NULL;
        st = mcp_client_request(ctx, client, "tools/call", params, &result);
        if (st != MCP_OK) {
            /* params consumed by mcp_client_request */
            break;
        }

        /* Check for InputRequiredResult */
        if (!mcp_mrtr_is_input_required(ctx, result)) {
            /* Done — complete result */
            break;
        }

        /* No elicitation handler → return the InputRequiredResult as-is */
        if (client->mrtr_elicit_fn == NULL) {
            break;
        }

        if (hop == MCP_MRTR_MAX_HOPS) {
            /* Too many hops */
            mcp_json_destroy(ctx, result);
            result = NULL;
            st = MCP_ERR_PROTOCOL;
            break;
        }

        /* Extract state for next hop */
        const char *rs = mcp_mrtr_get_request_state(ctx, result);
        if (rs != NULL) {
            const mcp_allocator_t *a = ctx != NULL
                ? mcp_context_allocator(ctx) : mcp_default_allocator();
            pending_state = (char *)a->malloc_fn(strlen(rs) + 1, a->userdata);
            if (pending_state == NULL) {
                mcp_json_destroy(ctx, result);
                result = NULL;
                st = MCP_ERR_NOMEM;
                break;
            }
            memcpy(pending_state, rs, strlen(rs) + 1);
        }

        const mcp_json_value_t *reqs = mcp_mrtr_get_input_requests(ctx, result);
        pending_responses = client->mrtr_elicit_fn(ctx, reqs, pending_state,
                                                   client->mrtr_elicit_ud);
        mcp_json_destroy(ctx, result);
        result = NULL;

        if (pending_responses == NULL) {
            /* User cancelled */
            if (pending_state != NULL) {
                const mcp_allocator_t *a = ctx != NULL
                    ? mcp_context_allocator(ctx) : mcp_default_allocator();
                a->free_fn(pending_state, a->userdata);
                pending_state = NULL;
            }
            st = MCP_ERR_CANCELLED;
            break;
        }
    }

    if (result_out != NULL) {
        *result_out = result;
    } else {
        mcp_json_destroy(ctx, result);
    }
    return st;
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
    if (mcp_json_object_set_take(ctx, params, "uri", uv) != MCP_OK) {
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
    if (mcp_json_object_set_take(ctx, params, "name", nv) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    if (args != NULL && mcp_json_object_set_take(ctx, params, "arguments", args) != MCP_OK) {
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
    if (mcp_json_object_set_take(ctx, ref_obj, "type", type_v) != MCP_OK) {
        mcp_json_destroy(ctx, val_v); /* unattached; type_v consumed */
        mcp_json_destroy(ctx, ref_obj);
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, ref_obj, "value", val_v) != MCP_OK) {
        mcp_json_destroy(ctx, ref_obj); /* frees attached type_v */
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, params, "ref", ref_obj) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    if (args != NULL && mcp_json_object_set_take(ctx, params, "argument", args) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "completion/complete", params, result_out);
}

mcp_status_t mcp_client_tasks_get(mcp_context_t *ctx, mcp_client_t *client,
                                  const char *task_id, mcp_json_value_t **result_out) {
    if (client == NULL || task_id == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *tv = params != NULL ? mcp_json_string_new(ctx, task_id) : NULL;
    if (params == NULL || tv == NULL) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, params, "taskId", tv) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "tasks/get", params, result_out);
}

mcp_status_t mcp_client_tasks_update(mcp_context_t *ctx, mcp_client_t *client,
                                     const char *task_id, mcp_json_value_t *input_responses,
                                     mcp_json_value_t **result_out) {
    if (client == NULL || task_id == NULL) {
        if (input_responses != NULL) {
            mcp_json_destroy(ctx, input_responses);
        }
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *tv = params != NULL ? mcp_json_string_new(ctx, task_id) : NULL;
    if (params == NULL || tv == NULL) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        if (input_responses != NULL) {
            mcp_json_destroy(ctx, input_responses);
        }
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, params, "taskId", tv) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        if (input_responses != NULL) {
            mcp_json_destroy(ctx, input_responses);
        }
        return MCP_ERR_NOMEM;
    }
    if (input_responses != NULL &&
        mcp_json_object_set_take(ctx, params, "inputResponses", input_responses) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "tasks/update", params, result_out);
}

mcp_status_t mcp_client_tasks_cancel(mcp_context_t *ctx, mcp_client_t *client,
                                     const char *task_id, mcp_json_value_t **result_out) {
    if (client == NULL || task_id == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *tv = params != NULL ? mcp_json_string_new(ctx, task_id) : NULL;
    if (params == NULL || tv == NULL) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, params, "taskId", tv) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "tasks/cancel", params, result_out);
}

mcp_status_t mcp_client_skills_list(mcp_context_t *ctx, mcp_client_t *client,
                                    const char *cursor, mcp_json_value_t **result_out) {
    if (client == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = NULL;
    if (cursor != NULL) {
        params = mcp_json_object_new(ctx);
        mcp_json_value_t *cv = params != NULL ? mcp_json_string_new(ctx, cursor) : NULL;
        if (params == NULL || cv == NULL) {
            if (params != NULL) {
                mcp_json_destroy(ctx, params);
            }
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, params, "cursor", cv) != MCP_OK) {
            mcp_json_destroy(ctx, params);
            return MCP_ERR_NOMEM;
        }
    }
    return mcp_client_request(ctx, client, "skills/list", params, result_out);
}

mcp_status_t mcp_client_skills_get(mcp_context_t *ctx, mcp_client_t *client,
                                   const char *name_or_uri, mcp_json_value_t **result_out) {
    if (client == NULL || name_or_uri == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *val = params != NULL ? mcp_json_string_new(ctx, name_or_uri) : NULL;
    if (params == NULL || val == NULL) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        return MCP_ERR_NOMEM;
    }
    const char *key = (strstr(name_or_uri, "://") != NULL) ? "uri" : "name";
    if (mcp_json_object_set_take(ctx, params, key, val) != MCP_OK) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    return mcp_client_request(ctx, client, "skills/get", params, result_out);
}

mcp_status_t mcp_client_subscriptions_listen(mcp_context_t *ctx, mcp_client_t *client,
                                             mcp_json_value_t *notifications_filter,
                                             mcp_message_t **ack_out) {
    if (client == NULL) {
        if (notifications_filter != NULL) {
            mcp_json_destroy(ctx, notifications_filter);
        }
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (ack_out != NULL) {
        *ack_out = NULL;
    }

    mcp_json_value_t *params = mcp_json_object_new(ctx);
    if (params == NULL) {
        if (notifications_filter != NULL) {
            mcp_json_destroy(ctx, notifications_filter);
        }
        return MCP_ERR_NOMEM;
    }
    if (notifications_filter != NULL) {
        if (mcp_json_object_set_take(ctx, params, "notifications", notifications_filter) != MCP_OK) {
            mcp_json_destroy(ctx, notifications_filter);
            mcp_json_destroy(ctx, params);
            return MCP_ERR_NOMEM;
        }
    }

    double id = client->next_id;
    client->next_id += 1.0;

    mcp_message_t *req = mcp_request_new_number_id(ctx, id, "subscriptions/listen", params);
    if (req == NULL) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    char *out = mcp_message_serialize(ctx, req);
    mcp_message_destroy(ctx, req);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_transport_send(ctx, client->t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    if (st != MCP_OK) {
        return st;
    }

    char *line = NULL;
    st = mcp_transport_recv(ctx, client->t, &line);
    if (st != MCP_OK) {
        return st;
    }
    mcp_message_t *msg = mcp_message_parse(ctx, line, strlen(line));
    mcp_json_free_string(ctx, line);
    if (msg == NULL) {
        return MCP_ERR_PROTOCOL;
    }

    if (mcp_message_kind(ctx, msg) == MCP_MSG_RESPONSE) {
        int code = 0;
        if (mcp_message_error_code(ctx, msg, &code) == MCP_OK) {
            mcp_message_destroy(ctx, msg);
            return mcp_rpc_code_to_status(code);
        }
        mcp_message_destroy(ctx, msg);
        return MCP_ERR_PROTOCOL;
    }

    if (mcp_message_kind(ctx, msg) == MCP_MSG_NOTIFICATION) {
        const char *method = mcp_message_method(ctx, msg);
        if (method == NULL || strcmp(method, "notifications/subscriptions/acknowledged") != 0) {
            mcp_message_destroy(ctx, msg);
            return MCP_ERR_PROTOCOL;
        }
        const mcp_json_value_t *ack_p = mcp_message_params(ctx, msg);
        if (ack_p == NULL) {
            mcp_message_destroy(ctx, msg);
            return MCP_ERR_PROTOCOL;
        }
        const mcp_json_value_t *meta = mcp_json_object_get(ctx, ack_p, "_meta");
        if (meta == NULL || mcp_json_object_get(ctx, meta, "io.modelcontextprotocol/subscriptionId") == NULL) {
            mcp_message_destroy(ctx, msg);
            return MCP_ERR_PROTOCOL;
        }
        if (ack_out != NULL) {
            *ack_out = msg;
        } else {
            mcp_message_destroy(ctx, msg);
        }
        return MCP_OK;
    }

    mcp_message_destroy(ctx, msg);
    return MCP_ERR_PROTOCOL;
}

mcp_status_t mcp_client_cancel_subscription(mcp_context_t *ctx, mcp_client_t *client,
                                            const char *subscription_id) {
    if (client == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *params = NULL;
    if (subscription_id != NULL) {
        params = mcp_json_object_new(ctx);
        mcp_json_value_t *meta = mcp_json_object_new(ctx);
        mcp_json_value_t *sub_v = mcp_json_string_new(ctx, subscription_id);
        if (params == NULL || meta == NULL || sub_v == NULL) {
            mcp_json_destroy(ctx, params);
            mcp_json_destroy(ctx, meta);
            mcp_json_destroy(ctx, sub_v);
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, meta, "io.modelcontextprotocol/subscriptionId", sub_v) != MCP_OK ||
            mcp_json_object_set_take(ctx, params, "_meta", meta) != MCP_OK) {
            mcp_json_destroy(ctx, sub_v);
            mcp_json_destroy(ctx, meta);
            mcp_json_destroy(ctx, params);
            return MCP_ERR_NOMEM;
        }
    }
    mcp_message_t *ntf = mcp_notification_new(ctx, "notifications/cancelled", params);
    if (ntf == NULL) {
        mcp_json_destroy(ctx, params);
        return MCP_ERR_NOMEM;
    }
    char *out = mcp_message_serialize(ctx, ntf);
    mcp_message_destroy(ctx, ntf);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_transport_send(ctx, client->t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

mcp_status_t mcp_client_recv_message(mcp_context_t *ctx, mcp_client_t *client,
                                     mcp_message_t **msg_out) {
    if (client == NULL || msg_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *msg_out = NULL;
    char *line = NULL;
    mcp_status_t st = mcp_transport_recv(ctx, client->t, &line);
    if (st != MCP_OK) {
        return st;
    }
    mcp_message_t *msg = mcp_message_parse(ctx, line, strlen(line));
    mcp_json_free_string(ctx, line);
    if (msg == NULL) {
        return MCP_ERR_PROTOCOL;
    }
    *msg_out = msg;
    return MCP_OK;
}

void mcp_client_set_roots_provider(mcp_context_t *ctx, mcp_client_t *c,
                                   mcp_client_roots_fn fn, void *user_data) {
    (void)ctx;
    if (c == NULL) {
        return;
    }
    c->roots_fn = fn;
    c->roots_ud = user_data;
}

mcp_status_t mcp_client_notify_roots_list_changed(mcp_context_t *ctx, mcp_client_t *client) {
    if (client == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_message_t *ntf = mcp_notification_new(ctx, "notifications/roots/list_changed", NULL);
    if (ntf == NULL) {
        return MCP_ERR_NOMEM;
    }
    char *out = mcp_message_serialize(ctx, ntf);
    mcp_message_destroy(ctx, ntf);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_transport_send(ctx, client->t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

void mcp_client_set_sample_provider(mcp_context_t *ctx, mcp_client_t *c,
                                    mcp_client_sample_fn fn, void *user_data) {
    (void)ctx;
    if (c == NULL) {
        return;
    }
    c->sample_fn = fn;
    c->sample_ud = user_data;
}

void mcp_client_set_elicitation_provider(mcp_context_t *ctx, mcp_client_t *c,
                                        mcp_client_elicitation_fn fn, void *user_data) {
    (void)ctx;
    if (c == NULL) {
        return;
    }
    c->elicitation_fn = fn;
    c->elicitation_ud = user_data;
}

mcp_status_t mcp_client_handle_server_request(mcp_context_t *ctx, mcp_client_t *c,
                                              const mcp_message_t *req,
                                              mcp_message_t **resp_out) {
    if (c == NULL || req == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *resp_out = NULL;

    const char *method = mcp_message_method(ctx, req);
    if (method == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    const mcp_json_value_t *params = mcp_message_params(ctx, req);

    if (strcmp(method, "roots/list") == 0) {
        if (c->roots_fn == NULL) {
            *resp_out = mcp_response_err_new(ctx, req, MCP_RPC_METHOD_NOT_FOUND,
                                             "roots not supported", NULL);
            return *resp_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
        }
        mcp_json_value_t *result = c->roots_fn(ctx, c->roots_ud);
        if (result == NULL) {
            *resp_out = mcp_response_err_new(ctx, req, MCP_RPC_METHOD_NOT_FOUND,
                                             "roots not supported", NULL);
            return *resp_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
        }
        *resp_out = mcp_response_ok_new(ctx, req, result);
        if (*resp_out == NULL) {
            mcp_json_destroy(ctx, result);
            return MCP_ERR_NOMEM;
        }
        return MCP_OK;
    }

    if (strcmp(method, "sampling/createMessage") == 0) {
        if (c->sample_fn == NULL) {
            *resp_out = mcp_response_err_new(ctx, req, MCP_RPC_METHOD_NOT_FOUND,
                                             "sampling not supported", NULL);
            return *resp_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
        }
        mcp_json_value_t *result = c->sample_fn(ctx, params, c->sample_ud);
        if (result == NULL) {
            *resp_out = mcp_response_err_new(ctx, req, MCP_RPC_METHOD_NOT_FOUND,
                                             "sampling not supported", NULL);
            return *resp_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
        }
        *resp_out = mcp_response_ok_new(ctx, req, result);
        if (*resp_out == NULL) {
            mcp_json_destroy(ctx, result);
            return MCP_ERR_NOMEM;
        }
        return MCP_OK;
    }

    if (strcmp(method, "elicitation/create") == 0) {
        if (c->elicitation_fn == NULL) {
            *resp_out = mcp_response_err_new(ctx, req, MCP_RPC_METHOD_NOT_FOUND,
                                             "elicitation not supported", NULL);
            return *resp_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
        }
        mcp_json_value_t *result = c->elicitation_fn(ctx, params, c->elicitation_ud);
        if (result == NULL) {
            *resp_out = mcp_response_err_new(ctx, req, MCP_RPC_METHOD_NOT_FOUND,
                                             "elicitation not supported", NULL);
            return *resp_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
        }
        *resp_out = mcp_response_ok_new(ctx, req, result);
        if (*resp_out == NULL) {
            mcp_json_destroy(ctx, result);
            return MCP_ERR_NOMEM;
        }
        return MCP_OK;
    }

    *resp_out = mcp_response_err_new(ctx, req, MCP_RPC_METHOD_NOT_FOUND,
                                     "unknown method", NULL);
    return *resp_out != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

mcp_status_t mcp_client_set_bearer_token(mcp_context_t *ctx, mcp_client_t *client,
                                         const char *bearer_token) {
    if (client == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_allocator_t *a = alloc_of(ctx != NULL ? ctx : client->ctx);
    if (client->bearer_token != NULL) {
        a->free_fn(client->bearer_token, a->userdata);
        client->bearer_token = NULL;
    }
    if (bearer_token != NULL) {
        size_t len = strlen(bearer_token);
        client->bearer_token = a->malloc_fn(len + 1, a->userdata);
        if (client->bearer_token == NULL) {
            return MCP_ERR_NOMEM;
        }
        memcpy(client->bearer_token, bearer_token, len + 1);
    }
    return MCP_OK;
}

const char *mcp_client_get_bearer_token(mcp_context_t *ctx, const mcp_client_t *client) {
    (void)ctx;
    if (client == NULL) {
        return NULL;
    }
    return client->bearer_token;
}

/* ------------------------------------------------------------------------
 * P4-a T4: SEP-2243 client-side Mcp-Param-* mirroring
 *
 * When a 2026-07-28 client has a cached tool schema with x-mcp-header
 * annotations, mcp_client_call_tool walks the schema before sending,
 * extracts the argument values, applies the 5-step encoding, and stores
 * the result on the client handle.  The host HTTP transport reads them
 * via mcp_client_get_xmcp_params() and calls
 * mcp_http_request_set_xmcp_param() per entry.
 *
 * 5-step encoding:
 *   1. Type conversion (string as-is, number via %.17g, bool "true"/"false")
 *   2. Leading/trailing whitespace -> base64 sentinel
 *   3. Non-ASCII byte -> base64 sentinel
 *   4. Control char (< 0x20 or 0x7f) -> base64 sentinel
 *   5. Otherwise: plain header value
 *
 * Gate: client->version must be "2026-07-28".  2025-11-25 and earlier
 * skip mirroring entirely even if a schema is set.
 */

/* Check whether the client negotiated the 2026-07-28 protocol version. */
static bool client_version_is_2026(const mcp_client_t *c) {
    return c->version != NULL &&
           strcmp(c->version, MCP_PROTOCOL_VERSION_2026_07_28) == 0;
}

/* --- schema walk: collect x-mcp-header annotations --------------------- */

typedef struct {
    const char *param_name;  /* property key, e.g. "region" (borrowed) */
    const char *header_name; /* x-mcp-header value, e.g. "Region" (borrowed) */
} xmcp_anno_t;

/* Collect annotations from `properties` in `schema`.
 * Writes up to cap entries to out; returns the count written.
 * Each out entry has param_name and header_name as borrowed pointers. */
static size_t xmcp_collect_anno(mcp_context_t *ctx, const mcp_json_value_t *schema,
                                xmcp_anno_t *out, size_t cap) {
    if (schema == NULL || cap == 0) {
        return 0;
    }
    const mcp_json_value_t *props = mcp_json_object_get(ctx, schema, "properties");
    if (props == NULL || mcp_json_type(ctx, props) != MCP_JSON_OBJECT) {
        return 0;
    }
    size_t n = mcp_json_object_size(ctx, props);
    size_t written = 0;
    for (size_t i = 0; i < n && written < cap; i++) {
        const char *key = mcp_json_object_key_at(ctx, props, i);
        if (key == NULL) {
            continue;
        }
        const mcp_json_value_t *child = mcp_json_object_get(ctx, props, key);
        if (child == NULL || mcp_json_type(ctx, child) != MCP_JSON_OBJECT) {
            continue;
        }
        if (mcp_json_object_has(ctx, child, "x-mcp-header")) {
            const mcp_json_value_t *hdr_v = mcp_json_object_get(ctx, child, "x-mcp-header");
            const char *hdr_str = NULL;
            if (hdr_v != NULL &&
                mcp_json_string_value(ctx, hdr_v, &hdr_str) == MCP_OK &&
                hdr_str != NULL) {
                out[written].param_name  = key;
                out[written].header_name = hdr_str;
                written++;
            }
        }
    }
    return written;
}

/* --- 5-step encoding ---------------------------------------------------- */

/* Encode a JSON argument value into the header value string.
 * Returns a caller-owned string (free with a->free_fn), or NULL if the
 * value is null / missing / non-encodable. */
static char *xmcp_encode_value(mcp_context_t *ctx, const mcp_json_value_t *v) {
    if (v == NULL || mcp_json_type(ctx, v) == MCP_JSON_NULL) {
        return NULL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    char tmp[64];
    const char *plain = NULL;
    size_t plain_len = 0;
    bool need_sentinel = false;

    switch (mcp_json_type(ctx, v)) {
    case MCP_JSON_STRING: {
        const char *s;
        if (mcp_json_string_value(ctx, v, &s) != MCP_OK || s == NULL) {
            return NULL;
        }
        plain = s;
        plain_len = strlen(s);
        /* Step 2: leading/trailing whitespace */
        if (plain_len > 0 &&
            (plain[0] == ' ' || plain[0] == '\t' ||
             plain[plain_len - 1] == ' ' || plain[plain_len - 1] == '\t')) {
            need_sentinel = true;
        }
        /* Step 3: non-ASCII byte */
        if (!need_sentinel) {
            for (size_t i = 0; i < plain_len; i++) {
                if ((unsigned char)plain[i] >= 0x80u) {
                    need_sentinel = true;
                    break;
                }
            }
        }
        /* Step 4: control characters (< 0x20 or 0x7f) */
        if (!need_sentinel) {
            for (size_t i = 0; i < plain_len; i++) {
                unsigned char c = (unsigned char)plain[i];
                if (c < 0x20u || c == 0x7fu) {
                    need_sentinel = true;
                    break;
                }
            }
        }
        break;
    }
    case MCP_JSON_NUMBER: {
        double d;
        if (mcp_json_number_value(ctx, v, &d) != MCP_OK) {
            return NULL;
        }
        snprintf(tmp, sizeof(tmp), "%.17g", d);
        plain = tmp;
        plain_len = strlen(tmp);
        /* Number representations rarely need a sentinel; check anyway. */
        for (size_t i = 0; i < plain_len; i++) {
            unsigned char c = (unsigned char)plain[i];
            if (c < 0x20u || c == 0x7fu) {
                need_sentinel = true;
                break;
            }
        }
        break;
    }
    case MCP_JSON_BOOL: {
        bool b;
        if (mcp_json_bool_value(ctx, v, &b) != MCP_OK) {
            return NULL;
        }
        plain = b ? "true" : "false";
        plain_len = strlen(plain);
        /* "true"/"false" never need a sentinel. */
        break;
    }
    default:
        return NULL; /* arrays / objects: no scalar header value */
    }

    if (need_sentinel) {
        /* Build =?base64?<b64>?= */
        char *b64 = mcp_base64_encode(ctx, (const uint8_t *)plain, plain_len);
        if (b64 == NULL) {
            return NULL;
        }
        size_t b64_len = strlen(b64);
        /* "=?base64?" (9) + b64 + "?=" (2) + NUL */
        size_t out_cap = 9 + b64_len + 2 + 1;
        char *out = a->malloc_fn(out_cap, a->userdata);
        if (out == NULL) {
            mcp_oauth_free_string(ctx, b64);
            return NULL;
        }
        int n = snprintf(out, out_cap, "=?base64?%s?=", b64);
        mcp_oauth_free_string(ctx, b64);
        if (n < 0 || (size_t)n >= out_cap) {
            a->free_fn(out, a->userdata);
            return NULL;
        }
        return out;
    }

    char *out = a->malloc_fn(plain_len + 1, a->userdata);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, plain, plain_len + 1);
    return out;
}

/* --- client storage helpers --------------------------------------------- */

/* Free all stored xmcp_params entries (used by mirror and destroy). */
static void xmcp_clear_params(mcp_context_t *ctx, mcp_client_t *client) {
    if (client->xmcp_params == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    for (size_t i = 0; i < client->n_xmcp_params; i++) {
        if (client->xmcp_params[i].name != NULL) {
            a->free_fn(client->xmcp_params[i].name, a->userdata);
        }
        if (client->xmcp_params[i].value != NULL) {
            a->free_fn(client->xmcp_params[i].value, a->userdata);
        }
    }
    a->free_fn(client->xmcp_params, a->userdata);
    client->xmcp_params = NULL;
    client->n_xmcp_params = 0;
    client->cap_xmcp_params = 0;
}

/* Deep-copy a string using the ctx allocator. */
static char *xmcp_strdup(mcp_context_t *ctx, const char *s) {
    if (s == NULL) {
        return NULL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    size_t len = strlen(s);
    char *out = a->malloc_fn(len + 1, a->userdata);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

/* Compute and store Mcp-Param-* headers for a tools/call.
 * Gated: 2026 only; no schema -> no-op.
 * On MCP_OK the client's xmcp_params list is rebuilt (previous freed). */
static mcp_status_t xmcp_mirror_headers(mcp_context_t *ctx, mcp_client_t *client,
                                        const char *tool_name,
                                        const mcp_json_value_t *args) {
    /* Clear any previous pending headers first. */
    xmcp_clear_params(ctx, client);

    if (!client_version_is_2026(client)) {
        return MCP_OK;
    }

    /* Look up cached schema for this tool. */
    const mcp_json_value_t *schema = NULL;
    for (size_t i = 0; i < client->schema_cache.n; i++) {
        if (strcmp(client->schema_cache.tool_names[i], tool_name) == 0) {
            schema = client->schema_cache.schemas[i];
            break;
        }
    }
    if (schema == NULL) {
        return MCP_OK; /* no cached schema -> no headers */
    }

    xmcp_anno_t annos[32];
    size_t n_anno = xmcp_collect_anno(ctx, schema, annos, 32);
    if (n_anno == 0) {
        return MCP_OK; /* no annotations -> no headers */
    }

    const mcp_allocator_t *a = alloc_of(ctx);
    for (size_t i = 0; i < n_anno; i++) {
        const char *param_name  = annos[i].param_name;
        const char *header_name = annos[i].header_name;
        if (param_name == NULL || header_name == NULL) {
            continue;
        }
        /* Extract the argument value (NULL if missing). */
        const mcp_json_value_t *arg_v =
            (args != NULL) ? mcp_json_object_get(ctx, args, param_name) : NULL;
        char *encoded = xmcp_encode_value(ctx, arg_v);
        if (encoded == NULL) {
            continue; /* null / missing / non-scalar -> no header */
        }

        /* Ensure capacity. */
        if (client->n_xmcp_params >= client->cap_xmcp_params) {
            size_t newcap = client->cap_xmcp_params == 0 ? 4 : client->cap_xmcp_params * 2;
            mcp_client_xmcp_param_t *new_params = a->realloc_fn(
                client->xmcp_params, newcap * sizeof(*new_params), a->userdata);
            if (new_params == NULL) {
                a->free_fn(encoded, a->userdata);
                return MCP_ERR_NOMEM;
            }
            client->xmcp_params = new_params;
            client->cap_xmcp_params = newcap;
        }

        char *name_copy  = xmcp_strdup(ctx, header_name);
        char *value_copy = xmcp_strdup(ctx, encoded);
        a->free_fn(encoded, a->userdata);
        if (name_copy == NULL || value_copy == NULL) {
            if (name_copy != NULL)  a->free_fn(name_copy,  a->userdata);
            if (value_copy != NULL) a->free_fn(value_copy, a->userdata);
            return MCP_ERR_NOMEM;
        }
        client->xmcp_params[client->n_xmcp_params].name  = name_copy;
        client->xmcp_params[client->n_xmcp_params].value = value_copy;
        client->n_xmcp_params++;
    }
    return MCP_OK;
}

/* --- public API --------------------------------------------------------- */

mcp_status_t mcp_client_set_tool_schema(mcp_context_t *ctx, mcp_client_t *client,
                                       const char *tool_name,
                                       const mcp_json_value_t *input_schema) {
    if (client == NULL || tool_name == NULL || input_schema == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *clone = mcp_json_clone(ctx, input_schema);
    if (clone == NULL) {
        return MCP_ERR_NOMEM;
    }
    const mcp_allocator_t *a = alloc_of(ctx);

    /* Grow the schema_cache arrays on demand. */
    if (client->schema_cache.n >= client->schema_cache.cap) {
        size_t newcap = client->schema_cache.cap == 0 ? 4 : client->schema_cache.cap * 2;
        char **new_names = a->realloc_fn(
            client->schema_cache.tool_names, newcap * sizeof(char *), a->userdata);
        if (new_names == NULL) {
            mcp_json_destroy(ctx, clone);
            return MCP_ERR_NOMEM;
        }
        mcp_json_value_t **new_schemas = a->realloc_fn(
            client->schema_cache.schemas, newcap * sizeof(mcp_json_value_t *), a->userdata);
        if (new_schemas == NULL) {
            a->free_fn(new_names, a->userdata);
            mcp_json_destroy(ctx, clone);
            return MCP_ERR_NOMEM;
        }
        client->schema_cache.tool_names = new_names;
        client->schema_cache.schemas    = new_schemas;
        client->schema_cache.cap        = newcap;
    }

    /* Replace existing entry or append. */
    bool found = false;
    for (size_t i = 0; i < client->schema_cache.n; i++) {
        if (strcmp(client->schema_cache.tool_names[i], tool_name) == 0) {
            a->free_fn(client->schema_cache.tool_names[i], a->userdata);
            mcp_json_destroy(ctx, client->schema_cache.schemas[i]);
            client->schema_cache.tool_names[i] = xmcp_strdup(ctx, tool_name);
            if (client->schema_cache.tool_names[i] == NULL) {
                mcp_json_destroy(ctx, clone);
                return MCP_ERR_NOMEM;
            }
            client->schema_cache.schemas[i] = clone;
            found = true;
            break;
        }
    }
    if (!found) {
        char *name_copy = xmcp_strdup(ctx, tool_name);
        if (name_copy == NULL) {
            mcp_json_destroy(ctx, clone);
            return MCP_ERR_NOMEM;
        }
        client->schema_cache.tool_names[client->schema_cache.n] = name_copy;
        client->schema_cache.schemas[client->schema_cache.n]    = clone;
        client->schema_cache.n++;
    }
    return MCP_OK;
}

const mcp_client_xmcp_param_view_t *mcp_client_get_xmcp_params(
    const mcp_client_t *client, size_t *n_out) {
    if (n_out != NULL) {
        *n_out = (client != NULL) ? client->n_xmcp_params : 0;
    }
    if (client == NULL || client->xmcp_params == NULL) {
        return NULL;
    }
    /* The layout of mcp_client_xmcp_param_t (internals.h) and
     * mcp_client_xmcp_param_view_t (client.h) is intentionally identical
     * (two char* fields), so a pointer cast is safe. */
    return (const mcp_client_xmcp_param_view_t *)(void *)&client->xmcp_params[0];
}
