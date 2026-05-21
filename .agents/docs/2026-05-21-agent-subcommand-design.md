# `xlings agent` 子命令设计方案

**日期**: 2026-05-21
**状态**: Design draft (rev2), 待 review
**关联代码**: `src/cli.cppm`、`src/capabilities.cppm`、`src/interface.cppm`、`.agents/skills/xlings-usage/SKILL.md`

---

## 1. 背景与动机

### 1.1 问题

大模型（Claude Code、Cursor、Copilot、Codex 等）在使用 xlings 时面临两个核心问题：

1. **不知道怎么用** — Agent 第一次遇到 xlings，没有内置知识，不知道命令格式、参数、工作流
2. **输出难解析** — 当前 CLI 输出是 FTXUI 渲染的彩色 TUI，Agent 需要从 ANSI 转义码中"猜"结果

现有的 `.agents/skills/` 目录里已有完整的 skill 内容（xlings-usage、xlings-quickstart），但这些是**文件系统中的 markdown**——只有在 xlings repo 内工作的 Agent 才能读到。用户在任意目录使用 xlings 时，Agent 看不到这些。

### 1.2 核心洞察

> **Agent 需要的不是新协议，而是"自学能力"。**

把使用指南（skill/prompt）**编译进二进制**，Agent 通过一条命令就能获取。这比 MCP Server 更通用——任何能跑 shell 的 Agent 都能用，零配置。

### 1.3 设计原则

- **渐进式披露** — 从概览到详情，Agent 按需深入，不一次性倾倒所有信息
- **提示词即输出** — 子命令输出的内容本身就是对 LLM 有引导性的 prompt，不只是文档
- **编译时嵌入** — skill 内容随 binary 分发，不依赖外部文件
- **复用现有架构** — EventStream + Capability Registry 不变，只加输出层

---

## 2. 命令设计

### 2.1 命令树

```
xlings agent                     # 概览 + 内置 skill 列表
xlings agent skills              # 同上（列出所有内置 skill）
xlings agent skills <name>       # 输出指定 skill 的完整内容
```

### 2.2 渐进式披露

```
第一层: xlings agent
  ↓ "我是什么工具？有哪些 skill 可以学？"
  ↓ 输出概览 + skill 列表（名称 + 一句话描述）
  ↓ 引导: "运行 xlings agent skills <name> 查看详情"

第二层: xlings agent skills <name>
  ↓ 输出完整的 skill prompt 内容
  ↓ Agent 读完即学会该场景的完整用法
```

两层就够。不需要更多层级——Agent 不需要"菜单式"导航，它需要的是**一次获取足够的上下文**。

### 2.3 输出详设

#### `xlings agent` / `xlings agent skills`（概览 + skill 列表）

```
xlings — Developer tool version manager

A CLI tool for installing, managing, and switching between multiple versions
of development tools (compilers, runtimes, build systems) with isolated
environments (SubOS).

Available skills (run `xlings agent skills <name>` for full content):

  usage         Complete usage guide: install, search, remove, version
                switching, SubOS, project mode, and all flags
  setup         Set up a development environment from scratch
  debug-build   Diagnose build failures caused by missing or wrong tool versions
  subos         Create and manage isolated environments (SubOS)

Quick start (if you just need to run a command now):
  xlings search <keyword>              Search packages
  xlings install <pkg>[@<ver>] --yes   Install (--yes skips confirmation)
  xlings list                          List installed
  xlings use <pkg> <ver>               Switch version

Tip: Add --agent flag to any command for clean plain-text output
     without TUI formatting (no ANSI codes, no progress bars).
```

**设计意图**:
- 概览部分让 Agent 立刻能做简单操作（search/install）
- skill 列表引导 Agent 按需深入
- 不强制 Agent 先读完 skill 才能开始——Quick start 直接可用

#### `xlings agent skills usage`（完整 skill）

输出编译时嵌入的完整使用 skill。这是**面向 LLM 优化的 prompt**，不是普通文档：

