/**
 * @file result.h
 * @brief Lightweight (status, message) pair for functions that cannot
 * return an out-parameter.
 *
 * `message` is always borrowed (typically a static string) and must
 * never be freed by the caller.
 */

#ifndef MCPKIT_CORE_RESULT_H
#define MCPKIT_CORE_RESULT_H

#include "mcpkit/core/error.h"

typedef struct {
  mcp_status_t status; /**< Operation status code. */
  const char *message; /**< Borrowed diagnostic message; may be NULL. */
} mcp_result_t;

/**
 * @brief Compile-time constant representing a clean success.
 */
#define MCP_RESULT_OK ((mcp_result_t){.status = MCP_OK, .message = NULL})

#endif
