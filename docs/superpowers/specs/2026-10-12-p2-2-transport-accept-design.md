# P2-2 传输验收 Spec：socket 真 TCP + http 文件背载 wire 验收

**Status:** draft（未提交，待用户 review）
**Date:** 2026-10-12
**Background:** 第 3 轮审计 P2-2：socket-server / http-server 示例无 wire 级 CI 驱动。
已确认：C 探针（不用 python3）、方案 A（独立交付，不碰现有 example/CLI）。

## 现状证据

- `examples/socket-server/main.c`（102 行）：`usage: <port>`，echo 工具，
  `mcp_socket_transport_create(ctx, NULL, port, true)` 绑定后 `mcp_stdio_serve`
  只服务一个连接，对端断开即退出。行定界 JSON-RPC（与 stdio 同帧格式）。
- `examples/http-server/main.c`（104 行）：文件背载 IO（stdin 读 / stdout 写），
  `mcp_http_serve`；请求字节格式见 `tests/unit/test_http_serve.c:140-160`
  （`POST /mcp HTTP/1.1\r\nContent-Length / Content-Type: application/json`，
  首包无 session，带 `Mcp-Session-Id` 回后续包——plan 阶段照抄该文件字节）。
- 既有脚本惯例（`tests/acceptance/apps_accept.sh` 28 行）：`#!/bin/sh` + `set -e`，
  argv 传二进制路径，`case … FAIL … exit 1`，末尾 echo OK。
- `examples/client` 只会 stdio（`mcp_stdio_transport_create`），不能直连 socket——
  这就是需要 C 探针的原因。

## 设计

1. **C 探针** `tests/acceptance/probe_sock.c`（随测编译，不装机，仅 socket 用，
   http 用 printf 管道零探针）：
   - argv：`<host> <port>`；阻塞 connect（带 5s 超时，`select` 判定）；
   - stdin 读行 → 发 TCP，读行 → 印 stdout；EOF/对端关闭即退出，exit 0；
   - 无 JSON 解析，只做字节管道（断言留给脚本）。
2. **脚本** `tests/acceptance/transport_accept.sh argv: <probe> <socket-srv> <http-srv>`：
   - socket：按序试端口 `18765/28765/38765`（被占则换）；后台起 server，
     轮询 connect 探活（≤5s）；printf 三行
     （initialize `2025-06-18` / initialized 通知 / tools/call echo）喂探针；
     断言 `echo` 回显 + `resultType`；`wait` 收 server 退出码 0。
   - http：printf 原始 POST 字节（照抄 test_http_serve 握手序列）喂 stdin；
     断言首包 `Mcp-Session-Id` + 后续包 `200` + echo 文本。
3. **CMake**：`tests/CMakeLists.txt` 加 `add_executable(sock-probe …)`（仅
   `MCPKIT_BUILD_SOCKET=ON` 时）+ `add_test(transport_acceptance …)`；
   http 部分只在 `MCPKIT_BUILD_HTTP=ON` 时传 http-srv 路径——脚本按 argc
   决定是否跑 http 段（默认配置无 http 时只验 socket；plan 阶段先确认两
   option 默认值）。

## 不做

- 不改两个 example、不扩 CLI、不碰 apps-host；
- 不做 TLS wire 验收（单测 `test_tls_transport` 已覆盖）；
- 端口占用导致全灭时脚本 FAIL（不静默跳过）。

## 验收标准

- `ctest -R transport_acceptance` 在默认 + HTTP 双配置通过；
- 全量保持 60→61（默认）/ 61（HTTP）全绿、零警告；
- README examples 表 + CHANGELOG 各加一行。
