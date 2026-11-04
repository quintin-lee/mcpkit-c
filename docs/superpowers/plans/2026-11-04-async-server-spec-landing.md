# Async Server Spec-Landing Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 10-30 spec 未闭合部分：还原 `conn_detach_and_close` + 写路径单一所有权（根除 `conn->closing`/`write_buf` 竞态）、HTTP 流式两阶段解析、WS Continuation 分片、引用计数 session reaper，并补齐 5 个新单测 + sanitizer 门禁。

**Architecture:** 全部改动集中在 `src/transport/async_server.c`（结构体骨架已就位）+ 新增 5 个单测文件 + `tests/CMakeLists.txt` 注册。写路径收敛为"worker 只 `enqueue_response`，reactor 线程独占改写 `conn->write_buf`"；连接关闭统一走 `conn_detach_and_close`（幂等 + 双向链表 O(1) 脱钩）；session 生命周期由 `ref_count` 保护，5s 批次 reaper 锁外断开 SSE 避免死锁。

**Tech Stack:** C23, POSIX sockets, epoll/poll reactor, C11 `threads.h`（mtx/cnd/stdatomic）, CMake/CTest, ASan+UBSan（同构建）/TSan（独立构建，二者互斥）。

**前置 spec：** `docs/superpowers/specs/2026-11-04-async-server-spec-landing-design.md`（已 APPROVE）。

**关键现状（实现前必读）：**
- `src/transport/async_server.c` 结构体字段已就位（`detached`/`closing`/`http_in_flight`/`prev/next`/`ws_frag_*`/session `ref_count`/`dead`/`conns_count`），但 `conn_detach_and_close` 被 `#if 0` 注释成 `...` 占位（约 L180-184），`active_connections`/`reap`/分片状态机/ref 增持均未实现。
- 测试用 `tests/test_check.h` 的 `CHECK(x)` 宏（NDEBUG 下仍生效，失败 `abort()`）。
- 既有单测注册模式（`tests/CMakeLists.txt` L133-135）：`add_executable(test_x unit/test_x.c)` + `target_link_libraries(... PRIVATE mcpkit_core)` + `add_test(NAME test_x COMMAND test_x)`。
- 现有 `handle_client_read` 写完成分支（L586-597）：释放 `write_buf` → 检查 `closing` → `mcp_reactor_modify` 回 `MCP_EVENT_READ`。
- `on_accept`（L694+）当前**没有**维护 `conns_count`，且直接 `mcp_reactor_add` 后返回。
- 构建/测试基线命令（HTTP+SOCKET 全开）：
  ```
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
  cmake --build build -j
  ctest --test-dir build --output-on-failure
  ```

---

## Chunk 1: 连接生命周期 + 写路径单一所有权（P0/P1 根除）

### Task 1: 还原 `conn_detach_and_close` + `conns_count` 原子化 + `active_connections` 探针

**Files:**
- Modify: `src/transport/async_server.c`
- Test: 通过既有 `test_async_server` + 新增 `test_async_leak` 验证

- [ ] **Step 1: `conns_count` 升级为 `atomic_size_t` 并维护**

在 `src/transport/async_server.c` 的 `struct mcp_async_server` 中把
```c
    size_t conns_count;
```
改为
```c
    atomic_size_t conns_count;
```
并在 `mcp_async_server_create` 的 `memset(s, 0, sizeof(*s));` 之后、`mtx_init` 附近加
```c
    atomic_init(&s->conns_count, 0);
```
（注意：`memset` 对 `atomic_size_t` 不可靠，必须用 `atomic_init`；把它放在 `memset` 之后。）

在 `on_accept` 中，每个连接压入 `s->conns` 的双向链表处（现有 `mtx_lock(&s->conns_mtx); conn->next = s->conns; s->conns = conn; mtx_unlock(...)`）改为同时维护 `prev` 与计数：
```c
        mtx_lock(&s->conns_mtx);
        conn->prev = NULL;
        conn->next = s->conns;
        if (s->conns != NULL) {
            s->conns->prev = conn;
        }
        s->conns = conn;
        atomic_fetch_add_explicit(&s->conns_count, 1, memory_order_relaxed);
        mtx_unlock(&s->conns_mtx);
```
（`on_accept` 创建 conn 后 `memset(conn, 0, sizeof(*conn))`，`prev/next` 已清零。）

- [ ] **Step 2: 还原 `conn_detach_and_close`**

