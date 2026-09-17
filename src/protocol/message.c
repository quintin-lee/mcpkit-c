// message.c — JSON-RPC 2.0 envelope implementation.
//
// Each mcp_message_t is a thin wrapper around a JSON DOM tree
// (structurally validated: root must be a JSON object). The message
// owns the DOM; mcp_message_destroy frees both.
//
// Allocation goes through the context allocator (ctx_malloc/ctx_free)
// so counting allocators observe protocol-layer allocations too.

#include "mcpkit/protocol/message.h"

#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"

/**
 * JSON-RPC 2.0 envelope. Wraps an owned JSON DOM tree.
 * The DOM must be a JSON object with at least one of "method",
 * "result", or "error" keys; otherwise mcp_message_kind returns
 * MCP_MSG_INVALID.
 */
struct mcp_message {
    mcp_json_value_t *dom;
};

// Struct allocation follows the context allocator so counting
// allocators observe protocol-layer allocations too. Destroy with
// the same ctx used at creation.
static void *ctx_malloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->malloc_fn(n, a->userdata);
}

static void ctx_free(mcp_context_t *ctx, void *p) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    a->free_fn(p, a->userdata);
}

static mcp_message_t *wrap(mcp_context_t *ctx, mcp_json_value_t *dom) {
    mcp_message_t *msg = ctx_malloc(ctx, sizeof(*msg));
    if (msg == NULL) {
        return NULL;
    }
    msg->dom = dom;
    return msg;
}

static const mcp_json_value_t *get_key(mcp_context_t *ctx, const mcp_message_t *msg,
                                       const char *key) {
    if (msg == NULL || msg->dom == NULL) {
        return NULL;
    }
    return mcp_json_object_get(ctx, msg->dom, key);
}

static mcp_msg_kind_t classify(mcp_context_t *ctx, const mcp_json_value_t *dom) {
    const mcp_json_value_t *method = mcp_json_object_get(ctx, dom, "method");
    if (method != NULL) {
        const char *name = NULL;
        if (mcp_json_string_value(ctx, method, &name) != MCP_OK) {
            return MCP_MSG_INVALID;
        }
        (void)name;
        return mcp_json_object_has(ctx, dom, "id") ? MCP_MSG_REQUEST : MCP_MSG_NOTIFICATION;
    }
    if (mcp_json_object_has(ctx, dom, "result") || mcp_json_object_has(ctx, dom, "error")) {
        return MCP_MSG_RESPONSE;
    }
    return MCP_MSG_INVALID;
}

mcp_message_t *mcp_message_parse(mcp_context_t *ctx, const char *text, size_t len) {
    if (text == NULL || len > MCP_PROTOCOL_MAX_MESSAGE_BYTES) {
        return NULL;
    }
    mcp_json_value_t *dom = mcp_json_parse(ctx, text, len);
    if (dom == NULL || mcp_json_type(ctx, dom) != MCP_JSON_OBJECT) {
        if (dom != NULL) {
            mcp_json_destroy(ctx, dom);
        }
        return NULL;
    }
    mcp_message_t *msg = wrap(ctx, dom);
    if (msg == NULL) {
        mcp_json_destroy(ctx, dom);
    }
    return msg;
}

void mcp_message_destroy(mcp_context_t *ctx, mcp_message_t *msg) {
    if (msg == NULL) {
        return;
    }
    mcp_json_destroy(ctx, msg->dom);
    ctx_free(ctx, msg);
}

mcp_msg_kind_t mcp_message_kind(mcp_context_t *ctx, const mcp_message_t *msg) {
    if (msg == NULL || msg->dom == NULL) {
        return MCP_MSG_INVALID;
    }
    return classify(ctx, msg->dom);
}

const char *mcp_message_jsonrpc(mcp_context_t *ctx, const mcp_message_t *msg) {
    const mcp_json_value_t *jv = get_key(ctx, msg, "jsonrpc");
    const char *s = NULL;
    if (jv == NULL || mcp_json_string_value(ctx, jv, &s) != MCP_OK) {
        return NULL;
    }
    return s;
}

