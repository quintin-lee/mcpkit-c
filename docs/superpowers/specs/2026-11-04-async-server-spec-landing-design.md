# 异步服务器 Spec 落地实现设计（write_buf 竞态 + 零泄漏 + UAF 根除）

> 状态：提案（Brainstorming 完成，待 Spec Reviewer 验证与用户批准）
> 日期：2026-11-04
> 基线：`HEAD`（master），`VERSION 0.1.0`
> 责任模块：`src/transport/async_server.c`、`include/mcpkit/transport/async_server.h`、
> `tests/unit/`、`tests/CMakeLists.txt`
> 前置 spec：`docs/superpowers/specs/2026-10-30-async-server-stability-and-leak-free-design.md`
> 前置 plan：`docs/superpowers/plans/2026-10-30-async-server-stability-and-leak-free.md`

---

## 0. 现状诊断（本 spec 的关键前提）

10-30 spec 已写就并 review 过，但**实现处于"骨架已就位、逻辑未闭合"状态**。本次修复
**不是**设计新架构，而是把既定 spec 的实现缺口补齐、把一次误删还原。已核实的真实
起点：

| 项 | spec 要求 | 当前 HEAD 实际状态 |
|---|---|---|
| 结构体字段（`detached`/`http_in_flight`/双向链表 `prev/next`/`ws_frag_buf`/session `ref_count`/`dead`/`conns_count`） | 添加 | ✅ 已添加（`async_server.c:48-122`） |
| `conn_detach_and_close` 实现 | 实现 | ❌ 被 `21e04e4` 误当死代码用 `#if 0` 注释，body 沦为 `...` 占位（`async_server.c:180-184`） |
| `mcp_async_server_active_connections` 探针 | 实现 | ❌ 仅在 `.h:99` 声明，`.c` 中无定义、无调用者（故 69/69 全绿也未暴露） |
| `find_session` 内增持 ref（TOCTOU 安全） | 实现 | ❌ 未做（`async_server.c:212` 命中即解锁返回裸指针） |
| `reap_expired_sessions` 会话超时巡检 | 实现 | ❌ 未做（仅 `last_reap_time` 字段空置） |
| WS Continuation 分片状态机 | 实现 | ❌ `ws_frag_buf` 字段存在但无逻辑，`fin==false` 数据帧直接 `close_conn` |
| 写路径所有权（worker 只 enqueue，不碰 `conn->write_buf`） | 实现 | ❌ `drain_responses` 直接拼 `conn->write_buf`，与 worker `enqueue_response` 双线程竞态（**P0 根因**） |

**根因链**：`21e04e4` 提交 message 声称"移除未使用的静态函数消除编译警告"，实际把
**已被调用的** `conn_detach_and_close` 注释掉；此后骨架与逻辑脱节，写缓冲的双线程访问
从未收敛，P0 竞态 + P1 泄漏 + UAF 三类问题并存。

---

## 1. 目标与非目标

### 1.1 目标
1. **根除 write_buf 双线程竞态（P0）**：确立"连接写缓冲仅由 reactor 线程独占维护，
   worker 线程一律走 enqueue→resp 队列"的单一所有权边界。
2. **零泄漏 + 确定性生命周期（P1）**：所有连接关闭路径统一走 `conn_detach_and_close`，
   双向链表 O(1) 脱钩，`conns_count` 锁外可读。
3. **UAF 消除**：session entry 引用计数 + 5s 批次 reaper，worker 持有 entry 期间不会被
   reaper 释放。
4. **协议健全**：HTTP 流式两阶段解析（消除分包误杀/粘包截断）、严格保序、
   RFC 6455 §5.4 WS 分片重组（含 4MB DoS 上限）。
5. **门禁**：既有 69 项 + 新增 4 项单测 100% 通过；`-Wall -Wextra` 零告警；
   ASan/UBSan/TSan 全绿零泄漏。

### 1.2 非目标
- 不破坏公开 API：`async_server.h` 既有签名保持稳定，仅确保 `active_connections`
  声明有对应实现。
