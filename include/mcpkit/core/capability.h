#ifndef MCPKIT_CORE_CAPABILITY_H
#define MCPKIT_CORE_CAPABILITY_H

#include <stdbool.h>

typedef struct {
    bool tools;
    bool resources;
    bool prompts;
} mcp_capabilities_t;

#define MCP_CAPABILITIES_INIT {false, false, false}

#endif
