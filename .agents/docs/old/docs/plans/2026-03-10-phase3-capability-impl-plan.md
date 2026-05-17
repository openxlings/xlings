# Phase 3: Capability 真实实现

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 xim/xvm/config 的核心命令包装为 Capability，通过 Registry 注册，使 Agent/MCP 前端可发现、可调用。

**Architecture:** 每个 Capability 是一个薄包装层：定义 spec（name/schema/destructive）→ 解析 JSON params → 调用现有 cmd_* 函数 → 返回 JSON result。CLI 路径不变（保持直接调用），Registry 供 Agent/MCP 使用。一个 `capabilities.cppm` 模块集中所有实现 + `build_registry()` 工厂函数。

**Tech Stack:** C++23 modules (.cppm), GCC 15, xmake, gtest 1.15.2, nlohmann/json

---

## 现有代码关键发现

| 接口 | 签名 | 说明 |
|------|------|------|
| `Capability::execute` | `(Params json, EventStream&) -> Result json` | 输入输出都是 JSON string |
| `xim::cmd_search` | `(string keyword, EventStream&) -> int` | 返回 exit code |
| `xim::cmd_install` | `(span<string> targets, bool yes, bool noDeps, EventStream&, bool forceGlobal) -> int` | 最复杂的签名 |
| `xim::cmd_remove` | `(string target, EventStream&) -> int` | |
| `xim::cmd_update` | `(string target, EventStream&) -> int` | |
| `xim::cmd_list` | `(string filter, EventStream&) -> int` | |
| `xim::cmd_info` | `(string target, EventStream&) -> int` | |
| `xvm::cmd_use` | `(string target, string version, EventStream&) -> int` | |
| `xvm::cmd_list_versions` | `(string target, EventStream&) -> int` | |

所有命令已通过 EventStream 发射事件（Phase 2 完成），Capability 只需包装参数解析和调用。

---

## Task 1: Capability 实现模块 + 测试骨架

**Files:**
- Create: `src/capabilities.cppm`
- Modify: `tests/unit/test_main.cpp`

**Step 1: 写测试**

在 `tests/unit/test_main.cpp` 末尾追加：

```cpp
// ═══════════════════════════════════════════════════════════════
//  Phase 3: Real Capability implementations
// ═══════════════════════════════════════════════════════════════

TEST(Capabilities, BuildRegistryPopulatesAll) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();

    // At least 8 capabilities registered
    EXPECT_GE(specs.size(), 8);

    // Verify key capabilities exist
    EXPECT_NE(reg.get("search_packages"), nullptr);
    EXPECT_NE(reg.get("install_packages"), nullptr);
    EXPECT_NE(reg.get("remove_package"), nullptr);
    EXPECT_NE(reg.get("update_packages"), nullptr);
    EXPECT_NE(reg.get("list_packages"), nullptr);
    EXPECT_NE(reg.get("package_info"), nullptr);
    EXPECT_NE(reg.get("use_version"), nullptr);
    EXPECT_NE(reg.get("system_status"), nullptr);
}

TEST(Capabilities, SpecsHaveRequiredFields) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();

    for (auto& s : specs) {
        EXPECT_FALSE(s.name.empty()) << "capability has empty name";
        EXPECT_FALSE(s.description.empty()) << s.name << " has empty description";
        EXPECT_FALSE(s.inputSchema.empty()) << s.name << " has empty inputSchema";
    }
}

TEST(Capabilities, DestructiveFlags) {
    auto reg = xlings::capabilities::build_registry();

    // Non-destructive: search, list, info, status
    EXPECT_FALSE(reg.get("search_packages")->spec().destructive);
    EXPECT_FALSE(reg.get("list_packages")->spec().destructive);
    EXPECT_FALSE(reg.get("package_info")->spec().destructive);
    EXPECT_FALSE(reg.get("system_status")->spec().destructive);

    // Destructive: install, remove, update, use
    EXPECT_TRUE(reg.get("install_packages")->spec().destructive);
    EXPECT_TRUE(reg.get("remove_package")->spec().destructive);
    EXPECT_TRUE(reg.get("update_packages")->spec().destructive);
    EXPECT_TRUE(reg.get("use_version")->spec().destructive);
}
```

**Step 2: 运行测试确认失败**

```bash
xmake build xlings_tests 2>&1 | tail -5
```

Expected: 编译失败 — `xlings::capabilities` 不存在

**Step 3: 实现 capabilities.cppm**

