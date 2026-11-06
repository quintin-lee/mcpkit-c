#include "test_check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/server/dispatcher.h"

/*
 * Red-light gate for the queue_grow NOMEM fix in src/server/queue.c:
 * queue_grow first reallocs q->msgs and only then q->sessions. The
 * pre-fix path left q->msgs pointing at the freshly allocated buffer
 * while q->cap still held the old value whenever the sessions realloc
 * failed, producing a dangling-pointer / divide-by-zero hazard in the
 * ring math. The fixed path rolls q->msgs back to the old buffer and
 * returns MCP_ERR_NOMEM cleanly.
 *
 * The allocator-failure injection follows test_alloc_fail.c: a
 * fail-after-N allocator wired into the context, swept over every
 * allocation index. The sweep is iteration-independent: one success
 * pass counts all allocations across the entire pre-state build
 * (fresh context, server, session, queue, and 8-message prefill) plus
 * the final push, and for each failure index n the whole pre-state is
 * rebuilt from scratch on a fresh context with the gate armed ONLY for
 * the final push — so allocations added by mcp_server_create_session's
 * per-server sessions-array growth can never leak in and skew the
 * calibration. The queue is pre-loaded so the final push takes the
 * true grow path (len == cap, both reallocs run); every N in the sweep
 * must yield either a clean MCP_OK or a clean MCP_ERR_NOMEM with the
 * queue still fully usable afterwards. A 100000-item over-wrap FIFO
 * stress locks in the ring-math regression.
 */
static size_t g_count;
static size_t g_fail_at = SIZE_MAX;

static void *fail_malloc(size_t n, void *ud) {
    (void)ud;
    if (g_count++ >= g_fail_at) {
        return NULL;
    }
    return malloc(n);
}

static void *fail_calloc(size_t nmemb, size_t size, void *ud) {
    (void)ud;
    if (g_count++ >= g_fail_at) {
        return NULL;
    }
    return calloc(nmemb, size);
}

static void *fail_realloc(void *ptr, size_t size, void *ud) {
    (void)ud;
    if (g_count++ >= g_fail_at) {
        return NULL;
    }
    return realloc(ptr, size);
}

static void fail_free(void *p, void *ud) {
    (void)ud;
    free(p);
}

static const mcp_allocator_t kFailAlloc = {
    .malloc_fn = fail_malloc,
    .free_fn = fail_free,
    .calloc_fn = fail_calloc,
    .realloc_fn = fail_realloc,
    .userdata = NULL,
};

static mcp_context_t *fail_ctx(void) {
    mcp_context_config_t cfg = {.allocator = &kFailAlloc, .logger = NULL,
                                .json_backend = NULL};
    mcp_context_t *ctx = mcp_context_create(&cfg);
    CHECK(ctx != NULL);
    return ctx;
}

static mcp_session_t *mk_session(mcp_context_t *ctx, mcp_server_t *srv) {
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);
    return s;
}

static mcp_message_t *mk_msg(mcp_context_t *ctx, const char *id) {
    return mcp_request_new_string_id(ctx, id, "ping", NULL);
}

/* Build the complete pre-state for the sweep: fresh context, server,
 * session, and a queue pre-loaded to len == cap so the final push must
 * take the true grow path. The first push grows cap 0->8, and once 8
 * items are in the ring (len == cap, head may be non-zero) the next
 * push must run BOTH reallocs inside queue_grow (rewind is
 * allocation-free). The entire sequence runs with the gate off, so the
 * caller can arm it just before the final push. */
static mcp_queue_t *prefill(mcp_context_t *ctx, mcp_server_t *srv,
                            mcp_session_t **s_out) {
    mcp_session_t *s = mk_session(ctx, srv);
    *s_out = s;
    mcp_queue_t *q = mcp_queue_create(ctx);
    CHECK(q != NULL);
    for (size_t i = 0; i < 8; i++) {
        char id[8];
        snprintf(id, sizeof(id), "%zu", i);
        mcp_message_t *m = mk_msg(ctx, id);
        CHECK(m != NULL);
        CHECK(mcp_queue_push(ctx, q, s, m) == MCP_OK);
    }
    return q;
}

static void drain_fifo(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *s,
                       size_t expect_len) {
    for (size_t i = 0; i < expect_len; i++) {
        mcp_message_t *m = NULL;
        CHECK(mcp_queue_pop(ctx, q, &s, &m) == MCP_OK);
        mcp_message_destroy(ctx, m);
    }
    mcp_message_t *tmp = NULL;
    CHECK(mcp_queue_pop(ctx, q, &s, &tmp) == MCP_ERR_NOT_FOUND);
}

/* The NOMEM sweep gate: at every allocation failure index 0..total,
 * one final push into a full queue must return either MCP_OK or a clean
 * MCP_ERR_NOMEM; after every NOMEM the queue must still be fully
 * usable (FIFO intact). The pre-fix code left cap inconsistent on the
 * sessions-realloc failure and the ring math then hit divide-by-zero. */
