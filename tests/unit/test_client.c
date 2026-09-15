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
    assert(len < sizeof(f->sent));
    memcpy(f->sent, data, len);
    f->sent[len] = '\0';
    f->sent_len = len;
    f->nsent++;
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

    assert(mcp_client_initialize(ctx, NULL, "n", "v", NULL) == MCP_ERR_INVALID_ARGUMENT);

    fake_t fake2;
    memset(&fake2, 0, sizeof(fake2));
    static const char *kShake[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\","
        "\"serverInfo\":{\"name\":\"s\",\"version\":\"1\"},\"capabilities\":{}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32602,\"message\":\"Unknown tool\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}",
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"error\":{\"code\":-32600}}",
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"tools\":[{\"name\":\"echo\"}]}}",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}}",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"contents\":[]}}",
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{\"messages\":[]}}",
    };
    fake2.script = kShake;
    fake2.nscript = 8;
    mcp_transport_t *t2 = mcp_transport_create(ctx, &kFake, &fake2);
    assert(t2 != NULL);
    mcp_client_t *c2 = mcp_client_create(ctx, t2);
    assert(c2 != NULL);
    assert(mcp_client_protocol_version(ctx, c2) == NULL);
    mcp_json_value_t *info = NULL;
    assert(mcp_client_initialize(ctx, c2, "n", "v", &info) == MCP_OK);
    assert(fake2.nsent == 2);
    assert(strstr(fake2.sent, "\"method\":\"notifications/initialized\"") != NULL);
    assert(strstr(fake2.sent, "\"id\"") == NULL);
    const char *ver = mcp_client_protocol_version(ctx, c2);
    assert(ver != NULL && strcmp(ver, "2025-06-18") == 0);
    assert(info != NULL);
    const char *sname = NULL;
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, info, "name"), &sname) == MCP_OK);
    assert(strcmp(sname, "s") == 0);
    mcp_json_destroy(ctx, info);
    assert(mcp_client_call_tool(ctx, c2, "nope", NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_client_call_tool(ctx, c2, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_client_ping(ctx, c2) == MCP_OK);
    assert(mcp_client_request(ctx, c2, "ping", NULL, NULL) == MCP_ERR_PROTOCOL);
    mcp_json_value_t *tools = NULL;
    assert(mcp_client_list_tools(ctx, c2, &tools) == MCP_OK && tools != NULL);
    const mcp_json_value_t *tarr = mcp_json_object_get(ctx, tools, "tools");
    assert(mcp_json_array_size(ctx, tarr) == 1);
    const char *tname = NULL;
    assert(mcp_json_string_value(ctx, mcp_json_object_get(ctx, mcp_json_array_get(ctx, tarr, 0),
                                                          "name"),
                                 &tname) == MCP_OK);
    assert(strcmp(tname, "echo") == 0);
    mcp_json_destroy(ctx, tools);
    assert(mcp_client_list_tools(ctx, c2, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_value_t *args = mcp_json_object_new(ctx);
    assert(args != NULL);
    assert(mcp_json_object_set(ctx, args, "text", mcp_json_string_new(ctx, "hi")) == MCP_OK);
    mcp_json_value_t *cres = NULL;
    assert(mcp_client_call_tool(ctx, c2, "echo", args, &cres) == MCP_OK && cres != NULL);
    const char *etext = NULL;
    assert(mcp_json_string_value(ctx,
                                 mcp_json_object_get(ctx,
                                                     mcp_json_array_get(ctx,
                                                                        mcp_json_object_get(ctx,
                                                                                            cres,
                                                                                            "content"),
                                                                        0),
                                                     "text"),
                                 &etext) == MCP_OK);
    assert(strcmp(etext, "hi") == 0);
    mcp_json_destroy(ctx, cres);
    assert(mcp_client_read_resource(ctx, c2, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_value_t *rres = NULL;
    assert(mcp_client_read_resource(ctx, c2, "test://doc", &rres) == MCP_OK && rres != NULL);
    mcp_json_destroy(ctx, rres);
    mcp_json_value_t *pres = NULL;
    assert(mcp_client_get_prompt(ctx, c2, "p", NULL, &pres) == MCP_OK && pres != NULL);
    mcp_json_destroy(ctx, pres);
    assert(mcp_client_get_prompt(ctx, c2, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_client_destroy(ctx, c2);
    mcp_transport_destroy(ctx, t2);
    mcp_client_destroy(ctx, NULL);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    printf("test_client OK\n");
    return 0;
}
