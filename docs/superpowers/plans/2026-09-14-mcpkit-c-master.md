# mcpkit-c Implementation Plan (Master + Phase 0)

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 从空仓库起步，按 P0→P9 分阶段建成模块化、生产级 C23 MCP SDK（含 MCP Apps 扩展层），每个阶段产出可构建、可测试、可提交的增量。

**Architecture:** 严格遵守 Protocol ≠ Runtime ≠ Transport 分离与依赖方向（Apps→Server/Client→Protocol→Core；Transport/Runtime/JSON 只向内依赖 Core，禁止 core→libuv/curl/OpenSSL、protocol→HTTP、apps→pthread）。公共类型一律 opaque（`typedef struct mcp_server mcp_server_t;`，只暴露指针），Core 默认 0 外部依赖，JSON/Runtime/Transport 走 interface→backend 静态后端先行。

**Tech Stack:** C23 (实测工具链：GCC 16 / Clang 22 / CMake 4.4，全支持 `nullptr`/`bool`/`static_assert`/`<threads.h>`/`<stdatomic.h>`)；CMake + CTest；断言式单测先行（暂不引入 cmocka/unity）；ASan/UBSan/TSan 接入；后期可选 yyjson / libuv / curl / OpenSSL / SQLite 走 `adapters/`。

**Scope 说明（按 writing-plans Scope Check）：** 本 spec 覆盖 10 个独立子系统，必须拆分为多个子计划，每份子计划独立产出可工作、可测试的软件。本文件 = 总路线图 + Phase 0 完整详细计划；Phase 1..9 只锁定接口边界与验收标准，详细 TDD 步骤见后续子计划文件（Chunk 3 清单）。

---

## Chunk 1: 总路线图 P0–P9（分析结论）

依赖方向铁律（spec §4，贯穿所有阶段）：

```text
Examples → mcpkit-apps → Server/Client → Protocol → Core
Transport ──→ Protocol    Runtime ──→ Core    JSON ──→ Core
```

| 阶段 | 目标 | 关键文件 | 独立验收标准 |
|---|---|---|---|
| P0 Bootstrap | 仓库骨架/构建/治理/C23 基线 | `CMakeLists.txt`, `cmake/`, `VERSION`, `LICENSE`, `README.md`, `include/mcpkit/mcpkit.h`, `include/mcpkit/core/version.h`, `tests/` | `cmake -S . -B build && cmake --build build -j && ctest --test-dir build` 全绿；`gcc/clang -std=c2x` 均通过 |
| P1 Core+Logging+JSON | opaque 类型、错误码、result、context、logger、零依赖 JSON 抽象+内置后端 | `include/mcpkit/core/*.h`, `include/mcpkit/json/*.h`, `include/mcpkit/logging/*.h`, `src/core|json|logging/` | 所有权规则文档化；JSON 往返单测通过；backend 可替换（测试替桩） |
| P2 Protocol | JSON-RPC/message/request/response/notification/initialize + tools/resources/prompts/completion + Schema Builder + 三级验证管线 | `include/mcpkit/protocol/*.h`, `include/mcpkit/json/schema.h` | 负面用例全过：malformed JSON / invalid JSON-RPC / unknown method / invalid params / oversized / invalid UTF-8 / duplicate ID（spec §26 前 7 项） |
| P3 Server+Session+Dispatcher | server/session/tool/resource/prompt/handler 生命周期 + 多 Session + Transport→Queue→Dispatcher（禁 transport 回调直调 tool） | `include/mcpkit/server/*.h`, `examples/minimal-server tool-server resource-server multi-session` | 多 Session 并存；慢 Tool 不阻塞 Server（并发冒烟） |
| P4 Transport stdio | `mcp_transport_t` 四方法接口 + stdio 实现 | `include/mcpkit/transport/*.h`, `src/transport/`, `transports/stdio/` | stdio 回环集成测试通过；超大消息拒绝 |
| P5 Client | client/session/tool/resource + `examples/client` | `include/mcpkit/client/*.h` | client↔server（stdio）initialize→tools/call 全流程通过 |
| P6 Runtime | executor ops（sync + threadpool 先行，libuv/custom 留扩展点）+ task/timer/event_loop | `include/mcpkit/runtime/*.h` | sync/threadpool 两后端跑通同一 Dispatcher 用例 |
| P7 MCP Apps（重点） | `ui://` + `text/html;profile=mcp-app` + `_meta.ui.resourceUri` + 结构化 CSP（4 域）+ 权限枚举集 + 生命周期回调 + tool visibility + 非 Apps Host 文本回退 | `include/mcpkit/apps/*.h`, `examples/mcp-app dashboard-app` | `get_system_status`-类端到端：普通 Host 得 text、Apps Host 得 resourceUri→HTML；CSP 缺省 deny；`model`/`app` 可见性隔离可测 |
| P8 HTTP+Adapter+Plugin | Streamable HTTP（SSE 只做兼容）、adapters 桩、plugin 静态 registry（动态 `.so` 延后到 ABI 稳定） | `include/mcpkit/transport/http.h`, `streamable_http.h`, `adapters/*`, `include/mcpkit/plugin/*`, `examples/http-server` | HTTP 长连接多 Session；静态后端注册/注销单测 |
| P9 CLI/硬化/一致性 | `mcpkit inspect/run/test/validate` + fuzz + conformance + 打包 + docs/security | `tools/mcpkit-cli mcp-inspect mcp-codegen`, `tests/fuzz conformance`, `packaging/`, `docs/` | `mcpkit inspect` 可调通 P3/P7 示例服务；ASan/UBSan/TSan 全绿 |

