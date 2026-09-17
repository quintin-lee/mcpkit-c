// Fuzz harness: feed raw bytes as a JSON-RPC message line, parse + validate.
// Exits 0 on clean parse/rejection; 1 on internal error.
#include <stdio.h>

#include "mcpkit/mcpkit.h"

int main(void) {
    char buf[1u << 20];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }
    int rc = 0;
    mcp_message_t *msg = mcp_message_parse(ctx, buf, n);
    if (msg != NULL) {
        int code = 0;
        mcp_status_t st = mcp_message_validate(ctx, msg, &code);
        (void)st; // any status is a valid fuzz outcome
        mcp_message_destroy(ctx, msg);
    }
    mcp_context_destroy(ctx);
    return rc;
}
