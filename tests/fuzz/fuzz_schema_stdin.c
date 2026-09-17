// Fuzz harness: feed raw bytes as a JSON document to the schema validator.
// Builds a string-typed schema, validates the parsed input against it.
// Exits 0 on clean result; 1 on internal error.
#include <stdio.h>

#include "mcpkit/mcpkit.h"

int main(void) {
    char buf[1u << 20];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }
    mcp_json_value_t *schema = mcp_schema_string_new(ctx);
    if (schema == NULL) {
        mcp_context_destroy(ctx);
        return 0;
    }
    mcp_json_value_t *v = mcp_json_parse(ctx, buf, n);
    int rc = 0;
    if (v != NULL) {
        mcp_status_t st = mcp_schema_validate(ctx, schema, v);
        (void)st; // any status is a valid fuzz outcome
        mcp_json_destroy(ctx, v);
    }
    mcp_json_destroy(ctx, schema);
    mcp_context_destroy(ctx);
    return rc;
}
