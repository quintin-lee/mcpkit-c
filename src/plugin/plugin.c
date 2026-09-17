/**
 * @file plugin.c
 *
 * Global static plugin registry (no context; mirrors the built-in
 * JSON backend precedent). Fixed 32-slot table with first-fit reuse:
 * a slot released by mcp_plugin_unregister becomes available to a new
 * (kind,name) pair, so register/unregister cycles do not permanently
 * exhaust the table. Stored pointers are borrowed — the caller must
 * keep the pointee alive for the registration lifetime.
 */
#include "mcpkit/plugin/plugin.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    bool used;
    mcp_plugin_kind_t kind;
    const char *name;
    const void *ptr;
} plugin_entry_t;

static plugin_entry_t g_entries[MCP_PLUGIN_MAX_ENTRIES];

mcp_status_t mcp_plugin_register(mcp_plugin_kind_t kind, const char *name, const void *ptr) {
    if (name == NULL || ptr == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    // First-fit scan: a slot released by mcp_plugin_unregister becomes
    // reusable for a new (kind,name) pair. The fixed 32-slot table therefore
    // only rejects when 32 distinct pairs are registered simultaneously.
    for (size_t i = 0; i < MCP_PLUGIN_MAX_ENTRIES; i++) {
        if (!g_entries[i].used) {
            g_entries[i].used = true;
            g_entries[i].kind = kind;
            g_entries[i].name = name;
            g_entries[i].ptr = ptr;
            return MCP_OK;
        }
        if (g_entries[i].used && g_entries[i].kind == kind
            && strcmp(g_entries[i].name, name) == 0) {
            return MCP_ERR_ALREADY_EXISTS;
        }
    }
    return MCP_ERR_NOMEM;
}

mcp_status_t mcp_plugin_unregister(mcp_plugin_kind_t kind, const char *name) {
    if (name == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < MCP_PLUGIN_MAX_ENTRIES; i++) {
        if (g_entries[i].used && g_entries[i].kind == kind
            && strcmp(g_entries[i].name, name) == 0) {
            g_entries[i].used = false;
            g_entries[i].name = NULL;
            g_entries[i].ptr = NULL;
            return MCP_OK;
        }
    }
    return MCP_ERR_NOT_FOUND;
}

const void *mcp_plugin_find(mcp_plugin_kind_t kind, const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < MCP_PLUGIN_MAX_ENTRIES; i++) {
        if (g_entries[i].used && g_entries[i].kind == kind
            && strcmp(g_entries[i].name, name) == 0) {
            return g_entries[i].ptr;
        }
    }
    return NULL;
}

size_t mcp_plugin_count(mcp_plugin_kind_t kind) {
    size_t n = 0;
    for (size_t i = 0; i < MCP_PLUGIN_MAX_ENTRIES; i++) {
        if (g_entries[i].used && g_entries[i].kind == kind) {
            n++;
        }
    }
    return n;
}
