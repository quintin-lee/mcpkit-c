/* tests/unit/test_error_mapping.c -- Phase 2 T5: status <-> RPC code coverage.
 *
 * Proves: the explicit mapping rows round-trip; unmapped statuses collapse
 * to MCP_RPC_INTERNAL_ERROR; unknown codes collapse to MCP_ERR_PROTOCOL;
 * mcp_response_err_new emits the exact code; mcp_status_string is total.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/core/error.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/message.h"

static void check_explicit_rows_round_trip(void)
{
    assert(mcp_status_to_rpc_code(MCP_ERR_INVALID_ARGUMENT) == MCP_RPC_INVALID_PARAMS);
    assert(mcp_status_to_rpc_code(MCP_ERR_NOT_FOUND) == MCP_RPC_METHOD_NOT_FOUND);
    assert(mcp_status_to_rpc_code(MCP_ERR_PROTOCOL) == MCP_RPC_INVALID_REQUEST);
    assert(mcp_rpc_code_to_status(MCP_RPC_INVALID_PARAMS) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_rpc_code_to_status(MCP_RPC_METHOD_NOT_FOUND) == MCP_ERR_NOT_FOUND);
    assert(mcp_rpc_code_to_status(MCP_RPC_INVALID_REQUEST) == MCP_ERR_PROTOCOL);
    assert(mcp_rpc_code_to_status(MCP_RPC_PARSE_ERROR) == MCP_ERR_PROTOCOL);
    assert(mcp_rpc_code_to_status(0) == MCP_OK);
    assert(mcp_status_to_rpc_code(MCP_OK) == 0);
}

static void check_unmapped_statuses_collapse(void)
{
    /* Every status without an explicit row must use INTERNAL_ERROR. */
    static const mcp_status_t kUnmapped[] = {
        MCP_ERR_NOMEM,      MCP_ERR_IO,         MCP_ERR_TIMEOUT,
        MCP_ERR_CANCELLED,  MCP_ERR_ALREADY_EXISTS,
        MCP_ERR_UNSUPPORTED, MCP_ERR_PERMISSION,
    };
    for (size_t i = 0; i < sizeof(kUnmapped) / sizeof(kUnmapped[0]); i++)
        assert(mcp_status_to_rpc_code(kUnmapped[i]) == MCP_RPC_INTERNAL_ERROR);
}

static void check_unknown_codes_collapse(void)
{
    assert(mcp_rpc_code_to_status(-31999) == MCP_ERR_PROTOCOL);
    assert(mcp_rpc_code_to_status(-33000) == MCP_ERR_PROTOCOL);
    assert(mcp_rpc_code_to_status(42) == MCP_ERR_PROTOCOL);
}

static void check_builder_emits_exact_code(void)
{
    mcp_message_t *err =
        mcp_response_err_new(NULL, NULL, MCP_RPC_METHOD_NOT_FOUND, "no such tool", NULL);
    assert(err != NULL);
    char *s = mcp_message_serialize(NULL, err);
    assert(s != NULL);
    assert(strstr(s, "\"code\":-32601") != NULL);
    mcp_json_free_string(NULL, s);
    mcp_message_destroy(NULL, err);
}

static void check_status_string_is_total(void)
{
    static const mcp_status_t kAll[] = {
        MCP_OK,             MCP_ERR_INVALID_ARGUMENT, MCP_ERR_NOMEM,
        MCP_ERR_IO,         MCP_ERR_PROTOCOL,         MCP_ERR_TIMEOUT,
        MCP_ERR_CANCELLED,  MCP_ERR_NOT_FOUND,        MCP_ERR_ALREADY_EXISTS,
        MCP_ERR_UNSUPPORTED, MCP_ERR_PERMISSION,
    };
    for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); i++) {
        const char *msg = mcp_status_string(kAll[i]);
        assert(msg != NULL && msg[0] != '\0');
    }
    assert(strcmp(mcp_status_string((mcp_status_t)999), "MCP_ERR_UNKNOWN") == 0);
}

int main(void)
{
    check_explicit_rows_round_trip();
    check_unmapped_statuses_collapse();
    check_unknown_codes_collapse();
    check_builder_emits_exact_code();
    check_status_string_is_total();
    printf("error_mapping: all assertions passed\n");
    return 0;
}
