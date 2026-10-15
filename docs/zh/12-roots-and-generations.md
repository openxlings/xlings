# 根与代

[English](../12-roots-and-generations.md) | **简体中文**

SubOS 可以**作为 `/`** 呈现：它的 `/usr` 是包 payload 的视图，`/etc`、`/home`、`/var` 是这台机器自己的状态。
同一份声明可以作为沙箱进入、导出成容器镜像或磁盘，或者由内核直接启动。[Luban](luban/README.md) 就建立在这上面；
本章讲 xlings 这一侧。

## 根

```bash
xlings subos new box --rootfs --from subos:luban-core   # a root; its packages installed into it
xlings subos use box                                    # enter: uid 0, its own /usr and /etc
xlings subos exec box -- sh -c 'gcc hello.c && ./a.out'
xlings subos exec box -- xlings install -y nginx        # installed inside: in its /usr/bin at once
xlings subos status box                                 # the generation, the history, name conflicts
```

| 部分 | 内容 |
|---|---|
| `/usr` | 一代：指向 payload 的链接树——先是已注册的程序和库，再是各活跃包的 `bin/` 与 `lib/`（busybox 的 applet 就这样进入 `/usr/bin`） |
| `/etc` | 缺什么从包的 factory 文件补什么（`usr/share/factory/etc`）；系统用户只追加 |
| `/home`、`/var`、`/root` | 机器自己的；换代不改变它们 |

## 代

根的 workspace 每变动一次（`install`、`use`、`remove`）就产生新的一代：`<subos>/root` 指向 `root.gen/<k>`，
切换是一次 rename。放置好的代从不修改。

```bash
xlings subos rollback box --list    # the generations
xlings subos rollback box           # back one: only the pointer moves
xlings subos rollback box --to 3
```

glibc 坏了也不妨碍回滚：xlings 是静态的。

## 导出

```bash
xlings subos export box --tar box.tar.gz        # docker import / podman import / wsl --import
xlings subos export box --rootfs ./box/         # a directory
xlings subos export box --disk box.img --size 8G   # an ext4 filesystem image
luban export box box-drive.img --size 8G        # a bootable drive: GPT, UEFI (and BIOS on x86_64)
luban export box box.qcow2 --size 8G            # the same drive as qcow2
luban export box box.iso                        # a live ISO: the root in memory
```

机器启动用的镜像属于 Luban（`luban export`）；`xlings subos export` 把根写成目录、tar 包或文件系统镜像。导出的镜像是自足的：实例成为镜像里默认的 SubOS，带上闭包里的每个 payload（workspace、已安装的包、每个 ELF 的
loader 与搜索路径）、静态的 xlings 以及启动阶段。镜像在进程内写出——GPT、FAT、带 BIOS 与 UEFI El Torito 启动项的
ISO 9660、让同一个 ISO 也能从驱动器启动的混合分区表、qcow2——所以制作镜像的机器不需要分区、ISO 或 qemu-img 工具。
由什么来启动它（内核、limine、内核命令行）是启动层（boot profile），见 [Luban：镜像](luban/05-images.md)。

输出目标必须不存在；重用的目标会被拒绝，原文件保持不变。

## 启动

以 `init=<home>/boot/luban-init`（`xlings-init` 是它以前的名字）启动的内核运行 stage-0：挂载内核文件系统，按 `boot.json` 选一个 SubOS，
把 `/usr` 指向它，执行它的 init；init 起不来就试下一个。

```bash
luban boot box --once       # try box on the next boot; back to the default unless confirmed
luban boot box --mark-good  # confirm it
luban boot box              # make it the default
luban boot box --now        # switch userland without restarting the kernel (when the init supports it)
```

## 域：镜像里的 home 在哪里

Linux 上普通用户可以指定镜像将来的 home 路径（`--domain`）：xlings 在用户拥有的私有 namespace 里构建，
把包装在这个逻辑路径上，数据保存在用户自己的 home 里。

```bash
xlings subos new mydistro --rootfs --domain /xlings --from subos:luban-tiny
xlings subos exec mydistro -- sh
xlings subos export mydistro --tar mydistro.tar
```

这需要可用的 bwrap 和 user namespace。

`luban new` 都这样建系统：里面的 home 是 `/xlings`，不与本机的 SubOS 共享 payload；本机上的 `xlings subos list`
用一行统计这些系统（`--all` 列出全部）。在某个系统里，它自己的 xlings 只看得到它和它的视图，看不到同一个域里的其他系统。
系统在自己的 generation 里带着自己的运行时（`usr/lib/xlings-abi`），里面看到的是 `/xlings/abi`：两个用不同 glibc 的系统共享同样的 payload。

## 自己的发行版

```bash
xlings subos new mydistro --rootfs --from subos:luban-core
xlings subos exec mydistro -- xlings install -y nginx
xlings subos pack mydistro --as myns:mydistro@1.0      # a SubOS package for your index
xlings subos diff box mydistro                         # what differs
```

把它写成模板见 [32](32-authoring-a-subos-template.md) 和 [Luban：edition](luban/02-editions.md)。

## 系统层

多用户机器上，管理员可以把包为所有人装一次；每个用户自己的版本优先：

```bash
sudo xlings install --system gcc cmake      # into /xlings, owned by root
cmake --version                              # a user: after their own packages, before the host's
```
