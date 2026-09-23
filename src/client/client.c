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
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/mrtr.h"
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
