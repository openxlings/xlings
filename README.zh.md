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

## 为什么选 xlings?

一个工具,安装任意版本的任意软件,无需 root 运行,并提供 OS 级的环境隔离 —— 在 Linux / macOS / Windows 上统一。

"支持"是三个不同的问题，所以拆成三列。xlings 能跑的平台，不等于每个包都有
对应架构的产物；而这两件事都不说明 `--sandbox` 到底能隔离到什么程度。

| 平台 | xlings 发布产物 | 包生态覆盖 | `subos --sandbox` 隔离的是 |
|---|---|---|---|
| Linux x86_64 | ✅ 有 | 完整 | 文件系统（bwrap / proot） |
| Linux aarch64 | ✅ 有 | 部分 —— 不少配方只发布 x86_64 产物 | 文件系统，前提是该架构有可用后端 |
| macOS 14+ arm64 | ✅ 有 | 部分 —— 部分配方只发布 x86_64 | **仅 `$HOME`** |
| Windows x86_64 | ✅ 有 | 部分 | **仅 `%USERPROFILE%`** |

包生态覆盖是配方的属性，不是 xlings 的：只有当配方**逐架构列出**了自己的产物、
而其中没有你的架构时，`xlings install` 才会拒绝；配方只提供单一产物时照常安装，
并提示该配方无法确认你的架构。

> **macOS 与 Windows 的 `--sandbox` 不是安全边界。** 它重定向 home 目录，让工具
> 把 dotfile 写到隔离位置，但不隔离文件系统、网络或进程。要运行不受信任的代码，
> 请使用操作系统级沙箱或虚拟机。

→ [xlings 与 apt / nix / docker 对比](docs/comparison.md)

## 核心能力

1. **通用包管理基础设施** —— binary / script / config / subos / tutorial 统统是 xpkg
2. **多版本共存** —— 同一工具 N 个版本并存;版本视图 + 引用计数(N 个环境 ≈ 1 份存储)
3. **三级 SubOS 隔离** —— shell(env 切换)/ FS(bwrap/proot,无需 root)/ image(ext4,需 root)
4. **去中心化包索引** —— 官方 + 第三方 + 自建仓库;资源服务器做二进制镜像分发
5. **JSON 事件接口** —— `xlings interface`(NDJSON 协议)面向 AI Agent、CI 和第三方工具
6. **自我诊断** —— `xlings self doctor --fix` 检查四层状态并一次修完

## 快速开始

**安装 —— Linux / macOS**

```bash
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

**安装 —— Windows (PowerShell)**

```powershell
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

```bash
xlings install gcc@16 node@24 cmake   # 安装(版本可选)
xlings use gcc@16                      # 切换当前版本
xlings search python                   # 搜索包
xlings list                            # 查看已安装
```

### 让 AI Agent 帮你安装 & 讲解

把以下内容复制给任意 AI agent(Claude / Codex / OpenCode 等):

```
阅读 https://github.com/openxlings/xlings 的 README,并在我的机器上安装 xlings。
- Linux/macOS: curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
- Windows: irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
然后运行 `xlings agent usage`,按它的说明教我怎么用 xlings。
```

xlings 内置了面向 agent 的使用指南 —— 安装后,任意 agent 都能自助学习完整用法:

```bash
xlings agent           # 概览 + skill 列表
xlings agent usage     # 为 LLM agent 编写的完整使用指南
```

## 使用场景

### 🛠 多版本工具链,一套命令跨平台

安装任意版本的任意软件,多个版本并存、即时切换 —— 互不冲突,无需 root。同一套命令在 Linux / macOS / Windows 上通用。

```bash
xlings install gcc@16 gcc@11 node@24 cmake
xlings use gcc@11        # 随时切回 —— 两个版本都还在
```

→ [多版本管理](docs/quick-start/multi-version.md)

### 📦 可复现的项目环境,跨系统一致

提交一份按平台声明版本的 `.xlings.json`。每个项目拥有**独立的 SubOS**,团队和 CI 不论在哪个发行版/系统上,进入项目目录即得到完全一致的环境,不影响宿主机和其他项目。

