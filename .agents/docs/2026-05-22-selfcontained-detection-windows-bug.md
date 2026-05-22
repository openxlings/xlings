# Windows selfContained 检测误判 Bug 分析与修复

**日期**: 2026-05-22
**状态**: Fix in progress
**关联代码**: `src/core/config.cppm:400-418`
**影响版本**: 0.4.38 及之前所有版本
**影响平台**: Windows（Linux/macOS 不受影响）

---

## 1. 问题描述

在 Windows 上，当 xlings 从 `~/.xlings/subos/current/bin/xlings.exe`（PATH 中的标准位置）运行时，selfContained 检测误触发，导致 `homeDir` 被错误地设置为 `~/.xlings/subos/current/` 而不是 `~/.xlings/`。

**表现**：`xlings install <pkg> -y -g` 成功完成，但安装的包无法通过命令行找到（`The term 'xxx' is not recognized`）。

**发现场景**：mcpp 项目 CI（GitHub Actions Windows runner）中 `xlings install mcpp -y -g` 后 `mcpp --version` 报 command not found。

## 2. 根因分析

### 2.1 selfContained 检测逻辑

`config.cppm:404-418` 的 selfContained 检测用于支持"免安装/便携版"场景——解压发布包后直接运行，不需要 `self install`：

```cpp
auto exePath   = platform::get_executable_path();
auto exeParent = exePath.parent_path();          // bin/
auto candidate = exeParent.parent_path();        // 发布包根目录
auto hasRootConfig = fs::exists(candidate / ".xlings.json");
auto hasRootBin    = fs::exists(candidate / "bin" / "xlings.exe");
if (hasRootConfig && hasRootBin) {
    paths_.homeDir = candidate;       // ← 把发布包根目录当 home
    paths_.selfContained = true;
}
```

### 2.2 正常安装后的目录结构

```
~/.xlings/                          ← 真正的 homeDir
├── bin/xlings.exe                  ← 主 binary
├── .xlings.json                    ← home 配置（含 version, mirror, xim 等）
├── subos/
│   ├── default/
│   │   ├── bin/xlings.exe          ← shim (hardlink/copy on Windows)
│   │   └── .xlings.json           ← subos 配置 {"workspace":{}}
│   └── current → default          ← junction (mklink /J)
```

### 2.3 误判原因

从 `~/.xlings/subos/current/bin/xlings.exe` 运行时：

| 检测步骤 | 值 | 结果 |
|----------|-----|------|
| `exePath` | `~/.xlings/subos/current/bin/xlings.exe` | |
| `candidate` | `~/.xlings/subos/current/` | |
| `candidate/.xlings.json` | `{"workspace":{}}` (subos 配置) | ✅ 存在 |
| `candidate/bin/xlings.exe` | shim 自己 | ✅ 存在 |
| **判定** | **selfContained = true** | **❌ 误判** |

→ `homeDir` = `~/.xlings/subos/current/`（错误）

### 2.4 为什么只影响 Windows

- **Linux/macOS**: shim 通过 **symlink** 创建。`get_executable_path()` 解析 symlink 返回真实路径 `~/.xlings/bin/xlings`，candidate 为 `~/.xlings`（正确的 home）
- **Windows**: 不支持 symlink（需管理员权限），shim 通过 **hardlink 或 copy** 创建。`get_executable_path()` 返回 shim 自身路径，candidate 为 `~/.xlings/subos/current/`（subos 目录）

### 2.5 影响链

homeDir 错误导致一系列连锁问题：

1. `dataDir` = `subos/current/data`（不是 `~/.xlings/data`）
2. 读取的配置是 subos 的 `{"workspace":{}}`，丢失所有全局配置
3. versions/workspace 写入位置错误
4. shim 创建路径错误——新安装的包在 PATH 中找不到

## 3. 修复方案

### 方案 A（采用）：`if constexpr` 编译期分支 + `.xlings.json` 内容双字段检测

Linux/macOS 上 shim 是 symlink，不存在此问题，额外的 JSON 解析是不必要的开销。
使用 `if constexpr (platform::OS_NAME == "windows")` 做编译期分支，仅在
Windows 上执行内容检查。同时消除了 `#ifdef _WIN32` 宏，将平台差异收敛到
`platform` 模块的 `OS_NAME` 常量。

真正的 home `.xlings.json` 同时包含 `version` 和 `activeSubos` 字段（由
`self install` 写入），而 subos 的 `.xlings.json` 只有 `{"workspace":{}}`。
双字段检查避免了未来 subos config 增加单个字段导致再次误判。

```cpp
constexpr auto kBinName = (platform::OS_NAME == "windows")
    ? "bin/xlings.exe" : "bin/xlings";
auto hasRootBin = !exePath.empty() && fs::exists(candidate / kBinName);

bool isSelfContained = hasRootConfig && hasRootBin;
if constexpr (platform::OS_NAME == "windows") {
    if (isSelfContained) {
        try {
            auto cfg = platform::read_file_to_string(
                (candidate / ".xlings.json").string());
            auto j = nlohmann::json::parse(cfg, nullptr, false);
            isSelfContained = !j.is_discarded()
                && j.contains("version")
                && j.contains("activeSubos");
        } catch (...) { isSelfContained = false; }
    }
}
```

**优点**：
- Linux/macOS 零开销（编译期排除）
- 双字段检查更健壮，不易被未来 schema 变更击穿
- 消除 `#ifdef` 宏，平台差异由 `platform::OS_NAME` 统一管理

### 备选方案（未采用）

- **方案 B**：排除路径中包含 `subos/` 的 candidate — 依赖路径命名约定，脆弱
- **方案 C**：用标记文件 `.xlings-selfcontained` — 需要修改发布包构建流程，改动大
- **方案 D**：Windows shim 中嵌入原始路径 — 改动 shim 机制，影响面大

## 4. 验证

修复后需验证：
1. Windows CI：`xlings install <pkg> -y -g` 后包可正常运行
2. selfContained 模式：从解压目录直接运行仍然正常
3. Linux/macOS：行为无变化
