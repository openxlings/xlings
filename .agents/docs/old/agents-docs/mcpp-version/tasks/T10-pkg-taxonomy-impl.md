# T10 — XPackage.lua: 实现 `source` / `maintainer` 字段

> **Wave**: 1（无前置依赖，可立即执行）
> **预估改动**: ~10 行 Lua，2 个文件

---

## 1. 任务概述

在 `XPackage.lua` 的 `info()` 方法中暴露 `source` 和 `maintainer` 字段，并在 `XPkgManager.lua` 的 `info()` 打印中展示这两个字段，使 `xlings info <pkg>` 能够显示包的来源和维护者信息。

**设计背景**: 详见 [../pkg-taxonomy.md §3](../pkg-taxonomy.md)

---

## 2. 依赖前置

无（Wave 1，独立任务）

---

## 3. 涉及文件

| 文件 | 操作 |
|------|------|
| `core/xim/pm/XPackage.lua` | `info()` 新增 `source`、`maintainer` 字段 |
| `core/xim/pm/XPkgManager.lua` | `info()` 打印新增两个字段展示 |

---

## 4. 实施步骤

### 4.1 修改 `XPackage.lua` 的 `info()` 方法

当前 `info()` 返回表（第 34-53 行），新增两个字段：

```lua
function XPackage:info()
    return {
        type        = self.pdata.type,
        namespace   = self.pdata.namespace,
        name        = self.pdata.name,
        homepage    = self.pdata.homepage,
        version     = self.version,
        authors     = self.pdata.authors,
        maintainers = self.pdata.maintainers,
        -- ── 新增 ──────────────────────────────────────────────
        source      = self.pdata.source,      -- "official" | "upstream" | "community" | nil
        maintainer  = self.pdata.maintainer,  -- "xlings"   | "community" | "local"    | nil
        -- ─────────────────────────────────────────────────────
        categories  = self.pdata.categories,
        keywords    = self.pdata.keywords,
        contributors= self.pdata.contributors,
        licenses    = self.pdata.licenses or self.pdata.license,
        repo        = self.pdata.repo,
        docs        = self.pdata.docs,
        forum       = self.pdata.forum,
        description = self.pdata.description,
        programs    = self.pdata.programs,
    }
end
```

### 4.2 修改 `XPkgManager.lua` 的 `info()` 打印

在 `XPkgManager:info()` 方法的 `fields` 列表（约第 165 行）中新增两个字段：

```lua
local fields = {
    { key = "name",         label = "name" },
    { key = "homepage",     label = "homepage" },
    { key = "version",      label = "version" },
    { key = "namespace",    label = "namespace" },
    -- ── 新增 ─────────────────────────────────────
    { key = "source",       label = "source" },      -- 新增
    { key = "maintainer",   label = "maintainer" },  -- 新增
    -- ─────────────────────────────────────────────
    { key = "authors",      label = "authors" },
    { key = "maintainers",  label = "maintainers" },
    { key = "contributors", label = "contributors" },
    { key = "licenses",     label = "licenses" },
    { key = "repo",         label = "repo" },
    { key = "docs",         label = "docs" },
    { key = "forum",        label = "forum" },
}
```

---

## 5. 字段语义说明

### `source` 字段

| 值 | 含义 |
|----|------|
| `"official"` | xlings 官方构建并托管预编译包 |
| `"upstream"` | 上游项目方发布（如 cmake.org）|
| `"community"` | 社区贡献者构建/托管 |
| `nil`（默认） | 未指定，等价于 `"upstream"` |

### `maintainer` 字段

| 值 | 含义 |
|----|------|
| `"xlings"` | xlings 团队维护 xpkg 脚本 |
| `"community"` | 社区贡献者维护 |
| `"local"` | 用户本地包（不在公共索引）|
| `nil`（默认） | 未指定，等价于 `"community"` |

两个字段均为**可选**，现有 xpkg 文件不需要修改。

---

## 6. 验收标准

### 6.1 新字段在 `xlings info` 中显示

对有 `source` / `maintainer` 字段的包：

```bash
xlings info cmake
```

期望输出包含：
```
source:     upstream
maintainer: xlings
```

### 6.2 旧包不受影响

对没有 `source` / `maintainer` 字段的现有包：

```bash
xlings info dadk
```

期望输出：没有 `source:` / `maintainer:` 行（字段为 nil 时 `info()` 中已有的 `if value then` 判断自动跳过）。

### 6.3 xpkg 脚本新写法验证

创建一个测试用 xpkg（或修改现有包），添加字段：

```lua
package = {
    name       = "cmake",
    type       = "binary",
    source     = "upstream",
    maintainer = "xlings",
    -- ...
}
```

执行 `xlings info cmake` 后确认两个字段正确显示。
