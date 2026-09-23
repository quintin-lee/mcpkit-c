/**
 * @file skills.c
 * @brief MCP Skills extension implementation (SEP-2640).
 * @ingroup mcpkit-protocol
 */

#include "mcpkit/protocol/skills.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct mcp_skill_registry {
    mcp_skill_t *skills;
    size_t count;
    size_t capacity;
};

static void *skill_malloc(mcp_context_t *ctx, size_t sz) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->malloc_fn(sz, a->userdata);
}

static void *skill_realloc(mcp_context_t *ctx, void *ptr, size_t sz) {
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    return a->realloc_fn(ptr, sz, a->userdata);
}

static void skill_free(mcp_context_t *ctx, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    const mcp_allocator_t *a = mcp_context_allocator(ctx);
    a->free_fn(ptr, a->userdata);
}

static char *skill_strdup(mcp_context_t *ctx, const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *out = skill_malloc(ctx, n);
    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

mcp_skill_registry_t *mcp_skill_registry_new(mcp_context_t *ctx) {
    mcp_skill_registry_t *reg = skill_malloc(ctx, sizeof(*reg));
    if (reg == NULL) {
        return NULL;
    }
    memset(reg, 0, sizeof(*reg));
    return reg;
}

static void free_skill_contents(mcp_context_t *ctx, mcp_skill_t *sk) {
    skill_free(ctx, sk->name);
    skill_free(ctx, sk->description);
    skill_free(ctx, sk->uri);
    if (sk->frontmatter != NULL) {
        mcp_json_destroy(ctx, sk->frontmatter);
        sk->frontmatter = NULL;
    }
    for (size_t i = 0; i < sk->n_resources; i++) {
        skill_free(ctx, sk->resources[i].uri);
        skill_free(ctx, sk->resources[i].digest);
    }
    skill_free(ctx, sk->resources);
}

void mcp_skill_registry_free(mcp_context_t *ctx, mcp_skill_registry_t *reg) {
    if (reg == NULL) {
        return;
    }
    for (size_t i = 0; i < reg->count; i++) {
        free_skill_contents(ctx, &reg->skills[i]);
    }
    skill_free(ctx, reg->skills);
    skill_free(ctx, reg);
}

mcp_status_t mcp_skill_registry_add(mcp_context_t *ctx, mcp_skill_registry_t *reg,
                                    const char *name, const char *description,
                                    const char *uri, mcp_json_value_t *frontmatter) {
    if (reg == NULL || name == NULL || name[0] == '\0') {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (mcp_skill_registry_find(reg, name) != NULL) {
        return MCP_ERR_ALREADY_EXISTS;
    }

    if (reg->count == reg->capacity) {
        size_t new_cap = reg->capacity == 0 ? 8 : reg->capacity * 2;
        mcp_skill_t *new_arr = skill_realloc(ctx, reg->skills, new_cap * sizeof(*new_arr));
        if (new_arr == NULL) {
            return MCP_ERR_NOMEM;
        }
        reg->skills = new_arr;
        reg->capacity = new_cap;
    }

    mcp_skill_t *sk = &reg->skills[reg->count];
    memset(sk, 0, sizeof(*sk));

    sk->name = skill_strdup(ctx, name);
    sk->description = skill_strdup(ctx, description != NULL ? description : "");
    if (uri != NULL && uri[0] != '\0') {
        sk->uri = skill_strdup(ctx, uri);
    } else {
        char auto_uri[256];
        snprintf(auto_uri, sizeof(auto_uri), "skill://%s/SKILL.md", name);
        sk->uri = skill_strdup(ctx, auto_uri);
    }
    sk->frontmatter = frontmatter;

    if (sk->name == NULL || sk->description == NULL || sk->uri == NULL) {
        free_skill_contents(ctx, sk);
        return MCP_ERR_NOMEM;
    }

    reg->count++;
    return MCP_OK;
}

mcp_status_t mcp_skill_add_resource(mcp_context_t *ctx, mcp_skill_registry_t *reg,
                                    const char *name_or_uri, const char *res_uri,
                                    const char *digest, size_t size) {
    if (reg == NULL || name_or_uri == NULL || res_uri == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    mcp_skill_t *sk = (mcp_skill_t *)mcp_skill_registry_find(reg, name_or_uri);
    if (sk == NULL) {
        return MCP_ERR_NOT_FOUND;
    }

    if (sk->n_resources == sk->cap_resources) {
        size_t new_cap = sk->cap_resources == 0 ? 4 : sk->cap_resources * 2;
        mcp_skill_resource_t *new_arr = skill_realloc(ctx, sk->resources, new_cap * sizeof(*new_arr));
        if (new_arr == NULL) {
            return MCP_ERR_NOMEM;
        }
        sk->resources = new_arr;
        sk->cap_resources = new_cap;
    }

    mcp_skill_resource_t *r = &sk->resources[sk->n_resources];
    memset(r, 0, sizeof(*r));
    r->uri = skill_strdup(ctx, res_uri);
    r->digest = skill_strdup(ctx, digest != NULL ? digest : "");
    r->size = size;

    if (r->uri == NULL || r->digest == NULL) {
        skill_free(ctx, r->uri);
        skill_free(ctx, r->digest);
        return MCP_ERR_NOMEM;
    }

    sk->n_resources++;
    return MCP_OK;
}

const mcp_skill_t *mcp_skill_registry_find(const mcp_skill_registry_t *reg,
                                          const char *name_or_uri) {
    if (reg == NULL || name_or_uri == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->skills[i].name, name_or_uri) == 0 ||
            (reg->skills[i].uri != NULL && strcmp(reg->skills[i].uri, name_or_uri) == 0)) {
            return &reg->skills[i];
        }
    }
    return NULL;
}

size_t mcp_skill_registry_count(const mcp_skill_registry_t *reg) {
    return reg != NULL ? reg->count : 0;
}

const mcp_skill_t *mcp_skill_registry_get_at(const mcp_skill_registry_t *reg, size_t index) {
    if (reg == NULL || index >= reg->count) {
        return NULL;
    }
    return &reg->skills[index];
}

mcp_json_value_t *mcp_skill_to_json(mcp_context_t *ctx, const mcp_skill_t *skill) {
    if (skill == NULL) {
        return NULL;
    }
    mcp_json_value_t *obj = mcp_json_object_new(ctx);
    if (obj == NULL) {
        return NULL;
    }

    mcp_json_value_t *v;
    if ((v = mcp_json_string_new(ctx, skill->uri)) == NULL ||
        mcp_json_object_set_take(ctx, obj, "uri", v) != MCP_OK) {
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    if (skill->frontmatter != NULL) {
        mcp_json_value_t *copy = mcp_json_clone(ctx, skill->frontmatter);
        if (copy == NULL || mcp_json_object_set_take(ctx, obj, "frontmatter", copy) != MCP_OK) {
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    } else {
        mcp_json_value_t *fm = mcp_json_object_new(ctx);
        if (fm != NULL) {
            mcp_json_object_set_take(ctx, fm, "name", mcp_json_string_new(ctx, skill->name));
            mcp_json_object_set_take(ctx, fm, "description", mcp_json_string_new(ctx, skill->description));
            mcp_json_object_set_take(ctx, obj, "frontmatter", fm);
        }
    }

    mcp_json_value_t *res_arr = mcp_json_array_new(ctx);
    if (res_arr == NULL) {
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    for (size_t i = 0; i < skill->n_resources; i++) {
        const mcp_skill_resource_t *r = &skill->resources[i];
        mcp_json_value_t *item = mcp_json_object_new(ctx);
        if (item == NULL) {
            mcp_json_destroy(ctx, res_arr);
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
        mcp_json_object_set_take(ctx, item, "uri", mcp_json_string_new(ctx, r->uri));
        mcp_json_object_set_take(ctx, item, "digest", mcp_json_string_new(ctx, r->digest));
        mcp_json_object_set_take(ctx, item, "size", mcp_json_number_new(ctx, (double)r->size));
        if (mcp_json_array_append(ctx, res_arr, item) != MCP_OK) {
            mcp_json_destroy(ctx, item);
            mcp_json_destroy(ctx, res_arr);
            mcp_json_destroy(ctx, obj);
            return NULL;
        }
    }

    if (mcp_json_object_set_take(ctx, obj, "resources", res_arr) != MCP_OK) {
        mcp_json_destroy(ctx, res_arr);
        mcp_json_destroy(ctx, obj);
        return NULL;
    }

    return obj;
}
