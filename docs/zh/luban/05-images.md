# 镜像

[English](../../luban/05-images.md) | **简体中文**

环境用 `luban export` 变成镜像；格式由文件名决定。

## 格式

```bash
luban export box box.iso              # a live ISO: the root runs in memory
luban export box box.img              # a drive: GPT, an EFI system partition, an ext4 root, persistent
luban export box box.qcow2            # the same drive as qcow2
luban export box box.tar.zst          # a root filesystem (docker import, wsl --import)
luban export box ./box-root/          # a directory
```

| | 在哪里启动 | 由谁写出 |
|---|---|---|
| `.iso` | x86_64 上 BIOS 和 UEFI，aarch64 上 UEFI；从光盘启动，也能从写入了它的驱动器启动（混合分区表） | 进程内：ISO 9660、El Torito、GPT |
| `.img` | x86_64 上 BIOS 和 UEFI，aarch64 上 UEFI | 进程内：GPT、FAT；根分区用 `mkfs.ext4`（xim:e2fsprogs） |
| `.qcow2` | 同上 | 进程内 |
| `.tar.*` | ——（由容器或 WSL 导入） | 进程内 |

制作镜像的机器不需要分区、ISO、FAT 或 qemu-img 工具；`limine`（xim:limine）是引导器，在 x86_64 上还让镜像能从驱动器以 BIOS 启动。

## 启动层

镜像怎样启动——内核、引导器和内核命令行——是一个**启动层**（boot profile），一个只在制作镜像时才装进环境的包：

| `--boot` | 用于 | 内核 |
|---|---|---|
| `generic` | 真机（x86_64） | `linux-kernel`，带真机需要的驱动 |
| `virt` | 虚拟机（x86_64、aarch64） | `linux-kernel-virt`（6.12 LTS）：virtio、ext4 与控制台内建 |
| `ns:name` | 你自己的启动层 | 它所写的 |

```bash
luban export box box.iso --boot virt
```

不写 `--boot` 时用 edition 的启动层。没有任何启动层时，用环境里已有的内核（`xlings install linux-kernel --subos box`），
或者用 `xlings subos export` 的 `--kernel <vmlinuz>`。

## 试运行镜像

```bash
luban try box                       # export and boot in a local VM
luban try box.iso                   # an image you already have
luban try box --proxy socks5h://127.0.0.1:7897   # its only network is the proxy
luban try box --nographic           # its console in this terminal
```

`luban try` 默认用 `virt` 启动层（没有时退回环境自己的内核）；这个用户能用 KVM 时用 KVM，否则纯模拟。aarch64 上它在 qemu 的
`virt` 机器上启动 UEFI 固件。它需要的东西来自索引——`xlings install qemu-system`，一个包含两个模拟器、用户态网络
和 UEFI 固件的包；缺失时 luban 会说明。

## 写入驱动器

```bash
luban write box.iso /dev/sdb        # a boot stick from an image
luban write box /dev/sdb            # an environment straight onto a drive
```

`luban write` 只写整个驱动器，拒绝分区、已挂载或正在使用的驱动器（包括系统所在的盘）。它会要求你输入驱动器名确认；
agent 模式下需要同时给出 `-y` 和 `--serial <序列号>`。
