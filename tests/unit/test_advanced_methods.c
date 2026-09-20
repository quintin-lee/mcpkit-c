#include <string.h>

#include "mcpkit/mcpkit.h"
#include "test_check.h"

static char g_log[4096];
static size_t g_log_len;
static int g_warns;
static void mem_sink(mcp_log_level_t level, const char *msg, void *ud) {
    (void)ud;
    size_t n = strlen(msg);
    if (g_log_len + n < sizeof(g_log)) {
        memcpy(g_log + g_log_len, msg, n);
        g_log_len += n;
    }
    if (level == MCP_LOG_WARN) {
        g_warns++;
    }
}

static mcp_message_t *dispatch_new(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                   const char *id, const char *method,
                                   mcp_json_value_t *params) {
    mcp_message_t *req = mcp_request_new_string_id(ctx, id, method, params);
    CHECK(req != NULL);
    mcp_message_t *resp = NULL;
    mcp_status_t st = mcp_server_dispatch(ctx, srv, s, req, &resp);
    mcp_message_destroy(ctx, req);
    CHECK(st == MCP_OK || st == MCP_ERR_INVALID_ARGUMENT);
    return resp;
}

static mcp_json_value_t *obj_pair(mcp_context_t *ctx, const char *key, const char *val) {
    mcp_json_value_t *o = mcp_json_object_new(ctx);
    mcp_json_value_t *v = mcp_json_string_new(ctx, val);
    CHECK(o != NULL && v != NULL);
    CHECK(mcp_json_object_set(ctx, o, key, v) == MCP_OK);
    return o;
}

/* notifications/cancelled + notifications/progress are CONSUMED as notifications
 * (counter++, DEBUG log) — not routed as requests. */
static void t_notifications_consumed(mcp_context_t *ctx, mcp_server_t *srv,
                                     mcp_session_t *s) {
    mcp_server_counters_t c0;
    CHECK(mcp_server_counters(ctx, srv, &c0) == MCP_OK);
    mcp_message_t *n = mcp_notification_new(ctx, "notifications/cancelled",
                                             mcp_json_null_new(ctx));
    CHECK(n != NULL);
    CHECK(mcp_server_notify(ctx, srv, s, n) == MCP_OK);
    mcp_message_destroy(ctx, n);
    n = mcp_notification_new(ctx, "notifications/progress", mcp_json_null_new(ctx));
    CHECK(n != NULL);
    CHECK(mcp_server_notify(ctx, srv, s, n) == MCP_OK);
    mcp_message_destroy(ctx, n);
    mcp_server_counters_t c1;
    CHECK(mcp_server_counters(ctx, srv, &c1) == MCP_OK);
    CHECK(c1.notifications_total == c0.notifications_total + 2);
}

/* mcp_server_request_client pushes a number-id request into the outbox. */
static void t_request_client(mcp_context_t *ctx, mcp_server_t *srv) {
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    CHECK(params != NULL);
    CHECK(mcp_server_request_client(ctx, srv, "roots/list", params) == MCP_OK);
    mcp_message_t *out = NULL;
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_OK);
    CHECK(out != NULL);
    CHECK(mcp_message_kind(ctx, out) == MCP_MSG_REQUEST);
    const char *method = mcp_message_method(ctx, out);
    CHECK(method != NULL && strcmp(method, "roots/list") == 0);
    mcp_message_destroy(ctx, out);
    /* NULL params is valid (request with no params). */
    CHECK(mcp_server_request_client(ctx, srv, "sampling/createMessage", NULL) == MCP_OK);
    CHECK(mcp_server_outbox_pop(ctx, srv, &out) == MCP_OK);
    mcp_message_destroy(ctx, out);
    /* NULL guards. */
    CHECK(mcp_server_request_client(ctx, NULL, "roots/list", NULL) == MCP_ERR_INVALID_ARGUMENT);
}

