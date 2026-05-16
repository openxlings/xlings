# Agent Context Construction & Loop Architecture Optimization

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Optimize the agent loop by adding manage_tree batch support, eliminating code duplication, connecting Memory/Context tools to LLM, and cleaning up the 18-parameter function signature.

**Architecture:** Refactor `loop.cppm` to use a `TurnConfig` struct, add a provider template to eliminate 37-line duplication, implement `batch` action in manage_tree handler, add Memory/Context Capability subclasses wrapping existing `MemoryStore` and `ContextManager`, and integrate `PromptBuilder` into the agent session init.

**Tech Stack:** C++23 modules, xmake, llmapi 0.2.2 (external), nlohmann::json

**Constraints:**
- `llmapi` is an external package (v0.2.2) — cannot modify its types. Prompt caching requires a separate llmapi PR and is **out of scope** for this plan.
- Parallel tool execution (P1) requires thread-safe event buffer redesign — **deferred** to a follow-up.
- Full ToolRegistry (T0-T5 layers) — **deferred**; this plan adds individual tools via existing `capability::Registry`.
- Web search tool — **deferred**; needs API integration design.

---

## Chunk 1: manage_tree batch + TurnConfig struct

### Task 1: Add `batch` action handler to manage_tree

The `manage_tree_tool_def()` already declares the `batch` action with `operations` array in its JSON Schema (loop.cppm:32,55-70), but `handle_manage_tree()` has no handler for it — it falls through to the `unknown action` error.

**Files:**
- Modify: `src/agent/loop.cppm:156-261` (handle_manage_tree function)

- [ ] **Step 1: Add batch handler before the unknown-action else clause**

In `handle_manage_tree()`, between the `update_task` handler (line 246) and the final `else` (line 248), add:

```cpp
    } else if (action == "batch") {
        if (!json.contains("operations") || !json["operations"].is_array()) {
            return llm::ToolResultContent{
                .toolUseId = call.id,
                .content = R"({"error":"operations array is required for batch action"})",
                .isError = true,
            };
        }
        nlohmann::json results = nlohmann::json::array();
        for (auto it = json["operations"].begin(); it != json["operations"].end(); ++it) {
            auto& op = *it;
            auto op_action = op.value("action", "");

            if (op_action == "add_task") {
                int parent_id = op.value("parent_id", 0);
                auto title = op.value("title", "");
                auto details = op.value("details", "");
                if (title.empty()) {
                    results.push_back({{"error", "title required"}, {"action", "add_task"}});
                    continue;
                }
                int node_id = task_tree.add_task(root, parent_id, title, details);
                results.push_back({{"ok", true}, {"action", "add_task"}, {"node_id", node_id}, {"title", title}});
                if (on_tree_update) on_tree_update("add_task", node_id, title);

            } else if (op_action == "start_task") {
                int node_id = op.value("node_id", -1);
                if (node_id <= 0) {
                    results.push_back({{"error", "node_id required"}, {"action", "start_task"}});
                    continue;
                }
                task_tree.start_task(root, node_id);
                results.push_back({{"ok", true}, {"action", "start_task"}, {"node_id", node_id}});
                if (on_tree_update) {
                    auto* node = root.find_node(node_id);
                    on_tree_update("start_task", node_id, node ? node->title : "");
                }

            } else if (op_action == "complete_task") {
                int node_id = op.value("node_id", -1);
                auto task_result = op.value("result", "");
                if (node_id <= 0) {
                    results.push_back({{"error", "node_id required"}, {"action", "complete_task"}});
                    continue;
                }
                task_tree.complete_task(root, node_id, tui::TreeNode::Done, task_result);
                results.push_back({{"ok", true}, {"action", "complete_task"}, {"node_id", node_id}});
                if (on_tree_update) on_tree_update("complete_task", node_id, "");

            } else if (op_action == "cancel_task") {
                int node_id = op.value("node_id", -1);
                if (node_id <= 0) {
                    results.push_back({{"error", "node_id required"}, {"action", "cancel_task"}});
                    continue;
                }
                task_tree.complete_task(root, node_id, tui::TreeNode::Cancelled);
                results.push_back({{"ok", true}, {"action", "cancel_task"}, {"node_id", node_id}});
                if (on_tree_update) on_tree_update("cancel_task", node_id, "");

            } else if (op_action == "update_task") {
                int node_id = op.value("node_id", -1);
                auto new_title = op.value("title", "");
                auto new_details = op.value("details", "");
                if (node_id <= 0) {
                    results.push_back({{"error", "node_id required"}, {"action", "update_task"}});
                    continue;
                }
                task_tree.update_task(root, node_id, new_title, new_details);
                results.push_back({{"ok", true}, {"action", "update_task"}, {"node_id", node_id}});
                if (on_tree_update) on_tree_update("update_task", node_id, new_title);

            } else {
                results.push_back({{"error", "unknown action: " + op_action}});
            }
        }
        result_json = {{"ok", true}, {"action", "batch"}, {"results", std::move(results)}};
```

