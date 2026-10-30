# 高并发异步 HTTP/SSE 驱动与 WebSocket 传输层设计

> 状态：提案 (Brainstorming 完成，待 Builder Check 与用户批准)  
> 日期：2026-10-30  
> 责任团队：mcpkit-c 核心组  

---

## 1. 目标与背景 (Intent & Why)

### 1.1 背景
`mcpkit-c` 是面向 C23 的模块化、生产级 MCP (Model Context Protocol) SDK 与运行时，其核心设计准则为**零外部依赖（Zero dependencies: libc only）**。

在目前的传输层实现中：
- **HTTP / SSE**（[`src/transport/streamable_http.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/transport/streamable_http.c)）依赖顺序同步的抽象 I/O 接口 [`mcp_http_io_t`](file:///data/home/quintin/workspace/source/c/mcpkit-c/include/mcpkit/transport/streamable_http.h#L30-L37)，具有固定 16 会话槽位上限，且现有示例仅通过 stdin/stdout 进行文件流模拟；
- **TCP Socket**（[`src/transport/socket.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/transport/socket.c)）采用“主线程 poll + 每个连接占用一个线程池 Worker”的阻塞调度模型。对于瞬态请求尚可工作，但面对长连接（SSE 推送长轮询或全双工 WebSocket），每个连接独占一个操作系统线程会导致线程迅速耗尽；
- **WebSocket 缺失**：仓库内目前尚无任何 RFC 6455 WebSocket 协议实现。

### 1.2 目标
为 `mcpkit-c` 构建一套兼具高性能与零依赖的高并发异步网络栈：
1. **统一网络反应器（Reactor）**：在底层提供基于非阻塞 I/O 的轻量事件多路复用（Linux `epoll` + POSIX `poll` 回退），实现单线程驱动海量连接；
2. **高并发 Streamable HTTP / SSE 服务端**：支持非阻塞 HTTP/1.1 解析、动态会话生命周期管理、以及无线程占用的 SSE 事件广播；
3. **原生 RFC 6455 WebSocket 传输层**：实现完整的 HTTP 握手升级（`Upgrade: websocket`）、帧编解码（含掩码与分片）、Ping/Pong 保活，并包装为标准的 `mcp_transport_t`；
4. **单端口多协议统一入口**：支持在同一个 TCP 端口上同时承载 HTTP POST、SSE 长连接与 WebSocket 升级，自动识别并分流。

---

## 2. 目标、非目标与反目标 (Goals, Non-Goals, Anti-Goals)

### 2.1 目标 (Goals)
1. **C10K 级长连接能力**：SSE 与 WebSocket 连接在空闲或待接收状态下不占用任何执行线程，仅维护轻量连接结构体与 socket fd。
2. **零外部依赖 (Zero External Dependencies)**：全部采用 C23 标准与 POSIX 系统调用（`sys/epoll.h` / `poll.h` / `sys/socket.h`），核心库保持纯 libc/POSIX 编译。
3. **极简握手加密支持**：内置约 80 行纯 C 的 RFC 3174 SHA-1 算法与 Base64 编码，自包含完成 RFC 6455 `Sec-WebSocket-Accept` 握手。
4. **统一分层与所有权**：
   - 保持 Layering 纪律：Reactor 位于 `runtime/`，HTTP/WS 位于 `transport/`，核心依赖单向内聚；
   - 暴露标准 `mcp_transport_t` 接口，无缝接入现有的 `mcp_server_t` 与 `mcp_client_t`。
5. **完整测试与健壮性**：单元测试、集成并发测试（100+ 并发连接）、ASan / UBSan 验证无泄漏无越界，`-Wall -Wextra` 零警告。

### 2.2 非目标 (Non-Goals)
1. **核心库集成 TLS**：TLS 保持独立设计，继续通过 `MCPKIT_BUILD_TLS`（OpenSSL）或建议在生产中使用反向代理（如 Nginx / Caddy）提供安全层。
2. **HTTP/2 或 HTTP/3 支持**：MCP 规范明确采用 HTTP/1.1 Streamable HTTP (SSE) 与 WebSocket，暂不引入复杂的 HTTP/2 二进制分帧。
3. **外部网络库强制绑定**：不以 `libuv` 或 `libevent` 为默认核心，第三方库仅作为 `src/adapters/` 的可选外挂。
4. **2024 旧版 SSE 兼容**：严格遵循最新 MCP Streamable HTTP 规范，不向下兼容 2024 旧版通过 SSE 初始帧推送 `endpoint` 地址的废弃方案。