把 `#if 0 ... #endif` 包裹的占位（约 L180-184）替换为完整实现：
```c
static void conn_detach_and_close(mcp_async_server_t *s, async_conn_t *conn) {
    if (conn == NULL) {
        return;
    }
    if (atomic_exchange(&conn->detached, true)) {
        return; /* 已脱钩，幂等返回 */
    }
    atomic_store(&conn->closing, true);
    if (conn->fd >= 0) {
        mcp_reactor_remove(s->ctx, s->reactor, conn->fd);
        close(conn->fd);
        conn->fd = -1;
    }
    if (conn->type == CONN_SSE && conn->session_id[0] != '\0') {
        mtx_lock(&s->sessions_mtx);
        for (session_entry_t *cur = s->sessions; cur != NULL; cur = cur->next) {
            if (cur->sse_conn == conn) {
                cur->sse_conn = NULL;
                break;
            }
        }
        mtx_unlock(&s->sessions_mtx);
    }
    mtx_lock(&s->conns_mtx);
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    } else {
        s->conns = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    if (atomic_load_explicit(&s->conns_count, memory_order_relaxed) > 0) {
        atomic_fetch_sub_explicit(&s->conns_count, 1, memory_order_relaxed);
    }
    mtx_unlock(&s->conns_mtx);
    conn_unref(conn);
}
```

- [ ] **Step 3: 全部关闭路径改走 `conn_detach_and_close`（单一明确方案）**

**唯一方案：把 `close_conn` 函数体改为调用 `conn_detach_and_close`，9 处调用点不动；**同时把 `mcp_async_server_destroy` 遍历 `s->conns` 循环里的直接 `close(cur_conn->fd)`（L912 附近）改为 `conn_detach_and_close`。

- `close_conn(s, conn)` 函数体改为：`conn_detach_and_close(s, conn);`（保留函数名作为薄封装，`handle_client_read` 内 9 处调用点 `close_conn` 全部不动）。
- 以下直接 `close()` **保持原样**（非连接 fd 或无 conn 可脱钩）：`on_accept` 分配失败的 `close(cfd)`（L722，conn 未入链）、`mcp_async_server_create/destroy` 的监听 fd `close(lfd)`/`close(s->listen_fd)`（L798/899）、`conn_unref` 内归零兜底的 `close(conn->fd)`（L157，脱钩后 fd 已 -1 不会触发）。
- 除 `close_conn`（已改薄封装）与 destroy 循环（L912 改 `conn_detach_and_close`）外，不得新增任何直接 `close(fd)` 关闭连接的路径。

- [ ] **Step 4: 实现 `mcp_async_server_active_connections`**

在 `.c` 中新增（放在 `mcp_async_server_destroy` 附近）：
```c
size_t mcp_async_server_active_connections(const mcp_async_server_t *s) {
    if (s == NULL) {
        return 0;
    }
    return atomic_load_explicit(&s->conns_count, memory_order_relaxed);
}
```
（`.h` L99 已有声明，无需改。）

- [ ] **Step 6: 会话 id 生成器去 `time(NULL)`**

在 `create_session_entry`（约 L226）把
```c
    static atomic_uint id_counter = 1000;
    unsigned val = atomic_fetch_add(&id_counter, 1);
    snprintf(entry->id, sizeof(entry->id), "sess-%u-%ld", val, (long)time(NULL));
```
改为单调原子 64 位、不含时钟：
```c
    static atomic_uint_fast64_t id_counter = 1000;
    uint64_t val = atomic_fetch_add(&id_counter, 1);
    snprintf(entry->id, sizeof(entry->id), "sess-%llu", (unsigned long long)val);
```
并确保 `entry->id` 缓冲（`char id[64]`）容纳 `1000...` 到 `2^64-1` 的十进制（20 位）无压力。

- [ ] **Step 7: 编译 + 既有测试回归**

Run:
```
cmake --build build -j
ctest --test-dir build -R "async|ws|http" --output-on-failure
```
Expected: 构建零告警（`-Wall -Wextra`），`test_async_server` / `test_ws_transport` 等既有测试 PASS。

- [ ] **Step 8: 提交**

```bash
git add src/transport/async_server.c
git commit -m "refactor(transport): restore conn_detach_and_close, atomic conns_count, active_connections probe"
```

---

### Task 2: 新增 `test_async_leak`（零泄漏门禁）

**Files:**
- Create: `tests/unit/test_async_leak.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写测试**

创建 `tests/unit/test_async_leak.c`：启动 async server（127.0.0.1:0），循环 300 次短连接（发 `initialize` POST 后关闭 socket），驱动 `mcp_async_server_step` 至连接清空，断言 `mcp_async_server_active_connections(s) == 0`，销毁后靠 ASan 门禁验证零泄漏。骨架：
```c
#include "test_check.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <threads.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "mcpkit/mcpkit.h"
#include "mcpkit/transport/async_server.h"

static int run_server(void *a) { mcp_async_server_t *s = a; mcp_async_server_run(NULL, s); return 0; }

