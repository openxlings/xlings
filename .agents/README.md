# .agents/

AI Agent 工作空间。存放 agent 使用的 skills、工作文档、实施计划和任务状态。

## 目录结构

```
.agents/
├── README.md          ← 本文件
├── docs/              ← Agent 工作文档(设计草案、调研、方案)
│   ├── README.md      ← 命名规范 + 生命周期说明
│   └── old/           ← 归档(已实施或过时的文档)
├── skills/            ← Agent 技能定义(SKILL.md 格式)
│   ├── xlings-usage/         ← xlings 完整使用指南
│   ├── xlings-contributing/  ← 项目贡献规范流程
│   ├── xlings-build/         ← 三平台构建指南
│   ├── xlings-quickstart/    ← 快速入门(legacy)
│   ├── system-design/        ← 系统设计 skill
│   ├── mcpp-style-ref/       ← C++ 代码风格参考
│   └── ui-ux-pro-max/        ← UI/UX 设计 skill
├── plans/             ← 实施计划(agent 生成的逐步执行方案)
└── tasks/             ← 任务状态跟踪(运行时产物)
```

## 各子目录职责

| 目录 | 用途 | 谁写 | 生命周期 |
|------|------|------|----------|
| `docs/` | 设计文档、技术调研、架构方案 | Agent | 草稿 → 定稿升级到 `docs/` → 归档到 `old/` |
| `skills/` | Agent 技能(指导 agent 如何执行特定任务) | 人 + Agent | 长期维护,随项目演进更新 |
| `plans/` | 从设计文档拆解出的逐步实施计划 | Agent | 执行完即归档或删除 |
| `tasks/` | 运行时任务状态(自动生成) | Agent runtime | 临时,可随时清理 |

## 约定

- `.agents/` 下的内容是 **agent 工作产物**,不出现在用户面向的 README 索引中
- Skill 文件固定格式:`<skill-name>/SKILL.md`(YAML frontmatter + markdown body)
- 文档命名:`YYYY-MM-DD-<topic-slug>.md`
- 定稿的设计文档应升级到项目根 `docs/` 目录(architecture/ design/ spec/ guide/ 分层)
