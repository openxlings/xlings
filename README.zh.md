<div align=center>
  <img width="120" src="https://xlings.d2learn.org/imgs/xlings-logo.png">

  <h1>xlings</h1>

  <em>通用包管理基础设施 + OS-like SubOS 隔离<br/>
  多版本共存 · 无需 Root · 去中心化索引 · 面向 Agent</em>

  <b> [官网] | [文档] | [包索引] | [社区论坛] </b>

  中文 | [English](README.md)
</div>

[官网]: https://openxlings.github.io/
[文档]: docs/zh/README.md
[包索引]: https://openxlings.github.io/xim-pkgindex
[社区论坛]: https://forum.d2learn.org/category/9/xlings

<p align=center>
  <em>使用者: <a href="https://github.com/mcpp-community/mcpp">MCPP</a> · <a href="docs/zh/luban/README.md"><b>Luban</b></a></em>
</p>

一个工具，安装任意版本的任意软件，无需 root 运行，并像一个独立的 OS 一样隔离它 ——
在 Linux / macOS / Windows 上用法一致。

## 快速开始

### xlings

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

```bash
xlings install mcpp luban node   # 安装（最新版本）
xlings install node@20           # 再装一个老版本，与新版本并存
xlings use node@20               # 切换当前版本
xlings search gcc                # 搜索包
xlings list                      # 已安装的包
```

用 AI Agent？让它运行 `xlings agent usage` —— xlings 自带一份写给 Agent 的完整使用指南。

### Luban OS

`luban`（命令）与 Luban OS（窗口）是各自独立的包：先安装，再创建一个系统并进入。

```bash
xlings install luban luban-os    # the luban command and the Luban OS window
luban new box                    # a Linux system, no root needed (Luban Core)
luban enter box                  # its shell; exit to leave
luban-os                         # or open the Luban OS window
```

## 能做什么

| 方面 | 能力 |
|---|---|
| **包** | 二进制、脚本、配置、SubOS 基础环境 —— 统一为 xpkg，来自官方、第三方或自建索引 |
| **版本** | 任意多个版本并存；N 个环境共享同一份安装产物 |
| **SubOS** | 有名字的环境：自己的工具、home 和策略 —— 从切换 PATH 到带私有网络的无 root 沙箱 |
| **Agent** | `xlings subos exec`、有含义的退出码、`XLINGS_AGENT_MODE=1`，以及 NDJSON 接口（`xlings interface`） |
| **修复** | `xlings self doctor --fix` 检查自身状态，一次修完 |

## 场景

**多套工具链，互不冲突** —— [指南](docs/zh/03-packages-and-versions.md)

```bash
xlings install gcc@16 gcc@11 node@24
xlings use gcc@11        # 随时切回，两个版本都保留
```

**整个团队同一个环境** —— [指南](docs/zh/04-projects.md)

```json
{ "workspace": { "xmake": "3.0.7", "gcc": { "linux": "16.1.0" }, "llvm": { "macosx": "20.1.7" } } }
```

```bash
cd my-project/ && xlings install     # 项目自己的 SubOS，装声明的版本
```

**在沙箱里运行 Agent 和不受信任的代码** —— [指南](docs/zh/10-subos.md)

```bash
xlings subos new agent-ws --sandbox=private      # 隔离程度在创建时声明一次
xlings subos exec agent-ws -- python run.py      # 之后每次进入都是这个沙箱；返回命令自己的退出码
xlings subos use agent-ws                        # 或进入一个 shell，不用再加参数
```

| 平台 | 发布产物 | `--sandbox` 隔离的是 |
|---|---|---|
| Linux x86_64 / aarch64 | ✅ | 文件系统、进程、网络、身份（bwrap） |
| macOS 14+ arm64 | ✅ | **仅 `$HOME`** |
| Windows x86_64 | ✅ | **仅 `%USERPROFILE%`** |

> macOS 与 Windows 的 `--sandbox` 不是安全边界：运行不受信任的代码请使用操作系统级沙箱或
> 虚拟机。`xlings subos status <name>` 会显示一个 SubOS 要求什么、这台机器实际给了什么。

