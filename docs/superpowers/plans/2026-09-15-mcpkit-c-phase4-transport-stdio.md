# Phase 4 Transport stdio Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `mcp_transport_t` 四方法接口 + stdio 行定界实现 + serve 循环，回环集成测试通过、超大消息拒绝。

**Architecture:** Transport 只向内依赖 Core/Protocol/Server（`transport → server → protocol → core`，禁 transport 直调 tool handler，一律走 `mcp_server_dispatch`/`mcp_server_notify`）。stdio 用换行定界（MCP stdio 惯例），上限复用 `MCP_PROTOCOL_MAX_MESSAGE_BYTES`（4MB，`message.h` 已有）。`FILE*` 可注入以便测试。

**Tech Stack:** C23, CMake + CTest, 断言式单测, ASan/UBSan。

---

## Chunk 1: 契约头文件（先锁定，不写实现）

### `include/mcpkit/transport/transport.h`

```c
#ifndef MCPKIT_TRANSPORT_TRANSPORT_H
#define MCPKIT_TRANSPORT_TRANSPORT_H

#include <stddef.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;

typedef struct mcp_transport_ops {
    mcp_status_t (*start)(mcp_context_t *ctx, mcp_transport_t *t);
    mcp_status_t (*send)(mcp_context_t *ctx, mcp_transport_t *t, const char *data, size_t len);
    mcp_status_t (*recv)(mcp_context_t *ctx, mcp_transport_t *t, char **line_out);
    mcp_status_t (*stop)(mcp_context_t *ctx, mcp_transport_t *t);
} mcp_transport_ops_t;

mcp_transport_t *mcp_transport_create(mcp_context_t *ctx, const mcp_transport_ops_t *ops,
                                      void *backend);
void mcp_transport_destroy(mcp_context_t *ctx, mcp_transport_t *t);
mcp_status_t mcp_transport_start(mcp_context_t *ctx, mcp_transport_t *t);
mcp_status_t mcp_transport_send(mcp_context_t *ctx, mcp_transport_t *t, const char *data,
                                size_t len);
mcp_status_t mcp_transport_recv(mcp_context_t *ctx, mcp_transport_t *t, char **line_out);
mcp_status_t mcp_transport_stop(mcp_context_t *ctx, mcp_transport_t *t);

#endif
```

语义（写进实现注释只留关键句）：
- `recv` 返回一条完整帧（不含换行符，调用者用 `mcp_json_free_string` 释放）；EOF 返回 `MCP_ERR_IO` 且 `*line_out = NULL`；单行超 4MB 返回 `MCP_ERR_PROTOCOL`（4MB 上限即协议级约束）。
- `send` 写入 `data[len]` 并追加 `\n`，刷出。
- 任一 ops 为 NULL → 对应方法返回 `MCP_ERR_UNSUPPORTED`；`ops == NULL` 时 create 失败。
- 状态码只用 `error.h` 十一码（无 TOO_LARGE / NOT_SUPPORTED，禁私加）。

### `include/mcpkit/transport/stdio.h`

```c
#ifndef MCPKIT_TRANSPORT_STDIO_H
#define MCPKIT_TRANSPORT_STDIO_H

#include <stdio.h>

#include "mcpkit/core/error.h"

typedef struct mcp_context mcp_context_t;
typedef struct mcp_transport mcp_transport_t;
typedef struct mcp_server mcp_server_t;

mcp_transport_t *mcp_stdio_transport_create(mcp_context_t *ctx, FILE *in, FILE *out);
mcp_status_t mcp_stdio_serve(mcp_context_t *ctx, mcp_server_t *server, mcp_transport_t *t);

#endif
```

语义：
- `in/out` 为 NULL → 用 `stdin`/`stdout`；create 不拥有 FILE（不 fclose），destroy 只释 transport。
- `mcp_stdio_serve`：新建一个 session；循环 `recv → mcp_message_parse → dispatch/notify → serialize → send`；parse 失败（malformed/超大/非法 UTF-8）→ 发送 `-32700` 固定错误响应后继续（stdio 无 id 可回，`req_or_null` 传 NULL）；notification → 无回复；EOF → 正常返回 `MCP_OK`；destroy session 后返回。

---

## Chunk 2: TDD 任务

### Task 1: transport 四方法接口 + 单测

**Files:**
- Create: `include/mcpkit/transport/transport.h`, `src/transport/transport.c`, `tests/unit/test_transport.c`
- Modify: `CMakeLists.txt`（+2 源文件）, `include/mcpkit/mcpkit.h`（+2 include）, `tests/CMakeLists.txt`（+1 test）

- [ ] **Step 1: 写头文件与测试（stub 后缀）**

`tests/unit/test_transport.c` 用静态内存后端（录制 send、回放 recv 行）证明四方法分发：
```c
#include <assert.h>
#include <string.h>

#include "mcpkit/mcpkit.h"

typedef struct {
    char sent[256];
    size_t sent_len;
    int started;
    int stopped;
} fake_t;

static mcp_status_t fake_start(mcp_context_t *ctx, mcp_transport_t *t) {
    (void)ctx; (void)t;
    return MCP_OK;
}
/* ... send 记录到 fake.sent；recv 返回固定行 "hi" 的拷贝；stop 置位 ... */

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    assert(ctx != NULL);
    /* ops 全 NULL → create 返回 NULL */
    assert(mcp_transport_create(ctx, NULL, NULL) == NULL);
    /* 缺 recv 的 ops → recv 返回 NOT_SUPPORTED（以 error.h 实有码为准） */
    /* 完整 ops → start/send/recv/stop 全通，send 内容一致，recv 行需 free */
    mcp_context_destroy(ctx);
    return 0;
}
```

