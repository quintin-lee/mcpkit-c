#include "test_check.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

/* Live-allocation counter: proves session + all loop temporaries are
 * freed on every exit path (a leaked session would leave live > 0). */
static long g_live = 0;

static void *cnt_malloc(size_t n, void *ud) {
    (void)ud;
    void *p = malloc(n);
    if (p != NULL) {
        g_live++;
    }
    return p;
}

static void cnt_free(void *ptr, void *ud) {
    (void)ud;
    if (ptr != NULL) {
        g_live--;
        free(ptr);
    }
}

static void *cnt_calloc(size_t nm, size_t size, void *ud) {
    (void)ud;
    void *p = calloc(nm, size);
    if (p != NULL) {
        g_live++;
    }
    return p;
}

static void *cnt_realloc(void *ptr, size_t n, void *ud) {
    (void)ud;
    if (ptr == NULL) {
        return cnt_malloc(n, NULL);
    }
    return realloc(ptr, n);
}

static const mcp_allocator_t kCntAlloc = {
    .malloc_fn = cnt_malloc,
    .free_fn = cnt_free,
    .calloc_fn = cnt_calloc,
    .realloc_fn = cnt_realloc,
    .userdata = NULL,
};

static mcp_context_t *cnt_ctx(void) {
    mcp_context_config_t cfg = {.allocator = &kCntAlloc, .logger = NULL,
                                .json_backend = NULL};
    mcp_context_t *ctx = mcp_context_create(&cfg);
    CHECK(ctx != NULL);
    return ctx;
}

typedef struct {
    char sent[4096];
    int nsent;
    const char **script;
    size_t nscript;
    size_t cursor;
} fake_t;

static mcp_status_t fake_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static mcp_status_t fake_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                              size_t len) {
    (void)ctx;
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    CHECK(len < sizeof(f->sent));
    memcpy(f->sent, data, len);
    f->sent[len] = '\0';
    f->nsent++;
    return MCP_OK;
}

static mcp_status_t fake_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    if (f->cursor >= f->nscript) {
        *line_out = NULL;
        return MCP_ERR_IO;
    }
    const char *line = f->script[f->cursor++];
    size_t n = strlen(line) + 1;
    char *buf = mcp_context_allocator(ctx)->malloc_fn(n, mcp_context_allocator(ctx)->userdata);
    CHECK(buf != NULL);
    memcpy(buf, line, n);
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t fake_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static const mcp_transport_ops_t kFake = {fake_start, fake_send, fake_recv, fake_stop};

static int g_invocations = 0;
static int g_mode = 0; /* 0 = plain echo, 1 = request stop, 2 = raise SIGUSR1 */

static mcp_status_t echo_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                                 void *u, mcp_json_value_t **o) {
    (void)s;
    (void)a;
    (void)u;
    g_invocations++;
    if (g_mode == 1) {
        mcp_request_shutdown();
    } else if (g_mode == 2) {
        raise(SIGUSR1);
    }
    mcp_json_value_t *res = mcp_json_object_new(c);
    CHECK(res != NULL);
    *o = res;
    return MCP_OK;
}

static void on_usr1(int sig) {
    (void)sig;
    mcp_request_shutdown();
}

static mcp_server_t *echo_server(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1");
    CHECK(srv != NULL);
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "echo", "echo", NULL, echo_handler,
                                                     NULL)) == MCP_OK);
    return srv;
}

static const char *kInit =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2025-06-18\",\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
static const char *kNotif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
static const char *kCall2 =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\"}}";
static const char *kCall3 =
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\"}}";

