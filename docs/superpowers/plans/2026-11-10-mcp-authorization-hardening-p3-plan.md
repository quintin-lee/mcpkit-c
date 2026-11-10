# P3 实施计划：MCP 2026 授权强化（RFC 9207 / 9728 / CIMD / SEP-835 / OIDC）

**日期**：2026-11-10
**Spec**：`docs/superpowers/specs/2026-11-10-mcp-authorization-hardening-p3-design.md`（`463e49a`）
**范围**：P3 五功能块，可声明 "2026 授权模型完整"。门控 ≥2025-11-25 激活（SHOULD），
2026-07-28 下 9207/9728/CIMD MUST；≤2025-06-18 现状零回归。
**执行模式**：子代理驱动；TDD 先红后绿（每个新测试先在旧实现下跑红，再实现转绿）。
**构建门禁**：C23、核心库仅依赖 libc、`-Wall -Wextra` 零告警；HTTP 面挂
`MCPKIT_BUILD_HTTP`；每任务完成后跑默认/ASan/Release 三套构建 + 全量测试。

## 关键代码现状锚点（编写计划时已核实）

- `src/core/auth.c`（1027 行）**已有**：PKCE（`mcp_pkce_*`）、RFC 8414 AS metadata
  解析（`mcp_oauth_metadata_parse`，含 `client_id_metadata_document_supported` 字段
  L303）、RFC 8693 token exchange、RFC 9207 `iss` 校验器
  （`mcp_oauth_validate_issuer` L89-90，**尚无流程挂钩**）、
  **SEP-991 CIMD 完整实现**（`mcp_oauth_client_metadata_parse/validate/serialize/
  cleanup` L585-840）。
- `include/mcpkit/core/auth.h`：`mcp_oauth_metadata_t`（L70-78）现有 7 字段
  （`issuer/authorization_endpoint/token_endpoint/registration_endpoint/jwks_uri/
  supports_pkce_s256/client_id_metadata_document_supported`），**追加 OIDC 字段
  安全**（本库非 ABI-stable，允许尾部追加）。
- `src/transport/streamable_http.c`：`send_unauthorized`（L126-151）现状固定
  `WWW-Authenticate: Bearer` + `Content-Length: 0`；`validate_http_metadata`
  （L178）已提取 `_meta.protocolVersion`；`handle_post` 按版本路由。
  **grep 证实** `oauth-protected-resource` / `insufficient_scope` / `mcp_server_set_
  protected_resource` 均**不存在** → P3 块 3/4 为净新增。
- `mcp_http_serve_with_auth`（`streamable_http.h:108`）+ `mcp_http_auth_fn`
  （L81，`bool` 返回）已交付；`mcp_http_serve_with_auth_v2` 需新增（枚举返回，
  兼容旧 bool 调用点）。
- 单一版本源：`mcp_session_negotiated`（`session.c:57`）/ `mcp_session_set_
  negotiated`（`session.c:65`），P1+P2 已交付，P3 复用不新增 session 字段。
- 策略层：`include/mcpkit/protocol/policy.h` / `src/protocol/policy.c` 已有 3 个
  `mcp_proto_policy_*` 接口（P1+P2），P3 追加 2 个。
- `tests/CMakeLists.txt`：`MCPKIT_BUILD_HTTP` 块（L190-199）挂 `test_http_serve`/
  `test_http_2026_stateless`；核心测试块（L1-188，无门控）挂 `test_auth_pkce`
  （L360）等。P3 单元测试挂核心块，HTTP 集成测试挂 HTTP 块。

## 任务总览（按执行顺序：1→2→5 纯数据面先行，3→4 共享 HTTP 面）

```
P3 CHUNK
 T1  块1(9207)：PKCE iss 兑换前校验挂钩 + 2026 MUST/2025 SHOULD 门控【先红后绿】
 T2  块2(CIMD)：DCR deprecation 头生成 + CIMD HTTP 端点 + AS metadata 驱动路径【先红后绿】
 T5  块5(OIDC)：mcp_oauth_metadata_t 追加 OIDC 字段 + discovery merge + 门控【先红后绿】
 T3  块3(9728)：Protected Resource Metadata 解析/序列化 + server 配置 API
          + .well-known 端点 + 401 resource_metadata + 回退【先红后绿】
 T4  块4(SEP-835)：mcp_http_serve_with_auth_v2 + 401 增量 scope 头 + 旧调用点兼容【先红后绿】
```

