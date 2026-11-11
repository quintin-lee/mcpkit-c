/**
 * @file test_sep2243_xmcp_header.c
 * @brief P4-a T1: x-mcp-header annotation validation in mcp_server_add_tool.
 *
 * Core-library test (no HTTP gate). Validates that the registry walks the
 * tool's inputSchema JSON tree for x-mcp-header annotations and rejects:
 *  - number-type annotated properties
 *  - duplicate header names (case-insensitive)
 *  - annotation values that are not RFC 9110 tchar (CR/LF/empty)
 *  - annotations on union-type that is all-forbidden
 * and accepts:
 *  - string/integer/boolean annotated properties
 *  - union types containing at least one allowed type
 *  - schemas with no x-mcp-header annotations (zero overhead)
 */

#include "test_check.h"

#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/value.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/array.h"
#include "mcpkit/server/tool.h"
#include "mcpkit/server/server.h"

static mcp_status_t ok_handler(mcp_context_t *ctx, mcp_session_t *s,
                               const mcp_json_value_t *args, void *u,
                               mcp_json_value_t **out) {
    (void)s; (void)args; (void)u;
    *out = mcp_json_object_new(ctx);
    return *out ? MCP_OK : MCP_ERR_NOMEM;
}

/* Parse a JSON text into a mcp_json_value_t (caller must destroy). */
static mcp_json_value_t *parse_schema(mcp_context_t *ctx, const char *json) {
    return mcp_json_parse(ctx, json, strlen(json));
}

/* Register a tool with the given schema JSON; return the add status.
   The tool is destroyed on all paths (either by server on add, or here). */
