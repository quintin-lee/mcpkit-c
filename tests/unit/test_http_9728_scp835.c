/**
 * @file test_http_9728_scp835.c
 * @brief P3 blocks 3+4 (T3/T4) red/green test: RFC 9728 PRM endpoint,
 *        401 resource_metadata extension, and (T4, added next) SEP-835
 *        incremental scope via mcp_http_serve_with_auth_v2.
 *
 * Red phase: mcp_oauth_protected_resource_* / mcp_server_set_
 * protected_resource_metadata do not exist; the .well-known route is
 * absent; send_unauthorized's challenge is the bare string "Bearer".
 * Green phase:
 *  - PRM parse/serialize round-trip.
 *  - GET /.well-known/oauth-protected-resource -> 200 + JSON body when
 *    configured, 404 when not.
 *  - 401 challenges carry resource_metadata="<resource>" when the
 *    request's MCP-Protocol-Version header is >=2025-11-25 AND pr_meta is
 *    configured; bare "Bearer" otherwise (zero regression, guarded by
 *    the existing test_http_serve cases too).
 */
#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static mcp_status_t echo_handler(mcp_context_t *ctx, mcp_session_t *session,
                                 const mcp_json_value_t *args, void *user_data,
                                 mcp_json_value_t **result_out) {
    (void)session;
    (void)user_data;
    const mcp_json_value_t *text = mcp_json_object_get(ctx, args, "text");
    const char *s = NULL;
    if (text == NULL || mcp_json_string_value(ctx, text, &s) != MCP_OK) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_json_value_t *result = mcp_json_object_new(ctx);
    mcp_json_value_t *stv = mcp_json_string_new(ctx, s);
    if (result == NULL || stv == NULL
        || mcp_json_object_set(ctx, result, "echo", stv) != MCP_OK) {
        mcp_json_destroy(ctx, stv);
        mcp_json_destroy(ctx, result);
        return MCP_ERR_NOMEM;
    }
    *result_out = result;
    return MCP_OK;
}

typedef struct {
    const char **reqs;
    size_t nreqs;
    size_t next;
    char *out;
    size_t olen;
} mem_io_t;

static mcp_status_t mem_read(mcp_context_t *ctx, void *user, char *buf, size_t cap,
                             size_t *n_out) {
    (void)ctx;
    mem_io_t *m = user;
    if (m->next >= m->nreqs) {
        *n_out = 0;
        return MCP_OK;
    }
    size_t len = strlen(m->reqs[m->next]);
    if (len > cap) {
        return MCP_ERR_NOMEM;
    }
    memcpy(buf, m->reqs[m->next], len);
    m->next++;
    *n_out = len;
    return MCP_OK;
}

static mcp_status_t mem_write(mcp_context_t *ctx, void *user, const char *data, size_t len) {
    (void)ctx;
    mem_io_t *m = user;
    char *nbuf = realloc(m->out, m->olen + len + 1);
    CHECK(nbuf != NULL);
    m->out = nbuf;
    memcpy(m->out + m->olen, data, len);
    m->olen += len;
    m->out[m->olen] = '\0';
    return MCP_OK;
}

static bool auth_always_false(mcp_context_t *ctx, const char *tok, void *ud) {
    (void)ctx; (void)tok; (void)ud;
    return false;
}

static mcp_server_t *make_server(mcp_context_t *ctx) {
    mcp_server_t *srv = mcp_server_create(ctx, "http9728srv", "0.1.0");
    CHECK(srv != NULL);
    mcp_json_value_t *schema = mcp_schema_object_new(ctx);
    CHECK(schema != NULL);
    CHECK(mcp_schema_add_property(ctx, schema, "text", mcp_schema_string_new(ctx)) == MCP_OK);
    CHECK(mcp_schema_add_required(ctx, schema, "text") == MCP_OK);
    CHECK(mcp_server_add_tool(ctx, srv,
                              mcp_tool_new(ctx, "echo", "Echo", schema, echo_handler,
                                           NULL)) == MCP_OK);
    return srv;
}

static char *run_script_auth(mcp_context_t *ctx, mcp_server_t *srv,
                             const char **reqs, size_t n, mcp_status_t *st_out,
                             mcp_http_auth_fn auth_fn, void *auth_ud) {
    mem_io_t m;
    memset(&m, 0, sizeof(m));
    m.reqs = reqs;
    m.nreqs = n;
    mcp_http_io_t io;
    io.user = &m;
    io.read = mem_read;
    io.write = mem_write;
    *st_out = mcp_http_serve_with_auth(ctx, srv, &io, auth_fn, auth_ud);
    return m.out;
}

