#ifndef MCPKIT_TRANSPORT_TRANSPORT_H
#define MCPKIT_TRANSPORT_TRANSPORT_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

typedef struct mcp_transport_ops {
    mcp_status_t (*start)(mcp_context_t *ctx, mcp_transport_t *t);
    mcp_status_t (*send)(mcp_context_t *ctx, mcp_transport_t *t, const char *data, size_t len);
    mcp_status_t (*recv)(mcp_context_t *ctx, mcp_transport_t *t, char **line_out);
    mcp_status_t (*stop)(mcp_context_t *ctx, mcp_transport_t *t);
} mcp_transport_ops_t;

mcp_transport_t *mcp_transport_create(mcp_context_t *ctx, const mcp_transport_ops_t *ops,
                                      void *backend);
void mcp_transport_destroy(mcp_context_t *ctx, mcp_transport_t *t);
// Backend pointer from create, for ops callbacks; NULL transport yields NULL.
void *mcp_transport_backend(mcp_context_t *ctx, const mcp_transport_t *t);
mcp_status_t mcp_transport_start(mcp_context_t *ctx, mcp_transport_t *t);
mcp_status_t mcp_transport_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                                size_t len);
mcp_status_t mcp_transport_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out);
mcp_status_t mcp_transport_stop(mcp_context_t *ctx, mcp_transport_t *t);

#endif
