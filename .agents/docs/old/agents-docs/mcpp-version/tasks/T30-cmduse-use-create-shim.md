# T30 — cmd_use() 改用统一 create_shim()

> **Wave**: 依赖 T29
> **预估改动**: ~10 行修改
> **设计文档**: [../shim-optimization-design.md](../../shim-optimization-design.md)

---

## 1. 任务概述

将 `core/xvm/commands.cppm` 中 `cmd_use()` 的 hardlink/copy 逻辑替换为 `xself::create_shim()` 调用，消除重复的 shim 创建代码。

---

## 2. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/xvm/commands.cppm` | 修改：cmd_use() 中 shim 创建改用 create_shim() |

---

## 3. 实施步骤

### 3.1 添加 import

```cpp
import xlings.xself;  // for create_shim()
```

### 3.2 替换 cmd_use() 中的 shim 创建

将以下模式：
```cpp
fs::create_hard_link(xlings_bin, shim_path, ec);
if (ec) { fs::copy_file(...); }
```

替换为：
```cpp
xself::create_shim(xlings_bin, shim_path);
```

对主 shim 和所有 binding shim 均执行此替换。

---

## 4. 验收

```bash
xlings use gcc 15
ls -la ~/.xlings/subos/default/bin/gcc
# Linux/macOS: 应为 symlink
# Windows: 应为 hardlink 或 copy
```