### 2.3 反目标 (Anti-Goals)
1. **禁止在 Reactor 线程阻塞**：任何耗时、阻塞的 Tool Handler 或复杂 JSON 计算不得阻塞网络 I/O 线程，必须交由 `mcp_executor_t` 线程池异步完成。
2. **禁止破坏既有 API 兼容性**：现有的 `mcp_transport_t`、`mcp_socket_serve`、`mcp_loop_run` 必须保持原样可用。

---

## 3. 架构方案抉择 (Approaches Considered)

| 方案 | 机制 | 依赖要求 | 并发模型 | 最终决策 |
|---|---|---|---|---|
| **方案一：原生零依赖 Reactor（选用）** | POSIX `epoll` + `poll` 回退，自研非阻塞状态机与 RFC 6455 编解码 | **零依赖**（纯 libc / POSIX） | 单线程 I/O 多路复用 + 线程池 Worker 计算 | **采纳**。完美契合 mcpkit-c 核心哲学，开箱即用，资源消耗最低 |
| **方案二：外部 Adapter（libuv / libevent）** | 通过可选插件引入第三方成熟事件库 | 需安装系统开发库（libuv-dev） | 事件库线程模型 | **留存为未来可选适配器**，不作为核心实现 |
| **方案三：线程池连接复用 (Thread-per-connection 优化)** | 沿用已有 socket 线程池，增加 select 超时与池扩容 | **零依赖** | 阻塞/半阻塞多线程 | **否决**。在数千个 SSE/WS 并发长连接下内存开销与线程切换无法承受 |

---

## 4. 详细技术设计 (Detailed Design)

### 4.1 核心组件划分

```text
┌─────────────────────────────────────────────────────────────┐
│                       mcp_server_t                          │
└──────────────┬───────────────────────────────┬──────────────┘
               │                               │
               ▼                               ▼
    ┌──────────────────────┐        ┌──────────────────────┐
    │  Async HTTP/SSE Svc  │        │  WebSocket Transport │
    │ (mcp_async_http_t)   │        │   (mcp_ws_t)         │
    └──────────┬───────────┘        └──────────┬───────────┘
               │                               │
               └───────────────┬───────────────┘
                               ▼
               ┌───────────────────────────────┐
               │    mcp_reactor_t (epoll/poll) │
               │   Non-blocking I/O & Timers   │
               └───────────────┬───────────────┘
                               ▼
               ┌───────────────────────────────┐
               │  POSIX TCP Sockets (Kernel)   │
               └───────────────────────────────┘
```

#### A. 运行时事件反应器：`mcpkit/runtime/reactor.h`
```c
typedef struct mcp_reactor mcp_reactor_t;
typedef uint32_t mcp_event_mask_t;

#define MCP_EVENT_READ   (1u << 0)
#define MCP_EVENT_WRITE  (1u << 1)
#define MCP_EVENT_ERROR  (1u << 2)

typedef void (*mcp_reactor_cb_t)(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user);

mcp_reactor_t *mcp_reactor_create(mcp_context_t *ctx);
void           mcp_reactor_destroy(mcp_context_t *ctx, mcp_reactor_t *r);

mcp_status_t   mcp_reactor_add(mcp_context_t *ctx, mcp_reactor_t *r, int fd,
                               mcp_event_mask_t events, mcp_reactor_cb_t cb, void *user);
mcp_status_t   mcp_reactor_modify(mcp_context_t *ctx, mcp_reactor_t *r, int fd,
                                  mcp_event_mask_t events);
mcp_status_t   mcp_reactor_remove(mcp_context_t *ctx, mcp_reactor_t *r, int fd);

/* 运行一次或单步循环，timeout_ms < 0 为无限阻塞 */
mcp_status_t   mcp_reactor_step(mcp_context_t *ctx, mcp_reactor_t *r, int timeout_ms);
mcp_status_t   mcp_reactor_run(mcp_context_t *ctx, mcp_reactor_t *r);
void           mcp_reactor_stop(mcp_context_t *ctx, mcp_reactor_t *r);

/* 跨线程唤醒：通过 eventfd (Linux) 或 self-pipe (POSIX) 唤醒阻塞在 epoll 中的 reactor */
mcp_status_t   mcp_reactor_wakeup(mcp_context_t *ctx, mcp_reactor_t *r);
```

