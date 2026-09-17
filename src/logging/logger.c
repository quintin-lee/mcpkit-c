#include "mcpkit/logging/logger.h"

#include <stdio.h>

/* Private logger layout. The allocator snapshot captured at create time is
 * the one used by mcp_logger_destroy, so the logger always frees with the
 * allocator that created it (counting-allocator observability is preserved). */
struct mcp_logger {
    mcp_allocator_t alloc;   /* snapshot of the create-time allocator */
    mcp_log_sink_fn sink;    /* NULL sink is replaced with stderr at create */
    void *userdata;          /* forwarded to sink on every log call */
    mcp_log_level_t level;   /* minimum level emitted; default DEBUG */
};

/*
 * Returns a statically-allocated level name; no allocation, caller must not
 * free. "UNKNOWN" for out-of-range values so the map is total.
 */
const char *mcp_log_level_string(mcp_log_level_t level) {
    switch (level) {
        case MCP_LOG_DEBUG: return "DEBUG";
        case MCP_LOG_INFO: return "INFO";
        case MCP_LOG_WARN: return "WARN";
        case MCP_LOG_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void mcp_log_sink_stderr(mcp_log_level_t level, const char *message, void *userdata) {
    (void)userdata;
    if (message != NULL) {
        fprintf(stderr, "[%s] %s", mcp_log_level_string(level), message);
    }
}

mcp_logger_t *mcp_logger_create(const mcp_allocator_t *alloc, mcp_log_sink_fn sink, void *userdata) {
    const mcp_allocator_t *a = alloc != NULL ? alloc : mcp_default_allocator();
    mcp_logger_t *logger = a->malloc_fn(sizeof(*logger), a->userdata);
    if (logger == NULL) {
        return NULL;
    }
    logger->alloc = *a;
    logger->sink = sink != NULL ? sink : mcp_log_sink_stderr;
    logger->userdata = userdata;
    logger->level = MCP_LOG_DEBUG;
    return logger;
}

void mcp_logger_destroy(mcp_logger_t *logger) {
    if (logger == NULL) {
        return;
    }
    logger->alloc.free_fn(logger, logger->alloc.userdata);
}

void mcp_logger_set_level(mcp_logger_t *logger, mcp_log_level_t level) {
    if (logger != NULL) {
        logger->level = level;
    }
}

mcp_log_level_t mcp_logger_get_level(mcp_logger_t *logger) {
    if (logger == NULL) {
        return MCP_LOG_ERROR;
    }
    return logger->level;
}

void mcp_logger_log(mcp_logger_t *logger, mcp_log_level_t level, const char *message) {
    if (logger == NULL || message == NULL || level < logger->level) {
        return;
    }
    logger->sink(level, message, logger->userdata);
}

mcp_logger_t *mcp_logger_default_stderr(const mcp_allocator_t *alloc) {
    return mcp_logger_create(alloc, mcp_log_sink_stderr, NULL);
}
