# mcpkit-cli Inspector Subcommands (discover & listen) 设计

日期：2026-09-24
状态：approved

## 背景

在 MCP 2026-07-28 规范中，官方 Inspector CLI 提供了对 MCP 服务端能力的深度探索与事件流监听能力。当前 `mcpkit-cli` 提供了 `inspect`、`call`、`validate` 和 `test` 四个子命令，但缺乏：
1. **轻量无状态能力发现 (`discover`)**：无须进行 `initialize` 握手，直接通过 `server/discover` 探测服务端所支持的协议版本、能力集合、扩展能力（tasks/skills）及元数据缓存配置。
2. **实时通知流监听 (`listen`)**：对齐 Inspector 调试面板中的 Notification 实时流，支持通过 `subscriptions/listen` 建立长轮询/流式订阅，按通知类型（`toolsListChanged`、`promptsListChanged`、`resourcesListChanged`、资源 URI）过滤，实时输出服务端异步推送的事件，并在 Ctrl-C 或超时到达时优雅注销订阅 (`notifications/cancelled`)。

---

## §1 C SDK API 补充：`mcp_client_discover`

为了使 C 客户端 SDK 与 CLI 保持对称统一，在 `include/mcpkit/client/client.h` 与 `src/client/client.c` 增加 `mcp_client_discover` 公开 API：

```c
/**
 * @brief Sends server/discover request to inspect server capabilities statelessly.
 *
 * Can be sent before or after initialize. Result contains serverName,
 * serverVersion, protocolVersion, capabilities, etc.
 *
 * @param ctx        Context; may be NULL.
 * @param client     Target client.
 * @param result_out Receives the caller-owned cloned result JSON object on MCP_OK.
 * @return MCP_OK on success; MCP_ERR_* on transport or protocol error.
 */
mcp_status_t mcp_client_discover(mcp_context_t *ctx, mcp_client_t *client,
                                 mcp_json_value_t **result_out);
```

### 实现要点
直接委托给 `mcp_client_request(ctx, client, "server/discover", NULL, result_out)`。

---

## §2 `mcpkit-cli discover <server-bin>`

### 语法
```bash
mcpkit-cli discover <server-bin>
```

### 执行流
1. 通过 `spawn(server_bin, &cli)` 启动子进程服务。
2. 创建 `cli.ctx`、`cli.transport` (stdio) 与 `cli.client`，调用 `mcp_client_connect(cli.ctx, cli.client)`。
3. **注意**：不调用 `mcp_client_initialize(...)`。直接发送 `server/discover` 请求，验证服务端无状态握手支持。
4. 调用 `mcp_client_discover(cli.ctx, cli.client, &result)`。
5. 若成功，使用 `mcp_json_serialize` 将 JSON 打印至 `stdout`，释放内存。
6. 调用 `mcp_client_disconnect` 与 `cli_cleanup(&cli)`，正常退出返回 0；失败返回 1。

---

## §3 `mcpkit-cli listen <server-bin> [filter] [timeout_sec]`

### 语法
```bash
mcpkit-cli listen <server-bin> [filter] [timeout_sec]
```

### 参数解析策略
1. `server_bin`（必填）：目标服务可执行文件路径。
2. 可选参数分派：
   - 若参数 3 为纯十进制整数（>= 0），则解析为 `timeout_sec`，`filter` 采用默认值（全部监听）。
   - 若参数 3 为字符串，则解析为 `filter`；若存在参数 4，参数 4 解析为 `timeout_sec`。
3. 过滤器值转换：
   - `NULL`、`""`、`"all"` 或 `"*"`：
     `{"toolsListChanged": true, "promptsListChanged": true, "resourcesListChanged": true}`
   - 以 `{"` 开头：调用 `mcp_json_parse` 解析为 JSON 对象。
   - `"toolsListChanged"` 或 `"tools"`：`{"toolsListChanged": true}`
   - `"promptsListChanged"` 或 `"prompts"`：`{"promptsListChanged": true}`
   - `"resourcesListChanged"` 或 `"resources"`：`{"resourcesListChanged": true}`
   - 包含 `"://"`（如 `file:///tmp/demo.txt`）：`{"resourceSubscriptions": [filter]}`

### 执行流
1. `spawn(server_bin, &cli)` 启动子进程。
2. 调用 `cli_init(&cli)` 完成标准客户端连接与 `initialize` 握手。
3. 配置 transport 读取超时（200ms），使 `mcp_client_recv_message` 具备周期性唤醒检测信号与超时的能力：
   `mcp_transport_set_timeout(cli.ctx, cli.transport, 200, 0);`
4. 调用 `mcp_client_subscriptions_listen(cli.ctx, cli.client, filter_val, &ack)`。
5. 从 `ack` 消息的 `params._meta["io.modelcontextprotocol/subscriptionId"]` 提取并保存 `subscriptionId`。
6. 注册 `SIGINT` 和 `SIGTERM` 信号处理函数，触发 `mcp_request_shutdown()`。
7. 启动事件流接收循环：
   - 检查 `mcp_shutdown_requested()`：若置位则跳出循环。
   - 检查 `timeout_sec > 0` 且流转耗时已达超时：若超时则跳出循环。
   - 调用 `mcp_client_recv_message(cli.ctx, cli.client, &msg)`。
   - 若返回 `MCP_ERR_TIMEOUT`：继续下一轮循环检查。
   - 若返回 `MCP_OK`：序列化 `msg` 并打印至 `stdout`，调用 `fflush(stdout)`，销毁 `msg`。
   - 若返回 `MCP_ERR_IO` 或对端断开：退出循环。
8. 循环退出与注销：
   - 若持有 `subscriptionId`，调用 `mcp_client_cancel_subscription(cli.ctx, cli.client, subscription_id)`。
   - 恢复原信号处理句柄，调用 `mcp_shutdown_clear()`。
   - 释放 `ack`、断开 client、清理子进程并返回 0。

---

## §4 测试与验收规范

1. **单元测试 (`tests/unit/test_client.c`)**：
   - 验证 `mcp_client_discover(ctx, client, &res)` 在初始化前与初始化后均能正确获取 `serverName` / `capabilities`。
2. **CLI 验收测试 (`tests/acceptance/cli_accept.sh`)**：
   - 针对 `stdio-server` 运行 `$CLI discover "$STDIO_SRV"`，断言输出中包含 `"serverName":"stdio-server"`。
   - 针对 `stdio-server` 运行 `$CLI listen "$STDIO_SRV" all 1`，断言返回码为 0 且平稳退出（1秒超时退出，且正常发送/清理资源）。
3. **内存与并发验证**：
   - 全套 CTest（56+项）100% 通过。
   - ASan + UBSan 运行全套测试及 CLI 验收，0 leak、0 violation。