static char *run_script(mcp_context_t *ctx, mcp_server_t *srv, const char **reqs, size_t n,
                        mcp_status_t *st_out) {
    mem_io_t m;
    memset(&m, 0, sizeof(m));
    m.reqs = reqs;
    m.nreqs = n;
    mcp_http_io_t io;
    io.user = &m;
    io.read = mem_read;
    io.write = mem_write;
    *st_out = mcp_http_serve(ctx, srv, &io);
    return m.out;
}

static char *make_post(const char *body, const char *extra_hdrs, char *buf, size_t cap) {
    const char *sep = (extra_hdrs != NULL && strstr(extra_hdrs, "\r\n\r\n") != NULL) ? "" : "\r\n";
    int n = snprintf(buf, cap,
                     "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n"
                     "Content-Type: application/json\r\n%s%s%s",
                     strlen(body), extra_hdrs ? extra_hdrs : "", sep, body);
    CHECK(n > 0 && (size_t)n < cap);
    return buf;
}

typedef struct {
    mcp_http_auth_result_code_t code;
    const char *scope;
    const char *resource_metadata;
    const char *error_description;
} v2_expected_t;

static void auth_v2_fixed(mcp_context_t *ctx, const char *tok, void *ud,
                          mcp_http_auth_result_t *out) {
    (void)ctx;
    (void)tok;
    v2_expected_t *e = ud;
    out->code = e->code;
    out->scope = e->scope;
    out->resource_metadata = e->resource_metadata;
    out->error_description = e->error_description;
}

