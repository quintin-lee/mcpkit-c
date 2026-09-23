#include "mcpkit/protocol/tasks.h"
#include "mcpkit/server/server.h"
#include "mcpkit/protocol/message.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/json/json.h"
#include "test_check.h"
#include <string.h>

int main(void) {
    mcp_context_t *ctx = NULL;

    mcp_task_mgr_t *mgr = mcp_task_mgr_new(ctx);
    CHECK(mgr != NULL);

    /* Test 1: Task creation */
    CHECK(mcp_task_mgr_create(ctx, mgr, "task-1001", 60000, 1000) == MCP_OK);
    /* Duplicate fails */
    CHECK(mcp_task_mgr_create(ctx, mgr, "task-1001", 60000, 1000) == MCP_ERR_ALREADY_EXISTS);

    /* Test 2: Status check */
    mcp_task_desc_t desc;
    CHECK(mcp_task_mgr_get(ctx, mgr, "task-1001", &desc) == MCP_OK);
    CHECK(strcmp(desc.task_id, "task-1001") == 0);
    CHECK(desc.status == MCP_TASK_STATUS_WORKING);
    CHECK(desc.ttl_ms == 60000);
    CHECK(desc.poll_interval_ms == 1000);

    /* Test 3: Input required transition */
    mcp_json_value_t *reqs = mcp_json_object_new(ctx);
    mcp_json_value_t *elicit = mcp_json_string_new(ctx, "Please confirm deployment");
    mcp_json_object_set_take(ctx, reqs, "confirm", elicit);
    CHECK(mcp_task_mgr_set_input_requests(ctx, mgr, "task-1001", reqs) == MCP_OK);

    CHECK(mcp_task_mgr_get(ctx, mgr, "task-1001", &desc) == MCP_OK);
    CHECK(desc.status == MCP_TASK_STATUS_INPUT_REQUIRED);
    CHECK(desc.input_requests != NULL);

    /* Test 4: Complete transition */
    mcp_json_value_t *res = mcp_json_object_new(ctx);
    mcp_json_value_t *deployed = mcp_json_bool_new(ctx, true);
    mcp_json_object_set_take(ctx, res, "deployed", deployed);
    CHECK(mcp_task_mgr_set_result(ctx, mgr, "task-1001", res) == MCP_OK);

    CHECK(mcp_task_mgr_get(ctx, mgr, "task-1001", &desc) == MCP_OK);
    CHECK(desc.status == MCP_TASK_STATUS_COMPLETED);
    CHECK(desc.result != NULL);

    /* Test 5: Serialization to JSON */
    mcp_json_value_t *json_desc = mcp_task_desc_to_json(ctx, &desc);
    CHECK(json_desc != NULL);
    const mcp_json_value_t *v_id = mcp_json_object_get(ctx, json_desc, "taskId");
    CHECK(v_id != NULL);
    const char *id_str = NULL;
    CHECK(mcp_json_string_value(ctx, v_id, &id_str) == MCP_OK);
    CHECK(strcmp(id_str, "task-1001") == 0);

    const mcp_json_value_t *v_st = mcp_json_object_get(ctx, json_desc, "status");
    CHECK(v_st != NULL);
    const char *st_str = NULL;
    CHECK(mcp_json_string_value(ctx, v_st, &st_str) == MCP_OK);
    CHECK(strcmp(st_str, "completed") == 0);

    const mcp_json_value_t *v_res = mcp_json_object_get(ctx, json_desc, "result");
    CHECK(v_res != NULL);
    const mcp_json_value_t *v_dep = mcp_json_object_get(ctx, v_res, "deployed");
    CHECK(v_dep != NULL);
    bool dep_bool = false;
    CHECK(mcp_json_bool_value(ctx, v_dep, &dep_bool) == MCP_OK);
    CHECK(dep_bool == true);

    mcp_json_destroy(ctx, json_desc);

    /* Test 6: Second task for cancellation */
    CHECK(mcp_task_mgr_create(ctx, mgr, "task-1002", 30000, 500) == MCP_OK);
    CHECK(mcp_task_mgr_cancel(ctx, mgr, "task-1002") == MCP_OK);
    CHECK(mcp_task_mgr_get(ctx, mgr, "task-1002", &desc) == MCP_OK);
    CHECK(desc.status == MCP_TASK_STATUS_CANCELLED);

    /* Non-existent task returns NOT_FOUND */
    CHECK(mcp_task_mgr_get(ctx, mgr, "non-existent", &desc) == MCP_ERR_NOT_FOUND);

    mcp_task_mgr_free(ctx, mgr);

    /* Test 7: Server dispatch integration */
    mcp_server_t *srv = mcp_server_create(ctx, "test-tasks-srv", "1.0.0");
    CHECK(srv != NULL);
    CHECK(mcp_server_enable_tasks(ctx, srv) == MCP_OK);
    mcp_task_mgr_t *srv_mgr = mcp_server_get_task_mgr(srv);
    CHECK(srv_mgr != NULL);
    CHECK(mcp_task_mgr_create(ctx, srv_mgr, "task-2001", 60000, 1000) == MCP_OK);

    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    CHECK(sess != NULL);

    /* 7a: Discover advertises tasks extension */
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
    CHECK(mcp_json_object_get(ctx, exts, "io.modelcontextprotocol/tasks") != NULL);
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

    /* 7b: tasks/get */
    mcp_json_value_t *get_params = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, get_params, "taskId", mcp_json_string_new(ctx, "task-2001"));
    mcp_message_t *get_req = mcp_request_new_number_id(ctx, 2, "tasks/get", get_params);
    mcp_message_t *get_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, sess, get_req, &get_resp) == MCP_OK);
    CHECK(get_resp != NULL);
    const mcp_json_value_t *get_res = mcp_message_result(ctx, get_resp);
    CHECK(get_res != NULL);
    const char *st_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, get_res, "status"), &st_val) == MCP_OK);
    CHECK(strcmp(st_val, "working") == 0);
    mcp_message_destroy(ctx, get_req);
    mcp_message_destroy(ctx, get_resp);

    /* 7c: tasks/update */
    CHECK(mcp_task_mgr_set_status(ctx, srv_mgr, "task-2001", MCP_TASK_STATUS_INPUT_REQUIRED, "need input") == MCP_OK);
    mcp_json_value_t *upd_params = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, upd_params, "taskId", mcp_json_string_new(ctx, "task-2001"));
    mcp_message_t *upd_req = mcp_request_new_number_id(ctx, 3, "tasks/update", upd_params);
    mcp_message_t *upd_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, sess, upd_req, &upd_resp) == MCP_OK);
    CHECK(upd_resp != NULL);
    const mcp_json_value_t *upd_res = mcp_message_result(ctx, upd_resp);
    CHECK(upd_res != NULL);
    bool accepted = false;
    CHECK(mcp_json_bool_value(ctx, mcp_json_object_get(ctx, upd_res, "accepted"), &accepted) == MCP_OK);
    CHECK(accepted == true);
    mcp_message_destroy(ctx, upd_req);
    mcp_message_destroy(ctx, upd_resp);

    /* 7d: tasks/cancel */
    mcp_json_value_t *can_params = mcp_json_object_new(ctx);
    mcp_json_object_set_take(ctx, can_params, "taskId", mcp_json_string_new(ctx, "task-2001"));
    mcp_message_t *can_req = mcp_request_new_number_id(ctx, 4, "tasks/cancel", can_params);
    mcp_message_t *can_resp = NULL;
    CHECK(mcp_server_dispatch(ctx, srv, sess, can_req, &can_resp) == MCP_OK);
    CHECK(can_resp != NULL);
    const mcp_json_value_t *can_res = mcp_message_result(ctx, can_resp);
    CHECK(can_res != NULL);
    bool cancelled = false;
    CHECK(mcp_json_bool_value(ctx, mcp_json_object_get(ctx, can_res, "cancelled"), &cancelled) == MCP_OK);
    CHECK(cancelled == true);
    mcp_message_destroy(ctx, can_req);
    mcp_message_destroy(ctx, can_resp);

    mcp_server_destroy_session(ctx, srv, sess);
    mcp_server_destroy(ctx, srv);

    return 0;
}
