# xpkg 规范化设计

> 关联文档:
> - [xim-issues-design.md — xim 模块问题与改进](xim-issues-design.md)
> - [tasks/README.md — 任务总览](tasks/README.md)

---

## 一、目标与愿景

xpkg 的定位不只是 xlings 的内部格式，而是一个**通用的包描述规范**：

1. **通用性** — 能描述任何可分发的产物：编译器、脚本工具、系统配置、项目模板、AI skill/agent/model 等
2. **规范化** — 有明确的版本号、字段约束、语义契约，第三方工具可可靠地解析和消费
3. **独立性** — 未来可提取为 `libxpkg` 独立库，处理包索引、格式校验、依赖解析，不绑定 xlings 实现
4. **自举性** — 包索引仓库本身也是包，包管理器管理自己的包源

### 1.1 设计约束

- **单文件格式** — xpkg.lua 是刻意的单文件设计，声明和逻辑放在一起，降低包作者门槛
- **元数据索引** — Index DB 预构建最简信息，按需加载 xpkg.lua，解决"不执行 Lua 无法索引"的问题
- **向前兼容** — 现有 130+ 个包不能 break，新增字段渐进引入

---

## 二、当前状态评估

### 2.1 xpkg.lua 字段使用统计（56 个主仓库包）

| 字段 | 使用率 | 类型一致性 |
|------|--------|-----------|
| `name` | 100% | string |
| `xpm` | 100% | table |
| `description` | 98% | string |
| `status` | 93% | string (dev/stable/deprecated) |
| `categories` | 89% | table |
| `keywords` | 84% | table |
| `licenses` | 80% | 混用 string/table，且有 `license` vs `licenses` |
| `repo` | 79% | string |
| `type` | 75% | string，13 个包不写 |
| `archs` | 63% | table |
| `programs` | 54% | table |
| `authors` | 34% | 混用 string/table |
| `docs` | 32% | string |
| `homepage` | 27% | string |
| `namespace` | 9% | string (仅 5 个 `"config"`) |

### 2.2 问题点

| 问题 | 影响 |
|------|------|
| 无 spec 版本号 | 格式演进无法保证兼容 |
| 字段命名不一致 | `license` vs `licenses`、`maintainer` vs `maintainers` |
| 字段类型不一致 | `authors` 有时 string 有时 table |
| `type` 无默认值约定 | 23% 的包不写 type |
| 资源声明有 6+ 种模式 | 无形式化语法 |
| 依赖语法原始 | 只有 `"name@version"` 精确匹配 |
| Hook 语义无正式定义 | 包作者误用（xvm.add 放在 install 里） |

### 2.3 Index DB 现状

当前 `xim-index-db.lua` 只存储最简路由信息：

```lua
["cmake@4.0.2"] = {
    path = "/path/to/cmake.lua",
    version = "4.0.2",
    installed = false,
}
```

不存储 description、type、categories、programs 等字段，无法做富元数据搜索。

---

## 三、多仓库过渡策略

### 3.1 当前多仓库结构

```
xim-pkgindex (main, 56 pkgs)              ← 官方核心
  └── xim-indexrepos.lua                    ← 内置声明 5 个子仓库

xim-index-repos/ (自动拉取)
  ├── xim-pkgindex-fromsource  (45 pkgs)   ← 源码构建
  ├── xim-pkgindex-scode       (15 pkgs)   ← 源码包
  ├── xim-pkgindex-awesome      (7 pkgs)   ← 社区包
  ├── xim-pkgindex-d2x          (6 pkgs)   ← d2learn 工具
  └── xim-pkgindex-template     (1 pkg)    ← 项目模板
```

问题：5 个子仓库全部内置加载，新用户首次 sync 慢；namespace 导致路径猜测问题。

### 3.2 目标结构：xim-pkgindex + awesome

```
xim-pkgindex (main, 56+ pkgs)             ← 官方核心，新 xlings 唯一默认源
  └── pkgs/
      ├── cmake.lua
      ├── gcc.lua
      └── ...

xim-pkgindex-awesome (社区生态入口)        ← 用户按需添加
  └── pkgs/
      ├── index-repos/
      │   ├── fromsource-index.lua         ← type = "index-repo"
      │   ├── scode-index.lua              ← type = "index-repo"
      │   ├── d2x-index.lua               ← type = "index-repo"
      │   └── template-index.lua           ← type = "index-repo"
      └── apps/
          ├── some-community-pkg.lua
          └── ...
```

### 3.3 包索引仓库也是包

核心思路：一个 index-repo 用 xpkg.lua 描述自己，`install()` 就是 `xim --add-indexrepo`：

