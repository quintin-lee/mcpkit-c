#include <assert.h>
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/message.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    // params build + validate OK, protocolVersion is latest
    mcp_json_value_t *params = mcp_initialize_params_new(ctx, "test-client", "1.0");
    assert(params != NULL);
    assert(mcp_initialize_params_validate(ctx, params) == MCP_OK);
    const mcp_json_value_t *pv = mcp_json_object_get(ctx, params, "protocolVersion");
    assert(pv != NULL);
    const char *pvs = NULL;
    assert(mcp_json_string_value(ctx, pv, &pvs) == MCP_OK);
    assert(strcmp(pvs, MCP_PROTOCOL_VERSION_LATEST) == 0);
    const mcp_json_value_t *ci = mcp_json_object_get(ctx, params, "clientInfo");
    assert(ci != NULL);
    const mcp_json_value_t *cn = mcp_json_object_get(ctx, ci, "name");
    const char *cns = NULL;
    assert(mcp_json_string_value(ctx, cn, &cns) == MCP_OK);
    assert(strcmp(cns, "test-client") == 0);

    // missing protocolVersion → INVALID_ARGUMENT
    mcp_json_value_t *bad = mcp_json_object_new(ctx);
    assert(bad != NULL);
    assert(mcp_initialize_params_validate(ctx, bad) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_value_t *caps = mcp_json_object_new(ctx);
    assert(caps != NULL);
    assert(mcp_json_object_set(ctx, bad, "capabilities", caps) == MCP_OK);
    assert(mcp_initialize_params_validate(ctx, bad) == MCP_ERR_INVALID_ARGUMENT);
    mcp_json_destroy(ctx, bad);

    // explicit older version builds fine
    mcp_json_value_t *old = mcp_initialize_params_new_v(ctx, "2024-11-05", "c", "0.1");
    assert(old != NULL);
    assert(mcp_initialize_params_validate(ctx, old) == MCP_OK);

    // negotiation
    assert(strcmp(mcp_protocol_negotiate(ctx, "2025-06-18"), "2025-06-18") == 0);
    assert(strcmp(mcp_protocol_negotiate(ctx, "2024-11-05"), "2024-11-05") == 0);
    assert(mcp_protocol_negotiate(ctx, "1999-01-01") == NULL);
    assert(mcp_protocol_negotiate(ctx, NULL) == NULL);

    // result carries protocolVersion + serverInfo
    mcp_json_value_t *res = mcp_initialize_result_new(ctx, "test-server", "0.1.0");
    assert(res != NULL);
    const mcp_json_value_t *si = mcp_json_object_get(ctx, res, "serverInfo");
    assert(si != NULL);
    const mcp_json_value_t *sn = mcp_json_object_get(ctx, si, "name");
    const char *sns = NULL;
    assert(mcp_json_string_value(ctx, sn, &sns) == MCP_OK);
    assert(strcmp(sns, "test-server") == 0);

    // initialized notification shape
    mcp_message_t *ntf = mcp_initialized_notification_new(ctx);
    assert(ntf != NULL);
    assert(mcp_message_kind(ctx, ntf) == MCP_MSG_NOTIFICATION);
    assert(strcmp(mcp_message_method(ctx, ntf), "notifications/initialized") == 0);
    assert(mcp_message_id_type(ctx, ntf) == MCP_ID_NONE);
    mcp_message_destroy(ctx, ntf);

    mcp_json_destroy(ctx, params);
    mcp_json_destroy(ctx, old);
    mcp_json_destroy(ctx, res);
    mcp_context_destroy(ctx);
    return 0;
}
