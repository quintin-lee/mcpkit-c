/**
 * @file mrtr_state.c
 *
 * Multi Round-Trip Requests (MRTR) — secure requestState token engine.
 *
 * Implements tamper-proof opaque state packing and unpacking with HMAC-SHA256
 * and URL-safe Base64 encoding. Pure C99 with zero external dependencies.
 */
#define _POSIX_C_SOURCE 200809L

#include "mcpkit/protocol/mrtr.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/value.h"

/* -------------------------------------------------------------------------
 * Memory allocation helper
 * ---------------------------------------------------------------------- */

static const mcp_allocator_t *get_allocator(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

void mcp_mrtr_state_free(mcp_context_t *ctx, char *state) {
    if (state == NULL) {
        return;
    }
    const mcp_allocator_t *a = get_allocator(ctx);
    a->free_fn(state, a->userdata);
}

/* -------------------------------------------------------------------------
 * Constant-time comparison (prevents timing side-channel attacks)
 * ---------------------------------------------------------------------- */

static int mcp_crypto_subtle_equal(const void *a, const void *b, size_t len) {
    const uint8_t *p1 = (const uint8_t *)a;
    const uint8_t *p2 = (const uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(p1[i] ^ p2[i]);
    }
    return diff == 0 ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * SHA-256 implementation (FIPS 180-4 / RFC 6234)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t count; /* total bytes processed */
    uint8_t buffer[64];
} mcp_sha256_t;

static inline uint32_t rotr32(uint32_t x, unsigned int n) {
    return (x >> n) | (x << (32 - n));
}

#define SHA256_CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x)       (rotr32((x), 2) ^ rotr32((x), 13) ^ rotr32((x), 22))
#define SHA256_EP1(x)       (rotr32((x), 6) ^ rotr32((x), 11) ^ rotr32((x), 25))
#define SHA256_SIG0(x)      (rotr32((x), 7) ^ rotr32((x), 18) ^ ((x) >> 3))
#define SHA256_SIG1(x)      (rotr32((x), 17) ^ rotr32((x), 19) ^ ((x) >> 10))

static const uint32_t K256[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_init(mcp_sha256_t *ctx) {
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->count = 0;
}

static void sha256_transform(mcp_sha256_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) |
               ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) |
               ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = SHA256_SIG1(m[i - 2]) + m[i - 7] + SHA256_SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    for (int i = 0; i < 64; ++i) {
        t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + K256[i] + m[i];
        t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_update(mcp_sha256_t *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t buf_idx = (size_t)(ctx->count % 64);
    ctx->count += len;
    while (len > 0) {
        size_t take = 64 - buf_idx;
        if (take > len) take = len;
        memcpy(&ctx->buffer[buf_idx], p, take);
        buf_idx += take;
        p += take;
        len -= take;
        if (buf_idx == 64) {
            sha256_transform(ctx, ctx->buffer);
            buf_idx = 0;
        }
    }
}

static void sha256_final(mcp_sha256_t *ctx, uint8_t digest[32]) {
    size_t i = (size_t)(ctx->count % 64);
    ctx->buffer[i++] = 0x80;
    if (i > 56) {
        memset(&ctx->buffer[i], 0, 64 - i);
        sha256_transform(ctx, ctx->buffer);
        memset(ctx->buffer, 0, 56);
    } else {
        memset(&ctx->buffer[i], 0, 56 - i);
    }
    uint64_t total_bits = ctx->count * 8ULL;
    for (int b = 0; b < 8; ++b) {
        ctx->buffer[56 + b] = (uint8_t)(total_bits >> ((7 - b) * 8));
    }
    sha256_transform(ctx, ctx->buffer);
    for (int b = 0; b < 8; ++b) {
        digest[b * 4]     = (uint8_t)(ctx->state[b] >> 24);
        digest[b * 4 + 1] = (uint8_t)(ctx->state[b] >> 16);
        digest[b * 4 + 2] = (uint8_t)(ctx->state[b] >> 8);
        digest[b * 4 + 3] = (uint8_t)(ctx->state[b]);
    }
}

static void sha256_hash(const void *data, size_t len, uint8_t digest[32]) {
    mcp_sha256_t c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, digest);
}

/* -------------------------------------------------------------------------
 * HMAC-SHA256 implementation (RFC 2104)
 * ---------------------------------------------------------------------- */

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const void *data, size_t data_len,
                        uint8_t out[32]) {
    uint8_t k_pad[64];
    memset(k_pad, 0, sizeof(k_pad));
    if (key_len > 64) {
        sha256_hash(key, key_len, k_pad);
    } else if (key_len > 0) {
        memcpy(k_pad, key, key_len);
    }

    uint8_t i_key_pad[64];
    uint8_t o_key_pad[64];
    for (int i = 0; i < 64; i++) {
        i_key_pad[i] = k_pad[i] ^ 0x36;
        o_key_pad[i] = k_pad[i] ^ 0x5c;
    }

    /* Inner hash = SHA256(i_key_pad || data) */
    mcp_sha256_t inner_ctx;
    sha256_init(&inner_ctx);
    sha256_update(&inner_ctx, i_key_pad, 64);
    sha256_update(&inner_ctx, data, data_len);
    uint8_t inner_hash[32];
    sha256_final(&inner_ctx, inner_hash);

    /* Outer hash = SHA256(o_key_pad || inner_hash) */
    mcp_sha256_t outer_ctx;
    sha256_init(&outer_ctx);
    sha256_update(&outer_ctx, o_key_pad, 64);
    sha256_update(&outer_ctx, inner_hash, 32);
    sha256_final(&outer_ctx, out);
}