int main(void) {
    mcp_logger_t *lg = mcp_logger_create(NULL, mem_sink, NULL);
    CHECK(lg != NULL);
    mcp_logger_set_level(lg, MCP_LOG_DEBUG);
    mcp_context_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.logger = lg;
    mcp_context_t *ctx = mcp_context_create(&cfg);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1");
    CHECK(srv != NULL);
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);

    // initialize + initialized so the session passes the gate.
    mcp_message_t *r = dispatch_new(ctx, srv, s, "init", "initialize",
                                    mcp_initialize_params_new(ctx, "cli", "1"));
    mcp_message_destroy(ctx, r);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif != NULL);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);

    // --- logging/setLevel happy: maps spec level strings, result is {} ---
    r = dispatch_new(ctx, srv, s, "s1", "logging/setLevel", obj_pair(ctx, "level", "warn"));
    CHECK(r != NULL);
    int ecode = 0;
    CHECK(mcp_message_error_code(ctx, r, &ecode) != MCP_OK); // no error on success
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res != NULL && mcp_json_type(ctx, res) == MCP_JSON_OBJECT);
    mcp_message_destroy(ctx, r);

    // --- logging/setLevel bad level -> -32602 ---
    r = dispatch_new(ctx, srv, s, "s2", "logging/setLevel", obj_pair(ctx, "level", "bogus"));
    CHECK(r != NULL);
    CHECK(mcp_message_error_code(ctx, r, &ecode) == MCP_OK);
    CHECK(ecode == (int)MCP_RPC_INVALID_PARAMS);
    mcp_message_destroy(ctx, r);

    // --- resources/subscribe dedup: two subscribes both OK ---
    r = dispatch_new(ctx, srv, s, "u1", "resources/subscribe", obj_pair(ctx, "uri", "res://a"));
    CHECK(r != NULL && mcp_message_error_code(ctx, r, &ecode) != MCP_OK);
    mcp_message_destroy(ctx, r);
    r = dispatch_new(ctx, srv, s, "u2", "resources/subscribe", obj_pair(ctx, "uri", "res://a"));
    CHECK(r != NULL && mcp_message_error_code(ctx, r, &ecode) != MCP_OK);
    mcp_message_destroy(ctx, r);
    // --- unsubscribe idempotent ---
    r = dispatch_new(ctx, srv, s, "u3", "resources/unsubscribe", obj_pair(ctx, "uri", "res://a"));
    CHECK(r != NULL && mcp_message_error_code(ctx, r, &ecode) != MCP_OK);
    mcp_message_destroy(ctx, r);

    // --- resources/templates/list: honest empty {templates: []} ---
    r = dispatch_new(ctx, srv, s, "t1", "resources/templates/list", NULL);
    CHECK(r != NULL);
    CHECK(mcp_message_error_code(ctx, r, &ecode) != MCP_OK);
    res = mcp_message_result(ctx, r);
    CHECK(res != NULL);
    const mcp_json_value_t *tpl = mcp_json_object_get(ctx, res, "templates");
    CHECK(tpl != NULL && mcp_json_type(ctx, tpl) == MCP_JSON_ARRAY);
    CHECK(mcp_json_array_size(ctx, tpl) == 0);
    mcp_message_destroy(ctx, r);

    // --- floor effect on dlogf: roots/list -> -32601 + unknown_method WARN
    //     (suppressed when floor is raised above WARN; restored at DEBUG). ---
    r = dispatch_new(ctx, srv, s, "s3", "logging/setLevel",
                     obj_pair(ctx, "level", "error")); // floor=ERROR
    mcp_message_destroy(ctx, r);
    int warns_at_error = g_warns;
    r = dispatch_new(ctx, srv, s, "q1", "roots/list", NULL); // WARN suppressed
    mcp_message_destroy(ctx, r);
    CHECK(g_warns == warns_at_error);
    r = dispatch_new(ctx, srv, s, "s4", "logging/setLevel",
                     obj_pair(ctx, "level", "debug")); // floor=DEBUG
    mcp_message_destroy(ctx, r);
    int warns_at_debug = g_warns;
    r = dispatch_new(ctx, srv, s, "q2", "roots/list", NULL); // WARN recorded
    mcp_message_destroy(ctx, r);
    CHECK(g_warns == warns_at_debug + 1);
    (void)warns_at_error;

    t_notifications_consumed(ctx, srv, s);
    t_request_client(ctx, srv);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    mcp_logger_destroy(lg);
    return 0;
}
