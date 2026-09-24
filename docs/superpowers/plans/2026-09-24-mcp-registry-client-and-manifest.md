# 路线 B：`mcpkit-cli` 对接官方 MCP Registry (MCP Registry Client & Manifest Tooling) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide full MCP Registry manifest serialization, validation, and CLI commands (`registry search`, `registry info`, `manifest init`, `manifest validate`) in `mcpkit-c` to enable C developers to discover, configure, and publish MCP servers to the official MCP Registry.

**Architecture:** Create `include/mcpkit/protocol/registry.h` and `src/protocol/registry.c` implementing the official MCP Registry Server Manifest schema (`mcp.json`) parser, serializer, and validator. Extend `tools/mcpkit-cli/main.c` with `registry` and `manifest` command suites for searching packages and validating server metadata against official specifications.

**Tech Stack:** C99, POSIX, CMake, CTest, ASan/UBSan.

---

### File Structure Map

- `include/mcpkit/protocol/registry.h`: C data structures for MCP server registry manifests (`mcp.json`) and search query models.
- `src/protocol/registry.c`: JSON parsing, serialization, and validation against the official Registry schema.
- `tools/mcpkit-cli/main.c`: Add `registry search`, `registry info`, `manifest init`, and `manifest validate` subcommands.
- `tests/unit/test_registry_manifest.c`: Comprehensive unit tests for manifest creation, validation, and search result parsing.
- `tests/CMakeLists.txt`: Register `test_registry_manifest`.
- `tests/acceptance/cli_accept.sh`: CLI acceptance tests for manifest generation and validation.

---

### Task B1: Server Manifest (`mcp.json`) Parser, Serializer, and Validator

**Files:**
- Create: `include/mcpkit/protocol/registry.h`
- Create: `src/protocol/registry.c`
- Create: `tests/unit/test_registry_manifest.c`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [x] **Step 1: Write failing unit test in `tests/unit/test_registry_manifest.c`**

```c
#include <stdio.h>
#include <string.h>
#include "mcpkit/core/context.h"
#include "mcpkit/protocol/registry.h"
#include "test_check.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // 1. Parse valid mcp.json manifest
    const char *manifest_json =
        "{\"name\":\"mcp-echo-c\",\"version\":\"1.0.0\",\"description\":\"High performance C echo server\","
        "\"transport\":\"stdio\",\"repository\":\"https://github.com/example/mcp-echo-c\","
        "\"capabilities\":{\"tools\":true,\"resources\":false}}";

    mcp_registry_manifest_t m;
    memset(&m, 0, sizeof(m));
    CHECK(mcp_registry_manifest_parse(ctx, manifest_json, strlen(manifest_json), &m) == MCP_OK);
    CHECK(m.name != NULL && strcmp(m.name, "mcp-echo-c") == 0);
    CHECK(m.version != NULL && strcmp(m.version, "1.0.0") == 0);
    CHECK(m.transport == MCP_TRANSPORT_KIND_STDIO);
    CHECK(m.has_tools == true);
    CHECK(m.has_resources == false);

    // 2. Validate manifest rules
    CHECK(mcp_registry_manifest_validate(ctx, &m, NULL) == MCP_OK);

    // 3. Serialize back and check content
    char *out_json = mcp_registry_manifest_serialize(ctx, &m);
    CHECK(out_json != NULL);
    CHECK(strstr(out_json, "\"name\":\"mcp-echo-c\"") != NULL);
    CHECK(strstr(out_json, "\"transport\":\"stdio\"") != NULL);
    mcp_context_free(ctx, out_json);

    mcp_registry_manifest_cleanup(ctx, &m);

    // 4. Negative validation: missing required version
    const char *bad_json = "{\"name\":\"bad-server\"}";
    memset(&m, 0, sizeof(m));
    CHECK(mcp_registry_manifest_parse(ctx, bad_json, strlen(bad_json), &m) == MCP_OK);
    char err_buf[128] = {0};
    CHECK(mcp_registry_manifest_validate(ctx, &m, err_buf) != MCP_OK);
    CHECK(strstr(err_buf, "version") != NULL);
    mcp_registry_manifest_cleanup(ctx, &m);

    mcp_context_destroy(ctx);
    printf("test_registry_manifest OK\n");
    return 0;
}
```

