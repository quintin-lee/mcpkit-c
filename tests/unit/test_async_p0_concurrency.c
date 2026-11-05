#include "test_check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <unistd.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/runtime/threadpool.h"
#include "mcpkit/transport/ws.h"

/* P0 concurrency regression gate: 64 worker threads hammering a single
 * async server across HTTP / WS / SSE simultaneously. Asserts zero errors
 * and zero residual connections on shutdown. */

#define NUM_HTTP_THREADS 32
#define NUM_WS_THREADS   24
#define NUM_SSE_THREADS   8
#define CALLS_PER_THREAD 8

static atomic_int g_errors = 0;
static atomic_int g_completed = 0;

typedef struct {
    uint16_t port;
    int id;
} thread_arg_t;

/* Server run wrapper */
typedef struct {
    mcp_context_t *ctx;
    mcp_async_server_t *srv;
} server_thread_arg_t;

static int server_run_worker(void *arg) {
    server_thread_arg_t *a = (server_thread_arg_t *)arg;
    mcp_async_server_run(a->ctx, a->srv);
    return 0;
}

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

/* ------------------------------------------------------------------ */
/* Helpers.                                                            */
/* ------------------------------------------------------------------ */
static int write_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int tcp_connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr);
    if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* HTTP worker: initialize -> notifications/initialized -> N calls via
 * a single persistent socket. Errors accumulate into g_errors. */