- [ ] **Step 2: Build and verify compilation**

Run: `rm -rf build && xmake build`
Expected: Clean compilation with no errors.

- [ ] **Step 3: Run tests**

Run: `xmake run xlings_tests`
Expected: All 209+ tests pass (batch handler is a new code path, existing tests should still pass).

- [ ] **Step 4: Commit**

```bash
git add src/agent/loop.cppm
git commit -m "feat(agent): add batch action handler for manage_tree virtual tool"
```

---

### Task 2: Extract TurnConfig struct from run_one_turn's 18 parameters

**Files:**
- Modify: `src/agent/loop.cppm:148-352` (callback types + run_one_turn signature)
- Modify: `src/cli.cppm:1656-1825` (call site)

- [ ] **Step 1: Define TurnConfig struct in loop.cppm**

After the callback type aliases (line 154), add:

```cpp
export struct TurnConfig {
    llm::Conversation& conversation;
    std::string_view user_input;
    const std::string& system_prompt;
    const std::vector<llm::ToolDef>& tools;
    ToolBridge& bridge;
    EventStream& stream;
    const LlmConfig& cfg;
    // Callbacks
    std::function<void(std::string_view)> on_stream_chunk;
    ApprovalPolicy* policy {nullptr};
    ConfirmCallback confirm_cb;
    ToolCallCallback on_tool_call;
    ToolResultCallback on_tool_result;
    ContextManager* ctx_mgr {nullptr};
    TokenTracker* tracker {nullptr};
    AutoCompactCallback on_auto_compact;
    CancellationToken* cancel {nullptr};
    tui::TaskTree* task_tree {nullptr};
    tui::TreeNode* tree_root {nullptr};
    TreeUpdateCallback on_tree_update;
    TokenUpdateCallback on_token_update;
};
```

- [ ] **Step 2: Add new run_one_turn overload that takes TurnConfig**

Add a new overload below the struct:

```cpp
export auto run_one_turn(TurnConfig& tc) -> TurnResult;
```

Change the existing 18-parameter `run_one_turn` to delegate to the new one:

```cpp
export auto run_one_turn(
    llm::Conversation& conversation,
    std::string_view user_input,
    const std::string& system_prompt,
    const std::vector<llm::ToolDef>& tools,
    ToolBridge& bridge,
    EventStream& stream,
    const LlmConfig& cfg,
    std::function<void(std::string_view)> on_stream_chunk,
    ApprovalPolicy* policy,
    ConfirmCallback confirm_cb,
    ToolCallCallback on_tool_call,
    ToolResultCallback on_tool_result,
    ContextManager* ctx_mgr,
    TokenTracker* tracker,
    AutoCompactCallback on_auto_compact,
    CancellationToken* cancel,
    tui::TaskTree* task_tree,
    tui::TreeNode* tree_root,
    TreeUpdateCallback on_tree_update,
    TokenUpdateCallback on_token_update
) -> TurnResult {
    TurnConfig tc{
        .conversation = conversation,
        .user_input = user_input,
        .system_prompt = system_prompt,
        .tools = tools,
        .bridge = bridge,
        .stream = stream,
        .cfg = cfg,
        .on_stream_chunk = std::move(on_stream_chunk),
        .policy = policy,
        .confirm_cb = std::move(confirm_cb),
        .on_tool_call = std::move(on_tool_call),
        .on_tool_result = std::move(on_tool_result),
        .ctx_mgr = ctx_mgr,
        .tracker = tracker,
        .on_auto_compact = std::move(on_auto_compact),
        .cancel = cancel,
        .task_tree = task_tree,
        .tree_root = tree_root,
        .on_tree_update = std::move(on_tree_update),
        .on_token_update = std::move(on_token_update),
    };
    return run_one_turn(tc);
}
```

