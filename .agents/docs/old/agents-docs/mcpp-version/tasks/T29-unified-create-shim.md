# T29 — 统一 create_shim() 函数：symlink 优先策略

> **Wave**: 独立
> **预估改动**: ~30 行新增，~20 行删除
> **设计文档**: [../shim-optimization-design.md](../../shim-optimization-design.md)

---

## 1. 任务概述

在 `core/self/init.cppm` 中新增统一的 `create_shim()` 函数，采用分层链接策略：
- Unix: symlink（相对路径优先）> hardlink > copy
- Windows: hardlink > copy

替换 `ensure_subos_shims()` 中的 `link_or_copy` lambda，使 Linux 从 copy 提升为 symlink。

---

## 2. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/self/init.cppm` | 修改：新增 `create_shim()`，重构 `ensure_subos_shims()` |

---

## 3. 实施步骤

### 3.1 新增 create_shim()

```cpp
enum class LinkResult { Symlink, Hardlink, Copy, Failed };

export LinkResult create_shim(const fs::path& source, const fs::path& target);
```

逻辑：
1. 若目标已存在或为 symlink，先 remove
2. Unix: 尝试 relative symlink → absolute symlink → hardlink → copy
3. Windows: 尝试 hardlink → copy
4. 返回实际使用的方式

### 3.2 重构 ensure_subos_shims()

将 `link_or_copy` lambda 替换为 `create_shim()` 调用。

---

## 4. 验收

```bash
# Linux 上 shim 为 symlink
ls -la ~/.xlings/subos/default/bin/xim
# 输出应包含 -> ../../bin/xlings
```
