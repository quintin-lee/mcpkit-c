#ifndef MCPKIT_PROTOCOL_VALIDATE_H
#define MCPKIT_PROTOCOL_VALIDATE_H

#include <stdbool.h>

#include "mcpkit/core/error.h"
#include "mcpkit/protocol/message.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;

typedef struct mcp_idset mcp_idset_t;

mcp_idset_t *mcp_idset_create(mcp_context_t *ctx);
void mcp_idset_destroy(mcp_context_t *ctx, mcp_idset_t *set);
mcp_status_t mcp_idset_add(mcp_context_t *ctx, mcp_idset_t *set,
                           mcp_id_type_t type, const char *s, double n);
void mcp_idset_remove(mcp_context_t *ctx, mcp_idset_t *set,
                      mcp_id_type_t type, const char *s, double n);
bool mcp_idset_contains(mcp_context_t *ctx, const mcp_idset_t *set,
                        mcp_id_type_t type, const char *s, double n);

bool mcp_method_known(const char *method);
mcp_status_t mcp_validate_envelope(mcp_context_t *ctx, const mcp_message_t *msg,
                                   int *rpc_code_out);
mcp_status_t mcp_validate_method(mcp_context_t *ctx, const mcp_message_t *msg,
                                 int *rpc_code_out);
mcp_status_t mcp_validate_params(mcp_context_t *ctx, const mcp_message_t *msg,
                                 int *rpc_code_out);
mcp_status_t mcp_message_validate(mcp_context_t *ctx, const mcp_message_t *msg,
                                  int *rpc_code_out);

#endif
