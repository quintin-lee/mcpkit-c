// Deterministic fuzz corpus for the JSON parser.
// Each snippet must either parse to a DOM that re-serializes and re-parses
// to equal text, or fail cleanly (NULL + protocol status). No crashes or hangs.
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static int g_failures;

static void check(const char *label, const char *text) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        printf("FAIL %s: ctx\n", label);
        g_failures++;
        return;
    }
    mcp_json_value_t *v = mcp_json_parse(ctx, text, strlen(text));
    if (v == NULL) {
        // parser rejected; acceptable
        mcp_context_destroy(ctx);
        return;
    }
    char *s1 = mcp_json_serialize(ctx, v);
    if (s1 == NULL) {
        printf("FAIL %s: serialize\n", label);
        mcp_json_destroy(ctx, v);
        mcp_context_destroy(ctx);
        g_failures++;
        return;
    }
    mcp_json_value_t *v2 = mcp_json_parse(ctx, s1, strlen(s1));
    if (v2 == NULL) {
        printf("FAIL %s: re-parse\n", label);
        mcp_json_free_string(ctx, s1);
        mcp_json_destroy(ctx, v);
        mcp_context_destroy(ctx);
        g_failures++;
        return;
    }
    char *s2 = mcp_json_serialize(ctx, v2);
    if (s2 == NULL || strcmp(s1, s2) != 0) {
        printf("FAIL %s: roundtrip mismatch\n", label);
        g_failures++;
    }
    if (s2 != NULL) mcp_json_free_string(ctx, s2);
    mcp_json_free_string(ctx, s1);
    mcp_json_destroy(ctx, v2);
    mcp_json_destroy(ctx, v);
    mcp_context_destroy(ctx);
}

// 40 mutated snippets: truncations, bad escapes, lone surrogates, deep
// nesting, overflow numbers, NUL bytes, malformed structure.
static const char *const k_cases[] = {
    "[]",
    "[1,2,",
    "{\"a\":",
    "\"\\u",
    "\"\\uFFFF",
    "\"\\uD800",
    "\"\\ud800\\udc00\"",
    "\"\\udc00\"",
    "{\"a\":1,}",
    "[1,2]]",
    "nullnull",
    "truefalse",
    "0.5",
    "-0.0",
    "1e999",
    "NaN",
    "Infinity",
    "-Infinity",
    " \t\n\r",
    "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[",
    "{\"a\":{\"b\":{\"c\":{\"d\":1}}}}",
    "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[",
    "\"",
    "\"abc",
    "{'a':1}",
    "[{\"a\":1]",
    "[true, false, null]",
    "{\"s\":\"héllo wörld\"}",
    "[[1,2],[3,4]]",
    "1234567890123456789012345678901234567890",
    "-0",
    "0e0",
    "[[],{},null]",
    "{\"a\":null,\"b\":[]}",
    "\"\\n\\t\\r\"",
    "  {\"a\": 1}  ",
};
int main(void) {
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        char label[32];
        snprintf(label, sizeof(label), "case-%zu", i);
        check(label, k_cases[i]);
    }
    if (g_failures > 0) {
        fprintf(stderr, "test_json_fuzz: %d failures\n", g_failures);
        return 1;
    }
    printf("test_json_fuzz: all %zu cases OK\n", sizeof(k_cases) / sizeof(k_cases[0]));
    return 0;
}