```markdown
# xlings Usage Skill

You are using xlings, a developer tool version manager. This skill teaches
you how to use it correctly and efficiently.

## Rules

1. Add --yes to all install/remove commands to skip interactive prompts
2. Add --agent for clean output without TUI formatting
3. Use `xlings search` first if you are unsure about the exact package name
4. Check for .xlings.json in the project root before installing tools manually

## DO NOT

- Run xlings without --yes in non-interactive contexts (it will hang)
- Guess package names — always search first if unsure
- Install globally when a project .xlings.json exists (use project mode)

## Package Management

### Search
  xlings search <keyword> [--agent]
  Example: xlings search gcc

### Install
  xlings install <pkg>@<ver> --yes [--agent]
  xlings install <pkg1> <pkg2> --yes [--agent]    # multiple
  Example: xlings install gcc@15 --yes

### Remove
  xlings remove <pkg>[@<ver>] [--agent]

### List installed
  xlings list [--agent]

### Package info
  xlings info <pkg> [--agent]

### Update index
  xlings update [--agent]

### Switch version
  xlings use <pkg> <ver> [--agent]
  xlings use <pkg>                     # list available versions
  Example: xlings use gcc 15

## SubOS (Isolated Environments)

Use SubOS when:
- Project needs tool versions that conflict with global versions
- Agent needs a sandboxed execution environment
- Multiple projects need different toolchains on the same machine

Commands:
  xlings subos new <name>                    # create
  xlings subos use <name>                    # enter (interactive shell)
  xlings subos use <name> --cmd "<command>"  # run single command inside
  xlings subos use <name> --sandbox          # filesystem-level isolation
  xlings subos list [--agent]               # list all
  xlings subos remove <name>                 # delete

## Project Mode

If the current directory (or a parent) has .xlings.json:
  {"workspace": {"gcc": "15.1.0", "node": "22.17.1"}}

Run `xlings install` with no arguments to install all declared tools.

## Decision Trees

### "I need to install a tool"
  1. Know the exact name? → xlings install <name>@<ver> --yes
  2. Unsure? → xlings search <keyword> → pick from results → install
  3. Need to see available versions? → xlings info <name> → pick version

### "Build fails with 'command not found'"
  1. xlings list → is the tool installed?
  2. Not installed → xlings search + install
  3. Installed, wrong version → xlings use <tool> <version>
  4. Installed, still not found → restart shell (source profile)

### "Project has .xlings.json"
  1. Run xlings install (no args) → installs everything declared
  2. Need to add a new tool → xlings install <pkg>@<ver> --yes
     then add to .xlings.json workspace

## Key Flags

  --yes       Skip confirmation prompts (required for non-interactive use)
  --agent     Clean plain-text output, no TUI/ANSI formatting
  -g          Install to global scope (not project-local subos)
  -u          Activate installed version even if another is active
```

#### `xlings agent skills setup`（场景 skill）

```markdown
# Setup Skill — Set up a development environment

Follow this workflow to set up a development environment from scratch.

## Steps

1. Check what tools are already installed:
     xlings list

2. Search for needed tools:
     xlings search <keyword>

3. Install tools with specific versions:
     xlings install <pkg>@<ver> --yes
   Install multiple at once:
     xlings install gcc@15 cmake node --yes

4. Verify installation:
     <tool> --version
   If "command not found", restart shell or run: source ~/.bashrc

5. (Optional) Create project config for reproducibility:
   Create .xlings.json in project root:
     {"workspace": {"gcc": "15.1.0", "cmake": "3.31.6"}}
   Others can then run `xlings install` to get the same environment.

## Common tool names
  gcc, clang, node, python, cmake, rust, go, java, zig, deno, bun
  Use `xlings search <keyword>` to find more.
```

#### `xlings agent skills debug-build`（场景 skill）

```markdown
# Debug Build Skill — Fix build failures from missing or wrong tools

## Diagnosis

1. Read the error message. Common patterns:
   - "command not found: <tool>"  →  tool not installed
   - "version X required"        →  wrong version active
   - "unsupported option"        →  tool too old

2. Check current state:
     xlings list
     <tool> --version

3. Identify the fix:
   - Not installed → xlings install <tool> --yes
   - Wrong version → xlings info <tool>   (see available versions)
                    → xlings use <tool> <correct-version>
   - Too old, need upgrade → xlings install <tool>@<newer> --yes -u

4. Retry the build.

## Example: gcc too old for C++23

  Error: '__cpp_lib_format' was not declared
  Diagnosis: gcc --version → 13.3.0 (C++23 incomplete)
  Fix: xlings install gcc@15 --yes -u
  Verify: gcc --version → 15.1.0
  Retry: make
```

