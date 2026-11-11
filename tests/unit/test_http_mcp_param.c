/**
 * @file test_http_mcp_param.c
 * @brief P4-a T3: SEP-2243 Mcp-Param-* header consistency check (S2).
 *
 * HTTP-gated test (MCPKIT_BUILD_HTTP). Validates that on a 2026-07-28
 * Streamable-HTTP request the server walks the tool's inputSchema for
 * x-mcp-header annotations, reads the corresponding Mcp-Param-* headers,
 * decodes base64 sentinels, and rejects mismatches with HTTP 400 +
 * JSON-RPC -32001 (MCP_RPC_XMCP_PARAM_MISMATCH).
 *
 * Zero-regression cases:
 *  - 2025-11-25 / 2025-06-18 requests: Mcp-Param-* headers are NOT checked
 *  - Tools without x-mcp-header annotations: no header check at all
 */
#include "test_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/value.h"

/* Echo handler that accepts arbitrary arguments and returns them. */
static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (args != NULL) {
        const char *s = NULL;
        const mcp_json_value_t *region = mcp_json_object_get(ctx, args, "region");
        if (region != NULL) {
            if (mcp_json_string_value(ctx, region, &s) == MCP_OK && s != NULL) {
                mcp_json_value_t *sv = mcp_json_string_new(ctx, s);
                if (sv != NULL) {
                    if (mcp_json_object_set(ctx, result, "region", sv) == MCP_OK) {
                        /* sv ownership transferred */
                    } else {
                        mcp_json_destroy(ctx, sv);
                    }
                }
            }
        }
    }
    *result_out = result;
    return MCP_OK;
}

/* mem_io transport (same pattern as test_http_serve.c) */
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

/* Build a server with a tool that has region param annotated with x-mcp-header */
static mcp_server_t *make_server_xmcp(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "xmcp_srv", "0.1.0");
    CHECK(srv != NULL);
    /* Schema: region param is string type with x-mcp-header: "Region" */
    const char *schema_json =
        "{\"type\":\"object\","
        "\"properties\":{"
        "  \"region\":{\"type\":\"string\",\"x-mcp-header\":\"Region\"},"
        "  \"count\":{\"type\":\"integer\"}"
        "},"
        "\"required\":[\"region\"]}";
    mcp_json_value_t *schema = mcp_json_parse(ctx, schema_json, strlen(schema_json));
    CHECK(schema != NULL);
    mcp_tool_t *tool = mcp_tool_new(ctx, "echo_xmcp", "echo", schema, echo_handler, NULL);
    CHECK(tool != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);
    return srv;
}

/* Build a server with a tool that has NO x-mcp-header annotations */
static mcp_server_t *make_server_plain(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "plain_srv", "0.1.0");
    CHECK(srv != NULL);
    const char *schema_json =
        "{\"type\":\"object\","
        "\"properties\":{\"region\":{\"type\":\"string\"}},"
        "\"required\":[\"region\"]}";
    mcp_json_value_t *schema = mcp_json_parse(ctx, schema_json, strlen(schema_json));
    CHECK(schema != NULL);
    mcp_tool_t *tool = mcp_tool_new(ctx, "echo_plain", "echo", schema, echo_handler, NULL);
    CHECK(tool != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, tool) == MCP_OK);
    return srv;
}

static char *run_script(mcp_context_t *ctx, mcp_server_t *srv, const char **reqs, size_t n,
                        mcp_status_t *st_out) {
    mem_io_t m;
    memset(&m, 0, sizeof(m));
    m.reqs = reqs;
    m.nreqs = n;
    mcp_http_io_t io;
    io.user = &m;
    io.read = mem_read;
    io.write = mem_write;
    *st_out = mcp_http_serve(ctx, srv, &io);
    return m.out;
}

/* Build a 2026-07-28 stateless tools/call POST with the given body and extra
   HTTP headers (Mcp-Param-* etc). */
static char *make_req_2026(const char *body, const char *extra_hdrs,
                            char *buf, size_t cap) {
    const char *sep = (extra_hdrs != NULL && strstr(extra_hdrs, "\r\n\r\n") != NULL) ? "" : "\r\n";
    int n = snprintf(buf, cap,
                    "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                    "Content-Type: application/json\r\n"
                    "MCP-Protocol-Version: 2026-07-28\r\n"
                    "Mcp-Method: tools/call\r\nMcp-Name: echo_xmcp\r\n"
                    "%s%s%s",
                    strlen(body), extra_hdrs ? extra_hdrs : "", sep, body);
    CHECK(n > 0 && (size_t)n < cap);
    return buf;
}

/* Build a 2025-11-25 tools/call POST (no SEP-2243 gate). */
static char *make_req_2025(const char *body, const char *extra_hdrs,
                            char *buf, size_t cap) {
    const char *sep = (extra_hdrs != NULL && strstr(extra_hdrs, "\r\n\r\n") != NULL) ? "" : "\r\n";
    int n = snprintf(buf, cap,
                    "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                    "Content-Type: application/json\r\n"
                    "MCP-Protocol-Version: 2025-11-25\r\n"
                    "%s%s%s",
                    strlen(body), extra_hdrs ? extra_hdrs : "", sep, body);
    CHECK(n > 0 && (size_t)n < cap);
    return buf;
}

static const char kBody[] =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
    "\"params\":{\"name\":\"echo_xmcp\",\"arguments\":{\"region\":\"us-east-1\"}},"
    "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}";

