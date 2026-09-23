#include "mcpkit/protocol/skills.h"
#include "mcpkit/server/server.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/json/json.h"
#include "test_check.h"
#include <string.h>

int main(void) {
    mcp_context_t *ctx = NULL;

    mcp_skill_registry_t *reg = mcp_skill_registry_new(ctx);
    CHECK(reg != NULL);

    /* Test 1: Register skill */
    mcp_json_value_t *fm = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, fm, "name", mcp_json_string_new(ctx, "code-review"));
    mcp_json_object_set_take(ctx, fm, "description", mcp_json_string_new(ctx, "Review code against checklist"));

    CHECK(mcp_skill_registry_add(ctx, reg, "code-review", "Review code against checklist",
                                NULL, fm) == MCP_OK);
    /* Duplicate registration rejected */
    mcp_json_value_t *fm2 = mcp_json_object_new(ctx);
    CHECK(mcp_skill_registry_add(ctx, reg, "code-review", "Duplicate", NULL, fm2) == MCP_ERR_ALREADY_EXISTS);
    mcp_json_destroy(ctx, fm2);

    /* Test 2: Add resources to skill manifest */
    CHECK(mcp_skill_add_resource(ctx, reg, "code-review", "skill://code-review/SKILL.md",
                                "sha256:d2489d6c182e", 149) == MCP_OK);
    CHECK(mcp_skill_add_resource(ctx, reg, "code-review", "skill://code-review/checklist.md",
                                "sha256:dbab2de7bf7d", 65) == MCP_OK);

    /* Test 3: Lookup by name and URI */
    const mcp_skill_t *sk = mcp_skill_registry_find(reg, "code-review");
    CHECK(sk != NULL);
    CHECK(strcmp(sk->name, "code-review") == 0);
    CHECK(strcmp(sk->uri, "skill://code-review/SKILL.md") == 0);
    CHECK(sk->n_resources == 2);

    const mcp_skill_t *sk_uri = mcp_skill_registry_find(reg, "skill://code-review/SKILL.md");
    CHECK(sk_uri == sk);

    /* Test 4: JSON serialization */
    mcp_json_value_t *sk_json = mcp_skill_to_json(ctx, sk);
    CHECK(sk_json != NULL);

    const mcp_json_value_t *v_uri = mcp_json_object_get(ctx, sk_json, "uri");
    CHECK(v_uri != NULL);
    const char *uri_str = NULL;
    CHECK(mcp_json_string_value(ctx, v_uri, &uri_str) == MCP_OK);
    CHECK(strcmp(uri_str, "skill://code-review/SKILL.md") == 0);

    const mcp_json_value_t *v_res_arr = mcp_json_object_get(ctx, sk_json, "resources");
    CHECK(v_res_arr != NULL);
    CHECK(mcp_json_array_size(ctx, v_res_arr) == 2);

    mcp_json_destroy(ctx, sk_json);
    mcp_skill_registry_free(ctx, reg);

    return 0;
}
