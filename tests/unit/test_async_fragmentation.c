#include "test_check.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <threads.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/runtime/threadpool.h"

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *tv = mcp_json_object_get(ctx, args, "text");
    const char *text = NULL;
    if (tv == NULL || mcp_json_string_value(ctx, tv, &text) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *res = mcp_json_object_new(ctx);

    mcp_json_object_set(ctx, item, "type", mcp_json_string_new(ctx, "text"));
    mcp_json_object_set(ctx, item, "text", mcp_json_string_new(ctx, text));
    mcp_json_array_append(ctx, content, item);
    mcp_json_object_set(ctx, res, "content", content);

    *result_out = res;
    return MCP_OK;
}

typedef struct {
    mcp_context_t *ctx;
    mcp_async_server_t *srv;
} server_thread_arg_t;

static int server_run_worker(void *arg) {
    server_thread_arg_t *a = (server_thread_arg_t *)arg;
    mcp_async_server_run(a->ctx, a->srv);
    return 0;
}

static int tcp_connect_loopback(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    CHECK(connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0);
    return fd;
}

/* Build a tools/call POST using the given session id; returns total byte count. */
static size_t build_post(char *out, size_t out_cap, uint16_t port,
                         const char *session_id, const char *tool_payload) {
    int n = snprintf(out, out_cap,
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Mcp-Session-Id: %s\r\n"
                     "\r\n"
                     "%s",
                     port, strlen(tool_payload), session_id, tool_payload);
    CHECK(n > 0 && (size_t)n < out_cap);
    return (size_t)n;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "frag-test", "0.1.0");
    CHECK(server != NULL);

    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    CHECK(mcp_server_add_tool(ctx, server,
                              mcp_tool_new(ctx, "echo", "Echo text", schema,
                                           echo_handler, NULL)) == MCP_OK);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .bind_addr = "127.0.0.1",
        .max_connections = 100,
        .keepalive_timeout_s = 60,
        .enable_websocket = true,
        .enable_sse = true,
    };

    mcp_async_server_t *srv = mcp_async_server_create(ctx, server, pool, &cfg);
    CHECK(srv != NULL);

    uint16_t port = mcp_async_server_get_port(srv);
    CHECK(port > 0);

    server_thread_arg_t sarg = {.ctx = ctx, .srv = srv};
    thrd_t sthrd;
    CHECK(thrd_create(&sthrd, server_run_worker, &sarg) == thrd_success);

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    /* Obtain a session id via initialize. */
    int http_fd = tcp_connect_loopback(port);
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"frag\",\"version\":\"1.0\"}}}";
    char http_req[1024];
    int n = snprintf(http_req, sizeof(http_req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     port, strlen(init_body), init_body);
    CHECK(write(http_fd, http_req, (size_t)n) == n);
    char resp_buf[4096] = {0};
    ssize_t total_rn = 0;
    for (int i = 0; i < 100; i++) {
        ssize_t rr = read(http_fd, resp_buf + total_rn, sizeof(resp_buf) - 1 - (size_t)total_rn);
        if (rr <= 0) break;
        total_rn += rr;
        if (strstr(resp_buf, "Mcp-Session-Id:") != NULL && strstr(resp_buf, "\"id\":1") != NULL) {
            break;
        }
    }
    CHECK(total_rn > 0);
    CHECK(strstr(resp_buf, "Mcp-Session-Id:") != NULL);
    const char *sid_hdr = strstr(resp_buf, "Mcp-Session-Id: ");
    CHECK(sid_hdr != NULL);
    sid_hdr += 16;
    const char *sid_end = strstr(sid_hdr, "\r\n");
    CHECK(sid_end != NULL);
    char session_id[64] = {0};
    memcpy(session_id, sid_hdr, (size_t)(sid_end - sid_hdr));
    close(http_fd);

    /* Send notifications/initialized so tools/call is allowed on the session. */
    {
        int fd2 = tcp_connect_loopback(port);
        const char *notify =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        char nreq[512];
        int nn = snprintf(nreq, sizeof(nreq),
                          "POST /mcp HTTP/1.1\r\n"
                          "Host: 127.0.0.1:%u\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: %zu\r\n"
                          "Mcp-Session-Id: %s\r\n"
                          "\r\n"
                          "%s",
                          port, strlen(notify), session_id, notify);
        CHECK(write(fd2, nreq, (size_t)nn) == nn);
        /* notifications/initialized 是 MCP notification：服务端无响应，
         * 发完即关连接，不读回包；稍等让 worker 线程处理完再发起 tools/call */
        close(fd2);
        struct timespec wait = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
        thrd_sleep(&wait, NULL);
    }

    /* --- Case A: fragmented delivery (header + 4 bytes body, sleep, rest) --- */
    {
        const char *tool_payload =
            "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"frag-a\"}}}";
        char req[1024];
        size_t total = build_post(req, sizeof(req), port, session_id, tool_payload);
        /* Find where the body begins (after \r\n\r\n). */
        char *body_start = strstr(req, "\r\n\r\n") + 4;
        size_t header_part = (size_t)(body_start - req) + 4; /* header + 4 body bytes */
        CHECK(header_part < total);

        int fd = tcp_connect_loopback(port);
        CHECK(write(fd, req, header_part) == (ssize_t)header_part);
        struct timespec w = {.tv_sec = 0, .tv_nsec = 15 * 1000 * 1000};
        thrd_sleep(&w, NULL);
        CHECK(write(fd, req + header_part, total - header_part) ==
              (ssize_t)(total - header_part));

        char r[4096] = {0};
        ssize_t total_r = 0;
        for (int i = 0; i < 100; i++) {
            ssize_t rr = read(fd, r + total_r, sizeof(r) - 1 - (size_t)total_r);
            if (rr <= 0) break;
            total_r += rr;
            if (strstr(r, "\"id\":20") != NULL) break;
        }
        CHECK(total_r > 0);
        CHECK(strstr(r, "200 OK") != NULL);
        CHECK(strstr(r, "400 Bad Request") == NULL);
        CHECK(strstr(r, "frag-a") != NULL);
        close(fd);
    }

    /* --- Case B: pipelined requests (two full POSTs in a single send) --- */
    {
        const char *p1 =
            "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"pipe-1\"}}}";
        const char *p2 =
            "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"pipe-2\"}}}";
        char r1[1024];
        char r2[1024];
        size_t n1 = build_post(r1, sizeof(r1), port, session_id, p1);
        size_t n2 = build_post(r2, sizeof(r2), port, session_id, p2);

        int fd = tcp_connect_loopback(port);
        char combined[4096];
        memcpy(combined, r1, n1);
        memcpy(combined + n1, r2, n2);
        CHECK(write(fd, combined, n1 + n2) == (ssize_t)(n1 + n2));
        fprintf(stderr, "[traceB-start]\n");
        fflush(stderr);

        /* Response 1 first: keep reading until the first response id is seen,
         * accumulating (response 2 may already be pipelined right after). */
        char resp_a[4096] = {0};
        ssize_t total_a = 0;
        for (int i = 0; i < 100; i++) {
            ssize_t rr = read(fd, resp_a + total_a, sizeof(resp_a) - 1 - (size_t)total_a);
            if (rr <= 0) break;
            total_a += rr;
            if (strstr(resp_a, "\"id\":22") != NULL) break;
        }
        fprintf(stderr, "[traceB] total_a=%zd resp_a=%.500s\n", total_a, resp_a);
        fflush(stderr);
        CHECK(total_a > 0);
        CHECK(strstr(resp_a, "\"id\":21") != NULL);
        CHECK(strstr(resp_a, "pipe-1") != NULL);

        /* Response 2: keep reading until the second response arrives. */
        char resp_b[4096] = {0};
        ssize_t total_b = 0;
        if (strstr(resp_a, "\"id\":22") == NULL) {
            for (int i = 0; i < 100; i++) {
                ssize_t rr = read(fd, resp_b + total_b, sizeof(resp_b) - 1 - (size_t)total_b);
                if (rr <= 0) {
                    break;
                }
                total_b += rr;
                if (strstr(resp_b, "\"id\":22") != NULL) {
                    break;
                }
            }
        } else {
            /* 第二个响应已混入 resp_a：直接用它校验，避免漏读 */
            memcpy(resp_b, resp_a, total_a);
            total_b = total_a;
        }
        CHECK(strstr(resp_b, "\"id\":22") != NULL);
        CHECK(strstr(resp_b, "pipe-2") != NULL);
        close(fd);
    }

    mcp_async_server_stop(ctx, srv);
    thrd_join(sthrd, NULL);

    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
    printf("test_async_fragmentation: OK\n");
    return 0;
}
