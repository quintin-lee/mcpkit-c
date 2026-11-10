#include "test_check.h"
#include <stdio.h>
#include <string.h>

#include "mcpkit/core/error.h"
#include "mcpkit/mcpkit.h"
#include "mcpkit/protocol/policy.h"

typedef struct {
    const char *method;
    int id;
} case_entry_t;

static const case_entry_t k_cases[] = {
    { "ping", 1 },
    { "logging/setLevel", 2 },
    { "resources/subscribe", 3 },
    { "resources/unsubscribe", 4 },
    { "tools/list", 5 },
    { "prompts/list", 6 },
    { "resources/list", 7 },
};

static mcp_status_t ok_handler(mcp_context_t *ctx, mcp_session_t *session,
                               const mcp_json_value_t *args, void *user_data,
                               mcp_json_value_t **result_out) {
    (void)ctx; (void)session; (void)args; (void)user_data;
    *result_out = NULL;
    return MCP_OK;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "policy-matrix", "1.0");
    CHECK(srv != NULL);

    /* Legacy session: 2024-11-05 (baseline) — must complete initialize
     * handshake so the dispatcher's initialized gate passes. */
    mcp_session_t *s_legacy = mcp_server_create_session(ctx, srv);
    CHECK(s_legacy != NULL);
    CHECK(mcp_session_set_negotiated(ctx, s_legacy, MCP_PROTO_2024_11_05) == MCP_OK);
    {
        const char *init_req =
            "{\"jsonrpc\":\"2.0\",\"id\":1000,\"method\":\"initialize\","
            "\"params\":{\"protocolVersion\":\"2024-11-05\","
            "\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"0\"}}}";
        mcp_message_t *init_msg = mcp_message_parse(ctx, init_req, strlen(init_req));
        CHECK(init_msg != NULL);
        mcp_message_t *init_resp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, s_legacy, init_msg, &init_resp) == MCP_OK);
        mcp_message_destroy(ctx, init_resp);
        mcp_message_destroy(ctx, init_msg);
        /* Complete the handshake with notifications/initialized */
        const char *notif =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        mcp_message_t *notif_msg = mcp_message_parse(ctx, notif, strlen(notif));
        CHECK(notif_msg != NULL);
        CHECK(mcp_server_notify(ctx, srv, s_legacy, notif_msg) == MCP_OK);
        mcp_message_destroy(ctx, notif_msg);
    }

    /* 2026 session: stateless — no initialize handshake required */
    mcp_session_t *s_2026 = mcp_server_create_session(ctx, srv);
    CHECK(s_2026 != NULL);
    CHECK(mcp_session_set_negotiated(ctx, s_2026, MCP_PROTO_2026_07_28) == MCP_OK);

    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        const case_entry_t *c = &k_cases[i];
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\"}",
                 c->id, c->method);
        mcp_message_t *req = mcp_message_parse(ctx, buf, strlen(buf));
        CHECK(req != NULL);

        mcp_message_t *resp_legacy = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, s_legacy, req, &resp_legacy) == MCP_OK);
        CHECK(resp_legacy != NULL);
        /* Legacy: no 2026 deprecation gate; methods may return their own
         * error, but must NOT be -32601 (method not found). */
        int legacy_code = 0;
        bool legacy_err = (mcp_message_error_code(ctx, resp_legacy, &legacy_code) == MCP_OK);
        CHECK(!legacy_err || legacy_code != MCP_RPC_METHOD_NOT_FOUND);
        mcp_message_destroy(ctx, resp_legacy);

        mcp_message_t *resp_2026 = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, s_2026, req, &resp_2026) == MCP_OK);
        CHECK(resp_2026 != NULL);
        int code_2026 = 0;
        bool has_err = (mcp_message_error_code(ctx, resp_2026, &code_2026) == MCP_OK);
        /* For deprecated methods, expect -32601 in 2026. */
        if (strcmp(c->method, "ping") == 0 ||
            strcmp(c->method, "logging/setLevel") == 0 ||
            strcmp(c->method, "resources/subscribe") == 0 ||
            strcmp(c->method, "resources/unsubscribe") == 0) {
            CHECK(has_err && code_2026 == MCP_RPC_METHOD_NOT_FOUND);
        } else {
            /* tools/list / prompts/list / resources/list: should succeed or at
             * least not be rejected by the deprecation gate. */
            CHECK(!has_err || code_2026 != MCP_RPC_METHOD_NOT_FOUND);
        }
        mcp_message_destroy(ctx, resp_2026);
        mcp_message_destroy(ctx, req);
    }

    /* Verify deterministic sorting on 2026 session: register 3 tools in
     * unsorted order and confirm tools/list returns them alphabetically. */
    mcp_tool_t *tool_zeta = mcp_tool_new(ctx, "zeta", "tool zeta", NULL, ok_handler, NULL);
    mcp_tool_t *tool_alpha = mcp_tool_new(ctx, "alpha", "tool alpha", NULL, ok_handler, NULL);
    mcp_tool_t *tool_mid = mcp_tool_new(ctx, "mid", "tool mid", NULL, ok_handler, NULL);
    CHECK(tool_zeta != NULL && tool_alpha != NULL && tool_mid != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool_zeta) == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv, tool_alpha) == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv, tool_mid) == MCP_OK);

    const char *k_tools_list_req =
        "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/list\"}";
    mcp_message_t *req_tl = mcp_message_parse(ctx, k_tools_list_req, strlen(k_tools_list_req));
    CHECK(req_tl != NULL);

    mcp_message_t *resp_tl = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s_2026, req_tl, &resp_tl) == MCP_OK);
    CHECK(resp_tl != NULL);
    /* tools/list succeeds (no error envelope) on a well-formed 2026 session */
    const mcp_json_value_t *tl_result = mcp_message_result(ctx, resp_tl);
    CHECK(tl_result != NULL);
    const mcp_json_value_t *tools_arr = mcp_json_object_get(ctx, tl_result, "tools");
    CHECK(tools_arr != NULL && mcp_json_type(ctx, tools_arr) == MCP_JSON_ARRAY);
    CHECK(mcp_json_array_size(ctx, tools_arr) == 3);
    const mcp_json_value_t *t0 = mcp_json_array_get(ctx, tools_arr, 0);
    const mcp_json_value_t *t1 = mcp_json_array_get(ctx, tools_arr, 1);
    const mcp_json_value_t *t2 = mcp_json_array_get(ctx, tools_arr, 2);
    const char *n0 = NULL, *n1 = NULL, *n2 = NULL;
    const mcp_json_value_t *tn0 = mcp_json_object_get(ctx, t0, "name");
    const mcp_json_value_t *tn1 = mcp_json_object_get(ctx, t1, "name");
    const mcp_json_value_t *tn2 = mcp_json_object_get(ctx, t2, "name");
    CHECK(mcp_json_string_value(ctx, tn0, &n0) == MCP_OK);
    CHECK(mcp_json_string_value(ctx, tn1, &n1) == MCP_OK);
    CHECK(mcp_json_string_value(ctx, tn2, &n2) == MCP_OK);
    CHECK(strcmp(n0, "alpha") == 0);
    CHECK(strcmp(n1, "mid") == 0);
    CHECK(strcmp(n2, "zeta") == 0);
    mcp_message_destroy(ctx, resp_tl);
    mcp_message_destroy(ctx, req_tl);

    /* Verify 2026 default cache hint injection on tools/list (fresh id to
     * avoid duplicate_id on the same session). */
    mcp_message_t *req_tl2 = mcp_message_parse(ctx, "{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"tools/list\"}", strlen("{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"tools/list\"}"));
    CHECK(req_tl2 != NULL);
    mcp_message_t *resp_tl2 = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s_2026, req_tl2, &resp_tl2) == MCP_OK);
    CHECK(resp_tl2 != NULL);
    const mcp_json_value_t *res2 = mcp_message_result(ctx, resp_tl2);
    CHECK(res2 != NULL);
    const mcp_json_value_t *ttl = mcp_json_object_get(ctx, res2, "ttlMs");
    const mcp_json_value_t *scope = mcp_json_object_get(ctx, res2, "cacheScope");
    CHECK(ttl != NULL);
    double ttl_val = 0;
    CHECK(mcp_json_number_value(ctx, ttl, &ttl_val) == MCP_OK);
    CHECK(ttl_val == 0.0);
    const char *scope_str = NULL;
    CHECK(scope != NULL && mcp_json_string_value(ctx, scope, &scope_str) == MCP_OK);
    CHECK(strcmp(scope_str, "private") == 0);
    mcp_message_destroy(ctx, resp_tl2);
    mcp_message_destroy(ctx, req_tl2);

    /* Legacy session: no default cache hint (ttlMs/cacheScope must be absent). */
    mcp_message_t *req_tl3 = mcp_message_parse(ctx, "{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"tools/list\"}", strlen("{\"jsonrpc\":\"2.0\",\"id\":101,\"method\":\"tools/list\"}"));
    CHECK(req_tl3 != NULL);
    mcp_message_t *resp_tl3 = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s_legacy, req_tl3, &resp_tl3) == MCP_OK);
    CHECK(resp_tl3 != NULL);
    const mcp_json_value_t *res3 = mcp_message_result(ctx, resp_tl3);
    CHECK(res3 != NULL);
    CHECK(mcp_json_object_get(ctx, res3, "ttlMs") == NULL);
    CHECK(mcp_json_object_get(ctx, res3, "cacheScope") == NULL);
    mcp_message_destroy(ctx, resp_tl3);
    mcp_message_destroy(ctx, req_tl3);

    mcp_server_destroy_session(ctx, srv, s_2026);
    mcp_server_destroy_session(ctx, srv, s_legacy);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("test_proto_policy_matrix OK\n");
    return 0;
}
