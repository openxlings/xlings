# T04 — 新建 `core/profile.cppm`：世代管理 + GC

> **Wave**: 3（依赖 T02，与 T03 可并行）
> **预估改动**: ~150 行 C++，1 个新文件

---

## 1. 任务概述

新建 `core/profile.cppm` 模块，实现：

1. **世代管理**：每次安装/删除包后追加 `generations/0XX.json`，同步 `xvm/.workspace.xvm.yaml`
2. **回滚**：切换到任意历史世代（仅改 yaml 文件，shim 无需动）
3. **GC**：扫描所有环境的引用集，删除 `xpkgs` 中无任何引用的包版本

**设计背景**: 详见 [../env-store-design.md §3.3-3.4、§6-7](../env-store-design.md)

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T02 | 需要 `Config::PathInfo`（含 `homeDir`、`pkgDir`、`env_data_dir()`、`list_env_names()`） |

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/profile.cppm` | 新建模块 |
| `xmake.lua` | 将 `core/profile.cppm` 加入构建（通配符自动包含则无需手动添加） |

---

## 4. 实施步骤

### 4.1 模块接口

```cpp
export module xlings.profile;

import std;
import xlings.config;
import xlings.json;
import xlings.log;

export namespace xlings::profile {

struct Generation {
    int                                    number;
    std::string                            created;   // ISO 8601 UTC
    std::string                            reason;    // 操作描述（如 "install dadk@0.4.0"）
    std::map<std::string, std::string>     packages;  // name → version
};

// 读取当前世代
Generation load_current(const std::filesystem::path& envDir);

// 提交新世代（安装/删除后调用），同步 workspace.yaml
// packages: 完整的 name→version 快照（非差量）
// reason:   "install dadk@0.4.0" / "remove gcc@15.0" 等
int commit(const std::filesystem::path& envDir,
           std::map<std::string, std::string> packages,
           const std::string& reason);

// 列出所有历史世代（按编号升序）
std::vector<Generation> list_generations(const std::filesystem::path& envDir);

// 回滚到第 N 世代（0 = 空世代）
int rollback(const std::filesystem::path& envDir, int targetGen);

// GC: 扫描所有 envs 的引用，删除 xpkgs 中孤立的版本目录
// dryRun = true 时只打印，不删除
int gc(const std::filesystem::path& xlingHome, bool dryRun = false);

} // namespace xlings::profile
```

### 4.2 实现：`commit()`

```cpp
int commit(const std::filesystem::path& envDir,
           std::map<std::string, std::string> packages,
           const std::string& reason) {
    auto gensDir = envDir / "generations";
    std::filesystem::create_directories(gensDir);

    // 确定下一世代编号
    int nextGen = 1;
    for (auto& entry : std::filesystem::directory_iterator(gensDir)) {
        auto stem = entry.path().stem().string();
        try { nextGen = std::max(nextGen, std::stoi(stem) + 1); }
        catch (...) {}
    }

    // 获取当前 UTC 时间
    auto now   = std::chrono::system_clock::now();
    auto nowTT = std::chrono::system_clock::to_time_t(now);
    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTT));

    // 写世代文件
    nlohmann::json genJson = {
        {"generation", nextGen},
        {"created",    timeBuf},
        {"reason",     reason},
        {"packages",   packages}
    };
    auto genFile = gensDir / (std::format("{:03d}", nextGen) + ".json");
    utils::write_string_to_file(genFile.string(), genJson.dump(2));

    // 更新 .profile.json
    nlohmann::json profileJson = {
        {"current_generation", nextGen},
        {"packages",           packages}
    };
    utils::write_string_to_file((envDir / ".profile.json").string(),
                                profileJson.dump(2));

    // 同步 xvm/.workspace.xvm.yaml
    sync_workspace_yaml_(envDir, packages);

    return 0;
}
```

### 4.3 实现：`sync_workspace_yaml_()`（内部函数）

将 profile 的 packages 写入 xvm 能读取的 YAML 格式：

```cpp
void sync_workspace_yaml_(const std::filesystem::path& envDir,
                          const std::map<std::string, std::string>& packages) {
    auto xvmDir = envDir / "xvm";
    std::filesystem::create_directories(xvmDir);
    auto yamlPath = xvmDir / ".workspace.xvm.yaml";

    // 手写简单 YAML（避免引入 yaml 依赖）
    // 格式: versions:\n  cmake: 4.0.2\n  dadk: 0.4.0\n
    std::string yaml = "versions:\n";
    for (auto& [name, ver] : packages) {
        yaml += "  " + name + ": " + ver + "\n";
    }
    utils::write_string_to_file(yamlPath.string(), yaml);
}
```

> **注意**: xvm 的 workspace.yaml 实际上可能有更复杂的结构（含 `path`、`envs` 等字段），此处仅更新 `versions` 字段。若现有 yaml 已有其他字段（path、envs），应保留它们，只更新 `versions` 下对应的版本号。实现时需先读取现有 yaml，然后 patch `versions` 字段。

### 4.4 实现：`rollback()`

```cpp
int rollback(const std::filesystem::path& envDir, int targetGen) {
    auto gensDir = envDir / "generations";

    std::map<std::string, std::string> packages;
    if (targetGen == 0) {
        // 回滚到空世代（无任何包）
        packages = {};
    } else {
        auto genFile = gensDir / (std::format("{:03d}", targetGen) + ".json");
        if (!std::filesystem::exists(genFile)) {
            log::error("[xlings:profile] generation {} not found", targetGen);
            return 1;
        }
        try {
            auto content = utils::read_file_to_string(genFile.string());
            auto json = nlohmann::json::parse(content);
            for (auto& [k, v] : json["packages"].items())
                packages[k] = v.get<std::string>();
        } catch (...) {
            log::error("[xlings:profile] failed to read generation {}", targetGen);
            return 1;
        }
    }

    // 同步 workspace.yaml
    sync_workspace_yaml_(envDir, packages);

    // 更新 .profile.json
    nlohmann::json profileJson = {
        {"current_generation", targetGen},
        {"packages",           packages}
    };
    utils::write_string_to_file((envDir / ".profile.json").string(),
                                profileJson.dump(2));

    log::info("[xlings:profile] rolled back to generation {}", targetGen);
    return 0;
}
```

### 4.5 实现：`gc()`

```cpp
int gc(const std::filesystem::path& xlingHome, bool dryRun) {
    // 收集所有环境对 name@version 的引用
    std::set<std::string> referenced;  // "gcc@15.0", "cmake@4.0.2"

    auto envsDir = xlingHome / "envs";
    if (std::filesystem::exists(envsDir)) {
        for (auto& envEntry : std::filesystem::directory_iterator(envsDir)) {
            if (!envEntry.is_directory()) continue;
            // 扫描所有 generations/*.json
            auto gensDir = envEntry.path() / "generations";
            if (!std::filesystem::exists(gensDir)) continue;
            for (auto& genEntry : std::filesystem::directory_iterator(gensDir)) {
                try {
                    auto content = utils::read_file_to_string(genEntry.path().string());
                    auto json = nlohmann::json::parse(content);
                    for (auto& [k, v] : json["packages"].items())
                        referenced.insert(k + "@" + v.get<std::string>());
                } catch (...) {}
            }
        }
    }

    // 遍历 xpkgs/<name>/<version>/，找出未被引用的
    auto pkgDir = xlingHome / "xim" / "xpkgs";
    if (!std::filesystem::exists(pkgDir)) {
        log::info("[xlings:store] xpkgs not found, nothing to gc");
        return 0;
    }

    std::uintmax_t freedBytes = 0;
    int removedCount = 0;
    for (auto& pkgEntry : std::filesystem::directory_iterator(pkgDir)) {
        if (!pkgEntry.is_directory()) continue;
        auto pkgName = pkgEntry.path().filename().string();
        for (auto& verEntry : std::filesystem::directory_iterator(pkgEntry)) {
            if (!verEntry.is_directory()) continue;
            auto ver = verEntry.path().filename().string();
            auto key = pkgName + "@" + ver;
            if (!referenced.count(key)) {
                auto size = dir_size_(verEntry.path());
                if (dryRun) {
                    std::println("  would remove xpkgs/{}/{} ({:.1f} MB)",
                        pkgName, ver, size / 1e6);
                } else {
                    std::filesystem::remove_all(verEntry.path());
                    log::info("[xlings:store] removed xpkgs/{}/{}", pkgName, ver);
                }
                freedBytes += size;
                ++removedCount;
            }
        }
    }

    if (dryRun) {
        std::println("[xlings:store] gc dry-run: {} packages, {:.1f} MB would be freed",
            removedCount, freedBytes / 1e6);
    } else {
        std::println("[xlings:store] gc: {} packages removed, {:.1f} MB freed",
            removedCount, freedBytes / 1e6);
    }
    return 0;
}
```

---

## 5. 世代文件格式

```json
// envs/default/generations/001.json
{
  "generation": 1,
  "created": "2026-02-23T14:30:00Z",
  "reason": "init",
  "packages": {}
}

