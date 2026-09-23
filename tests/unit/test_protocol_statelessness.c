/**
 * @file test_protocol_statelessness.c
 *
 * Protocol statelessness: _meta injection/extraction, resultType decoration,
 * ttlMs/cacheScope injection, session client_meta storage, and server/discover.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "test_check.h"

/* Minimal tool handler: returns an empty object on success. */
static mcp_status_t noop_handler(mcp_context_t *ctx, mcp_session_t *s,
                                 const mcp_json_value_t *args, void *u,
                                 mcp_json_value_t **out) {
    (void)s;
    (void)args;
    (void)u;
    *out = mcp_json_object_new(ctx);
    return *out != NULL ? MCP_OK : MCP_ERR_NOMEM;
}

static mcp_status_t sample_read(mcp_context_t *ctx, mcp_session_t *s,
                                const char *uri, void *ud, mcp_json_value_t **out) {
    (void)s;
    (void)ud;
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, item, "uri", mcp_json_string_new(ctx, uri));
    mcp_json_object_set_take(ctx, item, "text", mcp_json_string_new(ctx, "content"));
    mcp_json_array_append(ctx, arr, item);
    *out = arr;
    return MCP_OK;
}

/* init_session: dispatch initialize + notify so subsequent requests pass the gate. */
static void init_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    mcp_json_value_t *ip = mcp_initialize_params_new(ctx, "cli", "1");
    CHECK(ip);
    mcp_message_t *req = mcp_request_new_string_id(ctx, "init", "initialize", ip);
    CHECK(req);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    CHECK(resp);
    mcp_message_destroy(ctx, resp);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);
}

/* Dispatch a no-params request and return the caller-owned response message. */
static mcp_message_t *dispatch_req(mcp_context_t *ctx, mcp_server_t *srv,
                                    mcp_session_t *s, const char *id, const char *method) {
    mcp_message_t *req = mcp_request_new_string_id(ctx, id, method, NULL);
    CHECK(req);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    CHECK(resp);
    return resp;
}

/* case 1: mcp_message_meta returns _meta when present, NULL when absent.
 * _meta is a top-level key on the JSON-RPC envelope (sibling to params),
 * so we must use mcp_message_parse on a raw string, not the builder API. */
