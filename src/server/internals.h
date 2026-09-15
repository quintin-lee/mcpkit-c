#ifndef MCPKIT_SERVER_INTERNALS_H
#define MCPKIT_SERVER_INTERNALS_H

#include <stdbool.h>
#include <stddef.h>

#include "mcpkit/core/context.h"
#include "mcpkit/protocol/validate.h"
#include "mcpkit/server/resource.h"
#include "mcpkit/server/prompt.h"
#include "mcpkit/server/tool.h"

struct mcp_tool {
    char *name;
    char *description;
    mcp_json_value_t *schema;
    mcp_tool_handler_fn handler;
    void *user_data;
    mcp_tool_visibility_t vis;
    uint32_t required;
};

struct mcp_resource {
    char *uri;
    char *name;
    char *mime_type;
    mcp_resource_read_fn on_read;
    void *user_data;
    mcp_resource_cleanup_fn cleanup;
};

struct mcp_prompt {
    char *name;
    char *description;
    mcp_prompt_get_fn on_get;
    void *user_data;
};

struct mcp_session {
    bool initialized;
    bool apps_host;
    uint32_t granted;
    char *client_name;
    char *client_version;
    mcp_idset_t *ids;
};

struct mcp_server {
    char *name;
    char *version;
    mcp_tool_t **tools;
    size_t n_tools;
    size_t cap_tools;
    mcp_resource_t **resources;
    size_t n_resources;
    size_t cap_resources;
    mcp_prompt_t **prompts;
    size_t n_prompts;
    size_t cap_prompts;
    mcp_session_t **sessions;
    size_t n_sessions;
    size_t cap_sessions;
};

struct mcp_queue {
    mcp_session_t **sessions;
    mcp_message_t **msgs;
    size_t head;
    size_t len;
    size_t cap;
};

// Shared allocation helpers (server.c). NULL ctx falls back to the
// default allocator via mcp_context_allocator; destroy with the same
// ctx used at creation when a counting allocator is in play.
void *srv_malloc(mcp_context_t *ctx, size_t n);
void *srv_realloc(mcp_context_t *ctx, void *ptr, size_t n);
void srv_free(mcp_context_t *ctx, void *ptr);
char *srv_strdup(mcp_context_t *ctx, const char *s);

// Defined in session.c; frees one session struct. server.c uses it to
// drain the session list, mcp_server_destroy_session uses it after detach.
void session_free(mcp_context_t *ctx, mcp_session_t *s);

#endif