里程碑顺序（MVP 最早可用）：P0 → P1 → P2 → P3+P4（sync executor 内联，产出可用 stdio MCP Server）→ P6 threadpool → P5 Client → P7 Apps → P8 HTTP → P9 CLI/硬化。

全局非功能门（每个阶段 DoD 必须满足）：新增公共头 opaque 化（spec §27，禁裸露 `pthread_mutex_t` 等内部成员）；API 命名统一 `mcp_xxx_{create,init,destroy,set,get,add,remove,start,stop}`（§24）；错误只用 `mcp_status_t`（§24十一码，禁裸 `return -1`）；`create()`→caller 拥有，`mcp_server_add_*` 后→server 拥有（§6）；Core 零外部依赖。

---

## Chunk 2: Phase 0 详细计划（本文件唯一完整 TDD 粒度部分）

### File Structure（Phase 0 结束时）

```text
mcpkit-c/
├── CMakeLists.txt
├── cmake/MCPKitOptions.cmake
├── VERSION                      # 0.1.0
├── LICENSE                      # MIT（假设，见 Task 0 备注）
├── README.md                    # 定位/分层/所有权/构建
├── CHANGELOG.md / CONTRIBUTING.md / SECURITY.md
├── .gitignore
├── include/mcpkit/mcpkit.h
├── include/mcpkit/core/types.h
├── include/mcpkit/core/error.h
├── include/mcpkit/core/result.h
├── include/mcpkit/core/version.h
├── src/core/version.c
├── src/core/error.c
├── tests/CMakeLists.txt
├── tests/unit/test_version.c
├── tests/unit/test_error.c
└── docs/architecture/overview.md  # 依赖方向图（文字版）
```

### Task 0: 治理与骨架文件

**Files:**
- Create: `VERSION`, `LICENSE`, `README.md`, `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`, `.gitignore`, `docs/architecture/overview.md`

- [ ] **Step 1: 创建治理文件（内容完整给出，不写“待补充”）**

`VERSION`:
```text
0.1.0
```

`.gitignore`:
```gitignore
build/
cmake-build-*/
*.o
*.a
*.so
CTestTestfile.cmake
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
Testing/
```

`README.md`（最小可用，锁定三条铁律）:
```markdown
# mcpkit-c

Modular, production-grade MCP SDK and runtime for C23. MCP Apps is an extension layer.

## Layering (must not invert)

Examples -> apps -> server/client -> protocol -> core.
Transport/Runtime/JSON depend inward on core only.

## Ownership

`create()` -> caller owns. After `mcp_server_add_*()` -> server owns,
`mcp_server_destroy()` frees all. No unclear borrowed pointers.

## Build

cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build
```