Then rename the implementation body to use `tc.conversation`, `tc.user_input`, etc. throughout.

- [ ] **Step 3: Build and verify**

Run: `rm -rf build && xmake build`
Expected: Clean compilation — the old call site in cli.cppm still uses the 18-param overload (backward compatible).

- [ ] **Step 4: Run tests**

Run: `xmake run xlings_tests`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/agent/loop.cppm
git commit -m "refactor(agent): extract TurnConfig struct from run_one_turn's 18 parameters"
```

---

### Task 3: Eliminate provider worker thread duplication with template

The Anthropic worker (lines 414-443) and OpenAI worker (lines 451-480) are nearly identical — only the Config and Provider types differ.

**Files:**
- Modify: `src/agent/loop.cppm:386-484` (LLM call section in the iteration loop)

- [ ] **Step 1: Add a helper function template before run_one_turn**

```cpp
// Cancellable LLM call via worker thread — generic over provider type
template<typename Provider, typename Config>
auto llm_call_worker(
    Config cfg,
    std::vector<llm::Message> msgs,
    llm::ChatParams& params,
    std::function<void(std::string_view)> safe_chunk,
    bool has_stream_cb,
    CancellationToken* cancel
) -> llm::ChatResponse {
    auto abandoned = std::make_shared<std::atomic<bool>>(false);
    auto done_flag = std::make_shared<std::atomic<bool>>(false);
    auto resp_ptr = std::make_shared<llm::ChatResponse>();
    auto err_ptr = std::make_shared<std::exception_ptr>();
    auto cv_mtx = std::make_shared<std::mutex>();
    auto cv_done = std::make_shared<std::condition_variable>();

    auto wrapped_chunk = [abandoned, safe_chunk](std::string_view chunk) {
        if (abandoned->load(std::memory_order_acquire)) throw CancelledException{};
        if (safe_chunk) safe_chunk(chunk);
    };

    std::thread worker([done_flag, resp_ptr, err_ptr, cv_mtx, cv_done,
                        provider = Provider(std::move(cfg)),
                        call_msgs = std::move(msgs), &params,
                        wrapped_chunk, has_stream_cb]() mutable {
        try {
            if (has_stream_cb) {
                *resp_ptr = provider.chat_stream(call_msgs, params, wrapped_chunk);
            } else {
                *resp_ptr = provider.chat(call_msgs, params);
            }
        } catch (...) {
            *err_ptr = std::current_exception();
        }
        done_flag->store(true, std::memory_order_release);
        cv_done->notify_all();
    });

    {
        std::unique_lock lk(*cv_mtx);
        while (!done_flag->load(std::memory_order_acquire)) {
            if (cancel && !cancel->is_active()) {
                abandoned->store(true, std::memory_order_release);
                worker.detach();
                if (cancel->is_paused()) throw PausedException{};
                throw CancelledException{};
            }
            cv_done->wait_for(lk, std::chrono::milliseconds{200});
        }
    }
    worker.join();

    if (*err_ptr) std::rethrow_exception(*err_ptr);
    return std::move(*resp_ptr);
}
```

- [ ] **Step 2: Replace the two provider blocks with template calls**

Replace the `if (tc.cfg.provider == "anthropic") { ... } else { ... }` block with:

```cpp
        if (tc.cfg.provider == "anthropic") {
            llm::anthropic::Config acfg{
                .apiKey = tc.cfg.api_key,
                .model = tc.cfg.model,
            };
            if (!tc.cfg.base_url.empty()) acfg.baseUrl = tc.cfg.base_url;
            response = llm_call_worker<llm::anthropic::Anthropic>(
                std::move(acfg), std::move(msgs), params, safe_chunk, has_stream_cb, tc.cancel);
        } else {
            llm::openai::Config ocfg{
                .apiKey = tc.cfg.api_key,
                .model = tc.cfg.model,
            };
            if (!tc.cfg.base_url.empty()) ocfg.baseUrl = tc.cfg.base_url;
            response = llm_call_worker<llm::openai::OpenAI>(
                std::move(ocfg), std::move(msgs), params, safe_chunk, has_stream_cb, tc.cancel);
        }
