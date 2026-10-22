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

## 它的文件

机器的目录树和它读的 home 是共享给它的，不是复制的：走 virtio-fs，每个共享一个非特权的 `virtiofsd`
（第一台机器会安装 `xim:virtiofsd`）；做不到时走 9p——没有硬件虚拟化的机器、这台主机没有它的构建，或守护进程起不来。
`luban status <name>` 会说用的是哪个，以及原因：

```bash
luban status box                  # files     virtiofs
luban config box --files 9p       # always 9p, from the next start
```

在 virtio-fs 上列出和读取大量文件大约快十倍；系统能对文件做的事两者相同。

## 机器的 home 在哪里

| 布局 | 系统 home | |
|---|---|---|
| single | 构建它的 home 的路径 | 一个用户，等价于 root |
| multi | `/xlings`，root 拥有 | 多个用户，各有自己的 home |

stage-0 通过 `/etc/xlings/root.json` 找到系统 home。
