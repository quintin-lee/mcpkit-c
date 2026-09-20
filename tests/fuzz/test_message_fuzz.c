// Deterministic corpus for message parsing + validation: edge cases that must not crash.
#include "test_check.h"
#include <string.h>

#include "mcpkit/mcpkit.h"

static void check(mcp_context_t *ctx, const char *input, size_t len) {
    mcp_message_t *msg = mcp_message_parse(ctx, input, len);
    if (msg != NULL) {
        int code = 0;
        mcp_message_validate(ctx, msg, &code);
        mcp_message_destroy(ctx, msg);
    }
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // Valid requests
    const char *valid[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"tools/list\",\"params\":{}}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hi\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"prompts/get\",\"params\":{\"name\":\"greet\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":100,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"0\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":-1,\"method\":\"unknown/method\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"ping\",\"params\":null}",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        check(ctx, valid[i], strlen(valid[i]));
    }

    // Invalid / malformed
    const char *invalid[] = {
        "not json at all",
        "{bad json",
        "[]",
        "\"string root\"",
        "42",
        "null",
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}",
        "{\"id\":1,\"method\":\"ping\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"extra\":true}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":42}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        check(ctx, invalid[i], strlen(invalid[i]));
    }

    mcp_context_destroy(ctx);
    return 0;
}
