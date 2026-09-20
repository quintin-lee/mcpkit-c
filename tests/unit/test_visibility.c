#include "test_check.h"
#include <string.h>

#include "mcpkit/mcpkit.h"

static mcp_status_t echo_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                                 void *u, mcp_json_value_t **o) {
    (void)s;
    (void)a;
    (void)u;
    *o = mcp_json_object_new(c);
    return *o ? MCP_OK : MCP_ERR_NOMEM;
}

static int error_code(mcp_context_t *ctx, const mcp_message_t *resp) {
    int code = 0;
    CHECK(mcp_message_error_code(ctx, resp, &code) == MCP_OK);
    return code;
}

static mcp_message_t *dispatch_new(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                   const char *id, const char *method, mcp_json_value_t *params) {
    mcp_message_t *req = mcp_request_new_string_id(ctx, id, method, params);
    CHECK(req);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    CHECK(resp);
    return resp;
}

static size_t list_size(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s, const char *id) {
    mcp_message_t *r = dispatch_new(ctx, srv, s, id, "tools/list", NULL);
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *tools = mcp_json_object_get(ctx, res, "tools");
    CHECK(tools);
    size_t n = mcp_json_array_size(ctx, tools);
    mcp_message_destroy(ctx, r);
    return n;
}

static bool list_has(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s, const char *id,
                     const char *want) {
    mcp_message_t *r = dispatch_new(ctx, srv, s, id, "tools/list", NULL);
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *tools = mcp_json_object_get(ctx, res, "tools");
    CHECK(tools);
    bool found = false;
    for (size_t i = 0; i < mcp_json_array_size(ctx, tools); i++) {
        const mcp_json_value_t *t = mcp_json_array_get(ctx, tools, i);
        const mcp_json_value_t *n = mcp_json_object_get(ctx, t, "name");
        const char *ns = NULL;
        CHECK(mcp_json_string_value(ctx, n, &ns) == MCP_OK);
        if (strcmp(ns, want) == 0) {
            found = true;
        }
    }
    mcp_message_destroy(ctx, r);
    return found;
}

static mcp_json_value_t *call_params(mcp_context_t *ctx, const char *name) {
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    mcp_json_value_t *n = mcp_json_string_new(ctx, name);
    if (!p || !n || mcp_json_object_set(ctx, p, "name", n) != MCP_OK) {
        mcp_json_destroy(ctx, n);
        mcp_json_destroy(ctx, p);
        return NULL;
    }
    return p;
}

static void init_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    mcp_json_value_t *params = mcp_initialize_params_new(ctx, "cli", "1");
    CHECK(params);
    mcp_message_t *resp = dispatch_new(ctx, srv, s, "init", "initialize", params);
    CHECK(mcp_message_result(ctx, resp) != NULL);
    mcp_message_destroy(ctx, resp);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);
    CHECK(mcp_session_is_initialized(ctx, s));
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx);
    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1");
    CHECK(srv);

    mcp_tool_t *echo = mcp_tool_new(ctx, "echo", NULL, NULL, echo_handler, NULL);
    CHECK(echo);
    mcp_tool_t *app_only = mcp_tool_new(ctx, "app_only", NULL, NULL, echo_handler, NULL);
    CHECK(app_only);
    mcp_tool_t *model_only = mcp_tool_new(ctx, "model_only", NULL, NULL, echo_handler, NULL);
    CHECK(model_only);
    CHECK(mcp_tool_set_visibility(ctx, app_only, MCP_TOOL_VIS_APP) == MCP_OK);
    CHECK(mcp_tool_set_visibility(ctx, model_only, MCP_TOOL_VIS_MODEL) == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv, echo) == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv, app_only) == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv, model_only) == MCP_OK);

    mcp_session_t *plain = mcp_server_create_session(ctx, srv);
    mcp_session_t *apps = mcp_server_create_session(ctx, srv);
    CHECK(plain && apps);
    init_session(ctx, srv, plain);
    init_session(ctx, srv, apps);
    CHECK(mcp_session_set_apps_host(ctx, apps, true) == MCP_OK);
    CHECK(!mcp_session_is_apps_host(ctx, plain));
    CHECK(mcp_session_is_apps_host(ctx, apps));

    // default visibility: echo visible to both
    CHECK(list_size(ctx, srv, plain, "l1") == 2);
    CHECK(list_size(ctx, srv, apps, "l2") == 2);
    CHECK(list_has(ctx, srv, plain, "l3", "echo"));
    CHECK(list_has(ctx, srv, plain, "l4", "model_only"));
    CHECK(!list_has(ctx, srv, plain, "l5", "app_only"));
    CHECK(list_has(ctx, srv, apps, "l6", "app_only"));
    CHECK(!list_has(ctx, srv, apps, "l7", "model_only"));

    // hidden tool call -> -32601 (no existence leak)
    mcp_message_t *r = dispatch_new(ctx, srv, plain, "c1", "tools/call",
                                    call_params(ctx, "app_only"));
    CHECK(error_code(ctx, r) == (int)MCP_RPC_METHOD_NOT_FOUND);
    mcp_message_destroy(ctx, r);
    r = dispatch_new(ctx, srv, apps, "c2", "tools/call", call_params(ctx, "model_only"));
    CHECK(error_code(ctx, r) == (int)MCP_RPC_METHOD_NOT_FOUND);
    mcp_message_destroy(ctx, r);

    // permission gating with raw mask (server test stays apps-free)
    CHECK(mcp_tool_require_perms(ctx, echo, 0x01u) == MCP_OK);
    r = dispatch_new(ctx, srv, plain, "c3", "tools/call", call_params(ctx, "echo"));
    CHECK(mcp_message_result(ctx, r) != NULL);
    mcp_message_destroy(ctx, r);
    CHECK(mcp_session_revoke(ctx, plain, 0x01u) == MCP_OK);
    CHECK(!mcp_session_grants(ctx, plain, 0x01u));
    r = dispatch_new(ctx, srv, plain, "c4", "tools/call", call_params(ctx, "echo"));
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_PARAMS);
    mcp_message_destroy(ctx, r);
    CHECK(mcp_session_grant(ctx, plain, 0x01u) == MCP_OK);
    CHECK(mcp_session_grants(ctx, plain, 0x01u));
    r = dispatch_new(ctx, srv, plain, "c5", "tools/call", call_params(ctx, "echo"));
    CHECK(mcp_message_result(ctx, r) != NULL);
    mcp_message_destroy(ctx, r);

    // NULL / range guards
    CHECK(mcp_tool_set_visibility(ctx, NULL, MCP_TOOL_VIS_APP) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_tool_set_visibility(NULL, echo, MCP_TOOL_VIS_APP) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_tool_set_visibility(ctx, echo, (mcp_tool_visibility_t)99) ==
           MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_tool_require_perms(ctx, NULL, 1) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_session_set_apps_host(ctx, NULL, true) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(!mcp_session_is_apps_host(ctx, NULL));
    CHECK(mcp_session_grant(ctx, NULL, 1) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_session_revoke(ctx, NULL, 1) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(!mcp_session_grants(ctx, NULL, 1));

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
