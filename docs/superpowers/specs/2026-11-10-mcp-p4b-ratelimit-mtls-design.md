# MCP P4-b 运维面：会话/客户端级限速 + 认证锁out + mTLS 补强

**日期**：2026-11-10
**状态**：待评审
**前置**：P3 已交付（401 挑战扩展 `mcp_http_serve_with_auth_v2`）。P4-b 复用 P3 T4 的 v2 认证回调与 `mcp_server` 既有原子计数器（`c_requests_total`）。

## 背景与现状

P3 完成后库声明 "2026 授权模型完整"，但缺**运维面**能力：

| 能力 | 现状 | 缺口 |
|---|---|---|
| 请求速率限制 | `mcp_server.c_requests_total`（P2 交付）仅全局计数，无阈值/窗口 | 会话级滑动窗口 + 429 + Retry-After |
| 认证锁out | 无 | 连续 N 次认证失败锁定 + 解锁 |
| mTLS（双向认证） | P0-2 选 `SSL_VERIFY_NONE`（`tls.c` 注释确认服务端/客户端两侧均不验 peer） | 可配 `SSL_VERIFY_PEER` + CA 链 + host verify 回调 |

P4-b 是**配置驱动**而非版本驱动——与 P3 版本门控的本质区别：host 显式 `mcp_server_set_rate_limit`/`set_lockout`/`mcp_tls_set_verify_peer` 才激活，未配置则**零行为变更**（现状保持）。

## 净新增（P4-b）

### 数据面 / 配置 API（`server.h` + `server.c` + `internals.h`）

1. **会话/客户端级速率限制**：
   ```c
   /* per_session: 窗口内每会话最大请求数（0 = 未启用，零行为变更）。
      window_ms: 滑动窗口时长（毫秒）。
      激活时：超限请求 → HTTP 429 + `Retry-After: <sec>`（窗口剩余秒数，向上取整，最小 1）。
      仅 2026 路径经 streamable_http/async_server 生效；stdio/socket 不受限（无会话概念）。 */
   mcp_status_t mcp_server_set_rate_limit(mcp_context_t *ctx, mcp_server_t *srv,
                                          uint32_t per_session, uint64_t window_ms);
   bool mcp_server_rate_limit_active(const mcp_server_t *srv);
   ```
   - `mcp_server` 新增字段 `rate_limit`（`uint32_t`，0=未启用）、`rate_window_ms`（`uint64_t`）。
   - `mcp_session` 新增字段 `rate_head`（环形队列头指针）、`rate_ring`（固定大小 `uint64_t[64]` 时间戳环）、`rate_count`（当前窗口计数）。**会话级**计数，随 session 销毁释放。
   - 滑动窗口：每次请求 push 当前时间戳到 `rate_ring`，pop 超出 `window_ms` 的旧戳；`rate_count` 即当前窗口请求数。超限（`rate_count >= rate_limit`）→ 429。
   - **放置点（审查修正）**：限速检查与计数在 `handle_post` 中 `sess` 已 resolve 之后、`mcp_server_dispatch` 之前执行（此时会话对象可用）。`rate_ring` 固定 64 槽为 **best-effort 上限**：`window_ms` 很大导致窗口内请求数 > 64 时 ring 溢出则旧戳被覆盖（实际计数偏少，限速略宽松）——host 须按 `window_ms` 合理配置（建议 `rate_limit <= 64`）。
   - **并发守护（审查修正）**：`mcp_session_t` 的 rate/lockout 字段非 atomic（与既有字段一致），读写须由 `mcp_server` 既有的 `sessions_lock`（sync 路径）或新增 `rate_lock`（async 路径）守护。

2. **认证锁out**：
   ```c
   /* max_fails: 连续认证失败阈值（0 = 未启用）。lockout_ms: 锁定时长。
      激活时：会话级 `auth_fail_count` 达 max_fails → 锁定 `lockout_ms`；
      锁定期间该会话所有 401 响应附 `Retry-After: <sec>` + `X-Auth-Lockout: <expiry-epoch>` 头。
      基于 P3 T4 v2 回调 DENY 结果累计（OK 结果重置计数）。 */
   mcp_status_t mcp_server_set_lockout(mcp_context_t *ctx, mcp_server_t *srv,
                                       uint32_t max_fails, uint64_t lockout_ms);
   bool mcp_server_lockout_active(const mcp_server_t *srv);
   ```
   - 状态码统一为 **429**（审查修正：原 spec 混用 401/429，现锁定期间统一 429——"暂时锁定，N 秒后重试"；401 仅用于正常认证失败，429 用于锁定期间的快速拒绝）：
   - 计数规则：v2 回调返回 `DENY` → 该会话 `auth_fail_count++`；返回 `OK` → 重置 `auth_fail_count=0`；`auth_fail_count >= max_fails` → `lockout_until = now_ms() + lockout_ms`，重置计数。锁定期间（`now_ms() < lockout_until`）该会话所有请求在 auth 回调**前**（计数检查点短路，不执行 auth 回调）直接 429 锁定响应（附 `Retry-After` + `X-Auth-Lockout`）。
   - `mcp_session` 新增字段 `auth_fail_count`（`uint32_t`）、`lockout_until`（`int64_t` epoch ms，0=未锁定）。
   - **计数身份键（审查修正）**：锁out 计数身份键是 `Mcp-Session-Id` 头值（非 bearer token、非 session 对象指针）。2026 无状态请求（`sid==NULL`）的 `Mcp-Session-Id` 头存在但请求作用域 session 用完即弃（P2 行为），故 2026 无状态路径的锁out 身份键仍取 `Mcp-Session-Id` 头字符串（请求携带时才有效）；无 `Mcp-Session-Id` 头的 2026 无状态请求 **不进入锁out 计数**（零回归，host 须自行用 client_id 等标识）。
   - 计数挂点（审查修正）：v2 回调在 `handle_post` 中位于会话解析**之前**（此时 `sess==NULL`），故认证失败计数**不**在回调处直接挂 `mcp_session_t`，而是在 v2 回调返回后、`sess` 已 resolve 处（`handle_post` 中 dispatch 前）累计。`mcp_session` 新增 `auth_fail_count`/`lockout_until` 字段，随会话销毁释放。

