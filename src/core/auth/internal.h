/**
 * @file internal.h
 * @brief Internal helpers for OAuth and PKCE modules.
 */

#ifndef MCPKIT_CORE_AUTH_INTERNAL_H
#define MCPKIT_CORE_AUTH_INTERNAL_H

#include "mcpkit/core/auth.h"
#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include "mcpkit/json/object.h"
#include "mcpkit/json/array.h"
#include "mcpkit/json/value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline const mcp_allocator_t *alloc_of(mcp_context_t *ctx) {
    return ctx != NULL ? mcp_context_allocator(ctx) : mcp_default_allocator();
}

static inline char *auth_strdup(mcp_context_t *ctx, const char *s) {
    if (s == NULL) return NULL;
    size_t len = strlen(s);
    const mcp_allocator_t *a = alloc_of(ctx);
    char *dup = (char *)a->malloc_fn(len + 1, a->userdata);
    if (dup != NULL) {
        memcpy(dup, s, len + 1);
    }
    return dup;
}

#endif /* MCPKIT_CORE_AUTH_INTERNAL_H */
