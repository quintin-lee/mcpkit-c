# 全量重审：功能缺口审计（第 3 轮）设计

> 范围：HEAD=`330d03f` 全仓库。侧重：功能缺口（缺功能 / 不合规 / bug 风险），代码风格只记不展开。
> 方法：A 为主（分层走查）+ B 为辅（2026-07-28 conformance 基线交叉验证）。
> 批准记录：用户 2026-10-12 回复 "ok"，§1–§3 通过。

## 1. 输入与基线（已取证，只读）

- 默认配置构建零警告，`ctest` 60/60；HTTP 开启配置待审计执行时复验。
- 版图：`include/mcpkit/` 10 个模块头、`src/` 11 个目录（含 `adapters/`）、
  `examples/` 12 个示例、`tests/unit/` 55 个单测文件 + `cli_accept.sh` / `apps_accept.sh`。
- 对照基线：10-09 体检结论 + `docs/superpowers/plans/2026-09-24-mcp-2026-07-28-full-conformance.md`。
- 已知遗留项（预走查结论，供审计复核，不预判）：
  `_meta` 存非 object 时 `mcp_apps_tool_set_ui` 经 `object_set` 类型检查返回
  `INVALID_ARGUMENT`，`fail_obj` 路径销毁完整、无泄漏无崩溃（`src/apps/ui.c:225-264`）。

## 2. 方法：五层走查 + 三类记法

自顶向下：①公开头文件（承诺了什么）→ ②dispatcher（做到了什么）→
③transport（stdio/socket/HTTP/TLS 是否都通）→ ④client 侧（调得动吗）→
⑤examples+tests（证据链完整吗）。每层记三类：

- `缺`：头文件/文档承诺了，实现没有。
- `藏`：实现了，公开 API 或文档没暴露。
- `偏`：与 MCP 2026-07-28 规范行为不一致。

收尾串一次跨层链路：opencode→file-server、host→apps-server、CLI inspect/call，
引用 `cli_accept.sh` / `apps_accept.sh` 的真实链路证据，不另起炉灶。

## 3. 输出格式与验收

缺口清单每条含：位置（文件:行）、等级、证据（复现命令或 grep）、建议动作。
等级：P0（阻塞：崩溃/泄漏/错行为）/ P1（该补：功能洞/文档债）/ P2（可缓）。
验收：P0 必须为零才算“无阻塞缺口”；P1/P2 允许存在，但每条必须有去向
（修、单独立项、或明确不修的理由）。本轮不直接修代码，只出清单与定级。

## 4. 非目标

- 不重写代码、不做 refactor、不碰 `build-*/` 产物目录。
- 代码风格/命名/注释规范问题只在清单附录记一笔，不展开。
- spec 评审用自审（subagent 已禁用），最多 2 轮，超限直接交用户定。
