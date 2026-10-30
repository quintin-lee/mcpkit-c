# 异步网络核心稳定性、零内存泄漏与 HTTP/WebSocket 协议健全性加固设计

> 状态：提案 (Brainstorming 完成，待 Spec Reviewer 验证与用户批准)  
> 日期：2026-10-30  
> 责任模块：`src/transport/async_server.c`, `include/mcpkit/transport/async_server.h`, `tests/CMakeLists.txt`, `cmake/MCPKitOptions.cmake`, `CMakeLists.txt`, `README.md`

---

## 1. 目标与背景 (Intent & Why)

### 1.1 背景
在上一阶段演进中，`mcpkit-c` 引入了基于 Reactor 事件循环的高并发异步服务 `mcp_async_server_t`（位于 [`src/transport/async_server.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/transport/async_server.c)）以及 RFC 6455 原生 WebSocket 编解码（位于 [`src/transport/ws.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/transport/ws.c)）。

在近期的深度代码审查与并发边缘压力测试中，发现以下关键技术债与稳定性缺陷：
1. **连接与缓冲池永久泄漏**：`on_accept` 创建连接对象 `async_conn_t`（初始 `ref_count=1`）并压入 `s->conns` 链表。当连接断开时，系统虽关闭了 socket 并移出 reactor，**但从未从 `s->conns` 链表中脱钩，也未释放初始持有计数**。导致长跑服务器上的每一个历史连接对象及其高达 4MB 的 `read_buf` 永久驻留，高频请求下内存无上限线性泄露。
2. **HTTP 分包误杀与粘包截断**：
   - 现行实现只要在缓冲区检索到 `\r\n\r\n` 就判定请求到达并调用 `process_http_request()`；当客户端发送带 Body 的 POST 请求且 Body 跨 TCP 包时，底层 HTTP 解析器由于数据不足返回 `NULL`，服务端直接粗暴地向客户端返回 `400 Bad Request` 并关闭连接。
   - 请求处理完成后直接将 `conn->read_len = 0;` 重置，若同一 TCP 连接中有 HTTP Pipelining 或后续请求粘包数据，未消费的数据被直接清空丢弃。此外，多请求并发派发可能导致违反 RFC 9112 严格串行应答规范。
3. **Session 缺乏并发同步与超时淘汰巡检（Session Leak & UAF 隐患）**：`mcp_async_server_config_t` 配置了 `keepalive_timeout_s = 60`，代码中亦记录了 `last_active`，但从未有定时巡检逻辑释放过期会话。此外，正在线程池 Worker 中执行的慢任务持有原始 session 指针，缺乏生命周期保护，直接释放会触发 Use-After-Free。同时 SSE 连接断开未反向置空 `entry->sse_conn`，会导致悬挂指针崩溃；若在锁内直接断开 SSE 会引发 `s->sessions_mtx` 互斥锁重入死锁。
4. **WebSocket 缺失 Continuation 帧重组**：RFC 6455 §5.4 规定长消息可以被拆分为多个连续帧传输。底层 `ws.c` 已支持 Continuation 帧解析，但 `async_server.c` 缺少分片缓冲拼装状态机与容量上限约束，收到分片帧时会报错失败。
5. **构建缺少制度化警告约束与死选项**：CMake 配置未统一加上 `-Wall -Wextra`，`cmake/MCPKitOptions.cmake` 存在未使用的死选项（`MCPKIT_BUILD_APPS`、`MCPKIT_BUILD_CLIENT`、`MCPKIT_BUILD_SERVER`）。

### 1.2 目标
本方案旨在彻底解决上述稳定性缺陷与技术债，使 `mcp_async_server_t` 达到真正的生产级高可用：
- 实现**连接与内存零泄漏**，在压力测试（数百次连接/断开循环）下验证内存绝对平稳、活跃连接计数归零；
- 健全 HTTP 接收流式状态机，彻底消除分包误判与粘包丢失，确保连接级严格串行响应顺序；
- 补全 WebSocket Continuation 帧重组（含防 DoS 保护与协议冲突检测）与线程安全、死锁免疫的 Session 定时超时清理；
- 制度化 `-Wall -Wextra` 编译门禁并保持全绿，清理失效 CMake 选项。

---

## 2. 目标、非目标与反目标 (Goals, Non-Goals, Anti-Goals)

