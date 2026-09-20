#include <string.h>

#include "mcpkit/mcpkit.h"
#include "test_check.h"

static char g_log[2048];
static size_t g_log_len;
static void mem_sink(mcp_log_level_t level, const char *msg, void *ud) {
    (void)level;
    (void)ud;
    size_t n = strlen(msg);
    if (g_log_len + n < sizeof(g_log)) {
        memcpy(g_log + g_log_len, msg, n);
        g_log_len += n;
    }
}

#define MAX_EVENTS 16
static struct {
    mcp_trace_event_t ev;
    char method[64];
    int status;
    uint64_t duration_ns;
} g_events[MAX_EVENTS];
static size_t g_nev;
static void recorder(mcp_context_t *ctx, mcp_trace_event_t ev, const char *method,
                     int status, uint64_t duration_ns, void *ud) {
    (void)ctx;
    (void)ud;
    if (g_nev < MAX_EVENTS) {
        g_events[g_nev].ev = ev;
        strncpy(g_events[g_nev].method, method != NULL ? method : "",
                sizeof(g_events[g_nev].method) - 1);
        g_events[g_nev].method[sizeof(g_events[g_nev].method) - 1] = '\0';
        g_events[g_nev].status = status;
        g_events[g_nev].duration_ns = duration_ns;
        g_nev++;
    }
}

static mcp_status_t ok_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                               void *u, mcp_json_value_t **o) {
    (void)s;
    (void)a;
    (void)u;
    *o = mcp_json_object_new(c);
    return *o != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

static mcp_message_t *dispatch_new(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                   const char *id, const char *method,
                                   mcp_json_value_t *params) {
    mcp_message_t *req = mcp_request_new_string_id(ctx, id, method, params);
    CHECK(req != NULL);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    CHECK(resp != NULL);
    return resp;
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
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "ok", NULL, NULL, ok_handler,
                                                     NULL)) == MCP_OK);
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);

    // Zero snapshot before any traffic.
    mcp_server_counters_t c;
    CHECK(mcp_server_counters(ctx, srv, &c) == MCP_OK);
    CHECK(c.requests_total == 0 && c.requests_error == 0 && c.notifications_total == 0 &&
          c.tools_called == 0);
    CHECK(mcp_server_counters(ctx, NULL, &c) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_server_counters(ctx, srv, NULL) == MCP_ERR_INVALID_ARGUMENT);

    // initialize + initialized notification.
    mcp_message_t *r =
        dispatch_new(ctx, srv, s, "init", "initialize",
                     mcp_initialize_params_new(ctx, "cli", "1"));
    mcp_message_destroy(ctx, r);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif != NULL);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);

    // ping ok, tools/call ok, L2-rejected method, known-but-unrouted method.
    r = dispatch_new(ctx, srv, s, "p1", "ping", NULL);
    mcp_message_destroy(ctx, r);
    mcp_json_value_t *cp = mcp_json_object_new(ctx);
    mcp_json_value_t *nv = mcp_json_string_new(ctx, "ok");
    CHECK(cp != NULL && nv != NULL && mcp_json_object_set(ctx, cp, "name", nv) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "c1", "tools/call", cp);
    mcp_message_destroy(ctx, r);
    r = dispatch_new(ctx, srv, s, "u1", "nope/missing", NULL);
    mcp_message_destroy(ctx, r);
    r = dispatch_new(ctx, srv, s, "u1b", "roots/list", NULL);
    int ecode = 0;
    CHECK(mcp_message_error_code(ctx, r, &ecode) == MCP_OK);
    CHECK(ecode == (int)MCP_RPC_METHOD_NOT_FOUND);
    mcp_message_destroy(ctx, r);

    CHECK(mcp_server_counters(ctx, srv, &c) == MCP_OK);
    CHECK(c.requests_total == 5);
    CHECK(c.requests_error == 2);
    CHECK(c.notifications_total == 1);
    CHECK(c.tools_called == 1);

    // Error paths log flat k=v events.
    CHECK(strstr(g_log, "event=invalid_request") != NULL);
    CHECK(strstr(g_log, "event=unknown_method method=roots/list") != NULL);

    // Tracer: BEGIN/END pair per routed request.
    CHECK(mcp_server_set_tracer(ctx, srv, recorder, NULL) == MCP_OK);
    CHECK(mcp_server_set_tracer(ctx, NULL, recorder, NULL) == MCP_ERR_INVALID_ARGUMENT);
    r = dispatch_new(ctx, srv, s, "p2", "ping", NULL);
    mcp_message_destroy(ctx, r);
    CHECK(g_nev == 2);
    CHECK(g_events[0].ev == MCP_TRACE_BEGIN && strcmp(g_events[0].method, "ping") == 0 &&
          g_events[0].status == 0 && g_events[0].duration_ns == 0);
    CHECK(g_events[1].ev == MCP_TRACE_END && strcmp(g_events[1].method, "ping") == 0 &&
          g_events[1].status == 0);
    r = dispatch_new(ctx, srv, s, "u2", "roots/list", NULL);
    mcp_message_destroy(ctx, r);
    CHECK(g_nev == 4);
    CHECK(g_events[2].ev == MCP_TRACE_BEGIN && strcmp(g_events[2].method, "roots/list") == 0);
    CHECK(g_events[3].ev == MCP_TRACE_END && g_events[3].status == (int)MCP_RPC_METHOD_NOT_FOUND);

    // Removing the tracer silences events; counters keep accumulating.
    CHECK(mcp_server_set_tracer(ctx, srv, NULL, NULL) == MCP_OK);
    r = dispatch_new(ctx, srv, s, "p3", "ping", NULL);
    mcp_message_destroy(ctx, r);
    CHECK(g_nev == 4);
    CHECK(mcp_server_counters(ctx, srv, &c) == MCP_OK);
    CHECK(c.requests_total == 8 && c.requests_error == 3);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    mcp_logger_destroy(lg);
    return 0;
}
