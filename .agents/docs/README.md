# .agents/docs/

Agent 工作文档目录。存放设计草案、技术调研、实施方案等 agent 产出物。

## 目录结构

```
.agents/docs/
├── README.md                          ← 本文件
├── <date>-<topic>.md                  ← 活跃设计文档
├── documentation-architecture-proposal.md  ← 文档架构规划
└── old/                               ← 归档(已实施或过时的文档)
    ├── docs/                          ← 原 docs/ 下迁移的文档
    └── agents-docs/                   ← 原 .agents/docs/ 迁移的文档
```

## 命名规范

所有文档使用以下格式:

```
YYYY-MM-DD-<topic-slug>.md
```

示例:
- `2026-05-16-subos-as-xpkg-design.md`
- `2026-05-17-readme-redesign-plan.md`
- `2026-06-01-keeper-auto-spawn-integration.md`

规则:
- 日期为文档创建日期
- `<topic-slug>` 用 kebab-case,简短描述主题
- 设计文档后缀 `-design`,实施计划后缀 `-plan`,调研后缀 `-survey`

## 文档生命周期

1. **草稿**: 新建 `YYYY-MM-DD-xxx.md`,agent 在此迭代
2. **定稿**: 实现完成后,核心内容升级到 `docs/design/` 或 `docs/spec/`
3. **归档**: 不再维护的文档移入 `old/`

## 注意

- `.agents/docs/` 是 agent 工作区,**不出现在项目 README 的文档索引中**
- 不在这里写用户面向的文档(那些去 `docs/guide/`)
- 不重复代码中已经显而易见的信息
