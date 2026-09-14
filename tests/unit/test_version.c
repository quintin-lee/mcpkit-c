#include <assert.h>
#include <string.h>

#include "mcpkit/core/version.h"

int main(void) {
    assert(strcmp(mcpkit_version_string(), "0.1.0") == 0);
    return 0;
}
