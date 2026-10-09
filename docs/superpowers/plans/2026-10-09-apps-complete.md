# MCP Apps Complete Support Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:**补齐 MCP Apps 三个缺口：tool 级 `_meta`、client 端 UI helper、stdio 传输层 E2E。

**Architecture:** 全部仿照现有模式零发明：`mcp_tool_set_meta` 复制 `mcp_tool_set_output_schema` 的 TAKE 语义（`src/server/registry.c:89-100`）；tools/list 序列化复制 inputSchema 的 clone+set_take 段（`src/server/dispatcher.c:422-440`）；`apps-server` 抄 `stdio-server`/`file-server` 骨架；`apps_accept.sh` 抄 `cli_accept.sh` 的 `case ... in *...*` 断言风格。

**Tech Stack:** C23，零依赖，现有 mcpkit_core + mcpkit-cli + ctest。

**Spec:** `docs/superpowers/specs/2026-10-09-apps-complete-design.md`

---

## Chunk 1: tool 级 `_meta` 存储 + 序列化

**Files:**
- Modify: `src/server/internals.h:38-49`（`struct mcp_tool` 加 `mcp_json_value_t *meta;`，紧随 `output_schema`）
- Modify: `src/server/registry.c`（两处 init 置 NULL：第 38 行 `mcp_tool_new`、第 80 行 `mcp_tool_new_v2`；`mcp_tool_destroy` 加 `mcp_json_destroy(ctx, tool->meta);`；新增 setter，紧随 `mcp_tool_set_output_schema`）
- Modify: `include/mcpkit/server/tool.h`（setter 声明 + doxygen，仿 186–201 行 output_schema 注释风格）
- Modify: `src/server/dispatcher.c`（`route_tools_list`，outputSchema 块之后加 `_meta` 块）
- Modify: `tests/unit/test_apps.c`（纯 API 单测，追加到 `main` 尾部、cleanup 之前）
- Modify: `tests/unit/test_apps_e2e.c`（tools/list 含 `_meta` 的 dispatch 断言）

### Task 1: struct 加字段 + init/destroy

- [ ] **Step 1: 写 setter 初版（先只做存储，不做序列化）**

在 `src/server/internals.h` 的 `struct mcp_tool` 中 `output_schema` 下加一行：

```c
    mcp_json_value_t *output_schema;
    mcp_json_value_t *meta;
```

`src/server/registry.c` 两处 `tool->output_schema = NULL;`（第 38、80 行）之后各加
`tool->meta = NULL;`；`mcp_tool_destroy` 中 `mcp_json_destroy(ctx, tool->output_schema);`
之后加 `mcp_json_destroy(ctx, tool->meta);`。`mcp_tool_set_output_schema` 之后加：

```c
mcp_status_t mcp_tool_set_meta(mcp_context_t *ctx, mcp_tool_t *tool,
                               mcp_json_value_t *meta) {
    if (tool == NULL) {
        if (meta != NULL) {
            mcp_json_destroy(ctx, meta);
        }
        return MCP_ERR_INVALID_ARGUMENT;
    }
    if (tool->meta != NULL) {
        mcp_json_destroy(ctx, tool->meta);
    }
    tool->meta = meta;
    return MCP_OK;
}
```

`include/mcpkit/server/tool.h` 在 `mcp_tool_set_output_schema` 声明后加声明 + doxygen
（TAKE 语义、NULL 清除、tool-NULL 销毁传入值三句话）。

- [ ] **Step 2: 构建验证**

Run: `cmake --build build 2>&1 | head -20`
Expected: 零警告零错误（`-Wall -Wextra` 下新增行无警告）。

### Task 2: tools/list 序列化 `_meta`

- [ ] **Step 1: 加序列化块**

在 `src/server/dispatcher.c` 的 outputSchema 块（约 431–440 行）之后加：

```c
        if (t->meta != NULL) {
            mcp_json_value_t *copy = mcp_json_clone(ctx, t->meta);
            if (copy == NULL ||
                mcp_json_object_set_take(ctx, entry, "_meta", copy) != MCP_OK) {
                mcp_json_destroy(ctx, entry);
                mcp_json_destroy(ctx, result);
                mcp_json_destroy(ctx, arr);
                return NULL;
            }
        }
```

- [ ] **Step 2: 先写 failing 单测（test_apps_e2e.c 风格，走 in-process dispatch）**

在 `tests/unit/test_apps_e2e.c` 的 tool 注册后加：构造
`{"ui":{"resourceUri":"ui://app/main"}}` → `mcp_tool_set_meta` → tools/list dispatch →
断言 entry 含 `_meta.ui.resourceUri == "ui://app/main"`；再测无 meta 的 tool 不含 `_meta` 键。
（TDD：此步先加测试、实现已在上步完成，此处顺序是"补测锁定行为"——若测试习惯严格，
可先 revert 序列化块、看测试失败、再加回。）

