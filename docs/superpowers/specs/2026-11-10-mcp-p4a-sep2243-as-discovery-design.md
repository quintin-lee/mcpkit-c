# MCP P4-a 授权面：SEP-2243 x-mcp-header + RFC 9728 AS 自动发现

**日期**：2026-11-10
**状态**：待评审
**前置**：P3 已交付（commit `4e0f39f`/`973e447`/`a60d9a5`/`ec5f004`/`15b931e`/`5d2526d`）。P4-a 复用 P3 交付的门控基础设施（`mcp_session_negotiated` 单一版本源、`mcp_proto_at_least`、`mcp_oauth_discovery_merge` P3 T5、PRM 结构 P3 T3）。

## 背景与现状

P3 完成后库声明 "2026 授权模型完整"，但仍停留在 2026 授权模型的核心面。P4-a 补齐两块**P3 明确推迟**的能力（P3 spec §「明确不做」：RFC 9728 issuer 自动探测留 P4、`x-mcp-header` SEP-2243 留 P4）：

| 能力 | 现状 | 缺口 |
|---|---|---|
| SEP-2243 标准头 `Mcp-Method`/`Mcp-Name` | P2 已交付校验（`-32020 HeaderMismatch`） | — |
| SEP-2243 自定义头 `Mcp-Param-{Name}`（`x-mcp-header` 注解） | 无 | `registry.c` 注解校验、`streamable_http.c` 头一致性校验（`-32001`）、客户端镜像 |
| RFC 9728 AS 自动发现 | P3 T3 交付响应端（server 侧 PRM 端点 + 401 头） | 客户端探测：`issuer` 拼 `/.well-known/oauth-authorization-server` 拉 AS metadata |

### SEP-2243 语义（已查证）

`x-mcp-header` 是工具 `inputSchema` 属性的 JSON Schema 扩展，标注某参数值在 2026 Streamable HTTP 下须镜像为 `Mcp-Param-{Name}` HTTP 头（`Name` 取自注解值）。约束（spec 原文 + 澄清 issue #2762）：
- 值非空、匹配 RFC 9110 `tchar`、无 CR/LF、case-insensitively 唯一；
- **仅**允许 `string`/`integer`/`boolean` 型（`number` 型禁止；违反 → 客户端剔除该工具）；
- 值编码 5 步：type 转换 → 首尾空白检查（空白→base64）→ ASCII 校验（非 ASCII→base64）→ 控制字符检查（→base64）→ base64 sentinel `=?base64?{B64}?=`（RFC 4648 标准 base64，**非** url-safe、带 padding）；
- 服务端 MUST 校验头与 body 一致，失败 → HTTP 400 + JSON-RPC `-32001 HeaderMismatch`。

### AS 自动发现语义（RFC 8414 §3.1 探测 + P3 T5 合并）

给定 RFC 9728 PRM 的 `authorization_server`（issuer URL），客户端探测 AS metadata：
- RFC 8414 §3.1 探测规则：`issuer` 去掉尾 `/`，在其 authority 后拼 `/.well-known/oauth-authorization-server`；
- 拉取后解析为 `mcp_oauth_metadata_t`，用 P3 T5 `mcp_oauth_discovery_merge` 与既有 AS metadata 合并（AS 字段优先、OIDC 专有补全）。

## 净新增（P4-a）

### 数据面工具（`src/core/auth.c`，非 HTTP-gated）

1. **`mcp_base64_encode/decode`**（RFC 4648 §4 标准 base64，带 `=` padding，可带 `\n` 处理；库内现有 `b64url_encode_32` 是定长 32B url-safe unpadded，**不满足** SEP-2243 的变长标准 base64 需求，故净新增）：
   ```c
   char *mcp_base64_encode(mcp_context_t *ctx, const uint8_t *in, size_t in_len); /* malloc'd, NUL-terminated */
   int mcp_base64_decode(mcp_context_t *ctx, const char *in, size_t in_len, uint8_t **out, size_t *out_len);
   ```
   复用 `src/core/sha1.c` 的 allocator 约定；`decode` 拒绝非 base64 字符与错误 padding。

