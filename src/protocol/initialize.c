/**
 * @file initialize.c
 *
 * initialize handshake builders and protocol negotiation.
 *
 * mcp_protocol_negotiate returns a pointer to a static string in
 * k_supported (no allocation, no ownership transfer). Returns NULL
 * when client_version is NULL or does not match any supported version;
 * callers should fall back to MCP_PROTOCOL_VERSION_LATEST in that case.
 */
#include "mcpkit/protocol/initialize.h"

#include <string.h>

#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/protocol/message.h"

const char *mcp_proto_version_string(mcp_proto_version_t v) {
    switch (v) {
        case MCP_PROTO_2026_07_28: return MCP_PROTOCOL_VERSION_2026_07_28;
        case MCP_PROTO_2025_11_25: return MCP_PROTOCOL_VERSION_2025_11_25;
        case MCP_PROTO_2025_06_18: return "2025-06-18";
        case MCP_PROTO_2025_03_26: return "2025-03-26";
        case MCP_PROTO_2024_11_05: return "2024-11-05";
        case MCP_PROTO_NONE:
        default:
            return NULL;
    }
}

mcp_proto_version_t mcp_proto_version_parse(const char *s) {
    if (s == NULL) {
        return MCP_PROTO_NONE;
    }
    if (strcmp(s, MCP_PROTOCOL_VERSION_2026_07_28) == 0) return MCP_PROTO_2026_07_28;
    if (strcmp(s, MCP_PROTOCOL_VERSION_2025_11_25) == 0) return MCP_PROTO_2025_11_25;
    if (strcmp(s, "2025-06-18") == 0) return MCP_PROTO_2025_06_18;
    if (strcmp(s, "2025-03-26") == 0) return MCP_PROTO_2025_03_26;
    if (strcmp(s, "2024-11-05") == 0) return MCP_PROTO_2024_11_05;
    return MCP_PROTO_NONE;
}

bool mcp_proto_at_least(mcp_proto_version_t v, mcp_proto_version_t min) {
    if (v == MCP_PROTO_NONE || min == MCP_PROTO_NONE) {
        return false;
    }
    return v >= min;
}

// Supported protocol versions, most-recent first.
static const char *const k_supported[] = {
    MCP_PROTOCOL_VERSION_2026_07_28,
    MCP_PROTOCOL_VERSION_2025_11_25,
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
};

static mcp_json_value_t *info_block(mcp_context_t *ctx, const char *name,
                                    const char *version) {
    mcp_json_value_t *info = mcp_json_object_new(ctx);
    mcp_json_value_t *n = info != NULL ? mcp_json_string_new(ctx, name) : NULL;
    mcp_json_value_t *v = n != NULL ? mcp_json_string_new(ctx, version) : NULL;
    if (info == NULL || n == NULL || v == NULL) {
        if (info != NULL) {
            mcp_json_destroy(ctx, info);
        }
        if (n != NULL) {
            mcp_json_destroy(ctx, n);
        }
        if (v != NULL) {
            mcp_json_destroy(ctx, v);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, info, "name", n) != MCP_OK) {
        mcp_json_destroy(ctx, n);
        mcp_json_destroy(ctx, v);
        mcp_json_destroy(ctx, info);
        return NULL;
    }
    if (mcp_json_object_set(ctx, info, "version", v) != MCP_OK) {
        mcp_json_destroy(ctx, v);
        mcp_json_destroy(ctx, info);
        return NULL;
    }
    return info;
}

mcp_json_value_t *mcp_initialize_params_new_v(mcp_context_t *ctx, const char *protocol_version,
                                              const char *client_name,
                                              const char *client_version) {
    if (protocol_version == NULL || client_name == NULL || client_version == NULL) {
        return NULL;
    }
    mcp_json_value_t *params = mcp_json_object_new(ctx);
    mcp_json_value_t *pv = params != NULL ? mcp_json_string_new(ctx, protocol_version) : NULL;
    mcp_json_value_t *caps = pv != NULL ? mcp_json_object_new(ctx) : NULL;
    mcp_json_value_t *ci = caps != NULL ? info_block(ctx, client_name, client_version) : NULL;
    if (params == NULL || pv == NULL || caps == NULL || ci == NULL) {
        if (params != NULL) {
            mcp_json_destroy(ctx, params);
        }
        if (pv != NULL) {
            mcp_json_destroy(ctx, pv);
        }
        if (caps != NULL) {
            mcp_json_destroy(ctx, caps);
        }
        if (ci != NULL) {
            mcp_json_destroy(ctx, ci);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, params, "protocolVersion", pv) != MCP_OK) {
        mcp_json_destroy(ctx, pv);
        mcp_json_destroy(ctx, caps);
        mcp_json_destroy(ctx, ci);
        mcp_json_destroy(ctx, params);
        return NULL;
    }
    if (mcp_json_object_set(ctx, params, "capabilities", caps) != MCP_OK) {
        mcp_json_destroy(ctx, caps);
        mcp_json_destroy(ctx, ci);
        mcp_json_destroy(ctx, params);
        return NULL;
    }
    if (mcp_json_object_set(ctx, params, "clientInfo", ci) != MCP_OK) {
        mcp_json_destroy(ctx, ci);
        mcp_json_destroy(ctx, params);
        return NULL;
    }
    return params;
}