- [ ] **Step 2: 确认 LICENSE 假设**

假设用 MIT（如项目另有要求，此步改为替换）。记录假设，后续可改，不阻塞构建。

- [ ] **Step 3: Commit**

```bash
git add VERSION LICENSE README.md CHANGELOG.md CONTRIBUTING.md SECURITY.md .gitignore docs/architecture/overview.md
git commit -m "chore: bootstrap repo governance and skeleton"
```

### Task 1: CMake 根 + 选项模块

**Files:**
- Create: `CMakeLists.txt`, `cmake/MCPKitOptions.cmake`
- Modify: 无（新仓）

- [ ] **Step 1: 写 `cmake/MCPKitOptions.cmake`（完整内容）**

```cmake
option(MCPKIT_BUILD_TESTS "Build tests" ON)
option(MCPKIT_BUILD_EXAMPLES "Build examples" ON)
option(MCPKIT_BUILD_APPS "Build MCP Apps extension" ON)
option(MCPKIT_BUILD_CLIENT "Build client" ON)
option(MCPKIT_BUILD_SERVER "Build server" ON)
option(MCPKIT_BUILD_HTTP "Build HTTP transport" OFF)
option(MCPKIT_BUILD_FUZZ "Build fuzz targets" OFF)
option(MCPKIT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MCPKIT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MCPKIT_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
```

- [ ] **Step 2: 写根 `CMakeLists.txt`（完整内容）**

```cmake
cmake_minimum_required(VERSION 3.28)
project(mcpkit VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(cmake/MCPKitOptions.cmake)

add_library(mcpkit_core
  src/core/version.c
  src/core/error.c
)
target_include_directories(mcpkit_core PUBLIC include)

if(MCPKIT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
```

- [ ] **Step 3: 运行配置验证失败先行（tests 目录尚不存在，预期失败）**

Run: `cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON`
Expected: FAIL（`add_subdirectory(tests)` 缺失）—— 失败即确认接线生效，继续 Task 2/3 把缺口补齐。

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt cmake/MCPKitOptions.cmake
git commit -m "build: add CMake root with C23 and feature options"
```

### Task 2: Core 公共头（opaque + 错误码 + result + version）

**Files:**
- Create: `include/mcpkit/mcpkit.h`, `include/mcpkit/core/types.h`, `include/mcpkit/core/error.h`, `include/mcpkit/core/result.h`, `include/mcpkit/core/version.h`
- Create: `src/core/version.c`, `src/core/error.c`

- [ ] **Step 1: 写头文件（完整内容）**

`include/mcpkit/core/types.h`:
```c
#ifndef MCPKIT_CORE_TYPES_H
#define MCPKIT_CORE_TYPES_H

#include <stddef.h>

// Opaque forward declarations (ABI stability, spec section 27).
typedef struct mcp_context mcp_context_t;
typedef struct mcp_server mcp_server_t;
typedef struct mcp_session mcp_session_t;
typedef struct mcp_tool mcp_tool_t;
typedef struct mcp_resource mcp_resource_t;
typedef struct mcp_prompt mcp_prompt_t;

#endif
```

`include/mcpkit/core/error.h`:
```c
#ifndef MCPKIT_CORE_ERROR_H
#define MCPKIT_CORE_ERROR_H

typedef enum {
    MCP_OK = 0,
    MCP_ERR_INVALID_ARGUMENT,
    MCP_ERR_NOMEM,
    MCP_ERR_IO,
    MCP_ERR_PROTOCOL,
    MCP_ERR_TIMEOUT,
    MCP_ERR_CANCELLED,
    MCP_ERR_NOT_FOUND,
    MCP_ERR_ALREADY_EXISTS,
    MCP_ERR_UNSUPPORTED,
    MCP_ERR_PERMISSION,
} mcp_status_t;

const char *mcp_status_string(mcp_status_t status);

