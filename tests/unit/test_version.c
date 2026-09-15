#include <assert.h>
#include <string.h>

#include "mcpkit/core/version.h"
#include "mcpkit_version_generated.h"

int main(void) {
    assert(strcmp(mcpkit_version_string(), MCPKIT_VERSION) == 0);
    return 0;
}
