# T08 — xself.cppm: 新增 `migrate` 子命令

> **Wave**: 4（依赖 T02，与 T05 可并行）
> **预估改动**: ~80 行 C++，主要在 `xself.cppm`

---

## 1. 任务概述

在 `core/xself.cppm` 中新增 `migrate` 子命令，将旧版单目录数据结构（`$XLINGS_DATA/bin/`、`$XLINGS_DATA/xim/xpkgs/`、`$XLINGS_DATA/xvm/`）迁移到新版多环境结构（`$XLINGS_HOME/envs/default/`、`$XLINGS_HOME/xim/xpkgs/`），并生成初始 `generation 001.json`。

**设计背景**: 详见 [../env-store-design.md §9](../env-store-design.md)

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T02 | 需要新的 `Config::PathInfo`（`homeDir`、`pkgDir`、`env_data_dir()`） |

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/xself.cppm` | 新增 `cmd_migrate()` 实现 + `run()` 中的 dispatch |

---

## 4. 实施步骤

### 4.1 旧 → 新结构映射

```
旧结构（单 XLINGS_DATA）:           新结构（多环境）:
─────────────────────────────────    ──────────────────────────────────────────
$XLINGS_DATA/                        $XLINGS_HOME/
  bin/                         →       envs/default/bin/
  xim/                                 xim/
    xpkgs/                     →         xpkgs/           ← 全局共享
  xvm/                         →       envs/default/xvm/
    .workspace.xvm.yaml        →         .workspace.xvm.yaml
```

**迁移策略**：
- 文件/目录 **移动**（`fs::rename`，同文件系统内为原子操作；跨文件系统则 copy + remove）
- 旧路径保留**软链接**（Linux/macOS）或保留空目录（Windows），确保已有脚本不受影响
- 已是新结构时直接跳过（`envs/default/` 已存在）

### 4.2 新增 `cmd_migrate()` 实现

```cpp
static int cmd_migrate() {
    auto& p = Config::paths();
    auto homeDir    = p.homeDir;
    auto envsDefault = homeDir / "envs" / "default";

    // 检查是否已迁移
    if (fs::exists(envsDefault)) {
        std::println("[xlings:self]: already migrated (envs/default exists), skip");
        return 0;
    }

    std::println("[xlings:self]: starting migration to multi-env structure...");

    // 尝试推断旧 XLINGS_DATA 路径
    // 旧版：XLINGS_DATA 可能来自 .xlings.json 的 "data" 字段，或默认 $homeDir/data
    std::filesystem::path oldDataDir;
    auto configPath = homeDir / ".xlings.json";
    if (fs::exists(configPath)) {
        try {
            auto content = utils::read_file_to_string(configPath.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (!json.is_discarded() && json.contains("data") && json["data"].is_string()) {
                std::string customData = json["data"].get<std::string>();
                if (!customData.empty()) oldDataDir = customData;
            }
        } catch (...) {}
    }
    if (oldDataDir.empty()) {
        oldDataDir = homeDir / "data";  // 旧版默认路径
    }

    if (!fs::exists(oldDataDir)) {
        std::println("[xlings:self]: old data dir not found: {}", oldDataDir.string());
        std::println("  nothing to migrate, creating default env structure...");
        // 创建初始结构
        fs::create_directories(envsDefault / "bin");
        fs::create_directories(envsDefault / "xvm");
        fs::create_directories(envsDefault / "generations");
    } else {
        std::println("[xlings:self]: old data: {}", oldDataDir.string());

        // Step 1: 迁移 bin/ → envs/default/bin/
        move_or_link_(oldDataDir / "bin", envsDefault / "bin");

        // Step 2: 迁移 xim/xpkgs/ → xim/xpkgs/（提升到 homeDir 级别）
        auto oldPkgDir = oldDataDir / "xim" / "xpkgs";
        auto newPkgDir = homeDir / "xim" / "xpkgs";
        if (fs::exists(oldPkgDir) && !fs::exists(newPkgDir)) {
            fs::create_directories(homeDir / "xim");
            move_or_link_(oldPkgDir, newPkgDir);
        }

        // Step 3: 迁移 xvm/ → envs/default/xvm/
        move_or_link_(oldDataDir / "xvm", envsDefault / "xvm");

        // Step 4: 创建 generations/ 目录
        fs::create_directories(envsDefault / "generations");
    }

    // Step 5: 从现有 workspace.yaml 生成初始 generation 001.json
    generate_initial_generation_(envsDefault);

    // Step 6: 更新 .xlings.json（添加 activeEnv + envs 字段）
    update_config_for_multienv_(configPath);

    std::println("[xlings:self]: migration complete!");
    std::println("  envs/default: {}", envsDefault.string());
    std::println("  xim/xpkgs:   {}", (homeDir / "xim" / "xpkgs").string());
    return 0;
}
```

### 4.3 辅助函数：`move_or_link_()`

```cpp
static void move_or_link_(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src)) return;
    if (fs::exists(dst)) {
        std::println("[xlings:self]:   skip (already exists): {}", dst.string());
        return;
    }
    std::error_code ec;
    fs::rename(src, dst, ec);  // 原子移动（同文件系统）
    if (ec) {
        // 跨文件系统：copy + remove
        fs::copy(src, dst, fs::copy_options::recursive, ec);
        if (!ec) fs::remove_all(src);
    }
    if (ec) {
        std::println(stderr, "[xlings:self]: warning: failed to move {} -> {}: {}",
            src.string(), dst.string(), ec.message());
        return;
    }

    // 在原位置创建软链接（Linux/macOS）
