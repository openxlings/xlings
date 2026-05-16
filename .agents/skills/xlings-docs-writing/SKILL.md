---
name: xlings-docs-writing
description: xlings 项目文档编写规范 — 面向人的 docs/ 目录文档应遵循的风格、格式和组织约定。Use when creating or modifying documents in the docs/ directory.
---

# xlings 文档编写规范

## 适用范围

`docs/` 目录下的所有文档。此规范不适用于 `.agents/docs/`(agent 工作区,格式自由)。

## 语言风格

- 使用中文
- 简洁的陈述性语句,学术/技术文档风格
- 不使用口语化表达、网络用语、推广性语句
- 不使用语气词("吧"、"呢"、"啊")
- 不使用 emoji(除 README.md 以外)
- 不添加面向 agent 的指令或元信息(如"约定"、"规则"段落)

## 文档结构

每份文档必须包含:

```markdown
# 标题

> 编写日期: YYYY-MM-DD | 版本: X.Y.Z

## 第一节
...
```

- 标题用一级标题 `#`
- 紧跟日期和版本标识(blockquote 格式)
- 正文从二级标题 `##` 开始

## 图表

- 所有图表使用 Mermaid 语法(GitHub 原生渲染)
- 支持的图表类型:graph、sequenceDiagram、flowchart、classDiagram
- 不使用 ASCII art

## 目录组织

```
docs/
├── README.md              文档索引(含快速了解 + 分章节目录)
├── architecture/          系统级全景
├── design/                子系统设计决策
├── spec/                  接口契约(版本化)
└── guide/                 面向使用者的操作指南
```

### 各层内容定位

| 层 | 回答什么问题 | 读者 |
|---|---|---|
| architecture | "系统整体是什么样的" | 初次了解项目的人 |
| design | "为什么这样设计" | 需要修改/扩展功能的开发者 |
| spec | "接口契约是什么" | 包作者、Agent 集成者 |
| guide | "怎么操作" | 终端用户 |

## docs/README.md 格式

1. 开头:一段话概述项目(无标题前缀)
2. "快速了解"表格:核心概念速查
3. 分章节目录:一、架构 → 二、设计 → 三、规范 → 四、指南
4. 不包含编写约定、agent 指令等元信息

## 文件命名

- 使用 kebab-case:`subos-as-xpkg.md`、`xvm-version-management.md`
- 不加日期前缀(日期在文档内部标注)
- 文件名应能独立表达内容主题

## 注意事项

- docs/ 目录面向人类读者,不放 agent 工作草稿
- agent 工作草稿放 `.agents/docs/`(命名格式 `YYYY-MM-DD-xxx.md`)
- 设计文档从 `.agents/docs/` 定稿后升级到 `docs/design/`
- 已规划但未编写的文档在 README.md 中用 *(待补充)* 标注
