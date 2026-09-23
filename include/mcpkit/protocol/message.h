/**
 * @file message.h
 * JSON-RPC 2.0 envelope over the MCP JSON DOM.
 *
 * An mcp_message_t wraps a JSON value tree with accessors that
 * distinguish request / notification / response kinds. The message
 * owns its JSON tree; callers free it with mcp_message_destroy(ctx, msg).
 *
 * All accessor functions return BORROWED pointers valid until the
 * message is destroyed. The builders mcp_request_new_*, mcp_notification_new,
 * mcp_response_*_new take ownership of `params` / `result` / `data`
 * (non-NULL out-params are set on success; on failure the caller
 * retains ownership).
 *
 * @defgroup mcpkit-protocol Protocol
 * @brief JSON-RPC 2.0 message envelope, initialization negotiation, and
 * three-level validation (L1 syntax, L2 known-methods, L3 per-method params).
 * @ingroup mcpkit-protocol
 * @see mcpkit-server (dispatch routes these messages), mcpkit-client
 */

#ifndef MCPKIT_PROTOCOL_MESSAGE_H
#define MCPKIT_PROTOCOL_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;

/**
 * @brief JSON-RPC 2.0 error codes.
 *
 * The MCP spec uses a subset of the standard JSON-RPC codes;
 * mcp_status_to_rpc_code / mcp_rpc_code_to_status map between
 * MCP status codes and these integer codes.
 */
typedef enum {
    MCP_RPC_PARSE_ERROR = -32700,
    MCP_RPC_INVALID_REQUEST = -32600,
    MCP_RPC_METHOD_NOT_FOUND = -32601,
    MCP_RPC_INVALID_PARAMS = -32602,
    MCP_RPC_INTERNAL_ERROR = -32603,
    /* MCP 2026-07-28 spec error codes (-32020 to -32099 reserved) */
    MCP_RPC_HEADER_MISMATCH = -32020,
    MCP_RPC_MISSING_REQUIRED_CLIENT_CAPABILITY = -32021,
    MCP_RPC_UNSUPPORTED_PROTOCOL_VERSION = -32022,
} mcp_rpc_code_t;

/**
 * @brief Message kind.
 *
 * A JSON-RPC 2.0 envelope is either a request (has "id" and "method"),
 * a notification (has "method" but no "id"), or a response (has "id"
 * and either "result" or "error"). MCP_MSG_INVALID is returned when
 * the kind cannot be determined.
 */
typedef enum {
    MCP_MSG_INVALID = 0,
    MCP_MSG_REQUEST,
    MCP_MSG_NOTIFICATION,
    MCP_MSG_RESPONSE,
} mcp_msg_kind_t;

/**
 * @brief Discriminator for the "id" field.
 *
 * JSON-RPC 2.0 ids are either a string or a number (or absent for
 * notifications).
 */
typedef enum {
    MCP_ID_NONE = 0,
    MCP_ID_STRING,
    MCP_ID_NUMBER,
} mcp_id_type_t;

/**
 * @brief Maximum serialized message size in bytes.
 *
 * Messages longer than this are rejected by the validation layer
 * (mcp_message_validate) with MCP_RPC_PARSE_ERROR.
 */
#define MCP_PROTOCOL_MAX_MESSAGE_BYTES (4u * 1024u * 1024u)

/**
 * @brief Opaque JSON-RPC 2.0 envelope.
 *
 * Owns its internal JSON tree.
 * Allocate with mcp_message_parse or one of the builder functions;
 * free with mcp_message_destroy(ctx, msg).
 */
typedef struct mcp_message mcp_message_t;

/**
 * @brief Parses a single-line JSON-RPC 2.0 envelope from `text`.
 *
 * @param ctx   Context; NULL uses the default allocator and built-in backend.
 * @param text  JSON text.
 * @param len   Length in bytes.
 * @return Caller-owned message, or NULL on parse failure or when the
 *         root is not a JSON object.
 */
mcp_message_t *mcp_message_parse(mcp_context_t *ctx, const char *text, size_t len);

/**
 * @brief Destroys `msg` and its owned JSON tree.
 *
 * @param ctx  Context that created `msg` (or NULL).
 * @param msg  Message to destroy; NULL is a no-op.
 */
