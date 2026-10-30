/**
 * @file sha1.c
 * @brief Native RFC 3174 SHA-1 and RFC 6455 Sec-WebSocket-Accept key generator.
 */

#include "sha1.h"

#include <stdint.h>
#include <string.h>

#define SHA1_ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4 + 0] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = SHA1_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    for (int i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t temp = SHA1_ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = SHA1_ROL(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void mcp_sha1(const uint8_t *data, size_t len, uint8_t digest[20]) {
    uint32_t state[5] = {
        0x67452301,
        0xEFCDAB89,
        0x98BADCFE,
        0x10325476,
        0xC3D2E1F0
    };

    uint64_t total_bits = (uint64_t)len * 8;
    size_t offset = 0;

    while (len >= 64) {
        sha1_transform(state, data + offset);
        offset += 64;
        len -= 64;
    }

    uint8_t buffer[64] = {0};
    if (len > 0) {
        memcpy(buffer, data + offset, len);
    }
    buffer[len] = 0x80;

    if (len >= 56) {
        sha1_transform(state, buffer);
        memset(buffer, 0, 64);
    }

    for (int i = 0; i < 8; i++) {
        buffer[63 - i] = (uint8_t)((total_bits >> (i * 8)) & 0xFF);
    }
    sha1_transform(state, buffer);

    for (int i = 0; i < 5; i++) {
        digest[i * 4 + 0] = (uint8_t)((state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (uint8_t)((state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (uint8_t)((state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (uint8_t)(state[i] & 0xFF);
    }
}

static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode_20(const uint8_t in[20], char out[29]) {
    size_t o = 0;
    for (size_t i = 0; i < 18; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = kBase64Chars[(v >> 18) & 0x3F];
        out[o++] = kBase64Chars[(v >> 12) & 0x3F];
        out[o++] = kBase64Chars[(v >> 6) & 0x3F];
        out[o++] = kBase64Chars[v & 0x3F];
    }
    /* Remaining 2 bytes */
    uint32_t v = ((uint32_t)in[18] << 16) | ((uint32_t)in[19] << 8);
    out[o++] = kBase64Chars[(v >> 18) & 0x3F];
    out[o++] = kBase64Chars[(v >> 12) & 0x3F];
    out[o++] = kBase64Chars[(v >> 6) & 0x3F];
    out[o++] = '=';
    out[o] = '\0';
}

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

mcp_status_t mcp_ws_compute_accept_key(const char *client_key, char accept_buf[32]) {
    if (client_key == NULL || accept_buf == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    size_t klen = strlen(client_key);
    size_t glen = strlen(WS_GUID);
    if (klen > 256) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    char combined[512];
    memcpy(combined, client_key, klen);
    memcpy(combined + klen, WS_GUID, glen);

    uint8_t digest[20];
    mcp_sha1((const uint8_t *)combined, klen + glen, digest);

    char b64[29];
    base64_encode_20(digest, b64);
    memcpy(accept_buf, b64, 29);
    return MCP_OK;
}
