<div align=center>
  <img width="120" src="https://xlings.d2learn.org/imgs/xlings-logo.png">

  <h1>xlings</h1>

  <em>通用包管理基础设施 + OS-like SubOS 隔离<br/>
  多版本共存 · 无需 Root · 去中心化索引 · 面向 Agent</em>

  <b> [官网] | [文档] | [包索引] | [社区论坛] </b>

  中文 | [English](README.md)
</div>

[官网]: https://openxlings.github.io/
[文档]: docs/
[包索引]: https://openxlings.github.io/xim-pkgindex
[社区论坛]: https://forum.d2learn.org/category/9/xlings

<p align=center>
  <em>使用者: <a href="https://github.com/mcpp-community/mcpp">MCPP</a> · 即将推出的 <b>Luban</b> Linux</em>
</p>

一个工具，安装任意版本的任意软件，无需 root 运行，并像一个独立的 OS 一样隔离它 ——
在 Linux / macOS / Windows 上用法一致。

## 快速开始

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

```bash
xlings install gcc@16 node@24 cmake   # 安装（版本可省略）
xlings use gcc@16                      # 切换当前版本
xlings search python                   # 搜索包
xlings list                            # 已安装的包
```

用 AI Agent？让它运行 `xlings agent usage` —— xlings 自带一份写给 Agent 的完整使用指南。

## 能做什么

| 方面 | 能力 |
|---|---|
| **包** | 二进制、脚本、配置、SubOS 基础环境 —— 统一为 xpkg，来自官方、第三方或自建索引 |
| **版本** | 任意多个版本并存；N 个环境共享同一份安装产物 |
| **SubOS** | 有名字的环境：自己的工具、home 和策略 —— 从切换 PATH 到带私有网络的无 root 沙箱 |
| **Agent** | `xlings subos exec`、有含义的退出码、`XLINGS_AGENT_MODE=1`，以及 NDJSON 接口（`xlings interface`） |
| **修复** | `xlings self doctor --fix` 检查自身状态，一次修完 |

## 场景

**多套工具链，互不冲突** —— [指南](docs/quick-start/multi-version.md)

```bash
xlings install gcc@16 gcc@11 node@24
xlings use gcc@11        # 随时切回，两个版本都保留
```

**整个团队同一个环境** —— [指南](docs/quick-start/project-env.md)

```json
{ "workspace": { "xmake": "3.0.7", "gcc": { "linux": "16.1.0" }, "llvm": { "macosx": "20.1.7" } } }
```

```bash
cd my-project/ && xlings install     # 项目自己的 SubOS，装声明的版本
```

**在沙箱里运行 Agent 和不受信任的代码** —— [指南](docs/quick-start/subos-and-agent.md)

```bash
xlings subos new agent-ws --sandbox=private      # 隔离程度在创建时声明一次
xlings subos exec agent-ws -- python run.py      # 之后每次进入都是这个沙箱；返回命令自己的退出码
xlings subos use agent-ws                        # 或进入一个 shell，不用再加参数
```

| 平台 | 发布产物 | `--sandbox` 隔离的是 |
|---|---|---|
| Linux x86_64 / aarch64 | ✅ | 文件系统、进程、网络、身份（bwrap；aarch64 需该架构有可用的 bwrap） |
| macOS 14+ arm64 | ✅ | **仅 `$HOME`** |
| Windows x86_64 | ✅ | **仅 `%USERPROFILE%`** |

> macOS 与 Windows 的 `--sandbox` 不是安全边界：运行不受信任的代码请使用操作系统级沙箱或
> 虚拟机。`xlings subos status <name>` 会显示一个 SubOS 要求什么、这台机器实际给了什么。

**升级与修复** —— [指南](docs/quick-start/self-management.md)

```bash
xlings self update
xlings self doctor --fix
```

## 文档

| 类别 | 文档 |
|---|---|
| **指南** | [多版本](docs/quick-start/multi-version.md) · [项目环境](docs/quick-start/project-env.md) · [SubOS 与 Agent](docs/quick-start/subos-and-agent.md) · [自定义索引](docs/quick-start/custom-index.md) · [自我管理](docs/quick-start/self-management.md) · [从源码构建](docs/build-from-source.md) |
| **设计** | [架构](docs/architecture/overview.md) · [SubOS 隔离](docs/design/subos-isolation.md) · [SubOS-as-XPKG](docs/design/subos-as-xpkg.md) · [xvm 版本管理](docs/design/xvm-version-management.md) · [索引生态](docs/design/package-index-ecosystem.md) · [接口协议](docs/design/interface-protocol.md) |
| **规范** | [xpkg manifest v1](docs/spec/xpkg-manifest-v1.md) · [.xlings.json schema](docs/spec/xlings-json-schema.md) · [Interface NDJSON v1](docs/spec/interface-ndjson-v1.md) · [命令参考](docs/generated/command-reference.md) |
| **对比** | [xlings 与 apt / nix / docker](docs/comparison.md) |

## 生态与社区

- [MCPP](https://github.com/mcpp-community/mcpp) —— 现代 C++ 工具链，通过 xlings 分发
- [xim-pkgindex](https://github.com/openxlings/xim-pkgindex) —— 官方包索引
- **Luban Linux** —— 即将推出的发行版，用 xlings 作为系统包管理器
- [论坛](https://forum.d2learn.org/category/9/xlings) · QQ 群 167535744 / 1006282943 · [Issues](https://github.com/openxlings/xlings/issues)
- 参与贡献：[Issue 与 Bug 修复](https://xlings.d2learn.org/documents/community/contribute/issues.html) · [添加新包](https://xlings.d2learn.org/documents/community/contribute/add-xpkg.html) · [文档编写](https://xlings.d2learn.org/documents/community/contribute/documentation.html)

<a href="https://github.com/openxlings/xlings/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=openxlings/xlings" />
</a>

[![Star History Chart](https://api.star-history.com/svg?repos=openxlings/xlings,openxlings/xim-pkgindex&type=Date)](https://star-history.com/#openxlings/xlings&openxlings/xim-pkgindex&Date)
