// Deterministic corpus for the HTTP parser: edge cases that must not crash.
// Each case is fed through mcp_http_parse_request; a non-NULL result is destroyed,
// a NULL result is an accepted rejection.  No case may cause a crash or hang.
#include "test_check.h"
#include <string.h>

#include "mcpkit/mcpkit.h"

static void check(mcp_context_t *ctx, const char *input, size_t len, int expect_null) {
    mcp_http_request_t *req = mcp_http_parse_request(ctx, input, len);
    if (expect_null) {
        CHECK(req == NULL);
    } else {
        CHECK(req != NULL);
        mcp_http_request_destroy(ctx, req);
    }
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // Minimal valid GET request
    const char *cases_ok[] = {
        "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}",
        "DELETE / HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /a%20b%20c HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET / HTTP/1.1\r\nX-Large-Header: "
        "aaaaaaabbbbbbbaaaaaaabbbbbbaaaaaaabbbbb"
        "ccccccaddddddddeeeeeeefffffffgggggggg\r\n\r\n",
    };
    for (size_t i = 0; i < sizeof(cases_ok) / sizeof(cases_ok[0]); i++) {
        check(ctx, cases_ok[i], strlen(cases_ok[i]), 0);
    }

    // Invalid: no CRLFCRLF terminator, target with spaces (should be NULL)
    const char *bad[] = {
        "GET / HTTP/1.1\r\nHost: x",
        "",
        "garbage",
        "GET /a b c HTTP/1.1\r\nHost: x\r\n\r\n", // space in target
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        check(ctx, bad[i], strlen(bad[i]), 1);
    }

    // POST without Content-Length is valid, body=NULL
    check(ctx, "POST / HTTP/1.1\r\n\r\n", strlen("POST / HTTP/1.1\r\n\r\n"), 0);

    // NUL byte embedded — strlen stops at NUL, so the CRLFCRLF terminator is absent
    const char *nul = "GET / HTTP/1.1\r\nHost: \x00\r\n\r\n";
    check(ctx, nul, strlen(nul), 1); // strlen returns 20, no CRLFCRLF in that range → NULL

    // Very long single line (> 8KB) — exceeds MAX_LINE, expect NULL
    char longline[9000];
    memset(longline, 'A', 8999);
    longline[8999] = '\0';
    check(ctx, longline, 8999, 1);

    mcp_context_destroy(ctx);
    return 0;
}
