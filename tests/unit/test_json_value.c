#include <assert.h>
#include <math.h>
#include <string.h>

#include "mcpkit/json/json.h"

int main(void) {
    mcp_json_value_t *n = mcp_json_null_new(NULL);
    assert(n != NULL && mcp_json_type(NULL, n) == MCP_JSON_NULL);
    mcp_json_value_t *b = mcp_json_bool_new(NULL, true);
    assert(b != NULL && mcp_json_type(NULL, b) == MCP_JSON_BOOL);
    mcp_json_value_t *num = mcp_json_number_new(NULL, 1.5);
    assert(num != NULL && mcp_json_type(NULL, num) == MCP_JSON_NUMBER);
    mcp_json_value_t *s = mcp_json_string_new(NULL, "hi");
    assert(s != NULL && mcp_json_type(NULL, s) == MCP_JSON_STRING);
    mcp_json_value_t *a = mcp_json_array_new(NULL);
    assert(a != NULL && mcp_json_type(NULL, a) == MCP_JSON_ARRAY);
    mcp_json_value_t *o = mcp_json_object_new(NULL);
    assert(o != NULL && mcp_json_type(NULL, o) == MCP_JSON_OBJECT);
    assert(mcp_json_type(NULL, NULL) == MCP_JSON_NULL);

    bool bv = false;
    assert(mcp_json_bool_value(NULL, b, &bv) == MCP_OK && bv);
    assert(mcp_json_bool_value(NULL, num, &bv) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_json_bool_value(NULL, b, NULL) == MCP_ERR_INVALID_ARGUMENT);
    double dv = 0;
    assert(mcp_json_number_value(NULL, num, &dv) == MCP_OK && dv == 1.5);
    assert(mcp_json_number_value(NULL, b, &dv) == MCP_ERR_INVALID_ARGUMENT);
    const char *sv = NULL;
    assert(mcp_json_string_value(NULL, s, &sv) == MCP_OK && strcmp(sv, "hi") == 0);
    assert(mcp_json_string_value(NULL, num, &sv) == MCP_ERR_INVALID_ARGUMENT);

    assert(mcp_json_number_new(NULL, INFINITY) == NULL);
    assert(mcp_json_number_new(NULL, NAN) == NULL);
    assert(mcp_json_string_new(NULL, "\xff") == NULL);
    assert(mcp_json_string_new(NULL, NULL) == NULL);
    mcp_json_value_t *sub = mcp_json_string_new_n(NULL, "ab", 1);
    assert(sub != NULL);
    const char *subv = NULL;
    assert(mcp_json_string_value(NULL, sub, &subv) == MCP_OK && strcmp(subv, "a") == 0);
    mcp_json_destroy(NULL, sub);

    mcp_json_value_t *child = mcp_json_bool_new(NULL, true);
    assert(mcp_json_object_set(NULL, o, "k", child) == MCP_OK);
    assert(mcp_json_object_size(NULL, o) == 1);
    assert(mcp_json_object_has(NULL, o, "k"));
    assert(mcp_json_object_get(NULL, o, "k") == child);
    mcp_json_value_t *child2 = mcp_json_number_new(NULL, 2.0);
    assert(mcp_json_object_set(NULL, o, "k", child2) == MCP_OK);
    assert(mcp_json_object_size(NULL, o) == 1);
    const mcp_json_value_t *got = mcp_json_object_get(NULL, o, "k");
    double rv = 0;
    assert(mcp_json_number_value(NULL, got, &rv) == MCP_OK && rv == 2.0);

    mcp_json_value_t *orphan = mcp_json_bool_new(NULL, false);
    assert(mcp_json_object_set(NULL, num, "k", orphan) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_destroy(NULL, orphan);
    assert(mcp_json_object_get(NULL, num, "k") == NULL);
    assert(!mcp_json_object_has(NULL, num, "k"));
    assert(mcp_json_object_size(NULL, num) == 0);

    mcp_json_value_t *e = mcp_json_number_new(NULL, 7.0);
    assert(mcp_json_array_append(NULL, a, e) == MCP_OK);
    assert(mcp_json_array_size(NULL, a) == 1);
    assert(mcp_json_array_get(NULL, a, 0) == e);
    assert(mcp_json_array_get(NULL, a, 1) == NULL);
    mcp_json_value_t *orphan2 = mcp_json_null_new(NULL);
    assert(mcp_json_array_append(NULL, num, orphan2) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_destroy(NULL, orphan2);

    mcp_json_destroy(NULL, n);
    mcp_json_destroy(NULL, b);
    mcp_json_destroy(NULL, num);
    mcp_json_destroy(NULL, s);
    mcp_json_destroy(NULL, a);
    mcp_json_destroy(NULL, o);
    mcp_json_destroy(NULL, NULL);
    return 0;
}
