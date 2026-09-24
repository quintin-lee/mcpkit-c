# 路线 C：Sampling with Tools (SEP-1577) 规范实现 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the SEP-1577 / MCP 2026-07-28 "Sampling with Tools" extension, allowing MCP servers to pass tool definitions and tool choices into `sampling/createMessage` and process `tool_use` / `tool_result` content blocks in the host LLM response.

**Architecture:** Extend protocol sampling structures in `include/mcpkit/protocol/sampling.h` and `src/protocol/sampling.c`. Add builders for tool definitions (`mcp_sampling_tool_def_t`), tool choice modes (`auto`, `none`, `required`, or specific tool), and message content blocks (`tool_use`, `tool_result`). Extend client-side sampling handler dispatching in `client.c` and server-side request helpers in `server.c`.

**Tech Stack:** C99, CMake, CTest, ASan/UBSan.

---

### File Structure Map

- `include/mcpkit/protocol/sampling.h`: Add tool definitions, choice modes, and `tool_use`/`tool_result` content constructors.
- `src/protocol/sampling.c`: Implement JSON serialization/parsing for tools and content blocks.
- `include/mcpkit/client/client.h`: Update client sampling provider signature to receive tools and choices.
- `src/client/client.c`: Route incoming `sampling/createMessage` requests carrying tools to the host provider.
- `tests/unit/test_sampling_tools.c`: Unit tests verifying serialization, parsing, and round-trip tool sampling.
- `tests/CMakeLists.txt`: Register `test_sampling_tools`.

---

### Task C1: Protocol Data Structures and Serialization for Sampling Tools

**Files:**
- Create: `include/mcpkit/protocol/sampling.h`
- Create: `src/protocol/sampling.c`
- Create: `tests/unit/test_sampling_tools.c`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write failing unit test in `tests/unit/test_sampling_tools.c`**

```c
#include <stdio.h>
#include <string.h>
#include "mcpkit/core/context.h"
#include "mcpkit/json/json.h"
#include "mcpkit/protocol/sampling.h"
#include "test_check.h"

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    CHECK(ctx != NULL);

    // 1. Create a tool definition for sampling
    mcp_json_value_t *schema = mcp_json_object_new(ctx);
    mcp_sampling_tool_def_t tool = {
        .name = "calculator",
        .description = "Performs arithmetic calculations",
        .input_schema = schema
    };

    mcp_json_value_t *params = mcp_sampling_params_new(ctx, 1024);
    CHECK(params != NULL);
    CHECK(mcp_sampling_params_add_tool(ctx, params, &tool) == MCP_OK);
    CHECK(mcp_sampling_params_set_tool_choice(ctx, params, MCP_TOOL_CHOICE_REQUIRED, NULL) == MCP_OK);

    // 2. Serialize params and verify tools array & toolChoice
    char *s = mcp_json_serialize(ctx, params);
    CHECK(s != NULL);
    CHECK(strstr(s, "\"name\":\"calculator\"") != NULL);
    CHECK(strstr(s, "\"toolChoice\":{\"mode\":\"required\"}") != NULL || strstr(s, "\"toolChoice\":\"required\"") != NULL);
    mcp_context_free(ctx, s);
    mcp_json_destroy(ctx, params);

    // 3. Create tool_use and tool_result content blocks
    mcp_json_value_t *input_args = mcp_json_object_new(ctx);
    mcp_json_value_t *tu = mcp_sampling_content_tool_use_new(ctx, "call_1", "calculator", input_args);
    CHECK(tu != NULL);
    char *tu_s = mcp_json_serialize(ctx, tu);
    CHECK(strstr(tu_s, "\"type\":\"tool_use\"") != NULL);
    CHECK(strstr(tu_s, "\"id\":\"call_1\"") != NULL);
    mcp_context_free(ctx, tu_s);
    mcp_json_destroy(ctx, tu);

    mcp_json_value_t *tr = mcp_sampling_content_tool_result_new(ctx, "call_1", "result is 42", false);
    CHECK(tr != NULL);
    char *tr_s = mcp_json_serialize(ctx, tr);
    CHECK(strstr(tr_s, "\"type\":\"tool_result\"") != NULL);
    CHECK(strstr(tr_s, "\"toolUseId\":\"call_1\"") != NULL);
    CHECK(strstr(tr_s, "\"isError\":false") != NULL);
    mcp_context_free(ctx, tr_s);
    mcp_json_destroy(ctx, tr);

    mcp_context_destroy(ctx);
    printf("test_sampling_tools OK\n");
    return 0;
}
```

