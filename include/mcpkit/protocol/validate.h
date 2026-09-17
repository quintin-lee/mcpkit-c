/**
 * @file validate.h
 * JSON-RPC envelope validation and duplicate-id detection.
 *
 * Validation pipeline (run in order by the dispatcher):
 *   L1: mcp_validate_envelope   — structural checks (jsonrpc, method,
 *        id fields, size cap, UTF-8)
 *   L2: mcp_validate_method     — method is in the known-methods table
 *   L3: mcp_validate_params     — per-method params type/field checks
 *
 * mcp_message_validate runs all three levels and writes the JSON-RPC
 * error code into *rpc_code_out on failure.
 *
 * mcp_idset_t tracks in-flight request ids per session to detect
 * duplicates before routing (JSON-RPC 2.0 §6).
 */

#ifndef MCPKIT_PROTOCOL_VALIDATE_H
#define MCPKIT_PROTOCOL_VALIDATE_H

#include <stdbool.h>

#include "mcpkit/core/error.h"
#include "mcpkit/protocol/message.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;

/**
 * @brief Opaque set of in-flight JSON-RPC ids.
 *
 * Supports both string and numeric ids (distinct types, so "1" and 1.0
 * are separate entries).
 * Allocate with mcp_idset_create, free with mcp_idset_destroy.
 */
typedef struct mcp_idset mcp_idset_t;

/**
 * @brief Creates an empty id set.
 *
 * @param ctx  Context; NULL uses the default allocator.
 * @return Caller-owned id set, or NULL on NOMEM.
 */
mcp_idset_t *mcp_idset_create(mcp_context_t *ctx);

/**
 * @brief Destroys the id set and all its entries.
 *
 * @param ctx  Context that created the set (or NULL).
 * @param set  Id set to destroy; NULL is a no-op.
 */
void mcp_idset_destroy(mcp_context_t *ctx, mcp_idset_t *set);

/**
 * @brief Adds an id to the set.
 *
 * For string ids pass `s` with `type=MCP_ID_STRING`; for numeric ids
 * pass `n` with `type=MCP_ID_NUMBER`.
 *
 * @param ctx  Context.
 * @param set  Id set to modify.
 * @param type  ID type discriminator.
 * @param s     String value (for MCP_ID_STRING).
 * @param n     Numeric value (for MCP_ID_NUMBER).
 * @return MCP_OK on success; MCP_ERR_ALREADY_EXISTS if the id is already present.
 */
mcp_status_t mcp_idset_add(mcp_context_t *ctx, mcp_idset_t *set,
                            mcp_id_type_t type, const char *s, double n);

/**
 * @brief Removes an id from the set.
 *
 * Missing ids are silently ignored.
 *
 * @param ctx  Context.
 * @param set  Id set to modify.
 * @param type  ID type discriminator.
 * @param s     String value (for MCP_ID_STRING).
 * @param n     Numeric value (for MCP_ID_NUMBER).
 */
void mcp_idset_remove(mcp_context_t *ctx, mcp_idset_t *set,
                       mcp_id_type_t type, const char *s, double n);

/**
 * @brief Returns true if the id is present in the set.
 *
 * @param ctx  Context.
 * @param set  Id set to query.
 * @param type  ID type discriminator.
 * @param s     String value (for MCP_ID_STRING).
 * @param n     Numeric value (for MCP_ID_NUMBER).
 * @return True if present; false otherwise.
 */
bool mcp_idset_contains(mcp_context_t *ctx, const mcp_idset_t *set,
                         mcp_id_type_t type, const char *s, double n);

/**
 * @brief Returns true if `method` is a known MCP method.
 *
 * No context needed (static table lookup).
 *
 * @param method  NUL-terminated method string; must be non-NULL.
 * @return True if known; false otherwise.
 */
bool mcp_method_known(const char *method);

/**
 * @brief L1 envelope validation.
 *
 * Checks that the message has a valid "jsonrpc":"2.0" field, a known
 * message kind, and that the serialized size is within
 * MCP_PROTOCOL_MAX_MESSAGE_BYTES.
 *
 * @param ctx          Context; NULL is accepted.
 * @param msg          Message to validate.
 * @param rpc_code_out  Out-parameter; receives the JSON-RPC error code on failure.
 * @return MCP_OK on success; MCP_ERR_PROTOCOL on failure.
 */
mcp_status_t mcp_validate_envelope(mcp_context_t *ctx, const mcp_message_t *msg,
                                    int *rpc_code_out);

/**
 * @brief L2 method validation.
 *
 * For requests and notifications, checks that the method string is in
 * the known-methods table. For responses, always returns MCP_OK
 * (responses have no method).
 *
 * @param ctx          Context.
 * @param msg          Message to validate.
 * @param rpc_code_out  Out-parameter; receives the JSON-RPC error code on failure.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT on failure.
 */
mcp_status_t mcp_validate_method(mcp_context_t *ctx, const mcp_message_t *msg,
                                  int *rpc_code_out);

/**
 * @brief L3 params validation.
 *
 * Per-method checks on the params JSON object (e.g. initialize requires
 * protocolVersion string, tools/call requires name string + optional
 * arguments object). Returns MCP_OK for methods with no params
 * constraints.
 *
 * @param ctx          Context.
 * @param msg          Message to validate.
 * @param rpc_code_out  Out-parameter; receives the JSON-RPC error code on failure.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT on failure.
 */
mcp_status_t mcp_validate_params(mcp_context_t *ctx, const mcp_message_t *msg,
                                  int *rpc_code_out);

/**
 * @brief Runs L1+L2+L3 validation in sequence.
 *
 * On the first failure writes the JSON-RPC error code to *rpc_code_out
 * and returns the corresponding MCP status.
 *
 * @param ctx          Context; NULL is accepted.
 * @param msg          Message to validate.
 * @param rpc_code_out  Out-parameter; receives the JSON-RPC error code on failure.
 * @return MCP_OK if all levels pass; the failing level's status otherwise.
 */
mcp_status_t mcp_message_validate(mcp_context_t *ctx, const mcp_message_t *msg,
                                   int *rpc_code_out);

#endif
