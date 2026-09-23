/**
 * @file trace.c
 * @brief W3C Trace Context propagation helpers for MCP _meta (SEP-414).
 * @ingroup mcpkit-core
 */

#include "mcpkit/core/trace.h"
#include <string.h>
#include <stdio.h>

static void safe_copy_str(char *dst, size_t dst_sz, const char *src) {
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_sz) {
        len = dst_sz - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

mcp_status_t mcp_trace_extract_from_meta(mcp_context_t *ctx,
                                         const mcp_json_value_t *meta,
                                         mcp_trace_context_t *out) {
    if (meta == NULL || out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_json_type(ctx, meta) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));

    const mcp_json_value_t *v_tp = mcp_json_object_get(ctx, meta, "traceparent");
    if (v_tp != NULL && mcp_json_type(ctx, v_tp) == MCP_JSON_STRING) {
        const char *s = NULL;
        if (mcp_json_string_value(ctx, v_tp, &s) == MCP_OK && s != NULL) {
            safe_copy_str(out->traceparent, sizeof(out->traceparent), s);
        }
    }

    const mcp_json_value_t *v_ts = mcp_json_object_get(ctx, meta, "tracestate");
    if (v_ts != NULL && mcp_json_type(ctx, v_ts) == MCP_JSON_STRING) {
        const char *s = NULL;
        if (mcp_json_string_value(ctx, v_ts, &s) == MCP_OK && s != NULL) {
            safe_copy_str(out->tracestate, sizeof(out->tracestate), s);
        }
    }

    const mcp_json_value_t *v_bg = mcp_json_object_get(ctx, meta, "baggage");
    if (v_bg != NULL && mcp_json_type(ctx, v_bg) == MCP_JSON_STRING) {
        const char *s = NULL;
        if (mcp_json_string_value(ctx, v_bg, &s) == MCP_OK && s != NULL) {
            safe_copy_str(out->baggage, sizeof(out->baggage), s);
        }
    }

    return MCP_OK;
}

mcp_status_t mcp_trace_inject_into_meta(mcp_context_t *ctx,
                                        mcp_json_value_t *meta,
                                        const mcp_trace_context_t *trace) {
    if (meta == NULL || trace == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_json_type(ctx, meta) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    if (trace->traceparent[0] != '\0') {
        mcp_json_value_t *v = mcp_json_string_new(ctx, trace->traceparent);
        if (v == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, meta, "traceparent", v) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    }

    if (trace->tracestate[0] != '\0') {
        mcp_json_value_t *v = mcp_json_string_new(ctx, trace->tracestate);
        if (v == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, meta, "tracestate", v) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    }

    if (trace->baggage[0] != '\0') {
        mcp_json_value_t *v = mcp_json_string_new(ctx, trace->baggage);
        if (v == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, meta, "baggage", v) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    }

    return MCP_OK;
}
