/**
 * @file registry.h
 * @brief MCP Server Registry Manifest (mcp.json) parser, validator, and serializer.
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_REGISTRY_H
#define MCPKIT_PROTOCOL_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mcp_context mcp_context_t;

/**
 * @brief Supported transport kinds in server registry manifest.
 */
typedef enum {
    MCP_TRANSPORT_KIND_UNKNOWN = 0,
    MCP_TRANSPORT_KIND_STDIO,
    MCP_TRANSPORT_KIND_STREAMABLE_HTTP,
    MCP_TRANSPORT_KIND_SOCKET
} mcp_registry_transport_kind_t;

/**
 * @brief Server manifest model representing mcp.json.
 */
typedef struct {
    char *name;
    char *version;
    char *description;
    char *author;
    char *license;
    char *repository;
    mcp_registry_transport_kind_t transport;
    bool has_tools;
    bool has_resources;
    bool has_prompts;
} mcp_registry_manifest_t;

/**
 * @brief Parses an mcp.json server manifest JSON document.
 *
 * Strings in m_out are allocated via ctx and owned by m_out.
 * Must be cleaned up with mcp_registry_manifest_cleanup.
 *
 * @param ctx Context; may be NULL.
 * @param json_str JSON input buffer.
 * @param len Length in bytes.
 * @param m_out Manifest destination struct.
 * @return MCP_OK on success, or an error code.
 */
mcp_status_t mcp_registry_manifest_parse(mcp_context_t *ctx, const char *json_str,
                                         size_t len, mcp_registry_manifest_t *m_out);

/**
 * @brief Validates an mcp.json server manifest against MCP Registry rules.
 *
 * Validation checks:
 * - name is non-NULL, non-empty, and valid identifier (alphanumeric, -, _, .)
 * - version is non-NULL, non-empty (valid semver format: X.Y.Z)
 * - transport is a known transport kind (STDIO, STREAMABLE_HTTP, SOCKET)
 *
 * If err_msg_out is non-NULL, it will be populated with a human-readable error description
 * on failure (must have at least 128 bytes of capacity).
 *
 * @param ctx Context; may be NULL.
 * @param m Manifest to validate.
 * @param err_msg_out Optional buffer for error description (minimum 128 bytes).
 * @return MCP_OK if valid, or MCP_ERR_INVALID_ARGUMENT if invalid.
 */
mcp_status_t mcp_registry_manifest_validate(mcp_context_t *ctx, const mcp_registry_manifest_t *m,
                                            char *err_msg_out);

/**
 * @brief Serializes an mcp.json server manifest to JSON string.
 *
 * Allocated via ctx; caller frees using mcp_registry_free_string(ctx, str).
 *
 * @param ctx Context; may be NULL.
 * @param m Manifest to serialize.
 * @return Serialized JSON string, or NULL on OOM.
 */
char *mcp_registry_manifest_serialize(mcp_context_t *ctx, const mcp_registry_manifest_t *m);

/**
 * @brief Frees a string returned by mcp_registry_manifest_serialize.
 *
 * @param ctx Context; may be NULL.
 * @param str String to free.
 */
void mcp_registry_free_string(mcp_context_t *ctx, char *str);

/**
 * @brief Frees all dynamically allocated strings in m and resets fields to zero.
 *
 * @param ctx Context; may be NULL.
 * @param m Manifest to clean up.
 */
void mcp_registry_manifest_cleanup(mcp_context_t *ctx, mcp_registry_manifest_t *m);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_REGISTRY_H */
