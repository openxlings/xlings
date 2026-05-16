# T28 - Windows 平台适配（MSVC 编译修复 + CI E2E 脚本化）

> 父文档: [../README.md](../README.md) | 分支: `feat/xvm-cpp-integration`

## 1. 背景

xlings C++ 二进制在 Windows（MSVC）上编译失败，原因是 C++20 模块分区的重复导入问题。同时，Windows CI 工作流中的 E2E 测试以内联 PowerShell 形式写在 YAML 中，与 macOS/Linux 已经采用的独立脚本模式不一致，不利于维护和复用。

本任务修复编译错误，并将 Windows CI/E2E 对齐到 macOS 的模式（编号 E2E 阶段 + 可复用脚本）。

## 2. 问题分析

### 2.1 MSVC 编译失败根因

`core/xself.cppm`（第 8-12 行）存在**重复的分区导入**：

```cpp
import :init;            // 第 8 行 — 非导出导入
import :install;         // 第 9 行 — 非导出导入

export import :install;  // 第 11 行 — 重新导出
export import :init;     // 第 12 行 — 重新导出
```

`export import :X;` 本身已经同时完成了导入和导出。对同一个分区同时写 `import :X;` 和 `export import :X;` 会导致 MSVC 无法正确暴露从 `:init` 分区导出的符号（`is_builtin_shim`、`ensure_subos_shims`），使得 `subos.cppm` 等消费模块编译失败。

### 2.2 Windows CI 现状 vs 目标

| 方面 | macOS/Linux（当前） | Windows（当前） | Windows（目标） |
|------|---------------------|----------------|-----------------|
| E2E 测试 | 独立 `.sh` 脚本 | 内联在 CI YAML 中 | 独立 `.ps1` 脚本 |
| E2E 编号 | E2E-01 ~ E2E-05 | 混合内联步骤 | E2E-01 ~ E2E-04 |
| 测试工具库 | `release_test_lib.sh` | 无 | `release_test_lib.ps1` |
| 发布脚本 | `macos_release.sh` | `windows_release.ps1` | 保持不变（已正确） |

## 3. 改动清单

### 3.1 修改的文件

| 文件 | 改动说明 |
|------|---------|
| `core/xself.cppm` | 删除第 8-9 行重复的 `import :init;` 和 `import :install;`（2 行删除） |
| `.github/workflows/xlings-ci-windows.yml` | 重构为编号 E2E 阶段，调用独立 `.ps1` 脚本 |

### 3.2 新增的文件

| 文件 | 说明 |
|------|------|
| `tests/e2e/release_test_lib.ps1` | 共享 PowerShell 测试工具函数 |
| `tests/e2e/bootstrap_home_test.ps1` | E2E-01：便携模式、移动后验证、self install |
| `tests/e2e/release_self_install_test.ps1` | E2E-02：解压发布包 → self install → 验证 shim 和路径 |
| `tests/e2e/release_subos_smoke_test.ps1` | E2E-04：创建 subos s1/s2 → 安装 d2x → 切换验证隔离 |

### 3.3 未修改的文件（不影响其他平台）

- `core/self/init.cppm` — 无需改动
- `core/subos.cppm` — 无需改动
- `xmake.lua` — 无需改动（MSVC 已是 Windows 默认）
- `tools/windows_release.ps1` — 已正确
- 所有 macOS/Linux 脚本 — 未触碰

## 4. 实现细节

### 4.1 xself.cppm 修复

```cpp
// 修复前:
import std;
import :init;
import :install;

export import :install;
export import :init;

// 修复后:
import std;

export import :init;
export import :install;
```

**跨平台安全性**：`export import :X;` 已提供与 `import :X; export import :X;` 相同的语义 — 既导入到当前翻译单元，又重新导出给消费者。Clang 和 GCC 不受影响。

### 4.2 PowerShell 测试工具库 (`release_test_lib.ps1`)

提供以下共享函数，对应 bash 版本的 `release_test_lib.sh`：

| 函数 | 说明 |
|------|------|
| `Log($msg)` | 输出带前缀的日志 |
| `Fail($msg)` | 输出错误并退出 |
| `Get-MinimalSystemPath` | 返回最小 Windows 系统 PATH |
| `Require-ReleaseArchive($path)` | 验证发布包存在 |
| `Expand-ReleaseArchive($path, $name)` | 解压 zip 并返回包目录路径 |
| `Get-DefaultD2xVersion` | Windows 返回 `"0.1.1"` |
| `Write-FixtureReleaseConfig($pkgDir)` | 写入测试用 `.xlings.json` |
| `Require-FixtureIndex` | 准备 fixture index 仓库 |

### 4.3 Windows CI 工作流结构

```
Phase 1: 配置 + 构建
  ├─ Install xmake
  ├─ Configure xmake (MSVC 默认)
  └─ Build and run unit tests

Phase 2: 发布构建
  ├─ Build (windows_release.ps1)
  ├─ E2E-01: Bootstrap Home
  └─ Prepare release artifact

Phase 3: E2E 测试
  ├─ E2E-02: Release Self Install
  ├─ E2E-03: Quick Install Smoke (continue-on-error)
  └─ E2E-04: Release Subos + d2x
```

Windows 与 macOS CI 的关键差异：
- 无需 LLVM 引导（使用默认 MSVC）
- 使用 `.zip` 而非 `.tar.gz`
- 使用 junction 而非 symlink（已在平台代码中处理）
- 无需 `xattr` 处理

## 5. 验证

1. **编译修复验证**：移除 `xself.cppm` 中的重复导入后，在所有平台上执行 `xmake build xlings` 和 `xmake build xlings_tests` 均应通过。
2. **E2E 测试验证**：运行 Windows CI 工作流，验证所有 E2E 阶段通过。
3. **跨平台回归**：macOS/Linux CI 不受影响（未修改任何 bash 脚本或非 Windows 工作流）。
