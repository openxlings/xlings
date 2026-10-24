# 机器

[English](../../luban/06-machine.md) | **简体中文**

Luban 机器是用户态由一个 SubOS 构成的机器：内核启动 stage-0，stage-0 选择一个环境，这个环境就是系统。

## 启动

内核以 `init=<home>/boot/luban-init` 启动（镜像会设置好）。stage-0 是第一个进程：`/proc` 存在之前它不读任何配置，
它挂载内核文件系统，读取 `boot.json`，把 `/usr` 指向所选环境的代，执行它的 init。init 起不来就退到下一个启动项。

## 启动项

```bash
luban boot                        # this machine's entries
luban boot box2 --once            # try box2 on the next boot only
luban boot --mark-good            # this boot worked: keep it
luban boot box2                   # make box2 the default
luban boot box2 --now             # switch now, without restarting the kernel (when the init supports it)
```

没有被确认的试启动（`--once`）在下次启动时回到默认项；启动一直失败的启动项让位给它的 `--fallback`。

## 升级与回滚机器

在机器里 xlings 就是包管理器：`xlings install` 以新的一代装进正在运行的系统；`luban rollback` 回到上一代。
坏掉的库阻止不了回滚——xlings 是静态的。

## 它的内核

机器启动的是它的系统自己的内核：版本声明的那个（`boot.virt`，一个 `linux-kernel-virt` 的版本），第一次用时安装，
随 `luban upgrade` 和系统的其余部分一起前进。每个版本都自称 `6.12.112-virt`，区分它们的是包的版本，`luban status <name>`
会显示。只给这个系统换一个：

```bash
luban config box --kernel 6.12.112-r3     # this version, from the next start
luban config box --kernel default         # its own again
```

启动前按这次启动需要的能力检查内核（读它旁边的构建配置）：文件要 virtio-fs，3D 要 virtio-gpu。有退路的退回并说明原因——
`files 9p (the kernel 6.12.112 has no virtio-fs: luban upgrade box)`——没有退路的（没有 9p）拒绝并给出修法。版本早于"系统自带内核"的
系统会被提示 `luban upgrade`。

## 它的文件

机器的目录树和它读的 home 是共享给它的，不是复制的：走 virtio-fs，每个共享一个非特权的 `virtiofsd`
（第一台机器会安装 `xim:virtiofsd`）；做不到时走 9p——没有硬件虚拟化的机器、这台主机没有它的构建、守护进程起不来，或内核没有 virtio-fs。宿主限制非特权命名空间
（Ubuntu 的 AppArmor）时，`virtiofsd` 从一开始就不用它自己的沙箱，并说明这一点。
`luban status <name>` 会说用的是哪个，以及原因：

```bash
luban status box                  # files     virtiofs
luban config box --files 9p       # always 9p, from the next start
```

在 virtio-fs 上列出和读取大量文件大约快十倍；系统能对文件做的事两者相同。

两者都让每个文件归宿主上的用户。机器里除了建它时的用户、还有自己的用户（在里面 `adduser alice`）时，
从下一次启动起它的 `/home` 走 9p 的 `mapped-xattr`：每个用户的属主与权限存在宿主文件的扩展属性里，
之前建的 home 在启动时交给它的用户。原来的用户感觉不到变化；`luban status` 会写 `/home over 9p (users of their own)`。

## 机器的 home 在哪里

| 布局 | 系统 home | |
|---|---|---|
| single | 构建它的 home 的路径 | 一个用户，等价于 root |
| multi | `/xlings`，root 拥有 | 多个用户，各有自己的 home |

stage-0 通过 `/etc/xlings/root.json` 找到系统 home。
