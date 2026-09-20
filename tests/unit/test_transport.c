#include "test_check.h"
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

typedef struct {
    char sent[256];
    size_t sent_len;
    int started;
    int stopped;
} fake_t;

static mcp_status_t fake_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    f->started++;
    return MCP_OK;
}

static mcp_status_t fake_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                              size_t len) {
    (void)t;
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    CHECK(len < sizeof(f->sent));
    memcpy(f->sent, data, len);
    f->sent[len] = '\0';
    f->sent_len = len;
    return MCP_OK;
}

static mcp_status_t fake_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    (void)t;
    const char *line = "{\"jsonrpc\":\"2.0\"}";
    size_t n = strlen(line) + 1;
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    char *buf = a->malloc_fn(n, a->userdata);
    CHECK(buf != NULL);
    memcpy(buf, line, n);
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t fake_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    f->stopped++;
    return MCP_OK;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    CHECK(mcp_transport_create(ctx, NULL, NULL) == NULL);

    static const mcp_transport_ops_t kPartial = { NULL, NULL, NULL, NULL };
    mcp_transport_t *p = mcp_transport_create(ctx, &kPartial, NULL);
    CHECK(p != NULL);
    CHECK(mcp_transport_start(ctx, p) == MCP_ERR_UNSUPPORTED);
    CHECK(mcp_transport_send(ctx, p, "x", 1) == MCP_ERR_UNSUPPORTED);
    char *line = (char *)0x1;
    CHECK(mcp_transport_recv(ctx, p, &line) == MCP_ERR_UNSUPPORTED);
    CHECK(mcp_transport_stop(ctx, p) == MCP_ERR_UNSUPPORTED);
    CHECK(mcp_transport_start(ctx, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_transport_recv(ctx, p, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_transport_destroy(ctx, p);

    static const mcp_transport_ops_t kFull = { fake_start, fake_send, fake_recv, fake_stop };
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    mcp_transport_t *t = mcp_transport_create(ctx, &kFull, &fake);
    CHECK(t != NULL);
    CHECK(mcp_transport_start(ctx, t) == MCP_OK && fake.started == 1);
    CHECK(mcp_transport_send(ctx, t, "hello", 5) == MCP_OK);
    CHECK(fake.sent_len == 5 && strcmp(fake.sent, "hello") == 0);
    line = NULL;
    CHECK(mcp_transport_recv(ctx, t, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, "{\"jsonrpc\":\"2.0\"}") == 0);
    mcp_json_free_string(ctx, line);
    CHECK(mcp_transport_stop(ctx, t) == MCP_OK && fake.stopped == 1);
    mcp_transport_destroy(ctx, t);
    mcp_transport_destroy(ctx, NULL);

    mcp_context_destroy(ctx);
    printf("test_transport OK\n");
    return 0;
}
