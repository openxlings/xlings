# xlings 日志系统改进设计方案

## Context

xlings 项目已有结构化日志模块 (`log.cppm`) 和 `--verbose`/`--quiet` 标志，但代码中约 187 处 `std::print/println` 绕过了日志系统直接输出。导致：
- 默认输出过于冗杂，状态信息和用户输出混在一起
- `--quiet` 无法抑制非必要输出
- 错误信息写到 stdout 而非 stderr
- 缺少关键调试信息，`--verbose` 时可用信息不足

## 设计原则

**日志模块核心关注两个层面：信息（Info）和调试（Debug）。**

用户交互/展示类的 `std::println`（如帮助文本、搜索结果、列表、交互提示）**保持不变，加 `// TODO(tui)` 备注**，留给后续 TUI 模块统一设计。

本次改动聚焦：
1. 将**操作状态/进度**消息纳入 `log::info` 管控（默认显示，`--quiet` 时抑制）
2. 将**内部细节**降级/新增为 `log::debug`（仅 `--verbose` 时显示）
3. 将 **stderr 错误**统一为 `log::error`
4. `log::error()`/`log::warn()` 改写 stderr（修正当前写 stdout 的问题）
5. 新增 `log::println()` — 受日志级别控制但无前缀的干净输出

### 日志级别行为

| 模式 | 可见输出 |
|------|---------|
| 默认 (Info) | `std::println`(用户交互/结果, TODO:TUI) + `log::println`(状态) + `log::info`(操作进度) + `log::warn` + `log::error` |
| `--quiet` (Error) | `std::println`(用户交互/结果) + `log::error` |
| `--verbose` (Debug) | 以上全部 + `log::debug`(路径解析、配置细节、版本匹配等) |

### 跨平台注意事项

- `stderr` 宏来自 `<cstdio>`，`log.cppm` 已有 `#include <cstdio>`
- `std::print(stderr, ...)` / `std::println(stderr, ...)` 在 C++23 中是标准 API（`<print>` header），通过 `import std` 可用
- Windows 上 `stderr` 同样可用（MSVC / MinGW 均支持）
- 转换 `std::println(stderr, ...)` → `log::error(...)` 后，原文件中不再直接使用 `stderr`，可移除 `#include <cstdio>`（前提是该文件无其他 C 头依赖）

---

## 实现步骤

### Step 1: 增强 `core/log.cppm`

**1a. 新增 `log::println()` 和 `log::print()`**

无前缀标签、受日志级别控制的输出，用于**干净的操作状态消息**。

```cpp
export template<typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Info) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        std::println("{}", msg);
        write_to_file_("[status] ", msg);
    }
}

export template<typename... Args>
void print(std::format_string<Args...> fmt, Args&&... args) {
    if (gLevel_ <= Level::Info) {
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        std::print("{}", msg);
    }
}
```

**1b. `log::error()` 和 `log::warn()` 改写 stderr**

```cpp
// error(): std::print(...) → std::print(stderr, ...)
// warn():  同上
```

### Step 2: 转换 `core/xvm/shim.cppm`

所有 `std::println(stderr, ...)` → `log::error(...)`

新增调试信息：
- shim 入口：`log::debug("shim dispatch: program={}, depth={}", ...)`
- 版本解析：`log::debug("resolved version: {} -> {}", ...)`
- 路径解析：`log::debug("exe path: {}", ...)`
- binding 查找：`log::debug("binding found: {} -> {} (via {})", ...)`

### Step 3: 转换 `core/xvm/commands.cppm`

- stderr 错误 → `log::error()`
- 状态输出 → `log::println()`
- 降级 `log::info("[xvm] overwriting header")` → `log::debug`

### Step 4: 转换 `core/cli.cppm`

- legacy config 状态消息 → `log::println()`
- 帮助/版本/usage → 保留 + `// TODO(tui)`

### Step 5: 转换 `core/xim/commands.cppm`

- 安装状态 → `log::println()`
- 交互提示/搜索结果/列表 → 保留 + `// TODO(tui)`

### Step 6: 转换 `core/self/install.cppm`

- 新增 `import xlings.log;`
- 调试信息（candidate 等）→ `log::debug()`
- stderr 错误 → `log::error()`
- 状态消息 → `log::println()`

### Step 7: 转换 `core/subos.cppm` + `core/self/init.cppm`

- 操作确认消息 → `log::println()`
- stderr 错误 → `log::error()`
- 列表/info 显示 → 保留 + `// TODO(tui)`

### Step 8: 降级现有 `log::info()` 调用

以下 `log::info()` 降为 `log::debug()`，减少默认噪音：
- `core/xim/index.cppm`: index building/loading 详情
- `core/xim/repo.cppm`: local repo link 信息
- `core/xim/downloader.cppm`: clone pull 细节
- `core/xvm/commands.cppm`: header overwrite

### Step 9: 补充关键 `log::debug()` 调试信息

在以下位置添加 debug 日志（`--verbose` 可见）：
1. `core/config.cppm` — 配置加载路径
2. `core/xvm/shim.cppm` — shim 调度流程
3. `core/xim/commands.cppm` — 依赖解析结果
4. `core/xim/installer.cppm` — 安装各阶段详细路径
5. `core/xim/catalog.cppm` — catalog rebuild 来源

---

## 涉及文件

- `core/log.cppm` — 核心改动
- `core/cli.cppm`
- `core/xim/commands.cppm`
- `core/xim/index.cppm`
- `core/xim/repo.cppm`
- `core/xim/downloader.cppm`
- `core/xvm/commands.cppm`
- `core/xvm/shim.cppm`
- `core/subos.cppm`
- `core/self/install.cppm`
- `core/self/init.cppm`
