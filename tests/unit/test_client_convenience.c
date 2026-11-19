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
    if (f->cursor >= f->nscript) {
        uint64_t r_ms = 0, w_ms = 0;
        if (mcp_transport_get_timeout(ctx, t, &r_ms, &w_ms) == MCP_OK && r_ms > 0) {
            return MCP_ERR_TIMEOUT;
        }
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

static void test_call_tool_json_validation(mcp_context_t *ctx) {
    mcp_json_value_t *out_res = (mcp_json_value_t *)0x1;

    CHECK(mcp_client_call_tool_json(ctx, NULL, "echo", NULL, &out_res) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(out_res == NULL);

    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);

    out_res = (mcp_json_value_t *)0x1;
    CHECK(mcp_client_call_tool_json(ctx, c, NULL, NULL, &out_res) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(out_res == NULL);

    CHECK(mcp_client_call_tool_json(ctx, c, "echo", NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    /* non-object args_obj */
    mcp_json_value_t *non_obj = mcp_json_string_new(ctx, "scalar");
    CHECK(non_obj != NULL);
    out_res = (mcp_json_value_t *)0x1;
    CHECK(mcp_client_call_tool_json(ctx, c, "echo", non_obj, &out_res) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(out_res == NULL);
    mcp_json_destroy(ctx, non_obj);

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static void test_call_tool_json_success(mcp_context_t *ctx) {
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    static const char *kResp[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"42\"}],\"computed\":42}}"
    };
    fake.script = kResp;
    fake.nscript = 1;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK);

    mcp_json_value_t *args = mcp_json_object_new(ctx);
    CHECK(args != NULL);
    mcp_json_value_t *val = mcp_json_number_new(ctx, 21.0);
    CHECK(val != NULL);
    CHECK(mcp_json_object_set_take(ctx, args, "val", val) == MCP_OK);

    mcp_json_value_t *res = NULL;
    mcp_status_t st = mcp_client_call_tool_json(ctx, c, "calc", args, &res);
    CHECK(st == MCP_OK);
    CHECK(res != NULL);

    /* Verify args is still owned and valid in caller */
    const mcp_json_value_t *v = mcp_json_object_get(ctx, args, "val");
    CHECK(v != NULL);
    double vd = 0.0;
    CHECK(mcp_json_number_value(ctx, v, &vd) == MCP_OK && vd == 21.0);
    mcp_json_destroy(ctx, args);

    /* Verify res has computed: 42 */
    const mcp_json_value_t *comp = mcp_json_object_get(ctx, res, "computed");
    CHECK(comp != NULL);
    double cd = 0.0;
    CHECK(mcp_json_number_value(ctx, comp, &cd) == MCP_OK && cd == 42.0);
    mcp_json_destroy(ctx, res);

    CHECK(strstr(fake.sent, "\"method\":\"tools/call\"") != NULL);
    CHECK(strstr(fake.sent, "\"name\":\"calc\"") != NULL);
    CHECK(strstr(fake.sent, "\"arguments\":{\"val\":21}") != NULL);

    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static void test_call_tool_json_null_args(mcp_context_t *ctx) {
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    static const char *kResp[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"status\":\"ready\"}}"
    };
    fake.script = kResp;
    fake.nscript = 1;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK);

    mcp_json_value_t *res = NULL;
    mcp_status_t st = mcp_client_call_tool_json(ctx, c, "status_tool", NULL, &res);
    CHECK(st == MCP_OK);
    CHECK(res != NULL);
    mcp_json_destroy(ctx, res);

    CHECK(strstr(fake.sent, "\"method\":\"tools/call\"") != NULL);
    CHECK(strstr(fake.sent, "\"name\":\"status_tool\"") != NULL);

    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static bool stream_count_cb(mcp_client_t *client, const char *method, const mcp_json_value_t *params, void *user_data) {
    (void)client;
    (void)method;
    (void)params;
    int *count = (int *)user_data;
    (*count)++;
    return true;
}

static bool stream_stop_early_cb(mcp_client_t *client, const char *method, const mcp_json_value_t *params, void *user_data) {
    (void)client;
    (void)method;
    (void)params;
    int *count = (int *)user_data;
    (*count)++;
    return false;
}

static void test_listen_stream_validation(mcp_context_t *ctx) {
    CHECK(mcp_client_listen_stream(ctx, NULL, "tools", 0, 0, stream_count_cb, NULL) == MCP_ERR_INVALID_ARGUMENT);

    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);

    CHECK(mcp_client_listen_stream(ctx, c, "tools", 0, 0, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static void test_listen_stream_event_limit(mcp_context_t *ctx) {
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    static const char *kScript[] = {
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/subscriptions/acknowledged\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":\"sub-limit\"}}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\",\"params\":{}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\",\"params\":{}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\",\"params\":{}}"
    };
    fake.script = kScript;
    fake.nscript = 4;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK);

    int count = 0;
    mcp_status_t st = mcp_client_listen_stream(ctx, c, "tools", 0, 2, stream_count_cb, &count);
    CHECK(st == MCP_OK);
    CHECK(count == 2);
    CHECK(strstr(fake.sent, "\"method\":\"notifications/cancelled\"") != NULL);
    CHECK(strstr(fake.sent, "\"io.modelcontextprotocol/subscriptionId\":\"sub-limit\"") != NULL);

    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static void test_listen_stream_early_exit(mcp_context_t *ctx) {
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    static const char *kScript[] = {
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/subscriptions/acknowledged\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":\"sub-early\"}}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\",\"params\":{}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\",\"params\":{}}"
    };
    fake.script = kScript;
    fake.nscript = 3;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK);

    int count = 0;
    mcp_status_t st = mcp_client_listen_stream(ctx, c, "tools", 0, 10, stream_stop_early_cb, &count);
    CHECK(st == MCP_OK);
    CHECK(count == 1);
    CHECK(strstr(fake.sent, "\"method\":\"notifications/cancelled\"") != NULL);
    CHECK(strstr(fake.sent, "\"io.modelcontextprotocol/subscriptionId\":\"sub-early\"") != NULL);

    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
}

static void test_listen_stream_timeout(mcp_context_t *ctx) {
    fake_t fake;
    memset(&fake, 0, sizeof(fake));
    static const char *kScript[] = {
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/subscriptions/acknowledged\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":\"sub-timeout\"}}}"
    };
    fake.script = kScript;
    fake.nscript = 1;

    mcp_transport_t *t = mcp_transport_create(ctx, &kFake, &fake);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK);

    int count = 0;
    mcp_status_t st = mcp_client_listen_stream(ctx, c, "tools", 50, 5, stream_count_cb, &count);
    CHECK(st == MCP_ERR_TIMEOUT);
    CHECK(count == 0);
    CHECK(strstr(fake.sent, "\"method\":\"notifications/cancelled\"") != NULL);
    CHECK(strstr(fake.sent, "\"io.modelcontextprotocol/subscriptionId\":\"sub-timeout\"") != NULL);

    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK);
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

    test_call_tool_json_validation(ctx);
    test_call_tool_json_success(ctx);
    test_call_tool_json_null_args(ctx);

    test_listen_stream_validation(ctx);
    test_listen_stream_event_limit(ctx);
    test_listen_stream_early_exit(ctx);
    test_listen_stream_timeout(ctx);

    mcp_context_destroy(ctx);
    printf("test_client_convenience: ALL TESTS PASSED\n");
    return 0;
}