- 不引入新第三方依赖，保持 C23 + POSIX + `threads.h`。
- 不拆分 `async_server.c` 为多文件（超范围，单独评估）。

### 1.3 明确不纳入（拆入第二份"杂项清理 spec"，本周期不设计细节）
- `mcp_queue_destroy` / `queue_grow` NOMEM 边缘泄漏（sessions realloc 失败时旧缓冲 `om` 归属丢失）。
- `mcp_context_set_json_backend` 的 `const void*` → 具体指针类型（`context.c:115` 隐式转换，C23 约束违规）。
- `docs/module-reference.md` 残留 `mcp_dispatcher_*` 幽灵 API + 新头文件未入文档。
- `CHANGELOG.md` `[Unreleased]` 未切分 released section。

---

## 2. 详细设计

### 2.1 连接生命周期与写路径所有权（P0 + P1 根除）

**结构体**（字段已就位，无需改动，确认语义）：
- `async_conn_t`：`prev/next`（双向链表）、`atomic_bool detached`（幂等脱钩门）、
  `atomic_bool closing`（写完后断开）、`http_in_flight`（reactor 线程私有的 in-flight 标记）。
- `mcp_async_server_t`：`conns_count` 改为 **`atomic_size_t`**（锁外读支持
  `active_connections` 探针）。

**`conn_detach_and_close(s, conn)`（还原 `#if 0` 内被删逻辑并正式启用）**：
```
1. if (conn == NULL) return;
2. if (atomic_exchange(&conn->detached, true)) return;   // 幂等，防二次脱钩/二次 close
3. atomic_store(&conn->closing, true);
4. 锁外：if (conn->fd >= 0) { mcp_reactor_remove(ctx, reactor, fd); close(fd); fd = -1; }
5. 若 CONN_SSE 且 session_id[0] != '\0'：
     mtx_lock(&s->sessions_mtx);
     遍历 s->sessions，cur->sse_conn == conn 则置 cur->sse_conn = NULL;
     mtx_unlock;
6. mtx_lock(&s->conns_mtx);
   双向链表脱钩（prev/next 维护）；s->conns_count--（atomic）；
   mtx_unlock;
7. conn_unref(conn);   // 冲抵 on_accept 分配的基准引用
```

**写路径单一所有权（竞态根除核心）**：
- `conn->write_buf/write_len/write_cap/write_off` **仅由 reactor 线程独占改写**：
  - `handle_client_read` 的写事件分支（`write_off >= write_len`）独占释放 + 重置；
  - `drain_responses()` 也只在 reactor 线程（`mcp_async_server_step`）运行，独占拼接
    resp 链表项到 `conn->write_buf`。
- **worker 线程（`worker_dispatch_fn`）绝不直接写 `conn->write_buf`**，只能
  `enqueue_response`：把序列化好的字节包成 `response_item_t` 挂到 `resp_head` 链表
  + `conn_ref(conn)` + `mcp_reactor_wakeup`。`resp_mtx` 仅保护 resp 链表本身。
- 结果：`write_buf` 永远只在单线程（reactor）被写 → P0 竞态根除，**无需**额外写锁。
- 全仓连接关闭路径（HTTP/WS 读错、WS Close 帧、reaper、`mcp_async_server_destroy`）
  统一走 `conn_detach_and_close`，禁止再直接 `close(fd)`。

**`active_connections` 探针**：
```c
size_t mcp_async_server_active_connections(const mcp_async_server_t *s) {
    if (s == NULL) return 0;
    return atomic_load_explicit(&s->conns_count, memory_order_relaxed);
}
```
（`conns_count` 需从 `size_t` 升级为 `atomic_size_t`。）

**清理**：删除 `#if 0 ... #endif` 包裹的死代码块，`conn_detach_and_close` 还原为正式
函数。`create_session_entry` 的 session id 生成器去掉 `time(NULL)` 依赖，改为单调原子
计数（`static atomic_uint` 起始 1000），保证同秒高并发唯一。

### 2.2 HTTP 流式两阶段解析与严格保序（spec §3.2）

