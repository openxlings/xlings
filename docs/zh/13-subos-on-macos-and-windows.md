# macOS 与 Windows 上的 SubOS

[English](../13-subos-on-macos-and-windows.md) | **简体中文**

SubOS 的内容在各平台上相同；不同的是平台能强制什么，以及 SubOS 在哪里运行。

## 各平台强制什么

| 平台 | 沙箱 | 内核强制 | 会话 |
|---|---|---|---|
| Linux | bwrap、landlock、proot | 是（proot 除外） | 是 |
| macOS 14+ | 重定向 home | 否（advisory） | 否 |
| Windows | 重定向 `%USERPROFILE%` | 否（advisory） | 否 |

macOS 与 Windows 上，`--sandbox` 让工具不往你真正的 home 里写东西；它拦不住试图逃逸的代码。
`xlings subos status <name>` 和 `xlings subos doctor` 显示这台机器实际给了什么——它们打印的表来自各后端自己的探测。

## Windows 上的 Linux SubOS

payload 是 Linux 程序的 SubOS 运行在一个**承载（carrier）**里：Windows 上是这个 home 专属的一个 WSL2 发行版，
你从不需要进入它。Windows 上的 `xlings subos …` 会转发给里面的 xlings。

```powershell
xlings subos new box --carrier wsl2 --rootfs --from subos:luban-tiny
xlings subos exec box -- uname -a
```

与 Windows 之间的隔离由该发行版的配置保证：不自动挂载 Windows 驱动器，不开启 interop（在里面运行 `.exe`）。
Windows 目录只能以授权的方式进入 SubOS：只挂载那一个目录。

### 一次性设置

Windows 机器上的第一个 Linux SubOS 需要 WSL2。xlings 把它带来：来自索引的 WSL（`xim:wsl-installer`，
微软的安装程序，按摘要锁定），以及它运行所需的虚拟机平台，各自经提权提示（UAC）执行。`subos new` 在需要时当场询问，
也可以自己运行：

```powershell
xlings self doctor --isolation          # is WSL2 ready here
xlings self doctor --isolation --fix    # install WSL and turn on the Virtual Machine Platform
```

Windows 要求重启时，`subos new` 以退出码 3 结束：重启一次，然后再创建 SubOS。agent 模式下不提问：命令以退出码 2 结束并给出这项设置。

## macOS 上的 Linux SubOS

`vz` 承载在 Luban 宿主机里运行 Linux SubOS：每个 home 一台小型 Linux 机器，由发布包里的 luban
（`luban __vm`，带 virtualization 权限签名）经 Virtualization.framework 运行。它的文件在 `<home>/carriers/vz` 下一个
大小写敏感的 APFS 卷上（用系统自带的 `hdiutil` 创建并挂载），经 virtio-fs 交给机器；第一次使用时取回这个版本的 Linux 构建
与机器内核。不能运行虚拟机的 Mac（Intel Mac，或者它本身就是虚拟机）会以这个原因被拒绝，本机原生的 SubOS 不受影响。

## 选择承载

```bash
xlings subos new box --carrier local     # this machine (the default where the payloads are native)
xlings subos new box --carrier wsl2      # Windows
xlings subos new box --abi linux         # whatever runs Linux payloads here
```

由 payload 的目标决定：Windows 上的 Linux 程序需要 `wsl2`；Linux 上承载是 `local`。
