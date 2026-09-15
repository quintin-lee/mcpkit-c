#ifndef MCPKIT_SERVER_SESSION_H
#define MCPKIT_SERVER_SESSION_H

#include <stdbool.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;

bool mcp_session_is_initialized(mcp_context_t *ctx, const mcp_session_t *session);

#endif