#endif
```

`include/mcpkit/core/result.h`:
```c
#ifndef MCPKIT_CORE_RESULT_H
#define MCPKIT_CORE_RESULT_H

#include "mcpkit/core/error.h"

typedef struct {
    mcp_status_t status;
    const char *message; // borrowed, may be NULL; never owned
} mcp_result_t;

#define MCP_RESULT_OK ((mcp_result_t){.status = MCP_OK, .message = NULL})

#endif
```

`include/mcpkit/core/version.h`:
```c
#ifndef MCPKIT_CORE_VERSION_H
#define MCPKIT_CORE_VERSION_H

#define MCPKIT_VERSION_MAJOR 0
#define MCPKIT_VERSION_MINOR 1
#define MCPKIT_VERSION_PATCH 0

const char *mcpkit_version_string(void);

#endif
```

`include/mcpkit/mcpkit.h`:
```c
#ifndef MCPKIT_H
#define MCPKIT_H

#include "mcpkit/core/types.h"
#include "mcpkit/core/error.h"
#include "mcpkit/core/result.h"
#include "mcpkit/core/version.h"

#endif
```

`src/core/version.c`:
```c
#include "mcpkit/core/version.h"

const char *mcpkit_version_string(void) {
    return "0.1.0";
}
```

`src/core/error.c`:
```c
#include "mcpkit/core/error.h"

const char *mcp_status_string(mcp_status_t status) {
    switch (status) {
        case MCP_OK: return "MCP_OK";
        case MCP_ERR_INVALID_ARGUMENT: return "MCP_ERR_INVALID_ARGUMENT";
        case MCP_ERR_NOMEM: return "MCP_ERR_NOMEM";
        case MCP_ERR_IO: return "MCP_ERR_IO";
        case MCP_ERR_PROTOCOL: return "MCP_ERR_PROTOCOL";
        case MCP_ERR_TIMEOUT: return "MCP_ERR_TIMEOUT";
        case MCP_ERR_CANCELLED: return "MCP_ERR_CANCELLED";
        case MCP_ERR_NOT_FOUND: return "MCP_ERR_NOT_FOUND";
        case MCP_ERR_ALREADY_EXISTS: return "MCP_ERR_ALREADY_EXISTS";
        case MCP_ERR_UNSUPPORTED: return "MCP_ERR_UNSUPPORTED";
        case MCP_ERR_PERMISSION: return "MCP_ERR_PERMISSION";
        default: return "MCP_ERR_UNKNOWN";
    }
}
```

- [ ] **Step 2: 写 C23 静态断言门（追加到 `src/core/version.c` 末尾，编译期验证工具链）**

```c
#include <assert.h>
static_assert(MCPKIT_VERSION_MAJOR == 0, "version major must start at 0");
```

- [ ] **Step 3: Commit**

```bash
git add include/mcpkit/mcpkit.h include/mcpkit/core/ src/core/
git commit -m "feat(core): add opaque types, status codes, result and version"
```

### Task 3: 测试骨架 + TDD 首个绿灯

**Files:**
- Create: `tests/CMakeLists.txt`, `tests/unit/test_version.c`, `tests/unit/test_error.c`
- Test: `ctest --test-dir build`

- [ ] **Step 1: 写失败先行的测试（完整内容）**

`tests/unit/test_version.c`:
```c
#include <assert.h>
#include <string.h>
#include "mcpkit/core/version.h"

int main(void) {
    assert(strcmp(mcpkit_version_string(), "0.1.0") == 0);
    return 0;
}
```

`tests/unit/test_error.c`:
```c
#include <assert.h>
#include <string.h>
#include "mcpkit/core/error.h"

int main(void) {
    assert(strcmp(mcp_status_string(MCP_OK), "MCP_OK") == 0);
    assert(strcmp(mcp_status_string(MCP_ERR_NOMEM), "MCP_ERR_NOMEM") == 0);
    assert(strcmp(mcp_status_string((mcp_status_t)999), "MCP_ERR_UNKNOWN") == 0);
    return 0;
}
```

`tests/CMakeLists.txt`:
```cmake
add_executable(test_version unit/test_version.c)
target_link_libraries(test_version PRIVATE mcpkit_core)
add_test(NAME test_version COMMAND test_version)

