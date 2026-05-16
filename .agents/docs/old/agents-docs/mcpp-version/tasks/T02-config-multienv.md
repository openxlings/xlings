# T02 — config.cppm: 多环境路径重构

> **Wave**: 2（依赖 T01）
> **预估改动**: ~60 行 C++，1 个文件

---

## 1. 任务概述

重构 `core/config.cppm` 的路径解析逻辑，新增三项能力：

1. **自包含检测**：通过 `platform::get_executable_path()` 检测是否处于自包含模式
2. **activeEnv 路由**：`XLINGS_DATA` 改为 `$XLINGS_HOME/envs/<activeEnv>`
3. **XLINGS_PKGDIR 设置**：全局共享 store 路径 `$XLINGS_HOME/xim/xpkgs`

这是多环境功能的核心基础，T03/T04/T08 均依赖本任务建立的 `PathInfo` 结构。

**设计背景**: 详见 [../main.md §4](../main.md) 和 [../env-store-design.md §4](../env-store-design.md)

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T01 | 需要 `platform::get_executable_path()` 实现自包含检测 |

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/config.cppm` | 重构路径解析，扩展 `PathInfo`，新增自包含检测 |
| `core/main.cpp` | 新增 `XLINGS_PKGDIR` 环境变量设置 |

---

## 4. 实施步骤

### 4.1 扩展 `PathInfo` 结构体

```cpp
export class Config {
public:
    struct PathInfo {
        std::filesystem::path homeDir;    // XLINGS_HOME (~/.xlings)
        std::filesystem::path dataDir;    // 当前环境数据目录 ($homeDir/envs/<activeEnv>)
        std::filesystem::path binDir;     // $dataDir/bin
        std::filesystem::path libDir;     // $dataDir/lib（暂保留兼容）
        std::filesystem::path pkgDir;     // 全局共享 xpkgs ($homeDir/xim/xpkgs)
        std::string           activeEnv;  // 当前激活环境名（默认 "default"）
        bool                  selfContained = false;  // 是否自包含模式
    };
```

### 4.2 重构 `Config()` 构造函数

完整实现：

```cpp
Config() {
    // ── Step 1: 解析 XLINGS_HOME ──────────────────────────────────
    auto envHome = utils::get_env_or_default("XLINGS_HOME");
    if (!envHome.empty()) {
        paths_.homeDir = envHome;
    } else {
        // 自包含检测：可执行文件同级的上一层目录有 xim/ 目录
        auto exePath   = platform::get_executable_path();  // T01 提供
        auto exeParent = exePath.parent_path();            // bin/
        auto candidate = exeParent.parent_path();          // xlings-x.y.z/
        if (!exePath.empty() && std::filesystem::exists(candidate / "xim")) {
            paths_.homeDir      = candidate;
            paths_.selfContained = true;
        } else {
            paths_.homeDir = std::filesystem::path(platform::get_home_dir()) / ".xlings";
        }
    }

    // ── Step 2: 读取 .xlings.json ─────────────────────────────────
    std::string activeEnv  = "default";
    std::string customData;
    auto configPath = paths_.homeDir / ".xlings.json";
    if (std::filesystem::exists(configPath)) {
        try {
            auto content = utils::read_file_to_string(configPath.string());
            auto json    = nlohmann::json::parse(content, nullptr, false);
            if (!json.is_discarded()) {
                // activeEnv 字段
                if (json.contains("activeEnv") && json["activeEnv"].is_string())
                    activeEnv = json["activeEnv"].get<std::string>();
                // 旧版 data 字段兼容
                if (json.contains("data") && json["data"].is_string())
                    customData = json["data"].get<std::string>();
                // mirror / lang
                if (json.contains("mirror") && json["mirror"].is_string())
                    mirror_ = json["mirror"].get<std::string>();
                if (json.contains("lang") && json["lang"].is_string())
                    lang_ = json["lang"].get<std::string>();
            }
        } catch (...) {}
    }

    // ── Step 3: 解析 XLINGS_DATA ──────────────────────────────────
    paths_.activeEnv = activeEnv;
    auto envData = utils::get_env_or_default("XLINGS_DATA");
    if (!envData.empty()) {
        // 最高优先级：显式环境变量（绕过环境系统，直接指定）
        paths_.dataDir = envData;
    } else if (!customData.empty()) {
        // 旧版兼容：.xlings.json 中的 "data" 字段
        paths_.dataDir = customData;
    } else {
        // 多环境模式：$homeDir/envs/<activeEnv>
        paths_.dataDir = paths_.homeDir / "envs" / activeEnv;
    }

    // ── Step 4: 派生路径 ─────────────────────────────────────────
    paths_.binDir = paths_.dataDir / "bin";
    paths_.libDir = paths_.dataDir / "lib";

    // ── Step 5: 全局共享 xpkgs 路径 ──────────────────────────────
    paths_.pkgDir = paths_.homeDir / "xim" / "xpkgs";
}
```

### 4.3 新增工具函数（可选但推荐）

```cpp
public:
    // 获取指定环境的 data 目录
    static std::filesystem::path env_data_dir(const std::string& envName) {
        return instance_().paths_.homeDir / "envs" / envName;
    }

