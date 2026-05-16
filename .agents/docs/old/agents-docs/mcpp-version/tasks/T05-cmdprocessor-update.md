# T05 — cmdprocessor.cppm: 新增 `env` 和 `store` 命令

> **Wave**: 4（依赖 T03 + T04）
> **预估改动**: ~40 行 C++，1 个文件

---

## 1. 任务概述

在 `core/cmdprocessor.cppm` 的 `create_processor()` 中新增两个顶层命令：

- `xlings env` → 委托给 `xlings::env::run()`（T03）
- `xlings store` → `store gc` 委托给 `xlings::profile::gc()`（T04）

同时在 `xself` 中补充 `migrate` 子命令入口（委托给 T08）。

**设计背景**: 详见 [../main.md §5.2](../main.md)

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T03 | 需要 `xlings::env::run()` 接口 |
| T04 | 需要 `xlings::profile::gc()` 接口 |

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/cmdprocessor.cppm` | 新增 import + 2 个命令注册 |

---

## 4. 实施步骤

### 4.1 新增 import 声明

在文件顶部 import 区域新增：

```cpp
import xlings.env;      // T03
import xlings.profile;  // T04
```

### 4.2 新增 `env` 命令

在 `create_processor()` 的 `.add(...)` 链末尾新增：

```cpp
.add("env",
    "manage isolated environments (new/use/list/remove/info/rollback)",
    [](int argc, char* argv[]) {
        return env::run(argc, argv);
    },
    "xlings env <new|use|list|remove|info|rollback> [name]")
```

### 4.3 新增 `store` 命令

```cpp
.add("store",
    "package store management (gc)",
    [](int argc, char* argv[]) {
        if (argc < 3) {
            std::println("Usage: xlings store <gc>");
            std::println("  gc [--dry-run]   remove unreferenced packages from xpkgs");
            return 1;
        }
        std::string sub = argv[2];
        if (sub == "gc") {
            bool dryRun = false;
            for (int i = 3; i < argc; ++i) {
                if (std::string(argv[i]) == "--dry-run") dryRun = true;
            }
            return profile::gc(Config::paths().homeDir, dryRun);
        }
        log::error("Unknown store subcommand: {}", sub);
        return 1;
    },
    "xlings store gc [--dry-run]")
```

### 4.4 完整的 `create_processor()` 结构（更新后）

```cpp
export CommandProcessor create_processor() {
    return CommandProcessor{}
        .add("install", ...)
        .add("remove",  ...)
        .add("update",  ...)
        .add("search",  ...)
        .add("use",     ...)
        .add("config",  ...)
        .add("env",     ...)   // ← 新增
        .add("store",   ...)   // ← 新增
        .add("self",    ...);
}
```

### 4.5 更新 `help` 展示中的路径信息（可选增强）

在 `print_help()` 中将 `Config::print_paths()` 扩展到显示 `XLINGS_PKGDIR` 和 `activeEnv`（T02 中已在 `print_paths()` 添加，此处自动生效）。

---

## 5. 验收标准

### 5.1 命令注册验证

```bash
xlings help
# 期望输出包含:
#   env          manage isolated environments (new/use/list/remove/info/rollback)
#   store        package store management (gc)
```

### 5.2 env 命令正常转发

```bash
xlings env list
# 期望: 调用 env::run() 并显示环境列表

xlings env new test-env
# 期望: 创建 test-env 环境

xlings env use test-env
# 期望: 切换到 test-env
```

### 5.3 store gc 命令正常转发

```bash
xlings store gc --dry-run
# 期望: 调用 profile::gc(homeDir, true)，显示可清理的包

xlings store gc
# 期望: 调用 profile::gc(homeDir, false)，执行清理
```

### 5.4 未知子命令报错

```bash
xlings store unknown
# 期望: Unknown store subcommand: unknown（非 panic，正常退出 1）

xlings env unknown
# 期望: env::run() 返回错误信息
```

### 5.5 编译无错误

```bash
xmake build xlings
```
