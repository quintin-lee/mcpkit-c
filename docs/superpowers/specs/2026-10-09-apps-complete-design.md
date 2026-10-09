# MCP Apps Complete Support Design (DRAFT — pending user review)

**Status:**草稿，未批准，未实现。批准前不动代码、不 commit。

**Goal:**补齐 MCP Apps 支持的三个缺口：tool 级 `_meta` 声明、client 端 UI 提取 helper、端到端测试。

**Background:** Phase 7 已有 server 端积木（`include/mcpkit/apps/ui.h`、`src/apps/ui.c`、
`tests/unit/test_apps.c` 174 行、`examples/apps-host`）：ui:// 资源 + CSP 自动补 meta、
`mcp_apps_result_with_ui` 结果打标、工具可见性 MODEL/APP/BOTH（dispatcher 双 enforcement）、
session 权限 grant/revoke、mount/unmount lifecycle。缺的是：tools/list 声明不了 tool 级 UI、
client 拿原始 JSON 得手挖 `_meta.ui`、apps 无 stdio/HTTP 端到端测试。

---

## 1. Tool 级 `_meta`（server）

**Files:**
- Modify: `src/server/internals.h`（`struct mcp_tool` 加 `mcp_json_value_t *meta;` 字段，
  与 `output_schema` 相邻）
- Modify: `src/server/registry.c`（`mcp_tool_new`/`mcp_tool_new_v2` 初始化为 NULL；
  `mcp_tool_destroy` 销毁；新增 setter）
- Modify: `include/mcpkit/server/tool.h`（setter 声明 + 文档）
- Modify: `src/server/dispatcher.c`（`route_tools_list` 序列化，仿 inputSchema/outputSchema
  的 clone + set_take pattern，约 431–440 行处）

**API:**

```c
/* 通用 setter，TAKE 语义（仿 mcp_tool_set_output_schema，registry.c:89-100）：
 * - tool 为 NULL 时销毁传入的 meta（防泄漏）并返回 MCP_ERR_INVALID_ARGUMENT
 * - 重复设置替换旧值；meta 为 NULL 表示清除 */
mcp_status_t mcp_tool_set_meta(mcp_context_t *ctx, mcp_tool_t *tool,
                               mcp_json_value_t *meta);
```

```c
/* apps 快捷函数（放 apps/ui.h，实现放 src/apps/ui.c）：
 * - 构造 {"ui": {"resourceUri": resource_uri}}，merge 进 tool 已有 _meta
 *   （保留其他键，只写 ui.resourceUri；tool 无 _meta 时新建）
 * - resource_uri 为 NULL → MCP_ERR_INVALID_ARGUMENT */
mcp_status_t mcp_apps_tool_set_ui(mcp_context_t *ctx, mcp_tool_t *tool,
                                  const char *resource_uri);
```

**语义约定：** tool `_meta` 是"广告"（tools/list 可见），结果 `_meta` 是"绑定"
（`mcp_apps_result_with_ui` 每次调用 overwrite，行为不变）。两者不一致时以结果为准。

## 2. Client 端 helper（apps）

**Files:**
- Modify: `include/mcpkit/apps/ui.h`（声明，与 `mcp_apps_result_with_ui` 对称）
- Modify: `src/apps/ui.c`（实现，纯 JSON reader，不碰 client）

**API:**

```c
/* 读 result → _meta → ui → resourceUri，返回 borrowed 指针
 * （零拷贝，生命周期跟 result 走，调用方不得 free/久存）。
 * 任一环节缺失或类型不对 → MCP_ERR_INVALID_ARGUMENT。 */
mcp_status_t mcp_apps_result_ui_uri(mcp_context_t *ctx,
                                    const mcp_json_value_t *result,
                                    const char **uri_out);
```

不做 client 结构体侵入：`mcp_client_call_tool` 已返回 caller-owned result JSON，
helper 只读 JSON，够用且零耦合。

## 3. 测试

**Files:**
- Modify: `tests/unit/test_apps.c`（纯 API 单测：set_meta 存取/替换/NULL 行为、
  tool_set_ui merge、result_ui_uri 正/负路径）
- Modify: `tests/unit/test_apps_e2e.c`（已有 in-process list→call→read 骨架，
  追加：tools/list 序列化出 `_meta`、tool 级 UI + 结果 UI 全链路断言）
- Create: `examples/file-server` 式最小服务 `examples/apps-server/main.c`
  （stdio：1 个 UI tool + 1 个 ui:// resource，抄 stdio-server 骨架）
- Modify: `examples/CMakeLists.txt`（注册 target）
- Create: `tests/acceptance/apps_accept.sh`（仿 `cli_accept.sh` 接线）
- Modify: `tests/CMakeLists.txt`（`add_test(NAME apps_acceptance ...)`）

**单测覆盖（test_apps.c 纯 API + test_apps_e2e.c in-process dispatch）：**
- set_meta 存取 / 替换旧值 / tool-NULL 销毁传入值 / meta-NULL 清除
- tools/list 序列化出 `_meta`（in-process dispatch 断言）
- tool_set_ui 的 merge 行为（已有 `_meta` 其他键保留）与 NULL 入参
- result_ui_uri 正常路径 + 缺 `_meta`/缺 `ui`/缺 `resourceUri`/类型错四条负路径

**E2E 覆盖（apps_accept.sh，传输层证明——in-process 部分已有 test_apps_e2e.c）：**
- CLI `inspect`：tools/list 含 `_meta.ui.resourceUri`；resources/list 含 ui:// 资源
- CLI `call`：结果含 `_meta.ui.resourceUri`（再用 result_ui_uri 语义断言）
- 管道直连 `resources/read`：返回 CSP `<meta>` + HTML 正文（ui.c 已有逻辑的传输层证明）

**成功标准：** 全量 ctest 通过（含新增项）、新代码 `-Wall -Wextra` 零警告、
acceptance 脚本在 Release 与 HTTP 开启两种配置下都过（file-server 那次两者都跑了，
这次沿用）。

## 非目标

- host 端 HTML 渲染（天然不在 SDK 范围）
- `mcpkit-cli` 新增 `resources/read` 子命令（E2E 用管道直连覆盖，不扩 CLI  scope）
- 改动 `apps-host`（它自有 session，动它有 regression 风险）
