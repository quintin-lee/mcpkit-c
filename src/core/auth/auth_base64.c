/**
 * @file auth_base64.c
 * @brief RFC 4648 §4 standard base64 and SEP-2243 sentinel encoding/decoding.
 */

#include "internal.h"

static const char b64_std_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *mcp_base64_encode(mcp_context_t *ctx, const uint8_t *in, size_t in_len) {
    if (in == NULL && in_len > 0) {
        return NULL;
    }
    size_t out_len = ((in_len + 2) / 3) * 4; /* +4 covers padding */
    const mcp_allocator_t *a = alloc_of(ctx);
    char *out = (char *)a->malloc_fn(out_len + 1, a->userdata);
    if (out == NULL) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= (uint32_t)in[i + 2];
        out[j++] = b64_std_chars[(v >> 18) & 0x3F];
        out[j++] = b64_std_chars[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < in_len) ? b64_std_chars[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < in_len) ? b64_std_chars[v & 0x3F] : '=';
    }
    out[out_len] = '\0';
    return out;
}

static int b64_std_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

mcp_status_t mcp_base64_decode(mcp_context_t *ctx, const char *in, size_t in_len,
                               uint8_t **out, size_t *out_len) {
    if (out == NULL || out_len == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    *out_len = 0;
    if (in == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    if (in_len == 0) {
        uint8_t *zero = (uint8_t *)a->malloc_fn(1, a->userdata);
        if (zero == NULL) {
            return MCP_ERR_NOMEM;
        }
        *out = zero;
        *out_len = 0;
        return MCP_OK;
    }
    /* RFC 4648 §2: strip trailing '=' padding. */
    size_t pad = 0;
    while (pad < in_len && in[in_len - 1 - pad] == '=') {
        pad++;
    }
    size_t n = in_len - pad;
    if (n == 0) {
        uint8_t *zero = (uint8_t *)a->malloc_fn(1, a->userdata);
        if (zero == NULL) {
            return MCP_ERR_NOMEM;
        }
        *out = zero;
        *out_len = 0;
        return MCP_OK;
    }
    if (n % 4 == 1) {
        return MCP_ERR_PROTOCOL;
    }
    size_t byte_len = (n / 4) * 3 + (n % 4 == 2 ? 1 : n % 4 == 3 ? 2 : 0);
    uint8_t *out_buf = (uint8_t *)a->malloc_fn(byte_len, a->userdata);
    if (out_buf == NULL) {
        return MCP_ERR_NOMEM;
    }
    /* Process complete 4-char groups first. */
    size_t full_groups = n / 4;
    size_t o = 0;
    for (size_t g = 0; g < full_groups; g++) {
        uint32_t v = (uint32_t)b64_std_val(in[g * 4]) << 18 |
                     (uint32_t)b64_std_val(in[g * 4 + 1]) << 12 |
                     (uint32_t)b64_std_val(in[g * 4 + 2]) << 6 |
                     (uint32_t)b64_std_val(in[g * 4 + 3]);
        if (b64_std_val(in[g * 4]) < 0 || b64_std_val(in[g * 4 + 1]) < 0 ||
            b64_std_val(in[g * 4 + 2]) < 0 || b64_std_val(in[g * 4 + 3]) < 0) {
            a->free_fn(out_buf, a->userdata);
            *out = NULL;
            *out_len = 0;
            return MCP_ERR_PROTOCOL;
        }
        out_buf[o++] = (uint8_t)((v >> 16) & 0xFF);
        out_buf[o++] = (uint8_t)((v >> 8) & 0xFF);
        out_buf[o++] = (uint8_t)(v & 0xFF);
    }
    /* Process the leftover partial group (n%4==2 or n%4==3 chars).
       2 chars -> 1 byte: byte = (c0<<2) | (c1>>4)
       3 chars -> 2 bytes: b0 = (c0<<2)|(c1>>4); b1 = ((c1&0xF)<<4)|(c2>>2) */
    size_t rem = n % 4;
    if (rem == 2) {
        size_t base = full_groups * 4;
        int c0 = b64_std_val(in[base]);
        int c1 = b64_std_val(in[base + 1]);
        if (c0 < 0 || c1 < 0) {
            a->free_fn(out_buf, a->userdata);
            *out = NULL;
            *out_len = 0;
            return MCP_ERR_PROTOCOL;
        }
        out_buf[o++] = (uint8_t)(((uint32_t)c0 << 2) | ((uint32_t)c1 >> 4));
    } else if (rem == 3) {
        size_t base = full_groups * 4;
        int c0 = b64_std_val(in[base]);
        int c1 = b64_std_val(in[base + 1]);
        int c2 = b64_std_val(in[base + 2]);
        if (c0 < 0 || c1 < 0 || c2 < 0) {
            a->free_fn(out_buf, a->userdata);
            *out = NULL;
            *out_len = 0;
            return MCP_ERR_PROTOCOL;
        }
        out_buf[o++] = (uint8_t)(((uint32_t)c0 << 2) | ((uint32_t)c1 >> 4));
        out_buf[o++] = (uint8_t)(((uint32_t)(c1 & 0xF) << 4) | ((uint32_t)c2 >> 2));
    }
    *out = out_buf;
    *out_len = byte_len;
    return MCP_OK;
}

/* SEP-2243 base64 sentinel: `=?base64?{B64}?=` (RFC 4648 §4 padded).
   mcp_base64_sentinel_decode (mcp_base64_decode for the stripped payload).
   Non-sentinel input is returned as a heap copy so the caller always owns *out. */
mcp_status_t mcp_base64_sentinel_decode(mcp_context_t *ctx, const char *in, char **out) {
    if (in == NULL || out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    const mcp_allocator_t *alloc = alloc_of(ctx);

    const char *PREFIX = "=?base64?";
    const size_t PREFIX_LEN = 9;
    const size_t SUFFIX_LEN = 2; /* "?=" */

    if (strncmp(in, PREFIX, PREFIX_LEN) == 0 && strlen(in) > PREFIX_LEN + SUFFIX_LEN) {
        const char *payload = in + PREFIX_LEN;
        size_t payload_len = strlen(payload) - SUFFIX_LEN; /* strip trailing "?=" */
        uint8_t *decoded = NULL;
        size_t decoded_len = 0;
        mcp_status_t st = mcp_base64_decode(ctx, payload, payload_len, &decoded, &decoded_len);
        if (st != MCP_OK) {
            return st;
        }
        /* Allocate NUL-terminated copy via ctx allocator */
        char *result = (char *)alloc->malloc_fn(decoded_len + 1, alloc->userdata);
        if (result == NULL) {
            mcp_free(ctx, decoded);
            return MCP_ERR_NOMEM;
        }
        if (decoded_len > 0) {
            memcpy(result, decoded, decoded_len);
        }
        result[decoded_len] = '\0';
        mcp_free(ctx, decoded);
        *out = result;
        return MCP_OK;
    }

    /* Non-sentinel: return a heap copy of the original value */
    size_t len = strlen(in);
    char *result = (char *)alloc->malloc_fn(len + 1, alloc->userdata);
    if (result == NULL) {
        return MCP_ERR_NOMEM;
    }
    memcpy(result, in, len + 1);
    *out = result;
    return MCP_OK;
}
