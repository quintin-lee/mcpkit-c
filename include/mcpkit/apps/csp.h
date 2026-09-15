#ifndef MCPKIT_APPS_CSP_H
#define MCPKIT_APPS_CSP_H

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_csp mcp_csp_t;

mcp_csp_t *mcp_csp_default_deny_new(mcp_context_t *ctx);
void mcp_csp_destroy(mcp_context_t *ctx, mcp_csp_t *csp);
mcp_status_t mcp_csp_set(mcp_context_t *ctx, mcp_csp_t *csp, const char *directive,
                         const char *sources_or_null);
mcp_status_t mcp_csp_serialize(mcp_context_t *ctx, const mcp_csp_t *csp, char **out);

#endif