---

## 3. `--agent` 全局 Flag

### 3.1 定位

`--agent` 不是"输出 JSON"，而是**输出干净的纯文本**——去掉 TUI 渲染（ANSI 颜色、进度条、表格边框），让 LLM 直接读懂。

### 3.2 三种输出模式对比

```
┌─────────────────────────────────────────────────────────────┐
│                   xlings 输出模式                            │
├─────────────┬──────────────────┬────────────────────────────┤
│  默认 (TUI) │  --agent         │  interface                 │
│  给人看     │  给 LLM 看       │  给程序看                   │
├─────────────┼──────────────────┼────────────────────────────┤
│  FTXUI 渲染 │  纯文本，无 ANSI │  NDJSON 结构化事件流        │
│  彩色表格   │  简洁的文本表格  │  JSON 逐行                  │
│  进度条动画 │  静态进度行      │  ProgressEvent              │
│  交互提示   │  配合 --yes 静默 │  PromptEvent + reply        │
├─────────────┴──────────────────┴────────────────────────────┤
│            共享: EventStream + Capability Registry           │
└─────────────────────────────────────────────────────────────┘
```

### 3.3 为什么纯文本而不是 JSON

| 维度 | JSON | 纯文本 |
|------|------|--------|
| LLM 理解成本 | 需要知道每个字段的含义 | 直接读懂，就像读终端 |
| 实现成本 | 每个命令定义输出 schema | 去掉 ANSI 渲染即可 |
| 已有 JSON 方案 | `xlings interface` 已经覆盖 | — |
| Agent 实际体验 | 多一层解析 | 直接在 stdout 中读 |

**JSON 的场景已经被 `interface` 模式覆盖。** `--agent` 定位于更轻量的"LLM 友好文本"。

### 3.4 输出示例

```bash
# 默认 TUI（人看）
$ xlings search gcc
╭──────────────────────────────╮
│  🔍 Search: gcc              │
├──────────────────────────────┤
│  ● gcc        15.1.0         │
│  ● musl-gcc   15.1.0         │
╰──────────────────────────────╯

# --agent（LLM 看）
$ xlings search gcc --agent
Search results for "gcc":
  gcc       — versions: 15.1.0, 14.2.0, 13.3.0
  musl-gcc  — versions: 15.1.0

# interface（程序看）— 已有
$ xlings interface search_packages --args '{"keyword":"gcc"}'
{"kind":"data","dataKind":"search_results","payload":{"results":[...]}}
```

```bash
# 默认 TUI
$ xlings list
╭─ Installed packages ────────────╮
│  gcc     15.1.0  ✔ active       │
│  cmake   3.31.6  ✔ active       │
╰─────────────────────────────────╯

# --agent
$ xlings list --agent
Installed packages:
  gcc     15.1.0  (active)
  cmake   3.31.6  (active)
```

```bash
# --agent install
$ xlings install gcc@15 --yes --agent
Installing gcc@15...
Resolved: gcc 15.1.0
Downloading: gcc-15.1.0-linux-x86_64.tar.xz (85 MB)
Download complete.
Extracting...
Installed gcc 15.1.0 (activated)
```

### 3.5 实现

在 `cli::run()` 中：

