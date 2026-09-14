#ifndef MCPKIT_CORE_ERROR_H
#define MCPKIT_CORE_ERROR_H

typedef enum {
    MCP_OK = 0,
    MCP_ERR_INVALID_ARGUMENT,
    MCP_ERR_NOMEM,
    MCP_ERR_IO,
    MCP_ERR_PROTOCOL,
    MCP_ERR_TIMEOUT,
    MCP_ERR_CANCELLED,
    MCP_ERR_NOT_FOUND,
    MCP_ERR_ALREADY_EXISTS,
    MCP_ERR_UNSUPPORTED,
    MCP_ERR_PERMISSION,
} mcp_status_t;

const char *mcp_status_string(mcp_status_t status);

#endif
