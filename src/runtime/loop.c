/**
 * @file loop.c
 *
 * Single-transport serve loop. Creates one session for the lifetime
 * of the loop. When an executor is supplied, each request is submitted
 * and waited on synchronously (session-safe, not truly concurrent);
 * when NULL, dispatch happens inline on the calling thread. The timer
 * (if any) is polled before every recv. The caller owns and manages
 * the transport's start/stop/destroy lifecycle — mcp_loop_run does
 * not call them.
 */
#include <string.h>

#include "mcpkit/core/error.h"
#include "mcpkit/core/shutdown.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/value.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/runtime/executor.h"
#include "mcpkit/runtime/loop.h"
#include "mcpkit/runtime/timer.h"
#include "mcpkit/server/server.h"
#include "mcpkit/transport/transport.h"

#include "internals.h"

typedef struct {
    mcp_server_t *server;
    mcp_session_t *sess;
    mcp_message_t *msg;
    mcp_message_t *resp;
    mcp_status_t status;
} dispatch_job_t;

static void dispatch_task(mcp_context_t *ctx, void *arg) {
    dispatch_job_t *job = (dispatch_job_t *)arg;
    job->status = mcp_server_dispatch(ctx, job->server, job->sess, job->msg, &job->resp);
}

static mcp_status_t send_error(mcp_context_t *ctx, mcp_transport_t *t, int code,
                               const char *text) {
    mcp_message_t *err = mcp_response_err_new(ctx, NULL, code, text, NULL);
    if (err == NULL) {
        return MCP_ERR_NOMEM;
    }
    char *out = mcp_message_serialize(ctx, err);
    mcp_message_destroy(ctx, err);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_transport_send(ctx, t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

static mcp_status_t send_response(mcp_context_t *ctx, mcp_transport_t *t,
                                  mcp_message_t *resp) {
    char *out = mcp_message_serialize(ctx, resp);
    mcp_message_destroy(ctx, resp);
    if (out == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t st = mcp_transport_send(ctx, t, out, strlen(out));
    mcp_json_free_string(ctx, out);
    return st;
}

static mcp_status_t dispatch_inline(mcp_context_t *ctx, mcp_server_t *server,
                                    mcp_session_t *sess, mcp_message_t *msg,
                                    mcp_executor_t *ex, mcp_message_t **resp_out) {
    if (ex == NULL) {
        return mcp_server_dispatch(ctx, server, sess, msg, resp_out);
    }
    dispatch_job_t job = {
        .server = server,
        .sess = sess,
        .msg = msg,
        .resp = NULL,
        .status = MCP_OK,
    };
    mcp_status_t st = mcp_executor_submit(ctx, ex, dispatch_task, &job);
    if (st != MCP_OK) {
        return st;
    }
    st = mcp_executor_wait(ctx, ex);
    if (st != MCP_OK) {
        return st;
    }
    *resp_out = job.resp;
    return job.status;
}

mcp_status_t mcp_loop_run(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t,
                          mcp_executor_t *ex_or_null, mcp_timer_t *timer_or_null) {
    if (ctx == NULL || server == NULL || t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    // A stale flag from a previous run must not kill a fresh loop.
    mcp_shutdown_clear();
    mcp_session_t *sess = mcp_server_create_session(ctx, server);
    if (sess == NULL) {
        return MCP_ERR_NOMEM;
    }
    mcp_status_t status = MCP_ERR_NOT_FOUND;
    for (;;) {
        // Shutdown requested while idle or during the previous
        // dispatch: stop before taking new work. The in-flight
        // request (if any) already ran to completion above.
        if (mcp_shutdown_requested()) {
            status = MCP_ERR_CANCELLED;
            break;
        }
        if (timer_or_null != NULL) {
            size_t fired = 0;
            if (mcp_timer_poll(ctx, timer_or_null, &fired) != MCP_OK) {
                status = MCP_ERR_NOMEM;
                break;
            }
        }
        char *line = NULL;
        mcp_status_t st = mcp_transport_recv(ctx, t, &line);
        if (st == MCP_ERR_IO) {
            break;
        }
        // A recv timeout is an idle wakeup: exit only if shutdown was
        // requested, otherwise keep the pre-existing break semantics.
        if (st == MCP_ERR_TIMEOUT) {
            status = mcp_shutdown_requested() ? MCP_ERR_CANCELLED : st;
            break;
        }
        if (st != MCP_OK) {
            status = st;
            break;
        }
        mcp_message_t *msg = mcp_message_parse(ctx, line, strlen(line));
        mcp_json_free_string(ctx, line);
        if (msg == NULL) {
            if (send_error(ctx, t, MCP_RPC_PARSE_ERROR, "Parse error") != MCP_OK) {
                status = MCP_ERR_NOMEM;
                break;
            }
            continue;
        }
        mcp_msg_kind_t kind = mcp_message_kind(ctx, msg);
        if (kind == MCP_MSG_NOTIFICATION) {
            mcp_server_notify(ctx, server, sess, msg);
            mcp_message_destroy(ctx, msg);
            continue;
        }
        if (kind != MCP_MSG_REQUEST) {
            mcp_message_destroy(ctx, msg);
            continue;
        }
        mcp_message_t *resp = NULL;
        st = dispatch_inline(ctx, server, sess, msg, ex_or_null, &resp);
        mcp_message_destroy(ctx, msg);
        if (st != MCP_OK) {
            status = st;
            break;
        }
        if (resp != NULL) {
            st = send_response(ctx, t, resp);
            if (st != MCP_OK) {
                status = st;
                break;
            }
        }
    }
    mcp_server_destroy_session(ctx, server, sess);
    return status;
}
