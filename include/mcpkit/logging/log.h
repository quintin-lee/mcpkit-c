#ifndef MCPKIT_LOGGING_LOG_H
#define MCPKIT_LOGGING_LOG_H

typedef enum {
    MCP_LOG_DEBUG = 0,
    MCP_LOG_INFO,
    MCP_LOG_WARN,
    MCP_LOG_ERROR,
} mcp_log_level_t;

const char *mcp_log_level_string(mcp_log_level_t level);

#endif
