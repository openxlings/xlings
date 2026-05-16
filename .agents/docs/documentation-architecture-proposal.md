# xlings 项目文档架构规划方案

**日期**: 2026-05-17
**状态**: 提案,等 review

---

## 现状分析

**原有文档**: 80+ 个 .md 文件,分布在 `docs/` 和 `.agents/docs/` 两处
**问题**:
1. 无分类体系 — plan/design/spec/history 混杂
2. 大量已执行完的 plan(30+)与现行设计文档混在一起
3. migration-era 文档(mcpp-version/ 26 个文件)已完成使命
4. 无"当前权威真相"标识 — 读者不知道哪些是最新的
5. `.agents/docs/` vs `docs/` 定位不清

**已完成**: 全部旧文档移入 `.agents/docs/old/`,项目文档清零重建。

---

## 新文档架构

### 设计原则

1. **代码是唯一真相** — 文档只记录代码不显而易见的"为什么"和"接口契约"
2. **单一入口** — 所有文档在 `docs/` 下,`.agents/docs/` 仅放 agent 工作草稿(不进 main)
3. **分层**: Architecture(大图)→ Design(子系统)→ Spec(接口契约)→ Guide(怎么用)
4. **可发现** — `docs/README.md` 做索引,链接到每一份
5. **时间标记** — 每份文档标明"最后验证日期",过期即 archive

### 目录结构

```
docs/
├── README.md                          ← 文档索引(导航入口)
│
├── architecture/                      ← 系统级大图
│   ├── overview.md                    ← 整体架构(模块图 + 数据流)
│   ├── module-map.md                  ← 源码模块一览表(每个 .cppm 一行)
│   └── data-layout.md                 ← 磁盘布局(xpkgs/ subos/ .xlings.json 等)
│
├── design/                            ← 子系统设计决策(living docs)
│   ├── xim-installer.md               ← 包安装流程:resolver → downloader → installer → xvm
│   ├── xvm-version-management.md      ← 多版本共存原理:版本视图 + 引用计数 + shim 分发
│   ├── subos-isolation.md             ← SubOS 三级隔离:shell / FS(bwrap/proot) / image
│   ├── subos-as-xpkg.md              ← type="subos" 包 + fork + --cmd(0.4.36)
│   ├── sandbox-backend.md            ← bwrap vs proot,setuid 设计,auto-install
│   ├── package-index-ecosystem.md    ← 去中心化:官方 + 三方 + 自建 + resource servers
│   ├── interface-protocol.md         ← xlings interface:NDJSON v1.0 协议 spec
│   ├── project-mode.md              ← .xlings.json 项目模式 + 自动 SubOS 激活
│   ├── self-update.md               ← xlings self update / self install 流程
│   └── elfpatch.md                   ← ELF 动态链接修补(runtime path patching)
│
├── guide/                             ← 面向使用者的操作指南
│   ├── quick-start.md                 ← 安装 + 第一个包
│   ├── multi-version.md               ← 多版本切换详解
│   ├── project-env.md                 ← 项目级环境(.xlings.json)
│   ├── subos-for-agents.md            ← Agent 在 SubOS 中运行(完整教程)
│   ├── create-xpkg.md               ← 写一个自己的 xpkg(含 type=subos 示例)
│   ├── custom-index.md               ← 搭建自定义包索引仓库
│   └── build-from-source.md           ← 从源码构建 xlings
│
├── spec/                              ← 接口契约(版本化,不轻易改)
│   ├── xpkg-manifest-v1.md           ← .lua 包描述格式 spec(字段、type、hook 约定)
│   ├── xlings-json-schema.md         ← .xlings.json 各字段语义
│   ├── interface-ndjson-v1.md        ← xlings interface NDJSON 协议(request/response/events)
│   └── eventstream-events.md         ← EventStream 事件清单(subos_created, installed, ...)
│
└── changelog/                         ← 版本 changelog(可选,或用 GitHub Releases)
    └── 0.4.36.md
```

### `.agents/docs/` 定位(内部工作区,不进 main README 索引)

```
.agents/docs/
├── subos-as-xpkg-design-2026-05-16.md    ← 当前活跃设计(验证后升级到 docs/design/)
├── old/                                    ← 归档(已执行的 plan、migration-era 文档等)
│   ├── docs/plans/...
│   └── agents-docs/...
└── (future working drafts...)
```

**规则**: `.agents/docs/` 是 agent 工作草稿区。一份文档一旦"定稿"并在代码中实现,应**升级到 `docs/design/` 或 `docs/spec/`**,并从 `.agents/docs/` 删除。

---

## 各层职责对比

| 层 | 内容 | 读者 | 更新频率 | 示例 |
|---|---|---|---|---|
| **architecture/** | 系统全景图、模块关系、数据流 | 新贡献者、架构评审 | 少(大改时) | overview.md |
| **design/** | 子系统"为什么这样做"、关键决策 | 开发者修 bug / 加 feature | 中(feature 落地时) | subos-isolation.md |
| **spec/** | 接口契约、格式标准、协议版本 | 包作者、agent 集成者 | 少(版本化,兼容性保证) | xpkg-manifest-v1.md |
| **guide/** | 使用教程、操作步骤 | 终端用户、agent 操作者 | 中(UI/CLI 变化时) | subos-for-agents.md |
| **changelog/** | 版本变更日志 | 所有人 | 每版 release | 0.4.36.md |

---

## 优先级建议(从哪开始写)

| 优先级 | 文档 | 理由 |
|:---:|---|---|
| **P0** | `docs/README.md`(索引) | 无入口 = 找不到任何文档 |
| **P0** | `docs/architecture/overview.md` | 新人第一个看的;现有 architecture.md 基本可升级 |
| **P0** | `docs/design/subos-as-xpkg.md` | 刚落地的 0.4.36 最大 feature |
| **P1** | `docs/spec/xpkg-manifest-v1.md` | 包作者最需要;从代码 + 老 xpkg-spec-design.md 可提取 |
| **P1** | `docs/spec/interface-ndjson-v1.md` | Agent 集成者最需要;从 interface.cppm + 老 plan 可提取 |
| **P1** | `docs/design/xvm-version-management.md` | "版本视图 + 引用计数"是差异化亮点,值得说清楚 |
| **P2** | `docs/guide/subos-for-agents.md` | Agent 教程;README 里有简版,这里是详版 |
| **P2** | `docs/guide/create-xpkg.md` | 包作者教程 |
| **P2** | `docs/design/sandbox-backend.md` | bwrap/proot 双后端选择逻辑 |
| **P3** | 其它 guide / design 补齐 | 按使用频率排序 |

---

## 迁移路径

1. **本 PR**: 
   - ✅ 旧文档已全部归档到 `.agents/docs/old/`
   - ✅ 新架构方案(本文件)

2. **下一步**(视你 review 结果):
   - 建 `docs/` 骨架目录 + `docs/README.md` 索引
   - P0 文档:升级 architecture.md + 把 subos-as-xpkg design 升到 `docs/design/`
   - P1 文档:从代码 + 老 docs 提取 spec

3. **长期**:
   - 每个 feature PR 配套 design/ 或 guide/ 文档
   - 季度 review:过期文档标 archive 或更新

---

## 与 README 的关系

```
README.md (项目首页,30 秒了解 + 核心场景)
    ↓ 链接
docs/README.md (文档索引,按层分类)
    ↓ 链接
docs/architecture/ · design/ · spec/ · guide/
```

README 是 landing page(卖点 + quick start),文档是 reference(深入)。两者不重复。
