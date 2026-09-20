#include "test_check.h"
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static mcp_status_t echo_handler(mcp_context_t *c, mcp_session_t *s,
                                 const mcp_json_value_t *a, void *u,
                                 mcp_json_value_t **o) {
    (void)s;
    (void)u;
    const mcp_json_value_t *t = mcp_json_object_get(c, a, "text");
    const char *str = NULL;
    if (t == NULL || mcp_json_string_value(c, t, &str) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *content = mcp_json_array_new(c);
    mcp_json_value_t *item = mcp_json_object_new(c);
    mcp_json_value_t *type = mcp_json_string_new(c, "text");
    mcp_json_value_t *sv = mcp_json_string_new(c, str);
    mcp_json_value_t *result = mcp_json_object_new(c);
    if (!content || !item || !type || !sv || !result ||
        mcp_json_object_set(c, item, "type", type) != MCP_OK ||
        mcp_json_object_set(c, item, "text", sv) != MCP_OK ||
        mcp_json_array_append(c, content, item) != MCP_OK ||
        mcp_json_object_set(c, result, "content", content) != MCP_OK) {
        mcp_json_destroy(c, type);
        mcp_json_destroy(c, sv);
        mcp_json_destroy(c, item);
        mcp_json_destroy(c, content);
        mcp_json_destroy(c, result);
        return MCP_ERR_NOMEM;
    }
    *o = result;
    return MCP_OK;
}

static const char *kInit =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
    "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
    "\"clientInfo\":{\"name\":\"loop\",\"version\":\"0.1.0\"}}}\n";
static const char *kNotif = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n";
static const char *kCall =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
    "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hello loop\"}}}\n";

static mcp_server_t *make_server(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "loop", "0.1.0");
    CHECK(srv != NULL);
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    CHECK(schema != NULL);
    CHECK(mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx)) ==
           MCP_OK);
    CHECK(mcp_schema_add_required(ctx, schema, "text") == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "echo", "Echo", schema,
                                                     echo_handler, NULL)) == MCP_OK);
    return srv;
}

static void check_lines(mcp_context_t *ctx, FILE *out) {
    char line[8192];
    CHECK(fgets(line, (int)sizeof(line), out) != NULL);
    mcp_message_t *init = mcp_message_parse(ctx, line, strlen(line));
    CHECK(init != NULL && mcp_message_kind(ctx, init) == MCP_MSG_RESPONSE);
    const mcp_json_value_t *pv =
        mcp_json_object_get(ctx, mcp_message_result(ctx, init), "protocolVersion");
    const char *s = NULL;
    CHECK(pv != NULL && mcp_json_string_value(ctx, pv, &s) == MCP_OK);
    CHECK(strcmp(s, "2025-06-18") == 0);
    mcp_message_destroy(ctx, init);
    CHECK(fgets(line, (int)sizeof(line), out) != NULL);
    mcp_message_t *call = mcp_message_parse(ctx, line, strlen(line));
    CHECK(call != NULL && mcp_message_kind(ctx, call) == MCP_MSG_RESPONSE);
    const mcp_json_value_t *content =
        mcp_json_object_get(ctx, mcp_message_result(ctx, call), "content");
    CHECK(content != NULL && mcp_json_array_size(ctx, content) == 1);
    const mcp_json_value_t *text = mcp_json_object_get(
        ctx, mcp_json_array_get(ctx, content, 0), "text");
    s = NULL;
    CHECK(text != NULL && mcp_json_string_value(ctx, text, &s) == MCP_OK);
    CHECK(strcmp(s, "hello loop") == 0);
    mcp_message_destroy(ctx, call);
    CHECK(fgets(line, (int)sizeof(line), out) == NULL);
}

static void on_fire(mcp_context_t *ctx, void *arg) {
    (void)ctx;
    (*(int *)arg)++;
}

static void write_script(FILE *in) {
    CHECK(fputs(kInit, in) != EOF && fputs(kNotif, in) != EOF &&
           fputs(kCall, in) != EOF);
    rewind(in);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = make_server(ctx);
    CHECK(mcp_loop_run(NULL, srv, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_loop_run(ctx, NULL, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_loop_run(ctx, srv, NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_server_destroy(ctx, srv);

    {
        mcp_server_t *s2 = make_server(ctx);
        FILE *in = tmpfile();
        CHECK(in != NULL);
        FILE *out = tmpfile();
        CHECK(out != NULL);
        mcp_transport_t *t = mcp_stdio_transport_create(ctx, in, out);
        CHECK(t != NULL);
        CHECK(mcp_transport_start(ctx, t) == MCP_OK);
        mcp_status_t st = mcp_loop_run(ctx, s2, t, NULL, NULL);
        CHECK(st == MCP_ERR_IO || st == MCP_ERR_NOT_FOUND);
        CHECK(mcp_transport_stop(ctx, t) == MCP_OK);
        mcp_transport_destroy(ctx, t);
        fclose(in);
        fclose(out);
        mcp_server_destroy(ctx, s2);
    }

    {
        int now = 0, later = 0;
        mcp_timer_t *timer = mcp_timer_create(ctx);
        CHECK(timer != NULL);
        mcp_executor_t *sync = mcp_sync_executor_create(ctx);
        CHECK(sync != NULL);
        FILE *in = tmpfile();
        CHECK(in != NULL);
        write_script(in);
        FILE *out = tmpfile();
        CHECK(out != NULL);
        mcp_server_t *s3 = make_server(ctx);
        mcp_transport_t *t = mcp_stdio_transport_create(ctx, in, out);
        CHECK(t != NULL);
        CHECK(mcp_transport_start(ctx, t) == MCP_OK);
        CHECK(mcp_timer_schedule(ctx, timer, 0, on_fire, &now) == MCP_OK);
        CHECK(mcp_timer_schedule(ctx, timer, 60000, on_fire, &later) == MCP_OK);
        CHECK(mcp_loop_run(ctx, s3, t, sync, timer) == MCP_ERR_NOT_FOUND);
        CHECK(now == 1 && later == 0);
        CHECK(mcp_transport_stop(ctx, t) == MCP_OK);
        mcp_transport_destroy(ctx, t);
        rewind(out);
        check_lines(ctx, out);
        fclose(in);
        fclose(out);
        mcp_executor_destroy(ctx, sync);
        mcp_timer_destroy(ctx, timer);
        mcp_server_destroy(ctx, s3);
    }

    {
        mcp_executor_t *pool = mcp_threadpool_create(ctx, 2);
        CHECK(pool != NULL);
        FILE *in = tmpfile();
        CHECK(in != NULL);
        write_script(in);
        FILE *out = tmpfile();
        CHECK(out != NULL);
        mcp_server_t *s4 = make_server(ctx);
        mcp_transport_t *t = mcp_stdio_transport_create(ctx, in, out);
        CHECK(t != NULL);
        CHECK(mcp_transport_start(ctx, t) == MCP_OK);
        CHECK(mcp_loop_run(ctx, s4, t, pool, NULL) == MCP_ERR_NOT_FOUND);
        CHECK(mcp_transport_stop(ctx, t) == MCP_OK);
        mcp_transport_destroy(ctx, t);
        rewind(out);
        check_lines(ctx, out);
        fclose(in);
        fclose(out);
        mcp_executor_destroy(ctx, pool);
        mcp_server_destroy(ctx, s4);
    }

    mcp_context_destroy(ctx);
    printf("test_loop OK\n");
    return 0;
}
