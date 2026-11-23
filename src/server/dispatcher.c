/**
 * @file dispatcher.c
 *
 * Request dispatch pipeline: validate (L1/L2/L3) -> session gate ->
 * per-session duplicate-id check -> route delegation -> tracing.
 */
#define _DEFAULT_SOURCE

#include <stdbool.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "internals.h"
#include "../method_table.h"
#include "mcpkit/core/context.h"
#include "mcpkit/json/object.h"
#include "mcpkit/logging/logger.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/validate.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"

static mcp_message_t *err_resp(mcp_context_t *ctx, const mcp_message_t *req, int code,
                               const char *text) {
    return mcp_response_err_new(ctx, req, code, text, NULL);
}

void dlogf_srv(mcp_context_t *ctx, mcp_server_t *srv, mcp_log_level_t level,
               const char *fmt, ...) {
    if (level < (mcp_log_level_t)atomic_load(&srv->log_floor)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    mcp_logger_t *lg = mcp_context_logger(ctx);
    if (lg == NULL) {
        va_end(ap);
        return;
    }
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mcp_logger_log(lg, level, buf);
}

static uint64_t trace_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void trace_begin(mcp_context_t *ctx, mcp_server_t *srv, const char *method) {
    if (srv->tracer != NULL) {
        srv->tracer(ctx, MCP_TRACE_BEGIN, method, 0, 0, srv->tracer_ud);
    }
}

static void trace_end(mcp_context_t *ctx, mcp_server_t *srv, const char *method,
                      uint64_t t0, int status) {
    if (srv->tracer != NULL) {
        srv->tracer(ctx, MCP_TRACE_END, method, status, trace_now() - t0, srv->tracer_ud);
    }
}

mcp_status_t mcp_server_dispatch(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *session,
                                 const mcp_message_t *req, mcp_message_t **resp_out) {
    if (srv == NULL || session == NULL || req == NULL || resp_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    *resp_out = NULL;
    if (mcp_message_kind(ctx, req) == MCP_MSG_RESPONSE) {
        return mcp_server_dispatch_response(ctx, srv, session, req);
    }
    if (mcp_message_kind(ctx, req) != MCP_MSG_REQUEST) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    atomic_fetch_add(&srv->c_requests_total, 1);
    int code = 0;
    if (mcp_message_validate(ctx, req, &code) != MCP_OK) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=invalid_request code=%d", code);
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, code, "invalid request");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    const char *method = mcp_message_method(ctx, req);
    if (method == NULL) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=missing_method");
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "missing method");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    const mcp_json_value_t *req_meta = mcp_message_meta(ctx, req);
    if (req_meta != NULL) {
        const mcp_json_value_t *lv = mcp_json_object_get(ctx, req_meta, "io.modelcontextprotocol/logLevel");
        if (lv == NULL) {
            lv = mcp_json_object_get(ctx, req_meta, "logLevel");
        }
        const char *lvl = NULL;
        if (lv != NULL && mcp_json_string_value(ctx, lv, &lvl) == MCP_OK && lvl != NULL) {
            if (strcmp(lvl, "debug") == 0) {
                atomic_store(&srv->log_floor, (int)MCP_LOG_DEBUG);
            } else if (strcmp(lvl, "info") == 0 || strcmp(lvl, "notice") == 0) {
                atomic_store(&srv->log_floor, (int)MCP_LOG_INFO);
            } else if (strcmp(lvl, "warning") == 0 || strcmp(lvl, "warn") == 0) {
                atomic_store(&srv->log_floor, (int)MCP_LOG_WARN);
            } else if (strcmp(lvl, "error") == 0) {
                atomic_store(&srv->log_floor, (int)MCP_LOG_ERROR);
            }
        }
    }
    bool is_stateless = (req_meta != NULL) || (session->negotiated == MCP_PROTO_2026_07_28);
    bool is_discover = (strcmp(method, "server/discover") == 0);
    if (!session->initialized && strcmp(method, "initialize") != 0 && !is_discover && !is_stateless) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=uninitialized method=%s", method);
        atomic_fetch_add(&srv->c_requests_error, 1);
        *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "session not initialized");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    mcp_context_t *id_ctx = (session != NULL && session->ctx != NULL) ? session->ctx : ctx;
    mcp_id_type_t id_type = mcp_message_id_type(ctx, req);
    const char *id_str = NULL;
    double id_num = 0;
    if (id_type != MCP_ID_NONE) {
        id_str = mcp_message_id_string(ctx, req);
        mcp_message_id_number(ctx, req, &id_num);
        if (mcp_idset_contains(id_ctx, session->ids, id_type, id_str, id_num)) {

            dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=duplicate_id method=%s", method);
            atomic_fetch_add(&srv->c_requests_error, 1);
            *resp_out = err_resp(ctx, req, MCP_RPC_INVALID_REQUEST, "duplicate request id");
            return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
        }
        if (mcp_idset_add(id_ctx, session->ids, id_type, id_str, id_num) != MCP_OK) {
            dlogf_srv(ctx, srv, MCP_LOG_ERROR, "event=id_tracking_failed method=%s", method);
            atomic_fetch_add(&srv->c_requests_error, 1);
            *resp_out = err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "id tracking failed");
            return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
        }
    }
    mcp_in_flight_req_t *in_flight = NULL;
    if (id_type != MCP_ID_NONE) {
        in_flight = srv_malloc(ctx, sizeof(*in_flight));
        if (in_flight != NULL) {
            memset(in_flight, 0, sizeof(*in_flight));
            in_flight->id_type = id_type;
            if (id_type == MCP_ID_STRING && id_str != NULL) {
                in_flight->id_str = srv_strdup(ctx, id_str);
            }
            in_flight->id_num = id_num;
            atomic_init(&in_flight->is_cancelled, false);

            pthread_mutex_lock(&session->active_req_lock);
            in_flight->next = session->active_requests;
            session->active_requests = in_flight;
            pthread_mutex_unlock(&session->active_req_lock);
        }
    }
    uint64_t t0 = srv->tracer != NULL ? trace_now() : 0;
    trace_begin(ctx, srv, method);
    mcp_message_t *resp = mcp_server_route_request(ctx, srv, session, req, method);
    if (in_flight != NULL) {
        pthread_mutex_lock(&session->active_req_lock);
        mcp_in_flight_req_t **pp = &session->active_requests;
        while (*pp != NULL) {
            if (*pp == in_flight) {
                *pp = in_flight->next;
                break;
            }
            pp = &(*pp)->next;
        }
        pthread_mutex_unlock(&session->active_req_lock);
        srv_free(ctx, in_flight->id_str);
        srv_free(ctx, in_flight->cancel_reason);
        srv_free(ctx, in_flight);
    }
    if (resp == NULL) {
        dlogf_srv(ctx, srv, MCP_LOG_ERROR, "event=route_failed method=%s", method);
        atomic_fetch_add(&srv->c_requests_error, 1);
        trace_end(ctx, srv, method, t0, MCP_RPC_INTERNAL_ERROR);
        *resp_out = err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "internal error");
        return *resp_out == NULL ? MCP_ERR_NOMEM : MCP_OK;
    }
    int ecode = 0;
    int end_status = mcp_message_error_code(ctx, resp, &ecode) == MCP_OK ? ecode : 0;
    if (end_status != 0) {
        atomic_fetch_add(&srv->c_requests_error, 1);
    }
    trace_end(ctx, srv, method, t0, end_status);
    *resp_out = resp;
    return MCP_OK;
}

