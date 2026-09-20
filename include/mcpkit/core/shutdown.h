/**
 * @file shutdown.h
 * @brief Process-wide graceful-shutdown flag for serve loops.
 *
 * A single async-signal-safe flag shared by all serve loops in the
 * process. The library never installs signal handlers: the host
 * installs its own SIGTERM/SIGINT handler that calls
 * mcp_request_shutdown(), and mcp_loop_run / mcp_stdio_serve drain
 * the in-flight request, destroy the session, and return
 * MCP_ERR_CANCELLED. The flag is stored in a volatile sig_atomic_t
 * so setting it from a signal handler is safe.
 */

#ifndef MCPKIT_CORE_SHUTDOWN_H
#define MCPKIT_CORE_SHUTDOWN_H

#include <stdbool.h>

/**
 * @brief Requests shutdown of all serve loops in the process.
 *
 * Async-signal-safe: safe to call directly from a SIGTERM/SIGINT
 * handler. Takes effect at the next loop-iteration check; a request
 * already being dispatched runs to completion first (drain).
 */
void mcp_request_shutdown(void);

/**
 * @brief Returns true if shutdown has been requested since the last clear.
 *
 * @return Flag state; false on a fresh process or after mcp_shutdown_clear().
 */
bool mcp_shutdown_requested(void);

/**
 * @brief Clears a pending shutdown request.
 *
 * Loops never clear the flag themselves, so a request made before a
 * run takes effect immediately. A host that reuses the process for a
 * fresh run after a cancelled one must call this first.
 */
void mcp_shutdown_clear(void);

#endif