int main(void) {
    mcp_context_t *ctx = mcp_context_create(NULL);
    mcp_server_t *srv = mcp_server_create(ctx, "leak", "0.1.0");
    mcp_executor_t *pool = mcp_threadpool_create(ctx, 4);
    mcp_async_server_config_t cfg = {.port = 0, .bind_addr = "127.0.0.1",
        .max_connections = 400, .keepalive_timeout_s = 5,
        .enable_websocket = true, .enable_sse = true};
    mcp_async_server_t *s = mcp_async_server_create(ctx, srv, pool, &cfg);
    CHECK(s != NULL);
    uint16_t port = mcp_async_server_get_port(s);
    CHECK(port > 0);

    thrd_t t; thrd_create(&t, run_server, s);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000}; thrd_sleep(&ts, NULL);

    for (int i = 0; i < 300; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(port)};
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
            const char *b = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                "\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
                "\"clientInfo\":{\"name\":\"l\",\"version\":\"0\"}}}";
            char req[512];
            snprintf(req, sizeof req, "POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Length:%zu\r\n\r\n%s",
                     strlen(b), b);
            write(fd, req, strlen(req));
            char tmp[16]; (void)!read(fd, tmp, sizeof tmp);
        }
        close(fd);
    }
    /* 驱动 reactor 清空连接 */
    for (int i = 0; i < 50; i++) { mcp_async_server_step(ctx, s, 5); }
    CHECK(mcp_async_server_active_connections(s) == 0);

    mcp_async_server_stop(ctx, s);
    thrd_join(t, NULL);
    mcp_async_server_destroy(ctx, s);
    mcp_executor_destroy(ctx, pool);
    mcp_server_destroy(ctx, srv);
    mcp_context_destroy(ctx);
    printf("test_async_leak: OK\n");
    return 0;
}
```
（实现者按需在 `for` 循环里插入 `mcp_async_server_step` 让 reactor 及时回收；关键是最终 `active_connections==0` 且 ASan 无泄漏。）

- [ ] **Step 2: 注册到 CMake**

在 `tests/CMakeLists.txt` 的 `test_async_server` 块（L133-135）后追加：
```cmake
add_executable(test_async_leak unit/test_async_leak.c)
target_link_libraries(test_async_leak PRIVATE mcpkit_core)
add_test(NAME test_async_leak COMMAND test_async_leak)
```

- [ ] **Step 3: 运行验证通过**

Run:
```
cmake --build build -j
ctest --test-dir build -R test_async_leak --output-on-failure
```
Expected: PASS（`test_async_leak: OK`）。

- [ ] **Step 4: 提交**

```bash
git add tests/unit/test_async_leak.c tests/CMakeLists.txt
git commit -m "test(transport): add test_async_leak zero-leak gate"
```

---

## Chunk 2: HTTP 流式两阶段解析 + 严格保序

### Task 3: `parse_http_content_length` 辅助 + 两阶段完整性判定 + 滑窗

**Files:**
- Modify: `src/transport/async_server.c`

- [ ] **Step 1: 先写失败测试 `test_async_fragmentation`（TDD 红灯）**

创建 `tests/unit/test_async_fragmentation.c`：A 分包（Header+4B Body → sleep 15ms → 剩余 Body，断言 200 不 400）；B 粘包（单次 `send` 两个完整 POST，断言按序两响应）。在**当前旧实现**下运行：
Run:
```
cmake --build build -j
cmake --build build -j 2>/dev/null; ctest --test-dir build -R test_async_fragmentation --output-on-failure
```
Expected: FAIL（旧实现分包返回 400 / 粘包丢包，证明修复必要性）。
随后把它注册进 `tests/CMakeLists.txt`（与 `test_async_server` 同构）。

- [ ] **Step 2: 加常量与 `parse_http_content_length` 辅助（最小实现使 A/B 转绿）**

在 `src/transport/async_server.c` 顶部（`#include` 后、类型定义前）加：
```c
#define MCP_ASYNC_MAX_BODY    (4u << 20)
#define MCP_ASYNC_MAX_READ_CAP (MCP_ASYNC_MAX_BODY + 65536u)
```
并新增私有辅助（大小写不敏感取 `Content-Length`，缺省 0，格式错返回 -1）：
```c
static long parse_http_content_length(const char *buf, size_t header_len) {
    const char *line = buf;
    while ((line = memchr(line, '\n', header_len - (size_t)(line - buf))) != NULL) {
        if (line[0] == 'C' && strncasecmp(line, "Content-Length:", 15) == 0) {
            const char *p = line + 15;
            while (*p == ' ' || *p == '\t') p++;
            char *endp = NULL;
            long v = strtol(p, &endp, 10);
            if (endp == p) return -1; /* 非数字 */
            while (*endp == ' ' || *endp == '\t') endp++;
            if (*endp != '\r' && *endp != '\n' && *endp != '\0') return -1;
            return v;
        }
        line++; /* 跳过分隔符继续下一行 */
    }
    return 0; /* 无 Content-Length */
}
```
（实现者用 `#include <ctype.h>`/确认 `strncasecmp` 可用；若编译器无 `strncasecmp`，手动实现大小写比较。）

