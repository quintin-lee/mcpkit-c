/**
 * @file routes.c
 * @brief MCP server method route handlers and response decorators.
 */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internals.h"
#include "../method_table.h"
#include "mcpkit/core/context.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/schema.h"
#include "mcpkit/logging/logger.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/mrtr.h"
#include "mcpkit/protocol/policy.h"
#include "mcpkit/protocol/uri_template.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"
#include "mcpkit/server/tool.h"

#define MCP_LIST_PAGE_SIZE 100u

static mcp_message_t *err_resp(mcp_context_t *ctx, const mcp_message_t *req, int code,
                               const char *text) {
    return mcp_response_err_new(ctx, req, code, text, NULL);
}

static size_t page_offset(mcp_context_t *ctx, const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const mcp_json_value_t *cur = params != NULL ? mcp_json_object_get(ctx, params, "cursor") : NULL;
    if (cur == NULL) {
        return 0;
    }
    const char *s = NULL;
    if (mcp_json_type(ctx, cur) != MCP_JSON_STRING ||
        mcp_json_string_value(ctx, cur, &s) != MCP_OK || s == NULL || *s == '\0') {
        return (size_t)-1;
    }
    size_t off = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return (size_t)-1;
        }
        off = off * 10u + (size_t)(*p - '0');
    }
    return off;
}

