#include "test_check.h"
#include <string.h>

#include "mcpkit/logging/log.h"
#include "mcpkit/logging/logger.h"

static char g_buf[256];
static size_t g_len;
static void mem_sink(mcp_log_level_t level, const char *msg, void *ud) {
    (void)level;
    (void)ud;
    size_t n = strlen(msg);
    if (g_len + n < sizeof(g_buf)) {
        memcpy(g_buf + g_len, msg, n);
        g_len += n;
    }
}

int main(void) {
    CHECK(strcmp(mcp_log_level_string(MCP_LOG_WARN), "WARN") == 0);
    CHECK(strcmp(mcp_log_level_string((mcp_log_level_t)99), "UNKNOWN") == 0);
    mcp_logger_t *lg = mcp_logger_create(NULL, mem_sink, NULL);
    CHECK(lg != NULL);
    mcp_logger_set_level(lg, MCP_LOG_WARN);
    mcp_logger_log(lg, MCP_LOG_DEBUG, "dropped");
    CHECK(g_len == 0);
    mcp_logger_log(lg, MCP_LOG_ERROR, "kept");
    CHECK(g_len == 4);
    mcp_logger_log(NULL, MCP_LOG_ERROR, "noop");
    mcp_logger_log(lg, MCP_LOG_ERROR, NULL);
    CHECK(mcp_logger_get_level(NULL) == MCP_LOG_ERROR);
    mcp_logger_destroy(lg);
    mcp_logger_destroy(NULL);
    mcp_log_sink_stderr(MCP_LOG_INFO, "smoke\n", NULL);
    mcp_logger_t *d = mcp_logger_default_stderr(NULL);
    CHECK(d != NULL);
    mcp_logger_destroy(d);
    return 0;
}