### TLS 层 API（`tls.h` + `tls.c`）

3. **可配 peer 校验（mTLS 补强）**：
   ```c
   /* enable=true → SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, cb)。
      ca_dir/ca_file：SSL_CTX_set_verify_locations（校验客户端证书链信任）；NULL = 仅要求 peer 证书存在不验链。
      verify_cb：host 注入回调（返回 1 放行、0 拒绝）；NULL = 默认接受（VERIFY_PEER 但无自定义逻辑）。
      默认（host 未调用）保持 SSL_VERIFY_NONE（P0-2 零回归）。
      客户端侧（tls_connect）与服务端侧（tls_accept）两侧均支持，对称。 */
   mcp_status_t mcp_tls_set_verify_peer(mcp_context_t *ctx, mcp_transport_t *t,
                                        bool enable,
                                        const char *ca_dir, const char *ca_file,
                                        mcp_tls_verify_cb verify_cb, void *cb_ud);
   ```
   - `tls.c` 的 `tls_backend_t` 新增字段 `verify_peer`（`bool`）、`verify_cb`（函数指针）、`cb_ud`、`ca_dir`/`ca_file`（`char*`）。
   - **调用顺序约束（审查修正，关键）**：`SSL_CTX` 在 `mcp_tls_transport_create`（`tls.c:509`）创建，`SSL_new(sctx)` 在 `tls_connect`/`tls_accept` 中调用。`SSL_CTX_set_verify`/`SSL_CTX_set_verify_locations`/`SSL_CTX_set_verify_cb` 是 **context 级调用，MUST 在 `SSL_new` 之前**应用到 `sctx`——即 `mcp_tls_set_verify_peer` 在 `tls_start` 中、创建 SSL_CTX 后 / `SSL_new` 前插入；**不**在 `tls_connect`/`tls_accept` 内部（此时 `SSL_new` 已发生，context 级调用已来不及生效）。
   - **两侧对称**：`tls_connect`（客户端验服务端证书）与 `tls_accept`（服务端验客户端证书，mTLS）两侧都需支持；verify 配置统一在 `tls_start` 注入，客户端/服务端差异仅在既有 `SSL_set_connect_state`/`SSL_set_accept_state`，verify 逻辑两侧相同。
   - `tls_stop` 释放 `ca_dir`/`ca_file` 字符串。

## 文件触及面（P4-b）

| 文件 | 变更 | 风险 |
|---|---|---|
| `include/mcpkit/server/server.h` / `src/server/server.c` | +`mcp_server_set_rate_limit`/`set_lockout` + active getter | 中（会话计数 + 429 路径） |
| `src/server/internals.h` | +`mcp_server` 限速/锁out 配置字段（`rate_limit`/`rate_window_ms`/`lockout_max_fails`/`lockout_ms`）+ `mcp_session` 计数窗口字段（`rate_head`/`rate_ring`/`rate_count`/`auth_fail_count`/`lockout_until`） | 中（结构变更，非 ABI-stable；会话 ring 固定 64 槽） |
| `src/transport/streamable_http.c` | +429 限速响应路径（dispatch 前）+ 锁out 快速拒绝（auth 前）+ `send_bytes` 扩展为可携带多响应头（锁out 429 需 `Retry-After`+`X-Auth-Lockout` 双头，现 `send_bytes` 仅 1 个额外头须扩展） | 中 |
| `src/transport/async_server.c` | +429 限速路径（`worker_dispatch_fn` 中 `entry->session` resolve 后；`send_http_error` 扩展为可携带 `Retry-After` 头，现为固定 4 行模板）；async 侧**无 auth 回调**故锁out 路径不适用 | 中 |
| `include/mcpkit/transport/tls.h` / `src/transport/tls.c` | +`mcp_tls_set_verify_peer` + `tls_backend_t` 新字段 + `SSL_set_verify`/`SSL_CTX_set_verify_locations` | 中（SSL_CTX 配置 + 两侧对称） |
| `tests/unit/test_rate_limit_lockout.c` | 新增（限速窗口 429 + Retry-After + 锁out 触发/解锁 + 未配置零行为） | 低 |
| `tests/unit/test_mtls.c` | 新增（`MCPKIT_BUILD_TLS` 门控：VERIFY_PEER 开/关 + CA 链校验 + verify_cb 拒绝路径） | 低 |
| `tests/CMakeLists.txt` | 注册 2 新测试（TLS 门控块内） | 低 |
| `docs/module-reference.md` | +server.h / tls.h 段落 | 低 |