add_executable(test_error unit/test_error.c)
target_link_libraries(test_error PRIVATE mcpkit_core)
add_test(NAME test_error COMMAND test_error)
```

- [ ] **Step 2: 运行构建+测试，预期全绿**

Run: `cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON && cmake --build build -j && ctest --test-dir build`
Expected: `100% tests passed, 0 tests failed out of 2`

- [ ] **Step 3: 双编译器验证 C23**

Run: `gcc -std=c2x -Wall -Wextra -I include -c src/core/version.c -o /tmp/v_gcc.o && clang -std=c2x -Wall -Wextra -I include -c src/core/version.c -o /tmp/v_clang.o`
Expected: 零警告零错误（有警告即修头文件，不加 `-Wno-*` 掩盖）。

- [ ] **Step 4: Commit**

```bash
git add tests/
git commit -m "test: add version and status-string unit tests"
```

### Task 4: Phase 0 验收（DoD）

- [ ] **Step 1: 完整三命令验收**

Run: `cmake -S . -B build -DMCPKIT_BUILD_TESTS=ON && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 构建 exit 0，`2/2 passed`。

- [ ] **Step 2: Sanitizer 冒烟（可选但推荐，Ubuntu 24.04+gcc16 直接可用）**

Run: `cmake -S . -B build-asan -DMCPKIT_ENABLE_ASAN=ON -DCMAKE_C_FLAGS="-fsanitize=address,undefined" && cmake --build build-asan -j && ctest --test-dir build-asan`
Expected: 全绿（如工具链缺 asan 则记录跳过原因，不视为失败）。

- [ ] **Step 3: 清理临时构建目录（保持工作树干净）**

```bash
rm -rf build build-asan /tmp/v_gcc.o /tmp/v_clang.o
git status --short
```
Expected: 仅源码/文档变更，无构建产物被追踪。

---

## Chunk 3: 后续子计划清单（每个一项，独立成文）

按下述顺序逐个生成详细 TDD 计划文件（命名 `docs/superpowers/plans/YYYY-MM-DD-<phase>.md`），每个文件自包含 Goal/Architecture/文件结构/分钟级步骤/精确命令/提交信息：

1. `phase1-core-json-logging` — JSON 抽象（value/object/array）+ 内置零依赖后端 + 可插拔 backend 接口 + logger；验收：往返/嵌套/转义单测 + 替桩后端替换演示。
2. `phase2-protocol-schema` — JSON-RPC 全系消息 + initialize 握手 + Schema Builder + 三级验证管线；验收：§26 负面矩阵全过。
3. `phase3-server-session-dispatcher` — 生命周期 + 多 Session + Queue→Dispatcher；验收：慢 Tool 不阻塞冒烟 + `minimal/multi-session` 示例。
4. `phase4-transport-stdio` — `mcp_transport_t` + stdio；验收：回环集成 + 超大消息拒绝。
5. `phase5-client` — client 全流程；验收：stdio 上 initialize→tools/call。
6. `phase6-runtime` — sync + threadpool executor；验收：同一 Dispatcher 用例双后端通过。
7. `phase7-mcp-apps` — §9–§12、§21–§23 全部机制；验收：双 Host 回退矩阵（普通 Host text / Apps Host resourceUri→HTML）+ CSP 缺省 deny + visibility 隔离。
8. `phase8-http-plugin-adapters` — Streamable HTTP + 静态 plugin registry + adapters 桩；验收：HTTP 多 Session。
9. `phase9-cli-hardening` — `mcpkit inspect/run/test/validate` + fuzz + conformance + packaging；验收：inspect 调通 P3/P7 示例 + 三 sanitizer 全绿。

**砍范围原则（YAGNI）：** Phase 0–4 不碰 HTTP/动态插件/libuv/curl/OpenSSL/SQLite、不断言完整 Apps；任何“顺手重构/提前抽象”一律推迟到所属阶段的子计划。