- [ ] **Step 2: Add test target to `tests/CMakeLists.txt` and verify compile failure**

```cmake
add_executable(test_sampling_tools unit/test_sampling_tools.c)
target_link_libraries(test_sampling_tools PRIVATE mcpkit_core)
add_test(NAME test_sampling_tools COMMAND test_sampling_tools)
```

- [ ] **Step 3: Define structs and APIs in `include/mcpkit/protocol/sampling.h`**

```c
typedef enum {
    MCP_TOOL_CHOICE_AUTO = 0,
    MCP_TOOL_CHOICE_NONE,
    MCP_TOOL_CHOICE_REQUIRED,
    MCP_TOOL_CHOICE_SPECIFIC
} mcp_sampling_tool_choice_t;

typedef struct {
    const char *name;
    const char *description;
    const mcp_json_value_t *input_schema;
} mcp_sampling_tool_def_t;

mcp_json_value_t *mcp_sampling_params_new(mcp_context_t *ctx, uint32_t max_tokens);

mcp_status_t mcp_sampling_params_add_tool(mcp_context_t *ctx, mcp_json_value_t *params,
                                          const mcp_sampling_tool_def_t *tool);

mcp_status_t mcp_sampling_params_set_tool_choice(mcp_context_t *ctx, mcp_json_value_t *params,
                                                 mcp_sampling_tool_choice_t mode,
                                                 const char *specific_tool_name);

mcp_json_value_t *mcp_sampling_content_tool_use_new(mcp_context_t *ctx, const char *id,
                                                    const char *name, mcp_json_value_t *input);

mcp_json_value_t *mcp_sampling_content_tool_result_new(mcp_context_t *ctx, const char *tool_use_id,
                                                       const char *content, bool is_error);
```

- [ ] **Step 4: Implement in `src/protocol/sampling.c` and add to `src/CMakeLists.txt`**

Implement JSON construction, memory ownership handling, and error checking.

- [ ] **Step 5: Run tests and verify PASS**

Run: `cmake --build build --target test_sampling_tools && ./build/tests/test_sampling_tools`
Expected: `test_sampling_tools OK`

- [ ] **Step 6: Commit**

```bash
git add include/mcpkit/protocol/sampling.h src/protocol/sampling.c src/CMakeLists.txt tests/unit/test_sampling_tools.c tests/CMakeLists.txt
git commit -m "feat(sampling): add Sampling with Tools (SEP-1577) data structures and builders"
```

---

### Task C2: Client Sampling Provider Integration & Dispatch

**Files:**
- Modify: `include/mcpkit/client/client.h`
- Modify: `src/client/client.c`
- Modify: `tests/unit/test_sampling_tools.c`

- [ ] **Step 1: Write client sampling test with tools**

Simulate server-to-client request `sampling/createMessage` carrying `tools` and verify host callback receives the parsed tools and can return a `tool_use` response.

- [ ] **Step 2: Update `client.h` and `client.c` provider handling**

Extend `mcp_client_handle_server_request` to parse `tools` and `toolChoice` from `sampling/createMessage` params and pass them to the registered provider.

- [ ] **Step 3: Run test suite & verify**

Run: `cmake --build build && ctest --test-dir build --output-on-failure`
Expected: 100% tests PASS.

- [ ] **Step 4: Commit**

```bash
git add include/mcpkit/client/client.h src/client/client.c tests/unit/test_sampling_tools.c
git commit -m "feat(client): support sampling with tools in client request dispatch"
```
