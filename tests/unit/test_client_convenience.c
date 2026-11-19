#include "test_check.h"
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
    int nsent;
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
    CHECK(len < sizeof(f->sent));
    memcpy(f->sent, data, len);
    f->sent[len] = '\0';
    f->sent_len = len;
    f->nsent++;
    return MCP_OK;
}

static mcp_status_t fake_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    fake_t *f = (fake_t *)mcp_transport_backend(ctx, t);
    CHECK(f->cursor < f->nscript);
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
    ((fake_t *)mcp_transport_backend(ctx, t))->stopped++;
    return MCP_OK;
}

static const mcp_transport_ops_t kFake = { fake_start, fake_send, fake_recv, fake_stop };

static void test_call_tool_text_validation(mcp_context_t *ctx) {
    char *out_text = (char *)0x1;
    bool is_err = true;

    /* Invalid arguments */
    CHECK(mcp_client_call_tool_text(ctx, NULL, "echo", NULL, &out_text, &is_err) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(out_text == NULL);

    out_text = (char *)0x1;
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);

    CHECK(mcp_client_call_tool_text(ctx, c, NULL, NULL, &out_text, &is_err) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(out_text == NULL);

    CHECK(mcp_client_call_tool_text(ctx, c, "echo", NULL, NULL, &is_err) == MCP_ERR_INVALID_ARGUMENT);

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static void test_call_tool_text_success(mcp_context_t *ctx) {
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    static const char *kResp[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"hello world\"}],\"isError\":false}}"
    };
    fake.script = kResp;
    fake.nscript = 1;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK);

    char *out_text = NULL;
    bool is_err = true;
    mcp_status_t st = mcp_client_call_tool_text(ctx, c, "echo", "{\"text\":\"hello\"}", &out_text, &is_err);
    CHECK(st == MCP_OK);
    CHECK(is_err == false);
    CHECK(out_text != NULL);
    CHECK(strcmp(out_text, "hello world") == 0);
    CHECK(strstr(fake.sent, "\"method\":\"tools/call\"") != NULL);
    CHECK(strstr(fake.sent, "\"name\":\"echo\"") != NULL);
    CHECK(strstr(fake.sent, "\"arguments\":{\"text\":\"hello\"}") != NULL);

    mcp_free(ctx, out_text);

    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static void test_call_tool_text_error(mcp_context_t *ctx) {
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    static const char *kResp[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"something broke\"}],\"isError\":true}}"
    };
    fake.script = kResp;
    fake.nscript = 1;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK);

    char *out_text = NULL;
    bool is_err = false;
    mcp_status_t st = mcp_client_call_tool_text(ctx, c, "fail_tool", NULL, &out_text, &is_err);
    CHECK(st == MCP_OK);
    CHECK(is_err == true);
    CHECK(out_text != NULL);
    CHECK(strcmp(out_text, "something broke") == 0);

    mcp_free(ctx, out_text);

    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static void test_call_tool_text_invalid_json(mcp_context_t *ctx) {
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);

    char *out_text = (char *)0x1;
    bool is_err = false;
    mcp_status_t st = mcp_client_call_tool_text(ctx, c, "echo", "{not valid json", &out_text, &is_err);
    CHECK(st == MCP_ERR_PROTOCOL);
    CHECK(out_text == NULL);

    /* Not an object */
    out_text = (char *)0x1;
    st = mcp_client_call_tool_text(ctx, c, "echo", "[1, 2, 3]", &out_text, &is_err);
    CHECK(st == MCP_ERR_PROTOCOL);
    CHECK(out_text == NULL);

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    test_call_tool_text_validation(ctx);
    test_call_tool_text_success(ctx);
    test_call_tool_text_error(ctx);
    test_call_tool_text_invalid_json(ctx);

    mcp_context_destroy(ctx);
    printf("test_client_convenience: ALL TESTS PASSED\n");
    return 0;
}