void mcp_message_destroy(mcp_context_t *ctx, mcp_message_t *msg);

/* --- Accessors (all return BORROWED pointers; msg must outlive use) --- */

/**
 * @brief Returns the message kind.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return The message kind.
 */
mcp_msg_kind_t mcp_message_kind(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Returns the `"jsonrpc"` field.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return Borrowed string, or NULL if absent.
 */
const char *mcp_message_jsonrpc(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Returns the `"method"` field.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return Borrowed string, or NULL if absent (e.g. response messages).
 */
const char *mcp_message_method(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Returns the `"params"` field.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return Borrowed value, or NULL if absent.
 */
const mcp_json_value_t *mcp_message_params(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Returns the type of the `"id"` field.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return ID type discriminator.
 */
mcp_id_type_t mcp_message_id_type(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Returns the `"id"` field as a string.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return Borrowed string, or NULL if the id is not a string.
 */
const char *mcp_message_id_string(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Returns the `"id"` field as a number.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @param out  Out-parameter for the numeric id.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if the id is not a number.
 */
mcp_status_t mcp_message_id_number(mcp_context_t *ctx, const mcp_message_t *msg, double *out);

/**
 * @brief Returns the `"result"` field.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return Borrowed value, or NULL if absent.
 */
const mcp_json_value_t *mcp_message_result(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Returns the `"error.code"` field.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @param out  Out-parameter for the error code.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT if there is no error field.
 */
mcp_status_t mcp_message_error_code(mcp_context_t *ctx, const mcp_message_t *msg, int *out);

/**
 * @brief Returns the `"error.message"` field.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return Borrowed string, or NULL if there is no error field.
 */
const char *mcp_message_error_text(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Returns the top-level `"_meta"` field of the request.
 *
 * The client may carry extension metadata in a top-level `_meta` object
 * on any JSON-RPC request. This accessor returns a BORROWED pointer valid
 * while `msg` is alive; NULL if the key is absent or the value is not an
 * object.
 *
 * @param ctx  Context.
 * @param msg  Message to inspect.
 * @return Borrowed `_meta` value, or NULL.
 */
const mcp_json_value_t *mcp_message_meta(mcp_context_t *ctx, const mcp_message_t *msg);

/* --- Builders --- */
/* params / result / data are owned by the caller; on success ownership
 * transfers to the message. On failure (MCP_ERR_NOMEM / NULL) the
 * caller retains ownership of any non-NULL argument it passed. */

/**
 * @brief Creates a JSON-RPC 2.0 request with a string `id`.
 *
 * @param ctx     Context.
 * @param id      Request id (must be non-NULL).
 * @param method  Method name (must be non-NULL).
 * @param params  Params value; ownership transfers on MCP_OK; may be NULL.
 * @return Caller-owned message, or NULL on allocation failure.
 */
mcp_message_t *mcp_request_new_string_id(mcp_context_t *ctx, const char *id,
                                          const char *method, mcp_json_value_t *params);

/**
 * @brief Creates a JSON-RPC 2.0 request with a numeric `id`.
 *
 * @param ctx     Context.
 * @param id      Numeric request id.
 * @param method  Method name (must be non-NULL).
 * @param params  Params value; ownership transfers on MCP_OK; may be NULL.
 * @return Caller-owned message, or NULL on allocation failure.
 */
mcp_message_t *mcp_request_new_number_id(mcp_context_t *ctx, double id,
                                          const char *method, mcp_json_value_t *params);

/**
 * @brief Creates a JSON-RPC 2.0 notification (no "id" field).
 *
 * @param ctx     Context.
 * @param method  Method name (must be non-NULL).
 * @param params  Params value; ownership transfers on MCP_OK; may be NULL.
 * @return Caller-owned message, or NULL on allocation failure.
 */
mcp_message_t *mcp_notification_new(mcp_context_t *ctx, const char *method,
                                     mcp_json_value_t *params);

/**
 * @brief Creates a JSON-RPC 2.0 success response.
 *
 * @param ctx     Context.
 * @param req     Request to respond to (supplies the "id"); NULL for
 *                a standalone response with no id.
 * @param result  Result value; ownership transfers to the response on success.
 * @return Caller-owned message, or NULL on allocation failure.
 */
mcp_message_t *mcp_response_ok_new(mcp_context_t *ctx, const mcp_message_t *req,
                                    mcp_json_value_t *result);

/**
 * @brief Creates a JSON-RPC 2.0 success response with an explicit string id.
 *
 * @param ctx     Context.
 * @param id      String id (must be non-NULL).
 * @param result  Result value; ownership transfers to the response on success.
 * @return Caller-owned message, or NULL on allocation failure.
 */
mcp_message_t *mcp_response_ok_string_id_new(mcp_context_t *ctx, const char *id,
                                             mcp_json_value_t *result);

/**
 * @brief Creates a JSON-RPC 2.0 success response with an explicit numeric id.
 *
 * @param ctx     Context.
 * @param id      Numeric id.
 * @param result  Result value; ownership transfers to the response on success.
 * @return Caller-owned message, or NULL on allocation failure.
 */
mcp_message_t *mcp_response_ok_number_id_new(mcp_context_t *ctx, double id,
                                             mcp_json_value_t *result);

/**
 * @brief Creates a JSON-RPC 2.0 error response.
 *
 * @param ctx         Context.
 * @param req_or_null Request to respond to (supplies the "id"); NULL
 *                    omits the "id" field.
 * @param code        JSON-RPC error code (see mcp_rpc_code_t).
 * @param message     Human-readable error message.
 * @param data        Optional "error.data" value; ownership transfers on success.
 * @return Caller-owned message, or NULL on allocation failure.
 */
mcp_message_t *mcp_response_err_new(mcp_context_t *ctx, const mcp_message_t *req_or_null,
                                     int code, const char *message, mcp_json_value_t *data);

/**
 * @brief Serializes `msg` to a heap-allocated JSON string.
 *
 * @param ctx  Context that created `msg` (or NULL).
 * @param msg  Message to serialize.
 * @return Caller-owned NUL-terminated string; free with
 *         mcp_json_free_string(ctx, s) using the SAME ctx.
 */
char *mcp_message_serialize(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * @brief Injects `"resultType": "complete"` into a result object.
 *
 * Signals to the client that this response is a complete result, not a
 * partial/streaming fragment. No allocation is made for the value string
 * (a static literal is used internally).
 *
 * @param ctx     Context.
 * @param result  Result object to decorate (must be a non-NULL JSON object;
 *                NOMEM/INVALID_ARGUMENT otherwise).
 * @return MCP_OK on success; MCP_ERR_NOMEM on allocation failure;
 *         MCP_ERR_INVALID_ARGUMENT if `result` is NULL or not an object.
 */
mcp_status_t mcp_result_inject_result_type(mcp_context_t *ctx, mcp_json_value_t *result);

/**
 * @brief Injects a caller-owned `_meta` object into a result object.
 *
 * @param ctx     Context.
 * @param result  Result object to decorate (must be a non-NULL JSON object).
 * @param meta    Meta value; caller-owned. On success ownership transfers to
 *                `result` (destroyed with the result). On failure the caller
 *                retains ownership of `meta`.
 * @return MCP_OK on success; MCP_ERR_NOMEM on allocation failure;
 *         MCP_ERR_INVALID_ARGUMENT if `result` or `meta` is NULL or
 *         `result` is not an object.
 */
mcp_status_t mcp_result_inject_meta(mcp_context_t *ctx, mcp_json_value_t *result,
                                    mcp_json_value_t *meta);

/**
 * @brief Maps an MCP status code to the nearest JSON-RPC error code.
 *
 * MCP_OK maps to 0; MCP_ERR_NOMEM/IO/PROTOCOL/etc. map to the
 * corresponding MCP_RPC codes. Unmapped codes collapse to
 * MCP_RPC_INTERNAL_ERROR.
 *
 * @param status  MCP status code.
 * @return The nearest JSON-RPC error code.
 */
int mcp_status_to_rpc_code(mcp_status_t status);

/**
 * @brief Maps a JSON-RPC error code to the nearest MCP status.
 *
 * Standard codes (e.g. -32602) map to specific MCP status values;
 * unknown codes map to MCP_ERR_PROTOCOL.
 *
 * @param code  JSON-RPC error code.
 * @return The nearest MCP status.
 */
mcp_status_t mcp_rpc_code_to_status(int code);

#endif