- [ ] **Step 3: 读缓冲扩容钳制**

`handle_client_read` 的 `CONN_HTTP_INIT` 读事件分支，把现有
```c
            size_t ncap = conn->read_cap == 0 ? 8192 : conn->read_cap * 2;
            if (ncap > (4u << 20)) { close_conn(s, conn); return; }
```
改为
```c
            size_t ncap = conn->read_cap == 0 ? 8192 : conn->read_cap * 2;
            if (ncap > MCP_ASYNC_MAX_READ_CAP) { ncap = MCP_ASYNC_MAX_READ_CAP; }
            if (conn->read_len + 4096 > conn->read_cap && conn->read_cap >= MCP_ASYNC_MAX_READ_CAP) {
                conn_detach_and_close(s, conn);
                return;
            }
```

- [ ] **Step 4: 改 `CONN_HTTP_INIT` 完整性判定 + 传精确长度解析（两阶段）**

把 `handle_client_read` 的
```c
        if (conn->type == CONN_HTTP_INIT) {
            if (strstr(conn->read_buf, "\r\n\r\n") != NULL) {
                process_http_request(s, conn);
            }
        }
```
替换为两阶段判定（**替换而非新增**，旧 L637-639 分支整体被新逻辑取代）：
1. 在 `read_buf`（长度 `read_len`）内定位 `\r\n\r\n`，得 `header_len`；未找到 → `return`（等下个 read）。
2. `content_len = parse_http_content_length(read_buf, header_len)`；`content_len < 0` → `send_http_error(400)` + `return`；`content_len > MCP_ASYNC_MAX_BODY` → `send_http_error(413)` + `closing` + `return`。
3. `total_needed = header_len + (size_t)content_len`。（防御性：若 `total_needed > MCP_ASYNC_MAX_READ_CAP` → `send_http_error(413)` + `return`；因第 2 点已拦 `content_len > MCP_ASYNC_MAX_BODY`，且 `header_len` 远小于 65536，此分支实际近乎不可达，仅作冗余防御。）
4. **未收全守卫（关键）**：若 `conn->read_len < total_needed` → **直接 `return`，不调用 `process_http_request`、不重置 `read_len`**（等下个 read 继续 `recv` 追加，由现有 L602 扩容 + L620 `recv` 天然满足）。
5. 收全后调 `process_http_request(s, conn, total_needed)`（给函数头加 `size_t req_len` 参数，内部 `mcp_http_parse_request(ctx, read_buf, req_len)` 传精确长度）。

**滑窗消费（硬伤 #2：必须在调用者侧、仅 POST in-flight 才滑窗）**：
因为 `mcp_http_parse_request` 的 body 是 **in-place 指针**（`http.c:214,230`：`req->body = hs_end + 4`，直接指向 `read_buf`，不复制），**绝不能**在 `process_http_request` 内部各分支末尾做 `memmove`——那会破坏正在被 POST 分支 `memcpy(task->json_body, body, ...)` 消费的 body 指针。正确做法：
- `process_http_request` 内部 **保持现状**：内部**所有** `conn->read_len = 0` 清零点（含 GET/SSE/101 分支中间与 return 前，如 L451/494）一律保留不动；解析改用传入的 `req_len`，与调用者侧滑窗互不干扰。GET/SSE 路径仍清零；101 按 Step 6 保留 `read_len` 交 WS 循环消化。
- 滑窗**只在调用者侧 `handle_client_read` 里、且仅当本次为 POST 且 submit 成功（`http_in_flight` 被置位）时**执行：
```c
    /* POST in-flight 才滑窗；GET/SSE/101 已在 process_http_request 内清零 */
    if (conn->http_in_flight) {
        size_t remaining = conn->read_len - total_needed;
        if (remaining > 0) {
            memmove(conn->read_buf, conn->read_buf + total_needed, remaining);
        }
        conn->read_len = remaining;
        conn->read_buf[conn->read_len] = '\0';
    }
```
  其中 `http_in_flight` 由 POST 分支成功 `mcp_executor_submit` 后置位（Step 5，严格保序步），同步错误路径不置位、其响应由后续 drain 兜底，且这些路径 `read_len` 已被 `process_http_request` 清零，无需滑窗。
  ⚠️ 注意：滑窗必须在 `process_http_request` **返回之后**执行（此时 POST 分支已 `memcpy` 完 body 进 `task->json_body` 并 `mcp_http_request_destroy`），body 指针不再被使用，`memmove` 才安全。

- [ ] **Step 5: 严格保序 `http_in_flight` 置位/清除**

