/**
 * @file version.h
 * MCP protocol version enumeration and comparison utilities.
 *
 * Provides typed representation of MCP protocol versions to replace
 * string-based comparisons in dispatchers and route handlers.
 *
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_VERSION_H
#define MCPKIT_PROTOCOL_VERSION_H

#include <stdbool.h>

/**
 * @brief Known MCP protocol versions in chronological order.
 *
 * Value 0 (MCP_PROTO_NONE) represents unnegotiated, unknown, or
 * invalid version strings.
 */
typedef enum {
    MCP_PROTO_NONE = 0,            /**< Unknown or unnegotiated */
    MCP_PROTO_2024_11_05,          /**< Initial public spec (2024-11-05) */
    MCP_PROTO_2025_03_26,          /**< 2025-03-26 spec */
    MCP_PROTO_2025_06_18,          /**< 2025-06-18 spec */
    MCP_PROTO_2025_11_25,          /**< 2025-11-25 spec (LATEST stable) */
    MCP_PROTO_2026_07_28,          /**< 2026-07-28 spec (stateless core) */
} mcp_proto_version_t;

/**
 * @brief Converts a protocol version enum to its canonical string representation.
 *
 * @param v Version enum value.
 * @return Static string pointer (e.g. "2025-11-25"), or NULL if v is MCP_PROTO_NONE
 *         or unknown.
 */
const char *mcp_proto_version_string(mcp_proto_version_t v);

/**
 * @brief Parses a protocol version string into its enum representation.
 *
 * @param s Version string (e.g. "2025-11-25"); may be NULL.
 * @return Corresponding enum value, or MCP_PROTO_NONE if NULL or unrecognized.
 */
mcp_proto_version_t mcp_proto_version_parse(const char *s);

/**
 * @brief Checks if version @p v meets or exceeds minimum version @p min.
 *
 * MCP_PROTO_NONE never satisfies any minimum version (returns false),
 * and is not satisfied by any version if min is not NONE.
 *
 * @param v   The active or negotiated version to test.
 * @param min The minimum required version.
 * @return true if v >= min and v != MCP_PROTO_NONE; false otherwise.
 */
bool mcp_proto_at_least(mcp_proto_version_t v, mcp_proto_version_t min);

#endif /* MCPKIT_PROTOCOL_VERSION_H */
