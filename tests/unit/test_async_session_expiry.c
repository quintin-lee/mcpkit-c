#include "test_check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/runtime/threadpool.h"

/* ------------------------------------------------------------------ */
/* Echo tool used by the server.                                       */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/* TCP helpers.                                                        */
/* ------------------------------------------------------------------ */
static int tcp_connect(uint16_t port) {
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

static int write_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Send an HTTP POST request body with the given session id; returns HTTP
 * status code read from the response, or -1 on EOF. */
static int send_post(int fd, uint16_t port, const char *session_id,
                     const char *body) {
    char req[2048];
    int n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Mcp-Session-Id: %s\r\n"
                     "\r\n"
                     "%s",
                     port, strlen(body), session_id, body);
    CHECK(write_all(fd, req, (size_t)n) == 0);

    char resp[8192] = {0};
    ssize_t total = 0;
    for (int i = 0; i < 200; i++) {
        ssize_t rn = recv(fd, resp + total, sizeof(resp) - 1 - (size_t)total, 0);
        if (rn <= 0) break;
        total += rn;
        resp[total] = '\0';
        /* Stop after first response is complete */
        if (strstr(resp, "\r\n\r\n") != NULL) break;
    }
    if (strstr(resp, "404") != NULL) return 404;
    if (strstr(resp, "200") != NULL) return 200;
    if (strstr(resp, "400") != NULL) return 400;
    return (total > 0) ? 0 : -1;
}

/* Obtain a session id via the initialize handshake. */
static int obtain_session_id(int fd, uint16_t port, char *out_sid, size_t out_cap) {
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"expiry\",\"version\":\"1.0\"}}}";
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     port, strlen(init_body), init_body);
    CHECK(write_all(fd, req, (size_t)n) == 0);

    char resp[8192] = {0};
    ssize_t total = 0;
    for (int i = 0; i < 200; i++) {
        ssize_t rn = recv(fd, resp + total, sizeof(resp) - 1 - (size_t)total, 0);
        if (rn <= 0) break;
        total += rn;
        if (strstr(resp, "\"id\":1") != NULL && strstr(resp, "Mcp-Session-Id:") != NULL)
            break;
    }
    CHECK(total > 0);

    const char *hdr = strstr(resp, "Mcp-Session-Id: ");
    CHECK(hdr != NULL);
    hdr += 16;
    const char *end = strstr(hdr, "\r\n");
    CHECK(end != NULL);
    size_t len = (size_t)(end - hdr);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out_sid, hdr, len);
    out_sid[len] = '\0';
    return 0;
}

static void wait_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    thrd_sleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Test: session expires after keepalive_timeout_s and is 404.        */
/* ------------------------------------------------------------------ */
static void test_session_expiry_404(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "expiry", "0.1.0");
    CHECK(server != NULL);
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    CHECK(mcp_server_add_tool(ctx, server,
                              mcp_tool_new(ctx, "echo", "Echo", schema,
                                           echo_handler, NULL)) == MCP_OK);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 2);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .bind_addr = "127.0.0.1",
        .max_connections = 50,
        .keepalive_timeout_s = 1,  /* short timeout for test */
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
    wait_ms(50);

    /* Establish session via initialize + initialized notification. */
    int fd1 = tcp_connect(port);
    char session_id[64] = {0};
    CHECK(obtain_session_id(fd1, port, session_id, sizeof(session_id)) == 0);
    close(fd1);

    /* Send notifications/initialized on a new connection. */
    {
        int fd2 = tcp_connect(port);
        const char *notify =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        char req[512];
        int n = snprintf(req, sizeof(req),
                         "POST /mcp HTTP/1.1\r\n"
                         "Host: 127.0.0.1:%u\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: %zu\r\n"
                         "Mcp-Session-Id: %s\r\n"
                         "\r\n"
                         "%s",
                         port, strlen(notify), session_id, notify);
        CHECK(write_all(fd2, req, (size_t)n) == 0);
        close(fd2);
    }
    wait_ms(100);

    /* Verify the session is alive (tools/call works). */
    {
        int fd3 = tcp_connect(port);
        const char *call =
            "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"alive\"}}}";
        int code = send_post(fd3, port, session_id, call);
        CHECK(code == 200);
        close(fd3);
    }

    /* Wait past keepalive_timeout_s (1s) + reaper cycle (5s) so the
     * session is eligible for reaping. The step interval is 5s, and
     * the reaper runs once per step when 5s have elapsed since last
     * reap. We wait 7s to be safe. */
    wait_ms(7000);

    /* Now the session should have been reaped: subsequent request 404. */
    {
        int fd4 = tcp_connect(port);
        const char *call =
            "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"dead\"}}}";
        int code = send_post(fd4, port, session_id, call);
        CHECK(code == 404);
        close(fd4);
    }

    mcp_async_server_stop(ctx, srv);
    thrd_join(sthrd, NULL);
    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
}