- POST 成功 `mcp_executor_submit` 之后（reactor 线程内）置 `conn->http_in_flight = true`；同步错误路径（`create_session_entry`/task OOM → 500）**不置位**。
- 在 `handle_client_read` 写完成分支（`write_off >= write_len`，L586-597）里，释放 `write_buf` 后、`mcp_reactor_modify` 前插入：
```c
            conn->http_in_flight = false;
```
并在 `mcp_reactor_modify` 前，若 `!conn->closing && conn->read_len > 0 && !conn->http_in_flight`，把事件改为 `MCP_EVENT_READ | MCP_EVENT_WRITE`（有粘包就保持可写），否则只 `MCP_EVENT_READ`。
- 在 `CONN_HTTP_INIT` 读事件分发前加守卫：`if (conn->http_in_flight) return;`（已有 in-flight 时不分发下一个，粘包留在 `read_buf`）。

- [ ] **Step 6: 101 升级后保留 `read_len`，交下次 CONN_WS 循环消化（与 Task 4 解耦）**

为避免 Task 3 强依赖 Task 4 尚未实现的 WS 分片状态机，**不做就地解析**：101 升级分支（现有 `conn->type = CONN_WS` 处）把 `conn->read_len = 0` 改为**保留 `read_len`**（不清零），下次 `handle_client_read` 进入 `CONN_WS` 分支时由既有/Task 4 的 `mcp_ws_parse_frame` 循环消化粘在升级帧后的 WS 首帧。Task 3 自身不引用任何 `ws_frag_*` 逻辑。

- [ ] **Step 7: 编译 + 回归**

Run:
```
cmake --build build -j
ctest --test-dir build -R "async|http" --output-on-failure
```
Expected: PASS（既有 `test_async_server` 的 HTTP/SSE/WS 用例仍通过；新 `test_async_fragmentation` 在 Chunk 5 加入前，本步靠既有测试验证不回归）。

- [ ] **Step 7: 提交**

```bash
git add src/transport/async_server.c
git commit -m "feat(transport): streaming HTTP two-phase parsing, strict ordering, sliding window"
```

---

## Chunk 3: WebSocket Continuation 分片重组

### Task 4: `test_ws_continuation` 前置（TDD 红灯）+ WS 分片状态机

**Files:**
- Modify: `src/transport/async_server.c`
- Create: `tests/unit/test_ws_continuation.c`

- [ ] **Step 1: 先写失败测试 `test_ws_continuation`（TDD 红灯）**

创建 `tests/unit/test_ws_continuation.c`：构造 Text(fin=0)+Continuation(fin=0)+空负载 Continuation(fin=1) 序列，断言拼装后收到 JSON-RPC 结果；异常（未初始化 Continuation / 超大分片）断言连接被安全关闭。在**当前旧实现**下运行：
Run:
```
cmake --build build -j
cmake --build build -j 2>/dev/null; ctest --test-dir build -R test_ws_continuation --output-on-failure
```
Expected: FAIL（旧实现 `fin==false` 帧被当完整帧提交、解析失败静默丢弃，不会正常返回结果）。
随后把它注册进 `tests/CMakeLists.txt`（与 `test_async_server` 同构）。随后实现下方状态机使其转绿。

- [ ] **Step 2: 控制帧优先就地处理**

在 `CONN_WS` 分支的帧循环里，保持 PING/PONG/CLOSE 现有就地处理；它们**不触碰** `ws_frag_buf`。

- [ ] **Step 3: 起始分片帧 / Continuation 状态机**