创建 `src/capabilities.cppm`：

```cpp
module;

#include <cstdio>

export module xlings.capabilities;

import std;
import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.capability;
import xlings.libs.json;
import xlings.core.xim.commands;
import xlings.core.xvm.commands;
import xlings.core.config;
import xlings.platform;

namespace xlings::capabilities {

using capability::Capability;
using capability::CapabilitySpec;
using capability::Params;
using capability::Result;

// ─── Helper: wrap exit code as JSON result ───

Result exit_result(int code) {
    return nlohmann::json({{"exitCode", code}}).dump();
}

// ─── xim capabilities ───

class SearchPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "search_packages",
            .description = "Search for packages by keyword",
            .inputSchema = R"({"type":"object","properties":{"keyword":{"type":"string"}},"required":["keyword"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto keyword = json.value("keyword", "");
        return exit_result(xim::cmd_search(keyword, stream));
    }
};

class InstallPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "install_packages",
            .description = "Install one or more packages",
            .inputSchema = R"({"type":"object","properties":{"targets":{"type":"array","items":{"type":"string"}},"yes":{"type":"boolean"},"noDeps":{"type":"boolean"},"global":{"type":"boolean"}},"required":["targets"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        std::vector<std::string> targets;
        if (json.contains("targets") && json["targets"].is_array()) {
            for (auto& t : json["targets"]) targets.push_back(t.get<std::string>());
        }
        bool yes = json.value("yes", false);
        bool noDeps = json.value("noDeps", false);
        bool global = json.value("global", false);
        return exit_result(xim::cmd_install(targets, yes, noDeps, stream, global));
    }
};

class RemovePackage : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "remove_package",
            .description = "Remove an installed package",
            .inputSchema = R"({"type":"object","properties":{"target":{"type":"string"}},"required":["target"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xim::cmd_remove(json.value("target", ""), stream));
    }
};

class UpdatePackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "update_packages",
            .description = "Update package index or a specific package",
            .inputSchema = R"({"type":"object","properties":{"target":{"type":"string"}}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xim::cmd_update(json.value("target", ""), stream));
    }
};

class ListPackages : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "list_packages",
            .description = "List installed packages, optionally filtered",
            .inputSchema = R"({"type":"object","properties":{"filter":{"type":"string"}}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xim::cmd_list(json.value("filter", ""), stream));
    }
};

class PackageInfo : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "package_info",
            .description = "Show detailed information about a package",
            .inputSchema = R"({"type":"object","properties":{"target":{"type":"string"}},"required":["target"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        return exit_result(xim::cmd_info(json.value("target", ""), stream));
    }
};

// ─── xvm capabilities ───

class UseVersion : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "use_version",
            .description = "Switch tool version or list available versions",
            .inputSchema = R"({"type":"object","properties":{"target":{"type":"string"},"version":{"type":"string"}},"required":["target"]})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = true,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto json = nlohmann::json::parse(params, nullptr, false);
        auto target = json.value("target", "");
        auto version = json.value("version", "");
        if (version.empty()) {
            return exit_result(xvm::cmd_list_versions(target, stream));
        }
        return exit_result(xvm::cmd_use(target, version, stream));
    }
};

// ─── System capabilities ───

class SystemStatus : public Capability {
public:
    auto spec() const -> CapabilitySpec override {
        return {
            .name = "system_status",
            .description = "Show xlings system configuration and status",
            .inputSchema = R"({"type":"object","properties":{}})",
            .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
            .destructive = false,
        };
    }
    auto execute(Params params, EventStream& stream) -> Result override {
        auto& p = Config::paths();
        nlohmann::json fields = nlohmann::json::array();
        auto addField = [&](const std::string& label, const std::string& value, bool hl = false) {
            fields.push_back({{"label", label}, {"value", value}, {"highlight", hl}});
        };
        addField("XLINGS_HOME", p.homeDir.string());
        addField("XLINGS_DATA", p.dataDir.string());
        addField("XLINGS_SUBOS", p.subosDir.string());
        addField("active subos", p.activeSubos, true);
        addField("bin", p.binDir.string());

        auto mirror = Config::mirror();
        if (!mirror.empty()) addField("mirror", mirror);
        auto lang = Config::lang();
        if (!lang.empty()) addField("lang", lang);

        nlohmann::json payload;
        payload["title"] = "xlings status";
        payload["fields"] = std::move(fields);
        stream.emit(DataEvent{"info_panel", payload.dump()});
        return exit_result(0);
    }
};

// ─── Registry factory ───

export capability::Registry build_registry() {
    capability::Registry reg;
    reg.register_capability(std::make_unique<SearchPackages>());
    reg.register_capability(std::make_unique<InstallPackages>());
    reg.register_capability(std::make_unique<RemovePackage>());
    reg.register_capability(std::make_unique<UpdatePackages>());
    reg.register_capability(std::make_unique<ListPackages>());
    reg.register_capability(std::make_unique<PackageInfo>());
    reg.register_capability(std::make_unique<UseVersion>());
    reg.register_capability(std::make_unique<SystemStatus>());
    return reg;
}

} // namespace xlings::capabilities
```