```lua
-- awesome/pkgs/index-repos/fromsource-index.lua
package = {
    spec = "1",
    name = "fromsource-index",
    type = "index-repo",
    description = "Source-build package index (gcc, glibc, openssl from source)",
    categories = {"index-repo", "fromsource"},
    
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

function install()
    -- 注册为包索引源
    xim.add_indexrepo("fromsource", pkginfo.resource_url())
    return true
end

function uninstall()
    xim.remove_indexrepo("fromsource")
    return true
end
```

用户体验：

```bash
# 新 xlings 默认只有核心包
xlings search gcc          # → 找到 gcc (main repo)

# 想要源码构建版？添加 awesome 源
xlings repo add awesome    # 添加 awesome 仓库

# 然后安装 fromsource 索引（它本身是一个包）
xlings install fromsource-index

# 现在可以搜到源码构建版了
xlings search fromsource:gcc  # → 找到 fromsource:gcc
```

也可以直接通过本地/远程 xpkg 文件注入索引包（已有能力）：

```bash
xlings install --add-xpkg /path/to/fromsource-index.lua
# 或
xlings install --add-xpkg https://example.com/fromsource-index.lua
```

### 3.4 过渡计划

| 阶段 | 动作 | 影响 |
|------|------|------|
| Phase 0（当前） | 保持现状，不改多仓库机制 | 无 |
| Phase 1 | `xim-indexrepos.lua` 清空，不再内置子仓库声明 | 新安装的 xlings 只有 56 个核心包 |
| Phase 2 | awesome 仓库中为每个子仓库创建 `type = "index-repo"` 的 xpkg | 用户按需安装 |
| Phase 3 | 框架支持 `type = "index-repo"` 的安装/卸载 hook | `xim.add_indexrepo()` / `xim.remove_indexrepo()` |

**对现有用户的迁移**：`xlings self migrate` 时自动将已注册的子仓库保留在 `xim-indexrepos.json` 中，不影响已有环境。

### 3.5 优势

- **默认简洁** — 新用户开箱 56 个核心包，首次 sync 快
- **按需扩展** — 需要源码构建、社区包时才拉取
- **自举** — 包索引仓库本身作为包被管理，统一生命周期
- **去中心化** — 任何人都可以创建 index-repo 类型的包，发布自己的包源
- **namespace 自然解决** — 每个 index-repo 安装时注册自己的 namespace，不再硬编码

---

## 四、Spec v1 — 本次可实施的改进

### 4.1 spec 版本字段

在 `package` 表中新增 `spec` 字段：

```lua
package = {
    spec = "1",       -- 新增，必选（框架对旧包容忍缺失，视为 spec = "0"）
    name = "cmake",
    -- ...
}
```

框架行为：
- `spec` 缺失 → 视为 `"0"`（兼容模式，不做校验）
- `spec = "1"` → 启用字段校验（warning 级别，不 break）
- 未来 `spec = "2"` → 可引入破坏性变更

### 4.2 字段规范化

统一字段命名和类型约定（spec v1）：

| 字段 | 类型 | 必选 | 说明 |
|------|------|------|------|
| `spec` | string | 是 | 规范版本号 |
| `name` | string | 是 | 包名（全局唯一，小写+连字符） |
| `description` | string | 是 | 一句话描述 |
| `type` | string | 是 | 包类型（见 4.3） |
| `licenses` | table | 否 | SPDX 标识符列表 |
| `authors` | table | 否 | 作者列表 |
| `maintainers` | table | 否 | 维护者列表 |
| `categories` | table | 否 | 分类标签 |
| `keywords` | table | 否 | 搜索关键词 |
| `programs` | table | 否 | 提供的可执行程序名 |
| `homepage` | string | 否 | 项目主页 |
| `repo` | string | 否 | 源码仓库 |
| `docs` | string | 否 | 文档链接 |
| `xpm` | table | 是 | 平台资源声明 |

兼容处理：
- `license` (单数/string) → 框架自动视为 `licenses = {license}`
- `maintainer` (单数) → 框架自动视为 `maintainers = {maintainer}`
- `authors` 为 string 时 → 框架自动视为 `authors = {authors}`

### 4.3 type 分类体系

定义核心类型（spec v1），预留扩展能力：

```
核心类型（spec v1）:
  package      — 标准包（二进制、库、工具）
  script       — xpkg_main 脚本工具
  config       — 系统配置操作（一次性）
  template     — 项目模板
  index-repo   — 包索引仓库（新增）

预留类型（spec v2+，未来）:
  skill        — AI 技能包
  agent        — AI Agent
  model        — AI 模型
  dataset      — 数据集
  extension    — 编辑器/IDE 扩展
```

框架对未知 type 不报错，只在 Index DB 中标记，为未来扩展留空间。

### 4.4 Hook 语义契约