## 门控与零回归

- **配置驱动**：`rate_limit=0`（默认）→ 无 429 行为；`lockout_max_fails=0`（默认）→ 无锁out；`verify_peer=false`（默认）→ `SSL_VERIFY_NONE`（P0-2 现状）。host 不配置则**所有现有测试零回归**。
- **版本无关**：限速/锁out/mTLS 不绑协议版本（纯运维策略）；但限速 429 路径仅在 Streamable HTTP / async HTTP 生效（stdio/socket 无会话 → 不适用），这是**传输层**限制而非版本限制。
- **审查修正（async 侧）**：`async_server.c` 现状**无 auth 回调、无 401、无 `Authorization` 头处理**（grep 零命中）。故：(1) **锁out 仅 sync 路径适用**（async 无 auth 可锁定）；(2) 限速 429 对 async 对称（`entry->session` 计数），但 async 的 `send_http_error` 须扩展为可携带 `Retry-After` 头；(3) async 路径默认无行为（`rate_limit=0` 时 dispatch 路径不变，零回归）。

## 测试策略（TDD 先红后绿，每功能一个红灯门禁）

| 功能 | 新增测试 | 关键断言 |
|---|---|---|
| 限速 | `test_rate_limit_lockout.c` §1 | `rate_limit=5, window_ms=1000`：第 6 请求 429 + `Retry-After` 存在；窗口滑动后（超时）恢复 200；`rate_limit=0`（未启用）：100 请求全 200（零回归）；stdio 路径不受限 |
| 锁out | 同上 §2 | `max_fails=3, lockout_ms=2000`：连续 3 次 DENY 后第 4 次快速 **429**（统一状态码）+ `X-Auth-Lockout` + `Retry-After`；OK 后计数重置；锁定超时后恢复；旧 bool 回调不触发锁out（零回归） |
| 限速 ring 上限 | 同上 §1 | `rate_limit=80, window_ms=100000`：ring 64 槽溢出时实际计数 <= 配置值（best-effort 宽松断言）；`rate_limit<=64` 时精确 429 |
| mTLS | `test_mtls.c` | 服务端验客户端证书（mTLS）：`verify_peer=true, ca_file=…` 无客户端证书→握手失败；合法客户端证书→成功；客户端验服务端证书：`verify_peer=true, ca_file=…` 无服务端证书→失败；`verify_cb` 返回 0→拒绝；`verify_peer=false`（默认）→连接成功（P0-2 零回归） |
| 全量 | ASan + 零回归 | 未配置时 429/锁out 不出现；`-Wall -Wextra` 零告警 |

## 验收口径

- P4-b 完成 → 库可声明 "会话/客户端级限速 + 认证锁out + 可选 mTLS（P0-2 补强）"。
- 构建要求沿用：C23、libc-only 核心（限速/锁out 纯会话计数，无外部依赖）、`MCPKIT_BUILD_TLS` 门控的 mTLS 面、`-Wall -Wextra` 零告警。
- 每功能独立可交付（限速/锁out 共享 `internals.h` 会话字段故同批；mTLS 独立 TLS 层无耦合）。

## 与 P3 / P0 的衔接点

- 锁out 计数挂 P3 T4 `mcp_http_serve_with_auth_v2` 的 DENY 结果（`mcp_http_auth_result_t.code`）；计数发生在 v2 回调返回后会话 resolve 处（非回调本身）。
- 限速复用 P2 交付的 `mcp_server.c_requests_total` 原子计数器模式（新增会话级计数，不动全局）。
- mTLS 补强 P0-2 `SSL_VERIFY_NONE` 决策（`tls.c:218-219` 注释"Peer verification disabled"）——P4-b 把"不可配"改为"可配但默认关闭"，P0-2 行为零回归。

## 明确不做（留 P5+）

- 全局限速（两级：全局 + 会话）——P4-b 只做会话级；全局上限留 P5。
- 分布式限速（多进程/多实例）——单机会话计数，不跨实例同步。
- mTLS 证书自动轮换 / OCSP 在线校验——`verify_cb` 留 host 自行实现；库只提供 `SSL_CTX` 配置面。
- **async 侧锁out**（审查修正：async 无 auth 回调，P4-b 不加 async 锁out；若 P5 给 async 加 auth 钩子，锁out 可对称扩展）。
