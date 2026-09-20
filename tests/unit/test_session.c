#include "test_check.h"

#include "mcpkit/mcpkit.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx);
    mcp_server_t *srv = mcp_server_create(ctx, "t", "1");
    CHECK(srv);
    mcp_session_t *a = mcp_server_create_session(ctx, srv);
    mcp_session_t *b = mcp_server_create_session(ctx, srv);
    CHECK(a && b && a != b);
    CHECK(!mcp_session_is_initialized(ctx, a));
    CHECK(!mcp_session_is_initialized(ctx, NULL));
    CHECK(mcp_server_create_session(ctx, NULL) == NULL);
    mcp_server_destroy_session(ctx, srv, a);
    mcp_server_destroy_session(ctx, srv, a);
    mcp_server_destroy_session(ctx, NULL, b);
    mcp_server_destroy_session(ctx, srv, NULL);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
