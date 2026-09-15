#ifndef MCPKIT_TRANSPORT_INTERNALS_H
#define MCPKIT_TRANSPORT_INTERNALS_H

#include "mcpkit/transport/transport.h"

// Backend pointer stored at create; owned by the backend itself,
// transport only passes it through to ops callbacks.
void *mcp_transport_backend(mcp_context_t *ctx, const mcp_transport_t *t);
void mcp_transport_set_backend(mcp_context_t *ctx, mcp_transport_t *t, void *backend);

#endif
