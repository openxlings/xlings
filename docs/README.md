# xlings 文档

xlings 是一个通用包管理基础设施，支持多版本共存、SubOS 环境隔离和去中心化包索引。它可以在 Linux、macOS 和 Windows 上以单一二进制运行，并提供面向 Agent 的 NDJSON 接口。

| 发布目标 | 包管理 | Sandbox 边界 |
|---|---|---|
| Linux x86_64 / aarch64 | 支持 | bwrap / proot 文件系统隔离 |
| macOS 14+ arm64 | 支持 | 仅 HOME 重定向 |
| Windows x86_64 | 支持 | 仅 USERPROFILE 重定向 |

macOS 与 Windows 的重定向不是不受信代码的安全边界。

## 快速了解

| 概念 | 一句话说明 | 入口 |
|---|---|---|
| 多版本共存 | 同一工具可安装多个版本，通过 shim 在环境内切换 | [多版本管理](guide/multi-version.md) |
| 项目环境 | `.xlings.json` 声明依赖，项目获得独立 SubOS | [项目环境](guide/project-env.md) |
| SubOS | shell、文件系统、镜像三级隔离环境 | [SubOS 隔离模型](design/subos-isolation.md) |
| xim | 包索引、依赖解析、下载与安装子系统 | [包索引生态](design/package-index-ecosystem.md) |
| xvm | 版本视图、引用计数与 shim 分发子系统 | [xvm 版本管理](design/xvm-version-management.md) |
| xpkg | 包描述与资源声明格式 | [xpkg 规范 v1](spec/xpkg-manifest-v1.md) |
| interface | 面向 Agent、CI 与 IDE 的 NDJSON 协议 | [NDJSON v1](spec/interface-ndjson-v1.md) |
| self | 客户端升级、体检、修复与清理 | [自我管理与修复](guide/self-management.md) |

## 一、架构

| 文档 | 内容 |
|---|---|
| [系统架构概览](architecture/overview.md) | 模块关系、数据布局、安装流程与隔离模型 |

## 二、设计

| 文档 | 内容 |
|---|---|
| [SubOS-as-XPKG](design/subos-as-xpkg.md) | `type="subos"` 包格式、fork 机制与非交互执行 |
| [SubOS 隔离模型](design/subos-isolation.md) | shell / FS / image 三级隔离与存储模式 |
| [xvm 版本管理](design/xvm-version-management.md) | 版本视图、引用计数与 shim 分发 |
| [包索引生态](design/package-index-ecosystem.md) | 多源索引、命名空间与资源服务器 |
| [索引分发](design/index-distribution.md) | 索引工件的获取、区域回退与网络边界 |
| [索引版本契约](design/index-version-contract.md) | 索引快照对客户端版本的要求与发布方流程 |
| [xpkg 资源解析](design/xpkg-resource-resolution.md) | `xpm.source`、多架构 SHA256 与兼容规则 |
| [Interface 协议](design/interface-protocol.md) | `xlings interface` 的事件与会话设计 |

## 三、规范

| 文档 | 内容 |
|---|---|
| [xpkg 包描述格式 v1](spec/xpkg-manifest-v1.md) | 包字段、类型、hook、依赖与资源矩阵 |
| [.xlings.json 字段](spec/xlings-json-schema.md) | 全局、项目与 SubOS 配置字段 |
| [Interface NDJSON v1](spec/interface-ndjson-v1.md) | 请求、事件、能力与退出码契约 |
| [诊断信息规约](spec/diagnostics.md) | 诊断字段、严重级、交互与稳定错误码 |
| [配色主题](spec/themes.md) | 角色槽、部分覆盖与主题加载规则 |
| [命令参考](generated/command-reference.md) | 由 `CommandSpec` 生成并经 CI 校验 |

## 四、指南

| 文档 | 内容 |
|---|---|
| [多版本管理](guide/multi-version.md) | 安装、切换、卸载与坐标写法 |
| [项目环境](guide/project-env.md) | 项目声明、SubOS 模式与团队协作 |
| [SubOS 与 Agent](guide/subos-and-agent.md) | 创建隔离环境、运行 Agent 与多实例 |
| [自定义包索引](guide/custom-index.md) | 第三方索引、artifact 与私有部署 |
| [自我管理与修复](guide/self-management.md) | `self update`、`self doctor` 与清理 |
| [配色主题](guide/themes.md) | 主题设置、自定义与故障回退 |
| [从源码构建](guide/build-from-source.md) | 使用 mcpp 构建与测试 xlings |
| [与其他工具对比](guide/comparison.md) | 与 apt、nix、docker 的定位对比 |