mcp_json_value_t *mcp_initialize_params_new(mcp_context_t *ctx, const char *client_name,
                                            const char *client_version) {
    return mcp_initialize_params_new_v(ctx, MCP_PROTOCOL_VERSION_LATEST, client_name,
                                       client_version);
}

static mcp_status_t require_string_field(mcp_context_t *ctx, const mcp_json_value_t *obj,
                                         const char *key) {
    const mcp_json_value_t *f = mcp_json_object_get(ctx, obj, key);
    const char *s = NULL;
    if (f == NULL || mcp_json_string_value(ctx, f, &s) != MCP_OK || s == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return MCP_OK;
}

mcp_status_t mcp_initialize_params_validate(mcp_context_t *ctx, const mcp_json_value_t *params) {
    if (params == NULL || mcp_json_type(ctx, params) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (require_string_field(ctx, params, "protocolVersion") != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_json_value_t *caps = mcp_json_object_get(ctx, params, "capabilities");
    if (caps == NULL || mcp_json_type(ctx, caps) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    const mcp_json_value_t *ci = mcp_json_object_get(ctx, params, "clientInfo");
    if (ci == NULL || mcp_json_type(ctx, ci) != MCP_JSON_OBJECT) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (require_string_field(ctx, ci, "name") != MCP_OK ||
        require_string_field(ctx, ci, "version") != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    return MCP_OK;
}

const char *mcp_protocol_negotiate(mcp_context_t *ctx, const char *client_version) {
    (void)ctx;
    if (client_version == NULL) {
        return NULL;
    }
    if (strcmp(client_version, MCP_PROTOCOL_VERSION_2026_07_28) == 0) {
        return MCP_PROTOCOL_VERSION_2025_11_25;
    }
    for (size_t i = 1; i < sizeof(k_supported) / sizeof(k_supported[0]); i++) {
        if (strcmp(client_version, k_supported[i]) == 0) {
            return k_supported[i];
        }
    }
    return NULL;
}

mcp_json_value_t *mcp_initialize_result_new_v(mcp_context_t *ctx,
                                              mcp_proto_version_t negotiated,
                                              const char *server_name,
                                              const char *server_version) {
    if (server_name == NULL || server_version == NULL) {
        return NULL;
    }
    const char *ver_str = mcp_proto_version_string(negotiated);
    if (ver_str == NULL) {
        ver_str = MCP_PROTOCOL_VERSION_LATEST;
    }
    mcp_json_value_t *res = mcp_json_object_new(ctx);
    mcp_json_value_t *pv = res != NULL ? mcp_json_string_new(ctx, ver_str) : NULL;
    mcp_json_value_t *caps = pv != NULL ? mcp_json_object_new(ctx) : NULL;
    mcp_json_value_t *si = caps != NULL ? info_block(ctx, server_name, server_version) : NULL;
    if (res == NULL || pv == NULL || caps == NULL || si == NULL) {
        if (res != NULL) {
            mcp_json_destroy(ctx, res);
        }
        if (pv != NULL) {
            mcp_json_destroy(ctx, pv);
        }
        if (caps != NULL) {
            mcp_json_destroy(ctx, caps);
        }
        if (si != NULL) {
            mcp_json_destroy(ctx, si);
        }
        return NULL;
    }
    if (mcp_json_object_set(ctx, res, "protocolVersion", pv) != MCP_OK) {
        mcp_json_destroy(ctx, pv);
        mcp_json_destroy(ctx, caps);
        mcp_json_destroy(ctx, si);
        mcp_json_destroy(ctx, res);
        return NULL;
    }
    if (mcp_json_object_set(ctx, res, "capabilities", caps) != MCP_OK) {
        mcp_json_destroy(ctx, caps);
        mcp_json_destroy(ctx, si);
        mcp_json_destroy(ctx, res);
        return NULL;
    }
    if (mcp_json_object_set(ctx, res, "serverInfo", si) != MCP_OK) {
        mcp_json_destroy(ctx, si);
        mcp_json_destroy(ctx, res);
        return NULL;
    }
    return res;
}

mcp_json_value_t *mcp_initialize_result_new(mcp_context_t *ctx, const char *server_name,
                                            const char *server_version) {
    return mcp_initialize_result_new_v(ctx, MCP_PROTO_NONE, server_name, server_version);
}

mcp_message_t *mcp_initialized_notification_new(mcp_context_t *ctx) {
    return mcp_notification_new(ctx, "notifications/initialized", NULL);
}
