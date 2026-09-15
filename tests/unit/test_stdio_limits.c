#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

static FILE *make_input(const char *data, size_t len) {
    FILE *f = tmpfile();
    assert(f != NULL);
    assert(fwrite(data, 1, len, f) == len);
    rewind(f);
    return f;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);

    FILE *in = make_input("{\"a\":1}\r\n", 9);
    FILE *out = tmpfile();
    assert(out != NULL);
    mcp_transport_t *t = mcp_stdio_transport_create(ctx, in, out);
    assert(t != NULL);

    char *line = NULL;
    assert(mcp_transport_recv(ctx, t, &line) == MCP_OK);
    assert(line != NULL && strcmp(line, "{\"a\":1}") == 0);
    mcp_json_free_string(ctx, line);

    line = (char *)0x1;
    assert(mcp_transport_recv(ctx, t, &line) == MCP_ERR_IO);
    assert(line == NULL);

    assert(mcp_transport_send(ctx, t, "{\"ok\":true}", strlen("{\"ok\":true}")) == MCP_OK);
    rewind(out);
    char back[32];
    assert(fgets(back, (int)sizeof(back), out) != NULL);
    assert(strcmp(back, "{\"ok\":true}\n") == 0);

    assert(mcp_transport_send(ctx, t, NULL, 0) == MCP_OK);
    assert(mcp_transport_stop(ctx, t) == MCP_OK);
    assert(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);
    fclose(in);
    fclose(out);

    FILE *empty = make_input("", 0);
    t = mcp_stdio_transport_create(ctx, empty, NULL);
    assert(t != NULL);
    line = (char *)0x1;
    assert(mcp_transport_recv(ctx, t, &line) == MCP_ERR_IO);
    assert(line == NULL);
    assert(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);
    fclose(empty);

    FILE *partial = make_input("tail-no-newline", 15);
    t = mcp_stdio_transport_create(ctx, partial, NULL);
    assert(t != NULL);
    line = NULL;
    assert(mcp_transport_recv(ctx, t, &line) == MCP_OK);
    assert(line != NULL && strcmp(line, "tail-no-newline") == 0);
    mcp_json_free_string(ctx, line);
    assert(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);
    fclose(partial);

    size_t big = (size_t)4 * 1024 * 1024 + 1;
    FILE *huge = tmpfile();
    assert(huge != NULL);
    for (size_t i = 0; i < big; i++) {
        assert(fputc('x', huge) != EOF);
    }
    assert(fputc('\n', huge) != EOF);
    rewind(huge);
    t = mcp_stdio_transport_create(ctx, huge, NULL);
    assert(t != NULL);
    line = (char *)0x1;
    assert(mcp_transport_recv(ctx, t, &line) == MCP_ERR_PROTOCOL);
    assert(line == NULL);
    line = (char *)0x1;
    assert(mcp_transport_recv(ctx, t, &line) == MCP_ERR_IO);
    assert(line == NULL);
    assert(mcp_transport_stop(ctx, t) == MCP_OK);
    mcp_transport_destroy(ctx, t);
    fclose(huge);

    mcp_context_destroy(ctx);
    printf("test_stdio_limits OK\n");
    return 0;
}
