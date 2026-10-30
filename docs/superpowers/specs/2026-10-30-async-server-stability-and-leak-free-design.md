# 异步网络核心稳定性、零内存泄漏与 HTTP/WebSocket 协议健全性加固设计

> 状态：提案 (Brainstorming 完成，待 Spec Reviewer 验证与用户批准)  
> 日期：2026-10-30  
> 责任模块：`src/transport/async_server.c`, `src/transport/ws.c`, `CMakeLists.txt`

---

## 1. 目标与背景 (Intent & Why)

### 1.1 背景
在上一阶段演进中，`mcpkit-c` 引入了基于 Reactor 事件循环的高并发异步服务 `mcp_async_server_t`（位于 [`src/transport/async_server.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/transport/async_server.c)）以及 RFC 6455 原生 WebSocket 编解码（位于 [`src/transport/ws.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/transport/ws.c)）。

在近期的深度代码审查与并发边缘压力测试中，发现以下关键技术债与稳定性缺陷：
1. **连接与缓冲池永久泄漏**：`on_accept` 创建连接对象 `async_conn_t`（初始 `ref_count=1`）并压入 `s->conns` 链表。当连接断开时，系统虽关闭了 socket 并移出 reactor，**但从未从 `s->conns` 链表中脱钩，也未释放初始持有计数**。导致长跑服务器上的每一个历史连接对象及其高达 4MB 的 `read_buf` 永久驻留，高频请求下内存无上限线性泄露。
2. **HTTP 分包误杀与粘包截断**：
   - 现行实现只要在缓冲区检索到 `\r\n\r\n` 就判定请求到达并调用 `process_http_request()`；当客户端发送带 Body 的 POST 请求且 Body 跨 TCP 包时，底层 HTTP 解析器由于数据不足返回 `NULL`，服务端直接粗暴地向客户端返回 `400 Bad Request` 并关闭连接。
   - 请求处理完成后直接将 `conn->read_len = 0;` 重置，若同一 TCP 连接中有 HTTP Pipelining 或后续请求粘包数据，未消费的数据被直接清空丢弃。
3. **Session 缺乏超时淘汰巡检（Session Leak）**：`mcp_async_server_config_t` 配置了 `keepalive_timeout_s = 60`，代码中亦记录了 `last_active`，但从未有定时巡检逻辑释放过期会话，导致底层 `mcp_session_t` 随客户端访问持续膨胀。
4. **WebSocket 缺失 Continuation 帧重组**：RFC 6455 §5.4 规定长消息可以被拆分为多个连续帧传输。当前 `ws.c` 缺少分片缓冲拼装状态机，收到分片帧时会报错失败。
5. **构建缺少制度化警告约束**：CMake 配置未统一加上 `-Wall -Wextra`，代码卫生依赖人工自觉。

### 1.2 目标
本方案旨在彻底解决上述稳定性缺陷与技术债，使 `mcp_async_server_t` 达到真正的生产级高可用：
- 实现**连接与内存零泄漏**，在压力测试（数千次连接/断开循环）下验证内存平稳；
- 健全 HTTP 接收流式状态机，彻底消除分包误判与粘包丢失；
- 补全 WebSocket Continuation 帧重组与 Session 定时超时清理；
- 制度化 `-Wall -Wextra` 编译门禁并保持全绿。

---

## 2. 目标、非目标与反目标 (Goals, Non-Goals, Anti-Goals)

### 2.1 目标 (Goals)
1. **确定性连接生命周期**：连接关闭时在 $O(1)$ 时间复杂度内从全局连接池摘除，平衡初始引用计数，并在引用归零时彻底释放连接对象和读写缓冲区。
2. **流式 HTTP 两阶段解析与滑动窗口**：
   - 仅当接收到的数据同时满足“头部完整（出现 `\r\n\r\n`）”与“负载完整（已到达字节数 $\ge$ `header_len + Content-Length`）”时才触发业务分发；
   - 分发消费精准的字节数，利用滑动窗口（`memmove`）保留粘包与 Pipelining 数据。
3. **Session Timeout 自动巡检回收**：按固定周期巡检已超时的 `session_entry_t`，优雅清理绑定的 SSE 连接与 server 内部 session。
4. **WebSocket 分片帧支持**：支持 RFC 6455 §5.4 标准分片重组（首帧 `fin=0, opcode=TEXT/BIN`，中间帧 `opcode=CONTINUATION, fin=0`，末帧 `opcode=CONTINUATION, fin=1`）。
5. **严苛的门禁保障**：保持 C23 零依赖，通过全部 69+ 项已有测试，新增针对分包、内存泄漏、会话超时与 WebSocket 分片的单元测试，并在 ASan+UBSan 及 `-Wall -Wextra` 下无泄漏无告警。

