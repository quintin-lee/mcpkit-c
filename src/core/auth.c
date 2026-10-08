/**
 * @file auth.c
 * @brief RFC 7636 PKCE & OAuth 2.1 utilities.
 */

#include "mcpkit/core/auth.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mcpkit/json/array.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"

/* -------------------------------------------------------------------------
 * SHA-256 implementation (FIPS 180-4 / RFC 6234)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t count;
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
 * Base64URL encoding (RFC 4648 §5, unpadded)
 * ---------------------------------------------------------------------- */

static void b64url_encode_32(const uint8_t in[32], char out[44]) {
    static const char chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0, j = 0;
    while (i + 2 < 32) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1] << 8) |
                     ((uint32_t)in[i + 2]);
        out[j++] = chars[(v >> 18) & 0x3F];
        out[j++] = chars[(v >> 12) & 0x3F];
        out[j++] = chars[(v >> 6) & 0x3F];
        out[j++] = chars[v & 0x3F];
        i += 3;
    }
    uint32_t v = ((uint32_t)in[30] << 16) | ((uint32_t)in[31] << 8);
    out[j++] = chars[(v >> 18) & 0x3F];
    out[j++] = chars[(v >> 12) & 0x3F];
    out[j++] = chars[(v >> 6) & 0x3F];
    out[j] = '\0';
}

static inline bool is_pkce_unreserved(char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

mcp_status_t mcp_pkce_compute_challenge(const char *code_verifier,
                                        char code_challenge_out[44]) {
    if (code_verifier == NULL || code_challenge_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    size_t len = strlen(code_verifier);
    if (len < MCP_PKCE_VERIFIER_MIN_LEN || len > MCP_PKCE_VERIFIER_MAX_LEN) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_pkce_unreserved(code_verifier[i])) {
            return MCP_ERR_INVALID_ARGUMENT;
        }
    }

    uint8_t digest[32];
    sha256_hash(code_verifier, len, digest);
    b64url_encode_32(digest, code_challenge_out);
    return MCP_OK;
}

