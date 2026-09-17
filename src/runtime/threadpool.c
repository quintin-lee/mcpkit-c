/**
 * @file threadpool.c
 *
 * C11 <threads.h> fixed-size pool. Each submitted task node captures
 * the submit-time context so the task and its later free both route
 * through that context's allocator (a pool outliving its creating
 * context stays self-consistent). destroy() discards pending tasks
 * without running them; call mcp_executor_wait first to drain.
 */
#include "mcpkit/runtime/threadpool.h"

#include <stdbool.h>
#include <string.h>
#include <threads.h>

#include "internals.h"
#include "mcpkit/runtime/executor.h"

typedef struct task_node {
    mcp_context_t *ctx;
    mcp_task_fn fn;
    void *arg;
    struct task_node *next;
} task_node_t;

typedef struct pool {
    mtx_t lock;
    cnd_t avail;
    cnd_t drained;
    task_node_t *head;
    task_node_t *tail;
    thrd_t *workers;
    size_t n_workers;
    size_t pending;
    size_t active;
    bool stop;
} pool_t;

static int pool_worker(void *v) {
    pool_t *pl = v;
    for (;;) {
        mtx_lock(&pl->lock);
        while (!pl->stop && pl->head == NULL) {
            cnd_wait(&pl->avail, &pl->lock);
        }
        if (pl->stop && pl->head == NULL) {
            mtx_unlock(&pl->lock);
            return 0;
        }
        task_node_t *n = pl->head;
        pl->head = n->next;
        if (pl->head == NULL) {
            pl->tail = NULL;
        }
        pl->pending--;
        pl->active++;
        mtx_unlock(&pl->lock);

        n->fn(n->ctx, n->arg);
        rt_free(n->ctx, n);

        mtx_lock(&pl->lock);
        pl->active--;
        if (pl->pending == 0 && pl->active == 0) {
            cnd_broadcast(&pl->drained);
        }
        mtx_unlock(&pl->lock);
    }
}

static void pool_abort(mcp_context_t *ctx, pool_t *pl, size_t started) {
    mtx_lock(&pl->lock);
    pl->stop = true;
    cnd_broadcast(&pl->avail);
    mtx_unlock(&pl->lock);
    for (size_t i = 0; i < started; i++) {
        thrd_join(pl->workers[i], NULL);
    }
    cnd_destroy(&pl->drained);
    cnd_destroy(&pl->avail);
    mtx_destroy(&pl->lock);
    rt_free(ctx, pl);
}

static mcp_status_t pool_submit(mcp_context_t *ctx, mcp_executor_t *ex, mcp_task_fn fn,
                                void *arg) {
    if (ex == NULL || fn == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    pool_t *pl = mcp_executor_backend(ctx, ex);
    if (pl == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    task_node_t *n = rt_malloc(ctx, sizeof(*n));
    if (n == NULL) {
        return MCP_ERR_NOMEM;
    }
    n->ctx = ctx;
    n->fn = fn;
    n->arg = arg;
    n->next = NULL;
    mtx_lock(&pl->lock);
    if (pl->tail == NULL) {
        pl->head = n;
    } else {
        pl->tail->next = n;
    }
    pl->tail = n;
    pl->pending++;
    cnd_signal(&pl->avail);
    mtx_unlock(&pl->lock);
    return MCP_OK;
}

static mcp_status_t pool_wait(mcp_context_t *ctx, mcp_executor_t *ex) {
    (void)ctx;
    if (ex == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    pool_t *pl = mcp_executor_backend(ctx, ex);
    if (pl == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mtx_lock(&pl->lock);
    while (pl->pending + pl->active > 0) {
        cnd_wait(&pl->drained, &pl->lock);
    }
    mtx_unlock(&pl->lock);
    return MCP_OK;
}

static void pool_destroy_backend(mcp_context_t *ctx, mcp_executor_t *ex) {
    if (ex == NULL) {
        return;
    }
    pool_t *pl = mcp_executor_backend(ctx, ex);
    if (pl == NULL) {
        return;
    }
    mtx_lock(&pl->lock);
    pl->stop = true;
    cnd_broadcast(&pl->avail);
    mtx_unlock(&pl->lock);
    for (size_t i = 0; i < pl->n_workers; i++) {
        thrd_join(pl->workers[i], NULL);
    }
    for (task_node_t *n = pl->head; n != NULL;) {
        task_node_t *next = n->next;
        rt_free(ctx, n);
        n = next;
    }
    cnd_destroy(&pl->drained);
    cnd_destroy(&pl->avail);
    mtx_destroy(&pl->lock);
    rt_free(ctx, pl);
}

static const mcp_executor_ops_t pool_ops = {
    .submit = pool_submit,
    .wait = pool_wait,
    .destroy_backend = pool_destroy_backend,
};

mcp_executor_t *mcp_threadpool_create(mcp_context_t *ctx, size_t thread_count) {
    if (thread_count == 0) {
        return NULL;
    }
    pool_t *pl = rt_malloc(ctx, sizeof(*pl) + thread_count * sizeof(thrd_t));
    if (pl == NULL) {
        return NULL;
    }
    memset(pl, 0, sizeof(*pl));
    pl->workers = (thrd_t *)(pl + 1);
    pl->n_workers = thread_count;
    if (mtx_init(&pl->lock, mtx_plain) != thrd_success || cnd_init(&pl->avail) != thrd_success ||
        cnd_init(&pl->drained) != thrd_success) {
        rt_free(ctx, pl);
        return NULL;
    }
    size_t started = 0;
    for (; started < thread_count; started++) {
        if (thrd_create(&pl->workers[started], pool_worker, pl) != thrd_success) {
            break;
        }
    }
    if (started < thread_count) {
        pool_abort(ctx, pl, started);
        return NULL;
    }
    mcp_executor_t *ex = mcp_executor_create(ctx, &pool_ops, pl);
    if (ex == NULL) {
        pool_abort(ctx, pl, thread_count);
        return NULL;
    }
    return ex;
}