static void test_queue_grow_nomem(void) {
    /* Calibration pass: build the exact pre-state on a fresh context
     * (gate off), then measure the number of allocations the FINAL
     * push needs on its own (mk_msg("over") + mcp_queue_push). That is
     * the range the sweep's gate covers: g_count is reset to 0 right
     * before that final push in every sweep iteration, so g_fail_at = n
     * with n in 0..push_cost targets every allocation point inside it,
     * which is where queue_grow's two reallocs live. */
    {
        mcp_context_t *ctx = fail_ctx();
        mcp_server_t *srv = mcp_server_create(ctx, "q", "1");
        CHECK(srv != NULL);
        g_fail_at = SIZE_MAX;
        mcp_session_t *s = NULL;
        mcp_queue_t *q = prefill(ctx, srv, &s);
        mcp_message_t *over = mk_msg(ctx, "over");
        CHECK(over != NULL);
        g_count = 0;
        CHECK(mcp_queue_push(ctx, q, s, over) == MCP_OK);
        size_t push_cost = g_count; /* allocs for the grow path itself */
        g_fail_at = SIZE_MAX;
        drain_fifo(ctx, q, s, 9);
        mcp_queue_destroy(ctx, q);
        mcp_server_destroy_session(ctx, srv, s);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);

        /* Sweep every allocation point inside the final push. Each
         * iteration rebuilds the identical pre-state from scratch on a
         * fresh context (gate off), then arms g_count = 0 / g_fail_at = n
         * for mcp_queue_push only — the proven test_alloc_fail.c
         * pattern, scoped to the window that actually contains
         * queue_grow's two reallocs. */
        for (size_t n = 0; n <= push_cost; n++) {
            ctx = fail_ctx();
            mcp_server_t *srv2 = mcp_server_create(ctx, "q", "1");
            CHECK(srv2 != NULL);
            g_fail_at = SIZE_MAX;
            mcp_session_t *s2 = NULL;
            mcp_queue_t *q2 = prefill(ctx, srv2, &s2);
            /* Build over with the gate still off so it exists; only the
             * push is gated. */
            mcp_message_t *over2 = mk_msg(ctx, "over");
            CHECK(over2 != NULL);
            g_count = 0;
            g_fail_at = n;
            mcp_status_t st = mcp_queue_push(ctx, q2, s2, over2);
            g_fail_at = SIZE_MAX;
            CHECK(st == MCP_OK || st == MCP_ERR_NOMEM);
            /* Gate-free teardown: every destructor below may still
             * allocate or free through the context allocator, so the
             * gate must stay off or the sweep's own cleanup crashes. */
            g_fail_at = SIZE_MAX;
            if (st == MCP_OK) {
                /* over was consumed by the queue; the ring holds 9. */
                drain_fifo(ctx, q2, s2, 9);
            } else {
                /* Grow failed: over was NOT consumed; queue still holds 8. */
                drain_fifo(ctx, q2, s2, 8);
                mcp_message_destroy(ctx, over2);
            }
            mcp_queue_destroy(ctx, q2);
            mcp_server_destroy_session(ctx, srv2, s2);
            mcp_server_destroy(ctx, srv2);
            mcp_context_destroy(ctx);
        }
    }
}

/* Large over-wrap FIFO stress on a plain context: push 100000
 * messages, drain them all, push 100000 more, and verify every id
 * comes back in strict FIFO order. This hammers queue_grow's fast
 * path and the ring math repeatedly. */
static void test_queue_fifo_stress(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "q2", "1");
    CHECK(srv != NULL);
    mcp_session_t *s = mk_session(ctx, srv);
    mcp_queue_t *q = mcp_queue_create(ctx);
    CHECK(q != NULL);

    const size_t kN = MCP_QUEUE_MAX_LEN + 1; /* last push hits the cap guard */
    char **expected = malloc(kN * sizeof(char *));
    CHECK(expected != NULL);
    for (size_t i = 0; i < kN; i++) {
        char id[32];
        snprintf(id, sizeof(id), "msg-%zu", i);
        expected[i] = strdup(id);
        CHECK(expected[i] != NULL);
    }

    for (size_t round = 0; round < 2; round++) {
        for (size_t i = 0; i < kN; i++) {
            mcp_message_t *m = mcp_request_new_string_id(ctx, expected[i], "ping", NULL);
            CHECK(m != NULL);
            mcp_status_t st = mcp_queue_push(ctx, q, s, m);
            if (i == kN - 1) {
                /* Cap boundary: the (MAX_LEN+1)-th push (len already ==
                 * MAX_LEN) is rejected by the MCP_QUEUE_MAX_LEN guard. */
                CHECK(st == MCP_ERR_NOMEM);
                mcp_message_destroy(ctx, m);
            } else {
                CHECK(st == MCP_OK);
            }
        }
        CHECK(mcp_queue_size(ctx, q) == kN - 1);
        for (size_t i = 0; i < kN - 1; i++) {
            mcp_message_t *m = NULL;
            CHECK(mcp_queue_pop(ctx, q, &s, &m) == MCP_OK);
            const char *got = mcp_message_id_string(ctx, m);
            CHECK(got != NULL && strcmp(got, expected[i]) == 0);
            mcp_message_destroy(ctx, m);
        }
    }
    {
        mcp_message_t *tmp = NULL;
        CHECK(mcp_queue_pop(ctx, q, &s, &tmp) == MCP_ERR_NOT_FOUND);
    }

    for (size_t i = 0; i < kN; i++) {
        free(expected[i]);
    }
    free(expected);

    mcp_queue_destroy(ctx, q);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

int main(void) {
    test_queue_grow_nomem();
    test_queue_fifo_stress();
    printf("test_queue_nomem: OK\n");
    return 0;
}