    // 获取所有环境列表（扫描 envs/ 目录）
    static std::vector<std::string> list_env_names() {
        std::vector<std::string> names;
        auto envsDir = instance_().paths_.homeDir / "envs";
        if (!std::filesystem::exists(envsDir)) return names;
        for (auto& entry : std::filesystem::directory_iterator(envsDir)) {
            if (entry.is_directory())
                names.push_back(entry.path().filename().string());
        }
        return names;
    }
```

### 4.4 修改 `main.cpp`：设置 `XLINGS_PKGDIR`

```cpp
int main(int argc, char* argv[]) {
    auto& p = xlings::Config::paths();
    xlings::platform::set_env_variable("XLINGS_HOME", p.homeDir.string());
    xlings::platform::set_env_variable("XLINGS_DATA", p.dataDir.string());
    // 新增：全局共享 xpkgs 路径（供 xim Lua 的 get_xim_install_basedir() 读取）
    xlings::platform::set_env_variable("XLINGS_PKGDIR", p.pkgDir.string());
    auto processor = xlings::cmdprocessor::create_processor();
    return processor.run(argc, argv);
}
```

### 4.5 更新 `print_paths()`

```cpp
static void print_paths() {
    auto& p = paths();
    std::println("XLINGS_HOME:     {}", p.homeDir.string());
    std::println("XLINGS_DATA:     {}", p.dataDir.string());
    std::println("XLINGS_PKGDIR:   {}", p.pkgDir.string());
    std::println("  activeEnv:     {}", p.activeEnv);
    std::println("  selfContained: {}", p.selfContained);
    std::println("  bin:           {}", p.binDir.string());
}
```

---

## 5. .xlings.json 新格式支持

本任务需能正确解析新版配置文件：

```json
{
  "version": "0.2.0",
  "activeEnv": "default",
  "mirror": "",
  "lang": "auto",
  "data": "",
  "envs": {
    "default": { "data": "" },
    "work": { "data": "" }
  }
}
```

当 `envs.<name>.data` 非空时，该 env 使用自定义路径（未来 T03 `env new --data <path>` 时写入）。当前任务只需正确读取 `activeEnv` 字段。

---

## 6. 验收标准

### 6.1 默认模式路径验证

```bash
# 无环境变量、无 .xlings.json（全新安装）
xlings config paths
# 期望:
# XLINGS_HOME:     /home/user/.xlings
# XLINGS_DATA:     /home/user/.xlings/envs/default
# XLINGS_PKGDIR:   /home/user/.xlings/xim/xpkgs
# activeEnv:       default
# selfContained:   false
```

### 6.2 自包含模式验证

```bash
# 解压自包含发布包
tar -xzf xlings-0.2.0-linux-x86_64.tar.gz
./xlings-0.2.0-linux-x86_64/bin/xlings config paths
# 期望:
# XLINGS_HOME:     /path/to/xlings-0.2.0-linux-x86_64
# selfContained:   true
```

### 6.3 XLINGS_HOME 环境变量覆盖

```bash
XLINGS_HOME=/opt/myxlings xlings config paths
# 期望: XLINGS_HOME: /opt/myxlings
```

### 6.4 activeEnv 切换后路径变化

```bash
# .xlings.json 中 activeEnv 改为 "work"
xlings config paths
# 期望: XLINGS_DATA: /home/user/.xlings/envs/work
```

### 6.5 旧版 data 字段兼容

```bash
# .xlings.json 中有 "data": "/custom/path"，无 "activeEnv"
xlings config paths
# 期望: XLINGS_DATA: /custom/path（旧版行为不变）
```

### 6.6 编译无错误

```bash
xmake build xlings
```
