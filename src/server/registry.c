/**
 * @file registry.c
 *
 * Tool/resource/prompt constructors and destructors. On a successful
 * *_new the returned handle is caller-owned and carries ownership of
 * its schema/user-data; on allocation failure nothing is leaked and
 * NULL is returned. *_destroy frees the handle and any owned schema
 * (but not the user_data pointer, which the caller owns).
 */
#include "mcpkit/server/tool.h"

#include <string.h>

#include "internals.h"
#include "mcpkit/json/value.h"
#include "mcpkit/server/prompt.h"
#include "mcpkit/server/resource.h"

mcp_tool_t *mcp_tool_new(mcp_context_t *ctx, const char *name, const char *description,
                         mcp_json_value_t *input_schema, mcp_tool_handler_fn handler,
                         void *user_data) {
    if (name == NULL || handler == NULL) {
        return NULL;
    }
    mcp_tool_t *tool = srv_malloc(ctx, sizeof(*tool));
    if (tool == NULL) {
        return NULL;
    }
    tool->name = srv_strdup(ctx, name);
    tool->description = srv_strdup(ctx, description);
    if (tool->name == NULL || (description != NULL && tool->description == NULL)) {
        srv_free(ctx, tool->name);
        srv_free(ctx, tool->description);
        srv_free(ctx, tool);
        return NULL;
    }
    tool->schema = input_schema;
    tool->handler = handler;
    tool->handler_v2 = NULL;
    tool->user_data = user_data;
    tool->vis = MCP_TOOL_VIS_BOTH;
    tool->required = 0;
    return tool;
}

void mcp_tool_destroy(mcp_context_t *ctx, mcp_tool_t *tool) {
    if (tool == NULL) {
        return;
    }
    srv_free(ctx, tool->name);
    srv_free(ctx, tool->description);
    mcp_json_destroy(ctx, tool->schema);
    srv_free(ctx, tool);
}

mcp_tool_t *mcp_tool_new_v2(mcp_context_t *ctx, const char *name, const char *description,
                             mcp_json_value_t *input_schema, mcp_tool_handler_v2_fn handler,
                             void *user_data) {
    if (name == NULL || handler == NULL) {
        mcp_json_destroy(ctx, input_schema);
        return NULL;
    }
    mcp_tool_t *tool = srv_malloc(ctx, sizeof(*tool));
    if (tool == NULL) {
        mcp_json_destroy(ctx, input_schema);
        return NULL;
    }
    tool->name = srv_strdup(ctx, name);
    tool->description = srv_strdup(ctx, description);
    if (tool->name == NULL || (description != NULL && tool->description == NULL)) {
        srv_free(ctx, tool->name);
        srv_free(ctx, tool->description);
        srv_free(ctx, tool);
        mcp_json_destroy(ctx, input_schema);
        return NULL;
    }
    tool->schema = input_schema;
    tool->handler = NULL;
    tool->handler_v2 = handler;
    tool->user_data = user_data;
    tool->vis = MCP_TOOL_VIS_BOTH;
    tool->required = 0;
    return tool;
}

mcp_status_t mcp_tool_set_visibility(mcp_context_t *ctx, mcp_tool_t *tool,
                                     mcp_tool_visibility_t vis) {
    if (ctx == NULL || tool == NULL || vis > MCP_TOOL_VIS_BOTH) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    tool->vis = vis;
    return MCP_OK;
}

mcp_status_t mcp_tool_require_perms(mcp_context_t *ctx, mcp_tool_t *tool, uint32_t perm_mask) {
    if (ctx == NULL || tool == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    tool->required = perm_mask;
    return MCP_OK;
}

mcp_resource_t *mcp_resource_new(mcp_context_t *ctx, const char *uri, const char *name,
                                const char *mime_type_or_null, mcp_resource_read_fn on_read,
                                void *user_data) {
    if (uri == NULL || name == NULL) {
        return NULL;
    }
    mcp_resource_t *res = srv_malloc(ctx, sizeof(*res));
    if (res == NULL) {
        return NULL;
    }
    res->uri = srv_strdup(ctx, uri);
    res->name = srv_strdup(ctx, name);
    res->mime_type = srv_strdup(ctx, mime_type_or_null);
    if (res->uri == NULL || res->name == NULL ||
        (mime_type_or_null != NULL && res->mime_type == NULL)) {
        srv_free(ctx, res->uri);
        srv_free(ctx, res->name);
        srv_free(ctx, res->mime_type);
        srv_free(ctx, res);
        return NULL;
    }
    res->on_read = on_read;
    res->user_data = user_data;
    res->cleanup = NULL;
    return res;
}

void mcp_resource_destroy(mcp_context_t *ctx, mcp_resource_t *res) {
    if (res == NULL) {
        return;
    }
    if (res->cleanup != NULL) {
        res->cleanup(ctx, res->user_data);
    }
    srv_free(ctx, res->uri);
    srv_free(ctx, res->name);
    srv_free(ctx, res->mime_type);
    srv_free(ctx, res);
}

mcp_status_t mcp_resource_set_cleanup(mcp_context_t *ctx, mcp_resource_t *res,
                                      mcp_resource_cleanup_fn fn_or_null) {
    if (ctx == NULL || res == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    res->cleanup = fn_or_null;
    return MCP_OK;
}

mcp_prompt_t *mcp_prompt_new(mcp_context_t *ctx, const char *name, const char *description_or_null,
                            mcp_prompt_get_fn on_get, void *user_data) {
    if (name == NULL) {
        return NULL;
    }
    mcp_prompt_t *prompt = srv_malloc(ctx, sizeof(*prompt));
    if (prompt == NULL) {
        return NULL;
    }
    prompt->name = srv_strdup(ctx, name);
    prompt->description = srv_strdup(ctx, description_or_null);
    if (prompt->name == NULL ||
        (description_or_null != NULL && prompt->description == NULL)) {
        srv_free(ctx, prompt->name);
        srv_free(ctx, prompt->description);
        srv_free(ctx, prompt);
        return NULL;
    }
    prompt->on_get = on_get;
    prompt->user_data = user_data;
    return prompt;
}

void mcp_prompt_destroy(mcp_context_t *ctx, mcp_prompt_t *prompt) {
    if (prompt == NULL) {
        return;
    }
    srv_free(ctx, prompt->name);
    srv_free(ctx, prompt->description);
    srv_free(ctx, prompt);
}