static mcp_status_t set_next_cursor(mcp_context_t *ctx, mcp_json_value_t *result, size_t end) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%zu", end);
    mcp_json_value_t *v = mcp_json_string_new(ctx, buf);
    if (v == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set_take(ctx, result, "nextCursor", v) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

static mcp_status_t set_string(mcp_context_t *ctx, mcp_json_value_t *obj, const char *key,
                               const char *val) {
    mcp_json_value_t *v = mcp_json_string_new(ctx, val);
    if (v == NULL) {
        return MCP_ERR_NOMEM;
    }
    if (mcp_json_object_set(ctx, obj, key, v) != MCP_OK) {
        mcp_json_destroy(ctx, v);
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

static mcp_status_t get_string(mcp_context_t *ctx, const mcp_json_value_t *obj,
                               const char *key, const char **out) {
    const mcp_json_value_t *v = mcp_json_object_get(ctx, obj, key);
    if (v == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    return mcp_json_string_value(ctx, v, out);
}

static mcp_tool_t *find_tool(mcp_server_t *srv, const char *name) {
    for (size_t i = 0; i < srv->n_tools; i++) {
        if (strcmp(srv->tools[i]->name, name) == 0) {
            return srv->tools[i];
        }
    }
    return NULL;
}

static mcp_resource_t *find_resource(mcp_server_t *srv, const char *uri) {
    for (size_t i = 0; i < srv->n_resources; i++) {
        if (strcmp(srv->resources[i]->uri, uri) == 0) {
            return srv->resources[i];
        }
    }
    return NULL;
}

static mcp_prompt_t *find_prompt(mcp_server_t *srv, const char *name) {
    for (size_t i = 0; i < srv->n_prompts; i++) {
        if (strcmp(srv->prompts[i]->name, name) == 0) {
            return srv->prompts[i];
        }
    }
    return NULL;
}

static int tool_cmp_name(const void *a, const void *b) {
    const mcp_tool_t *ta = *(const mcp_tool_t * const *)a;
    const mcp_tool_t *tb = *(const mcp_tool_t * const *)b;
    const char *na = (ta != NULL && ta->name != NULL) ? ta->name : "";
    const char *nb = (tb != NULL && tb->name != NULL) ? tb->name : "";
    return strcmp(na, nb);
}

static int resource_cmp_name(const void *a, const void *b) {
    const mcp_resource_t *ra = *(const mcp_resource_t * const *)a;
    const mcp_resource_t *rb = *(const mcp_resource_t * const *)b;
    const char *na = (ra != NULL && ra->name != NULL) ? ra->name : "";
    const char *nb = (rb != NULL && rb->name != NULL) ? rb->name : "";
    return strcmp(na, nb);
}

static int prompt_cmp_name(const void *a, const void *b) {
    const mcp_prompt_t *pa = *(const mcp_prompt_t * const *)a;
    const mcp_prompt_t *pb = *(const mcp_prompt_t * const *)b;
    const char *na = (pa != NULL && pa->name != NULL) ? pa->name : "";
    const char *nb = (pb != NULL && pb->name != NULL) ? pb->name : "";
    return strcmp(na, nb);
}

// Internal helper: decorates a result object before mcp_response_ok_new.
// - is_list=true:  inject resultType + ttlMs + cacheScope + _meta
// - is_list=false: inject resultType only
// NOMEM: caller must destroy result and return NULL from the route.
static mcp_status_t decorate_result(mcp_context_t *ctx, mcp_server_t *srv,
                                    mcp_session_t *s,
                                    mcp_json_value_t *result, bool is_list) {
    if (mcp_result_inject_result_type(ctx, result) != MCP_OK) {
        return MCP_ERR_NOMEM;
    }
    if (!is_list) {
        return MCP_OK;
    }
    bool is_2026 = mcp_proto_policy_cache_defaults_applied(ctx, mcp_session_negotiated(ctx, s));
    if (srv->list_ttl_ms != 0) {
        mcp_json_value_t *ttl = mcp_json_number_new(ctx, (double)srv->list_ttl_ms);
        if (ttl == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, result, "ttlMs", ttl) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    } else if (is_2026) {
        mcp_json_value_t *ttl = mcp_json_number_new(ctx, 0.0);
        if (ttl == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, result, "ttlMs", ttl) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    }
    if (srv->list_cache_scope != NULL) {
        mcp_json_value_t *scope = mcp_json_string_new(ctx, srv->list_cache_scope);
        if (scope == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, result, "cacheScope", scope) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
    } else if (is_2026) {
        mcp_json_value_t *scope = mcp_json_string_new(ctx, "private");
        if (scope == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, result, "cacheScope", scope) != MCP_OK) {
            return MCP_ERR_NOMEM;
        }
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=cache_hint_default version=2026");
    }
    if (srv->response_meta != NULL) {
        mcp_json_value_t *meta_clone = mcp_json_clone(ctx, srv->response_meta);
        if (meta_clone == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_result_inject_meta(ctx, result, meta_clone) != MCP_OK) {
            // set_take consumed meta_clone on failure; do not free again.
            return MCP_ERR_NOMEM;
        }
    }
    return MCP_OK;
}

static mcp_message_t *route_server_discover(mcp_context_t *ctx, mcp_server_t *srv,
                                              mcp_session_t *s,
                                              const mcp_message_t *req) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return NULL;
    }
    mcp_json_value_t *v;
    if ((v = mcp_json_string_new(ctx, srv->name)) == NULL ||
        mcp_json_object_set_take(ctx, result, "serverName", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_string_new(ctx, srv->version)) == NULL ||
        mcp_json_object_set_take(ctx, result, "serverVersion", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    const char *pver = mcp_proto_version_string(mcp_session_negotiated(ctx, s));
    if (pver == NULL) {
        pver = MCP_PROTOCOL_VERSION_LATEST;
    }
    if ((v = mcp_json_string_new(ctx, pver)) == NULL ||
        mcp_json_object_set_take(ctx, result, "protocolVersion", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_number_new(ctx, (double)srv->n_tools)) == NULL ||
        mcp_json_object_set_take(ctx, result, "toolsCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_number_new(ctx, (double)srv->n_resources)) == NULL ||
        mcp_json_object_set_take(ctx, result, "resourcesCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_number_new(ctx, (double)srv->n_prompts)) == NULL ||
        mcp_json_object_set_take(ctx, result, "promptsCount", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (srv->list_ttl_ms != 0) {
        if ((v = mcp_json_number_new(ctx, (double)srv->list_ttl_ms)) == NULL ||
            mcp_json_object_set_take(ctx, result, "listTtlMs", v) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    if (srv->list_cache_scope != NULL) {
        if ((v = mcp_json_string_new(ctx, srv->list_cache_scope)) == NULL ||
            mcp_json_object_set_take(ctx, result, "listCacheScope", v) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    if ((v = mcp_json_bool_new(ctx, true)) == NULL ||
        mcp_json_object_set_take(ctx, result, "supportsStateless", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if ((v = mcp_json_bool_new(ctx, true)) == NULL ||
        mcp_json_object_set_take(ctx, result, "supportsMeta", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (srv->task_mgr != NULL || srv->skill_reg != NULL) {
        mcp_json_value_t *caps = (mcp_json_value_t *)mcp_json_object_get(ctx, result, "capabilities");
        if (caps == NULL) {
            caps = mcp_json_object_new(ctx);
            if (caps == NULL || mcp_json_object_set_take(ctx, result, "capabilities", caps) != MCP_OK) {
                mcp_json_destroy(ctx, caps);
                mcp_json_destroy(ctx, result);
                return NULL;
            }
        }
        mcp_json_value_t *exts = (mcp_json_value_t *)mcp_json_object_get(ctx, caps, "extensions");
        if (exts == NULL) {
            exts = mcp_json_object_new(ctx);
            if (exts == NULL || mcp_json_object_set_take(ctx, caps, "extensions", exts) != MCP_OK) {
                mcp_json_destroy(ctx, exts);
                mcp_json_destroy(ctx, result);
                return NULL;
            }
        }
        if (srv->task_mgr != NULL) {
            mcp_json_value_t *task_ext = mcp_json_object_new(ctx);
            if (task_ext == NULL || mcp_json_object_set_take(ctx, exts, "io.modelcontextprotocol/tasks", task_ext) != MCP_OK) {
                mcp_json_destroy(ctx, task_ext);
                mcp_json_destroy(ctx, result);
                return NULL;
            }
        }
        if (srv->skill_reg != NULL) {
            mcp_json_value_t *skill_ext = mcp_json_object_new(ctx);
            if (skill_ext == NULL || mcp_json_object_set_take(ctx, exts, "io.modelcontextprotocol/skills", skill_ext) != MCP_OK) {
                mcp_json_destroy(ctx, skill_ext);
                mcp_json_destroy(ctx, result);
                return NULL;
            }
        }
    }
    if (decorate_result(ctx, srv, s, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_initialize(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                       const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *ver = NULL;
    if (get_string(ctx, params, "protocolVersion", &ver) != MCP_OK || ver == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "initialize: missing protocolVersion");
    }
    const char *neg_str = mcp_protocol_negotiate(ctx, ver);
    if (neg_str == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "initialize: unsupported version");
    }
    s->negotiated = mcp_proto_version_parse(ver);
    const mcp_json_value_t *info = mcp_json_object_get(ctx, params, "clientInfo");
    mcp_context_t *pctx = (s != NULL && s->ctx != NULL) ? s->ctx : ctx;
    if (info != NULL) {
        const char *name = NULL;
        const char *cver = NULL;
        if (get_string(ctx, info, "name", &name) == MCP_OK && name != NULL) {
            char *copy = srv_strdup(pctx, name);
            if (copy == NULL) {
                return NULL;
            }
            srv_free(pctx, s->client_name);
            s->client_name = copy;
        }
        if (get_string(ctx, info, "version", &cver) == MCP_OK && cver != NULL) {
            char *copy = srv_strdup(pctx, cver);
            if (copy == NULL) {
                return NULL;
            }
            srv_free(pctx, s->client_version);
            s->client_version = copy;
        }
    }
    mcp_proto_version_t resp_ver = mcp_proto_version_parse(neg_str);
    mcp_json_value_t *result = mcp_initialize_result_new_v(ctx, resp_ver, srv->name, srv->version);
    if (result == NULL) {
        return NULL;
    }
    mcp_json_value_t *caps = (mcp_json_value_t *)mcp_json_object_get(ctx, result, "capabilities");
    if (caps != NULL) {
        /* Always advertise logging capability */
        mcp_json_value_t *logging_cap = mcp_json_object_new(ctx);
        if (logging_cap != NULL) {
            mcp_json_object_set_take(ctx, caps, "logging", logging_cap);
        }

        /* Advertise tools capability if tools are registered */
        if (srv->n_tools > 0) {
            mcp_json_value_t *tools_cap = mcp_json_object_new(ctx);
            if (tools_cap != NULL) {
                mcp_json_object_set_take(ctx, tools_cap, "listChanged", mcp_json_bool_new(ctx, true));
                mcp_json_object_set_take(ctx, caps, "tools", tools_cap);
            }
        }

        /* Advertise resources capability if resources or templates are registered */
        if (srv->n_resources > 0 || srv->n_resource_templates > 0) {
            mcp_json_value_t *res_cap = mcp_json_object_new(ctx);
            if (res_cap != NULL) {
                mcp_json_object_set_take(ctx, res_cap, "subscribe", mcp_json_bool_new(ctx, true));
                mcp_json_object_set_take(ctx, res_cap, "listChanged", mcp_json_bool_new(ctx, true));
                mcp_json_object_set_take(ctx, caps, "resources", res_cap);
            }
        }

        /* Advertise prompts capability if prompts are registered */
        if (srv->n_prompts > 0) {
            mcp_json_value_t *prompts_cap = mcp_json_object_new(ctx);
            if (prompts_cap != NULL) {
                mcp_json_object_set_take(ctx, prompts_cap, "listChanged", mcp_json_bool_new(ctx, true));
                mcp_json_object_set_take(ctx, caps, "prompts", prompts_cap);
            }
        }

        /* Advertise extensions: tasks and skills */
        if (srv->task_mgr != NULL || srv->skill_reg != NULL) {
            mcp_json_value_t *exts = mcp_json_object_new(ctx);
            if (exts != NULL) {
                if (srv->task_mgr != NULL) {
                    mcp_json_object_set_take(ctx, exts, "io.modelcontextprotocol/tasks", mcp_json_object_new(ctx));
                }
                if (srv->skill_reg != NULL) {
                    mcp_json_object_set_take(ctx, exts, "io.modelcontextprotocol/skills", mcp_json_object_new(ctx));
                }
                mcp_json_object_set_take(ctx, caps, "extensions", exts);
            }
        }

        /* Merge host-supplied custom capabilities if configured */
        if (srv->custom_capabilities != NULL) {
            size_t n = mcp_json_object_size(ctx, srv->custom_capabilities);
            for (size_t i = 0; i < n; i++) {
                const char *k = mcp_json_object_key_at(ctx, srv->custom_capabilities, i);
                const mcp_json_value_t *v = mcp_json_object_get(ctx, srv->custom_capabilities, k);
                if (k != NULL && v != NULL) {
                    mcp_json_value_t *v_clone = mcp_json_clone(ctx, v);
                    if (v_clone != NULL) {
                        mcp_json_object_set_take(ctx, caps, k, v_clone);
                    }
                }
            }
        }
    }
    const mcp_json_value_t *client_caps = mcp_json_object_get(ctx, params, "capabilities");
    if (client_caps != NULL) {
        mcp_json_value_t *caps_clone = mcp_json_clone(pctx, client_caps);
        if (caps_clone != NULL) {
            mcp_json_destroy(pctx, s->client_capabilities);
            s->client_capabilities = caps_clone;
        }
    }
    if (srv->instructions != NULL && srv->instructions[0] != '\0') {
        if (set_string(ctx, result, "instructions", srv->instructions) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    const mcp_json_value_t *meta = mcp_message_meta(ctx, req);
    if (meta != NULL) {
        mcp_json_value_t *clone = mcp_json_clone(pctx, meta);
        if (clone == NULL) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_json_destroy(pctx, s->client_meta);
        s->client_meta = clone;
    }
    if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_tools_list(mcp_context_t *ctx, mcp_server_t *srv,
                                       mcp_session_t *s, const mcp_message_t *req) {
    size_t offset = page_offset(ctx, req);
    if (offset == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/list: bad cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    const mcp_tool_t **tlist = NULL;
    if (srv->n_tools > 0) {
        tlist = srv_malloc(ctx, sizeof(const mcp_tool_t *) * srv->n_tools);
        if (tlist == NULL) {
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }
    }
    size_t visible = 0;
    for (size_t i = 0; i < srv->n_tools; i++) {
        const mcp_tool_t *t = srv->tools[i];
        if (t->vis != MCP_TOOL_VIS_BOTH && (t->vis == MCP_TOOL_VIS_APP) != (s != NULL && s->apps_host)) {
            continue;
        }
        if (tlist != NULL) {
            tlist[visible] = t;
        }
        visible++;
    }
    if (mcp_proto_policy_sort_lists_by_name(ctx, mcp_session_negotiated(ctx, s)) && visible > 1 && tlist != NULL) {
        qsort((void *)tlist, visible, sizeof(const mcp_tool_t *), tool_cmp_name);
    }
    size_t end = offset + MCP_LIST_PAGE_SIZE < visible ? offset + MCP_LIST_PAGE_SIZE : visible;
    for (size_t i = offset; i < end; i++) {
        const mcp_tool_t *t = tlist[i];
        mcp_json_value_t *entry = mcp_json_object_new(ctx);
        if (entry == NULL || set_string(ctx, entry, "name", t->name) != MCP_OK ||
            (t->description != NULL && set_string(ctx, entry, "description", t->description) != MCP_OK)) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            srv_free(ctx, (void *)tlist);
            return NULL;
        }
        if (t->schema != NULL) {
            mcp_json_value_t *copy = mcp_json_clone(ctx, t->schema);
            if (copy == NULL ||
                mcp_json_object_set_take(ctx, entry, "inputSchema", copy) != MCP_OK) {
                mcp_json_destroy(ctx, entry);
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                srv_free(ctx, (void *)tlist);
                return NULL;
            }
        } else {
            /* Official MCP Schema requires inputSchema to be present and of type "object" */
            mcp_json_value_t *def_schema = mcp_json_object_new(ctx);
            if (def_schema == NULL ||
                mcp_json_object_set_take(ctx, def_schema, "type", mcp_json_string_new(ctx, "object")) != MCP_OK ||
                mcp_json_object_set_take(ctx, entry, "inputSchema", def_schema) != MCP_OK) {
                mcp_json_destroy(ctx, def_schema);
                mcp_json_destroy(ctx, entry);
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                srv_free(ctx, (void *)tlist);
                return NULL;
            }
        }
        if (t->output_schema != NULL) {
            mcp_json_value_t *copy = mcp_json_clone(ctx, t->output_schema);
            if (copy == NULL ||
                mcp_json_object_set_take(ctx, entry, "outputSchema", copy) != MCP_OK) {
                mcp_json_destroy(ctx, entry);
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                srv_free(ctx, (void *)tlist);
                return NULL;
            }
        }
        if (t->meta != NULL) {
            mcp_json_value_t *copy = mcp_json_clone(ctx, t->meta);
            if (copy == NULL ||
                mcp_json_object_set_take(ctx, entry, "_meta", copy) != MCP_OK) {
                mcp_json_destroy(ctx, entry);
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                srv_free(ctx, (void *)tlist);
                return NULL;
            }
        }
        if (mcp_json_array_append(ctx, arr, entry) != MCP_OK) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            srv_free(ctx, (void *)tlist);
            return NULL;
        }
    }
    srv_free(ctx, (void *)tlist);
    if (mcp_json_object_set_take(ctx, result, "tools", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (offset + MCP_LIST_PAGE_SIZE < visible &&
        set_next_cursor(ctx, result, offset + MCP_LIST_PAGE_SIZE) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *tool_error_result(mcp_context_t *ctx, const mcp_message_t *req,
                                        mcp_status_t st) {
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *content = mcp_json_array_new(ctx);
    mcp_json_value_t *item = mcp_json_object_new(ctx);
    if (result == NULL || content == NULL || item == NULL) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (set_string(ctx, item, "type", "text") != MCP_OK ||
        set_string(ctx, item, "text", mcp_status_string(st)) != MCP_OK ||
        mcp_json_array_append(ctx, content, item) != MCP_OK) {
        mcp_json_destroy(ctx, item);
        mcp_json_destroy(ctx, content);
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    /* item now owned by content; take consumes content/flag on all paths */
    if (mcp_json_object_set_take(ctx, result, "content", content) != MCP_OK ||
        mcp_json_object_set_take(ctx, result, "isError", mcp_json_bool_new(ctx, true)) !=
            MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

mcp_status_t mcp_tool_report_progress(mcp_context_t *ctx,
                                      const mcp_tool_call_ctx_t *call_ctx,
                                      double progress, double total,
                                      const char *message_or_null) {
    if (call_ctx == NULL || call_ctx->session == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (call_ctx->progress_token == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    return mcp_session_notify_progress(ctx, call_ctx->session, call_ctx->progress_token,
                                       progress, total, message_or_null);
}

bool mcp_tool_is_cancelled(const mcp_tool_call_ctx_t *call_ctx) {
    if (call_ctx == NULL || call_ctx->cancellation_token == NULL) {
        return false;
    }
    mcp_in_flight_req_t *in_flight = (mcp_in_flight_req_t *)call_ctx->cancellation_token;
    return atomic_load(&in_flight->is_cancelled);
}

const char *mcp_tool_cancel_reason(const mcp_tool_call_ctx_t *call_ctx) {
    if (call_ctx == NULL || call_ctx->cancellation_token == NULL) {
        return NULL;
    }
    mcp_in_flight_req_t *in_flight = (mcp_in_flight_req_t *)call_ctx->cancellation_token;
    return in_flight->cancel_reason;
}


static mcp_message_t *route_tools_call(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                       const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *name = NULL;
    if (params == NULL || get_string(ctx, params, "name", &name) != MCP_OK || name == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/call: missing name");
    }
    mcp_tool_t *tool = find_tool(srv, name);
    if (tool == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/call: unknown tool");
    }
    if (tool->vis != MCP_TOOL_VIS_BOTH && (tool->vis == MCP_TOOL_VIS_APP) != s->apps_host) {
        return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "tools/call: unknown tool");
    }
    const mcp_json_value_t *args = mcp_json_object_get(ctx, params, "arguments");
    const mcp_json_value_t *rs_val = mcp_json_object_get(ctx, params, "requestState");
    bool is_mrtr_retry = (tool->handler_v2 != NULL && rs_val != NULL);

    if (tool->schema != NULL && !is_mrtr_retry) {
        mcp_status_t vst;
        if (args != NULL) {
            vst = mcp_schema_validate(ctx, tool->schema, args);
        } else {
            mcp_json_value_t *null = mcp_json_null_new(ctx);
            vst = null == NULL ? MCP_ERR_NOMEM : mcp_schema_validate(ctx, tool->schema, null);
            mcp_json_destroy(ctx, null);
        }
        if (vst != MCP_OK) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/call: invalid arguments");
        }
    }
    if ((tool->required & ~s->granted) != 0) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tools/call: permission denied");
    }

    const mcp_json_value_t *progress_token = NULL;
    const mcp_json_value_t *meta_obj = params != NULL ? mcp_json_object_get(ctx, params, "_meta") : NULL;
    if (meta_obj == NULL) {
        meta_obj = mcp_message_meta(ctx, req);
    }
    if (meta_obj != NULL && mcp_json_type(ctx, meta_obj) == MCP_JSON_OBJECT) {
        progress_token = mcp_json_object_get(ctx, meta_obj, "progressToken");
    }

    mcp_json_value_t *result = NULL;
    mcp_status_t st;
    if (tool->handler_v2 != NULL) {
        /* V2 path: extract MRTR fields, progress token, and populate call context */
        mcp_tool_call_ctx_t call_ctx;
        call_ctx.args            = args;
        call_ctx.input_responses = mcp_json_object_get(ctx, params, "inputResponses");
        const char *rs_str = NULL;
        if (rs_val != NULL && mcp_json_type(ctx, rs_val) == MCP_JSON_STRING) {
            mcp_json_string_value(ctx, rs_val, &rs_str);
        }
        call_ctx.request_state  = rs_str;
        call_ctx.progress_token = progress_token;
        call_ctx.session        = s;
        mcp_id_type_t id_t = mcp_message_id_type(ctx, req);
        const char *ids = mcp_message_id_string(ctx, req);
        double idn = 0;
        mcp_message_id_number(ctx, req, &idn);
        call_ctx.cancellation_token = mcp_session_find_in_flight(s, id_t, ids, idn);
        st = tool->handler_v2(ctx, s, &call_ctx, tool->user_data, &result);

    } else {
        /* V1 path: unchanged */
        st = tool->handler(ctx, s, args, tool->user_data, &result);
    }
    atomic_fetch_add(&srv->c_tools_called, 1);
    if (st != MCP_OK) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=tool_error tool=%s status=%s", name,
              mcp_status_string(st));
        return tool_error_result(ctx, req, st);
    }
    if (result == NULL) {
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "tools/call: empty result");
    }
    /* For InputRequiredResult, skip the "complete" decoration and pass
     * the result through as-is so the client gets the full MRTR payload. */
    if (!mcp_mrtr_is_input_required(ctx, result)) {
        if (tool->output_schema != NULL) {
            const mcp_json_value_t *sc = mcp_json_object_get(ctx, result, "structuredContent");
            if (sc != NULL) {
                mcp_status_t vst = mcp_schema_validate(ctx, tool->output_schema, sc);
                if (vst != MCP_OK) {
                    mcp_json_destroy(ctx, result);
                    return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS,
                                    "tools/call: structuredContent failed outputSchema validation");
                }
            }
        }
        if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_resources_list(mcp_context_t *ctx, mcp_server_t *srv,
                                           mcp_session_t *s,
                                           const mcp_message_t *req) {
    size_t offset = page_offset(ctx, req);
    if (offset == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "resources/list: bad cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    const mcp_resource_t **rlist = NULL;
    if (srv->n_resources > 0) {
        rlist = srv_malloc(ctx, sizeof(const mcp_resource_t *) * srv->n_resources);
        if (rlist == NULL) {
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }
        for (size_t i = 0; i < srv->n_resources; i++) {
            rlist[i] = srv->resources[i];
        }
    }
    if (mcp_proto_policy_sort_lists_by_name(ctx, mcp_session_negotiated(ctx, s)) && srv->n_resources > 1 && rlist != NULL) {
        qsort((void *)rlist, srv->n_resources, sizeof(const mcp_resource_t *), resource_cmp_name);
    }
    size_t end = offset + MCP_LIST_PAGE_SIZE < srv->n_resources ? offset + MCP_LIST_PAGE_SIZE
                                                                : srv->n_resources;
    for (size_t i = offset; i < end; i++) {
        const mcp_resource_t *r = rlist[i];
        mcp_json_value_t *entry = mcp_json_object_new(ctx);
        if (entry == NULL || set_string(ctx, entry, "uri", r->uri) != MCP_OK ||
            set_string(ctx, entry, "name", r->name) != MCP_OK ||
            (r->description != NULL && set_string(ctx, entry, "description", r->description) != MCP_OK) ||
            (r->mime_type != NULL && set_string(ctx, entry, "mimeType", r->mime_type) != MCP_OK) ||
            (r->has_size && mcp_json_object_set_take(ctx, entry, "size", mcp_json_number_new(ctx, (double)r->size)) != MCP_OK) ||
            (r->annotations != NULL && mcp_json_object_set_take(ctx, entry, "annotations", mcp_json_clone(ctx, r->annotations)) != MCP_OK) ||
            mcp_json_array_append(ctx, arr, entry) != MCP_OK) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            srv_free(ctx, (void *)rlist);
            return NULL;
        }
    }
    srv_free(ctx, (void *)rlist);
    if (mcp_json_object_set_take(ctx, result, "resources", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (end < srv->n_resources && set_next_cursor(ctx, result, end) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_resources_read(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                           const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *uri = NULL;
    if (params == NULL || get_string(ctx, params, "uri", &uri) != MCP_OK || uri == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "resources/read: missing uri");
    }
    mcp_resource_t *res = find_resource(srv, uri);
    mcp_json_value_t *contents = NULL;
    mcp_status_t st = MCP_OK;

    if (res != NULL) {
        if (res->on_read == NULL) {
            return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "resources/read: no reader");
        }
        st = res->on_read(ctx, s, uri, res->user_data, &contents);
    } else {
        /* Not a static resource; attempt matching against registered URI templates */
        mcp_resource_template_t *matched_tmpl = NULL;
        mcp_json_value_t *matched_vars = NULL;
        for (size_t i = 0; i < srv->n_resource_templates; i++) {
            mcp_json_value_t *vars = NULL;
            if (mcp_uri_template_match(ctx, srv->resource_templates[i]->uri_template, uri, &vars) == MCP_OK) {
                matched_tmpl = srv->resource_templates[i];
                matched_vars = vars;
                break;
            }
        }
        if (matched_tmpl == NULL) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "resources/read: unknown resource");
        }
        if (matched_tmpl->on_read == NULL) {
            mcp_json_destroy(ctx, matched_vars);
            return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "resources/read: no reader");
        }
        st = matched_tmpl->on_read(ctx, s, uri, matched_vars, matched_tmpl->user_data, &contents);
        mcp_json_destroy(ctx, matched_vars);
    }

    if (st != MCP_OK) {
        return err_resp(ctx, req, mcp_status_to_rpc_code(st), "resources/read: handler failed");
    }
    if (contents == NULL || mcp_json_type(ctx, contents) != MCP_JSON_ARRAY) {
        mcp_json_destroy(ctx, contents);
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "resources/read: bad contents");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        mcp_json_destroy(ctx, contents);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, result, "contents", contents) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_prompts_list(mcp_context_t *ctx, mcp_server_t *srv,
                                         mcp_session_t *s,
                                         const mcp_message_t *req) {
    size_t offset = page_offset(ctx, req);
    if (offset == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "prompts/list: bad cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    const mcp_prompt_t **plist = NULL;
    if (srv->n_prompts > 0) {
        plist = srv_malloc(ctx, sizeof(const mcp_prompt_t *) * srv->n_prompts);
        if (plist == NULL) {
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }
        for (size_t i = 0; i < srv->n_prompts; i++) {
            plist[i] = srv->prompts[i];
        }
    }
    if (mcp_proto_policy_sort_lists_by_name(ctx, mcp_session_negotiated(ctx, s)) && srv->n_prompts > 1 && plist != NULL) {
        qsort((void *)plist, srv->n_prompts, sizeof(const mcp_prompt_t *), prompt_cmp_name);
    }
    size_t end = offset + MCP_LIST_PAGE_SIZE < srv->n_prompts ? offset + MCP_LIST_PAGE_SIZE
                                                              : srv->n_prompts;
    for (size_t i = offset; i < end; i++) {
        const mcp_prompt_t *p = plist[i];
        mcp_json_value_t *entry = mcp_json_object_new(ctx);
        if (entry == NULL || set_string(ctx, entry, "name", p->name) != MCP_OK ||
            (p->description != NULL &&
             set_string(ctx, entry, "description", p->description) != MCP_OK)) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            srv_free(ctx, (void *)plist);
            return NULL;
        }
        if (p->n_args > 0) {
            mcp_json_value_t *args_arr = mcp_json_array_new(ctx);
            if (args_arr == NULL) {
                mcp_json_destroy(ctx, entry);
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                srv_free(ctx, (void *)plist);
                return NULL;
            }
            for (size_t a_idx = 0; a_idx < p->n_args; a_idx++) {
                mcp_json_value_t *arg_obj = mcp_json_object_new(ctx);
                if (arg_obj == NULL ||
                    set_string(ctx, arg_obj, "name", p->args[a_idx].name) != MCP_OK ||
                    (p->args[a_idx].description != NULL &&
                     set_string(ctx, arg_obj, "description", p->args[a_idx].description) != MCP_OK) ||
                    (p->args[a_idx].required &&
                     mcp_json_object_set_take(ctx, arg_obj, "required", mcp_json_bool_new(ctx, true)) != MCP_OK) ||
                    mcp_json_array_append(ctx, args_arr, arg_obj) != MCP_OK) {
                    mcp_json_destroy(ctx, arg_obj);
                    mcp_json_destroy(ctx, args_arr);
                    mcp_json_destroy(ctx, entry);
                    mcp_json_destroy(ctx, result);
                    mcp_json_destroy(ctx, arr);
                    srv_free(ctx, (void *)plist);
                    return NULL;
                }
            }
            if (mcp_json_object_set_take(ctx, entry, "arguments", args_arr) != MCP_OK) {
                mcp_json_destroy(ctx, args_arr);
                mcp_json_destroy(ctx, entry);
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                srv_free(ctx, (void *)plist);
                return NULL;
            }
        }
        if (mcp_json_array_append(ctx, arr, entry) != MCP_OK) {
            mcp_json_destroy(ctx, entry);
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            srv_free(ctx, (void *)plist);
            return NULL;
        }
    }
    srv_free(ctx, (void *)plist);
    if (mcp_json_object_set_take(ctx, result, "prompts", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (end < srv->n_prompts && set_next_cursor(ctx, result, end) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_prompts_get(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                        const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *name = NULL;
    if (params == NULL || get_string(ctx, params, "name", &name) != MCP_OK || name == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "prompts/get: missing name");
    }
    mcp_prompt_t *prompt = find_prompt(srv, name);
    if (prompt == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "prompts/get: unknown prompt");
    }
    if (prompt->on_get == NULL) {
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "prompts/get: no handler");
    }
    const mcp_json_value_t *args = mcp_json_object_get(ctx, params, "arguments");
    mcp_json_value_t *messages = NULL;
    mcp_status_t st = prompt->on_get(ctx, s, args, prompt->user_data, &messages);
    if (st != MCP_OK) {
        return err_resp(ctx, req, mcp_status_to_rpc_code(st), "prompts/get: handler failed");
    }
    if (messages == NULL || mcp_json_type(ctx, messages) != MCP_JSON_ARRAY) {
        mcp_json_destroy(ctx, messages);
        return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR, "prompts/get: bad messages");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        mcp_json_destroy(ctx, messages);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, result, "messages", messages) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (prompt->description != NULL && set_string(ctx, result, "description", prompt->description) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_completion_list(mcp_context_t *ctx, mcp_server_t *srv,
                                            mcp_session_t *s,
                                            const mcp_message_t *req) {
    // Return an empty array – completion/list has no meaningful payload.
    // The cursor is still validated so a bad cursor fails consistently
    // across all list methods.
    if (page_offset(ctx, req) == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "completion/list: bad cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *comps = mcp_json_array_new(ctx);
    if (result == NULL || comps == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, comps);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, result, "completions", comps) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_completion_complete(mcp_context_t *ctx, mcp_server_t *srv,
                                                mcp_session_t *s, const mcp_message_t *req) {
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const mcp_json_value_t *ref_obj = params != NULL ? mcp_json_object_get(ctx, params, "ref") : NULL;
    if (ref_obj == NULL || mcp_json_type(ctx, ref_obj) != MCP_JSON_OBJECT) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS,
                        "completion/complete: missing ref object");
    }
    const char *ref_str = NULL;
    const char *ref_type = NULL;
    get_string(ctx, ref_obj, "type", &ref_type);

    if (ref_type != NULL) {
        if (strcmp(ref_type, "ref/prompt") == 0) {
            get_string(ctx, ref_obj, "name", &ref_str);
        } else if (strcmp(ref_type, "ref/resource") == 0) {
            get_string(ctx, ref_obj, "uri", &ref_str);
        }
    }
    if (ref_str == NULL) {
        /* Fallback: try "value", then "name", then "uri" */
        if (get_string(ctx, ref_obj, "value", &ref_str) != MCP_OK || ref_str == NULL) {
            if (get_string(ctx, ref_obj, "name", &ref_str) != MCP_OK || ref_str == NULL) {
                get_string(ctx, ref_obj, "uri", &ref_str);
            }
        }
    }
    if (ref_str == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS,
                        "completion/complete: ref identifier (name/uri/value) missing or not a string");
    }
    // Find the first registered provider whose ref_prefix matches the reference.
    for (size_t i = 0; i < srv->n_completions; i++) {
        const char *prefix = srv->completions[i].ref_prefix;
        size_t plen = strlen(prefix);
        bool match = (strncmp(ref_str, prefix, plen) == 0);
        if (!match && ref_type != NULL) {
            if (strcmp(ref_type, "ref/prompt") == 0) {
                if (strncmp(prefix, "prompt/", 7) == 0) {
                    if (strncmp(ref_str, prefix + 7, strlen(prefix + 7)) == 0) {
                        match = true;
                    }
                } else if (strcmp(prefix, "ref/prompt") == 0) {
                    match = true;
                }
            } else if (strcmp(ref_type, "ref/resource") == 0) {
                if (strncmp(prefix, "resource/", 9) == 0) {
                    if (strncmp(ref_str, prefix + 9, strlen(prefix + 9)) == 0) {
                        match = true;
                    }
                } else if (strcmp(prefix, "ref/resource") == 0) {
                    match = true;
                }
            }
        }
        if (match) {
            const mcp_json_value_t *args_ref = mcp_json_object_get(ctx, params, "argument");
            // Clone so the provider never sees a dangling pointer into the request
            // DOM (which the caller destroys after mcp_server_dispatch returns).
            mcp_json_value_t *args;
            if (args_ref != NULL) {
                args = mcp_json_clone(ctx, args_ref);
                if (args == NULL) {
                    return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR,
                                    "completion/complete: arg clone failed");
                }
            } else {
                args = mcp_json_null_new(ctx);
                if (args == NULL) {
                    return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR,
                                    "completion/complete: arg clone failed");
                }
            }
            mcp_json_value_t *comps = srv->completions[i].fn(ctx, s, args,
                                                               srv->completions[i].user_data);
            mcp_json_destroy(ctx, args);
            if (comps == NULL) {
                return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR,
                                "completion/complete: provider returned NULL");
            }
            if (mcp_json_type(ctx, comps) != MCP_JSON_ARRAY) {
                mcp_json_destroy(ctx, comps);
                return err_resp(ctx, req, MCP_RPC_INTERNAL_ERROR,
                                "completion/complete: provider returned non-array");
            }
            mcp_json_value_t *result = mcp_json_object_new(ctx);
            if (result == NULL) {
                mcp_json_destroy(ctx, comps);
                return NULL;
            }
            /* Official MCP structure: completion: { values: string[], total: int, hasMore: bool } */
            mcp_json_value_t *completion_obj = mcp_json_object_new(ctx);
            if (completion_obj != NULL) {
                size_t total_count = mcp_json_array_size(ctx, comps);
                mcp_json_value_t *comps_clone = mcp_json_clone(ctx, comps);
                if (comps_clone != NULL) {
                    mcp_json_object_set_take(ctx, completion_obj, "values", comps_clone);
                }
                mcp_json_object_set_take(ctx, completion_obj, "total", mcp_json_number_new(ctx, (double)total_count));
                mcp_json_object_set_take(ctx, completion_obj, "hasMore", mcp_json_bool_new(ctx, false));
                mcp_json_object_set_take(ctx, result, "completion", completion_obj);
            }
            if (mcp_json_object_set_take(ctx, result, "completions", comps) != MCP_OK) {
                mcp_json_destroy(ctx, result);
                return NULL;
            }
            if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
                mcp_json_destroy(ctx, result);
                return NULL;
            }
            mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
            if (resp == NULL) {
                mcp_json_destroy(ctx, result);
            }
            return resp;
        }
    }
    // No matching provider registered – return empty completions.
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *comps = mcp_json_array_new(ctx);
    if (result == NULL || comps == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, comps);
        return NULL;
    }
    mcp_json_value_t *completion_obj = mcp_json_object_new(ctx);
    if (completion_obj != NULL) {
        mcp_json_value_t *values_empty = mcp_json_array_new(ctx);
        if (values_empty != NULL) {
            mcp_json_object_set_take(ctx, completion_obj, "values", values_empty);
        }
        mcp_json_object_set_take(ctx, completion_obj, "total", mcp_json_number_new(ctx, 0.0));
        mcp_json_object_set_take(ctx, completion_obj, "hasMore", mcp_json_bool_new(ctx, false));
        mcp_json_object_set_take(ctx, result, "completion", completion_obj);
    }
    if (mcp_json_object_set_take(ctx, result, "completions", comps) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_advanced(mcp_context_t *ctx, mcp_server_t *srv, mcp_session_t *s,
                                     const mcp_message_t *req, const char *method) {
    (void)s;
    // Returns NULL when method is not one of the advanced routes so the
    // caller falls through to the unknown_method handler.
    if (strcmp(method, "logging/setLevel") == 0) {
        const mcp_json_value_t *params = mcp_message_params(ctx, req);
        const char *lvl = NULL;
        if (params == NULL || get_string(ctx, params, "level", &lvl) != MCP_OK || lvl == NULL) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "logging/setLevel: missing level");
        }
        mcp_log_level_t mapped;
        if (strcmp(lvl, "debug") == 0) {
            mapped = MCP_LOG_DEBUG;
        } else if (strcmp(lvl, "info") == 0 || strcmp(lvl, "notice") == 0) {
            mapped = MCP_LOG_INFO;
        } else if (strcmp(lvl, "warning") == 0 || strcmp(lvl, "warn") == 0) {
            mapped = MCP_LOG_WARN;
        } else if (strcmp(lvl, "error") == 0 || strcmp(lvl, "critical") == 0 ||
                   strcmp(lvl, "alert") == 0 || strcmp(lvl, "emergency") == 0) {
            mapped = MCP_LOG_ERROR;
        } else {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "logging/setLevel: bad level");
        }
        if (s != NULL) {
            s->session_log_floor = mapped;
            s->session_log_level_set = true;
        }
        atomic_store(&srv->log_floor, (int)mapped);

        mcp_json_value_t *result = mcp_json_object_new(ctx);
        if (result == NULL) {
            return NULL;
        }
        if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    if (strcmp(method, "resources/subscribe") == 0 ||
        strcmp(method, "resources/unsubscribe") == 0) {
        const mcp_json_value_t *params = mcp_message_params(ctx, req);
        const char *uri = NULL;
        if (params == NULL || get_string(ctx, params, "uri", &uri) != MCP_OK || uri == NULL) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "missing uri");
        }
        bool is_sub = strcmp(method, "resources/subscribe") == 0;
        if (s != NULL) {
            if (is_sub) {
                mcp_session_subscribe_resource(ctx, s, uri);
            } else {
                mcp_session_unsubscribe_resource(ctx, s, uri);
            }
        }
        pthread_mutex_lock(&srv->subscribed_lock);
        {
            mcp_context_t *pctx = (srv->ctx != NULL) ? srv->ctx : ctx;
            if (is_sub) {
                bool seen = false;
                for (size_t i = 0; i < srv->n_subscribed; i++) {
                    if (strcmp(srv->subscribed_uris[i], uri) == 0) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    if (srv->n_subscribed == srv->cap_subscribed) {
                        size_t ncap = srv->cap_subscribed == 0 ? 4 : srv->cap_subscribed * 2;
                        char **nu = srv_realloc(pctx, srv->subscribed_uris, ncap * sizeof(*nu));
                        if (nu == NULL) {
                            pthread_mutex_unlock(&srv->subscribed_lock);
                            return NULL;
                        }
                        srv->subscribed_uris = nu;
                        srv->cap_subscribed = ncap;
                    }
                    char *copy = srv_strdup(pctx, uri);
                    if (copy == NULL) {
                        pthread_mutex_unlock(&srv->subscribed_lock);
                        return NULL;
                    }
                    srv->subscribed_uris[srv->n_subscribed++] = copy;
                }
            } else {
                for (size_t i = 0; i < srv->n_subscribed; i++) {
                    if (strcmp(srv->subscribed_uris[i], uri) == 0) {
                        srv_free(pctx, srv->subscribed_uris[i]);
                        srv->subscribed_uris[i] = srv->subscribed_uris[--srv->n_subscribed];
                        break;
                    }
                }
            }
        }
        pthread_mutex_unlock(&srv->subscribed_lock);
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        if (result == NULL) {
            return NULL;
        }
        if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    if (strcmp(method, "resources/templates/list") == 0) {
        size_t off = page_offset(ctx, req);
        if (off == (size_t)-1) {
            return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "resources/templates/list: bad cursor");
        }

        mcp_json_value_t *result = mcp_json_object_new(ctx);
        mcp_json_value_t *arr = mcp_json_array_new(ctx);
        if (result == NULL || arr == NULL) {
            mcp_json_destroy(ctx, result);
            mcp_json_destroy(ctx, arr);
            return NULL;
        }

        size_t end = off + MCP_LIST_PAGE_SIZE;
        if (end > srv->n_resource_templates) {
            end = srv->n_resource_templates;
        }

        for (size_t i = off; i < end; i++) {
            mcp_resource_template_t *t = srv->resource_templates[i];
            mcp_json_value_t *item = mcp_json_object_new(ctx);
            if (item == NULL) {
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                return NULL;
            }
            set_string(ctx, item, "uriTemplate", t->uri_template);
            set_string(ctx, item, "name", t->name);
            if (t->description != NULL) {
                set_string(ctx, item, "description", t->description);
            }
            if (t->mime_type != NULL) {
                set_string(ctx, item, "mimeType", t->mime_type);
            }
            if (t->annotations != NULL) {
                mcp_json_object_set_take(ctx, item, "annotations", mcp_json_clone(ctx, t->annotations));
            }
            mcp_json_array_append(ctx, arr, item);
        }

        mcp_json_value_t *arr_legacy = mcp_json_clone(ctx, arr);
        if (arr_legacy != NULL) {
            mcp_json_object_set_take(ctx, result, "templates", arr_legacy);
        }
        if (mcp_json_object_set_take(ctx, result, "resourceTemplates", arr) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        if (end < srv->n_resource_templates) {
            if (set_next_cursor(ctx, result, end) != MCP_OK) {
                mcp_json_destroy(ctx, result);
                return NULL;
            }
        }
        if (decorate_result(ctx, srv, s, result, true) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    return NULL;
}

static mcp_message_t *route_subscriptions_listen(mcp_context_t *ctx, mcp_server_t *srv,
                                                 mcp_session_t *s, const mcp_message_t *req) {
    (void)srv;
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    if (params == NULL || mcp_json_type(ctx, params) != MCP_JSON_OBJECT) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "subscriptions/listen requires params object");
    }

    const mcp_json_value_t *notifs = mcp_json_object_get(ctx, params, "notifications");
    if (notifs != NULL && mcp_json_type(ctx, notifs) != MCP_JSON_OBJECT) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "notifications must be an object");
    }

    if (s != NULL) {
        mcp_context_t *pctx = (s->ctx != NULL) ? s->ctx : ctx;
        s->subscription_active = false;
        if (s->sub_id_str != NULL) {
            srv_free(pctx, s->sub_id_str);
            s->sub_id_str = NULL;
        }
        if (s->sub_resource_uris != NULL) {
            for (size_t i = 0; i < s->n_sub_resource_uris; i++) {
                srv_free(pctx, s->sub_resource_uris[i]);
            }
            srv_free(pctx, s->sub_resource_uris);
            s->sub_resource_uris = NULL;
            s->n_sub_resource_uris = 0;
        }
        s->sub_tools_list_changed = false;
        s->sub_prompts_list_changed = false;
        s->sub_resources_list_changed = false;

        s->sub_id_type = mcp_message_id_type(ctx, req);
        if (s->sub_id_type == MCP_ID_STRING) {
            const char *id_str = mcp_message_id_string(ctx, req);
            s->sub_id_str = srv_strdup(pctx, id_str ? id_str : "");
        } else if (s->sub_id_type == MCP_ID_NUMBER) {
            mcp_message_id_number(ctx, req, &s->sub_id_num);
        }

        if (notifs != NULL) {
            const mcp_json_value_t *tlc = mcp_json_object_get(ctx, notifs, "toolsListChanged");
            if (tlc != NULL) {
                bool b = false;
                if (mcp_json_bool_value(ctx, tlc, &b) == MCP_OK) {
                    s->sub_tools_list_changed = b;
                }
            }
            const mcp_json_value_t *plc = mcp_json_object_get(ctx, notifs, "promptsListChanged");
            if (plc != NULL) {
                bool b = false;
                if (mcp_json_bool_value(ctx, plc, &b) == MCP_OK) {
                    s->sub_prompts_list_changed = b;
                }
            }
            const mcp_json_value_t *rlc = mcp_json_object_get(ctx, notifs, "resourcesListChanged");
            if (rlc != NULL) {
                bool b = false;
                if (mcp_json_bool_value(ctx, rlc, &b) == MCP_OK) {
                    s->sub_resources_list_changed = b;
                }
            }
            const mcp_json_value_t *ru = mcp_json_object_get(ctx, notifs, "resourceSubscriptions");
            if (ru != NULL && mcp_json_type(ctx, ru) == MCP_JSON_ARRAY) {
                size_t n = mcp_json_array_size(ctx, ru);
                if (n > 0) {
                    s->sub_resource_uris = srv_malloc(pctx, n * sizeof(char *));
                    if (s->sub_resource_uris != NULL) {
                        for (size_t i = 0; i < n; i++) {
                            const mcp_json_value_t *item = mcp_json_array_get(ctx, ru, i);
                            const char *uri_str = NULL;
                            if (item != NULL && mcp_json_string_value(ctx, item, &uri_str) == MCP_OK && uri_str != NULL) {
                                s->sub_resource_uris[s->n_sub_resource_uris++] = srv_strdup(pctx, uri_str);
                            }
                        }
                    }
                }
            }
        }
        s->subscription_active = true;
    }

    mcp_json_value_t *params_out = mcp_json_object_new(ctx);
    mcp_json_value_t *meta_out = mcp_json_object_new(ctx);
    if (params_out == NULL || meta_out == NULL) {
        mcp_json_destroy(ctx, params_out);
        mcp_json_destroy(ctx, meta_out);
        return NULL;
    }

    mcp_id_type_t idt = mcp_message_id_type(ctx, req);
    mcp_json_value_t *sub_id_val = NULL;
    if (idt == MCP_ID_STRING) {
        sub_id_val = mcp_json_string_new(ctx, mcp_message_id_string(ctx, req));
    } else if (idt == MCP_ID_NUMBER) {
        double d = 0;
        mcp_message_id_number(ctx, req, &d);
        sub_id_val = mcp_json_number_new(ctx, d);
    } else {
        sub_id_val = mcp_json_null_new(ctx);
    }

    if (sub_id_val == NULL ||
        mcp_json_object_set_take(ctx, meta_out, "io.modelcontextprotocol/subscriptionId", sub_id_val) != MCP_OK) {
        mcp_json_destroy(ctx, sub_id_val);
        mcp_json_destroy(ctx, meta_out);
        mcp_json_destroy(ctx, params_out);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, params_out, "_meta", meta_out) != MCP_OK) {
        mcp_json_destroy(ctx, meta_out);
        mcp_json_destroy(ctx, params_out);
        return NULL;
    }

    mcp_json_value_t *notifs_out = NULL;
    if (notifs != NULL) {
        notifs_out = mcp_json_clone(ctx, notifs);
    } else {
        notifs_out = mcp_json_object_new(ctx);
    }
    if (notifs_out == NULL ||
        mcp_json_object_set_take(ctx, params_out, "notifications", notifs_out) != MCP_OK) {
        mcp_json_destroy(ctx, notifs_out);
        mcp_json_destroy(ctx, params_out);
        return NULL;
    }

    mcp_message_t *ack = mcp_notification_new(ctx, "notifications/subscriptions/acknowledged", params_out);
    if (ack == NULL) {
        mcp_json_destroy(ctx, params_out);
        return NULL;
    }
    return ack;
}

static mcp_message_t *route_tasks_get(mcp_context_t *ctx, mcp_server_t *srv,
                                      mcp_session_t *s,
                                      const mcp_message_t *req) {
    if (srv->task_mgr == NULL) {
        return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "tasks extension not enabled");
    }
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *task_id = NULL;
    if (get_string(ctx, params, "taskId", &task_id) != MCP_OK || task_id == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tasks/get: missing taskId");
    }
    mcp_task_desc_t desc;
    if (mcp_task_mgr_get(ctx, srv->task_mgr, task_id, &desc) != MCP_OK) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tasks/get: task not found");
    }
    mcp_json_value_t *result = mcp_task_desc_to_json(ctx, &desc);
    if (result == NULL) {
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_tasks_update(mcp_context_t *ctx, mcp_server_t *srv,
                                         mcp_session_t *s,
                                         const mcp_message_t *req) {
    if (srv->task_mgr == NULL) {
        return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "tasks extension not enabled");
    }
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *task_id = NULL;
    if (get_string(ctx, params, "taskId", &task_id) != MCP_OK || task_id == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tasks/update: missing taskId");
    }
    mcp_task_desc_t desc;
    if (mcp_task_mgr_get(ctx, srv->task_mgr, task_id, &desc) != MCP_OK) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tasks/update: task not found");
    }
    if (desc.status == MCP_TASK_STATUS_INPUT_REQUIRED) {
        mcp_task_mgr_set_status(ctx, srv->task_mgr, task_id, MCP_TASK_STATUS_WORKING, "Input received; resuming");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return NULL;
    }
    mcp_json_value_t *v = mcp_json_bool_new(ctx, true);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "accepted", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_tasks_cancel(mcp_context_t *ctx, mcp_server_t *srv,
                                         mcp_session_t *s,
                                         const mcp_message_t *req) {
    if (srv->task_mgr == NULL) {
        return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "tasks extension not enabled");
    }
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *task_id = NULL;
    if (get_string(ctx, params, "taskId", &task_id) != MCP_OK || task_id == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tasks/cancel: missing taskId");
    }
    if (mcp_task_mgr_cancel(ctx, srv->task_mgr, task_id) != MCP_OK) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "tasks/cancel: task not found");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    if (result == NULL) {
        return NULL;
    }
    mcp_json_value_t *v = mcp_json_bool_new(ctx, true);
    if (v == NULL || mcp_json_object_set_take(ctx, result, "cancelled", v) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_skills_list(mcp_context_t *ctx, mcp_server_t *srv,
                                        mcp_session_t *s,
                                        const mcp_message_t *req) {
    if (srv->skill_reg == NULL) {
        return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "skills extension not enabled");
    }
    size_t offset = page_offset(ctx, req);
    if (offset == (size_t)-1) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "invalid cursor");
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *arr = mcp_json_array_new(ctx);
    if (result == NULL || arr == NULL) {
        mcp_json_destroy(ctx, result);
        mcp_json_destroy(ctx, arr);
        return NULL;
    }
    size_t count = mcp_skill_registry_count(srv->skill_reg);
    size_t end = offset + MCP_LIST_PAGE_SIZE;
    if (end > count) {
        end = count;
    }
    for (size_t i = offset; i < end; i++) {
        const mcp_skill_t *sk = mcp_skill_registry_get_at(srv->skill_reg, i);
        if (sk == NULL) {
            continue;
        }
        mcp_json_value_t *item = mcp_skill_to_json(ctx, sk);
        if (item == NULL || mcp_json_array_append(ctx, arr, item) != MCP_OK) {
            mcp_json_destroy(ctx, item);
            mcp_json_destroy(ctx, arr);
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    if (mcp_json_object_set_take(ctx, result, "skills", arr) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    if (end < count) {
        if (set_next_cursor(ctx, result, end) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
    }
    if (decorate_result(ctx, srv, s, result, true) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

static mcp_message_t *route_skills_get(mcp_context_t *ctx, mcp_server_t *srv,
                                       mcp_session_t *s,
                                       const mcp_message_t *req) {
    if (srv->skill_reg == NULL) {
        return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "skills extension not enabled");
    }
    const mcp_json_value_t *params = mcp_message_params(ctx, req);
    const char *name = NULL;
    const char *uri = NULL;
    get_string(ctx, params, "name", &name);
    get_string(ctx, params, "uri", &uri);
    const char *lookup_key = (name != NULL && *name != '\0') ? name : uri;
    if (lookup_key == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "skills/get: missing name or uri");
    }
    const mcp_skill_t *sk = mcp_skill_registry_find(srv->skill_reg, lookup_key);
    if (sk == NULL) {
        return err_resp(ctx, req, MCP_RPC_INVALID_PARAMS, "skills/get: skill not found");
    }
    mcp_json_value_t *result = mcp_skill_to_json(ctx, sk);
    if (result == NULL) {
        return NULL;
    }
    if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
        mcp_json_destroy(ctx, result);
        return NULL;
    }
    mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
    if (resp == NULL) {
        mcp_json_destroy(ctx, result);
    }
    return resp;
}

mcp_message_t *mcp_server_route_request(mcp_context_t *ctx, mcp_server_t *srv,
                                        mcp_session_t *s, const mcp_message_t *req,
                                        const char *method) {
    if (mcp_proto_policy_method_deprecated(ctx, mcp_session_negotiated(ctx, s), method)) {
        dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=deprecated_method method=%s version=2026", method);
        return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "method deprecated in 2026-07-28");
    }
    if (strcmp(method, k_mcp_server_methods[0]) == 0) {
        return route_initialize(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[1]) == 0) {
        mcp_json_value_t *result = mcp_json_object_new(ctx);
        if (result == NULL) {
            return NULL;
        }
        if (decorate_result(ctx, srv, s, result, false) != MCP_OK) {
            mcp_json_destroy(ctx, result);
            return NULL;
        }
        mcp_message_t *resp = mcp_response_ok_new(ctx, req, result);
        if (resp == NULL) {
            mcp_json_destroy(ctx, result);
        }
        return resp;
    }
    if (strcmp(method, k_mcp_server_methods[2]) == 0) {
        return route_tools_list(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[3]) == 0) {
        return route_tools_call(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[4]) == 0) {
        return route_resources_list(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[5]) == 0) {
        return route_resources_read(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[6]) == 0) {
        return route_prompts_list(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[7]) == 0) {
        return route_prompts_get(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[8]) == 0) {
        return route_completion_list(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[9]) == 0) {
        return route_completion_complete(ctx, srv, s, req);
    }
    mcp_message_t *adv = route_advanced(ctx, srv, s, req, method);
    if (adv != NULL) {
        return adv;
    }
    if (strcmp(method, k_mcp_server_methods[15]) == 0) {
        return route_server_discover(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[16]) == 0) {
        return route_subscriptions_listen(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[17]) == 0) {
        return route_tasks_get(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[18]) == 0) {
        return route_tasks_update(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[19]) == 0) {
        return route_tasks_cancel(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[20]) == 0) {
        return route_skills_list(ctx, srv, s, req);
    }
    if (strcmp(method, k_mcp_server_methods[21]) == 0) {
        return route_skills_get(ctx, srv, s, req);
    }
    dlogf_srv(ctx, srv, MCP_LOG_WARN, "event=unknown_method method=%s", method);
    return err_resp(ctx, req, MCP_RPC_METHOD_NOT_FOUND, "unknown method");
}
