#include <assert.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static int lifecycle_order = 0;
static int mounted_at = 0;
static int unmounted_at = 0;

static void on_mount(mcp_context_t *c, mcp_session_t *s, void *u) {
    (void)c;
    (void)s;
    (void)u;
    mounted_at = ++lifecycle_order;
}

static void on_unmount(mcp_context_t *c, mcp_session_t *s, void *u) {
    (void)c;
    (void)s;
    (void)u;
    unmounted_at = ++lifecycle_order;
}

static mcp_message_t *dispatch_new(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                   const char *id, const char *method, mcp_json_value_t *params) {
    mcp_message_t *req = mcp_request_new_string_id(ctx, id, method, params);
    assert(req);
    mcp_message_t *resp = NULL;
    assert(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    assert(resp);
    return resp;
}

static void init_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    mcp_json_value_t *params = mcp_initialize_params_new(ctx, "cli", "1");
    assert(params);
    mcp_message_t *resp = dispatch_new(ctx, srv, s, "init", "initialize", params);
    assert(mcp_message_result(ctx, resp) != NULL);
    mcp_message_destroy(ctx, resp);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    assert(notif);
    assert(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);
}

static const char *contents_text(mcp_context_t *ctx, const mcp_message_t *resp) {
    const mcp_json_value_t *res = mcp_message_result(ctx, resp);
    assert(res);
    const mcp_json_value_t *contents = mcp_json_object_get(ctx, res, "contents");
    assert(contents && mcp_json_array_size(ctx, contents) == 1);
    const mcp_json_value_t *item = mcp_json_array_get(ctx, contents, 0);
    const mcp_json_value_t *mime = mcp_json_object_get(ctx, item, "mimeType");
    const char *ms = NULL;
    assert(mcp_json_string_value(ctx, mime, &ms) == MCP_OK);
    assert(strcmp(ms, MCP_APPS_UI_MIME) == 0);
    const mcp_json_value_t *text = mcp_json_object_get(ctx, item, "text");
    const char *ts = NULL;
    assert(mcp_json_string_value(ctx, text, &ts) == MCP_OK);
    return ts;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx);

    // CSP: default deny
    mcp_csp_t *csp = mcp_csp_default_deny_new(ctx);
    assert(csp);
    char *s = NULL;
    assert(mcp_csp_serialize(ctx, csp, &s) == MCP_OK);
    assert(strcmp(s, "default-src 'none'") == 0);
    mcp_json_free_string(ctx, s);
    s = NULL;
    assert(mcp_csp_serialize(ctx, NULL, &s) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_csp_serialize(ctx, csp, NULL) == MCP_ERR_INVALID_ARGUMENT);

    // CSP: setters, order, clear, unknown directive
    assert(mcp_csp_set(ctx, csp, "script-src", "'self'") == MCP_OK);
    assert(mcp_csp_set(ctx, csp, "style-src", "'self'") == MCP_OK);
    assert(mcp_csp_set(ctx, csp, "connect-src", "https:") == MCP_OK);
    assert(mcp_csp_serialize(ctx, csp, &s) == MCP_OK);
    assert(strcmp(s, "default-src 'none'; script-src 'self'; style-src 'self'; connect-src "
                     "https:") == 0);
    mcp_json_free_string(ctx, s);
    s = NULL;
    assert(mcp_csp_set(ctx, csp, "script-src", NULL) == MCP_OK);
    assert(mcp_csp_serialize(ctx, csp, &s) == MCP_OK);
    assert(strstr(s, "script-src") == NULL);
    mcp_json_free_string(ctx, s);
    s = NULL;
    assert(mcp_csp_set(ctx, csp, "frame-src", "x") == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_csp_set(ctx, NULL, "script-src", "x") == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_csp_set(ctx, csp, NULL, "x") == MCP_ERR_INVALID_ARGUMENT);
    mcp_csp_destroy(ctx, NULL);

    // UI resource: scheme gate
    assert(mcp_apps_ui_resource_new(ctx, "https://x/app", "app", "<p>hi</p>", csp) == NULL);
    assert(mcp_apps_ui_resource_new(ctx, NULL, "app", "<p>hi</p>", csp) == NULL);
    assert(mcp_apps_ui_resource_new(ctx, "ui://app/main", "app", NULL, csp) == NULL);

    // UI resource served through resources/read with CSP meta prepended
    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1");
    assert(srv);
    mcp_resource_t *ui = mcp_apps_ui_resource_new(ctx, "ui://app/main", "app", "<p>hi</p>", csp);
    assert(ui);
    assert(mcp_server_add_resource(ctx, srv, ui) == MCP_OK);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    assert(sess);
    init_session(ctx, srv, sess);
    mcp_json_value_t *p = mcp_json_object_new(ctx);
    assert(p && mcp_json_object_set(ctx, p, "uri", mcp_json_string_new(ctx, "ui://app/main")) ==
                   MCP_OK);
    mcp_message_t *r = dispatch_new(ctx, srv, sess, "r1", "resources/read", p);
    const char *text = contents_text(ctx, r);
    assert(strstr(text, "<meta http-equiv=\"Content-Security-Policy\" content=\"") == text);
    assert(strstr(text, "default-src 'none'") != NULL);
    assert(strstr(text, "<p>hi</p>") != NULL);
    mcp_message_destroy(ctx, r);

    // html already carrying a policy passes through untouched
    mcp_resource_t *ui2 = mcp_apps_ui_resource_new(ctx, "ui://app/raw", "raw",
                                                   "<meta http-equiv=\"Content-Security-Policy\" "
                                                   "content=\"x\"><p>raw</p>",
                                                   NULL);
    assert(ui2);
    assert(mcp_server_add_resource(ctx, srv, ui2) == MCP_OK);
    p = mcp_json_object_new(ctx);
    assert(p && mcp_json_object_set(ctx, p, "uri", mcp_json_string_new(ctx, "ui://app/raw")) ==
                   MCP_OK);
    r = dispatch_new(ctx, srv, sess, "r2", "resources/read", p);
    text = contents_text(ctx, r);
    assert(strcmp(text, "<meta http-equiv=\"Content-Security-Policy\" content=\"x\"><p>raw</p>") ==
           0);
    mcp_message_destroy(ctx, r);

    // result wrap
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    assert(result && mcp_json_object_set(ctx, result, "content", mcp_json_array_new(ctx)) ==
                         MCP_OK);
    assert(mcp_apps_result_with_ui(ctx, result, "ui://app/main") == MCP_OK);
    const mcp_json_value_t *meta = mcp_json_object_get(ctx, result, "_meta");
    assert(meta);
    const mcp_json_value_t *mui = mcp_json_object_get(ctx, meta, "ui");
    assert(mui);
    const mcp_json_value_t *muri = mcp_json_object_get(ctx, mui, "resourceUri");
    const char *muris = NULL;
    assert(mcp_json_string_value(ctx, muri, &muris) == MCP_OK &&
           strcmp(muris, "ui://app/main") == 0);
    mcp_json_destroy(ctx, result);
    assert(mcp_apps_result_with_ui(ctx, NULL, "ui://x") == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_value_t *notobj = mcp_json_array_new(ctx);
    assert(mcp_apps_result_with_ui(ctx, notobj, "ui://x") == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_destroy(ctx, notobj);
    result = mcp_json_object_new(ctx);
    assert(result);
    assert(mcp_apps_result_with_ui(ctx, result, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_destroy(ctx, result);

    // lifecycle: explicit mount/unmount ordering
    mcp_apps_mount_t *h = NULL;
    assert(mcp_apps_mount(ctx, NULL, on_mount, on_unmount, NULL, &h) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_apps_mount(ctx, sess, on_mount, on_unmount, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_apps_mount(ctx, sess, on_mount, on_unmount, NULL, &h) == MCP_OK);
    assert(h && mounted_at == 1 && unmounted_at == 0);
    assert(mcp_apps_unmount(ctx, h) == MCP_OK);
    assert(unmounted_at == 2);
    assert(mcp_apps_unmount(ctx, NULL) == MCP_ERR_INVALID_ARGUMENT);

    mcp_csp_destroy(ctx, csp);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
