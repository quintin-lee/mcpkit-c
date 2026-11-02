#define _POSIX_C_SOURCE 200809L

#include "test_check.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/runtime/reactor.h"

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    CHECK(flags >= 0);
    CHECK(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void test_invalid_args(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    CHECK(mcp_reactor_add(ctx, NULL, 0, MCP_EVENT_READ, (mcp_reactor_cb_t)1, NULL) ==
          MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_reactor_modify(ctx, NULL, 0, MCP_EVENT_READ) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_reactor_remove(ctx, NULL, 0) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_reactor_step(ctx, NULL, 0) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_reactor_run(ctx, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_reactor_wakeup(ctx, NULL) == MCP_ERR_INVALID_ARGUMENT);

    /* NULL-safe functions */
    mcp_reactor_stop(ctx, NULL);
    mcp_reactor_destroy(ctx, NULL);

    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    CHECK(mcp_reactor_add(ctx, r, -1, MCP_EVENT_READ, (mcp_reactor_cb_t)1, NULL) ==
          MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_reactor_add(ctx, r, 0, MCP_EVENT_READ, NULL, NULL) ==
          MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_reactor_modify(ctx, r, -1, MCP_EVENT_READ) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_reactor_remove(ctx, r, -1) == MCP_ERR_INVALID_ARGUMENT);

    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}

static void dummy_cb(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    (void)fd;
    (void)events;
    (void)user;
}

static void test_add_modify_remove(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    /* sv[0] is not added yet */
    CHECK(mcp_reactor_modify(ctx, r, sv[0], MCP_EVENT_WRITE) == MCP_ERR_NOT_FOUND);
    CHECK(mcp_reactor_remove(ctx, r, sv[0]) == MCP_ERR_NOT_FOUND);

    /* Add sv[0] */
    CHECK(mcp_reactor_add(ctx, r, sv[0], MCP_EVENT_READ, dummy_cb, NULL) == MCP_OK);

    /* Add again -> ALREADY_EXISTS */
    CHECK(mcp_reactor_add(ctx, r, sv[0], MCP_EVENT_READ, dummy_cb, NULL) ==
          MCP_ERR_ALREADY_EXISTS);

    /* Modify existing */
    CHECK(mcp_reactor_modify(ctx, r, sv[0], MCP_EVENT_WRITE) == MCP_OK);

    /* Remove existing */
    CHECK(mcp_reactor_remove(ctx, r, sv[0]) == MCP_OK);

    /* Remove again -> NOT_FOUND */
    CHECK(mcp_reactor_remove(ctx, r, sv[0]) == MCP_ERR_NOT_FOUND);

    close(sv[0]);
    close(sv[1]);
    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}

typedef struct {
    int read_count;
    int write_count;
    mcp_event_mask_t last_events;
    char read_buf[64];
    size_t read_bytes;
} event_stats_t;

static void io_callback(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    event_stats_t *st = (event_stats_t *)user;
    st->last_events = events;

    if (events & MCP_EVENT_WRITE) {
        st->write_count++;
    }
    if (events & MCP_EVENT_READ) {
        st->read_count++;
        ssize_t n = read(fd, st->read_buf + st->read_bytes,
                         sizeof(st->read_buf) - 1 - st->read_bytes);
        if (n > 0) {
            st->read_bytes += (size_t)n;
            st->read_buf[st->read_bytes] = '\0';
        }
    }
}

static void test_io_events(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nonblocking(sv[0]);
    set_nonblocking(sv[1]);

    event_stats_t stats0 = {0};
    event_stats_t stats1 = {0};

    CHECK(mcp_reactor_add(ctx, r, sv[0], MCP_EVENT_READ, io_callback, &stats0) == MCP_OK);
    CHECK(mcp_reactor_add(ctx, r, sv[1], MCP_EVENT_WRITE, io_callback, &stats1) == MCP_OK);

    /* sv[1] is write ready, sv[0] has no data */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);
    CHECK(stats1.write_count > 0);
    CHECK(stats0.read_count == 0);

    /* Write data from sv[1] to sv[0] */
    const char msg[] = "hello reactor";
    CHECK(write(sv[1], msg, sizeof(msg)) == (ssize_t)sizeof(msg));

    /* sv[0] should now be read ready */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);
    CHECK(stats0.read_count > 0);
    CHECK(strcmp(stats0.read_buf, msg) == 0);

    close(sv[0]);
    close(sv[1]);
    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}