```json
{
  "workspace": {
    "xmake": "3.0.7",
    "gcc":  { "linux":  "16.1.0" },
    "llvm": { "macosx": "20.1.7", "windows": "19.1.0" }
  }
}
```

```bash
cd my-project/           # 进入目录即激活项目级 SubOS
xlings install           # 按声明的版本装进项目级隔离环境
```

→ [项目环境](docs/quick-start/project-env.md)

### 🤖 在隔离 SubOS 中运行 Agent / 不受信代码

Linux 可在 rootless 文件系统隔离的 SubOS 中运行 Agent。macOS 和 Windows
仅重定向用户目录；若需运行不受信代码，请使用操作系统沙箱或虚拟机。

```bash
xlings subos new agent-ws --from subos:dev-env@latest
xlings subos use agent-ws --sandbox                       # 进入隔离世界
xlings subos use agent-ws --sandbox --cmd "python run.py" # 或一次性执行
```

→ [SubOS 与 Agent](docs/quick-start/subos-and-agent.md)

### 🩺 升级与修复，不用手改状态文件

升级客户端之后，让 xlings 自己检查四层状态 —— workspace、版本数据库、shim、payload
—— 并把能修的一次修完。你读到的报告是**修复之后**的状态，打印出来的命令都可以直接粘贴执行。

```bash
xlings self update                     # 升级 xlings 自己
xlings self doctor                     # 只读体检(退出码 0 表示健康)
xlings self doctor --fix --dry-run     # 先看会做什么
xlings self doctor --fix               # 修复
```

→ [自我管理与修复](docs/quick-start/self-management.md)

## 文档

详细的使用指南、设计文档与规范都在 [`docs/`](docs/)。

| 分类 | 文档 |
|------|------|
| **快速上手** | [多版本管理](docs/quick-start/multi-version.md) · [项目环境](docs/quick-start/project-env.md) · [SubOS 与 Agent](docs/quick-start/subos-and-agent.md) · [自定义索引](docs/quick-start/custom-index.md) · [自我管理与修复](docs/quick-start/self-management.md) · [从源码构建](docs/build-from-source.md) |
| **架构** | [系统架构概览](docs/architecture/overview.md) |
| **设计** | [SubOS-as-XPKG](docs/design/subos-as-xpkg.md) · [xvm 版本管理](docs/design/xvm-version-management.md) · [SubOS 隔离机制](docs/design/subos-isolation.md) · [包索引生态](docs/design/package-index-ecosystem.md) · [Interface 协议](docs/design/interface-protocol.md) |
| **规范** | [xpkg 包描述格式 v1](docs/spec/xpkg-manifest-v1.md) · [.xlings.json 字段](docs/spec/xlings-json-schema.md) · [Interface NDJSON v1](docs/spec/interface-ndjson-v1.md) |

## 生态

| 项目 | 角色 |
|------|------|
| [MCPP](https://github.com/mcpp-community/mcpp) | 现代 C++ 构建工具生态 —— 通过 xlings 分发 |
| **Luban Linux** | 即将推出的 Linux 发行版,采用 xlings 作为系统级包管理器 *(发布时更新链接)* |
| [xim-pkgindex](https://github.com/openxlings/xim-pkgindex) | 官方包索引 —— 60+ 个包持续增长 |

## 社区

- **论坛**: [forum.d2learn.org/category/9/xlings](https://forum.d2learn.org/category/9/xlings)
- **QQ 群**: 167535744 / 1006282943
- **Issues**: [github.com/openxlings/xlings/issues](https://github.com/openxlings/xlings/issues)

### 参与贡献

- [Issue 处理与 Bug 修复](https://xlings.d2learn.org/documents/community/contribute/issues.html)
- [添加新包](https://xlings.d2learn.org/documents/community/contribute/add-xpkg.html)
- [文档编写](https://xlings.d2learn.org/documents/community/contribute/documentation.html)

**贡献者**

<a href="https://github.com/openxlings/xlings/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=openxlings/xlings" />
</a>

[![Star History Chart](https://api.star-history.com/svg?repos=openxlings/xlings,openxlings/xim-pkgindex&type=Date)](https://star-history.com/#openxlings/xlings&openxlings/xim-pkgindex&Date)
