/**
 * @file internals.h
 *
 * Internal layout of the client handle: the bound context, transport,
 * the next JSON-RPC request id, and the negotiated protocol version.
 * NOT a public header.
 */
#ifndef MCPKIT_CLIENT_INTERNALS_H
#define MCPKIT_CLIENT_INTERNALS_H

#include "mcpkit/client/client.h"

struct mcp_client {
    mcp_context_t *ctx;
    mcp_transport_t *t;
    double next_id;
    char *version;
};

#endif
