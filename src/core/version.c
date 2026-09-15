#include "mcpkit/core/version.h"

#include "mcpkit_version_generated.h"

#include <assert.h>

static_assert(MCPKIT_VERSION_MAJOR == 0, "version major must start at 0");

const char *mcpkit_version_string(void) {
    return MCPKIT_VERSION;
}
