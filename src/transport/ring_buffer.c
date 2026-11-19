#include "mcpkit/transport/ring_buffer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#define MCP_DEFAULT_RING_CAPACITY (128)

typedef struct {
    uint64_t id;
    char *data;
    size_t len;
} event_slot_t;

struct mcp_event_ring_buffer {
    event_slot_t *slots;
    size_t capacity;
    size_t head;        /* Next write index */
    size_t tail;        /* Oldest valid item index */
    size_t count;       /* Number of active items */
    uint64_t next_id;   /* Monotonic counter, starts at 1 */
    uint64_t oldest_id; /* Lowest preserved ID in buffer */
    const mcp_allocator_t *alloc;
    mtx_t lock;
};

static void *rb_malloc(const mcp_allocator_t *alloc, size_t size) {
    if (alloc != NULL && alloc->malloc_fn != NULL) {
        return alloc->malloc_fn(size, alloc->userdata);
    }
    return malloc(size);
}

static void rb_free(const mcp_allocator_t *alloc, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    if (alloc != NULL && alloc->free_fn != NULL) {
        alloc->free_fn(ptr, alloc->userdata);
    } else {
        free(ptr);
    }
}

mcp_event_ring_buffer_t *mcp_event_ring_buffer_create(const mcp_allocator_t *alloc,
                                                      size_t capacity) {
    size_t cap = (capacity > 0) ? capacity : MCP_DEFAULT_RING_CAPACITY;

    mcp_event_ring_buffer_t *rb =
        (mcp_event_ring_buffer_t *)rb_malloc(alloc, sizeof(mcp_event_ring_buffer_t));
    if (rb == NULL) {
        return NULL;
    }
    rb->slots = (event_slot_t *)rb_malloc(alloc, cap * sizeof(event_slot_t));
    if (rb->slots == NULL) {
        rb_free(alloc, rb);
        return NULL;
    }
    memset(rb->slots, 0, cap * sizeof(event_slot_t));

    rb->capacity = cap;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->next_id = 1;
    rb->oldest_id = 1;
    rb->alloc = alloc;
    mtx_init(&rb->lock, mtx_plain);

    return rb;
}

void mcp_event_ring_buffer_destroy(mcp_event_ring_buffer_t *rb) {
    if (rb == NULL) {
        return;
    }
    mtx_lock(&rb->lock);
    for (size_t i = 0; i < rb->capacity; i++) {
        if (rb->slots[i].data != NULL) {
            rb_free(rb->alloc, rb->slots[i].data);
            rb->slots[i].data = NULL;
        }
    }
    rb_free(rb->alloc, rb->slots);
    mtx_unlock(&rb->lock);
    mtx_destroy(&rb->lock);
    rb_free(rb->alloc, rb);
}

