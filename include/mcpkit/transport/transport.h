/**
 * @file transport.h
 * @brief Abstract line-transport with pluggable backend.
 *
 * Ownership:
 * - mcp_transport_create(): caller owns the transport; destroy with
 *   mcp_transport_destroy(ctx, t). The backend pointer is stored but NOT
 *   freed — the backend's lifetime is managed by the caller or the ops'
 *   stop callback.
 * - mcp_transport_recv(): on MCP_OK *line_out is an owned heap string
 *   (allocated via the ctx allocator); caller must free it with
 *   mcp_json_free_string(ctx, line) after use.
 * - Ops callbacks: start/stop are called at most once per transport
 *   lifetime; send/recv may be called any number of times between
 *   start and stop.
 */

#ifndef MCPKIT_TRANSPORT_TRANSPORT_H
#define MCPKIT_TRANSPORT_TRANSPORT_H

#include <stddef.h>

#include "mcpkit/core/error.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

/**
 * @brief Backend operation table.
 *
 * All four callbacks are required; a NULL ops pointer is rejected by
 * mcp_transport_create.
 */
typedef struct mcp_transport_ops {
    mcp_status_t (*start)(mcp_context_t *ctx, mcp_transport_t *t);
    mcp_status_t (*send)(mcp_context_t *ctx, mcp_transport_t *t, const char *data, size_t len);
    mcp_status_t (*recv)(mcp_context_t *ctx, mcp_transport_t *t, char **line_out);
    mcp_status_t (*stop)(mcp_context_t *ctx, mcp_transport_t *t);
} mcp_transport_ops_t;

/**
 * @brief Creates a transport wrapper around an ops table and a backend
 *        pointer.
 * @param ctx Context; may be NULL (default allocator).
 * @param ops Required ops table; NULL is rejected.
 * @param backend Opaque backend pointer; stored, not owned.
 * @return Owned mcp_transport_t, or NULL on OOM or NULL ops.
 */
mcp_transport_t *mcp_transport_create(mcp_context_t *ctx, const mcp_transport_ops_t *ops,
                                      void *backend);

/**
 * @brief Destroys a transport wrapper (does not free the backend).
 * @param ctx Context; may be NULL.
 * @param t Transport to destroy; NULL is a no-op.
 */
void mcp_transport_destroy(mcp_context_t *ctx, mcp_transport_t *t);

/**
 * @brief Returns the backend pointer passed to mcp_transport_create.
 *
 * Used by ops implementations to access their own state.
 *
 * @param ctx Context; may be NULL.
 * @param t Transport handle; NULL yields NULL.
 * @return The stored backend pointer.
 */
void *mcp_transport_backend(mcp_context_t *ctx, const mcp_transport_t *t);

/**
 * @brief Invokes the backend's start callback.
 * @param ctx Context; may be NULL.
 * @param t Target transport.
 * @return Backend's status.
 */
mcp_status_t mcp_transport_start(mcp_context_t *ctx, mcp_transport_t *t);

/**
 * @brief Sends a line to the backend (newline appended by the ops).
 * @param ctx Context; may be NULL.
 * @param t Target transport.
 * @param data Null-terminated data; only the first len bytes are sent.
 * @param len Number of bytes to send.
 * @return Backend's status.
 */
mcp_status_t mcp_transport_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                                size_t len);

/**
 * @brief Receives one newline-delimited line.
 *
 * On MCP_OK *line_out is set to an owned heap string (no trailing
 * newline). On EOF the line may be a partial (incomplete) line — still
 * returned as MCP_OK.
 *
 * @param ctx Context; may be NULL.
 * @param t Target transport.
 * @param line_out Receives an owned heap string; caller must free with
 *                 mcp_json_free_string(ctx, line).
 * @return MCP_OK on success (including EOF-partial-line); MCP_ERR_IO on
 *         I/O error; MCP_ERR_PROTOCOL on oversize.
 */
mcp_status_t mcp_transport_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out);

/**
 * @brief Invokes the backend's stop callback.
 * @param ctx Context; may be NULL.
 * @param t Target transport.
 * @return Backend's status.
 */
mcp_status_t mcp_transport_stop(mcp_context_t *ctx, mcp_transport_t *t);

#endif