```

This eliminates ~37 lines of duplicated worker thread code.

- [ ] **Step 3: Build and verify**

Run: `rm -rf build && xmake build`
Expected: Clean compilation. Note: GCC 15 can be finicky with templates in modules — if ICE occurs, move the template to the module implementation section or use a non-template approach with `std::function`.

- [ ] **Step 4: Run tests**

Run: `xmake run xlings_tests`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/agent/loop.cppm
git commit -m "refactor(agent): eliminate provider worker thread duplication with template"
```

---

## Chunk 2: Memory & Context Capabilities

### Task 4: Add Memory tool capabilities (SaveMemory, SearchMemory, ForgetMemory)

These wrap the existing `MemoryStore` API from `src/libs/semantic_memory.cppm` and register them as LLM-callable tools.

**Files:**
- Modify: `src/capabilities.cppm` (add 3 new Capability subclasses + register them)

- [ ] **Step 1: Add imports at top of capabilities.cppm**

After the existing imports, add:

```cpp
import xlings.libs.semantic_memory;
import xlings.libs.agentfs;
import xlings.agent.context_manager;
```

- [ ] **Step 2: Add MemoryStore holder**

After `shared_output_buffer()` function (line 33), add:

```cpp
// Shared MemoryStore — initialized once by build_registry()
libs::semantic_memory::MemoryStore* shared_memory_store_{nullptr};

void set_memory_store(libs::semantic_memory::MemoryStore* store) {
    shared_memory_store_ = store;
}
```

- [ ] **Step 3: Add SaveMemory capability**

```cpp
class SaveMemory : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "save_memory",
            .description = "Save a piece of information to long-term memory for future sessions",
            .inputSchema = R"({"type":"object","properties":{"content":{"type":"string","description":"The information to remember"},"category":{"type":"string","enum":["fact","preference","experience"],"default":"fact"}},"required":["content"]})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        if (!shared_memory_store_) {
            return nlohmann::json({{"error", "memory store not initialized"}}).dump();
        }
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto content = json.value("content", "");
        auto category = json.value("category", "fact");
        if (content.empty()) {
            return nlohmann::json({{"error", "content is required"}}).dump();
        }
        auto id = shared_memory_store_->remember(content, category);
        return nlohmann::json({{"ok", true}, {"id", id}, {"category", category}}).dump();
    }
};
```

- [ ] **Step 4: Add SearchMemory capability**

```cpp
class SearchMemory : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "search_memory",
            .description = "Search long-term memory by keyword",
            .inputSchema = R"({"type":"object","properties":{"query":{"type":"string","description":"Search keyword"},"max_results":{"type":"integer","default":5}},"required":["query"]})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        if (!shared_memory_store_) {
            return nlohmann::json({{"error", "memory store not initialized"}}).dump();
        }
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto query = json.value("query", "");
        int max_results = json.value("max_results", 5);
        auto results = shared_memory_store_->recall_text(query, max_results);
        nlohmann::json arr = nlohmann::json::array();
        for (auto& r : results) {
            arr.push_back({{"id", r.entry.id}, {"content", r.entry.content},
                           {"category", r.entry.category}, {"score", r.score}});
        }
        return nlohmann::json({{"results", std::move(arr)}, {"count", static_cast<int>(results.size())}}).dump();
    }
};
```

- [ ] **Step 5: Add ForgetMemory capability**

```cpp
class ForgetMemory : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "forget_memory",
            .description = "Delete a memory entry by ID",
            .inputSchema = R"({"type":"object","properties":{"id":{"type":"string","description":"Memory entry ID to delete"}},"required":["id"]})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        if (!shared_memory_store_) {
            return nlohmann::json({{"error", "memory store not initialized"}}).dump();
        }
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto id = json.value("id", "");
        bool ok = shared_memory_store_->forget(id);
        return nlohmann::json({{"ok", ok}}).dump();
    }
};
```

- [ ] **Step 6: Register memory tools in build_registry()**

Modify `build_registry()` to accept an optional MemoryStore pointer and register the new tools:

