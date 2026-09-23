/**
 * @file tasks.c
 * @brief MCP Tasks extension implementation (SEP-2663).
 * @ingroup mcpkit-protocol
 */

#include "mcpkit/protocol/tasks.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void safe_copy_str(char *dst, size_t dst_sz, const char *src) {
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dst_sz) {
        len = dst_sz - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

const char *mcp_task_status_name(mcp_task_status_t status) {
    switch (status) {
    case MCP_TASK_STATUS_WORKING:
        return "working";
    case MCP_TASK_STATUS_INPUT_REQUIRED:
        return "input_required";
    case MCP_TASK_STATUS_COMPLETED:
        return "completed";
    case MCP_TASK_STATUS_FAILED:
        return "failed";
    case MCP_TASK_STATUS_CANCELLED:
        return "cancelled";
    default:
        return "unknown";
    }
}

typedef struct mcp_task_entry {
    char task_id[64];
    mcp_task_status_t status;
    uint64_t ttl_ms;
    uint64_t poll_interval_ms;
    char status_message[256];
    mcp_json_value_t *result;
    mcp_json_value_t *error;
    mcp_json_value_t *input_requests;
} mcp_task_entry_t;

struct mcp_task_mgr {
    mcp_task_entry_t *tasks;
    size_t count;
    size_t capacity;
};

static void *task_malloc(mcp_context_t *ctx, size_t sz) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->malloc_fn(sz, a->userdata);
}

static void *task_realloc(mcp_context_t *ctx, void *ptr, size_t sz) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->realloc_fn(ptr, sz, a->userdata);
}

static void task_free(mcp_context_t *ctx, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    a->free_fn(ptr, a->userdata);
}

mcp_task_mgr_t *mcp_task_mgr_new(mcp_context_t *ctx) {
    mcp_task_mgr_t *mgr = task_malloc(ctx, sizeof(*mgr));
    if (mgr == NULL) {
        return NULL;
    }
    memset(mgr, 0, sizeof(*mgr));
    return mgr;
}

void mcp_task_mgr_free(mcp_context_t *ctx, mcp_task_mgr_t *mgr) {
    if (mgr == NULL) {
        return;
    }
    for (size_t i = 0; i < mgr->count; i++) {
        mcp_task_entry_t *t = &mgr->tasks[i];
        if (t->result != NULL) {
            mcp_json_destroy(ctx, t->result);
            t->result = NULL;
        }
        if (t->error != NULL) {
            mcp_json_destroy(ctx, t->error);
            t->error = NULL;
        }
        if (t->input_requests != NULL) {
            mcp_json_destroy(ctx, t->input_requests);
            t->input_requests = NULL;
        }
    }
    task_free(ctx, mgr->tasks);
    task_free(ctx, mgr);
}

static mcp_task_entry_t *find_task(mcp_task_mgr_t *mgr, const char *task_id) {
    if (mgr == NULL || task_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->tasks[i].task_id, task_id) == 0) {
            return &mgr->tasks[i];
        }
    }
    return NULL;
}

