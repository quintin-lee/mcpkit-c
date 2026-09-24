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

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/value.h"
#include "mcpkit/logging/log.h"
#include "mcpkit/protocol/validate.h"
#include "mcpkit/server/resource.h"
#include "mcpkit/server/prompt.h"
#include "mcpkit/server/tool.h"
#include "mcpkit/server/server.h"
#include "mcpkit/protocol/tasks.h"
#include "mcpkit/protocol/skills.h"

typedef struct {
    char *ref_prefix;
    mcp_completion_provider_fn fn;
    void *user_data;
} mcp_completion_entry_t;

struct mcp_tool {
    char *name;
    char *description;
    mcp_json_value_t *schema;
    mcp_tool_handler_fn handler;     /* V1; NULL when handler_v2 is set */
    mcp_tool_handler_v2_fn handler_v2; /* V2 (MRTR); NULL for V1 tools */
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
    // Deep clone of the "_meta" field carried in the initialize request.
    // NULL if the client did not send one. Destroyed with the session.
    mcp_json_value_t *client_meta;
    // Active subscription state (Phase 2, SEP-2575 subscriptions/listen)
    bool subscription_active;
    mcp_id_type_t sub_id_type;
    char *sub_id_str;
    double sub_id_num;
    bool sub_tools_list_changed;
    bool sub_prompts_list_changed;
    bool sub_resources_list_changed;
    char **sub_resource_uris;
    size_t n_sub_resource_uris;
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
    // Outbox: caller-pushed server-originated notifications awaiting a
    // transport flush. Growable; drained by serve loops (stdio/loop) or by
    // a host that owns its own transport. Destroyed with the server.
    mcp_message_t **outbox;
    size_t n_outbox;
    size_t cap_outbox;
    // Advanced-method state (Phase 5): log floor consulted by dlogf on
    // dispatch routes, the resource-subscription uri set, and the
    // server-originated request id counter (starts 1.0, monotonic per-server).
    // log_floor is atomic so dlogf_srv can read it without a lock;
    // subscribed_uris mutations are guarded by subscribed_lock.
    atomic_int log_floor;
    char **subscribed_uris;
    size_t n_subscribed;
    size_t cap_subscribed;
    double next_server_id;
    // List-response cache parameters; 0/NULL = omit from list responses.
    // Set via mcp_server_set_list_cache; overridable; no lock needed
    // (read in dispatch threads, written by host before serving starts).
    uint64_t list_ttl_ms;
    char *list_cache_scope;
    // Host-supplied _meta object injected into list/discover responses.
    // Cloned at set time; NULL = no _meta injection.
    mcp_json_value_t *response_meta;
    // Guards all reads/writes of subscribed_uris, n_subscribed, cap_subscribed
    // from concurrent dispatch threads (route_advanced subscribe/unsubscribe).
    pthread_mutex_t subscribed_lock;
    // Guards sessions array and n_sessions/cap_sessions from concurrent worker threads.
    pthread_mutex_t sessions_lock;
    // MCP Tasks extension manager (SEP-2663). NULL if not enabled.
    mcp_task_mgr_t *task_mgr;
    // MCP Skills extension registry (SEP-2640). NULL if not enabled.
    mcp_skill_registry_t *skill_reg;
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