- [ ] **Step 3: 跑单测**

Run: `cmake --build build && ./build/tests/test_apps_e2e && ./build/tests/test_apps`
Expected: 全部 PASS（exit 0，无 CHECK 输出）。

- [ ] **Step 4: 提交（等用户批准后执行，不擅自 commit）**

```bash
git add src/server/internals.h src/server/registry.c include/mcpkit/server/tool.h \
  src/server/dispatcher.c tests/unit/test_apps_e2e.c
git commit -m "feat(server): add tool-level _meta with tools/list serialization"
```

---

## Chunk 2: apps 快捷函数 + client helper

**Files:**
- Modify: `include/mcpkit/apps/ui.h`（两个声明 + doxygen）
- Modify: `src/apps/ui.c`（两个实现）
- Modify: `tests/unit/test_apps.c`（单测）

### Task 3: `mcp_apps_tool_set_ui`（merge 语义）

- [ ] **Step 1: 先写 failing 单测**

`tests/unit/test_apps.c` 追加：
1. 新 tool + `mcp_apps_tool_set_ui(ctx, tool, "ui://app/main")` → tools/list（用文件顶部
   `dispatch_new` helper）断言 `_meta.ui.resourceUri` 正确；
2. 先 `mcp_tool_set_meta` 存 `{"other":1}`，再 set_ui → 断言 `other` 保留且 `ui` 写入
   （merge 行为）；
3. `resource_uri == NULL` → `MCP_ERR_INVALID_ARGUMENT`；
4. `tool == NULL` → `MCP_ERR_INVALID_ARGUMENT`。

Run: `cmake --build build && ./build/tests/test_apps`
Expected: FAIL（新测试挂，函数未定义则链接失败——先只加测试不加实现，确认红灯）。

- [ ] **Step 2: 最小实现（src/apps/ui.c）**

```c
mcp_status_t mcp_apps_tool_set_ui(mcp_context_t *ctx, mcp_tool_t *tool,
                                  const char *resource_uri) {
    if (tool == NULL || resource_uri == NULL) {
        return MCP_ERR_INVALID_ARGUMENT;
    }
    /* 取已有 meta（若有则 clone 复用，保留其他键；用 getter 拿 borrowed 指针，
       无 getter 则直接构造新 meta——以实际头文件为准，见下方备注） */
    ...
}
```

备注：`mcp_tool_t` 对 apps 层是 opaque（定义在 `src/server/internals.h`，server 内部头）。
`src/apps/ui.c` 能否 include server internals？**动手前先确认**：若不能，则 merge 语义
改由 server 层提供 `mcp_tool_set_meta` 之上的只读 getter
（`const mcp_json_value_t *mcp_tool_meta(const mcp_tool_t *tool)`，仿
`mcp_tool_output_schema`，registry.c:104），ui.c 经 getter clone→改→set_meta 回写。
这是本计划唯一需现场确认的点，两种走法都在 TAKE/clone 既有模式内。

- [ ] **Step 3: 跑单测**

Run: `cmake --build build && ./build/tests/test_apps`
Expected: PASS。

### Task 4: `mcp_apps_result_ui_uri`（borrowed reader）

- [ ] **Step 1: 先写 failing 单测**

追加：正常 result（含 `_meta.ui.resourceUri`，用 `mcp_apps_result_with_ui` 构造）
→ 读出指针相等；缺 `_meta` / 缺 `ui` / 缺 `resourceUri` / `resourceUri` 非字符串 →
`MCP_ERR_INVALID_ARGUMENT`；`result == NULL` / `uri_out == NULL` → 同样错误码。

- [ ] **Step 2: 最小实现**

逐层 `mcp_json_object_get` + `mcp_json_string_value`，任一失败即返回
`MCP_ERR_INVALID_ARGUMENT`，成功时 `*uri_out` 指向 borrowed 字符串（不拷贝、不拥有）。

- [ ] **Step 3: 跑单测 + 提交（等用户批准）**

Run: `cmake --build build && ./build/tests/test_apps && ./build/tests/test_apps_e2e`
Expected: PASS。

```bash
git add include/mcpkit/apps/ui.h src/apps/ui.c tests/unit/test_apps.c \
  src/server/registry.c include/mcpkit/server/tool.h
git commit -m "feat(apps): add tool_set_ui merge helper and result_ui_uri reader"
```

---

## Chunk 3: apps-server 示例 + apps_accept.sh