在 XPackage.lua 的 spec 注释和文档中明确定义：

```
Hook           作用域       契约
─────────────────────────────────────────────────────────
installed()    per-subos    检查当前 subos 是否已激活此包
                            返回 boolean
                            框架默认实现：xvm.has(name, version)

install()      global       将包文件物化到 install_dir
                            前置：install_dir 已创建，download 文件在 runtime_dir
                            后置：install_dir 中包含完整的包文件
                            幂等性：框架保证 xpkgs 已有文件时不调用

config()       per-subos    将包激活到当前 subos
                            职责：xvm 注册、环境配置、header/lib 软链
                            幂等性：可重复调用（切换 subos 时自动调用）
                            框架兜底：template 类型自动注册 xvm

uninstall()    per-subos    从当前 subos 注销此包
                            职责：xvm 注销、环境清理
                            注意：物理文件删除由框架根据引用计数决定

build()        global       编译/构建（可选，源码包使用）
                            前置：源码已下载到 runtime_dir

xpkg_main()    —            脚本入口（script 类型包）
                            通过 xvm alias 调用
```

### 4.5 Index DB 丰富化

rebuild 时从 xpkg.lua 中多提取元数据字段：

```lua
-- 当前
["cmake@4.0.2"] = {
    path = "...", version = "4.0.2", installed = false,
}

-- 丰富化后
["cmake@4.0.2"] = {
    path = "...",
    version = "4.0.2",
    installed = false,
    -- 新增字段
    type = "package",
    description = "Cross-platform build system",
    categories = {"build-system"},
    programs = {"cmake", "ctest", "cpack"},
}
```

这使得 `xlings search` 可以按 type 过滤、按 description 模糊搜索、按 category 分组展示。

改动范围：只需修改 `IndexStore:build_xpkg_index()` 中的数据提取逻辑，约 10 行。

---

## 五、未来路线图

### 5.1 libxpkg 独立库（P4）

将 xpkg 格式解析、Index DB 构建/查询、Schema 校验、依赖解析提取为独立的 Lua 模块：

```
libxpkg/
  parser.lua       — xpkg.lua 格式解析
  schema.lua       — spec 版本校验
  index.lua        — Index DB 构建与查询
  resolver.lua     — 依赖解析
  spec.lua         — spec 版本定义与字段约束
```

可被 xlings CLI、Web 前端、CI 工具、IDE 插件等独立引用。

### 5.2 AI 生态类型扩展（P5）

当 type 体系稳定后，扩展 AI 相关类型：

```lua
package = {
    spec = "2",
    name = "code-review-agent",
    type = "agent",
    description = "AI agent for automated code review",
    
    -- AI 特有字段（spec v2）
    capabilities = {"code-review", "refactoring", "test-generation"},
    requires = {"llm-runtime >= 1.0"},
    model_size = "7B",
    
    xpm = { ... },
}
```

xpkg 格式天然适合描述 AI 产物：
- `install()` → 下载模型权重/配置文件
- `config()` → 注册到 agent runtime、配置推理引擎
- `uninstall()` → 注销
- `xpkg_main()` → 直接运行 agent

### 5.3 依赖约束语法（P4）

```lua
-- 当前
deps = { "gcc@15.1.0", "cmake" }

-- 未来
deps = {
    { name = "gcc", version = ">=13.0.0" },
    { name = "glibc", version = "~2.38", optional = true },
    { name = "cmake", version = ">=3.20", build_only = true },
}
```

### 5.4 跨语言索引格式（P5）

为 Web 前端和非 Lua 工具提供 JSON 格式的 Index DB：

```bash
xim --export-index --format json > xim-index.json
```

### 5.5 包签名与验证（P5）

```lua
package = {
    spec = "2",
    name = "cmake",
    signature = "sha256:abc123...",  -- 包内容签名
    signed_by = "d2learn-official",  -- 签名者
}
```

---

## 六、实施优先级

| 优先级 | 内容 | 任务 | Wave |
|--------|------|------|------|
| P0 | 多 subos 复用 + remove 安全 | T14, T15, T19 | 当前 |
| P1 | 命名空间统一 + dep_install_dir | T16, T17 | 当前 |
| P2 | 项目级 .xlings.json 依赖 | T18 | 当前 |
| P3-a | spec 版本字段 + 字段规范化 | T20 | 下一阶段 |
| P3-b | Index DB 丰富化 | T21 | 下一阶段 |
| P3-c | 多仓库简化（xim-pkgindex + awesome） | T22 | 下一阶段 |
| P4 | libxpkg 独立库 / 依赖约束语法 | — | 长期 |
| P5 | AI 生态类型 / 跨语言索引 / 包签名 | — | 远期 |
