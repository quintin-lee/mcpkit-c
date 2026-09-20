# Resilience Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 mcpkit-c 加上 I/O 超时、有界队列、list 分页三件套，消除挂死与 OOM 型 DoS。

**Architecture:** 超时状态存在 transport wrapper（`mcp_transport_set_timeout`，默认 0=永阻塞，后向兼容，ops 表不动）；socket 后端用 `poll()`+单调时钟做收发 deadline，超时返回 `MCP_ERR_TIMEOUT`；stdio 后端每行首次 `fgetc` 前 `poll(fileno)`；队列加 `MCP_QUEUE_MAX_LEN 1024` 上限；list 系列按 MCP 规范 `cursor`/`nextCursor` 分页（cursor=十进制偏移，每页 100），client `list_tools` 自动翻页保持调用方兼容。

**Tech Stack:** C23, POSIX poll(2)/monotonic clock, existing CHECK test harness.

---

## Chunk 1: Transport timeouts

### Task 1: wrapper 存超时 + socket 实现

**Files:**
- Modify: `include/mcpkit/transport/transport.h`
- Modify: `src/transport/transport.c`
- Modify: `src/transport/socket.c`

- [ ] **Step 1: transport.h 加 API**

```c
/**
 * @brief Sets I/O timeouts (0 = block forever, the default).
 * ...
 */
mcp_status_t mcp_transport_set_timeout(mcp_context_t *ctx, mcp_transport_t *t,
                                       uint64_t read_ms, uint64_t write_ms);
mcp_status_t mcp_transport_get_timeout(mcp_context_t *ctx, const mcp_transport_t *t,
                                       uint64_t *read_ms_out, uint64_t *write_ms_out);
```

struct 加 `uint64_t read_ms, write_ms;`（create 时置 0）。

- [ ] **Step 2: transport.c 实现**（NULL t → INVALID_ARGUMENT；get 的 NULL out → INVALID_ARGUMENT）
- [ ] **Step 3: socket.c poll deadline**：`#include <poll.h>`（`_DEFAULT_SOURCE` 已有）；helper `wait_fd(fd, POLLIN/POLLOUT, deadline_ms)` 用 `clock_gettime(CLOCK_MONOTONIC)` 算剩余，`poll` 返回 0 → TIMEOUT，EINTR → 重算继续；`sock_recv` 主循环每次 `read` 前先 `wait_fd(POLLIN)`（timeout=0 跳过）；`sock_send` 每次 `send` 前 `wait_fd(POLLOUT)`。超时路径必须 free buf 置 NULL（与现有 IO/PROTOCOL 路径一致）。
- [ ] **Step 4: build**：`cmake -S . -B build -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON && cmake --build build -j && ctest --test-dir build` expect 全过（37/37）

### Task 2: stdio 超时

**Files:**
- Modify: `src/transport/stdio.c`

- [ ] **Step 1: stdio_recv 加 poll**：`#include <poll.h>`；取 `mcp_transport_get_timeout`；若 read_ms>0，每轮 `fgetc` 前 `poll(fileno(b->in), POLLIN, 剩余ms)`，超时 → free buf 置 NULL 返回 `MCP_ERR_TIMEOUT`。Caveat 写进 @file 注释：stdio 缓冲区已有数据但对端停顿时可能误报 TIMEOUT（只影响主动启用超时者，默认 0 行为不变）。
- [ ] **Step 2: build+ctest** expect 37/37

---

## Chunk 2: 有界队列 + list 分页

### Task 3: 队列上限

**Files:**
- Modify: `include/mcpkit/server/dispatcher.h`（`#define MCP_QUEUE_MAX_LEN 1024` + push 的 Doxygen 注明满时 NOMEM）
- Modify: `src/server/dispatcher.c`（`mcp_queue_push` 开头 `if (q->len >= MCP_QUEUE_MAX_LEN) return MCP_ERR_NOMEM;`）

- [ ] **Step 1: 改两处**，build+ctest expect 37/37

### Task 4: list 分页（server）

**Files:**
- Modify: `src/server/dispatcher.c`（4 个 list 路由 + 1 个 static helper）

- [ ] **Step 1: helper**

