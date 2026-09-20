#include "test_check.h"
#include <stdlib.h>

#include "mcpkit/core/context.h"
#include "mcpkit/core/capability.h"

static int g_allocs;
static void *counting_malloc(size_t n, void *ud) {
    (void)ud;
    g_allocs++;
    return malloc(n);
}
static void counting_free(void *p, void *ud) {
    (void)ud;
    free(p);
}

int main(void) {
    mcp_capabilities_t caps = MCP_CAPABILITIES_INIT;
    CHECK(!caps.tools && !caps.resources && !caps.prompts);
    mcp_context_t *c = mcp_context_create(NULL);
    CHECK(c != NULL);
    CHECK(mcp_context_allocator(c) != NULL);
    CHECK(mcp_context_logger(c) != NULL);
    CHECK(mcp_context_json_backend(c) == NULL);
    mcp_logger_t *borrowed = mcp_logger_default_stderr(NULL);
    CHECK(borrowed != NULL);
    static const mcp_allocator_t counting = {
        .malloc_fn = counting_malloc,
        .free_fn = counting_free,
        .calloc_fn = NULL,
        .realloc_fn = NULL,
        .userdata = NULL,
    };
    int before = g_allocs;
    mcp_context_config_t cfg = {.allocator = &counting, .logger = borrowed, .json_backend = NULL};
    mcp_context_t *c2 = mcp_context_create(&cfg);
    CHECK(c2 != NULL);
    CHECK(mcp_context_logger(c2) == borrowed);
    CHECK(mcp_context_allocator(c2)->malloc_fn == counting_malloc);
    CHECK(g_allocs > before);
    mcp_context_destroy(c2);
    mcp_logger_destroy(borrowed);
    mcp_context_destroy(c);
    CHECK(mcp_context_allocator(NULL) == mcp_default_allocator());
    CHECK(mcp_context_logger(NULL) == NULL);
    CHECK(mcp_context_json_backend(NULL) == NULL);
    mcp_context_destroy(NULL);
    return 0;
}
