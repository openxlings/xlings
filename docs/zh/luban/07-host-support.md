# 宿主支持

[English](../../luban/07-host-support.md) | **简体中文**

## 在哪些宿主上检查

每次发布都以普通用户（没有 sudo）运行同样的场景——一次性设置、每个策略预设、只走代理的网络、剪贴板、nano、tiny、
tiny-musl、agent 工作区、升级、连接终端的运行——覆盖：

| 宿主 | 内核与 LSM |
|---|---|
| Ubuntu 24.04，AppArmor 限制保持默认 / 关闭 | runner 的 |
| Ubuntu 22.04 | runner 的 |
| arm（aarch64）上的 Ubuntu 24.04 与 22.04 | runner 的 |
| Fedora（SELinux enforcing）、Debian 12、Arch | 各自的内核，在虚拟机里 |
| Fedora、Arch、openSUSE Tumbleweed、Debian testing 与 12、Ubuntu 20.04 | 容器：各自的用户态 |

## 宿主需要有什么

它的内核（user namespace），不需要从它的包管理器装任何东西。环境、镜像或虚拟机需要的一切都来自索引：
bwrap、pasta、e2fsprogs、limine、内核、qemu、UEFI 固件、剪贴板工具。唯一取自宿主的是操作系统自身——内核、
一次性设置用的 `sudo`、AppArmor 的加载器（[支持的平台](../51-supported-platforms.md)）。

## 一次性设置

Ubuntu 23.10 及以后用 AppArmor 限制非特权 user namespace。这项设置安装一个 root 拥有的 bwrap 和一个只允许它做这件事的
AppArmor profile——一次，对所有用户生效；它不会关闭这项限制。

```bash
luban setup                              # = xlings self doctor --isolation --fix
xlings self doctor --isolation           # what this host can isolate with, and why not
```

`luban new` 在下载任何东西之前检查宿主并当场询问；agent 模式下以退出码 2 结束并给出命令。无人值守时（CI、脚本），
`luban setup -y` 回答它唯一的那个问题。

在 macOS 与 Windows 上，`luban status` 回答的是那里运行 Luban 系统的载体（vz、wsl2）：是否可用、为什么不可用、怎样继续。
就绪退出码为 0，未就绪为 1，各平台一致。

## 出问题时

```bash
luban status                     # this host: what it gives, what is missing
luban status box                 # an environment: what it asks for and gets, and what a shared kernel cannot hide
xlings subos doctor              # every environment, with a real entry
xlings self doctor               # the home itself
```

| 看到 | 含义 | 怎么办 |
|---|---|---|
| `setting up uid map: Permission denied` | AppArmor 的限制 | `luban setup` |
| 私有环境拒绝进入：bwrap 无法禁止嵌套 user namespace | setuid 的 bwrap | `luban setup` |
| `--temp` 或 `--persist home` 说这里没有支持 overlay 的 bwrap | 宿主的 bwrap 早于 0.10，或者是 setuid 的 | `xlings install bwrap`（能装时第一次就会自动装上） |
| 退出码 125 并附一个列表 | 策略要求这台宿主给不了的东西 | 列表逐项给出原因与修法 |
| edition 在创建任何东西之前被拒绝 | 它没有为这台机器的架构构建 | 换一个 edition（[02](02-editions.md)） |
| agent 模式下退出码 2 | agent 无法回答的问题 | 错误信息给出能回答它的参数 |
