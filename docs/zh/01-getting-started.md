# 快速开始

[English](../01-getting-started.md) | **简体中文**

## 安装

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

安装脚本把 home 放在 `~/.xlings`（Windows 为 `%USERPROFILE%\.xlings`），并把它的 `bin/` 加入 shell 的 PATH。
机器上的其他东西都不变。

在中国大陆，镜像设置让下载走 GitCode 而不是 GitHub：

```bash
xlings config --mirror CN
```

## 第一个包

```bash
xlings install gcc@16 node@24 cmake    # a version, or the latest
xlings use gcc@16                       # which version is active
xlings search python                    # what the indexes offer
xlings list                             # what is installed here
xlings remove cmake                     # uninstall
```

不写版本时安装索引提供的最新版。各平台命令相同；没有为你的机器构建的包，在下载之前就会被拒绝。

## 常见任务

| 任务 | 做法 |
|---|---|
| 同一个工具的多个版本 | `xlings install gcc@16 gcc@11`，然后 `xlings use`（[03](03-packages-and-versions.md)） |
| 项目自己的工具链 | 在项目里放 `.xlings.json`，然后 `xlings install`（[04](04-projects.md)） |
| 给 agent 的隔离环境 | `xlings subos new agent-ws --sandbox=private`（[10](10-subos.md)） |
| 在里面运行一条命令 | `xlings subos exec agent-ws -- python run.py` |
| 一整个 Linux 环境、镜像或机器 | `luban new box`（[Luban](luban/README.md)） |
| 升级 xlings | `xlings self update`（[06](06-self-management.md)） |
| 出了问题 | `xlings self doctor`，然后 `xlings self doctor --fix` |

## 接下来

- [场景](02-scenarios.md) —— 每类工作用到什么
- [包与版本](03-packages-and-versions.md)
- [SubOS](10-subos.md) 与 [Luban](luban/README.md)
- [命令参考](../generated/command-reference.md)
