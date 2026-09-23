# Protocol Statelessness + Metadata 设计

日期：2026-09-23
状态：approved

## 背景

MCP 官方 spec 中 `_meta` 是协议层的扩展通道：客户端可在请求顶层携带
`_meta`（协议版本、客户端能力、身份信息），服务端可在成功响应的 `result` 内
注入 `_meta`。同时 Streamable HTTP 部署场景希望响应支持缓存语义
（`ttlMs`/`cacheScope`）与完成标志（`resultType: "complete"`），以及一个
`server/discover` RPC 让客户端发现服务端能力。

本特性包在 mcpkit-c 中一次实现全部内容。

---

## §1 message.h —— `_meta` 存取 API

在 `message.h` 新增两个 builder 辅助函数 + 一个访问器：

```c
// 访问器：返回 request 顶层 "_meta" 键（客户端携带的 _meta）
// 借指针，msg 存活期内有效；缺失或非 object 则返回 NULL
const mcp_json_value_t *mcp_message_meta(mcp_context_t *ctx, const mcp_message_t *msg);

// 在既有 result 对象上注入 resultType="complete"（固定字符串，无分配）
// 成功：result 已含 "resultType":"complete"，MCP_OK
// 失败（NOMEM 或 result 非 object）：caller 仍持有 result，返回 MCP_ERR_NOMEM/INVALID_ARGUMENT
mcp_status_t mcp_result_inject_result_type(mcp_context_t *ctx, mcp_json_value_t *result);

// 在既有 result 对象上注入 _meta
// meta 为 caller-owned；成功：所有权转给 result，caller 不得再碰 meta
// 失败：caller 仍持有 meta（与 object_set 失败保留所有权契约一致）
mcp_status_t mcp_result_inject_meta(mcp_context_t *ctx, mcp_json_value_t *result, mcp_json_value_t *meta);
```

`mcp_result_inject_meta` 与 `mcp_result_inject_result_type` 均走
`mcp_json_object_set_take` 路径，多步 attach 安全。

---

## §2 server/discover —— 能力发现 RPC

### 路由

`method_table.h` 的 `k_mcp_server_methods` 追加第 16 个条目 `"server/discover"`，
`MCP_SERVER_METHOD_COUNT` 同步 15→16，`_Static_assert` 更新。

`validate.c` 的 `k_l2_only_methods` 不含此方法（它已被路由，不需要 L2 表声明）。

### 处理

`dispatcher.c` 新增 `route_server_discover(ctx, srv, req)`：

返回 result object 含以下字段（全部为值，不依赖 session 状态）：

| 字段 | 类型 | 来源 |
|---|---|---|
| `serverName` | string | `srv->name`（strdup） |
| `serverVersion` | string | `srv->version`（strdup） |
| `protocolVersion` | string | `MCP_PROTOCOL_VERSION_LATEST`（静态字符串） |
| `toolsCount` | number | `srv->n_tools` |
| `resourcesCount` | number | `srv->n_resources` |
| `promptsCount` | number | `srv->n_prompts` |
| `listTtlMs` | number | `srv->list_ttl_ms`（0 = 省略，不写入） |
| `listCacheScope` | string | `srv->list_cache_scope`（NULL = 省略） |
| `supportsStateless` | bool | 固定 `true` |
| `supportsMeta` | bool | 固定 `true` |

`listTtlMs`/`listCacheScope` 仅当 host 调用过 `mcp_server_set_list_cache` 时
非零/非 NULL，此时写入 result；否则省略（与 §4 的「未配置则省略」语义一致）。

### 注入 resultType

`route_server_discover` 在 result 构建完成后、`mcp_response_ok_new` 之前调用
`mcp_result_inject_result_type(ctx, result)`，NOMEM 则返回 NULL（同其它路由）。

---

## §3 session —— 客户端 `_meta` 持久化

### 存储

`internals.h` 的 `struct mcp_session` 新增字段：

```c
// 来自 initialize 请求顶层 "_meta" 的 deep clone；NULL = 客户端未携带
// session 在 destroy 时销毁（同 client_name/client_version 生命周期）
mcp_json_value_t *client_meta;
```

`session.c` 的 `session_new`（或对应 init 路径）将 `client_meta` 初始化为 NULL。
`mcp_session_destroy` / `session_free` 在释放 `client_name`/`client_version` 的
同位置调用 `mcp_json_destroy(ctx, s->client_meta)`（NULL-safe）。

### 注入时机

