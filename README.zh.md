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

---

## 为什么选 xlings?

一个工具,安装任意版本的任意软件,无需 root 运行,并提供 OS 级的环境隔离 —— 在 Linux / macOS / Windows 上统一。

| | apt / brew | nix | docker | **xlings** |
|---|:---:|:---:|:---:|:---:|
| 多版本共存 | ❌ | ✅ | ✅ | ✅ |
| 无需 Root | ❌ | ⚠️ | ⚠️ | ✅(image 模式除外)|
| 无 daemon | ✅ | ✅ | ❌ | ✅ |
| 跨平台统一命令 | ❌ | ⚠️ | ✅ | ✅ Linux / macOS / Windows |
| 隔离粒度 | ❌ | FS | FS+ | 🔒 shell / FS / image 三级 |
| 存储复用 | — | ✅ store | ❌ 镜像膨胀 | ✅ 版本视图 + 引用计数 |
| 去中心化索引 | ❌ | ❌ | ❌ | ✅ 官方 + 第三方 + 自建 |
| Agent / JSON 接口 | ❌ | ❌ | ⚠️ API | ✅ `xlings interface`(NDJSON)|
| 可作 OS 级包管理器 | apt 本身是 | NixOS | ❌ | ✅(Luban Linux,即将推出)|

---

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

---

## 使用场景

### 🛠 多版本工具链,免 sudo

多个版本并存,即时切换 —— 互不冲突,无需 root。

```bash
xlings install gcc@16 gcc@11
xlings use gcc@11        # 随时切回 —— 共存,不影响宿主机
```

→ [多版本管理](docs/quick-start/multi-version.md)

### 📦 可复现的项目环境

提交一份 `.xlings.json`,团队和 CI 拿到的就是同一套隔离环境。

```json
{
  "workspace": {
    "mcpp": "0.0.33",
    "gcc": { "linux": "16.1.0" },
    "llvm": { "macosx": "20.1.7" }
  }
}
```

```bash
cd my-project/           # 进入目录即激活项目级 SubOS
xlings install           # 依赖装进项目级隔离环境
```

→ [项目环境](docs/quick-start/project-env.md)

### 🤖 在隔离 SubOS 中运行 Agent / 不受信代码

把 claude / codex / opencode —— 或任意不受信代码 —— 跑在**隔离的 SubOS 内部**:内部拥有完全权限,宿主机不受影响。一台机器可跑多个隔离实例。

```bash
xlings subos new agent-ws --from subos:dev-env@latest
xlings subos use agent-ws --sandbox                       # 进入隔离世界
xlings subos use agent-ws --sandbox --cmd "python run.py" # 或一次性执行
```

→ [SubOS 与 Agent](docs/quick-start/subos-and-agent.md)

---

## 核心能力

1. 📦 **通用包管理基础设施** —— binary / script / config / subos / tutorial 统统是 xpkg
2. 🔀 **多版本共存** —— 同一工具 N 个版本并存;版本视图 + 引用计数(N 个环境 ≈ 1 份存储)
3. 🏗️ **三级 SubOS 隔离** —— shell(env 切换)/ FS(bwrap/proot,无需 root)/ image(ext4,需 root)
4. 🌐 **去中心化包索引** —— 官方 + 第三方 + 自建仓库;资源服务器做二进制镜像分发
5. 🤖 **JSON 事件接口** —— `xlings interface`(NDJSON 协议)面向 AI Agent、CI 和第三方工具

---

## 文档

详细的使用指南、设计文档与规范都在 [`docs/`](docs/)。

| 分类 | 文档 |
|------|------|
| **快速上手** | [多版本管理](docs/quick-start/multi-version.md) · [项目环境](docs/quick-start/project-env.md) · [SubOS 与 Agent](docs/quick-start/subos-and-agent.md) · [自定义索引](docs/quick-start/custom-index.md) |
| **架构** | [系统架构概览](docs/architecture/overview.md) |
| **设计** | [SubOS-as-XPKG](docs/design/subos-as-xpkg.md) · [xvm 版本管理](docs/design/xvm-version-management.md) · [SubOS 隔离机制](docs/design/subos-isolation.md) · [包索引生态](docs/design/package-index-ecosystem.md) · [Interface 协议](docs/design/interface-protocol.md) |
| **规范** | [xpkg 包描述格式 v1](docs/spec/xpkg-manifest-v1.md) · [.xlings.json 字段](docs/spec/xlings-json-schema.md) · [Interface NDJSON v1](docs/spec/interface-ndjson-v1.md) |

---

## 从源码构建

```bash
xlings install           # 读取 .xlings.json → 安装 mcpp 构建工具链
mcpp build
mcpp test
```

`.xlings.json` 同时驱动 CI 和 release 流水线。

---

## 生态

| 项目 | 角色 | 链接 |
|------|------|------|
| **MCPP** | 现代 C++ 构建工具生态 —— 通过 xlings 分发 | [github.com/mcpp-community/mcpp](https://github.com/mcpp-community/mcpp) |
| **Luban Linux** | 即将推出的 Linux 发行版,采用 xlings 作为系统级包管理器 | *(发布时更新链接)* |
| **xim-pkgindex** | 官方包索引 —— 60+ 个包持续增长 | [openxlings/xim-pkgindex](https://github.com/openxlings/xim-pkgindex) |

---

## 社区

- **论坛**: [forum.d2learn.org/category/9/xlings](https://forum.d2learn.org/category/9/xlings)
- **QQ 群**: 167535744 / 1006282943
- **Issues**: [github.com/openxlings/xlings/issues](https://github.com/openxlings/xlings/issues)

### 参与贡献

- [Issue 处理与 Bug 修复](https://xlings.d2learn.org/documents/community/contribute/issues.html)
- [添加新包](https://xlings.d2learn.org/documents/community/contribute/add-xpkg.html)
- [文档编写](https://xlings.d2learn.org/documents/community/contribute/documentation.html)

---

**贡献者**

<a href="https://github.com/openxlings/xlings/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=openxlings/xlings" />
</a>

[![Star History Chart](https://api.star-history.com/svg?repos=openxlings/xlings,openxlings/xim-pkgindex&type=Date)](https://star-history.com/#openxlings/xlings&openxlings/xim-pkgindex&Date)