// envs/default/generations/042.json
{
  "generation": 42,
  "created": "2026-02-23T16:45:00Z",
  "reason": "install dadk@0.4.0",
  "packages": {
    "cmake": "4.0.2",
    "dadk":  "0.4.0",
    "node":  "22.17.1"
  }
}
```

---

## 6. 验收标准

### 6.1 commit() 创建世代文件

```bash
xlings install cmake   # 触发 C++ 侧调用 profile::commit()

ls ~/.xlings/envs/default/generations/
# 期望: 001.json  002.json ...

cat ~/.xlings/envs/default/generations/002.json
# 期望: reason = "install cmake@4.0.2", packages.cmake = "4.0.2"

cat ~/.xlings/envs/default/xvm/.workspace.xvm.yaml
# 期望: versions:\n  cmake: 4.0.2
```

### 6.2 rollback()

```bash
xlings env rollback --to 1
# 期望: workspace.yaml 中不再有 cmake（回到初始世代）

cmake --version   # 期望: 找不到命令（xvm-shim 路由失败，cmake 不在激活列表）
```

### 6.3 gc() dry-run

```bash
xlings store gc --dry-run
# 期望: 列出 xpkgs 中未被任何 env 引用的包版本（若有）
```

### 6.4 gc() 实际执行

```bash
# 先创建一个孤立版本（只在 xpkgs 中，不在任何 generation 里）
mkdir -p ~/.xlings/xim/xpkgs/dummy-pkg/1.0.0/

xlings store gc
# 期望: removed xpkgs/dummy-pkg/1.0.0, freed ...

ls ~/.xlings/xim/xpkgs/dummy-pkg/  # 期望: 目录不存在
```

### 6.5 编译无错误

```bash
xmake build xlings
```
