#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

typedef struct {
    char sent[4096];
    size_t sent_len;
    int started;
    int stopped;
    const char **script;
    size_t nscript;
    size_t cursor;
} fake_t;

static mcp_status_t fake_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    ((fake_t *)mcp_transport_backend(ctx, t))->started++;
    return MCP_OK;
}

static mcp_status_t fake_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                              size_t len) {
    (void)ctx;
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    assert(len < sizeof(f->sent));
    memcpy(f->sent, data, len);
    f->sent[len] = '\0';
    f->sent_len = len;
    return MCP_OK;
}

static mcp_status_t fake_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    assert(f->cursor < f->nscript);
    const char *line = f->script[f->cursor++];
    size_t n = strlen(line) + 1;
    char *buf = mcp_context_allocator(ctx)->malloc_fn(n, mcp_context_allocator(ctx)->userdata);
    assert(buf != NULL);
    memcpy(buf, line, n);
    *line_out = buf;
    return MCP_OK;
}

static mcp_status_t fake_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    ((fake_t *)mcp_transport_backend(ctx, t))->stopped++;
    return MCP_OK;
}

static const mcp_transport_ops_t kFake = { fake_start, fake_send, fake_recv, fake_stop };

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    assert(mcp_client_create(ctx, NULL) == NULL);
    assert(mcp_client_request(ctx, NULL, "ping", NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    assert(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    assert(c != NULL);
    assert(mcp_client_protocol_version(ctx, c) == NULL);
    assert(mcp_client_request(ctx, c, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_client_connect(ctx, c) == MCP_OK && fake.started == 1);

    static const char *kOk[] = { "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ok\":true}}" };
    fake.script = kOk;
    fake.nscript = 1;
    mcp_json_value_t *res = NULL;
    assert(mcp_client_request(ctx, c, "ping", NULL, &res) == MCP_OK);
    assert(res != NULL);
    bool ok = false;
    assert(mcp_json_bool_value(ctx, mcp_json_object_get(ctx, res, "ok"), &ok) == MCP_OK && ok);
    mcp_json_destroy(ctx, res);
    assert(strstr(fake.sent, "\"method\":\"ping\"") != NULL);
    assert(strstr(fake.sent, "\"id\":1") != NULL);

    static const char *kMismatch[] = { "{\"jsonrpc\":\"2.0\",\"id\":99,\"result\":{}}" };
    fake.script = kMismatch;
    fake.nscript = 1;
    fake.cursor = 0;
    res = (mcp_json_value_t *)0x1;
    assert(mcp_client_request(ctx, c, "ping", NULL, &res) == MCP_ERR_PROTOCOL);
    assert(res == NULL);

    assert(mcp_client_disconnect(ctx, c) == MCP_OK && fake.stopped == 1);
    mcp_client_destroy(ctx, c);
    mcp_client_destroy(ctx, NULL);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    printf("test_client OK\n");
    return 0;
}
