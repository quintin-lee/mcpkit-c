#include "test_check.h"
#include <stddef.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/runtime/threadpool.h"

static void bump(mcp_context_t *ctx, void *arg) {
    (void)ctx;
    (*(int *)arg)++;
}

static void slow_bump(mcp_context_t *ctx, void *arg) {
    (void)ctx;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    thrd_sleep(&ts, NULL);
    (*(int *)arg)++;
}

static mcp_status_t echo_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                                 void *u, mcp_json_value_t **o) {
    (void)s;
    (void)u;
    const mcp_json_value_t *t = mcp_json_object_get(c, a, "text");
    const char *str = NULL;
    if (t == NULL || mcp_json_string_value(c, t, &str) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *out = mcp_json_object_new(c);
    mcp_json_value_t *sv = mcp_json_string_new(c, str);
    if (out == NULL || sv == NULL || mcp_json_object_set(c, out, "text", sv) != MCP_OK) {
        mcp_json_destroy(c, sv);
        mcp_json_destroy(c, out);
        return MCP_ERR_NOMEM;
    }
    *o = out;
    return MCP_OK;
}

#define NJOBS 8

typedef struct {
    mcp_context_t *ctx;
    mcp_server_t *srv;
    mcp_session_t *session;
    mcp_message_t *req;
    mcp_message_t *resp;
    mcp_status_t status;
} job_t;

static void dispatch_job(mcp_context_t *ctx, void *arg) {
    job_t *j = arg;
    (void)ctx;
    j->status = mcp_server_dispatch(j->ctx, j->srv, j->session, j->req, &j->resp);
    mcp_message_destroy(j->ctx, j->req);
    j->req = NULL;
}

static void run_dispatch_jobs(mcp_executor_t *ex, int *ok_count) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx);
    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1");
    CHECK(srv);
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "echo", "echo", NULL, echo_handler,
                                                     NULL)) == MCP_OK);
    job_t jobs[NJOBS];
    memset(jobs, 0, sizeof(jobs));
    for (int i = 0; i < NJOBS; i++) {
        jobs[i].ctx = ctx;
        jobs[i].srv = srv;
        jobs[i].session = mcp_server_create_session(ctx, srv);
        CHECK(jobs[i].session);
        mcp_json_value_t *init = mcp_initialize_params_new(ctx, "cli", "1");
        CHECK(init);
        mcp_message_t *ireq = mcp_request_new_number_id(ctx, 1000.0 + i, "initialize", init);
        CHECK(ireq);
        mcp_message_t *iresp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, jobs[i].session, ireq, &iresp) == MCP_OK);
        CHECK(mcp_message_result(ctx, iresp) != NULL);
        mcp_message_destroy(ctx, ireq);
        mcp_message_destroy(ctx, iresp);
        mcp_message_t *notif = mcp_initialized_notification_new(ctx);
        CHECK(notif);
        CHECK(mcp_server_notify(ctx, srv, jobs[i].session, notif) == MCP_OK);
        mcp_message_destroy(ctx, notif);

        mcp_json_value_t *args = mcp_json_object_new(ctx);
        mcp_json_value_t *tv = mcp_json_string_new(ctx, "hey");
        CHECK(args && tv && mcp_json_object_set(ctx, args, "text", tv) == MCP_OK);
        mcp_json_value_t *params = mcp_json_object_new(ctx);
        mcp_json_value_t *nm = mcp_json_string_new(ctx, "echo");
        CHECK(params && nm && mcp_json_object_set(ctx, params, "name", nm) == MCP_OK &&
               mcp_json_object_set(ctx, params, "arguments", args) == MCP_OK);
        jobs[i].req = mcp_request_new_number_id(ctx, 1.0 + i, "tools/call", params);
        CHECK(jobs[i].req);
        CHECK(mcp_executor_submit(ctx, ex, dispatch_job, &jobs[i]) == MCP_OK);
    }
    CHECK(mcp_executor_wait(ctx, ex) == MCP_OK);
    int ok = 0;
    for (int i = 0; i < NJOBS; i++) {
        CHECK(jobs[i].status == MCP_OK);
        CHECK(jobs[i].resp != NULL);
        const mcp_json_value_t *res = mcp_message_result(ctx, jobs[i].resp);
        CHECK(res);
        const char *bs = NULL;
        CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, res, "text"), &bs) ==
                   MCP_OK &&
               strcmp(bs, "hey") == 0);
        ok++;
        mcp_message_destroy(ctx, jobs[i].resp);
        mcp_server_destroy_session(ctx, srv, jobs[i].session);
    }
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    *ok_count = ok;
}

int main(void) {
    mcp_executor_t *ex = mcp_sync_executor_create(NULL);
    CHECK(ex != NULL);

    int n = 0;
    CHECK(mcp_executor_submit(NULL, ex, bump, &n) == MCP_OK);
    CHECK(n == 1);
    CHECK(mcp_executor_submit(NULL, ex, bump, &n) == MCP_OK);
    CHECK(mcp_executor_submit(NULL, ex, bump, &n) == MCP_OK);
    CHECK(mcp_executor_wait(NULL, ex) == MCP_OK);
    CHECK(n == 3);

    CHECK(mcp_executor_wait(NULL, ex) == MCP_OK);

    CHECK(mcp_executor_submit(NULL, ex, NULL, &n) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_executor_submit(NULL, NULL, bump, &n) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_executor_wait(NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_executor_backend(NULL, NULL) == NULL);
    CHECK(mcp_executor_backend(NULL, ex) == NULL);

    int sync_ok = 0;
    run_dispatch_jobs(ex, &sync_ok);
    CHECK(sync_ok == NJOBS);
    mcp_executor_destroy(NULL, ex);

    CHECK(mcp_threadpool_create(NULL, 0) == NULL);
    mcp_executor_t *tp = mcp_threadpool_create(NULL, 4);
    CHECK(tp != NULL);

    int m = 0;
    for (int i = 0; i < 32; i++) {
        CHECK(mcp_executor_submit(NULL, tp, bump, &m) == MCP_OK);
    }
    CHECK(mcp_executor_wait(NULL, tp) == MCP_OK);
    CHECK(m == 32);
    CHECK(mcp_executor_wait(NULL, tp) == MCP_OK);

    int fast = 0;
    int slow = 0;
    CHECK(mcp_executor_submit(NULL, tp, slow_bump, &slow) == MCP_OK);
    for (int i = 0; i < 4; i++) {
        CHECK(mcp_executor_submit(NULL, tp, bump, &fast) == MCP_OK);
    }
    CHECK(mcp_executor_wait(NULL, tp) == MCP_OK);
    CHECK(fast == 4 && slow == 1);

    int pool_ok = 0;
    run_dispatch_jobs(tp, &pool_ok);
    CHECK(pool_ok == NJOBS);
    mcp_executor_destroy(NULL, tp);

    mcp_executor_t *tp2 = mcp_threadpool_create(NULL, 2);
    CHECK(tp2 != NULL);
    int q = 0;
    CHECK(mcp_executor_submit(NULL, tp2, bump, &q) == MCP_OK);
    CHECK(mcp_executor_submit(NULL, tp2, bump, &q) == MCP_OK);
    mcp_executor_destroy(NULL, tp2);

    mcp_executor_destroy(NULL, NULL);
    return 0;
}