const char *mcp_message_method(mcp_context_t *ctx, const mcp_message_t *msg) {
    const mcp_json_value_t *method = get_key(ctx, msg, "method");
    const char *name = NULL;
    if (method == NULL || mcp_json_string_value(ctx, method, &name) != MCP_OK) {
        return NULL;
    }
    return name;
}

const mcp_json_value_t *mcp_message_params(mcp_context_t *ctx, const mcp_message_t *msg) {
    return get_key(ctx, msg, "params");
}

mcp_id_type_t mcp_message_id_type(mcp_context_t *ctx, const mcp_message_t *msg) {
    const mcp_json_value_t *id = get_key(ctx, msg, "id");
    if (id == NULL) {
        return MCP_ID_NONE;
    }
    switch (mcp_json_type(ctx, id)) {
        case MCP_JSON_STRING:
            return MCP_ID_STRING;
        case MCP_JSON_NUMBER:
            return MCP_ID_NUMBER;
        default:
            return MCP_ID_NONE;
    }
}

const char *mcp_message_id_string(mcp_context_t *ctx, const mcp_message_t *msg) {
    const mcp_json_value_t *id = get_key(ctx, msg, "id");
    const char *s = NULL;
    if (id == NULL || mcp_json_string_value(ctx, id, &s) != MCP_OK) {
        return NULL;
    }
    return s;
}