#### B. RFC 6455 WebSocket 协议层：`mcpkit/transport/ws.h`
```c
typedef enum {
    MCP_WS_OP_CONTINUATION = 0x0,
    MCP_WS_OP_TEXT         = 0x1,
    MCP_WS_OP_BINARY       = 0x2,
    MCP_WS_OP_CLOSE        = 0x8,
    MCP_WS_OP_PING         = 0x9,
    MCP_WS_OP_PONG         = 0xA
} mcp_ws_opcode_t;

typedef struct mcp_ws_frame {
    bool fin;
    mcp_ws_opcode_t opcode;
    bool masked;
    uint8_t mask_key[4];
    uint64_t payload_len;
    char *payload; /* owned or borrowed */
} mcp_ws_frame_t;

/* 帧编解码器 */
mcp_status_t mcp_ws_parse_frame(mcp_context_t *ctx, const uint8_t *buf, size_t len,
                                mcp_ws_frame_t *frame_out, size_t *consumed_out);
mcp_status_t mcp_ws_encode_frame(mcp_context_t *ctx, const mcp_ws_frame_t *frame,
                                 uint8_t **bytes_out, size_t *len_out);

/* 握手辅助：输入 Sec-WebSocket-Key，输出 Base64 编码的 Sec-WebSocket-Accept */
mcp_status_t mcp_ws_compute_accept_key(const char *client_key, char accept_buf[32]);

/* 客户端连接辅助：解析 ws:// URL 并完成 HTTP Upgrade 握手，返回就绪的 mcp_transport_t */
mcp_transport_t *mcp_ws_client_connect(mcp_context_t *ctx, const char *url);

/* 创建基于已有已连接/已升级 socket fd 的标准同步 mcp_transport_t（供客户端或单连接服务使用） */
mcp_transport_t *mcp_ws_transport_create(mcp_context_t *ctx, int fd, bool is_server);
```

#### C. 统一异步 HTTP/SSE/WS 服务管理器：`mcpkit/transport/async_server.h`
```c
typedef struct mcp_async_server mcp_async_server_t;

typedef struct {
    uint16_t port;
    const char *bind_addr;        /* default "0.0.0.0" */
    size_t max_connections;       /* default 10000 */
    uint32_t keepalive_timeout_s; /* default 60 */
    bool enable_websocket;        /* default true */
    bool enable_sse;              /* default true */
} mcp_async_server_config_t;

/* pool 为必须参数（传 NULL 返回 MCP_ERR_INVALID_ARGUMENT），由调用方管理生命周期 */
mcp_async_server_t *mcp_async_server_create(mcp_context_t *ctx, mcp_server_t *server,
                                            mcp_executor_t *pool,
                                            const mcp_async_server_config_t *cfg);
void                mcp_async_server_destroy(mcp_context_t *ctx, mcp_async_server_t *s);

/* 启动并在当前线程运行 Reactor 事件循环（直到 mcp_request_shutdown） */
mcp_status_t        mcp_async_server_run(mcp_context_t *ctx, mcp_async_server_t *s);
void                mcp_async_server_stop(mcp_context_t *ctx, mcp_async_server_t *s);
```

---

### 4.2 单端口统一协议分流状态机

每个新接入的客户端 socket 设置为 `O_NONBLOCK`，生命周期经历以下阶段：