```cpp
// 检测 --agent flag（与 --verbose/--quiet 同级处理）
bool agent_mode = false;
for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--agent") {
        agent_mode = true;
        break;
    }
}

if (agent_mode) {
    // 关闭 TUI listener，注册 agent listener
    stream.set_enabled(tui_listener, false);
    platform::set_tui_mode(true);  // 已有机制：跳过 FTXUI 渲染

    // agent listener：将 DataEvent 转为纯文本输出
    stream.on_event([](const Event& e) {
        if (auto* d = std::get_if<DataEvent>(&e)) {
            // 将结构化 DataEvent 转为人类可读文本
            agent::print_data_event(*d);
        }
        else if (auto* l = std::get_if<LogEvent>(&e)) {
            // 日志直接输出（无颜色）
            std::cout << l->message << "\n";
        }
        else if (auto* er = std::get_if<ErrorEvent>(&e)) {
            std::cout << "Error: " << er->message << "\n";
            if (!er->hint.empty()) std::cout << "Hint: " << er->hint << "\n";
        }
        else if (auto* p = std::get_if<ProgressEvent>(&e)) {
            // 静态单行进度（不用动画）
            std::cout << p->phase << ": " << p->message << "\n";
        }
    });
}
```

`agent::print_data_event()` 将每种 `DataEvent.kind` 转为纯文本：

```cpp
void print_data_event(const DataEvent& e) {
    auto json = nlohmann::json::parse(e.json, nullptr, false);

    if (e.kind == "search_results") {
        std::cout << "Search results:\n";
        for (auto& r : json["results"]) {
            std::cout << "  " << r[0] << "  — " << r[1] << "\n";
        }
    }
    else if (e.kind == "info_panel") {
        std::cout << json.value("title", "") << "\n";
        for (auto& f : json["fields"]) {
            std::cout << "  " << f["label"] << ": " << f["value"] << "\n";
        }
    }
    else if (e.kind == "table") {
        // 简单文本表格，无边框
        for (auto& h : json["headers"]) std::cout << h << "\t";
        std::cout << "\n";
        for (auto& row : json["rows"]) {
            for (auto& cell : row) std::cout << cell << "\t";
            std::cout << "\n";
        }
    }
    else if (e.kind == "install_plan") {
        std::cout << "Install plan:\n";
        for (auto& p : json["packages"]) {
            std::cout << "  " << p[0] << " " << p[1] << "\n";
        }
    }
    // ... 其他 kind 类推
}
```

---

## 4. 编译时资源嵌入

### 4.1 资源来源与 skill 注册表

所有内置 skill 在编译时嵌入 binary。每个 skill 有：名称、一句话描述、完整内容。

```
.agents/skills/xlings-usage/SKILL.md    → skill "usage"
新增 src/agent_skills/setup.md          → skill "setup"
新增 src/agent_skills/debug-build.md    → skill "debug-build"
新增 src/agent_skills/subos.md          → skill "subos"
```

### 4.2 嵌入方式（C++ raw string literal）

```cpp
export module xlings.agent.resources;

namespace xlings::agent::resources {

struct SkillEntry {
    const char* name;
    const char* description;
    const char* content;
};

constexpr SkillEntry kSkills[] = {
    {
        "usage",
        "Complete usage guide: install, search, remove, version switching, SubOS, project mode",
        R"SKILL(
# xlings Usage Skill
...完整 skill 内容...
)SKILL"
    },
    {
        "setup",
        "Set up a development environment from scratch",
        R"SKILL(
# Setup Skill
...
)SKILL"
    },
    {
        "debug-build",
        "Diagnose build failures caused by missing or wrong tool versions",
        R"SKILL(
# Debug Build Skill
...
)SKILL"
    },
    {
        "subos",
        "Create and manage isolated SubOS environments",
        R"SKILL(
# SubOS Skill
...
)SKILL"
    },
};

constexpr const char* kOverview = R"PROMPT(
xlings — Developer tool version manager
...
)PROMPT";

}
```

### 4.3 同步脚本

```bash
#!/bin/bash
# tools/sync-agent-resources.sh
# 从 .agents/skills/ 和 src/agent_skills/ 生成 src/agent_resources.cppm
```

Phase 1 手动维护，后续视更新频率决定是否自动化。

---

## 5. 模块设计

### 5.1 新增文件

```
src/
├── agent.cppm              # xlings agent 子命令逻辑
├── agent_resources.cppm    # 编译时嵌入的 skill 内容
```

### 5.2 `src/agent.cppm`