static void case_meta_access(mcp_context_t *ctx) {
    static const char kReqMeta[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"m1\",\"method\":\"initialize\","
        "\"params\":{},\"_meta\":{\"clientHint\":\"hello\"}}";
    mcp_message_t *req = mcp_message_parse(ctx, kReqMeta, sizeof(kReqMeta) - 1);
    CHECK(req);
    const mcp_json_value_t *meta = mcp_message_meta(ctx, req);
    CHECK(meta);
    CHECK(mcp_json_type(ctx, meta) == MCP_JSON_OBJECT);
    const mcp_json_value_t *hint = mcp_json_object_get(ctx, meta, "clientHint");
    CHECK(hint);
    const char *s = NULL;
    CHECK(mcp_json_string_value(ctx, hint, &s) == MCP_OK && s != NULL && strcmp(s, "hello") == 0);
    mcp_message_destroy(ctx, req);

    /* No _meta -> NULL */
    static const char kReqNoMeta[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"m2\",\"method\":\"initialize\",\"params\":{}}";
    mcp_message_t *req2 = mcp_message_parse(ctx, kReqNoMeta, sizeof(kReqNoMeta) - 1);
    CHECK(req2);
    CHECK(mcp_message_meta(ctx, req2) == NULL);
    mcp_message_destroy(ctx, req2);
}

/* case 2: mcp_result_inject_result_type adds resultType="complete". */
static void case_result_type_inject(mcp_context_t *ctx) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    CHECK(result);
    CHECK(mcp_result_inject_result_type(ctx, result) == MCP_OK);
    const mcp_json_value_t *rt = mcp_json_object_get(ctx, result, "resultType");
    CHECK(rt);
    const char *rs = NULL;
    CHECK(mcp_json_string_value(ctx, rt, &rs) == MCP_OK && rs != NULL && strcmp(rs, "complete") == 0);
    mcp_json_destroy(ctx, result);
    CHECK(mcp_result_inject_result_type(ctx, NULL) == MCP_ERR_INVALID_ARGUMENT);
}

/* case 3: mcp_result_inject_meta stores meta under _meta (set_take ownership). */
static void case_meta_inject(mcp_context_t *ctx) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    CHECK(result);
    mcp_json_value_t *meta = mcp_json_object_new(ctx);
    CHECK(meta);
    mcp_json_value_t *val = mcp_json_string_new(ctx, "x");
    CHECK(val);
    CHECK(mcp_json_object_set(ctx, meta, "k", val) == MCP_OK);
    CHECK(mcp_result_inject_meta(ctx, result, meta) == MCP_OK);
    /* meta is now owned by result */
    const mcp_json_value_t *injected = mcp_json_object_get(ctx, result, "_meta");
    CHECK(injected);
    CHECK(mcp_json_type(ctx, injected) == MCP_JSON_OBJECT);
    mcp_json_destroy(ctx, result);

    /* NULL meta -> INVALID_ARGUMENT */
    mcp_json_value_t *r2 = mcp_json_object_new(ctx);
    CHECK(r2);
    CHECK(mcp_result_inject_meta(ctx, r2, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_destroy(ctx, r2);
}

/* case 4: server/discover returns a full capability object. */
static void case_server_discover(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    init_session(ctx, srv, s);
    mcp_message_t *r = dispatch_req(ctx, srv, s, "disc", "server/discover");
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res);
    CHECK(mcp_json_type(ctx, res) == MCP_JSON_OBJECT);

    const char *sv = NULL;
    const mcp_json_value_t *sv_val = mcp_json_object_get(ctx, res, "serverName");
    CHECK(sv_val);
    CHECK(mcp_json_string_value(ctx, sv_val, &sv) == MCP_OK && sv != NULL && strcmp(sv, "srv") == 0);

    const mcp_json_value_t *pv = mcp_json_object_get(ctx, res, "protocolVersion");
    CHECK(pv);
    const char *pv_s = NULL;
    CHECK(mcp_json_string_value(ctx, pv, &pv_s) == MCP_OK &&
          pv_s != NULL && strcmp(pv_s, MCP_PROTOCOL_VERSION_LATEST) == 0);

    bool bv = false;
    const mcp_json_value_t *ss = mcp_json_object_get(ctx, res, "supportsStateless");
    CHECK(ss);
    CHECK(mcp_json_bool_value(ctx, ss, &bv) == MCP_OK && bv);
    const mcp_json_value_t *sm = mcp_json_object_get(ctx, res, "supportsMeta");
    CHECK(sm);
    CHECK(mcp_json_bool_value(ctx, sm, &bv) == MCP_OK && bv);

    /* resultType present (is_list=true path) */
    CHECK(mcp_json_object_has(ctx, res, "resultType"));
    mcp_message_destroy(ctx, r);
}

/* case 5: tools/list reflects list-cache config (ttlMs + cacheScope appear/disappear). */
static void case_list_cache_inject(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    init_session(ctx, srv, s);

    /* Without cache config: resultType yes, ttlMs/cacheScope no */
    mcp_message_t *r = dispatch_req(ctx, srv, s, "tl1", "tools/list");
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res);
    CHECK(mcp_json_object_has(ctx, res, "resultType"));
    CHECK(!mcp_json_object_has(ctx, res, "ttlMs"));
    CHECK(!mcp_json_object_has(ctx, res, "cacheScope"));
    mcp_message_destroy(ctx, r);

    /* With cache config: ttlMs + cacheScope appear */
    CHECK(mcp_server_set_list_cache(ctx, srv, 30000, "session") == MCP_OK);
    r = dispatch_req(ctx, srv, s, "tl2", "tools/list");
    res = mcp_message_result(ctx, r);
    CHECK(res);
    CHECK(mcp_json_object_has(ctx, res, "resultType"));
    CHECK(mcp_json_object_has(ctx, res, "ttlMs"));
    CHECK(mcp_json_object_has(ctx, res, "cacheScope"));
    double d = 0;
    const mcp_json_value_t *ttl = mcp_json_object_get(ctx, res, "ttlMs");
    CHECK(ttl);
    CHECK(mcp_json_number_value(ctx, ttl, &d) == MCP_OK && d == 30000.0);
    const char *cs_s = NULL;
    const mcp_json_value_t *cs = mcp_json_object_get(ctx, res, "cacheScope");
    CHECK(cs);
    CHECK(mcp_json_string_value(ctx, cs, &cs_s) == MCP_OK && cs_s != NULL &&
          strcmp(cs_s, "session") == 0);
    mcp_message_destroy(ctx, r);

    /* Test resources/templates/list has cache metadata */
    r = dispatch_req(ctx, srv, s, "rtl1", "resources/templates/list");
    res = mcp_message_result(ctx, r);
    CHECK(res);
    CHECK(mcp_json_object_has(ctx, res, "ttlMs"));
    CHECK(mcp_json_object_has(ctx, res, "cacheScope"));
    mcp_message_destroy(ctx, r);

    /* Test resources/read has cache metadata */
    mcp_json_value_t *read_p = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, read_p, "uri", mcp_json_string_new(ctx, "file:///res.txt"));
    mcp_message_t *rr_req = mcp_request_new_string_id(ctx, "rr1", "resources/read", read_p);
    mcp_message_t *rr_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, rr_req, &rr_resp) == MCP_OK && rr_resp != NULL);
    mcp_message_destroy(ctx, rr_req);
    const mcp_json_value_t *rr_res = mcp_message_result(ctx, rr_resp);
    CHECK(rr_res != NULL);
    CHECK(mcp_json_object_has(ctx, rr_res, "ttlMs"));
    CHECK(mcp_json_object_has(ctx, rr_res, "cacheScope"));
    mcp_message_destroy(ctx, rr_resp);

    /* Clear: ttlMs/cacheScope omitted again */
    CHECK(mcp_server_set_list_cache(ctx, srv, 0, NULL) == MCP_OK);
    r = dispatch_req(ctx, srv, s, "tl3", "tools/list");
    res = mcp_message_result(ctx, r);
    CHECK(res);
    CHECK(!mcp_json_object_has(ctx, res, "ttlMs"));
    CHECK(!mcp_json_object_has(ctx, res, "cacheScope"));
    mcp_message_destroy(ctx, r);
}

