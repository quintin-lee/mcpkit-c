#define _POSIX_C_SOURCE 200809L

#include "mcpkit/runtime/timer.h"

#include <stdint.h>
#include <time.h>

#include "internals.h"

typedef struct entry {
    uint64_t due_ns;
    mcp_task_fn fn;
    void *arg;
    struct entry *next;
} entry_t;

struct mcp_timer {
    entry_t *head;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static uint64_t saturating_add(uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    return r < a ? UINT64_MAX : r;
}

static uint64_t delay_to_ns(uint64_t delay_ms) {
    if (delay_ms > UINT64_MAX / 1000000u) {
        return UINT64_MAX;
    }
    return delay_ms * 1000000u;
}

mcp_timer_t *mcp_timer_create(mcp_context_t *ctx) {
    mcp_timer_t *t = rt_malloc(ctx, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->head = NULL;
    return t;
}

void mcp_timer_destroy(mcp_context_t *ctx, mcp_timer_t *t) {
    if (t == NULL) {
        return;
    }
    for (entry_t *e = t->head; e != NULL;) {
        entry_t *next = e->next;
        rt_free(ctx, e);
        e = next;
    }
    rt_free(ctx, t);
}

mcp_status_t mcp_timer_schedule(mcp_context_t *ctx, mcp_timer_t *t, uint64_t delay_ms,
                                mcp_task_fn fn, void *arg) {
    if (t == NULL || fn == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    entry_t *e = rt_malloc(ctx, sizeof(*e));
    if (e == NULL) {
        return MCP_ERR_NOMEM;
    }
    e->due_ns = saturating_add(now_ns(), delay_to_ns(delay_ms));
    e->fn = fn;
    e->arg = arg;
    entry_t **link = &t->head;
    while (*link != NULL && (*link)->due_ns <= e->due_ns) {
        link = &(*link)->next;
    }
    e->next = *link;
    *link = e;
    return MCP_OK;
}

size_t mcp_timer_cancel(mcp_context_t *ctx, mcp_timer_t *t, mcp_task_fn fn, void *arg) {
    if (t == NULL || fn == NULL) {
        return 0;
    }
    size_t n = 0;
    entry_t **link = &t->head;
    while (*link != NULL) {
        if ((*link)->fn == fn && (*link)->arg == arg) {
            entry_t *dead = *link;
            *link = dead->next;
            rt_free(ctx, dead);
            n++;
        } else {
            link = &(*link)->next;
        }
    }
    return n;
}

mcp_status_t mcp_timer_poll(mcp_context_t *ctx, mcp_timer_t *t, size_t *fired_out) {
    if (t == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    size_t fired = 0;
    for (;;) {
        entry_t *e = t->head;
        if (e == NULL || e->due_ns > now_ns()) {
            break;
        }
        t->head = e->next;
        e->fn(ctx, e->arg);
        rt_free(ctx, e);
        fired++;
    }
    if (fired_out != NULL) {
        *fired_out = fired;
    }
    return MCP_OK;
}
