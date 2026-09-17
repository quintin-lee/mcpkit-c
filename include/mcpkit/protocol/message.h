// message.h — JSON-RPC 2.0 envelope over the MCP JSON DOM.
//
// An mcp_message_t wraps a JSON value tree with accessors that
// distinguish request / notification / response kinds. The message
// owns its JSON tree; callers free it with mcp_message_destroy(ctx, msg).
//
// All accessor functions return BORROWED pointers valid until the
// message is destroyed. The builders mcp_request_new_*, mcp_notification_new,
// mcp_response_*_new take ownership of `params` / `result` / `data`
// (non-NULL out-params are set on success; on failure the caller
// retains ownership).

#ifndef MCPKIT_PROTOCOL_MESSAGE_H
#define MCPKIT_PROTOCOL_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;

/**
 * JSON-RPC 2.0 error codes. The MCP spec uses a subset of the standard
 * JSON-RPC codes; mcp_status_to_rpc_code / mcp_rpc_code_to_status map
 * between MCP status codes and these integer codes.
 */
typedef enum {
    MCP_RPC_PARSE_ERROR = -32700,
    MCP_RPC_INVALID_REQUEST = -32600,
    MCP_RPC_METHOD_NOT_FOUND = -32601,
    MCP_RPC_INVALID_PARAMS = -32602,
    MCP_RPC_INTERNAL_ERROR = -32603,
} mcp_rpc_code_t;

/**
 * Message kind. A JSON-RPC 2.0 envelope is either a request (has "id"
 * and "method"), a notification (has "method" but no "id"), or a
 * response (has "id" and either "result" or "error"). MCP_MSG_INVALID
 * is returned when the kind cannot be determined.
 */
typedef enum {
    MCP_MSG_INVALID = 0,
    MCP_MSG_REQUEST,
    MCP_MSG_NOTIFICATION,
    MCP_MSG_RESPONSE,
} mcp_msg_kind_t;

/**
 * Discriminator for the "id" field: JSON-RPC 2.0 ids are either a
 * string or a number (or absent for notifications).
 */
typedef enum {
    MCP_ID_NONE = 0,
    MCP_ID_STRING,
    MCP_ID_NUMBER,
} mcp_id_type_t;

/**
 * Maximum serialized message size in bytes. Messages longer than this
 * are rejected by the validation layer (mcp_message_validate) with
 * MCP_RPC_PARSE_ERROR.
 */
#define MCP_PROTOCOL_MAX_MESSAGE_BYTES (4u * 1024u * 1024u)

/**
 * Opaque JSON-RPC 2.0 envelope. Owns its internal JSON tree.
 * Allocate with mcp_message_parse or one of the builder functions;
 * free with mcp_message_destroy(ctx, msg).
 */
typedef struct mcp_message mcp_message_t;

/**
 * Parses a single-line JSON-RPC 2.0 envelope from `text` (length `len`).
 * Returns NULL on parse failure or when the root is not a JSON object.
 * The returned message owns its JSON tree; free with mcp_message_destroy.
 * `ctx` may be NULL (default allocator, built-in backend).
 */
mcp_message_t *mcp_message_parse(mcp_context_t *ctx, const char *text, size_t len);

/**
 * Destroys `msg` and its owned JSON tree. `ctx` must be the same
 * context (or NULL) used to create `msg`. NULL msg is a no-op.
 */
void mcp_message_destroy(mcp_context_t *ctx, mcp_message_t *msg);

// --- Accessors (all return BORROWED pointers; msg must outlive use) ---

mcp_msg_kind_t mcp_message_kind(mcp_context_t *ctx, const mcp_message_t *msg);
const char *mcp_message_jsonrpc(mcp_context_t *ctx, const mcp_message_t *msg);
const char *mcp_message_method(mcp_context_t *ctx, const mcp_message_t *msg);
const mcp_json_value_t *mcp_message_params(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_id_type_t mcp_message_id_type(mcp_context_t *ctx, const mcp_message_t *msg);
const char *mcp_message_id_string(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_status_t mcp_message_id_number(mcp_context_t *ctx, const mcp_message_t *msg, double *out);
const mcp_json_value_t *mcp_message_result(mcp_context_t *ctx, const mcp_message_t *msg);
mcp_status_t mcp_message_error_code(mcp_context_t *ctx, const mcp_message_t *msg, int *out);
const char *mcp_message_error_text(mcp_context_t *ctx, const mcp_message_t *msg);

// --- Builders ---
// params / result / data are owned by the caller; on success ownership
// transfers to the message. On failure (MCP_ERR_NOMEM / NULL) the
// caller retains ownership of any non-NULL argument it passed.

/**
 * Creates a JSON-RPC 2.0 request with a string `id`. `method` must
 * be non-NULL. `params` may be NULL (omitted in the serialized form).
 */
mcp_message_t *mcp_request_new_string_id(mcp_context_t *ctx, const char *id,
                                         const char *method, mcp_json_value_t *params);

/**
 * Creates a JSON-RPC 2.0 request with a numeric `id`. `method` must
 * be non-NULL. `params` may be NULL.
 */
mcp_message_t *mcp_request_new_number_id(mcp_context_t *ctx, double id,
                                         const char *method, mcp_json_value_t *params);

/**
 * Creates a JSON-RPC 2.0 notification (no "id" field). `method` must
 * be non-NULL. `params` may be NULL.
 */
mcp_message_t *mcp_notification_new(mcp_context_t *ctx, const char *method,
                                    mcp_json_value_t *params);

/**
 * Creates a JSON-RPC 2.0 success response carrying `result` (owned).
 * `req` supplies the "id"; pass NULL for a standalone error response
 * with no id. The response JSON object owns `result` on success.
 */
mcp_message_t *mcp_response_ok_new(mcp_context_t *ctx, const mcp_message_t *req,
                                   mcp_json_value_t *result);

/**
 * Creates a JSON-RPC 2.0 error response. `req_or_null` supplies the
 * "id" (NULL → "id" field omitted). `code` is a JSON-RPC error code
 * (see mcp_rpc_code_t). `message` is a human-readable string;
 * `data` (optional) is attached as "error.data".
 */
mcp_message_t *mcp_response_err_new(mcp_context_t *ctx, const mcp_message_t *req_or_null,
                                    int code, const char *message, mcp_json_value_t *data);

/**
 * Serializes `msg` to a heap-allocated JSON string. The caller owns
 * the returned string and must free it with mcp_json_free_string(ctx, s)
 * using the SAME context that created `msg`.
 */
char *mcp_message_serialize(mcp_context_t *ctx, const mcp_message_t *msg);

/**
 * Maps an MCP status code to the nearest JSON-RPC error code.
 * MCP_OK → 0; MCP_ERR_NOMEM/IO/PROTOCOL/etc. → MCP_RPC codes.
 * Unmapped codes collapse to MCP_RPC_INTERNAL_ERROR.
 */
int mcp_status_to_rpc_code(mcp_status_t status);

/**
 * Maps a JSON-RPC error code to the nearest MCP status.
 * Standard codes (e.g. -32602) map to specific MCP status values;
 * unknown codes map to MCP_ERR_PROTOCOL.
 */
mcp_status_t mcp_rpc_code_to_status(int code);

#endif