/* ------------------------------------------------------------------ */
/* Test: hanging SSE connection + slow task: no crash/deadlock/UAF.   */
/* ------------------------------------------------------------------ */
static void test_hanging_sse_no_crash(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "hang-sse", "0.1.0");
    CHECK(server != NULL);
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 2);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .bind_addr = "127.0.0.1",
        .max_connections = 20,
        .keepalive_timeout_s = 1,
        .enable_websocket = false,
        .enable_sse = true,
    };
    mcp_async_server_t *srv = mcp_async_server_create(ctx, server, pool, &cfg);
    CHECK(srv != NULL);
    uint16_t port = mcp_async_server_get_port(srv);
    CHECK(port > 0);

    server_thread_arg_t sarg = {.ctx = ctx, .srv = srv};
    thrd_t sthrd;
    CHECK(thrd_create(&sthrd, server_run_worker, &sarg) == thrd_success);
    wait_ms(50);

    /* Get a session id. */
    int fd = tcp_connect(port);
    char session_id[64] = {0};
    CHECK(obtain_session_id(fd, port, session_id, sizeof(session_id)) == 0);
    close(fd);

    /* Open a GET /mcp SSE connection and leave it open (hanging).
     * The SSE connection pins a ref on the session entry, but the
     * reaper will close it after keepalive_timeout_s expires. */
    int sse_fd = tcp_connect(port);
    {
        char req[512];
        int n = snprintf(req, sizeof(req),
                         "GET /mcp HTTP/1.1\r\n"
                         "Host: 127.0.0.1:%u\r\n"
                         "Accept: text/event-stream\r\n"
                         "Mcp-Session-Id: %s\r\n"
                         "\r\n",
                         port, session_id);
        CHECK(write_all(sse_fd, req, (size_t)n) == 0);
    }
    /* Read the SSE 200 header to confirm the connection is accepted. */
    {
        char buf[512] = {0};
        ssize_t rn = recv(sse_fd, buf, sizeof(buf) - 1, 0);
        CHECK(rn > 0);
        CHECK(strstr(buf, "200") != NULL);
    }

    /* Wait for the reaper to detect the expired session and close the
     * SSE connection. After 7s the SSE fd should see EOF or error. */
    wait_ms(7000);

    /* The SSE connection should have been closed by the reaper:
     * a read should return 0 (EOF). */
    {
        char buf[64];
        ssize_t rn = recv(sse_fd, buf, sizeof(buf), 0);
        /* EOF (0) or error (-1) both mean the connection was closed. */
        CHECK(rn <= 0);
    }
    close(sse_fd);

    /* Server still runs cleanly (no deadlock/UAF in reaper path). */
    mcp_async_server_stop(ctx, srv);
    thrd_join(sthrd, NULL);
    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_session_expiry_404();
    test_hanging_sse_no_crash();
    printf("test_async_session_expiry: OK\n");
    return 0;
}
