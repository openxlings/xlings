# 架构

[English](../../luban/91-architecture.md) | **简体中文**

给开发者：Luban 是怎样组成的。luban 在 xlings 之上增加的东西刻意保持得很少。

## 代码在哪里

`luban/` 里有三类包（`luban/README.md`）：

| 包 | 内容 |
|---|---|
| `luban/specs/opensal` | 表面：接收像素、返回事件；`opensal.wire` 跨地址空间传递 |
| `luban/specs/openhal` | 设备：一台机器有哪些 GPU、显示器、音频端点和输入设备 |
| `luban/specs/openas` | 主体与它们做的事：主体与委托、效果类别、一次运行的信封 |
| `luban/kits/openhal-host` | openhal 在 Linux、macOS、Windows 上的设备源 |
| `luban/kits/opensal-wlr`、`imkit`、`tray` | 把 wlroots 合成器接成 opensal 表面；控件；状态栏图标 |
| `luban/xdesk/{base,layout,compositor,app}` | xdesk 桌面：它的库与它的程序 |
| `luban/os/system` | stage-0（第一个进程）、启动项、机器的 `/etc`、归档 |
| `luban/os/service` | 环境与机器的操作与事件；这台电脑能做什么（`luban status`） |
| `luban/os/vm` | 机器：supervisor、终端中继、guest 会话、hypervisor |
| `luban/os/cli` | `luban` 命令：一棵声明式命令树；每个命令的效果类别；给程序用的 `luban interface` |
| `luban/os/window` | Luban OS 应用的窗口（SDL3）与 Dear ImGui |
| `luban/os/compat` | 只为已发布的旧版本保留的东西，各自写明何时删除 |
| `modules/image` | 进程内写出的镜像：GPT、FAT、带 El Torito 的 ISO 9660、qcow2 |
| `apps/luban` | 二进制：luban，或以 `luban-init` 启动时的 stage-0 |
| `apps/luban-os` | Luban OS 应用：它的窗口，以及托盘（`luban-os __tray`） |
| `src/core/subos/root` | 根的 xlings 一侧：投影、代、回滚、导出 |

规范只引用标准库和它自己；kit 从不引用 xlings 或 Luban；OS 服务不引用任何呈现层；`luban/` 里的任何东西都不引用
xlings 前端（stage-0 路径上的东西不能读 Config）。`tools/lint_layer_deps.sh` 逐条检查。

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

## 应用的进程

每个 Luban OS 窗口是一个独立的进程；另有一个 `luban-os __tray` 为所有窗口持有状态栏图标，除此之外什么都不画。每个 home 一个托盘：
持有 `<home>/run/luban-os/tray.lock` 的就是它。窗口经 `tray.sock`（本地 stream socket，只有属主能连；Windows 上是 AF_UNIX）与它通信，
每行一条 JSON：窗口说明自己是谁、在显示什么（`hello`、`register`），在做什么（`job`），要告诉人什么（`notify`）；托盘请一个窗口带着某个视图到前面
（`raise`）、询问是否退出（`confirm-quit`）、结束（`close`）。存活靠锁判断——每个窗口持有 `windows/<pid>.lock`——从不只靠 pid。
找不到托盘的窗口会启动一个，锁保证只有一个。没有窗口、没有运行中的系统、也没有*常驻状态栏*时托盘退出；装上更新版本后它立刻换成新版本。

## 版本

luban 与它旁边的 xlings 一起发布，版本相同；edition、策略和启动层各自按日期编号，已发布版本的输出永不改变
（索引会检查每个已发布版本写出的文件）。
