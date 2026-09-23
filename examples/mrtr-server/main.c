/**
 * @file main.c
 * @brief Multi Round-Trip Requests (MRTR) example server.
 *
 * Demonstrates a V2 tool handler (`transfer_funds`) that pauses execution
 * to elicit two-factor authentication (2FA) confirmation from the client
 * before completing a high-value transaction.
 *
 * Usage:
 *   build/examples/mrtr-server
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/protocol/mrtr.h"

/* Server HMAC secret key for tamper-proofing requestState tokens */
static const uint8_t k_server_secret[] = "mrtr-server-secret-key-2026-demo";

static mcp_status_t transfer_handler(mcp_context_t *ctx, mcp_session_t *session,
                                     const mcp_tool_call_ctx_t *call_ctx, void *user_data,
                                     mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;

    /* If this is the initial invocation (no request_state), check arguments and
     * ask for 2FA confirmation via an InputRequiredResult. */
    if (call_ctx->request_state == NULL) {
        double amount = 0.0;
        const char *to = "unknown";
        if (call_ctx->args != NULL) {
            const mcp_json_value_t *av = mcp_json_object_get(ctx, call_ctx->args, "amount");
            if (av != NULL) {
                mcp_json_number_value(ctx, av, &amount);
            }
            const mcp_json_value_t *tv = mcp_json_object_get(ctx, call_ctx->args, "to");
            if (tv != NULL) {
                mcp_json_string_value(ctx, tv, &to);
            }
        }

        /* Build the elicitation request schema for the expected 2FA code */
        mcp_json_value_t *code_schema = mcp_schema_object_new(ctx);
        if (code_schema != NULL) {
            mcp_schema_add_property(ctx, code_schema, "code", mcp_schema_string_new(ctx));
            mcp_schema_add_required(ctx, code_schema, "code");
        }

        char prompt[256];
        snprintf(prompt, sizeof(prompt),
                 "Confirm transfer of $%.2f to %s. Enter 6-digit 2FA code:", amount, to);

        mcp_json_value_t *req_item =
            mcp_mrtr_elicit_request_new(ctx, prompt, "modal", code_schema);
        if (req_item == NULL) {
            return MCP_ERR_NOMEM;
        }

        mcp_json_value_t *reqs_array = mcp_json_array_new(ctx);
        if (reqs_array == NULL || mcp_json_array_append(ctx, reqs_array, req_item) != MCP_OK) {
            mcp_json_destroy(ctx, req_item);
            mcp_json_destroy(ctx, reqs_array);
            return MCP_ERR_NOMEM;
        }

        /* Encode state token to resume transaction in the next hop.
         * We pack a JSON state object protected with HMAC-SHA256 and a 2-minute TTL (120000ms). */
        mcp_json_value_t *state_payload = mcp_json_object_new(ctx);
        if (state_payload == NULL) {
            mcp_json_destroy(ctx, reqs_array);
            return MCP_ERR_NOMEM;
        }
        mcp_json_object_set(ctx, state_payload, "to", mcp_json_string_new(ctx, to));
        mcp_json_object_set(ctx, state_payload, "amount", mcp_json_number_new(ctx, amount));

        char *state_token = NULL;
        mcp_status_t st = mcp_mrtr_state_pack(ctx, state_payload, k_server_secret,
                                              sizeof(k_server_secret) - 1, 120000, &state_token);
        mcp_json_destroy(ctx, state_payload);
        if (st != MCP_OK || state_token == NULL) {
            mcp_json_destroy(ctx, reqs_array);
            return MCP_ERR_NOMEM;
        }

        mcp_json_value_t *ir =
            mcp_mrtr_result_input_required_new(ctx, reqs_array, state_token);
        mcp_mrtr_state_free(ctx, state_token);
        if (ir == NULL) {
            return MCP_ERR_NOMEM;
        }

        *result_out = ir;
        return MCP_OK;
    }

    /* Subsequent hop: unpack and verify the tamper-proof requestState */
    mcp_json_value_t *resumed_state = NULL;
    mcp_status_t unpack_st = mcp_mrtr_state_unpack(ctx, call_ctx->request_state,
                                                   k_server_secret, sizeof(k_server_secret) - 1,
                                                   &resumed_state);
    if (unpack_st == MCP_ERR_TIMEOUT) {
        mcp_json_value_t *res = mcp_json_object_new(ctx);
        if (res == NULL) return MCP_ERR_NOMEM;
        mcp_json_object_set(ctx, res, "status", mcp_json_string_new(ctx, "error_session_expired"));
        *result_out = res;
        return MCP_OK;
    } else if (unpack_st == MCP_ERR_PERMISSION) {
        mcp_json_value_t *res = mcp_json_object_new(ctx);
        if (res == NULL) return MCP_ERR_NOMEM;
        mcp_json_object_set(ctx, res, "status", mcp_json_string_new(ctx, "error_state_tampered"));
        *result_out = res;
        return MCP_OK;
    } else if (unpack_st != MCP_OK || resumed_state == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    /* Inspect user response from input_responses */
    const char *action_str = NULL;
    const char *code_str = NULL;

    if (call_ctx->input_responses != NULL &&
        mcp_json_array_size(ctx, call_ctx->input_responses) > 0) {
        const mcp_json_value_t *resp =
            mcp_json_array_get(ctx, call_ctx->input_responses, 0);
        if (resp != NULL) {
            const mcp_json_value_t *act_val = mcp_json_object_get(ctx, resp, "action");
            if (act_val != NULL) {
                mcp_json_string_value(ctx, act_val, &action_str);
            }
            const mcp_json_value_t *content = mcp_json_object_get(ctx, resp, "content");
            if (content != NULL) {
                const mcp_json_value_t *code_val = mcp_json_object_get(ctx, content, "code");
                if (code_val != NULL) {
                    mcp_json_string_value(ctx, code_val, &code_str);
                }
            }
        }
    }

    /* Handle rejection or cancellation */
    if (action_str != NULL && strcmp(action_str, "accept") != 0) {
        mcp_json_destroy(ctx, resumed_state);
        mcp_json_value_t *res = mcp_json_object_new(ctx);
        if (res == NULL) return MCP_ERR_NOMEM;
        mcp_json_object_set(ctx, res, "status", mcp_json_string_new(ctx, "cancelled_by_user"));
        *result_out = res;
        return MCP_OK;
    }

    /* In a real server, verify 2FA code here */
    mcp_json_value_t *res = mcp_json_object_new(ctx);
    if (res == NULL) {
        mcp_json_destroy(ctx, resumed_state);
        return MCP_ERR_NOMEM;
    }
    mcp_json_object_set(ctx, res, "status", mcp_json_string_new(ctx, "transfer_complete"));
    mcp_json_object_set(ctx, res, "verified_code",
                        mcp_json_string_new(ctx, code_str ? code_str : ""));
    mcp_json_object_set(ctx, res, "state", resumed_state);

    *result_out = res;
    return MCP_OK;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    mcp_server_t *srv = mcp_server_create(ctx, "mrtr-server", "0.1.0");
    if (srv == NULL) {
        fprintf(stderr, "Failed to create server\n");
        mcp_context_destroy(ctx);
        return 1;
    }

    /* Input schema for initial transfer_funds call */
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    if (schema != NULL) {
        mcp_schema_add_property(ctx, schema, "to", mcp_schema_string_new(ctx));
        mcp_schema_add_property(ctx, schema, "amount", mcp_schema_number_new(ctx));
        mcp_schema_add_required(ctx, schema, "to");
        mcp_schema_add_required(ctx, schema, "amount");
    }

    /* Register V2 (MRTR-capable) tool */
    mcp_tool_t *tool = mcp_tool_new_v2(ctx, "transfer_funds",
                                       "Transfer funds with 2FA verification",
                                       schema, transfer_handler, NULL);
    if (tool == NULL || mcp_server_add_tool(ctx, srv, tool) != MCP_OK) {
        fprintf(stderr, "Failed to register tool\n");
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
        return 1;
    }

    /* Serve over stdio */
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, stdin, stdout);
    if (t == NULL) {
        fprintf(stderr, "Failed to create stdio transport\n");
        mcp_server_destroy(ctx, srv);
        mcp_context_destroy(ctx);
        return 1;
    }

    mcp_status_t st = mcp_stdio_serve(ctx, srv, t);
    mcp_transport_destroy(ctx, t);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);

    return st == MCP_OK ? 0 : 1;
}