```cpp
export capability::Registry build_registry(
    libs::semantic_memory::MemoryStore* memory_store = nullptr
) {
    // ... existing registrations ...

    // Memory tools
    if (memory_store) {
        set_memory_store(memory_store);
        reg.register_capability(std::make_unique<SaveMemory>());
        reg.register_capability(std::make_unique<SearchMemory>());
        reg.register_capability(std::make_unique<ForgetMemory>());
    }
    return reg;
}
```

- [ ] **Step 7: Build and verify**

Run: `rm -rf build && xmake build`
Expected: Clean compilation.

- [ ] **Step 8: Run tests**

Run: `xmake run xlings_tests`
Expected: All tests pass. Existing tests call `build_registry()` with no args (backward compatible).

- [ ] **Step 9: Commit**

```bash
git add src/capabilities.cppm
git commit -m "feat(agent): add SaveMemory, SearchMemory, ForgetMemory capabilities"
```

---

### Task 5: Add ManageContext capability

Wraps `ContextManager` to let LLM manually trigger compaction or check context stats.

**Files:**
- Modify: `src/capabilities.cppm`

- [ ] **Step 1: Add ContextManager holder**

Near `shared_memory_store_`:

```cpp
agent::ContextManager* shared_ctx_mgr_{nullptr};

void set_context_manager(agent::ContextManager* mgr) {
    shared_ctx_mgr_ = mgr;
}
```

- [ ] **Step 2: Add ManageContext capability class**

```cpp
class ManageContext : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "manage_context",
            .description = "Manage conversation context: check stats or retrieve relevant history",
            .inputSchema = R"({"type":"object","properties":{"action":{"type":"string","enum":["status","retrieve"],"description":"status=show cache stats, retrieve=find relevant past turns"},"query":{"type":"string","description":"Search query for retrieve action"}},"required":["action"]})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        if (!shared_ctx_mgr_) {
            return nlohmann::json({{"error", "context manager not initialized"}}).dump();
        }
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto action = json.value("action", "");

        if (action == "status") {
            return nlohmann::json({
                {"l1_messages", "see conversation"},
                {"l2_summaries", shared_ctx_mgr_->l2_count()},
                {"l3_keywords", shared_ctx_mgr_->l3_keyword_count()},
                {"evicted_tokens", shared_ctx_mgr_->total_evicted_tokens()},
                {"turn_count", shared_ctx_mgr_->next_turn_id()},
            }).dump();
        } else if (action == "retrieve") {
            auto query = json.value("query", "");
            if (query.empty()) {
                return nlohmann::json({{"error", "query required for retrieve"}}).dump();
            }
            auto results = shared_ctx_mgr_->retrieve_relevant(query, 5);
            nlohmann::json arr = nlohmann::json::array();
            for (auto* s : results) {
                arr.push_back({{"turn_id", s->turn_id}, {"user", s->user_brief},
                               {"assistant", s->assistant_brief}, {"tools", s->tool_names}});
            }
            return nlohmann::json({{"results", std::move(arr)}}).dump();
        }
        return nlohmann::json({{"error", "unknown action: " + action}}).dump();
    }
};
```

- [ ] **Step 3: Register in build_registry()**

Update `build_registry()` signature to also accept ContextManager:

```cpp
export capability::Registry build_registry(
    libs::semantic_memory::MemoryStore* memory_store = nullptr,
    agent::ContextManager* ctx_mgr = nullptr
) {
    // ... existing + memory registrations ...

    // Context management tool
    if (ctx_mgr) {
        set_context_manager(ctx_mgr);
        reg.register_capability(std::make_unique<ManageContext>());
    }
    return reg;
}
```

- [ ] **Step 4: Build and verify**

Run: `rm -rf build && xmake build`
Expected: Clean compilation.

- [ ] **Step 5: Run tests**

Run: `xmake run xlings_tests`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/capabilities.cppm
git commit -m "feat(agent): add ManageContext capability for context cache inspection"
```

---

## Chunk 3: Integration — Wire Memory + Context into Agent Session

### Task 6: Initialize MemoryStore in cli.cppm and pass to build_registry

**Files:**
- Modify: `src/cli.cppm` (agent session init section, ~line 963-1010)

- [ ] **Step 1: Add MemoryStore initialization after AgentFS/Soul setup**

After `auto cfg = agent::resolve_llm_config(...)` (around line 966), add:

```cpp
            // Memory store
            libs::semantic_memory::MemoryStore memory_store(afs);
            memory_store.load();
