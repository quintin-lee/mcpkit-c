#include "mcpkit/core/trace.h"
#include "mcpkit/json/json.h"
#include "test_check.h"
#include <string.h>

int main(void) {
    mcp_context_t *ctx = NULL;

    /* Test 1: Null validation */
    mcp_trace_context_t trace;
    memset(&trace, 0, sizeof(trace));
    CHECK(mcp_trace_extract_from_meta(ctx, NULL, &trace) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_trace_extract_from_meta(ctx, (const mcp_json_value_t *)1, NULL) == MCP_ERR_INVALID_ARGUMENT);

    /* Test 2: Extraction from well-formed _meta */
    mcp_json_value_t *meta = mcp_json_object_new(ctx);
    CHECK(meta != NULL);

    mcp_json_value_t *tp = mcp_json_string_new(ctx, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    mcp_json_object_set_take(ctx, meta, "traceparent", tp);
    mcp_json_value_t *ts = mcp_json_string_new(ctx, "rojo=1,congo=2");
    mcp_json_object_set_take(ctx, meta, "tracestate", ts);
    mcp_json_value_t *bg = mcp_json_string_new(ctx, "userId=alice");
    mcp_json_object_set_take(ctx, meta, "baggage", bg);

    CHECK(mcp_trace_extract_from_meta(ctx, meta, &trace) == MCP_OK);
    CHECK(strcmp(trace.traceparent, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01") == 0);
    CHECK(strcmp(trace.tracestate, "rojo=1,congo=2") == 0);
    CHECK(strcmp(trace.baggage, "userId=alice") == 0);

    /* Test 3: Injection into empty _meta */
    mcp_json_value_t *out_meta = mcp_json_object_new(ctx);
    CHECK(mcp_trace_inject_into_meta(ctx, out_meta, &trace) == MCP_OK);

    const mcp_json_value_t *v_tp = mcp_json_object_get(ctx, out_meta, "traceparent");
    CHECK(v_tp != NULL);
    const char *str_tp = NULL;
    CHECK(mcp_json_string_value(ctx, v_tp, &str_tp) == MCP_OK);
    CHECK(strcmp(str_tp, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01") == 0);

    const mcp_json_value_t *v_ts = mcp_json_object_get(ctx, out_meta, "tracestate");
    CHECK(v_ts != NULL);
    const char *str_ts = NULL;
    CHECK(mcp_json_string_value(ctx, v_ts, &str_ts) == MCP_OK);
    CHECK(strcmp(str_ts, "rojo=1,congo=2") == 0);

    const mcp_json_value_t *v_bg = mcp_json_object_get(ctx, out_meta, "baggage");
    CHECK(v_bg != NULL);
    const char *str_bg = NULL;
    CHECK(mcp_json_string_value(ctx, v_bg, &str_bg) == MCP_OK);
    CHECK(strcmp(str_bg, "userId=alice") == 0);

    /* Test 4: Empty fields are not injected */
    mcp_trace_context_t empty_trace;
    memset(&empty_trace, 0, sizeof(empty_trace));
    mcp_json_value_t *meta_sparse = mcp_json_object_new(ctx);
    CHECK(mcp_trace_inject_into_meta(ctx, meta_sparse, &empty_trace) == MCP_OK);
    CHECK(mcp_json_object_get(ctx, meta_sparse, "traceparent") == NULL);
    CHECK(mcp_json_object_get(ctx, meta_sparse, "tracestate") == NULL);
    CHECK(mcp_json_object_get(ctx, meta_sparse, "baggage") == NULL);

    mcp_json_destroy(ctx, meta);
    mcp_json_destroy(ctx, out_meta);
    mcp_json_destroy(ctx, meta_sparse);
    return 0;
}
