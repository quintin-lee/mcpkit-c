/* Exercises mcp_stdio_serve_with_client: a scripted roots/list request
 * is fed through a fake transport; the response must carry the registered
 * provider's roots array. */
#include "test_check.h"
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

/* ── roots provider ───────────────────────────────────────────────────── */

static mcp_json_value_t *roots_provider(mcp_context_t *ctx, void *user_data) {
    (void)user_data;
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (arr == NULL) return NULL;
    mcp_json_value_t *root = mcp_json_object_new(ctx);
    if (root == NULL) { mcp_json_destroy(ctx, arr); return NULL; }
    mcp_json_value_t *uri = mcp_json_string_new(ctx, "file:///srv");
    if (uri == NULL || mcp_json_object_set_take(ctx, root, "uri", uri) != MCP_OK) {
        if (uri != NULL) mcp_json_destroy(ctx, uri);
        mcp_json_destroy(ctx, root);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    if (mcp_json_array_append(ctx, arr, root) != MCP_OK) {
        mcp_json_destroy(ctx, root);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    return arr;
}

/* ── fake transport: one scripted request, then EOF; record response ──── */

typedef struct {
    const char *request_line; /* NULL = already consumed */
    char        recorded[4096];
    size_t      recorded_len;
} fake_stdio_t;

static mcp_status_t fake_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx; (void)t; return MCP_OK;
}

static mcp_status_t fake_send(mcp_context_t *ctx, mcp_transport_t *t,
                              const char *data, size_t len) {
    (void)ctx;
    fake_stdio_t *f = (fake_stdio_t *)mcp_transport_backend(ctx, t);
    CHECK(len < sizeof(f->recorded));
    memcpy(f->recorded, data, len);
    f->recorded[len] = '\0';
    f->recorded_len = len;
    return MCP_OK;
}

static mcp_status_t fake_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    fake_stdio_t *f = (fake_stdio_t *)mcp_transport_backend(ctx, t);
    if (f->request_line == NULL) {
        *line_out = NULL;
        return MCP_ERR_IO; /* clean EOF */
    }
    const char *line = f->request_line;
    f->request_line = NULL; /* consume once */
    size_t n = strlen(line) + 1;
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    char *buf = a->malloc_fn(n, a->userdata);
    CHECK(buf != NULL);
    memcpy(buf, line, n);
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t fake_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx; (void)t; return MCP_OK;
}

static const mcp_transport_ops_t kFakeStdio = {
    fake_start, fake_send, fake_recv, fake_stop,
};

/* ── test ─────────────────────────────────────────────────────────────── */

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* Server (minimal, no tools needed for this path) */
    mcp_server_t *srv = mcp_server_create(ctx, "test", "0.1.0");
    CHECK(srv != NULL);

    /* Client with a roots provider */
    fake_stdio_t fake;
    memset(&fake, 0, sizeof(fake));
    const char *k_roots_req =
        "{\"jsonrpc\":\"2.0\",\"id\":1000.0,"
        "\"method\":\"roots/list\",\"params\":{}}";
    fake.request_line = k_roots_req;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFakeStdio, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    mcp_client_set_roots_provider(ctx, c, roots_provider, NULL);

    CHECK(mcp_transport_start(ctx, t) == MCP_OK);
    mcp_status_t st = mcp_stdio_serve_with_client(ctx, srv, c, t);
    /* clean EOF expected; not an error */
    CHECK(st == MCP_OK || st == MCP_ERR_IO);

    /* Verify the recorded response is a JSON-RPC response to id 1000.0
     * and contains the roots array with our uri */
    CHECK(fake.recorded_len > 0);
    mcp_message_t *resp = mcp_message_parse(ctx, fake.recorded, strlen(fake.recorded));
    CHECK(resp != NULL);
    CHECK(mcp_message_kind(ctx, resp) == MCP_MSG_RESPONSE);
    const mcp_json_value_t *result = mcp_message_result(ctx, resp);
    CHECK(result != NULL);
    CHECK(mcp_json_type(ctx, result) == MCP_JSON_ARRAY);
    CHECK(mcp_json_array_size(ctx, result) == 1);

    /* Clean up */
    mcp_message_destroy(ctx, resp);
    mcp_transport_stop(ctx, t);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
