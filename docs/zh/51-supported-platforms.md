# 支持的平台

[English](../51-supported-platforms.md) | **简体中文**

## 平台

| 平台 | 发布 | 包 | SubOS 沙箱 | Luban 根与镜像 |
|---|---|---|---|---|
| Linux x86_64 | 静态（musl） | 是 | bwrap、landlock、proot | 是；镜像在 BIOS 和 UEFI 上启动 |
| Linux aarch64 | 静态（musl） | 是 | bwrap、landlock、proot | 是；镜像在 UEFI 上启动 |
| macOS 14+ arm64 | 是 | 是 | 重定向 home（advisory） | 在 Luban 宿主机里（`vz`，Apple 芯片） |
| Windows x86_64 | 是 | 是 | 重定向 `%USERPROFILE%`（advisory） | 经 `wsl2` 承载 |

aarch64 镜像在虚拟机（`virt`）上启动；在 aarch64 真机上启动需要为它准备的内核，索引里暂时还没有。

## 机器需要提供什么

三层决定 xlings 运行的每样东西来自哪里：

| 层 | 内容 | 规则 |
|---|---|---|
| **T0 —— 操作系统** | 内核及其能力（user namespace、Landlock、seccomp、KVM）；一次性设置与 `--system` 用的 `sudo`；`apparmor_parser`；经提权执行的 `mount` 与 `chown`；macOS 和 Windows 自带的工具（`pbcopy`、`clip`、`wsl.exe`、`msiexec`） | 唯一可以来自机器的一层；xlings 写明它是什么以及原因 |
| **T1 —— 索引** | 其余所有程序和数据文件：bwrap、pasta、proot、curl、e2fsprogs、limine、qemu、UEFI 固件、剪贴板工具、WSL | 索引里的包，每个架构都有；可以用机器自带的副本，但缺失时给出的永远是 `xlings install <package>`，从不是机器的包管理器 |
| **T2 —— 进程内** | GPT、FAT、ISO 9660 与 El Torito、混合分区表、qcow2、tar 包、文件克隆 | 不需要任何程序 |

所以一台干净的 Linux 机器只需要 `curl`（或浏览器）来安装 xlings，任何功能都不再需要别的。
`xlings self doctor --isolation` 显示这台机器给了什么、每个工具来自哪里。

## 版本

发布按日期编号：`YYYY.M.D.N`，`N` 从 1 开始（`.0` 保留给正式版本）。`luban` 与它旁边的 xlings 一起发布，版本相同。

| 对象 | 版本方式 |
|---|---|
| xlings 与 luban | 发布日期 |
| Luban 的 edition、策略、启动层 | 各自的日期；已发布版本的输出永不改变 |
| 包 | 上游版本；payload 或配置变化时在版本上提升 `revision` |
| 索引快照 | 自己的版本；写明支持哪些客户端（[specs/index-version-contract](../specs/index-version-contract.md)） |

### 兼容

- **`min_client`**：用到较新客户端字段的 edition 或策略，写明能理解它的最旧客户端；更旧的客户端会给出升级命令并拒绝，而不是误读。
- 旧客户端会忽略的字段，被设计成忽略也安全。
- `xlings self update` 升级后的 home 用现有的东西照常工作；旧记录的修复用 `xlings self doctor --fix`。
- 升级从不删除：SubOS 的 home 以及任何属于用户的东西。
