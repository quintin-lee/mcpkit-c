#ifndef MCPKIT_PLUGIN_PLUGIN_H
#define MCPKIT_PLUGIN_PLUGIN_H

#include <stddef.h>

#include "mcpkit/core/error.h"

/**
 * @brief Global static plugin registry (no ctx; uses builtin allocation).
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

/**
 * @brief Registers a plugin pointer in the global registry.
 *
 * The registry uses builtin (libc) allocation and has no ctx; it is
 * process-global.
 *
 * @param kind Plugin category.
 * @param name Plugin name; strdup'd internally; caller may free.
 * @param ptr Opaque pointer stored by value; not owned by the registry.
 * @return MCP_OK on success; MCP_ERR_ALREADY_EXISTS if (kind, name) is
 *         already registered; MCP_ERR_NOMEM if all 32 slots are full
 *         of distinct pairs.
 */
mcp_status_t mcp_plugin_register(mcp_plugin_kind_t kind,
                                 const char *name, const void *ptr);

/**
 * @brief Unregisters a plugin by (kind, name).
 *
 * Frees the slot; first-fit slot reuse keeps the table usable for
 * future registrations of different names.
 *
 * @param kind Plugin category.
 * @param name Exact name used at registration.
 * @return MCP_OK on success; MCP_ERR_NOT_FOUND if (kind, name) is not
 *         registered.
 */
mcp_status_t mcp_plugin_unregister(mcp_plugin_kind_t kind, const char *name);

/**
 * @brief Looks up the stored pointer for (kind, name).
 *
 * @param kind Plugin category.
 * @param name Plugin name.
 * @return BORROWED const void* (the stored pointer), or NULL if absent.
 *         Caller must not free the returned pointer.
 */
const void *mcp_plugin_find(mcp_plugin_kind_t kind, const char *name);

/**
 * @brief Returns the number of currently-registered plugins of the
 *        given kind.
 * @param kind Plugin category.
 * @return Count of registered entries for that kind.
 */
size_t mcp_plugin_count(mcp_plugin_kind_t kind);

#endif
