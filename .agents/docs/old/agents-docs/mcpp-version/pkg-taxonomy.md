# xlings 包分类体系设计

> 关联文档:
> - [main.md — 顶层设计方案](main.md)
> - [rpath-and-os-vision.md — RPATH 与 OS 演进](rpath-and-os-vision.md)

---

## 一、现状梳理

通过分析 `/xpkgs/` 下的 160 个真实包条目，当前命名规范已自然形成了分类语义，但缺乏显式定义：

| 命名模式 | 实例 | 隐含含义 |
|---------|------|---------|
| `<name>/` | `cmake`, `node`, `dadk` | 用户直接使用的工具 |
| `fromsource-x-<name>/` | `fromsource-x-gcc`, `fromsource-x-glib` | xim 从源码编译的依赖库 |
| `fromsource@<name>/` | `fromsource@gcc`, `fromsource@musl-gcc` | 同上，命名空间写法 |
| `scode-x-<name>/` | `scode-x-linux`, `scode-x-cmake` | 仅下载源码，不编译 |
| `local-x-<name>/` | `local-x-gcc`, `local-x-nvim` | 本地 Lua 脚本包 |
| `config@<name>/` | `config@rust-crates-mirror` | 环境/镜像配置包 |
| `d2x-x-<name>/` | `d2x-x-mcpp-standard` | 交互式教程包 |
| `<ns>@<name>/` | `dragonos@dragonos-dev` | 项目命名空间包 |
| `<ns>-x-<name>/` | `moonbitlang-x-moonbit-cli` | 项目命名空间包（x 写法） |

**问题**：这些命名区分了"怎么装"，但没有区分"谁构建/谁维护"，也没有显式区分"xlings 官方预构建"和"外部上游预构建"。

---

## 二、两个核心维度

设计包分类体系需要两个正交维度：

```
维度 A: 获取/构建方式（How）
  ┌─ binary    ─ 下载预编译二进制
  ├─ fromsource ─ xim 从源码编译
  ├─ script    ─ Lua 脚本运行
  ├─ config    ─ 环境配置
  ├─ meta      ─ 元包（纯依赖声明）
  ├─ template  ─ 项目模板
  ├─ scode     ─ 下载源码（不编译）
  └─ d2x       ─ 交互式教程

维度 B: 构建者/维护者（Who）
  ┌─ official   ─ xlings 官方构建并托管预编译包
  ├─ upstream   ─ 上游项目方发布的原始预编译包
  ├─ community  ─ 社区贡献者维护
  └─ local      ─ 用户本地（不在公共索引中）
```

两个维度组合，形成完整的包类型描述。

---

## 三、正式包类型体系

### 3.1 按 `type` 字段（已有字段，扩展定义）

| type 值 | 含义 | 当前对应命名 | xpkgs 目录格式 |
|---------|------|------------|--------------|
| `binary` | 预编译二进制包 | 无前缀的包名 | `<name>/<version>/` |
| `fromsource` | 从源码构建 | `fromsource-x-*` | `fromsource-x-<name>/<version>/` |
| `script` | Lua 脚本包 | `local-x-*` | `local-x-<name>/` |
| `config` | 环境配置包 | `config@*` | `config-x-<name>/<profile>/` |
| `meta` | 元包（只有依赖） | 无（新增） | `meta-x-<name>/<version>/` |
| `template` | 项目模板 | `*-x-project-template` | `<ns>-x-<template>/` |
| `scode` | 源码下载包 | `scode-x-*` | `scode-x-<name>/<version>/` |
| `d2x` | 交互式教程 | `d2x-x-*` | `d2x-x-<name>/<version>/` |

### 3.2 新增 `source` 字段（针对 `binary` 类型）

```lua
-- xpkg 包描述中新增 source 字段
package = {
    name = "cmake",
    type = "binary",
    source = "upstream",   -- "official" | "upstream" | "community"
    -- ...
}
```

