#ifndef MCPKIT_CORE_RESULT_H
#define MCPKIT_CORE_RESULT_H

#include "mcpkit/core/error.h"

/**
 * Lightweight (status, message) pair used by functions that cannot
 * return an out-parameter. `message` is always borrowed (typically a
 * static string) and must never be freed by the caller.
 */
typedef struct {
  mcp_status_t status;
  const char *message; /**< Borrowed, may be NULL; caller never owns. */
} mcp_result_t;

/** Compile-time constant representing a clean success. */
#define MCP_RESULT_OK ((mcp_result_t){.status = MCP_OK, .message = NULL})

#endif
