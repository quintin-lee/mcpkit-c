/**
 * @file shutdown.c
 *
 * Process-wide shutdown flag. A plain volatile sig_atomic_t load/store
 * is async-signal-safe; no lock needed and none wanted (a mutex would
 * be illegal inside a signal handler).
 */
#include "mcpkit/core/shutdown.h"

#include <signal.h>

static volatile sig_atomic_t g_shutdown = 0;

void mcp_request_shutdown(void) {
    g_shutdown = 1;
}

bool mcp_shutdown_requested(void) {
    return g_shutdown != 0;
}

void mcp_shutdown_clear(void) {
    g_shutdown = 0;
}