static mcp_status_t try_add_tool(mcp_context_t *ctx, mcp_server_t *srv,
                                  const char *name, const char *schema_json) {
    mcp_json_value_t *schema = parse_schema(ctx, schema_json);
    if (schema == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_tool_t *tool = mcp_tool_new(ctx, name, "desc", schema, ok_handler, NULL);
    if (tool == NULL) {
        mcp_json_destroy(ctx, schema);
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_status_t st = mcp_server_add_tool(ctx, srv, tool);
    if (st != MCP_OK) {
        /* Tool not registered; caller retains ownership */
        mcp_tool_destroy(ctx, tool);
    }
    return st;
}

static void test_number_type_rejected(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1num", "1.0.0");
    CHECK(srv != NULL);
    /* number type + x-mcp-header: must reject */
    mcp_status_t st = try_add_tool(ctx, srv, "num_tool",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"number\",\"x-mcp-header\":\"X-V\"}}\"}");
    CHECK(st == MCP_ERR_INVALID_ARGUMENT);
    mcp_server_destroy(ctx, srv);
}

static void test_string_type_accepted(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1str", "1.0.0");
    CHECK(srv != NULL);
    mcp_status_t st = try_add_tool(ctx, srv, "str_tool",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"string\",\"x-mcp-header\":\"X-V\"}}}");
    CHECK(st == MCP_OK);
    mcp_server_destroy(ctx, srv);
}

static void test_integer_type_accepted(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1int", "1.0.0");
    CHECK(srv != NULL);
    mcp_status_t st = try_add_tool(ctx, srv, "int_tool",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"integer\",\"x-mcp-header\":\"X-V\"}}}");
    CHECK(st == MCP_OK);
    mcp_server_destroy(ctx, srv);
}

static void test_boolean_type_accepted(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1bool", "1.0.0");
    CHECK(srv != NULL);
    mcp_status_t st = try_add_tool(ctx, srv, "bool_tool",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"boolean\",\"x-mcp-header\":\"X-V\"}}}");
    CHECK(st == MCP_OK);
    mcp_server_destroy(ctx, srv);
}

static void test_union_type_any_allowed_passes(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1union", "1.0.0");
    CHECK(srv != NULL);
    /* ["string","integer"] — both allowed */
    mcp_status_t st = try_add_tool(ctx, srv, "union_ok",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":[\"string\",\"integer\"],\"x-mcp-header\":\"X-V\"}}}");
    CHECK(st == MCP_OK);

    /* ["number","string"] — string is allowed, so passes */
    st = try_add_tool(ctx, srv, "union_mixed",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":[\"number\",\"string\"],\"x-mcp-header\":\"X-V\"}}}");
    CHECK(st == MCP_OK);
    mcp_server_destroy(ctx, srv);
}

static void test_union_type_all_forbidden_rejected(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1unionbad", "1.0.0");
    CHECK(srv != NULL);
    /* ["number","object"] — all forbidden */
    mcp_status_t st = try_add_tool(ctx, srv, "union_bad",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":[\"number\",\"object\"],\"x-mcp-header\":\"X-V\"}}}");
    CHECK(st == MCP_ERR_INVALID_ARGUMENT);
    mcp_server_destroy(ctx, srv);
}

static void test_duplicate_header_rejected(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1dup", "1.0.0");
    CHECK(srv != NULL);
    /* Two properties with the same x-mcp-header value (case-insensitive) */
    mcp_status_t st = try_add_tool(ctx, srv, "dup_tool",
        "{\"type\":\"object\",\"properties\":{"
        "\"a\":{\"type\":\"string\",\"x-mcp-header\":\"X-Dup\"},"
        "\"b\":{\"type\":\"string\",\"x-mcp-header\":\"x-dup\"}}}");
    CHECK(st == MCP_ERR_INVALID_ARGUMENT);
    mcp_server_destroy(ctx, srv);
}

static void test_cr_lf_in_header_rejected(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1crlf", "1.0.0");
    CHECK(srv != NULL);
    /* Header name containing CR */
    mcp_status_t st = try_add_tool(ctx, srv, "crlf_tool",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"string\",\"x-mcp-header\":\"X\\rBad\"}}}");
    CHECK(st == MCP_ERR_INVALID_ARGUMENT);

    /* Header name containing LF */
    st = try_add_tool(ctx, srv, "crlf_tool2",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"string\",\"x-mcp-header\":\"X\\nBad\"}}}");
    CHECK(st == MCP_ERR_INVALID_ARGUMENT);
    mcp_server_destroy(ctx, srv);
}

static void test_empty_header_rejected(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1empty", "1.0.0");
    CHECK(srv != NULL);
    mcp_status_t st = try_add_tool(ctx, srv, "empty_tool",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"string\",\"x-mcp-header\":\"\"}}}");
    CHECK(st == MCP_ERR_INVALID_ARGUMENT);
    mcp_server_destroy(ctx, srv);
}

static void test_nested_depth_found(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1nested", "1.0.0");
    CHECK(srv != NULL);
    /* Deeply nested: object -> items -> properties -> x-mcp-header on number type */
    mcp_status_t st = try_add_tool(ctx, srv, "nested_tool",
        "{\"type\":\"object\",\"properties\":{"
        "\"outer\":{\"type\":\"object\",\"items\":{"
        "  \"type\":\"object\",\"properties\":{"
        "    \"v\":{\"type\":\"number\",\"x-mcp-header\":\"X-Deep\"}"
        "  }"
        "}"
        "}}}");
    CHECK(st == MCP_ERR_INVALID_ARGUMENT);
    mcp_server_destroy(ctx, srv);
}

static void test_no_annotation_zero_regression(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1noanno", "1.0.0");
    CHECK(srv != NULL);
    /* No x-mcp-header anywhere: registers cleanly */
    mcp_status_t st = try_add_tool(ctx, srv, "plain_tool",
        "{\"type\":\"object\",\"properties\":{\"v\":{\"type\":\"number\"}}}");
    CHECK(st == MCP_OK);
    mcp_server_destroy(ctx, srv);
}

static void test_null_schema_zero_regression(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "t1null", "1.0.0");
    CHECK(srv != NULL);
    /* NULL schema (no inputSchema) — no annotation walk, zero overhead */
    mcp_tool_t *tool = mcp_tool_new(ctx, "null_schema_tool", "desc", NULL, ok_handler, NULL);
    CHECK(tool != NULL);
    mcp_status_t st = mcp_server_add_tool(ctx, srv, tool);
    CHECK(st == MCP_OK);
    mcp_server_destroy(ctx, srv);
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    test_number_type_rejected(ctx);
    test_string_type_accepted(ctx);
    test_integer_type_accepted(ctx);
    test_boolean_type_accepted(ctx);
    test_union_type_any_allowed_passes(ctx);
    test_union_type_all_forbidden_rejected(ctx);
    test_duplicate_header_rejected(ctx);
    test_cr_lf_in_header_rejected(ctx);
    test_empty_header_rejected(ctx);
    test_nested_depth_found(ctx);
    test_no_annotation_zero_regression(ctx);
    test_null_schema_zero_regression(ctx);

    mcp_context_destroy(ctx);
    printf("test_sep2243_xmcp_header: OK\n");
    return 0;
}
