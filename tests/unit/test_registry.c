#include "test_check.h"

#include "mcpkit/mcpkit.h"

static mcp_status_t ok_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                               void *u, mcp_json_value_t **o) {
    (void)s;
    (void)a;
    (void)u;
    *o = mcp_json_object_new(c);
    return *o ? MCP_OK : MCP_ERR_NOMEM;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx);
    mcp_server_t *srv = mcp_server_create(ctx, "t", "1");
    CHECK(srv);
    CHECK(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "echo", "e", NULL, ok_handler, NULL)) == MCP_OK);
    mcp_tool_t *dup = mcp_tool_new(ctx, "echo", "e", NULL, ok_handler, NULL);
    CHECK(mcp_server_add_tool(ctx, srv, dup) == MCP_ERR_ALREADY_EXISTS);
    mcp_tool_destroy(ctx, dup);
    CHECK(mcp_server_add_resource(ctx, srv, mcp_resource_new(ctx, "f:///a", "a", "text/plain", NULL, NULL)) == MCP_OK);
    CHECK(mcp_server_add_prompt(ctx, srv, mcp_prompt_new(ctx, "p", NULL, NULL, NULL)) == MCP_OK);
    CHECK(mcp_server_remove_tool(ctx, srv, "echo") == MCP_OK);
    CHECK(mcp_server_remove_tool(ctx, srv, "echo") == MCP_ERR_NOT_FOUND);
    CHECK(mcp_server_remove_resource(ctx, srv, "f:///a") == MCP_OK);
    CHECK(mcp_server_remove_prompt(ctx, srv, "p") == MCP_OK);
    CHECK(mcp_tool_new(ctx, NULL, NULL, NULL, ok_handler, NULL) == NULL);
    CHECK(mcp_server_add_tool(ctx, srv, NULL) == MCP_ERR_INVALID_ARGUMENT);
    mcp_tool_t *rejected = mcp_tool_new(ctx, "x", NULL, NULL, ok_handler, NULL);
    CHECK(mcp_server_add_tool(ctx, NULL, rejected) == MCP_ERR_INVALID_ARGUMENT);
    mcp_tool_destroy(ctx, rejected);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
