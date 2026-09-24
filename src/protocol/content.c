/**
 * @file content.c
 * @brief MCP Content Annotations and annotated content item builders (SEP-1249).
 */

#include "mcpkit/protocol/content.h"

#include <string.h>

#include "mcpkit/json/array.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/value.h"

mcp_json_value_t *mcp_annotations_to_json(mcp_context_t *ctx,
                                          const mcp_content_annotations_t *annotations) {
    if (annotations == NULL) {
        return NULL;
    }
    mcp_json_value_t *ann = mcp_json_object_new(ctx);
    if (ann == NULL) {
        return NULL;
    }

    if (annotations->audience != MCP_AUDIENCE_NONE) {
        mcp_json_value_t *aud = mcp_json_array_new(ctx);
        if (aud == NULL) {
            mcp_json_destroy(ctx, ann);
            return NULL;
        }
        if (annotations->audience & MCP_AUDIENCE_USER) {
            mcp_json_value_t *u = mcp_json_string_new(ctx, "user");
            if (u == NULL) {
                mcp_json_destroy(ctx, aud);
                mcp_json_destroy(ctx, ann);
                return NULL;
            }
            if (mcp_json_array_append(ctx, aud, u) != MCP_OK) {
                mcp_json_destroy(ctx, u);
                mcp_json_destroy(ctx, aud);
                mcp_json_destroy(ctx, ann);
                return NULL;
            }
        }
        if (annotations->audience & MCP_AUDIENCE_ASSISTANT) {
            mcp_json_value_t *a = mcp_json_string_new(ctx, "assistant");
            if (a == NULL) {
                mcp_json_destroy(ctx, aud);
                mcp_json_destroy(ctx, ann);
                return NULL;
            }
            if (mcp_json_array_append(ctx, aud, a) != MCP_OK) {
                mcp_json_destroy(ctx, a);
                mcp_json_destroy(ctx, aud);
                mcp_json_destroy(ctx, ann);
                return NULL;
            }
        }
        if (mcp_json_object_set_take(ctx, ann, "audience", aud) != MCP_OK) {
            mcp_json_destroy(ctx, ann);
            return NULL;
        }
    }

    if (annotations->priority >= 0.0 && annotations->priority <= 1.0) {
        mcp_json_value_t *p = mcp_json_number_new(ctx, annotations->priority);
        if (p == NULL || mcp_json_object_set_take(ctx, ann, "priority", p) != MCP_OK) {
            mcp_json_destroy(ctx, p);
            mcp_json_destroy(ctx, ann);
            return NULL;
        }
    }

    if (annotations->last_modified[0] != '\0') {
        mcp_json_value_t *lm = mcp_json_string_new(ctx, annotations->last_modified);
        if (lm == NULL || mcp_json_object_set_take(ctx, ann, "lastModified", lm) != MCP_OK) {
            mcp_json_destroy(ctx, lm);
            mcp_json_destroy(ctx, ann);
            return NULL;
        }
    }

    return ann;
}

