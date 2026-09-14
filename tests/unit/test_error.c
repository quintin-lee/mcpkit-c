#include <assert.h>
#include <string.h>

#include "mcpkit/core/error.h"

int main(void) {
    assert(strcmp(mcp_status_string(MCP_OK), "MCP_OK") == 0);
    assert(strcmp(mcp_status_string(MCP_ERR_NOMEM), "MCP_ERR_NOMEM") == 0);
    assert(strcmp(mcp_status_string((mcp_status_t)999), "MCP_ERR_UNKNOWN") == 0);
    return 0;
}