### 2.1 目标 (Goals)
1. **确定性连接生命周期**：连接关闭时在 $O(1)$ 时间复杂度内从全局连接池摘除，平衡初始引用计数，并在引用归零时彻底释放连接对象和读写缓冲区。独立维护 `closing`（写完后断开标记）与 `detached`（原子幂等脱钩标记）。
2. **流式 HTTP 两阶段解析与严格保序处理**：
   - 仅当接收到的数据同时满足“头部完整（出现 `\r\n\r\n`）”与“负载完整（已到达字节数 $\ge$ `header_len + Content-Length`）”时才触发业务分发；
   - 分发消费精准的字节数，利用滑动窗口（`memmove`）保留粘包数据；单连接保持单一活动任务，响应由 Reactor 线程在写完成时驱动后续请求，确保响应与到达严格保序。若为 101 WebSocket Upgrade 且缓冲区有粘包帧，立即流转至 WS 帧处理。
3. **Session Timeout 自动巡检与引用计数安全**：对 `session_entry_t` 引入引用计数机制（`ref_count`），`find_session` 在临界区内增持引用，由 `worker_dispatch_fn` 在任务完全结束时释放，消除 Use-After-Free；巡检断开 SSE 采取锁外安全执行与定额批次处理，规避互斥锁重入死锁与栈溢出。
4. **WebSocket 分片帧支持**：支持 RFC 6455 §5.4 标准分片重组，对乱序交错帧报错断开，支持零负载 Continuation 结束帧，并严格施加 4MB 最大缓冲容量限制。
5. **严苛的门禁保障**：保持 C23 零依赖，通过全部 69+ 项已有测试，新增针对分包、内存泄漏、会话超时与 WebSocket 分片的单元测试，并在 ASan+UBSan 及 `-Wall -Wextra` 下无泄漏无告警。

### 2.2 非目标 (Non-Goals)
1. **跨平台 Reactor POSIX poll 重构**：此工作划入第二阶段专项，本期专注网络核心稳定性与零泄漏。
2. **公开 API 破坏性改动**：保持 `include/mcpkit/transport/async_server.h` 与 `ws.h` 的既有公开函数签名稳定向后兼容，仅新增非破坏性的测试探针函数。
3. **HTTP/2 或 HTTP/3 支持**：继续专注 RFC-compliant HTTP/1.1 SSE 与 WebSocket。

### 2.3 反目标 (Anti-Goals)
1. **严禁破坏线程安全模型**：网络 I/O 线程与 Worker 线程间的锁职责与生命周期交接必须严谨，禁止因引用计数错误引发 Use-After-Free 或锁竞争死锁。禁止 Worker 线程直接跨线程修改 Reactor 线程私有的读缓冲区状态。
2. **严禁在未收到完整包前分配超大内存**：HTTP Body 超过限额（4MB）或未完成传输时不得进行无节制的内存重新分配。

---

## 3. 详细设计 (Detailed Design)

### 3.1 连接生命周期与链表脱钩 (Connection Lifecycle & Zero-Leak)

#### 3.1.1 双向链表与服务器统计结构调整
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

    atomic_bool closing;   /* True when connection should close after pending writes */
    atomic_bool detached;  /* Atomic idempotency guard for detachment and fd closure */
    bool http_in_flight;   /* Reactor-thread-owned: true when an HTTP request is dispatched and waiting for write completion */

    /* Doubly-linked list for O(1) removal from s->conns */
    async_conn_t *prev;
    async_conn_t *next;
};

struct mcp_async_server {
    mcp_context_t *ctx;
    mcp_server_t *server;
    mcp_executor_t *pool;
    mcp_async_server_config_t cfg;

    int listen_fd;
    uint16_t bound_port;
    mcp_reactor_t *reactor;

    atomic_bool running;
    atomic_bool stop_requested;

    mtx_t resp_mtx;
    response_item_t *resp_head;
    response_item_t *resp_tail;

    mtx_t sessions_mtx;
    session_entry_t *sessions;
    time_t last_reap_time; /* Timestamp of the last session reaper pass */

