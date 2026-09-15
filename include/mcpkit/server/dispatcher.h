#ifndef MCPKIT_SERVER_DISPATCHER_H
#define MCPKIT_SERVER_DISPATCHER_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_message mcp_message_t;
typedef struct mcp_queue mcp_queue_t;

mcp_queue_t *mcp_queue_create(mcp_context_t *ctx);
void mcp_queue_destroy(mcp_context_t *ctx, mcp_queue_t *q);
mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session,
                            mcp_message_t *msg);
mcp_status_t mcp_queue_pop(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t **session_out,
                           mcp_message_t **msg_out);
size_t mcp_queue_size(mcp_context_t *ctx, const mcp_queue_t *q);
mcp_status_t mcp_server_process_one(mcp_context_t *ctx, mcp_server_t *server, mcp_queue_t *q,
                                    mcp_message_t **resp_out);

#endif
