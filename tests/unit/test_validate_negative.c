#include "test_check.h"
#include <stdlib.h>
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/validate.h"

static mcp_message_t *parse(mcp_context_t *ctx, const char *text) {
    return mcp_message_parse(ctx, text, strlen(text));
}

static void expect_code(mcp_context_t *ctx, const char *text, int want) {
    mcp_message_t *m = parse(ctx, text);
    CHECK(m != NULL);
    int code = 0;
    mcp_status_t st = mcp_message_validate(ctx, m, &code);
    CHECK(code == want);
    CHECK((want == 0 && st == MCP_OK) || (want != 0 && st != MCP_OK));
    // NULL out-param is allowed
    CHECK(mcp_message_validate(ctx, m, NULL) == st);
    mcp_message_destroy(ctx, m);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // 1. malformed JSON → parse NULL
    CHECK(parse(ctx, "{\"jsonrpc\":") == NULL);
    // 3. root array → parse NULL (not an object envelope)
    CHECK(parse(ctx, "[1,2]") == NULL);

    // 2. invalid JSON-RPC → -32600
    expect_code(ctx, "{\"id\":1,\"method\":\"ping\"}", -32600); // missing jsonrpc
    expect_code(ctx, "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}", -32600);
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1}", -32600); // request w/o method
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{},"
                     "\"error\":{\"code\":-32603,\"message\":\"x\"}}",
                -32600); // result+error both present
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":null}",
                -32600); // error present but not an object
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1}", -32600); // response w/o result/error
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}",
                -32600); // null id request
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":true,\"method\":\"ping\"}",
                -32600); // bool id
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"message\":\"x\"}}",
                -32600); // error w/o code
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603}}",
                -32600); // error w/o message

    // valid shapes → 0
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", 0);
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", 0);
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"result\":{}}", 0);
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":null,"
                     "\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}", 0);
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                     "\"params\":{\"name\":\"ls\"}}", 0);

    // 3. unknown method → -32601
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"frobnicate\"}", -32601);
    CHECK(mcp_method_known("tools/call"));
    CHECK(mcp_method_known("notifications/initialized"));
    CHECK(!mcp_method_known("frobnicate"));
    CHECK(!mcp_method_known(NULL));

    // 4. invalid params → -32602
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                     "\"params\":{}}", -32602); // missing name
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                     "\"params\":[1]}", -32602); // params not object
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/read\","
                     "\"params\":{}}", -32602); // missing uri
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"prompts/get\","
                     "\"params\":{\"name\":7}}", -32602); // name not string
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                     "\"params\":{}}", -32602);
    expect_code(ctx, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}",
                -32602); // missing params

    // 5. oversized → parse NULL
    size_t big_n = MCP_PROTOCOL_MAX_MESSAGE_BYTES + 1;
    char *big = malloc(big_n + 1);
    CHECK(big != NULL);
    memset(big, ' ', big_n);
    big[big_n] = '\0';
    CHECK(mcp_message_parse(ctx, big, big_n) == NULL);
    free(big);

    // 6. invalid UTF-8 → parse NULL
    CHECK(parse(ctx, "{\"a\":\"\xff\"}") == NULL);

    // 7. duplicate id via idset
    mcp_idset_t *ids = mcp_idset_create(ctx);
    CHECK(ids != NULL);
    CHECK(mcp_idset_add(ctx, ids, MCP_ID_STRING, "r1", 0) == MCP_OK);
    CHECK(mcp_idset_contains(ctx, ids, MCP_ID_STRING, "r1", 0));
    CHECK(mcp_idset_add(ctx, ids, MCP_ID_STRING, "r1", 0) == MCP_ERR_ALREADY_EXISTS);
    CHECK(mcp_idset_add(ctx, ids, MCP_ID_NUMBER, NULL, 1) == MCP_OK);
    CHECK(mcp_idset_add(ctx, ids, MCP_ID_NUMBER, NULL, 1) == MCP_ERR_ALREADY_EXISTS);
    // string "1" and number 1 are distinct ids
    CHECK(mcp_idset_add(ctx, ids, MCP_ID_STRING, "1", 0) == MCP_OK);
    CHECK(!mcp_idset_contains(ctx, ids, MCP_ID_NUMBER, NULL, 2));
    mcp_idset_remove(ctx, ids, MCP_ID_STRING, "r1", 0);
    CHECK(!mcp_idset_contains(ctx, ids, MCP_ID_STRING, "r1", 0));
    CHECK(mcp_idset_add(ctx, ids, MCP_ID_STRING, "r1", 0) == MCP_OK);
    CHECK(mcp_idset_add(ctx, ids, MCP_ID_NONE, NULL, 0) == MCP_ERR_INVALID_ARGUMENT);
    mcp_idset_destroy(ctx, ids);

    // response skips L2/L3
    mcp_message_t *resp = parse(ctx, "{\"jsonrpc\":\"2.0\",\"id\":9,\"result\":[1,2]}");
    CHECK(resp != NULL);
    int rc = -1;
    CHECK(mcp_validate_method(ctx, resp, &rc) == MCP_OK && rc == 0);
    CHECK(mcp_validate_params(ctx, resp, &rc) == MCP_OK && rc == 0);
    mcp_message_destroy(ctx, resp);

    mcp_context_destroy(ctx);
    return 0;
}
