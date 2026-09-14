#ifndef MCPKIT_CORE_RESULT_H
#define MCPKIT_CORE_RESULT_H

#include "mcpkit/core/error.h"

typedef struct {
    mcp_status_t status;
    const char *message; // borrowed, may be NULL; never owned
} mcp_result_t;

#define MCP_RESULT_OK ((mcp_result_t){.status = MCP_OK, .message = NULL})

#endif
