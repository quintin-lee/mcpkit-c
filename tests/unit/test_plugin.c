#include "test_check.h"
#include <stdio.h>

#include "mcpkit/plugin/plugin.h"

static int dummy_a;
static int dummy_b;

int main(void) {
    CHECK(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "a", &dummy_a) == MCP_OK);
    CHECK(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "a", &dummy_a)
           == MCP_ERR_ALREADY_EXISTS);
    CHECK(mcp_plugin_register(MCP_PLUGIN_ADAPTER, NULL, &dummy_a)
           == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "b", NULL)
           == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "a") == &dummy_a);
    CHECK(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "missing") == NULL);
    CHECK(mcp_plugin_find(MCP_PLUGIN_TRANSPORT, "a") == NULL);
    CHECK(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 1);
    CHECK(mcp_plugin_register(MCP_PLUGIN_ADAPTER, "b", &dummy_b) == MCP_OK);
    CHECK(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 2);
    CHECK(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "a") == MCP_OK);
    CHECK(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "a") == MCP_ERR_NOT_FOUND);
    CHECK(mcp_plugin_find(MCP_PLUGIN_ADAPTER, "a") == NULL);
    CHECK(mcp_plugin_unregister(MCP_PLUGIN_ADAPTER, "b") == MCP_OK);
    CHECK(mcp_plugin_count(MCP_PLUGIN_ADAPTER) == 0);
    {
        // 40 distinct-pair register/unregister cycles: with first-fit reuse
        // of freed slots, only the transient 32-simultaneous limit applies.
        char names[40][4];
        for (int i = 0; i < 40; i++) {
            int n = snprintf(names[i], sizeof(names[i]), "n%d", i);
            CHECK(n > 0 && (size_t)n < sizeof(names[i]));
            CHECK(mcp_plugin_register(MCP_PLUGIN_JSON_BACKEND, names[i], &dummy_a) == MCP_OK);
            CHECK(mcp_plugin_unregister(MCP_PLUGIN_JSON_BACKEND, names[i]) == MCP_OK);
        }
        // All 32 slots simultaneously filled, then a 33rd must be rejected.
        for (int i = 0; i < MCP_PLUGIN_MAX_ENTRIES; i++) {
            int n = snprintf(names[i], sizeof(names[i]), "f%d", i);
            CHECK(n > 0 && (size_t)n < sizeof(names[i]));
            CHECK(mcp_plugin_register(MCP_PLUGIN_TRANSPORT, names[i], &dummy_a) == MCP_OK);
        }
        CHECK(mcp_plugin_count(MCP_PLUGIN_TRANSPORT) == MCP_PLUGIN_MAX_ENTRIES);
        char extra[4];
        snprintf(extra, sizeof(extra), "x%d", MCP_PLUGIN_MAX_ENTRIES);
        CHECK(mcp_plugin_register(MCP_PLUGIN_TRANSPORT, extra, &dummy_a) == MCP_ERR_NOMEM);
        // Free one slot: the 33rd pair now fits into the reused slot.
        CHECK(mcp_plugin_unregister(MCP_PLUGIN_TRANSPORT, names[0]) == MCP_OK);
        CHECK(mcp_plugin_register(MCP_PLUGIN_TRANSPORT, extra, &dummy_a) == MCP_OK);
        for (int i = 1; i < MCP_PLUGIN_MAX_ENTRIES; i++) {
            CHECK(mcp_plugin_unregister(MCP_PLUGIN_TRANSPORT, names[i]) == MCP_OK);
        }
        CHECK(mcp_plugin_unregister(MCP_PLUGIN_TRANSPORT, extra) == MCP_OK);
        CHECK(mcp_plugin_count(MCP_PLUGIN_TRANSPORT) == 0);
    }
    return 0;
}
