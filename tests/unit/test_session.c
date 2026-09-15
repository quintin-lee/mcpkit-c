#include <assert.h>

#include "mcpkit/mcpkit.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx);
    mcp_server_t *srv = mcp_server_create(ctx, "t", "1");
    assert(srv);
    mcp_session_t *a = mcp_server_create_session(ctx, srv);
    mcp_session_t *b = mcp_server_create_session(ctx, srv);
    assert(a && b && a != b);
    assert(!mcp_session_is_initialized(ctx, a));
    assert(!mcp_session_is_initialized(ctx, NULL));
    assert(mcp_server_create_session(ctx, NULL) == NULL);
    mcp_server_destroy_session(ctx, srv, a);
    mcp_server_destroy_session(ctx, srv, a);
    mcp_server_destroy_session(ctx, NULL, b);
    mcp_server_destroy_session(ctx, srv, NULL);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