2. **`mcp_oauth_discover_as`**（AS 自动发现，数据面拼 URL + 调 host fetch 回调 + merge）：
   ```c
   typedef int (*mcp_http_fetch_fn)(void *ud, const char *url, char **body_out, size_t *body_len_out);
   /* 分配器约定（审查修正）：body_out MUST 由 fetch 实现方用 libc malloc 分配，
      调用方 mcp_oauth_free_string(ctx, body_out) 释放。fetch 实现方不得用 ctx 路由的
      自定义分配器（否则 ctx 非默认分配器时 mismatch free）。NULL fetch 视为探测失败 → MCP_ERR_PROTOCOL。 */
   mcp_status_t mcp_oauth_discover_as(mcp_context_t *ctx,
                                      const mcp_protected_resource_metadata_t *pr_meta,
                                      mcp_http_fetch_fn fetch, void *fetch_ud,
                                      const mcp_oauth_metadata_t *existing_as, /* 可为 NULL */
                                      mcp_oauth_metadata_t *out_merged);
   ```
   - 由 `pr_meta->authorization_server`（issuer）拼 RFC 8414 §3.1 探测 URL；
   - 调 `fetch` 拉 AS discovery JSON；
   - `mcp_oauth_metadata_parse` 解析 → 若 `existing_as != NULL` 走 `mcp_oauth_discovery_merge(existing_as, parsed, out_merged)`，否则 `*out_merged = *parsed`；
   - 库**不内置** HTTP 拉取（避免把网络耦合进核心库），拉取交给 host 注入的 `fetch`（host 决定用 `mcp_http_request` 或自身 HTTP 栈）。

### 版本门控（`policy.c`，1 新接口）

SEP-2243 自定义头行为**仅 2026-07-28 激活**。遵循 P1-P3 确立的**「单版本源 + 两参 `(ctx, session)`」策略层模式**（`policy.c` 既有 5 接口全部只收 `(ctx, session)`）：

```c
/* x-mcp-header / Mcp-Param-* 行为在 2026-07-28 下 MUST（MUST 级缺失即 -32001），
   ≤2025-11-25 下 x-mcp-header 注解可存在但客户端 MAY 忽略（SHOULD 级，不校验头一致性）。 */
bool mcp_proto_policy_sep2243_active(const mcp_context_t *ctx, const mcp_session_t *session);
```
- 实现：`mcp_proto_at_least(mcp_session_negotiated(ctx, session), MCP_PROTO_2026_07_28)`（与既有 5 接口签名完全一致，**不引入 message/meta 形参**）；
- session NULL（2026 无状态路径）：`mcp_session_negotiated(ctx, NULL)` 依 `session.h:44-50` 语义保守返回 `MCP_PROTO_2024_11_05` 基线 → `sep2243_active = false`。**P4-a 不在 policy 层读 `_meta.protocolVersion`**——无状态 2026 请求的 SEP-2243 门控由调用方在请求作用域 session 已写入 `negotiated` 字段后（P1+P2 既有写入路径）再调 `sep2243_active`，policy 层本身保持纯 `(ctx, session)` 签名（与 P3 一致的架构收敛）。
- 这是 P4-a 与 P3 的差异点显式声明：P3 spec 说「session NULL 时 2026 路径按请求 `_meta.protocolVersion` 判定」，但 P3 实现实际是调用方在建请求作用域 session 时已写入 `negotiated`，policy 层仍只读 session；P4-a 沿用此实现（不偏离 P3 实际架构）。

### 错误码（`message.h`，1 新增）

**注意**：`MCP_RPC_HEADER_MISMATCH` 已被 P2 占用于 `Mcp-Method`/`Mcp-Name` 标准头校验，值为 `-32020`（`message.h:47`），且在 `streamable_http.c` 有 5 处 `send_rpc_error(... MCP_RPC_HEADER_MISMATCH ...)` 调用点。**P4-a 不改动 `-32020` 的语义与值**（避免 P2 零回归被破坏）。

