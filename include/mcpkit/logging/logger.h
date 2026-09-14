#ifndef MCPKIT_LOGGING_LOGGER_H
#define MCPKIT_LOGGING_LOGGER_H

#include <stddef.h>

#include "mcpkit/core/types.h"
#include "mcpkit/logging/log.h"

typedef struct mcp_logger mcp_logger_t;
typedef void (*mcp_log_sink_fn)(mcp_log_level_t level, const char *message, void *userdata);

// alloc NULL -> libc. sink NULL -> stderr sink.
mcp_logger_t *mcp_logger_create(const mcp_allocator_t *alloc, mcp_log_sink_fn sink, void *userdata);
void mcp_logger_destroy(mcp_logger_t *logger);
void mcp_logger_set_level(mcp_logger_t *logger, mcp_log_level_t level);
mcp_log_level_t mcp_logger_get_level(mcp_logger_t *logger);
void mcp_logger_log(mcp_logger_t *logger, mcp_log_level_t level, const char *message);
void mcp_log_sink_stderr(mcp_log_level_t level, const char *message, void *userdata);
mcp_logger_t *mcp_logger_default_stderr(const mcp_allocator_t *alloc);

#endif
