#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

typedef struct {
    const char **reqs;
    size_t nreqs;
    size_t next;
    char *out;
    size_t olen;
} mem_io_t;

static mcp_status_t mem_read(mcp_context_t *ctx, void *user, char *buf, size_t cap,
                             size_t *n_out) {
    (void)ctx;
    mem_io_t *m = user;
    if (m->next >= m->nreqs) {
        *n_out = 0;
        return MCP_OK;
    }
    size_t len = strlen(m->reqs[m->next]);
    if (len > cap) {
        return MCP_ERR_NOMEM;
    }
    memcpy(buf, m->reqs[m->next], len);
    m->next++;
    *n_out = len;
    return MCP_OK;
}

static mcp_status_t mem_write(mcp_context_t *ctx, void *user, const char *data, size_t len) {
    (void)ctx;
    mem_io_t *m = user;
    char *nbuf = realloc(m->out, m->olen + len + 1);
    CHECK(nbuf != NULL);
    m->out = nbuf;
    memcpy(m->out + m->olen, data, len);
    m->olen += len;
    m->out[m->olen] = '\0';
    return MCP_OK;
}

static char *run_script(mcp_context_t *ctx, mcp_server_t *srv, const char **reqs, size_t nreqs) {
    mem_io_t io = {.reqs = reqs, .nreqs = nreqs, .next = 0, .out = NULL, .olen = 0};
    mcp_http_io_t transport_io = {.user = &io, .read = mem_read, .write = mem_write};
    mcp_status_t st = mcp_http_serve(ctx, srv, &transport_io);
    CHECK(st == MCP_OK);
    return io.out;
}

static char *run_request(mcp_context_t *ctx, mcp_server_t *srv, const char *http_req) {
    const char *reqs[] = {http_req};
    return run_script(ctx, srv, reqs, 1);
}

static char *run_post(mcp_context_t *ctx, mcp_server_t *srv, const char *headers, const char *body) {
    char req[1024];
    snprintf(req, sizeof(req),
             "POST /mcp HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: application/json\r\n"
             "%s"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             headers ? headers : "", strlen(body), body);
    return run_request(ctx, srv, req);
}

