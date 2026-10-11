# xlings 是什么

[English](../00-what-xlings-is.md) | **简体中文**

xlings 把包管理器和环境管理器放在一个静态二进制里，运行在 Linux、macOS 和 Windows 上。
它不需要 root 就能安装任何东西的任何版本，让所有版本并存，并提供自成一体的环境——
从一次 PATH 切换，到带私有网络的无 root 沙箱，再到一整个操作系统（Luban）。

## 它做什么

| 方面 | 你得到什么 |
|---|---|
| **包** | 二进制、脚本、配置、SubOS 模板，统一为一种格式（xpkg），来自官方索引、第三方索引或你自己的索引 |
| **版本** | 任意多个版本并存；N 个环境共享每个 payload 的同一份拷贝 |
| **SubOS** | 有名字的环境：自己的工具、home 和策略——PATH 叠加、沙箱，或者一个本身就是整个系统的根 |
| **Agent** | 从不等待输入的 agent 模式、每个含义唯一的退出码、`--json`，以及 NDJSON 接口（`xlings interface`） |
| **修复** | `xlings self doctor --fix` 一次检查并修复自身状态 |

## 保证

- **一个二进制。** Linux 发布版是静态构建：运行时不需要机器上的任何东西，home 可以搬到一台没有任何库的机器上。
- **不需要 root。** 安装、切换、沙箱都不需要管理员。少数只有管理员能做的事（Ubuntu 23.10+ 上的一次性沙箱设置、系统层），
  由需要它的命令当场询问一次。
- **不重复安装。** 一个 payload 在 home 的 store 里只存一份；每个环境持有自己所用版本的视图。
- **它运行的东西来自索引。** xlings 用到的每个外部程序都是索引里的包，操作系统本身除外（[51](51-supported-platforms.md)）。

## 与其他工具对比

| | apt / brew | nix | docker | **xlings** |
|---|:---:|:---:|:---:|:---:|
| 多版本并存 | 否 | 是 | 是 | 是 |
| 不需要 root | 否 | 部分 | 部分 | 是 |
| 没有 daemon | 是 | 是 | 否 | 是 |
| Linux、macOS、Windows 同一套命令 | 否 | 部分 | 是 | 是 |
| 隔离 | 无 | 文件系统 | 容器 | PATH 叠加、沙箱、根 |
| 环境之间共享存储 | — | store | 镜像层 | 同一个 store 上的版本视图 |
| 去中心化索引 | 否 | 否 | 否 | 官方、第三方、自建 |
| Agent / JSON 接口 | 否 | 否 | API | `xlings interface`（NDJSON） |
| 作为操作系统的包管理器 | apt 本身是 | NixOS | 否 | Luban |

## 接下来

[快速开始](01-getting-started.md)，然后是[场景](02-scenarios.md)。
