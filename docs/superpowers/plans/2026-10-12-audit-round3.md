# 全量重审审计执行计划（第 3 轮）

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 对 HEAD=`330d03f` 做五层功能缺口走查，输出带证据与定级的缺口清单。

**Architecture:** 只读审计，不改任何源码。按 spec（`docs/superpowers/specs/2026-10-12-audit-round3-design.md`）§2 的五层顺序取证，每条发现按 缺/藏/偏 归类、P0/P1/P2 定级，最后串跨层链路。

**Tech Stack:** C23 代码库，`grep`/`ctest`/CLI 实测取证。

---

## Chunk 1: 五层走查 + 清单（单 chunk，自审）

### Task 1: 复验双配置基线

**Files:**（只读）`build/`, `build-http/`

- [ ] **Step 1: 默认配置构建 + ctest**

Run: `cmake --build build 2>&1 | grep -ciE "warning|error:"` → Expected: `0`
Run: `ctest --test-dir build 2>&1 | grep -E "tests passed|tests failed"` → Expected: `100% tests passed out of 60`

- [ ] **Step 2: HTTP 开启配置构建 + ctest**

Run: `cmake --build build-http 2>&1 | grep -ciE "warning|error:"` → Expected: `0`
Run: `ctest --test-dir build-http 2>&1 | grep -E "tests passed|tests failed"` → Expected: `100% tests passed out of 60`

### Task 2: ①公开头文件层（承诺了什么）

**Files:**（只读）`include/mcpkit/*/*.h`, `docs/module-reference.md`

- [ ] **Step 1: 列出所有公开 API**

Run: `grep -rh "^mcp_.*(.*);" include/mcpkit/ | wc -l` 记录总数；抽查 `tool.h` / `ui.h` / `server.h` 的 doxygen 与 module-reference 是否一一对应。

- [ ] **Step 2: 记缺/藏**

每条格式：`[缺|藏] include/…:行 — 一句话 — 证据(grep)`。

### Task 3: ②dispatcher 层（做到了什么）

**Files:**（只读）`src/server/dispatcher.c`, `src/server/registry.c`

- [ ] **Step 1: 核 tools/list + _meta 序列化**

确认 `t->meta` clone 路径；确认 `_meta` 非 object 存入后 `tools/list` 是否原样透出（读代码即可）。

- [ ] **Step 2: 记缺/藏/偏**

重点：`_meta` 非 object 的tools/list 表现 + `mcp_apps_tool_set_ui` 合并失败语义（已知预走查结论，复核一次）。

### Task 4: ③transport 层（四条路是否都通）

**Files:**（只读）`src/transport/stdio.c`, `socket.c`, `http.c`, `streamable_http.c`, `tls.c`

- [ ] **Step 1: 各传输 E2E 证据**

stdio：`apps_accept.sh` 已覆盖。socket/HTTP：跑对应 acceptance 或 CLI 实测一次，记录 PASS/FAIL。

- [ ] **Step 2: 记缺/藏/偏**

### Task 5: ④client 侧 + ⑤examples/tests（调得动吗 / 证据链完整吗）

**Files:**（只读）`src/client/`, `include/mcpkit/client/`, `examples/*/main.c`, `tests/`

- [ ] **Step 1: client 公开 API 清单 vs server 能力**

`_meta.ui` 有 server 打标 + `mcp_apps_result_ui_uri` reader；client 侧有无对应消费入口？无则记 `[藏]` 或 `[缺]`（按“client 应不应该有”判）。

- [ ] **Step 2: examples 覆盖矩阵**

12 个示例 × 能力（tool/resource/prompt/apps/transport），空格记 `[缺]`（示例层面的功能洞）。

### Task 6: 规范交叉 + 跨层串链 + 出清单

**Files:**（只读）`docs/superpowers/plans/2026-09-24-mcp-2026-07-28-full-conformance.md`

- [ ] **Step 1: 规范抽查**

对照 conformance 基线抽 5 条协议声明，`grep` 验证现状（全对则记“抽查 5/5 通过”）。

- [ ] **Step 2: 跨层串链**

opencode→file-server、host→apps-server、CLI inspect/call 三条链路结论（引用 acceptance 脚本结果，不重跑不清楚的部分补跑）。

- [ ] **Step 3: 输出清单**

向用户呈现：P0（必须零条）/ P1 / P2，每条含位置、证据、建议动作。不写报告文件（用户要落盘再说），不提交。
