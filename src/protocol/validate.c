#include "mcpkit/protocol/validate.h"

#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/protocol/initialize.h"

typedef struct {
    mcp_id_type_t type;
    char *s;
    double n;
} mcp_id_entry_t;

struct mcp_idset {
    mcp_id_entry_t *items;
    size_t len;
    size_t cap;
};

static void *id_malloc(mcp_context_t *ctx, size_t n) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->malloc_fn(n, a->userdata);
}

static void id_free(mcp_context_t *ctx, void *p) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    a->free_fn(p, a->userdata);
}

static void *id_realloc(mcp_context_t *ctx, void *p, size_t n) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->realloc_fn(p, n, a->userdata);
}

mcp_idset_t *mcp_idset_create(mcp_context_t *ctx) {
    mcp_idset_t *set = id_malloc(ctx, sizeof(*set));
    if (set == NULL) {
        return NULL;
    }
    set->items = NULL;
    set->len = 0;
    set->cap = 0;
    return set;
}

void mcp_idset_destroy(mcp_context_t *ctx, mcp_idset_t *set) {
    if (set == NULL) {
        return;
    }
    for (size_t i = 0; i < set->len; i++) {
        id_free(ctx, set->items[i].s);
    }
    id_free(ctx, set->items);
    id_free(ctx, set);
}

static bool id_equal(const mcp_id_entry_t *e, mcp_id_type_t type, const char *s, double n) {
    if (e->type != type) {
        return false;
    }
    if (type == MCP_ID_STRING) {
        return s != NULL && e->s != NULL && strcmp(e->s, s) == 0;
    }
    if (type == MCP_ID_NUMBER) {
        return e->n == n;
    }
    return false;
}

static int id_find(const mcp_idset_t *set, mcp_id_type_t type, const char *s, double n) {
    for (size_t i = 0; i < set->len; i++) {
        if (id_equal(&set->items[i], type, s, n)) {
            return (int)i;
        }
    }
    return -1;
}