    mtx_t conns_mtx;
    async_conn_t *conns;
    size_t conns_count;    /* Number of active connections tracked in s->conns */
};
```

#### 3.1.2 确定性脱钩与销毁逻辑
区分 `closing`（写完后断开标志）与 `detached`（仅脱钩一次的原子幂等标志）：
实现私有辅助函数 `conn_detach_and_close(mcp_async_server_t *s, async_conn_t *conn)`：
1. 若 `atomic_exchange(&conn->detached, true)` 已为 `true`，说明此前已执行过脱钩，直接返回（防止重复脱钩与二次 close）；
2. 标记 `atomic_store(&conn->closing, true)`；
3. 若 `conn->fd >= 0`：
   - `mcp_reactor_remove(s->ctx, s->reactor, conn->fd)`；
   - `close(conn->fd)`；
   - `conn->fd = -1`；
4. 若连接为 `CONN_SSE` 且记录了 `conn->session_id[0] != '\0'`：
   - 加锁 `mtx_lock(&s->sessions_mtx)`；
   - 检索对应的 `session_entry_t`，若 `entry->sse_conn == conn`，将其置空 `entry->sse_conn = NULL`；
   - 解锁 `mtx_unlock(&s->sessions_mtx)`；
5. 加锁 `mtx_lock(&s->conns_mtx)`：
   - 将 `conn` 从 `s->conns` 双向链表中脱钩；
   - 更新 `s->conns_count--` 统计；
   - 解锁 `mtx_unlock(&s->conns_mtx)`；
6. 调用一次 `conn_unref(conn)`，冲抵 `on_accept` 分配连接时的基准计数（`ref_count=1`）；
7. 最终在 `conn_unref` 中：当引用归零时，释放 `read_buf`、`write_buf`、`ws_frag_buf`、关闭 WS 私有会话并释放 `conn` 结构体内存。

#### 3.1.3 服务器销毁规范
在 `mcp_async_server_destroy` 中：
- 遍历 `s->conns` 时，调用 `conn_detach_and_close`，确保每个连接被正确脱钩、关闭 socket 并减持基准引用；
- 遍历 `s->sessions` 时，对每个 Session 调用 `session_entry_unref`，统一资源清理路径。

#### 3.1.4 测试探针 API
在 `include/mcpkit/transport/async_server.h` 中提供只读观察函数，供测试验证连接完全释放：
```c
/**
 * @brief Returns the current number of active connections tracked by the server.
 * @param s Async server handle; may be NULL.
 * @return Active connection count, or 0 if s is NULL.
 */
size_t mcp_async_server_active_connections(const mcp_async_server_t *s);
```

---

### 3.2 HTTP 两阶段流式解析与严格保序处理 (Streaming HTTP State Machine)

#### 3.2.1 头部定位、Content-Length 提取与缓冲区限制
在 `src/transport/async_server.c` 内部封装轻量、完全自包含的私有静态辅助函数，避免跨文件符号耦合：
1. **定义硬上限常量**：`#define MCP_ASYNC_MAX_BODY (4u << 20)` (4MB), `#define MCP_ASYNC_MAX_READ_CAP (MCP_ASYNC_MAX_BODY + 65536u)` (4MB + 64KB Headers)；
2. **读缓冲区扩容策略**：在 `handle_client_read` 中，当需要扩容时：
   ```c
   size_t ncap = conn->read_cap == 0 ? 8192 : conn->read_cap * 2;
   if (ncap > MCP_ASYNC_MAX_READ_CAP) {
       ncap = MCP_ASYNC_MAX_READ_CAP;
   }
   if (conn->read_len + 4096 > conn->read_cap && conn->read_cap >= MCP_ASYNC_MAX_READ_CAP) {
       /* 确实超出硬上限，关闭连接防 OOM */
       conn_detach_and_close(s, conn);
       return;
   }
   ```
3. **搜索 Headers 结束标志**：在 `conn->read_buf`（长度 `conn->read_len`）中搜索 `\r\n\r\n`。若未搜到，直接返回（等待下一次 `read` 事件）；
4. **计算头部长度**：`header_len = (size_t)(crlfcrlf + 4 - conn->read_buf)`；
5. **提取 `Content-Length`**：
   - 大小写不敏感扫描以 `\nContent-Length:` 开始的头部行；
   - 若不存在该字段，则 `content_len = 0`；
   - 若存在，提取数值字符串（跳过前导空格与制表符）。若遇到非数字字符、负数或格式错误，立即调用 `send_http_error(s, conn, 400, "Bad Request")` 并退出；
