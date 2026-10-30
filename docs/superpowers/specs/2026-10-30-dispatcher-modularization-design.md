# 服务端分发引擎 `dispatcher.c` 模块化解耦重构设计

> 状态：提案 (Brainstorming 完成，待 Spec Reviewer 验证与用户批准)  
> 日期：2026-10-30  
> 责任模块：`src/server/dispatcher.c`, `src/server/routes.c`, `src/server/queue.c`, `src/server/internals.h`, `src/method_table.h`, `CMakeLists.txt`

---

## 1. 目标与背景 (Intent & Why)

### 1.1 背景
[`src/server/dispatcher.c`](file:///data/home/quintin/workspace/source/c/mcpkit-c/src/server/dispatcher.c) 是 `mcpkit-c` 服务端核心最庞大的源文件，目前达 1648 行（约 65KB）。随着 MRTR 多轮请求、SEP-2663 Tasks 扩展、SEP-2640 Skills 扩展、Content Annotations 以及流式订阅机制的持续演进，`dispatcher.c` 逐渐演变成一个包含了多种异构职责的 God File：
1. **消息队列数据结构与调度**：`struct mcp_queue`、容量上限控制、队列动态扩容以及单步消费派发（`mcp_server_process_one`）；
2. **顶层分发管线与横切关注点**：原子计数器更新（`c_requests_total`、`c_requests_error`）、L1/L2/L3 三级协议校验网关、Session 状态门禁（初始化/Stateless 判定）、W3C Trace 链路耗时测量；
3. **MCP 基础核心路由**：`initialize`、`ping`、`tools/list`、`tools/call`、`resources/list`、`resources/read`、`prompts/list`、`prompts/get`、`completion/list`、`completion/complete`；
4. **高级与订阅路由**：`logging/setLevel`、`resources/subscribe*`、`resources/templates/list`、`server/discover`、`subscriptions/listen`；
5. **协议扩展路由**：Tasks 任务系统（`tasks/get`、`tasks/update`、`tasks/cancel`）与 Skills 技能系统（`skills/list`、`skills/get`）。

多职责混合不仅增加了单个模块的代码圈复杂度，还导致后续功能扩展（如接入新 SEP 扩展或路由优化）容易引发隐蔽回归或合并冲突。

### 1.2 目标
采用**领域清晰三分法**重构解耦 `dispatcher.c`：
- 将会话消息队列下沉为独立的 `src/server/queue.c`，并将 `struct mcp_queue` 完全私有化封装在该文件内部；
- 将所有具体的业务方法路由逻辑与结果装潢抽离至独立的 `src/server/routes.c`；
- 精简 `src/server/dispatcher.c`，使其专注于纯粹的请求验证门禁、会话状态机、Tracing 观测以及顶层分发编排；
- 在 `src/method_table.h` 中为静态方法表声明添加 `__attribute__((unused))`，避免拆分后因单文件只引用部分表引发 `-Wunused-const-variable` 告警；
- **保持 100% 公共 API 稳定性与行为一致性**，全部 69+ 项已有测试全绿，且在 `-Wall -Wextra` 下保持零告警。

---

## 2. 目标、非目标与反目标 (Goals, Non-Goals, Anti-Goals)

### 2.1 目标 (Goals)
1. **职责单一化与代码瘦身**：
   - `src/server/queue.c`：约 120 行，纯粹的 FIFO 消息队列与单步处理；
   - `src/server/dispatcher.c`：瘦身至约 230 行，仅负责管线调度与通知分发；
   - `src/server/routes.c`：约 1250 行，集中收口全部方法路由实现。
2. **零外部破坏性改动 (Zero Public API Breakage)**：
   - 公开头文件 `include/mcpkit/server/dispatcher.h` 和 `include/mcpkit/server/server.h` 函数签名、错误码与所有权语义 100% 保持不变。
3. **高内聚的内部接口契约**：
   - 通过内部私有头文件 `src/server/internals.h` 声明跨文件内部接口，避免在头文件暴露非公共符号；`struct mcp_queue` 从 `internals.h` 移除并在 `queue.c` 内部私有化。
4. **无损性能与零回归**：
   - 内部拆分保持纯 C 静态链接调用，无虚函数或额外间接寻址开销，不引入任何堆内存重分配或性能损耗。

### 2.2 非目标 (Non-Goals)
1. **重构现有路由业务逻辑**：本期专注物理文件解耦与结构清晰化，不改变各路由内部既有的协议处理行为。
2. **拆分为过多碎片文件**：不采用细粒度多文件拆分（避免增加 7~8 个单路由的小文件），保持代码库紧凑易维护。
3. **修改公共数据类型**：不变更 `mcp_queue_t`、`mcp_server_t`、`mcp_session_t` 等对外暴露的不透明句柄设计。

### 2.3 反目标 (Anti-Goals)
1. **严禁引入全局可变状态**：解耦后各模块依然保持无全局变量、线程安全只依赖入参结构体。
2. **严禁破坏所有权语义**：消息的所有权（借用还是转移）在跨文件边界调用时严格维持现有既定规则。

---

## 3. 详细技术设计 (Detailed Design)

### 3.1 架构与模块拆分映射

```text
原架构：
  src/server/dispatcher.c (1648 行)
  └── [Queue + Pipeline + Routes + Extensions + Helpers]

重构后：
  src/server/
  ├── queue.c         (约 120 行) ── 实现 include/mcpkit/server/dispatcher.h 声明的 mcp_queue_* 及 mcp_server_process_one
  ├── dispatcher.c    (约 230 行) ── 实现 mcp_server_dispatch 与 mcp_server_notify 顶层管线
  ├── routes.c        (约 1250 行) ── 实现 mcp_server_route_request 及全部具体方法实现
  └── internals.h     (头文件)   ── 内部符号跨文件声明 seam
```

#### 3.1.1 `src/server/queue.c`（消息队列子模块）
- **职责**：管理 Session 级别的单步请求/通知消息队列，对应 `include/mcpkit/server/dispatcher.h` 中定义的全部队列操作及 `mcp_server_process_one`。
- **数据结构封装**：
  将目前位于 `src/server/internals.h` 中的 `struct mcp_queue` 定义移除并下沉至 `src/server/queue.c`，保留原本的字段排布：
  ```c
  struct mcp_queue {
      mcp_session_t **sessions;
      mcp_message_t **msgs;
      size_t head;
      size_t len;
      size_t cap;
  };
  ```
- **包含符号**：
  - `static mcp_status_t queue_grow(mcp_context_t *ctx, mcp_queue_t *q)`：动态扩容与数据平移；
  - `mcp_queue_t *mcp_queue_create(mcp_context_t *ctx)`；
  - `void mcp_queue_destroy(mcp_context_t *ctx, mcp_queue_t *q)`；
  - `mcp_status_t mcp_queue_push(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t *session, mcp_message_t *msg)`；
  - `mcp_status_t mcp_queue_pop(mcp_context_t *ctx, mcp_queue_t *q, mcp_session_t **session_out, mcp_message_t **msg_out)`；
  - `size_t mcp_queue_size(mcp_context_t *ctx, const mcp_queue_t *q)`；
  - `mcp_status_t mcp_server_process_one(mcp_context_t *ctx, mcp_server_t *srv, mcp_queue_t *q, mcp_message_t **resp_out)`。

#### 3.1.2 `src/server/routes.c`（方法路由子模块）
- **职责**：收敛全部协议业务逻辑处理，包括入参解构、数据校验、权限校验、业务回调执行及结果装潢。
- **包含辅助函数**：
  - `static mcp_message_t *err_resp(mcp_context_t *ctx, const mcp_message_t *req, int code, const char *text)`：构建 RPC 错误响应的私有静态辅助；
  - `page_offset`、`set_next_cursor`：分页 Cursor 转换；
  - `set_string`、`get_string`：JSON 属性存取快捷辅助；
  - `find_tool`、`find_resource`、`find_prompt`：Server 注册表项线性查找；
  - `decorate_result`：集中注入 `resultType`、`ttlMs`、`cacheScope` 与 `_meta`；
- **包含路由实现**：
  - 核心协议：`route_initialize`、`route_server_discover`；
  - 工具系统：`route_tools_list`、`route_tools_call`（含 MRTR 状态机流转与签名透传）；
  - 资源系统：`route_resources_list`、`route_resources_read`、`route_advanced`（subscribe/unsubscribe/templates）；
  - 提示词与补全：`route_prompts_list`、`route_prompts_get`、`route_completion_list`、`route_completion_complete`；
  - 扩展系统：`route_subscriptions_listen`、`route_tasks_get`、`route_tasks_update`、`route_tasks_cancel`、`route_skills_list`、`route_skills_get`；
- **方法表依赖**：
  包含 `#include "../method_table.h"`，用于通过 `k_mcp_server_methods` 执行方法匹配；
- **核心对外入口**：
  ```c
  mcp_message_t *mcp_server_route_request(mcp_context_t *ctx, mcp_server_t *srv,
                                          mcp_session_t *s, const mcp_message_t *req,
                                          const char *method);
  ```

#### 3.1.3 `src/server/dispatcher.c`（核心分发引擎）
- **职责**：顶层执行流水线，纯粹负责：
  1. 参数前置防空校验；
  2. 确保消息类型为 `MCP_MSG_REQUEST`；
  3. 原子累加请求总量 `atomic_fetch_add(&srv->c_requests_total, 1)`；
  4. 驱动三级校验管线 `mcp_message_validate(ctx, req, &code)`；
  5. 检查与同步 `_meta.io.modelcontextprotocol/logLevel`；
  6. 会话初始化状态与 Stateless 豁免判定；
  7. Tracing 监控（`trace_begin`、`trace_end`）；
  8. 调用 `mcp_server_route_request` 执行路由分发；
  9. 统计错误响应 `c_requests_error` 并输出；
  10. 实现 `mcp_server_notify` 入站通知消费（依赖 `k_mcp_server_notifications`）；
- **包含辅助函数**：
  - `static mcp_message_t *err_resp(mcp_context_t *ctx, const mcp_message_t *req, int code, const char *text)`：在本地保留私有静态辅助；
  - `void dlogf_srv(...)`：实现顶层日志函数，并在 `internals.h` 中导出供 `routes.c` 调用。

---

### 3.2 内部接口与头文件契约

#### 3.2.1 `src/server/internals.h` 更新
1. 移除 `struct mcp_queue` 结构体字段定义（完全下沉至 `queue.c` 私有）；
2. 增设跨模块调用接口声明：
```c
/* ------------------------------------------------------------------ */
/* Server Route Handlers Seam (routes.c <-> dispatcher.c)             */
/* ------------------------------------------------------------------ */

/**
 * @brief Logs formatted messages respecting server's dynamic log_floor level.
 * Defined in src/server/dispatcher.c and used across server routes.
 */
void dlogf_srv(mcp_context_t *ctx, mcp_server_t *srv, mcp_log_level_t level,
               const char *fmt, ...);

/**
 * @brief Routes a validated MCP request to its concrete method handler.
 * Defined in src/server/routes.c and called by mcp_server_dispatch.
 *
 * @param ctx Context; may be NULL.
 * @param srv Target server instance (must not be NULL).
 * @param s Active session (must not be NULL).
 * @param req Borrowed validated request message (must not be NULL).
 * @param method Borrowed method name (must not be NULL).
 * @return Owned response message on success/RPC error, or NULL on memory allocation failure.
 */
mcp_message_t *mcp_server_route_request(mcp_context_t *ctx, mcp_server_t *srv,
                                        mcp_session_t *s, const mcp_message_t *req,
                                        const char *method);
```

#### 3.2.2 `src/method_table.h` 编译器警告防护
为避免 `routes.c`（仅使用 methods 表）与 `dispatcher.c`（仅使用 notifications 表）分别包含头文件时触发 GCC/Clang 的 `-Wunused-const-variable` 告警，在 `src/method_table.h` 中为两个静态表添加 `__attribute__((unused))` 属性注解：
```c
static const char *const k_mcp_server_methods[] __attribute__((unused)) = { ... };
static const char *const k_mcp_server_notifications[] __attribute__((unused)) = { ... };
```

---

### 3.3 构建配置更新

在项目根目录 `CMakeLists.txt` 的 `MCPKIT_CORE_SOURCES` 列表中添加新增模块：
```cmake
set(MCPKIT_CORE_SOURCES
  ...
  src/server/server.c
  src/server/registry.c
  src/server/session.c
  src/server/dispatcher.c
  src/server/routes.c
  src/server/queue.c
  ...
)
```

---

## 4. 验证计划与质量门禁 (Testing & Quality Gates)

### 4.1 自动化测试覆盖
本次改动为纯粹的架构内部解耦，不涉及行为变更。现有自动化测试套件将全量用于回归校验：
1. **调度器专项单元测试**：
   - `ctest --test-dir build -R "test_dispatcher" --output-on-failure`
   - 验证所有 22 个已知方法名、未初始化拦截、Stateless 豁免、权限过滤及未知方法 `-32601` 表现保持一致。
2. **扩展与端到端测试**：
   - `test_tasks_extension`、`test_skills_extension`、`test_subscriptions_stream`、`test_mrtr`、`test_apps_e2e`。
3. **集成验收测试**：
   - `cli_acceptance`、`apps_acceptance`、`async_acceptance`。

### 4.2 质量门禁 (Quality Gates)
- 全量 69/69 个单元与验收测试 **100% 全绿**；
- 在 `-Wall -Wextra` 强制门禁下保持 **零警告、零错误**；
- 源码符合 Google C 风格规范，不引入内存泄漏或静态分析缺陷。
