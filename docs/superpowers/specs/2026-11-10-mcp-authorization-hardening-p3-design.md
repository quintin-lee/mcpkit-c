# MCP 2026 授权强化（P3）独立设计

**日期**：2026-11-10
**状态**：待评审
**前置**：P1+P2 已交付并提交（version strategy / session 存储 negotiated 版本 / 2026 无状态核心 +
`mcp_proto_policy_*` 策略层），见
`docs/superpowers/specs/2026-11-09-mcp-2025-11-25-2026-07-28-support-design.md` L137-167。
本文是该文档中"P3 授权强化"的独立 spec，按用户既定策略拆出单独设计循环。

## 背景与现状

P1+P2 让 mcpkit-c 可声明支持 2026-07-28 **核心**。授权面仍停留在 2025-11-25
前状态：库能解析 metadata，但 2026 授权模型（issuer 校验、CIMD、
protected-resource discovery、增量 scope 同意）尚未交付。P3 完成后库可声明
"2026 授权模型完整"。

`src/core/auth.c`（1027 行）**已有**的基础（复用，不重建）：

| 已有 | 位置 | 说明 |
|---|---|---|
| RFC 7636 PKCE S256 | `mcp_pkce_compute_challenge` / `mcp_pkce_generate` | verifier/challenge 计算 |
| RFC 8414 AS metadata | `mcp_oauth_metadata_parse` | 解析 issuer/端点/`client_id_metadata_document_supported` |
| RFC 8693 token exchange | `mcp_oauth_build_token_exchange_request` / `*_response_parse` | subject/actor 兑换 |
| RFC 9207 `iss` 校验 | `mcp_oauth_validate_issuer`（L89-90 注释已标 SEP-2468） | 单发校验器，**尚无流程挂钩** |
| SEP-991 CIMD | `mcp_oauth_client_metadata_parse/validate/serialize/cleanup` | **已完整** parse/validate/serialize，缺 HTTP 端点与 DCR deprecation |

**P3 的净新增**集中在三处：
1. **版本门控层**：把 9207/9728/CIMD/SEP-835 行为接到 `mcp_session_negotiated`
   单一版本源（P1+P2 已建立的架构），`mcp_proto_policy_*` 扩展 2 个新查询接口。
2. **HTTP 面**：`streamable_http.c` 新增 protected-resource discovery 端点
   + 401 响应携带 scope/resource_metadata（SEP-835）+ CIMD 端点生成。
3. **纯数据面**：`auth.c` 新增 RFC 9728 Protected Resource Metadata 解析、
   OIDC Discovery 1.0 合并、DCR deprecation 标记。

## 门控策略（单一版本源）

复用 P1+P2 的 `mcp_proto_policy_*` 层（`include/mcpkit/protocol/policy.h` /
`src/protocol/policy.c`），新增 2 个查询接口，保持"transport/server 层只调
策略、不直接判版本枚举"的收敛原则：

```c
/* RFC 9728 / 9207 / SEP-835 授权强化在 2026-07-28 下 MUST（MUST NOT 静默） */
bool mcp_proto_policy_auth_hardening_active(const mcp_context_t *ctx,
                                             const mcp_session_t *session);
/* CIMD 优先、DCR deprecation 提示：≥2025-11-25 激活，2026 下 MUST */
bool mcp_proto_policy_cimd_preferred(const mcp_context_t *ctx,
                                     const mcp_session_t *session);
```

- `auth_hardening_active`：`mcp_proto_at_least(negotiated, MCP_PROTO_2025_11_25)`
  为 true（SEP-835 增量 scope 与 OIDC Discovery 同门控）；但 9728/9207/CIMD
  的 MUST 语义仅在 `MCP_PROTO_2026_07_28` 下生效——实现内区分 SHOULD（≥
  2025-11-25）与 MUST（2026）两级，MUST 级行为缺失时记 WARN/ERR，SHOULD 级
  缺失仅提示。