### 2.2 非目标 (Non-Goals)
1. **跨平台 Reactor POSIX poll 重构**：此工作划入第二阶段专项，本期专注网络核心稳定性与零泄漏。
2. **公开 API 破坏性改动**：保持 `include/mcpkit/transport/async_server.h` 与 `ws.h` 的所有公开函数签名 100% 稳定向后兼容。
3. **HTTP/2 或 HTTP/3 支持**：继续专注 RFC-compliant HTTP/1.1 SSE 与 WebSocket。

### 2.3 反目标 (Anti-Goals)
1. **严禁破坏线程安全模型**：网络 I/O 线程与 Worker 线程间的锁职责与生命周期交接必须严谨，禁止因引用计数错误引发 Use-After-Free 或锁竞争死锁。
2. **严禁在未收到完整包前分配超大内存**：HTTP Body 超过限额（4MB）或未完成传输时不得进行无节制的内存重新分配。

---

## 3. 详细设计 (Detailed Design)

### 3.1 连接生命周期与链表脱钩 (Connection Lifecycle & Zero-Leak)

#### 3.1.1 双向链表改造
在 [`src/transport/async_server.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/transport/async_server.c) 中：
```c
struct async_conn {
    int fd;
    atomic_uint ref_count;
    conn_type_t type;

    char *read_buf;
    size_t read_len;
    size_t read_cap;

    char *write_buf;
    size_t write_len;
    size_t write_cap;
    size_t write_off;

    /* WebSocket Continuation frame reassembly buffer */
    char *ws_frag_buf;
    size_t ws_frag_len;
    size_t ws_frag_cap;
    mcp_ws_opcode_t ws_frag_opcode;

    char session_id[64];
    mcp_session_t *session;
    mcp_async_server_t *server;
    atomic_bool closing;

    /* Doubly-linked list for O(1) removal from s->conns */
    async_conn_t *prev;
    async_conn_t *next;
};
```

#### 3.1.2 确定性脱钩与销毁逻辑
实现私有辅助函数 `conn_detach_and_close(mcp_async_server_t *s, async_conn_t *conn)`：
1. 若 `atomic_exchange(&conn->closing, true)` 已为 true，说明正在关闭流程中，避免重复脱钩；
2. 若 `conn->fd >= 0`：
   - `mcp_reactor_remove(s->ctx, s->reactor, conn->fd)`；
   - `close(conn->fd)`；
   - `conn->fd = -1`；
3. 持有 `mtx_lock(&s->conns_mtx)`：
   - 将 `conn` 从 `s->conns` 双向链表中解构摘除；
   - `mtx_unlock(&s->conns_mtx)`；
4. 调用一次 `conn_unref(conn)`，冲抵 `on_accept` 分配连接时的基准计数（`ref_count=1`）；
5. 最终在 `conn_unref` 中：当引用归零时，释放 `read_buf`、`write_buf`、`ws_frag_buf`、关闭 WS 会话并释放 `conn` 内存。

---

### 3.2 HTTP 两阶段流式解析与滑动窗口 (Streaming HTTP State Machine)

#### 3.2.1 两阶段检查算法
在 `handle_client_read` 处理 `CONN_HTTP_INIT` 时：
1. 查找 Headers 结束符：`const char *hs_end = find_headers_end(conn->read_buf, conn->read_len)`；
   若未找到（`hs_end == NULL`），说明 Header 尚未收齐，不采取任何破坏性操作，退出等待下次数据到达。
2. 计算 Header 长度：`header_len = (size_t)(hs_end + 4 - conn->read_buf)`；
3. 提取 `Content-Length`：从 Header 中提取数值，若无则 `content_len = 0`；
4. 校验总长度上限：`total_needed = header_len + content_len`；
   若 `total_needed > (4u << 20)`（4MB 上限），立即发送 `413 Payload Too Large` 并关闭连接；
5. 判断 Body 完整性：
   - 若 `conn->read_len < total_needed`：**Body 分包尚未收齐，直接 return，保持连接继续接收**；
   - 若 `conn->read_len >= total_needed`：数据包完整，调用 `process_http_request_bounded(s, conn, total_needed)` 进行后续分发。

#### 3.2.2 滑动窗口消费与粘包处理
当请求被分发提交（或立即完成响应）后：
```c
size_t remaining = conn->read_len - total_needed;
if (remaining > 0) {
    memmove(conn->read_buf, conn->read_buf + total_needed, remaining);
}
conn->read_len = remaining;
conn->read_buf[conn->read_len] = '\0';
```
在滑动窗口移动后，若连接未处于关闭状态且 `conn->read_len > 0`，再次检测是否包含下一个完整的 HTTP 请求（支持 Pipelining 连续吞吐）。

---

### 3.3 WebSocket Continuation 帧重组 (RFC 6455 §5.4)

在 `handle_client_read` 处理 `CONN_WS` 接收循环中：
1. 调用 `mcp_ws_parse_frame` 解析当前帧；
2. **初始分片帧**（`opcode == TEXT/BINARY` 且 `fin == false`）：
   - 将当前帧负载暂存至 `conn->ws_frag_buf`；
   - 记录 `conn->ws_frag_opcode = frame.opcode`；
   - 不提交业务调度，继续读取下一帧；
3. **后继续帧**（`opcode == CONTINUATION`）：
   - 若此前未收到起始帧（`conn->ws_frag_buf == NULL`），视为非法协议帧，关闭连接；
   - 将当前帧负载追加到 `conn->ws_frag_buf`；
   - 若 `fin == true`，说明分片消息接收完结：
     - 将 `conn->ws_frag_buf` 作为完整 JSON 消息提交给 `worker_dispatch_fn`；
     - 重置 `conn->ws_frag_buf = NULL`，`conn->ws_frag_len = 0`；
4. **控制帧处理**：Ping/Pong/Close 等控制帧（`opcode >= 0x8`）允许穿插在分片帧序列之间传输，优先就地应答，不破坏正在拼装的 `ws_frag_buf`。

---

### 3.4 会话超时淘汰巡检 (Session Reaper)

1. 在 `mcp_async_server_t` 结构体中记录 `time_t last_reap_time`；
2. 在 `mcp_async_server_step` 中检测：若 `now - s->last_reap_time >= 5`（每 5 秒一次），触发 `reap_expired_sessions(s)`；
3. `reap_expired_sessions(s)` 流程：
   - 加锁 `mtx_lock(&s->sessions_mtx)`；
   - 遍历 `s->sessions` 链表；
   - 若 `now - entry->last_active > s->cfg.keepalive_timeout_s`：
     - 将该 `entry` 从单向链表中摘除；
     - 若 `entry->sse_conn != NULL`，对其调用 `conn_detach_and_close` 安全断开关联的长连接；
     - 调用 `mcp_server_destroy_session(ctx, s->server, entry->session)`；
     - 销毁 `mtx_destroy(&entry->session_lock)` 并释放 `entry`；
   - 解锁 `mtx_unlock(&s->sessions_mtx)`。

---

### 3.5 构建与代码卫生规范

在根目录 `CMakeLists.txt` 中添加编译告警强制规范：
```cmake
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()
```
清理 `cmake/MCPKitOptions.cmake` 中未被读取的三个选项，增加注释明确其静态编译特性或移除死代码分支。

---

## 4. 测试与验证计划 (Testing & Verification Plan)

### 4.1 新增单元测试与验收测试
在 `tests/unit/` 中新增针对性测试并注册至 CTest：
1. **`test_async_leak`（连接生命周期与零泄漏门禁）**：
   - 启动本地 `mcp_async_server`；
   - 循环创建 5,000 次短连接（发送简单 ping 请求并断开）；
   - 断开后调用 `mcp_async_server_step` 推进；
   - 断言 `s->conns` 为 NULL，且在 ASan 监测下无任何内存未释放告警。
2. **`test_async_fragmentation`（分包与粘包模拟测试）**：
   - 测试 A（分包）：分两次向 server 发送单个 POST 请求（先发 Header + 4 字节 Body，等待 20ms 后发送剩余 Body），断言成功收到 200 OK 且结果正确，绝不返回 400。
   - 测试 B（粘包/Pipelining）：单次发送包含两个连续 HTTP 请求的 Payload，断言按序成功收到两个独立的 HTTP 响应。
3. **`test_ws_continuation`（WebSocket 分片重组测试）**：
   - 构造 3 个帧：Text 帧（fin=0）+ Continuation 帧（fin=0）+ Continuation 帧（fin=1）；
   - 送入解析与处理，断言业务层成功接收并执行该完整工具调用。
4. **`test_async_session_expiry`（会话超时巡检测试）**：
   - 设置 `keepalive_timeout_s = 1`；
   - 发送 initialize 建立会话，等待 2 秒并执行 step 巡检；
   - 断言再次使用该 `Mcp-Session-Id` 发送请求时返回 404 Session Not Found。

### 4.2 质量门禁 (Quality Gates)
- 全量 69+ 项已有测试 100% 通过；
- 新增测试全量通过；
- `-Wall -Wextra` 零告警；
- AddressSanitizer (ASan) 与 UndefinedBehaviorSanitizer (UBSan) 全绿，无任何泄漏或未定义行为。