| source 值 | 含义 | 责任方 | RPATH 控制 | 示例 |
|----------|------|--------|-----------|------|
| `official` | xlings 官方构建并托管 | xlings 团队 | xlings 全控 | xvm, xim, xlings 自身 |
| `upstream` | 上游项目方发布原始包 | 上游项目 | 上游决定 | cmake, node, vscode |
| `community` | 社区构建并维护 | 社区贡献者 | 社区决定 | 大多数社区包 |

### 3.3 `maintainer` 字段（包脚本维护者，不同于构建者）

```lua
package = {
    name    = "cmake",
    type    = "binary",
    source  = "upstream",
    maintainer = "xlings",   -- "xlings" | "community" | "local"
    -- ...
}
```

`maintainer` 表示"谁写并维护这个 xpkg.lua 脚本"，与 `source` 区分：

```
cmake 包:
  maintainer = "xlings"    ← xlings 团队写 xpkg.lua 脚本
  source     = "upstream"  ← 但二进制来自 cmake.org 官方发布

xvm 包（xlings 自身组件）:
  maintainer = "xlings"    ← xlings 团队写 xpkg.lua 脚本
  source     = "official"  ← 二进制也由 xlings 官方构建托管
```

---

## 四、三种预构建包的完整对比

这是用户最关心的核心区分：

```
                    ┌─────────────────────────────────────────────┐
                    │           预构建包（type = "binary"）         │
                    └──────────────┬──────────────────────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                     ▼
    source = "official"   source = "upstream"   source = "community"
    ─────────────────     ──────────────────     ───────────────────
    xlings 官方预构建      上游官方预构建          社区预构建

    xlings 编译并托管      cmake.org 发布          社区成员编译
    可保证 $ORIGIN RPATH   RPATH 由上游决定        RPATH 由社区决定
    可保证依赖一致性       依赖系统 glibc           不保证

    xpkg: xpm.url 指向    xpkg: xpm.url 指向      xpkg: xpm.url 指向
    xlings-res CDN        上游 GitHub Releases     社区服务器

    例: xvm, xim,         例: cmake, node,         例: 小众工具
        .xlings.real          vscode, rustup            社区脚本
```

### 4.1 xlings 官方预构建（`source = "official"`）

```
特点:
  - xlings 团队控制编译环境和编译参数
  - 使用 $ORIGIN/../lib 作为 RPATH
  - 跨包依赖通过 workspace.yaml 的 ${XLINGS_HOME} 变量注入
  - 托管于 xlings-res CDN（GitHub Releases / gitcode.com）
  - 支持多架构（x86_64, arm64, riscv64...）

xpkg 脚本示例:
  source = "official"
  xpm.url 格式: XLINGS_RES（使用 xlings 镜像加速体系）
```

### 4.2 上游官方预构建（`source = "upstream"`）

```
特点:
  - xlings 不控制编译过程，直接包装上游 Release
  - RPATH 由上游决定（通常静态链接或 $ORIGIN）
  - 如果上游 RPATH 有问题，xpkg config 钩子可用 patchelf 修复
  - URL 指向上游 GitHub Releases、官网下载页等

xpkg 脚本示例:
  source = "upstream"
  xpm = {
    linux = {
      ["4.0.2"] = {
        url = "https://github.com/Kitware/CMake/releases/download/v4.0.2/cmake-4.0.2-linux-x86_64.tar.gz",
        sha256 = "..."
      }
    }
  }
```

### 4.3 xlings 维护的从源码构建（`type = "fromsource"`）

```
特点:
  - xim 在安装时于本地编译源码
  - xlings 控制编译参数，确保 $ORIGIN RPATH
  - xpkg 的 build 钩子传入正确的 --prefix 和 LDFLAGS
  - workspace.yaml 的 envs 用 ${XLINGS_HOME} 声明跨包依赖

xpkg 脚本示例:
  type = "fromsource"
  function build()
    -- prefix 指向 xpkgs 安装目录
    -- LDFLAGS 加入 -Wl,-rpath,'$ORIGIN/../lib'
  end
  function config()
    xvm.add("gcc", {
      path = "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gcc/15.0",
      envs = { LD_LIBRARY_PATH = "${XLINGS_HOME}/xim/xpkgs/fromsource-x-gmp/6.3.0/lib" }
    })
  end
```