mcp_status_t mcp_server_notify(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *session,
                               const mcp_message_t *notif) {
    if (srv == NULL || session == NULL || notif == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_message_kind(ctx, notif) != MCP_MSG_NOTIFICATION) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const char *method = mcp_message_method(ctx, notif);
    if (method == NULL) {
        return MCP_OK;
    }
    if (strcmp(method, "notifications/initialized") == 0) {
        session->initialized = true;
    } else if (strcmp(method, "notifications/cancelled") == 0) {
        const mcp_json_value_t *params = mcp_message_params(ctx, notif);
        if (params != NULL) {
            const mcp_json_value_t *req_id_val = mcp_json_object_get(ctx, params, "requestId");
            const char *reason = NULL;
            const mcp_json_value_t *reason_val = mcp_json_object_get(ctx, params, "reason");
            if (reason_val != NULL && mcp_json_type(ctx, reason_val) == MCP_JSON_STRING) {
                mcp_json_string_value(ctx, reason_val, &reason);
            }
            if (req_id_val != NULL) {
                mcp_id_type_t cid_type = MCP_ID_NONE;
                const char *cid_str = NULL;
                double cid_num = 0;
                if (mcp_json_type(ctx, req_id_val) == MCP_JSON_STRING) {
                    cid_type = MCP_ID_STRING;
                    mcp_json_string_value(ctx, req_id_val, &cid_str);
                } else if (mcp_json_type(ctx, req_id_val) == MCP_JSON_NUMBER) {
                    cid_type = MCP_ID_NUMBER;
                    mcp_json_number_value(ctx, req_id_val, &cid_num);
                }
                if (cid_type != MCP_ID_NONE) {
                    mcp_session_cancel_in_flight(session, cid_type, cid_str, cid_num, reason);
                }
            } else {
                session->subscription_active = false;
            }
        } else {
            session->subscription_active = false;
        }
    }

    bool advanced = false;
    for (int i = 0; i < (int)MCP_SERVER_NOTIFICATION_COUNT; i++) {
        if (strcmp(method, k_mcp_server_notifications[i]) == 0) {
            advanced = true;
            break;
        }
    }
    if (advanced) {
        dlogf_srv(ctx, srv, MCP_LOG_DEBUG, "event=notification method=%s", method);
    }
    atomic_fetch_add(&srv->c_notifications_total, 1);
    return MCP_OK;
}
