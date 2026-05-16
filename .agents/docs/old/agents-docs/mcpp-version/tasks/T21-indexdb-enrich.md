# T21 — Index DB 丰富化

> **优先级**: P3-b
> **Wave**: xim-Wave 4（可与 T20 并行）
> **预估改动**: ~15 行 Lua（1 个文件）
> **设计文档**: [../xpkg-spec-design.md](../xpkg-spec-design.md) §四

---

## 1. 任务概述

修改 `IndexStore:build_xpkg_index()`，在构建 Index DB 时从 xpkg.lua 的 `package` 表中多提取 `type`、`description`、`categories`、`programs` 字段，使得搜索和展示可以使用这些信息。

---

## 2. 依赖前置

无强依赖。建议与 T20 同一阶段实施。

---

## 3. 实现方案

### 3.1 修改 IndexStore:build_xpkg_index()

文件: `core/xim/index/IndexStore.lua`

在 `self._index_data[key]` 赋值时增加字段提取：

```lua
-- 现有
self._index_data[key] = {
    mutex_group = pkg.mutex_group,
    version = version,
    installed = false,
    path = xpkg_file
}

-- 改为
self._index_data[key] = {
    mutex_group = pkg.mutex_group,
    version = version,
    installed = false,
    path = xpkg_file,
    -- 新增丰富元数据
    type = pkg.type or "package",
    description = pkg.description,
    categories = pkg.categories,
    programs = pkg.programs,
}
```

### 3.2 搜索增强（可选）

在 `IndexManager:search()` 中支持按 type 过滤和 description 模糊搜索：

```lua
function IndexManager:search(query, opt)
    -- ...existing logic...
    
    -- 增加 type 过滤
    if opt.type and pkg.type ~= opt.type then
        -- skip
    end
    
    -- 增加 description 模糊匹配
    if pkg.description and pkg.description:find(query, 1, true) then
        add_name(names, name, alias_name)
    end
end
```

---

## 4. 验收标准

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| `xim --update index` 后 Index DB 包含 type 字段 | 检查 xim-index-db.lua | [ ] |
| `xim --update index` 后 Index DB 包含 description | 检查 xim-index-db.lua | [ ] |
| `xlings search cmake` 显示描述信息 | 输出包含描述 | [ ] |
| Index DB 文件大小增长合理 | < 200KB（当前 ~58KB） | [ ] |
| 无 description 的旧包不报错 | description 为 nil | [ ] |