**依赖**：T3/T4 共享 `send_unauthorized` 扩展，同批；T2 的 CIMD HTTP 端点与
T3 的 `.well-known` 端点同属 `handle_post`/路由面，执行时相邻但独立可交付；
T1/T2/T5 纯数据面（`auth.c` + `policy.c`），不依赖 HTTP 面。

---

# T1：块 1 — RFC 9207 授权码兑换前 `iss` 强制校验

**文件**：`include/mcpkit/core/auth.h` + `src/core/auth.c` +
`include/mcpkit/protocol/policy.h` / `src/protocol/policy.c` +
`tests/unit/test_auth_rfc9207_cimds.c`（新增）。

1. **`auth.h`/`auth.c`**：
   - `mcp_oauth_build_token_request_pkce` 增加**向后兼容变体**
     `mcp_oauth_build_token_request_pkce_iss(ctx, code, code_verifier,
     redirect_uri, client_id, expected_issuer, body_out)`（旧签名保留，
     调 `_iss` 变体传 `expected_issuer=NULL`）。
   - `mcp_oauth_token_response_parse` 在解析后提取响应 `iss` 字段（若存在），
     新增**可选出参** `const char **out_iss`（NULL 安全，`out_iss!=NULL` 时
     回填借用语义指针指向响应 JSON 的 iss 字符串；调用方在 JSON 销毁前消费）。
     若响应含 `iss` 且调用方提供 `expected_issuer`（新增入参
     `expected_issuer`，NULL 跳过校验）→ `mcp_oauth_validate_issuer` 失败即
     返回 `MCP_ERR_PROTOCOL`。
   - 旧 `mcp_oauth_token_response_parse` 调用点：签名加 `expected_issuer` 形参
     （尾部追加，`=NULL` 保持现状）。
2. **`policy.c`/`policy.h`**：P3 净新增 2 个策略接口（现状 `policy.c` 仅有 3 个：
   `method_deprecated`/`cache_defaults_applied`/`sort_lists_by_name`）：
   ```c
   bool mcp_proto_policy_auth_hardening_active(mcp_context_t *ctx,
                                                const mcp_session_t *session); /* T1 建 */
   bool mcp_proto_policy_cimd_preferred(mcp_context_t *ctx,
                                         const mcp_session_t *session);         /* T2 建 */
   ```
   `auth_hardening_active` 返回 `mcp_proto_at_least(negotiated, MCP_PROTO_2025_11_25)`；
   **MUST 级**判定内联于 `auth.c`：`negotiated==MCP_PROTO_2026_07_28` 时 `iss`
   缺失/不匹配 MUST 拒绝；`MCP_PROTO_2025_11_25` 下 SHOULD（不匹配记 WARN 不阻断）。
   - 实现落在 `auth.c` 的 `mcp_oauth_token_response_parse` 内：新增
     `bool must_check_iss` 形参（尾部追加），true（2026）时 `iss` 不匹配/缺失
     按 MUST 处理，false（≤2025）按 SHOULD（缺失放行，不匹配 WARN）。
3. **测试**（`test_auth_rfc9207_cimds.c` 新增 §1）：
   - iss 匹配（2026）→ 解析成功。
   - iss 不匹配（2026，MUST）→ `MCP_ERR_PROTOCOL`。
   - iss 缺失（2026）→ 按 SEP-2468，响应可省略 `iss` → 放行（记录为可省略）。
   - iss 不匹配（2025，SHOULD）→ 返回 OK + WARN（非阻断）。
   - `mcp_oauth_build_token_request_pkce_iss` 携带 `expected_issuer` 的 body
     不额外改变（iss 校验发生在响应侧，请求侧仅透传 expected）。
   - 旧 `mcp_oauth_token_response_parse`（`must_check_iss=false`）行为不变（零回归）。

**先红**：`mcp_oauth_token_response_parse` 无 `must_check_iss`/`expected_issuer`
形参前，2026 iss 不匹配场景测试失败。转绿后 `ctest -R test_auth_rfc9207_cimds`。

---

# T2：块 2 — CIMD 取代 DCR（SEP-991）

**文件**：`include/mcpkit/core/auth.h` + `src/core/auth.c` +
`include/mcpkit/transport/streamable_http.h` + `src/transport/streamable_http.c` +
`tests/unit/test_auth_rfc9207_cimds.c`（§2）。

