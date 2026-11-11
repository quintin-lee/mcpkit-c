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

#include <stdbool.h>
#include <stddef.h>

/* P4-a T4: a single pending Mcp-Param-* header computed during a
 * tools/call mirroring pass. name/value are owned by the client. */
typedef struct mcp_client_xmcp_param {
    char *name;   /* header suffix, e.g. "Region" */
    char *value;  /* encoded value (plain or =?base64?…?= sentinel) */
} mcp_client_xmcp_param_t;

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
    /* P4-a T4: client-side tool schema cache for Mcp-Param-* mirroring.
     * Grows on demand; entries are deep-cloned input schemas. */
    struct {
        char **tool_names;         /* owned */
        mcp_json_value_t **schemas; /* owned (deep-cloned) */
        size_t n;
        size_t cap;
    } schema_cache;
    /* P4-a T4: Mcp-Param-* headers computed for the next/last tools/call.
     * The host's HTTP transport reads these before sending and calls
     * mcp_http_request_set_xmcp_param per entry. Rebuilt on every call;
     * cleared on destroy. */
    mcp_client_xmcp_param_t *xmcp_params;
    size_t n_xmcp_params;
    size_t cap_xmcp_params;
};

#endif
