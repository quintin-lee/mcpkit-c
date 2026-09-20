/**
 * @file logger.h
 * @brief Pluggable logger with a level filter and swappable sink.
 *
 * The logger routes every log call through a user-supplied sink
 * function, passing through the level and message. Messages below
 * the configured threshold are discarded before reaching the sink,
 * so sinks can be expensive (e.g. network) without paying for
 * suppressed levels.
 */

#ifndef MCPKIT_LOGGING_LOGGER_H
#define MCPKIT_LOGGING_LOGGER_H

#include <stddef.h>

#include "mcpkit/core/types.h"
#include "mcpkit/logging/log.h"


/** Opaque logger handle. */
typedef struct mcp_logger mcp_logger_t;

/**
 * @brief Sink callback signature.
 *
 * @param level Log level of the message.
 * @param message Null-terminated log message; valid only for the
 *                duration of the call.
 * @param userdata Pointer supplied to mcp_logger_create; not
 *                 interpreted by the logger.
 *
 * The sink must not call back into the logger that owns it while
 * the message is in flight.
 */
typedef void (*mcp_log_sink_fn)(mcp_log_level_t level, const char *message, void *userdata);

/**
 * @brief Creates a logger.
 *
 * @param alloc Allocator for internal buffers; NULL routes to libc.
 * @param sink Message sink; NULL routes to the built-in stderr sink.
 * @param userdata Passed through to sink calls; may be NULL.
 *
 * The level is initialized to MCP_LOG_DEBUG (all messages pass).
 * Caller owns the logger on success; release with mcp_logger_destroy.
 *
 * @return Owned mcp_logger_t, or NULL on OOM.
 */
mcp_logger_t *mcp_logger_create(const mcp_allocator_t *alloc, mcp_log_sink_fn sink,
                                void *userdata);

/**
 * @brief Destroys a logger.
 * @param logger Logger to destroy; NULL is a no-op.
 */
void mcp_logger_destroy(mcp_logger_t *logger);

/**
 * @brief Sets the minimum level that reaches the sink.
 *
 * Messages with a level numerically lower than the threshold are
 * discarded before reaching the sink.
 *
 * @param logger Target logger; NULL is a no-op.
 * @param level New minimum level.
 */
void mcp_logger_set_level(mcp_logger_t *logger, mcp_log_level_t level);

/**
 * @brief Returns the current minimum level.
 * @param logger Target logger; NULL returns MCP_LOG_ERROR.
 * @return Current minimum log level.
 */
mcp_log_level_t mcp_logger_get_level(mcp_logger_t *logger);

/**
 * @brief Emits a message at the given level.
 *
 * If the level passes the threshold the sink is invoked; otherwise
 * the call is a no-op. Safe to call with a NULL logger (discarded
 * silently).
 *
 * @param logger Target logger; NULL discards silently.
 * @param level Level of the message.
 * @param message Null-terminated message; may be NULL (no-op).
 */
void mcp_logger_log(mcp_logger_t *logger, mcp_log_level_t level, const char *message);

/**
 * @brief Emits a printf-style message at the given level.
 *
 * Formats into a 256-byte stack buffer (255 chars + NUL); longer
 * output is truncated. Otherwise identical to mcp_logger_log:
 * NULL logger or NULL fmt discards silently, and messages below
 * the threshold never reach the sink.
 *
 * @param logger Target logger; NULL discards silently.
 * @param level Level of the message.
 * @param fmt printf-style format; may be NULL (no-op).
 */
void mcp_logger_logf(mcp_logger_t *logger, mcp_log_level_t level, const char *fmt, ...);

/**
 * @brief Built-in stderr sink.
 *
 * Writes `[LEVEL] message\n` to stderr. Suitable for use directly as
 * a mcp_log_sink_fn.
 *
 * @param level Log level of the message.
 * @param message Message to print.
 * @param userdata Ignored.
 */
void mcp_log_sink_stderr(mcp_log_level_t level, const char *message, void *userdata);

/**
 * @brief Convenience constructor for a stderr-backed logger at the
 *        default (debug) level.
 * @param alloc Allocator for internal buffers; NULL routes to libc.
 * @return Owned mcp_logger_t, or NULL on OOM.
 */
mcp_logger_t *mcp_logger_default_stderr(const mcp_allocator_t *alloc);

#endif