```cpp
export module xlings.agent;

import std;
import xlings.agent.resources;

namespace xlings::agent {

// xlings agent / xlings agent skills — 列出所有 skill
export void print_overview() {
    std::cout << resources::kOverview << "\n\n";
    std::cout << "Available skills (run `xlings agent skills <name>`):\n\n";
    for (auto& s : resources::kSkills) {
        // 左对齐 name，右接 description
        std::cout << "  " << std::left << std::setw(14) << s.name
                  << s.description << "\n";
    }
    std::cout << "\n";
    // Quick start 部分
    std::cout << "Quick start:\n";
    std::cout << "  xlings search <keyword>              Search packages\n";
    std::cout << "  xlings install <pkg>[@<ver>] --yes   Install\n";
    std::cout << "  xlings list                          List installed\n";
    std::cout << "  xlings use <pkg> <ver>               Switch version\n";
    std::cout << "\n";
    std::cout << "Tip: Add --agent for clean output without TUI formatting.\n";
}

// xlings agent skills <name> — 输出指定 skill
export bool print_skill(std::string_view name) {
    for (auto& s : resources::kSkills) {
        if (name == s.name) {
            std::cout << s.content << "\n";
            return true;
        }
    }
    // 未找到：提示可用 skill
    std::cerr << "Unknown skill: " << name << "\n";
    std::cerr << "Available: ";
    for (auto& s : resources::kSkills) {
        std::cerr << s.name << " ";
    }
    std::cerr << "\n";
    return false;
}

// --agent 模式下的 DataEvent 纯文本渲染
export void print_data_event(const DataEvent& e);

}
```

### 5.3 CLI 注册

```cpp
.subcommand("agent")
    .description("Agent integration: built-in skills and machine-friendly output")
    .action(wrap_rc([](const cmdline::ParsedArgs& args) -> int {
        if (args.positional_count() == 0) {
            // xlings agent → 概览 + skill 列表
            agent::print_overview();
            return 0;
        }
        auto sub = std::string(args.positional(0));
        if (sub == "skills") {
            if (args.positional_count() < 2) {
                // xlings agent skills → 同概览
                agent::print_overview();
                return 0;
            }
            // xlings agent skills <name> → 输出 skill 内容
            return agent::print_skill(args.positional(1)) ? 0 : 1;
        }
        log::error("unknown: xlings agent {}", sub);
        log::error("  try: xlings agent skills");
        return 1;
    }));
```

---

## 6. Agent 使用流程

### 6.1 首次接触

用户的 CLAUDE.md / AGENTS.md 只需写一行：

```markdown
使用 xlings 管理开发工具版本。运行 `xlings agent` 了解用法。
```

Agent 的行为：

```
Agent (看到 CLAUDE.md 提到 xlings):
  → Bash: xlings agent
  → 读到概览 + skill 列表
  → 发现有 "usage" skill
  → Bash: xlings agent skills usage
  → 读到完整使用 prompt，学会所有命令
  → 开始使用: xlings search gcc --agent
```

### 6.2 按需学习

```
Agent 遇到构建失败:
  → 想起 xlings agent 有 skill 列表
  → Bash: xlings agent skills debug-build
  → 读到诊断流程
  → 按流程: xlings list → xlings info gcc → xlings use gcc 15
  → 问题解决
```

### 6.3 日常使用

学过一次 skill 后，Agent 直接使用命令：

```
用户: 帮我装 Node.js

Agent:
  → Bash: xlings search node --agent
  → 读到: node — versions: 22.17.1, 20.18.3, ...
  → Bash: xlings install node@22 --yes --agent
  → 读到: Installed node 22.17.1 (activated)
  → 告诉用户: Node.js 22.17.1 已安装
```

---

