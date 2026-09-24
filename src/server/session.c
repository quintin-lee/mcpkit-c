/**
 * @file session.c
 *
 * Session allocation and per-session duplicate-id tracking. session_new
 * zero-clears the struct then sets granted = 0xFFFFFFFFu (ALL granted)
 * as the default permission mask; memset alone would leave it NONE.
 */
#include "mcpkit/server/session.h"

#include <string.h>

#include "internals.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/server/server.h"

static mcp_session_t *session_new(mcp_context_t *ctx) {
    mcp_session_t *s = srv_malloc(ctx, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->granted = 0xFFFFFFFFu;
    s->ids = mcp_idset_create(ctx);
    if (s->ids == NULL) {
        srv_free(ctx, s);
        return NULL;
    }
    return s;
}

void session_free(mcp_context_t *ctx, mcp_session_t *s) {
    if (s == NULL) {
        return;
    }
    mcp_idset_destroy(ctx, s->ids);
    mcp_json_destroy(ctx, s->client_meta);
    srv_free(ctx, s->client_name);
    srv_free(ctx, s->client_version);
    srv_free(ctx, s->sub_id_str);
    if (s->sub_resource_uris != NULL) {
        for (size_t i = 0; i < s->n_sub_resource_uris; i++) {
            srv_free(ctx, s->sub_resource_uris[i]);
        }
        srv_free(ctx, s->sub_resource_uris);
    }
    srv_free(ctx, s);
}

bool mcp_session_is_initialized(mcp_context_t *ctx, const mcp_session_t *session) {
    (void)ctx;
    return session != NULL && session->initialized;
}

mcp_status_t mcp_session_set_apps_host(mcp_context_t *ctx, mcp_session_t *session,
                                       bool apps_host) {
    if (ctx == NULL || session == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    session->apps_host = apps_host;
    return MCP_OK;
}

bool mcp_session_is_apps_host(mcp_context_t *ctx, const mcp_session_t *session) {
    (void)ctx;
    return session != NULL && session->apps_host;
}

mcp_status_t mcp_session_grant(mcp_context_t *ctx, mcp_session_t *session, uint32_t perm_mask) {
    if (ctx == NULL || session == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    session->granted |= perm_mask;
    return MCP_OK;
}

mcp_status_t mcp_session_revoke(mcp_context_t *ctx, mcp_session_t *session, uint32_t perm_mask) {
    if (ctx == NULL || session == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    session->granted &= ~perm_mask;
    return MCP_OK;
}

bool mcp_session_grants(mcp_context_t *ctx, const mcp_session_t *session, uint32_t perm_mask) {
    (void)ctx;
    return session != NULL && (session->granted & perm_mask) == perm_mask;
}

const mcp_json_value_t *mcp_session_client_meta(mcp_context_t *ctx,
                                                 const mcp_session_t *session) {
    (void)ctx;
    if (session == NULL) {
        return NULL;
    }
    return session->client_meta;
}

mcp_session_t *mcp_server_create_session(mcp_context_t *ctx, mcp_server_t *srv) {
    if (srv == NULL) {
        return NULL;
    }
    mcp_session_t *s = session_new(ctx);
    if (s == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&srv->sessions_lock);
    if (srv->n_sessions == srv->cap_sessions) {
        size_t ncap = srv->cap_sessions == 0 ? 4 : srv->cap_sessions * 2;
        mcp_session_t **narr = srv_realloc(ctx, srv->sessions, ncap * sizeof(*narr));
        if (narr == NULL) {
            pthread_mutex_unlock(&srv->sessions_lock);
            session_free(ctx, s);
            return NULL;
        }
        srv->sessions = narr;
        srv->cap_sessions = ncap;
    }
    srv->sessions[srv->n_sessions++] = s;
    pthread_mutex_unlock(&srv->sessions_lock);
    return s;
}

void mcp_server_destroy_session(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *session) {
    if (srv == NULL || session == NULL) {
        return;
    }
    pthread_mutex_lock(&srv->sessions_lock);
    for (size_t i = 0; i < srv->n_sessions; i++) {
        if (srv->sessions[i] == session) {
            srv->sessions[i] = srv->sessions[--srv->n_sessions];
            pthread_mutex_unlock(&srv->sessions_lock);
            session_free(ctx, session);
            return;
        }
    }
    pthread_mutex_unlock(&srv->sessions_lock);
}

bool mcp_session_has_active_subscription(mcp_context_t *ctx, const mcp_session_t *session) {
    (void)ctx;
    return session != NULL && session->subscription_active;
}

bool mcp_session_is_subscribed_to_notification(mcp_context_t *ctx, const mcp_session_t *session,
                                              const char *method, const mcp_json_value_t *params) {
    (void)ctx;
    if (session == NULL || !session->subscription_active || method == NULL) {
        return true;
    }
    if (strcmp(method, "notifications/tools/list_changed") == 0) {
        return session->sub_tools_list_changed;
    }
    if (strcmp(method, "notifications/prompts/list_changed") == 0) {
        return session->sub_prompts_list_changed;
    }
    if (strcmp(method, "notifications/resources/list_changed") == 0) {
        return session->sub_resources_list_changed;
    }
    if (strcmp(method, "notifications/resources/updated") == 0) {
        if (params == NULL || session->sub_resource_uris == NULL || session->n_sub_resource_uris == 0) {
            return false;
        }
        const mcp_json_value_t *uri_val = mcp_json_object_get(ctx, params, "uri");
        const char *uri_str = NULL;
        if (uri_val == NULL || mcp_json_string_value(ctx, uri_val, &uri_str) != MCP_OK || uri_str == NULL) {
            return false;
        }
        for (size_t i = 0; i < session->n_sub_resource_uris; i++) {
            if (strcmp(session->sub_resource_uris[i], uri_str) == 0) {
                return true;
            }
        }
        return false;
    }
    return true;
}

mcp_status_t mcp_session_build_notification(mcp_context_t *ctx, const mcp_session_t *session,
                                            const char *method, mcp_json_value_t *params,
                                            mcp_message_t **notif_out) {
    if (method == NULL || notif_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *notif_out = NULL;
    if (session != NULL && session->subscription_active) {
        if (!mcp_session_is_subscribed_to_notification(ctx, session, method, params)) {
            if (params != NULL) {
                mcp_json_destroy(ctx, params);
            }
            return MCP_OK;
        }
        if (params == NULL) {
            params = mcp_json_object_new(ctx);
            if (params == NULL) {
                return MCP_ERR_NOMEM;
            }
        }
        mcp_json_value_t *meta = (mcp_json_value_t *)mcp_json_object_get(ctx, params, "_meta");
        if (meta == NULL) {
            meta = mcp_json_object_new(ctx);
            if (meta == NULL || mcp_json_object_set_take(ctx, params, "_meta", meta) != MCP_OK) {
                mcp_json_destroy(ctx, meta);
                mcp_json_destroy(ctx, params);
                return MCP_ERR_NOMEM;
            }
        }
        mcp_json_value_t *sub_id_v = NULL;
        if (session->sub_id_type == MCP_ID_STRING) {
            sub_id_v = mcp_json_string_new(ctx, session->sub_id_str ? session->sub_id_str : "");
        } else if (session->sub_id_type == MCP_ID_NUMBER) {
            sub_id_v = mcp_json_number_new(ctx, session->sub_id_num);
        } else {
            sub_id_v = mcp_json_null_new(ctx);
        }
        if (sub_id_v == NULL ||
            mcp_json_object_set_take(ctx, meta, "io.modelcontextprotocol/subscriptionId", sub_id_v) != MCP_OK) {
            mcp_json_destroy(ctx, sub_id_v);
            mcp_json_destroy(ctx, params);
            return MCP_ERR_NOMEM;
        }
    }
    mcp_message_t *msg = mcp_notification_new(ctx, method, params);
    if (msg == NULL) {
        return MCP_ERR_NOMEM;
    }
    *notif_out = msg;
    return MCP_OK;
}

mcp_status_t mcp_session_build_subscription_closure(mcp_context_t *ctx, mcp_session_t *session,
                                                    mcp_message_t **resp_out) {
    if (session == NULL || !session->subscription_active || resp_out == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    *resp_out = NULL;
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *meta = mcp_json_object_new(ctx);
    mcp_json_value_t *rt = mcp_json_string_new(ctx, "complete");
    if (result == NULL || meta == NULL || rt == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, meta);
        mcp_json_destroy(ctx, rt);
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, result, "resultType", rt) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, meta);
        return MCP_ERR_NOMEM;
    }
    mcp_json_value_t *sub_id_v = NULL;
    if (session->sub_id_type == MCP_ID_STRING) {
        sub_id_v = mcp_json_string_new(ctx, session->sub_id_str ? session->sub_id_str : "");
    } else if (session->sub_id_type == MCP_ID_NUMBER) {
        sub_id_v = mcp_json_number_new(ctx, session->sub_id_num);
    } else {
        sub_id_v = mcp_json_null_new(ctx);
    }
    if (sub_id_v == NULL ||
        mcp_json_object_set_take(ctx, meta, "io.modelcontextprotocol/subscriptionId", sub_id_v) != MCP_OK ||
        mcp_json_object_set_take(ctx, result, "_meta", meta) != MCP_OK) {
        mcp_json_destroy(ctx, sub_id_v);
        mcp_json_destroy(ctx, meta);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }

    mcp_message_t *resp = NULL;
    if (session->sub_id_type == MCP_ID_STRING) {
        resp = mcp_response_ok_string_id_new(ctx, session->sub_id_str ? session->sub_id_str : "", result);
    } else if (session->sub_id_type == MCP_ID_NUMBER) {
        resp = mcp_response_ok_number_id_new(ctx, session->sub_id_num, result);
    } else {
        resp = mcp_response_ok_string_id_new(ctx, "", result);
    }
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }

    session->subscription_active = false;
    if (session->sub_id_str != NULL) {
        srv_free(ctx, session->sub_id_str);
        session->sub_id_str = NULL;
    }
    if (session->sub_resource_uris != NULL) {
        for (size_t i = 0; i < session->n_sub_resource_uris; i++) {
            srv_free(ctx, session->sub_resource_uris[i]);
        }
        srv_free(ctx, session->sub_resource_uris);
        session->sub_resource_uris = NULL;
        session->n_sub_resource_uris = 0;
    }
    session->sub_tools_list_changed = false;
    session->sub_prompts_list_changed = false;
    session->sub_resources_list_changed = false;

    *resp_out = resp;
    return MCP_OK;
}
