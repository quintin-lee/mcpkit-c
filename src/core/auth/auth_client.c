/**
 * @file auth_client.c
 * @brief Client ID Metadata Documents (SEP-991) and RFC 7591 DCR deprecation.
 */

#include "internal.h"

mcp_status_t mcp_oauth_build_dcr_deprecation_headers(mcp_context_t *ctx,
                                                     char **deprecation_out,
                                                     char **sunset_out) {
    if (deprecation_out == NULL || sunset_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *deprecation_out = NULL;
    *sunset_out = NULL;

    const mcp_allocator_t *a = alloc_of(ctx);
    *deprecation_out = a->malloc_fn(5, a->userdata);
    if (*deprecation_out == NULL) {
        return MCP_ERR_NOMEM;
    }
    memcpy(*deprecation_out, "true", 5);

    time_t now = time(NULL);
    time_t sunset_epoch = now + MCP_OAUTH_DCR_DEPRECATION_WINDOW_DAYS * 86400;
    struct tm tm_buf;
    if (gmtime_r(&sunset_epoch, &tm_buf) == NULL) {
        mcp_oauth_free_string(ctx, *deprecation_out);
        *deprecation_out = NULL;
        return MCP_ERR_NOMEM;
    }
    /* RFC 1123 / HTTP-date (RFC 9110 § 5.6.7): "Www, DD Mon YYYY HH:MM:SS GMT" */
    static const char *const k_days[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const k_months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (tm_buf.tm_wday < 0 || tm_buf.tm_wday > 6 ||
        tm_buf.tm_mon < 0 || tm_buf.tm_mon > 11) {
        mcp_oauth_free_string(ctx, *deprecation_out);
        *deprecation_out = NULL;
        return MCP_ERR_NOMEM;
    }
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%s, %02d %s %d %02d:%02d:%02d GMT",
                     k_days[tm_buf.tm_wday], tm_buf.tm_mday, k_months[tm_buf.tm_mon],
                     tm_buf.tm_year + 1900, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        mcp_oauth_free_string(ctx, *deprecation_out);
        *deprecation_out = NULL;
        return MCP_ERR_NOMEM;
    }
    size_t len = (size_t)n + 1;
    *sunset_out = a->malloc_fn(len, a->userdata);
    if (*sunset_out == NULL) {
        mcp_oauth_free_string(ctx, *deprecation_out);
        *deprecation_out = NULL;
        return MCP_ERR_NOMEM;
    }
    memcpy(*sunset_out, tmp, len);
    return MCP_OK;
}

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
