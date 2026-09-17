/**
 * @brief Content Security Policy (CSP) builder for MCP Apps resources.
 *
 * - mcp_csp_default_deny_new(): returns a caller-owned CSP object with
 *   all four directives set to "none".
 * - mcp_csp_set(): replaces the value of a directive; the previous value
 *   is freed with the same ctx used at creation.
 *   sources_or_null sets the directive to "none" (strict) when NULL.
 * - mcp_csp_serialize(): returns an owned heap string (the canonical
 *   CSP policy string); caller frees with mcp_json_free_string(ctx, s).
 */

#ifndef MCPKIT_APPS_CSP_H
#define MCPKIT_APPS_CSP_H

#include "mcpkit/core/error.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_csp mcp_csp_t;

/**
 * @brief Creates a caller-owned CSP object with all four directives set
 *        to "none" (default-deny).
 * @param ctx Context; may be NULL (default allocator).
 * @return Owned mcp_csp_t, or NULL on OOM.
 */
mcp_csp_t *mcp_csp_default_deny_new(mcp_context_t *ctx);

/**
 * @brief Destroys a CSP object.
 * @param ctx Context; may be NULL.
 * @param csp CSP to destroy; NULL is a no-op.
 */
void mcp_csp_destroy(mcp_context_t *ctx, mcp_csp_t *csp);

/**
 * @brief Sets the value of a CSP directive.
 *
 * The previous value is freed with the same ctx used at creation.
 *
 * @param ctx Context; may be NULL.
 * @param csp Target CSP object.
 * @param directive Directive name (e.g. "default-src", "script-src").
 * @param sources_or_null New value; NULL sets the directive to "none".
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if csp or
 *         directive is NULL; MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_csp_set(mcp_context_t *ctx, mcp_csp_t *csp, const char *directive,
                        const char *sources_or_null);

/**
 * @brief Serializes the CSP to a policy string.
 *
 * @param ctx Context; may be NULL.
 * @param csp CSP to serialize.
 * @param out Receives an owned heap string; caller must free with
 *            mcp_json_free_string(ctx, *out).
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if csp or out is
 *         NULL; MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_csp_serialize(mcp_context_t *ctx, const mcp_csp_t *csp, char **out);

#endif
