#include "test_check.h"
#include <string.h>

#include "mcpkit/core/error.h"

int main(void) {
    CHECK(strcmp(mcp_status_string(MCP_OK), "MCP_OK") == 0);
    CHECK(strcmp(mcp_status_string(MCP_ERR_NOMEM), "MCP_ERR_NOMEM") == 0);
    CHECK(strcmp(mcp_status_string((mcp_status_t)999), "MCP_ERR_UNKNOWN") == 0);
    return 0;
}
