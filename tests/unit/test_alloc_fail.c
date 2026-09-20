#include "test_check.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

/*
 * Fail-after-N allocator: allocations (malloc/calloc/realloc) succeed
 * while g_count < g_fail_at, then return NULL. free passes through.
 * Setup/teardown phases run with g_fail_at = SIZE_MAX (never fail);
 * the target call runs with g_count = 0 and g_fail_at = N.
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

/* ---------- ui path: mcp_apps_result_with_ui ---------- */

static void sweep_ui(void) {
    /* Calibrate: count allocations used by one successful call. */
    mcp_context_t *ctx = fail_ctx();
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    CHECK(result != NULL);
    g_count = 0;
    CHECK(mcp_apps_result_with_ui(ctx, result, "ui://x") == MCP_OK);
    size_t total = g_count;
    mcp_json_destroy(ctx, result);
    mcp_context_destroy(ctx);

    for (size_t n = 0; n <= total; n++) {
        ctx = fail_ctx();
        result = mcp_json_object_new(ctx);
        CHECK(result != NULL);
        g_count = 0;
        g_fail_at = n;
        mcp_status_t st = mcp_apps_result_with_ui(ctx, result, "ui://x");
        g_fail_at = SIZE_MAX;
        /* Only OK (attached) or NOMEM (clean rollback) are legal. */
        CHECK(st == MCP_OK || st == MCP_ERR_NOMEM);
        mcp_json_destroy(ctx, result);
        mcp_context_destroy(ctx);
    }
}

/* ---------- client path: mcp_client_complete ---------- */

typedef struct {
    const char **script;
    size_t nscript;
    size_t cursor;
} cli_fake_t;

static mcp_status_t cli_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static mcp_status_t cli_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                             size_t len) {
    (void)ctx;
    (void)t;
    (void)data;
    (void)len;
    return MCP_OK;
}

static mcp_status_t cli_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    cli_fake_t *f = (cli_fake_t *)mcp_transport_backend(ctx, t);
    CHECK(f->cursor < f->nscript);
    const char *line = f->script[f->cursor++];
    size_t n = strlen(line) + 1;
    char *buf = mcp_context_allocator(ctx)->malloc_fn(n, mcp_context_allocator(ctx)->userdata);
    if (buf == NULL) {
        return MCP_ERR_NOMEM;
    }
    memcpy(buf, line, n);
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t cli_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static const mcp_transport_ops_t kCliFake = {cli_start, cli_send, cli_recv, cli_stop};

static void sweep_client(void) {
    static const char *kResp[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"completions\":[]}}",
    };

    /* Calibrate with args == NULL (covers the fixed chained-set block). */
    mcp_context_t *ctx = fail_ctx();
    cli_fake_t fake = {.script = kResp, .nscript = 1, .cursor = 0};
    mcp_transport_t *t = mcp_transport_create(ctx, &kCliFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    mcp_json_value_t *out = NULL;
    g_count = 0;
    CHECK(mcp_client_complete(ctx, c, "prompt/greet", NULL, &out) == MCP_OK);
    size_t total = g_count;
    CHECK(out != NULL);
    mcp_json_destroy(ctx, out);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);

    for (size_t n = 0; n <= total; n++) {
        ctx = fail_ctx();
        fake.cursor = 0;
        t = mcp_transport_create(ctx, &kCliFake, &fake);
        CHECK(t != NULL);
        c = mcp_client_create(ctx, t);
        CHECK(c != NULL);
        out = NULL;
        g_count = 0;
        g_fail_at = n;
        mcp_status_t st = mcp_client_complete(ctx, c, "prompt/greet", NULL, &out);
        g_fail_at = SIZE_MAX;
        if (st == MCP_OK) {
            CHECK(out != NULL);
            mcp_json_destroy(ctx, out);
        } else {
            /* Any error (NOMEM, PROTOCOL from a gated parse, ...) is legal
               as long as no partial result escapes. */
            CHECK(out == NULL);
        }
        mcp_client_destroy(ctx, c);
        mcp_transport_destroy(ctx, t);
        mcp_context_destroy(ctx);
    }

    /* One args != NULL success path (attach takes ownership). */
    ctx = fail_ctx();
    fake.cursor = 0;
    t = mcp_transport_create(ctx, &kCliFake, &fake);
    CHECK(t != NULL);
    c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    mcp_json_value_t *args = mcp_json_object_new(ctx);
    CHECK(args != NULL);
    out = NULL;
    CHECK(mcp_client_complete(ctx, c, "prompt/greet", args, &out) == MCP_OK);
    CHECK(out != NULL);
    mcp_json_destroy(ctx, out);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
}

/* ---------- dispatcher path: completion provider arg clone ---------- */

