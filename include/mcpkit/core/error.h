#ifndef MCPKIT_CORE_ERROR_H
#define MCPKIT_CORE_ERROR_H

/**
 * @file error.h
 * Unified status codes returned by every mcpkit function.
 *
 * All public API functions return `mcp_status_t`; `MCP_OK` (0) means
 * success, any other value indicates the specific failure mode. The
 * codes are a closed set: unknown values in `mcp_status_string` map
 * to `"MCP_ERR_UNKNOWN"`.
 */

/** Return codes for every mcpkit API function. */
typedef enum {
    MCP_OK = 0,               /**< Success. */
    MCP_ERR_INVALID_ARGUMENT, /**< A pointer, string, or value is malformed. */
    MCP_ERR_NOMEM,            /**< Out of memory; no partial state left behind. */
    MCP_ERR_IO,               /**< Underlying file/stream I/O failed (includes EOF). */
    MCP_ERR_PROTOCOL,         /**< Malformed wire data or protocol violation. */
    MCP_ERR_TIMEOUT,          /**< An operation exceeded its deadline. */
    MCP_ERR_CANCELLED,        /**< The operation was cancelled before completion. */
    MCP_ERR_NOT_FOUND,        /**< The named entity does not exist. */
    MCP_ERR_ALREADY_EXISTS,   /**< The named entity is already registered. */
    MCP_ERR_UNSUPPORTED,      /**< The requested capability is not built in this SDK. */
    MCP_ERR_PERMISSION,      /**< The caller lacks a required permission bit. */
} mcp_status_t;

/**
 * Returns a human-readable, stable string for a status code.
 * The returned pointer is static and valid for the lifetime of the
 * process; the caller must not free it.
 */
const char *mcp_status_string(mcp_status_t status);

#endif