mcp_status_t mcp_pkce_generate(mcp_context_t *ctx,
                               char code_verifier_out[129],
                               char code_challenge_out[44]) {
    (void)ctx;
    if (code_verifier_out == NULL || code_challenge_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    static const char verifier_charset[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    size_t charset_len = strlen(verifier_charset);

    uint8_t rand_bytes[64];
    FILE *f = fopen("/dev/urandom", "rb");
    bool read_ok = false;
    if (f != NULL) {
        if (fread(rand_bytes, 1, sizeof(rand_bytes), f) == sizeof(rand_bytes)) {
            read_ok = true;
        }
        fclose(f);
    }
    if (!read_ok) {
        unsigned int seed = (unsigned int)(time(NULL) ^ (uintptr_t)code_verifier_out);
        for (size_t i = 0; i < sizeof(rand_bytes); i++) {
            seed = seed * 1103515245U + 12345U;
            rand_bytes[i] = (uint8_t)((seed >> 16) & 0xFF);
        }
    }

    for (size_t i = 0; i < 64; i++) {
        code_verifier_out[i] = verifier_charset[rand_bytes[i] % charset_len];
    }
    code_verifier_out[64] = '\0';

    return mcp_pkce_compute_challenge(code_verifier_out, code_challenge_out);
}

static void copy_string_field(mcp_context_t *ctx, const mcp_json_value_t *obj,
                              const char *key, char *out, size_t out_cap) {
    const mcp_json_value_t *v = mcp_json_object_get(ctx, obj, key);
    if (v == NULL || mcp_json_type(ctx, v) != MCP_JSON_STRING) {
        out[0] = '\0';
        return;
    }
    const char *s = NULL;
    if (mcp_json_string_value(ctx, v, &s) == MCP_OK && s != NULL) {
        strncpy(out, s, out_cap - 1);
        out[out_cap - 1] = '\0';
    } else {
        out[0] = '\0';
    }
}

mcp_status_t mcp_oauth_metadata_parse(mcp_context_t *ctx,
                                      const mcp_json_value_t *json_metadata,
                                      mcp_oauth_metadata_t *out_metadata) {
    if (json_metadata == NULL || out_metadata == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_json_type(ctx, json_metadata) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    memset(out_metadata, 0, sizeof(*out_metadata));

    copy_string_field(ctx, json_metadata, "issuer", out_metadata->issuer, sizeof(out_metadata->issuer));
    copy_string_field(ctx, json_metadata, "authorization_endpoint", out_metadata->authorization_endpoint, sizeof(out_metadata->authorization_endpoint));
    copy_string_field(ctx, json_metadata, "token_endpoint", out_metadata->token_endpoint, sizeof(out_metadata->token_endpoint));
    copy_string_field(ctx, json_metadata, "registration_endpoint", out_metadata->registration_endpoint, sizeof(out_metadata->registration_endpoint));
    copy_string_field(ctx, json_metadata, "jwks_uri", out_metadata->jwks_uri, sizeof(out_metadata->jwks_uri));

    if (out_metadata->issuer[0] == '\0' ||
        out_metadata->authorization_endpoint[0] == '\0' ||
        out_metadata->token_endpoint[0] == '\0') {
        return MCP_ERR_PROTOCOL;
    }

    const mcp_json_value_t *cc_methods = mcp_json_object_get(ctx, json_metadata, "code_challenge_methods_supported");
    if (cc_methods != NULL && mcp_json_type(ctx, cc_methods) == MCP_JSON_ARRAY) {
        size_t n = mcp_json_array_size(ctx, cc_methods);
        for (size_t i = 0; i < n; i++) {
            const mcp_json_value_t *item = mcp_json_array_get(ctx, cc_methods, i);
            const char *method = NULL;
            if (mcp_json_string_value(ctx, item, &method) == MCP_OK && method != NULL) {
                if (strcmp(method, "S256") == 0) {
                    out_metadata->supports_pkce_s256 = true;
                    break;
                }
            }
        }
    }

    const mcp_json_value_t *doc_sup = mcp_json_object_get(ctx, json_metadata, "client_id_metadata_document_supported");
    if (doc_sup != NULL && mcp_json_type(ctx, doc_sup) == MCP_JSON_BOOL) {
        bool b = false;
        if (mcp_json_bool_value(ctx, doc_sup, &b) == MCP_OK) {
            out_metadata->client_id_metadata_document_supported = b;
        }
    }

    return MCP_OK;
}

bool mcp_oauth_validate_issuer(const char *expected_issuer, const char *response_issuer) {
    if (expected_issuer == NULL) {
        return false;
    }
    if (response_issuer == NULL) {
        return true;
    }
    return strcmp(expected_issuer, response_issuer) == 0;
}

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static char *auth_strdup(mcp_context_t *ctx, const char *s) {
    if (s == NULL) return NULL;
    size_t len = strlen(s);
    const mcp_allocator_t *a = alloc_of(ctx);
    char *dup = a->malloc_fn(len + 1, a->userdata);
    if (dup != NULL) {
        memcpy(dup, s, len + 1);
    }
    return dup;
}

void mcp_oauth_free_string(mcp_context_t *ctx, char *str) {
    if (str != NULL) {
        const mcp_allocator_t *a = alloc_of(ctx);
        a->free_fn(str, a->userdata);
    }
}

mcp_status_t mcp_oauth_token_response_parse(mcp_context_t *ctx,
                                            const char *json_str,
                                            size_t len,
                                            mcp_oauth_token_response_t *resp_out) {
    if (json_str == NULL || resp_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    memset(resp_out, 0, sizeof(*resp_out));
    mcp_json_value_t *root = mcp_json_parse(ctx, json_str, len);
    if (root == NULL || mcp_json_type(ctx, root) != MCP_JSON_OBJECT) {
        if (root != NULL) mcp_json_destroy(ctx, root);
        return MCP_ERR_PROTOCOL;
    }
    const mcp_json_value_t *v_acc = mcp_json_object_get(ctx, root, "access_token");
    const mcp_json_value_t *v_typ = mcp_json_object_get(ctx, root, "token_type");
    if (v_acc == NULL || v_typ == NULL ||
        mcp_json_type(ctx, v_acc) != MCP_JSON_STRING ||
        mcp_json_type(ctx, v_typ) != MCP_JSON_STRING) {
        mcp_json_destroy(ctx, root);
        return MCP_ERR_PROTOCOL;
    }
    const char *s_acc = NULL;
    const char *s_typ = NULL;
    mcp_json_string_value(ctx, v_acc, &s_acc);
    mcp_json_string_value(ctx, v_typ, &s_typ);

    resp_out->access_token = auth_strdup(ctx, s_acc);
    resp_out->token_type = auth_strdup(ctx, s_typ);

    const mcp_json_value_t *v_exp = mcp_json_object_get(ctx, root, "expires_in");
    if (v_exp != NULL && mcp_json_type(ctx, v_exp) == MCP_JSON_NUMBER) {
        double d = 0;
        if (mcp_json_number_value(ctx, v_exp, &d) == MCP_OK && d >= 0) {
            resp_out->expires_in = (uint32_t)d;
        }
    }

    const mcp_json_value_t *v_ref = mcp_json_object_get(ctx, root, "refresh_token");
    if (v_ref != NULL && mcp_json_type(ctx, v_ref) == MCP_JSON_STRING) {
        const char *s_ref = NULL;
        mcp_json_string_value(ctx, v_ref, &s_ref);
        resp_out->refresh_token = auth_strdup(ctx, s_ref);
    }

    const mcp_json_value_t *v_scp = mcp_json_object_get(ctx, root, "scope");
    if (v_scp != NULL && mcp_json_type(ctx, v_scp) == MCP_JSON_STRING) {
        const char *s_scp = NULL;
        mcp_json_string_value(ctx, v_scp, &s_scp);
        resp_out->scope = auth_strdup(ctx, s_scp);
    }

    mcp_json_destroy(ctx, root);
    if (resp_out->access_token == NULL || resp_out->token_type == NULL) {
        mcp_oauth_token_response_cleanup(ctx, resp_out);
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

void mcp_oauth_token_response_cleanup(mcp_context_t *ctx,
                                      mcp_oauth_token_response_t *resp) {
    if (resp == NULL) return;
    mcp_oauth_free_string(ctx, resp->access_token);
    mcp_oauth_free_string(ctx, resp->token_type);
    mcp_oauth_free_string(ctx, resp->refresh_token);
    mcp_oauth_free_string(ctx, resp->scope);
    memset(resp, 0, sizeof(*resp));
}

static char *url_encode(mcp_context_t *ctx, const char *str) {
    if (str == NULL) return NULL;
    size_t len = strlen(str);
    size_t cap = len * 3 + 1;
    const mcp_allocator_t *a = alloc_of(ctx);
    char *out = a->malloc_fn(cap, a->userdata);
    if (out == NULL) return NULL;
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[pos++] = (char)c;
        } else {
            out[pos++] = '%';
            out[pos++] = hex[(c >> 4) & 0x0F];
            out[pos++] = hex[c & 0x0F];
        }
    }
    out[pos] = '\0';
    return out;
}

mcp_status_t mcp_oauth_build_token_request_pkce(mcp_context_t *ctx,
                                                const char *code,
                                                const char *code_verifier,
                                                const char *redirect_uri,
                                                const char *client_id,
                                                char **body_out) {
    if (code == NULL || code_verifier == NULL || body_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *body_out = NULL;
    char *enc_code = url_encode(ctx, code);
    char *enc_ver = url_encode(ctx, code_verifier);
    char *enc_uri = redirect_uri ? url_encode(ctx, redirect_uri) : NULL;
    char *enc_cid = client_id ? url_encode(ctx, client_id) : NULL;
    if (enc_code == NULL || enc_ver == NULL ||
        (redirect_uri && enc_uri == NULL) || (client_id && enc_cid == NULL)) {
        mcp_oauth_free_string(ctx, enc_code);
        mcp_oauth_free_string(ctx, enc_ver);
        mcp_oauth_free_string(ctx, enc_uri);
        mcp_oauth_free_string(ctx, enc_cid);
        return MCP_ERR_NOMEM;
    }
    size_t needed = 64 + strlen(enc_code) + strlen(enc_ver) +
                    (enc_uri ? strlen(enc_uri) + 16 : 0) +
                    (enc_cid ? strlen(enc_cid) + 12 : 0);
    const mcp_allocator_t *a = alloc_of(ctx);
    char *buf = a->malloc_fn(needed, a->userdata);
    if (buf == NULL) {
        mcp_oauth_free_string(ctx, enc_code);
        mcp_oauth_free_string(ctx, enc_ver);
        mcp_oauth_free_string(ctx, enc_uri);
        mcp_oauth_free_string(ctx, enc_cid);
        return MCP_ERR_NOMEM;
    }
    int written = snprintf(buf, needed,
             "grant_type=authorization_code&code=%s&code_verifier=%s%s%s%s%s",
             enc_code, enc_ver,
             enc_uri ? "&redirect_uri=" : "", enc_uri ? enc_uri : "",
             enc_cid ? "&client_id=" : "", enc_cid ? enc_cid : "");
    mcp_oauth_free_string(ctx, enc_code);
    mcp_oauth_free_string(ctx, enc_ver);
    mcp_oauth_free_string(ctx, enc_uri);
    mcp_oauth_free_string(ctx, enc_cid);
    if (written < 0) {
        a->free_fn(buf, a->userdata);
        return MCP_ERR_NOMEM;
    }
    *body_out = buf;
    return MCP_OK;
}

mcp_status_t mcp_oauth_build_refresh_request(mcp_context_t *ctx,
                                             const char *refresh_token,
                                             const char *client_id,
                                             const char *scope,
                                             char **body_out) {
    if (refresh_token == NULL || body_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *body_out = NULL;
    char *enc_ref = url_encode(ctx, refresh_token);
    char *enc_cid = client_id ? url_encode(ctx, client_id) : NULL;
    char *enc_scp = scope ? url_encode(ctx, scope) : NULL;
    if (enc_ref == NULL || (client_id && enc_cid == NULL) || (scope && enc_scp == NULL)) {
        mcp_oauth_free_string(ctx, enc_ref);
        mcp_oauth_free_string(ctx, enc_cid);
        mcp_oauth_free_string(ctx, enc_scp);
        return MCP_ERR_NOMEM;
    }
    size_t needed = 64 + strlen(enc_ref) +
                    (enc_cid ? strlen(enc_cid) + 12 : 0) +
                    (enc_scp ? strlen(enc_scp) + 8 : 0);
    const mcp_allocator_t *a = alloc_of(ctx);
    char *buf = a->malloc_fn(needed, a->userdata);
    if (buf == NULL) {
        mcp_oauth_free_string(ctx, enc_ref);
        mcp_oauth_free_string(ctx, enc_cid);
        mcp_oauth_free_string(ctx, enc_scp);
        return MCP_ERR_NOMEM;
    }
    int written = snprintf(buf, needed,
             "grant_type=refresh_token&refresh_token=%s%s%s%s%s",
             enc_ref,
             enc_cid ? "&client_id=" : "", enc_cid ? enc_cid : "",
             enc_scp ? "&scope=" : "", enc_scp ? enc_scp : "");
    mcp_oauth_free_string(ctx, enc_ref);
    mcp_oauth_free_string(ctx, enc_cid);
    mcp_oauth_free_string(ctx, enc_scp);
    if (written < 0) {
        a->free_fn(buf, a->userdata);
        return MCP_ERR_NOMEM;
    }
    *body_out = buf;
    return MCP_OK;
}

mcp_status_t mcp_oauth_build_client_credentials_request(mcp_context_t *ctx,
                                                        const char *client_id,
                                                        const char *client_secret,
                                                        const char *scope,
                                                        char **body_out) {
    if (client_id == NULL || body_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *body_out = NULL;
    char *enc_cid = url_encode(ctx, client_id);
    char *enc_sec = client_secret ? url_encode(ctx, client_secret) : NULL;
    char *enc_scp = scope ? url_encode(ctx, scope) : NULL;
    if (enc_cid == NULL || (client_secret && enc_sec == NULL) || (scope && enc_scp == NULL)) {
        mcp_oauth_free_string(ctx, enc_cid);
        mcp_oauth_free_string(ctx, enc_sec);
        mcp_oauth_free_string(ctx, enc_scp);
        return MCP_ERR_NOMEM;
    }
    size_t needed = 64 + strlen(enc_cid) +
                    (enc_sec ? strlen(enc_sec) + 16 : 0) +
                    (enc_scp ? strlen(enc_scp) + 8 : 0);
    const mcp_allocator_t *a = alloc_of(ctx);
    char *buf = a->malloc_fn(needed, a->userdata);
    if (buf == NULL) {
        mcp_oauth_free_string(ctx, enc_cid);
        mcp_oauth_free_string(ctx, enc_sec);
        mcp_oauth_free_string(ctx, enc_scp);
        return MCP_ERR_NOMEM;
    }
    int written = snprintf(buf, needed,
             "grant_type=client_credentials&client_id=%s%s%s%s%s",
             enc_cid,
             enc_sec ? "&client_secret=" : "", enc_sec ? enc_sec : "",
             enc_scp ? "&scope=" : "", enc_scp ? enc_scp : "");
    mcp_oauth_free_string(ctx, enc_cid);
    mcp_oauth_free_string(ctx, enc_sec);
    mcp_oauth_free_string(ctx, enc_scp);
    if (written < 0) {
        a->free_fn(buf, a->userdata);
        return MCP_ERR_NOMEM;
    }
    *body_out = buf;
    return MCP_OK;
}

/* -------------------------------------------------------------------------
 * Client ID Metadata Documents (SEP-991 / draft-ietf-oauth-client-id-metadata-document)
 * ---------------------------------------------------------------------- */

void mcp_oauth_client_metadata_cleanup(mcp_context_t *ctx,
                                       mcp_oauth_client_metadata_t *meta) {
    if (meta == NULL) return;
    const mcp_allocator_t *a = alloc_of(ctx);
    mcp_oauth_free_string(ctx, meta->client_id);
    mcp_oauth_free_string(ctx, meta->client_name);
    mcp_oauth_free_string(ctx, meta->client_uri);
    mcp_oauth_free_string(ctx, meta->logo_uri);
    mcp_oauth_free_string(ctx, meta->token_endpoint_auth_method);
    mcp_oauth_free_string(ctx, meta->jwks_uri);

    if (meta->redirect_uris != NULL) {
        for (size_t i = 0; i < meta->redirect_uris_count; i++) {
            mcp_oauth_free_string(ctx, meta->redirect_uris[i]);
        }
        a->free_fn(meta->redirect_uris, a->userdata);
    }
    if (meta->grant_types != NULL) {
        for (size_t i = 0; i < meta->grant_types_count; i++) {
            mcp_oauth_free_string(ctx, meta->grant_types[i]);
        }
        a->free_fn(meta->grant_types, a->userdata);
    }
    if (meta->response_types != NULL) {
        for (size_t i = 0; i < meta->response_types_count; i++) {
            mcp_oauth_free_string(ctx, meta->response_types[i]);
        }
        a->free_fn(meta->response_types, a->userdata);
    }
    memset(meta, 0, sizeof(*meta));
}

mcp_status_t mcp_oauth_client_metadata_parse(mcp_context_t *ctx,
                                             const char *json_str,
                                             size_t len,
                                             mcp_oauth_client_metadata_t *meta_out) {
    if (json_str == NULL || meta_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    memset(meta_out, 0, sizeof(*meta_out));
    mcp_json_value_t *root = mcp_json_parse(ctx, json_str, len);
    if (root == NULL || mcp_json_type(ctx, root) != MCP_JSON_OBJECT) {
        if (root != NULL) mcp_json_destroy(ctx, root);
        return MCP_ERR_PROTOCOL;
    }

    const mcp_json_value_t *v_cid = mcp_json_object_get(ctx, root, "client_id");
    const mcp_json_value_t *v_name = mcp_json_object_get(ctx, root, "client_name");
    const mcp_json_value_t *v_redirs = mcp_json_object_get(ctx, root, "redirect_uris");

    if (v_cid == NULL || v_name == NULL || v_redirs == NULL ||
        mcp_json_type(ctx, v_cid) != MCP_JSON_STRING ||
        mcp_json_type(ctx, v_name) != MCP_JSON_STRING ||
        mcp_json_type(ctx, v_redirs) != MCP_JSON_ARRAY) {
        mcp_json_destroy(ctx, root);
        return MCP_ERR_PROTOCOL;
    }

    const char *s_cid = NULL;
    const char *s_name = NULL;
    mcp_json_string_value(ctx, v_cid, &s_cid);
    mcp_json_string_value(ctx, v_name, &s_name);
    meta_out->client_id = auth_strdup(ctx, s_cid);
    meta_out->client_name = auth_strdup(ctx, s_name);

    const mcp_json_value_t *v_curi = mcp_json_object_get(ctx, root, "client_uri");
    if (v_curi != NULL && mcp_json_type(ctx, v_curi) == MCP_JSON_STRING) {
        const char *s_curi = NULL;
        mcp_json_string_value(ctx, v_curi, &s_curi);
        meta_out->client_uri = auth_strdup(ctx, s_curi);
    }

    const mcp_json_value_t *v_logo = mcp_json_object_get(ctx, root, "logo_uri");
    if (v_logo != NULL && mcp_json_type(ctx, v_logo) == MCP_JSON_STRING) {
        const char *s_logo = NULL;
        mcp_json_string_value(ctx, v_logo, &s_logo);
        meta_out->logo_uri = auth_strdup(ctx, s_logo);
    }

    const mcp_json_value_t *v_meth = mcp_json_object_get(ctx, root, "token_endpoint_auth_method");
    if (v_meth != NULL && mcp_json_type(ctx, v_meth) == MCP_JSON_STRING) {
        const char *s_meth = NULL;
        mcp_json_string_value(ctx, v_meth, &s_meth);
        meta_out->token_endpoint_auth_method = auth_strdup(ctx, s_meth);
    }

    const mcp_json_value_t *v_jwks = mcp_json_object_get(ctx, root, "jwks_uri");
    if (v_jwks != NULL && mcp_json_type(ctx, v_jwks) == MCP_JSON_STRING) {
        const char *s_jwks = NULL;
        mcp_json_string_value(ctx, v_jwks, &s_jwks);
        meta_out->jwks_uri = auth_strdup(ctx, s_jwks);
    }

    size_t r_count = mcp_json_array_size(ctx, v_redirs);
    if (r_count == 0) {
        mcp_json_destroy(ctx, root);
        mcp_oauth_client_metadata_cleanup(ctx, meta_out);
        return MCP_ERR_PROTOCOL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    meta_out->redirect_uris = a->malloc_fn(r_count * sizeof(char *), a->userdata);
    if (meta_out->redirect_uris == NULL) {
        mcp_json_destroy(ctx, root);
        mcp_oauth_client_metadata_cleanup(ctx, meta_out);
        return MCP_ERR_NOMEM;
    }
    memset(meta_out->redirect_uris, 0, r_count * sizeof(char *));
    meta_out->redirect_uris_count = r_count;
    for (size_t i = 0; i < r_count; i++) {
        const mcp_json_value_t *item = mcp_json_array_get(ctx, v_redirs, i);
        const char *str = NULL;
        if (item == NULL || mcp_json_type(ctx, item) != MCP_JSON_STRING ||
            mcp_json_string_value(ctx, item, &str) != MCP_OK || str == NULL) {
            mcp_json_destroy(ctx, root);
            mcp_oauth_client_metadata_cleanup(ctx, meta_out);
            return MCP_ERR_PROTOCOL;
        }
        meta_out->redirect_uris[i] = auth_strdup(ctx, str);
    }

    const mcp_json_value_t *v_grants = mcp_json_object_get(ctx, root, "grant_types");
    if (v_grants != NULL && mcp_json_type(ctx, v_grants) == MCP_JSON_ARRAY) {
        size_t g_count = mcp_json_array_size(ctx, v_grants);
        if (g_count > 0) {
            meta_out->grant_types = a->malloc_fn(g_count * sizeof(char *), a->userdata);
            if (meta_out->grant_types != NULL) {
                memset(meta_out->grant_types, 0, g_count * sizeof(char *));
                meta_out->grant_types_count = g_count;
                for (size_t i = 0; i < g_count; i++) {
                    const mcp_json_value_t *item = mcp_json_array_get(ctx, v_grants, i);
                    const char *str = NULL;
                    if (item && mcp_json_string_value(ctx, item, &str) == MCP_OK && str) {
                        meta_out->grant_types[i] = auth_strdup(ctx, str);
                    }
                }
            }
        }
    }

    const mcp_json_value_t *v_resps = mcp_json_object_get(ctx, root, "response_types");
    if (v_resps != NULL && mcp_json_type(ctx, v_resps) == MCP_JSON_ARRAY) {
        size_t resp_cnt = mcp_json_array_size(ctx, v_resps);
        if (resp_cnt > 0) {
            meta_out->response_types = a->malloc_fn(resp_cnt * sizeof(char *), a->userdata);
            if (meta_out->response_types != NULL) {
                memset(meta_out->response_types, 0, resp_cnt * sizeof(char *));
                meta_out->response_types_count = resp_cnt;
                for (size_t i = 0; i < resp_cnt; i++) {
                    const mcp_json_value_t *item = mcp_json_array_get(ctx, v_resps, i);
                    const char *str = NULL;
                    if (item && mcp_json_string_value(ctx, item, &str) == MCP_OK && str) {
                        meta_out->response_types[i] = auth_strdup(ctx, str);
                    }
                }
            }
        }
    }

    mcp_json_destroy(ctx, root);
    return MCP_OK;
}

mcp_status_t mcp_oauth_client_metadata_validate(const mcp_oauth_client_metadata_t *meta,
                                                const char *expected_url) {
    if (meta == NULL || meta->client_id == NULL || meta->client_name == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (meta->client_name[0] == '\0') {
        return MCP_ERR_PROTOCOL;
    }
    const char *cid = meta->client_id;
    if (strncmp(cid, "https://", 8) != 0) {
        return MCP_ERR_PROTOCOL;
    }
    const char *host_start = cid + 8;
    const char *slash = strchr(host_start, '/');
    if (slash == NULL || slash == host_start || strlen(slash) <= 1) {
        return MCP_ERR_PROTOCOL;
    }
    if (expected_url != NULL && strcmp(cid, expected_url) != 0) {
        return MCP_ERR_PROTOCOL;
    }
    if (meta->redirect_uris == NULL || meta->redirect_uris_count == 0) {
        return MCP_ERR_PROTOCOL;
    }
    for (size_t i = 0; i < meta->redirect_uris_count; i++) {
        if (meta->redirect_uris[i] == NULL || meta->redirect_uris[i][0] == '\0') {
            return MCP_ERR_PROTOCOL;
        }
    }
    return MCP_OK;
}

char *mcp_oauth_client_metadata_serialize(mcp_context_t *ctx,
                                          const mcp_oauth_client_metadata_t *meta) {
    if (meta == NULL) return NULL;
    mcp_json_value_t *root = mcp_json_object_new(ctx);
    if (root == NULL) return NULL;

    if (meta->client_id) {
        mcp_json_object_set_take(ctx, root, "client_id", mcp_json_string_new(ctx, meta->client_id));
    }
    if (meta->client_name) {
        mcp_json_object_set_take(ctx, root, "client_name", mcp_json_string_new(ctx, meta->client_name));
    }
    if (meta->client_uri) {
        mcp_json_object_set_take(ctx, root, "client_uri", mcp_json_string_new(ctx, meta->client_uri));
    }
    if (meta->logo_uri) {
        mcp_json_object_set_take(ctx, root, "logo_uri", mcp_json_string_new(ctx, meta->logo_uri));
    }
    if (meta->redirect_uris && meta->redirect_uris_count > 0) {
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        if (arr != NULL) {
            for (size_t i = 0; i < meta->redirect_uris_count; i++) {
                if (meta->redirect_uris[i]) {
                    mcp_json_array_append(ctx, arr, mcp_json_string_new(ctx, meta->redirect_uris[i]));
                }
            }
            mcp_json_object_set_take(ctx, root, "redirect_uris", arr);
        }
    }
    if (meta->grant_types && meta->grant_types_count > 0) {
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        if (arr != NULL) {
            for (size_t i = 0; i < meta->grant_types_count; i++) {
                if (meta->grant_types[i]) {
                    mcp_json_array_append(ctx, arr, mcp_json_string_new(ctx, meta->grant_types[i]));
                }
            }
            mcp_json_object_set_take(ctx, root, "grant_types", arr);
        }
    }
    if (meta->response_types && meta->response_types_count > 0) {
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        if (arr != NULL) {
            for (size_t i = 0; i < meta->response_types_count; i++) {
                if (meta->response_types[i]) {
                    mcp_json_array_append(ctx, arr, mcp_json_string_new(ctx, meta->response_types[i]));
                }
            }
            mcp_json_object_set_take(ctx, root, "response_types", arr);
        }
    }
    if (meta->token_endpoint_auth_method) {
        mcp_json_object_set_take(ctx, root, "token_endpoint_auth_method",
                                 mcp_json_string_new(ctx, meta->token_endpoint_auth_method));
    }
    if (meta->jwks_uri) {
        mcp_json_object_set_take(ctx, root, "jwks_uri", mcp_json_string_new(ctx, meta->jwks_uri));
    }

    char *out = mcp_json_serialize(ctx, root);
    mcp_json_destroy(ctx, root);
    return out;
}

mcp_status_t mcp_oauth_build_token_exchange_request(mcp_context_t *ctx,
                                                    const mcp_oauth_token_exchange_req_t *req,
                                                    char **body_out) {
    if (req == NULL || body_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *body_out = NULL;
    if (req->subject_token == NULL || req->subject_token[0] == '\0' ||
        req->subject_token_type == NULL || req->subject_token_type[0] == '\0') {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (req->actor_token != NULL && req->actor_token[0] != '\0') {
        if (req->actor_token_type == NULL || req->actor_token_type[0] == '\0') {
            return MCP_ERR_INVALID_ARGUMENT;
        }
    }

    char *enc_sub = url_encode(ctx, req->subject_token);
    char *enc_sub_type = url_encode(ctx, req->subject_token_type);
    char *enc_actor = (req->actor_token && req->actor_token[0] != '\0') ? url_encode(ctx, req->actor_token) : NULL;
    char *enc_actor_type = (req->actor_token_type && req->actor_token_type[0] != '\0') ? url_encode(ctx, req->actor_token_type) : NULL;
    char *enc_res = (req->resource && req->resource[0] != '\0') ? url_encode(ctx, req->resource) : NULL;
    char *enc_aud = (req->audience && req->audience[0] != '\0') ? url_encode(ctx, req->audience) : NULL;
    char *enc_scp = (req->scope && req->scope[0] != '\0') ? url_encode(ctx, req->scope) : NULL;
    char *enc_req_type = (req->requested_token_type && req->requested_token_type[0] != '\0') ? url_encode(ctx, req->requested_token_type) : NULL;

    if (enc_sub == NULL || enc_sub_type == NULL ||
        ((req->actor_token && req->actor_token[0] != '\0') && enc_actor == NULL) ||
        ((req->actor_token_type && req->actor_token_type[0] != '\0') && enc_actor_type == NULL) ||
        ((req->resource && req->resource[0] != '\0') && enc_res == NULL) ||
        ((req->audience && req->audience[0] != '\0') && enc_aud == NULL) ||
        ((req->scope && req->scope[0] != '\0') && enc_scp == NULL) ||
        ((req->requested_token_type && req->requested_token_type[0] != '\0') && enc_req_type == NULL)) {
        mcp_oauth_free_string(ctx, enc_sub);
        mcp_oauth_free_string(ctx, enc_sub_type);
        mcp_oauth_free_string(ctx, enc_actor);
        mcp_oauth_free_string(ctx, enc_actor_type);
        mcp_oauth_free_string(ctx, enc_res);
        mcp_oauth_free_string(ctx, enc_aud);
        mcp_oauth_free_string(ctx, enc_scp);
        mcp_oauth_free_string(ctx, enc_req_type);
        return MCP_ERR_NOMEM;
    }

    size_t needed = 128 + strlen(enc_sub) + strlen(enc_sub_type) +
                    (enc_actor ? strlen(enc_actor) + 16 : 0) +
                    (enc_actor_type ? strlen(enc_actor_type) + 20 : 0) +
                    (enc_res ? strlen(enc_res) + 12 : 0) +
                    (enc_aud ? strlen(enc_aud) + 12 : 0) +
                    (enc_scp ? strlen(enc_scp) + 10 : 0) +
                    (enc_req_type ? strlen(enc_req_type) + 24 : 0);

    const mcp_allocator_t *a = alloc_of(ctx);
    char *buf = a->malloc_fn(needed, a->userdata);
    if (buf == NULL) {
        mcp_oauth_free_string(ctx, enc_sub);
        mcp_oauth_free_string(ctx, enc_sub_type);
        mcp_oauth_free_string(ctx, enc_actor);
        mcp_oauth_free_string(ctx, enc_actor_type);
        mcp_oauth_free_string(ctx, enc_res);
        mcp_oauth_free_string(ctx, enc_aud);
        mcp_oauth_free_string(ctx, enc_scp);
        mcp_oauth_free_string(ctx, enc_req_type);
        return MCP_ERR_NOMEM;
    }

    snprintf(buf, needed,
             "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Atoken-exchange"
             "&subject_token=%s&subject_token_type=%s%s%s%s%s%s%s%s%s%s%s%s%s",
             enc_sub, enc_sub_type,
             enc_actor ? "&actor_token=" : "", enc_actor ? enc_actor : "",
             enc_actor_type ? "&actor_token_type=" : "", enc_actor_type ? enc_actor_type : "",
             enc_res ? "&resource=" : "", enc_res ? enc_res : "",
             enc_aud ? "&audience=" : "", enc_aud ? enc_aud : "",
             enc_scp ? "&scope=" : "", enc_scp ? enc_scp : "",
             enc_req_type ? "&requested_token_type=" : "", enc_req_type ? enc_req_type : "");

    mcp_oauth_free_string(ctx, enc_sub);
    mcp_oauth_free_string(ctx, enc_sub_type);
    mcp_oauth_free_string(ctx, enc_actor);
    mcp_oauth_free_string(ctx, enc_actor_type);
    mcp_oauth_free_string(ctx, enc_res);
    mcp_oauth_free_string(ctx, enc_aud);
    mcp_oauth_free_string(ctx, enc_scp);
    mcp_oauth_free_string(ctx, enc_req_type);

    *body_out = buf;
    return MCP_OK;
}

void mcp_oauth_token_exchange_response_cleanup(mcp_context_t *ctx,
                                               mcp_oauth_token_exchange_response_t *resp) {
    if (resp == NULL) {
        return;
    }
    mcp_oauth_free_string(ctx, resp->access_token);
    mcp_oauth_free_string(ctx, resp->issued_token_type);
    mcp_oauth_free_string(ctx, resp->token_type);
    mcp_oauth_free_string(ctx, resp->refresh_token);
    mcp_oauth_free_string(ctx, resp->scope);
    memset(resp, 0, sizeof(*resp));
}

mcp_status_t mcp_oauth_token_exchange_response_parse(mcp_context_t *ctx,
                                                     const char *json_str,
                                                     size_t len,
                                                     mcp_oauth_token_exchange_response_t *resp_out) {
    if (json_str == NULL || resp_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    memset(resp_out, 0, sizeof(*resp_out));

    mcp_json_value_t *root = mcp_json_parse(ctx, json_str, len);
    if (root == NULL || mcp_json_type(ctx, root) != MCP_JSON_OBJECT) {
        if (root != NULL) {
            mcp_json_destroy(ctx, root);
        }
        return MCP_ERR_PROTOCOL;
    }

    const mcp_json_value_t *v_acc = mcp_json_object_get(ctx, root, "access_token");
    const mcp_json_value_t *v_iss = mcp_json_object_get(ctx, root, "issued_token_type");
    const mcp_json_value_t *v_typ = mcp_json_object_get(ctx, root, "token_type");
    if (v_acc == NULL || v_iss == NULL || v_typ == NULL ||
        mcp_json_type(ctx, v_acc) != MCP_JSON_STRING ||
        mcp_json_type(ctx, v_iss) != MCP_JSON_STRING ||
        mcp_json_type(ctx, v_typ) != MCP_JSON_STRING) {
        mcp_json_destroy(ctx, root);
        return MCP_ERR_PROTOCOL;
    }

    const char *s_acc = NULL;
    const char *s_iss = NULL;
    const char *s_typ = NULL;
    mcp_json_string_value(ctx, v_acc, &s_acc);
    mcp_json_string_value(ctx, v_iss, &s_iss);
    mcp_json_string_value(ctx, v_typ, &s_typ);

    resp_out->access_token = auth_strdup(ctx, s_acc);
    resp_out->issued_token_type = auth_strdup(ctx, s_iss);
    resp_out->token_type = auth_strdup(ctx, s_typ);

    if (resp_out->access_token == NULL ||
        resp_out->issued_token_type == NULL ||
        resp_out->token_type == NULL) {
        mcp_json_destroy(ctx, root);
        mcp_oauth_token_exchange_response_cleanup(ctx, resp_out);
        return MCP_ERR_NOMEM;
    }

    const mcp_json_value_t *v_exp = mcp_json_object_get(ctx, root, "expires_in");
    if (v_exp != NULL && mcp_json_type(ctx, v_exp) == MCP_JSON_NUMBER) {
        double d = 0;
        if (mcp_json_number_value(ctx, v_exp, &d) == MCP_OK && d >= 0) {
            resp_out->expires_in = (uint32_t)d;
        }
    }

    const mcp_json_value_t *v_ref = mcp_json_object_get(ctx, root, "refresh_token");
    if (v_ref != NULL && mcp_json_type(ctx, v_ref) == MCP_JSON_STRING) {
        const char *s_ref = NULL;
        mcp_json_string_value(ctx, v_ref, &s_ref);
        resp_out->refresh_token = auth_strdup(ctx, s_ref);
        if (resp_out->refresh_token == NULL) {
            mcp_json_destroy(ctx, root);
            mcp_oauth_token_exchange_response_cleanup(ctx, resp_out);
            return MCP_ERR_NOMEM;
        }
    }

    const mcp_json_value_t *v_scp = mcp_json_object_get(ctx, root, "scope");
    if (v_scp != NULL && mcp_json_type(ctx, v_scp) == MCP_JSON_STRING) {
        const char *s_scp = NULL;
        mcp_json_string_value(ctx, v_scp, &s_scp);
        resp_out->scope = auth_strdup(ctx, s_scp);
        if (resp_out->scope == NULL) {
            mcp_json_destroy(ctx, root);
            mcp_oauth_token_exchange_response_cleanup(ctx, resp_out);
            return MCP_ERR_NOMEM;
        }
    }

    mcp_json_destroy(ctx, root);
    return MCP_OK;
}