6. **校验长度上限**：
   - 检查 `content_len > MCP_ASYNC_MAX_BODY`，若超出，立即响应 `send_http_error(s, conn, 413, "Payload Too Large")` 并标记 `conn->closing = true`；
   - 计算 `total_needed = header_len + content_len`；
   - 若 `total_needed > MCP_ASYNC_MAX_READ_CAP`，返回 413 错误；
7. **判断 Body 完整性**：
   - 若 `conn->read_len < total_needed`：**Body 分包尚未传输完结，不报错、不重置缓冲区，直接 return，保持连接继续读取**；
   - 若 `conn->read_len >= total_needed`：数据完整到达，可进行请求处理与调度。
8. **解析传参规范**：
   调用 `mcp_http_parse_request` 时，**必须传入 `total_needed` 作为长度参数**（`mcp_http_parse_request(ctx, conn->read_buf, total_needed)`），杜绝缓冲区中后续粘包请求数据干扰当前请求的解析。

#### 3.2.2 严格保序与滑动窗口消费 (RFC 9112 Ordering & Sliding Window)
1. **严格有序保序（Single in-flight request per TCP connection）**：
   - `conn->http_in_flight` 为 Reactor 线程私有标记；
   - 若当前连接已标记 `conn->http_in_flight == true`（前一个异步 POST 正在执行或排队写回），Reactor 暂不启动新请求分发，剩余粘包数据完整保留在 `conn->read_buf` 滑动窗口中；
   - **清除时机（消除并发竞争）**：`conn->http_in_flight = false` **仅由 Reactor 线程在 `handle_client_read` 的写完成分支（`conn->write_off >= conn->write_len`）清除**！此时底层 socket 已完成上一响应发送，Reactor 立即就地触发检查 `conn->read_buf` 中的剩余粘包数据，实现真正的串行流水线。
2. **滑动窗口消费**：
   当请求被分发提交（或立即完成响应）后：
   ```c
   size_t remaining = conn->read_len - total_needed;
   if (remaining > 0) {
       memmove(conn->read_buf, conn->read_buf + total_needed, remaining);
   }
   conn->read_len = remaining;
   conn->read_buf[conn->read_len] = '\0';
   ```
   - 若该请求为 WebSocket Upgrade（101 Switching Protocols），且 `conn->read_len > 0`，连接模式已切换为 `CONN_WS`，立即直接进入 WebSocket 帧解析循环处理首批携带的 WS 帧，无需等待下一次 epoll 唤醒；
   - 若连接未处于关闭状态且 `conn->read_len > 0` 且 `!conn->http_in_flight`，循环继续解析下一个完整的 HTTP 请求。

---

### 3.3 WebSocket Continuation 帧重组与防 DoS (RFC 6455 §5.4)

底层 `src/transport/ws.c` 已支持 `mcp_ws_parse_frame` 的 Continuation 帧解析，其 API 保持不变；分片拼装状态机与容量约束完全在 `src/transport/async_server.c` 内部实现。

#### 3.3.1 状态机设计
在 `handle_client_read` 的 `CONN_WS` 分支：
1. `mcp_ws_parse_frame` 正常提取单帧；
2. **控制帧优先就地处理**：
   - Ping/Pong/Close 可以在分片帧序列之间穿插传输；
   - 收到 Ping 立即回应 Pong；收到 Close 立即调用 `conn_detach_and_close`，不破坏正在拼装的 `ws_frag_buf`；
3. **起始分片帧**（`opcode == TEXT || opcode == BINARY`，且 `fin == false`）：
   - 协议检查：若 `conn->ws_frag_buf != NULL`，说明前序分片未完结又发来了新的起始帧，严重违反 RFC 6455 §5.4。立即调用 `conn_detach_and_close(s, conn)`；
   - 初始化分片缓冲区：分配并复制负载，记录 `conn->ws_frag_opcode = frame.opcode`；
