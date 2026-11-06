#include "test_check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <threads.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/runtime/threadpool.h"

/* Red-light UAF gate: destroy-with-pending-worker-tasks regression.
 *
 * Bug: mcp_async_server_destroy() used to tear down sessions and the
 * response queue immediately. Worker threads may still be executing
 * dispatched tasks that hold conn/session references and call
 * mcp_async_server_enqueue_response — tearing the structures out from
 * under them is a use-after-free.
 *
 * Fix: a pending counter on the server + a pthread_cond wait inside
 * mcp_async_server_destroy blocks until every in-flight worker task
 * finishes.
 *
 * Test: connect 32 real TCP clients, each POSTing a valid JSON-RPC
 * initialize, then IMMEDIATELY stop + destroy with zero delay.
 * With the old code this races the pending workers against the freed
 * session/response-queue structures (ASan catches it). With the fix,
 * destroy blocks until all 32 dispatches complete and returns clean.
 */

#define NUM_CLIENTS 32

static mcp_status_t noop_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)ctx;
    (void)session;
    (void)args;
    (void)user_data;
    if (result_out != NULL) {
        mcp_json_value_t *res = mcp_json_object_new(ctx);
        if (res != NULL) *result_out = res;
    }
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

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "inflight-destroy", "0.1.0");
    CHECK(server != NULL);
    /* One trivial tool so initialize dispatch has a fast handler target. */
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    CHECK(mcp_server_add_tool(ctx, server,
                              mcp_tool_new(ctx, "noop", "No-op", schema,
                                           noop_handler, NULL)) == MCP_OK);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 2);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .bind_addr = "127.0.0.1",
        .max_connections = 128,
        .keepalive_timeout_s = 5,
        .enable_websocket = true,
    };
    mcp_async_server_t *srv = mcp_async_server_create(ctx, server, pool, &cfg);
    CHECK(srv != NULL);
    uint16_t port = mcp_async_server_get_port(srv);
    CHECK(port > 0);

    /* Run the reactor in a dedicated thrd. */
    server_thread_arg_t sarg = {.ctx = ctx, .srv = srv};
    thrd_t sthrd;
    CHECK(thrd_create(&sthrd, server_run_worker, &sarg) == thrd_success);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    /* Open 32 real TCP connections; each sends a valid JSON-RPC
     * initialize POST. Do NOT wait for responses — the requests are
     * still queued in the reactor and may still be dispatching on
     * the 2 worker threads when we destroy below. */
    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"inflight\",\"version\":\"1.0\"}}}";

    char req[1024];
    int n = snprintf(req, sizeof(req),
                     "POST /mcp HTTP/1.1\r\n"
                     "Host: 127.0.0.1:%u\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n"
                     "%s",
                     port, strlen(init_body), init_body);
    CHECK(n > 0);

    int fds[NUM_CLIENTS];
    int failed = 0;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        int fd = tcp_connect(port);
        if (fd < 0 || write_all(fd, req, (size_t)n) != 0) {
            if (fd >= 0) close(fd);
            fd = -1;
            failed++;
        }
        fds[i] = fd;
    }
    /* All 32 sends must have hit the socket before the destroy window,
     * otherwise we are not actually exercising pending worker tasks. */
    CHECK(failed == 0);

    /* The red-light window: stop + destroy with ZERO delay. The old
     * code tore down sessions/response-queue here while worker
     * threads could still be inside their initialize dispatch and
     * calling mcp_async_server_enqueue_response on the freed state.
     * The fix makes mcp_async_server_destroy block (pending counter
     * + pthread_cond) until every in-flight worker task finishes. */
    mcp_async_server_stop(ctx, srv);
    mcp_async_server_destroy(ctx, srv);

    thrd_join(sthrd, NULL);
    for (int i = 0; i < NUM_CLIENTS; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }

    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);

    printf("test_async_destroy_inflight: OK (%d in-flight requests at destroy)\n",
           NUM_CLIENTS);
    return 0;
}
