# T20 — xpkg spec 版本字段 + 字段规范化

> **优先级**: P3-a
> **Wave**: xim-Wave 4
> **预估改动**: ~30 行 Lua（2 个文件）
> **设计文档**: [../xpkg-spec-design.md](../xpkg-spec-design.md) §四

---

## 1. 任务概述

在 xpkg 格式中引入 `spec` 版本字段，并在框架层增加字段规范化（字段名兼容、类型标准化）和基础 schema 校验（warning 级别，不阻断安装）。

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T14, T15 | P0 问题先解决 |

---

## 3. 实现方案

### 3.1 XPackage.lua — 解析 spec 字段

```lua
function XPackage:_init(version, package)
    -- ... existing init logic ...
    
    self.spec = package.spec or "0"
    
    -- 字段兼容化
    if type(package.license) == "string" then
        package.licenses = package.licenses or {package.license}
    end
    if type(package.authors) == "string" then
        package.authors = {package.authors}
    end
    if type(package.maintainer) == "string" then
        package.maintainers = package.maintainers or {package.maintainer}
    end
    
    self.type = package.type or "package"
    -- ...
end
```

### 3.2 Schema 校验（warning 级别）

在 `IndexStore:build_xpkg_index()` 或 `XPkgManager:info()` 中增加校验：

```lua
function _validate_spec_v1(pkg, filepath)
    local warnings = {}
    if not pkg.spec then
        table.insert(warnings, "missing 'spec' field")
    end
    if not pkg.description then
        table.insert(warnings, "missing 'description' field")
    end
    if not pkg.type then
        table.insert(warnings, "missing 'type' field (defaulting to 'package')")
    end
    -- 输出 warnings，不阻断
    for _, w in ipairs(warnings) do
        cprint("[xpkg:spec]: ${yellow}%s${clear} - %s", w, filepath)
    end
end
```

### 3.3 推荐的 xpkg.lua 示例（spec v1）

```lua
package = {
    spec = "1",
    name = "cmake",
    description = "Cross-platform build system",
    type = "package",
    
    licenses = {"BSD-3-Clause"},
    authors = {"Kitware"},
    categories = {"build-system"},
    keywords = {"cmake", "build", "cross-platform"},
    programs = {"cmake", "ctest", "cpack"},
    
    homepage = "https://cmake.org",
    repo = "https://github.com/Kitware/CMake",
    docs = "https://cmake.org/documentation",
    
    xpm = { ... },
}
```

---

## 4. 验收标准

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| 有 `spec = "1"` 的包正常工作 | 安装/卸载无影响 | [ ] |
| 无 `spec` 字段的旧包正常工作 | 兼容模式，不报错 | [ ] |
| `license` (string) 自动转为 `licenses` (table) | `xlings info` 显示正确 | [ ] |
| `authors` (string) 自动转为 table | `xlings info` 显示正确 | [ ] |
| `xlings info cmake` 无 warning | spec v1 合规包无告警 | [ ] |
| Index rebuild 对不合规包输出 warning | 黄色告警，不阻断 | [ ] |
