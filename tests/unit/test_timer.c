#include "test_check.h"
#include <stddef.h>
#include <stdint.h>

#include "mcpkit/runtime/timer.h"

static int seq[8];
static int nseq;

static void rec(mcp_context_t *ctx, void *arg) {
    (void)ctx;
    seq[nseq++] = (int)(intptr_t)arg;
}

int main(void) {
    mcp_timer_t *probe = mcp_timer_create(NULL);
    CHECK(probe != NULL);
    mcp_timer_destroy(NULL, probe);
    mcp_timer_t *t = mcp_timer_create(NULL);
    CHECK(t != NULL);

    CHECK(mcp_timer_schedule(NULL, NULL, 0, rec, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_timer_schedule(NULL, t, 0, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_timer_poll(NULL, NULL, NULL) == MCP_ERR_INVALID_ARGUMENT);
    CHECK(mcp_timer_cancel(NULL, NULL, rec, NULL) == 0);
    CHECK(mcp_timer_cancel(NULL, t, NULL, NULL) == 0);

    int fired_total = 0;
    size_t fired = 0;

    CHECK(mcp_timer_schedule(NULL, t, 0, rec, (void *)(intptr_t)1) == MCP_OK);
    CHECK(mcp_timer_schedule(NULL, t, 60000, rec, (void *)(intptr_t)2) == MCP_OK);
    CHECK(mcp_timer_poll(NULL, t, &fired) == MCP_OK);
    CHECK(fired == 1);
    CHECK(nseq == 1 && seq[0] == 1);
    fired_total += (int)fired;

    nseq = 0;
    CHECK(mcp_timer_schedule(NULL, t, 0, rec, (void *)(intptr_t)10) == MCP_OK);
    CHECK(mcp_timer_schedule(NULL, t, 0, rec, (void *)(intptr_t)11) == MCP_OK);
    CHECK(mcp_timer_schedule(NULL, t, 60000, rec, (void *)(intptr_t)12) == MCP_OK);
    CHECK(mcp_timer_poll(NULL, t, NULL) == MCP_OK);
    CHECK(nseq == 2 && seq[0] == 10 && seq[1] == 11);

    nseq = 0;
    CHECK(mcp_timer_schedule(NULL, t, 0, rec, (void *)(intptr_t)20) == MCP_OK);
    CHECK(mcp_timer_schedule(NULL, t, 0, rec, (void *)(intptr_t)21) == MCP_OK);
    CHECK(mcp_timer_cancel(NULL, t, rec, (void *)(intptr_t)21) == 1);
    CHECK(mcp_timer_poll(NULL, t, &fired) == MCP_OK);
    CHECK(fired == 1);
    CHECK(nseq == 1 && seq[0] == 20);
    CHECK(mcp_timer_cancel(NULL, t, rec, (void *)(intptr_t)99) == 0);

    CHECK(mcp_timer_poll(NULL, t, &fired) == MCP_OK);
    CHECK(fired == 0);

    mcp_timer_destroy(NULL, t);
    mcp_timer_destroy(NULL, NULL);
    (void)fired_total;
    (void)seq;
    return 0;
}
