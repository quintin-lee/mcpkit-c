#include <assert.h>
#include <stdio.h>

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
    {
        // 40 distinct-pair register/unregister cycles: with first-fit reuse
        // of freed slots, only the transient 32-simultaneous limit applies.
        char names[40][4];
        for (int i = 0; i < 40; i++) {
            int n = snprintf(names[i], sizeof(names[i]), "n%d", i);
            assert(n > 0 && (size_t)n < sizeof(names[i]));
            assert(mcp_plugin_register(MCP_PLUGIN_JSON_BACKEND, names[i], &dummy_a) == MCP_OK);
            assert(mcp_plugin_unregister(MCP_PLUGIN_JSON_BACKEND, names[i]) == MCP_OK);
        }
        // All 32 slots simultaneously filled, then a 33rd must be rejected.
        for (int i = 0; i < MCP_PLUGIN_MAX_ENTRIES; i++) {
            int n = snprintf(names[i], sizeof(names[i]), "f%d", i);
            assert(n > 0 && (size_t)n < sizeof(names[i]));
            assert(mcp_plugin_register(MCP_PLUGIN_TRANSPORT, names[i], &dummy_a) == MCP_OK);
        }
        assert(mcp_plugin_count(MCP_PLUGIN_TRANSPORT) == MCP_PLUGIN_MAX_ENTRIES);
        char extra[4];
        snprintf(extra, sizeof(extra), "x%d", MCP_PLUGIN_MAX_ENTRIES);
        assert(mcp_plugin_register(MCP_PLUGIN_TRANSPORT, extra, &dummy_a) == MCP_ERR_NOMEM);
        // Free one slot: the 33rd pair now fits into the reused slot.
        assert(mcp_plugin_unregister(MCP_PLUGIN_TRANSPORT, names[0]) == MCP_OK);
        assert(mcp_plugin_register(MCP_PLUGIN_TRANSPORT, extra, &dummy_a) == MCP_OK);
        for (int i = 1; i < MCP_PLUGIN_MAX_ENTRIES; i++) {
            assert(mcp_plugin_unregister(MCP_PLUGIN_TRANSPORT, names[i]) == MCP_OK);
        }
        assert(mcp_plugin_unregister(MCP_PLUGIN_TRANSPORT, extra) == MCP_OK);
        assert(mcp_plugin_count(MCP_PLUGIN_TRANSPORT) == 0);
    }
    return 0;
}