把 `CONN_WS` 分支的
```c
                if (frame.opcode == MCP_WS_OP_TEXT || frame.opcode == MCP_WS_OP_BINARY) {
                    /* 现有：直接提交 worker */
                }
```
改为状态机（字段 `ws_frag_buf/len/cap/opcode` 已就位）：
```c
                if (frame.opcode == MCP_WS_OP_TEXT || frame.opcode == MCP_WS_OP_BINARY) {
                    if (frame.fin) {
                        /* 完整数据帧：若正在拼装则违规 */
                        if (conn->ws_frag_buf != NULL) {
                            conn_detach_and_close(s, conn);
                            mcp_ws_frame_destroy(ctx, &frame);
                            return;
                        }
                        /* 现有提交 worker 逻辑（task->json_body = frame.payload; frame.payload=NULL; submit） */
                        ...
                    } else {
                        /* 起始分片帧 */
                        if (conn->ws_frag_buf != NULL) {
                            conn_detach_and_close(s, conn);
                            mcp_ws_frame_destroy(ctx, &frame);
                            return;
                        }
                        size_t cap = frame.payload_len + 1;
                        char *nb = srv_malloc(ctx, cap);
                        if (nb == NULL) { conn_detach_and_close(s, conn); mcp_ws_frame_destroy(ctx, &frame); return; }
                        memcpy(nb, frame.payload ? frame.payload : "", frame.payload_len);
                        nb[frame.payload_len] = '\0';
                        srv_free(ctx, conn->ws_frag_buf); /* 无旧值 */
                        conn->ws_frag_buf = nb;
                        conn->ws_frag_len = frame.payload_len;
                        conn->ws_frag_cap = cap;
                        conn->ws_frag_opcode = frame.opcode;
                        mcp_ws_frame_destroy(ctx, &frame);
                    }
                } else if (frame.opcode == MCP_WS_OP_CONTINUATION) {
                    if (conn->ws_frag_buf == NULL) {
                        conn_detach_and_close(s, conn);
                        mcp_ws_frame_destroy(ctx, &frame);
                        return;
                    }
                    size_t new_len = conn->ws_frag_len + frame.payload_len;
                    if (new_len > MCP_ASYNC_MAX_BODY) {
                        conn_detach_and_close(s, conn);
                        mcp_ws_frame_destroy(ctx, &frame);
                        return;
                    }
                    size_t ncap = conn->ws_frag_cap;
                    if (ncap < new_len + 1) {
                        char *nb = srv_malloc(ctx, new_len + 1);
                        if (nb == NULL) { conn_detach_and_close(s, conn); mcp_ws_frame_destroy(ctx, &frame); return; }
                        memcpy(nb, conn->ws_frag_buf, conn->ws_frag_len);
                        srv_free(ctx, conn->ws_frag_buf);
                        conn->ws_frag_buf = nb;
                        ncap = new_len + 1;
                    }
                    if (frame.payload != NULL) {
                        memcpy(conn->ws_frag_buf + conn->ws_frag_len, frame.payload, frame.payload_len);
                    }
                    conn->ws_frag_len = new_len;
                    conn->ws_frag_buf[conn->ws_frag_len] = '\0';
                    if (frame.fin) {
                        /* 拼装完毕：所有权移交 worker */
                        worker_task_t *task = srv_malloc(ctx, sizeof(*task));
                        if (task != NULL) {
                            conn_ref(conn);
                            task->server = s;
                            task->conn = conn;
                            task->session = conn->session;
                            task->entry = NULL;
                            task->is_ws = true;
                            task->is_init = false;
                            task->session_id[0] = '\0';
                            task->json_body = conn->ws_frag_buf;
                            conn->ws_frag_buf = NULL;
                            conn->ws_frag_len = 0;
                            conn->ws_frag_cap = 0;
                            mcp_executor_submit(ctx, s->pool, worker_dispatch_fn, task);
                        }
                    }
                    mcp_ws_frame_destroy(ctx, &frame);
                }
```
注意 `mcp_ws_frame_destroy(ctx, &frame)` 会释放 `frame.payload`；分片帧的 `frame.payload` 已被 `memcpy` 进 `ws_frag_buf`，destroy 释放原 payload 无副作用（无 double-free，与 spec 一致）。0 负载 Continuation 结束帧（`payload_len==0, fin==true`）走 `new_len` 不变、直接提交路径。

- [ ] **Step 4: `conn_unref` 已含 `ws_frag_buf` 释放**

确认 `conn_unref`（L172-174）已 `srv_free(ctx, conn->ws_frag_buf)`，无需改动。

- [ ] **Step 5: 编译 + 回归**

Run:
```
cmake --build build -j
ctest --test-dir build -R "ws|async" --output-on-failure
```
Expected: PASS（`test_ws_transport` / `test_ws_frame` / `test_async_server` 不回归；`test_ws_continuation` 已在 Step 1 注册，本步验证分片转绿）。

- [ ] **Step 6: 提交**

```bash
git add src/transport/async_server.c tests/unit/test_ws_continuation.c tests/CMakeLists.txt
git commit -m "feat(transport): RFC6455 WebSocket continuation frame reassembly with DoS cap"
```

---

## Chunk 4: 会话引用计数 + 5s 批次 reaper（UAF 消除 + 死锁免疫）

### Task 5: `session_entry_t` ref 管理 + `find_session` 增持 + worker unref

**Files:**
- Modify: `src/transport/async_server.c`

- [ ] **Step 1: 加 `session_entry_ref/unref` 辅助**

```c
static void session_entry_ref(session_entry_t *e) {
    if (e != NULL) atomic_fetch_add(&e->ref_count, 1);
}
static void session_entry_unref(mcp_async_server_t *s, session_entry_t *e) {
    if (e == NULL) return;
    if (atomic_fetch_sub(&e->ref_count, 1) == 1) {
        if (e->session != NULL && s->server != NULL) {
            mcp_server_destroy_session(s->ctx, s->server, e->session);
        }
        mtx_destroy(&e->session_lock);
        srv_free(s->ctx, e);
    }
}
```
（注意：`session_entry_t` 结构体需 `atomic_init(&e->ref_count, 1)` 在 `create_session_entry` 内 `memset` 之后补上；`dead` 同理 `atomic_init`。）

- [ ] **Step 2: `create_session_entry` 基准 ref + 返回再 ref**