---

## 五、完整 xpkg 包描述规范（扩展版）

```lua
package = {
    -- ── 基础信息 ──────────────────────────────────────────────
    name        = "cmake",
    namespace   = nil,           -- 可选，如 "dragonos", "moonbitlang"
    description = "Cross-platform build system",
    homepage    = "https://cmake.org",
    repo        = "https://github.com/Kitware/CMake",
    docs        = "https://cmake.org/documentation/",
    license     = "BSD-3-Clause",

    authors     = "Kitware",
    maintainers = "xlings-team",  -- xpkg 脚本维护者

    -- ── 分类字段 ──────────────────────────────────────────────
    type        = "binary",       -- binary | fromsource | script | config
                                  -- meta | template | scode | d2x
    source      = "upstream",     -- official | upstream | community
                                  -- (仅 type = "binary" 时有意义)
    maintainer  = "xlings",       -- xlings | community | local

    categories  = {"build-tools", "development"},
    keywords    = {"cmake", "build", "cross-platform"},
    status      = "stable",       -- dev | stable | deprecated

    -- ── 版本与平台 ────────────────────────────────────────────
    archs       = {"x86_64", "arm64", "riscv64"},

    xpm = {
        linux = {
            -- 跨版本引用
            ["latest"] = { ref = "4.0.2" },
            -- 具体版本
            ["4.0.2"] = {
                url    = { default = "https://...", gitcode = "https://..." },
                sha256 = "abc123...",
            },
        },
        macos = { ... },
        windows = { ... },
    },

    -- ── 依赖声明 ──────────────────────────────────────────────
    -- （由 xpkg:get_deps() 返回，已有字段，放在 xpm.<os>.deps 下）
}
```

---

## 六、xpkgs 目录命名约定（完整版）

```
xpkgs/
│
│  ── 用户直接使用的工具（type: binary / fromsource）──
├── cmake/4.0.2/              ← binary, source=upstream
├── node/22.0/                ← binary, source=upstream
├── gcc/15.0/                 ← binary 或 fromsource，用户命令
│
│  ── xlings 官方组件 ──
├── xvm/0.0.5/                ← binary, source=official
├── xim/0.0.2/                ← binary, source=official
│
│  ── 从源码构建的依赖库（内部依赖，非用户直接调用）──
├── fromsource-x-gcc/15.0/    ← fromsource（内部依赖，与用户 gcc 不同）
├── fromsource-x-gmp/6.3.0/
├── fromsource-x-glib/2.82.2/
│
│  ── 源码下载包（不编译，供调试/学习/构建使用）──
├── scode-x-linux/5.11.1/     ← scode
├── scode-x-cmake/3.28.0/
│
│  ── 配置包 ──
├── config-x-rust-crates-mirror/tsinghua/   ← config
├── config-x-rustup-mirror/ustc/
│
│  ── 命名空间包（项目/组织维护）──
├── dragonos@dragonos-dev/0.2.0/            ← <ns>@<name>/<ver>
├── moonbitlang-x-moonbit-cli/0.1.0/        ← <ns>-x-<name>/<ver>
│
│  ── 交互式教程包 ──
├── d2x-x-mcpp-standard/latest/             ← d2x
│
│  ── 本地脚本包 ──
├── local-x-git-autosync/                   ← script，无版本目录
├── local-x-nvim/
│
│  ── 元包（仅声明依赖）──
└── meta-x-dev-env/latest/                  ← meta（新增类型，暂无实例）
```

---

## 七、安装流程对比

