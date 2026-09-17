#ifndef MCPKIT_SERVER_DISPATCHER_H
#define MCPKIT_SERVER_DISPATCHER_H

#include <stddef.h>

#include "mcpkit/core/error.h"

/**
 * @file dispatcher.h
 * Per-session message queue and single-step processing.
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

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_message mcp_message_t;
typedef struct mcp_queue mcp_queue_t;

mcp_queue_t *mcp_queue_create(mcp_context_t *ctx);
void mcp_queue_destroy(mcp_context_t *ctx, mcp_queue_t *q);

/**
 * Pushes a message onto the queue. Queue takes ownership on MCP_OK.
 * The queue is thread-safe with respect to single-producer/single-consumer
 * usage; multi-producer requires external synchronization.
 */
mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session,
                            mcp_message_t *msg);

/**
 * Pops the oldest message. On MCP_OK *session_out and *msg_out are
 * set; the caller now owns msg. Returns MCP_ERR_NOT_FOUND if the queue
 * is empty.
 */
mcp_status_t mcp_queue_pop(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t **session_out,
                           mcp_message_t **msg_out);
size_t mcp_queue_size(mcp_context_t *ctx, const mcp_queue_t *q);

/**
 * Pops and processes one message. The message is destroyed internally
 * after dispatch. On MCP_OK *resp_out is set to a caller-owned response
 * (NULL if it was a notification). Returns MCP_ERR_NOT_FOUND if the
 * queue is empty.
 */
mcp_status_t mcp_server_process_one(mcp_context_t *ctx, mcp_server_t *server,
                                    mcp_queue_t *q, mcp_message_t **resp_out);

#endif