`create_session_entry` 末尾，把 `memset(entry, 0, ...)` 后补
```c
    atomic_init(&entry->ref_count, 1);
    atomic_init(&entry->dead, false);
```
返回前 `session_entry_ref(entry);`（基准 1 + 调用方 1 = 2；调用方按场景 unref 一次，见 spec §2.4 规则：HTTP POST 把引用移给 task、SSE 在 `sse_conn` 赋值后就地 unref）。

实现者按 spec §2.4 的三条流转规则统一：
- HTTP POST：`worker_task_t` 持有的 entry 引用随 task 移交；`worker_dispatch_fn` 结束、释放 task 之前 `session_entry_unref(s, task->entry)`；OOM 失败路径（`task==NULL`/`json_body` 失败）调用方就地 `session_entry_unref(s, entry)`。
- GET SSE：`mtx_lock(sessions_mtx)` 内赋 `entry->sse_conn = conn`，解锁后调用方就地 `session_entry_unref(s, entry)`。

- [ ] **Step 3: `find_session` 锁内增持**

`find_session` 命中后**不**直接 `mtx_unlock; return cur;`，改为锁内 `session_entry_ref(cur)` 再解锁返回：
```c
        if (strcmp(cur->id, sid) == 0) {
            session_entry_ref(cur);
            mtx_unlock(&s->sessions_mtx);
            return cur;
        }
```
调用方据此对每个 `find_session` 成功结果在用完（或移交给 task）后 `session_entry_unref`。

- [ ] **Step 4: `last_active` 原子化**

`session_entry_t.last_active` 由普通 `time_t` 改为 `atomic_time_t`（C23：`atomic_time_t`）；worker `worker_dispatch_fn` 写用 `atomic_store(&entry->last_active, time(NULL))`，reaper 读用 `atomic_load`。若目标编译器无 `atomic_time_t`（C23 新增类型），退化为 `stdatomic` 包装一个 `int64_t` 时间戳。

- [ ] **Step 5: 编译 + 回归**

Run:
```
cmake --build build -j
ctest --test-dir build -R "async|http|session" --output-on-failure
```
Expected: PASS（引用计数平衡：无提前释放、无泄漏；`test_async_server` 的 initialize→SSE→WS 生命周期通过）。

- [ ] **Step 6: 提交**

```bash
git add src/transport/async_server.c
git commit -m "feat(transport): ref-counted session entries, TOCTOU-safe find_session, atomic last_active"
```

---

### Task 6: 5s 批次 `reap_expired_sessions`（锁外断开，死锁免疫）

**Files:**
- Modify: `src/transport/async_server.c`

- [ ] **Step 1: 实现 `reap_expired_sessions`**

```c
#define MCP_SESSION_REAP_BATCH 32
static void reap_expired_sessions(mcp_async_server_t *s) {
    time_t now = time(NULL);
    async_conn_t *sse_to_close[MCP_SESSION_REAP_BATCH];
    size_t count = 0;
    mtx_lock(&s->sessions_mtx);
    session_entry_t **pprev = &s->sessions;
    while (*pprev != NULL) {
        session_entry_t *e = *pprev;
        if ((long)(now - atomic_load(&e->last_active)) > (long)s->cfg.keepalive_timeout_s) {
            *pprev = e->next;
            atomic_store(&e->dead, true);
            if (e->sse_conn != NULL) {
                sse_to_close[count++] = e->sse_conn;
                e->sse_conn = NULL;
            }
            session_entry_unref(s, e); /* 释放基准引用（链表持有） */
        } else {
            pprev = &e->next;
        }
        if (count == MCP_SESSION_REAP_BATCH) break; /* 超批保留，下周期再清 */
    }
    mtx_unlock(&s->sessions_mtx);
    for (size_t i = 0; i < count; i++) {
        conn_detach_and_close(s, sse_to_close[i]); /* 锁外断开，杜绝自锁死锁 */
    }
}
```
注意：`s->cfg.keepalive_timeout_s` 为 0 时取默认 60（在 `create` 里已做默认化，见 spec §3.5/现有 `mcp_async_server_create` 的 `else` 分支）。

- [ ] **Step 2: `step` 里 5s 触发**

`mcp_async_server_step` 开头：
```c
    if ((long)time(NULL) - (long)s->last_reap_time >= 5) {
        s->last_reap_time = time(NULL);
        reap_expired_sessions(s);
    }
```

- [ ] **Step 3: `destroy` 统一走 `session_entry_unref`**

`mcp_async_server_destroy` 遍历 `s->sessions` 时，对每个 `cur_sess` 调 `session_entry_unref(s, cur_sess)`（替代现有直接 `mcp_server_destroy_session` + `mtx_destroy` + `srv_free`）。

- [ ] **Step 4: 编译 + 回归**

Run:
```
cmake --build build -j
ctest --test-dir build -R "async|http|session" --output-on-failure
```
Expected: PASS。

- [ ] **Step 5: 提交**