mcp_status_t mcp_event_ring_buffer_push(mcp_event_ring_buffer_t *rb,
                                        const char *data,
                                        size_t len,
                                        uint64_t *assigned_id) {
    if (rb == NULL || data == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    mtx_lock(&rb->lock);
    uint64_t current_id = rb->next_id++;

    char id_line[48];
    int id_len = snprintf(id_line, sizeof(id_line), "id: %" PRIu64 "\n", current_id);

    char *framed = NULL;
    size_t framed_len = 0;

    /* Format framing */
    if (strncmp(data, "id:", 3) == 0) {
        /* Already has id line */
        framed_len = len;
        framed = (char *)rb_malloc(rb->alloc, framed_len + 1);
        if (framed != NULL) {
            memcpy(framed, data, len);
            framed[framed_len] = '\0';
        }
    } else if (strncmp(data, "event:", 6) == 0 || strncmp(data, "data:", 5) == 0) {
        /* Pre-framed event/data line, prepend id: */
        framed_len = (size_t)id_len + len;
        framed = (char *)rb_malloc(rb->alloc, framed_len + 1);
        if (framed != NULL) {
            memcpy(framed, id_line, (size_t)id_len);
            memcpy(framed + id_len, data, len);
            framed[framed_len] = '\0';
        }
    } else {
        /* Raw data payload: "id: <id>\ndata: <data>\n\n" */
        const char *data_pfx = "data: ";
        const char *data_sfx = "\n\n";
        size_t pfx_len = strlen(data_pfx);
        size_t sfx_len = strlen(data_sfx);

        framed_len = (size_t)id_len + pfx_len + len + sfx_len;
        framed = (char *)rb_malloc(rb->alloc, framed_len + 1);
        if (framed != NULL) {
            char *p = framed;
            memcpy(p, id_line, (size_t)id_len);
            p += id_len;
            memcpy(p, data_pfx, pfx_len);
            p += pfx_len;
            memcpy(p, data, len);
            p += len;
            memcpy(p, data_sfx, sfx_len);
            p += sfx_len;
            *p = '\0';
        }
    }

    if (framed == NULL) {
        mtx_unlock(&rb->lock);
        return MCP_ERR_NOMEM;
    }

    /* Slot placement */
    if (rb->count == rb->capacity) {
        /* Evict oldest slot at tail */
        rb_free(rb->alloc, rb->slots[rb->tail].data);
        rb->slots[rb->tail].data = NULL;
        rb->tail = (rb->tail + 1) % rb->capacity;
        rb->oldest_id = rb->slots[rb->tail].id;
    } else {
        rb->count++;
    }

    event_slot_t *slot = &rb->slots[rb->head];
    slot->id = current_id;
    slot->data = framed;
    slot->len = framed_len;

    rb->head = (rb->head + 1) % rb->capacity;

    if (assigned_id != NULL) {
        *assigned_id = current_id;
    }

    mtx_unlock(&rb->lock);
    return MCP_OK;
}

mcp_status_t mcp_event_ring_buffer_replay(mcp_event_ring_buffer_t *rb,
                                          uint64_t last_event_id,
                                          mcp_event_replay_fn replay_fn,
                                          void *userdata) {
    if (rb == NULL || replay_fn == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }

    mtx_lock(&rb->lock);

    if (rb->count == 0) {
        if (last_event_id + 1 < rb->oldest_id) {
            mtx_unlock(&rb->lock);
            return MCP_ERR_EXPIRED;
        }
        mtx_unlock(&rb->lock);
        return MCP_OK;
    }

    /* If client's last seen ID is strictly less than (oldest_id - 1),
     * the event (last_event_id + 1) was evicted. Return MCP_ERR_EXPIRED. */
    if (last_event_id + 1 < rb->oldest_id) {
        mtx_unlock(&rb->lock);
        return MCP_ERR_EXPIRED;
    }

    /* Client is already up to date */
    if (last_event_id >= rb->next_id - 1) {
        mtx_unlock(&rb->lock);
        return MCP_OK;
    }

    /* Replay in FIFO order */
    for (size_t i = 0; i < rb->count; i++) {
        size_t idx = (rb->tail + i) % rb->capacity;
        event_slot_t *slot = &rb->slots[idx];
        if (slot->id > last_event_id) {
            replay_fn(slot->id, slot->data, slot->len, userdata);
        }
    }

    mtx_unlock(&rb->lock);
    return MCP_OK;
}

uint64_t mcp_event_ring_buffer_oldest_id(mcp_event_ring_buffer_t *rb) {
    if (rb == NULL) {
        return 0;
    }
    mtx_lock(&rb->lock);
    uint64_t val = rb->oldest_id;
    mtx_unlock(&rb->lock);
    return val;
}

uint64_t mcp_event_ring_buffer_latest_id(mcp_event_ring_buffer_t *rb) {
    if (rb == NULL) {
        return 0;
    }
    mtx_lock(&rb->lock);
    uint64_t val = (rb->count > 0) ? (rb->next_id - 1) : 0;
    mtx_unlock(&rb->lock);
    return val;
}

size_t mcp_event_ring_buffer_count(mcp_event_ring_buffer_t *rb) {
    if (rb == NULL) {
        return 0;
    }
    mtx_lock(&rb->lock);
    size_t val = rb->count;
    mtx_unlock(&rb->lock);
    return val;
}
