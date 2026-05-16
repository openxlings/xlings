# 系统架构概览

> 编写日期: 2026-05-17 | 版本: 0.4.36

## 概述

xlings 是一个用 C++23 模块���现的单二进制包管理工具。提供多版本包管理、环境隔离(SubOS)和面向 Agent 的程序化接口。

## 模块关系

```mermaid
graph TD
    CLI[cli.cppm<br/>命令行入口] --> XIM[xim/<br/>包管理]
    CLI --> SUBOS[subos.cppm<br/>环境隔离]
    CLI --> XSELF[xself.cppm<br/>自管理]
    CLI --> IFACE[interface.cppm<br/>NDJSON 接口]

    XIM --> RESOLVER[resolver.cppm<br/>DAG 依赖解析]
    XIM --> DOWNLOADER[downloader.cppm<br/>并行下载]
    XIM --> INSTALLER[installer.cppm<br/>安装编排]
    XIM --> CATALOG[catalog.cppm<br/>多仓库目录]

    INSTALLER --> TYPE_PKG[type: package<br/>标准包]
    INSTALLER --> TYPE_SCRIPT[type: script<br/>脚本包]
    INSTALLER --> TYPE_SUBOS[type: subos<br/>环境��]

    SUBOS --> KEEPER[keeper.cppm<br/>命名空间复用]
    SUBOS --> XVM[xvm/<br/>版本管理]

    XVM --> SHIM[shim.cppm<br/>多版本分发]
    XVM --> DB[db.cppm<br/>版本数据库]

    CONFIG[config.cppm<br/>三层配置] --> CLI
    CONFIG --> XIM
    CONFIG --> SUBOS

    PLATFORM[platform.cppm<br/>跨平台抽象] --> CLI
    PLATFORM --> SUBOS
    PLATFORM --> XSELF

    style CLI fill:#e3f2fd
    style IFACE fill:#e8f5e9
    style SUBOS fill:#fff3e0
    style XIM fill:#f3e5f5
```

## 数据布局

```mermaid
graph LR
    subgraph "~/.xlings/ (XLINGS_HOME)"
        CFG[".xlings.json<br/>全局配置"]
        BIN["bin/xlings<br/>主二进制"]

        subgraph "data/"
            XPKGS["xpkgs/<br/>包载荷(共享, 引用计数)"]
            INDEX["xim-pkgindex/<br/>包索引仓库"]
        end

        subgraph "subos/"
            DEFAULT["default/<br/>默认 SubOS"]
            USER["&lt;name&gt;/<br/>用户创建的 SubOS"]
        end
    end

    XPKGS --> |"版本视图"| DEFAULT
    XPKGS --> |"版本视图"| USER
```

每个 SubOS 通过 workspace 元数据(.xlings.json)声明���见的包版本。包载荷存储在 `xpkgs/` 中,多个 SubOS 共享同一份物理文件(版本视图 + 引用计数)。

## 包安装流程

```mermaid
sequenceDiagram
    participant U as 用户/Agent
    participant R as Resolver
    participant D as Downloader
    participant I as Installer
    participant V as xvm

    U->>R: xlings install gcc@16
    R->>R: 解析依赖 DAG, 拓扑排序
    R->>D: 下载任务列表
    D->>D: 并行下载 + SHA256 校验
    D->>I: 下载完成
    I->>I: 类型分发(package/script/subos)
    I->>I: 解压 + 执行 install hook
    I->>V: 注册版本 + 创建 shim
    V->>U: 安装完成
```

## SubOS 隔离模型

```mermaid
graph TD
    subgraph "三级隔离"
        L1["Shell 级<br/>env/PATH 切换<br/>无需 root"]
        L2["FS 级<br/>bwrap / proot 沙箱<br/>无需 root"]
        L3["Image 级<br/>ext4 稀疏镜像<br/>需要 root"]
    end

    L1 --> |"更强隔离"| L2
    L2 --> |"更强隔离"| L3

    SHARED["xpkgs/ 共享存储"] --> L1
    SHARED --> L2
    SHARED --> L3

    style L1 fill:#e8f5e9
    style L2 fill:#e3f2fd
    style L3 fill:#fff3e0
```

| 级别 | 机制 | 需要 root | 隔离范围 | 适用场景 |
|------|------|:---------:|----------|----------|
| Shell | 环境变量 + PATH 操作 | 否 | 工具版本可见性 | 日常开发 |
| FS | bwrap(优先)/ proot 沙箱 | 否 | 文件系统(HOME, /tmp 私有)| Agent, 实验 |
| Image | ext4 稀疏文件 + loop 挂载 | 是 | ��设备级完整隔离 | 重型工作负载 |

三个级别共享同一个 `xpkgs/` 载荷存储。隔离通过 workspace 元数据(哪些版本可见)和文件系统命名空间(哪些路径���访问)实现。

## 核心设计原则

1. **单二进制** — 除 libc 外无运行时依赖(Linux release 使用 musl 静态链接)
2. **版本视图 + 引用计数** — N 个 SubOS 引用同一份物理包载荷,无重复���储
3. **类��驱动分发** — 包类型(package/script/subos)决定安装/配置/卸载行为
4. **去中心化索引** — 官方、第三方、自建仓库共存;资源服务器提供二进制镜像
5. **跨平台** — Linux / macOS / Windows 共用同一代码库,平台特定代码隔离在 `platform/`
