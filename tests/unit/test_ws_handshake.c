#include "test_check.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "src/core/sha1.h"

static void test_sha1_rfc3174_vectors(void) {
    /* Test vector 1: "abc" -> a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d */
    uint8_t digest[20];
    mcp_sha1((const uint8_t *)"abc", 3, digest);

    const uint8_t expected_abc[20] = {
        0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
        0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d
    };
    CHECK(memcmp(digest, expected_abc, 20) == 0);

    /* Test vector 2: "" (empty string) -> da39a3ee 5e6b4b0d 3255bfef 95601890 afd80709 */
    mcp_sha1((const uint8_t *)"", 0, digest);
    const uint8_t expected_empty[20] = {
        0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
        0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09
    };
    CHECK(memcmp(digest, expected_empty, 20) == 0);
}

static void test_ws_accept_key_rfc6455(void) {
    /* RFC 6455 Section 1.3:
     * Sec-WebSocket-Key: "dGhlIHNhbXBsZSBub25jZQ=="
     * Sec-WebSocket-Accept: "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
     */
    char accept_buf[32];
    CHECK(mcp_ws_compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept_buf) == MCP_OK);
    CHECK(strcmp(accept_buf, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

    /* Negative checks */
    CHECK(mcp_ws_compute_accept_key(NULL, accept_buf) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_ws_compute_accept_key("dGhlIHNhbXBsZSBub25jZQ==", NULL) == MCP_ERR_INVALID_ARGUMENT);
}

int main(void) {
    test_sha1_rfc3174_vectors();
    test_ws_accept_key_rfc6455();
    printf("test_ws_handshake: OK\n");
    return 0;
}
