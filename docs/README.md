# xlings 文档

xlings 是一个通用包管理基础设施,支持多版本共存、OS-like SubOS 环境隔离和去中心化包索引生态。可以作为操作系统的系统级包管理器使用。

## 目录

- **[一、快速开始](#一快速开始)**
  - [1.1 安装](#11-安装)
  - [1.2 基本使用](#12-基本使用)
  - [1.3 典型场景](#13-典型场景)
  - [1.4 SubOS 环境隔离](#14-subos-环境隔离)
  - [1.5 包索引](#15-包索引)
- **[二、使用指南](#二使用指南)**
  - [2.1 多版本管理](quick-start/multi-version.md)
  - [2.2 项目环境](quick-start/project-env.md)
  - [2.3 SubOS 与 Agent](quick-start/subos-and-agent.md)
  - [2.4 自定义包索引](quick-start/custom-index.md)
- **[三、高级主题](#三高级主题)**
  - [3.1 架构](#31-架构)
  - [3.2 设计](#32-设计)
  - [3.3 规范](#33-规范)

---

## 一、快速开始

### 1.1 安装

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash

# Windows PowerShell
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

### 1.2 基本使用

```bash
xlings install gcc@16 node@24 cmake    # 安装工具(支持指定版本)
xlings use gcc@16                       # 切换当前使用的版本
xlings search python                    # 搜索可用包
xlings list                             # 查看已安装的包
xlings remove gcc                       # 卸载
```

### 1.3 典型场景

| 场景 | 操作 |
|------|------|
| 多版本共存 | `xlings install gcc@16 gcc@11` 后通过 `xlings use` 切换 |
| 项目环境复现 | 在项目目录放 `.xlings.json` 声明依赖,`xlings install` 一键安装 |
| 为 Agent 创建隔离环境 | `xlings subos new agent-ws --from subos:dev-env@latest` |
| 进入隔离的 SubOS | `xlings subos use agent-ws --sandbox` |
| 在 SubOS 中执行命令 | `xlings subos use agent-ws --sandbox --cmd "python run.py"` |

### 1.4 SubOS 环境隔离

SubOS 提供三级隔离,满足从日常开发到 Agent 安全执行的不同需求:

| 级别 | 隔离范围 | 需要 root | 适用场景 |
|------|----------|:---------:|----------|
| Shell | 工具版本 | 否 | 日常开发、版本切换 |
| FS | 文件系统(HOME, /tmp) | 否 | Agent 运行、实验、不信任代码 |
| Image | 块设备完整隔离 | 是 | 重型工作负载 |

### 1.5 包索引

支持同时使用多个包索引仓库:

- 官方索引:`openxlings/xim-pkgindex`
- 第三方社区索引:任何人可以创建
- 自建私有索引:团队内部使用

---

## 二、使用指南

详细的操作说明,按主题组织。

| 文档 | 内容 |
|------|------|
| [多版本管理](quick-start/multi-version.md) | 安装、切换、共存原理 |
| [项目环境](quick-start/project-env.md) | .xlings.json 配置、一键安装、项目级 SubOS |
| [SubOS 与 Agent](quick-start/subos-and-agent.md) | 创建隔离环境、运行 Agent、多实例 |
| [自定义包索引](quick-start/custom-index.md) | 搭建私有仓库、添加第三方索引 |
| [从源码构建](build-from-source.md) | 用 mcpp 构建 xlings 本身 |
| [与其他工具对比](comparison.md) | xlings vs apt / nix / docker |

---

## 三、高级主题

面向需要了解内部实现或参与开发的读者。

### 3.1 架构

| 文档 | 内容 |
|------|------|
| [系统架构概览](architecture/overview.md) | 模块关系、数据布局、安装流程、隔离模型 |

### 3.2 设计

| 文档 | 内容 |
|------|------|
| [SubOS-as-XPKG](design/subos-as-xpkg.md) | type="subos" 包格式、fork 机制、非交互执行 |
| [xvm 版本管理](design/xvm-version-management.md) | 版本视图 + 引用计数实现多版本共存 |
| [SubOS 隔离机制](design/subos-isolation.md) | 三级隔离(shell / FS / image)的实现细节 |
| [包索引生态](design/package-index-ecosystem.md) | 去中心化索引设计 |
| [Interface 协议](design/interface-protocol.md) | `xlings interface` NDJSON 通信协议 |

### 3.3 规范

| 文档 | 内容 |
|------|------|
| [xpkg 包描述格式 v1](spec/xpkg-manifest-v1.md) | .lua 包描述文件的字段、type、hook 约定 |
| [.xlings.json 字段](spec/xlings-json-schema.md) | 配置文件各字段语义 |
| [Interface NDJSON v1](spec/interface-ndjson-v1.md) | 请求/响应/事件协议 |
