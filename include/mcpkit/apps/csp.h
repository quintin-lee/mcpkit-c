#ifndef MCPKIT_APPS_CSP_H
#define MCPKIT_APPS_CSP_H

#include "mcpkit/core/error.h"

/**
 * @file csp.h
 * Content Security Policy (CSP) builder for MCP Apps resources.
 *
 * - mcp_csp_default_deny_new(): returns a caller-owned CSP object with
 *   all four directives set to "none".
 * - mcp_csp_set(): replaces the value of a directive; the previous value
 *   is freed with the same ctx used at creation.
 *   sources_or_null sets the directive to "none" (strict) when NULL.
 * - mcp_csp_serialize(): returns an owned heap string (the canonical
 *   CSP policy string); caller frees with mcp_json_free_string(ctx, s).
 */

typedef struct mcp_context mcp_context_t;
typedef struct mcp_csp mcp_csp_t;

mcp_csp_t *mcp_csp_default_deny_new(mcp_context_t *ctx);
void mcp_csp_destroy(mcp_context_t *ctx, mcp_csp_t *csp);
mcp_status_t mcp_csp_set(mcp_context_t *ctx, mcp_csp_t *csp, const char *directive,
                          const char *sources_or_null);

/**
 * Serializes the CSP to a policy string. *out is an owned heap string;
 * caller must free it with mcp_json_free_string(ctx, *out).
 */
mcp_status_t mcp_csp_serialize(mcp_context_t *ctx, const mcp_csp_t *csp, char **out);

#endif
