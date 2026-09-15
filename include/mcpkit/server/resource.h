#ifndef MCPKIT_SERVER_RESOURCE_H
#define MCPKIT_SERVER_RESOURCE_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_resource mcp_resource_t;

typedef mcp_status_t (*mcp_resource_read_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                              const char *uri, void *user_data,
                                              mcp_json_value_t **contents_out);

typedef void (*mcp_resource_cleanup_fn)(mcp_context_t *ctx, void *user_data);

mcp_resource_t *mcp_resource_new(mcp_context_t *ctx, const char *uri, const char *name,
                                 const char *mime_type_or_null, mcp_resource_read_fn on_read,
                                 void *user_data);
void mcp_resource_destroy(mcp_context_t *ctx, mcp_resource_t *res);
mcp_status_t mcp_resource_set_cleanup(mcp_context_t *ctx, mcp_resource_t *res,
                                      mcp_resource_cleanup_fn fn_or_null);

#endif
