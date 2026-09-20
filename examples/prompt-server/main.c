#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static mcp_status_t greet_prompt(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args_or_null, void *user_data,
                                 mcp_json_value_t **messages_out) {
    (void)session;
    (void)user_data;
    const char *name = "friend";
    if (args_or_null != NULL) {
        const mcp_json_value_t *v = mcp_json_object_get(ctx, args_or_null, "name");
        if (v != NULL) {
            mcp_json_string_value(ctx, v, &name);
        }
    }
    /* Build messages = [{ "role":"user", "content":{"type":"text","text":"Hello <name>"} }]
     * object_set / array_append take ownership of the child on MCP_OK; on failure the caller
     * retains it, so every unadopted value is destroyed on the error path.
     */
    mcp_json_value_t *role = mcp_json_string_new(ctx, "user");
    mcp_json_value_t *c_type = mcp_json_string_new(ctx, "text");
    mcp_json_value_t *msg = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_object_new(ctx);
    mcp_json_value_t *msgs = mcp_json_array_new(ctx);
    const mcp_allocator_t *alloc = mcp_context_allocator(ctx);
    size_t nlen = strlen(name);
    char *text_buf = alloc->malloc_fn(6 + nlen + 1, alloc->userdata);
    mcp_json_value_t *c_text = NULL;
    if (role == NULL || c_type == NULL || msg == NULL || content == NULL || msgs == NULL ||
        text_buf == NULL) {
        alloc->free_fn(text_buf, alloc->userdata);
        mcp_json_destroy(ctx, role);
        mcp_json_destroy(ctx, c_type);
        mcp_json_destroy(ctx, msg);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, msgs);
        return MCP_ERR_NOMEM;
    }
    snprintf(text_buf, 6 + nlen + 1, "Hello %s", name);
    c_text = mcp_json_string_new_n(ctx, text_buf, 6 + nlen);
    alloc->free_fn(text_buf, alloc->userdata);
    if (c_text == NULL ||
        mcp_json_object_set(ctx, msg, "role", role) != MCP_OK ||
        mcp_json_object_set(ctx, content, "type", c_type) != MCP_OK ||
        mcp_json_object_set(ctx, content, "text", c_text) != MCP_OK ||
        mcp_json_object_set(ctx, msg, "content", content) != MCP_OK ||
        mcp_json_array_append(ctx, msgs, msg) != MCP_OK) {
        mcp_json_destroy(ctx, role);
        mcp_json_destroy(ctx, c_type);
        mcp_json_destroy(ctx, c_text);
        mcp_json_destroy(ctx, msg);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, msgs);
        return MCP_ERR_NOMEM;
    }
    *messages_out = msgs;
    return MCP_OK;
}

int main(void) {
    /* A closed peer must surface as EPIPE/MCP_ERR_IO, not a SIGPIPE kill. */
    signal(SIGPIPE, SIG_IGN);
    int rc = 1;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        fprintf(stderr, "context create failed\n");
        return 1;
    }
    mcp_server_t *srv = mcp_server_create(ctx, "prompt-server", "0.1.0");
    if (srv == NULL) {
        fprintf(stderr, "server create failed\n");
        goto done;
    }
    if (mcp_server_add_prompt(ctx, srv,
                              mcp_prompt_new(ctx, "greet", "Greeting prompt with optional name",
                                             greet_prompt, NULL)) != MCP_OK) {
        fprintf(stderr, "prompt register failed\n");
        goto done;
    }
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, NULL, NULL);
    if (t == NULL) {
        fprintf(stderr, "transport create failed\n");
        goto done;
    }
    if (mcp_transport_start(ctx, t) != MCP_OK) {
        fprintf(stderr, "transport start failed\n");
        goto done_transport;
    }
    if (mcp_stdio_serve(ctx, srv, t) != MCP_OK) {
        fprintf(stderr, "serve failed\n");
        goto done_transport;
    }
    rc = 0;

done_transport:
    mcp_transport_stop(ctx, t);
    mcp_transport_destroy(ctx, t);
done:
    if (srv != NULL) {
        mcp_server_destroy(ctx, srv);
    }
    mcp_context_destroy(ctx);
    return rc;
}
