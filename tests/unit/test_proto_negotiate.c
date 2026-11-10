#include "test_check.h"
#include <string.h>

#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/protocol/initialize.h"
#include "mcpkit/protocol/version.h"
#include "mcpkit/server/server.h"
#include "mcpkit/server/session.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* 1. Version parse */
    CHECK(mcp_proto_version_parse("2026-07-28") == MCP_PROTO_2026_07_28);
    CHECK(mcp_proto_version_parse("2025-11-25") == MCP_PROTO_2025_11_25);
    CHECK(mcp_proto_version_parse("2025-06-18") == MCP_PROTO_2025_06_18);
    CHECK(mcp_proto_version_parse("2025-03-26") == MCP_PROTO_2025_03_26);
    CHECK(mcp_proto_version_parse("2024-11-05") == MCP_PROTO_2024_11_05);
    CHECK(mcp_proto_version_parse("1999-01-01") == MCP_PROTO_NONE);
    CHECK(mcp_proto_version_parse(NULL) == MCP_PROTO_NONE);

    /* 2. Version string */
    CHECK(strcmp(mcp_proto_version_string(MCP_PROTO_2026_07_28), "2026-07-28") == 0);
    CHECK(strcmp(mcp_proto_version_string(MCP_PROTO_2025_11_25), "2025-11-25") == 0);
    CHECK(strcmp(mcp_proto_version_string(MCP_PROTO_2025_06_18), "2025-06-18") == 0);
    CHECK(strcmp(mcp_proto_version_string(MCP_PROTO_2025_03_26), "2025-03-26") == 0);
    CHECK(strcmp(mcp_proto_version_string(MCP_PROTO_2024_11_05), "2024-11-05") == 0);
    CHECK(mcp_proto_version_string(MCP_PROTO_NONE) == NULL);

    /* 3. Version comparison (at_least) */
    CHECK(mcp_proto_at_least(MCP_PROTO_2026_07_28, MCP_PROTO_2025_11_25) == true);
    CHECK(mcp_proto_at_least(MCP_PROTO_2025_11_25, MCP_PROTO_2025_11_25) == true);
    CHECK(mcp_proto_at_least(MCP_PROTO_2025_06_18, MCP_PROTO_2025_11_25) == false);
    CHECK(mcp_proto_at_least(MCP_PROTO_2024_11_05, MCP_PROTO_2024_11_05) == true);
    CHECK(mcp_proto_at_least(MCP_PROTO_NONE, MCP_PROTO_2024_11_05) == false);
    CHECK(mcp_proto_at_least(MCP_PROTO_2025_11_25, MCP_PROTO_NONE) == false);
    CHECK(mcp_proto_at_least(MCP_PROTO_NONE, MCP_PROTO_NONE) == false);

    /* 4. Negotiation table (5 versions + down-negotiation for 2026-07-28) */
    CHECK(strcmp(MCP_PROTOCOL_VERSION_LATEST, "2025-11-25") == 0);
    CHECK(strcmp(mcp_protocol_negotiate(ctx, "2025-11-25"), "2025-11-25") == 0);
    CHECK(strcmp(mcp_protocol_negotiate(ctx, "2025-06-18"), "2025-06-18") == 0);
    CHECK(strcmp(mcp_protocol_negotiate(ctx, "2025-03-26"), "2025-03-26") == 0);
    CHECK(strcmp(mcp_protocol_negotiate(ctx, "2024-11-05"), "2024-11-05") == 0);
    /* 2026-07-28 requests down-negotiate to 2025-11-25 on stateful initialize */
    CHECK(strcmp(mcp_protocol_negotiate(ctx, "2026-07-28"), "2025-11-25") == 0);
    CHECK(mcp_protocol_negotiate(ctx, "1999-01-01") == NULL);
    CHECK(mcp_protocol_negotiate(ctx, NULL) == NULL);

    /* 5. mcp_initialize_result_new_v echo */
    mcp_json_value_t *r2026 = mcp_initialize_result_new_v(ctx, MCP_PROTO_2026_07_28, "srv", "1.0");
    CHECK(r2026 != NULL);
    const mcp_json_value_t *pv2026 = mcp_json_object_get(ctx, r2026, "protocolVersion");
    const char *s2026 = NULL;
    CHECK(mcp_json_string_value(ctx, pv2026, &s2026) == MCP_OK);
    CHECK(strcmp(s2026, "2026-07-28") == 0);
    mcp_json_destroy(ctx, r2026);

    mcp_json_value_t *r2025 = mcp_initialize_result_new_v(ctx, MCP_PROTO_2025_11_25, "srv", "1.0");
    CHECK(r2025 != NULL);
    const mcp_json_value_t *pv2025 = mcp_json_object_get(ctx, r2025, "protocolVersion");
    const char *s2025 = NULL;
    CHECK(mcp_json_string_value(ctx, pv2025, &s2025) == MCP_OK);
    CHECK(strcmp(s2025, "2025-11-25") == 0);
    mcp_json_destroy(ctx, r2025);

    mcp_json_value_t *r_none = mcp_initialize_result_new_v(ctx, MCP_PROTO_NONE, "srv", "1.0");
    CHECK(r_none != NULL);
    const mcp_json_value_t *pv_none = mcp_json_object_get(ctx, r_none, "protocolVersion");
    const char *s_none = NULL;
    CHECK(mcp_json_string_value(ctx, pv_none, &s_none) == MCP_OK);
    CHECK(strcmp(s_none, "2025-11-25") == 0);
    mcp_json_destroy(ctx, r_none);

    /* 6. Session negotiated accessor */
    CHECK(mcp_session_negotiated(ctx, NULL) == MCP_PROTO_2024_11_05);

    mcp_server_t *srv = mcp_server_create(ctx, "srv", "1.0");
    CHECK(srv != NULL);
    mcp_session_t *sess = mcp_server_create_session(ctx, srv);
    CHECK(sess != NULL);
    CHECK(mcp_session_negotiated(ctx, sess) == MCP_PROTO_2024_11_05);

    mcp_server_destroy_session(ctx, srv, sess);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