- `cimd_preferred`：`MCP_PROTO_2025_11_25` 起，DCR 端点（若保留）响应头注入
  `Deprecation: true` + `Sunset`（12 个月窗口），≥2025-11-25 激活；2026 下
  CIMD 为唯一推荐路径。
- session 为 NULL（2026 无状态请求作用域 session）时：2026 路径按请求
  `_meta.protocolVersion` 判定（P1+P2 已建立的会话版本写入路径），
  ≤2025-11-25 保守返回 false，保持"现状"零回归。

## 五个功能块

### 1. RFC 9207 — 授权码兑换前 `iss` 强制校验
- **数据面**：`mcp_oauth_build_token_request_pkce` 增加可选
  `expected_issuer` 形参（或新增 `mcp_oauth_build_token_request_pkce_iss`
  变体，保留旧签名向后兼容）。兑换响应解析（`mcp_oauth_token_response_parse`）
  在响应体中提取 `iss` 字段，若存在且 `mcp_oauth_validate_issuer` 失败 →
  返回 `MCP_ERR_PROTOCOL`。
- **门控**：2026 下 MUST（缺失/不匹配即拒绝兑换）；≤2025-11-25 仅 SHOULD
  （校验失败记 WARN 不阻断）。
- **测试**：`test_auth_rfc9207_cimds.c` 单元 — 匹配通过、不匹配拒绝、
  缺失 iss 放行（2026 下缺失按 spec 视为可省略则放行，见 SEP-2468）。

### 2. CIMD 取代 DCR（SEP-991）
- **数据面**：`mcp_oauth_client_metadata_*` 已完整。新增：
  - `mcp_oauth_build_dcr_deprecation_headers(ctx, ...)` — 生成
    `Deprecation: true` + `Sunset: <date>` 头对（12 个月窗口常量
    `MCP_OAUTH_DCR_DEPRECATION_WINDOW_DAYS 365`）。
  - AS metadata `client_id_metadata_document_supported` 字段（L303 已解析）
    驱动：true 时客户端应优先走 CIMD URL 注册，DCR `registration_endpoint`
    标 deprecated。
- **HTTP 面**：`streamable_http.c` 新增 GET
  `/.well-known/cim/<client-id>`（或 host 配置的 CIMD 文档路径）路由，
  返回由 `mcp_oauth_client_metadata_serialize` 生成的文档（`Content-Type:
  application/json`）。2026 下 GET 该端点无 version 头也放行（元数据
  文档非 MCP RPC）。
- **门控**：`cimd_preferred` 激活时 DCR 端点（若 host 仍实现）注入
  deprecation 头；2026 下 CIMD 为 MUST。
- **测试**：CIMD 文档往返（serialize → parse → 校验）、DCR deprecation 头
  存在性、AS metadata `client_id_metadata_document_supported=true` 时
  注册路径选择 CIMD。

### 3. RFC 9728 — Protected Resource Metadata 强制
- **数据面**：`auth.h/.c` 新增
  - `typedef struct mcp_protected_resource_metadata { char issuer[256];
    char resource[512]; char authorization_server[512]; char
    scope[256]; }` 结构 + `mcp_oauth_protected_resource_parse(ctx, json,
    out)` + `mcp_oauth_protected_resource_serialize(ctx, meta)`。
  - `.well-known/oauth-protected-resource` 端点响应构造器。
- **HTTP 面**：`streamable_http.c` 新增 GET
  `/.well-known/oauth-protected-resource` 路由，由 host 注入
  `mcp_protected_resource_metadata_t`（新 server 配置 API
  `mcp_server_set_protected_resource_metadata(ctx, srv, pr_meta)`）生成；
  host 未配置时 404。
- **`WWW-Authenticate` 回退**：`send_unauthorized`（现有，固定 `Bearer`）
  扩展为：当 `auth_hardening_active` 且 host 配置了 protected-resource
  metadata 时，401 头改为
  `WWW-Authenticate: Bearer realm="<resource>", resource_metadata="<url>"`
  （RFC 9728 §3.2 客户端据此发现 AS），并保留裸 `Bearer` 作为无配置时的
  现状回退（≤2025-06-18 行为不变）。