/* case 6: response_meta is injected as _meta in results when set. */
static void case_response_meta_inject(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    init_session(ctx, srv, s);

    mcp_json_value_t *meta = mcp_json_object_new(ctx);
    CHECK(meta);
    mcp_json_value_t *val = mcp_json_string_new(ctx, "v1");
    CHECK(val);
    CHECK(mcp_json_object_set(ctx, meta, "vendor", val) == MCP_OK);
    CHECK(mcp_server_set_response_meta(ctx, srv, meta) == MCP_OK);
    /* set_response_meta clones internally; caller may free original */
    mcp_json_destroy(ctx, meta);

    mcp_message_t *r = dispatch_req(ctx, srv, s, "tlm", "tools/list");
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res);
    const mcp_json_value_t *injected = mcp_json_object_get(ctx, res, "_meta");
    CHECK(injected);
    const mcp_json_value_t *vend = mcp_json_object_get(ctx, injected, "vendor");
    CHECK(vend);
    const char *vs = NULL;
    CHECK(mcp_json_string_value(ctx, vend, &vs) == MCP_OK && vs != NULL && strcmp(vs, "v1") == 0);
    mcp_message_destroy(ctx, r);

    /* Clear response_meta */
    CHECK(mcp_server_set_response_meta(ctx, srv, NULL) == MCP_OK);
    r = dispatch_req(ctx, srv, s, "tlm2", "tools/list");
    res = mcp_message_result(ctx, r);
    CHECK(res);
    CHECK(!mcp_json_object_has(ctx, res, "_meta"));
    mcp_message_destroy(ctx, r);
}

/* case 7: session stores the _meta from initialize.
 * Uses mcp_message_parse because _meta is a top-level envelope key. */
static void case_session_client_meta(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    static const char kReqMeta[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"init2\",\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"" MCP_PROTOCOL_VERSION_LATEST "\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"cli\",\"version\":\"1\"}},"
        "\"_meta\":{\"clientHint\":\"hello\"}}";
    mcp_message_t *req = mcp_message_parse(ctx, kReqMeta, sizeof(kReqMeta) - 1);
    CHECK(req);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    CHECK(resp);
    mcp_message_destroy(ctx, resp);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);

    const mcp_json_value_t *stored = mcp_session_client_meta(ctx, s);
    CHECK(stored);
    CHECK(mcp_json_type(ctx, stored) == MCP_JSON_OBJECT);
    const mcp_json_value_t *hint = mcp_json_object_get(ctx, stored, "clientHint");
    CHECK(hint);
    const char *s2 = NULL;
    CHECK(mcp_json_string_value(ctx, hint, &s2) == MCP_OK && s2 != NULL &&
          strcmp(s2, "hello") == 0);
}

