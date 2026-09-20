// Deterministic corpus for schema validation: edge cases that must not crash.
// Each case is parsed and validated against a string-typed schema.
#include "test_check.h"
#include <string.h>

#include "mcpkit/mcpkit.h"

static void check(mcp_context_t *ctx, const char *input, size_t len) {
    mcp_json_value_t *schema = mcp_schema_string_new(ctx);
    CHECK(schema != NULL);
    mcp_json_value_t *v = mcp_json_parse(ctx, input, len);
    mcp_status_t st = mcp_schema_validate(ctx, schema, v);
    (void)st; // any status is a valid outcome; must not crash
    mcp_json_destroy(ctx, v);
    mcp_json_destroy(ctx, schema);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    const char *cases[] = {
        "\"hello\"",
        "\"\"",
        "\"emoji \\ud83d\\ude00\"",
        "\"with \\u0041 escape\"",
        "\"long string of text that goes on and on\"",
        "null",
        "42",
        "-42.5",
        "true",
        "false",
        "[1,2,3]",
        "{}",
        "{\"a\":1}",
        "\"escaped \\n\\t\\r\\b\\f\"",
        "\"unicode \\u00e9\"",
        "\"lone surrogate \\ud800\"",
        "1e300",
        "-1e300",
        "0.0001",
        "\"\\ud800\\udc00\"",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        check(ctx, cases[i], strlen(cases[i]));
    }

    // Truncated strings
    const char *trunc[] = {
        "\"unterminated",
        "\"bad \\u00",
        "[1,2",
        "{\"a\":",
    };
    for (size_t i = 0; i < sizeof(trunc) / sizeof(trunc[0]); i++) {
        // These may parse to NULL; just make sure no crash
        mcp_json_value_t *schema = mcp_schema_string_new(ctx);
        mcp_json_value_t *v = mcp_json_parse(ctx, trunc[i], strlen(trunc[i]));
        if (v != NULL) {
            mcp_schema_validate(ctx, schema, v);
            mcp_json_destroy(ctx, v);
        }
        mcp_json_destroy(ctx, schema);
    }

    mcp_context_destroy(ctx);
    return 0;
}