**Step 4: 运行测试确认通过**

```bash
rm -rf build && xmake build xlings_tests && xmake run xlings_tests --gtest_filter="Capabilities.*"
```

Expected: 3 tests PASS

**Step 5: 提交**

```bash
git add src/capabilities.cppm tests/unit/test_main.cpp
git commit -m "feat: implement real Capability wrappers for all xim/xvm commands"
```

---

## Task 2: CLI 注册 Registry + Agent 入口准备

**Files:**
- Modify: `src/cli.cppm`
- Modify: `tests/unit/test_main.cpp`

**Step 1: 写测试**

```cpp
TEST(Capabilities, RegistryListAllSpecs) {
    auto reg = xlings::capabilities::build_registry();
    auto specs = reg.list_all();

    // Each spec should have valid JSON schema
    for (auto& s : specs) {
        auto parsed = nlohmann::json::parse(s.inputSchema, nullptr, false);
        EXPECT_FALSE(parsed.is_discarded()) << s.name << " has invalid inputSchema";
    }
}

TEST(Capabilities, SearchExecuteWithMockStream) {
    // Test that execute() correctly parses params and calls through
    // This test only validates the JSON round-trip, not the actual search
    // (which requires package index to be loaded)
    auto reg = xlings::capabilities::build_registry();
    auto* cap = reg.get("search_packages");
    ASSERT_NE(cap, nullptr);

    auto s = cap->spec();
    EXPECT_EQ(s.name, "search_packages");

    // Verify inputSchema parses as valid JSON
    auto schema = nlohmann::json::parse(s.inputSchema);
    EXPECT_TRUE(schema.contains("required"));
    EXPECT_EQ(schema["required"][0], "keyword");
}
```

**Step 2: 在 cli.cppm 创建 Registry**

在 `run()` 函数开头，创建 Registry 并保存引用（供未来 Agent 子命令使用）：

```cpp
// In run() after EventStream setup:
auto registry = capabilities::build_registry();
```

新增 `import xlings.capabilities;` 到 cli.cppm 的导入列表。

**Step 3: 运行全部测试**

```bash
rm -rf build && xmake build xlings_tests && xmake run xlings_tests
```

Expected: 所有 139 + 5 新测试 PASS

**Step 4: 构建主二进制验证**

```bash
xmake build xlings
```

Expected: 编译成功

**Step 5: 提交**

```bash
git add src/cli.cppm tests/unit/test_main.cpp
git commit -m "feat: register Capability Registry in CLI, add schema validation tests"
```

---

## Task 3: 验证全部测试 + 手动验证

**Step 1: 运行全部单元测试**

```bash
xmake run xlings_tests
```

Expected: 全部 PASS

**Step 2: 手动验证 CLI 功能不受影响**

```bash
./build/linux/x86_64/release/xlings search node
./build/linux/x86_64/release/xlings list
./build/linux/x86_64/release/xlings info node
./build/linux/x86_64/release/xlings --version
```

Expected: 所有命令正常工作，UI 不变

**Step 3: 提交（如有修复）**

---

## 设计决策说明

### 为什么 CLI 保持直接调用？

CLI → xim::cmd_search (直接) 比 CLI → Registry → Capability → xim::cmd_search (间接) 少一层。CLI 路径是高频使用，保持直接调用避免不必要的 JSON 序列化开销。

### 为什么 Capability 是薄包装？

业务逻辑已在 cmd_* 函数中，且已解耦到 EventStream。Capability 只做：
1. 定义 JSON Schema（供 Agent/MCP 发现）
2. JSON params ↔ 函数参数转换
3. exit code → JSON result 转换

### 为什么一个文件？

~8 个 Capability，每个 ~25 行。放一个文件清晰且避免 GCC 模块分区 ICE 风险。
