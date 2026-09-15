#ifndef MCPKIT_APPS_UI_H
#define MCPKIT_APPS_UI_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

#define MCP_APPS_UI_MIME "text/html;profile=mcp-app"
#define MCP_APPS_UI_SCHEME "ui://"

#define MCP_APPS_PERM_CALL_TOOL (1u << 0)
#define MCP_APPS_PERM_READ_STATE (1u << 1)

typedef struct mcp_apps_mount mcp_apps_mount_t;
typedef struct mcp_context mcp_context_t;
typedef struct mcp_csp mcp_csp_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_session mcp_session_t;

mcp_resource_t *mcp_apps_ui_resource_new(mcp_context_t *ctx, const char *uri, const char *name,
                                         const char *html, const mcp_csp_t *csp_or_null);
mcp_status_t mcp_apps_result_with_ui(mcp_context_t *ctx, mcp_json_value_t *result,
                                     const char *resource_uri);

typedef void (*mcp_apps_lifecycle_fn)(mcp_context_t *ctx, mcp_session_t *session, void *user_data);
mcp_status_t mcp_apps_mount(mcp_context_t *ctx, mcp_session_t *session,
                            mcp_apps_lifecycle_fn on_mount_or_null,
                            mcp_apps_lifecycle_fn on_unmount_or_null, void *user_data,
                            mcp_apps_mount_t **handle_out);
mcp_status_t mcp_apps_unmount(mcp_context_t *ctx, mcp_apps_mount_t *handle);

#endif
