/**
 * @defgroup mcpkit-logging Logging
 *
 * Pluggable logger with a level filter and swappable sink. The library
 * uses it for lifecycle and error diagnostics; hosts can route logs to
 * their own sink or replace the logger entirely via
 * mcp_context_create.
 *
 * @{
 */
/**
 * @file log.h
 * @brief Log level definitions.
 * @ingroup mcpkit-logging
 *
 * Levels are ordered from most verbose to least; a logger configured
 * to `MCP_LOG_WARN` will emit WARN and ERROR messages but suppress
 * DEBUG and INFO.
 */

#ifndef MCPKIT_LOGGING_LOG_H
#define MCPKIT_LOGGING_LOG_H


/**
 * @brief Severity levels, ordered ascending by importance.
 */
typedef enum {
    MCP_LOG_DEBUG = 0, /**< Detailed trace information. */
    MCP_LOG_INFO,      /**< Informational milestones. */
    MCP_LOG_WARN,      /**< Recoverable anomalies. */
    MCP_LOG_ERROR,     /**< Failures that may need operator attention. */
} mcp_log_level_t;

/**
 * @brief Returns a stable, human-readable name for a log level.
 *
 * @param level Any value in the mcp_log_level_t range; out-of-range
 *              values return "UNKNOWN" so the map is total.
 * @return Static string pointer (e.g. "INFO"); must NOT be freed.
 */
const char *mcp_log_level_string(mcp_log_level_t level);

/**
 * @brief End of the mcpkit-logging group.
 *
 * @}
 */

#endif