/* 2026 request where the body region value matches the header value */
static void test_match_200(mcp_context_t *ctx, mcp_server_t *srv) {
    char buf[8192];
    char *req = make_req_2026(kBody,
                              "Mcp-Param-Region: us-east-1\r\n",
                              buf, sizeof(buf));
    const char *reqs[] = { req };
    mcp_status_t st;
    char *out = run_script(ctx, srv, reqs, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
    CHECK(strstr(out, "us-east-1") != NULL);
    free(out);
}

/* 2026 request: header value differs from body value → 400 + -32001 */
static void test_mismatch_400(mcp_context_t *ctx, mcp_server_t *srv) {
    char buf[8192];
    char *req = make_req_2026(kBody,
                              "Mcp-Param-Region: eu-west-2\r\n",
                              buf, sizeof(buf));
    const char *reqs[] = { req };
    mcp_status_t st;
    char *out = run_script(ctx, srv, reqs, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    CHECK(strstr(out, "HTTP/1.1 400") != NULL);
    CHECK(strstr(out, "-32001") != NULL);
    free(out);
}

/* 2026 request: body has region value but no Mcp-Param-Region header → 400 + -32001 */
static void test_missing_header_400(mcp_context_t *ctx, mcp_server_t *srv) {
    char buf[8192];
    char *req = make_req_2026(kBody, NULL, buf, sizeof(buf));
    const char *reqs[] = { req };
    mcp_status_t st;
    char *out = run_script(ctx, srv, reqs, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    CHECK(strstr(out, "HTTP/1.1 400") != NULL);
    CHECK(strstr(out, "-32001") != NULL);
    free(out);
}

/* 2026 request: base64 sentinel in header decodes to "us-east-1" which matches body → 200 */
static void test_base64_sentinel_200(mcp_context_t *ctx, mcp_server_t *srv) {
    /* base64("us-east-1") = "dXMtZWFzdC0x" */
    char buf[8192];
    char *req = make_req_2026(kBody,
                              "Mcp-Param-Region: =?base64?dXMtZWFzdC0x?=\r\n",
                              buf, sizeof(buf));
    const char *reqs[] = { req };
    mcp_status_t st;
    char *out = run_script(ctx, srv, reqs, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
    free(out);
}

/* 2026 request: base64 sentinel decodes to a value that does NOT match body → 400 */
static void test_base64_sentinel_mismatch_400(mcp_context_t *ctx, mcp_server_t *srv) {
    /* base64("eu-west-2") = "ZXUtd2VzdC0y" */
    char buf[8192];
    char *req = make_req_2026(kBody,
                              "Mcp-Param-Region: =?base64?ZXUtd2VzdC0y?=\r\n",
                              buf, sizeof(buf));
    const char *reqs[] = { req };
    mcp_status_t st;
    char *out = run_script(ctx, srv, reqs, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    CHECK(strstr(out, "HTTP/1.1 400") != NULL);
    CHECK(strstr(out, "-32001") != NULL);
    free(out);
}

/* 2025-11-25 request: Mcp-Param-* header is NOT checked → 200 even with wrong value */
static void test_2025_no_check_200(mcp_context_t *ctx, mcp_server_t *srv) {
    /* Build a 2025-11-25 tools/call for echo_xmcp (tool name is registered on
       this server, but the 2025 protocol version does NOT trigger the S2 check) */
    const char *body_2025 =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo_xmcp\",\"arguments\":{\"region\":\"us-east-1\"}},"
        "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2025-11-25\"}}";
    char buf[8192];
    char *req = make_req_2025(body_2025,
                               "Mcp-Param-Region: wrong-value\r\n",
                               buf, sizeof(buf));
    const char *reqs[] = { req };
    mcp_status_t st;
    char *out = run_script(ctx, srv, reqs, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    /* No -32001 error: S2 check is gated to 2026 only */
    CHECK(strstr(out, "-32001") == NULL);
    free(out);
}

/* 2026 request for a tool with NO x-mcp-header annotations → no header check → 200 */
static void test_no_annotation_no_check_200(mcp_context_t *ctx) {
    mcp_server_t *plain_srv = make_server_plain(ctx);
    /* Build a 2026 tools/call for echo_plain (no x-mcp-header in schema) */
    const char *body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo_plain\",\"arguments\":{\"region\":\"us-east-1\"}},"
        "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}";
    char buf[8192];
    int n = snprintf(buf, sizeof(buf),
                    "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                    "Content-Type: application/json\r\n"
                    "MCP-Protocol-Version: 2026-07-28\r\n"
                    "Mcp-Method: tools/call\r\nMcp-Name: echo_plain\r\n\r\n%s",
                    strlen(body), body);
    CHECK(n > 0 && (size_t)n < sizeof(buf));
    const char *reqs[] = { buf };
    mcp_status_t st;
    char *out = run_script(ctx, plain_srv, reqs, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
    CHECK(strstr(out, "-32001") == NULL);
    free(out);
    mcp_server_destroy(ctx, plain_srv);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = make_server_xmcp(ctx);

    test_match_200(ctx, srv);
    test_mismatch_400(ctx, srv);
    test_missing_header_400(ctx, srv);
    test_base64_sentinel_200(ctx, srv);
    test_base64_sentinel_mismatch_400(ctx, srv);
    test_2025_no_check_200(ctx, srv);
    test_no_annotation_no_check_200(ctx);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("test_http_mcp_param: OK\n");
    return 0;
}
