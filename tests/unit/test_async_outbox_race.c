#include "test_check.h"

#include <stdatomic.h>
#include <stdio.h>
#include <threads.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"
#include "mcpkit/runtime/threadpool.h"

/* Shared state for the outbox race. All threads share the same
 * mcp_server_t; writers push via mcp_server_notify_client, readers
 * pop + destroy via mcp_server_outbox_pop. No barrier needed — each
 * thread starts as soon as thrd_create returns; the race is the point. */
#define NUM_WRITERS      8
#define NUM_READERS      4
#define ITERS_PER_THREAD 500

static atomic_int g_total_pushed = 0;
static atomic_int g_total_popped = 0;

typedef struct {
    mcp_context_t *ctx;
    mcp_server_t *server;
} worker_arg_t;

typedef struct {
    mcp_context_t *ctx;
    mcp_async_server_t *srv;
} server_thread_arg_t;

static int server_run_worker(void *arg) {
    server_thread_arg_t *a = (server_thread_arg_t *)arg;
    mcp_async_server_run(a->ctx, a->srv);
    return 0;
}

static int writer_worker(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    mcp_context_t *ctx = wa->ctx;
    mcp_server_t *srv = wa->server;
    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        if (mcp_server_notify_client(ctx, srv, "x", NULL) == MCP_OK) {
            atomic_fetch_add_explicit(&g_total_pushed, 1, memory_order_relaxed);
        }
    }
    return 0;
}

static int reader_worker(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    mcp_context_t *ctx = wa->ctx;
    mcp_server_t *srv = wa->server;
    for (int i = 0; i < ITERS_PER_THREAD; i++) {
        mcp_message_t *msg = NULL;
        if (mcp_server_outbox_pop(ctx, srv, &msg) == MCP_OK && msg != NULL) {
            mcp_message_destroy(ctx, msg);
            atomic_fetch_add_explicit(&g_total_popped, 1, memory_order_relaxed);
        }
    }
    return 0;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    mcp_server_t *server = mcp_server_create(ctx, "outbox_race", "1.0.0");
    CHECK(server != NULL);

    mcp_executor_t *pool = mcp_threadpool_create(ctx, 2);
    CHECK(pool != NULL);

    mcp_async_server_config_t cfg = {
        .port = 0,
        .bind_addr = "127.0.0.1",
        .max_connections = 64,
        .keepalive_timeout_s = 3,
        .enable_websocket = true,
        .enable_sse = true,
    };
    mcp_async_server_t *srv = mcp_async_server_create(ctx, server, pool, &cfg);
    CHECK(srv != NULL);

    /* Reactor in a dedicated thrd so the outbox is exercised while the
     * event loop is live (a real production scenario). */
    thrd_t sthrd;
    server_thread_arg_t sarg = {.ctx = ctx, .srv = srv};
    CHECK(thrd_create(&sthrd, server_run_worker, &sarg) == thrd_success);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);

    const int NWORKERS = NUM_WRITERS + NUM_READERS;
    thrd_t threads[NWORKERS];
    worker_arg_t args[NWORKERS];
    int t = 0;

    for (int i = 0; i < NWORKERS; i++) {
        args[i] = (worker_arg_t){.ctx = ctx, .server = server};
        int rc = (i < NUM_WRITERS) ? thrd_create(&threads[t], writer_worker, &args[i])
                                   : thrd_create(&threads[t], reader_worker, &args[i]);
        CHECK(rc == thrd_success);
        t++;
    }

    for (int i = 0; i < NWORKERS; i++) {
        thrd_join(threads[i], NULL);
    }

    /* Drain anything the readers did not pop: writers may have outrun
     * readers. No message can be popped more times than it was pushed
     * — that invariant catches double-free / lost-slot bugs. */
    int drained = 0;
    for (;;) {
        mcp_message_t *msg = NULL;
        if (mcp_server_outbox_pop(ctx, server, &msg) != MCP_OK || msg == NULL) {
            break;
        }
        mcp_message_destroy(ctx, msg);
        g_total_popped++;
        drained++;
    }

    int pushed = atomic_load(&g_total_pushed);
    int popped = atomic_load(&g_total_popped);
    CHECK(popped <= pushed);
    CHECK(pushed > 0);

    mcp_async_server_stop(ctx, srv);
    thrd_join(sthrd, NULL);

    mcp_async_server_destroy(ctx, srv);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, server);
    mcp_context_destroy(ctx);

    printf("test_async_outbox_race: OK (%d pushed, %d popped, %d drained post-join)\n",
           pushed, popped, drained);
    return 0;
}