**升级与修复** —— [指南](docs/zh/06-self-management.md)

```bash
xlings self update
xlings self doctor --fix
```

## Luban

一条命令得到完整的 Linux 用户态，无需 root——然后把它当作机器启动。Luban 是建立在 xlings 上的 OS 模型：
由 edition 创建环境、按代回滚、导出为镜像，并连同桌面显示在 Luban OS 窗口里。
[快速开始](docs/zh/luban/01-quick-start.md) · [桌面](docs/zh/luban/08-desktop.md)

```bash
xlings install luban  # the luban command (luban-os: the window)
luban new box         # a Linux system, no root needed (Luban Core: shells, editors, git, compilers)
luban enter box       # its shell; exit to leave
```
更小的或带桌面的 edition（`--edition tiny`、`--edition desktop`）、用自己的内核把同一个系统作为机器启动
（`--mode machine`）、可启动镜像（`luban export`）、Luban OS 窗口、macOS 与 Windows：[快速开始](docs/zh/luban/01-quick-start.md)。


## 文档

| 类别 | 文档 |
|---|---|
| **开始** | [xlings 是什么](docs/zh/00-what-xlings-is.md) · [快速开始](docs/zh/01-getting-started.md) · [场景](docs/zh/02-scenarios.md) · [按场景查命令](docs/zh/09-commands-by-scenario.md) |
| **使用** | [包与版本](docs/zh/03-packages-and-versions.md) · [项目](docs/zh/04-projects.md) · [索引与镜像](docs/zh/05-indexes-and-mirrors.md) · [自我管理](docs/zh/06-self-management.md) |
| **SubOS** | [SubOS](docs/zh/10-subos.md) · [沙箱与策略](docs/zh/11-sandbox-and-policy.md) · [根与代](docs/zh/12-roots-and-generations.md) · [macOS 与 Windows](docs/zh/13-subos-on-macos-and-windows.md) |
| **扩展** | [编写包](docs/zh/30-authoring-a-package.md) · [自建索引](docs/zh/31-hosting-an-index.md) · [SubOS 模板](docs/zh/32-authoring-a-subos-template.md) |
| **约定** | [Agent 与机器接口](docs/zh/50-agents-and-machine-interface.md) · [支持的平台](docs/zh/51-supported-platforms.md) · [规范（英文）](docs/specs/README.md) · [命令参考](docs/generated/command-reference.md) |
| **Luban** | [Luban OS](docs/zh/luban/README.md) —— 建在 xlings 上的环境、edition、镜像与机器 |
| **xlings 本身** | [从源码构建](docs/zh/90-build-from-source.md) · [架构](docs/zh/91-architecture.md) · [发布](docs/zh/92-release.md) |

全部文档：[docs/zh/](docs/zh/README.md)。

## 生态与社区

- [MCPP](https://github.com/mcpp-community/mcpp) —— 现代 C++ 工具链，通过 xlings 分发
- [xim-pkgindex](https://github.com/openxlings/xim-pkgindex) —— 官方包索引
- [Luban](docs/zh/luban/README.md) —— 建在 xlings 上的 OS 模型：环境、edition、可启动镜像
- [论坛](https://forum.d2learn.org/category/9/xlings) · QQ 群 167535744 / 1006282943 · [Issues](https://github.com/openxlings/xlings/issues)
- 参与贡献：[Issue 与 Bug 修复](https://xlings.d2learn.org/documents/community/contribute/issues.html) · [添加新包](https://xlings.d2learn.org/documents/community/contribute/add-xpkg.html) · [文档编写](https://xlings.d2learn.org/documents/community/contribute/documentation.html)

<a href="https://github.com/openxlings/xlings/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=openxlings/xlings" />
</a>

[![Star History Chart](https://api.star-history.com/svg?repos=openxlings/xlings,openxlings/xim-pkgindex&type=Date)](https://star-history.com/#openxlings/xlings&openxlings/xim-pkgindex&Date)
