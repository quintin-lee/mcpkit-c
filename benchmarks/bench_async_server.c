#include "bench_common.h"
#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/runtime/executor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_child_stop = 0;

static void child_sigterm_handler(int sig) {
    (void)sig;
    g_child_stop = 1;
}

static mcp_status_t async_echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                       const mcp_json_value_t *args, void *user_data,
                                       mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *tv = mcp_json_object_get(ctx, args, "text");
    const char *text = "pong";
    if (tv != NULL) {
        (void)mcp_json_string_value(ctx, tv, &text);
    }
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *res = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, item, "type", mcp_json_string_new(ctx, "text"));
    mcp_json_object_set_take(ctx, item, "text", mcp_json_string_new(ctx, text != NULL ? text : ""));
    mcp_json_array_append(ctx, content, item);
    mcp_json_object_set_take(ctx, res, "content", content);
    *result_out = res;
    return MCP_OK;
}

static void run_child_server(int pipe_write_fd) {
    signal(SIGTERM, child_sigterm_handler);
    signal(SIGINT, child_sigterm_handler);

    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) _exit(1);

    mcp_server_t *server = mcp_server_create(ctx, "async-bench-srv", "1.0.0");
    if (server == NULL) _exit(1);

    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx));
    mcp_server_add_tool(ctx, server, mcp_tool_new(ctx, "echo", "Echo text", schema, async_echo_handler, NULL));

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    if (pool == NULL) _exit(1);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .bind_addr = "127.0.0.1",
        .max_connections = 10000,
        .keepalive_timeout_s = 300,
        .enable_websocket = true,
        .enable_sse = true,
    };

    mcp_async_server_t *srv = mcp_async_server_create(ctx, server, pool, &cfg);
    if (srv == NULL) _exit(1);

    uint16_t port = mcp_async_server_get_port(srv);
    ssize_t nw = write(pipe_write_fd, &port, sizeof(port));
    close(pipe_write_fd);
    if (nw != sizeof(port)) _exit(1);

    while (!g_child_stop) {
        mcp_async_server_step(ctx, srv, 100);
    }

    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);
    _exit(0);
}

int main(int argc, char **argv) {
    size_t target_connections = 500;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            target_connections = 50;
        } else if (strncmp(argv[i], "--connections=", 14) == 0) {
            target_connections = (size_t)strtoul(argv[i] + 14, NULL, 10);
            if (target_connections == 0) target_connections = 10;
        }
    }

    int pfd[2];
    if (pipe(pfd) != 0) {
        perror("pipe");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Child process: async server */
        close(pfd[0]);
        run_child_server(pfd[1]);
        return 0;
    }

    /* Parent process: benchmark driver */
    close(pfd[1]);
    uint16_t port = 0;
    if (read(pfd[0], &port, sizeof(port)) != sizeof(port) || port == 0) {
        fprintf(stderr, "Failed to receive bound port from child server\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(pfd[0]);
        return 1;
    }
    close(pfd[0]);

    /* Give server a brief moment to initialize reactor */
    usleep(20000);

    size_t rss_baseline = bench_get_rss_kb(pid);

    int *client_fds = (int *)malloc(target_connections * sizeof(int));
    if (client_fds == NULL) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

    size_t connected = 0;
    uint64_t *latencies = (uint64_t *)malloc(target_connections * sizeof(uint64_t));
    size_t latency_count = 0;

    uint64_t t_start = bench_now_ns();
    for (size_t i = 0; i < target_connections; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) break;
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        uint64_t conn_t0 = bench_now_ns();
        if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) != 0) {
            close(fd);
            break;
        }
        uint64_t conn_t1 = bench_now_ns();
        client_fds[connected++] = fd;
        if (latencies != NULL) {
            latencies[latency_count++] = conn_t1 - conn_t0;
        }
    }
    uint64_t t_end = bench_now_ns();
    double duration_sec = (double)(t_end - t_start) / 1000000000.0;

    /* Sample server RSS under full connection load */
    usleep(50000);
    size_t rss_loaded = bench_get_rss_kb(pid);

    double bytes_per_conn = 0.0;
    if (connected > 0 && rss_loaded >= rss_baseline) {
        bytes_per_conn = ((double)(rss_loaded - rss_baseline) * 1024.0) / (double)connected;
    }

    printf("\n===================================================\n");
    printf("Benchmark: process_isolated_async_server\n");
    printf("---------------------------------------------------\n");
    printf("Child Server PID:        %d\n", (int)pid);
    printf("Server Port:             %u\n", (unsigned)port);
    printf("Concurrent Connections:  %zu\n", connected);
    printf("Connection Setup Time:   %.3f s\n", duration_sec);
    printf("Server Baseline RSS:     %zu KB\n", rss_baseline);
    printf("Server Loaded RSS:       %zu KB\n", rss_loaded);
    printf("Delta RSS (Server):      %zd KB\n", (ssize_t)(rss_loaded - rss_baseline));
    printf("Estimated RSS / Conn:    %.1f bytes\n", bytes_per_conn);

    bench_stats_t conn_stats;
    bench_calc_stats("async_server_tcp_connect_latency", latencies, latency_count, duration_sec, rss_loaded, &conn_stats);
    printf("TCP Connect Latency:\n");
    printf("  Min: %.2f us, P50: %.2f us, P90: %.2f us, P99: %.2f us, Max: %.2f us\n",
           (double)conn_stats.min_ns / 1000.0, (double)conn_stats.p50_ns / 1000.0,
           (double)conn_stats.p90_ns / 1000.0, (double)conn_stats.p99_ns / 1000.0,
           (double)conn_stats.max_ns / 1000.0);
    printf("JSON: {\"benchmark\":\"process_isolated_async_server\",\"pid\":%d,\"connections\":%zu,"
           "\"duration_sec\":%.4f,\"rss_baseline_kb\":%zu,\"rss_loaded_kb\":%zu,"
           "\"bytes_per_connection\":%.1f,\"connect_p50_us\":%.1f,\"connect_p99_us\":%.1f}\n",
           (int)pid, connected, duration_sec, rss_baseline, rss_loaded, bytes_per_conn,
           (double)conn_stats.p50_ns / 1000.0, (double)conn_stats.p99_ns / 1000.0);
    printf("===================================================\n\n");

    /* Cleanup client sockets */
    for (size_t i = 0; i < connected; i++) {
        close(client_fds[i]);
    }
    free(client_fds);
    free(latencies);

    /* Cleanly terminate child server */
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);

    return 0;
}