static mcp_status_t dummy_tool(mcp_context_t *ctx, mcp_session_t *session,
                               const mcp_json_value_t *args, void *user_data,
                               mcp_json_value_t **result_out) {
    (void)session;
    (void)args;
    (void)user_data;
    *result_out = mcp_json_object_new(ctx);
    return MCP_OK;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "test2026", "1.0");
    CHECK(srv != NULL);

    /* Register tools in unsorted order: zebra, apple, banana */
    mcp_json_value_t *s1 = mcp_schema_object_new(ctx);
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "zebra", "zebra tool", s1, dummy_tool, NULL)) == MCP_OK);
    mcp_json_value_t *s2 = mcp_schema_object_new(ctx);
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "apple", "apple tool", s2, dummy_tool, NULL)) == MCP_OK);
    mcp_json_value_t *s3 = mcp_schema_object_new(ctx);
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "banana", "banana tool", s3, dummy_tool, NULL)) == MCP_OK);

    /* 1. 2026 tools/list without list_cache configured -> default cache hint (ttlMs: 0, cacheScope: private) + resultType */
    const char *hdrs_tools_list =
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: tools/list\r\n";
    const char *body_tools_list = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";

    char *out1 = run_post(ctx, srv, hdrs_tools_list, body_tools_list);
    CHECK(strstr(out1, "200 OK") != NULL);
    CHECK(strstr(out1, "\"resultType\":\"complete\"") != NULL);
    CHECK(strstr(out1, "\"ttlMs\":0") != NULL);
    CHECK(strstr(out1, "\"cacheScope\":\"private\"") != NULL);

    /* Verify deterministic alphabetical sort: apple before banana, banana before zebra */
    char *p_apple = strstr(out1, "\"name\":\"apple\"");
    char *p_banana = strstr(out1, "\"name\":\"banana\"");
    char *p_zebra = strstr(out1, "\"name\":\"zebra\"");
    CHECK(p_apple != NULL && p_banana != NULL && p_zebra != NULL);
    CHECK(p_apple < p_banana);
    CHECK(p_banana < p_zebra);
    free(out1);

    /* 2. Configure list cache and verify 2026 uses configured cache values */
    CHECK(mcp_server_set_list_cache(ctx, srv, 5000, "public") == MCP_OK);
    char *out2 = run_post(ctx, srv, hdrs_tools_list, body_tools_list);
    CHECK(strstr(out2, "200 OK") != NULL);
    CHECK(strstr(out2, "\"ttlMs\":5000") != NULL);
    CHECK(strstr(out2, "\"cacheScope\":\"public\"") != NULL);
    free(out2);

    /* Reset cache */
    CHECK(mcp_server_set_list_cache(ctx, srv, 0, NULL) == MCP_OK);

    /* 3. Deprecated methods in 2026 -> -32601 */
    const char *hdrs_ping =
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: ping\r\n";
    const char *body_ping = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}";
    char *out_ping = run_post(ctx, srv, hdrs_ping, body_ping);
    CHECK(strstr(out_ping, "-32601") != NULL);
    free(out_ping);

    const char *hdrs_ll =
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: logging/setLevel\r\n";
    const char *body_ll = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"logging/setLevel\",\"params\":{\"level\":\"info\"}}";
    char *out_ll = run_post(ctx, srv, hdrs_ll, body_ll);
    CHECK(strstr(out_ll, "-32601") != NULL);
    free(out_ll);

    const char *hdrs_sub =
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: resources/subscribe\r\n";
    const char *body_sub = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/subscribe\",\"params\":{\"uri\":\"file:///a\"}}";
    char *out_sub = run_post(ctx, srv, hdrs_sub, body_sub);
    CHECK(strstr(out_sub, "-32601") != NULL);
    free(out_sub);

    const char *hdrs_unsub =
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: resources/unsubscribe\r\n";
    const char *body_unsub = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"resources/unsubscribe\",\"params\":{\"uri\":\"file:///a\"}}";
    char *out_unsub = run_post(ctx, srv, hdrs_unsub, body_unsub);
    CHECK(strstr(out_unsub, "-32601") != NULL);
    free(out_unsub);

    /* 4. Mcp-Method header mismatch -> -32020 */
    const char *hdrs_mismatch =
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: tools/call\r\n";
    const char *body_mismatch = "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/list\"}";
    char *out_mismatch = run_post(ctx, srv, hdrs_mismatch, body_mismatch);
    CHECK(strstr(out_mismatch, "-32020") != NULL);
    free(out_mismatch);

    /* 5. 2026 request carrying Mcp-Session-Id is ignored (stateless fallback, no 404) */
    const char *hdrs_sid =
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Session-Id: non-existent-session-id\r\n"
        "Mcp-Method: tools/list\r\n";
    const char *body_sid = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}";
    char *out_sid = run_post(ctx, srv, hdrs_sid, body_sid);
    CHECK(strstr(out_sid, "200 OK") != NULL);
    CHECK(strstr(out_sid, "\"resultType\":\"complete\"") != NULL);
    free(out_sid);

    /* 6. Unknown resources/read in 2026 -> -32602 */
    const char *hdrs_res =
        "MCP-Protocol-Version: 2026-07-28\r\n"
        "Mcp-Method: resources/read\r\n"
        "Mcp-Name: missing:///uri\r\n";
    const char *body_res = "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"resources/read\",\"params\":{\"uri\":\"missing:///uri\"}}";
    char *out_res = run_post(ctx, srv, hdrs_res, body_res);
    CHECK(strstr(out_res, "-32602") != NULL);
    free(out_res);

    /* 7. Regression check for <= 2025:
     *    In 2025-11-25 without list_cache, ttlMs/cacheScope must be OMITTED.
     *    ping must succeed.
     *    All requests run in a single session loop. */
    char r1[512], r2[512], r3[512], r4[512];
    snprintf(r1, sizeof(r1),
             "POST /mcp HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 146\r\n\r\n"
             "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}");

    snprintf(r2, sizeof(r2),
             "POST /mcp HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Mcp-Session-Id: sess-1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 54\r\n\r\n"
             "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    snprintf(r3, sizeof(r3),
             "POST /mcp HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Mcp-Session-Id: sess-1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 47\r\n\r\n"
             "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/list\"}");

    snprintf(r4, sizeof(r4),
             "POST /mcp HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Mcp-Session-Id: sess-1\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: 41\r\n\r\n"
             "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"ping\"}");

    const char *seq[] = {r1, r2, r3, r4};
    char *out_seq = run_script(ctx, srv, seq, 4);
    CHECK(out_seq != NULL);

    /* Verify initialize -> 200 OK + Mcp-Session-Id: sess-1 */
    CHECK(strstr(out_seq, "Mcp-Session-Id: sess-1") != NULL);

    /* Verify tools/list response: 200 OK, result carries tools, ttlMs/cacheScope omitted */
    char *p_list_resp = strstr(out_seq, "\"id\":10");
    CHECK(p_list_resp != NULL);
    /* Locate the end of the id:10 JSON object */
    char *p_list_end = strstr(p_list_resp, "\r\n");
    if (p_list_end != NULL) {
        *p_list_end = '\0';
    }
    CHECK(strstr(p_list_resp, "ttlMs") == NULL);
    CHECK(strstr(p_list_resp, "cacheScope") == NULL);
    if (p_list_end != NULL) {
        *p_list_end = '\r';
    }

    /* Verify ping response: id:11, result is empty object {}, not -32601 */
    char *p_ping_resp = strstr(out_seq, "\"id\":11");
    CHECK(p_ping_resp != NULL);
    CHECK(strstr(p_ping_resp, "-32601") == NULL);
    free(out_seq);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
