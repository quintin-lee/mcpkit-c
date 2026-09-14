#include <assert.h>
#include <string.h>

#include "mcpkit/json/json.h"

int main(void) {
    mcp_json_value_t *doc = mcp_json_object_new(NULL);
    assert(doc != NULL);
    mcp_json_value_t *name = mcp_json_parse(NULL, "\"caf\\u00e9 \\ud83d\\ude00\"",
                                            strlen("\"caf\\u00e9 \\ud83d\\ude00\""));
    assert(name != NULL);
    assert(mcp_json_object_set(NULL, doc, "name", name) == MCP_OK);
    mcp_json_value_t *num = mcp_json_number_new(NULL, -1.5e10);
    assert(num != NULL);
    assert(mcp_json_object_set(NULL, doc, "n", num) == MCP_OK);
    assert(mcp_json_object_set(NULL, doc, "t", mcp_json_bool_new(NULL, true)) == MCP_OK);
    assert(mcp_json_object_set(NULL, doc, "z", mcp_json_null_new(NULL)) == MCP_OK);
    mcp_json_value_t *arr = mcp_json_array_new(NULL);
    assert(arr != NULL);
    assert(mcp_json_array_append(NULL, arr, mcp_json_number_new(NULL, 1.0)) == MCP_OK);
    mcp_json_value_t *ab = mcp_json_parse(NULL, "\"a\\\\b\"", 6);
    assert(ab != NULL);
    assert(mcp_json_array_append(NULL, arr, ab) == MCP_OK);
    assert(mcp_json_array_append(NULL, arr, mcp_json_object_new(NULL)) == MCP_OK);
    assert(mcp_json_object_set(NULL, doc, "arr", arr) == MCP_OK);

    char *s1 = mcp_json_serialize(NULL, doc);
    assert(s1 != NULL);
    assert(strstr(s1, "\xf0\x9f\x98\x80") != NULL);
    mcp_json_value_t *back = mcp_json_parse(NULL, s1, strlen(s1));
    assert(back != NULL);
    char *s2 = mcp_json_serialize(NULL, back);
    assert(s2 != NULL);
    assert(strcmp(s1, s2) == 0);
    mcp_json_free_string(NULL, s1);
    mcp_json_free_string(NULL, s2);
    mcp_json_free_string(NULL, NULL);
    mcp_json_destroy(NULL, doc);
    mcp_json_destroy(NULL, back);
    return 0;
}
