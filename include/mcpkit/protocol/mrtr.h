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
#include <stdint.h>

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

/* -------------------------------------------------------------------------
 * Server-side: requestState packing & verification (Phase 2)
 * ---------------------------------------------------------------------- */

/**
 * @brief Free a string or buffer allocated by mcp_mrtr_state_pack / unpack.
 *
 * @param ctx   Context; may be NULL.
 * @param state String or buffer to free; NULL-safe.
 */
void mcp_mrtr_state_free(mcp_context_t *ctx, char *state);

/**
 * @brief Pack arbitrary binary data into a tamper-proof, opaque `requestState` string
 *        with an explicit issue timestamp.
 *
 * Token format:
 *   `<base64url(payload)>.<issued_at_ms>.<ttl_ms>.<base64url(hmac)>`
 * or if key is NULL / key_len == 0:
 *   `<base64url(payload)>.<issued_at_ms>.<ttl_ms>.unsigned`
 *
 * The HMAC-SHA256 signature covers `<base64url(payload)>.<issued_at_ms>.<ttl_ms>`,
 * preventing tampering with payload, timestamp, or expiration window.
 *
 * @param ctx          Context; may be NULL.
 * @param data         Binary payload to pack; may be NULL if data_len == 0.
 * @param data_len     Length of data in bytes.
 * @param key          HMAC secret key; NULL or key_len==0 produces an unsigned token.
 * @param key_len      Length of HMAC key in bytes.
 * @param issued_at_ms Explicit issue timestamp in milliseconds (e.g. wall-clock epoch ms).
 * @param ttl_ms       Time-to-live in milliseconds (0 = no expiration).
 * @param state_out    Receives a newly allocated NUL-terminated token string.
 *                     Caller frees with mcp_mrtr_state_free() or mcp_json_free_string().
 * @return MCP_OK on success, MCP_ERR_NOMEM on OOM, MCP_ERR_INVALID_ARGUMENT on bad arguments.
 */
mcp_status_t mcp_mrtr_state_pack_raw_ex(mcp_context_t *ctx,
                                        const void *data,
                                        size_t data_len,
                                        const uint8_t *key,
                                        size_t key_len,
                                        uint64_t issued_at_ms,
                                        uint64_t ttl_ms,
                                        char **state_out);

/**
 * @brief Pack arbitrary binary data into a tamper-proof, opaque `requestState` string
 *        using the current wall-clock time as the issue timestamp.
 */
mcp_status_t mcp_mrtr_state_pack_raw(mcp_context_t *ctx,
                                     const void *data,
                                     size_t data_len,
                                     const uint8_t *key,
                                     size_t key_len,
                                     uint64_t ttl_ms,
                                     char **state_out);

/**
 * @brief Unpack and verify a raw binary `requestState` token with an explicit current time.
 *
 * @param ctx          Context; may be NULL.
 * @param state_str    Opaque token string received from client.
 * @param key          HMAC secret key; must match key used when packing.
 * @param key_len      Length of HMAC key in bytes.
 * @param now_ms       Explicit current timestamp in milliseconds for TTL comparison.
 * @param data_out     Receives newly allocated payload buffer; caller frees with mcp_mrtr_state_free().
 * @param data_len_out Receives length of unpacked data in bytes.
 * @return MCP_OK on success;
 *         MCP_ERR_TIMEOUT if ttl_ms has expired;
 *         MCP_ERR_PERMISSION if HMAC verification failed or signature presence mismatch;
 *         MCP_ERR_INVALID_ARGUMENT on malformed token syntax or clock skew;
 *         MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_mrtr_state_unpack_raw_ex(mcp_context_t *ctx,
                                          const char *state_str,
                                          const uint8_t *key,
                                          size_t key_len,
                                          uint64_t now_ms,
                                          void **data_out,
                                          size_t *data_len_out);

/**
 * @brief Unpack and verify a raw binary `requestState` token using current wall-clock time.
 */
mcp_status_t mcp_mrtr_state_unpack_raw(mcp_context_t *ctx,
                                       const char *state_str,
                                       const uint8_t *key,
                                       size_t key_len,
                                       void **data_out,
                                       size_t *data_len_out);

/**
 * @brief Pack a JSON value into a tamper-proof, opaque `requestState` string.
 *
 * Serializes `state` as JSON, then packs it via mcp_mrtr_state_pack_raw.
 *
 * @param ctx       Context; may be NULL.
 * @param state     Borrowed JSON value to serialize and pack.
 * @param key       HMAC secret key; NULL or key_len==0 produces an unsigned token.
 * @param key_len   Length of HMAC key in bytes.
 * @param ttl_ms    Time-to-live in milliseconds (0 = no expiration).
 * @param state_out Receives newly allocated NUL-terminated token string.
 *                  Caller frees with mcp_mrtr_state_free() or mcp_json_free_string().
 * @return MCP_OK on success, MCP_ERR_NOMEM on OOM, MCP_ERR_INVALID_ARGUMENT on bad arguments.
 */
mcp_status_t mcp_mrtr_state_pack(mcp_context_t *ctx,
                                 const mcp_json_value_t *state,
                                 const uint8_t *key,
                                 size_t key_len,
                                 uint64_t ttl_ms,
                                 char **state_out);

/**
 * @brief Unpack and verify a JSON `requestState` token.
 *
 * Verifies HMAC and TTL, then parses payload into an owned JSON value.
 *
 * @param ctx       Context; may be NULL.
 * @param state_str Opaque token string received from client.
 * @param key       HMAC secret key; NULL or key_len==0 for unsigned tokens.
 * @param key_len   Length of HMAC key in bytes.
 * @param state_out Receives newly allocated, owned JSON value.
 *                  Caller releases with mcp_json_destroy(ctx, *state_out).
 * @return MCP_OK on success;
 *         MCP_ERR_TIMEOUT if ttl_ms has expired;
 *         MCP_ERR_PERMISSION if HMAC verification failed;
 *         MCP_ERR_INVALID_ARGUMENT on malformed token or invalid JSON;
 *         MCP_ERR_NOMEM on allocation failure.
 */
mcp_status_t mcp_mrtr_state_unpack(mcp_context_t *ctx,
                                   const char *state_str,
                                   const uint8_t *key,
                                   size_t key_len,
                                   mcp_json_value_t **state_out);

#endif /* MCPKIT_PROTOCOL_MRTR_H */