1. **`auth.h`/`auth.c`**：
   - 新增 `mcp_oauth_build_dcr_deprecation_headers(ctx, out_deprecation,
     out_sunset)`：生成 `Deprecation: true` + `Sunset: <RFC 1123 date>` 两串；
     窗口常量 `MCP_OAUTH_DCR_DEPRECATION_WINDOW_DAYS 365`（现值 + 365 天）。
   - **复用**既有 `mcp_oauth_client_metadata_serialize`（L777）+
     `mcp_oauth_client_metadata_parse`（L617）+ `mcp_oauth_client_metadata_validate`
     （L743）——**不重建**。
2. **`streamable_http.c`**：`handle_post`/`handle_get` 新增 GET 路由
   `/.well-known/cim/<client-id>`（或 host 配置路径）：返回
   `mcp_oauth_client_metadata_serialize` 输出，`Content-Type: application/json`；
   2026 下无 `MCP-Protocol-Version` 头也放行（元数据文档非 RPC）。
   - AS metadata `client_id_metadata_document_supported=true`（L303 已解析）时，
     客户端注册路径选择 CIMD（文档记录于 `module-reference.md`；本任务在
     server 侧暴露 CIMD 端点供客户端拉取）。
3. **门控**：`cimd_preferred`（`policy.c`，本任务新建）= `mcp_proto_at_least(
   negotiated, MCP_PROTO_2025_11_25)`；true 时 DCR 端点（若 host 实现）注入
   deprecation 头；2026 下 CIMD MUST。
4. **测试**（`test_auth_rfc9207_cimds.c` §2）：
   - CIMD 文档往返：serialize → parse → validate 一致（client_id/client_name/
     redirect_uris/grant_types）。
   - `mcp_oauth_build_dcr_deprecation_headers` 输出 `Deprecation: true` + 合法
     `Sunset` 日期（=now+365d）。
   - HTTP 集成（挂 `test_http_9728_scp835.c`，T3 同文件）：GET
     `/.well-known/cim/<id>` 200 返回 JSON；`client_id_metadata_document_
     supported=true` 时注册路径选 CIMD。
   - DCR deprecation 头在 `cimd_preferred` 激活下存在；`≤2025-06-18` 无
     deprecation 头（零回归）。

**先红**：`mcp_oauth_build_dcr_deprecation_headers` / CIMD GET 路由不存在前，
测试失败。转绿后 `ctest -R "test_auth_rfc9207_cimds|test_http_9728_scp835"`。

---

# T5：块 5 — OIDC Discovery 1.0 合并

**文件**：`include/mcpkit/core/auth.h` + `src/core/auth.c` +
`tests/unit/test_auth_rfc9207_cimds.c`（§5）。

1. **`auth.h`**：`mcp_oauth_metadata_t`（L70-78）**尾部追加** OIDC 字段
   （布局变更，非 ABI-stable 允许）：
   ```c
   char userinfo_endpoint[256];
   char end_session_endpoint[256];
   char claims_supported[256];      /* 简化：JSON 子串或空 */
   char id_token_signing_algs[256]; /* 简化：空格分隔 */
   char scopes_supported[256];      /* 简化：空格分隔 */
   ```
   - `mcp_oauth_metadata_parse` 扩展解析上述 OIDC 字段（可选，缺失不报错）。
2. **`auth.h`/`auth.c`**：新增 `mcp_oauth_discovery_merge(ctx, oidc_json,
   as_json, out_meta)`：将 OpenID Provider Discovery 与 AS metadata 合并为单一
   `mcp_oauth_metadata_t`（AS 字段优先，OIDC 专有字段补全；任一为 NULL 时
   仅解析另一份）。
3. **门控**：`auth_hardening_active`（≥2025-11-25）激活；2026 下 MUST 支持
   读取 OIDC discovery（缺失 OIDC 专有字段不阻断，仅 AS 必填字段必检）。
4. **测试**（`test_auth_rfc9207_cimds.c` §5）：
   - OIDC discovery JSON 解析（`userinfo_endpoint`/`end_session_endpoint`/
     `claims_supported`/`id_token_signing_alg_values_supported`/
     `scopes_supported`）。
   - AS+OIDC 合并优先级：AS 有 `token_endpoint` 时以 AS 为准，OIDC 补
     `userinfo_endpoint`。
   - 仅 OIDC（as_json=NULL）/ 仅 AS（oidc_json=NULL）合并正确。
   - 缺 OIDC 专有字段不报错（可选）。
   - 2026 session 下缺 AS 必填字段（issuer/authorization_endpoint/
     token_endpoint）→ `MCP_ERR_PROTOCOL`（MUST）。

