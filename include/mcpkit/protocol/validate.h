// validate.h — JSON-RPC envelope validation and duplicate-id detection.
//
// Validation pipeline (run in order by the dispatcher):
//   L1: mcp_validate_envelope   — structural checks (jsonrpc, method,
//        id fields, size cap, UTF-8)
//   L2: mcp_validate_method     — method is in the known-methods table
//   L3: mcp_validate_params     — per-method params type/field checks
//
// mcp_message_validate runs all three levels and writes the JSON-RPC
// error code into *rpc_code_out on failure.
//
// mcp_idset_t tracks in-flight request ids per session to detect
// duplicates before routing (JSON-RPC 2.0 §6).

#ifndef MCPKIT_PROTOCOL_VALIDATE_H
#define MCPKIT_PROTOCOL_VALIDATE_H

#include <stdbool.h>

#include "mcpkit/core/error.h"
#include "mcpkit/protocol/message.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;

/**
 * Opaque set of in-flight JSON-RPC ids. Supports both string and
 * numeric ids (distinct types, so "1" and 1.0 are separate entries).
 * Allocate with mcp_idset_create, free with mcp_idset_destroy.
 */
typedef struct mcp_idset mcp_idset_t;

/**
 * Creates an empty id set. `ctx` may be NULL (default allocator).
 * Returns NULL on NOMEM.
 */
mcp_idset_t *mcp_idset_create(mcp_context_t *ctx);

/**
 * Destroys the id set and all its entries. `ctx` must match the
 * context used at create time. NULL set is a no-op.
 */
void mcp_idset_destroy(mcp_context_t *ctx, mcp_idset_t *set);

/**
 * Adds an id to the set. For string ids pass `s` with `type=MCP_ID_STRING`;
 * for numeric ids pass `n` with `type=MCP_ID_NUMBER`. Returns
 * MCP_ERR_ALREADY_EXISTS if the id is already present.
 */
mcp_status_t mcp_idset_add(mcp_context_t *ctx, mcp_idset_t *set,
                           mcp_id_type_t type, const char *s, double n);

/**
 * Removes an id from the set. Missing ids are silently ignored.
 */
void mcp_idset_remove(mcp_context_t *ctx, mcp_idset_t *set,
                      mcp_id_type_t type, const char *s, double n);

/**
 * Returns true if the id is present in the set.
 */
bool mcp_idset_contains(mcp_context_t *ctx, const mcp_idset_t *set,
                        mcp_id_type_t type, const char *s, double n);

/**
 * Returns true if `method` is a known MCP method. `method` must be
 * a non-NULL NUL-terminated string. No context needed (static table).
 */
bool mcp_method_known(const char *method);

/**
 * L1 envelope validation. Checks that the message has a valid
 * "jsonrpc":"2.0" field, a known message kind, and that the
 * serialized size is within MCP_PROTOCOL_MAX_MESSAGE_BYTES.
 * Writes the JSON-RPC error code to *rpc_code_out on failure.
 * `ctx` may be NULL.
 */
mcp_status_t mcp_validate_envelope(mcp_context_t *ctx, const mcp_message_t *msg,
                                   int *rpc_code_out);

/**
 * L2 method validation. For requests and notifications, checks that
 * the method string is in the known-methods table. For responses,
 * always returns MCP_OK (responses have no method).
 */
mcp_status_t mcp_validate_method(mcp_context_t *ctx, const mcp_message_t *msg,
                                 int *rpc_code_out);

/**
 * L3 params validation. Per-method checks on the params JSON object
 * (e.g. initialize requires protocolVersion string, tools/call
 * requires name string + optional arguments object).
 * Returns MCP_OK for methods with no params constraints.
 */
mcp_status_t mcp_validate_params(mcp_context_t *ctx, const mcp_message_t *msg,
                                 int *rpc_code_out);

/**
 * Runs L1+L2+L3 in sequence. On the first failure writes the JSON-RPC
 * error code to *rpc_code_out and returns the corresponding MCP status.
 * `ctx` may be NULL.
 */
mcp_status_t mcp_message_validate(mcp_context_t *ctx, const mcp_message_t *msg,
                                  int *rpc_code_out);

#endif
