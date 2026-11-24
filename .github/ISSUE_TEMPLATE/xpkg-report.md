---
name: xpkg report | 贡献 xpkg 时发现的问题
about: A problem in xlings found while adding or updating an xpkg (a package definition)
title: "[xpkg-report]: "
labels: xpkg-report
assignees: ''

---

<!-- Before posting, remove local paths, user names, e-mail addresses and tokens: an edit keeps the old text in the history. | 提交前请去掉本地路径、用户名、邮箱和 token：编辑会在历史中保留旧文本。 -->

<!-- Filed by people, or by an agent contributing to openxlings/xim-pkgindex. An xpkg's own
mistake is not a report. Search existing issues first and comment on a match. -->

### Context | 背景

Found while contributing `<namespace>:<name>` (link the pull request) | 贡献哪个包时发现（附 PR）:

### Versions | 版本

xlings version, and the version xim-pkgindex's CI pins; OS and architecture |
xlings 版本、xim-pkgindex CI 固定的版本、系统与架构:

### Reproduce | 复现

The smallest commands, in an isolated home | 最小复现命令，使用隔离的 home:

```bash
XLINGS_HOME="$(mktemp -d)" xlings install <name>@<version> -y
```

### Expected / actual | 预期与实际

### Analysis | 分析

Observed (facts) | 观察到的事实:

Suspected (a guess, marked as one) | 推测（标明是推测）:

Ruled out | 已排除:

### Authorship | 作者
Authorship: <by-human | human-and-ai | by-ai>
Agent: <optional | 可选>
Model: <optional | 可选>