**Files:**
- Create: `examples/apps-server/main.c`（抄 `examples/file-server/main.c` 骨架：
  `#define _DEFAULT_SOURCE` 顶部 + `unistd.h`，stdio 主循环；1 个 tool + 1 个 ui:// resource）
- Modify: `examples/CMakeLists.txt`（仿第 10–11 行 file-server 注册两行）
- Create: `tests/acceptance/apps_accept.sh`（`chmod +x`，仿 cli_accept.sh 风格）
- Modify: `tests/CMakeLists.txt`（`add_test(NAME apps_acceptance ...)`，仿 151–158 行）

### Task 5: apps-server 示例

- [ ] **Step 1: 写 main.c**

行为：`greet` tool（入参 `{"name"}`，返回 `{"greeting"}` + `mcp_apps_result_with_ui`
打标 `ui://app/greet`）+ `mcp_apps_ui_resource_new` 注册 `ui://app/greet`
（HTML 含 `<p>` 标记、CSP default-deny）。tool 注册后调 `mcp_apps_tool_set_ui`
声明 tool 级 `_meta`。`--help` 退出 0（抄 file-server 的 argv 处理）。

- [ ] **Step 2: 注册 + 构建**

examples/CMakeLists.txt 加：

```cmake
add_executable(apps-server apps-server/main.c)
target_link_libraries(apps-server PRIVATE mcpkit_core)
```

Run: `cmake --build build 2>&1 | grep -i "warn\|error" ; echo BUILD_OK`
Expected: 无警告无错误。

- [ ] **Step 3: 手动 CLI 验证**

Run: `./build/tools/mcpkit-cli inspect ./build/examples/apps-server`
Expected: 输出含 `_meta`、`resourceUri`、`ui://app/greet`。

Run: `./build/tools/mcpkit-cli call ./build/examples/apps-server greet '{"name":"qi"}'`
Expected: 输出含 greeting 与 `resourceUri`。

### Task 6: apps_accept.sh + 接线

- [ ] **Step 1: 写脚本**

```sh
#!/bin/sh
# Apps acceptance: tool-level _meta, result _meta.ui, ui:// resource read over stdio.
# argv: <cli> <apps-server>
set -e
CLI="$1"
APPS_SRV="$2"

out="$("$CLI" inspect "$APPS_SRV")"
case "$out" in *resourceUri*ui://app/greet*) ;; *) echo "FAIL: inspect missing tool _meta.ui"; exit 1 ;; esac

out="$("$CLI" call "$APPS_SRV" greet '{"name":"qi"}')"
case "$out" in *resourceUri*ui://app/greet*) ;; *) echo "FAIL: call missing result _meta.ui"; exit 1 ;; esac

# resources/read 经管道直连（CLI 无 read 子命令，不扩 scope）
out="$(printf '%s' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"acc","version":"1"}}}' | "$APPS_SRV" | head -c 4000)"
case "$out" in *serverInfo*) ;; *) echo "FAIL: apps-server handshake"; exit 1 ;; esac
```

注：resources/read 的完整握手+通知+读取管道序列，写脚本时仿 file-server 验证手法
（initialize → initialized 通知 → resources/read，逐段 printf 拼 JSON），断言返回含
`Content-Security-Policy` 与 `<p>` 标记。先在 shell 手动跑通再固化进脚本。

- [ ] **Step 2: 接线 + 全量验证**

tests/CMakeLists.txt 在 cli_acceptance 块后加：

```cmake
if(MCPKIT_BUILD_TOOLS AND MCPKIT_BUILD_EXAMPLES)
  add_test(NAME apps_acceptance
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/apps_accept.sh
                   ${CMAKE_BINARY_DIR}/tools/mcpkit-cli
                   ${CMAKE_BINARY_DIR}/examples/apps-server)
endif()
```

Run（两种配置都要过，沿用项目基线做法）：
`ctest --test-dir build` → 全部 PASS；
`cmake -B build-http -DMCPKIT_ENABLE_HTTP=1 ... && ctest --test-dir build-http` → 全部 PASS。

- [ ] **Step 3: 提交（等用户批准）**

```bash
git add examples/apps-server/main.c examples/CMakeLists.txt \
  tests/acceptance/apps_accept.sh tests/CMakeLists.txt
git commit -m "feat(examples): add apps-server UI demo and acceptance test"
```

---

## 验证总闸（三个 chunk 完成后）

- `ctest` Release 默认配置全过、HTTP 开启配置全过
- 新增/改动文件 `-Wall -Wextra` 零警告
- docs 若涉及公开 API 签名变化，同步 `docs/module-reference.md`（P1 时立过规矩：文档与头文件一致）
