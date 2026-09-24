#include "test_check.h"

#include <stdio.h>
#include <string.h>

#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/sse.h"

typedef struct {
    int count;
    char last_event[64];
    char last_data[256];
    char last_id[64];
    int64_t last_retry;
} test_recorder_t;

static void on_test_event(const mcp_sse_event_t *ev, void *user) {
    test_recorder_t *rec = (test_recorder_t *)user;
    rec->count++;
    if (ev->event != NULL) {
        strncpy(rec->last_event, ev->event, sizeof(rec->last_event) - 1);
    } else {
        rec->last_event[0] = '\0';
    }
    if (ev->data != NULL) {
        strncpy(rec->last_data, ev->data, sizeof(rec->last_data) - 1);
    } else {
        rec->last_data[0] = '\0';
    }
    if (ev->id != NULL) {
        strncpy(rec->last_id, ev->id, sizeof(rec->last_id) - 1);
    } else {
        rec->last_id[0] = '\0';
    }
    rec->last_retry = ev->retry_ms;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    test_recorder_t rec;
    memset(&rec, 0, sizeof(rec));

    /* 1. NULL checks */
    CHECK(mcp_sse_parser_create(ctx, NULL, NULL) == NULL);
    mcp_sse_parser_t *parser = mcp_sse_parser_create(ctx, on_test_event, &rec);
    CHECK(parser != NULL);
    CHECK(mcp_sse_parser_feed(ctx, NULL, "data: hi\n\n", 10) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_sse_parser_feed(ctx, parser, NULL, 5) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_sse_parser_feed(ctx, parser, "test", 0) == MCP_OK);
    CHECK(mcp_sse_parser_last_event_id(ctx, parser) == NULL);

    /* 2. Basic message */
    const char *chunk1 = "data: hello world\n\n";
    CHECK(mcp_sse_parser_feed(ctx, parser, chunk1, strlen(chunk1)) == MCP_OK);
    CHECK(rec.count == 1);
    CHECK(strcmp(rec.last_event, "message") == 0);
    CHECK(strcmp(rec.last_data, "hello world") == 0);
    CHECK(rec.last_id[0] == '\0');

    /* 3. Full event with event type, id, retry and comment */
    const char *chunk2 =
        ": comment line to ignore\n"
        "event: endpoint\n"
        "id: 42\n"
        "retry: 5000\n"
        "data: http://localhost:8080/mcp\n\n";
    CHECK(mcp_sse_parser_feed(ctx, parser, chunk2, strlen(chunk2)) == MCP_OK);
    CHECK(rec.count == 2);
    CHECK(strcmp(rec.last_event, "endpoint") == 0);
    CHECK(strcmp(rec.last_data, "http://localhost:8080/mcp") == 0);
    CHECK(strcmp(rec.last_id, "42") == 0);
    CHECK(rec.last_retry == 5000);
    CHECK(strcmp(mcp_sse_parser_last_event_id(ctx, parser), "42") == 0);

    /* 4. Multiline data concatenation */
    const char *chunk3 =
        "data: first line\n"
        "data: second line\n"
        "data: third line\n\n";
    CHECK(mcp_sse_parser_feed(ctx, parser, chunk3, strlen(chunk3)) == MCP_OK);
    CHECK(rec.count == 3);
    CHECK(strcmp(rec.last_data, "first line\nsecond line\nthird line") == 0);
    /* id was not set for chunk3, but last_event_id persists */
    CHECK(rec.last_id[0] == '\0');
    CHECK(strcmp(mcp_sse_parser_last_event_id(ctx, parser), "42") == 0);

    /* 5. Highly fragmented feed (1 byte at a time) */
    const char *frag = "event: frag_event\r\nid: 99\r\ndata: piecewise\r\n\r\n";
    size_t frag_len = strlen(frag);
    for (size_t i = 0; i < frag_len; i++) {
        CHECK(mcp_sse_parser_feed(ctx, parser, &frag[i], 1) == MCP_OK);
    }
    CHECK(rec.count == 4);
    CHECK(strcmp(rec.last_event, "frag_event") == 0);
    CHECK(strcmp(rec.last_data, "piecewise") == 0);
    CHECK(strcmp(rec.last_id, "99") == 0);
    CHECK(strcmp(mcp_sse_parser_last_event_id(ctx, parser), "99") == 0);

    mcp_sse_parser_destroy(ctx, parser);
    mcp_sse_parser_destroy(ctx, NULL);
    mcp_context_destroy(ctx);

    printf("test_sse_parser OK\n");
    return 0;
}
