# xlings C++23 迁移 — 设计方案导航

> **项目**: [xlings](https://github.com/d2learn/xlings) | **目标版本**: 0.2.0 | **语言标准**: C++23 Modules
>
> **状态**: 设计完成，实施阶段

---

## 文档地图

```
docs/mcpp-version/
├── README.md                 ← 本文件（导航入口）
│
├── 设计文档（6 份）
│   ├── main.md               ← 顶层设计方案（必读）
│   ├── env-analysis.md       ← 现有架构深度分析
│   ├── env-design.md         ← 多环境管理与业界对比
│   ├── env-store-design.md   ← Store/Profile/世代设计
│   ├── rpath-and-os-vision.md← RPATH 解决方案 + OS 演进
│   ├── pkg-taxonomy.md       ← 包分类体系设计
│   ├── elf-relocation-and-subos-design.md ← ELF 可重定位与多 subos 设计（预构建 loader/RPATH、LD_LIBRARY_PATH）
│   ├── elfpatch-shrink-rpath-mode.md ← elfpatch 可选 shrink-rpath 模式
│   ├── release-static-build.md ← Linux musl 静态构建方案
│   ├── install-scripts-design.md ← 安装脚本方案与设计
│   ├── xpkgs-subos-hybrid-design.md ← data/xpkgs + subos 混合视图设计
│   ├── hybrid-libpath-order-subplan.md ← 混合视图子方案（RPATH 唯一真相与库路径优先级）
│   ├── rpath-migration-summary.md ← RPATH 迁移方案总结与原理说明
│   ├── xim-dir-compat.md    ← xim 目录兼容方案（临时）
│   └── shim-unified-design.md ← 短命令与 xvm 配置全平台统一设计
│
└── tasks/                    ← 任务拆分
    ├── README.md             ← 任务总览 + 依赖拓扑 + 并行分组
    ├── T01-platform-exe-path.md
    ├── T02-config-multienv.md
    ├── T03-env-module.md
    ├── T04-profile-module.md
    ├── T05-cmdprocessor-update.md
    ├── T06-xvm-var-expand.md
    ├── T07-xim-pkgdir.md
    ├── T08-self-migrate.md
    ├── T09-ci-multiplatform.md
    ├── T10-pkg-taxonomy-impl.md
    ├── T11-xmake-musl-static.md
    ├── T12-ci-musl-gcc.md
    ├── T13-verify-static-binary.md
    ├── T24-shim-init-module.md
    ├── T25-subos-create-unified.md
    └── T26-release-scripts-unified.md
```

---

## 设计文档概览

### [main.md](main.md) — 顶层设计方案（必读）

涵盖迁移范围、整体架构、C++23 模块列表、自包含检测机制、多环境命令设计、多平台支持策略、代码风格规范和构建发布流程。是所有其他文档的上下文基础。

**关键结论**:
- xim（Lua）和 xvm（Rust）暂不迁移，C++ 层做入口编排
- `XLINGS_HOME` 路径可完全自定义，实现系统环境隔离
- 自包含检测：可执行文件同级有 `xim/` 目录 → 自包含模式

---

### [env-analysis.md](env-analysis.md) — 现有架构深度分析

分析 `/xpkgs/`（160+ 包）、`data/bin/`（616 个 xvm-shim 副本）和 `.workspace.xvm.yaml` 的真实数据，揭示 xlings 已天然形成三层 Nix-like 架构。

**关键结论**:
- `xim/xpkgs/` 已是 **content-addressed store**（版本寻址，多版本并存）
- `data/bin/` 是 **激活层**（xvm-shim 物理副本，无状态动态路由）
- `.workspace.xvm.yaml` 是 **profile**（激活版本清单）

---

### [env-design.md](env-design.md) — 多环境管理与业界对比

与 Nix、Conda、Python venv、Rustup、Docker、Sandbox 的详细对比分析，论证 xlings 多环境方案在跨平台性和轻便性上的优势，以及 `xlings env` 的 5 个子命令接口设计。

---

### [env-store-design.md](env-store-design.md) — Store/Profile/世代设计

核心结论：`xim/xpkgs/` 已是 store，无需另建。重点设计：

- **多环境目录结构**：`envs/<name>/bin/` + `envs/<name>/xvm/workspace.yaml`（per-env 隔离）
- **共享 store**：`xim/xpkgs/`（全局，所有环境共用）
- **Profile 世代**：`generations/0XX.json`（每次安装/删除追加，支持回滚）
- **GC**：扫描所有 envs 引用，清理 xpkgs 孤立版本
- **XLINGS_PKGDIR**：新环境变量，指向全局 xpkgs，xim 优先读取

---

### [rpath-and-os-vision.md](rpath-and-os-vision.md) — RPATH 解决方案 + OS 演进

**RPATH 方案**（两层，~15 行代码实现）:
- 同包内：`$ORIGIN/../lib`（构建时注入）
- 跨包：`${XLINGS_HOME}` 变量替换 → `LD_LIBRARY_PATH`（xvm shims.rs 运行时展开）

**OS 演进三阶段**:
1. 用户空间工具管理器（当前）：`${XLINGS_HOME}` 变量替换
2. 多用户系统级（中期）：固定 `/xls/store/` 路径
3. xlings OS 原生包管理器（远期）：不可变系统根 + overlayfs

---

### [pkg-taxonomy.md](pkg-taxonomy.md) — 包分类体系设计

两个正交维度：
- **type**（获取方式）：`binary | fromsource | script | config | meta | template | scode | d2x`
- **source**（构建者）：`official | upstream | community`（仅 binary 类型有意义）

命名空间约定即为粗分类，xpkg 文件本身是元数据源，无需额外索引文件。

---

### [elf-relocation-and-subos-design.md](elf-relocation-and-subos-design.md) — ELF 可重定位与多 subos 设计

预构建包 ELF 解释器写死构建机路径导致子进程无法执行；同一 subos 内多版本依赖（A 依赖 b@0.0.1、C 依赖 b@0.0.2）会因 subos/lib 按文件名聚合而冲突。文档含：问题与方案总结一览、install 时 patchelf 到系统 loader、libxpkg 通用函数、包索引与预构建职责、多 subos 下 loader 与 LD_LIBRARY_PATH 分离、多版本依赖冲突与 Conda/Nix/npm/Spack 对比、**可参考方案详解**（Nix per-package 闭包、npm 式 per-program LD_LIBRARY_PATH）、**包索引分发的包 vs 基于 subos 视图构建的用户应用**（用户应用可能的依赖冲突及应对：注册到 xvm 闭包、RPATH 写死、默认视图约定）、LD_LIBRARY_PATH 副作用与应对、**分阶段建议方案**（短期/中期/长期）。

---

### [elfpatch-shrink-rpath-mode.md](elfpatch-shrink-rpath-mode.md) — elfpatch 可选 shrink-rpath 模式

在 install 后的自动 ELF patch 中引入可选 shrink 流程：先写入依赖闭包 RPATH，再通过 `patchelf --shrink-rpath` 收缩到最小必要路径。文档包含 API 兼容设计（`elfpatch.auto(true)` 与 `elfpatch.auto({ enable=true, shrink=true })`）、默认策略、gcc 示例与风险说明。

---

### [release-static-build.md](release-static-build.md) — Linux musl 静态构建方案

解决 Linux 发布二进制 glibc >= 2.38 依赖过高的问题。使用 `musl-gcc@15.1.0` 替代 `gcc@15.1` 作为构建工具链，配合 `-static` 全静态链接，生成零外部依赖的二进制。

**关键结论**:
- 当前 `gcc@15.1` 编译的二进制要求 glibc >= 2.38，排除 Ubuntu 22.04、Debian 12、RHEL 9 等
- `musl-gcc@15.1.0` + `-static` 生成完全静态二进制，零外部依赖，任意 Linux x86_64 可运行
- 与 xvm/xvm-shim（Rust musl 静态）策略一致，所有二进制统一为零依赖
- musl-gcc SDK 自带完整静态库；如缺失可从 gcc SDK 兜底查找

---

### [install-scripts-design.md](install-scripts-design.md) — 安装脚本方案与设计

安装体系分为两层：`quick_install`（一键在线安装/下载器）和 `xlings self install`（C++ 内置安装器）。

**关键结论**:
- 新版安装流程零编译依赖：下载预编译 release 包 → 拷贝到 XLINGS_HOME → 配置 PATH
- 旧版 `install.unix.sh` / `install.win.bat` 已废弃（依赖已移除的 `xmake task("xlings")` + `self enforce-install`）
- macOS 平台标识统一为 `macosx`（与 xmake 一致）
- 支持 `XLINGS_GITHUB_MIRROR` 环境变量指定镜像加速
- 支持 `XLINGS_HOME` 自定义安装路径

---

### [shim-unified-design.md](shim-unified-design.md) — 短命令与 xvm 配置全平台统一

将 xlings 短命令（xim、xsubos、xself 等）的生成逻辑统一到 `core/self/init.cppm`，由 `xlings self init` 统一负责；xvm 默认配置提取到 `config/xvm/`，打包时复制两次；macOS/Windows 与 Linux 一样打包 xmake；发布脚本改为调用 `xlings self init` 完成 shim 创建。

**关键结论**:
- 短命令单一数据源：SHIM_NAMES_BASE + SHIM_NAMES_OPTIONAL（xmake）
- config/xvm 单套模板，打包时复制到 config/xvm 与 subos/default/xvm
- 三平台均含 8 个短命令 shim

---

### [xim-dir-compat.md](xim-dir-compat.md) — xim 目录兼容方案（临时）

解决 `xlings install xlings` 多版本共存时，每个版本的 xlings 二进制应加载自身版本的 xim Lua 代码而非全局代码的问题。

**关键结论**:
- C++ 侧 `find_xim_project_dir()` 优先按可执行文件位置解析 xim 项目目录
- Lua 侧 `xmake.lua` 自动检测 `core/xim/`（源码树）和 `xim/`（release/包）两种布局
- release 脚本直接复制 `core/xim/xmake.lua`，消除内联重复
- 临时方案：xim 迁移到 C++ 后可移除

---

### [xpkgs-subos-hybrid-design.md](xpkgs-subos-hybrid-design.md) — data/xpkgs + subos 混合视图设计

明确 xlings 的双层模型：`data/xpkgs` 是固定真实包层，`subos` 是版本视图层；在此基础上同时支持聚合默认视图（`subos/lib`）与 per-program 闭包视图（直接指向 `data/xpkgs/<pkg>/<ver>/lib*`），并给出解析优先级、分阶段落地路径与验收标准。

---

### [hybrid-libpath-order-subplan.md](hybrid-libpath-order-subplan.md) — 混合视图子方案（RPATH 唯一真相与库路径优先级）

将“闭包优先、聚合作为 fallback”的路径解析顺序固化为 ELF RUNPATH 字段，由 `elfpatch` 在安装时写入。shim 层不再注入 `LD_LIBRARY_PATH`，消除环境变量传染。已废弃 `XLINGS_PROGRAM_LIBPATH` / `XLINGS_EXTRA_LIBPATH` 字段，仅 `musl-ldd`/`musl-loader` 作为已记录例外使用直接 `LD_LIBRARY_PATH`。

---

### [rpath-migration-summary.md](rpath-migration-summary.md) — RPATH 迁移方案总结与原理说明

完整的 RPATH 迁移方案总结，包含问题回顾、解决方案原理、d2x 包完整生命周期示例、4 级库路径优先级说明、多版本并存示例、musl-ldd 例外分析、CI 防线、与业界方案（Nix/Guix/Flatpak/Spack/Conda）对比。

---

## 快速索引

| 想了解 | 去看 |
|--------|------|
| 短命令与 xvm 配置统一（xim/xsubos 等 + config/xvm 模板） | [shim-unified-design.md](shim-unified-design.md) |
| 项目整体目标和迁移范围 | [main.md §1](main.md#一项目定位与迁移目标) |
| 新旧架构对比 | [env-analysis.md](env-analysis.md) |
| `xlings env` 命令设计 | [main.md §5](main.md#五多环境管理设计) |
| 多环境目录结构 | [env-store-design.md §3](env-store-design.md#三多环境目录设计) |
| 世代回滚和 GC | [env-store-design.md §6-7](env-store-design.md#六回滚流程) |
| C++23 模块列表 | [main.md §3](main.md#三c23-模块设计) |
| 代码风格规范 | [main.md §7](main.md#七代码风格规范mcpp-style-ref-落地) |
| RPATH 解决方案 | [rpath-and-os-vision.md §3](rpath-and-os-vision.md#三最简洁的解决方案变量替换--origin) |
| 包类型和命名规范 | [pkg-taxonomy.md](pkg-taxonomy.md) |
| elfpatch shrink-rpath 方案 | [elfpatch-shrink-rpath-mode.md](elfpatch-shrink-rpath-mode.md) |
| OS 演进路线 | [rpath-and-os-vision.md §4](rpath-and-os-vision.md#四xlings-作为操作系统包管理器的演进) |
| Linux 静态构建方案 | [release-static-build.md](release-static-build.md) |
| 安装脚本方案与设计 | [install-scripts-design.md](install-scripts-design.md) |
| xim 目录多版本兼容 | [xim-dir-compat.md](xim-dir-compat.md) |
| xpkgs/subos 混合视图方案 | [xpkgs-subos-hybrid-design.md](xpkgs-subos-hybrid-design.md) |
| 混合视图 RPATH 子方案 | [hybrid-libpath-order-subplan.md](hybrid-libpath-order-subplan.md) |
| RPATH 迁移方案总结与原理 | [rpath-migration-summary.md](rpath-migration-summary.md) |
| musl-gcc 构建任务 | [tasks/README.md](tasks/README.md) T11-T13 |
| 实施任务和并行分组 | [tasks/README.md](tasks/README.md) |

---

## 现状 Gap（设计 vs 实现）

| 模块 | 状态 | 说明 |
|------|------|------|
| `core/config.cppm` | 部分实现 | 已有基础配置，缺 activeEnv / XLINGS_PKGDIR / 自包含检测 |
| `core/cmdprocessor.cppm` | 部分实现 | 已有基础命令分发，缺 env / store gc 命令 |
| `core/platform/` | 部分实现 | 三平台已有，缺 `get_executable_path()` |
| `core/env.cppm` | 未实现 | 需新建 |
| `core/profile.cppm` | 未实现 | 需新建 |
| `core/xvm/xvmlib/shims.rs` | 已迁移 RPATH | 已移除 LD_LIBRARY_PATH 组装逻辑，库路径通过 elfpatch RPATH 解决，缺 `${XLINGS_HOME}` 展开 |
| `core/xim/pm/XPackage.lua` | 部分实现 | 已有 type 字段，缺 source / maintainer |
| CI/CD | 部分实现 | 已有 Linux，缺 macOS / Windows |
| `xmake.lua` Linux 链接 | 待切换 | glibc 动态 → musl 全静态（T11） |
| CI/Release Linux 工具链 | 待切换 | gcc@15.1 → musl-gcc@15.1.0（T12） |

详细的任务拆分和实施步骤见 [tasks/README.md](tasks/README.md)。