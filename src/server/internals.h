/**
 * @file internals.h
 *
 * Internal struct layouts and shared helpers for the server layer.
 * NOT a public header: exposed here only so the dispatcher, session,
 * and registry translation units can share the opaque definitions.
 *
 * - Struct definitions for mcp_tool/resource/prompt/session/server/queue.
 * - srv_malloc/srv_realloc/srv_free/srv_strdup: ctx-routed allocation
 *   helpers that fall back to the default allocator on NULL ctx.
 */
#ifndef MCPKIT_SERVER_INTERNALS_H
#define MCPKIT_SERVER_INTERNALS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/context.h"
#include "mcpkit/protocol/validate.h"
#include "mcpkit/server/resource.h"
#include "mcpkit/server/prompt.h"
#include "mcpkit/server/tool.h"
#include "mcpkit/server/server.h"

typedef struct {
    char *ref_prefix;
    mcp_completion_provider_fn fn;
    void *user_data;
} mcp_completion_entry_t;

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
    mcp_completion_entry_t *completions;
    size_t n_completions;
    size_t cap_completions;
    mcp_session_t **sessions;
    size_t n_sessions;
    size_t cap_sessions;
    atomic_ullong c_requests_total;
    atomic_ullong c_requests_error;
    atomic_ullong c_notifications_total;
    atomic_ullong c_tools_called;
    mcp_trace_fn tracer;
    void *tracer_ud;
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