/* -------------------------------------------------------------------------
 * URL-Safe Base64 (RFC 4648 §5, unpadded encode, unpadded/padded decode)
 * ---------------------------------------------------------------------- */

static const char k_b64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64url_char_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    if (c == '=') return -2; /* padding */
    return -1;               /* invalid */
}

static char *b64url_encode(const mcp_allocator_t *a, const uint8_t *data, size_t len) {
    size_t out_len = (len / 3) * 4 + ((len % 3 == 1) ? 2 : (len % 3 == 2) ? 3 : 0);
    char *out = a->malloc_fn(out_len + 1, a->userdata);
    if (out == NULL) return NULL;

    size_t i = 0, j = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)data[i] << 16) |
                     ((uint32_t)data[i + 1] << 8) |
                     ((uint32_t)data[i + 2]);
        out[j++] = k_b64url_chars[(v >> 18) & 0x3F];
        out[j++] = k_b64url_chars[(v >> 12) & 0x3F];
        out[j++] = k_b64url_chars[(v >> 6) & 0x3F];
        out[j++] = k_b64url_chars[v & 0x3F];
        i += 3;
    }
    if (i < len) {
        if (len - i == 1) {
            uint32_t v = (uint32_t)data[i] << 16;
            out[j++] = k_b64url_chars[(v >> 18) & 0x3F];
            out[j++] = k_b64url_chars[(v >> 12) & 0x3F];
        } else {
            uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
            out[j++] = k_b64url_chars[(v >> 18) & 0x3F];
            out[j++] = k_b64url_chars[(v >> 12) & 0x3F];
            out[j++] = k_b64url_chars[(v >> 6) & 0x3F];
        }
    }
    out[j] = '\0';
    return out;
}

static uint8_t *b64url_decode(const mcp_allocator_t *a, const char *str, size_t len, size_t *out_len) {
    if (out_len == NULL) return NULL;
    *out_len = 0;

    size_t max_out = (len / 4 + 1) * 3;
    uint8_t *buf = a->malloc_fn(max_out + 1, a->userdata);
    if (buf == NULL) return NULL;

    uint32_t val = 0;
    int bits = 0;
    size_t out_idx = 0;

    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        int v = b64url_char_val(c);
        if (v == -2) {
            /* Padding '=': ensure remaining characters are all '=' */
            for (size_t k = i + 1; k < len; k++) {
                if (str[k] != '=') {
                    a->free_fn(buf, a->userdata);
                    return NULL;
                }
            }
            break;
        }
        if (v < 0) {
            a->free_fn(buf, a->userdata);
            return NULL;
        }
        val = (val << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[out_idx++] = (uint8_t)(val >> bits);
        }
    }

    if (bits >= 6 || (bits > 0 && (val & ((1U << bits) - 1)) != 0)) {
        /* Incomplete/dangling sextet or non-zero padding bits (invalid base64) */
        a->free_fn(buf, a->userdata);
        return NULL;
    }

    buf[out_idx] = '\0';
    *out_len = out_idx;
    return buf;
}

/* -------------------------------------------------------------------------
 * Decimal string parsing and clock
 * ---------------------------------------------------------------------- */

