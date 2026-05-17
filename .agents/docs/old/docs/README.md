# xlings 项目文档

## 项目简介

**xlings** 是一个跨平台通用包管理器，核心理念是 **"一切皆可成包"**。

- **当前版本**: v0.4.0
- **技术架构**: C++23 模块单二进制 multicall 架构
- **核心模块**:
  - **xim** — 安装管理（多版本共存、依赖解析、并行下载）
  - **xvm** — 版本管理（Shim 机制、版本切换）
- **环境隔离**: SubOS 隔离环境 + 项目级 `.xlings.json` 配置
- **平台支持**: Linux / macOS / Windows

**外部链接**:

| 资源 | 链接 |
|------|------|
| 官网 | https://xlings.d2learn.org |
| 包索引 | [xim-pkgindex](https://github.com/openxlings/xim-pkgindex) |
| 论坛 | https://forum.d2learn.org/category/9/xlings |
| QQ 群 | 167535744 / 1006282943 |

---

## 文档索引

### 入门与使用

| 文档 | 说明 | 状态 |
|------|------|------|
| [README.md](../README.md) | 英文快速开始、安装、使用示例 | 部分过时 |
| [README.zh.md](../README.zh.md) | 中文版 | 部分过时 |
| [changelog.md](../.agents/docs/changelog.md) | 变更日志 | 当前 |

### 架构与设计

| 文档 | 说明 | 状态 |
|------|------|------|
| [architecture.md](../.agents/docs/architecture.md) | 完整架构设计 | 当前 |
| [linux_adaptation_plan.md](../.agents/docs/linux_adaptation_plan.md) | Linux 适配方案 | 当前 |
| [log-system-design.md](../.agents/docs/log-system-design.md) | 日志系统设计 | 设计提案 |
| [shim-optimization-design.md](../.agents/docs/shim-optimization-design.md) | Shim 优化设计 | 设计提案 |

### C++23 迁移设计

| 文档 | 说明 | 状态 |
|------|------|------|
| [mcpp-version/README.md](../.agents/docs/mcpp-version/README.md) | C++23 迁移导航入口（25+ 设计文档，60+ 任务） | 部分过时 |

> 完整迁移设计文档列表请查看 [.agents/docs/mcpp-version/README.md](../.agents/docs/mcpp-version/README.md)

### 组件文档

| 文档 | 说明 | 状态 |
|------|------|------|
| [core/xim/README.md](../core/xim/README.md) | xim 模块架构与用法 | 当前 |
| [core/xvm/README.md](../core/xvm/README.md) | xvm 模块用法与迁移指南 | 当前 |

### XPackage 生态

#### libxpkg — 规范实现库

- **仓库**: [github.com/mcpplibs/libxpkg](https://github.com/mcpplibs/libxpkg)
- **简介**: C++23 独立库，负责解析/索引/搜索/执行 xpkg
- **四层模块**:
  - `xpkg` — 包模型定义
  - `loader` — xpkg 文件解析
  - `index` — 包索引与搜索
  - `executor` — 安装执行引擎
- **xlings 集成点**: `xim.index`、`xim.catalog`、`xim.installer`

#### xim-pkgindex — 官方包索引

- **仓库**: [github.com/openxlings/xim-pkgindex](https://github.com/openxlings/xim-pkgindex)
- **简介**: 61 个 xpkg 包定义 + V1 规范 + Pytest 测试框架
- **关键文档**:
  - [xpackage-spec.md](https://github.com/openxlings/xim-pkgindex/blob/main/docs/xpackage-spec.md) — xpkg 规范
  - [add-xpackage.md](https://github.com/openxlings/xim-pkgindex/blob/main/docs/add-xpackage.md) — 添加包指南
  - [test/design.md](https://github.com/openxlings/xim-pkgindex/blob/main/test/design.md) — 测试框架设计

#### xpkg 规范要点

- **格式**: Lua 文件，包含 `package` 表（静态元数据）和 hooks（运行时逻辑）
- **xpm 平台矩阵**: 按平台/版本定义下载资源
- **标准 hook**: `installed` / `install` / `config` / `uninstall`
- **内置 API**: `pkginfo`、`xvm`、`system`、`log`、`utils`、`json`、`elfpatch`、`pkgmanager`

### 测试

| 文档 | 说明 | 状态 |
|------|------|------|
| [tests/README.md](../tests/README.md) | 测试概览与运行方式 | 部分过时 |

### 开发者指南

| 文档 | 说明 | 状态 |
|------|------|------|
| [AGENTS.md](../AGENTS.md) | 构建、测试、平台指南 | 部分过时 |

### AI Agents 相关

| 目录 | 说明 |
|------|------|
| [.agents/skills/](../.agents/skills/) | Agent 操作技能（构建指南、使用指南、UI 设计） |
| [.agents/plans/](../.agents/plans/) | Agent 实施方案 |
| [.agents/tasks/](../.agents/tasks/) | Agent 任务拆分 |

> `.agents/` 是 AI Agent 的知识库，帮助 Agent 理解并贡献 xlings 项目。

---

## 贡献者指南

- **Issues 与 Bug 修复** — [官网指南](https://xlings.d2learn.org)
- **添加 XPackage 包** — [官网指南](https://xlings.d2learn.org) / [xim-pkgindex 仓库](https://github.com/openxlings/xim-pkgindex)
- **文档编写** — [官网指南](https://xlings.d2learn.org)
- **社区**: [论坛](https://forum.d2learn.org/category/9/xlings) / QQ 群 167535744 / 1006282943

---

## 文档状态说明

### 状态标签

| 标签 | 含义 |
|------|------|
| 当前 | 内容准确，与代码一致 |
| 部分过时 | 核心内容正确，部分细节需更新 |
| 过时 | 内容严重过时，仅供参考 |
| 设计提案 | 设计方案文档，未完全实现 |
| 规划中 | 尚未开始实现 |

### 已知问题

| 文档 | 问题 |
|------|------|
| `docs/quick_start.md` | 严重过时：引用 pre-v0.0.4 CLI（已归档） |
| `README.md` / `README.zh.md` | CAUTION 横幅说"迁移中"，但 xim/xvm 迁移已在 v0.4.0 完成 |
| `tests/README.md` | 测试数量与实际不符 |
| `AGENTS.md` | 仍引用已移除的 Rust xvm 构建命令 |
| `mcpp-version/README.md` | Gap 分析引用已删除的 `shims.rs` |
| `.agents/skills/xlings-build/SKILL.md` | 仍含 `cargo build` 命令 |

---

## 阅读指南

根据你的角色，推荐以下阅读路径：

**用户**:
[README.md](../README.md) → [官网](https://xlings.d2learn.org) → [xim README](../core/xim/README.md) → [xvm README](../core/xvm/README.md)

**开发者**:
[AGENTS.md](../AGENTS.md) → [architecture.md](../.agents/docs/architecture.md) → [tests/README.md](../tests/README.md) → [changelog.md](../.agents/docs/changelog.md)

**包贡献者**:
[xpackage-spec.md](https://github.com/openxlings/xim-pkgindex/blob/main/docs/xpackage-spec.md) → [add-xpackage.md](https://github.com/openxlings/xim-pkgindex/blob/main/docs/add-xpackage.md) → 示例包 → [测试框架](https://github.com/openxlings/xim-pkgindex/blob/main/test/design.md)

**设计贡献者**:
[architecture.md](../.agents/docs/architecture.md) → [mcpp-version/README.md](../.agents/docs/mcpp-version/README.md) → 设计文档 → [tasks/](../.agents/tasks/)
