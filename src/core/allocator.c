// Libc-backed default allocator. Each wrapper ignores `userdata`
// because the standard functions take no such parameter; the slot
// is kept so the struct shape matches user-supplied allocators.
#include "mcpkit/core/types.h"

#include <stdlib.h>

static void *libc_malloc(size_t size, void *userdata) {
    (void)userdata;
    return malloc(size);
}

static void libc_free(void *ptr, void *userdata) {
    (void)userdata;
    free(ptr);
}

static void *libc_calloc(size_t nmemb, size_t size, void *userdata) {
    (void)userdata;
    return calloc(nmemb, size);
}

static void *libc_realloc(void *ptr, size_t size, void *userdata) {
    (void)userdata;
    return realloc(ptr, size);
}

static const mcp_allocator_t k_default = {
    .malloc_fn = libc_malloc,
    .free_fn = libc_free,
    .calloc_fn = libc_calloc,
    .realloc_fn = libc_realloc,
    .userdata = NULL,
};

const mcp_allocator_t *mcp_default_allocator(void) {
    return &k_default;
}
