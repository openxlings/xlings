# Agent 上下文构造 & 循环架构分析

## Context

分析：每次 LLM 调用时如何构造上下文（system prompt + tools + memory + 对话历史），以及模型回复规范。

---

## 每次 LLM 调用的上下文构造

### 当前构造流程

```
每次 LLM 调用 (run_one_turn 内循环迭代):

┌─ API 请求 ──────────────────────────────────────────────────────┐
│                                                                  │
│  messages[]:                                                     │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ [0] System: build_system_prompt()                        │   │
│  │     "You are xlings-agent..."                            │   │
│  │     + 工具列表 (重复! API tools 已包含)                    │   │
│  │     + 规则 + manage_tree workflow + example              │   │
│  │     ~800 tokens                                          │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │ [1] System: context_summary (if compacted)               │   │
│  │     "[Previous conversation summary (N turns evicted):   │   │
│  │       Turn 1: User: ... → Agent: ... [tools: ...]       │   │
│  │       Turn 2: ..."                                       │   │
│  │     0~500 tokens (max 10 summaries)                      │   │
│  ├──────────────────────────────────────────────────────────┤   │
│  │ [2..N] 对话历史 (L1 热缓存):                              │   │
│  │     User → Assistant → Tool → Assistant → Tool → ...     │   │
│  │     (最近 3+ turns 的完整消息)                             │   │
│  │     变长: 几千到几十万 tokens                              │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                  │
│  params.tools[]:                    ← API 独立参数               │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │ 12 个 capability tools:                                   │   │
│  │   search_packages, install_packages, remove_package,     │   │
│  │   update_packages, list_packages, package_info,          │   │
│  │   use_version, system_status, set_log_level,             │   │
│  │   run_command, view_output, search_content               │   │
│  │ + 1 虚拟工具: manage_tree                                 │   │
│  │ 每个: name + description + inputSchema (JSON Schema)     │   │
│  │ ~1500 tokens                                              │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                  │
│  params.temperature, params.maxTokens                            │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘

代码路径 (loop.cppm + cli.cppm):

  // ═══ Session 开始时 (cli.cppm:1008-1010) ═══
  auto bridge = agent::ToolBridge(registry);
  auto system_prompt = agent::build_system_prompt(bridge);  // 构建一次
  auto tools = agent::to_llmapi_tools(bridge);              // 构建一次

  // ═══ 上下文模型 ═══
  // 维护一个长生命周期的 conversation 对象:
  //
  //   conversation.messages = [
  //     [0] System: system_prompt,     ← Session 开始时设置一次, 永驻
  //     [1] System: context_summary,   ← 可选, 压缩后插入/更新
  //     [2] User: turn1_input,
  //     [3] Asst: turn1_response_1,
  //     [4] Tool: turn1_tool_result_1,
  //     [5] Asst: turn1_response_2,
  //     ...                            ← Turn 1 的所有迭代
  //     [N] User: turn2_input,
  //     [N+1] Asst: ...               ← Turn 2 继续追加
  //     ...                            ← 整个 session 不断增长
  //   ]
  //
  // system_prompt 和 tools 只在 session 开始时构建一次。
  // conversation 是单一长上下文, 不断追加新消息。
  //
  // ═══ 每次 API 调用 (HTTP 是无状态的) ═══
  // API 无状态 → 每次 HTTP 请求必须发送完整上下文:
  //
  //   provider.chat_stream(conversation.messages, params):
  //   ┌──────────────────────────────────────────────────┐
  //   │ HTTP POST /v1/messages                            │
  //   │ {                                                 │
  //   │   "system": system_prompt,     ← 每次都发但内容固定│
  //   │   "tools": [...13个...],       ← 每次都发但内容固定│
  //   │   "messages": conversation,    ← 完整历史, 每轮更长│
  //   │ }                                                 │
  //   └──────────────────────────────────────────────────┘
  //
  // Prompt caching 的意义: system + tools + 旧消息构成稳定前缀,
  // 被 API 服务端缓存 (Anthropic 显式标记, OpenAI 自动匹配),
  // 只有新追加的消息是全价计费。
  //
  // ═══ run_one_turn (loop.cppm) ═══
  // conversation.push(User: user_input);       // 追加用户输入
  // for (i = 0; i < 40; ++i) {                 // 迭代循环
  //   params.tools = tools;                    // 固定引用, 不重建
  //   response = provider.chat_stream(conversation.messages, params);
  //   conversation.push(Asst: response);       // 追加 LLM 回复
  //   conversation.push(Tool: result);         // 追加工具结果
  //   // → 下次迭代发送更长的 conversation
  // }
  //
  // ═══ Turn 间 (cli.cppm) ═══
  // conversation 保留所有历史, 下一轮继续追加
  // auto_compact 在 ctx_used > 75% 时淘汰旧 turns 到 L2 摘要
```

