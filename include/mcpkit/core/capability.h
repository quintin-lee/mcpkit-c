/**
 * @brief Bitmask of protocol capability groups a server advertises.
 *
 * Pass to the server builder to gate which methods the dispatcher will
 * accept.
 */

#ifndef MCPKIT_CORE_CAPABILITY_H
#define MCPKIT_CORE_CAPABILITY_H

#include <stdbool.h>

typedef struct {
    bool tools;      /**< Accept tools/list and tools/call. */
    bool resources;  /**< Accept resources/list and resources/read. */
    bool prompts;    /**< Accept prompts/list and prompts/get. */
} mcp_capabilities_t;

/**
 * @brief Designated-initialiser value with all capabilities off.
 */
#define MCP_CAPABILITIES_INIT {false, false, false}

#endif
