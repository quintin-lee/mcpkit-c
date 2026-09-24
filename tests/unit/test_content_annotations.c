#include "mcpkit/protocol/content.h"
#include "mcpkit/json/json.h"
#include "test_check.h"
#include <string.h>

int main(void) {
    mcp_context_t *ctx = NULL;

    /* Test 1: Null argument handling */
    mcp_content_annotations_t ann;
    memset(&ann, 0, sizeof(ann));
    CHECK(mcp_annotations_to_json(ctx, NULL) == NULL);
    CHECK(mcp_annotations_from_json(ctx, NULL, &ann) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_annotations_from_json(ctx, (const mcp_json_value_t *)1, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_content_text_new_annotated(ctx, NULL, NULL) == NULL);
    CHECK(mcp_content_image_new_annotated(ctx, NULL, "image/png", NULL) == NULL);
    CHECK(mcp_content_image_new_annotated(ctx, "abc", NULL, NULL) == NULL);
    CHECK(mcp_content_resource_new_annotated(ctx, NULL, NULL, "hi", false, NULL) == NULL);
    CHECK(mcp_content_resource_new_annotated(ctx, "res://1", NULL, NULL, false, NULL) == NULL);
    CHECK(mcp_content_extract_annotations(ctx, NULL, &ann) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_content_extract_annotations(ctx, (const mcp_json_value_t *)1, NULL) == MCP_ERR_INVALID_ARGUMENT);

    /* Test 2: Text content with full annotations */
    ann.audience = MCP_AUDIENCE_ALL;
    ann.priority = 0.85;
    strncpy(ann.last_modified, "2026-09-24T10:00:00Z", sizeof(ann.last_modified) - 1);

    mcp_json_value_t *txt = mcp_content_text_new_annotated(ctx, "Hello annotations", &ann);
    CHECK(txt != NULL);

    const char *type_str = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, txt, "type"), &type_str) == MCP_OK);
    CHECK(strcmp(type_str, "text") == 0);

    const char *txt_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, txt, "text"), &txt_val) == MCP_OK);
    CHECK(strcmp(txt_val, "Hello annotations") == 0);

    /* Extract annotations back and verify */
    mcp_content_annotations_t extracted;
    memset(&extracted, 0, sizeof(extracted));
    CHECK(mcp_content_extract_annotations(ctx, txt, &extracted) == MCP_OK);
    CHECK(extracted.audience == MCP_AUDIENCE_ALL);
    CHECK(extracted.priority >= 0.849 && extracted.priority <= 0.851);
    CHECK(strcmp(extracted.last_modified, "2026-09-24T10:00:00Z") == 0);
    mcp_json_destroy(ctx, txt);

    /* Test 3: Image content with partial annotations (audience only) */
    memset(&ann, 0, sizeof(ann));
    ann.audience = MCP_AUDIENCE_USER;
    ann.priority = -1.0; /* unspecified */

    mcp_json_value_t *img = mcp_content_image_new_annotated(ctx, "iVBORw0KGgo=", "image/png", &ann);
    CHECK(img != NULL);
    memset(&extracted, 0, sizeof(extracted));
    CHECK(mcp_content_extract_annotations(ctx, img, &extracted) == MCP_OK);
    CHECK(extracted.audience == MCP_AUDIENCE_USER);
    CHECK(extracted.priority < 0.0);
    CHECK(extracted.last_modified[0] == '\0');
    mcp_json_destroy(ctx, img);

    /* Test 4: Resource content without annotations */
    mcp_json_value_t *res = mcp_content_resource_new_annotated(ctx, "file:///report.txt", "text/plain", "report contents", false, NULL);
    CHECK(res != NULL);
    CHECK(mcp_json_object_get(ctx, res, "annotations") == NULL);
    memset(&extracted, 0, sizeof(extracted));
    extracted.priority = 0.5;
    CHECK(mcp_content_extract_annotations(ctx, res, &extracted) == MCP_OK);
    CHECK(extracted.audience == MCP_AUDIENCE_NONE);
    CHECK(extracted.priority < 0.0);
    mcp_json_destroy(ctx, res);

    /* Test 5: Embedded binary resource with priority annotation */
    memset(&ann, 0, sizeof(ann));
    ann.priority = 0.3;
    mcp_json_value_t *bin_res = mcp_content_resource_new_annotated(ctx, "bin:///data", "application/octet-stream", "AAAA", true, &ann);
    CHECK(bin_res != NULL);
    const mcp_json_value_t *res_inner = mcp_json_object_get(ctx, bin_res, "resource");
    CHECK(res_inner != NULL);
    const char *blob_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, res_inner, "blob"), &blob_val) == MCP_OK);
    CHECK(strcmp(blob_val, "AAAA") == 0);

    memset(&extracted, 0, sizeof(extracted));
    CHECK(mcp_content_extract_annotations(ctx, bin_res, &extracted) == MCP_OK);
    CHECK(extracted.priority >= 0.299 && extracted.priority <= 0.301);
    mcp_json_destroy(ctx, bin_res);

    printf("test_content_annotations OK\n");
    return 0;
}