### 问题: 工具信息重复

```
build_system_prompt() 输出包含:
  "### Built-in Tools (ALWAYS prefer these)
   - **search_packages**: Search for packages by keyword
   - **install_packages**: Install one or more packages
   ..."

params.tools[] 又包含:
  ToolDef { name: "search_packages", description: "Search for packages by keyword",
            inputSchema: {"type":"object","properties":{"keyword":...}} }

→ 工具的 name + description 出现了两遍
→ 浪费 ~300 tokens / 请求
```

### 当前上下文生命周期

```
Session 开始
  │
  ├─ 新会话: conversation = []
  └─ resume: conversation = load_conversation() (全部 L1 消息)
  │
  ▼
Turn 1: 用户输入 "安装 mdbook"
  │
  ├─ conversation.push(User: "安装 mdbook")
  ├─ 迭代 1: [System + User] + tools → LLM
  │  └─ LLM: text + tool_use(manage_tree, search_packages)
  │     conversation.push(Assistant)
  │     conversation.push(Tool: manage_tree result)
  │     conversation.push(Tool: search result)
  ├─ 迭代 2: [System + User + Asst + Tool×2] + tools → LLM
  │  └─ ... 继续
  ├─ 迭代 N: stopReason == EndTurn → 返回 reply
  │
  ├─ session_mgr.save_conversation()     ← 持久化完整对话
  ├─ ctx_mgr.record_turn()              ← 递增 turn 计数器
  ▼
Turn 2: 用户输入 "用 0.4.40 版本"
  │
  ├─ maybe_auto_compact():
  │  └─ if ctx_used > 75% of limit:
  │     ├─ evict_oldest_turns_() → 旧 turns 从 L1 移到 L2 (摘要)
  │     ├─ update_context_summary_() → 在 messages[1] 注入/更新摘要
  │     └─ save_cache() → L2/L3 持久化到磁盘
  │
  ├─ conversation = [System, ContextSummary, ...recent turns...]
  └─ 发送给 LLM (带压缩摘要的上下文)
```

---

## 3 级上下文缓存 (ContextManager)

```
┌─── L1: 热缓存 (conversation.messages) ──────────────────────────┐
│  完整消息, 直接发送给 LLM                                        │
│  包含: System + ContextSummary(可选) + 最近 N turns              │
│  大小: 随对话增长, 到 75% ctx_limit 时触发压缩                    │
├─── L2: 暖缓存 (l2_summaries_) ──────────────────────────────────┤
│  TurnSummary { turn_id, user_brief, assistant_brief,            │
│                tool_names[], keywords[], estimated_tokens }      │
│  来源: 从 L1 淘汰的 turns 自动生成摘要                           │
│  注入: 压缩后作为 context_summary System 消息注入 (max 10 条)    │
│  持久化: .agents/sessions/{id}/context_cache/l2_summaries.json  │
├─── L3: 冷缓存 (l3_index_) ─────────────────────────────────────┤
│  关键词索引: keyword → [turn_id, turn_id, ...]                  │
│  用途: retrieve_relevant(query) → 按关键词匹配相关 turns         │
│  持久化: .agents/sessions/{id}/context_cache/l3_index.json      │
└──────────────────────────────────────────────────────────────────┘

自动压缩触发条件:
  ctx_used > trigger_ratio(0.75) × ctx_limit
  → 淘汰旧 turns 到 target_ratio(0.50) × ctx_limit
  → 保留 min_keep_turns(3) 个最近 turns

注: L3 的 retrieve_relevant() 已实现但 **当前未被调用**。
    build_context_prefix() 只按时间序注入最近 10 条 L2 摘要,
    不使用 L3 关键词检索。L3 是为未来 "相关性检索" 预留的。
```

---

## Memory (当前状态)

**已有 semantic_memory 模块** (`src/libs/semantic_memory.cppm`) 提供完整的记忆存储:
- `MemoryStore::remember(content, category, embedding)` — 保存记忆
- `MemoryStore::recall_text(keyword)` — 关键词搜索
- `MemoryStore::recall_embedding(query)` — 向量相似度搜索 (预留)
- `MemoryStore::forget(id)` — 删除记忆
- 持久化: `.agents/memory/entries/*.json` (由 AgentFS 管理)
- 分类: `fact` / `preference` / `experience`

**已有但未连通**: MemoryStore 存在但**未注册为 LLM 工具**, LLM 无法主动保存/检索记忆。
需要: 在 `capabilities.cppm` 中创建 SaveMemory/SearchMemory/ForgetMemory Capability 子类,
内部调用 MemoryStore API, 并在 session 开始时将已有记忆摘要注入 system prompt。

