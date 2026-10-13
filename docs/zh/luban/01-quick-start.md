# 快速开始

[English](../../luban/01-quick-start.md) | **简体中文**

`luban` 随 xlings 一起安装，版本相同；在宿主上、在每个 Luban 环境里、在每台 Luban 机器上都是同一个程序。本页是一条
完整的路线：一个环境、在里面工作、回退一步、agent 的工作区、升级、把环境当作机器运行、桌面、清理。每一步都写明你应该看到什么。

## 1. 一个环境

```bash
luban new box --edition tiny            # an edition: nano, tiny, core, desktop, desktop-xdesk, agent-workspace, or ns:name
```

你会看到 `▸ making box (tiny)`、下载过程，然后是 `✓ box is ready`、它是什么（`box  tiny@… · N packages ·
generation N`）以及下一步（`enter  luban enter box`）。宿主上第一次 `luban new` 可能先询问一次性设置（见下文）。
不写 edition 名时是 Luban Core，它还带有编译器和构建工具。中途停下时，再运行同一条命令就会补全。`nano` 没有 shell：
它会这样说，下一步是 `luban run box -- …`。

## 2. 在里面工作

```bash
luban enter box               # a shell in it; the prompt says where: [luban:box] ~ #
luban run box -- uname -a     # one command in it, from outside
luban run box -- xlings install -y cmake
```

控制台先说明它是什么：Luban OS 及其版本、edition、怎样运行、保留什么、怎样离开（你的语言是中文时用中文），然后才是提示符：

```
Luban OS 2026.10.13.2 · tiny 2026.10.11.1 · box
  a sandbox on this host, keeping what changes
  luban status box · exit to leave
```

装软件仍然是 xlings：`xlings install <package>`——在环境里就装进那个环境。`exit` 离开。

## 3. 回退一步

```bash
luban history box             # its generations: one per change
luban rollback box            # back one: cmake is gone again
```

一个 generation 既是文件，也是产生它们的软件包：回退之后再次进入，仍然是回退后的样子。

## 4. agent 的工作区

```bash
luban new agent --edition agent-workspace
luban run agent -- claude
luban status agent            # what it may do: its policy, network and grants
```

agent 的工作区由它的策略隔离；它唯一的网络可以是一个代理（`luban config agent --proxy socks5h://127.0.0.1:7897`）。
见 [Agent 工作区](04-agent-workspace.md)。

## 5. 升级

```bash
luban upgrade box --dry-run   # the plan: the edition, packages added, upgraded and kept
luban upgrade box             # one new generation; rollback undoes it
```

`--dry-run` 什么也不改：它说 `box is up to date`，或者列出会改变的内容，并给出 `luban upgrade box`。

## 6. 把它当作机器运行

```bash
xlings install -y qemu-system            # the virtual machine monitor, from the index
luban enter box --mode machine           # the environment itself, with a kernel of its own
luban enter box --mode machine --temp    # the same, and nothing it writes stays
luban config box --mode machine          # from now on, luban enter starts it this way
```

机器启动的是环境本身，不是它的拷贝：它写下的就是环境里的。几秒后你在当前终端得到它的登录 shell。`exit` 时会问
它是否继续运行；Ctrl-] 再按 `d` 让它继续运行，Ctrl-] 再按 `p` 关机。`luban stop box` 在任何地方都能关掉它。

## 7. 桌面

```bash
luban new desk --edition desktop         # labwc, a panel, a launcher, a terminal
xlings install luban-os                  # the Luban OS window
luban enter desk --display window
```

窗口按自己的大小显示桌面；Host 键（单独按 Ctrl+Alt）打开它的菜单。见[桌面与 Luban OS 窗口](08-desktop.md)。

## 8. 启动盘（可选）

```bash
luban write box.iso /dev/sdb  # every byte of the drive is replaced; it asks first
```

## 9. 清理

```bash
luban rm box                  # asks first, and says how much its home holds
xlings remove gcc --older-than 16 --dry-run
```

`xlings remove <package> --older-than <version>` 删除一个包不再被使用的旧版本；`--dry-run` 先列出它们。

## 在宿主上第一次使用

Ubuntu 23.10 及以后，AppArmor 不允许没有 profile 的程序创建环境所需的沙箱。第一次 `luban new` 会在下载任何东西之前检查宿主，
并询问一次性设置：一个 root 拥有的 bwrap 和一个范围很窄的 AppArmor profile，对这台机器的所有用户生效，只需一次 `sudo`。

```bash
luban setup                   # the same setup, whenever you like
```

宿主上完全没有 bwrap 时，同一步里会先从索引装上一个。不保留全部改动的会话（`--temp`、`--persist home`）需要 0.10
及以后的 bwrap；宿主自带的较旧时（Ubuntu 24.04 带的是 0.9），第一次会装上索引里的那个。agent（`--agent`）从不被提问：
命令以退出码 2 结束并给出这项设置。

## 更多

```bash
luban                         # one screen: where you are, the environments, what next
luban ls                      # the environments
luban help --all              # every command
luban <command> --help        # one command, and the xlings command it is
```

[Edition](02-editions.md) · [环境](03-environments.md) · [镜像](05-images.md) · [桌面](08-desktop.md)