```c
MCP_RPC_XMCP_PARAM_MISMATCH = -32001,   /* P4-a SEP-2243 自定义头 Mcp-Param-* 一致性校验失败 */
```
- 新枚举名 `MCP_RPC_XMCP_PARAM_MISMATCH`（避免与既有 `MCP_RPC_HEADER_MISMATCH` 同名冲突）；
- 值 `-32001` 与 `-32020` 同属 `-32000..-32099` 实现定义区间，语义独立：`-32001` 专用于自定义 `Mcp-Param-*` 头一致性/缺失失败，`-32020` 保持 P2 标准头语义不变；
- 现有 `-32020/-32021/-32022` 全部不动。

## 四个功能切片（顺序 TDD，每片先红后绿）

### S1 注册期 `x-mcp-header` 注解校验（`src/server/server.c`）

**归属说明**：`mcp_server_add_tool` 实际定义在 `src/server/server.c:261`（`registry.c` 仅含 `mcp_tool_new`/`mcp_resource_new` 等对象构造器，不含 add_tool 注册路径）。故 S1 校验逻辑落在 **`server.c` 的 `mcp_server_add_tool` 内**，递归遍历 `mcp_tool->schema`（inputSchema JSON 树，任意嵌套深度）：
- 每个 `x-mcp-header` 注解值 MUST 非空、匹配 RFC 9110 `tchar`、无 CR/LF；
- 同工具内 `x-mcp-header` 值 case-insensitively 唯一（重复 → `MCP_ERR_INVALID_ARGUMENT`）；
- 注解 MUST 落在 `type` 命中 {`string`,`integer`,`boolean`} 任一的属性上；JSON Schema 允许 `type` 为数组或联合（如 `["string","integer"]`）——**命中任一允许类型即通过**，全为禁止类型（`number`/`object`/`array`/`null`）才拒（`type` 缺省/省略视为允许，JSON Schema 默认任意型——但 SEP-2243 注解语义要求原语型，缺省 `type` 时按**允许**处理，host 自行负责保证原语）；
- 校验失败**不注册**该工具（不污染 server.tools 数组），返回错误码让 host 决策。
- **不改** `mcp_tool` 结构（注解留在 schema JSON 树内，不新增 C 字段；`mcp_tool->schema` 即权威源）。

### S2 streamable_http `Mcp-Param-*` 头一致性校验（`src/transport/streamable_http.c`）
2026 请求（`sep2243_active` 门控）下：
- 解析请求携带的所有 `Mcp-Param-*` 头；对每个头，找对应工具的 `x-mcp-header` 注解（Name 反查 `Mcp-Param-{Name}`）；
- 解码 sentinel（`=?base64?…?=` 前缀 → `mcp_base64_decode` 还原）；
- 与 body 对应参数值做**数值/类型比较**（integer 数值比较，spec issue #2762 第 5 条澄清：数值比较而非字符串比较）；
- 缺失（body 有值但头缺）/不匹配/解码失败 → HTTP `400` + JSON-RPC `-32001 HeaderMismatch`（消息含字段名）；
- 复用 P2 `validate_http_metadata` 门控模式（在 `handle_post` 的 2026 分支、body 解析后、dispatch 前插入）。
- ≤2025-11-25 请求**不校验** `Mcp-Param-*`（SHOULD 级，零回归）。

### S3 客户端 `Mcp-Param-*` 镜像（`src/client/client.c` + `client.h`/`internals.h`）

**设计决策（审查修正）**：`mcp_client_t` 现状**无任何 tools schema 缓存**（`client/internals.h` 仅含 ctx/t/next_id/version/各回调/bearer_token）。S3 须**扩展 `mcp_client_t` 增加可选 schema cache**：

