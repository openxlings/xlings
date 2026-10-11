# 文档

[English](../README.md) | **简体中文**

这里是 **xlings 已实现功能的使用手册**：每一章说明一项能力做什么、怎么用、边界在哪里。
设计为什么是这样、被否决的方案、尚在计划中的工作，记录在设计文档里，不在这里。

## 文档放在哪里

| 目录 | 读者 | 内容 |
|---|---|---|
| `docs/**` | 手头有任务要完成的人 | 怎样使用 xlings 已实现的功能 |
| [`docs/luban/**`](luban/README.md) | 使用 Luban 环境、镜像或机器的人 | Luban OS：edition、环境、镜像、机器 |
| [`docs/specs/**`](../specs/README.md) | 按格式或协议实现的人：索引作者、agent 与工具的作者 | 格式、协议及其规则（英文） |
| [`docs/generated/**`](../generated/command-reference.md) | 查命令的人 | 由命令树生成的命令参考（英文） |
| `.agents/docs/**` | 修改 xlings 的人，以及日后想知道原因的人 | 设计记录、测量、被否定的方案 |

章节需要精确规则时指向规范；不指向设计记录——设计记录描述的是某一时刻，不作承诺，
读者需要的内容都写在这里或规范里。

## 从哪里开始

| 想要 | 阅读 |
|---|---|
| 安装 xlings 和第一个包 | [01](01-getting-started.md) |
| 同一个工具保留多个版本 | [03](03-packages-and-versions.md) |
| 给项目一套自己的工具链 | [04](04-projects.md) |
| 在沙箱里运行 agent 或不受信任的代码 | [10](10-subos.md)、[11](11-sandbox-and-policy.md)、[50](50-agents-and-machine-interface.md) |
| 做一个环境、镜像或自己的机器 | [Luban](luban/README.md) |
| 发布包，或自建索引 | [30](30-authoring-a-package.md)、[31](31-hosting-an-index.md) |
| 修复出问题的 home | [06](06-self-management.md) |
| 构建 xlings 本身 | [90](90-build-from-source.md) |

## 章节

编号的第一位是所属部分；部分之内按阅读顺序排列。

| | |
|---|---|
| `0x` | 基础 |
| `1x` | SubOS：环境、沙箱、根 |
| `3x` | 扩展 xlings：包、索引、模板 |
| `5x` | 程序可以依赖的约定 |
| `9x` | xlings 本身 |

### 0x — 基础

- [00 — xlings 是什么](00-what-xlings-is.md) —— 它做什么，与 apt、nix、docker 有何不同
- [01 — 快速开始](01-getting-started.md) —— 安装、第一个包、接下来读什么
- [02 — 场景](02-scenarios.md) —— 常见任务分别用到哪些功能
- [03 — 包与版本](03-packages-and-versions.md) —— 安装、切换、卸载；多版本共存
- [04 — 项目](04-projects.md) —— `.xlings.json`：项目自己的工具链
- [05 — 索引与镜像](05-indexes-and-mirrors.md) —— 包从哪里来
- [06 — 自我管理](06-self-management.md) —— 升级、体检、清理
- [09 — 按场景查命令](09-commands-by-scenario.md) —— 知道名词之后的速查

### 1x — SubOS

- [10 — SubOS](10-subos.md) —— 有名字的环境：创建、进入、运行、共享、删除
- [11 — 沙箱与策略](11-sandbox-and-policy.md) —— 隔离、网络、身份、授权、审计
- [12 — 根与代](12-roots-and-generations.md) —— 作为 `/` 的 SubOS：代、回滚、导出、启动
- [13 — macOS 与 Windows 上的 SubOS](13-subos-on-macos-and-windows.md) —— 各平台实际强制什么；Windows 上的 Linux SubOS

### 3x — 扩展 xlings

- [30 — 编写包](30-authoring-a-package.md) —— 一个 xpkg 配方
- [31 — 自建索引](31-hosting-an-index.md) —— 自己的包索引
- [32 — 编写 SubOS 模板](32-authoring-a-subos-template.md) —— `type = "subos"` 的包

### 5x — 给程序的约定

- [50 — Agent 与机器接口](50-agents-and-machine-interface.md) —— agent 模式、`--json`、NDJSON 接口
- [51 — 支持的平台](51-supported-platforms.md) —— 平台、机器需要提供什么、版本与兼容

### 9x — xlings 本身

- [90 — 从源码构建](90-build-from-source.md)
- [91 — 架构](91-architecture.md) —— 模块、数据布局、安装与 SubOS 的内部机制
- [92 — 发布](92-release.md)

## 速查

**命令**

| | 章节 | | 章节 |
|---|---|---|---|
| `install`、`use`、`remove`、`list`、`search` | [03](03-packages-and-versions.md) | `update`、`index` | [05](05-indexes-and-mirrors.md) |
| `self update`、`self doctor`、`self clean` | [06](06-self-management.md) | `config` | [05](05-indexes-and-mirrors.md)、[06](06-self-management.md) |
| `subos new / use / exec / start / cp / remove` | [10](10-subos.md) | `subos config / status / log / report` | [11](11-sandbox-and-policy.md) |
| `subos rollback / export / boot / pack` | [12](12-roots-and-generations.md) | `interface` | [50](50-agents-and-machine-interface.md) |
| 全部命令与参数 | [生成的参考](../generated/command-reference.md) | `luban …` | [luban](luban/README.md)、[生成的参考](../generated/luban-command-reference.md) |

**文件与字段**

| | 位置 |
|---|---|
| `.xlings.json`（home、项目、SubOS） | [04](04-projects.md)、[规范](../specs/xlings-json.md) |
| xpkg 配方（`pkgs/<x>/<name>.lua`） | [30](30-authoring-a-package.md)、[规范](../specs/xpkg-manifest-v1.md) |
| SubOS 策略（`policy.json`） | [11](11-sandbox-and-policy.md) |
| NDJSON 接口 | [50](50-agents-and-machine-interface.md)、[规范](../specs/interface-ndjson-v1.md) |
| 诊断信息与退出码 | [specs/diagnostics](../specs/diagnostics.md)、[10](10-subos.md) |
| 主题 | [specs/themes](../specs/themes.md) |

## 规范

- [specs/](../specs/README.md) —— 全部规范的索引（英文）
