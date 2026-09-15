// Minimal stdin-driven fuzz harness for future libFuzzer integration.
// Reads all stdin, parses, exits 0 on clean parse or clean rejection.
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

int main(void) {
    char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }
    mcp_json_value_t *v = mcp_json_parse(ctx, buf, n);
    int rc = 0;
    if (v != NULL) {
        char *s = mcp_json_serialize(ctx, v);
        if (s == NULL) {
            rc = 1;
        } else {
            mcp_json_free_string(ctx, s);
        }
        mcp_json_destroy(ctx, v);
    }
    mcp_context_destroy(ctx);
    return rc;
}