static mcp_json_value_t *sweep_provider(mcp_context_t *ctx, mcp_session_t *s,
                                       const mcp_json_value_t *ref, void *ud) {
    (void)s;
    (void)ref;
    (void)ud;
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    mcp_json_value_t *e = arr != NULL ? mcp_json_string_new(ctx, "x") : NULL;
    if (arr == NULL || e == NULL || mcp_json_array_append(ctx, arr, e) != MCP_OK) {
        mcp_json_destroy(ctx, e);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    return arr;
}

static mcp_message_t *sweep_complete_req(mcp_context_t *ctx) {
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *ref = params != NULL ? mcp_json_object_new(ctx) : NULL;
    mcp_json_value_t *type = mcp_json_string_new(ctx, "ref");
    mcp_json_value_t *val = mcp_json_string_new(ctx, "prompt/greet");
    mcp_json_value_t *arg = mcp_json_object_new(ctx);
    if (!params || !ref || !type || !val || !arg ||
        mcp_json_object_set(ctx, ref, "type", type) != MCP_OK ||
        mcp_json_object_set(ctx, ref, "value", val) != MCP_OK ||
        mcp_json_object_set(ctx, params, "ref", ref) != MCP_OK ||
        mcp_json_object_set(ctx, params, "argument", arg) != MCP_OK) {
        mcp_json_destroy(ctx, type);
        mcp_json_destroy(ctx, val);
        mcp_json_destroy(ctx, ref);
        mcp_json_destroy(ctx, arg);
        mcp_json_destroy(ctx, params);
        return NULL;
    }
    mcp_message_t *req = mcp_request_new_string_id(ctx, "cc", "completion/complete", params);
    if (req == NULL) {
        mcp_json_destroy(ctx, params);
    }
    return req;
}

static void sweep_dispatch(void) {
    mcp_context_t *ctx = fail_ctx();
    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1");
    CHECK(srv != NULL);
    CHECK(mcp_server_register_completion_provider(ctx, srv, "prompt/", sweep_provider,
                                                   NULL) == MCP_OK);

    /* One full pass with the gate off: init a session, dispatch, count. */
    mcp_session_t *s = mcp_server_create_session(ctx, srv);
    CHECK(s != NULL);
    mcp_json_value_t *iparams = mcp_initialize_params_new(ctx, "cli", "1");
    CHECK(iparams != NULL);
    mcp_message_t *ireq = mcp_request_new_string_id(ctx, "init", "initialize", iparams);
    CHECK(ireq != NULL);
    mcp_message_t *iresp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, ireq, &iresp) == MCP_OK && iresp != NULL);
    mcp_message_destroy(ctx, ireq);
    mcp_message_destroy(ctx, iresp);
    mcp_message_t *notif = mcp_initialized_notification_new(ctx);
    CHECK(notif != NULL);
    CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
    mcp_message_destroy(ctx, notif);

    mcp_message_t *req = sweep_complete_req(ctx);
    CHECK(req != NULL);
    g_count = 0;
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, s, req, &resp) == MCP_OK && resp != NULL);
    size_t total = g_count;
    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
    mcp_server_destroy_session(ctx, srv, s);

    /* Sweep every failure point. Fresh session per iteration (per-session ids). */
    for (size_t n = 0; n <= total; n++) {
        s = mcp_server_create_session(ctx, srv);
        CHECK(s != NULL);
        iparams = mcp_initialize_params_new(ctx, "cli", "1");
        CHECK(iparams != NULL);
        ireq = mcp_request_new_string_id(ctx, "init", "initialize", iparams);
        CHECK(ireq != NULL);
        iresp = NULL;
        CHECK(mcp_server_dispatch(ctx, srv, s, ireq, &iresp) == MCP_OK && iresp != NULL);
        mcp_message_destroy(ctx, ireq);
        mcp_message_destroy(ctx, iresp);
        notif = mcp_initialized_notification_new(ctx);
        CHECK(notif != NULL);
        CHECK(mcp_server_notify(ctx, srv, s, notif) == MCP_OK);
        mcp_message_destroy(ctx, notif);

        req = sweep_complete_req(ctx);
        CHECK(req != NULL);
        g_count = 0;
        g_fail_at = n;
        resp = NULL;
        mcp_status_t st = mcp_server_dispatch(ctx, srv, s, req, &resp);
        g_fail_at = SIZE_MAX;
        if (st == MCP_OK) {
            CHECK(resp != NULL);
            mcp_message_destroy(ctx, resp);
        } else {
            CHECK(st == MCP_ERR_NOMEM && resp == NULL);
        }
        mcp_message_destroy(ctx, req);
        mcp_server_destroy_session(ctx, srv, s);
    }

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
}

int main(void) {
    sweep_ui();
    sweep_client();
    sweep_dispatch();
    printf("test_alloc_fail OK\n");
    return 0;
}
