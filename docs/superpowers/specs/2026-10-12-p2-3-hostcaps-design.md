# P2-3 反向调用 Spec：sampling/roots 示例 + wire 验收

**Status:** draft（未提交，待用户 review）
**Date:** 2026-10-12
**Background:** 第 3 轮审计 P2-3：sampling/roots 只有 in-process 单测，无示例、
无 wire 覆盖。已确认：完整 wire 验收、方案 A（新 example，不动 apps-host）。

## 现状证据

- client 侧 provider 机完整：`include/mcpkit/client/client.h` 有
  `mcp_client_roots_fn` + `mcp_client_set_roots_provider`（:397-425），
  sampling handler 注册（plan 阶段从该头文件摘确切名），
  `mcp_loop_run_with_client` 把 roots/list、sampling/createMessage、
  elicitation/create 路由给 provider；
- in-process 证据：`tests/unit/test_client_providers.c`；
- 空白：wire 上从未跑通过一次 host→client 反向调用；examples 下 11 个
  示例无一演示。

## 设计

1. **新 example** `examples/host-caps/main.c`（stdio，仿 apps-server 骨架）：
   - 工具 `ask_model`：handler 内向调用方 session 发 `sampling/createMessage`
    （取首个文本 content）+ `roots/list`（取首个 root uri），结果拼成
     `{"content":[{type,text: "model:…; root:…"}]}` 返回；
     对端无 provider 时透传上游错误（不伪造成功）。
   - 确切服务端发起 API 名 plan 阶段从 `test_client_providers.c` +
     server 头文件摘（spec 不编造签名）。
2. **C 对端探针**（随测编译，不装机）：stdio client，注册 canned sampling
   handler（固定回 `"canned"`）+ roots provider（固定回
   `file:///tmp/host-caps-root`）；跑完一次 tools/call 即退出。
3. **脚本** `tests/acceptance/hostcaps_accept.sh argv: <example> <peer>`：
   `mkfifo` 一对管道交叉连接两者 stdin/stdout；printf 握手 + tools/call
   喂 example 端；断言结果含 `canned` + `file:///tmp/host-caps-root`；
   用 `timeout 10` 防挂（无 timeout 命令时跳过并告警——plan 阶段确认 CI 有无）。
4. **CMake**：example target + peer target + `add_test(hostcaps_acceptance …)`，
   无条件（stdio，零 option 依赖）。

## 不做

- 不动 apps-host、不扩 CLI；
- 不演示 elicitation（client 侧只有 MRTR 版 handler，另立项）；
- peer 只回固定包，不做模型语义。

## 验收标准

- `ctest -R hostcaps_acceptance` 双配置通过；
- 全量 61→62（随 P2-2 落地顺序浮动，以实际为准）全绿、零警告；
- README examples 表 + CHANGELOG 各加一行。
