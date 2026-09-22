/**
 * @file loop.h
 * @brief Runs a synchronous serve loop on a single transport.
 * @ingroup mcpkit-runtime
 *
 * Creates one session for the transport's lifetime. For each line:
 * recv -> parse -> notify-or-dispatch -> send. When an executor is
 * provided, dispatch is submitted and waited (sequential,
 * session-safe); otherwise it runs inline. Timers are polled before
 * each recv.
 *
 * The caller manages the transport lifecycle: call mcp_transport_start
 * before mcp_loop_run and mcp_transport_stop/destroy after.
 */

#ifndef MCPKIT_RUNTIME_LOOP_H
#define MCPKIT_RUNTIME_LOOP_H

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_executor mcp_executor_t;
typedef struct mcp_timer mcp_timer_t;
typedef struct mcp_client mcp_client_t;

/**
 * @brief Runs a synchronous serve loop on a single transport.
 *
 * Creates one session for the transport's lifetime. For each line:
 * recv -> parse -> notify-or-dispatch -> send. When an executor is
 * provided, dispatch is submitted and waited (sequential,
 * session-safe); otherwise it runs inline. Timers are polled before
 * each recv.
 *
 * The caller manages the transport lifecycle: call mcp_transport_start
 * before mcp_loop_run and mcp_transport_stop/destroy after.
 *
 * Shutdown: if mcp_request_shutdown() was called (e.g. from the
 * host's own SIGTERM/SIGINT handler — the library installs none),
 * the in-flight request runs to completion and the loop returns
 * MCP_ERR_CANCELLED. A finite recv timeout lets an idle loop wake
 * up promptly to observe the flag.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server to dispatch to.
 * @param t Transport; caller must have called mcp_transport_start.
 * @param ex_or_null Optional executor for parallel dispatch; NULL for
 *                   inline (sequential) dispatch.
 * @param timer_or_null Optional timer list polled before each recv.
 * @return MCP_OK on clean EOF; MCP_ERR_IO on I/O failure;
 *         MCP_ERR_CANCELLED on requested shutdown.
 */
mcp_status_t mcp_loop_run(mcp_context_t *ctx, mcp_server_t *server,
                          mcp_transport_t *t,
                          mcp_executor_t *ex_or_null,
                          mcp_timer_t *timer_or_null);

/**
 * @brief Runs a synchronous serve loop that also routes server-originated
 *        requests (roots/list, sampling/createMessage, elicitation/create)
 *        to an optional client.
 *
 * Behaves identically to mcp_loop_run when client is NULL.  When client
 * is non-NULL, incoming REQUEST messages whose method is one of the three
 * server-to-client methods are routed to mcp_client_handle_server_request
 * instead of the executor/inline dispatch path; the response is
 * serialized and sent back over the transport.  All other methods still
 * go through dispatch_inline (executor or inline mcp_server_dispatch).
 *
 * The client's provider callbacks (mcp_client_set_roots_provider, etc.)
 * determine what is returned; if no provider is registered the client
 * replies -32601.
 *
 * The caller manages the transport lifecycle: call mcp_transport_start
 * before this function and mcp_transport_stop/destroy after.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server to dispatch to.
 * @param client Optional client for server-originated requests; NULL
 *               disables that routing (same as mcp_loop_run).
 * @param t Transport; caller must have called mcp_transport_start.
 * @param ex_or_null Optional executor for parallel dispatch; NULL for
 *                   inline (sequential) dispatch.
 * @param timer_or_null Optional timer list polled before each recv.
 * @return MCP_OK on clean EOF; MCP_ERR_IO on I/O failure;
 *         MCP_ERR_CANCELLED on requested shutdown.
 */
mcp_status_t mcp_loop_run_with_client(mcp_context_t *ctx, mcp_server_t *server,
                                      mcp_client_t *client,
                                      mcp_transport_t *t,
                                      mcp_executor_t *ex_or_null,
                                      mcp_timer_t *timer_or_null);

#endif