static char *run_script_auth_v2(mcp_context_t *ctx, mcp_server_t *srv,
                                 const char **reqs, size_t n, mcp_status_t *st_out,
                                 mcp_http_auth_result_v2_fn fn, void *ud) {
    mem_io_t m;
    memset(&m, 0, sizeof(m));
    m.reqs = reqs;
    m.nreqs = n;
    mcp_http_io_t io;
    io.user = &m;
    io.read = mem_read;
    io.write = mem_write;
    *st_out = mcp_http_serve_with_auth_v2(ctx, srv, &io, fn, ud);
    return m.out;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);
    mcp_server_t *srv = make_server(ctx);

    mcp_status_t st;
    char *out;
    char buf[8192];
    char *req;

    const char *init_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}},"
        "\"clientInfo\":{\"name\":\"h\",\"version\":\"0.1.0\"}}";

    /* 1. PRM parse + serialize round-trip (data plane, no HTTP) */
    {
        const char *doc =
            "{\"resource\":\"https://mcp.example.com/api\","
            "\"issuer\":\"https://auth.example.com\","
            "\"authorization_servers\":[\"https://auth.example.com\",\"https://auth2.example.com\"],"
            "\"scopes_supported\":[\"mcp.read\",\"mcp.write\"],"
            "\"jwks_uri\":\"https://auth.example.com/.well-known/jwks.json\"}";
        mcp_protected_resource_metadata_t pr;
        CHECK(mcp_oauth_protected_resource_parse(ctx, doc, strlen(doc), &pr) == MCP_OK);
        CHECK(strcmp(pr.resource, "https://mcp.example.com/api") == 0);
        CHECK(strcmp(pr.issuer, "https://auth.example.com") == 0);
        CHECK(strcmp(pr.authorization_server, "https://auth.example.com") == 0);
        CHECK(strcmp(pr.scope, "mcp.read") == 0);
        CHECK(strcmp(pr.jwks_uri, "https://auth.example.com/.well-known/jwks.json") == 0);

        char *ser = mcp_oauth_protected_resource_serialize(ctx, &pr);
        CHECK(ser != NULL);
        CHECK(strstr(ser, "\"resource\":\"https://mcp.example.com/api\"") != NULL);
        CHECK(strstr(ser, "\"authorization_servers\":[\"https://auth.example.com\"]") != NULL);
        CHECK(strstr(ser, "\"scopes_supported\":[\"mcp.read\"]") != NULL);
        mcp_oauth_free_string(ctx, ser);

        /* missing `resource` -> PROTOCOL */
        mcp_protected_resource_metadata_t pr2;
        CHECK(mcp_oauth_protected_resource_parse(ctx, "{\"issuer\":\"x\"}", 14, &pr2)
              == MCP_ERR_PROTOCOL);
        CHECK(mcp_oauth_protected_resource_parse(ctx, NULL, 0, &pr2)
              == MCP_ERR_INVALID_ARGUMENT);
    }

    /* 2. GET /.well-known/oauth-protected-resource: unconfigured -> 404 */
    {
        const char *reqs[] = {
            "GET /.well-known/oauth-protected-resource HTTP/1.1\r\n\r\n"
        };
        out = run_script(ctx, srv, reqs, 1, &st);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 404 Not Found") != NULL);
        free(out);
    }

    /* 3. Configure pr_meta -> endpoint returns 200 + JSON body */
    {
        mcp_protected_resource_metadata_t pr;
        memset(&pr, 0, sizeof(pr));
        snprintf(pr.resource, sizeof(pr.resource), "https://mcp.example.com/api");
        snprintf(pr.issuer, sizeof(pr.issuer), "https://auth.example.com");
        snprintf(pr.authorization_server, sizeof(pr.authorization_server),
                 "https://auth.example.com");
        CHECK(mcp_server_set_protected_resource_metadata(ctx, srv, &pr) == MCP_OK);
        CHECK(mcp_server_get_protected_resource_metadata(ctx, srv) != NULL);

        const char *reqs[] = {
            "GET /.well-known/oauth-protected-resource HTTP/1.1\r\n\r\n"
        };
        out = run_script(ctx, srv, reqs, 1, &st);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
        CHECK(strstr(out, "application/json") != NULL);
        CHECK(strstr(out, "\"resource\":\"https://mcp.example.com/api\"") != NULL);
        free(out);
    }

    /* 4. 401 with pr_meta configured + MCP-Protocol-Version: 2026-07-28
          (MUST grade) -> resource_metadata fragment present */
    {
        const char *hdrs = "MCP-Protocol-Version: 2026-07-28\r\n";
        req = make_post(init_body, hdrs, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth(ctx, srv, reqs, 1, &st, auth_always_false, NULL);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 401 Unauthorized") != NULL);
        CHECK(strstr(out, "resource_metadata=\"https://mcp.example.com/api\"") != NULL);
        free(out);
    }

    /* 5. 401 with pr_meta configured + MCP-Protocol-Version: 2025-11-25
          (SHOULD grade) -> resource_metadata fragment present */
    {
        const char *hdrs = "MCP-Protocol-Version: 2025-11-25\r\n";
        req = make_post(init_body, hdrs, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth(ctx, srv, reqs, 1, &st, auth_always_false, NULL);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 401 Unauthorized") != NULL);
        CHECK(strstr(out, "resource_metadata=\"https://mcp.example.com/api\"") != NULL);
        free(out);
    }

    /* 6. 401 with pr_meta configured + pre-P3 version (2025-06-18) -> bare
          Bearer, no resource_metadata (zero-regression gate check) */
    {
        const char *hdrs = "MCP-Protocol-Version: 2025-06-18\r\n";
        req = make_post(init_body, hdrs, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth(ctx, srv, reqs, 1, &st, auth_always_false, NULL);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 401 Unauthorized") != NULL);
        CHECK(strstr(out, "WWW-Authenticate: Bearer\r\n") != NULL);
        CHECK(strstr(out, "resource_metadata") == NULL);
        free(out);
    }

    /* 7. 401 with NO MCP-Protocol-Version header at all (legacy client) ->
          bare Bearer even though pr_meta is configured (gate inactive) */
    {
        req = make_post(init_body, NULL, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth(ctx, srv, reqs, 1, &st, auth_always_false, NULL);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "WWW-Authenticate: Bearer\r\n") != NULL);
        CHECK(strstr(out, "resource_metadata") == NULL);
        free(out);
    }

    /* ---- P3 T4: SEP-835 incremental scope consent via auth_fn_v2 ---- */

    /* A 2026 protocol-version header MUST agree with the initialize body's
       protocolVersion or validate_http_metadata rejects it as -32020 before
       auth is ever consulted; use a matching 2026-07-28 body for these cases. */
    const char *init_2026_body =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2026-07-28\","
        "\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"0.1.0\"}}}";
    const char *scope_hdrs =
        "Authorization: Bearer tok\r\nMCP-Protocol-Version: 2026-07-28\r\n";
    const char *scope_hdrs_2025_11_25 =
        "Authorization: Bearer tok\r\nMCP-Protocol-Version: 2025-11-25\r\n";
    const char *scope_hdrs_pre_p3 =
        "Authorization: Bearer tok\r\nMCP-Protocol-Version: 2025-06-18\r\n";

    /* 8. v2 SCOPE_INSUFFICIENT with scope + resource_metadata +
          error_description (2026 MUST) -> all three fragments in the
          WWW-Authenticate challenge */
    {
        static const char kScope[] = "mcp.read mcp.write";
        static const char kResMeta[] = "https://mcp.example.com/api";
        static const char kDesc[] = "token lacks mcp.write";
        v2_expected_t exp = { .code = MCP_HTTP_AUTH_SCOPE_INSUFFICIENT,
                              .scope = kScope, .resource_metadata = kResMeta,
                              .error_description = kDesc };
        req = make_post(init_2026_body, scope_hdrs, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth_v2(ctx, srv, reqs, 1, &st, auth_v2_fixed, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 401 Unauthorized") != NULL);
        CHECK(strstr(out, "scope=\"mcp.read mcp.write\"") != NULL);
        CHECK(strstr(out, "resource_metadata=\"https://mcp.example.com/api\"") != NULL);
        CHECK(strstr(out, "error_description=\"token lacks mcp.write\"") != NULL);
        free(out);
    }

    /* 9. v2 DENY -> bare 401 (no scope fragment); resource_metadata is
          still present because pr_meta is configured + 2026 header (T3 gate)
          but the callback's own fields are unused for DENY */
    {
        v2_expected_t exp = { .code = MCP_HTTP_AUTH_DENY, .scope = NULL,
                              .resource_metadata = NULL, .error_description = NULL };
        req = make_post(init_2026_body, scope_hdrs, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth_v2(ctx, srv, reqs, 1, &st, auth_v2_fixed, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 401 Unauthorized") != NULL);
        CHECK(strstr(out, "scope=") == NULL);
        /* resource_metadata comes from the T3 gate, not from the callback */
        CHECK(strstr(out, "resource_metadata=\"https://mcp.example.com/api\"") != NULL);
        free(out);
    }

    /* 10. v2 SCOPE_INSUFFICIENT with resource_metadata=NULL -> the T3 gate
           still supplies resource_metadata from the configured pr_meta */
    {
        static const char kScope[] = "mcp.read";
        v2_expected_t exp = { .code = MCP_HTTP_AUTH_SCOPE_INSUFFICIENT,
                              .scope = kScope, .resource_metadata = NULL,
                              .error_description = NULL };
        req = make_post(init_2026_body, scope_hdrs, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth_v2(ctx, srv, reqs, 1, &st, auth_v2_fixed, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 401 Unauthorized") != NULL);
        CHECK(strstr(out, "scope=\"mcp.read\"") != NULL);
        CHECK(strstr(out, "resource_metadata=\"https://mcp.example.com/api\"") != NULL);
        free(out);
    }

    /* 11. v2 SCOPE_INSUFFICIENT under 2025-11-25 (SHOULD grade): same
           fragments emitted; the grade difference is advisory (MUST-grade
           2026 makes resource_metadata mandatory, but this library's gate
           is the same boolean — documented in the P3 spec). */
    {
        static const char kScope[] = "mcp.read";
        v2_expected_t exp = { .code = MCP_HTTP_AUTH_SCOPE_INSUFFICIENT,
                              .scope = kScope, .resource_metadata = NULL,
                              .error_description = NULL };
        req = make_post(init_body, scope_hdrs_2025_11_25, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth_v2(ctx, srv, reqs, 1, &st, auth_v2_fixed, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "scope=\"mcp.read\"") != NULL);
        CHECK(strstr(out, "resource_metadata=\"https://mcp.example.com/api\"") != NULL);
        free(out);
    }

    /* 12. v2 OK -> request proceeds to dispatch (200); a valid 2026 request
           must carry the SEP-2243 header trio (Mcp-Method/Mcp-Name match the
           body) or it is rejected -32020 before auth; here the body has no
           params.name so Mcp-Name is not required, but Mcp-Method is */
    {
        const char *hdrs =
            "Authorization: Bearer tok\r\nMCP-Protocol-Version: 2026-07-28\r\n"
            "Mcp-Method: initialize\r\n";
        v2_expected_t exp = { .code = MCP_HTTP_AUTH_OK, .scope = NULL,
                              .resource_metadata = NULL, .error_description = NULL };
        req = make_post(init_2026_body, hdrs, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth_v2(ctx, srv, reqs, 1, &st, auth_v2_fixed, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "HTTP/1.1 200 OK") != NULL);
        CHECK(strstr(out, "\"serverInfo\"") != NULL);
        free(out);
    }

    /* 13. v2 SCOPE_INSUFFICIENT under a pre-P3 version header (2025-06-18):
           p3_active=false, so pr_resource=NULL; only the callback's own
           scope fragment is emitted, no resource_metadata at all. */
    {
        static const char kScope[] = "mcp.read";
        v2_expected_t exp = { .code = MCP_HTTP_AUTH_SCOPE_INSUFFICIENT,
                              .scope = kScope, .resource_metadata = NULL,
                              .error_description = NULL };
        req = make_post(init_body, scope_hdrs_pre_p3, buf, sizeof(buf));
        const char *reqs[] = { req };
        out = run_script_auth_v2(ctx, srv, reqs, 1, &st, auth_v2_fixed, &exp);
        CHECK(st == MCP_OK && out != NULL);
        CHECK(strstr(out, "scope=\"mcp.read\"") != NULL);
        CHECK(strstr(out, "resource_metadata") == NULL);
        free(out);
    }

    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("test_http_9728_scp835 OK\n");
    return 0;
}
