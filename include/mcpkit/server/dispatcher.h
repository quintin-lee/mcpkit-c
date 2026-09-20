/**
 * @file dispatcher.h
 * @brief Per-session message queue and single-step processing.
 *
 * Ownership:
 * - mcp_queue_create(): caller owns the queue; destroy with mcp_queue_destroy.
 * - mcp_queue_push(): the queue TAKES ownership of the message on MCP_OK.
 *   On error the message is NOT consumed; caller retains it.
 * - mcp_queue_pop(): the queue gives ownership of the message to the
 *   caller on MCP_OK.
 * - mcp_server_process_one(): pops one message from the queue and
 *   dispatches it. The message is destroyed internally after dispatch;
 *   on MCP_OK *resp_out is a caller-owned response (NULL if the message
 *   was a notification).
 */

#ifndef MCPKIT_SERVER_DISPATCHER_H
#define MCPKIT_SERVER_DISPATCHER_H

#include <stddef.h>

#include "mcpkit/core/error.h"


typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_message mcp_message_t;
typedef struct mcp_queue mcp_queue_t;

/**
 * @brief Maximum number of messages a queue holds.
 *
 * mcp_queue_push() returns MCP_ERR_NOMEM (message NOT consumed) once the
 * queue reaches this length, so a flooding peer cannot grow memory
 * without bound.
 */
#define MCP_QUEUE_MAX_LEN 1024u

/**
 * @brief Creates an empty per-session message queue.
 * @param ctx Context; may be NULL (default allocator).
 * @return Owned mcp_queue_t, or NULL on OOM.
 *         Caller destroys with mcp_queue_destroy.
 */
mcp_queue_t *mcp_queue_create(mcp_context_t *ctx);

/**
 * @brief Destroys a message queue.
 * @param ctx Context; may be NULL.
 * @param q Queue to destroy; NULL is a no-op.
 */
void mcp_queue_destroy(mcp_context_t *ctx, mcp_queue_t *q);

/**
 * @brief Pushes a message onto the queue.
 *
 * The queue TAKES ownership of the message on MCP_OK. On error the message
 * is NOT consumed; caller retains it.
 *
 * Thread-safe for single-producer / single-consumer usage; multi-producer
 * requires external synchronization.
 *
 * @param ctx Context; may be NULL.
 * @param q Target queue.
 * @param session Session that owns this message.
 * @param msg Message to enqueue; ownership transferred to queue on MCP_OK.
 * @return MCP_OK on success; MCP_ERR_NOMEM on allocation failure or when
 *         the queue already holds MCP_QUEUE_MAX_LEN messages.
 */
mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session,
                           mcp_message_t *msg);

/**
 * @brief Pops the oldest message from the queue.
 *
 * On MCP_OK the caller owns the returned message.
 *
 * @param ctx Context; may be NULL.
 * @param q Source queue.
 * @param session_out Receives the session associated with the message.
 * @param msg_out Receives the message; caller must destroy with
 *                mcp_message_destroy when done.
 * @return MCP_OK on success; MCP_ERR_NOT_FOUND if the queue is empty.
 */
mcp_status_t mcp_queue_pop(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t **session_out,
                           mcp_message_t **msg_out);

/**
 * @brief Returns the number of messages currently in the queue.
 * @param ctx Context; may be NULL.
 * @param q Queue to inspect.
 * @return Current queue depth.
 */
size_t mcp_queue_size(mcp_context_t *ctx, const mcp_queue_t *q);

/**
 * @brief Pops and processes one message from the queue.
 *
 * The message is destroyed internally after dispatch. On MCP_OK *resp_out
 * is set to a caller-owned response, or NULL if the message was a
 * notification.
 *
 * @param ctx Context; may be NULL.
 * @param server Target server.
 * @param q Queue to process from.
 * @param resp_out On MCP_OK receives a caller-owned response (NULL for
 *                 notifications); set to NULL before calling for safety.
 * @return MCP_OK after dispatch; MCP_ERR_NOT_FOUND if the queue is empty.
 */
mcp_status_t mcp_server_process_one(mcp_context_t *ctx, mcp_server_t *server,
                                    mcp_queue_t *q, mcp_message_t **resp_out);

#endif