```

- [ ] **Step 2: Move registry creation after MemoryStore + ContextManager init**

The current code creates the registry at line ~920 (before the agent block). For memory/context tools, move the registry creation into the agent session block, after both MemoryStore and ContextManager are initialized:

```cpp
            // Token tracker + context manager
            agent::TokenTracker tracker;
            agent::ContextManager ctx_mgr(cfg.model);
            auto cache_dir = afs.sessions_path() / session_meta.id / "context_cache";
            ctx_mgr.set_cache_dir(cache_dir);

            // Build registry with memory + context tools
            auto registry = capabilities::build_registry(&memory_store, &ctx_mgr);
            auto bridge = agent::ToolBridge(registry);
            auto system_prompt = agent::build_system_prompt(bridge);
            auto tools = agent::to_llmapi_tools(bridge);
```

Note: The registry is currently created higher up in cli.cppm for non-agent usage too. The non-agent path should keep using `build_registry()` with no args. Move only the agent-mode registry construction.

- [ ] **Step 3: Build and verify**

Run: `rm -rf build && xmake build`
Expected: Clean compilation. Watch for module import ordering issues.

- [ ] **Step 4: Run tests**

Run: `xmake run xlings_tests`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/cli.cppm
git commit -m "feat(agent): wire MemoryStore and ContextManager into agent session"
```

---

### Task 7: Inject memory summary into system prompt

When the session starts, load existing memories and append a summary to the system prompt so the LLM knows what it has remembered.

**Files:**
- Modify: `src/agent/loop.cppm` (build_system_prompt function)

- [ ] **Step 1: Change build_system_prompt to accept optional memory entries**

```cpp
export auto build_system_prompt(
    [[maybe_unused]] const ToolBridge& bridge,
    std::span<const libs::semantic_memory::MemoryEntry> memories = {}
) -> std::string {
    std::string prompt = R"(You are xlings-agent, ...)";
    // ... existing prompt content ...

    // Append memory summary if available
    if (!memories.empty()) {
        prompt += "\n## Remembered Context\n\n";
        prompt += "You have " + std::to_string(memories.size()) + " memories from previous sessions:\n";
        int count = 0;
        for (auto& m : memories) {
            if (count >= 20) {
                prompt += "... and " + std::to_string(memories.size() - 20) + " more (use search_memory to find them)\n";
                break;
            }
            prompt += "- [" + m.category + "] " + m.content.substr(0, 100);
            if (m.content.size() > 100) prompt += "...";
            prompt += "\n";
            ++count;
        }
        prompt += "\nUse save_memory/search_memory/forget_memory to manage your long-term memory.\n";
    }

    return prompt;
}
```

Note: This requires adding `import xlings.libs.semantic_memory;` to loop.cppm. **GCC 15 caution**: if the import causes ICE, pass a `std::vector<std::pair<std::string,std::string>>` (category+content pairs) instead to avoid cross-module type issues.

- [ ] **Step 2: Update cli.cppm call site to pass memories**

```cpp
auto system_prompt = agent::build_system_prompt(bridge, memory_store.all_entries());
```

- [ ] **Step 3: Build and verify**

Run: `rm -rf build && xmake build`
Expected: Clean compilation.

- [ ] **Step 4: Run tests**

Run: `xmake run xlings_tests`
Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/agent/loop.cppm src/cli.cppm
git commit -m "feat(agent): inject memory summary into system prompt at session start"
```

---

### Task 8: Add cache token tracking to TokenTracker

Track cache read/write tokens for future prompt caching support. Even though llmapi doesn't expose these yet, we add the fields now so the TUI can show them when available.

**Files:**
- Modify: `src/agent/token_tracker.cppm`

- [ ] **Step 1: Add cache tracking fields**

```cpp
export class TokenTracker {
    int session_input_ {0};
    int session_output_ {0};
    int session_cache_read_ {0};     // ← new
    int session_cache_write_ {0};    // ← new
    int last_context_size_ {0};

public:
    void record(int input_tokens, int output_tokens,
                int cache_read = 0, int cache_write = 0) {
        session_input_ += input_tokens;
        session_output_ += output_tokens;
        session_cache_read_ += cache_read;
        session_cache_write_ += cache_write;
        last_context_size_ = input_tokens;
    }