1. **连接建立**：`accept()` 获取 fd，设置非阻塞，注册 `MCP_EVENT_READ` 到 `mcp_reactor_t`；
2. **首包侦测**：
   - 使用环形/分段读缓冲接收 HTTP 首部；
   - 调用 [`mcp_http_parse_request`](file:///data/home/quintin/workspace/source/c/mcpkit-c/include/mcpkit/transport/http.h#L52) 解析方法和 Headers；
3. **协议升迁与请求路由**：
   - **WebSocket 升级**：若请求包含 `Upgrade: websocket`，校验 `Sec-WebSocket-Key`，计算 `Sec-WebSocket-Accept`，回写 `101 Switching Protocols`。连接接管权由 HTTP 解析器移交至 WebSocket 帧状态机；
   - **Streamable HTTP POST**：
     - 若为未携带 `Mcp-Session-Id` 的 `POST` 请求，仅当其方法为 `initialize` 时合法：分配全局唯一会话 ID，派发执行并在 HTTP 响应头附带 `Mcp-Session-Id: <id>`；
     - 若为后续业务调用 `POST`，校验请求头中的 `Mcp-Session-Id` 是否存在于会话表：命中则将请求作为任务提交至 `mcp_executor_t` 线程池（排入会话级 FIFO 挂起队列串行化派发），执行完毕后由 `mcp_reactor_wakeup()` 唤醒 Reactor 回写 HTTP 响应；未命中返回 `404 Not Found`；
   - **Streamable HTTP SSE 挂载 (GET)**：
     - 当且仅当 `GET` 携带有效 `Mcp-Session-Id` 且 `Accept` 包含 `text/event-stream` 时合法，回复 `200 OK`（含 `Content-Type: text/event-stream` 及长连接头），将该连接关联至会话的推送管道；
     - 无会话 ID 或非法会话 ID 的 `GET` 请求一律直接拒绝（返回 `400 Bad Request` 或 `404 Not Found`）；
   - **会话销毁 (DELETE)**：
     - 客户端发送带 `Mcp-Session-Id` 的 `DELETE` 请求，或会话空闲超时（超过 `keepalive_timeout_s`），服务端触发安全销毁。

---

### 4.3 关键并发与生命周期规范 (Conventions & Safety Details)
1. **连接生命周期与防 UAF**：连接上下文结构体维护 `atomic_uint ref_count`。Worker 执行任务期间持有连接引用；Reactor 在 socket 断开或对端关闭时仅从 epoll 摘除 fd，直至引用计数归零才释放内存。
2. **Worker 与 Reactor 通信**：`mcp_async_server_t` 内部维护互斥保护的 MPSC 响应队列。Worker 线程完成执行后压入响应队列，并调用 `mcp_reactor_wakeup()` 唤醒 Reactor 执行非阻塞写回。
3. **WebSocket 架构双层分工**：`src/transport/ws.c` 负责 RFC 6455 帧编解码与阻塞式 `mcp_transport_t` 封装（主要供给客户端与单连接测试）；`mcp_async_server.c` 直接基于 Reactor 事件驱动该帧解析器，不依赖阻塞 transport。
4. **客户端 WebSocket 辅助**：提供 `mcp_ws_client_connect(ctx, url)`，在 `transport_ops->start()` 中自动完成 TCP 连接与 HTTP Upgrade 握手及密钥校验。
5. **会话串行化与线程安全**：同一会话（Session）的多个请求通过会话级 FIFO 挂起队列串行化派发至 Worker，确保同一时刻每个会话仅有一个线程在执行，完全避免 `mcp_session_t` 内部竞态。
6. **线程池参数规范**：`mcp_async_server_create` 要求 `pool != NULL`，传 NULL 返回 `MCP_ERR_INVALID_ARGUMENT`，与 `mcp_socket_serve` 风格一致，从类型与参数层面严格保证 Anti-Goal 2.3.1。
7. **推送与广播路由**：全局通知（如 `notifications/tools/list_changed` 等）由 Reactor 统一扇出至所有活跃的 SSE/WS 连接；订阅通知（如 `resources/updated`）匹配对应会话的订阅列表分发。

---

### 4.4 留给实现者的自由度 (What's left to the builder)
1. **读写缓冲的具体结构**：由实现者决定采用平铺静态环形缓冲（RingBuffer）还是动态链表块缓冲，只要单连接内存基线保持在 4KB~8KB 内即可；
2. **SHA-1 内部实现路径**：放在 `src/core/sha1.c` 或直接内联在 `src/transport/ws.c` 均可；
3. **Timer 管理**：可直接复用现有 [`mcpkit/runtime/timer.h`](file:///data/home/quintin/workspace/source/c/mcpkit-c/include/mcpkit/runtime/timer.h) 单调时间轮进行心跳与超时剔除。

---

## 5. 验收标准与交付物 (Deliverables & Acceptance)

1. **头文件**：
   - `include/mcpkit/runtime/reactor.h`
   - `include/mcpkit/transport/ws.h`
   - `include/mcpkit/transport/async_server.h`
2. **源码实现**：
   - `src/runtime/reactor.c`（支持 Linux `epoll` + POSIX `poll` 回退）
   - `src/transport/ws.c`（RFC 6455 编解码、掩码处理、SHA-1 计算）
   - `src/transport/async_server.c`（多协议分流服务）
3. **测试覆盖**：
   - `tests/unit/test_reactor.c`：事件监听、超时、跨线程 wakeup；
   - `tests/unit/test_ws_frame.c`：RFC 6455 官方测试向量校验、分片装配、掩码解码；
   - `tests/unit/test_ws_handshake.c`：`Sec-WebSocket-Accept` 验证；
   - `tests/acceptance/test_async_concurrency.c`：模拟 100+ 并发客户端混合连接（POST + SSE + WS）；
4. **示例程序**：
   - `examples/async-server/main.c`：单端口承载 HTTP POST、SSE 与 WS 的完整示例；
   - `examples/ws-client/main.c`：原生 WebSocket 客户端示例。
5. **构建保证**：CTest 100% 通过，ASan / UBSan 内存无泄漏，`-Wall -Wextra` 零告警。