- [ ] **Step 2: 先配 CMake 跑出缺源失败** — 加 `src/transport/transport.c` 到 `add_library` 但不建文件，`cmake --build` 应报缺文件，证明 wiring 生效。
- [ ] **Step 3: 最小实现** — `transport.c`：create 拷贝 ops + 存 backend（backend 取法：`mcp_transport_create` 第三个参数 `void *backend`，结构体私存，stdio 实现用内部 accessor 或直接强转——选最简单的：transport.c 暴露 `void *mcp_transport_backend(ctx, t)` 内部函数经 `src/transport/internals.h` 给 stdio.c 用）。
- [ ] **Step 4: `ctest` 通过（17/17 中含新 `test_transport`）。**
- [ ] **Step 5: Commit** — `git add ... && git commit -m "feat(transport): add four-method transport interface"`。

### Task 2: stdio 帧实现 + 上限测试

**Files:**
- Create: `include/mcpkit/transport/stdio.h`, `src/transport/stdio.c`, `tests/unit/test_stdio_limits.c`
- Modify: `CMakeLists.txt`, `mcpkit.h`, `tests/CMakeLists.txt`

- [ ] **Step 1: 写测试**

```c
/* 用 tmpfile() 写超长行（4MB+1），rewind 后 create transport，recv 应返回 TOO_LARGE */
/* 短行 "{"a":1}\n" → recv 返回 "{\"a\":1}"（无换行），调用者 free */
/* 空输入（0 字节文件）→ recv 返回 ERR_IO 且 *out == NULL */
```

- [ ] **Step 2: 最小实现** — `stdio.c`：逐字节 `fgetc` 累积（`alloc_of` 走 ctx allocator），`\n` 结束（顺手吞 `\r`）；长度超 `MCP_PROTOCOL_MAX_MESSAGE_BYTES` 立刻停并返回 TOO_LARGE（丢弃至行尾，避免流错位）；`send` 用 `fwrite + fputc('\n') + fflush`。
- [ ] **Step 3: `ctest` 通过（18/18）。**
- [ ] **Step 4: Commit** — `git commit -m "feat(transport): add stdio line-delimited framing with size cap"`。

### Task 3: serve 循环 + 回环集成测试

**Files:**
- Create: `tests/unit/test_stdio_loopback.c`
- Modify: `src/transport/stdio.c`（+ `mcp_stdio_serve`）, `tests/CMakeLists.txt`

- [ ] **Step 1: 写回环测试** — 组装真实 server（echo tool，照抄 `examples/minimal-server/main.c` 的 handler 精简版）：
  1. `tmpfile()` 作输入，依次写入 `initialize` / `notifications/initialized` / `tools/call echo` 三行；
  2. 另 `tmpfile()` 作输出；`mcp_stdio_serve` 跑完应返回 `MCP_OK`；
  3. rewind 输出，逐行 parse：第一行是 result（含 `2025-06-18`），第二行无输出（notification），第三行 result 含 `"hello stdio"`。
- [ ] **Step 2: 实现 `mcp_stdio_serve`**（见 Chunk 1 语义；错误响应经 `mcp_response_err_new(ctx, NULL, -32700, "Parse error", NULL)` 构造）。
- [ ] **Step 3: `ctest` 通过（19/19）。**
- [ ] **Step 4: Commit** — `git commit -m "feat(transport): add stdio serve loop over server dispatch"`。

### Task 4: stdio-server 示例 + 管道集成

**Files:**
- Create: `examples/stdio-server/main.c`
- Modify: `examples/CMakeLists.txt`

- [ ] **Step 1: 写示例** — echo tool + `mcp_stdio_transport_create(ctx, NULL, NULL)`（真实 stdin/stdout）+ `mcp_stdio_serve`，返回值作 exit code。
- [ ] **Step 2: 管道验证（手动，非 ctest）** — `printf '%s\n' '{"jsonrpc":"2.0","id":1,...initialize...}' '{"jsonrpc":"2.0","method":"notifications/initialized"}' | ./build/examples/stdio-server`，肉眼确认两行输出（init result + 无 notification 回复即只有一行），exit 0。
- [ ] **Step 3: Commit** — `git commit -m "feat(examples): add stdio-server speaking real stdin/stdout"`。

### Task 5: CHANGELOG + 三门禁 + 合并 + 下一阶段

- [ ] **Step 1: CHANGELOG** 加 Phase 4 行。
- [ ] **Step 2: 三门禁** — 全新 `build`/`build-clang`/`build-asan`（asan 手动 `-fsanitize=address,undefined` 并 `ldd` 确认 libasan），`ctest` **20/20**（16 老 + transport/stdio_limits/stdio_loopback + 管道示例不计入 ctest）。
- [ ] **Step 3: Commit + 清理** — `git commit -m "chore: log phase 4 transport-stdio"`，删构建目录，`git status` 干净。
- [ ] **Step 4: 合并到 master** — `git checkout master && git merge phase4-transport-stdio && ctest 20/20 复验 && git branch -d phase4-transport-stdio`（用户已授权：阶段完成后自动合 master）。
- [ ] **Step 5: 下一阶段** — 直接开 `phase5-client` 分支继续（Client: `include/mcpkit/client/*.h`，initialize→tools/call 全流程，见 master 表 P5）。

## DoD

- [ ] `mcp_transport_*` 四方法 + stdio 实现落盘，opaque，状态码只用 `mcp_status_t`
- [ ] 回环测试：init→notify→call 全绿；超 4MB 行被拒；EOF 干净结束
- [ ] gcc / clang / ASan+UBSan `ctest` 全绿
- [ ] 工作树干净，已合 master
