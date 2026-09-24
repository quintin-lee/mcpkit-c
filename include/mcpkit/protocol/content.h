/**
 * @file content.h
 * @brief MCP Content Annotations and annotated content item builders (SEP-1249).
 *
 * Implements audience tagging ("user", "assistant"), priority (0.0 - 1.0),
 * and lastModified ISO-8601 annotations for TextContent, ImageContent,
 * and EmbeddedResource content items.
 */

#ifndef MCPKIT_PROTOCOL_CONTENT_H
#define MCPKIT_PROTOCOL_CONTENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/core/error.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/value.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mcp_content_audience {
    MCP_AUDIENCE_NONE      = 0,
    MCP_AUDIENCE_USER      = (1 << 0),
    MCP_AUDIENCE_ASSISTANT = (1 << 1),
    MCP_AUDIENCE_ALL       = (MCP_AUDIENCE_USER | MCP_AUDIENCE_ASSISTANT)
} mcp_content_audience_t;

typedef struct mcp_content_annotations {
    uint32_t audience;      /* Bitmask of mcp_content_audience_t */
    double priority;        /* 0.0 to 1.0; negative (e.g. -1.0) if unspecified */
    char last_modified[64]; /* ISO-8601 timestamp (e.g. "2026-09-24T00:00:00Z"), or empty */
} mcp_content_annotations_t;

/**
 * @brief Converts an annotations struct to a JSON object.
 *
 * @param ctx Context; may be NULL.
 * @param annotations Source annotations; must not be NULL.
 * @return Owned JSON object, or NULL on OOM.
 */
mcp_json_value_t *mcp_annotations_to_json(mcp_context_t *ctx,
                                          const mcp_content_annotations_t *annotations);

/**
 * @brief Parses an annotations JSON object into an annotations struct.
 *
 * @param ctx Context; may be NULL.
 * @param ann_obj JSON object containing "audience", "priority", and/or "lastModified".
 * @param out_annotations Destination struct.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT on NULL or invalid structure.
 */
mcp_status_t mcp_annotations_from_json(mcp_context_t *ctx,
                                       const mcp_json_value_t *ann_obj,
                                       mcp_content_annotations_t *out_annotations);

/**
 * @brief Creates a TextContent item with optional annotations.
 *
 * Result shape: {"type":"text","text":"...","annotations":{...}}
 *
 * @param ctx Context; may be NULL.
 * @param text UTF-8 text string; not owned, copied internally.
 * @param annotations Optional annotations; may be NULL.
 * @return Owned JSON object, or NULL on OOM.
 */
mcp_json_value_t *mcp_content_text_new_annotated(mcp_context_t *ctx,
                                                 const char *text,
                                                 const mcp_content_annotations_t *annotations);

/**
 * @brief Creates an ImageContent item with optional annotations.
 *
 * Result shape: {"type":"image","data":"...","mimeType":"...","annotations":{...}}
 *
 * @param ctx Context; may be NULL.
 * @param data_base64 Base64-encoded image data; not owned.
 * @param mime_type MIME type string (e.g. "image/png"); not owned.
 * @param annotations Optional annotations; may be NULL.
 * @return Owned JSON object, or NULL on OOM.
 */
mcp_json_value_t *mcp_content_image_new_annotated(mcp_context_t *ctx,
                                                  const char *data_base64,
                                                  const char *mime_type,
                                                  const mcp_content_annotations_t *annotations);

/**
 * @brief Creates an EmbeddedResource item with optional annotations.
 *
 * Result shape: {"type":"resource","resource":{"uri":"...","mimeType":"...",
 *                "text"|"blob":"..."},"annotations":{...}}
 *
 * @param ctx Context; may be NULL.
 * @param uri Resource URI; not owned.
 * @param mime_type Optional MIME type; not owned, may be NULL.
 * @param text_or_blob Content text string or Base64 blob; not owned.
 * @param is_binary True if text_or_blob is a base64 blob ("blob" field), false if UTF-8 ("text").
 * @param annotations Optional annotations; may be NULL.
 * @return Owned JSON object, or NULL on OOM.
 */
mcp_json_value_t *mcp_content_resource_new_annotated(mcp_context_t *ctx,
                                                     const char *uri,
                                                     const char *mime_type,
                                                     const char *text_or_blob,
                                                     bool is_binary,
                                                     const mcp_content_annotations_t *annotations);

/**
 * @brief Extracts annotations from a content item JSON object.
 *
 * Reads content_obj["annotations"] if present and populates out_annotations.
 * If absent, clears out_annotations and returns MCP_OK.
 *
 * @param ctx Context; may be NULL.
 * @param content_obj Content JSON object.
 * @param out_annotations Destination struct.
 * @return MCP_OK on success; MCP_ERR_INVALID_ARGUMENT on invalid format.
 */
mcp_status_t mcp_content_extract_annotations(mcp_context_t *ctx,
                                             const mcp_json_value_t *content_obj,
                                             mcp_content_annotations_t *out_annotations);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_CONTENT_H */