**先红**：`mcp_oauth_discovery_merge` 不存在、`mcp_oauth_metadata_t` 无 OIDC
字段前，测试失败。转绿后 `ctest -R test_auth_rfc9207_cimds`。

---

# T3：块 3 — RFC 9728 Protected Resource Metadata 强制

**文件**：`include/mcpkit/core/auth.h` + `src/core/auth.c` +
`include/mcpkit/server/server.h` + `src/server/server.c` +
`include/mcpkit/transport/streamable_http.h` + `src/transport/streamable_http.c` +
`tests/unit/test_http_9728_scp835.c`（新增，挂 `MCPKIT_BUILD_HTTP`）。

1. **`auth.h`/`auth.c`**：
   - 新增 `typedef struct mcp_protected_resource_metadata { char issuer[256];
     char resource[512]; char authorization_server[512]; char scope[256];
     char jwks_uri[256]; } mcp_protected_resource_metadata_t;`。
   - `mcp_oauth_protected_resource_parse(ctx, json_str, len, out_meta)`：
     解析 RFC 9728 字段（`resource`/`authorization_servers[]` 取首 /
     `scopes_supported`/`jwks_uri`/`issuer`）。
   - `mcp_oauth_protected_resource_serialize(ctx, meta)`：序列化
     `.well-known/oauth-protected-resource` 响应体。
2. **`server.h`/`server.c`**：新增 `mcp_server_set_protected_resource_metadata(
   ctx, srv, const mcp_protected_resource_metadata_t *pr_meta)` 配置 API（host
   注入；未配置时端点 404）。`mcp_server` struct（`internals.h`）追加
   `mcp_protected_resource_metadata_t pr_meta; bool has_pr_meta;`（P3 内部状态，
   不影响 P1+P2 已交付字段）。
3. **`streamable_http.c`**：
   - 新增 GET `/.well-known/oauth-protected-resource` 路由：host 已配置
     `pr_meta` 时 200 返回 `mcp_oauth_protected_resource_serialize` 输出；
     未配置 404。
   - `send_unauthorized`（L126）扩展：`auth_hardening_active` 且 host 配置了
     `pr_meta` → 401 头改为
     `WWW-Authenticate: Bearer realm="<resource>",
     resource_metadata="<url>"`（RFC 9728 §3.2）；未配置 → 保持现状裸
     `Bearer`（≤2025-06-18 零回归）。
   - **门控**：`mcp_proto_policy_auth_hardening_active`（≥2025-11-25 SHOULD，
     2026 MUST）。MUST 级：2026 下 host 若未配置 `pr_meta`，401 仍发裸
     Bearer 但记 WARN（不硬失败，因 host 可选配置）；SHOULD 级仅提示。
4. **测试**（`test_http_9728_scp835.c` 新增 §3）：
   - `.well-known/oauth-protected-resource` 端点：配置 → 200 返回 JSON；
     未配置 → 404。
   - `mcp_server_set_protected_resource_metadata` 后 401 头携带
     `resource_metadata` URL。
   - 未配置 → 401 头为裸 `Bearer`（零回归守护）。
   - `mcp_oauth_protected_resource_parse` 字段解析 + `serialize` 往返。
   - 2026 session 下 401 MUST 携带 `resource_metadata`（配置时）；
     2025-11-25 下 SHOULD（缺失 WARN 不阻断）。

**先红**：`.well-known` 端点 / `mcp_protected_resource_*` / `mcp_server_set_
protected_resource_metadata` 不存在前，测试失败。转绿后
`ctest -R test_http_9728_scp835`。

---

# T4：块 4 — 增量 scope 同意（SEP-835）

**文件**：`include/mcpkit/transport/streamable_http.h` +
`src/transport/streamable_http.c` + `tests/unit/test_http_9728_scp835.c`（§4）。

1. **`streamable_http.h`**：
   - 新增 `typedef enum { MCP_HTTP_AUTH_OK, MCP_HTTP_AUTH_DENY,
     MCP_HTTP_AUTH_SCOPE_INSUFFICIENT } mcp_http_auth_result_t;`。
   - 新增 `typedef struct mcp_http_auth_result {
     mcp_http_auth_result_t code;
     char scope[256];              /* 空格分隔，仅 SCOPE_INSUFFICIENT 时填 */
     const char *resource_metadata;/* 可选，401 resource_metadata 头 */
     const char *error_description;/* 可选 */
   } mcp_http_auth_result_t;`。
   - 新增 `typedef mcp_http_auth_result_t (*mcp_http_auth_fn_v2)(
     mcp_context_t *ctx, const char *bearer_token, void *user_data);`。
   - 新增 `mcp_status_t mcp_http_serve_with_auth_v2(ctx, server, io,
     mcp_http_auth_fn_v2 auth_fn, void *auth_user_data);`。
   - **旧 `mcp_http_auth_fn`（bool）保留**，`mcp_http_serve_with_auth`（L108）
     签名不变；内部映射 bool→`MCP_HTTP_AUTH_OK/DENY`。
