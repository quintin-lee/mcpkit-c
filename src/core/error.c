/**
 * @file error.c
 *
 * Table-driven mapping from mcp_status_t to a stable, human-readable
 * name. Unrecognised values return "MCP_ERR_UNKNOWN".
 */
#include "mcpkit/core/error.h"

const char *mcp_status_string(mcp_status_t status) {
    switch (status) {
        case MCP_OK: return "MCP_OK";
        case MCP_ERR_INVALID_ARGUMENT: return "MCP_ERR_INVALID_ARGUMENT";
        case MCP_ERR_NOMEM: return "MCP_ERR_NOMEM";
        case MCP_ERR_IO: return "MCP_ERR_IO";
        case MCP_ERR_PROTOCOL: return "MCP_ERR_PROTOCOL";
        case MCP_ERR_TIMEOUT: return "MCP_ERR_TIMEOUT";
        case MCP_ERR_CANCELLED: return "MCP_ERR_CANCELLED";
        case MCP_ERR_NOT_FOUND: return "MCP_ERR_NOT_FOUND";
        case MCP_ERR_ALREADY_EXISTS: return "MCP_ERR_ALREADY_EXISTS";
        case MCP_ERR_UNSUPPORTED: return "MCP_ERR_UNSUPPORTED";
        case MCP_ERR_PERMISSION: return "MCP_ERR_PERMISSION";
        default: return "MCP_ERR_UNKNOWN";
    }
}
