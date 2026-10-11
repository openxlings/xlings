# 架构

[English](../../luban/91-architecture.md) | **简体中文**

给开发者：Luban 是怎样组成的。luban 在 xlings 之上增加的东西刻意保持得很少。

## 代码在哪里

| 包 | 内容 |
|---|---|
| `luban/src/cli` | `luban` 命令：一棵声明式的命令树（common、more、expert 三级）；每个环境命令构造它对应的 xlings 命令，在同一个终端上运行 |
| `luban/src/image` | 进程内写出的镜像：GPT、支持长文件名的 FAT16、ISO 9660 的 El Torito 启动项与混合 GPT、qcow2；以及机器之间差异的表（UEFI 引导文件、有无 BIOS、qemu 机器、固件） |
| `luban/src/boot` | 启动项（`boot.json`：默认、fallback、试启动） |
| `luban/src/stage0` | 第一个进程；`/proc` 挂载之前什么都不读 |
| `luban/src/machine` | 机器的 `/etc`（factory 文件）与系统用户 |
| `apps/luban` | 二进制：luban，或以 `luban-init` 启动时的 stage-0 |
| `src/core/subos/root` | 根的 xlings 一侧：投影、代、回滚、启动、导出 |

`luban/` 引用 `modules/`，从不引用 xlings 前端：stage-0 路径上的东西不能读 Config。构建规则（`tools/lint_layer_deps.sh`）会检查这一点。

## 由 edition 创建环境

1. 读取 edition 的模板；在任何下载之前，按这台机器检查它的 ABI 和架构，并检查宿主（询问一次性设置）。
2. 创建根：edition 的 `from` 链，它的包作为一个计划安装（进程内，同一个终端），它的策略被选定并锁定。
3. `instance.json` 记录 edition：引用、链、每个包的安装版本、策略。`luban upgrade` 根据这份记录做计划：edition 带来且你没动过的会更新，你动过的保持不变。

## 镜像

1. 启动层装进根（它的 `share/luban/boot.json` 写明内核版本和命令行）。
2. 准备根：实例成为镜像的默认项，带上它的闭包。
3. 由 `luban.image` 在进程内写出镜像：驱动器是带 EFI 系统分区（FAT，在这里写出）和按 PARTUUID 挂载的 ext4 根的 GPT；
   ISO 是带 El Torito 启动项（机器有 BIOS 时有 BIOS 项，UEFI 项总有）和混合 GPT 的 ISO 9660，其 EFI 系统分区就是 El Torito 镜像。
4. x86_64 上 `limine bios-install` 让它能从驱动器以 BIOS 启动。

## 版本

luban 与它旁边的 xlings 一起发布，版本相同；edition、策略和启动层各自按日期编号，已发布版本的输出永不改变
（索引会检查每个已发布版本写出的文件）。
