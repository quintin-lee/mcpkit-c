#include "test_check.h"
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

typedef struct {
    FILE *c2s;
    FILE *s2c;
    long cpos;
    long rpos;
    mcp_server_t *srv;
    mcp_session_t *sess;
    mcp_context_t *ctx;
} pump_t;

// One server turn: consume a single client line, answer into s2c.
static void server_step(pump_t *p) {
    mcp_context_t *ctx = p->ctx;
    CHECK(fseek(p->c2s, p->cpos, SEEK_SET) == 0);
    static char buf[16384];
    CHECK(fgets(buf, (int)sizeof(buf), p->c2s) != NULL);
    p->cpos = ftell(p->c2s);
    mcp_message_t *msg = mcp_message_parse(ctx, buf, strlen(buf));
    CHECK(msg != NULL);
    if (mcp_message_kind(ctx, msg) == MCP_MSG_NOTIFICATION) {
        CHECK(mcp_server_notify(ctx, p->srv, p->sess, msg) == MCP_OK);
        mcp_message_destroy(ctx, msg);
        return;
    }
    mcp_message_t *resp = NULL;
    CHECK(mcp_server_dispatch(ctx, p->srv, p->sess, msg, &resp) == MCP_OK && resp != NULL);
    mcp_message_destroy(ctx, msg);
    char *out = mcp_message_serialize(ctx, resp);
    mcp_message_destroy(ctx, resp);
    CHECK(out != NULL);
    CHECK(fseek(p->s2c, 0, SEEK_END) == 0);
    CHECK(fputs(out, p->s2c) != EOF && fputc('\n', p->s2c) != EOF);
    CHECK(fflush(p->s2c) == 0);
    mcp_json_free_string(ctx, out);
}

static mcp_status_t pump_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static mcp_status_t pump_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                              size_t len) {
    pump_t *p = (pump_t *)mcp_transport_backend(ctx, t);
    CHECK(fseek(p->c2s, 0, SEEK_END) == 0);
    CHECK(fwrite(data, 1, len, p->c2s) == len);
    CHECK(fputc('\n', p->c2s) != EOF);
    CHECK(fflush(p->c2s) == 0);
    // Single-threaded e2e: the server processes each line synchronously inside
    // send, so the blocking client recv below always has its answer ready.
    server_step(p);
    return MCP_OK;
}

static mcp_status_t pump_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) {
    pump_t *p = (pump_t *)mcp_transport_backend(ctx, t);
    CHECK(fseek(p->s2c, p->rpos, SEEK_SET) == 0);
    static char buf[16384];
    CHECK(fgets(buf, (int)sizeof(buf), p->s2c) != NULL);
    p->rpos = ftell(p->s2c);
    size_t n = strlen(buf) + 1;
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    char *line = a->malloc_fn(n, a->userdata);
    CHECK(line != NULL);
    memcpy(line, buf, n);
    *line_out = line;
    return MCP_OK;
}

static mcp_status_t pump_stop(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx;
    (void)t;
    return MCP_OK;
}

static const mcp_transport_ops_t kPump = { pump_start, pump_send, pump_recv, pump_stop };

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const char *s = NULL;
    if (mcp_json_string_value(ctx, mcp_json_object_get(ctx, args, "text"), &s) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (item == NULL || content == NULL || result == NULL ||
        mcp_json_object_set(ctx, item, "type", mcp_json_string_new(ctx, "text")) != MCP_OK ||
        mcp_json_object_set(ctx, item, "text", mcp_json_string_new(ctx, s)) != MCP_OK ||
        mcp_json_array_append(ctx, content, item) != MCP_OK ||
        mcp_json_object_set(ctx, result, "content", content) != MCP_OK) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "pump", "0.1.0");
    CHECK(srv != NULL);
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    CHECK(schema != NULL);
    CHECK(mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx)) == MCP_OK);
    CHECK(mcp_schema_add_required(ctx, schema, "text") == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "echo", "Echo", schema, echo_handler,
                                                     NULL)) == MCP_OK);

    pump_t pump;
    memset(&pump, 0, sizeof(pump));
    pump.c2s = tmpfile();
    pump.s2c = tmpfile();
    CHECK(pump.c2s != NULL && pump.s2c != NULL);
    pump.srv = srv;
    pump.sess = mcp_server_create_session(ctx, srv);
    CHECK(pump.sess != NULL);
    pump.ctx = ctx;
    mcp_transport_t *t = mcp_transport_create(ctx, &kPump, &pump);
    CHECK(t != NULL);
    mcp_client_t *c = mcp_client_create(ctx, t);
    CHECK(c != NULL);
    CHECK(mcp_client_connect(ctx, c) == MCP_OK);

    mcp_json_value_t *info = NULL;
    CHECK(mcp_client_initialize(ctx, c, "e2e", "0.1.0", &info) == MCP_OK);
    CHECK(strcmp(mcp_client_protocol_version(ctx, c), "2025-06-18") == 0);
    CHECK(info != NULL);
    const char *sname = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, info, "name"), &sname) == MCP_OK);
    CHECK(strcmp(sname, "pump") == 0);
    mcp_json_destroy(ctx, info);

    mcp_json_value_t *tools = NULL;
    CHECK(mcp_client_list_tools(ctx, c, &tools) == MCP_OK && tools != NULL);
    const mcp_json_value_t *tarr = mcp_json_object_get(ctx, tools, "tools");
    CHECK(mcp_json_array_size(ctx, tarr) == 1);
    mcp_json_destroy(ctx, tools);

    mcp_json_value_t *args = mcp_json_object_new(ctx);
    CHECK(args != NULL);
    CHECK(mcp_json_object_set(ctx, args, "text", mcp_json_string_new(ctx, "hi")) == MCP_OK);
    mcp_json_value_t *cres = NULL;
    CHECK(mcp_client_call_tool(ctx, c, "echo", args, &cres) == MCP_OK && cres != NULL);
    const char *etext = NULL;
    CHECK(mcp_json_string_value(ctx,
                                 mcp_json_object_get(ctx,
                                                     mcp_json_array_get(ctx,
                                                                        mcp_json_object_get(ctx,
                                                                                            cres,
                                                                                            "content"),
                                                                        0),
                                                     "text"),
                                 &etext) == MCP_OK);
    CHECK(strcmp(etext, "hi") == 0);
    mcp_json_destroy(ctx, cres);

    CHECK(mcp_client_call_tool(ctx, c, "nope", NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_client_ping(ctx, c) == MCP_OK);
    CHECK(mcp_client_disconnect(ctx, c) == MCP_OK);

    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    fclose(pump.c2s);
    fclose(pump.s2c);
    mcp_server_destroy_session(ctx, srv, pump.sess);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("test_client_e2e OK\n");
    return 0;
}