4. **后继续帧**（`opcode == CONTINUATION`）：
   - 协议检查：若 `conn->ws_frag_buf == NULL`，说明未收到首帧却收到了 Continuation 帧，直接判定协议违规并关闭连接；
   - 防 DoS 扩容检查：计算拼接后长度 `new_len = conn->ws_frag_len + frame.payload_len`；若 `new_len > (4u << 20)`，立即关闭连接拒绝大包 DoS；
   - 拼接写入负载，末尾预留 1 字节始终填充 `\0` 以保证 JSON 字符串解析安全；
   - 若 `fin == true`（含 `payload_len == 0` 的纯结束标记帧）：
     - 说明完整消息拼装完毕；
     - 将 `conn->ws_frag_buf` 移交给 `worker_task_t->json_body`（转移所有权）；
     - 重置 `conn->ws_frag_buf = NULL`，`conn->ws_frag_len = 0`，`conn->ws_frag_cap = 0`；
     - 提交 Worker 线程池异步执行；
5. **普通无分片帧**（`opcode == TEXT || opcode == BINARY`，且 `fin == true`）：
   - 协议检查：若 `conn->ws_frag_buf != NULL`，说明在等待 Continuation 帧期间收到了非分片数据帧，判定协议错误并断开连接；
   - 正常提交执行。

---

### 3.4 线程安全、死锁免疫的会话超时淘汰巡检 (Deadlock-Free Session Reaper)

