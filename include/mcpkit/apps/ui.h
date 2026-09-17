#ifndef MCPKIT_APPS_UI_H
#define MCPKIT_APPS_UI_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

/**
 * @file ui.h
 * MCP Apps extension: ui:// HTML resources with per-resource CSP and
 * mount/unmount lifecycle callbacks.
 *
 * Ownership:
 * - mcp_apps_ui_resource_new(): returns a caller-owned mcp_resource_t.
 *   The CSP snapshot is taken at creation time; subsequent CSP mutations
 *   do not affect an already-created resource. csp_or_null may be NULL,
 *   in which case a default-deny policy ("none") is used.
 * - mcp_apps_result_with_ui(): stamps _meta.ui.resourceUri onto the
 *   provided result JSON object. The result is NOT cloned; the caller
 *   retains ownership.
 * - mcp_apps_mount(): if on_mount is non-NULL it is invoked IMMEDIATELY
 *   (synchronously, before mount returns). handle_out receives a
 *   caller-owned handle; mcp_apps_unmount() invokes on_unmount and frees
 *   the handle.
 */

#define MCP_APPS_UI_MIME "text/html;profile=mcp-app"
#define MCP_APPS_UI_SCHEME "ui://"

/*
 * Permission bits used by mcp_session_grant/revoke and
 * mcp_tool_require_perms.
 *   MCP_APPS_PERM_CALL_TOOL  - (1u << 0): permission to call tools
 *   MCP_APPS_PERM_READ_STATE - (1u << 1): permission to read app state
 */
#define MCP_APPS_PERM_CALL_TOOL (1u << 0)
#define MCP_APPS_PERM_READ_STATE (1u << 1)

typedef struct mcp_apps_mount mcp_apps_mount_t;
typedef struct mcp_context mcp_context_t;
typedef struct mcp_csp mcp_csp_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_session mcp_session_t;

/**
 * Creates a ui:// resource backed by an HTML document with a CSP policy.
 * The reader handler returns contents[{uri, mimeType, text}] where text
 * is the HTML document.
 */
mcp_resource_t *mcp_apps_ui_resource_new(mcp_context_t *ctx, const char *uri,
                                         const char *name,
                                         const char *html,
                                         const mcp_csp_t *csp_or_null);

/**
 * Adds _meta.ui.resourceUri to the result object. If _meta.ui is already
 * present the existing resourceUri is overwritten (documented behavior).
 */
mcp_status_t mcp_apps_result_with_ui(mcp_context_t *ctx, mcp_json_value_t *result,
                                     const char *resource_uri);

/**
 * Lifecycle callback. Invoked on mount (if non-NULL) and on unmount (if
 * non-NULL).
 */
typedef void (*mcp_apps_lifecycle_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                       void *user_data);

/**
 * Registers a mount on the session. on_mount, if non-NULL, is called
 * synchronously before this function returns. *handle_out receives a
 * caller-owned mount handle.
 */
mcp_status_t mcp_apps_mount(mcp_context_t *ctx, mcp_session_t *session,
                            mcp_apps_lifecycle_fn on_mount_or_null,
                            mcp_apps_lifecycle_fn on_unmount_or_null,
                            void *user_data,
                            mcp_apps_mount_t **handle_out);

/**
 * Unmounts and frees the handle. Invokes on_unmount if registered.
 * MCP_ERR_INVALID_ARGUMENT if handle is NULL.
 */
mcp_status_t mcp_apps_unmount(mcp_context_t *ctx, mcp_apps_mount_t *handle);

#endif