- **门控**：2026 下 MUST（401 必须携带 `resource_metadata` 或可发现
  `.well-known` 端点）；≥2025-11-25 SHOULD；≤2025-06-18 现状（裸 Bearer）。
- **测试**：`.well-known` 端点 200 返回 JSON、401 头携带
  `resource_metadata`、host 未配置时回退裸 Bearer。

### 4. 增量 scope 同意（SEP-835）
- **HTTP 面**：扩展 `mcp_http_serve_with_auth` 的 401 路径（当前只发固定
  `WWW-Authenticate: Bearer`，见 http-auth-middleware spec）。新增：
  - 扩展 `mcp_http_auth_fn` 返回值（`typedef` 改为返回
    `mcp_http_auth_result_t` 枚举：OK / DENY / SCOPE_INSUFFICIENT{scope,
    resource_metadata}）——**保持布尔兼容**：提供
    `mcp_http_serve_with_auth_v2` 或使用 union struct，旧 `mcp_http_auth_fn`
    仍可用（BOOL→OK/DENY 映射），不破坏现有调用点。
  - `send_unauthorized` 增加可选 `scope` / `resource_metadata` 形参：
    401 头拼成
    `WWW-Authenticate: Bearer error="insufficient_scope",
    error_description="...", scope="<space-separated>", resource_metadata="<url>"`
    （RFC 6750 §3 + SEP-835 + RFC 9728 组合挑战串）。
- **门控**：`auth_hardening_active` 且 401 因 scope 不足触发时 MUST 携带
  `scope`；`resource_metadata` 与块 3 共用门控。≤2025-11-25 仅 SHOULD。
- **测试**：scope 不足 401 头格式（`error=insufficient_scope` +
  `scope` + `resource_metadata` 全字段）、布尔旧回调 401 仍为裸 Bearer
  （零回归）。

### 5. OIDC Discovery 1.0 合并（2025-11-25）
- **数据面**：`mcp_oauth_metadata_parse` 扩展解析 OIDC 标准字段（
  `userinfo_endpoint`、`end_session_endpoint`、`claims_supported`、
  `id_token_signing_alg_values_supported`、`scopes_supported`），
  在 `mcp_oauth_metadata_t` 增对应字段（保持现有字段布局不变，追加尾部，
  ABI 风险：本库非 ABI-stable，允许）。
- **发现合并**：新增 `mcp_oauth_discovery_merge(ctx, oidc_json, as_json,
  out_meta)` — 将 OpenID Provider Discovery（RFC 8414 兼容超集）与 AS
  metadata 合并为单一 `mcp_oauth_metadata_t`（AS 字段优先，OIDC 专有
  字段补全）。
- **门控**：`auth_hardening_active`（≥2025-11-25）激活；2026 下 MUST 支持
  读取 OIDC discovery。
- **测试**：OIDC discovery JSON 解析、AS+OIDC 合并优先级、缺 OIDC 专有
  字段时不报错（可选字段）。

## 文件触及面（P3）

| 文件 | 变更 | 风险 |
|---|---|---|
| `include/mcpkit/protocol/policy.h` / `src/protocol/policy.c` | +2 策略接口 | 低（纯新增，现有 3 接口不动） |
| `include/mcpkit/core/auth.h` / `src/core/auth.c` | +9728 解析/序列化、+OIDC 字段、+PKCE iss 变体、+DCR deprecation 头、+discovery merge | 中（`mcp_oauth_metadata_t` 追加字段，C 非 ABI-stable 可接受） |
| `include/mcpkit/server/server.h` / `src/server/server.c` | +`mcp_server_set_protected_resource_metadata` 配置 API | 中 |
| `include/mcpkit/transport/streamable_http.h` / `src/transport/streamable_http.c` | +`/.well-known` 2 路由、+CIMD 路由、`send_unauthorized` 扩展、`mcp_http_serve_with_auth_v2` | 中（HTTP-gated，`MCPKIT_BUILD_HTTP` 后） |
| `tests/unit/test_auth_rfc9207_cimds.c` | 新增（单元，核心库，非 HTTP-gated） | 低 |
| `tests/unit/test_http_9728_scp835.c` | 新增（集成，`MCPKIT_BUILD_HTTP` 门控） | 低 |
| `tests/CMakeLists.txt` | 注册 2 新测试 | 低 |
| `docs/module-reference.md` | +auth.c / server.h / streamable_http.h 段落 | 低 |

