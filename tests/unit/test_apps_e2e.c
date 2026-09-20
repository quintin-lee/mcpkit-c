#include "test_check.h"
#include <string.h>

#include "mcpkit/mcpkit.h"

static int mounted = 0;
static int unmounted = 0;

static void on_mount(mcp_context_t *c, mcp_session_t *s, void *u) {
    (void)c;
    (void)s;
    (void)u;
    mounted++;
}

static void on_unmount(mcp_context_t *c, mcp_session_t *s, void *u) {
    (void)c;
    (void)s;
    (void)u;
    unmounted++;
}

// Plain host never sees UI; Apps host gets _meta.ui attached by the handler.
static mcp_status_t status_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                                   void *u, mcp_json_value_t **o) {
    (void)a;
    (void)u;
    mcp_json_value_t *out = mcp_json_object_new(c);
    mcp_json_value_t *sv = mcp_json_string_new(c, "ok");
    if (out == NULL || sv == NULL || mcp_json_object_set(c, out, "status", sv) != MCP_OK) {
        mcp_json_destroy(c, sv);
        mcp_json_destroy(c, out);
        return MCP_ERR_NOMEM;
    }
    if (mcp_session_is_apps_host(c, s)) {
        if (mcp_apps_result_with_ui(c, out, "ui://sys/status") != MCP_OK) {
            mcp_json_destroy(c, out);
            return MCP_ERR_NOMEM;
        }
    }
    *o = out;
    return MCP_OK;
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
}

static mcp_json_value_t *call_params(mcp_context_t *ctx, const char *name) {
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    CHECK(p);
    CHECK(mcp_json_object_set(ctx, p, "name", mcp_json_string_new(ctx, name)) == MCP_OK);
    CHECK(mcp_json_object_set(ctx, p, "arguments", mcp_json_object_new(ctx)) == MCP_OK);
    return p;
}

static int error_code(mcp_context_t *ctx, const mcp_message_t *resp) {
    int code = 0;
    CHECK(mcp_message_error_code(ctx, resp, &code) == MCP_OK);
    return code;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx);

    mcp_server_t *srv = mcp_server_create(ctx, "sys", "1");
    CHECK(srv);
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    CHECK(schema);
    mcp_tool_t *tool = mcp_tool_new(ctx, "get_system_status", "status", schema, status_handler,
                                    NULL);
    CHECK(tool);
    CHECK(mcp_tool_set_visibility(ctx, tool, MCP_TOOL_VIS_BOTH) == MCP_OK);
    CHECK(mcp_tool_require_perms(ctx, tool, MCP_APPS_PERM_CALL_TOOL) == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);

    mcp_csp_t *csp = mcp_csp_default_deny_new(ctx);
    CHECK(csp);
    mcp_resource_t *ui = mcp_apps_ui_resource_new(ctx, "ui://sys/status", "status",
                                                  "<div>ok</div>", csp);
    CHECK(ui);
    CHECK(mcp_server_add_resource(ctx, srv, ui) == MCP_OK);

    // Plain host: denied by permission mask first (grants default NONE only if we revoke;
    // default is ALL, so revoke to simulate an unprivileged host).
    mcp_session_t *plain = mcp_server_create_session(ctx, srv);
    CHECK(plain);
    CHECK(mcp_session_revoke(ctx, plain, MCP_APPS_PERM_CALL_TOOL) == MCP_OK);
    init_session(ctx, srv, plain);
    mcp_message_t *r = dispatch_new(ctx, srv, plain, "p1", "tools/call",
                                    call_params(ctx, "get_system_status"));
    CHECK(error_code(ctx, r) == (int)MCP_RPC_INVALID_PARAMS);
    mcp_message_destroy(ctx, r);

    // Same host after grant: call succeeds, result carries no _meta.
    CHECK(mcp_session_grant(ctx, plain, MCP_APPS_PERM_CALL_TOOL) == MCP_OK);
    r = dispatch_new(ctx, srv, plain, "p2", "tools/call",
                     call_params(ctx, "get_system_status"));
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res);
    CHECK(mcp_json_object_get(ctx, res, "_meta") == NULL);
    const mcp_json_value_t *st = mcp_json_object_get(ctx, res, "status");
    const char *sts = NULL;
    CHECK(mcp_json_string_value(ctx, st, &sts) == MCP_OK && strcmp(sts, "ok") == 0);
    mcp_message_destroy(ctx, r);

    // Apps host: mount fires, call carries resourceUri, resource reads back HTML.
    mcp_session_t *app = mcp_server_create_session(ctx, srv);
    CHECK(app);
    CHECK(mcp_session_set_apps_host(ctx, app, true) == MCP_OK);
    CHECK(mcp_session_grant(ctx, app, MCP_APPS_PERM_CALL_TOOL) == MCP_OK);
    mcp_apps_mount_t *h = NULL;
    CHECK(mcp_apps_mount(ctx, app, on_mount, on_unmount, NULL, &h) == MCP_OK);
    CHECK(mounted == 1);
    init_session(ctx, srv, app);
    r = dispatch_new(ctx, srv, app, "a1", "tools/call",
                     call_params(ctx, "get_system_status"));
    res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *meta = mcp_json_object_get(ctx, res, "_meta");
    CHECK(meta);
    const mcp_json_value_t *mui = mcp_json_object_get(ctx, meta, "ui");
    CHECK(mui);
    const mcp_json_value_t *muri = mcp_json_object_get(ctx, mui, "resourceUri");
    const char *muris = NULL;
    CHECK(mcp_json_string_value(ctx, muri, &muris) == MCP_OK &&
           strcmp(muris, "ui://sys/status") == 0);
    mcp_message_destroy(ctx, r);

    mcp_json_value_t *rp = mcp_json_object_new(ctx);
    CHECK(rp && mcp_json_object_set(ctx, rp, "uri",
                                     mcp_json_string_new(ctx, "ui://sys/status")) == MCP_OK);
    r = dispatch_new(ctx, srv, app, "a2", "resources/read", rp);
    res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *contents = mcp_json_object_get(ctx, res, "contents");
    CHECK(contents && mcp_json_array_size(ctx, contents) == 1);
    const mcp_json_value_t *item = mcp_json_array_get(ctx, contents, 0);
    const mcp_json_value_t *text = mcp_json_object_get(ctx, item, "text");
    const char *ts = NULL;
    CHECK(mcp_json_string_value(ctx, text, &ts) == MCP_OK);
    CHECK(strstr(ts, "<meta http-equiv=\"Content-Security-Policy\"") == ts);
    CHECK(strstr(ts, "<div>ok</div>") != NULL);
    mcp_message_destroy(ctx, r);

    CHECK(mcp_apps_unmount(ctx, h) == MCP_OK);
    CHECK(unmounted == 1);

    mcp_csp_destroy(ctx, csp);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
