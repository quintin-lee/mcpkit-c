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

    /* Test 5: Server dispatch integration */
    mcp_server_t *srv = mcp_server_create(ctx, "test-skills-srv", "1.0.0");
    CHECK(srv != NULL);
    CHECK(mcp_server_enable_skills(ctx, srv) == MCP_OK);
    mcp_skill_registry_t *srv_reg = mcp_server_get_skill_registry(srv);
    CHECK(srv_reg != NULL);

    mcp_json_value_t *fm_srv = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, fm_srv, "name", mcp_json_string_new(ctx, "build-app"));
    CHECK(mcp_skill_registry_add(ctx, srv_reg, "build-app", "Build app skill", NULL, fm_srv) == MCP_OK);

    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    CHECK(sess != NULL);

    /* 5a: Discover advertises skills extension */
    mcp_message_t *discover_req = mcp_request_new_number_id(ctx, 1, "server/discover", NULL);
    mcp_message_t *discover_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, sess, discover_req, &discover_resp) == MCP_OK);
    CHECK(discover_resp != NULL);
    const mcp_json_value_t *disc_res = mcp_message_result(ctx, discover_resp);
    CHECK(disc_res != NULL);
    const mcp_json_value_t *caps = mcp_json_object_get(ctx, disc_res, "capabilities");
    CHECK(caps != NULL);
    const mcp_json_value_t *exts = mcp_json_object_get(ctx, caps, "extensions");
    CHECK(exts != NULL);
    CHECK(mcp_json_object_get(ctx, exts, "io.modelcontextprotocol/skills") != NULL);
    mcp_message_destroy(ctx, discover_req);
    mcp_message_destroy(ctx, discover_resp);

    /* Initialize session */
    mcp_json_value_t *init_p = mcp_initialize_params_new(ctx, "cli", "1.0");
    mcp_message_t *init_req = mcp_request_new_number_id(ctx, 100, "initialize", init_p);
    mcp_message_t *init_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, sess, init_req, &init_resp) == MCP_OK);
    mcp_message_destroy(ctx, init_req);
    mcp_message_destroy(ctx, init_resp);
    mcp_message_t *init_notif = mcp_initialized_notification_new(ctx);
    CHECK(mcp_server_notify(ctx, srv, sess, init_notif) == MCP_OK);
    mcp_message_destroy(ctx, init_notif);

    /* 5b: skills/list */
    mcp_message_t *list_req = mcp_request_new_number_id(ctx, 2, "skills/list", NULL);
    mcp_message_t *list_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, sess, list_req, &list_resp) == MCP_OK);
    CHECK(list_resp != NULL);
    const mcp_json_value_t *list_res = mcp_message_result(ctx, list_resp);
    CHECK(list_res != NULL);
    const mcp_json_value_t *skills_arr = mcp_json_object_get(ctx, list_res, "skills");
    CHECK(skills_arr != NULL);
    CHECK(mcp_json_array_size(ctx, skills_arr) == 1);
    mcp_message_destroy(ctx, list_req);
    mcp_message_destroy(ctx, list_resp);

    /* 5c: skills/get */
    mcp_json_value_t *get_p = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, get_p, "name", mcp_json_string_new(ctx, "build-app"));
    mcp_message_t *get_req = mcp_request_new_number_id(ctx, 3, "skills/get", get_p);
    mcp_message_t *get_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, sess, get_req, &get_resp) == MCP_OK);
    CHECK(get_resp != NULL);
    const mcp_json_value_t *get_res = mcp_message_result(ctx, get_resp);
    CHECK(get_res != NULL);
    const char *sk_uri_str = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, get_res, "uri"), &sk_uri_str) == MCP_OK);
    CHECK(strcmp(sk_uri_str, "skill://build-app/SKILL.md") == 0);
    mcp_message_destroy(ctx, get_req);
    mcp_message_destroy(ctx, get_resp);

    mcp_server_destroy_session(ctx, srv, sess);
    mcp_server_destroy(ctx, srv);

    return 0;
}
