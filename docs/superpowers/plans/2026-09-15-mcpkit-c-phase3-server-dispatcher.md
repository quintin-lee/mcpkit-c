# Phase 3 Server/Session/Dispatcher Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 Server 注册表（tool/resource/prompt）+ 多 Session 生命周期 + Queue→Dispatcher 同步分发，`examples/minimal-server` 与 `examples/multi-session` 可跑。

**Architecture:** 单 `mcpkit_core` 库延续；所有公开类型 opaque；`create()`→caller 拥有，`mcp_server_add_*()` 后→server 拥有；dispatcher 只依赖 protocol/json（validate L1-L3、message builders、schema validator），不碰 transport；queue 给 P4 留 `transport push → server pump` 缝；并发执行延后到 P6，本阶段 dispatcher 严格单线程同步。

**Tech Stack:** C23, CMake + CTest, assert 单测, gcc/clang/ASan+UBSan 三门禁。

---

## Chunk 1: 契约头文件（实现前全部落盘，T1 一次写完）

### File Structure（Phase 3 结束时新增）

```text
include/mcpkit/server/server.h       # server 生命周期 + add/remove + session + dispatch/notify
include/mcpkit/server/tool.h         # tool 创建 + handler 类型
include/mcpkit/server/resource.h     # resource 创建 + read handler 类型
include/mcpkit/server/prompt.h       # prompt 创建 + get handler 类型
include/mcpkit/server/session.h      # session 状态查询
include/mcpkit/server/dispatcher.h   # queue + process_one
src/server/internals.h               # 私有结构体（不安装，registry/session/dispatch 共享）
src/server/server.c                  # server create/destroy/add/remove
src/server/registry.c                # tool/resource/prompt create/destroy
src/server/session.c                 # session create/destroy
src/server/dispatcher.c              # dispatch/notify/queue/路由与结果包装
tests/unit/test_registry.c test_session.c test_dispatcher.c
examples/minimal-server/main.c examples/multi-session/main.c examples/CMakeLists.txt
```

### 完整头文件内容（照抄）

`include/mcpkit/server/tool.h`:
```c
#ifndef MCPKIT_SERVER_TOOL_H
#define MCPKIT_SERVER_TOOL_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;

// handler 返回 MCP_OK 时 *result_out 必须非 NULL，所有权转交 dispatcher。
// 返回错误时 dispatcher 生成 {content:[{type:"text",...}],isError:true}，*result_out 被忽略。
// 需要富错误细节的 handler 应自行返回 MCP_OK 并在 result 里放 isError:true。
typedef mcp_status_t (*mcp_tool_handler_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                            const mcp_json_value_t *args, void *user_data,
                                            mcp_json_value_t **result_out);

// input_schema 可 NULL（不校验参数）；非 NULL 时所有权转交（失败返回 NULL 时不转交）。
mcp_tool_t *mcp_tool_new(mcp_context_t *ctx, const char *name, const char *description,
                         mcp_json_value_t *input_schema, mcp_tool_handler_fn handler,
                         void *user_data);
void mcp_tool_destroy(mcp_context_t *ctx, mcp_tool_t *tool);

#endif
```

`include/mcpkit/server/resource.h`:
```c
#ifndef MCPKIT_SERVER_RESOURCE_H
#define MCPKIT_SERVER_RESOURCE_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_resource mcp_resource_t;

// handler 填充 contents 数组（元素为 {uri,mimeType?,text} 对象），所有权转交 dispatcher。
typedef mcp_status_t (*mcp_resource_read_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                             const char *uri, void *user_data,
                                             mcp_json_value_t **contents_out);

mcp_resource_t *mcp_resource_new(mcp_context_t *ctx, const char *uri, const char *name,
                                const char *mime_type_or_null, mcp_resource_read_fn on_read,
                                void *user_data);
void mcp_resource_destroy(mcp_context_t *ctx, mcp_resource_t *res);

#endif
```