#### 3.4.1 会话引用计数（Ref-Counted Session Entry）
在 [`src/transport/async_server.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/transport/async_server.c) 中为 `session_entry_t` 增加引用计数与生命周期标识：
```c
typedef struct session_entry {
    char id[64];
    mcp_session_t *session;
    async_conn_t *sse_conn;
    time_t last_active;
    atomic_uint ref_count;   /* 1 (held by s->sessions) + N (held by active worker tasks) */
    atomic_bool dead;        /* True if unlinked by reaper */
    mtx_t session_lock;
    struct session_entry *next;
} session_entry_t;
```

#### 3.4.2 引用所有权流转模式 (Eliminating Use-After-Free)
1. `session_entry_ref(entry)`：`atomic_fetch_add(&entry->ref_count, 1)`；
2. `session_entry_unref(mcp_async_server_t *s, session_entry_t *entry)`：
   - 若 `atomic_fetch_sub(&entry->ref_count, 1) == 1`：
     - 说明该 Session 已从 `s->sessions` 脱钩且所有正在运行的 Worker 均已退出；
     - 安全调用 `mcp_server_destroy_session(s->ctx, s->server, entry->session)`；
     - `mtx_destroy(&entry->session_lock)`；
     - 释放 `entry` 结构体内存；
3. **消除 TOCTOU 竞争与流转规则**：
   - `find_session` 在持有 `mtx_lock(&s->sessions_mtx)` 的临界区内，找到匹配节点时**立即调用 `session_entry_ref(cur)`**，然后再解锁返回；
   - `create_session_entry` 在初始化节点时，基准引用计数设为 1（代表由 `s->sessions` 链表持有），在返回给请求调用方时再执行一次 `session_entry_ref`；
   - **对于 HTTP POST 请求**：获取的递增引用转移给 `worker_task_t`；**`session_entry_unref(s, task->entry)` 严格规定在 `worker_dispatch_fn` 执行结束、释放 task 之前调用**！
   - 若在 `process_http_request` 中分配 task 失败（OOM 等），调用方就地执行 `session_entry_unref(s, entry)`；
   - **对于 GET SSE 请求**：获取 `s->sessions_mtx` 锁后赋值 `entry->sse_conn = conn` 并解锁，随后调用方立即就地执行 `session_entry_unref(s, entry)`（该连接已由 `s->sessions` 的基准引用和 `entry->sse_conn` 持续维护）。

#### 3.4.3 死锁免疫与批次定额巡检 (Lock-Free Disconnection & Batch Reaper)
1. 在 `mcp_async_server_t` 结构体中记录 `time_t last_reap_time`；
2. 在 `mcp_async_server_step` 中：若 `time(NULL) - s->last_reap_time >= 5`（每 5 秒一次），更新时间戳并触发 `reap_expired_sessions(s)`；
3. **`reap_expired_sessions(s)` 批次执行规范（防栈溢出与死锁）**：
   - 定义批次上限 `#define MCP_SESSION_REAP_BATCH 32`；
   - 准备局部临时收集队列：`async_conn_t *sse_to_close[MCP_SESSION_REAP_BATCH]; size_t count = 0;`；
   - 加锁 `mtx_lock(&s->sessions_mtx)`；
   - 遍历 `s->sessions` 链表，当 `count < MCP_SESSION_REAP_BATCH` 时：
     - 若 `now - entry->last_active > s->cfg.keepalive_timeout_s`：
       - 将该 `entry` 从 `s->sessions` 链表中摘除；
       - 标记 `atomic_store(&entry->dead, true)`；
       - 收集关联的 SSE 连接指针并立即在锁内置空：`if (entry->sse_conn != NULL) { sse_to_close[count++] = entry->sse_conn; entry->sse_conn = NULL; }`；
       - 调用一次 `session_entry_unref(s, entry)`（释放 `s->sessions` 的基准引用）；
   - **立即解锁** `mtx_unlock(&s->sessions_mtx)`；
   - **在临界区外**，遍历 `sse_to_close` 临时数组（共 `count` 个），逐一调用 `conn_detach_and_close(s, sse_to_close[i])`，彻底杜绝在 `s->sessions_mtx` 锁内再次申请该锁导致的自锁死锁；若超出 32 个，剩余过期会话安全保留在链表中，在下一个 5 秒周期平滑淘汰，杜绝栈溢出与连接孤立泄漏。

---

### 3.5 构建配置与代码卫生规范

1. **启用警告门禁**：
   在根目录 `CMakeLists.txt` 中添加：
   ```cmake
   if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
     add_compile_options(-Wall -Wextra)
   endif()
   ```
2. **清理死选项**：
   在 `cmake/MCPKitOptions.cmake` 中彻底删除未被任何 CMake 逻辑读取的三个死选项：
   - 移除 `option(MCPKIT_BUILD_APPS ...)`
   - 移除 `option(MCPKIT_BUILD_CLIENT ...)`
   - 移除 `option(MCPKIT_BUILD_SERVER ...)`
   并在 `README.md` 构建选项表中移除这三行废弃描述。

---

## 4. 测试与验证计划 (Testing & Verification Plan)

### 4.1 新增单元测试
在 `tests/unit/` 中新增针对性测试并注册至 `tests/CMakeLists.txt`：
1. **`test_async_leak`（连接生命周期与零泄漏门禁）**：
   - 启动本地 `mcp_async_server`；
   - 循环创建 300 次短连接（发送短 HTTP GET / POST 请求后关闭 socket）；
   - 循环结束后执行 `mcp_async_server_step(ctx, s, 50)`；
   - 断言 `mcp_async_server_active_connections(s) == 0`；
   - 销毁服务器并在 ASan / UBSan 监控下验证零内存泄漏。
2. **`test_async_fragmentation`（分包与粘包仿真测试）**：
   - 测试 A（分包）：分两次向 server 发送单个 POST 请求（先发 Header + 4 字节 Body，休眠 15ms，再发剩余 Body），断言成功收到 200 OK，绝不返回 400。
   - 测试 B（粘包/Pipelining）：单次 `send` 发送连续两个完整 HTTP POST 请求，断言按序成功收到两个独立的 HTTP 响应。
3. **`test_ws_continuation`（WebSocket 分片重组测试）**：
   - 构造分片帧序列：Text 帧（fin=0）+ Continuation 帧（fin=0）+ 空负载 Continuation 结束帧（fin=1, payload_len=0）；
   - 发送给 WebSocket 服务端，断言服务端正常拼装并返回预期的 JSON-RPC 结果；
   - 异常测试：发送未初始化的 Continuation 帧或超大分片帧，验证连接被安全关闭。
4. **`test_async_session_expiry`（会话超时巡检与线程安全测试）**：
   - 启动服务器，配置 `keepalive_timeout_s = 1`；
   - 建立初始化会话，休眠 2 秒并驱动 `step` 巡检；
   - 断言后续请求返回 404 Session Not Found；
   - 模拟挂起长连接与慢任务期间 Session 超时，断言 Worker 退出与 Reaper 巡检时无崩溃、无死锁、无 UAF。

### 4.2 质量门禁 (Quality Gates)
- 全量 69+ 项已有测试 100% 通过；
- 新增 4 个单元测试全量通过；
- `-Wall -Wextra` 零告警；
- AddressSanitizer (ASan) 与 UndefinedBehaviorSanitizer (UBSan) 全绿，无任何泄漏或未定义行为。
