#include "test_check.h"
#include <string.h>

#include "mcpkit/json/json.h"
#include "mcpkit/core/context.h"

static const mcp_json_backend_ops_t *g_real;
static int g_parse_calls, g_serialize_calls, g_new_calls;

static mcp_json_value_t *wrap_parse(mcp_context_t *c, const char *t, size_t n) {
    g_parse_calls++;
    return g_real->parse(c, t, n);
}
static char *wrap_serialize(mcp_context_t *c, const mcp_json_value_t *v) {
    g_serialize_calls++;
    return g_real->serialize(c, v);
}
static mcp_json_value_t *wrap_new_object(mcp_context_t *c) {
    g_new_calls++;
    return g_real->new_object(c);
}

int main(void) {
    g_real = mcp_json_builtin_backend();
    static mcp_json_backend_ops_t wrap;
    memcpy(&wrap, g_real, sizeof(wrap));
    wrap.name = "counting-wrapper";
    wrap.parse = wrap_parse;
    wrap.serialize = wrap_serialize;
    wrap.new_object = wrap_new_object;
    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_json_set_backend(ctx, &wrap);
    mcp_json_value_t *o = mcp_json_object_new(ctx);
    CHECK(o != NULL && g_new_calls == 1);
    mcp_json_value_t *v = mcp_json_parse(ctx, "{\"a\":1}", 7);
    CHECK(v != NULL && g_parse_calls == 1);
    char *s = mcp_json_serialize(ctx, v);
    CHECK(s != NULL && g_serialize_calls == 1);
    CHECK(strcmp(s, "{\"a\":1}") == 0);
    mcp_json_free_string(ctx, s);
    mcp_json_destroy(ctx, v);
    mcp_json_destroy(ctx, o);
    mcp_json_set_backend(ctx, NULL); // restore builtin
    mcp_json_value_t *v2 = mcp_json_parse(ctx, "[true]", 6);
    CHECK(v2 != NULL && g_parse_calls == 1); // wrapper bypassed
    mcp_json_destroy(ctx, v2);
    mcp_json_set_backend(NULL, &wrap); // NULL-safe no-op
    mcp_context_destroy(ctx);
    return 0;
}
