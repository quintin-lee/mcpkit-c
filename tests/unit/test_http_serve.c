#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *text = mcp_json_object_get(ctx, args, "text");
    const char *s = NULL;
    if (text == NULL || mcp_json_string_value(ctx, text, &s) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *stv = mcp_json_string_new(ctx, s);
    if (result == NULL || stv == NULL
        || mcp_json_object_set(ctx, result, "echo", stv) != MCP_OK) {
        mcp_json_destroy(ctx, stv);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

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

static mcp_server_t *make_server(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "httpsrv", "0.1.0");
    CHECK(srv != NULL);
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    CHECK(schema != NULL);
    CHECK(mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx)) == MCP_OK);
    CHECK(mcp_schema_add_required(ctx, schema, "text") == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv,
                               mcp_tool_new(ctx, "echo", "Echo", schema, echo_handler,
                                            NULL)) == MCP_OK);
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

static const char *kInitBody =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"h\",\"version\":\"0.1.0\"}}}";
static const char *kNotifBody = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
static const char *kCallBody =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
    "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hi http\"}}}";

static char *make_post(const char *body, const char *sid, char *buf, size_t cap) {
    int n;
    if (sid != NULL) {
        n = snprintf(buf, cap,
                     "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                     "Content-Type: application/json\r\nMcp-Session-Id: %s\r\n\r\n%s",
                     strlen(body), sid, body);
    } else {
        n = snprintf(buf, cap,
                     "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                     "Content-Type: application/json\r\n\r\n%s",
                     strlen(body), body);
    }
    CHECK(n > 0 && (size_t)n < cap);
    return buf;
}

static void check_status_line(const char *resp, const char *expect) {
    CHECK(strncmp(resp, expect, strlen(expect)) == 0);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = make_server(ctx);

    char b1[4096], b2[4096], b3[4096];
    mcp_status_t st;

    /* (a) init without session -> 200 + Mcp-Session-Id + 2025-06-18 */
    const char *s1[] = { make_post(kInitBody, NULL, b1, sizeof(b1)) };
    char *    out = run_script(ctx, srv, s1, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    check_status_line(out, "HTTP/1.1 200 OK");
    const char *sid_at = strstr(out, "Mcp-Session-Id: ");
    CHECK(sid_at != NULL);
    char sid[64];
    CHECK(sscanf(sid_at + 16, "%63s", sid) == 1);
    CHECK(strstr(out, "2025-06-18") != NULL);
    free(out);

    /* (b)+(c) init + notify + call in ONE loop (sessions die with the loop) */
    const char *s2[] = {
        make_post(kInitBody, NULL, b1, sizeof(b1)),
        make_post(kNotifBody, "sess-1", b2, sizeof(b2)),
        make_post(kCallBody, "sess-1", b3, sizeof(b3)),
    };
    out = run_script(ctx, srv, s2, 3, &st);
    CHECK(st == MCP_OK && out != NULL);
    check_status_line(out, "HTTP/1.1 200 OK");
    CHECK(strstr(out, "Mcp-Session-Id: sess-1") != NULL);
    const char *acc = strstr(out, "HTTP/1.1 202 Accepted");
    CHECK(acc != NULL);
    CHECK(strstr(acc, "\"hi http\"") != NULL);
    free(out);

    /* (d) call without session id -> 400 */
    const char *s3[] = { make_post(kCallBody, NULL, b1, sizeof(b1)) };
    out = run_script(ctx, srv, s3, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    check_status_line(out, "HTTP/1.1 400 Bad Request");
    free(out);

    /* (e) unknown session id -> 404 */
    const char *s4[] = { make_post(kCallBody, "sess-999", b1, sizeof(b1)) };
    out = run_script(ctx, srv, s4, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    check_status_line(out, "HTTP/1.1 404 Not Found");
    free(out);

    /* (f) GET with SSE accept -> 200 event-stream with data: */
    const char *s5[] = { "GET /mcp HTTP/1.1\r\nAccept: text/event-stream\r\n\r\n" };
    out = run_script(ctx, srv, s5, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    check_status_line(out, "HTTP/1.1 200 OK");
    CHECK(strstr(out, "text/event-stream") != NULL);
    CHECK(strstr(out, "data: ") != NULL);
    free(out);

    /* (g) GET without SSE accept -> 405 */
    const char *s6[] = { "GET /mcp HTTP/1.1\r\n\r\n" };
    out = run_script(ctx, srv, s6, 1, &st);
    CHECK(st == MCP_OK && out != NULL);
    check_status_line(out, "HTTP/1.1 405 Method Not Allowed");
    free(out);

    /* (h) init + DELETE + reuse in ONE loop (sessions die with the loop,
       first session of each loop is deterministically sess-1) */
    char del[256];
    snprintf(del, sizeof(del), "DELETE /mcp HTTP/1.1\r\nMcp-Session-Id: sess-1\r\n\r\n");
    const char *s7[] = {
        make_post(kInitBody, NULL, b1, sizeof(b1)),
        del,
        make_post(kCallBody, "sess-1", b2, sizeof(b2)),
    };
    out = run_script(ctx, srv, s7, 3, &st);
    CHECK(st == MCP_OK && out != NULL);
    check_status_line(out, "HTTP/1.1 200 OK");
    CHECK(strstr(out, "Mcp-Session-Id: sess-1") != NULL);
    CHECK(strstr(out, "HTTP/1.1 404 Not Found") != NULL);
    free(out);

    /* (i) sse_wrap exact bytes + NULL guards */
    char *ev = mcp_sse_wrap(ctx, "{\"a\":1}");
    CHECK(ev != NULL && strcmp(ev, "data: {\"a\":1}\n\n") == 0);
    mcp_json_free_string(ctx, ev);
    CHECK(mcp_sse_wrap(ctx, NULL) == NULL);
    ev = mcp_sse_wrap(NULL, "{\"a\":1}");
    mcp_json_free_string(NULL, ev);

    /* NULL guards */
    CHECK(mcp_http_serve(ctx, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("test_http_serve OK\n");
    return 0;
}
