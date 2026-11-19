/**
 * @file auth_discovery.c
 * @brief RFC 8414 Authorization Server Metadata, RFC 9728 Protected Resource
 *        Metadata, and RFC 9207 Issuer Validation.
 */

#include "internal.h"

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

    /* P3: OIDC Discovery 1.0 endpoints (RFC 8414 §10 / OpenID Connect Discovery) */
    copy_string_field(ctx, json_metadata, "end_session_endpoint", out_metadata->end_session_endpoint, sizeof(out_metadata->end_session_endpoint));
    copy_string_field(ctx, json_metadata, "revocation_endpoint", out_metadata->revocation_endpoint, sizeof(out_metadata->revocation_endpoint));
    copy_string_field(ctx, json_metadata, "introspection_endpoint", out_metadata->introspection_endpoint, sizeof(out_metadata->introspection_endpoint));

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

/* RFC 9728 Protected Resource Metadata (P3 T3) */
static void pr_copy_str(char *dst, size_t dst_cap, const char *src) {
    if (src == NULL || dst == NULL || dst_cap == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_cap) {
        n = dst_cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

mcp_status_t mcp_oauth_protected_resource_parse(mcp_context_t *ctx,
                                                const char *json_str,
                                                size_t len,
                                                mcp_protected_resource_metadata_t *out_meta) {
    if (json_str == NULL || out_meta == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    memset(out_meta, 0, sizeof(*out_meta));
    mcp_json_value_t *root = mcp_json_parse(ctx, json_str, len);
    if (root == NULL || mcp_json_type(ctx, root) != MCP_JSON_OBJECT) {
        if (root != NULL) {
            mcp_json_destroy(ctx, root);
        }
        return MCP_ERR_PROTOCOL;
    }

    const mcp_json_value_t *v_res = mcp_json_object_get(ctx, root, "resource");
    if (v_res == NULL || mcp_json_type(ctx, v_res) != MCP_JSON_STRING) {
        mcp_json_destroy(ctx, root);
        return MCP_ERR_PROTOCOL;
    }
    const char *s_res = NULL;
    mcp_json_string_value(ctx, v_res, &s_res);
    pr_copy_str(out_meta->resource, sizeof(out_meta->resource), s_res);

    const mcp_json_value_t *v_iss = mcp_json_object_get(ctx, root, "issuer");
    if (v_iss != NULL && mcp_json_type(ctx, v_iss) == MCP_JSON_STRING) {
        const char *s_iss = NULL;
        mcp_json_string_value(ctx, v_iss, &s_iss);
        pr_copy_str(out_meta->issuer, sizeof(out_meta->issuer), s_iss);
    }

    const mcp_json_value_t *v_as = mcp_json_object_get(ctx, root, "authorization_servers");
    if (v_as != NULL && mcp_json_type(ctx, v_as) == MCP_JSON_ARRAY) {
        const mcp_json_value_t *first = mcp_json_array_get(ctx, v_as, 0);
        if (first != NULL && mcp_json_type(ctx, first) == MCP_JSON_STRING) {
            const char *s_as = NULL;
            mcp_json_string_value(ctx, first, &s_as);
            pr_copy_str(out_meta->authorization_server, sizeof(out_meta->authorization_server), s_as);
        }
    }

    const mcp_json_value_t *v_sc = mcp_json_object_get(ctx, root, "scopes_supported");
    if (v_sc != NULL && mcp_json_type(ctx, v_sc) == MCP_JSON_ARRAY) {
        const mcp_json_value_t *first = mcp_json_array_get(ctx, v_sc, 0);
        if (first != NULL && mcp_json_type(ctx, first) == MCP_JSON_STRING) {
            const char *s_sc = NULL;
            mcp_json_string_value(ctx, first, &s_sc);
            pr_copy_str(out_meta->scope, sizeof(out_meta->scope), s_sc);
        }
    }

    const mcp_json_value_t *v_jwks = mcp_json_object_get(ctx, root, "jwks_uri");
    if (v_jwks != NULL && mcp_json_type(ctx, v_jwks) == MCP_JSON_STRING) {
        const char *s_jwks = NULL;
        mcp_json_string_value(ctx, v_jwks, &s_jwks);
        pr_copy_str(out_meta->jwks_uri, sizeof(out_meta->jwks_uri), s_jwks);
    }

    mcp_json_destroy(ctx, root);
    return MCP_OK;
}

char *mcp_oauth_protected_resource_serialize(mcp_context_t *ctx,
                                             const mcp_protected_resource_metadata_t *meta) {
    if (meta == NULL) {
        return NULL;
    }
    mcp_json_value_t *root = mcp_json_object_new(ctx);
    if (root == NULL) {
        return NULL;
    }
    /* resource is the only REQUIRED field per RFC 9728 §2 */
    mcp_json_object_set_take(ctx, root, "resource", mcp_json_string_new(ctx, meta->resource));
    if (meta->issuer[0] != '\0') {
        mcp_json_object_set_take(ctx, root, "issuer", mcp_json_string_new(ctx, meta->issuer));
    }
    if (meta->authorization_server[0] != '\0') {
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        if (arr != NULL) {
            mcp_json_array_append(ctx, arr, mcp_json_string_new(ctx, meta->authorization_server));
            mcp_json_object_set_take(ctx, root, "authorization_servers", arr);
        }
    }
    if (meta->scope[0] != '\0') {
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        if (arr != NULL) {
            mcp_json_array_append(ctx, arr, mcp_json_string_new(ctx, meta->scope));
            mcp_json_object_set_take(ctx, root, "scopes_supported", arr);
        }
    }
    if (meta->jwks_uri[0] != '\0') {
        mcp_json_object_set_take(ctx, root, "jwks_uri", mcp_json_string_new(ctx, meta->jwks_uri));
    }
    char *out = mcp_json_serialize(ctx, root);
    mcp_json_destroy(ctx, root);
    return out;
}

mcp_status_t mcp_oauth_discovery_merge(mcp_context_t *ctx,
                                       const mcp_oauth_metadata_t *base,
                                       const mcp_oauth_metadata_t *oidc,
                                       mcp_oauth_metadata_t *merged_out) {
    (void)ctx;
    if (base == NULL || oidc == NULL || merged_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    /* Copy base as the starting point */
    *merged_out = *base;

    /* Endpoint/issuer fields: base wins when non-empty, else fill from oidc */
    if (merged_out->issuer[0] == '\0' && oidc->issuer[0] != '\0')
        memcpy(merged_out->issuer, oidc->issuer, sizeof(oidc->issuer));
    if (merged_out->authorization_endpoint[0] == '\0' && oidc->authorization_endpoint[0] != '\0')
        memcpy(merged_out->authorization_endpoint, oidc->authorization_endpoint, sizeof(oidc->authorization_endpoint));
    if (merged_out->token_endpoint[0] == '\0' && oidc->token_endpoint[0] != '\0')
        memcpy(merged_out->token_endpoint, oidc->token_endpoint, sizeof(oidc->token_endpoint));
    if (merged_out->registration_endpoint[0] == '\0' && oidc->registration_endpoint[0] != '\0')
        memcpy(merged_out->registration_endpoint, oidc->registration_endpoint, sizeof(oidc->registration_endpoint));
    if (merged_out->jwks_uri[0] == '\0' && oidc->jwks_uri[0] != '\0')
        memcpy(merged_out->jwks_uri, oidc->jwks_uri, sizeof(oidc->jwks_uri));
    if (merged_out->end_session_endpoint[0] == '\0' && oidc->end_session_endpoint[0] != '\0')
        memcpy(merged_out->end_session_endpoint, oidc->end_session_endpoint, sizeof(oidc->end_session_endpoint));
    if (merged_out->revocation_endpoint[0] == '\0' && oidc->revocation_endpoint[0] != '\0')
        memcpy(merged_out->revocation_endpoint, oidc->revocation_endpoint, sizeof(oidc->revocation_endpoint));
    if (merged_out->introspection_endpoint[0] == '\0' && oidc->introspection_endpoint[0] != '\0')
        memcpy(merged_out->introspection_endpoint, oidc->introspection_endpoint, sizeof(oidc->introspection_endpoint));

    /* Capability flags: OR of the two inputs */
    merged_out->supports_pkce_s256 = base->supports_pkce_s256 || oidc->supports_pkce_s256;
    merged_out->client_id_metadata_document_supported =
        base->client_id_metadata_document_supported || oidc->client_id_metadata_document_supported;

    return MCP_OK;
}

/* RFC 8414 §3.1 probe URL builder: issuer (trailing '/' stripped) +
 * "/.well-known/oauth-authorization-server". out_cap must be >=
 * strlen(issuer) + 64. */
static void build_as_probe_url(const char *issuer, char *out, size_t out_cap) {
    size_t ilen = strlen(issuer);
    while (ilen > 0 && issuer[ilen - 1] == '/') {
        ilen--;
    }
    size_t n = 0;
    for (size_t i = 0; i < ilen && n + 1 < out_cap; i++) {
        out[n++] = issuer[i];
    }
    const char *suffix = "/.well-known/oauth-authorization-server";
    for (const char *p = suffix; *p != '\0' && n + 1 < out_cap; p++) {
        out[n++] = *p;
    }
    out[n] = '\0';
}

mcp_status_t mcp_oauth_discover_as(mcp_context_t *ctx,
                                   const mcp_protected_resource_metadata_t *prm,
                                   mcp_http_fetch_fn fetch, void *fetch_ud,
                                   const mcp_oauth_metadata_t *existing_as,
                                   mcp_oauth_metadata_t *out_merged) {
    if (out_merged == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    /* Zero out the output struct before any early-return */
    memset(out_merged, 0, sizeof(*out_merged));
    if (prm == NULL || prm->authorization_server[0] == '\0') {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (fetch == NULL) {
        return MCP_ERR_PROTOCOL;
    }

    /* Build the RFC 8414 §3.1 probe URL */
    const mcp_allocator_t *alloc = alloc_of(ctx);
    size_t url_cap = strlen(prm->authorization_server) + 64;
    char *url = (char *)alloc->malloc_fn(url_cap, alloc->userdata);
    if (url == NULL) {
        return MCP_ERR_NOMEM;
    }
    build_as_probe_url(prm->authorization_server, url, url_cap);

    /* Fetch the AS discovery document (host-supplied callback) */
    char *body = NULL;
    size_t body_len = 0;
    int rc = fetch(fetch_ud, url, &body, &body_len);
    alloc->free_fn(url, alloc->userdata);
    if (rc != 0 || body == NULL) {
        if (body != NULL) {
            alloc->free_fn(body, alloc->userdata);
        }
        return MCP_ERR_PROTOCOL;
    }

    /* Parse the response as JSON */
    mcp_json_value_t *root = mcp_json_parse(ctx, body, body_len);
    alloc->free_fn(body, alloc->userdata);
    if (root == NULL) {
        return MCP_ERR_PROTOCOL;
    }

    /* Parse into AS metadata (POD struct; no heap allocation needed) */
    mcp_oauth_metadata_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    mcp_status_t st = mcp_oauth_metadata_parse(ctx, root, &parsed);
    mcp_json_destroy(ctx, root);
    if (st != MCP_OK) {
        return st;
    }

    if (existing_as != NULL) {
        /* Merge: parsed AS wins, existing_as fills gaps (P3 T5 merge semantics) */
        return mcp_oauth_discovery_merge(ctx, &parsed, existing_as, out_merged);
    }
    *out_merged = parsed;
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

mcp_status_t mcp_oauth_gate_issuer(mcp_context_t *ctx,
                                   const mcp_oauth_token_response_t *resp,
                                   const char *expected_issuer,
                                   bool enforce_must) {
    (void)ctx;
    if (resp == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (expected_issuer == NULL || resp->iss == NULL) {
        return MCP_OK;
    }
    if (strcmp(resp->iss, expected_issuer) == 0) {
        return MCP_OK;
    }
    if (enforce_must) {
        return MCP_ERR_PROTOCOL;
    }
    return MCP_OK;
}
