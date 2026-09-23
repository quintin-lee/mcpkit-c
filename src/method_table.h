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
    "logging/setLevel",
    "resources/subscribe",
    "resources/unsubscribe",
    "resources/templates/list",
    "server/discover",
};

#define MCP_SERVER_METHOD_COUNT 16

_Static_assert(sizeof(k_mcp_server_methods) / sizeof(*k_mcp_server_methods) ==
                   MCP_SERVER_METHOD_COUNT,
               "method count out of sync with table");

/*
 * Notifications consumed by mcp_server_notify (NOT routed as requests).
 * All of these may arrive as NOTIFICATION kind and are handled there;
 * arriving as REQUEST kind, they hit route fallback -> -32601 (honest).
 */
static const char *const k_mcp_server_notifications[] = {
    "notifications/initialized",
    "notifications/cancelled",
    "notifications/progress",
    "notifications/tools/list_changed",
    "notifications/resources/list_changed",
    "notifications/resources/updated",
    "notifications/prompts/list_changed",
    "notifications/message",
    "notifications/roots/list_changed",
};

#define MCP_SERVER_NOTIFICATION_COUNT 9

_Static_assert(sizeof(k_mcp_server_notifications) / sizeof(*k_mcp_server_notifications) ==
                   MCP_SERVER_NOTIFICATION_COUNT,
               "notification count out of sync");

#endif
