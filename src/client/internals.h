/**
 * @file internals.h
 *
 * Internal layout of the client handle: the bound context, transport,
 * the next JSON-RPC request id, and the negotiated protocol version.
 * Also holds the three host-injected response provider callbacks
 * (roots, sampling, elicitation) and the MRTR elicitation callback.
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
    /* Host-injected response providers (NULL = feature not supported,
     * client replies -32601 for the corresponding server request). */
    mcp_client_roots_fn roots_fn;
    void *roots_ud;
    mcp_client_sample_fn sample_fn;
    void *sample_ud;
    mcp_client_elicitation_fn elicitation_fn;
    void *elicitation_ud;
    /* MRTR elicitation callback (NULL = no auto-retry on input_required). */
    mcp_client_mrtr_elicit_fn mrtr_elicit_fn;
    void *mrtr_elicit_ud;
    /* Stored OAuth 2.1 Bearer token (NULL if unauthenticated). */
    char *bearer_token;
};

#endif
