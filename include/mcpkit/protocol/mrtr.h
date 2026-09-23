/**
 * @file mrtr.h
 * @brief Multi Round-Trip Requests (MRTR) protocol support.
 *
 * MRTR (MCP spec 2026-07-28) allows a tool handler to pause mid-execution
 * and request additional input from the client (elicitation). The client
 * collects the input and re-issues the tool call with `inputResponses` and
 * `requestState` so the handler can resume.
 *
 * ## Server side
 *
 * Implement `mcp_tool_handler_v2_fn` instead of `mcp_tool_handler_fn`. When
 * the handler needs user input it calls `mcp_mrtr_result_input_required_new`
 * to build an `InputRequiredResult` and returns it via `*result_out` with
 * status `MCP_OK`.  The dispatcher detects `resultType == "input_required"`
 * and returns the result to the client; the client re-issues the request with
 * the user-supplied `inputResponses` and the opaque `requestState` blob.
 *
 * ## Client side
 *
 * Call `mcp_client_call_tool_mrtr` instead of `mcp_client_call_tool`. It
 * drives the retry loop automatically, invoking the registered elicitation
 * handler (set via `mcp_client_set_mrtr_elicit_handler`) to collect user
 * input whenever the server returns `resultType == "input_required"`.
 *
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_MRTR_H
#define MCPKIT_PROTOCOL_MRTR_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;

/* -------------------------------------------------------------------------
 * Result-type tokens
 * ---------------------------------------------------------------------- */

/** @brief Value of the `resultType` field in a complete tool result. */
#define MCP_MRTR_RESULT_TYPE_COMPLETE       "complete"

/** @brief Value of the `resultType` field when the server needs more input. */
#define MCP_MRTR_RESULT_TYPE_INPUT_REQUIRED "input_required"

/* -------------------------------------------------------------------------
 * Elicitation action (client → server on the retry call)
 * ---------------------------------------------------------------------- */

/**
 * @brief Action the user took in response to an elicitation request.
 *
 * Passed back to the server inside `inputResponses[].action`.
 */
typedef enum mcp_elicit_action {
    /** User filled in the form and clicked Accept. */
    MCP_ELICIT_ACCEPT = 0,
    /** User declined to fill in the form. */
    MCP_ELICIT_REJECT = 1,
    /** User cancelled the entire tool call. */
    MCP_ELICIT_CANCEL = 2,
} mcp_elicit_action_t;

/* -------------------------------------------------------------------------
 * Server-side helpers: building InputRequiredResult
 * ---------------------------------------------------------------------- */

/**
 * @brief Build one elicitation-request entry for inclusion in
 *        `inputRequests`.
 *
 * @param ctx   Context; may be NULL.
 * @param message   Human-readable prompt shown to the user; strdup'd.
 * @param mode      Elicitation mode string (e.g. "default"); strdup'd.
 *                  May be NULL (field is omitted).
 * @param schema    Optional JSON Schema object for the expected response;
 *                  TAKEN on success (do NOT destroy separately).
 *                  May be NULL.
 * @return Owned JSON object `{ message, [mode,] [requestedSchema] }` on
 *         success, or NULL on OOM. On OOM `schema` is destroyed.
 */
mcp_json_value_t *mcp_mrtr_elicit_request_new(mcp_context_t *ctx,
                                               const char *message,
                                               const char *mode,
                                               mcp_json_value_t *schema);

/**
 * @brief Build an `InputRequiredResult` JSON object.
 *
 * Produces:
 * ```json
 * {
 *   "resultType": "input_required",
 *   "inputRequests": [ ... ],
 *   "requestState": "<opaque string>"
 * }
 * ```
 *
 * @param ctx            Context; may be NULL.
 * @param input_requests Owned JSON array or object of elicitation-request entries;
 *                       TAKEN on success. On OOM it is destroyed.
 *                       May be NULL if request_state is non-NULL.
 * @param request_state  Opaque server state string; strdup'd internally;
 *                       caller still owns the original. May be NULL if
 *                       input_requests is non-NULL.
 * @return Owned JSON object ready to be stored in `*result_out`, or NULL
 *         on OOM or if BOTH input_requests and request_state are NULL.
 */
mcp_json_value_t *mcp_mrtr_result_input_required_new(mcp_context_t *ctx,
                                                      mcp_json_value_t *input_requests,
                                                      const char *request_state);

/* -------------------------------------------------------------------------
 * Result introspection helpers (used by dispatcher and client)
 * ---------------------------------------------------------------------- */

/**
 * @brief Return true when `result` is an `InputRequiredResult`.
 *
 * Inspects `result["resultType"] == "input_required"`. Returns false
 * if `result` is NULL or not a JSON object.
 *
 * @param ctx    Context; may be NULL.
 * @param result Borrowed JSON value.
 */
bool mcp_mrtr_is_input_required(mcp_context_t *ctx, const mcp_json_value_t *result);

/**
 * @brief Extract `requestState` from an `InputRequiredResult`.
 *
 * @param ctx    Context; may be NULL.
 * @param result Borrowed JSON object (must be an InputRequiredResult).
 * @return Borrowed string pointer (valid while `result` is alive), or
 *         NULL if the field is absent.
 */
const char *mcp_mrtr_get_request_state(mcp_context_t *ctx,
                                        const mcp_json_value_t *result);

/**
 * @brief Extract `inputRequests` from an `InputRequiredResult`.
 *
 * @param ctx    Context; may be NULL.
 * @param result Borrowed JSON object.
 * @return Borrowed JSON array, or NULL if absent.
 */
const mcp_json_value_t *mcp_mrtr_get_input_requests(mcp_context_t *ctx,
                                                      const mcp_json_value_t *result);

/* -------------------------------------------------------------------------
 * Client-side: elicitation response builder
 * ---------------------------------------------------------------------- */

/**
 * @brief Build one `inputResponses` entry from a user action + data.
 *
 * @param ctx    Context; may be NULL.
 * @param action User's action (ACCEPT / REJECT / CANCEL).
 * @param data   Owned JSON object with the user-supplied field values;
 *               TAKEN on success. May be NULL (e.g. for REJECT/CANCEL).
 * @return Owned JSON object `{ action: "...", [content: {...}] }`, or
 *         NULL on OOM.
 */
mcp_json_value_t *mcp_mrtr_input_response_new(mcp_context_t *ctx,
                                               mcp_elicit_action_t action,
                                               mcp_json_value_t *data);

#endif /* MCPKIT_PROTOCOL_MRTR_H */