对话历史通过 session 持久化:
```
.agents/sessions/{session_id}/
  ├─ meta.json          (id, title, model, created_at, updated_at, turn_count)
  ├─ conversation.json  (完整 L1 消息序列)
  └─ context_cache/
     ├─ l2_summaries.json
     ├─ l3_index.json
     └─ context_meta.json
```

---

## 模型回复规范 (当前)

```
System prompt 中的规范 (loop.cppm:149-152):

"### Response Format:
 Start every reply with a one-line title summarizing your action or decision.
 Then provide details on subsequent lines."
```

**当前问题:**
- 没有 structured output (JSON mode / schema)
- 回复格式仅靠自然语言指令约束
- manage_tree 返回的 JSON 未强制 schema 验证
- 工具调用参数依赖 LLM 自觉遵守 inputSchema

---

---

## Prompt Caching 实施方案

### 缓存原理

**API 是无状态的** — 每次 HTTP 请求必须包含完整的 tools + messages。但两家 API 都有缓存机制：

```
请求 N: [System + Tools + msg1..msgN-1] + [msgN]
         ↑── 前缀与请求 N-1 相同 ──────↑   ↑新增
         缓存命中 (10% 价格)              全价
```

| | Anthropic | OpenAI |
|--|-----------|--------|
| 触发方式 | **显式**: 需要 `cache_control` 标记 | **自动**: 无需标记 |
| 缓存粒度 | 标记的 content block 及其之前的前缀 | 前缀自动匹配 (≥1024 tokens) |
| 缓存价格 | 读取 10%, 写入 125% | 读取 50% |
| TTL | 5 分钟 | ~5-10 分钟 |
| 位置 | system / messages / tools 上都可标记 | 无需标记 |

### Anthropic 实现

需要修改 `build_payload_()` — 把 system prompt 从 string 改为 content blocks array with cache_control:

```
当前 payload:
{
  "system": "You are xlings-agent...",    ← 纯字符串, 不能加 cache_control
  "messages": [...],
  "tools": [...]
}

改后 payload:
{
  "system": [                              ← 改为 content blocks array
    {
      "type": "text",
      "text": "You are xlings-agent...",
      "cache_control": {"type": "ephemeral"}   ← 标记缓存断点
    }
  ],
  "tools": [
    ...,
    {                                      ← 最后一个 tool 上也加缓存断点
      "name": "manage_tree",
      ...,
      "cache_control": {"type": "ephemeral"}
    }
  ],
  "messages": [
    ...,
    {                                      ← 最后一条旧消息上也可加断点
      "role": "user",
      "content": "...",
      "cache_control": {"type": "ephemeral"}
    },
    {"role": "user", "content": "新消息"}   ← 只有这条是全价
  ]
}
```

Anthropic 允许最多 **4 个** cache_control 断点。推荐策略:
1. System prompt 末尾 — 缓存角色 + 规则
2. Tools 最后一个 — 缓存工具定义
3. 倒数第 2 条消息 — 缓存对话前缀
4. (可选) Context summary — 缓存压缩历史

### OpenAI 实现

**不需要改代码** — OpenAI 自 2024 年底起自动缓存 ≥1024 tokens 的前缀。但需要:
1. 在 response.usage 中读取 `prompt_tokens_details.cached_tokens` 来追踪
2. 确保 stream 模式下 `stream_options: {"include_usage": true}` 被设置

### 需要修改的文件

#### 1. `mcpplibs/llmapi/src/types.cppm` — Message 添加 cache_control

```cpp
// 新增
export struct CacheControl {
    std::string type {"ephemeral"};
};

export struct Message {
    Role role;
    std::variant<std::string, std::vector<ContentPart>> content;
    std::optional<CacheControl> cacheControl;   // ← 新增
    // ... factory methods
};
```

#### 2. `mcpplibs/llmapi/src/providers/anthropic.cppm`

**`build_payload_()` 修改**:

```cpp
// system: string → content blocks array
if (!systemText.empty()) {
    Json sysBlocks = Json::array();
    Json block;
    block["type"] = "text";
    block["text"] = systemText;
    block["cache_control"] = {{"type", "ephemeral"}};  // 断点 1
    sysBlocks.push_back(block);
    payload["system"] = sysBlocks;
}

// tools: 最后一个 tool 加 cache_control
if (params.tools && !params.tools->empty()) {
    Json tools = Json::array();
    for (size_t i = 0; i < params.tools->size(); ++i) {
        Json t = /* 现有序列化 */;
        if (i == params.tools->size() - 1) {
            t["cache_control"] = {{"type", "ephemeral"}};  // 断点 2
        }
        tools.push_back(t);
    }
    payload["tools"] = tools;
}

// messages: 倒数第 2 条加 cache_control (缓存对话前缀)
// (在 extract_system_and_messages_ 中处理)
```

