#include "mcpkit/protocol/tasks.h"
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
    return 0;
}