> **推迟项**（P3 明确不做，留 P4）：`async_server.c` / `streamable_http.c`
> 大文件拆分（架构审计建议）——P3 不引入，避免在授权重构同期引入过大
> 结构扰动。

## 测试策略（TDD 先红后绿，每功能块一个红灯门禁）

| 块 | 新增测试 | 关键断言 |
|---|---|---|
| 1 (9207) | `test_auth_rfc9207_cimds.c` §1 | iss 匹配/不匹配/缺失三态；2026 MUST vs 2025 SHOULD 行为分叉 |
| 2 (CIMD) | 同上 §2 | serialize→parse 往返；DCR deprecation 头；`client_id_metadata_document_supported` 驱动路径选择 |
| 3 (9728) | `test_http_9728_scp835.c` | `.well-known` 端点 200/404；401 `resource_metadata` 头；未配置回退裸 Bearer |
| 4 (SEP-835) | 同上 §4 | 401 `error=insufficient_scope` + `scope` + `resource_metadata` 组合串；旧布尔回调零回归 |
| 5 (OIDC) | `test_auth_rfc9207_cimds.c` §5 | OIDC 字段解析；AS+OIDC 合并优先级；可选字段缺失不报错 |
| 全量 | ASan + 零回归 | ≤2025-06-18 401 形态不变；`-Wall -Wextra` 零告警 |

## 验收口径

- P3 完成 → 库可声明 "2026 授权模型完整"（9207/9728/CIMD/SEP-835/OIDC
  全部门控到位，2026 MUST 级行为在 2026 session 下激活，≤2025-06-18 现状
  零回归由现有测试守住）。
- 构建要求沿用：C23、libc-only 核心（auth/9728/OIDC 数据面）、
  `MCPKIT_BUILD_HTTP` 门控的集成面、`-Wall -Wextra` 零告警。
- 每功能块独立可交付（块 1/2/5 纯数据面先行，块 3/4 依赖 HTTP 面），
  执行计划按 1→2→5→3→4 分期切片，块间无强制依赖但 3/4 共享
  `send_unauthorized` 扩展故同批。

## 与 P1+P2 的衔接点

- 单一版本源：`mcp_session_negotiated`（`session.c:57`）——P3 不新增
  session 版本字段，复用 P1+P2 已交付的 `negotiated` 字段与
  `mcp_session_set_negotiated`（`session.c:65`）。
- 策略层：`mcp_proto_policy_*`（P1+P2 建立的"transport/server 只调策略"
  收敛原则），P3 追加 2 个接口，沿用 `policy.c` 现有
  `mcp_proto_at_least` 判据。
- 无状态 2026 请求：`streamable_http.c` 请求作用域 session 已带
  `_meta.protocolVersion`（P1+P2 写入），P3 的 HTTP 门控直接读该
  字段，不改 2026 无状态路径。

## 明确不做（P4 候选）

- RFC 9728 issuer 发现（`/.well-known/oauth-authorization-server` 自动
  探测 AS）——本 spec 只做响应端点与 metadata 解析，自动探测留 P4。
- `x-mcp-header`（SEP-2243）。
- mTLS 双向认证（P0-2 已选 `SSL_VERIFY_NONE`，独立关注）。
- 限速/锁out。
