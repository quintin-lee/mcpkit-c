#include <stdio.h>
#include <string.h>
#include "mcpkit/client/client.h"
#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/sampling.h"
#include "mcpkit/transport/transport.h"
#include "test_check.h"

static int g_sample_called = 0;
static size_t g_sample_tool_count = 0;

static mcp_json_value_t *sample_provider_with_tools(mcp_context_t *ctx,
                                                    const mcp_json_value_t *params,
                                                    void *user_data) {
    (void)user_data;
    g_sample_called++;
    g_sample_tool_count = mcp_sampling_params_get_tool_count(ctx, params);

    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return NULL;
    }

    mcp_json_object_set_take(ctx, result, "role", mcp_json_string_new(ctx, "assistant"));
    mcp_json_object_set_take(ctx, result, "model", mcp_json_string_new(ctx, "claude-3-5-sonnet"));
    mcp_json_object_set_take(ctx, result, "stopReason", mcp_json_string_new(ctx, "toolUse"));

    // Add tool_use content block
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *call_args = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, call_args, "a", mcp_json_number_new(ctx, 40));
    mcp_json_object_set_take(ctx, call_args, "b", mcp_json_number_new(ctx, 2));

    mcp_json_value_t *tu = mcp_sampling_content_tool_use_new(ctx, "call_calc_42", "calculator", call_args);
    mcp_json_array_append(ctx, content, tu);
    mcp_json_object_set_take(ctx, result, "content", content);

    return result;
}

