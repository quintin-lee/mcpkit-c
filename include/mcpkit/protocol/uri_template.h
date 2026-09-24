/**
 * @file uri_template.h
 * @brief RFC 6570 URI Template matching and expansion (Level 1 & 2).
 *
 * Implements URI template matching against concrete URIs to extract variables,
 * and URI template expansion to generate concrete URIs from variable values.
 *
 * Supports:
 *  - Level 1: Simple string expansion `{var}` (matches path segment, unreserved chars).
 *  - Level 2: Reserved expansion `{+var}` (matches path segment including reserved characters like `/`).
 *
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_URI_TEMPLATE_H
#define MCPKIT_PROTOCOL_URI_TEMPLATE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/error.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Matches a concrete URI against an RFC 6570 URI template pattern.
 *
 * If the URI matches the pattern, extracted variables are populated into
 * a caller-owned JSON object `{"var_name": "val", ...}`.
 *
 * @param ctx            Context; may be NULL (default allocator).
 * @param pattern        RFC 6570 URI template (e.g. "file:///{+path}" or "items/{id}").
 * @param uri            Concrete URI to match (e.g. "file:///docs/readme.md").
 * @param out_variables  Receives a caller-owned JSON object containing extracted
 *                       variable key-value string pairs on MCP_OK. May be NULL if
 *                       caller only wants to check matching without extracting.
 * @return MCP_OK if URI matches pattern;
 *         MCP_ERR_NOT_FOUND if URI does not match pattern;
 *         MCP_ERR_INVALID_ARGUMENT if pattern or uri is NULL;
 *         MCP_ERR_NOMEM on memory allocation failure.
 */
mcp_status_t mcp_uri_template_match(mcp_context_t *ctx,
                                    const char *pattern,
                                    const char *uri,
                                    mcp_json_value_t **out_variables);

/**
 * @brief Expands an RFC 6570 URI template with provided variables.
 *
 * @param ctx            Context; may be NULL (default allocator).
 * @param pattern        RFC 6570 URI template (e.g. "file:///{+path}").
 * @param variables      JSON object containing variable string values.
 * @param out_uri        Receives a caller-owned NUL-terminated expanded URI string
 *                       on MCP_OK (free with mcp_uri_template_free_string).
 * @return MCP_OK on success;
 *         MCP_ERR_INVALID_ARGUMENT if pattern or out_uri is NULL;
 *         MCP_ERR_NOMEM on memory allocation failure.
 */
mcp_status_t mcp_uri_template_expand(mcp_context_t *ctx,
                                     const char *pattern,
                                     const mcp_json_value_t *variables,
                                     char **out_uri);

/**
 * @brief Frees a string returned by mcp_uri_template_expand.
 *
 * @param ctx Context; must match the context passed to mcp_uri_template_expand.
 * @param uri String to free; NULL is safe.
 */
void mcp_uri_template_free_string(mcp_context_t *ctx, char *uri);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_URI_TEMPLATE_H */
