#include "test_check.h"
#include <string.h>

#include "mcpkit/transport/http.h"

int main(void) {
    const char *raw =
        "POST /mcp HTTP/1.1\r\nContent-Length: 2\r\nContent-Type: application/json\r\n\r\n{}";
    mcp_http_request_t *req = mcp_http_parse_request(NULL, raw, strlen(raw));
    CHECK(req != NULL);
    CHECK(mcp_http_request_method(NULL, req) == MCP_HTTP_POST);
    CHECK(strcmp(mcp_http_request_target(NULL, req), "/mcp") == 0);
    CHECK(strcmp(mcp_http_header(NULL, req, "content-type"), "application/json") == 0);
    size_t blen = 0;
    const char *body = mcp_http_request_body(NULL, req, &blen);
    CHECK(blen == 2 && strncmp(body, "{}", blen) == 0);
    CHECK(mcp_http_header(NULL, req, "x-missing") == NULL);
    mcp_http_request_destroy(NULL, req);

    CHECK(mcp_http_parse_request(NULL, "GARBAGE", 7) == NULL);
    CHECK(mcp_http_parse_request(NULL, "POST /mcp HTTP/1.1\r\n", 19) == NULL);
    CHECK(mcp_http_parse_request(NULL, NULL, 0) == NULL);

    /* \r\n\r\n at the very end of the buffer (i+4==len) must be detected. */
    {
        const char *hdrs = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        size_t hlen = strlen(hdrs);
        mcp_http_request_t *r2 = mcp_http_parse_request(NULL, hdrs, hlen);
        CHECK(r2 != NULL);
        CHECK(mcp_http_request_method(NULL, r2) == MCP_HTTP_GET);
        size_t bl = 0;
        CHECK(mcp_http_request_body(NULL, r2, &bl) == NULL || bl == 0);
        mcp_http_request_destroy(NULL, r2);
    }
    CHECK(mcp_http_parse_request(NULL,
        "POST /mcp HTTP/1.1\r\nContent-Length: 10\r\n\r\n{}", 44) == NULL);

    mcp_http_response_t *r = mcp_http_response_new(NULL, 200, "OK");
    CHECK(r != NULL);
    CHECK(mcp_http_response_set_header(NULL, r, "Content-Type", "application/json") == MCP_OK);
    CHECK(mcp_http_response_set_body(NULL, r, "{}", 2) == MCP_OK);
    const char *s = mcp_http_response_serialize(NULL, r);
    CHECK(s != NULL && strstr(s, "HTTP/1.1 200 OK\r\n") == s);
    CHECK(strstr(s, "Content-Length: 2\r\n\r\n{}") != NULL);
    /* s is BORROWED (valid until destroy) — no free */
    mcp_http_response_destroy(NULL, r);

    CHECK(mcp_http_response_set_header(NULL, NULL, "X", "y") == MCP_ERR_INVALID_ARGUMENT);
    mcp_http_request_destroy(NULL, NULL);
    mcp_http_response_destroy(NULL, NULL);
    /* P1-1: Connection header detection */
    {
        const char *close_req =
            "POST /mcp HTTP/1.1\r\nConnection: close\r\nContent-Length: 2\r\n\r\n{}";
        mcp_http_request_t *rc = mcp_http_parse_request(NULL, close_req, strlen(close_req));
        CHECK(rc != NULL);
        CHECK(mcp_http_request_wants_close(NULL, rc) == true);
        mcp_http_request_destroy(NULL, rc);
    }
    {
        const char *ka_req =
            "POST /mcp HTTP/1.1\r\nConnection: keep-alive\r\nContent-Length: 2\r\n\r\n{}";
        mcp_http_request_t *rk = mcp_http_parse_request(NULL, ka_req, strlen(ka_req));
        CHECK(rk != NULL);
        CHECK(mcp_http_request_wants_close(NULL, rk) == false);
        mcp_http_request_destroy(NULL, rk);
    }
    {
        /* No Connection header → default keep-alive (wants_close = false). */
        const char *no_conn =
            "POST /mcp HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}";
        mcp_http_request_t *rn = mcp_http_parse_request(NULL, no_conn, strlen(no_conn));
        CHECK(rn != NULL);
        CHECK(mcp_http_request_wants_close(NULL, rn) == false);
        mcp_http_request_destroy(NULL, rn);
    }
    return 0;
}
