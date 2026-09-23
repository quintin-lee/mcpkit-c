/**
 * @file skills.h
 * @brief MCP Skills extension data structures and registry (SEP-2640).
 * @ingroup mcpkit-protocol
 */

#ifndef MCPKIT_PROTOCOL_SKILLS_H
#define MCPKIT_PROTOCOL_SKILLS_H

#include "mcpkit/core/context.h"
#include "mcpkit/core/types.h"
#include "mcpkit/json/json.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief File resource entry within a skill directory manifest.
 */
typedef struct mcp_skill_resource {
    char *uri;
    char *digest;
    size_t size;
} mcp_skill_resource_t;

/**
 * @brief Represents an Agent Skill descriptor.
 */
typedef struct mcp_skill {
    char *name;
    char *description;
    char *uri;                     /* e.g. "skill://<name>/SKILL.md" */
    mcp_json_value_t *frontmatter; /* owned JSON object */
    mcp_skill_resource_t *resources;
    size_t n_resources;
    size_t cap_resources;
} mcp_skill_t;

/**
 * @brief Opaque registry for MCP Skills.
 */
typedef struct mcp_skill_registry mcp_skill_registry_t;

/**
 * @brief Creates a new skill registry.
 */
mcp_skill_registry_t *mcp_skill_registry_new(mcp_context_t *ctx);

/**
 * @brief Destroys a skill registry and all stored skills.
 */
void mcp_skill_registry_free(mcp_context_t *ctx, mcp_skill_registry_t *reg);

/**
 * @brief Adds a new skill to the registry.
 *
 * @param ctx          Context.
 * @param reg          Skill registry.
 * @param name         Skill name (unique identifier, e.g. "code-review").
 * @param description  Skill human-readable description.
 * @param uri          Resource URI for SKILL.md (e.g. "skill://code-review/SKILL.md"), or NULL to auto-generate.
 * @param frontmatter  Caller-owned JSON object containing YAML frontmatter; ownership transfers on MCP_OK.
 * @return MCP_OK on success; MCP_ERR_ALREADY_EXISTS if name already registered;
 *         MCP_ERR_NOMEM on memory allocation failure.
 */
mcp_status_t mcp_skill_registry_add(mcp_context_t *ctx, mcp_skill_registry_t *reg,
                                    const char *name, const char *description,
                                    const char *uri, mcp_json_value_t *frontmatter);

/**
 * @brief Adds a resource file entry to a registered skill's manifest.
 */
mcp_status_t mcp_skill_add_resource(mcp_context_t *ctx, mcp_skill_registry_t *reg,
                                    const char *name_or_uri, const char *res_uri,
                                    const char *digest, size_t size);

/**
 * @brief Looks up a skill by name or by its primary URI.
 *
 * @return Borrowed pointer to the skill, or NULL if not found.
 */
const mcp_skill_t *mcp_skill_registry_find(const mcp_skill_registry_t *reg,
                                          const char *name_or_uri);

/**
 * @brief Returns total number of registered skills.
 */
size_t mcp_skill_registry_count(const mcp_skill_registry_t *reg);

/**
 * @brief Returns skill at index for enumeration and pagination.
 */
const mcp_skill_t *mcp_skill_registry_get_at(const mcp_skill_registry_t *reg, size_t index);

/**
 * @brief Serializes a skill descriptor to JSON per SEP-2640 specification.
 */
mcp_json_value_t *mcp_skill_to_json(mcp_context_t *ctx, const mcp_skill_t *skill);

#ifdef __cplusplus
}
#endif

#endif /* MCPKIT_PROTOCOL_SKILLS_H */