`dispatcher.c` 的 `route_initialize` 在成功协商后，读
`mcp_message_meta(ctx, req)`；非 NULL 则
`client_meta = mcp_json_clone(ctx, req_meta)`；clone 成功存到 `s->client_meta`
（旧值先 destroy）；clone 失败（NOMEM）则 `s->client_meta` 保持 NULL，不中断
initialize 流程（降级：host 拿不到 _meta，但协议层不受影响）。

### 暴露 API

`session.h` 新增：

```c
// 返回 session 上存储的客户端 _meta（initialize 时 clone 的那份）
// 借指针；session 存活期内有效；从未携带则 NULL
const mcp_json_value_t *mcp_session_client_meta(mcp_context_t *ctx, const mcp_session_t *sess);
```

---

## §4 host 配置 API —— `mcp_server_set_list_cache` / `mcp_server_set_response_meta`

### struct mcp_server 新增字段

```c
// host 通过 mcp_server_set_list_cache 设置；0/NULL = list 响应省略这两个字段
uint64_t list_ttl_ms;
char *list_cache_scope;   // srv_strdup；NULL = 省略
// host 通过 mcp_server_set_response_meta 设置；caller-owned 成功则转存，失败 caller 保留
mcp_json_value_t *response_meta;
```

`server.c` 的 `mcp_server_destroy` 在释放 `list_cache_scope` 的同位置调用
`mcp_json_destroy(ctx, srv->response_meta)`。

### 新 API（`server.h`）

```c
// 配置 list 类响应（tools/list, resources/list, prompts/list, completion/list）
// 携带的 ttlMs 与 cacheScope。
// ttl_ms==0 或 cache_scope==NULL 时，对应字段从 list 响应中省略。
// 多次调用取最新值（覆盖式）。
// 返回 MCP_OK；INVALID_ARGUMENT 仅当 srv==NULL。
mcp_status_t mcp_server_set_list_cache(mcp_context_t *ctx, mcp_server_t *srv,
                                       uint64_t ttl_ms, const char *cache_scope_or_null);

// 配置服务端在每条成功响应的 result 内注入的 _meta。
// meta_json 为 caller-owned；成功：所有权转给 server，caller 不得再碰；
// 失败（NOMEM/INVALID_ARGUMENT）：caller 仍持有 meta_json。
// 传 NULL 清除已有配置（后续响应不再注入 _meta）。
// 返回 MCP_OK 或 MCP_ERR_NOMEM/INVALID_ARGUMENT。
mcp_status_t mcp_server_set_response_meta(mcp_context_t *ctx, mcp_server_t *srv,
                                          mcp_json_value_t *meta_json);
```

---

## §5 响应注入 wiring（dispatcher.c 内部 helper）

`dispatcher.c` 新增一个内部 helper（静态函数，不暴露）：

```c
// 在 result 上按当前 srv 配置注入 resultType + ttlMs + cacheScope + _meta
// 调用时机：route_tools_list / route_resources_list / route_prompts_list /
//           route_completion_list / route_server_discover 在 result 对象
//           构建完成、mcp_response_ok_new 之前调用
// 失败（NOMEM）：调用方负责 destroy result 并返回 NULL（与各路由现有 NOMEM 处理一致）
// 非 list 路由（tools/call、resources/read、prompts/get 等）只注入 resultType，
// 不注入 ttlMs/cacheScope/_meta（_meta 注入范围仅限 list 类 + discover）
static mcp_status_t decorate_result(mcp_context_t *ctx, mcp_server_t *srv,
                                    mcp_json_value_t *result, bool is_list);
```

`decorate_result` 内部逻辑（顺序）：
1. `mcp_result_inject_result_type(ctx, result)` — 所有成功响应均带，NOMEM → 返回 NOMEM
2. 若 `is_list`：
   - `srv->list_ttl_ms != 0` → `mcp_json_number_new(ctx, (double)srv->list_ttl_ms)` + `object_set_take(ctx, result, "ttlMs", v)`
   - `srv->list_cache_scope != NULL` → `srv_strdup(ctx, srv->list_cache_scope)` + `object_set_take(ctx, result, "cacheScope", v)`
   - `srv->response_meta != NULL` → `mcp_json_clone(ctx, srv->response_meta)` + `object_set_take(ctx, result, "_meta", v)`
3. 非 list 路由：只执行步骤 1

任何 `object_set_take` 失败 → 立即 destroy 尚未 attach 的值，返回 NOMEM；
已 attach 的由 result 容器持有，result 整体由调用方 destroy。

---

## §6 各路由注入点汇总