typedef struct {
    mcp_reactor_t *reactor;
    int target_fd;
    bool removed;
} reentrant_ctx_t;

static void reentrant_remove_cb(mcp_context_t *ctx, int fd, mcp_event_mask_t events,
                                void *user) {
    (void)events;
    reentrant_ctx_t *rctx = (reentrant_ctx_t *)user;
    CHECK(mcp_reactor_remove(ctx, rctx->reactor, fd) == MCP_OK);
    rctx->removed = true;
}

static void test_reentrant_remove(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nonblocking(sv[0]);
    set_nonblocking(sv[1]);

    reentrant_ctx_t rctx = {
        .reactor = r,
        .target_fd = sv[0],
        .removed = false,
    };

    CHECK(mcp_reactor_add(ctx, r, sv[0], MCP_EVENT_READ, reentrant_remove_cb, &rctx) ==
          MCP_OK);

    CHECK(write(sv[1], "x", 1) == 1);

    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);
    CHECK(rctx.removed == true);

    /* After removal, another step should not invoke it and fd should not be in reactor */
    CHECK(mcp_reactor_remove(ctx, r, sv[0]) == MCP_ERR_NOT_FOUND);

    close(sv[0]);
    close(sv[1]);
    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}

typedef struct {
    mcp_context_t *ctx;
    mcp_reactor_t *r;
    int delay_ms;
} stop_thread_arg_t;

static int stop_thread_fn(void *arg) {
    stop_thread_arg_t *a = (stop_thread_arg_t *)arg;
    struct timespec ts = {
        .tv_sec = a->delay_ms / 1000,
        .tv_nsec = (a->delay_ms % 1000) * 1000000L,
    };
    thrd_sleep(&ts, NULL);
    mcp_reactor_stop(a->ctx, a->r);
    return 0;
}

static void test_multithread_stop_and_wakeup(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    /* Test 1: Immediate wakeup */
    CHECK(mcp_reactor_wakeup(ctx, r) == MCP_OK);
    /* Step with large timeout should return promptly because wakeup fd is signaled */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(mcp_reactor_step(ctx, r, 3000) == MCP_OK);
    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000L +
                      (end.tv_nsec - start.tv_nsec) / 1000000L;
    CHECK(elapsed_ms < 1000L);

    /* Test 2: Background thread stops reactor running in mcp_reactor_run */
    stop_thread_arg_t arg = {
        .ctx = ctx,
        .r = r,
        .delay_ms = 50,
    };
    thrd_t t;
    CHECK(thrd_create(&t, stop_thread_fn, &arg) == thrd_success);

    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(mcp_reactor_run(ctx, r) == MCP_OK);
    clock_gettime(CLOCK_MONOTONIC, &end);
    elapsed_ms = (end.tv_sec - start.tv_sec) * 1000L +
                 (end.tv_nsec - start.tv_nsec) / 1000000L;
    CHECK(elapsed_ms < 1000L);

    thrd_join(t, NULL);

    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}

static void test_timeout_step(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(mcp_reactor_step(ctx, r, 30) == MCP_OK);
    clock_gettime(CLOCK_MONOTONIC, &end);
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000L +
                      (end.tv_nsec - start.tv_nsec) / 1000000L;
    CHECK(elapsed_ms >= 20L && elapsed_ms <= 300L);

    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}

typedef struct {
    mcp_context_t *ctx;
    mcp_reactor_t *r;
    int target_to_remove;
    bool cb1_fired;
    bool cb2_fired;
} sibling_test_ctx_t;

static void on_first_read(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    (void)fd;
    (void)events;
    sibling_test_ctx_t *tc = (sibling_test_ctx_t *)user;
    tc->cb1_fired = true;
    CHECK(mcp_reactor_remove(tc->ctx, tc->r, tc->target_to_remove) == MCP_OK);
}

static void on_second_read(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    (void)fd;
    (void)events;
    sibling_test_ctx_t *tc = (sibling_test_ctx_t *)user;
    tc->cb2_fired = true;
}