/* case 8: ping response carries resultType but not ttlMs. */
static void case_ping_result_type(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    init_session(ctx, srv, s);
    mcp_message_t *r = dispatch_req(ctx, srv, s, "p1", "ping");
    const mcp_json_value_t *res = mcp_message_result(ctx, r);
    CHECK(res);
    CHECK(mcp_json_object_has(ctx, res, "resultType"));
    CHECK(!mcp_json_object_has(ctx, res, "ttlMs"));
    mcp_message_destroy(ctx, r);
}

/* case 9: server/discover and requests with _meta work on UNINITIALIZED session (MCP 2026-07-28 statelessness) */
static void case_uninitialized_discover_and_stateless(mcp_context_t *ctx, mcp_server_t *srv) {
    mcp_session_t *s_raw = mcp_server_create_session(ctx, srv);
    CHECK(s_raw);

    /* 1. server/discover probe succeeds without init */
    mcp_message_t *r = dispatch_req(ctx, srv, s_raw, "d1", "server/discover");
    CHECK(r);
    CHECK(mcp_message_result(ctx, r) != NULL);
    mcp_message_destroy(ctx, r);

    /* 2. stateless request with _meta succeeds without init */
    static const char kStatelessCall[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"st1\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"noop\",\"arguments\":{}},"
        "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}";
    mcp_message_t *req = mcp_message_parse(ctx, kStatelessCall, strlen(kStatelessCall));
    CHECK(req);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s_raw, req, &resp) == MCP_OK);
    mcp_message_destroy(ctx, req);
    CHECK(resp);
    CHECK(mcp_message_result(ctx, resp) != NULL);
    mcp_message_destroy(ctx, resp);

    /* 3. legacy request without _meta is rejected with 'session not initialized' */
    mcp_message_t *leg = dispatch_req(ctx, srv, s_raw, "leg1", "tools/list");
    CHECK(leg);
    int err_code = 0;
    CHECK(mcp_message_error_code(ctx, leg, &err_code) == MCP_OK);
    CHECK(err_code == MCP_RPC_INVALID_REQUEST);
    const char *err_txt = mcp_message_error_text(ctx, leg);
    CHECK(err_txt != NULL && strstr(err_txt, "session not initialized") != NULL);
    mcp_message_destroy(ctx, leg);

    mcp_server_destroy_session(ctx, srv, s_raw);
}

static void case_request_log_level(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s) {
    init_session(ctx, srv, s);
    static const char kDebugReq[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"lvl1\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"noop\",\"arguments\":{}},"
        "\"_meta\":{\"io.modelcontextprotocol/logLevel\":\"debug\"}}";
    mcp_message_t *req = mcp_message_parse(ctx, kDebugReq, strlen(kDebugReq));
    CHECK(req);
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK && resp != NULL);
    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);

    static const char kWarnReq[] =
        "{\"jsonrpc\":\"2.0\",\"id\":\"lvl2\",\"method\":\"tools/call\","
        "\"params\":{\"name\":\"noop\",\"arguments\":{}},"
        "\"_meta\":{\"io.modelcontextprotocol/logLevel\":\"warn\"}}";
    req = mcp_message_parse(ctx, kWarnReq, strlen(kWarnReq));
    CHECK(req);
    resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK && resp != NULL);
    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx);
    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1.0.0");
    CHECK(srv);
    mcp_tool_t *tool = mcp_tool_new(ctx, "noop", "no-op", NULL, noop_handler, NULL);
    CHECK(tool);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);
    mcp_resource_t *res = mcp_resource_new(ctx, "file:///res.txt", "res", "text/plain", sample_read, NULL);
    CHECK(res);
    CHECK(mcp_server_add_resource(ctx, srv, res) == MCP_OK);
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s);

    case_meta_access(ctx);
    case_result_type_inject(ctx);
    case_meta_inject(ctx);
    case_server_discover(ctx, srv, s);
    case_list_cache_inject(ctx, srv, s);
    case_response_meta_inject(ctx, srv, s);
    case_session_client_meta(ctx, srv, s);
    case_ping_result_type(ctx, srv, s);
    case_uninitialized_discover_and_stateless(ctx, srv);
    case_request_log_level(ctx, srv, s);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