```c
/* host 通过新 API 注入该工具最近一次 tools/list 的 inputSchema（mcp_json_value_t 深克隆）。
   NULL = 客户端从未 list → 不发 Mcp-Param-* 头（spec：无 schema 的客户端 SHOULD 不带头）。 */
mcp_status_t mcp_client_set_tool_schema(mcp_context_t *ctx, mcp_client_t *client,
                                        const char *tool_name, mcp_json_value_t *input_schema);
/* 内部：mcp_client_t 新增 mcp_tool_schema_cache_t *schema_cache（NULL 默认），
   按 tool_name 索引；call_tool 时查 cache，命中则提取 x-mcp-header 注解镜像。 */
```

`mcp_client_call_tool`（及 `mcp_client_call_tool_mrtr`）：
- 查 `client->schema_cache` 中该 tool_name 的 inputSchema；命中则提取 `x-mcp-header` 标注参数，`mcp_client_set_tool_schema` 未调用/无 cache → 不发 `Mcp-Param-*` 头（被拒后 host 可重新 list 并 `mcp_client_set_tool_schema` 注入后 retry）；
- 对每个标注参数：type 转换（string 原样 / integer 十进制串 / boolean 小写 `true|false`）→ 5 步编码规则 → 设 `Mcp-Param-{Name}` 头；
- 参数值为 `null` 或缺失 → **不**设头（spec 表格：null/missing → 客户端省略头、服务端不期望头）；
- 仅 2026 连接生效（客户端侧门控读 `mcp_client` 协商版本 `client->version`）。

### S4 AS 自动发现（`src/core/auth.c` + `auth.h`）
即「数据面工具 §2」的 `mcp_oauth_discover_as`。纯数据面，无版本门控（host 何时调用由 host 决定；通常 2026 下 PRM 的 `authorization_server` 非空时调用）。

## 文件触及面（P4-a）

| 文件 | 变更 | 风险 |
|---|---|---|
| `include/mcpkit/protocol/message.h` | +`MCP_RPC_XMCP_PARAM_MISMATCH = -32001`（不动既有 `-32020/-32021/-32022`） | 低（纯新增枚举名） |
| `include/mcpkit/protocol/policy.h` / `src/protocol/policy.c` | +`mcp_proto_policy_sep2243_active`（纯 `(ctx,session)` 签名，不动现有 5 接口） | 低 |
| `include/mcpkit/client/client.h` / `src/client/internals.h` | +`mcp_client_set_tool_schema` + `mcp_client_t` 新增 `schema_cache` 字段 | 中（扩展 client 结构 + 新注入 API） |
| `include/mcpkit/core/auth.h` / `src/core/auth.c` | +`mcp_base64_encode/decode`、+`mcp_http_fetch_fn` typedef、+`mcp_oauth_discover_as` | 中（base64 净新增工具函数） |
| `src/server/server.c` | `mcp_server_add_tool` 内 +`x-mcp-header` 注解递归校验（遍历 schema 树 + 拒绝注册路径） | 中 |
| `src/transport/streamable_http.c` | +`Mcp-Param-*` 头一致性校验（2026 门控）+ 400/-32001 响应 | 中（复用 P2 门控模式，插入 dispatch 前） |
| `src/client/client.c` | +`x-mcp-header` 镜像（call_tool 读 schema + 5 步编码 + 设头） | 中（编码规则 5 步 + 无 schema 回退） |
| `tests/unit/test_sep2243_xmcp_header.c` | 新增（单元，核心库：S1 registry 校验 + S4 discovery 数据面 + base64 往返） | 低 |
| `tests/unit/test_http_mcp_param.c` | 新增（集成，`MCPKIT_BUILD_HTTP` 门控：S2 400/-32001 + S3 客户端镜像断言） | 低 |
| `tests/CMakeLists.txt` | 注册 2 新测试 | 低 |
| `docs/module-reference.md` | +auth.h / policy.h / registry / streamable_http / client 段落 | 低 |

## 门控与零回归