| 路由函数 | result 键 | 调用 decorate_result(ctx, srv, result, is_list) |
|---|---|---|
| `route_initialize` | （不注入，initialize 走独立 result 构建器） | 不注入 |
| `route_ping` | 空 object | `is_list=false` → 仅 resultType |
| `route_tools_list` | `tools` + `nextCursor` | `is_list=true` |
| `route_tools_call` | `content` | `is_list=false` → 仅 resultType |
| `route_resources_list` | `resources` + `nextCursor` | `is_list=true` |
| `route_resources_read` | `contents` | `is_list=false` → 仅 resultType |
| `route_prompts_list` | `prompts` + `nextCursor` | `is_list=true` |
| `route_prompts_get` | `messages` | `is_list=false` → 仅 resultType |
| `route_completion_list` | `completions` + `nextCursor` | `is_list=true` |
| `route_completion_complete` | `completions` | `is_list=false`（completion/complete 非 list，只带 resultType） |
| `route_server_discover` | 能力字段 | `is_list=true`（list 类缓存语义适用） |

---

## §7 L3 校验（validate.c）

`server/discover` 无参数，L3 规则：无（params 可为 NULL 或省略）。
`validate.c` 的 L3 表为 `server/discover` 添加「无必填参数」条目，
与 `resources/templates/list` 的无参规则相同。

---

## §8 测试（TDD，新增 tests/unit/test_protocol_statelessness.c）

以下用例必须全部通过后才提交：

1. `mcp_message_meta` 无 `_meta` → NULL；有 `_meta` 对象 → 借指针非 NULL
2. `mcp_result_inject_result_type` → result 含 `"resultType":"complete"`
3. `mcp_result_inject_meta` → result 含 `"_meta":{...}`；失败时 caller 仍持有 meta
4. `mcp_server_set_list_cache(ctx, srv, 5000, "server")` → tools/list 响应 result
   含 `"ttlMs":5000` + `"cacheScope":"server"` + `"resultType":"complete"`
5. `mcp_server_set_list_cache(ctx, srv, 0, NULL)` → tools/list 响应 result
   不含 `ttlMs`/`cacheScope`，仍含 `"resultType":"complete"`
6. `mcp_server_set_response_meta(ctx, srv, some_json)` → tools/list 响应 result
   含 `"_meta":<some_json 内容>`；传 NULL 清除 → 后续响应不含 `_meta`
7. `server/discover` 请求 → result 含 `serverName`/`serverVersion`/
   `toolsCount`/`supportsStateless:true`；带 list_cache 配置时含
   `listTtlMs`/`listCacheScope`
8. `mcp_session_client_meta` 在 initialize 带 `_meta` 后 → 非 NULL 且含正确字段；
   不带 `_meta` 的 initialize 后 → NULL
9. `route_ping` result 含 `"resultType":"complete"` 但无 `ttlMs`
10. 所有 list 路由响应（tools/list、resources/list、prompts/list、completion/list）
    带 `list_cache` 配置时均含 `ttlMs`/`cacheScope`

注册：`tests/CMakeLists.txt` 无条件添加（test_protocol_statelessness）。

---

## §9 文档

`docs/module-reference.md`：
- message.h 段加 `mcp_message_meta`、`mcp_result_inject_result_type`、`mcp_result_inject_meta` 三行
- server.h 段加 `mcp_server_set_list_cache`、`mcp_server_set_response_meta` 两行
- session.h 段加 `mcp_session_client_meta` 一行
- dispatcher.h 段加 `decorate_result` 说明（内部 helper，不暴露）

`CHANGELOG.md` [Unreleased] Added：
- 一条 bullet：`Protocol statelessness: _meta bidirectional support (mcp_message_meta, mcp_result_inject_meta), resultType="complete" on all success responses, host-configurable ttlMs/cacheScope on list responses, server/discover capability endpoint, mcp_session_client_meta accessor, mcp_server_set_list_cache / mcp_server_set_response_meta host APIs.`

Doxygen 重新生成，0 警告验证。

---

## 设计不变量（implementer 必知）

1. **`mcp_server_set_list_cache` 覆盖式**：每次调用替换前值，无累加语义
2. **`list_cache_scope` 字符串用 `srv_strdup`**（ctx allocator），destroy 时与
   `name`/`version` 同路径释放
3. **`response_meta` clone 时机**：`mcp_server_set_response_meta` 时 clone
   （非 set_take），后续 `decorate_result` 每次 list 响应前再 clone 一份
   挂到 result；这样 host 可以销毁原始 meta_json 而 server 不受影响
4. **`session->client_meta` 生命周期**：initialize 成功后 clone，session destroy
   时销毁；session 重建（新连接）时重新 initialize，覆盖旧 clone
5. **`mcp_session_client_meta` 借指针**：session 存活期内有效，与
   `mcp_session_grants` 等现有借指针访问器同契约