- [x] **Step 2: Add test target to `tests/CMakeLists.txt` and verify compile failure**

```cmake
add_executable(test_registry_manifest unit/test_registry_manifest.c)
target_link_libraries(test_registry_manifest PRIVATE mcpkit_core)
add_test(NAME test_registry_manifest COMMAND test_registry_manifest)
```

- [x] **Step 3: Define structs and APIs in `include/mcpkit/protocol/registry.h`**

```c
typedef enum {
    MCP_TRANSPORT_KIND_UNKNOWN = 0,
    MCP_TRANSPORT_KIND_STDIO,
    MCP_TRANSPORT_KIND_STREAMABLE_HTTP,
    MCP_TRANSPORT_KIND_SOCKET
} mcp_registry_transport_kind_t;

typedef struct {
    char *name;
    char *version;
    char *description;
    char *author;
    char *license;
    char *repository;
    mcp_registry_transport_kind_t transport;
    bool has_tools;
    bool has_resources;
    bool has_prompts;
} mcp_registry_manifest_t;

mcp_status_t mcp_registry_manifest_parse(mcp_context_t *ctx, const char *json_str,
                                         size_t len, mcp_registry_manifest_t *m_out);
mcp_status_t mcp_registry_manifest_validate(mcp_context_t *ctx, const mcp_registry_manifest_t *m,
                                            char *err_msg_out);
char *mcp_registry_manifest_serialize(mcp_context_t *ctx, const mcp_registry_manifest_t *m);
void mcp_registry_manifest_cleanup(mcp_context_t *ctx, mcp_registry_manifest_t *m);
```

- [x] **Step 4: Implement in `src/protocol/registry.c` and add to `src/CMakeLists.txt`**

Implement JSON mapping, schema required fields validation, and clean memory cleanup.

- [x] **Step 5: Run tests and verify PASS**

Run: `cmake --build build --target test_registry_manifest && ./build/tests/test_registry_manifest`
Expected: `test_registry_manifest OK`

- [x] **Step 6: Commit**

```bash
git add include/mcpkit/protocol/registry.h src/protocol/registry.c src/CMakeLists.txt tests/unit/test_registry_manifest.c tests/CMakeLists.txt
git commit -m "feat(registry): add mcp.json server manifest parser, validator, and serializer"
```

---

### Task B2: Implement CLI `manifest` and `registry` Subcommands in `mcpkit-cli`

**Files:**
- Modify: `tools/mcpkit-cli/main.c`
- Modify: `tests/acceptance/cli_accept.sh`

- [x] **Step 1: Implement `cmd_manifest_init` and `cmd_manifest_validate` in `tools/mcpkit-cli/main.c`**

- `mcpkit-cli manifest init [name]`: Scaffolds a template `mcp.json` file with standard transport, schema versions, and tool declarations.
- `mcpkit-cli manifest validate <path/to/mcp.json>`: Reads the file, executes `mcp_registry_manifest_validate`, and outputs OK or formatted error details.

- [x] **Step 2: Implement `cmd_registry_search` and `cmd_registry_info`**

- `mcpkit-cli registry search <query>`: Queries the MCP registry endpoint (or local registry index) and outputs matching servers with descriptions.
- `mcpkit-cli registry info <server-id>`: Displays detailed package metadata, installation command, and capability flags.

- [x] **Step 3: Update `main()` argument parsing and usage message**

Update usage:
```text
       mcpkit-cli manifest init [server-name]
       mcpkit-cli manifest validate <file>
       mcpkit-cli registry search <query>
       mcpkit-cli registry info <server-id>
```

- [x] **Step 4: Add acceptance test cases in `tests/acceptance/cli_accept.sh`**

Test running `manifest init tmp_mcp.json`, then validating it with `manifest validate tmp_mcp.json`, and cleaning up.

- [x] **Step 5: Verify build & tests**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 100% tests PASS.

- [x] **Step 6: Commit**

```bash
git add tools/mcpkit-cli/main.c tests/acceptance/cli_accept.sh
git commit -m "feat(cli): add manifest and registry subcommands to mcpkit-cli"
```
