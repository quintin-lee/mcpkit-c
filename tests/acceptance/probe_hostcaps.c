/* P2-3 acceptance peer: canned roots + sampling provider.
 *
 * Connects to host-caps over stdio, calls ask_model, then answers the
 * server's roots/list and sampling/createMessage requests with canned
 * values, printing PEER_ROOTS_OK / PEER_SAMPLING_OK markers to stderr.
 * Exits 0 once both markers are printed, 1 on transport error or after
 * 64 messages without both. The acceptance script wraps this in timeout
 * as the hang backstop. Not installed; built for tests only. */
#define _DEFAULT_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

#define PEER_ROOT_URI "file:///tmp/host-caps-root"

static mcp_json_value_t *roots_provider(mcp_context_t *ctx, void *user_data) {
    (void)user_data;
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    mcp_json_value_t *root = mcp_json_object_new(ctx);
    mcp_json_value_t *uri = mcp_json_string_new(ctx, PEER_ROOT_URI);
    mcp_json_value_t *name = mcp_json_string_new(ctx, "peer-root");
    if (arr == NULL || root == NULL || uri == NULL || name == NULL) {
        goto fail;
    }
    /* Each set/append takes ownership on success; NULL it out so fail
     * only destroys what is still owned here. */
    if (mcp_json_object_set(ctx, root, "uri", uri) != MCP_OK) {
        goto fail;
    }
    uri = NULL;
    if (mcp_json_object_set(ctx, root, "name", name) != MCP_OK) {
        goto fail;
    }
    name = NULL;
    if (mcp_json_array_append(ctx, arr, root) != MCP_OK) {
        goto fail;
    }
    root = NULL;
    return arr;
fail:
    mcp_json_destroy(ctx, arr);
    mcp_json_destroy(ctx, root);
    mcp_json_destroy(ctx, uri);
    mcp_json_destroy(ctx, name);
    return NULL;
}

static mcp_json_value_t *sample_provider(mcp_context_t *ctx,
                                         const mcp_json_value_t *params,
                                         void *user_data) {
    (void)params;
    (void)user_data;
    mcp_json_value_t *res = mcp_json_object_new(ctx);
    mcp_json_value_t *role = mcp_json_string_new(ctx, "assistant");
    mcp_json_value_t *model = mcp_json_string_new(ctx, "canned");
    mcp_json_value_t *stop = mcp_json_string_new(ctx, "endTurn");
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *block = mcp_json_object_new(ctx);
    mcp_json_value_t *btype = mcp_json_string_new(ctx, "text");
    mcp_json_value_t *btext = mcp_json_string_new(ctx, "canned");
    if (res == NULL || role == NULL || model == NULL || stop == NULL ||
        content == NULL || block == NULL || btype == NULL || btext == NULL) {
        goto fail;
    }
    if (mcp_json_object_set(ctx, block, "type", btype) != MCP_OK) {
        goto fail;
    }
    btype = NULL;
    if (mcp_json_object_set(ctx, block, "text", btext) != MCP_OK) {
        goto fail;
    }
    btext = NULL;
    if (mcp_json_array_append(ctx, content, block) != MCP_OK) {
        goto fail;
    }
    block = NULL;
    if (mcp_json_object_set(ctx, res, "role", role) != MCP_OK) {
        goto fail;
    }
    role = NULL;
    if (mcp_json_object_set(ctx, res, "content", content) != MCP_OK) {
        goto fail;
    }
    content = NULL;
    if (mcp_json_object_set(ctx, res, "model", model) != MCP_OK) {
        goto fail;
    }
    model = NULL;
    if (mcp_json_object_set(ctx, res, "stopReason", stop) != MCP_OK) {
        goto fail;
    }
    stop = NULL;
    return res;
fail:
    mcp_json_destroy(ctx, btype);
    mcp_json_destroy(ctx, btext);
    mcp_json_destroy(ctx, block);
    mcp_json_destroy(ctx, content);
    mcp_json_destroy(ctx, role);
    mcp_json_destroy(ctx, model);
    mcp_json_destroy(ctx, stop);
    mcp_json_destroy(ctx, res);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    int rc = 1;
    mcp_context_t *ctx = mcp_context_create(NULL);
    if (ctx == NULL) {
        return 1;
    }
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, NULL, NULL);
    mcp_client_t *c = (t != NULL) ? mcp_client_create(ctx, t) : NULL;
    if (c == NULL) {
        mcp_transport_destroy(ctx, t);
        mcp_context_destroy(ctx);
        return 1;
    }
    if (mcp_client_connect(ctx, c) != MCP_OK ||
        mcp_client_initialize(ctx, c, "probe-hostcaps", "0.1.0", NULL) != MCP_OK) {
        fprintf(stderr, "peer: connect/initialize failed\n");
        goto done;
    }
    mcp_client_set_roots_provider(ctx, c, roots_provider, NULL);
    mcp_client_set_sample_provider(ctx, c, sample_provider, NULL);

    mcp_json_value_t *args = mcp_json_object_new(ctx);
    mcp_json_value_t *res = NULL;
    if (args == NULL ||
        mcp_client_call_tool(ctx, c, "ask_model", args, &res) != MCP_OK) {
        fprintf(stderr, "peer: ask_model call failed\n");
        mcp_json_destroy(ctx, args);
        goto done;
    }
    char *rss = mcp_json_serialize(ctx, res);
    if (rss != NULL) {
        printf("%s\n", rss);
        mcp_json_free_string(ctx, rss);
    }
    mcp_json_destroy(ctx, res);

    int saw_roots = 0;
    int saw_sampling = 0;
    for (int i = 0; i < 64 && !(saw_roots && saw_sampling); i++) {
        mcp_message_t *msg = NULL;
        if (mcp_client_recv_message(ctx, c, &msg) != MCP_OK || msg == NULL) {
            break;
        }
        if (mcp_message_kind(ctx, msg) == MCP_MSG_REQUEST) {
            const char *method = mcp_message_method(ctx, msg);
            mcp_message_t *resp = NULL;
            if (mcp_client_handle_server_request(ctx, c, msg, &resp) == MCP_OK &&
                resp != NULL) {
                char *line = mcp_message_serialize(ctx, resp);
                if (line != NULL) {
                    mcp_transport_send(ctx, t, line, strlen(line));
                    mcp_json_free_string(ctx, line);
                }
                mcp_message_destroy(ctx, resp);
            }
            if (method != NULL) {
                if (strcmp(method, "roots/list") == 0) {
                    saw_roots = 1;
                    fprintf(stderr, "PEER_ROOTS_OK\n");
                } else if (strcmp(method, "sampling/createMessage") == 0) {
                    saw_sampling = 1;
                    fprintf(stderr, "PEER_SAMPLING_OK\n");
                }
            }
        }
        mcp_message_destroy(ctx, msg);
    }
    rc = (saw_roots && saw_sampling) ? 0 : 1;

done:
    mcp_client_disconnect(ctx, c);
    mcp_client_destroy(ctx, c);
    mcp_transport_destroy(ctx, t);
    mcp_context_destroy(ctx);
    return rc;
}
