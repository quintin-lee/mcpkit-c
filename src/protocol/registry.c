/**
 * @file registry.c
 * @brief MCP Server Registry Manifest (mcp.json) implementation.
 * @ingroup mcpkit-protocol
 */

#include "mcpkit/protocol/registry.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/core/types.h"
#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"

static const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static char *reg_strdup(mcp_context_t *ctx, const char *s) {
    if (s == NULL) {
        return NULL;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    size_t n = strlen(s);
    char *out = a->malloc_fn(n + 1, a->userdata);
    if (out != NULL) {
        memcpy(out, s, n + 1);
    }
    return out;
}

static char *get_obj_string(mcp_context_t *ctx, const mcp_json_value_t *obj, const char *key) {
    const mcp_json_value_t *v = mcp_json_object_get(ctx, obj, key);
    if (v == NULL) {
        return NULL;
    }
    const char *str = NULL;
    if (mcp_json_string_value(ctx, v, &str) != MCP_OK || str == NULL) {
        return NULL;
    }
    return reg_strdup(ctx, str);
}

mcp_status_t mcp_registry_manifest_parse(mcp_context_t *ctx, const char *json_str,
                                         size_t len, mcp_registry_manifest_t *m_out) {
    if (json_str == NULL || m_out == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    memset(m_out, 0, sizeof(*m_out));

    mcp_json_value_t *root = mcp_json_parse(ctx, json_str, len);
    if (root == NULL) {
        return MCP_ERR_PROTOCOL;
    }
    if (mcp_json_type(ctx, root) != MCP_JSON_OBJECT) {
        mcp_json_destroy(ctx, root);
        return MCP_ERR_PROTOCOL;
    }

    m_out->name = get_obj_string(ctx, root, "name");
    m_out->version = get_obj_string(ctx, root, "version");
    m_out->description = get_obj_string(ctx, root, "description");
    m_out->author = get_obj_string(ctx, root, "author");
    m_out->license = get_obj_string(ctx, root, "license");
    m_out->repository = get_obj_string(ctx, root, "repository");

    const mcp_json_value_t *tv = mcp_json_object_get(ctx, root, "transport");
    if (tv != NULL) {
        const char *ts = NULL;
        if (mcp_json_string_value(ctx, tv, &ts) == MCP_OK && ts != NULL) {
            if (strcmp(ts, "stdio") == 0) {
                m_out->transport = MCP_TRANSPORT_KIND_STDIO;
            } else if (strcmp(ts, "streamable_http") == 0 || strcmp(ts, "streamable-http") == 0 || strcmp(ts, "http") == 0) {
                m_out->transport = MCP_TRANSPORT_KIND_STREAMABLE_HTTP;
            } else if (strcmp(ts, "socket") == 0) {
                m_out->transport = MCP_TRANSPORT_KIND_SOCKET;
            } else {
                m_out->transport = MCP_TRANSPORT_KIND_UNKNOWN;
            }
        }
    }

    const mcp_json_value_t *caps = mcp_json_object_get(ctx, root, "capabilities");
    if (caps != NULL && mcp_json_type(ctx, caps) == MCP_JSON_OBJECT) {
        const mcp_json_value_t *b;
        bool val = false;
        b = mcp_json_object_get(ctx, caps, "tools");
        if (b != NULL && mcp_json_bool_value(ctx, b, &val) == MCP_OK) {
            m_out->has_tools = val;
        }
        val = false;
        b = mcp_json_object_get(ctx, caps, "resources");
        if (b != NULL && mcp_json_bool_value(ctx, b, &val) == MCP_OK) {
            m_out->has_resources = val;
        }
        val = false;
        b = mcp_json_object_get(ctx, caps, "prompts");
        if (b != NULL && mcp_json_bool_value(ctx, b, &val) == MCP_OK) {
            m_out->has_prompts = val;
        }
    }

    mcp_json_destroy(ctx, root);
    return MCP_OK;
}

static bool is_valid_name(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return true;
}

static bool is_valid_version(const char *ver) {
    if (ver == NULL || ver[0] == '\0') {
        return false;
    }
    /* Simple SemVer check: begins with digit, contains at least one dot */
    if (!isdigit((unsigned char)ver[0])) {
        return false;
    }
    const char *dot = strchr(ver, '.');
    return dot != NULL;
}

mcp_status_t mcp_registry_manifest_validate(mcp_context_t *ctx, const mcp_registry_manifest_t *m,
                                            char *err_msg_out) {
    (void)ctx;
    if (m == NULL) {
        if (err_msg_out != NULL) {
            snprintf(err_msg_out, 128, "Manifest is NULL");
        }
        return MCP_ERR_INVALID_ARGUMENT;
    }

    if (!is_valid_name(m->name)) {
        if (err_msg_out != NULL) {
            snprintf(err_msg_out, 128, "Missing or invalid required field 'name'");
        }
        return MCP_ERR_INVALID_ARGUMENT;
    }

    if (!is_valid_version(m->version)) {
        if (err_msg_out != NULL) {
            snprintf(err_msg_out, 128, "Missing or invalid required field 'version'");
        }
        return MCP_ERR_INVALID_ARGUMENT;
    }

    if (m->transport == MCP_TRANSPORT_KIND_UNKNOWN) {
        if (err_msg_out != NULL) {
            snprintf(err_msg_out, 128, "Missing or invalid 'transport' kind");
        }
        return MCP_ERR_INVALID_ARGUMENT;
    }

    if (err_msg_out != NULL) {
        err_msg_out[0] = '\0';
    }
    return MCP_OK;
}

char *mcp_registry_manifest_serialize(mcp_context_t *ctx, const mcp_registry_manifest_t *m) {
    if (m == NULL) {
        return NULL;
    }
    mcp_json_value_t *root = mcp_json_object_new(ctx);
    if (root == NULL) {
        return NULL;
    }

    if (m->name != NULL) {
        mcp_json_object_set_take(ctx, root, "name", mcp_json_string_new(ctx, m->name));
    }
    if (m->version != NULL) {
        mcp_json_object_set_take(ctx, root, "version", mcp_json_string_new(ctx, m->version));
    }
    if (m->description != NULL) {
        mcp_json_object_set_take(ctx, root, "description", mcp_json_string_new(ctx, m->description));
    }
    if (m->author != NULL) {
        mcp_json_object_set_take(ctx, root, "author", mcp_json_string_new(ctx, m->author));
    }
    if (m->license != NULL) {
        mcp_json_object_set_take(ctx, root, "license", mcp_json_string_new(ctx, m->license));
    }
    if (m->repository != NULL) {
        mcp_json_object_set_take(ctx, root, "repository", mcp_json_string_new(ctx, m->repository));
    }

    const char *ts = "stdio";
    if (m->transport == MCP_TRANSPORT_KIND_STREAMABLE_HTTP) {
        ts = "streamable_http";
    } else if (m->transport == MCP_TRANSPORT_KIND_SOCKET) {
        ts = "socket";
    } else if (m->transport == MCP_TRANSPORT_KIND_UNKNOWN) {
        ts = "unknown";
    }
    mcp_json_object_set_take(ctx, root, "transport", mcp_json_string_new(ctx, ts));

    mcp_json_value_t *caps = mcp_json_object_new(ctx);
    if (caps != NULL) {
        mcp_json_object_set_take(ctx, caps, "tools", mcp_json_bool_new(ctx, m->has_tools));
        mcp_json_object_set_take(ctx, caps, "resources", mcp_json_bool_new(ctx, m->has_resources));
        mcp_json_object_set_take(ctx, caps, "prompts", mcp_json_bool_new(ctx, m->has_prompts));
        mcp_json_object_set_take(ctx, root, "capabilities", caps);
    }

    char *out = mcp_json_serialize(ctx, root);
    mcp_json_destroy(ctx, root);
    return out;
}

void mcp_registry_free_string(mcp_context_t *ctx, char *str) {
    mcp_json_free_string(ctx, str);
}

void mcp_registry_manifest_cleanup(mcp_context_t *ctx, mcp_registry_manifest_t *m) {
    if (m == NULL) {
        return;
    }
    const mcp_allocator_t *a = alloc_of(ctx);
    if (m->name != NULL) {
        a->free_fn(m->name, a->userdata);
    }
    if (m->version != NULL) {
        a->free_fn(m->version, a->userdata);
    }
    if (m->description != NULL) {
        a->free_fn(m->description, a->userdata);
    }
    if (m->author != NULL) {
        a->free_fn(m->author, a->userdata);
    }
    if (m->license != NULL) {
        a->free_fn(m->license, a->userdata);
    }
    if (m->repository != NULL) {
        a->free_fn(m->repository, a->userdata);
    }
    memset(m, 0, sizeof(*m));
}
