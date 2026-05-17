# T03 — 新建 `core/env.cppm`：多环境管理命令

> **Wave**: 3（依赖 T02）
> **预估改动**: ~120 行 C++，1 个新文件 + xmake.lua 更新

---

## 1. 任务概述

新建 `core/env.cppm` 模块，实现 `xlings env` 的 5 个子命令：`new / use / list / remove / info`。本模块负责环境的目录创建、.xlings.json 读写和切换，不负责世代管理（由 T04 的 profile 模块承担）。

**设计背景**: 详见 [../main.md §5](../main.md) 和 [../env-store-design.md §3](../env-store-design.md)

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T02 | 需要新的 `Config::PathInfo`（含 `homeDir`、`activeEnv`、`env_data_dir()`） |

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/env.cppm` | 新建模块 |
| `xmake.lua` | 将 `core/env.cppm` 加入构建 |

---

## 4. 实施步骤

### 4.1 模块接口（完整代码框架）

```cpp
export module xlings.env;

import std;
import xlings.config;
import xlings.json;
import xlings.log;
import xlings.platform;

export namespace xlings::env {

struct EnvInfo {
    std::string             name;
    std::filesystem::path   dataDir;
    bool                    isActive;
    int                     toolCount;  // bin/ 下的工具数（非 xvm-shim 本体）
};

// 列出所有已创建的环境
std::vector<EnvInfo> list_envs();

// 创建新环境（创建目录 + 写入 .xlings.json 的 envs 字段）
int create_env(const std::string& name,
               const std::filesystem::path& customData = {});

// 切换当前激活环境（修改 .xlings.json 的 activeEnv）
int use_env(const std::string& name);

// 删除环境（非 default，且非当前激活环境）
int remove_env(const std::string& name);

// 显示环境详情
std::optional<EnvInfo> get_env_info(const std::string& name);

// 命令入口（由 cmdprocessor 调用）
int run(int argc, char* argv[]);

} // namespace xlings::env
```

### 4.2 实现：`create_env()`

```cpp
int create_env(const std::string& name,
               const std::filesystem::path& customData) {
    auto& p = Config::paths();

    // 验证名称（仅允许 [a-z0-9_-]）
    for (char c : name) {
        if (!std::isalnum(c) && c != '_' && c != '-') {
            log::error("[xlings:env] invalid env name: {}", name);
            log::error("  env name must match [a-z0-9_-]");
            return 1;
        }
    }

    // 检查是否已存在
    auto configPath = p.homeDir / ".xlings.json";
    auto json = read_config_json_(configPath);
    if (json.contains("envs") && json["envs"].contains(name)) {
        log::error("[xlings:env] environment '{}' already exists", name);
        return 1;
    }

    // 确定 data 目录
    auto dataDir = customData.empty()
        ? (p.homeDir / "envs" / name)
        : customData;

    // 创建目录结构
    std::filesystem::create_directories(dataDir / "bin");
    std::filesystem::create_directories(dataDir / "xvm");
    std::filesystem::create_directories(dataDir / "generations");

    // 复制 xvm-shim 到新 env 的 bin/
    auto shimSrc = p.dataDir / "bin" / "xvm-shim";
    if (std::filesystem::exists(shimSrc)) {
        std::filesystem::copy_file(shimSrc, dataDir / "bin" / "xvm-shim",
            std::filesystem::copy_options::overwrite_existing);
    }

    // 写入 .xlings.json
    if (!json.contains("envs")) json["envs"] = nlohmann::json::object();
    json["envs"][name] = {
        {"data", customData.empty() ? "" : customData.string()}
    };
    write_config_json_(configPath, json);

    log::info("[xlings:env] created environment '{}'", name);
    log::info("  data: {}", dataDir.string());
    return 0;
}
```

### 4.3 实现：`use_env()`

```cpp
int use_env(const std::string& name) {
    auto& p = Config::paths();
    auto configPath = p.homeDir / ".xlings.json";
    auto json = read_config_json_(configPath);

    if (!json.contains("envs") || !json["envs"].contains(name)) {
        log::error("[xlings:env] environment '{}' not found", name);
        log::error("  run: xlings env new {}", name);
        return 1;
    }

    json["activeEnv"] = name;
    write_config_json_(configPath, json);

    auto dataDir = Config::env_data_dir(name);
    log::info("[xlings:env] switched to '{}' (data: {})", name, dataDir.string());
    log::info("  note: restart shell or re-source profile to apply PATH changes");
    return 0;
}
```

### 4.4 实现：`list_envs()`

```cpp
std::vector<EnvInfo> list_envs() {
    auto& p = Config::paths();
    auto configPath = p.homeDir / ".xlings.json";
    auto json = read_config_json_(configPath);

    std::vector<EnvInfo> result;
    if (!json.contains("envs")) {
        // 至少返回 default
        result.push_back({"default", Config::env_data_dir("default"),
                          p.activeEnv == "default", 0});
        return result;
    }

    for (auto& [envName, _] : json["envs"].items()) {
        auto dataDir  = Config::env_data_dir(envName);
        int toolCount = 0;
        auto binDir   = dataDir / "bin";
        if (std::filesystem::exists(binDir)) {
            for (auto& e : std::filesystem::directory_iterator(binDir))
                if (e.path().filename() != "xvm-shim") ++toolCount;
        }
        result.push_back({envName, dataDir, p.activeEnv == envName, toolCount});
    }
    return result;
}
```

### 4.5 实现：`run()` 命令分发

```cpp
int run(int argc, char* argv[]) {
    if (argc < 3) {
        // 无子命令：默认显示列表
        return run_list_();
    }
    std::string sub = argv[2];
    if (sub == "new")     return (argc > 3) ? create_env(argv[3]) : (log::error("usage: xlings env new <name>"), 1);
    if (sub == "use")     return (argc > 3) ? use_env(argv[3])    : (log::error("usage: xlings env use <name>"), 1);
    if (sub == "list")    return run_list_();
    if (sub == "remove")  return (argc > 3) ? remove_env(argv[3]) : (log::error("usage: xlings env remove <name>"), 1);
    if (sub == "info")    return run_info_(argc > 3 ? argv[3] : "");
    if (sub == "rollback") {
        // 委托给 xlings.profile（T04 实现后接入）
        log::error("[xlings:env] rollback not yet implemented");
        return 1;
    }
    log::error("[xlings:env] unknown subcommand: {}", sub);
    return 1;
}
```

### 4.6 私有辅助函数

```cpp
namespace {  // 模块内部，不导出

nlohmann::json read_config_json_(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return nlohmann::json::object();
    try {
        auto content = xlings::utils::read_file_to_string(path.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        return json.is_discarded() ? nlohmann::json::object() : json;
    } catch (...) { return nlohmann::json::object(); }
}

void write_config_json_(const std::filesystem::path& path,
                        const nlohmann::json& json) {
    xlings::utils::write_string_to_file(path.string(), json.dump(2));
}

int run_list_() {
    auto envs = xlings::env::list_envs();
    std::println("[xlings:env] environments:");
    for (auto& e : envs) {
        std::println("  {}{}  ({}  tools: {})",
            e.isActive ? "* " : "  ",
            e.name, e.dataDir.string(), e.toolCount);
    }
    return 0;
}

int run_info_(const std::string& name) {
    auto& p  = xlings::Config::paths();
    auto envName = name.empty() ? p.activeEnv : name;
    auto info = xlings::env::get_env_info(envName);
    if (!info) {
        xlings::log::error("[xlings:env] environment '{}' not found", envName);
        return 1;
    }
    std::println("[xlings:env] info for '{}':", info->name);
    std::println("  active: {}", info->isActive);
    std::println("  data:   {}", info->dataDir.string());
    std::println("  tools:  {}", info->toolCount);
    return 0;
}

} // anonymous namespace
```

### 4.7 更新 `xmake.lua`

确保 `core/env.cppm` 在 `add_files` 中被包含（若使用 `core/**.cppm` 通配则自动包含，否则手动添加）：

```lua
target("xlings")
    add_files("core/main.cpp", "core/**.cppm")  -- ** 通配自动包含
```

---

## 5. 验收标准

### 5.1 env new

```bash
xlings env new work
# 期望输出:
# [xlings:env] created environment 'work'
#   data: ~/.xlings/envs/work

ls ~/.xlings/envs/work/
# 期望: bin/ xvm/ generations/
```

### 5.2 env list

```bash
xlings env list
# 期望输出:
# [xlings:env] environments:
# * default  (~/.xlings/envs/default  tools: N)
#   work     (~/.xlings/envs/work     tools: 0)
```

### 5.3 env use

```bash
xlings env use work
# 期望输出:
# [xlings:env] switched to 'work'...

xlings env list
# 期望:
#   default  (...)
# * work     (...)
```

### 5.4 env remove

```bash
xlings env use default   # 先切回 default
xlings env remove work
# 期望: work 的目录被删除，.xlings.json 中 envs.work 被移除

xlings env remove default   # 应报错
# 期望: [xlings:env] cannot remove the default environment
```

### 5.5 无效名称拒绝

```bash
xlings env new "my env"   # 空格
# 期望: [xlings:env] invalid env name: my env

xlings env new "abc/def"  # 斜杠
# 期望: 报错
```

### 5.6 编译无错误

```bash
xmake build xlings
```
