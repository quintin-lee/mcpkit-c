/**
 * @file mrtr.c
 *
 * Multi Round-Trip Requests (MRTR) — protocol-level helpers.
 *
 * All JSON-building functions follow the project's ownership convention:
 * `mcp_json_object_set_take` always consumes the value passed to it,
 * on both success AND failure. Every early-return path destroys the
 * partially-built object and any values that were already taken.
 */
#include "mcpkit/protocol/mrtr.h"

#include <string.h>

#include "mcpkit/json/array.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/** Shorthand: set a string field on obj; destroy obj+extra on failure. */
static bool set_str(mcp_context_t *ctx, mcp_json_value_t *obj,
                    const char *key, const char *val,
                    mcp_json_value_t *also_destroy) {
    mcp_json_value_t *sv = mcp_json_string_new(ctx, val);
    if (sv == NULL) {
        mcp_json_destroy(ctx, also_destroy);
        mcp_json_destroy(ctx, obj);
        return false;
    }
    if (mcp_json_object_set_take(ctx, obj, key, sv) != MCP_OK) {
        /* set_take already destroyed sv */
        mcp_json_destroy(ctx, also_destroy);
        mcp_json_destroy(ctx, obj);
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Server-side builders
 * ---------------------------------------------------------------------- */

mcp_json_value_t *mcp_mrtr_elicit_request_new(mcp_context_t *ctx,
                                               const char *message,
                                               const char *mode,
                                               mcp_json_value_t *schema) {
    if (message == NULL) {
        mcp_json_destroy(ctx, schema);
        return NULL;
    }

    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        mcp_json_destroy(ctx, schema);
        return NULL;
    }

    /* "message" field (required) */
    if (!set_str(ctx, obj, "message", message, schema)) {
        return NULL; /* obj already destroyed inside set_str */
    }

    /* "mode" field (optional) */
    if (mode != NULL) {
        if (!set_str(ctx, obj, "mode", mode, schema)) {
            return NULL;
        }
    }

    /* "requestedSchema" field (optional, takes ownership) */
    if (schema != NULL) {
        if (mcp_json_object_set_take(ctx, obj, "requestedSchema", schema) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    return obj;
}

mcp_json_value_t *mcp_mrtr_result_input_required_new(mcp_context_t *ctx,
                                                      mcp_json_value_t *input_requests,
                                                      const char *request_state) {
    /* Per MCP 2026-07-28 spec: at least one of inputRequests or requestState
     * MUST be present. */
    if (input_requests == NULL && request_state == NULL) {
        return NULL;
    }

    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        mcp_json_destroy(ctx, input_requests);
        return NULL;
    }

    /* "resultType": "input_required" */
    if (!set_str(ctx, obj, "resultType", MCP_MRTR_RESULT_TYPE_INPUT_REQUIRED,
                 input_requests)) {
        return NULL;
    }

    /* "inputRequests": [...] or {...} (optional if request_state is present, takes ownership) */
    if (input_requests != NULL) {
        if (mcp_json_object_set_take(ctx, obj, "inputRequests", input_requests) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    /* "requestState": "..." (optional if input_requests is present, strdup'd) */
    if (request_state != NULL) {
        if (!set_str(ctx, obj, "requestState", request_state, NULL)) {
            return NULL;
        }
    }

    return obj;
}

/* -------------------------------------------------------------------------
 * Result introspection
 * ---------------------------------------------------------------------- */

bool mcp_mrtr_is_input_required(mcp_context_t *ctx, const mcp_json_value_t *result) {
    if (result == NULL || mcp_json_type(ctx, result) != MCP_JSON_OBJECT) {
        return false;
    }
    const mcp_json_value_t *rt = mcp_json_object_get(ctx, result, "resultType");
    if (rt == NULL || mcp_json_type(ctx, rt) != MCP_JSON_STRING) {
        return false;
    }
    const char *s = NULL;
    if (mcp_json_string_value(ctx, rt, &s) != MCP_OK || s == NULL) {
        return false;
    }
    return strcmp(s, MCP_MRTR_RESULT_TYPE_INPUT_REQUIRED) == 0;
}

const char *mcp_mrtr_get_request_state(mcp_context_t *ctx,
                                        const mcp_json_value_t *result) {
    if (result == NULL) {
        return NULL;
    }
    const mcp_json_value_t *rs = mcp_json_object_get(ctx, result, "requestState");
    if (rs == NULL || mcp_json_type(ctx, rs) != MCP_JSON_STRING) {
        return NULL;
    }
    const char *s = NULL;
    if (mcp_json_string_value(ctx, rs, &s) != MCP_OK) {
        return NULL;
    }
    return s;
}

const mcp_json_value_t *mcp_mrtr_get_input_requests(mcp_context_t *ctx,
                                                      const mcp_json_value_t *result) {
    if (result == NULL) {
        return NULL;
    }
    return mcp_json_object_get(ctx, result, "inputRequests");
}

/* -------------------------------------------------------------------------
 * Client-side: input response builder
 * ---------------------------------------------------------------------- */

mcp_json_value_t *mcp_mrtr_input_response_new(mcp_context_t *ctx,
                                               mcp_elicit_action_t action,
                                               mcp_json_value_t *data) {
    static const char *const k_action_names[] = {
        "accept", /* MCP_ELICIT_ACCEPT */
        "reject", /* MCP_ELICIT_REJECT */
        "cancel", /* MCP_ELICIT_CANCEL */
    };

    if ((unsigned)action > MCP_ELICIT_CANCEL) {
        mcp_json_destroy(ctx, data);
        return NULL;
    }

    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        mcp_json_destroy(ctx, data);
        return NULL;
    }

    /* "action" field */
    if (!set_str(ctx, obj, "action", k_action_names[action], data)) {
        return NULL;
    }

    /* "content" field (optional, takes ownership) */
    if (data != NULL) {
        if (mcp_json_object_set_take(ctx, obj, "content", data) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    return obj;
}
