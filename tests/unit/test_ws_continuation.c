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
#include "mcpkit/transport/ws.h"

/* ------------------------------------------------------------------ */
/* Echo tool used by the server to answer a fragmented tools/call.     */
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
/* Raw TCP helpers.                                                    */
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

static ssize_t write_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, (const char *)buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)len;
}

/* Perform the WS upgrade handshake over a raw socket. Returns 0 on
 * success (101 received), -1 otherwise. */
static int ws_handshake(int fd, uint16_t port) {
    const char *key = "dGhlIHNhbXBsZSBub25jZQ=="; /* RFC 6455 sample key */
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "\r\n",
                     port, key);
    CHECK(write_all(fd, req, (size_t)n) == n);

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t rn = read(fd, buf, sizeof(buf) - 1);
    CHECK(rn > 0);
    return strstr(buf, "101") != NULL ? 0 : -1;
}

/* Encode a client->server (masked) WS frame and send it. */
static int ws_send_frame(mcp_context_t *ctx, int fd, bool fin,
                         mcp_ws_opcode_t opcode, const char *payload, size_t len) {
    mcp_ws_frame_t frame = {
        .fin = fin,
        .opcode = opcode,
        .masked = true,
        .mask_key = {0x12, 0x34, 0x56, 0x78},
        .payload_len = len,
        .payload = (char *)payload,
    };
    uint8_t *bytes = NULL;
    size_t blen = 0;
    if (mcp_ws_encode_frame(ctx, &frame, &bytes, &blen) != MCP_OK) {
        return -1;
    }
    int rc = write_all(fd, bytes, blen) < 0 ? -1 : 0;
    mcp_json_free_string(ctx, (char *)bytes);
    return rc;
}

/* Minimal receive of one server->client WS frame (unmasked). Skips PONG.
 * Returns 0 on a data frame (payload owned, in *out_payload / *out_len),
 * -1 on EOF/IO error, and 2 if the peer closed (recv 0). */
static int ws_recv_frame(mcp_context_t *ctx, int fd, char **out_payload, size_t *out_len) {
    uint8_t carry[8192];
    size_t carry_len = 0;
    for (;;) {
        /* Try to parse a complete frame from what we have. */
        mcp_ws_frame_t frame = {0};
        size_t consumed = 0;
        mcp_status_t st = mcp_ws_parse_frame(ctx, carry, carry_len, &frame, &consumed);
        if (st != MCP_OK) {
            mcp_ws_frame_destroy(ctx, &frame);
            return -1;
        }
        if (consumed > 0) {
            memmove(carry, carry + consumed, carry_len - consumed);
            carry_len -= consumed;
            if (frame.opcode == MCP_WS_OP_PONG) {
                mcp_ws_frame_destroy(ctx, &frame);
                continue;
            }
            if (frame.opcode == MCP_WS_OP_TEXT || frame.opcode == MCP_WS_OP_BINARY ||
                frame.opcode == MCP_WS_OP_CONTINUATION) {
                *out_payload = frame.payload;
                *out_len = frame.payload_len;
                return 0;
            }
            if (frame.opcode == MCP_WS_OP_CLOSE) {
                mcp_ws_frame_destroy(ctx, &frame);
                return 2;
            }
            mcp_ws_frame_destroy(ctx, &frame);
            continue;
        }
        /* Need more bytes. */
        ssize_t rn = recv(fd, carry + carry_len, sizeof(carry) - carry_len, 0);
        if (rn < 0) {
            return -1;
        }
        if (rn == 0) {
            return 2;
        }
        carry_len += (size_t)rn;
        if (carry_len >= sizeof(carry)) {
            return -1;
        }
    }
}

static void wait_ms(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    thrd_sleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Test: fragmented JSON-RPC request is reassembled and answered.      */
/* ------------------------------------------------------------------ */
static void test_fragmented_request_reassembled(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "ws-frag", "0.1.0");
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
    wait_ms(50);

    int fd = tcp_connect(port);
    CHECK(ws_handshake(fd, port) == 0);

    /* 1. initialize over WS (complete TEXT frame). */
    const char *ws_init =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"frag\",\"version\":\"1.0\"}}}";
    CHECK(ws_send_frame(ctx, fd, true, MCP_WS_OP_TEXT, ws_init, strlen(ws_init)) == 0);
    char *resp = NULL;
    size_t rlen = 0;
    CHECK(ws_recv_frame(ctx, fd, &resp, &rlen) == 0);
    CHECK(strstr(resp, "protocolVersion") != NULL);
    mcp_json_free_string(ctx, resp);

    /* 2. initialized notification. */
    const char *inited = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    CHECK(ws_send_frame(ctx, fd, true, MCP_WS_OP_TEXT, inited, strlen(inited)) == 0);
    wait_ms(30);

    /* 3. Fragmented tools/call:
     *    Text(fin=0, part1) + Continuation(fin=0, part2) +
     *    Continuation(fin=1, empty). Reassembled = full request. */
    const char *full =
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"frag-marker\"}}}";
    size_t mid = strlen(full) / 2;
    CHECK(ws_send_frame(ctx, fd, false, MCP_WS_OP_TEXT, full, mid) == 0);
    CHECK(ws_send_frame(ctx, fd, false, MCP_WS_OP_CONTINUATION, full + mid,
                       strlen(full) - mid) == 0);
    CHECK(ws_send_frame(ctx, fd, true, MCP_WS_OP_CONTINUATION, "", 0) == 0);

    resp = NULL;
    CHECK(ws_recv_frame(ctx, fd, &resp, &rlen) == 0);
    CHECK(strstr(resp, "frag-marker") != NULL);
    CHECK(strstr(resp, "\"id\":10") != NULL);
    mcp_json_free_string(ctx, resp);

    close(fd);
    mcp_async_server_stop(ctx, srv);
    thrd_join(sthrd, NULL);
    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
}

/* ------------------------------------------------------------------ */
/* Test: orphan Continuation frame (no preceding fragment) -> the     */
/* connection is safely closed, no crash.                              */
/* ------------------------------------------------------------------ */
static void test_orphan_continuation_closes(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "ws-orphan", "0.1.0");
    CHECK(server != NULL);
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 2);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .bind_addr = "127.0.0.1",
        .max_connections = 10,
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
    wait_ms(50);

    int fd = tcp_connect(port);
    CHECK(ws_handshake(fd, port) == 0);

    /* Send an orphan Continuation frame with no preceding fragment. */
    CHECK(ws_send_frame(ctx, fd, true, MCP_WS_OP_CONTINUATION, "garbage", 7) == 0);

    /* The server must close the connection: either a CLOSE frame or EOF. */
    char *resp = NULL;
    size_t rlen = 0;
    int rc = ws_recv_frame(ctx, fd, &resp, &rlen);
    CHECK(rc != 0); /* Not a data frame */
    if (resp != NULL) {
        mcp_json_free_string(ctx, resp);
    }

    close(fd);
    mcp_async_server_stop(ctx, srv);
    thrd_join(sthrd, NULL);
    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_fragmented_request_reassembled();
    test_orphan_continuation_closes();
    printf("test_ws_continuation: OK\n");
    return 0;
}