static bool parse_uint64(const char *s, size_t len, uint64_t *out) {
    if (len == 0 || len > 20) return false;
    uint64_t val = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        uint64_t digit = (uint64_t)(s[i] - '0');
        if (val > (UINT64_MAX - digit) / 10) return false;
        val = val * 10 + digit;
    }
    *out = val;
    return true;
}

static uint64_t current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* -------------------------------------------------------------------------
 * Raw packing and unpacking
 * ---------------------------------------------------------------------- */

mcp_status_t mcp_mrtr_state_pack_raw_ex(mcp_context_t *ctx,
                                        const void *data,
                                        size_t data_len,
                                        const uint8_t *key,
                                        size_t key_len,
                                        uint64_t issued_at_ms,
                                        uint64_t ttl_ms,
                                        char **state_out) {
    if (state_out == NULL || (data == NULL && data_len > 0)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *state_out = NULL;

    const mcp_allocator_t *a = get_allocator(ctx);

    char *b64_payload = b64url_encode(a, (const uint8_t *)(data ? data : ""), data_len);
    if (b64_payload == NULL) {
        return MCP_ERR_NOMEM;
    }
    size_t payload_len = strlen(b64_payload);

    char meta_buf[64];
    int meta_len = snprintf(meta_buf, sizeof(meta_buf), ".%" PRIu64 ".%" PRIu64,
                            issued_at_ms, ttl_ms);
    if (meta_len < 0 || (size_t)meta_len >= sizeof(meta_buf)) {
        a->free_fn(b64_payload, a->userdata);
        return MCP_ERR_NOMEM;
    }

    size_t signed_part_len = payload_len + (size_t)meta_len;
    char *signed_part = a->malloc_fn(signed_part_len + 1, a->userdata);
    if (signed_part == NULL) {
        a->free_fn(b64_payload, a->userdata);
        return MCP_ERR_NOMEM;
    }
    memcpy(signed_part, b64_payload, payload_len);
    memcpy(signed_part + payload_len, meta_buf, (size_t)meta_len + 1);
    a->free_fn(b64_payload, a->userdata);

    char *sig_str = NULL;
    if (key != NULL && key_len > 0) {
        uint8_t hmac_out[32];
        hmac_sha256(key, key_len, signed_part, signed_part_len, hmac_out);
        sig_str = b64url_encode(a, hmac_out, 32);
        if (sig_str == NULL) {
            a->free_fn(signed_part, a->userdata);
            return MCP_ERR_NOMEM;
        }
    } else {
        sig_str = a->malloc_fn(sizeof("unsigned"), a->userdata);
        if (sig_str == NULL) {
            a->free_fn(signed_part, a->userdata);
            return MCP_ERR_NOMEM;
        }
        memcpy(sig_str, "unsigned", sizeof("unsigned"));
    }

    size_t sig_len = strlen(sig_str);
    size_t total_len = signed_part_len + 1 + sig_len; /* +1 for '.' */
    char *token = a->malloc_fn(total_len + 1, a->userdata);
    if (token == NULL) {
        a->free_fn(signed_part, a->userdata);
        a->free_fn(sig_str, a->userdata);
        return MCP_ERR_NOMEM;
    }

    memcpy(token, signed_part, signed_part_len);
    token[signed_part_len] = '.';
    memcpy(token + signed_part_len + 1, sig_str, sig_len + 1);

    a->free_fn(signed_part, a->userdata);
    a->free_fn(sig_str, a->userdata);

    *state_out = token;
    return MCP_OK;
}

mcp_status_t mcp_mrtr_state_pack_raw(mcp_context_t *ctx,
                                     const void *data,
                                     size_t data_len,
                                     const uint8_t *key,
                                     size_t key_len,
                                     uint64_t ttl_ms,
                                     char **state_out) {
    return mcp_mrtr_state_pack_raw_ex(ctx, data, data_len, key, key_len,
                                      current_time_ms(), ttl_ms, state_out);
}

mcp_status_t mcp_mrtr_state_unpack_raw_ex(mcp_context_t *ctx,
                                          const char *state_str,
                                          const uint8_t *key,
                                          size_t key_len,
                                          uint64_t now_ms,
                                          void **data_out,
                                          size_t *data_len_out) {
    if (state_str == NULL || data_out == NULL || data_len_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *data_out = NULL;
    *data_len_out = 0;

    const mcp_allocator_t *a = get_allocator(ctx);

    const char *p1 = strchr(state_str, '.');
    if (p1 == NULL) return MCP_ERR_INVALID_ARGUMENT;
    const char *p2 = strchr(p1 + 1, '.');
    if (p2 == NULL) return MCP_ERR_INVALID_ARGUMENT;
    const char *p3 = strchr(p2 + 1, '.');
    if (p3 == NULL) return MCP_ERR_INVALID_ARGUMENT;
    if (strchr(p3 + 1, '.') != NULL) return MCP_ERR_INVALID_ARGUMENT;

    size_t part0_len = (size_t)(p1 - state_str);
    const char *part1 = p1 + 1;
    size_t part1_len = (size_t)(p2 - part1);
    const char *part2 = p2 + 1;
    size_t part2_len = (size_t)(p3 - part2);
    const char *part3 = p3 + 1;
    size_t part3_len = strlen(part3);

    uint64_t issued_at = 0, ttl = 0;
    if (!parse_uint64(part1, part1_len, &issued_at) ||
        !parse_uint64(part2, part2_len, &ttl)) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    size_t signed_len = (size_t)(p3 - state_str);
    if (key != NULL && key_len > 0) {
        if (strcmp(part3, "unsigned") == 0) {
            return MCP_ERR_PERMISSION;
        }
        size_t sig_len = 0;
        uint8_t *sig_bytes = b64url_decode(a, part3, part3_len, &sig_len);
        if (sig_bytes == NULL || sig_len != 32) {
            if (sig_bytes != NULL) a->free_fn(sig_bytes, a->userdata);
            return MCP_ERR_PERMISSION;
        }
        uint8_t expected_hmac[32];
        hmac_sha256(key, key_len, state_str, signed_len, expected_hmac);
        int match = mcp_crypto_subtle_equal(expected_hmac, sig_bytes, 32);
        a->free_fn(sig_bytes, a->userdata);
        if (!match) {
            return MCP_ERR_PERMISSION;
        }
    } else {
        if (strcmp(part3, "unsigned") != 0) {
            return MCP_ERR_PERMISSION;
        }
    }

    if (ttl > 0) {
        if (now_ms < issued_at) {
            if (issued_at - now_ms > 10000ULL) {
                return MCP_ERR_INVALID_ARGUMENT;
            }
        }
        if (now_ms > issued_at + ttl) {
            return MCP_ERR_TIMEOUT;
        }
    }

    size_t payload_len = 0;
    uint8_t *payload_data = b64url_decode(a, state_str, part0_len, &payload_len);
    if (payload_data == NULL && part0_len > 0) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    *data_out = payload_data;
    *data_len_out = payload_len;
    return MCP_OK;
}

mcp_status_t mcp_mrtr_state_unpack_raw(mcp_context_t *ctx,
                                       const char *state_str,
                                       const uint8_t *key,
                                       size_t key_len,
                                       void **data_out,
                                       size_t *data_len_out) {
    return mcp_mrtr_state_unpack_raw_ex(ctx, state_str, key, key_len,
                                        current_time_ms(), data_out, data_len_out);
}

/* -------------------------------------------------------------------------
 * JSON packing and unpacking
 * ---------------------------------------------------------------------- */

mcp_status_t mcp_mrtr_state_pack(mcp_context_t *ctx,
                                 const mcp_json_value_t *state,
                                 const uint8_t *key,
                                 size_t key_len,
                                 uint64_t ttl_ms,
                                 char **state_out) {
    if (state == NULL || state_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    char *json_str = mcp_json_serialize(ctx, state);
    if (json_str == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_mrtr_state_pack_raw(ctx, json_str, strlen(json_str),
                                             key, key_len, ttl_ms, state_out);
    mcp_json_free_string(ctx, json_str);
    return st;
}

mcp_status_t mcp_mrtr_state_unpack(mcp_context_t *ctx,
                                   const char *state_str,
                                   const uint8_t *key,
                                   size_t key_len,
                                   mcp_json_value_t **state_out) {
    if (state_str == NULL || state_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *state_out = NULL;

    void *data = NULL;
    size_t data_len = 0;
    mcp_status_t st = mcp_mrtr_state_unpack_raw(ctx, state_str, key, key_len,
                                               &data, &data_len);
    if (st != MCP_OK) {
        return st;
    }

    mcp_json_value_t *val = mcp_json_parse(ctx, (const char *)data, data_len);
    mcp_mrtr_state_free(ctx, (char *)data);
    if (val == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    *state_out = val;
    return MCP_OK;
}