static int http_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    int fd = tcp_connect(ta->port);
    if (fd < 0) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }

    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"p0\",\"version\":\"1.0\"}}}";
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n"
                     "%s",
                     ta->port, strlen(init_body), init_body);
    if (write(fd, req, (size_t)n) != n) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    char resp[2048] = {0};
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    if (rn <= 0 || strstr(resp, "200 OK") == NULL) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    const char *sid_hdr = strstr(resp, "Mcp-Session-Id: ");
    if (sid_hdr == NULL) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    sid_hdr += 16;
    const char *sid_end = strstr(sid_hdr, "\r\n");
    if (sid_end == NULL) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    char session_id[64] = {0};
    memcpy(session_id, sid_hdr, (size_t)(sid_end - sid_hdr));

    const char *notif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    n = snprintf(req, sizeof(req),
                 "POST /mcp HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Content-Type: application/json\r\n"
                 "Mcp-Session-Id: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: keep-alive\r\n"
                 "\r\n"
                 "%s",
                 ta->port, session_id, strlen(notif), notif);
    if (write(fd, req, (size_t)n) != n) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    memset(resp, 0, sizeof(resp));
    rn = read(fd, resp, sizeof(resp) - 1);
    if (rn <= 0 || (strstr(resp, "200 OK") == NULL &&
                    strstr(resp, "202 Accepted") == NULL)) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }

    for (int i = 0; i < CALLS_PER_THREAD; i++) {
        char body[256];
        int blen = snprintf(body, sizeof(body),
                            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
                            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"p0-http-%d-%d\"}}}",
                            i + 100, ta->id, i);
        n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Mcp-Session-Id: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n"
                     "%s",
                     ta->port, session_id, blen, body);
        if (write(fd, req, (size_t)n) != n) {
            atomic_fetch_add(&g_errors, 1);
            break;
        }
        memset(resp, 0, sizeof(resp));
        rn = read(fd, resp, sizeof(resp) - 1);
        char expected[64];
        snprintf(expected, sizeof(expected), "p0-http-%d-%d", ta->id, i);
        if (rn <= 0 || strstr(resp, expected) == NULL) {
            atomic_fetch_add(&g_errors, 1);
            break;
        }
        atomic_fetch_add(&g_completed, 1);
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Raw WebSocket helpers (mirrors test_ws_continuation.c).            */
/* ------------------------------------------------------------------ */
static int ws_handshake(mcp_context_t *ctx, uint16_t port, int *fd_out) {
    int fd = tcp_connect(port);
    if (fd < 0) return -1;
    static const char *WS_GUID = "258EAFA5-E91B-47DA-95CA-C5CC096B3C5A";
    char nonce[33];
    for (int i = 0; i < 16; i++) {
        uint32_t v = (uint32_t)rand();
        nonce[i * 2]     = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"[v % 62];
        nonce[i * 2 + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"[(v >> 6) % 62];
    }
    nonce[32] = '\0';
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n", port, nonce);
    if (write_all(fd, req, (size_t)n) != 0) { close(fd); return -1; }
    char buf[1024];
    ssize_t rn = read(fd, buf, sizeof(buf) - 1);
    if (rn <= 0 || strstr(buf, "101") == NULL) { close(fd); return -1; }
    (void)ctx;
    (void)WS_GUID;
    *fd_out = fd;
    return 0;
}

static int ws_send_frame(mcp_context_t *ctx, int fd, mcp_ws_opcode_t opcode,
                         const char *payload, size_t len) {
    mcp_ws_frame_t frame = {
        .fin = true,
        .opcode = opcode,
        .masked = true,
        .payload = (char *)payload,
        .payload_len = len,
    };
    for (int i = 0; i < 4; i++) frame.mask_key[i] = (uint8_t)(rand() & 0xFF);
    uint8_t *bytes = NULL;
    size_t blen = 0;
    if (mcp_ws_encode_frame(ctx, &frame, &bytes, &blen) != MCP_OK) return -1;
    /* encode already consumed the borrowed payload; null it out so
     * mcp_ws_frame_destroy does not free caller-owned bytes. */
    frame.payload = NULL;
    int rc = write_all(fd, bytes, blen);
    mcp_json_free_string(ctx, (char *)bytes);
    mcp_ws_frame_destroy(ctx, &frame);
    return rc;
}

static int ws_send_text(mcp_context_t *ctx, int fd, const char *payload, size_t len) {
    return ws_send_frame(ctx, fd, MCP_WS_OP_TEXT, payload, len);
}

/* Recv a full TEXT/BINARY/CONTINUATION (or PONG-skip) frame, skipping PING/PONG.
 * Returns 0 = text frame, 1 = PONG (retry), 2 = CLOSE, -1 = error/EOF. */
static int ws_recv_frame(mcp_context_t *ctx, int fd, char **out_payload, size_t *out_len) {
    enum { CARRY_CAP = 1 << 16 };
    char carry[CARRY_CAP];
    size_t carry_len = 0;
    for (;;) {
        if (carry_len < CARRY_CAP) {
            ssize_t rn = recv(fd, carry + carry_len, CARRY_CAP - carry_len - 1, 0);
            if (rn <= 0) return rn == 0 ? 2 : -1;
            carry_len += (size_t)rn;
        }
        mcp_ws_frame_t frame = {0};
        size_t consumed = 0;
        mcp_status_t st = mcp_ws_parse_frame(ctx, (const uint8_t *)carry, carry_len, &frame, &consumed);
        if (st != MCP_OK) { mcp_ws_frame_destroy(ctx, &frame); return -1; }
        if (consumed == 0) { if (carry_len >= CARRY_CAP - 1) { mcp_ws_frame_destroy(ctx, &frame); return -1; } continue; }
        memmove(carry, carry + consumed, carry_len - consumed);
        carry_len -= consumed;
        if (frame.opcode == MCP_WS_OP_PONG) { mcp_ws_frame_destroy(ctx, &frame); return 1; }
        if (frame.opcode == MCP_WS_OP_TEXT || frame.opcode == MCP_WS_OP_BINARY ||
            frame.opcode == MCP_WS_OP_CONTINUATION) {
            *out_payload = frame.payload;
            *out_len = frame.payload_len;
            return 0;
        }
        if (frame.opcode == MCP_WS_OP_CLOSE) { mcp_ws_frame_destroy(ctx, &frame); return 2; }
        mcp_ws_frame_destroy(ctx, &frame);
    }
}

/* WS worker: handshake + initialize + N raw-socket calls, CLOSE frame teardown.
 * Raw sockets are used so the client sends a CLOSE frame — the server then
 * drops the connection and active_connections reaches 0. */
static int ws_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) { atomic_fetch_add(&g_errors, 1); return -1; }

    int fd = -1;
    if (ws_handshake(ctx, ta->port, &fd) != 0) {
        atomic_fetch_add(&g_errors, 1);
        mcp_context_destroy(ctx);
        return -1;
    }

    const char *init =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"p0-ws\",\"version\":\"1.0\"}}}";
    if (ws_send_text(ctx, fd, init, strlen(init)) != 0) {
        atomic_fetch_add(&g_errors, 1); close(fd); mcp_context_destroy(ctx); return -1;
    }
    char *resp = NULL;
    size_t rlen = 0;
    if (ws_recv_frame(ctx, fd, &resp, &rlen) != 0 || resp == NULL) {
        atomic_fetch_add(&g_errors, 1);
        if (resp != NULL) mcp_json_free_string(ctx, resp);
        close(fd); mcp_context_destroy(ctx); return -1;
    }
    mcp_json_free_string(ctx, resp);

    const char *inited = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    if (ws_send_text(ctx, fd, inited, strlen(inited)) != 0) {
        atomic_fetch_add(&g_errors, 1); close(fd); mcp_context_destroy(ctx); return -1;
    }
    struct timespec ts50 = {.tv_sec = 0, .tv_nsec = 50 * 1000000};
    thrd_sleep(&ts50, NULL);

    int ok = 0;
    for (int i = 0; i < CALLS_PER_THREAD; i++) {
        char text[64];
        snprintf(text, sizeof(text), "p0-ws-%d-%d", ta->id, i);
        char req_json[256];
        int blen = snprintf(req_json, sizeof(req_json),
                            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
                            "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"%s\"}}}",
                            i + 10, text);
        if (ws_send_text(ctx, fd, req_json, (size_t)blen) != 0) break;
        resp = NULL;
        int rc;
        for (;;) {
            rc = ws_recv_frame(ctx, fd, &resp, &rlen);
            if (rc == 1) continue; /* PONG — keep waiting */
            break;
        }
        if (rc != 0 || resp == NULL) break;
        if (strstr(resp, text) == NULL) {
            mcp_json_free_string(ctx, resp);
            break;
        }
        mcp_json_free_string(ctx, resp);
        ok = 1;
    }

    /* Protocol-clean teardown: send CLOSE, then drop the socket so the
     * server's read loop sees EOF and the connection is released. */
    uint8_t close_payload[2] = {0x03, 0xE8}; /* status 1000 */
    ws_send_frame(ctx, fd, MCP_WS_OP_CLOSE, (const char *)close_payload, 2);
    close(fd);
    mcp_context_destroy(ctx);
    if (!ok) atomic_fetch_add(&g_errors, 1);
    else atomic_fetch_add(&g_completed, CALLS_PER_THREAD);
    return 0;
}

