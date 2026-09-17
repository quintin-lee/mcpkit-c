#ifndef MCPKIT_LOGGING_LOGGER_H
#define MCPKIT_LOGGING_LOGGER_H

#include <stddef.h>

#include "mcpkit/core/types.h"
#include "mcpkit/logging/log.h"

/**
 * @file logger.h
 * Pluggable logger with a level filter and swappable sink.
 *
 * The logger routes every log call through a user-supplied sink
 * function, passing through the level and message. Messages below
 * the configured threshold are discarded before reaching the sink,
 * so sinks can be expensive (e.g. network) without paying for
 * suppressed levels.
 */

/** Opaque logger handle. */
typedef struct mcp_logger mcp_logger_t;

/**
 * Sink callback signature. `userdata` is the pointer supplied to
 * `mcp_logger_create` and is not interpreted by the logger.
 * The sink must not call back into the logger that owns it while
 * the message is in flight.
 */
typedef void (*mcp_log_sink_fn)(mcp_log_level_t level, const char *message, void *userdata);

/**
 * Creates a logger.
 * - `alloc`  - allocator to use for internal buffers; NULL → libc.
 * - `sink`   - message sink; NULL → the built-in stderr sink.
 * - `userdata` - passed through to sink calls; may be NULL.
 *
 * The level is initialized to `MCP_LOG_DEBUG` (all messages pass).
 * Caller owns the logger on success; release with `mcp_logger_destroy`.
 */
mcp_logger_t *mcp_logger_create(const mcp_allocator_t *alloc, mcp_log_sink_fn sink,
                                void *userdata);

/** Destroys a logger; NULL-safe. */
void mcp_logger_destroy(mcp_logger_t *logger);

/**
 * Sets the minimum level that reaches the sink. Messages with a
 * level numerically lower than the threshold are discarded.
 */
void mcp_logger_set_level(mcp_logger_t *logger, mcp_log_level_t level);

/** Returns the current minimum level. */
mcp_log_level_t mcp_logger_get_level(mcp_logger_t *logger);

/**
 * Emits a message. If the level passes the threshold the sink is
 * invoked; otherwise the call is a no-op. Safe to call with a NULL
 * logger (discarded silently).
 */
void mcp_logger_log(mcp_logger_t *logger, mcp_log_level_t level, const char *message);

/**
 * Built-in stderr sink. Writes `[LEVEL] message\n` to stderr.
 * Suitable for use directly as a `mcp_log_sink_fn`.
 */
void mcp_log_sink_stderr(mcp_log_level_t level, const char *message, void *userdata);

/**
 * Convenience constructor for a stderr-backed logger at the default
 * (debug) level. `alloc` NULL → libc.
 */
mcp_logger_t *mcp_logger_default_stderr(const mcp_allocator_t *alloc);

#endif
