#ifndef MCPKIT_RUNTIME_LOOP_H
#define MCPKIT_RUNTIME_LOOP_H

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_executor mcp_executor_t;
typedef struct mcp_timer mcp_timer_t;

// Own-session serve loop over one transport: create session, then per line
// recv -> parse -> notify-or-dispatch -> send, until clean EOF (NOT_FOUND).
// ex/timer optional (NULL = inline dispatch / no timers); caller owns the
// transport lifecycle (start before, stop/destroy after).
mcp_status_t mcp_loop_run(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t,
                          mcp_executor_t *ex_or_null, mcp_timer_t *timer_or_null);

#endif