## 7. 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                     xlings 三模式架构                        │
├─────────────┬──────────────────┬────────────────────────────┤
│  默认 (TUI) │  --agent         │  interface                 │
│  给人看     │  给 LLM 看       │  给程序看                   │
├─────────────┼──────────────────┼────────────────────────────┤
│  FTXUI 渲染 │  纯文本，无 ANSI │  NDJSON 结构化事件流        │
│  彩色表格   │  简洁文本        │  JSON 逐行                  │
│  进度条动画 │  静态进度行      │  ProgressEvent              │
│  交互提示   │  配合 --yes 静默 │  PromptEvent + reply        │
├─────────────┴──────────────────┴────────────────────────────┤
│             共享: EventStream + Capability Registry          │
├─────────────────────────────────────────────────────────────┤
│  xlings agent — 内置 skill 系统（编译时嵌入的 prompt）       │
│  Agent 自学入口，不依赖外部文件                               │
└─────────────────────────────────────────────────────────────┘
```

### 关键区分

| | `xlings agent` 子命令 | `--agent` flag |
|---|---|---|
| 作用 | **教 Agent 怎么用 xlings** | **让命令输出对 Agent 友好** |
| 使用频率 | 一次性学习 | 每次命令都加 |
| 输出内容 | Skill prompt（教程） | 命令执行结果（纯文本） |
| 实现依赖 | agent_resources.cppm（嵌入内容） | EventStream listener 替换 |

两者配合：先用 `xlings agent skills usage` 学会怎么用，再用 `xlings xxx --agent` 实际操作。

---

## 8. Prompt 内容的引导性设计原则

`xlings agent` 输出的内容不是普通文档，是**面向 LLM 优化的 prompt**。

### 8.1 结构化指令

每个 skill 遵循固定结构：

```
# Skill 名称
[一段话说清这个 skill 是做什么的]

## Rules          ← 必须遵守的规则
## DO NOT         ← 明确的负面约束
## Commands       ← 精确的命令格式 + 示例
## Decision Trees ← 条件判断引导
## Key Flags      ← 参数速查
```

### 8.2 负面约束（DO NOT）

告诉 Agent **不要做什么**，这比正面指令更有效：

```
DO NOT:
  - Run xlings without --yes in non-interactive contexts (will hang)
  - Guess package names (use search first)
  - Install globally when project .xlings.json exists
```

### 8.3 决策树

帮 Agent 做条件判断，而不是列一堆命令让它自己选：

```
If you need to install a tool:
  1. Know exact name? → xlings install <name>@<ver> --yes
  2. Unsure?          → xlings search <keyword> → then install
  3. Need version?    → xlings info <name> → pick → install
```

### 8.4 每个 skill 自包含

Agent 只读一个 skill 就能工作，不需要交叉引用多个 skill。每个 skill 包含自己场景所需的全部命令和参数。

---

## 9. 实现计划

### Phase 1: `xlings agent` 子命令

- [ ] 创建 `src/agent_resources.cppm` — skill 注册表 + 嵌入内容
- [ ] 创建 `src/agent.cppm` — 子命令逻辑（overview + skills 列表/查询）
- [ ] `cli.cppm` 注册 `agent` 子命令
- [ ] 编写内置 skill 内容：`usage`（从现有 .agents/skills 改编优化）
- [ ] 编写内置 skill 内容：`setup`、`debug-build`、`subos`

### Phase 2: `--agent` 输出模式

- [ ] `cli.cppm` 全局 `--agent` flag 检测
- [ ] 实现 agent EventStream listener（DataEvent → 纯文本渲染）
- [ ] PromptEvent 处理（配合 `--yes` 自动应答，无 `--yes` 时文本提示）
- [ ] 验证所有命令的 `--agent` 纯文本输出

### Phase 3: 迭代

- [ ] 根据实际 Agent 使用反馈，优化 skill 内容
- [ ] 增加新 skill（ci、migrate 等）
- [ ] `tools/sync-agent-resources.sh` — .agents/skills/ 与 .cppm 同步
- [ ] 可选: `xlings mcp` — 在 `--agent` 格式基础上加 MCP 协议层

---

## 10. 与 MCP 的关系

`xlings agent` + `--agent` 是 **Phase 1** 方案——零配置、通用、任何 Agent 可用。

MCP 是 **Phase 2**（有明确需求时再做）：

```
现在:
  xlings agent skills usage  → Agent 自学（一次）
  xlings xxx --agent          → 纯文本输出（每次）
  覆盖: 90% Agent 场景

未来（有长连接需求时）:
  xlings mcp                 → MCP JSON-RPC server
  复用: Registry + skill 内容 + 输出格式
  覆盖: IDE 集成、实时进度、取消操作
```

skill 内容本身也可作为 MCP server 的 tool description 来源——两者天然共享同一套语义描述。