static mcp_status_t noop_start(mcp_context_t *ctx, mcp_transport_t *t) { (void)ctx; (void)t; return MCP_OK; }
static mcp_status_t noop_send(mcp_context_t *ctx, mcp_transport_t *t, const char *d, size_t n) { (void)ctx; (void)t; (void)d; (void)n; return MCP_OK; }
static mcp_status_t noop_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out) { (void)ctx; (void)t; *line_out = NULL; return MCP_ERR_IO; }
static mcp_status_t noop_stop(mcp_context_t *ctx, mcp_transport_t *t) { (void)ctx; (void)t; return MCP_OK; }
static const mcp_transport_ops_t kNoop = { noop_start, noop_send, noop_recv, noop_stop };

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // 1. Create a tool definition for sampling
    mcp_json_value_t *schema = mcp_json_object_new(ctx);
    mcp_sampling_tool_def_t tool = {
        .name = "calculator",
        .description = "Performs arithmetic calculations",
        .input_schema = schema
    };

    mcp_json_value_t *params = mcp_sampling_params_new(ctx, 1024);
    CHECK(params != NULL);
    CHECK(mcp_sampling_params_add_tool(ctx, params, &tool) == MCP_OK);
    CHECK(mcp_sampling_params_set_tool_choice(ctx, params, MCP_TOOL_CHOICE_REQUIRED, NULL) == MCP_OK);
    mcp_json_destroy(ctx, schema);

    // 2. Serialize params and verify tools array & toolChoice
    char *s = mcp_json_serialize(ctx, params);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"name\":\"calculator\"") != NULL);
    CHECK(strstr(s, "\"toolChoice\":{\"mode\":\"required\"}") != NULL || strstr(s, "\"toolChoice\":\"required\"") != NULL);
    mcp_json_free_string(ctx, s);

    // Verify inspect functions
    CHECK(mcp_sampling_params_get_tool_count(ctx, params) == 1);
    const mcp_json_value_t *t0 = mcp_sampling_params_get_tool_at(ctx, params, 0);
    CHECK(t0 != NULL);
    mcp_sampling_tool_choice_t mode;
    const char *spec_tool = NULL;
    CHECK(mcp_sampling_params_get_tool_choice(ctx, params, &mode, &spec_tool) == MCP_OK);
    CHECK(mode == MCP_TOOL_CHOICE_REQUIRED);

    // Test toolChoice modes: SPECIFIC, AUTO, NONE
    CHECK(mcp_sampling_params_set_tool_choice(ctx, params, MCP_TOOL_CHOICE_SPECIFIC, "calculator") == MCP_OK);
    CHECK(mcp_sampling_params_get_tool_choice(ctx, params, &mode, &spec_tool) == MCP_OK);
    CHECK(mode == MCP_TOOL_CHOICE_SPECIFIC);
    CHECK(spec_tool != NULL && strcmp(spec_tool, "calculator") == 0);

    CHECK(mcp_sampling_params_set_tool_choice(ctx, params, MCP_TOOL_CHOICE_AUTO, NULL) == MCP_OK);
    CHECK(mcp_sampling_params_get_tool_choice(ctx, params, &mode, &spec_tool) == MCP_OK);
    CHECK(mode == MCP_TOOL_CHOICE_AUTO);

    CHECK(mcp_sampling_params_set_tool_choice(ctx, params, MCP_TOOL_CHOICE_NONE, NULL) == MCP_OK);
    CHECK(mcp_sampling_params_get_tool_choice(ctx, params, &mode, &spec_tool) == MCP_OK);
    CHECK(mode == MCP_TOOL_CHOICE_NONE);

    // 3. Create tool_use and tool_result content blocks
    mcp_json_value_t *input_args = mcp_json_object_new(ctx);
    mcp_json_value_t *tu = mcp_sampling_content_tool_use_new(ctx, "call_1", "calculator", input_args);
    CHECK(tu != NULL);
    char *tu_s = mcp_json_serialize(ctx, tu);
    CHECK(strstr(tu_s, "\"type\":\"tool_use\"") != NULL);
    CHECK(strstr(tu_s, "\"id\":\"call_1\"") != NULL);
    mcp_json_free_string(ctx, tu_s);
    mcp_json_destroy(ctx, tu);

    mcp_json_value_t *tr = mcp_sampling_content_tool_result_new(ctx, "call_1", "result is 42", false);
    CHECK(tr != NULL);
    char *tr_s = mcp_json_serialize(ctx, tr);
    CHECK(strstr(tr_s, "\"type\":\"tool_result\"") != NULL);
    CHECK(strstr(tr_s, "\"toolUseId\":\"call_1\"") != NULL);
    CHECK(strstr(tr_s, "\"isError\":false") != NULL);
    mcp_json_free_string(ctx, tr_s);
    mcp_json_destroy(ctx, tr);

    // 4. Test client handling server sampling request with tools
    mcp_transport_t *t = mcp_transport_create(ctx, &kNoop, NULL);
    CHECK(t != NULL);
    mcp_client_t *client = mcp_client_create(ctx, t);
    CHECK(client != NULL);

    mcp_client_set_sample_provider(ctx, client, sample_provider_with_tools, NULL);

    // Server sends sampling/createMessage with tools
    mcp_message_t *req = mcp_request_new_string_id(ctx, "req-1", "sampling/createMessage", params);
    CHECK(req != NULL);

    mcp_message_t *resp = NULL;
    CHECK(mcp_client_handle_server_request(ctx, client, req, &resp) == MCP_OK);
    CHECK(resp != NULL);
    CHECK(g_sample_called == 1);
    CHECK(g_sample_tool_count == 1);

    char *resp_s = mcp_message_serialize(ctx, resp);
    CHECK(resp_s != NULL);
    CHECK(strstr(resp_s, "\"stopReason\":\"toolUse\"") != NULL);
    CHECK(strstr(resp_s, "\"type\":\"tool_use\"") != NULL);
    CHECK(strstr(resp_s, "\"call_calc_42\"") != NULL);
    mcp_json_free_string(ctx, resp_s);

    mcp_message_destroy(ctx, req);
    mcp_message_destroy(ctx, resp);
    mcp_client_destroy(ctx, client);
    mcp_transport_destroy(ctx, t);

    // 5. Negative and guard tests
    CHECK(mcp_sampling_params_add_tool(ctx, NULL, &tool) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_sampling_params_add_tool(ctx, params, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_sampling_params_set_tool_choice(ctx, NULL, MCP_TOOL_CHOICE_AUTO, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_sampling_params_set_tool_choice(ctx, params, MCP_TOOL_CHOICE_SPECIFIC, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_sampling_content_tool_use_new(ctx, NULL, "calc", NULL) == NULL);
    CHECK(mcp_sampling_content_tool_use_new(ctx, "id", NULL, NULL) == NULL);
    CHECK(mcp_sampling_content_tool_result_new(ctx, NULL, "res", false) == NULL);

    mcp_context_destroy(ctx);
    printf("test_sampling_tools OK\n");
    return 0;
}
