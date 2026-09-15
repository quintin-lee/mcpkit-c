#ifndef MCPKIT_TRANSPORT_HTTP_H
#define MCPKIT_TRANSPORT_HTTP_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_http_request mcp_http_request_t;
typedef struct mcp_http_response mcp_http_response_t;

typedef enum {
    MCP_HTTP_GET,
    MCP_HTTP_POST,
    MCP_HTTP_DELETE,
    MCP_HTTP_UNKNOWN,
} mcp_http_method_t;

#define MCP_HTTP_MAX_HEADERS 64
#define MCP_HTTP_MAX_LINE 8192
#define MCP_HTTP_MAX_BODY (4u << 20)

mcp_http_request_t *mcp_http_parse_request(mcp_context_t *ctx, const char *data, size_t len);
void mcp_http_request_destroy(mcp_context_t *ctx, mcp_http_request_t *req);
mcp_http_method_t mcp_http_request_method(mcp_context_t *ctx, const mcp_http_request_t *req);
const char *mcp_http_request_target(mcp_context_t *ctx, const mcp_http_request_t *req);
const char *mcp_http_header(mcp_context_t *ctx, const mcp_http_request_t *req, const char *name);
const char *mcp_http_request_body(mcp_context_t *ctx, const mcp_http_request_t *req, size_t *len_out);

mcp_http_response_t *mcp_http_response_new(mcp_context_t *ctx, int status, const char *reason);
mcp_status_t mcp_http_response_set_header(mcp_context_t *ctx, mcp_http_response_t *resp,
                                          const char *name, const char *value);
mcp_status_t mcp_http_response_set_body(mcp_context_t *ctx, mcp_http_response_t *resp,
                                        const char *body, size_t len);
const char *mcp_http_response_serialize(mcp_context_t *ctx, mcp_http_response_t *resp);
void mcp_http_response_destroy(mcp_context_t *ctx, mcp_http_response_t *resp);

#endif