2. **`streamable_http.c`**：
   - `send_unauthorized` 扩展形参（`const char *scope, const char *resource_
     metadata, const char *error_description`）：拼 RFC 6750 + SEP-835 组合
     挑战串
     `WWW-Authenticate: Bearer error="insufficient_scope",
     error_description="...", scope="<scope>", resource_metadata="<url>"`；
     各可选参数为 NULL 时省略对应片段。
   - `mcp_http_serve_with_auth_v2`：auth_fn_v2 返回 `MCP_HTTP_AUTH_
     SCOPE_INSUFFICIENT` 时 → 401 + 带 scope/resource_metadata 的
     `send_unauthorized`；`DENY` → 裸 401；`OK` → 放行。
   - **门控**：`auth_hardening_active` 且 401 因 scope 不足触发时 MUST 携带
     `scope`（2026）；SHOULD（2025-11-25）；`resource_metadata` 与 T3 共用
     门控。
3. **测试**（`test_http_9728_scp835.c` §4）：
   - scope 不足 401 头含 `error="insufficient_scope"` + `scope` +
     `resource_metadata` 全字段（2026）。
   - 旧 `mcp_http_auth_fn`（bool）调用 → 401 仍为裸 `Bearer`（零回归守护）。
   - `mcp_http_auth_fn_v2` 返回 `DENY` → 裸 401（无 scope 段）。
   - `SCOPE_INSUFFICIENT` 且 `resource_metadata=NULL` → 401 头省略
     `resource_metadata`（仅 scope 段）。
   - 2025-11-25 session 下 scope 不足 → SHOULD（携带 scope，缺失不硬失败）。

**先红**：`mcp_http_serve_with_auth_v2` / `send_unauthorized` 扩展形参不存在前，
测试失败。转绿后 `ctest -R test_http_9728_scp835`。

---

# 风险与回归守护

- **共享 `send_unauthorized` 扩展**（T3+T4 同批）：新形参全部尾部追加且
  默认 NULL → 旧 `mcp_http_serve_with_auth`（bool）路径 401 仍为裸 Bearer，
  由 `test_http_serve` 既有 6 例守住。
- **`mcp_oauth_metadata_t` 尾部追加 OIDC 字段**（T5）：非 ABI-stable 库，
  追加安全；`mcp_oauth_metadata_parse` 的 memset 已覆盖新字段（0 初始化），
  无未初始化读。
- **`mcp_oauth_token_response_parse` 签名加形参**（T1）：尾部追加
  `expected_issuer`/`must_check_iss`，既有调用点传 NULL/false 保持现状；
  全量测试守。
- **DCR deprecation 头**（T2）：仅当 `cimd_preferred` 激活注入，≤2025-06-18
  不注入（零回归）。
- **≤2025-06-18 401 形态**：由 `test_http_serve` + `test_http_2026_stateless`
  既有断言（裸 Bearer）守住，P3 不改旧路径。

## 验收

- **P3 完成** → 库可声明 "2026 授权模型完整"（9207/9728/CIMD/SEP-835/OIDC
  全部门控到位；2026 MUST 级行为在 2026 session 下激活；≤2025-06-18 现状
  零回归由 test_http_serve/test_http_2026_stateless 守）。
- 构建门禁：C23、libc-only 核心（T1/T2/T5 数据面）、`MCPKIT_BUILD_HTTP` 门控
  集成面（T2/T3/T4 HTTP）、`-Wall -Wextra` 零告警、ASan 无泄漏/UAF、CI 全绿。
- 每任务独立可交付（T1/T2/T5 数据面先行，T3/T4 依赖 HTTP 面且共享
  `send_unauthorized` 扩展故同批）；分片顺序 1→2→5→3→4，块间无强制依赖。

## 后续（不在本计划）

- P4 候选：RFC 9728 issuer 自动发现（`/.well-known/oauth-authorization-server`
  探测 AS）、mTLS 双向认证、`x-mcp-header`（SEP-2243）、限速/锁out、
  `async_server.c`/`streamable_http.c` 大文件拆分。
