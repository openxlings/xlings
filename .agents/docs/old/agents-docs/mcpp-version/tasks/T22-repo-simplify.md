# T22 — 多仓库简化（xim-pkgindex + awesome）

> **优先级**: P3-c
> **Wave**: xim-Wave 5
> **预估改动**: ~40 行 Lua + xpkg 文件
> **设计文档**: [../xpkg-spec-design.md](../xpkg-spec-design.md) §三

---

## 1. 任务概述

将新 xlings 的默认包索引源简化为只有 `xim-pkgindex` 一个仓库。原有的 5 个子仓库（fromsource、scode、d2x、template、awesome）改为通过 awesome 仓库按需添加。awesome 中的"索引仓库包"（`type = "index-repo"`）使得包索引仓库本身也是包。

---

## 2. 依赖前置

| 依赖 | 原因 |
|------|------|
| T20 | 需要 spec 字段和 type 分类体系 |
| T16 | namespace 解析统一后，子仓库注册更干净 |

---

## 3. 实现方案

### 3.1 清空 xim-indexrepos.lua

文件: `xim-pkgindex/xim-indexrepos.lua`

```lua
-- 子仓库不再内置，通过 awesome 仓库按需添加
xim_indexrepos = {}
```

### 3.2 框架支持 type = "index-repo"

在 `XPkgManager:install()` 或 `script.lua` 中增加对 `index-repo` 类型的处理：

```lua
-- 当 type == "index-repo" 时
-- install() hook 调用 IndexManager:add_subrepo()
-- uninstall() hook 调用移除逻辑
```

或者更简单：不在框架中特殊处理，让 `index-repo` 类型的 xpkg.lua 自己在 `install()` 中调用现有的 `xim --add-indexrepo` 机制。

### 3.3 为每个子仓库创建 xpkg 描述

在 awesome 仓库中创建 index-repo 类型的包定义：

```lua
-- awesome/pkgs/index-repos/fromsource-index.lua
package = {
    spec = "1",
    name = "fromsource-index",
    description = "Source-build packages: gcc, glibc, openssl, binutils from source",
    type = "index-repo",
    categories = {"index-repo"},
    keywords = {"fromsource", "source-build", "gcc", "glibc"},
    
    xpm = {
        linux = {
            ["latest"] = { ref = "main" },
            ["main"] = {
                url = {
                    GLOBAL = "https://github.com/d2learn/xim-pkgindex-fromsource.git",
                    CN = "https://github.com/d2learn/xim-pkgindex-fromsource.git",
                },
            },
        },
    },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.system")

function install()
    local url = pkginfo.resource_url()
    system.exec("xim --add-indexrepo fromsource:" .. url)
    return true
end

function uninstall()
    -- 移除索引仓库注册
    -- TODO: 需要 xim --remove-indexrepo 支持
    return true
end
```

类似地为 scode、d2x、template 各创建一个。

补充：已有能力 `xlings install --add-xpkg xxx/xx.lua` 可直接注入 index-repo 包并生效，因此 T22 第一阶段不需要新增专用 CLI。

### 3.4 用户迁移

在 `xlings self migrate` 中，检测 `xim-indexrepos.json` 是否有已注册的子仓库，保留它们：

```lua
-- 已有的子仓库注册不受影响
-- xim-indexrepos.json 继续生效
-- 只是新安装的 xlings 不再自动拉取子仓库
```

### 3.5 用户体验

```bash
# 新安装的 xlings — 只有核心包
xlings search gcc         # → gcc@15.1.0 (main repo)

# 添加 awesome 源
xlings repo add awesome

# 安装 fromsource 索引
xlings install fromsource-index
xlings search fromsource:gcc  # → fromsource:gcc@15.1.0

# 或者直接安装 awesome 中的独立社区包
xlings install some-community-tool
```

---

## 4. 验收标准

| 检查项 | 期望结果 | 通过 |
|--------|---------|------|
| 新安装 xlings 只有 xim-pkgindex 包源 | `xlings search` 只显示核心包 | [ ] |
| 首次 sync 不拉取 5 个子仓库 | sync 耗时 < 10 秒 | [ ] |
| 添加 awesome 后可搜索社区包 | `xlings search` 显示 awesome 包 | [ ] |
| 安装 fromsource-index 后可用 fromsource 包 | `fromsource:gcc` 可安装 | [ ] |
| 已有用户的 xim-indexrepos.json 不受影响 | 旧注册的子仓库继续工作 | [ ] |
| `xlings self migrate` 保留已注册子仓库 | 迁移后子仓库仍可用 | [ ] |
