/**
 * @file auth_token.c
 * @brief OAuth 2.0 / 2.1 token response parsing, request builders, and RFC 8693 token exchange.
 */

#include "internal.h"

void mcp_oauth_free_string(mcp_context_t *ctx, char *str) {
    mcp_free(ctx, str);
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

    const mcp_json_value_t *v_iss = mcp_json_object_get(ctx, root, "iss");
    if (v_iss != NULL && mcp_json_type(ctx, v_iss) == MCP_JSON_STRING) {
        const char *s_iss = NULL;
        mcp_json_string_value(ctx, v_iss, &s_iss);
        resp_out->iss = auth_strdup(ctx, s_iss);
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
    mcp_oauth_free_string(ctx, resp->iss);
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