int main(void) {
    /* Case 1: stop requested before the run takes effect immediately. */
    {
        mcp_context_t *ctx = cnt_ctx();
        mcp_server_t *srv = echo_server(ctx);
        fake_t fake;
        memset(&fake, 0, sizeof(fake));
        const char *script[] = {kInit};
        fake.script = script;
        fake.nscript = 1;
        mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
        CHECK(t != NULL);
        g_mode = 0;
        g_invocations = 0;
        mcp_request_shutdown();
        CHECK(mcp_loop_run(ctx, srv, t, NULL, NULL) == MCP_ERR_CANCELLED);
        CHECK(fake.cursor == 0 && fake.nsent == 0);
        mcp_shutdown_clear();
        mcp_transport_destroy(ctx, t);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }
    /* Case 2: shutdown during dispatch drains the in-flight request. */
    {
        mcp_context_t *ctx = cnt_ctx();
        mcp_server_t *srv = echo_server(ctx);
        fake_t fake;
        memset(&fake, 0, sizeof(fake));
        const char *script[] = {kInit, kNotif, kCall2, kCall3};
        fake.script = script;
        fake.nscript = 4;
        mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
        CHECK(t != NULL);
        g_mode = 1;
        g_invocations = 0;
        CHECK(mcp_loop_run(ctx, srv, t, NULL, NULL) == MCP_ERR_CANCELLED);
        CHECK(g_invocations == 1);          /* call id 3 never dispatched */
        CHECK(fake.cursor == 3);            /* call id 3 never read */
        CHECK(fake.nsent == 2);             /* init + call id 2 responses sent */
        CHECK(strstr(fake.sent, "\"id\":2") != NULL);
        mcp_shutdown_clear();
        mcp_transport_destroy(ctx, t);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }
    /* Case 3: after an explicit clear, a fresh run behaves normally. */
    {
        mcp_context_t *ctx = cnt_ctx();
        mcp_server_t *srv = echo_server(ctx);
        fake_t fake;
        memset(&fake, 0, sizeof(fake));
        const char *script[] = {kInit, kNotif, kCall2};
        fake.script = script;
        fake.nscript = 3;
        mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
        CHECK(t != NULL);
        g_mode = 0;
        g_invocations = 0;
        /* Stale flag from case 2 was cleared; prove a fresh flag still kills. */
        mcp_request_shutdown();
        CHECK(mcp_loop_run(ctx, srv, t, NULL, NULL) == MCP_ERR_CANCELLED);
        CHECK(fake.cursor == 0);
        mcp_shutdown_clear();
        fake.cursor = 0;
        fake.nsent = 0;
        CHECK(mcp_loop_run(ctx, srv, t, NULL, NULL) == MCP_ERR_NOT_FOUND);
        CHECK(g_invocations == 1 && fake.nsent == 2);
        mcp_transport_destroy(ctx, t);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }
    /* Case 4: same drain behavior through mcp_stdio_serve. */
    {
        mcp_context_t *ctx = cnt_ctx();
        mcp_server_t *srv = echo_server(ctx);
        fake_t fake;
        memset(&fake, 0, sizeof(fake));
        const char *script[] = {kInit, kNotif, kCall2, kCall3};
        fake.script = script;
        fake.nscript = 4;
        mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
        CHECK(t != NULL);
        g_mode = 1;
        g_invocations = 0;
        CHECK(mcp_stdio_serve(ctx, srv, t) == MCP_ERR_CANCELLED);
        CHECK(g_invocations == 1 && fake.cursor == 3 && fake.nsent == 2);
        mcp_shutdown_clear();
        mcp_transport_destroy(ctx, t);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }
    /* Case 5: a real signal lands via raise() inside the handler. */
    {
        CHECK(signal(SIGUSR1, on_usr1) != SIG_ERR);
        mcp_context_t *ctx = cnt_ctx();
        mcp_server_t *srv = echo_server(ctx);
        fake_t fake;
        memset(&fake, 0, sizeof(fake));
        const char *script[] = {kInit, kNotif, kCall2, kCall3};
        fake.script = script;
        fake.nscript = 4;
        mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
        CHECK(t != NULL);
        g_mode = 2;
        g_invocations = 0;
        CHECK(mcp_loop_run(ctx, srv, t, NULL, NULL) == MCP_ERR_CANCELLED);
        CHECK(g_invocations == 1 && fake.cursor == 3 && fake.nsent == 2);
        CHECK(signal(SIGUSR1, SIG_DFL) != SIG_ERR);
        mcp_shutdown_clear();
        mcp_transport_destroy(ctx, t);
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
    }
    CHECK(g_live == 0);
    return 0;
}