mcp_status_t mcp_idset_add(mcp_context_t *ctx, mcp_idset_t *set,
                           mcp_id_type_t type, const char *s, double n) {
    if (set == NULL || (type != MCP_ID_STRING && type != MCP_ID_NUMBER) ||
        (type == MCP_ID_STRING && s == NULL)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (id_find(set, type, s, n) >= 0) {
        return MCP_ERR_ALREADY_EXISTS;
    }
    if (set->len == set->cap) {
        size_t ncap = set->cap != 0 ? set->cap * 2 : 8;
        mcp_id_entry_t *nitems = id_realloc(ctx, set->items, ncap * sizeof(*nitems));
        if (nitems == NULL) {
            return MCP_ERR_NOMEM;
        }
        set->items = nitems;
        set->cap = ncap;
    }
    mcp_id_entry_t *e = &set->items[set->len];
    e->type = type;
    e->s = NULL;
    e->n = n;
    if (type == MCP_ID_STRING) {
        size_t klen = strlen(s);
        e->s = id_malloc(ctx, klen + 1);
        if (e->s == NULL) {
            return MCP_ERR_NOMEM;
        }
        memcpy(e->s, s, klen + 1);
    }
    set->len++;
    return MCP_OK;
}

void mcp_idset_remove(mcp_context_t *ctx, mcp_idset_t *set,
                      mcp_id_type_t type, const char *s, double n) {
    if (set == NULL) {
        return;
    }
    int idx = id_find(set, type, s, n);
    if (idx < 0) {
        return;
    }
    id_free(ctx, set->items[idx].s);
    size_t tail = set->len - (size_t)idx - 1;
    if (tail > 0) {
        memmove(&set->items[idx], &set->items[idx + 1], tail * sizeof(set->items[0]));
    }
    set->len--;
}

bool mcp_idset_contains(mcp_context_t *ctx, const mcp_idset_t *set,
                        mcp_id_type_t type, const char *s, double n) {
    (void)ctx;
    if (set == NULL) {
        return false;
    }
    return id_find(set, type, s, n) >= 0;
}

static const char *const k_known_methods[] = {
    "initialize",
    "ping",
    "tools/list",
    "tools/call",
    "resources/list",
    "resources/templates/list",
    "resources/read",
    "resources/subscribe",
    "resources/unsubscribe",
    "prompts/list",
    "prompts/get",
    "completion/complete",
    "completion/list",
    "logging/setLevel",
    "notifications/initialized",
    "notifications/cancelled",
    "notifications/progress",
    "notifications/tools/list_changed",
    "notifications/resources/list_changed",
    "notifications/resources/updated",
    "notifications/prompts/list_changed",
    "notifications/message",
    "roots/list",
    "roots/list_changed",
    "sampling/createMessage",
    "elicitation/create",
};

bool mcp_method_known(const char *method) {
    if (method == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_known_methods) / sizeof(k_known_methods[0]); i++) {
        if (strcmp(method, k_known_methods[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void set_code(int *out, int code) {
    if (out != NULL) {
        *out = code;
    }
}

mcp_status_t mcp_validate_envelope(mcp_context_t *ctx, const mcp_message_t *msg,
                                   int *rpc_code_out) {
    if (msg == NULL) {
        set_code(rpc_code_out, MCP_RPC_INVALID_REQUEST);
        return MCP_ERR_PROTOCOL;
    }
    mcp_msg_kind_t kind = mcp_message_kind(ctx, msg);
    const char *jr = mcp_message_jsonrpc(ctx, msg);
    if (jr == NULL || strcmp(jr, "2.0") != 0) {
        set_code(rpc_code_out, MCP_RPC_INVALID_REQUEST);
        return MCP_ERR_PROTOCOL;
    }
    if (kind == MCP_MSG_REQUEST) {
        mcp_id_type_t idt = mcp_message_id_type(ctx, msg);
        const char *method = mcp_message_method(ctx, msg);
        if ((idt != MCP_ID_STRING && idt != MCP_ID_NUMBER) || method == NULL) {
            set_code(rpc_code_out, MCP_RPC_INVALID_REQUEST);
            return MCP_ERR_PROTOCOL;
        }
    } else if (kind == MCP_MSG_NOTIFICATION) {
        if (mcp_message_method(ctx, msg) == NULL) {
            set_code(rpc_code_out, MCP_RPC_INVALID_REQUEST);
            return MCP_ERR_PROTOCOL;
        }
    } else if (kind == MCP_MSG_RESPONSE) {
        bool has_result = mcp_message_result(ctx, msg) != NULL;
        // error presence: error_code succeeds only on well-formed error object
        int dummy = 0;
        bool has_error = mcp_message_error_code(ctx, msg, &dummy) == MCP_OK;
        bool error_ok = !has_error || mcp_message_error_text(ctx, msg) != NULL;
        if (has_result == has_error || !error_ok) {
            set_code(rpc_code_out, MCP_RPC_INVALID_REQUEST);
            return MCP_ERR_PROTOCOL;
        }
    } else {
        set_code(rpc_code_out, MCP_RPC_INVALID_REQUEST);
        return MCP_ERR_PROTOCOL;
    }
    set_code(rpc_code_out, 0);
    return MCP_OK;
}

mcp_status_t mcp_validate_method(mcp_context_t *ctx, const mcp_message_t *msg,
                                 int *rpc_code_out) {
    if (mcp_message_kind(ctx, msg) == MCP_MSG_RESPONSE) {
        set_code(rpc_code_out, 0);
        return MCP_OK;
    }
    const char *method = mcp_message_method(ctx, msg);
    if (method == NULL || !mcp_method_known(method)) {
        set_code(rpc_code_out, MCP_RPC_METHOD_NOT_FOUND);
        return MCP_ERR_NOT_FOUND;
    }
    set_code(rpc_code_out, 0);
    return MCP_OK;
}

static mcp_status_t require_string_param(mcp_context_t *ctx, const mcp_json_value_t *params,
                                         const char *key) {
    const mcp_json_value_t *f = mcp_json_object_get(ctx, params, key);
    const char *s = NULL;
    if (f == NULL || mcp_json_string_value(ctx, f, &s) != MCP_OK || s == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return MCP_OK;
}

mcp_status_t mcp_validate_params(mcp_context_t *ctx, const mcp_message_t *msg,
                                 int *rpc_code_out) {
    mcp_status_t fail = MCP_ERR_INVALID_ARGUMENT;
    if (mcp_message_kind(ctx, msg) == MCP_MSG_RESPONSE) {
        set_code(rpc_code_out, 0);
        return MCP_OK;
    }
    const char *method = mcp_message_method(ctx, msg);
    const mcp_json_value_t *params = mcp_message_params(ctx, msg);
    if (method == NULL) {
        goto invalid;
    }
    if (params == NULL) {
        // Only parameterless notifications pass without params.
        if (mcp_message_kind(ctx, msg) == MCP_MSG_NOTIFICATION) {
            set_code(rpc_code_out, 0);
            return MCP_OK;
        }
        if (strcmp(method, "initialize") == 0 || strcmp(method, "tools/call") == 0 ||
            strcmp(method, "resources/read") == 0 || strcmp(method, "prompts/get") == 0 ||
            strcmp(method, "completion/complete") == 0) {
            goto invalid;
        }
        set_code(rpc_code_out, 0);
        return MCP_OK;
    }
    if (mcp_json_type(ctx, params) != MCP_JSON_OBJECT) {
        goto invalid;
    }
    if (strcmp(method, "initialize") == 0) {
        if (mcp_initialize_params_validate(ctx, params) != MCP_OK) {
            goto invalid;
        }
    } else if (strcmp(method, "tools/call") == 0) {
        if (require_string_param(ctx, params, "name") != MCP_OK) {
            goto invalid;
        }
    } else if (strcmp(method, "resources/read") == 0) {
        if (require_string_param(ctx, params, "uri") != MCP_OK) {
            goto invalid;
        }
    } else if (strcmp(method, "prompts/get") == 0) {
        if (require_string_param(ctx, params, "name") != MCP_OK) {
            goto invalid;
        }
    } else if (strcmp(method, "completion/complete") == 0) {
        const mcp_json_value_t *ref = mcp_json_object_get(ctx, params, "ref");
        const mcp_json_value_t *arg = mcp_json_object_get(ctx, params, "argument");
        if (ref == NULL || arg == NULL ||
            mcp_json_type(ctx, ref) != MCP_JSON_OBJECT ||
            mcp_json_type(ctx, arg) != MCP_JSON_OBJECT) {
            goto invalid;
        }
    }
    set_code(rpc_code_out, 0);
    return MCP_OK;
invalid:
    set_code(rpc_code_out, MCP_RPC_INVALID_PARAMS);
    return fail;
}

mcp_status_t mcp_message_validate(mcp_context_t *ctx, const mcp_message_t *msg,
                                  int *rpc_code_out) {
    mcp_status_t st = mcp_validate_envelope(ctx, msg, rpc_code_out);
    if (st != MCP_OK) {
        return st;
    }
    st = mcp_validate_method(ctx, msg, rpc_code_out);
    if (st != MCP_OK) {
        return st;
    }
    return mcp_validate_params(ctx, msg, rpc_code_out);
}
