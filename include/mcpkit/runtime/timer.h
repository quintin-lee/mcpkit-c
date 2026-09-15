#ifndef MCPKIT_RUNTIME_TIMER_H
#define MCPKIT_RUNTIME_TIMER_H

#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/runtime/task.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_timer mcp_timer_t;

mcp_timer_t *mcp_timer_create(mcp_context_t *ctx);
void mcp_timer_destroy(mcp_context_t *ctx, mcp_timer_t *t);
// Schedule fn to fire after delay_ms via CLOCK_MONOTONIC. delay 0 fires on
// the next poll. Returns MCP_OK; never fails except NULL args / NOMEM.
mcp_status_t mcp_timer_schedule(mcp_context_t *ctx, mcp_timer_t *t, uint64_t delay_ms,
                                mcp_task_fn fn, void *arg);
// Cancel all pending entries for (fn, arg). Returns count cancelled.
size_t mcp_timer_cancel(mcp_context_t *ctx, mcp_timer_t *t, mcp_task_fn fn, void *arg);
// Run all due callbacks inline. Returns MCP_OK, *fired_out gets the count
// (optional, may be NULL).
mcp_status_t mcp_timer_poll(mcp_context_t *ctx, mcp_timer_t *t, size_t *fired_out);

#endif
