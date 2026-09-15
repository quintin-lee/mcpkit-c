#include <assert.h>
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/schema.h"

static mcp_json_value_t *build_tool_schema(mcp_context_t *ctx) {
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    assert(schema != NULL);
    assert(mcp_schema_add_property(ctx, schema, "name", mcp_schema_string_new(ctx)) == MCP_OK);
    mcp_json_value_t *prio = mcp_schema_integer_new(ctx);
    assert(prio != NULL);
    assert(mcp_schema_set_minimum(ctx, prio, 0) == MCP_OK);
    assert(mcp_schema_set_maximum(ctx, prio, 10) == MCP_OK);
    assert(mcp_schema_add_property(ctx, schema, "priority", prio) == MCP_OK);
    mcp_json_value_t *tags = mcp_schema_array_new(ctx, mcp_schema_string_new(ctx));
    assert(tags != NULL);
    assert(mcp_schema_add_property(ctx, schema, "tags", tags) == MCP_OK);
    mcp_json_value_t *mode = mcp_schema_string_new(ctx);
    assert(mode != NULL);
    mcp_json_value_t *evals = mcp_json_array_new(ctx);
    assert(evals != NULL);
    assert(mcp_json_array_append(ctx, evals, mcp_json_string_new(ctx, "auto")) == MCP_OK);
    assert(mcp_json_array_append(ctx, evals, mcp_json_string_new(ctx, "manual")) == MCP_OK);
    assert(mcp_schema_add_enum(ctx, mode, evals) == MCP_OK);
    assert(mcp_schema_add_property(ctx, schema, "mode", mode) == MCP_OK);
    assert(mcp_schema_add_required(ctx, schema, "name") == MCP_OK);
    mcp_json_value_t *addr = mcp_schema_object_new(ctx);
    assert(addr != NULL);
    assert(mcp_schema_add_property(ctx, addr, "city", mcp_schema_string_new(ctx)) == MCP_OK);
    assert(mcp_schema_add_required(ctx, addr, "city") == MCP_OK);
    assert(mcp_schema_add_property(ctx, schema, "address", addr) == MCP_OK);
    return schema;
}

static mcp_json_value_t *parse_inst(mcp_context_t *ctx, const char *text) {
    mcp_json_value_t *v = mcp_json_parse(ctx, text, strlen(text));
    assert(v != NULL);
    return v;
}

static void expect_ok(mcp_context_t *ctx, const mcp_json_value_t *schema, const char *text) {
    mcp_json_value_t *inst = parse_inst(ctx, text);
    assert(mcp_schema_validate(ctx, schema, inst) == MCP_OK);
    mcp_json_destroy(ctx, inst);
}

static void expect_bad(mcp_context_t *ctx, const mcp_json_value_t *schema, const char *text) {
    mcp_json_value_t *inst = parse_inst(ctx, text);
    char detail[256];
    assert(mcp_schema_validate_verbose(ctx, schema, inst, detail, sizeof(detail)) ==
           MCP_ERR_INVALID_ARGUMENT);
    assert(detail[0] != '\0');
    assert(detail[strlen(detail)] == '\0');
    mcp_json_destroy(ctx, inst);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    // key_at enumeration used by the validator
    mcp_json_value_t *o = mcp_json_object_new(ctx);
    assert(o != NULL);
    assert(mcp_json_object_set(ctx, o, "a", mcp_json_number_new(ctx, 1)) == MCP_OK);
    assert(mcp_json_object_set(ctx, o, "b", mcp_json_number_new(ctx, 2)) == MCP_OK);
    assert(strcmp(mcp_json_object_key_at(ctx, o, 0), "a") == 0);
    assert(strcmp(mcp_json_object_key_at(ctx, o, 1), "b") == 0);
    assert(mcp_json_object_key_at(ctx, o, 2) == NULL);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    assert(arr != NULL);
    assert(mcp_json_object_key_at(ctx, arr, 0) == NULL);
    mcp_json_destroy(ctx, o);
    mcp_json_destroy(ctx, arr);

    mcp_json_value_t *schema = build_tool_schema(ctx);

    expect_ok(ctx, schema,
              "{\"name\":\"ls\",\"priority\":1,\"tags\":[\"a\"],\"mode\":\"auto\","
              "\"address\":{\"city\":\"x\"}}");
    expect_ok(ctx, schema, "{\"name\":\"ls\"}");
    expect_bad(ctx, schema, "{}");
    expect_bad(ctx, schema, "{\"name\":5}");
    expect_bad(ctx, schema, "{\"name\":\"ls\",\"priority\":1.5}");
    expect_bad(ctx, schema, "{\"name\":\"ls\",\"priority\":-1}");
    expect_bad(ctx, schema, "{\"name\":\"ls\",\"priority\":11}");
    expect_bad(ctx, schema, "{\"name\":\"ls\",\"tags\":[\"a\",5]}");
    expect_bad(ctx, schema, "{\"name\":\"ls\",\"mode\":\"weird\"}");
    expect_bad(ctx, schema, "{\"name\":\"ls\",\"address\":{}}");
    /* 2^53+1 is a finite double outside int64 range; the buggy is_integer_value
       returned isfinite(d) for such values, incorrectly accepting them. */
    expect_bad(ctx, schema, "{\"name\":\"ls\",\"priority\":9007199254740993.0}");

    mcp_json_destroy(ctx, schema);

    /* Pure integer type check, isolated from min/maximum bounds:
     * a value like 1.5 must fail the "integer" type itself, not just a max bound. */
    mcp_json_value_t *int_schema = mcp_schema_object_new(ctx);
    assert(int_schema != NULL);
    mcp_json_value_t *n = mcp_schema_integer_new(ctx);
    assert(n != NULL);
    assert(mcp_schema_add_property(ctx, int_schema, "n", n) == MCP_OK);
    expect_ok(ctx, int_schema, "{\"n\":3}");
    expect_ok(ctx, int_schema, "{\"n\":-17}");
    expect_bad(ctx, int_schema, "{\"n\":3.5}");
    expect_bad(ctx, int_schema, "{\"n\":0.5}");
    expect_bad(ctx, int_schema, "{\"n\":1.5}");
    /* 1e300 is a finite double far outside int64 range; the buggy
       is_integer_value returned isfinite(d) for values above 2^53,
       incorrectly accepting them as integers. */
    expect_bad(ctx, int_schema, "{\"n\":1e300}");
    expect_bad(ctx, int_schema, "{\"n\":-1e300}");
    mcp_json_destroy(ctx, int_schema);

    mcp_context_destroy(ctx);
    return 0;
}
