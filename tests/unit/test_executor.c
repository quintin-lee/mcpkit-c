#include <assert.h>
#include <stddef.h>

#include "mcpkit/runtime/executor.h"
#include "mcpkit/runtime/sync.h"

static void bump(mcp_context_t *ctx, void *arg) {
    (void)ctx;
    (*(int *)arg)++;
}

int main(void) {
    mcp_executor_t *ex = mcp_sync_executor_create(NULL);
    assert(ex != NULL);

    int n = 0;
    assert(mcp_executor_submit(NULL, ex, bump, &n) == MCP_OK);
    assert(n == 1);
    assert(mcp_executor_submit(NULL, ex, bump, &n) == MCP_OK);
    assert(mcp_executor_submit(NULL, ex, bump, &n) == MCP_OK);
    assert(mcp_executor_wait(NULL, ex) == MCP_OK);
    assert(n == 3);

    assert(mcp_executor_wait(NULL, ex) == MCP_OK);

    assert(mcp_executor_submit(NULL, ex, NULL, &n) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_executor_submit(NULL, NULL, bump, &n) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_executor_wait(NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_executor_backend(NULL, NULL) == NULL);
    assert(mcp_executor_backend(NULL, ex) == NULL);

    mcp_executor_destroy(NULL, ex);
    mcp_executor_destroy(NULL, NULL);
    return 0;
}
