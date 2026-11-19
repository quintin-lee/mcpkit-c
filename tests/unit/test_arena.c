#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcpkit/core/arena.h"
#include "mcpkit/core/context.h"

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL: %s at %s:%d\n", #cond, __FILE__, __LINE__);                     \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void test_arena_basic_and_alignment(void) {
    mcp_arena_t *arena = mcp_arena_create(NULL, 1024);
    ASSERT_TRUE(arena != NULL);

    void *p1 = mcp_arena_alloc(arena, 17);
    ASSERT_TRUE(p1 != NULL);
    ASSERT_TRUE(((uintptr_t)p1 % sizeof(void *)) == 0);

    void *p2 = mcp_arena_alloc_aligned(arena, 32, 64);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_TRUE(((uintptr_t)p2 % 64) == 0);

    void *p3 = mcp_arena_alloc_aligned(arena, 128, 16);
    ASSERT_TRUE(p3 != NULL);
    ASSERT_TRUE(((uintptr_t)p3 % 16) == 0);

    mcp_arena_destroy(arena);
}

static void test_arena_strdup(void) {
    mcp_arena_t *arena = mcp_arena_create(NULL, 1024);
    ASSERT_TRUE(arena != NULL);

    const char *orig = "hello world from arena allocator";
    char *dup = mcp_arena_strdup(arena, orig);
    ASSERT_TRUE(dup != NULL);
    ASSERT_TRUE(dup != orig);
    ASSERT_TRUE(strcmp(orig, dup) == 0);

    mcp_arena_destroy(arena);
}

static void test_arena_realloc(void) {
    mcp_arena_t *arena = mcp_arena_create(NULL, 1024);
    ASSERT_TRUE(arena != NULL);

    /* 1. NULL ptr acts like alloc */
    char *p1 = (char *)mcp_arena_realloc(arena, NULL, 64);
    ASSERT_TRUE(p1 != NULL);
    strcpy(p1, "first allocation");

    /* 2. Top-of-bump realloc expands in-place if capacity permits */
    char *p1_grown = (char *)mcp_arena_realloc(arena, p1, 128);
    ASSERT_TRUE(p1_grown == p1); /* In-place expansion */
    ASSERT_TRUE(strcmp(p1_grown, "first allocation") == 0);

    /* 3. Non-top realloc relocates and copies */
    char *p2 = (char *)mcp_arena_alloc(arena, 64);
    ASSERT_TRUE(p2 != NULL);
    strcpy(p2, "second allocation");

    /* Now p1 is no longer at top-of-bump */
    char *p1_relocated = (char *)mcp_arena_realloc(arena, p1, 256);
    ASSERT_TRUE(p1_relocated != NULL);
    ASSERT_TRUE(p1_relocated != p1);
    ASSERT_TRUE(strcmp(p1_relocated, "first allocation") == 0);

    /* 4. realloc to smaller size */
    char *p1_shrunk = (char *)mcp_arena_realloc(arena, p1_relocated, 32);
    ASSERT_TRUE(p1_shrunk != NULL);
    ASSERT_TRUE(strncmp(p1_shrunk, "first allocation", 16) == 0);

    mcp_arena_destroy(arena);
}

static void test_arena_multi_chunk_expansion(void) {
    /* Use a tiny 256-byte chunk size */
    mcp_arena_t *arena = mcp_arena_create(NULL, 256);
    ASSERT_TRUE(arena != NULL);

    /* Allocate more than 256 bytes total in small blocks */
    for (int i = 0; i < 20; i++) {
        void *p = mcp_arena_alloc(arena, 64);
        ASSERT_TRUE(p != NULL);
        memset(p, 0xAB, 64);
    }

    /* Allocate an oversized block larger than default chunk size */
    void *big = mcp_arena_alloc(arena, 1024);
    ASSERT_TRUE(big != NULL);
    memset(big, 0xCD, 1024);

    /* Subsequent normal allocation succeeds */
    void *next = mcp_arena_alloc(arena, 32);
    ASSERT_TRUE(next != NULL);

    mcp_arena_destroy(arena);
}

static void test_arena_reset_and_reuse(void) {
    mcp_arena_t *arena = mcp_arena_create(NULL, 512);
    ASSERT_TRUE(arena != NULL);

    void *first_pass_ptrs[10];
    for (int i = 0; i < 10; i++) {
        first_pass_ptrs[i] = mcp_arena_alloc(arena, 64);
        ASSERT_TRUE(first_pass_ptrs[i] != NULL);
    }

    /* Reset should rewind offsets without releasing chunks */
    mcp_arena_reset(arena);

    void *second_pass_ptrs[10];
    for (int i = 0; i < 10; i++) {
        second_pass_ptrs[i] = mcp_arena_alloc(arena, 64);
        ASSERT_TRUE(second_pass_ptrs[i] != NULL);
    }

    /* The first allocation after reset should reoccupy the start of the first chunk */
    ASSERT_TRUE(first_pass_ptrs[0] == second_pass_ptrs[0]);

    mcp_arena_destroy(arena);
}

static void test_arena_as_allocator_bridge(void) {
    mcp_arena_t *arena = mcp_arena_create(NULL, 1024);
    ASSERT_TRUE(arena != NULL);

    mcp_allocator_t alloc = mcp_arena_as_allocator(arena);
    ASSERT_TRUE(alloc.malloc_fn != NULL);
    ASSERT_TRUE(alloc.realloc_fn != NULL);
    ASSERT_TRUE(alloc.free_fn != NULL);
    ASSERT_TRUE(alloc.calloc_fn != NULL);
    ASSERT_TRUE(alloc.userdata == arena);

    void *p = alloc.malloc_fn(64, alloc.userdata);
    ASSERT_TRUE(p != NULL);
    memset(p, 0x55, 64);

    void *p_re = alloc.realloc_fn(p, 128, alloc.userdata);
    ASSERT_TRUE(p_re != NULL);

    void *c = alloc.calloc_fn(4, 16, alloc.userdata);
    ASSERT_TRUE(c != NULL);
    for (size_t i = 0; i < 64; i++) {
        ASSERT_TRUE(((char *)c)[i] == 0);
    }

    /* free_fn must be a safe no-op */
    alloc.free_fn(p_re, alloc.userdata);
    alloc.free_fn(c, alloc.userdata);

    /* Use with mcp_context */
    mcp_context_config_t cfg = {
        .allocator = &alloc,
        .logger = NULL,
        .json_backend = NULL
    };
    mcp_context_t *ctx = mcp_context_create(&cfg);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_TRUE(mcp_context_allocator(ctx)->userdata == arena);
    ASSERT_TRUE(mcp_context_allocator(ctx)->malloc_fn == alloc.malloc_fn);

    mcp_context_destroy(ctx);
    mcp_arena_destroy(arena);
}

int main(void) {
    test_arena_basic_and_alignment();
    test_arena_strdup();
    test_arena_realloc();
    test_arena_multi_chunk_expansion();
    test_arena_reset_and_reuse();
    test_arena_as_allocator_bridge();
    printf("All test_arena tests passed!\n");
    return 0;
}
