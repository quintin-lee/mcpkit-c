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

typedef struct {
    mcp_context_t *ctx;
    mcp_async_server_t *srv;
} server_thread_arg_t;

static int run_server(void *a) {
    server_thread_arg_t *s = (server_thread_arg_t *)a;
    mcp_async_server_run(s->ctx, s->srv);
    return 0;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *srv = mcp_server_create(ctx, "leak", "0.1.0");
    CHECK(srv != NULL);
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {.port = 0,
                                     .bind_addr = "127.0.0.1",
                                     .max_connections = 400,
                                     .keepalive_timeout_s = 5,
                                     .enable_websocket = true,
                                     .enable_sse = true};
    mcp_async_server_t *s = mcp_async_server_create(ctx, srv, pool, &cfg);
    CHECK(s != NULL);
    uint16_t port = mcp_async_server_get_port(s);
    CHECK(port > 0);

    server_thread_arg_t sarg = {.ctx = ctx, .srv = s};
    thrd_t t;
    CHECK(thrd_create(&t, run_server, &sarg) == thrd_success);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    for (int i = 0; i < 300; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            const char *b = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                            "\"params\":{\"protocolVersion\":\"2025-06-18\","
                            "\"capabilities\":{},\"clientInfo\":{\"name\":\"l\","
                            "\"version\":\"0\"}}}";
            char req[512];
            snprintf(req, sizeof req,
                     "POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n%s",
                     strlen(b), b);
            write(fd, req, strlen(req));
            char tmp[16];
            (void)!read(fd, tmp, sizeof tmp);
        }
        close(fd);
    }
    /* Drive the reactor so it reaps all closed connections. */
    for (int i = 0; i < 100; i++) {
        size_t active = mcp_async_server_active_connections(s);
        if (active == 0) break;
        struct timespec dts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
        thrd_sleep(&dts, NULL);
    }
    CHECK(mcp_async_server_active_connections(s) == 0);

    mcp_async_server_stop(ctx, s);
    thrd_join(t, NULL);
    mcp_async_server_destroy(ctx, s);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("test_async_leak: OK\n");
    return 0;
}
