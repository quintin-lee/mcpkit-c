#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/core/arena.h"
#include "mcpkit/core/context.h"
#include "mcpkit/protocol/json_stream.h"
#include "mcpkit/protocol/message.h"

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__);                     \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void test_string_escaping(void) {
    mcp_json_stream_t s;
    mcp_json_stream_init_allocator(&s, NULL);

    mcp_json_stream_write_string(&s, "hello\n\"world\"\t\\slash/test\r\b\f");
    ASSERT_TRUE(s.buf != NULL);
    const char *expected = "\"hello\\n\\\"world\\\"\\t\\\\slash/test\\r\\b\\f\"";
    ASSERT_TRUE(strcmp(s.buf, expected) == 0);

    mcp_json_stream_free(&s);
}

static void test_primitives(void) {
    mcp_json_stream_t s;
    mcp_json_stream_init_allocator(&s, NULL);

    mcp_json_stream_write_int64(&s, -42);
    mcp_json_stream_write_raw(&s, ",", 1);
    mcp_json_stream_write_uint64(&s, 18446744073709551615ULL);
    mcp_json_stream_write_raw(&s, ",", 1);
    mcp_json_stream_write_bool(&s, true);
    mcp_json_stream_write_raw(&s, ",", 1);
    mcp_json_stream_write_bool(&s, false);
    mcp_json_stream_write_raw(&s, ",", 1);
    mcp_json_stream_write_null(&s);

    const char *expected = "-42,18446744073709551615,true,false,null";
    ASSERT_TRUE(strcmp(s.buf, expected) == 0);

    mcp_json_stream_free(&s);
}

static void test_response_envelope(void) {
    mcp_arena_t *arena = mcp_arena_create(NULL, 1024);
    ASSERT_TRUE(arena != NULL);

    mcp_context_t *ctx = mcp_context_create(NULL);
    ASSERT_TRUE(ctx != NULL);

    /* 1. String ID */
    {
        mcp_json_stream_t s;
        mcp_json_stream_init_arena(&s, arena);
        mcp_json_rpc_id_t id = mcp_json_rpc_id_from_string("req-123");
        mcp_status_t status = mcp_serialize_response_stream(&s, &id, "{\"status\":\"ok\"}");
        ASSERT_TRUE(status == MCP_OK);
        const char *expected = "{\"jsonrpc\":\"2.0\",\"id\":\"req-123\",\"result\":{\"status\":\"ok\"}}";
        ASSERT_TRUE(strcmp(s.buf, expected) == 0);

        /* Parse back with message parser to ensure strict validity */
        mcp_message_t *msg = mcp_message_parse(ctx, s.buf, s.len);
        ASSERT_TRUE(msg != NULL);
        ASSERT_TRUE(strcmp(mcp_message_id_string(ctx, msg), "req-123") == 0);
        mcp_message_destroy(ctx, msg);
    }

    /* 2. Numeric ID */
    {
        mcp_json_stream_t s;
        mcp_json_stream_init_arena(&s, arena);
        mcp_json_rpc_id_t id = mcp_json_rpc_id_from_int(42);
        mcp_status_t status = mcp_serialize_response_stream(&s, &id, "[1,2,3]");
        ASSERT_TRUE(status == MCP_OK);
        const char *expected = "{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":[1,2,3]}";
        ASSERT_TRUE(strcmp(s.buf, expected) == 0);

        mcp_message_t *msg = mcp_message_parse(ctx, s.buf, s.len);
        ASSERT_TRUE(msg != NULL);
        double num_id = 0;
        ASSERT_TRUE(mcp_message_id_number(ctx, msg, &num_id) == MCP_OK && num_id == 42);
        mcp_message_destroy(ctx, msg);
    }

    /* 3. Null ID */
    {
        mcp_json_stream_t s;
        mcp_json_stream_init_arena(&s, arena);
        mcp_json_rpc_id_t id = mcp_json_rpc_id_null();
        mcp_status_t status = mcp_serialize_response_stream(&s, &id, "true");
        ASSERT_TRUE(status == MCP_OK);
        const char *expected = "{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":true}";
        ASSERT_TRUE(strcmp(s.buf, expected) == 0);
    }

    mcp_context_destroy(ctx);
    mcp_arena_destroy(arena);
}

static void test_error_envelope(void) {
    mcp_json_stream_t s;
    mcp_json_stream_init_allocator(&s, NULL);

    mcp_json_rpc_id_t id = mcp_json_rpc_id_from_int(101);
    mcp_status_t status = mcp_serialize_error_stream(&s, &id, -32601, "Method \"foo/bar\" not found\n");
    ASSERT_TRUE(status == MCP_OK);

    const char *expected =
        "{\"jsonrpc\":\"2.0\",\"id\":101,\"error\":{\"code\":-32601,\"message\":\"Method \\\"foo/bar\\\" not found\\n\"}}";
    ASSERT_TRUE(strcmp(s.buf, expected) == 0);

    /* Verify with mcp_message_parse */
    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_message_t *msg = mcp_message_parse(ctx, s.buf, s.len);
    ASSERT_TRUE(msg != NULL);
    mcp_message_destroy(ctx, msg);
    mcp_context_destroy(ctx);

    mcp_json_stream_free(&s);
}

int main(void) {
    test_string_escaping();
    test_primitives();
    test_response_envelope();
    test_error_envelope();
    printf("All test_json_stream tests passed!\n");
    return 0;
}