mcp_status_t mcp_message_id_number(mcp_context_t *ctx, const mcp_message_t *msg, double *out) {
    const mcp_json_value_t *id = get_key(ctx, msg, "id");
    if (out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (id == NULL || mcp_json_number_value(ctx, id, out) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return MCP_OK;
}

const mcp_json_value_t *mcp_message_result(mcp_context_t *ctx, const mcp_message_t *msg) {
    return get_key(ctx, msg, "result");
}

static const mcp_json_value_t *error_obj(mcp_context_t *ctx, const mcp_message_t *msg) {
    const mcp_json_value_t *err = get_key(ctx, msg, "error");
    if (err == NULL || mcp_json_type(ctx, err) != MCP_JSON_OBJECT) {
        return NULL;
    }
    return err;
}

mcp_status_t mcp_message_error_code(mcp_context_t *ctx, const mcp_message_t *msg, int *out) {
    if (out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_json_value_t *err = error_obj(ctx, msg);
    const mcp_json_value_t *code = err != NULL ? mcp_json_object_get(ctx, err, "code") : NULL;
    double d = 0;
    if (code == NULL || mcp_json_number_value(ctx, code, &d) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = (int)d;
    return MCP_OK;
}

const char *mcp_message_error_text(mcp_context_t *ctx, const mcp_message_t *msg) {
    const mcp_json_value_t *err = error_obj(ctx, msg);
    const mcp_json_value_t *text = err != NULL ? mcp_json_object_get(ctx, err, "message") : NULL;
    const char *s = NULL;
    if (text == NULL || mcp_json_string_value(ctx, text, &s) != MCP_OK) {
        return NULL;
    }
    return s;
}

static mcp_json_value_t *new_envelope(mcp_context_t *ctx) {
    mcp_json_value_t *dom = mcp_json_object_new(ctx);
    if (dom == NULL) {
        return NULL;
    }
    mcp_json_value_t *v = mcp_json_string_new(ctx, "2.0");
    if (v == NULL || mcp_json_object_set(ctx, dom, "jsonrpc", v) != MCP_OK) {
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        mcp_json_destroy(ctx, dom);
        return NULL;
    }
    return dom;
}

static mcp_status_t set_owned(mcp_context_t *ctx, mcp_json_value_t *dom, const char *key,
                              mcp_json_value_t *val) {
    if (val == NULL) {
        return MCP_OK;
    }
    if (mcp_json_object_set(ctx, dom, key, val) != MCP_OK) {
        mcp_json_destroy(ctx, val);
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

static mcp_message_t *finish(mcp_context_t *ctx, mcp_json_value_t *dom) {
    mcp_message_t *msg = wrap(ctx, dom);
    if (msg == NULL) {
        mcp_json_destroy(ctx, dom);
    }
    return msg;
}

// Ownership rule for all builders below: on success the message owns
// every passed-in value; on NULL return the caller retains them.
mcp_message_t *mcp_request_new_string_id(mcp_context_t *ctx, const char *id,
                                         const char *method, mcp_json_value_t *params) {
    if (id == NULL || method == NULL) {
        return NULL;
    }
    mcp_json_value_t *dom = new_envelope(ctx);
    mcp_json_value_t *idv = dom != NULL ? mcp_json_string_new(ctx, id) : NULL;
    mcp_json_value_t *mv = idv != NULL ? mcp_json_string_new(ctx, method) : NULL;
    if (dom == NULL || idv == NULL || mv == NULL) {
        if (idv != NULL) {
            mcp_json_destroy(ctx, idv);
        }
        if (mv != NULL) {
            mcp_json_destroy(ctx, mv);
        }
        if (dom != NULL) {
            mcp_json_destroy(ctx, dom);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, dom, "id", idv) != MCP_OK ||
        mcp_json_object_set(ctx, dom, "method", mv) != MCP_OK ||
        set_owned(ctx, dom, "params", params) != MCP_OK) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        mcp_json_destroy(ctx, dom);
        return NULL;
    }
    return finish(ctx, dom);
}

mcp_message_t *mcp_request_new_number_id(mcp_context_t *ctx, double id,
                                         const char *method, mcp_json_value_t *params) {
    if (method == NULL) {
        return NULL;
    }
    mcp_json_value_t *dom = new_envelope(ctx);
    mcp_json_value_t *idv = dom != NULL ? mcp_json_number_new(ctx, id) : NULL;
    mcp_json_value_t *mv = idv != NULL ? mcp_json_string_new(ctx, method) : NULL;
    if (dom == NULL || idv == NULL || mv == NULL) {
        if (idv != NULL) {
            mcp_json_destroy(ctx, idv);
        }
        if (mv != NULL) {
            mcp_json_destroy(ctx, mv);
        }
        if (dom != NULL) {
            mcp_json_destroy(ctx, dom);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, dom, "id", idv) != MCP_OK ||
        mcp_json_object_set(ctx, dom, "method", mv) != MCP_OK ||
        set_owned(ctx, dom, "params", params) != MCP_OK) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        mcp_json_destroy(ctx, dom);
        return NULL;
    }
    return finish(ctx, dom);
}

mcp_message_t *mcp_notification_new(mcp_context_t *ctx, const char *method,
                                    mcp_json_value_t *params) {
    if (method == NULL) {
        return NULL;
    }
    mcp_json_value_t *dom = new_envelope(ctx);
    mcp_json_value_t *mv = dom != NULL ? mcp_json_string_new(ctx, method) : NULL;
    if (dom == NULL || mv == NULL) {
        if (mv != NULL) {
            mcp_json_destroy(ctx, mv);
        }
        if (dom != NULL) {
            mcp_json_destroy(ctx, dom);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, dom, "method", mv) != MCP_OK ||
        set_owned(ctx, dom, "params", params) != MCP_OK) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        mcp_json_destroy(ctx, dom);
        return NULL;
    }
    return finish(ctx, dom);
}

static mcp_json_value_t *copy_id(mcp_context_t *ctx, const mcp_message_t *req) {
    if (req == NULL) {
        return mcp_json_null_new(ctx);
    }
    switch (mcp_message_id_type(ctx, req)) {
        case MCP_ID_STRING:
            return mcp_json_string_new(ctx, mcp_message_id_string(ctx, req));
        case MCP_ID_NUMBER: {
            double d = 0;
            if (mcp_message_id_number(ctx, req, &d) != MCP_OK) {
                return NULL;
            }
            return mcp_json_number_new(ctx, d);
        }
        default:
            return mcp_json_null_new(ctx);
    }
}

mcp_message_t *mcp_response_ok_new(mcp_context_t *ctx, const mcp_message_t *req,
                                   mcp_json_value_t *result) {
    if (req == NULL) {
        return NULL;
    }
    mcp_json_value_t *dom = new_envelope(ctx);
    mcp_json_value_t *idv = dom != NULL ? copy_id(ctx, req) : NULL;
    if (dom == NULL || idv == NULL) {
        if (idv != NULL) {
            mcp_json_destroy(ctx, idv);
        }
        if (dom != NULL) {
            mcp_json_destroy(ctx, dom);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, dom, "id", idv) != MCP_OK ||
        set_owned(ctx, dom, "result", result) != MCP_OK) {
        if (result != NULL) {
            mcp_json_destroy(ctx, result);
        }
        mcp_json_destroy(ctx, dom);
        return NULL;
    }
    return finish(ctx, dom);
}

mcp_message_t *mcp_response_err_new(mcp_context_t *ctx, const mcp_message_t *req_or_null,
                                    int code, const char *message, mcp_json_value_t *data) {
    if (message == NULL) {
        return NULL;
    }
    mcp_json_value_t *dom = new_envelope(ctx);
    mcp_json_value_t *idv = dom != NULL ? copy_id(ctx, req_or_null) : NULL;
    mcp_json_value_t *err = idv != NULL ? mcp_json_object_new(ctx) : NULL;
    mcp_json_value_t *codev = err != NULL ? mcp_json_number_new(ctx, (double)code) : NULL;
    mcp_json_value_t *msgv = codev != NULL ? mcp_json_string_new(ctx, message) : NULL;
    if (dom == NULL || idv == NULL || err == NULL || codev == NULL || msgv == NULL) {
        if (idv != NULL) {
            mcp_json_destroy(ctx, idv);
        }
        if (err != NULL) {
            mcp_json_destroy(ctx, err);
        }
        if (codev != NULL) {
            mcp_json_destroy(ctx, codev);
        }
        if (msgv != NULL) {
            mcp_json_destroy(ctx, msgv);
        }
        if (dom != NULL) {
            mcp_json_destroy(ctx, dom);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, dom, "id", idv) != MCP_OK ||
        mcp_json_object_set(ctx, err, "code", codev) != MCP_OK ||
        mcp_json_object_set(ctx, err, "message", msgv) != MCP_OK ||
        set_owned(ctx, err, "data", data) != MCP_OK ||
        mcp_json_object_set(ctx, dom, "error", err) != MCP_OK) {
        if (data != NULL) {
            mcp_json_destroy(ctx, data);
        }
        mcp_json_destroy(ctx, dom);
        return NULL;
    }
    return finish(ctx, dom);
}

char *mcp_message_serialize(mcp_context_t *ctx, const mcp_message_t *msg) {
    if (msg == NULL || msg->dom == NULL) {
        return NULL;
    }
    return mcp_json_serialize(ctx, msg->dom);
}

int mcp_status_to_rpc_code(mcp_status_t status) {
    switch (status) {
        case MCP_OK:
            return 0;
        case MCP_ERR_INVALID_ARGUMENT:
            return MCP_RPC_INVALID_PARAMS;
        case MCP_ERR_NOT_FOUND:
            return MCP_RPC_METHOD_NOT_FOUND;
        case MCP_ERR_PROTOCOL:
            return MCP_RPC_INVALID_REQUEST;
        default:
            return MCP_RPC_INTERNAL_ERROR;
    }
}

mcp_status_t mcp_rpc_code_to_status(int code) {
    switch (code) {
        case 0:
            return MCP_OK;
        case MCP_RPC_INVALID_PARAMS:
            return MCP_ERR_INVALID_ARGUMENT;
        case MCP_RPC_METHOD_NOT_FOUND:
            return MCP_ERR_NOT_FOUND;
        case MCP_RPC_INVALID_REQUEST:
        case MCP_RPC_PARSE_ERROR:
            return MCP_ERR_PROTOCOL;
        default:
            return MCP_ERR_PROTOCOL;
    }
}