**常量上限**：
```
#define MCP_ASYNC_MAX_BODY    (4u << 20)
#define MCP_ASYNC_MAX_READ_CAP (MCP_ASYNC_MAX_BODY + 65536u)
```
读缓冲扩容时 `ncap` 钳制在 `MAX_READ_CAP`；`read_len + 4096 > read_cap` 且已达上限则
`conn_detach_and_close` 防 OOM。

**两阶段完整性判定**（仅 `CONN_HTTP_INIT`）：
1. 在 `read_buf`（长度 `read_len`）找 `\r\n\r\n`，得 `header_len`；未找到则 return 等
   下个 read。
2. 私有辅助 `parse_http_content_length(ctx, buf, header_len)` 大小写不敏感取
   `Content-Length`（缺省 0；格式错/负数 → 400）。
3. `content_len > MCP_ASYNC_MAX_BODY` → 413 + `closing`。
4. `total_needed = header_len + content_len`；`total_needed > MAX_READ_CAP` → 413。
5. **`read_len < total_needed` → body 未收全：不报错、不重置缓冲区，直接 return**（消除
   "分包误杀 400"关键路径）。
6. 收全后 `mcp_http_parse_request(ctx, read_buf, total_needed)`（传精确长度，杜绝粘包
   干扰当前请求解析）。

**严格保序 + 滑动窗口（RFC 9112）**：
- `conn->http_in_flight` 守卫：同一连接已有 in-flight POST 时，reactor 不分发下一个，
  剩余粘包留在 `read_buf`。
- **清除点唯一**：仅在 `handle_client_read` 写完成分支（`write_off >= write_len`）置
  `http_in_flight = false`，并就地重查 `read_buf` 剩余粘包；若此时已 101 升级 WS，立即
  转入 WS 帧解析循环。
- 分发后滑窗消费：`remaining = read_len - total_needed`，`memmove(read_buf, read_buf +
  total_needed, remaining)`，`read_len = remaining`，`read_buf[read_len] = '\0'`。

### 2.3 WebSocket Continuation 分片重组（spec §3.3，`CONN_WS` 分支）

当前对 `fin==false` 数据帧直接 `close_conn`。改为状态机（`ws_frag_buf/len/cap/opcode`
字段已就位）：
1. **控制帧优先就地处理**：Ping→Pong、Close→`conn_detach_and_close`，可在分片序列间
   穿插，不破坏 `ws_frag_buf`。
2. **起始分片帧**（`fin==false` 且 TEXT/BINARY）：若 `ws_frag_buf != NULL`（前序未完结
   又来新起始帧）→ 协议违规断开；否则初始化 `ws_frag_buf`，记 `ws_frag_opcode`。
3. **Continuation 帧**：若 `ws_frag_buf == NULL`（未收首帧就来续帧）→ 违规断开；
   防 DoS：`new_len = ws_frag_len + payload_len > 4MB` → 断开；拼接（末尾保留 `\0`）；
   `fin==true`（含 0 负载纯结束帧）→ 拼装完毕，`ws_frag_buf` 所有权转移给
   `worker_task_t->json_body`，重置 `ws_frag_*` 字段，提交线程池。
4. **普通完整数据帧**（`fin==true`）：若 `ws_frag_buf != NULL`（等续帧期间来了非分片
   数据帧）→ 违规断开；否则正常提交。

### 2.4 引用计数会话 reaper（UAF 消除，spec §3.4）

**引用所有权流转**：
- `session_entry_ref/unref` 辅助；`unref` 归零时销毁 `mcp_session_t` + `mtx_destroy` +
  释放 entry。
- `find_session` 在 `sessions_mtx` 临界区内命中时**立即增持 1** 再解锁返回（消除
  TOCTOU）；
- `create_session_entry` 基准 ref=1（链表持有），返回给调用方时再 ref 一次；
- **HTTP POST**：`worker_task_t` 持有的 entry 引用转移给 task，`worker_dispatch_fn`
  结束、释放 task **之前** `session_entry_unref`；OOM 失败路径调用方就地 `unref`；