mcp_status_t mcp_task_mgr_create(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                 const char *task_id, uint64_t ttl_ms,
                                 uint64_t poll_interval_ms) {
    if (mgr == NULL || task_id == NULL || task_id[0] == '\0') {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (find_task(mgr, task_id) != NULL) {
        return MCP_ERR_ALREADY_EXISTS;
    }

    if (mgr->count == mgr->capacity) {
        size_t new_cap = mgr->capacity == 0 ? 8 : mgr->capacity * 2;
        mcp_task_entry_t *new_tasks = task_realloc(ctx, mgr->tasks, new_cap * sizeof(*new_tasks));
        if (new_tasks == NULL) {
            return MCP_ERR_NOMEM;
        }
        mgr->tasks = new_tasks;
        mgr->capacity = new_cap;
    }

    mcp_task_entry_t *t = &mgr->tasks[mgr->count++];
    memset(t, 0, sizeof(*t));
    safe_copy_str(t->task_id, sizeof(t->task_id), task_id);
    t->status = MCP_TASK_STATUS_WORKING;
    t->ttl_ms = ttl_ms;
    t->poll_interval_ms = poll_interval_ms;
    return MCP_OK;
}

mcp_status_t mcp_task_mgr_set_status(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                     const char *task_id, mcp_task_status_t status,
                                     const char *msg) {
    (void)ctx;
    mcp_task_entry_t *t = find_task(mgr, task_id);
    if (t == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    t->status = status;
    if (msg != NULL) {
        safe_copy_str(t->status_message, sizeof(t->status_message), msg);
    }
    return MCP_OK;
}

mcp_status_t mcp_task_mgr_set_result(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                     const char *task_id, mcp_json_value_t *result) {
    mcp_task_entry_t *t = find_task(mgr, task_id);
    if (t == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    if (t->result != NULL) {
        mcp_json_destroy(ctx, t->result);
    }
    t->result = result;
    t->status = MCP_TASK_STATUS_COMPLETED;
    return MCP_OK;
}

mcp_status_t mcp_task_mgr_set_error(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                    const char *task_id, mcp_json_value_t *error) {
    mcp_task_entry_t *t = find_task(mgr, task_id);
    if (t == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    if (t->error != NULL) {
        mcp_json_destroy(ctx, t->error);
    }
    t->error = error;
    t->status = MCP_TASK_STATUS_FAILED;
    return MCP_OK;
}

mcp_status_t mcp_task_mgr_set_input_requests(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                             const char *task_id, mcp_json_value_t *input_requests) {
    mcp_task_entry_t *t = find_task(mgr, task_id);
    if (t == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    if (t->input_requests != NULL) {
        mcp_json_destroy(ctx, t->input_requests);
    }
    t->input_requests = input_requests;
    t->status = MCP_TASK_STATUS_INPUT_REQUIRED;
    return MCP_OK;
}

mcp_status_t mcp_task_mgr_get(mcp_context_t *ctx, const mcp_task_mgr_t *mgr,
                              const char *task_id, mcp_task_desc_t *out_desc) {
    (void)ctx;
    if (mgr == NULL || task_id == NULL || out_desc == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_task_entry_t *t = find_task((mcp_task_mgr_t *)mgr, task_id);
    if (t == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    memset(out_desc, 0, sizeof(*out_desc));
    safe_copy_str(out_desc->task_id, sizeof(out_desc->task_id), t->task_id);
    out_desc->status = t->status;
    out_desc->ttl_ms = t->ttl_ms;
    out_desc->poll_interval_ms = t->poll_interval_ms;
    safe_copy_str(out_desc->status_message, sizeof(out_desc->status_message), t->status_message);
    out_desc->result = t->result;
    out_desc->error = t->error;
    out_desc->input_requests = t->input_requests;
    return MCP_OK;
}

mcp_status_t mcp_task_mgr_cancel(mcp_context_t *ctx, mcp_task_mgr_t *mgr,
                                 const char *task_id) {
    (void)ctx;
    mcp_task_entry_t *t = find_task(mgr, task_id);
    if (t == NULL) {
        return MCP_ERR_NOT_FOUND;
    }
    t->status = MCP_TASK_STATUS_CANCELLED;
    return MCP_OK;
}

mcp_json_value_t *mcp_task_desc_to_json(mcp_context_t *ctx, const mcp_task_desc_t *desc) {
    if (desc == NULL) {
        return NULL;
    }
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        return NULL;
    }

    mcp_json_value_t *v;
    if ((v = mcp_json_string_new(ctx, desc->task_id)) == NULL ||
        mcp_json_object_set_take(ctx, obj, "taskId", v) != MCP_OK) {
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    if ((v = mcp_json_string_new(ctx, mcp_task_status_name(desc->status))) == NULL ||
        mcp_json_object_set_take(ctx, obj, "status", v) != MCP_OK) {
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    if (desc->ttl_ms > 0) {
        if ((v = mcp_json_number_new(ctx, (double)desc->ttl_ms)) == NULL ||
            mcp_json_object_set_take(ctx, obj, "ttlMs", v) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    if (desc->poll_interval_ms > 0) {
        if ((v = mcp_json_number_new(ctx, (double)desc->poll_interval_ms)) == NULL ||
            mcp_json_object_set_take(ctx, obj, "pollIntervalMs", v) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    if (desc->status_message[0] != '\0') {
        if ((v = mcp_json_string_new(ctx, desc->status_message)) == NULL ||
            mcp_json_object_set_take(ctx, obj, "statusMessage", v) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    if (desc->result != NULL) {
        mcp_json_value_t *copy = mcp_json_clone(ctx, desc->result);
        if (copy == NULL || mcp_json_object_set_take(ctx, obj, "result", copy) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    if (desc->error != NULL) {
        mcp_json_value_t *copy = mcp_json_clone(ctx, desc->error);
        if (copy == NULL || mcp_json_object_set_take(ctx, obj, "error", copy) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    if (desc->input_requests != NULL) {
        mcp_json_value_t *copy = mcp_json_clone(ctx, desc->input_requests);
        if (copy == NULL || mcp_json_object_set_take(ctx, obj, "inputRequests", copy) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    return obj;
}