```bash
git add src/transport/async_server.c
git commit -m "feat(transport): batch session reaper with lock-free SSE disconnect"
```

---

## Chunk 5: 剩余 2 个单测 + sanitizer 门禁

### Task 7: `test_async_session_expiry` / `test_async_p0_concurrency`（前置测试已在 Chunk 2/3 落地）

> 注：`test_async_leak`（Chunk 1 Task 2）、`test_async_fragmentation`（Chunk 2 Task 3 Step 1）、`test_ws_continuation`（Chunk 3 Task 4 Step 0）已随各自实现前置落地。本 Task 仅剩剩余 2 个。

**Files:**
- Create: `tests/unit/test_async_session_expiry.c`, `tests/unit/test_async_p0_concurrency.c`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 两个测试按 spec §3.1 描述实现**

- `test_async_session_expiry`：`keepalive_timeout_s=1`，建立会话后 sleep 2s 驱动 `mcp_async_server_step`，断言后续请求 404；挂起长连接 + 慢任务期间会话超时，断言无崩溃/死锁/UAF。在 Chunk 4 实现前运行预期 FAIL（无 404，因旧实现无 reaper）。
- `test_async_p0_concurrency`：`thrd_create` 64 线程并发打同一 server（HTTP+SSE+WS 混合），结束后 `mcp_async_server_active_connections(s)==0` 且无泄漏（P0 写路径竞态的常态回归门禁）。

- [ ] **Step 2: 注册到 CMake**

在 `tests/CMakeLists.txt` 的 `test_async_server` 块（L133-135）后依次追加 2 段（与 `test_async_leak` 同构；`test_async_fragmentation`/`test_ws_continuation` 已在 Chunk 2/3 注册，此处勿重复）：
```cmake
add_executable(test_async_session_expiry unit/test_async_session_expiry.c)
target_link_libraries(test_async_session_expiry PRIVATE mcpkit_core)
add_test(NAME test_async_session_expiry COMMAND test_async_session_expiry)

add_executable(test_async_p0_concurrency unit/test_async_p0_concurrency.c)
target_link_libraries(test_async_p0_concurrency PRIVATE mcpkit_core)
add_test(NAME test_async_p0_concurrency COMMAND test_async_p0_concurrency)
```

- [ ] **Step 3: 运行全部 5 个新单测全绿**

Run:
```
cmake --build build -j
ctest --test-dir build -R "async|ws" --output-on-failure
```
Expected: `test_async_leak` / `test_async_fragmentation` / `test_ws_continuation` / `test_async_session_expiry` / `test_async_p0_concurrency` 全 PASS。

- [ ] **Step 4: 提交**

```bash
git add tests/unit/test_async_session_expiry.c tests/unit/test_async_p0_concurrency.c tests/CMakeLists.txt
git commit -m "test(transport): add session-expiry and p0-concurrency unit tests"
```

---

### Task 8: sanitizer 门禁（ASan+UBSan 同构建 / TSan 独立构建）

**Files:**
- （无源码改动，仅构建/验收）

- [ ] **Step 1: ASan+UBSan 全量**

Run:
```
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON \
  -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```
Expected: 全绿、零泄漏、零 UBSan 报告。

- [ ] **Step 2: TSan 独立构建（ASan/TSan 互斥）**

Run:
```
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DMCPKIT_ENABLE_TSAN=ON -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan -R "async|concurrent" --output-on-failure
```
Expected: 新增 `test_async_p0_concurrency` 与既有 `test_async_concurrency`（1000+ 并发）无 data race。

- [ ] **Step 3: `-Wall -Wextra` 零告警确认**

Run:
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON
cmake --build build -j 2>&1 | grep -iE "warning|error" || echo "CLEAN"
```
Expected: `CLEAN`（无 warning/error）。

- [ ] **Step 4: 提交门禁结论到 CHANGELOG**

更新 `CHANGELOG.md` 的 `[Unreleased]` → 归入已实现条目（可选，视用户是否要同步 changelog；默认本计划不动 changelog，仅记录于 commit message）。

---

## 验证与回滚

- **全量回归**：每个 Chunk 结束后跑 `ctest --test-dir build --output-on-failure`（HTTP+SOCKET 配置），既有 69 项不回归。
- **P0 防回归**：`test_async_p0_concurrency` + TSan 验收双保险。
- **回滚**：各 Task 独立 commit，可 `git revert`；`active_connections`/新单测为新增，回滚不影响既有签名。

## Out of Scope（另开"杂项清理 spec"）
- `mcp_queue_destroy`/`queue_grow` NOMEM 边缘泄漏。
- `mcp_context_set_json_backend` 的 `const void*` → 具体指针类型。
- `docs/module-reference.md` 幽灵 `mcp_dispatcher_*` 文档。
- `CHANGELOG.md` `[Unreleased]` 切分 released section。