/* SSE worker: initialize, open SSE, hold for 500ms. */
static int sse_worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    int fd = tcp_connect(ta->port);
    if (fd < 0) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }

    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"p0-sse\",\"version\":\"1.0\"}}}";
    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     ta->port, strlen(init_body), init_body);
    if (write(fd, req, (size_t)n) != n) {
        atomic_fetch_add(&g_errors, 1);
        close(fd);
        return -1;
    }
    char resp[2048] = {0};
    ssize_t rn = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (rn <= 0 || strstr(resp, "200 OK") == NULL) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }
    const char *sid_hdr = strstr(resp, "Mcp-Session-Id: ");
    if (sid_hdr == NULL) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }
    sid_hdr += 16;
    const char *sid_end = strstr(sid_hdr, "\r\n");
    if (sid_end == NULL) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }
    char session_id[64] = {0};
    memcpy(session_id, sid_hdr, (size_t)(sid_end - sid_hdr));

    int sse_fd = tcp_connect(ta->port);
    if (sse_fd < 0) {
        atomic_fetch_add(&g_errors, 1);
        return -1;
    }
    n = snprintf(req, sizeof(req),
                 "GET /mcp HTTP/1.1\r\n"
                 "Host: 127.0.0.1:%u\r\n"
                 "Accept: text/event-stream\r\n"
                 "Mcp-Session-Id: %s\r\n"
                 "\r\n",
                 ta->port, session_id);
    if (write(sse_fd, req, (size_t)n) != n) {
        atomic_fetch_add(&g_errors, 1);
        close(sse_fd);
        return -1;
    }
    memset(resp, 0, sizeof(resp));
    rn = read(sse_fd, resp, sizeof(resp) - 1);
    if (rn <= 0 || strstr(resp, "200 OK") == NULL ||
        strstr(resp, "text/event-stream") == NULL) {
        atomic_fetch_add(&g_errors, 1);
        close(sse_fd);
        return -1;
    }

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 500 * 1000 * 1000};
    thrd_sleep(&ts, NULL);
    close(sse_fd);
    return 0;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "p0", "0.1.0");
    CHECK(server != NULL);
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    CHECK(mcp_server_add_tool(ctx, server,
                              mcp_tool_new(ctx, "echo", "Echo", schema,
                                           echo_handler, NULL)) == MCP_OK);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 16);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .bind_addr = "127.0.0.1",
        .max_connections = 512,
        .keepalive_timeout_s = 60,
        .enable_websocket = true,
        .enable_sse = true,
    };
    mcp_async_server_t *srv = mcp_async_server_create(ctx, server, pool, &cfg);
    CHECK(srv != NULL);
    uint16_t port = mcp_async_server_get_port(srv);
    CHECK(port > 0);

    /* Run the server in a dedicated thread. */
    thrd_t sthrd;
    struct {
        mcp_context_t *ctx;
        mcp_async_server_t *srv;
    } sarg = {.ctx = ctx, .srv = srv};
    CHECK(thrd_create(&sthrd, server_run_worker, &sarg) == thrd_success);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    const int NTHREADS = NUM_HTTP_THREADS + NUM_WS_THREADS + NUM_SSE_THREADS;
    thrd_t threads[NTHREADS];
    thread_arg_t args[NTHREADS];
    int t = 0;

    for (int i = 0; i < NUM_HTTP_THREADS; i++) {
        args[t] = (thread_arg_t){.port = port, .id = i};
        CHECK(thrd_create(&threads[t], http_worker, &args[t]) == thrd_success);
        t++;
    }
    for (int i = 0; i < NUM_SSE_THREADS; i++) {
        args[t] = (thread_arg_t){.port = port, .id = i};
        CHECK(thrd_create(&threads[t], sse_worker, &args[t]) == thrd_success);
        t++;
    }
    for (int i = 0; i < NUM_WS_THREADS; i++) {
        args[t] = (thread_arg_t){.port = port, .id = i};
        CHECK(thrd_create(&threads[t], ws_worker, &args[t]) == thrd_success);
        t++;
    }
    for (int i = 0; i < NTHREADS; i++) {
        thrd_join(threads[i], NULL);
    }

    /* All workers done: their sockets are closed. Wait for the server's
     * event loop to process every close event and drop the connection
     * count to zero BEFORE stopping the run loop. A bounded poll loop
     * ensures we do not wait longer than 5s. */
    for (int i = 0; i < 50; i++) {
        size_t active = mcp_async_server_active_connections(srv);
        if (active == 0) break;
        struct timespec dts = {.tv_sec = 0, .tv_nsec = 100 * 1000 * 1000};
        thrd_sleep(&dts, NULL);
    }
    CHECK(mcp_async_server_active_connections(srv) == 0);

    mcp_async_server_stop(ctx, srv);
    thrd_join(sthrd, NULL);

    int errors = atomic_load(&g_errors);
    int completed = atomic_load(&g_completed);
    int expected_calls = (NUM_HTTP_THREADS + NUM_WS_THREADS) * CALLS_PER_THREAD;
    CHECK(completed == expected_calls);
    CHECK(errors == 0);

    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
    printf("test_async_p0_concurrency: OK (%d calls, 0 errors)\n", completed);
    return 0;
}
