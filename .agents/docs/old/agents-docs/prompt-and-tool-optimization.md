# xlings Agent 系统提示词 & Tool 描述优化

## Context

分析当前 agent 系统的 system prompt (`prompt.cppm`) 和 12 个 tool descriptions (`capabilities.cppm`)，找出并修复以下问题：token 冗余、描述不准确、Skills 未接入、Soul 信息未充分利用。

---

## 一、System Prompt 优化 (`src/agent/prompt.cppm`)

### 当前问题

1. **规则 4 重复编号** — 两个 `4.`
2. **规则 1 tool 映射表冗余** — tool name+description 在 tool definitions 里已存在（~100 tokens 浪费）
3. **规则 2 "NEVER say unavailable"** — defensive prompt，正常不会发生
4. **缺少语言跟随规则** — 用户中文提问可能得到英文回复
5. **缺少错误处理指导** — tool 返回错误时 LLM 不知道怎么办
6. **Soul scope 未体现** — system/project scope 影响操作范围但 prompt 没提
7. **Skills 未注入** — `SkillManager` 已实现但从未被调用

### 优化后 prompt 结构

```cpp
auto build_system_prompt(
    const ToolBridge& bridge,
    const libs::soul::Soul& soul,
    const std::vector<libs::agent_skill::Skill>& skills = {}
) -> std::string {

    // 1. Identity + scope
    "You are xlings agent, {persona}. Scope: {scope}.\n"

    // 2. Trust boundaries (保持现有逻辑)
    // readonly / denied_capabilities / forbidden_actions

    // 3. Rules (精简版)
    R"(
## Rules
1. Call tools directly — don't describe intent first.
2. Keep replies concise, plain text.
3. If a tool fails, explain the error to the user — don't retry the same call.
4. Use `search_memory` proactively when past context may help.
5. Reply in the user's language.
)"

    // 4. Skills (新增)
    if (!skills.empty()) {
        "## Skills\n"
        for each skill: "### {name}\n{prompt}\n"
    }
}
```

**Token 变化：** ~450 → ~200（不含 skills），节省约 250 tokens/请求

---

## 二、Tool Descriptions 优化 (`src/capabilities.cppm`)

### Description 修改

| Tool | 当前 | 优化后 |
|------|------|--------|
| `search_packages` | "Search for available packages by keyword. Returns only package names and brief descriptions. Use package_info to get full details (versions, metadata, install status)." | `"Search available packages by keyword"` |
| `list_packages` | "List installed packages, optionally filtered" | `"List installed packages. Filter is a keyword (fuzzy match, not regex)"` |
| `package_info` | "Get complete package information including name, all available versions, install status, and metadata. Use this when you need version details or package state." | `"Get detailed package info: versions, install status, metadata"` |
| `use_version` | "Switch tool version or list available versions" | `"Switch to a specific version, or list available versions if version is omitted"` |

其余 tool descriptions 保持不变（已经足够简洁）。

### inputSchema 补充参数 description

**`install_packages`** — 3 个 boolean 参数缺少说明：
```json
{
  "targets": {"type":"array","items":{"type":"string"},"description":"Package names to install"},
  "yes":     {"type":"boolean","description":"Auto-confirm without prompting"},
  "noDeps":  {"type":"boolean","description":"Skip dependency installation"},
  "global":  {"type":"boolean","description":"Install to global scope"}
}
```

**`list_packages`** — filter 参数补充说明：
```json
{
  "filter": {"type":"string","description":"Keyword to filter packages (fuzzy match)"}
}
```

**`use_version`** — version 参数补充说明：
```json
{
  "target":  {"type":"string","description":"Package name"},
  "version": {"type":"string","description":"Version to switch to. Omit to list available versions"}
}
```

### 可选：移除 outputSchema

核心 8 tools 定义了 `outputSchema`，但 `to_llmapi_tools()` 不传它给 LLM。可以从源码中移除以简化，不影响功能。

---

## 三、Skills 接入 (`src/agent/runtime.cppm` + `src/agent/prompt.cppm`)

### 现状

`SkillManager` (`src/libs/agent_skill.cppm`) 已实现：
- 从 `.agents/skills/builtin/` 和 `.agents/skills/user/` 加载 skill JSON
- 支持 keyword trigger 匹配
- `build_prompt()` 可将 skills 拼接为 prompt 片段

但 `AgentRuntime::init()` 没有调用它。

### 实现

**`src/agent/runtime.cppm`：**
```cpp
import xlings.libs.agent_skill;

// 在 AgentRuntime 中添加成员
libs::agent_skill::SkillManager skill_mgr_;

// init() 中：
void init(const libs::soul::Soul& soul, ..., agentfs::AgentFS& fs) {
    // 加载 skills
    skill_mgr_ = libs::agent_skill::SkillManager(fs);
    skill_mgr_.load_all();

    system_prompt_ = build_system_prompt(bridge, soul, skill_mgr_.all_skills());
    tools_ = to_llmapi_tools(bridge);
}
```

**`src/agent/prompt.cppm`：**
- `build_system_prompt` 增加 `skills` 参数
- Skills 非空时追加 `## Skills` section

---

## 四、修改文件清单

| 文件 | 改动 |
|------|------|
| `src/agent/prompt.cppm` | 精简 rules，新增 skills 参数，追加 skills section |
| `src/capabilities.cppm` | 修改 4 个 tool descriptions + 3 个 inputSchema |
| `src/agent/runtime.cppm` | import agent_skill，初始化 SkillManager，传入 build_system_prompt |

---

## 五、验证

- `rm -rf build && xmake build` — 编译通过
- `xmake run xlings_tests` — 测试通过
- 手动 agent 对话验证：
  - LLM 仍正确调用 tools
  - 用中文提问得到中文回复
  - tool 失败时 LLM 解释错误而非重试