#if !defined(_WIN32)
    std::error_code ec2;
    fs::create_symlink(dst, src, ec2);
    if (ec2) {
        std::println("[xlings:self]:   note: old path not linked: {}", src.string());
    }
#endif
    std::println("[xlings:self]:   moved: {} -> {}", src.string(), dst.string());
}
```

### 4.4 辅助函数：`generate_initial_generation_()`

```cpp
static void generate_initial_generation_(const fs::path& envDir) {
    auto yamlPath = envDir / "xvm" / ".workspace.xvm.yaml";
    std::map<std::string, std::string> packages;

    // 从现有 workspace.yaml 读取已激活的包版本
    if (fs::exists(yamlPath)) {
        try {
            auto content = utils::read_file_to_string(yamlPath.string());
            // 简单解析: "  name: version"
            std::istringstream ss(content);
            std::string line;
            bool inVersions = false;
            while (std::getline(ss, line)) {
                if (line == "versions:" || line == "versions:") { inVersions = true; continue; }
                if (inVersions && line.size() > 2 && line[0] == ' ') {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        auto k = line.substr(2, colon - 2);
                        auto v = line.substr(colon + 2);
                        // trim
                        k.erase(0, k.find_first_not_of(" \t"));
                        v.erase(0, v.find_first_not_of(" \t"));
                        v.erase(v.find_last_not_of(" \t\r") + 1);
                        if (!k.empty() && !v.empty()) packages[k] = v;
                    }
                } else if (inVersions && !line.empty() && line[0] != ' ') {
                    inVersions = false;
                }
            }
        } catch (...) {}
    }

    auto now   = std::chrono::system_clock::now();
    auto nowTT = std::chrono::system_clock::to_time_t(now);
    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTT));

    nlohmann::json genJson = {
        {"generation", 1},
        {"created",    timeBuf},
        {"reason",     "migrate from legacy structure"},
        {"packages",   packages}
    };
    auto genFile = envDir / "generations" / "001.json";
    utils::write_string_to_file(genFile.string(), genJson.dump(2));

    nlohmann::json profileJson = {
        {"current_generation", 1},
        {"packages",           packages}
    };
    utils::write_string_to_file((envDir / ".profile.json").string(),
                                profileJson.dump(2));

    std::println("[xlings:self]:   generated initial generation: {}",
        genFile.string());
}
```

### 4.5 更新 `run()` dispatch

在 `xself::run()` 中新增 `migrate` 分支：

```cpp
export int run(int argc, char* argv[]) {
    std::string action = (argc >= 3) ? argv[2] : "help";
    if (action == "init")    return cmd_init();
    if (action == "update")  return cmd_update();
    if (action == "config")  return cmd_config();
    if (action == "clean")   return cmd_clean();
    if (action == "migrate") return cmd_migrate();  // ← 新增
    return cmd_help();
}
```

### 4.6 更新 `cmd_help()` 帮助信息

```cpp
static int cmd_help() {
    std::println(R"(
xlings self [action]
  init      create home/data/bin/lib dirs
  update    git pull in XLINGS_HOME (if a repo)
  config    print paths
  clean     remove runtime cache
  migrate   migrate legacy single-env structure to multi-env structure
  help      this message
)");
    return 0;
}
```

---

## 5. 验收标准

### 5.1 基本迁移流程

```bash
# 模拟旧版数据结构
mkdir -p ~/.xlings/data/bin
mkdir -p ~/.xlings/data/xim/xpkgs/cmake/4.0.2
mkdir -p ~/.xlings/data/xvm
echo "versions:\n  cmake: 4.0.2" > ~/.xlings/data/xvm/.workspace.xvm.yaml

# 执行迁移
xlings self migrate
# 期望输出:
# [xlings:self]: starting migration to multi-env structure...
# [xlings:self]:   moved: ~/.xlings/data/bin -> ~/.xlings/envs/default/bin
# [xlings:self]:   moved: ~/.xlings/data/xim/xpkgs -> ~/.xlings/xim/xpkgs
# [xlings:self]:   moved: ~/.xlings/data/xvm -> ~/.xlings/envs/default/xvm
# [xlings:self]:   generated initial generation: ...
# [xlings:self]: migration complete!
```

### 5.2 迁移后目录结构验证

```bash
ls ~/.xlings/envs/default/
# 期望: bin/  xvm/  generations/  .profile.json

ls ~/.xlings/xim/xpkgs/
# 期望: cmake/

cat ~/.xlings/envs/default/generations/001.json
# 期望: {"packages": {"cmake": "4.0.2"}, "reason": "migrate from legacy structure", ...}
```

### 5.3 重复迁移幂等

```bash
xlings self migrate  # 第二次执行
# 期望: already migrated, skip
```

### 5.4 旧路径软链接（Linux/macOS）

```bash
ls -la ~/.xlings/data/bin
# 期望: 软链接指向 ~/.xlings/envs/default/bin
```

### 5.5 迁移后功能正常

```bash
# 迁移后，cmake 仍然可用
cmake --version   # 期望: xvm-shim 路由到 xpkgs/cmake/4.0.2/bin/cmake
```
