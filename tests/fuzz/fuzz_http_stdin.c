// Fuzz harness: feed raw bytes as an HTTP request, attempt to parse.
// Exits 0 on clean parse or clean rejection; 1 on internal error.
#include <stdio.h>

#include "mcpkit/mcpkit.h"

int main(void) {
    char buf[1u << 20];
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }
    mcp_http_request_t *req = mcp_http_parse_request(ctx, buf, n);
    int rc = 0;
    if (req != NULL) {
        mcp_http_request_destroy(ctx, req);
    }
    mcp_context_destroy(ctx);
    return rc;
}
