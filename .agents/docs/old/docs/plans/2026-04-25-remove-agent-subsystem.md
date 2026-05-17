# 方案：移除 xlings 的 agent 子系统

**目标：** 把 xlings 退回到纯粹的"跨平台包管理器 + xvm 运行时"，砍掉交互式 AI agent（`xlings agent chat/resume/sessions/config`）及其所有耦合代码，令主 binary 不再依赖 `llmapi`、`mcpplibs-tinyhttps`，并清掉围绕 agent 建立起来的 libs、文档、测试和运行时数据。

## 当前 agent 子系统的边界

### 纯 agent 代码（整段删除，无分支保留）

| 位置 | 文件 | 约行数 | 说明 |
|---|---|---|---|
| `src/agent/` | 14 × `.cppm` | ~2979 | agent 核心：llm / runtime / behavior_tree / loop / context_manager / session / prompt / approval / tool_bridge / token_tracker / commands + `tui/` (state, screen) |
| `src/libs/agentfs.cppm` | | 128 | agent workspace 文件系统抽象 |
| `src/libs/agent_skill.cppm` | | 93 | agent skill 加载 |
| `src/libs/soul.cppm` | | – | agent 人格 / prompt 装配 |
| `src/libs/semantic_memory.cppm` | | – | 向量记忆（仅 agent 消费） |
| `src/libs/journal.cppm` | | – | agent 对话日志 |
| `src/libs/tinytui.cppm` | | – | agent TUI 辅助 |
| `src/libs/mcp_server.cppm` | | 153 | MCP over tinyhttps（目前无 import，死代码） |
| `src/libs/tinyhttps.cppm` | | – | 对 `mcpplibs-tinyhttps` 的薄封装，只给 agent 用 |
| `tests/e2e/agent_smoke_test.sh` | | – | agent 冒烟 |

### 含 agent 耦合、需**部分裁剪**的文件

| 文件 | 耦合点 | 动作 |
|---|---|---|
| `src/cli.cppm` | L27-35 的 agent/agentfs/soul/journal/tinytui import；L640-652 的 agent 子命令 help；L682-823 的 agent dispatch 分支；若干 `agent::` 调用 | 删 imports + 删 agent 子命令块 + 从 `top-level subcommands` 列表中拿掉 `"agent"` |
| `src/capabilities.cppm` | L14-15 imports `libs::semantic_memory` + `agent::context_manager`；L213-214 `shared_memory_store_` / `shared_ctx_mgr_` 全局指针；L227-305 `memory.remember/recall/forget/context.stats/context.search` 五个 capability；L318-319 `init(..., memory_store, ctx_mgr)` 参数 | 删两个 imports、两个全局指针、五个 capability handler、init 的两个参数 |
| `tests/unit/test_main.cpp` | 2719+ 行里 `TEST(Agent*, ...)` ≥19 个；`TEST(Approval,…)`/`TEST(Journal,…)`/`TEST(McpServer,…)`/`TEST(SemanticMemory,…)`/`TEST(Session,…)`/`TEST(SkillManager,…)`/`TEST(Soul,…)` 若干；L28/32/35 的 3 个 agent imports | 按 TEST group 整段删；保留：`ConfigTest / EventStream / Xim* / Xvm* / CmdlineTest / LogTest / UiTest / UtilsTest / Capability / TaskManager(评估)` |
| `xmake.lua` | L27-28 `add_requires("mcpplibs-tinyhttps ...")`、`add_requires("llmapi ...")`；L37、L57 对应的 `add_packages("mcpplibs-tinyhttps", "llmapi")` | 删这两条 requires + target 中的 add_packages 里移除这两个名字 |

### 非源码周边

| 位置 | 动作 |
|---|---|
| `.agents/` (1.4M) | **保留结构**但清空运行时数据 `docs/ plans/ tasks/` 内容；`skills/` 如果还有用于 Claude Code harness 的技能需要单独留（xlings-build、xlings-quickstart 是 xlings 仓库约定，可能不想删，本方案建议保留） |
| `docs/plans/` | 删除 16 个 agent 相关设计文档（2026-03-10 ~ 2026-03-15，共 ~400 KB） |
| `docs/specs/` | 删除 4 个 `*agent*.md` spec |
| `skills-lock.json` | 与 Claude Code 的 skill 绑定，不属 xlings agent；不删 |
| `.github/workflows/` | 无 agent-only step，无需改 |

## 执行阶段

分 4 个独立 commit，每个都能单独编译 + 测试通过，便于审查与回滚。

### Phase 1 — 砍 CLI 入口与 capability 耦合（最小断裂面）
> commit 1：`refactor: unwire agent subcommand + capability bindings`

1. `src/cli.cppm`：
   - 删 L27-35 所有 `import xlings.agent*` 和 `import xlings.libs.agentfs/soul/journal/tinytui`
   - 删 L640-652 agent 的 `SubHelp`
   - 删 L667 subcommands 列表中的 `"agent"`
   - 删 L682-823 整个 `if (cmd == "agent")` 分支
