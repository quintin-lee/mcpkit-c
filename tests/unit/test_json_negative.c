#include <assert.h>
#include <string.h>

#include "mcpkit/json/json.h"

static void expect_null(const char *text) {
    mcp_json_value_t *v = mcp_json_parse(NULL, text, strlen(text));
    assert(v == NULL);
}

int main(void) {
    expect_null("");
    expect_null("   ");
    expect_null("{");
    expect_null("{\"a\":");
    expect_null("{\"a\":1,}");
    expect_null("[1,]");
    expect_null("{,}");
    expect_null("{} trailing");
    expect_null("[1 2]");
    expect_null("{'a':1}");
    expect_null("NaN");
    expect_null("Infinity");
    expect_null("[1e999]");
    expect_null("\"bad \\q escape\"");
    expect_null("\"lone \\ud800 surrogate\"");
    expect_null("\"\\ud83d\"");
    expect_null("\"raw \x01 control\"");
    expect_null("\"\\x41\"");
    expect_null("\"\xff\"");
    expect_null("\"abc\x80\"");
    expect_null("\"\\ud800\"");
    char deep_ok[512], deep_bad[520];
    size_t p = 0;
    for (int i = 0; i < 128; i++) { deep_ok[p++] = '['; }
    for (int i = 0; i < 128; i++) { deep_ok[p++] = ']'; }
    deep_ok[p] = '\0';
    mcp_json_value_t *v = mcp_json_parse(NULL, deep_ok, p);
    assert(v != NULL);
    mcp_json_destroy(NULL, v);
    p = 0;
    for (int i = 0; i < 129; i++) { deep_bad[p++] = '['; }
    for (int i = 0; i < 129; i++) { deep_bad[p++] = ']'; }
    deep_bad[p] = '\0';
    expect_null(deep_bad);
    return 0;
}
