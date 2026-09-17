#ifndef MCPKIT_PLUGIN_PLUGIN_H
#define MCPKIT_PLUGIN_PLUGIN_H

#include <stddef.h>

#include "mcpkit/core/error.h"

/**
 * @file plugin.h
 * Global static plugin registry (no ctx; uses builtin allocation).
 *
 * - At most MCP_PLUGIN_MAX_ENTRIES (32) distinct (kind, name) pairs may
 *   be registered simultaneously; first-fit slot reuse is used on
 *   unregister, so cycles of register/unregister of different names
 *   will not permanently exhaust the table.
 * - mcp_plugin_register(): MCP_ERR_ALREADY_EXISTS if the same
 *   (kind, name) is already registered; MCP_ERR_NOMEM if the table is
 *   full of distinct pairs.
 * - mcp_plugin_find(): returns a BORROWED const void* (the stored
 *   pointer); NULL if not found.
 * - The stored pointer is NOT dereferenced or validated; the caller
 *   is responsible for its lifetime.
 */

typedef enum {
    MCP_PLUGIN_JSON_BACKEND,
    MCP_PLUGIN_TRANSPORT,
    MCP_PLUGIN_ADAPTER,
} mcp_plugin_kind_t;

#define MCP_PLUGIN_MAX_ENTRIES 32

mcp_status_t mcp_plugin_register(mcp_plugin_kind_t kind,
                                  const char *name, const void *ptr);
mcp_status_t mcp_plugin_unregister(mcp_plugin_kind_t kind, const char *name);

/**
 * Returns the stored pointer for (kind, name), or NULL if absent.
 * The returned pointer is borrowed; the caller must not free it.
 */
const void *mcp_plugin_find(mcp_plugin_kind_t kind, const char *name);

/**
 * Returns the number of currently-registered plugins of the given kind.
 */
size_t mcp_plugin_count(mcp_plugin_kind_t kind);

#endif