`include/mcpkit/server/prompt.h`:
```c
#ifndef MCPKIT_SERVER_PROMPT_H
#define MCPKIT_SERVER_PROMPT_H

#include "mcpkit/core/error.h"
#include "mcpkit/json/value.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_prompt mcp_prompt_t;

// handler 填充 messages 数组（元素为 {role,content:{type:"text",text}}），所有权转交。
// args 可能为 NULL（客户端没传 arguments）。
typedef mcp_status_t (*mcp_prompt_get_fn)(mcp_context_t *ctx, mcp_session_t *session,
                                          const mcp_json_value_t *args_or_null, void *user_data,
                                          mcp_json_value_t **messages_out);

mcp_prompt_t *mcp_prompt_new(mcp_context_t *ctx, const char *name, const char *description_or_null,
                            mcp_prompt_get_fn on_get, void *user_data);
void mcp_prompt_destroy(mcp_context_t *ctx, mcp_prompt_t *prompt);

#endif
```

`include/mcpkit/server/session.h`:
```c
#ifndef MCPKIT_SERVER_SESSION_H
#define MCPKIT_SERVER_SESSION_H

#include <stdbool.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_session mcp_session_t;

bool mcp_session_is_initialized(mcp_context_t *ctx, const mcp_session_t *session);

#endif
```

`include/mcpkit/server/dispatcher.h`:
```c
#ifndef MCPKIT_SERVER_DISPATCHER_H
#define MCPKIT_SERVER_DISPATCHER_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_message mcp_message_t;
typedef struct mcp_queue mcp_queue_t;

mcp_queue_t *mcp_queue_create(mcp_context_t *ctx);
void mcp_queue_destroy(mcp_context_t *ctx, mcp_queue_t *q);
// session borrowed，msg owned；失败时 msg 所有权不转交。
mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session,
                            mcp_message_t *msg);
mcp_status_t mcp_queue_pop(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t **session_out,
                           mcp_message_t **msg_out);
size_t mcp_queue_size(mcp_context_t *ctx, const mcp_queue_t *q);
// 弹出一项并分发；notification 返回 *resp_out=NULL + MCP_OK；空队列返回 MCP_ERR_NOT_FOUND。
mcp_status_t mcp_server_process_one(mcp_context_t *ctx, mcp_server_t *server, mcp_queue_t *q,
                                    mcp_message_t **resp_out);

#endif
```

`include/mcpkit/server/server.h`:
```c
#ifndef MCPKIT_SERVER_SERVER_H
#define MCPKIT_SERVER_SERVER_H

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_message mcp_message_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_prompt mcp_prompt_t;

mcp_server_t *mcp_server_create(mcp_context_t *ctx, const char *name, const char *version);
void mcp_server_destroy(mcp_context_t *ctx, mcp_server_t *server);

// add 成功后所有权转交 server；失败（重复名/NULL）所有权不转交。
mcp_status_t mcp_server_add_tool(mcp_context_t *ctx, mcp_server_t *server, mcp_tool_t *tool);
mcp_status_t mcp_server_add_resource(mcp_context_t *ctx, mcp_server_t *server,
                                     mcp_resource_t *res);
mcp_status_t mcp_server_add_prompt(mcp_context_t *ctx, mcp_server_t *server, mcp_prompt_t *prompt);
mcp_status_t mcp_server_remove_tool(mcp_context_t *ctx, mcp_server_t *server, const char *name);
mcp_status_t mcp_server_remove_resource(mcp_context_t *ctx, mcp_server_t *server, const char *uri);
mcp_status_t mcp_server_remove_prompt(mcp_context_t *ctx, mcp_server_t *server, const char *name);

// session 由 server 拥有；destroy_session 分离并释放。
mcp_session_t *mcp_server_create_session(mcp_context_t *ctx, mcp_server_t *server);
void mcp_server_destroy_session(mcp_context_t *ctx, mcp_server_t *server, mcp_session_t *session);

// req 必须为 REQUEST（notification 走 mcp_server_notify）；*resp_out owned。
mcp_status_t mcp_server_dispatch(mcp_context_t *ctx, mcp_server_t *server,
                                 mcp_session_t *session, const mcp_message_t *req,
                                 mcp_message_t **resp_out);
mcp_status_t mcp_server_notify(mcp_context_t *ctx, mcp_server_t *server,
                               mcp_session_t *session, const mcp_message_t *notif);

#endif
```