**`parse_response_()` 修改** — 读取缓存统计:

```cpp
if (json.contains("usage")) {
    result.usage.inputTokens = usage.value("input_tokens", 0);
    result.usage.outputTokens = usage.value("output_tokens", 0);
    // 新增
    result.usage.cacheCreationTokens = usage.value("cache_creation_input_tokens", 0);
    result.usage.cacheReadTokens = usage.value("cache_read_input_tokens", 0);
}
```

需要在 header 中加 `anthropic-beta: prompt-caching-2024-07-31` (可能已毕业为正式 API)。

#### 3. `mcpplibs/llmapi/src/providers/openai.cppm`

**`build_payload_()` 修改** — stream 模式加 include_usage:

```cpp
if (stream) {
    payload["stream"] = true;
    payload["stream_options"] = {{"include_usage", true}};  // 已有? 检查
}
```

**`parse_response_()` 修改** — 读取缓存统计:

```cpp
if (json.contains("usage")) {
    // 新增
    if (usage.contains("prompt_tokens_details")) {
        result.usage.cacheReadTokens =
            usage["prompt_tokens_details"].value("cached_tokens", 0);
    }
}
```

#### 4. `mcpplibs/llmapi/src/types.cppm` — Usage 添加缓存字段

```cpp
export struct Usage {
    int inputTokens{0};
    int outputTokens{0};
    int totalTokens{0};
    int cacheCreationTokens{0};    // ← 新增 (Anthropic)
    int cacheReadTokens{0};        // ← 新增 (Anthropic + OpenAI)
};
```

#### 5. `src/agent/token_tracker.cppm` — 追踪缓存命中

```cpp
int session_cache_read_ {0};
int session_cache_write_ {0};
// 在 TUI 状态栏显示缓存命中率
```

### 实施后效果估算

```
10 次 LLM 迭代的 mdbook 任务:

                  无缓存 (当前)    有缓存 (实施后)
System+Tools:     2,300 × 10      2,300 + 2,300×0.1×9 = 4,370
对话前缀:         累计 ~35,000     ~35,000 × 0.1 = 3,500
新增内容:         ~5,000           ~5,000
─────────────────────────────────────────────────
总 input tokens:  ~58,000          ~12,870
节省:                              ~78%
```

---

## 动态工具注册架构设计

### 已有基础设施 (不需要新建)

```
src/libs/agentfs.cppm         — AgentFS: .agents/ 目录抽象层, JSON/JSONL I/O
src/libs/semantic_memory.cppm — MemoryStore: remember/recall_text/recall_embedding/forget
src/libs/agent_skill.cppm     — SkillManager: load_all/match/build_prompt
src/libs/soul.cppm            — SoulManager: 权限/能力策略
src/libs/journal.cppm         — Journal: turn/tool 日志
src/agent/resource_cache.cppm — ResourceCache: TTL 感知运行时缓存
src/agent/resource_tools.cppm — tool_search_resources/tool_load_resource
src/agent/package_tools.cppm  — PackageToolRegistry: 包贡献工具
src/agent/mcp_client.cppm     — McpClient: MCP 工具发现
src/agent/context_manager.cppm — ContextManager: 3 级上下文缓存

关键: Memory 由 semantic_memory 模块实现, Skills 由 agent_skill 模块实现,
     工具只是这些模块对 LLM 的接口层 (Capability 子类)。
```

### 工具来源分层