static void test_reentrant_remove_sibling(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    int sv1[2], sv2[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv1) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);

    /* Determine lower and higher fd numbers to guarantee dispatch order */
    int first_fd = (sv1[0] < sv2[0]) ? sv1[0] : sv2[0];
    int first_peer = (sv1[0] < sv2[0]) ? sv1[1] : sv2[1];
    int second_fd = (sv1[0] < sv2[0]) ? sv2[0] : sv1[0];
    int second_peer = (sv1[0] < sv2[0]) ? sv2[1] : sv1[1];

    sibling_test_ctx_t tc = {
        .ctx = ctx,
        .r = r,
        .target_to_remove = second_fd,
        .cb1_fired = false,
        .cb2_fired = false
    };

    CHECK(mcp_reactor_add(ctx, r, first_fd, MCP_EVENT_READ, on_first_read, &tc) == MCP_OK);
    CHECK(mcp_reactor_add(ctx, r, second_fd, MCP_EVENT_READ, on_second_read, &tc) == MCP_OK);

    /* Write to first_peer then second_peer so both are ready */
    char byte = 'x';
    CHECK(write(first_peer, &byte, 1) == 1);
    CHECK(write(second_peer, &byte, 1) == 1);

    /* Step reactor: first_fd fires and removes second_fd; second_fd must NOT fire */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);
    CHECK(tc.cb1_fired == true);
    CHECK(tc.cb2_fired == false);

    close(sv1[0]); close(sv1[1]);
    close(sv2[0]); close(sv2[1]);
    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}

typedef struct {
    mcp_context_t *ctx;
    mcp_reactor_t *r;
    int new_fd_to_add;
    bool new_cb_fired;
} add_test_ctx_t;

static void on_dynamic_read(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)ctx;
    (void)fd;
    (void)events;
    add_test_ctx_t *tc = (add_test_ctx_t *)user;
    tc->new_cb_fired = true;
}

static void on_trigger_add(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user) {
    (void)events;
    char b;
    CHECK(read(fd, &b, 1) == 1);
    add_test_ctx_t *tc = (add_test_ctx_t *)user;
    CHECK(mcp_reactor_add(ctx, tc->r, tc->new_fd_to_add, MCP_EVENT_READ, on_dynamic_read, tc) == MCP_OK);
}

static void test_reentrant_add_within_cb(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_reactor_t *r = mcp_reactor_create(ctx);
    CHECK(r != NULL);

    /* Fill reactor to initial capacity (16 descriptors) so next add triggers rt_realloc */
    int fill_sv[15][2];
    for (int i = 0; i < 15; i++) {
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fill_sv[i]) == 0);
        CHECK(mcp_reactor_add(ctx, r, fill_sv[i][0], MCP_EVENT_READ, dummy_cb, NULL) == MCP_OK);
    }

    int sv_trigger[2], sv_overflow[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_trigger) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv_overflow) == 0);

    add_test_ctx_t tc = {
        .ctx = ctx,
        .r = r,
        .new_fd_to_add = sv_overflow[0],
        .new_cb_fired = false
    };

    /* 16th descriptor: capacity reaches 16 */
    CHECK(mcp_reactor_add(ctx, r, sv_trigger[0], MCP_EVENT_READ, on_trigger_add, &tc) == MCP_OK);

    char byte = 'a';
    CHECK(write(sv_trigger[1], &byte, 1) == 1);
    CHECK(write(sv_overflow[1], &byte, 1) == 1);

    /* First step: on_trigger_add fires and dynamically adds 17th fd (sv_overflow[0]), triggering realloc */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);

    /* Second step: sv_overflow[0] should now be active and fire */
    CHECK(mcp_reactor_step(ctx, r, 50) == MCP_OK);
    CHECK(tc.new_cb_fired == true);

    for (int i = 0; i < 15; i++) {
        close(fill_sv[i][0]); close(fill_sv[i][1]);
    }
    close(sv_trigger[0]); close(sv_trigger[1]);
    close(sv_overflow[0]); close(sv_overflow[1]);
    mcp_reactor_destroy(ctx, r);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_invalid_args();
    test_add_modify_remove();
    test_io_events();
    test_reentrant_remove();
    test_reentrant_remove_sibling();
    test_reentrant_add_within_cb();
    test_multithread_stop_and_wakeup();
    test_timeout_step();
    printf("test_reactor: all assertions passed\n");
    return 0;
}