### 分发管线顺序（dispatch 实现约束，照做）

1. kind 非 REQUEST → `MCP_ERR_INVALID_ARGUMENT`（不产生 response，resp_out=NULL）。
2. `mcp_message_validate` L1-L3 → 失败用返回的 rpc code 做 `mcp_response_err_new`。
3. session 门：method 非 `initialize` 且未 initialized → `-32600`（含 `ping`，严格先握手）。
4. session 私有 idset 判重 → 重复 id → `-32600`；通过则记入（保留至 session 销毁，P5 client 做 response 匹配时复用 remove）。
5. 路由（见下表）；tool 参数若有 schema 则 `mcp_schema_validate`，失败 → `-32602`。

| method | 动作 |
|---|---|
| `initialize` | `mcp_protocol_negotiate` + 存 client 信息 + `mcp_initialize_result_new`（server 名/版本）；**不**置 initialized（等 notification） |
| `ping` | 结果 `{}` |
| `tools/list` | `{tools:[{name,description?,inputSchema?}]}`（无 description/schema 时省 key） |
| `tools/call` | `{name*, arguments?}`；未知 name → `-32602`；handler 错误 → isError 结果（非 RPC 错误） |
| `resources/list` | `{resources:[{uri,name,mimeType?}]}` |
| `resources/read` | `{uri*}`；未知 uri → `-32602`；包 `{contents:[...]}`；handler 错误 → `mcp_status_to_rpc_code` 的 RPC 错误 |
| `prompts/list` | `{prompts:[{name,description?}]}` |
| `prompts/get` | `{name*, arguments?}`；未知 name → `-32602`；包 `{description?,messages:[...]}`；handler 错误 → RPC 错误 |
| 其他 | 防御性 `-32601`（L2 已拦） |

`mcp_server_notify`：`notifications/initialized` → 置 flag 返回 OK；其余 notification 一律忽略返回 OK；NULL 参数 → INVALID_ARGUMENT。

### 并发诚实声明（本阶段硬约束）

dispatcher 严格单线程同步：慢 tool 会占用调用线程。P6 threadpool 到来前，“慢 tool 不阻塞”验收解释为**隔离性**：session A 的慢 tool 不污染 session B 的状态与后续分发（test_dispatcher 用双 session 先后分发证明）。禁止在本阶段引入 pthread/mutex（依赖方向铁律，spec §4）。

---

## Chunk 2: TDD 任务

### Task 1: 注册表（headers + server/registry + test_registry）

**Files:**
- Create: `include/mcpkit/server/{server,tool,resource,prompt,session,dispatcher}.h`（照抄 Chunk 1）
- Create: `src/server/internals.h`（私有结构体：server/tool/resource/prompt/session/queue + 动态数组 helper 声明）
- Create: `src/server/server.c`（create/destroy/add/remove/find 内部查找）
- Create: `src/server/registry.c`（tool/resource/prompt new/destroy：strdup 经 ctx allocator，NULL name/handler→NULL）
- Modify: `CMakeLists.txt`（加 2 个源文件）、`include/mcpkit/mcpkit.h`（加 6 个 include）、`tests/CMakeLists.txt`
- Test: `tests/unit/test_registry.c`