- SEP-2243 全部行为**仅 2026-07-28 激活**（`sep2243_active` 门控，纯 `(ctx,session)` 签名不读 meta）；≤2025-06-18 零回归由现有 `test_http_serve` / `test_proto_2026_stateless` 守住。
- AS discovery 数据面**无版本门控**（纯工具函数）。
- `-32001`（`MCP_RPC_XMCP_PARAM_MISMATCH`）新增不动现有 `-32020/-32021/-32022`。
- S1 校验失败拒绝注册：host 未用 `x-mcp-header` 时行为完全不变（注解不在 schema 树里则递归遍历零命中，零开销）。

## 测试策略（TDD 先红后绿，每切片一个红灯门禁）

| 切片 | 新增测试 | 关键断言 |
|---|---|---|
| S1 (registry) | `test_sep2243_xmcp_header.c` §1 | `x-mcp-header` 在 number 型 → `add_tool` 返回 `INVALID_ARGUMENT`；非法 tchar/CR/LF 拒绝；嵌套深度命中；合法 string/integer/boolean 注册成功；重复注解（case-insensitive）拒绝 |
| S2 (http 校验) | `test_http_mcp_param.c` §1 | 2026 请求：`Mcp-Param-Region` 头值与 body 一致→200；不一致→400/-32001；body 有值头缺→400/-32001；base64 sentinel 解码后一致→200；≤2025-11-25 不校验（同请求 200）；`-32001` 与既有 `-32020` 标准头校验互不干扰 |
| S3 (客户端镜像) | `test_http_mcp_param.c` §2 | `mcp_client_set_tool_schema` 注入 schema 后 call_tool 带 `x-mcp-header` 参数→设 `Mcp-Param-*` 头；null 参数→不设头；首尾空白→base64 sentinel；未注入 schema（默认无 cache）→不发头 |
| S4 (AS discovery) | `test_sep2243_xmcp_header.c` §2 | `mcp_oauth_discover_as`：fetch 返回 AS JSON→`out_merged` 含 token_endpoint；fetch NULL→`MCP_ERR_PROTOCOL`；`existing_as` 非空→merge 优先级（AS 字段优先）；base64 encode/decode 往返 |
| 全量 | ASan + 零回归 | ≤2025-06-18 401/400 形态不变；`-Wall -Wextra` 零告警 |

## 验收口径

- P4-a 完成 → 库可声明 "2026 `x-mcp-header` 合规（server 校验 + 客户端镜像 + registry 注解校验）+ RFC 9728 AS 自动发现"。
- 构建要求沿用：C23、libc-only 核心（base64/discovery 数据面）、`MCPKIT_BUILD_HTTP` 门控的 S2/S3 集成面、`-Wall -Wextra` 零告警。
- 每切片独立可交付（S1/S4 纯数据面先行，S2/S3 依赖 HTTP 面 + S1 的 schema 读取约定），执行顺序 S1→S4→S2→S3（S4 数据面先行，S2 复用 S1 校验产物）。

## 与 P3 的衔接点

- 单一版本源：`mcp_session_negotiated`（P1+P2 交付）——P4-a 不新增 session 版本字段，新增 `mcp_proto_policy_sep2243_active` 沿用 `policy.c` 的 `mcp_proto_at_least` 判据。
- AS discovery 接 P3 T5 `mcp_oauth_discovery_merge`（`auth.c:432`）+ P3 T3 PRM 结构（`auth.h` 的 `mcp_protected_resource_metadata_t.authorization_server`）。
- `-32001` 与 P2 `-32020` 同区间但语义独立（避免 P2 行为回归）。

## 明确不做（留 P4-b / P5）

- 限速/锁out、mTLS——归 P4-b（独立 spec `2026-11-10-mcp-p4b-ratelimit-mtls-design.md`）。
- `x-mcp-header` 在 stdio 传输的语义（spec 澄清：stdio 客户端 MAY 忽略注解；P4-a 仅在 Streamable HTTP 路径校验/镜像，stdio 路径零行为变更）。
- AS discovery 的 issuer 自动探测**递归**（P4-a 只做单跳：PRM.authorization_server → AS discovery；AS discovery 文档内的其他 well-known 引用留 host 自行处理）。
