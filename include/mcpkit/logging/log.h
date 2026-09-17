#ifndef MCPKIT_LOGGING_LOG_H
#define MCPKIT_LOGGING_LOG_H

/**
 * @file log.h
 * Log level definitions.
 *
 * Levels are ordered from most verbose to least; a logger configured
 * to `MCP_LOG_WARN` will emit WARN and ERROR messages but suppress
 * DEBUG and INFO.
 */

/** Severity levels, ordered ascending by importance. */
typedef enum {
    MCP_LOG_DEBUG = 0, /**< Detailed trace information. */
    MCP_LOG_INFO,      /**< Informational milestones. */
    MCP_LOG_WARN,      /**< Recoverable anomalies. */
    MCP_LOG_ERROR,     /**< Failures that may need operator attention. */
} mcp_log_level_t;

/**
 * Returns a stable, human-readable name for a log level
 * (e.g. `"INFO"`). The pointer is static and must not be freed.
 */
const char *mcp_log_level_string(mcp_log_level_t level);

#endif