- **GET SSE**：在 `sessions_mtx` 内赋 `entry->sse_conn = conn`，解锁后调用方就地 `unref`
  （由链表基准引用 + `sse_conn` 持续维护）。

**5s 批次巡检 `reap_expired_sessions(s)`**（在 `mcp_async_server_step` 中，
`time(NULL) - last_reap_time >= 5` 触发）：
- `#define MCP_SESSION_REAP_BATCH 32`，局部栈数组 `async_conn_t *sse_to_close[32]`；
- 加锁遍历 `s->sessions`：过期（`now - last_active > keepalive_timeout_s`）则摘链、
  `atomic_store(&dead, true)`、若 `sse_conn` 非空收集进数组并置空、`session_entry_unref`
  （释放基准引用）；
- **立即解锁**，再在锁外对 `sse_to_close[i]` 逐一 `conn_detach_and_close`（杜绝
  `sessions_mtx` 内自锁死锁）；
- 超出 32 个的过期会话安全保留在链表，下 5s 周期继续淘汰（防栈溢出）。

---

## 3. 测试与验证计划

### 3.1 新增 4 个单测（`tests/unit/` + 注册 `tests/CMakeLists.txt`）
1. **`test_async_leak`（零泄漏门禁）**：启动 async server → 循环 300 次短连接
   （GET/POST 后关闭 socket）→ `mcp_async_server_step` 驱动一次 → 断言
   `mcp_async_server_active_connections(s) == 0` → 销毁后 ASan/UBSan 下无泄漏。
2. **`test_async_fragmentation`（分包/粘包）**：
   - A 分包：分两次发单个 POST（Header + 4B Body，sleep 15ms，再发剩余 Body）→ 必须
     200 OK，绝不 400；
   - B 粘包/Pipelining：单次 `send` 连发两个完整 POST → 按序收到两个独立响应。
3. **`test_ws_continuation`（WS 分片重组）**：Text(fin=0) + Continuation(fin=0) +
   空负载 Continuation(fin=1) → 正常拼装并返回 JSON-RPC 结果；异常：未初始化
   Continuation、超大分片 → 连接被安全关闭。
4. **`test_async_session_expiry`（会话超时）**：`keepalive_timeout_s=1`，建立会话后
   sleep 2s 驱动 `step` 巡检 → 后续请求 404 Session Not Found；挂起长连接 + 慢任务期间
   会话超时，断言 worker 退出与 reaper 巡检无崩溃/死锁/UAF。

### 3.2 质量门禁
- 既有 69 项 + 新增 4 项 100% 通过；
- `-Wall -Wextra` 零告警（已入根 CMakeLists）；
- ASan + UBSan（`-DMCPKIT_ENABLE_ASAN=ON -DMCPKIT_ENABLE_UBSAN=ON`，
  `MCPKIT_BUILD_HTTP=ON -DMCPKIT_BUILD_SOCKET=ON`）全绿、零泄漏；
- 补 TSan 验收跑 `test_async_concurrency`（1000+ 并发）固化"写路径单一所有权"防回归。

---

## 4. 提交粒度

- **提交 1**：还原 `conn_detach_and_close` + 写路径单一所有权 + `active_connections`
  探针 + `conns_count` atomic 化 + 删 `#if 0` 死代码（P0 竞态 + 生命周期）。
- **提交 2**：HTTP 流式两阶段解析 + 严格保序滑窗。
- **提交 3**：WS Continuation 分片重组状态机。
- **提交 4**：session 引用计数 + 5s 批次 reaper + 会话 id 生成器去 `time(NULL)`。
- **提交 5**：4 个新单测 + CTest 注册 + 门禁（ASan/UBSan/TSan）。

## 5. 风险与回滚

- 主要风险：写路径所有权重构若漏掉某条 worker 直接写 `write_buf` 的路径会残留竞态；
  以 TSan 验收为最终裁决。
- 回滚：各提交独立可 revert；`active_connections` 为新增 API，回滚不影响既有签名。