- [ ] **Step 1: 写测试**
```c
#include <assert.h>
#include <string.h>
#include "mcpkit/mcpkit.h"

static mcp_status_t ok_handler(mcp_context_t *c, mcp_session_t *s, const mcp_json_value_t *a,
                               void *u, mcp_json_value_t **o) {
    (void)c; (void)s; (void)a; (void)u;
    *o = mcp_json_object_new(c);
    return *o ? MCP_OK : MCP_ERR_NOMEM;
}

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx);
    mcp_server_t *srv = mcp_server_create(ctx, "t", "1");
    assert(srv);
    // tool add + duplicate
    assert(mcp_server_add_tool(ctx, srv, mcp_tool_new(ctx, "echo", "e", NULL, ok_handler, NULL)) == MCP_OK);
    mcp_tool_t *dup = mcp_tool_new(ctx, "echo", "e", NULL, ok_handler, NULL);
    assert(mcp_server_add_tool(ctx, srv, dup) == MCP_ERR_ALREADY_EXISTS);
    mcp_tool_destroy(ctx, dup); // 失败不转交，caller 释放
    // resource/prompt add
    assert(mcp_server_add_resource(ctx, srv, mcp_resource_new(ctx, "f:///a", "a", "text/plain", NULL, NULL)) == MCP_OK);
    assert(mcp_server_add_prompt(ctx, srv, mcp_prompt_new(ctx, "p", NULL, NULL, NULL)) == MCP_OK);
    // remove + remove-missing
    assert(mcp_server_remove_tool(ctx, srv, "echo") == MCP_OK);
    assert(mcp_server_remove_tool(ctx, srv, "echo") == MCP_ERR_NOT_FOUND);
    assert(mcp_server_remove_resource(ctx, srv, "f:///a") == MCP_OK);
    assert(mcp_server_remove_prompt(ctx, srv, "p") == MCP_OK);
    // NULL guards
    assert(mcp_tool_new(ctx, NULL, NULL, NULL, ok_handler, NULL) == NULL);
    assert(mcp_server_add_tool(ctx, srv, NULL) == MCP_ERR_INVALID_ARGUMENT);
    assert(mcp_server_add_tool(ctx, NULL, mcp_tool_new(ctx, "x", NULL, NULL, ok_handler, NULL)) == MCP_ERR_INVALID_ARGUMENT);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    return 0;
}
```
注意：倒数第三行故意 leak 一个 tool（传 NULL server 必失败且返回码已断言，tool 指针丢失）。ASan 会报！修正：先存指针再 destroy。最终测试必须 ASan 干净——把该行写成：
```c
    mcp_tool_t *leak = mcp_tool_new(ctx, "x", NULL, NULL, ok_handler, NULL);
    assert(mcp_server_add_tool(ctx, NULL, leak) == MCP_ERR_INVALID_ARGUMENT);
    mcp_tool_destroy(ctx, leak);
```

- [ ] **Step 2: 接线并跑，预期链接失败（server.c/registry.c 未写）**
Run: `cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON && cmake --build build -j 2>&1 | tail -3`
Expected: FAIL（undefined reference）。
- [ ] **Step 3: 最小实现**（server.c + registry.c + internals.h；数组 `n/cap` 经 `mcp_context_allocator`；字符串拷贝；find-by-name/uri 线性查）
- [ ] **Step 4: 跑测试**
Run: `cmake --build build -j && ctest --test-dir build -R test_registry --output-on-failure`
Expected: PASS。
- [ ] **Step 5: Commit**
```bash
git add include/mcpkit/server/ src/server/ tests/unit/test_registry.c tests/CMakeLists.txt CMakeLists.txt include/mcpkit/mcpkit.h
git commit -m "feat(server): add tool/resource/prompt registry with ownership transfer"
```

### Task 2: Session（session.c + test_session）

**Files:**
- Create: `src/server/session.c`、`tests/unit/test_session.c`
- Modify: `src/server/server.c`（create/destroy_session：server 持 session 数组，destroy 连带释放）、`CMakeLists.txt`、`tests/CMakeLists.txt`

session 私有结构：`bool initialized; char *client_name, *client_version; mcp_idset_t *ids;`（idset create 在 session 创建时，destroy 时释放）。

- [ ] **Step 1: 写测试**：create 后 `mcp_session_is_initialized==false`；双 session 独立指针；`destroy_session` 后 server 可正常 destroy（ASan 无泄漏）；NULL guards（create NULL server→NULL；is_initialized NULL→false）。
- [ ] **Step 2: 跑，预期链接失败**（session.c 未写）。
- [ ] **Step 3: 最小实现**。
- [ ] **Step 4: 跑测试** Expected: `test_session` PASS（全量仍绿）。
- [ ] **Step 5: Commit** `feat(server): add multi-session lifecycle with per-session id tracking`

