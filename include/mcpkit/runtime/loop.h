#ifndef MCPKIT_RUNTIME_LOOP_H
#define MCPKIT_RUNTIME_LOOP_H

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_executor mcp_executor_t;
typedef struct mcp_timer mcp_timer_t;

/**
 * @file loop.h
 * Synchronous single-transport serve loop.
 *
 * - Creates one session for the transport's lifetime.
 * - For each line: recv → parse → notify-or-dispatch → send.
 *   When an executor is provided, dispatch is submitted and waited
 *   (sequential, session-safe); otherwise it runs inline.
 * - Timers are polled before each recv.
 * - Returns MCP_OK on clean EOF (MCP_ERR_NOT_FOUND mapped from the
 *   transport's end-of-stream signal).
 * - The caller manages the transport lifecycle: call mcp_transport_start
 *   before mcp_loop_run and mcp_transport_stop/destroy after.
 */
mcp_status_t mcp_loop_run(mcp_context_t *ctx, mcp_server_t *server,
                           mcp_transport_t *t,
                           mcp_executor_t *ex_or_null,
                           mcp_timer_t *timer_or_null);

#endif
