/**
 * @file method_table.h
 *
 * Single source of truth for the server-supported method names.
 * validate.c's L2 known-method table and dispatcher.c's route table
 * both include this header, so the two can never drift apart.
 */
#ifndef MCPKIT_METHOD_TABLE_H
#define MCPKIT_METHOD_TABLE_H

static const char *const k_mcp_server_methods[] = {
    "initialize",
    "ping",
    "tools/list",
    "tools/call",
    "resources/list",
    "resources/read",
    "prompts/list",
    "prompts/get",
    "completion/list",
    "completion/complete",
    "notifications/initialized",
};

#define MCP_SERVER_METHOD_COUNT 11

_Static_assert(sizeof(k_mcp_server_methods) / sizeof(*k_mcp_server_methods) ==
                   MCP_SERVER_METHOD_COUNT,
               "method count out of sync with table");

#endif