mcp_status_t mcp_annotations_from_json(mcp_context_t *ctx,
                                       const mcp_json_value_t *ann_obj,
                                       mcp_content_annotations_t *out_annotations) {
    if (ann_obj == NULL || out_annotations == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    memset(out_annotations, 0, sizeof(*out_annotations));
    out_annotations->priority = -1.0;

    const mcp_json_value_t *aud_v = mcp_json_object_get(ctx, ann_obj, "audience");
    if (aud_v != NULL && mcp_json_type(ctx, aud_v) == MCP_JSON_ARRAY) {
        size_t n = mcp_json_array_size(ctx, aud_v);
        for (size_t i = 0; i < n; i++) {
            const mcp_json_value_t *item = mcp_json_array_get(ctx, aud_v, i);
            const char *str = NULL;
            if (mcp_json_string_value(ctx, item, &str) == MCP_OK && str != NULL) {
                if (strcmp(str, "user") == 0) {
                    out_annotations->audience |= MCP_AUDIENCE_USER;
                } else if (strcmp(str, "assistant") == 0) {
                    out_annotations->audience |= MCP_AUDIENCE_ASSISTANT;
                }
            }
        }
    }

    const mcp_json_value_t *prio_v = mcp_json_object_get(ctx, ann_obj, "priority");
    if (prio_v != NULL && mcp_json_type(ctx, prio_v) == MCP_JSON_NUMBER) {
        double d = -1.0;
        if (mcp_json_number_value(ctx, prio_v, &d) == MCP_OK) {
            out_annotations->priority = d;
        }
    }

    const mcp_json_value_t *lm_v = mcp_json_object_get(ctx, ann_obj, "lastModified");
    if (lm_v != NULL && mcp_json_type(ctx, lm_v) == MCP_JSON_STRING) {
        const char *lm_str = NULL;
        if (mcp_json_string_value(ctx, lm_v, &lm_str) == MCP_OK && lm_str != NULL) {
            strncpy(out_annotations->last_modified, lm_str, sizeof(out_annotations->last_modified) - 1);
            out_annotations->last_modified[sizeof(out_annotations->last_modified) - 1] = '\0';
        }
    }

    return MCP_OK;
}

mcp_json_value_t *mcp_content_text_new_annotated(mcp_context_t *ctx,
                                                 const char *text,
                                                 const mcp_content_annotations_t *annotations) {
    if (text == NULL) {
        return NULL;
    }
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    mcp_json_value_t *type_v = obj != NULL ? mcp_json_string_new(ctx, "text") : NULL;
    mcp_json_value_t *text_v = obj != NULL ? mcp_json_string_new(ctx, text) : NULL;
    if (obj == NULL || type_v == NULL || text_v == NULL) {
        mcp_json_destroy(ctx, type_v);
        mcp_json_destroy(ctx, text_v);
        mcp_json_destroy(ctx, obj);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, obj, "type", type_v) != MCP_OK ||
        mcp_json_object_set_take(ctx, obj, "text", text_v) != MCP_OK) {
        mcp_json_destroy(ctx, text_v);
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    if (annotations != NULL) {
        mcp_json_value_t *ann_json = mcp_annotations_to_json(ctx, annotations);
        if (ann_json != NULL) {
            if (mcp_json_object_set_take(ctx, obj, "annotations", ann_json) != MCP_OK) {
                mcp_json_destroy(ctx, obj);
                return NULL;
            }
        }
    }

    return obj;
}

mcp_json_value_t *mcp_content_image_new_annotated(mcp_context_t *ctx,
                                                  const char *data_base64,
                                                  const char *mime_type,
                                                  const mcp_content_annotations_t *annotations) {
    if (data_base64 == NULL || mime_type == NULL) {
        return NULL;
    }
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    mcp_json_value_t *type_v = obj != NULL ? mcp_json_string_new(ctx, "image") : NULL;
    mcp_json_value_t *data_v = obj != NULL ? mcp_json_string_new(ctx, data_base64) : NULL;
    mcp_json_value_t *mime_v = obj != NULL ? mcp_json_string_new(ctx, mime_type) : NULL;
    if (obj == NULL || type_v == NULL || data_v == NULL || mime_v == NULL) {
        mcp_json_destroy(ctx, type_v);
        mcp_json_destroy(ctx, data_v);
        mcp_json_destroy(ctx, mime_v);
        mcp_json_destroy(ctx, obj);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, obj, "type", type_v) != MCP_OK ||
        mcp_json_object_set_take(ctx, obj, "data", data_v) != MCP_OK ||
        mcp_json_object_set_take(ctx, obj, "mimeType", mime_v) != MCP_OK) {
        mcp_json_destroy(ctx, mime_v);
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    if (annotations != NULL) {
        mcp_json_value_t *ann_json = mcp_annotations_to_json(ctx, annotations);
        if (ann_json != NULL) {
            if (mcp_json_object_set_take(ctx, obj, "annotations", ann_json) != MCP_OK) {
                mcp_json_destroy(ctx, obj);
                return NULL;
            }
        }
    }

    return obj;
}

mcp_json_value_t *mcp_content_resource_new_annotated(mcp_context_t *ctx,
                                                     const char *uri,
                                                     const char *mime_type,
                                                     const char *text_or_blob,
                                                     bool is_binary,
                                                     const mcp_content_annotations_t *annotations) {
    if (uri == NULL || text_or_blob == NULL) {
        return NULL;
    }
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    mcp_json_value_t *type_v = obj != NULL ? mcp_json_string_new(ctx, "resource") : NULL;
    mcp_json_value_t *res_inner = obj != NULL ? mcp_json_object_new(ctx) : NULL;
    if (obj == NULL || type_v == NULL || res_inner == NULL) {
        mcp_json_destroy(ctx, type_v);
        mcp_json_destroy(ctx, res_inner);
        mcp_json_destroy(ctx, obj);
        return NULL;
    }
    if (mcp_json_object_set_take(ctx, obj, "type", type_v) != MCP_OK) {
        mcp_json_destroy(ctx, res_inner);
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    mcp_json_value_t *uri_v = mcp_json_string_new(ctx, uri);
    const char *payload_key = is_binary ? "blob" : "text";
    mcp_json_value_t *payload_v = mcp_json_string_new(ctx, text_or_blob);
    if (uri_v == NULL || payload_v == NULL ||
        mcp_json_object_set_take(ctx, res_inner, "uri", uri_v) != MCP_OK ||
        mcp_json_object_set_take(ctx, res_inner, payload_key, payload_v) != MCP_OK) {
        mcp_json_destroy(ctx, payload_v);
        mcp_json_destroy(ctx, res_inner);
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    if (mime_type != NULL) {
        mcp_json_value_t *mime_v = mcp_json_string_new(ctx, mime_type);
        if (mime_v == NULL || mcp_json_object_set_take(ctx, res_inner, "mimeType", mime_v) != MCP_OK) {
            mcp_json_destroy(ctx, mime_v);
            mcp_json_destroy(ctx, res_inner);
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    if (mcp_json_object_set_take(ctx, obj, "resource", res_inner) != MCP_OK) {
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    if (annotations != NULL) {
        mcp_json_value_t *ann_json = mcp_annotations_to_json(ctx, annotations);
        if (ann_json != NULL) {
            if (mcp_json_object_set_take(ctx, obj, "annotations", ann_json) != MCP_OK) {
                mcp_json_destroy(ctx, obj);
                return NULL;
            }
        }
    }

    return obj;
}

mcp_status_t mcp_content_extract_annotations(mcp_context_t *ctx,
                                             const mcp_json_value_t *content_obj,
                                             mcp_content_annotations_t *out_annotations) {
    if (content_obj == NULL || out_annotations == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    memset(out_annotations, 0, sizeof(*out_annotations));
    out_annotations->priority = -1.0;

    const mcp_json_value_t *ann_v = mcp_json_object_get(ctx, content_obj, "annotations");
    if (ann_v == NULL) {
        return MCP_OK;
    }
    return mcp_annotations_from_json(ctx, ann_v, out_annotations);
}
