# 高并发异步 HTTP/SSE 驱动与 WebSocket 传输层实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `mcpkit-c` 中实现零外部依赖的原生 POSIX 非阻塞事件反应器（Reactor）、高并发 Streamable HTTP/SSE 服务以及 RFC 6455 WebSocket 传输层，支持单端口多协议统一接入。

**Architecture:** 底层由 Linux `epoll`（带 POSIX `poll` 回退）驱动单线程非阻塞 I/O 反应器 `mcp_reactor_t`；中间层提供纯 C 的 RFC 3174 SHA-1 与 RFC 6455 WebSocket 帧编解码器；顶层通过 `mcp_async_server_t` 监听单一端口，基于首包协议嗅探动态路由 HTTP POST、SSE 长连接与 WebSocket 升级，并通过引用计数与 MPSC 响应队列联动现有 `mcp_executor_t` 线程池。

**Tech Stack:** C23, POSIX sockets, sys/epoll.h, poll.h, stdatomic.h, threads.h, CMake, CTest, AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan).

**Spec:** [`docs/superpowers/specs/2026-10-30-async-http-sse-websocket-design.md`](file:///data/home/quintin/workspace/source/c/mcpkit-c/docs/superpowers/specs/2026-10-30-async-http-sse-websocket-design.md)

## Global Constraints

- **标准基线**：C23（`-std=c2x`），使用 `<stdatomic.h>` 与 `<threads.h>`。
- **零外部依赖**：仅依赖 libc 与标准 POSIX 系统调用（`sys/epoll.h`, `poll.h`, `sys/socket.h`, `unistd.h`, `fcntl.h`），严禁引入 libuv 等第三方事件库。
- **架构分层**：`runtime/` 不依赖 `transport/`；`transport/` 依赖 `runtime/`、`server/`、`core/`；严防依赖回灌。
- **内存所有权与错误码**：函数均返回 `mcp_status_t`；使用 `mcp_context_t` 分配器；所有堆指针分配必须检查 NULL。
- **编译标准**：`-Wall -Wextra` 零警告；ASan / UBSan 全绿无泄漏。

## Review Focus

1. **执行中对端突发断开（防 UAF）**：Worker 线程仍在执行耗时工具时对端断开，Reactor 摘除 fd 但持有引用的连接对象不提前释放，Worker 写回时检测到关闭并静默释放。
2. **超限 HTTP 报文拒绝**：非阻塞接收到超出 `MCP_HTTP_MAX_LINE` (8KB) 的 Header 或超 `MCP_HTTP_MAX_BODY` (4MB) 的 Body，立即回复 400/413 并切断，防止内存耗尽。
3. **畸形 WebSocket 帧安全**：客户端发送未掩码帧（RFC 6455 要求客户端必须掩码）或无效 Opcode 时，服务端安全回复 1002 (Protocol Error) Close 帧并断开。
4. **同会话并发请求串行化**：同 `Mcp-Session-Id` 的并发 POST 请求通过会话队列排队，避免 `mcp_session_t` 内部数据竞争。
5. **SSE GET 会话绑定校验**：未携带或携带无效 `Mcp-Session-Id` 的 SSE GET 请求直接拒绝（400/404），防止未认证的虚假长连接耗尽资源。

---

## File Structure

```text
include/mcpkit/
  runtime/
    reactor.h          # 非阻塞 I/O 多路复用反应器接口 (epoll + poll)
  transport/
    ws.h               # RFC 6455 WebSocket 协议定义、帧编解码及客户端传输接口
    async_server.h     # 单端口多协议统一异步服务管理器接口
src/
  core/
    sha1.h             # 内部 RFC 3174 SHA-1 计算私有头文件
    sha1.c             # 纯 C 原生 SHA-1 实现 (~80 行)
  runtime/
    reactor.c          # Reactor 实现 (Linux epoll 与通用 poll 回退实现)
  transport/
    ws.c               # WebSocket 帧解析/序列化、握手及同步 transport 实现
    async_server.c     # 统一异步服务核心：单端口协议分流、SSE 管理、线程池桥接
tests/
  unit/
    test_reactor.c     # Reactor 单元测试 (事件驱动、超时、跨线程唤醒)
    test_ws_handshake.c# RFC 3174 SHA-1 与 Sec-WebSocket-Accept 向量测试
    test_ws_frame.c    # RFC 6455 帧解码、掩码、分片单元测试
    test_ws_transport.c# WebSocket 同步 transport 环回单元测试
    test_async_server.c# 异步服务单端口混合协议分流单元测试
  acceptance/
    test_async_concurrency.c # 100+ 并发客户端压测探针
    async_accept.sh    # 端到端验收脚本
examples/
  async-server/main.c  # 单端口 HTTP POST + SSE + WS 综合示例
  ws-client/main.c     # 原生 WebSocket 客户端示例
```

---

## Task 1: 运行时非阻塞反应器 (`mcp_reactor_t`)

**Files:**
- Create: `include/mcpkit/runtime/reactor.h`
- Create: `src/runtime/reactor.c`
- Create: `tests/unit/test_reactor.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```c
  typedef struct mcp_reactor mcp_reactor_t;
  typedef uint32_t mcp_event_mask_t;
  #define MCP_EVENT_READ   (1u << 0)
  #define MCP_EVENT_WRITE  (1u << 1)
  #define MCP_EVENT_ERROR  (1u << 2)
  typedef void (*mcp_reactor_cb_t)(mcp_context_t *ctx, int fd, mcp_event_mask_t events, void *user);

  mcp_reactor_t *mcp_reactor_create(mcp_context_t *ctx);
  void           mcp_reactor_destroy(mcp_context_t *ctx, mcp_reactor_t *r);
  mcp_status_t   mcp_reactor_add(mcp_context_t *ctx, mcp_reactor_t *r, int fd, mcp_event_mask_t events, mcp_reactor_cb_t cb, void *user);
  mcp_status_t   mcp_reactor_modify(mcp_context_t *ctx, mcp_reactor_t *r, int fd, mcp_event_mask_t events);
  mcp_status_t   mcp_reactor_remove(mcp_context_t *ctx, mcp_reactor_t *r, int fd);
  mcp_status_t   mcp_reactor_step(mcp_context_t *ctx, mcp_reactor_t *r, int timeout_ms);
  mcp_status_t   mcp_reactor_run(mcp_context_t *ctx, mcp_reactor_t *r);
  void           mcp_reactor_stop(mcp_context_t *ctx, mcp_reactor_t *r);
  mcp_status_t   mcp_reactor_wakeup(mcp_context_t *ctx, mcp_reactor_t *r);
  ```

- [ ] **Step 1: 编写失败测试 `tests/unit/test_reactor.c`**
  测试包含：创建反应器、通过 socketpair 注册读就绪事件、触发回调、跨线程调用 `mcp_reactor_wakeup()` 唤醒阻塞在 `mcp_reactor_step()` 的反应器。

- [ ] **Step 2: 运行测试验证失败**
  Run: `cmake --build build --target test_reactor`
  Expected: 编译失败或链接失败（未定义符号）。

- [ ] **Step 3: 声明公共头文件 `include/mcpkit/runtime/reactor.h`**
  定义上述所有常量、函数原型与所有权规范。

- [ ] **Step 4: 实现 `src/runtime/reactor.c`**
  - Linux 环境使用 `epoll_create1(EPOLL_CLOEXEC)`、`epoll_ctl` 与 `epoll_wait`；
  - 内部集成 `eventfd`（或 POSIX self-pipe 作为兜底）用于 `mcp_reactor_wakeup()`；
  - 维护动态 fd 映射表（包含回调函数与 `void *user` 指针）；
  - 将 `src/runtime/reactor.c` 加入 `CMakeLists.txt` 的 `MCPKIT_CORE_SOURCES`。

- [ ] **Step 5: 运行单测验证通过**
  Run: `ctest --test-dir build -R test_reactor --output-on-failure`
  Expected: `100% tests passed, 0 tests failed out of 1`

- [ ] **Step 6: 提交代码**
  ```sh
  git add include/mcpkit/runtime/reactor.h src/runtime/reactor.c tests/unit/test_reactor.c CMakeLists.txt tests/CMakeLists.txt
  git commit -m "feat(runtime): implement non-blocking I/O event reactor"
  ```

---

## Task 2: 原生 RFC 3174 SHA-1 与 WebSocket 握手密钥计算

**Files:**
- Create: `src/core/sha1.h`
- Create: `src/core/sha1.c`
- Create: `tests/unit/test_ws_handshake.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```c
  void mcp_sha1(const uint8_t *data, size_t len, uint8_t digest[20]);
  mcp_status_t mcp_ws_compute_accept_key(const char *client_key, char accept_buf[32]);
  ```

- [ ] **Step 1: 编写失败测试 `tests/unit/test_ws_handshake.c`**
  - 测试 RFC 3174 标准测试向量（如 `"abc"` 的摘要）；
  - 测试 RFC 6455 官方握手用例：输入 Key `"dGhlIHNhbXBsZSBub25jZQ=="`，断言输出 Accept Key 为 `"s3pPLMBiTxaQ9kYGzzhZRbK+xOo="`。

- [ ] **Step 2: 运行测试验证失败**
  Run: `cmake --build build --target test_ws_handshake`
  Expected: 编译/链接失败。

- [ ] **Step 3: 实现 `src/core/sha1.h` 与 `src/core/sha1.c`**
  - 编写紧凑的标准 RFC 3174 SHA-1 算法（~80 行纯 C）；
  - 实现 `mcp_ws_compute_accept_key`：将客户端 Key 与 GUID `"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"` 拼接，计算 SHA-1 摘要，并使用已有 Base64 编码输出至 32 字节缓冲区。
  - 将 `src/core/sha1.c` 加入 `MCPKIT_CORE_SOURCES`。

- [ ] **Step 4: 运行单测验证通过**
  Run: `ctest --test-dir build -R test_ws_handshake --output-on-failure`
  Expected: PASS。

- [ ] **Step 5: 提交代码**
  ```sh
  git add src/core/sha1.h src/core/sha1.c tests/unit/test_ws_handshake.c CMakeLists.txt tests/CMakeLists.txt
  git commit -m "feat(crypto): implement native RFC 3174 SHA-1 and WebSocket accept key generator"
  ```

---

## Task 3: RFC 6455 WebSocket 数据帧编解码器

**Files:**
- Create: `include/mcpkit/transport/ws.h`
- Create: `src/transport/ws.c`
- Create: `tests/unit/test_ws_frame.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 (`mcp_ws_compute_accept_key`)
- Produces:
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
      char *payload;
  } mcp_ws_frame_t;

  mcp_status_t mcp_ws_parse_frame(mcp_context_t *ctx, const uint8_t *buf, size_t len,
                                  mcp_ws_frame_t *frame_out, size_t *consumed_out);
  mcp_status_t mcp_ws_encode_frame(mcp_context_t *ctx, const mcp_ws_frame_t *frame,
                                   uint8_t **bytes_out, size_t *len_out);
  void         mcp_ws_frame_destroy(mcp_context_t *ctx, mcp_ws_frame_t *frame);
  ```

- [ ] **Step 1: 编写失败测试 `tests/unit/test_ws_frame.c`**
  - 测试小负载文本帧（<125 字节）带掩码与未掩码的双向编解码；
  - 测试中等负载（126-65535 字节，2字节扩展长度）与大负载（>65535 字节，8字节扩展长度）；
  - 测试分片（`fin = false`）与 Ping/Pong 控制帧；
  - 测试畸形帧（客户端未掩码或数据不完整）的解析返回值。

- [ ] **Step 2: 运行测试验证失败**
  Run: `cmake --build build --target test_ws_frame`
  Expected: 编译/链接失败。

- [ ] **Step 3: 编写 `include/mcpkit/transport/ws.h` 与 `src/transport/ws.c`**
  - 实现基于流式缓冲的解析器：解析 FIN、Opcode、MASK、7位/16位/64位 Payload Length；
  - 执行 4 字节 XOR 掩码还原算法；
  - 实现帧序列化器，根据负载长度自动选择最优头部编码格式；
  - 将 `src/transport/ws.c` 加入 `MCPKIT_CORE_SOURCES`。

- [ ] **Step 4: 运行单测验证通过**
  Run: `ctest --test-dir build -R test_ws_frame --output-on-failure`
  Expected: PASS。

- [ ] **Step 5: 提交代码**
  ```sh
  git add include/mcpkit/transport/ws.h src/transport/ws.c tests/unit/test_ws_frame.c CMakeLists.txt tests/CMakeLists.txt
  git commit -m "feat(transport): implement RFC 6455 WebSocket frame parser and serializer"
  ```

---

## Task 4: 客户端 WebSocket 传输与阻塞式 `mcp_transport_t` 适配器

**Files:**
- Modify: `include/mcpkit/transport/ws.h`
- Modify: `src/transport/ws.c`
- Create: `tests/unit/test_ws_transport.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2, Task 3
- Produces:
  ```c
  mcp_transport_t *mcp_ws_transport_create(mcp_context_t *ctx, int fd, bool is_server);
  mcp_transport_t *mcp_ws_client_connect(mcp_context_t *ctx, const char *url);
  ```

- [ ] **Step 1: 编写失败测试 `tests/unit/test_ws_transport.c`**
  - 使用 socketpair 创建一对已连接的 fd；
  - 一端包装为 server ws transport，另一端包装为 client ws transport；
  - 相互发送和接收 JSON-RPC 文本，验证 `mcp_transport_send` 与 `mcp_transport_recv` 的行为。

- [ ] **Step 2: 运行测试验证失败**
  Run: `cmake --build build --target test_ws_transport`
  Expected: 编译/链接失败。

- [ ] **Step 3: 扩充 `src/transport/ws.c` 实现 `mcp_transport_ops_t` 结构**
  - 实现 `ws_start`、`ws_send`（将行封装为 Text 帧并发送）、`ws_recv`（接收并自动跳过 Ping/Pong 控制帧并解出文本行）和 `ws_stop`；
  - 实现 `mcp_ws_client_connect`：解析 `ws://host:port/path`，建立 TCP 连接，生成随机 16 字节 base64 密钥，发送 HTTP 升级报文并校验服务器返回的 101 及 `Sec-WebSocket-Accept`。

- [ ] **Step 4: 运行单测验证通过**
  Run: `ctest --test-dir build -R test_ws_transport --output-on-failure`
  Expected: PASS。

- [ ] **Step 5: 提交代码**
  ```sh
  git add include/mcpkit/transport/ws.h src/transport/ws.c tests/unit/test_ws_transport.c tests/CMakeLists.txt
  git commit -m "feat(transport): add WebSocket client connector and mcp_transport_t adapter"
  ```

---

## Task 5: 统一单端口高并发异步服务核心 (`mcp_async_server_t`)

**Files:**
- Create: `include/mcpkit/transport/async_server.h`
- Create: `src/transport/async_server.c`
- Create: `tests/unit/test_async_server.c`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 (`mcp_reactor_t`), Task 2 (`sha1`), Task 3 (`ws_frame`), `mcp_http_request_t`, `mcp_server_t`, `mcp_executor_t`
- Produces:
  ```c
  typedef struct mcp_async_server mcp_async_server_t;
  typedef struct {
      uint16_t port;
      const char *bind_addr;
      size_t max_connections;
      uint32_t keepalive_timeout_s;
      bool enable_websocket;
      bool enable_sse;
  } mcp_async_server_config_t;

  mcp_async_server_t *mcp_async_server_create(mcp_context_t *ctx, mcp_server_t *server,
                                              mcp_executor_t *pool,
                                              const mcp_async_server_config_t *cfg);
  void                mcp_async_server_destroy(mcp_context_t *ctx, mcp_async_server_t *s);
  mcp_status_t        mcp_async_server_run(mcp_context_t *ctx, mcp_async_server_t *s);
  void                mcp_async_server_stop(mcp_context_t *ctx, mcp_async_server_t *s);
  ```

- [ ] **Step 1: 编写失败测试 `tests/unit/test_async_server.c`**
  - 创建 `mcp_server_t`（注册 `echo` 工具）和线程池 `mcp_executor_t`；
  - 启动 `mcp_async_server_t`（绑定随机可用端口）；
  - 测试 1：客户端发 POST `initialize`，断言返回 `200 OK` 及 `Mcp-Session-Id`；
  - 测试 2：客户端携带会话 ID 发起 GET `Accept: text/event-stream`，断言收到 SSE 首部；
  - 测试 3：客户端发送 WebSocket Upgrade 请求，断言收到 101 Switching Protocols，并通过 WebSocket 成功调用 `tools/call`；
  - 测试 4：非法会话 ID 的 GET 请求被正确拒绝（400/404）。

- [ ] **Step 2: 运行测试验证失败**
  Run: `cmake --build build --target test_async_server`
  Expected: 编译/链接失败。

- [ ] **Step 3: 声明头文件 `include/mcpkit/transport/async_server.h`**

- [ ] **Step 4: 实现 `src/transport/async_server.c`**
  - 创建并驱动内部 `mcp_reactor_t`；
  - 监听 TCP 端口并设为非阻塞；
  - 新连接维护连接上下文：`atomic_uint ref_count`、输入输出 RingBuffer、当前状态（HTTP_HANDSHAKE / SSE_STREAM / WS_STREAM）；
  - 首包识别：
    - 若含 `Upgrade: websocket` 且开启 WS：计算 Accept Key，返回 101，切换至非阻塞 WebSocket 帧解析；
    - 若为 `POST`：检查或生成会话，提取 JSON-RPC 消息提交至 Worker 线程，通过会话队列实现同会话请求串行化；Worker 执行完毕压入 MPSC 响应队列并调用 `mcp_reactor_wakeup()`；
    - 若为 `GET`（SSE）：必须携带有效 `Mcp-Session-Id`，返回 200 SSE 响应头，连接保持在推送列表中；
  - 全局广播支持：定时或事件唤醒时将 `server->outbox` 中的消息编码为 SSE/WS 数据块非阻塞刷出；
  - 将 `src/transport/async_server.c` 加入 `MCPKIT_CORE_SOURCES`。

- [ ] **Step 5: 运行单测验证通过**
  Run: `ctest --test-dir build -R test_async_server --output-on-failure`
  Expected: PASS。

- [ ] **Step 6: 提交代码**
  ```sh
  git add include/mcpkit/transport/async_server.h src/transport/async_server.c tests/unit/test_async_server.c CMakeLists.txt tests/CMakeLists.txt
  git commit -m "feat(transport): implement unified high-concurrency async HTTP/SSE and WebSocket server"
  ```

---

## Task 6: 示例程序、高并发压力验收测试与构建体系整合

**Files:**
- Create: `examples/async-server/main.c`
- Create: `examples/ws-client/main.c`
- Create: `tests/acceptance/test_async_concurrency.c`
- Create: `tests/acceptance/async_accept.sh`
- Modify: `examples/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`
- Modify: `docs/module-reference.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: Task 1 - Task 5

- [ ] **Step 1: 编写示例程序 `examples/async-server/main.c`**
  演示在同一个端口上启动统一服务，支持通过 HTTP POST、SSE 与 WebSocket 同时访问服务器注册的工具。

- [ ] **Step 2: 编写示例程序 `examples/ws-client/main.c`**
  演示使用 `mcp_ws_client_connect` 建立连接，并执行 `tools/list` 与 `tools/call`。

- [ ] **Step 3: 编写并发压力测试探针 `tests/acceptance/test_async_concurrency.c`**
  并发建立 100+ 连接（混合 HTTP POST、长连接 SSE 与持续活跃的 WebSocket），同时并发收发 1,000+ 请求，断言零丢包、零崩溃、正常退出。

- [ ] **Step 4: 编写验收脚本 `tests/acceptance/async_accept.sh`**
  编译并拉起 `async-server`，驱动 `test_async_concurrency` 和 `ws-client`，验证服务退出码为 0。在 `tests/CMakeLists.txt` 中添加 `add_test(NAME async_acceptance ...)`.

- [ ] **Step 5: 全量构建与双配置 CTest 测试验证**
  - Run: `ctest --test-dir build --output-on-failure`
  - Expected: 全部测试（原有 63 项 + 新增项）100% 通过。
  - Run: `cmake --build build-http && ctest --test-dir build-http --output-on-failure`
  - Expected: 100% 通过。

- [ ] **Step 6: 更新文档与 CHANGELOG**
  - 在 `docs/module-reference.md` 补充 `reactor.h`、`ws.h` 与 `async_server.h` API 描述；
  - 在 `README.md` 更新架构图、特性列表与 Examples 表；
  - 在 `CHANGELOG.md` 的 `[Unreleased]` 下记录本次新增功能。

- [ ] **Step 7: 提交代码**
  ```sh
  git add examples/ tests/ README.md docs/ CHANGELOG.md
  git commit -m "feat(examples): add async-server, ws-client, and high-concurrency acceptance tests"
  ```