    auto session_cache_read() const -> int { return session_cache_read_; }
    auto session_cache_write() const -> int { return session_cache_write_; }

    // Existing methods unchanged...
    void reset() {
        session_input_ = 0;
        session_output_ = 0;
        session_cache_read_ = 0;
        session_cache_write_ = 0;
        last_context_size_ = 0;
    }
    // ... rest unchanged
};
```

- [ ] **Step 2: Build and verify**

Run: `rm -rf build && xmake build`
Expected: Clean compilation. The `record(int, int)` 2-arg call still works (defaults).

- [ ] **Step 3: Run tests**

Run: `xmake run xlings_tests`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/agent/token_tracker.cppm
git commit -m "feat(agent): add cache token tracking fields to TokenTracker"
```

---

## Chunk 4: Dynamic Iteration Budget

### Task 9: Replace fixed MAX_ITERATIONS with token-aware budget

**Files:**
- Modify: `src/agent/loop.cppm` (iteration loop in run_one_turn)

- [ ] **Step 1: Replace the fixed limit with a context-aware check**

Change:
```cpp
    constexpr int MAX_ITERATIONS = 40;
    for (int i = 0; i < MAX_ITERATIONS; ++i) {
```

To:
```cpp
    constexpr int MAX_ITERATIONS = 50;      // raised safety limit
    constexpr int TOOL_ONLY_LIMIT = 40;     // limit for tool-only iterations (no text output)
    int consecutive_tool_only = 0;

    for (int i = 0; i < MAX_ITERATIONS; ++i) {
```

- [ ] **Step 2: Add context budget check before each LLM call**

After the auto-compact check, add:

```cpp
        // Context budget check: stop if approaching limit
        if (tc.tracker) {
            int ctx_limit = TokenTracker::context_limit(tc.cfg.model);
            if (tc.tracker->context_used() > static_cast<int>(ctx_limit * 0.92)) {
                turn_result.reply = "[approaching context limit (" +
                    TokenTracker::format_tokens(tc.tracker->context_used()) + "/" +
                    TokenTracker::format_tokens(ctx_limit) + "), stopping]";
                return turn_result;
            }
        }
```

- [ ] **Step 3: Track consecutive tool-only iterations**

After processing tool calls (before the `continue` at end of loop), add a runaway detection:

```cpp
        // Runaway detection: too many iterations with only tool calls and no text
        if (response.text().empty()) {
            ++consecutive_tool_only;
        } else {
            consecutive_tool_only = 0;
        }
        if (consecutive_tool_only > TOOL_ONLY_LIMIT) {
            turn_result.reply = "[agent: too many tool-only iterations, stopping]";
            return turn_result;
        }
```

- [ ] **Step 4: Build and verify**

Run: `rm -rf build && xmake build`
Expected: Clean compilation.

- [ ] **Step 5: Run tests**

Run: `xmake run xlings_tests`
Expected: All tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/agent/loop.cppm
git commit -m "feat(agent): replace fixed iteration limit with token-aware budget"
```

---

## Summary of Changes

| File | Changes |
|------|---------|
| `src/agent/loop.cppm` | batch handler, TurnConfig struct, provider template, memory prompt, dynamic budget |
| `src/capabilities.cppm` | SaveMemory, SearchMemory, ForgetMemory, ManageContext capabilities |
| `src/cli.cppm` | Wire MemoryStore + ContextManager into agent session |
| `src/agent/token_tracker.cppm` | Cache token tracking fields |

## Deferred to Follow-up Plans

| Item | Reason |
|------|--------|
| **Prompt caching** | Requires llmapi changes (external package) — separate PR to llmapi repo |
| **Parallel tool execution** | Thread-safety redesign of ToolBridge event_buffer_ needed |
| **Full ToolRegistry (T0-T5)** | Over-engineered for current tool count; revisit when MCP/Skills tools grow |
| **Web search tool** | Needs search API integration design |
| **Skill tools** | Needs tool.json spec design in agent_skill module |
| **PromptBuilder integration** | PromptBuilder exists but isn't used by loop; defer until Soul/Skills are fully wired |