2. `src/capabilities.cppm`：
   - 删 L14-15 两个 imports
   - 删 L213-214 两个全局指针
   - 删 `memory.remember/recall/forget/context.stats/context.search` 五个 capability 方法
   - 改 `init(...)` 签名，去掉 `memory_store` 和 `ctx_mgr` 参数
3. `tests/unit/test_main.cpp`：
   - 删 L28/32/35 三个 agent imports
   - 删除 `TEST(Agent*,…)` / `TEST(Approval,…)` / `TEST(Journal,…)` / `TEST(McpServer,…)` / `TEST(SemanticMemory,…)` / `TEST(Session,…)` / `TEST(SkillManager,…)` / `TEST(Soul,…)` 全部 case
   - 评估 `TEST(Capabilities,…)` / `TEST(TaskManager,…)` —— 保留与 agent 无关的部分，删引用 memory/context 的断言

**验证：** `xmake build xlings`、`xmake build xlings_tests && xmake run xlings_tests`、`xlings agent --help` 应返回 "unknown command"、其他 subcommand (`install / remove / use / list / info / search / update / self / script / subos / config`) 全部正常。

### Phase 2 — 删源码目录
> commit 2：`chore: delete orphaned agent source tree`

删除（已在 Phase 1 变成 orphan，无 import 引用）：
- `src/agent/` 整个目录
- `src/libs/agentfs.cppm` / `agent_skill.cppm` / `soul.cppm` / `semantic_memory.cppm` / `journal.cppm` / `tinytui.cppm` / `tinyhttps.cppm` / `mcp_server.cppm`
- 若 `src/libs/resource_lock.cppm` 和 `flock.cppm` 在上述清理后再无导入者，一并删除（grep 确认）

**验证：** 同 Phase 1 的构建命令再跑一遍。

### Phase 3 — 砍外部依赖
> commit 3：`build: drop llmapi + tinyhttps from xmake deps`

- `xmake.lua`：
  - 删 `add_requires("mcpplibs-tinyhttps 0.2.0")`
  - 删 `add_requires("llmapi 0.2.3")`
  - 在 `target("xlings")` 和 `target("xlings_tests")` 里从 `add_packages(...)` 移除 `"mcpplibs-tinyhttps"` 和 `"llmapi"`

**验证：** `xmake f -c -y && xmake build` — 包解析环节应看到 llmapi / tinyhttps 不再出现；二进制体积应缩减（目测 ≥1 MB）。

### Phase 4 — 清文档与运行时数据
> commit 4：`docs: remove agent design docs + runtime data`

- 删 `docs/plans/2026-03-10-agent-*.md` 等 16 个 agent 相关 md
- 删 `docs/specs/*agent*.md`（4 个）
- 删 `tests/e2e/agent_smoke_test.sh`
- 清空 `.agents/docs/`、`.agents/plans/`、`.agents/tasks/`（保留 `.agents/skills/`，那是仓库约定的 Claude Code harness 技能）
- `README.md` / `README.zh.md` grep 确认无 agent 相关描述（Explore 扫描显示无）

**验证：** e2e 套件完整跑一遍（linux + macos），确保没人引用被删的 plan 文档。

## 不做的事

- **不改 `config/i18n/`** —— agent 字串都在 C++ 源码里，i18n 文件与 agent 解耦；不会 grep 到
- **不改 CI workflow yml** —— 无 agent-only step
- **不动 `skills-lock.json`** —— 属 Claude Code harness 管理
- **不动 `.agents/skills/`** —— 属仓库约定的 harness 技能

## 风险与注意

1. **`src/libs/semantic_memory.cppm` 的公共 API 被 capabilities 暴露过** —— 若有 external script / lua hook 依赖这几个 memory.* capability，砍掉后会缺失。需要先确认 xim-pkgindex 里没有包调用 `capabilities.memory.*` 或 `capabilities.context.*`（grep `xim-pkgindex` repo 即可）。
2. **`add_packages` 里 `"mcpplibs-xpkg"` 依赖 `llmapi` 吗？** —— 需在 Phase 3 编译前 grep mcpplibs-xpkg 源码确认，否则删 llmapi 会断这条链。
3. **runtime data `.agents/` 是否被 CI 或 e2e 间接读** —— agent_smoke_test.sh 删除后应无引用；但最后 `grep -R "\.agents" .github tests tools` 再扫一遍稳妥。

## 预计产出

- **删除：** ~3500 LOC C++ + ~900 行测试 + 20 份设计文档 + 1.4 MB 运行时数据
- **二进制：** xlings main binary 瘦身（估 llmapi + tinyhttps + agent 代码段 ≈ 2–3 MB）
- **依赖图：** 外部 xpkg 包从 7 个降到 5 个，构建速度减少一半（llmapi / tinyhttps 首次编译最慢）
- **回归范围：** 全部 `install / remove / use / list / subos / self / script` 路径已通过本分支 #219 的 E2E 覆盖，只要 Phase 1-2 构建过 + E2E 全绿就能信