```
binary (upstream / community)           binary (official)
───────────────────────────────────     ─────────────────────────────────
download()                              download()
  └─ 从上游 URL 下载预编译包              └─ 从 xlings-res CDN 下载
install()                               install()
  └─ 解压到 xpkgs/<name>/<ver>/          └─ 解压到 xpkgs/<name>/<ver>/
config()                                config()
  ├─ xvm.add(name, path)                 ├─ xvm.add(name, { path, envs })
  └─ (可选) patchelf 修复 RPATH           └─ RPATH 已正确，无需修复


fromsource                              script / local
───────────────────────────────────     ─────────────────────────────────
download()                              (无 download 步骤)
  └─ 下载源码压缩包或 git clone          install()
build()                                   └─ 复制 .lua 脚本到安装目录
  ├─ ./configure --prefix=<xpkgdir>     config()
  │   LDFLAGS="-Wl,-rpath,'$ORIGIN'      └─ xvm.add(name, { alias = "..." })
  └─ make && make install
config()
  └─ xvm.add(name, {
       path = "${XLINGS_HOME}/xim/xpkgs/...",
       envs = { LD_LIBRARY_PATH = "${XLINGS_HOME}/xim/xpkgs/<dep>/lib" }
     })
```

---

## 八、RPATH 策略汇总

| 包类型 | source | RPATH 策略 | 谁负责 |
|--------|--------|-----------|--------|
| `binary` | `official` | `$ORIGIN/../lib` + `${XLINGS_HOME}` workspace envs | xlings 团队 |
| `binary` | `upstream` | 由上游决定；必要时 xpkg config 钩子用 patchelf 修复 | 包维护者 |
| `binary` | `community` | 同上游，社区包维护者负责 | 社区贡献者 |
| `fromsource` | — | `$ORIGIN/../lib`（构建时强制注入）+ workspace `${XLINGS_HOME}` | xlings/社区 |
| `script` | — | 无 RPATH 问题（Lua 解释器，无编译产物） | — |
| `config` | — | 无 RPATH 问题 | — |

---

## 九、对现有实现的改动量

### 需要新增的 xpkg 字段（向后兼容）

```lua
-- 新字段均为可选，现有 xpkg 不填则使用默认值
source     = nil    -- 默认: "upstream"（对 binary 类型）
maintainer = nil    -- 默认: "community"
```

### 需要改动的代码

| 位置 | 改动 | 工作量 |
|------|------|--------|
| `xpkg` 包描述规范文档 | 新增 `source`/`maintainer` 字段说明 | 文档 |
| `XPackage.lua` `info()` | 返回 `source` 和 `maintainer` 字段 | ~5 行 |
| `XPkgManager.lua` `info()` | 展示新字段 | ~3 行 |
| `xpkg-index` 元数据 | 按 type/source 过滤搜索 | 索引侧 |
| `xlings search` 命令 | 支持 `--type fromsource` 等过滤 | C++ 侧传参给 xim |

现有所有包无需改动，新字段对旧包透明。

---

## 十、包的粗分类：命名空间约定

无需额外的元数据描述文件。xpkg 描述文件本身已包含所有元数据（`type`、`source`、`maintainer`、`categories` 等），索引工具直接解析 xpkg 文件即可。

粗粒度的分类通过 **命名空间（namespace）** 天然完成：

```
namespace@name      ← 项目/组织命名空间（dragonos@app-template，moonbitlang@moonbit-cli）
fromsource-x-name   ← 源码构建的内部依赖库
scode-x-name        ← 仅下载源码
local-x-name        ← 本地脚本包
config-x-name       ← 环境配置包
d2x-x-name          ← 交互式教程包
<name>              ← 用户直接使用的工具（binary / fromsource 均可）
```

细粒度过滤（如 `xlings search cmake --type binary --source official`）通过解析各 xpkg 文件中的 `type`/`source` 字段在运行时完成，不依赖单独的索引元数据格式。
