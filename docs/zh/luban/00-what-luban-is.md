# Luban 是什么

[English](../../luban/00-what-luban-is.md) | **简体中文**

## 能成立的最小系统

一个 Luban 系统是一个内核、xlings 和 luban。xlings 把包装进一个根，根的 `/usr` 是指向 payload 的一代链接；
luban 是系统这一侧——启动什么、哪个环境属于这台机器、镜像是什么。其余都是包，所以一个系统就是一组包，
而一组包就是任何人都能发布的模板。

## 同一个环境的三种运行方式

| 作为 | 怎么做 | 隔离 |
|---|---|---|
| 你机器上的沙箱 | `luban new box`、`luban run box -- …` | SubOS 沙箱（Linux 上是 bwrap） |
| 镜像 | `luban export box box.iso`（或 `.img`、`.qcow2`、`.tar.zst`） | 启动后有自己的内核 |
| 一台机器 | `luban write box /dev/sdX`，或启动一个镜像 | 硬件 |

三者里的环境相同：包、它的 `/etc`、它的策略。

## 用来做什么

- **不碰宿主的开发环境**：每个项目一套工具链，自己的 shell 和编辑器（`core`）。
- **Agent**：创建完成时就是私有的工作区——网络只有一个代理、有自己的身份、看不到你的 home（[04](04-agent-workspace.md)）。
- **镜像与设备**：用你做好的环境生成 live ISO 或驱动器，在 BIOS 和 UEFI 上启动（[05](05-images.md)）。
- **自己的发行版**：在自己的索引里发布自己的模板（[02](02-editions.md)）。

## 它不是什么

- 不是第二个包管理器：包是 xlings 的。
- 不是容器运行时：给 docker 或 WSL 的镜像是 `luban export box box.tar.zst`；luban 不运行容器。
- 不能在共享内核上藏住一切：共享内核藏不住的东西（内核版本、CPU、绑定进来的宿主路径）由 `luban status` 列出；
  需要自己的内核时用 `luban try`。
