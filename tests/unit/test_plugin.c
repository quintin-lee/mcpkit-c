#include <assert.h>

#include "mcpkit/plugin/plugin.h"

static int dummy_a;
static int dummy_b;

int main(void) {
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "a", &dummy_a) == MCP_OK);
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "a", &dummy_a)
           == MCP_ERR_ALREADY_EXISTS);
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, NULL, &dummy_a)
           == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "b", NULL)
           == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "a") == &dummy_a);
    assert(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "missing") == NULL);
    assert(mcp_plugin_find(MCP_PLUGIN_TRANSPORT, "a") == NULL);
    assert(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 1);
    assert(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "b", &dummy_b) == MCP_OK);
    assert(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 2);
    assert(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "a") == MCP_OK);
    assert(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "a") == MCP_ERR_NOT_FOUND);
    assert(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "a") == NULL);
    assert(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "b") == MCP_OK);
    assert(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 0);
    return 0;
}
