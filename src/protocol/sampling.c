/**
 * @file sampling.c
 * @brief Sampling with Tools protocol implementation (SEP-1577).
 * @ingroup mcpkit-protocol
 */

#include "mcpkit/protocol/sampling.h"
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"

mcp_json_value_t *mcp_sampling_params_new(mcp_context_t *ctx, uint32_t max_tokens) {
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        return NULL;
    }

    mcp_json_object_set_take(ctx, obj, "maxTokens", mcp_json_number_new(ctx, (double)max_tokens));

    mcp_json_value_t *msgs = mcp_json_array_new(ctx);
    if (msgs != NULL) {
        mcp_json_object_set_take(ctx, obj, "messages", msgs);
    }
    return obj;
}

mcp_status_t mcp_sampling_params_add_tool(mcp_context_t *ctx, mcp_json_value_t *params,
                                          const mcp_sampling_tool_def_t *tool) {
    if (params == NULL || tool == NULL || tool->name == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *tools = (mcp_json_value_t *)mcp_json_object_get(ctx, params, "tools");
    if (tools == NULL) {
        tools = mcp_json_array_new(ctx);
        if (tools == NULL) {
            return MCP_ERR_NOMEM;
        }
        if (mcp_json_object_set_take(ctx, params, "tools", tools) != MCP_OK) {
            mcp_json_destroy(ctx, tools);
            return MCP_ERR_NOMEM;
        }
    }

    mcp_json_value_t *tobj = mcp_json_object_new(ctx);
    if (tobj == NULL) {
        return MCP_ERR_NOMEM;
    }

    mcp_json_object_set_take(ctx, tobj, "name", mcp_json_string_new(ctx, tool->name));
    if (tool->description != NULL) {
        mcp_json_object_set_take(ctx, tobj, "description", mcp_json_string_new(ctx, tool->description));
    }
    if (tool->input_schema != NULL) {
        mcp_json_value_t *cloned_schema = mcp_json_clone(ctx, tool->input_schema);
        if (cloned_schema != NULL) {
            mcp_json_object_set_take(ctx, tobj, "inputSchema", cloned_schema);
        }
    }

    if (mcp_json_array_append(ctx, tools, tobj) != MCP_OK) {
        mcp_json_destroy(ctx, tobj);
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

mcp_status_t mcp_sampling_params_set_tool_choice(mcp_context_t *ctx, mcp_json_value_t *params,
                                                 mcp_sampling_tool_choice_t mode,
                                                 const char *specific_tool_name) {
    if (params == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mode == MCP_TOOL_CHOICE_SPECIFIC && (specific_tool_name == NULL || specific_tool_name[0] == '\0')) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    mcp_json_value_t *tc = mcp_json_object_new(ctx);
    if (tc == NULL) {
        return MCP_ERR_NOMEM;
    }

    const char *mode_str = "auto";
    if (mode == MCP_TOOL_CHOICE_NONE) {
        mode_str = "none";
    } else if (mode == MCP_TOOL_CHOICE_REQUIRED) {
        mode_str = "required";
    } else if (mode == MCP_TOOL_CHOICE_SPECIFIC) {
        mode_str = "tool";
    }

    mcp_json_object_set_take(ctx, tc, "mode", mcp_json_string_new(ctx, mode_str));
    if (mode == MCP_TOOL_CHOICE_SPECIFIC) {
        mcp_json_object_set_take(ctx, tc, "name", mcp_json_string_new(ctx, specific_tool_name));
    }

    if (mcp_json_object_set_take(ctx, params, "toolChoice", tc) != MCP_OK) {
        mcp_json_destroy(ctx, tc);
        return MCP_ERR_NOMEM;
    }
    return MCP_OK;
}

size_t mcp_sampling_params_get_tool_count(mcp_context_t *ctx, const mcp_json_value_t *params) {
    if (params == NULL) {
        return 0;
    }
    const mcp_json_value_t *tools = mcp_json_object_get(ctx, params, "tools");
    if (tools == NULL || mcp_json_type(ctx, tools) != MCP_JSON_ARRAY) {
        return 0;
    }
    return mcp_json_array_size(ctx, tools);
}

const mcp_json_value_t *mcp_sampling_params_get_tool_at(mcp_context_t *ctx,
                                                        const mcp_json_value_t *params,
                                                        size_t index) {
    if (params == NULL) {
        return NULL;
    }
    const mcp_json_value_t *tools = mcp_json_object_get(ctx, params, "tools");
    if (tools == NULL || mcp_json_type(ctx, tools) != MCP_JSON_ARRAY) {
        return NULL;
    }
    return mcp_json_array_get(ctx, tools, index);
}

mcp_status_t mcp_sampling_params_get_tool_choice(mcp_context_t *ctx, const mcp_json_value_t *params,
                                                 mcp_sampling_tool_choice_t *out_mode,
                                                 const char **out_specific_tool_name) {
    if (params == NULL || out_mode == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (out_specific_tool_name != NULL) {
        *out_specific_tool_name = NULL;
    }

    const mcp_json_value_t *tc = mcp_json_object_get(ctx, params, "toolChoice");
    if (tc == NULL) {
        return MCP_ERR_NOT_FOUND;
    }

    if (mcp_json_type(ctx, tc) == MCP_JSON_STRING) {
        const char *s = NULL;
        mcp_json_string_value(ctx, tc, &s);
        if (s == NULL) {
            return MCP_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(s, "auto") == 0) {
            *out_mode = MCP_TOOL_CHOICE_AUTO;
        } else if (strcmp(s, "none") == 0) {
            *out_mode = MCP_TOOL_CHOICE_NONE;
        } else if (strcmp(s, "required") == 0) {
            *out_mode = MCP_TOOL_CHOICE_REQUIRED;
        } else {
            return MCP_ERR_INVALID_ARGUMENT;
        }
        return MCP_OK;
    }

    if (mcp_json_type(ctx, tc) == MCP_JSON_OBJECT) {
        const mcp_json_value_t *mv = mcp_json_object_get(ctx, tc, "mode");
        if (mv == NULL) {
            return MCP_ERR_INVALID_ARGUMENT;
        }
        const char *mstr = NULL;
        if (mcp_json_string_value(ctx, mv, &mstr) != MCP_OK || mstr == NULL) {
            return MCP_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(mstr, "auto") == 0) {
            *out_mode = MCP_TOOL_CHOICE_AUTO;
        } else if (strcmp(mstr, "none") == 0) {
            *out_mode = MCP_TOOL_CHOICE_NONE;
        } else if (strcmp(mstr, "required") == 0) {
            *out_mode = MCP_TOOL_CHOICE_REQUIRED;
        } else if (strcmp(mstr, "tool") == 0) {
            *out_mode = MCP_TOOL_CHOICE_SPECIFIC;
            if (out_specific_tool_name != NULL) {
                const mcp_json_value_t *nv = mcp_json_object_get(ctx, tc, "name");
                if (nv != NULL) {
                    mcp_json_string_value(ctx, nv, out_specific_tool_name);
                }
            }
        } else {
            return MCP_ERR_INVALID_ARGUMENT;
        }
        return MCP_OK;
    }

    return MCP_ERR_INVALID_ARGUMENT;
}

mcp_json_value_t *mcp_sampling_content_tool_use_new(mcp_context_t *ctx, const char *id,
                                                    const char *name, mcp_json_value_t *input) {
    if (id == NULL || name == NULL) {
        if (input != NULL) {
            mcp_json_destroy(ctx, input);
        }
        return NULL;
    }
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        if (input != NULL) {
            mcp_json_destroy(ctx, input);
        }
        return NULL;
    }

    mcp_json_object_set_take(ctx, obj, "type", mcp_json_string_new(ctx, "tool_use"));
    mcp_json_object_set_take(ctx, obj, "id", mcp_json_string_new(ctx, id));
    mcp_json_object_set_take(ctx, obj, "name", mcp_json_string_new(ctx, name));

    if (input == NULL) {
        input = mcp_json_object_new(ctx);
    }
    if (input != NULL) {
        mcp_json_object_set_take(ctx, obj, "input", input);
    }
    return obj;
}

mcp_json_value_t *mcp_sampling_content_tool_result_new(mcp_context_t *ctx, const char *tool_use_id,
                                                       const char *content, bool is_error) {
    if (tool_use_id == NULL) {
        return NULL;
    }
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        return NULL;
    }

    mcp_json_object_set_take(ctx, obj, "type", mcp_json_string_new(ctx, "tool_result"));
    mcp_json_object_set_take(ctx, obj, "toolUseId", mcp_json_string_new(ctx, tool_use_id));
    mcp_json_object_set_take(ctx, obj, "content", mcp_json_string_new(ctx, content != NULL ? content : ""));
    mcp_json_object_set_take(ctx, obj, "isError", mcp_json_bool_new(ctx, is_error));
    return obj;
}
