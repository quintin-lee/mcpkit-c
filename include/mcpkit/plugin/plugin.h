#ifndef MCPKIT_PLUGIN_PLUGIN_H
#define MCPKIT_PLUGIN_PLUGIN_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef enum {
    MCP_PLUGIN_JSON_BACKEND,
    MCP_PLUGIN_TRANSPORT,
    MCP_PLUGIN_ADAPTER,
} mcp_plugin_kind_t;

#define MCP_PLUGIN_MAX_ENTRIES 32

mcp_status_t mcp_plugin_register(mcp_plugin_kind_t kind, const char *name, const void *ptr);
mcp_status_t mcp_plugin_unregister(mcp_plugin_kind_t kind, const char *name);
const void *mcp_plugin_find(mcp_plugin_kind_t kind, const char *name);
size_t mcp_plugin_count(mcp_plugin_kind_t kind);

#endif