```c
// 解析 params.cursor（NULL/missing → 0；非数字字符串 → (size_t)-1 表非法）
static size_t page_offset(mcp_context_t *ctx, const mcp_message_t *req);
// 挂 nextCursor：if (end < total) set_string(result, "nextCursor", "<end>")
```

- [ ] **Step 2: 四个路由** `route_tools_list` / `route_resources_list` / `route_prompts_list` / `route_completion_list`：读 offset（-1 → err_resp INVALID_PARAMS "…/list: bad cursor"）；循环只吐 `[offset, offset+100)`（`#define MCP_LIST_PAGE_SIZE 100` 放 dispatcher.c 顶部）；visibility 过滤保持（注意：tools 的 offset 应对**过滤后**序列计数——先收集可见条目再切片；最简实现：遍历中维护 visible_idx，只 append 落在窗口内的）。尾部挂 `nextCursor`（还有剩余时）。
- [ ] **Step 3: build+ctest** expect 37/37（现有单页行为不变）

### Task 5: client 自动翻页

**Files:**
- Modify: `src/client/client.c`（`mcp_client_list_tools`）

- [ ] **Step 1: 循环取页**：params=NULL 第一页；每轮 `mcp_client_request("tools/list")` 取 `tools` 数组 clone 逐个 `mcp_json_array_append` 进累积数组（append 失败 → 全销毁 NOMEM）；读 `nextCursor`（string）→ 下一轮 params=`{"cursor": cur}`（用 set_take 挂载）；无 nextCursor 结束。result_out=NULL 时仍翻完丢弃（语义与原来一致：原来 result_out=NULL 只做请求）。
- [ ] **Step 2: build+ctest** expect 37/37

---

## Chunk 3: 测试 + 收尾

### Task 6: test_timeout.c

**Files:**
- Create: `tests/unit/test_timeout.c`
- Modify: `tests/CMakeLists.txt`（仿 test_socket 条目）

- [ ] **Step 1: 用例**（CHECK 宏，`#include "test_check.h"`）：
  1. socket 读超时：server(45683)+client 建连，`mcp_transport_set_timeout(client, 200, 0)`，client recv → `MCP_ERR_TIMEOUT`（200ms 级不断言 wall time，只断言状态码；不断言 line 内容）。
  2. 超时可恢复：承上，server 发一行后 client 正常 recv 到（证明超时没破坏连接）。
  3. 队列上限：queue push 1024 个 notification → OK，第 1025 个 → NOMEM；pop 一个再 push → OK。
- [ ] **Step 2: build+ctest** expect 38/38

### Task 7: test_pagination.c

**Files:**
- Create: `tests/unit/test_pagination.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 用例**：注册 250 个 tool（`t000..t249`，静态 handler）；直调 `mcp_server_dispatch` tools/list 无 params → 100 条 + nextCursor=="100"；cursor=100 → 100 条 + nextCursor=="200"；cursor=200 → 50 条 + 无 nextCursor；cursor="abc" → -32602；client `list_tools`（经 tmpfile pump 或直连 fake？——用 test_client_e2e 式 tmpfile-pair pump 太重；改用 dispatch 直调累积等价路径？client 翻页逻辑需 request/response往返，必须走 transport。方案：复用 test_client.c 的 record/replay fake？replay 是固定脚本，翻页动态 cursor 难预演。**最终方案**：pagination 测试只覆盖 server 端 dispatch；client 翻页用 socketpair 真环回：server 端另起线程跑 serve？单进程单线程会死锁。**更简方案**：client 翻页逻辑测一半——用 tmpfile-pair + 手工 pump（仿 test_client_e2e 的 pump_send 模式，server_step 同步驱动），三页脚本可手写。看 test_client_e2e.c 的 pump 写法照抄。）
- [ ] **Step 2: build+ctest** expect 39/39

### Task 8: 门禁 + 提交

- [ ] **Step 1: CHANGELOG** `## [Unreleased]` 下加三行（timeouts / queue cap / pagination）
- [ ] **Step 2: 三门禁**（clean configure）：gcc 39/39、clang 39/39、ASan+UBSan 39/39（HTTP+SOCKET=ON），ldd 验 sanitizer
- [ ] **Step 3: 提交**（2 个 commit：`feat(transport): ...timeouts` 含 test_timeout；`feat(server): ...queue cap + pagination` 含 test_pagination；CHANGELOG 可并入后者），删 build dirs，树干净
