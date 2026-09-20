#include "test_check.h"
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static FILE *make_input(const char *data, size_t len) {
    FILE *f = tmpfile();
    CHECK(f != NULL);
    CHECK(fwrite(data, 1, len, f) == len);
    rewind(f);
    return f;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    FILE *in = make_input("{\"a\":1}\r\n", 9);
    FILE *out = tmpfile();
    CHECK(out != NULL);
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, in, out);
    CHECK(t != NULL);

    char *line = NULL;
    CHECK(mcp_transport_recv(ctx, t, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, "{\"a\":1}") == 0);
    mcp_json_free_string(ctx, line);

    line = (char *)0x1;
    CHECK(mcp_transport_recv(ctx, t, &line) == MCP_ERR_IO);
    CHECK(line == NULL);

    CHECK(mcp_transport_send(ctx, t, "{\"ok\":true}", strlen("{\"ok\":true}")) == MCP_OK);
    rewind(out);
    char back[32];
    CHECK(fgets(back, (int)sizeof(back), out) != NULL);
    CHECK(strcmp(back, "{\"ok\":true}\n") == 0);

    CHECK(mcp_transport_send(ctx, t, NULL, 0) == MCP_OK);
    CHECK(mcp_transport_stop(ctx, t) == MCP_OK);
    CHECK(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);
    fclose(in);
    fclose(out);

    FILE *empty = make_input("", 0);
    t = mcp_stdio_transport_create(ctx, empty, NULL);
    CHECK(t != NULL);
    line = (char *)0x1;
    CHECK(mcp_transport_recv(ctx, t, &line) == MCP_ERR_IO);
    CHECK(line == NULL);
    CHECK(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);
    fclose(empty);

    FILE *partial = make_input("tail-no-newline", 15);
    t = mcp_stdio_transport_create(ctx, partial, NULL);
    CHECK(t != NULL);
    line = NULL;
    CHECK(mcp_transport_recv(ctx, t, &line) == MCP_OK);
    CHECK(line != NULL && strcmp(line, "tail-no-newline") == 0);
    mcp_json_free_string(ctx, line);
    CHECK(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);
    fclose(partial);

    size_t big = (size_t)4 * 1024 * 1024 + 1;
    FILE *huge = tmpfile();
    CHECK(huge != NULL);
    for (size_t i = 0; i < big; i++) {
        CHECK(fputc('x', huge) != EOF);
    }
    CHECK(fputc('\n', huge) != EOF);
    rewind(huge);
    t = mcp_stdio_transport_create(ctx, huge, NULL);
    CHECK(t != NULL);
    line = (char *)0x1;
    CHECK(mcp_transport_recv(ctx, t, &line) == MCP_ERR_PROTOCOL);
    CHECK(line == NULL);
    line = (char *)0x1;
    CHECK(mcp_transport_recv(ctx, t, &line) == MCP_ERR_IO);
    CHECK(line == NULL);
    CHECK(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);
    fclose(huge);

    mcp_context_destroy(ctx);
    printf("test_stdio_limits OK\n");
    return 0;
}
