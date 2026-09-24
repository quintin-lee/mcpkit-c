#include <stdio.h>
#include <string.h>
#include "mcpkit/core/context.h"
#include "mcpkit/protocol/registry.h"
#include "test_check.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // 1. Parse valid mcp.json manifest
    const char *manifest_json =
        "{\"name\":\"mcp-echo-c\",\"version\":\"1.0.0\",\"description\":\"High performance C echo server\","
        "\"transport\":\"stdio\",\"repository\":\"https://github.com/example/mcp-echo-c\","
        "\"capabilities\":{\"tools\":true,\"resources\":false,\"prompts\":true}}";

    mcp_registry_manifest_t m;
    memset(&m, 0, sizeof(m));
    CHECK(mcp_registry_manifest_parse(ctx, manifest_json, strlen(manifest_json), &m) == MCP_OK);
    CHECK(m.name != NULL && strcmp(m.name, "mcp-echo-c") == 0);
    CHECK(m.version != NULL && strcmp(m.version, "1.0.0") == 0);
    CHECK(m.transport == MCP_TRANSPORT_KIND_STDIO);
    CHECK(m.has_tools == true);
    CHECK(m.has_resources == false);
    CHECK(m.has_prompts == true);

    // 2. Validate manifest rules
    CHECK(mcp_registry_manifest_validate(ctx, &m, NULL) == MCP_OK);

    // 3. Serialize back and check content
    char *out_json = mcp_registry_manifest_serialize(ctx, &m);
    CHECK(out_json != NULL);
    CHECK(strstr(out_json, "\"name\":\"mcp-echo-c\"") != NULL);
    CHECK(strstr(out_json, "\"transport\":\"stdio\"") != NULL);
    mcp_registry_free_string(ctx, out_json);

    mcp_registry_manifest_cleanup(ctx, &m);

    // 4. Negative validation: missing required version
    const char *bad_json = "{\"name\":\"bad-server\",\"transport\":\"stdio\"}";
    memset(&m, 0, sizeof(m));
    CHECK(mcp_registry_manifest_parse(ctx, bad_json, strlen(bad_json), &m) == MCP_OK);
    char err_buf[128] = {0};
    CHECK(mcp_registry_manifest_validate(ctx, &m, err_buf) != MCP_OK);
    CHECK(strstr(err_buf, "version") != NULL);
    mcp_registry_manifest_cleanup(ctx, &m);

    // 5. Negative validation: invalid transport
    const char *bad_transport_json = "{\"name\":\"bad-server\",\"version\":\"1.0.0\",\"transport\":\"invalid\"}";
    memset(&m, 0, sizeof(m));
    CHECK(mcp_registry_manifest_parse(ctx, bad_transport_json, strlen(bad_transport_json), &m) == MCP_OK);
    memset(err_buf, 0, sizeof(err_buf));
    CHECK(mcp_registry_manifest_validate(ctx, &m, err_buf) != MCP_OK);
    CHECK(strstr(err_buf, "transport") != NULL);
    mcp_registry_manifest_cleanup(ctx, &m);

    // 6. Test NULL guards
    CHECK(mcp_registry_manifest_parse(ctx, NULL, 0, &m) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_registry_manifest_parse(ctx, "{}", 2, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_registry_manifest_validate(ctx, NULL, err_buf) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_registry_manifest_serialize(ctx, NULL) == NULL);

    mcp_context_destroy(ctx);
    printf("test_registry_manifest OK\n");
    return 0;
}
