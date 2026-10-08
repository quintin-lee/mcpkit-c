#include "test_check.h"

#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/protocol/uri_template.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    /* 1. Invalid arguments */
    CHECK(mcp_uri_template_match(ctx, NULL, "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///a", NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{unclosed", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{var:}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{var:abc}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{var:0}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{var:3*}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_uri_template_match(ctx, "file:///{:3}", "file:///a", NULL) == MCP_ERR_INVALID_ARGUMENT);

    /* 2. Exact match without variables */
    CHECK(mcp_uri_template_match(ctx, "file:///etc/hosts", "file:///etc/hosts", NULL) == MCP_OK);
    CHECK(mcp_uri_template_match(ctx, "file:///etc/hosts", "file:///etc/passwd", NULL) == MCP_ERR_NOT_FOUND);
    CHECK(mcp_uri_template_match(ctx, "file:///etc/hosts", "file:///etc/hosts/extra", NULL) == MCP_ERR_NOT_FOUND);

    /* 3. Level 1 single variable matching */
    mcp_json_value_t *vars = NULL;
    CHECK(mcp_uri_template_match(ctx, "items/{id}", "items/123", &vars) == MCP_OK);
    CHECK(vars != NULL);
    const char *id_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, vars, "id"), &id_val) == MCP_OK);
    CHECK(strcmp(id_val, "123") == 0);
    mcp_json_destroy(ctx, vars);
    vars = NULL;

    /* 4. Level 1 cannot cross slashes */
    CHECK(mcp_uri_template_match(ctx, "file:///{path}", "file:///a/b/c", &vars) == MCP_ERR_NOT_FOUND);
    CHECK(vars == NULL);

    /* 5. Level 2 reserved expansion {+path} crosses slashes */
    CHECK(mcp_uri_template_match(ctx, "file:///{+path}", "file:///a/b/c.txt", &vars) == MCP_OK);
    CHECK(vars != NULL);
    const char *path_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, vars, "path"), &path_val) == MCP_OK);
    CHECK(strcmp(path_val, "a/b/c.txt") == 0);
    mcp_json_destroy(ctx, vars);
    vars = NULL;

    /* 6. Multi-variable matching with Level 2 and Level 1 */
    CHECK(mcp_uri_template_match(ctx, "repo:///{+path}/commits/{hash}",
                                 "repo:///src/core/utils/commits/deadbeef123", &vars) == MCP_OK);
    CHECK(vars != NULL);
    path_val = NULL;
    const char *hash_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, vars, "path"), &path_val) == MCP_OK);
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, vars, "hash"), &hash_val) == MCP_OK);
    CHECK(strcmp(path_val, "src/core/utils") == 0);
    CHECK(strcmp(hash_val, "deadbeef123") == 0);
    mcp_json_destroy(ctx, vars);
    vars = NULL;

    /* 7. URL decoding during match */
    CHECK(mcp_uri_template_match(ctx, "query/{term}", "query/hello%20world%21", &vars) == MCP_OK);
    CHECK(vars != NULL);
    const char *term_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, vars, "term"), &term_val) == MCP_OK);
    CHECK(strcmp(term_val, "hello world!") == 0);
    mcp_json_destroy(ctx, vars);
    vars = NULL;

    /* 8. URI template expansion */
    mcp_json_value_t *exp_vars = mcp_json_object_new(ctx);
    mcp_json_value_t *v1 = mcp_json_string_new(ctx, "hello world");
    mcp_json_value_t *v2 = mcp_json_string_new(ctx, "path/to/resource");
    CHECK(mcp_json_object_set_take(ctx, exp_vars, "simple", v1) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, exp_vars, "reserved", v2) == MCP_OK);

    char *expanded = NULL;
    CHECK(mcp_uri_template_expand(ctx, "https://api.example.com/{simple}", exp_vars, &expanded) == MCP_OK);
    CHECK(expanded != NULL);
    CHECK(strcmp(expanded, "https://api.example.com/hello%20world") == 0);
    mcp_uri_template_free_string(ctx, expanded);
    expanded = NULL;

    CHECK(mcp_uri_template_expand(ctx, "file:///{+reserved}", exp_vars, &expanded) == MCP_OK);
    CHECK(expanded != NULL);
    CHECK(strcmp(expanded, "file:///path/to/resource") == 0);
    mcp_uri_template_free_string(ctx, expanded);
    expanded = NULL;

    /* Undefined variable expands to empty */
    CHECK(mcp_uri_template_expand(ctx, "prefix/{missing}/suffix", exp_vars, &expanded) == MCP_OK);
    CHECK(expanded != NULL);
    CHECK(strcmp(expanded, "prefix//suffix") == 0);
    mcp_uri_template_free_string(ctx, expanded);
    expanded = NULL;

    /* 9. Level 3 operators: matching */
    mcp_json_value_t *l3_vars = NULL;

    // Path segment {/var}
    CHECK(mcp_uri_template_match(ctx, "repo://root{/sub}", "repo://root/branch", &l3_vars) == MCP_OK);
    CHECK(l3_vars != NULL);
    const char *sub_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, l3_vars, "sub"), &sub_val) == MCP_OK);
    CHECK(strcmp(sub_val, "branch") == 0);
    mcp_json_destroy(ctx, l3_vars);
    l3_vars = NULL;

    // Fragment {#var}
    CHECK(mcp_uri_template_match(ctx, "doc://guide{#sec}", "doc://guide#intro", &l3_vars) == MCP_OK);
    CHECK(l3_vars != NULL);
    const char *sec_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, l3_vars, "sec"), &sec_val) == MCP_OK);
    CHECK(strcmp(sec_val, "intro") == 0);
    mcp_json_destroy(ctx, l3_vars);
    l3_vars = NULL;

    // Label {.var}
    CHECK(mcp_uri_template_match(ctx, "archive{.fmt}", "archive.zip", &l3_vars) == MCP_OK);
    CHECK(l3_vars != NULL);
    const char *fmt_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, l3_vars, "fmt"), &fmt_val) == MCP_OK);
    CHECK(strcmp(fmt_val, "zip") == 0);
    mcp_json_destroy(ctx, l3_vars);
    l3_vars = NULL;

    // Query {?var}
    CHECK(mcp_uri_template_match(ctx, "search://items{?query}", "search://items?query=mcp%20tool", &l3_vars) == MCP_OK);
    CHECK(l3_vars != NULL);
    const char *q_val = NULL;
    CHECK(mcp_json_string_value(ctx, mcp_json_object_get(ctx, l3_vars, "query"), &q_val) == MCP_OK);
    CHECK(strcmp(q_val, "mcp tool") == 0);
    mcp_json_destroy(ctx, l3_vars);
    l3_vars = NULL;

    /* 10. Level 3 operators: expansion */
    mcp_json_value_t *l3_exp = mcp_json_object_new(ctx);
    CHECK(mcp_json_object_set_take(ctx, l3_exp, "sub", mcp_json_string_new(ctx, "main")) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, l3_exp, "sec", mcp_json_string_new(ctx, "heading/1")) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, l3_exp, "fmt", mcp_json_string_new(ctx, "json")) == MCP_OK);
    CHECK(mcp_json_object_set_take(ctx, l3_exp, "query", mcp_json_string_new(ctx, "mcp test")) == MCP_OK);

    // Expand {/sub}
    CHECK(mcp_uri_template_expand(ctx, "repo://root{/sub}", l3_exp, &expanded) == MCP_OK);
    CHECK(strcmp(expanded, "repo://root/main") == 0);
    mcp_uri_template_free_string(ctx, expanded);
    expanded = NULL;

    // Expand {#sec} (reserved chars allowed in fragment)
    CHECK(mcp_uri_template_expand(ctx, "doc://guide{#sec}", l3_exp, &expanded) == MCP_OK);
    CHECK(strcmp(expanded, "doc://guide#heading/1") == 0);
    mcp_uri_template_free_string(ctx, expanded);
    expanded = NULL;

    // Expand {.fmt}
    CHECK(mcp_uri_template_expand(ctx, "archive{.fmt}", l3_exp, &expanded) == MCP_OK);
    CHECK(strcmp(expanded, "archive.json") == 0);
    mcp_uri_template_free_string(ctx, expanded);
    expanded = NULL;

    // Expand {?query}
    CHECK(mcp_uri_template_expand(ctx, "search://items{?query}", l3_exp, &expanded) == MCP_OK);
    CHECK(strcmp(expanded, "search://items?query=mcp%20test") == 0);
    mcp_uri_template_free_string(ctx, expanded);
    expanded = NULL;

    // Undefined variable with Level 3 operators omits prefix completely
    CHECK(mcp_uri_template_expand(ctx, "search://items{?missing}", l3_exp, &expanded) == MCP_OK);
    CHECK(strcmp(expanded, "search://items") == 0);
    mcp_uri_template_free_string(ctx, expanded);
    expanded = NULL;

    mcp_json_destroy(ctx, l3_exp);
    mcp_json_destroy(ctx, exp_vars);
    mcp_context_destroy(ctx);

    printf("test_uri_template OK\n");
    return 0;
}