### Task 3: Dispatcher + Queue（dispatcher.c + test_dispatcher）

**Files:**
- Create: `src/server/dispatcher.c`、`tests/unit/test_dispatcher.c`
- Modify: `CMakeLists.txt`、`tests/CMakeLists.txt`

实现要点：按 Chunk 1 管线顺序；wrap helpers 全 static；handler 错误映射（tool→isError 结果，resource/prompt→RPC 错误）；`initialize` 从 params 取 `protocolVersion`/`clientInfo.{name,version}`（经 `mcp_json_object_get` + `mcp_json_string_value`，缺失走 L3 已拦，防御性判空→`-32602`）。

- [ ] **Step 1: 写测试**（覆盖：initialize→result 含 negotiated 版本且仍未 initialized；pre-init tools/call→-32600；notify initialized→flag 真；tools/list 含 echo；call 回显正确；schema 非法参数→-32602；未知 tool→-32602；handler 返回失败→isError true；resource read 未知 uri→-32602；prompt get 未知 name→-32602；重复 id→-32600；ping→{}；queue 顺序 + 空队列 NOT_FOUND + notification 经 process_one 无 response；慢 tool（busy-loop ~50ms）session A 与快 tool session B 先后分发皆正确）。
- [ ] **Step 2: 跑，预期链接失败**。
- [ ] **Step 3: 最小实现**。
- [ ] **Step 4: 跑测试** Expected: `test_dispatcher` PASS，全量 16/16（13 旧 + registry/session/dispatcher）。
- [ ] **Step 5: Commit** `feat(server): add dispatch pipeline with queue and session gating`

### Task 4: Examples（minimal-server + multi-session）

**Files:**
- Create: `examples/CMakeLists.txt`、`examples/minimal-server/main.c`、`examples/multi-session/main.c`
- Modify: 根 `CMakeLists.txt`（`if(MCPKIT_BUILD_EXAMPLES) add_subdirectory(examples)`）、`cmake/MCPKitOptions.cmake`（确认 `MCPKIT_BUILD_EXAMPLES` 存在，不存在则补）

minimal-server：建 ctx/server，注册 echo tool（args `{text}` 回显为 `{content:[{type:"text",text}], }` 普通结果）+ 静态 resource，parse 一段 tools/call JSON → dispatch → serialize 打印 → 全释放。multi-session：两 session 分别 initialize+notify，交错 call，打印两 response。

- [ ] **Step 1: 写 examples/CMakeLists.txt + 两个 main.c**。
- [ ] **Step 2: 构建并运行**
Run: `cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON && cmake --build build -j && ./build/examples/minimal-server/minimal-server && ./build/examples/multi-session/multi-session`
Expected: exit 0，打印合法 JSON-RPC response（含 result）。
- [ ] **Step 3: Commit** `feat(examples): add minimal-server and multi-session`

### Task 5: 收尾（CHANGELOG + 三门禁 + 清理）

- [ ] **Step 1: CHANGELOG 加 Phase 3 条目**（一行：server registry/session/dispatch/queue + examples）。
- [ ] **Step 2: 三门禁全绿**（clean configure 起跑）：
```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build  # gcc 16/16
CC=clang cmake -S . -B build-clang && cmake --build build-clang -j && ctest --test-dir build-clang  # 16/16
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined" && cmake --build build-asan -j && ctest --test-dir build-asan  # 16/16（先确认 libasan 已链接：ldd build-asan/tests/test_registry | grep asan）
```
- [ ] **Step 3: 清理 + Commit**
```bash
rm -rf build build-clang build-asan
git status --short  # 干净
git add CHANGELOG.md
git commit -m "chore: log phase 3 server-session-dispatcher"
```

DoD：16/16 三门禁 + examples 可运行 + 工作树干净（构建目录已删）。