```
┌─ 工具注册表 (ToolRegistry) ─────────────────────────────────────────┐
│                                                                      │
│  T0: 系统工具 (始终加载, 不可卸载)                                    │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ manage_tree        — 任务树管理 (虚拟工具, loop 拦截)           │  │
│  │ save_memory        — 保存记忆 (→ semantic_memory::remember)    │  │
│  │ search_memory      — 搜索记忆 (→ semantic_memory::recall_text) │  │
│  │ forget_memory      — 删除记忆 (→ semantic_memory::forget)      │  │
│  │ manage_context     — 上下文缓存管理 (→ ContextManager)         │  │
│  │ list_tools         — 列出当前工具集                             │  │
│  │ enable_tool        — 启用工具                                  │  │
│  │ disable_tool       — 禁用工具                                  │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  T1: 核心能力 (内置, 默认启用) — capabilities.cppm 现有              │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ search_packages, install_packages, remove_package,             │  │
│  │ update_packages, list_packages, package_info,                  │  │
│  │ use_version, system_status                                     │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  T2: 扩展工具 (内置, 默认启用)                                       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ run_command          — 执行 shell 命令                         │  │
│  │ view_output          — 查看命令输出                             │  │
│  │ search_content       — 搜索文件内容                             │  │
│  │ set_log_level        — 设置日志级别                             │  │
│  │ web_search           — 网络搜索 (→ tinyhttps)       ← 新增     │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  T3: Skills 工具 (从 .agents/skills/ 加载, → agent_skill 模块)       │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ SkillManager.load_all() 加载 builtin/ + user/ skills          │  │
│  │ 每个 skill 有 triggers[] → 匹配用户输入 → 注入 prompt          │  │
│  │ 可扩展: skill 定义 tool.json → 注册为 LLM 可调用工具           │  │
│  │ 当前已有 skills: xlings-quickstart, xlings-build,              │  │
│  │   system-design, mcpp-style-ref, ui-ux-pro-max                │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  T4: 包贡献工具 (已安装包自动注册, → package_tools 模块)    已有框架  │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ PackageToolRegistry: 从 .agents/cache/package_tools.json 加载 │  │
│  │ 如: mdbook_build (由 xim:mdbook 包提供)                       │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  T5: MCP 工具 (外部服务, → mcp_client 模块)                已有框架  │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ McpClient: 从 .agents/mcps/*.json 加载服务器配置              │  │
│  │ 工具名: "mcp:{server}:{tool}"                                 │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

### 新增工具集详细设计

#### Memory 工具集 (由 semantic_memory + agentfs 实现)

```
实现方式: 新增 Capability 子类, 内部调用已有 MemoryStore API
存储位置: .agents/memory/entries/*.json (已由 agentfs 管理)

save_memory(content, category?)
  → MemoryStore::remember(content, category, {})
  → category: "fact" | "preference" | "experience" (MemoryEntry 已有)
  → 返回 memory id

search_memory(query, category?)
  → MemoryStore::recall_text(query) → 关键词匹配
  → 返回匹配的记忆列表 (id + content + category)

forget_memory(id)
  → MemoryStore::forget(id)
```

Session 开始时: `MemoryStore` 加载 `.agents/memory/entries/` 下所有记忆,
将摘要注入 system prompt 尾部 (通过 prompt_builder)。

#### Context 工具集 (由 ContextManager 实现)

```
manage_context(action, ...)
  action: "compact" | "status" | "retrieve"
  compact:  → ctx_mgr.force_compact()       手动触发压缩
  status:   → ctx_mgr 的 L1/L2/L3 统计     turns, tokens, cache_hits
  retrieve: → ctx_mgr.retrieve_relevant()   从 L3 关键词检索相关历史
```

#### Tool 管理工具集 (由 ToolRegistry 实现)

```
list_tools()
  → 返回当前已注册的所有工具 (分层: T0-T5, 启用/禁用状态)

enable_tool(name)
  → 动态启用一个已注册但被禁用的工具
  → 下次 LLM 调用时 params.tools 包含此工具

disable_tool(name)
  → 动态禁用工具 (T0 不可禁用)
  → 下次 LLM 调用时 params.tools 不包含此工具
```

#### Web Search 工具 (新增 T2)

```
web_search(query, max_results?)
  → 通过 tinyhttps 调用搜索 API (DuckDuckGo / Searxng / 自建)
  → 返回: [{title, url, snippet}, ...]
  → 为 LLM 提供实时信息获取能力
```

#### Skills 工具集 (由 agent_skill 模块实现)

```
当前 agent_skill.cppm 已有:
  SkillManager.load_all()    — 加载 .agents/skills/{builtin,user}/*.json
  SkillManager.match(input)  — 按 triggers 关键词匹配
  SkillManager.build_prompt() — 生成 skill prompt

扩展方案:
  1. skill 目录可选包含 tool.json → 声明为 LLM 可调用工具
  2. 执行方式: spawn handler.sh / 调用内部函数
  3. SkillManager 注册到 ToolRegistry 作为 T3 层工具提供者

现有 skill 目录结构 (.agents/skills/):
  xlings-quickstart/ (SKILL.md + agents/ + references/)
  xlings-build/      (SKILL.md + agents/openai.yaml + references/)
  system-design/     (SKILL.md)
  mcpp-style-ref/    (SKILL.md + reference.md)
  ui-ux-pro-max/     (SKILL.md + scripts/ + data/)
```

### 工具集注入 LLM 的流程

```
Session 开始:
  1. AgentFS.ensure_initialized() — 确保 .agents/ 目录结构
  2. MemoryStore 加载 .agents/memory/entries/ → 注入 system prompt
  3. SkillManager.load_all() → 加载 skills (prompt 注入 + 可选 tool 注册)
  4. 注册 T0 系统工具 (memory, context, tool_mgmt Capabilities)
  5. 注册 T1+T2 核心+扩展工具 (capabilities.cppm build_registry())
  6. PackageToolRegistry 加载 → 注册 T4 包工具
  7. McpClient 加载 → 注册 T5 MCP 工具

每次 LLM 调用:
  params.tools = registry.enabled_tools()  ← 只包含启用的工具
  // LLM 可以调用 list_tools() 发现所有工具
  // LLM 可以调用 enable_tool() 动态启用被禁用的工具
```

### 已有代码复用表

| 已有模块 | 路径 | 复用方式 |
|----------|------|---------|
| **MemoryStore** | `src/libs/semantic_memory.cppm` | T0 memory 工具直接调用 remember/recall_text/forget |
| **AgentFS** | `src/libs/agentfs.cppm` | 目录管理 + JSON I/O, memory/skills 存储层 |
| **SkillManager** | `src/libs/agent_skill.cppm` | T3 skill 加载 + prompt 构建 |
| **Journal** | `src/libs/journal.cppm` | 工具调用日志 (已集成) |
| **SoulManager** | `src/libs/soul.cppm` | 工具权限策略 (已集成) |
| **PackageToolRegistry** | `src/agent/package_tools.cppm` | T4 工具加载 (已有) |
| **McpClient** | `src/agent/mcp_client.cppm` | T5 MCP 工具发现 (已有) |
| **ResourceCache** | `src/agent/resource_cache.cppm` | 运行时缓存 (ResourceKind::Memory/Skill) |
| **ContextManager** | `src/agent/context_manager.cppm` | T0 context 工具直接暴露 API |
| **capability::Registry** | `src/runtime/capability.cppm` | T1/T2 工具注册基础 |

### 需要新建/修改的文件

| 文件 | 操作 | 内容 |
|------|------|------|
| `src/agent/tool_registry.cppm` | **新建** | 统一工具注册表 (分层 T0-T5, 启用/禁用, 聚合所有来源) |
| `src/capabilities.cppm` | **修改** | 新增 Capability 子类: SaveMemory, SearchMemory, ForgetMemory, ManageContext, ListTools, EnableTool, DisableTool, WebSearch — 各自调用已有模块 API |
| `src/agent/loop.cppm` | **修改** | tools 从 ToolRegistry.enabled_tools() 获取; build_system_prompt() 删除工具列表重复, 注入 memory 摘要 |
| `src/agent/prompt_builder.cppm` | **修改** | 注入 memory 摘要 + skill prompts 到 system prompt |
| `src/cli.cppm` | **修改** | 初始化 ToolRegistry, 加载 memory + skills + packages + MCP |
| `src/libs/agent_skill.cppm` | **可选修改** | 扩展: skill 目录含 tool.json 时注册为可调用工具 |

---

## 当前架构的 5 个瓶颈

### 1. manage_tree 消耗大量迭代次数 (最严重)

```
一个典型 "安装所有 mdbook 版本" 任务:
  3× add_task     → 3 次 manage_tree tool call
  3× start_task   → 3 次 manage_tree tool call
  3× complete_task → 3 次 manage_tree tool call
  ─────────────────
  9 次 manage_tree = 9 次 LLM 迭代 (仅管理树!)

实际工具: search + info + install×2 + use_version×2 + list + run_command×3 = ~11 次
总计: 9 + 11 = 20 次迭代 (manage_tree 占 45%!)
```

**对比**: Claude Code 没有显式任务管理工具，任务结构由 UI 自动推断。
**对比**: 用户实际遇到 max iterations reached，根因是 manage_tree 吃掉了一半迭代预算。

### 2. Tool calls 顺序执行

```cpp
// 当前: 顺序执行
for (const auto& call : calls) {
    auto result = handle_tool_call(call, ...);  // 阻塞等待
    conversation.push(tool_msg);
}
```

LLM 可以一次返回多个独立 tool call (如 `search_packages` + `system_status`)，但当前逐个执行。

**对比**: Claude Code 并行执行无依赖的 tool calls。

### 3. Provider 代码重复 (可维护性)

```
lines 415-451: Anthropic worker thread (37 行)
lines 452-489: OpenAI worker thread (37 行)
↑ 几乎完全相同，仅 Config 和 Provider 类型不同
```

### 4. `run_one_turn` 参数爆炸 (18 个参数)

```cpp
run_one_turn(conversation, user_input, system_prompt, tools,
             bridge, stream, cfg,
             on_stream_chunk, policy, confirm_cb,
             on_tool_call, on_tool_result,
             ctx_mgr, tracker, on_auto_compact,
             cancel, task_tree, tree_root,
             on_tree_update, on_token_update)
```

**对比**: Claude Code 用 `AgentConfig` struct 聚合配置; OpenAI SDK 用 `RunConfig`.

### 5. 固定迭代上限

40 次硬上限 — 简单任务浪费，复杂任务不够。无 token 预算感知。

---

## 优化方案 (按优先级排序)

### P0: manage_tree 批处理 — 解决 max iterations

**问题**: 每个 add_task/start_task/complete_task 各占一次 LLM 迭代。
**方案**: 让 `manage_tree` 支持 `batch` action，一次调用执行多个操作。

```cpp
// manage_tree 新增 batch action:
{
  "action": "batch",
  "operations": [
    {"action": "add_task", "parent_id": 0, "title": "搜索版本"},
    {"action": "add_task", "parent_id": 0, "title": "安装版本"},
    {"action": "add_task", "parent_id": 0, "title": "验证安装"},
    {"action": "start_task", "node_id": 1}
  ]
}
```

**效果**: 9 次 manage_tree → 2-3 次 (规划 1 次 batch + 完成 1-2 次)
**风险**: 低 — 纯内部工具，不涉及外部 API
**文件**: `src/agent/loop.cppm` (handle_manage_tree + tool def schema)

### P1: 并行 Tool 执行

**方案**: LLM 返回多个 tool calls 时，并行执行无审批需求的工具。

```cpp
// 分类: 需审批 vs 不需审批
auto [auto_calls, confirm_calls] = partition(calls, needs_confirm);
// 并行执行 auto_calls
std::vector<std::future<ToolResultContent>> futures;
for (auto& call : auto_calls) {
    futures.push_back(std::async(std::launch::async, [&] {
        return handle_tool_call(call, bridge, stream, policy, confirm_cb, cancel);
    }));
}
// 收集结果
for (auto& f : futures) results.push_back(f.get());
// 顺序执行需审批的 (因为需要 UI 交互)
for (auto& call : confirm_calls) {
    results.push_back(handle_tool_call(call, ...));
}
```

**风险**: 中 — ToolBridge event_buffer_ 不是线程安全的，需要 per-call buffer
**文件**: `src/agent/loop.cppm`, `src/agent/tool_bridge.cppm`

### P2: TurnConfig struct — 消除参数爆炸

```cpp
struct TurnConfig {
    llm::Conversation& conversation;
    std::string_view user_input;
    const std::string& system_prompt;
    const std::vector<llm::ToolDef>& tools;
    ToolBridge& bridge;
    EventStream& stream;
    const LlmConfig& cfg;
    // Callbacks
    std::function<void(std::string_view)> on_stream_chunk;
    ApprovalPolicy* policy = nullptr;
    ConfirmCallback confirm_cb;
    ToolCallCallback on_tool_call;
    ToolResultCallback on_tool_result;
    ContextManager* ctx_mgr = nullptr;
    TokenTracker* tracker = nullptr;
    AutoCompactCallback on_auto_compact;
    CancellationToken* cancel = nullptr;
    tui::TaskTree* task_tree = nullptr;
    tui::TreeNode* tree_root = nullptr;
    TreeUpdateCallback on_tree_update;
    TokenUpdateCallback on_token_update;
};

export auto run_one_turn(TurnConfig& tc) -> TurnResult;
```

**风险**: 低 — 纯重构
**文件**: `src/agent/loop.cppm`, `src/cli.cppm` (调用处)

### P3: Provider 模板消除重复

```cpp
template<typename Provider, typename Config>
auto llm_call_worker(Config cfg, std::vector<llm::Message> msgs,
                     llm::ChatParams& params,
                     std::function<void(std::string_view)> on_chunk,
                     CancellationToken* cancel) -> llm::ChatResponse;
```

**风险**: 低 — GCC 15 模块中模板有时有坑，但这个足够简单
**文件**: `src/agent/loop.cppm`

### P4: 动态迭代预算 (长期)

替换固定 `MAX_ITERATIONS = 40` 为 token 预算感知:

```cpp
// 当 ctx_used 接近 ctx_limit 时自动停止
if (tracker && tracker->context_used() > ctx_limit * 0.9) {
    turn_result.reply = "[approaching context limit, stopping]";
    return turn_result;
}
```

---

## 上下文预算分析

### 固定开销 (每次 LLM 调用都存在)

| 组件 | 内容 | 估算 tokens |
|------|------|-------------|
| **System Prompt** | build_system_prompt(): 角色 + 工具说明 + 规则 + manage_tree workflow + example | ~800 |
| **Tools Schema** (params.tools) | 13 个工具定义 (12 capability + manage_tree), 各含 name + description + inputSchema (JSON Schema) | ~1500 |
| **L2 Context Prefix** | 自动注入的压缩历史摘要 (max 10 turns) | 0~500 |
| **合计固定开销** | | **~2300** |

### 上下文限制 (TokenTracker::context_limit)

| 模型 | ctx_limit | 固定开销占比 | 可用对话空间 |
|------|-----------|-------------|-------------|
| Claude | 200k | 1.2% | ~197k |
| GPT-4/5 | 128k | 1.8% | ~125k |
| DeepSeek | 64k | 3.6% | ~61k |
| Qwen | 128k | 1.8% | ~125k |
| 其他 | 32k (default) | 7.2% | ~29k |

### 当前架构特点

1. **System prompt 内嵌工具说明** — build_system_prompt() 把所有 tool 的 name+description 以 markdown 列表写入 system prompt，同时 tools schema 作为 params.tools 单独发送给 API → **工具信息重复了两遍**
2. **Tools schema 固定** — 所有 13 个工具每次都发送，无论任务是否需要
3. **manage_tree schema 最大** — 6 个 properties + enum，占 tools 总量的 ~20%
4. **自动压缩**: ctx_used > 75% 时触发，压缩到 50%，保留最近 3 turns

### 对比主流做法

| 方面 | 当前 xlings | Claude Code | OpenAI Agents |
|------|-------------|-------------|---------------|
| System prompt | 内嵌工具列表 + 规则 | 仅规则/角色 | instructions (不含工具) |
| Tools 传递 | system 重复 + params.tools | 仅 params.tools | 仅 tools 参数 |
| 动态工具集 | 全量 13 个 | 按任务类型筛选 | 按 Agent 角色分配 |
| 上下文管理 | 3级缓存 + 自动压缩 | 对话压缩 + 摘要 | 无 (依赖 API 限制) |

### 优化建议

**P0-a: 消除 system prompt 中的工具重复** — `build_system_prompt()` 删除 "Available Tools" 列表（API 的 tools 参数已包含完整定义），仅保留规则和 manage_tree workflow。节省 ~300 tokens/请求。

**P5 (长期): 动态工具集** — 根据对话阶段或用户意图只发送相关工具子集。如 "安装" 任务不需要 search_content/view_output。

## 推荐实施顺序

| 优先级 | 改动 | 效果 | 复杂度 |
|--------|------|------|--------|
| **P0** | manage_tree batch | 迭代次数减半，解决 max iterations | 低 |
| P1 | 并行 tool 执行 | 多工具任务加速 | 中 |
| P2 | TurnConfig struct | 可维护性提升 | 低 |
| P3 | Provider 模板 | 消除 37 行重复代码 | 低 |
| P4 | 动态迭代预算 | 消除硬上限 | 低 |

---

## 修改文件清单

| 文件 | 改动 |
|------|------|
| `src/agent/tool_registry.cppm` | **新建**: 统一工具注册表 (T0-T5 分层, 启用/禁用) |
| `src/capabilities.cppm` | 新增 Capability 子类: SaveMemory, SearchMemory, ForgetMemory, ManageContext, ListTools, EnableTool, DisableTool, WebSearch |
| `src/agent/loop.cppm` | P0: batch manage_tree; P2: TurnConfig; P3: provider 模板; P4: 动态预算; tools 从 ToolRegistry 获取 |
| `src/agent/prompt_builder.cppm` | 注入 memory 摘要 + matched skill prompts |
| `src/agent/tool_bridge.cppm` | P1: per-call event buffer (线程安全) |
| `src/cli.cppm` | P2: TurnConfig; 初始化 ToolRegistry + MemoryStore + SkillManager |
| `src/libs/agent_skill.cppm` | 可选: skill 含 tool.json 时注册为可调用工具 |
| `mcpplibs/llmapi/src/types.cppm` | Usage 添加 cacheCreationTokens/cacheReadTokens |
| `mcpplibs/llmapi/src/providers/anthropic.cppm` | system → content blocks + cache_control |
| `mcpplibs/llmapi/src/providers/openai.cppm` | 读取 cached_tokens; stream_options |
| `src/agent/token_tracker.cppm` | 追踪缓存命中率 |

---

## 验证

1. `rm -rf build && xmake build` — 编译通过
2. `xmake run xlings_tests` — 209 测试全过
3. 手动测试 "安装所有 mdbook 版本并使用第二新版本" — 不再 max iterations
4. 观察 manage_tree 调用次数: batch 后应 ≤ 3 次 (vs 之前 ~9 次)
