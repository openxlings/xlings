# 快速开始

[English](../../luban/01-quick-start.md) | **简体中文**

`luban` 随 xlings 一起安装，版本相同；在宿主上、在每个 Luban 环境里、在每台 Luban 机器上都是同一个程序。本页是一条
完整的路线：一个环境、在里面工作、回退一步、agent 的工作区、升级、把环境当作机器运行、桌面、清理。每一步都写明你应该看到什么。

## 1. 一个环境

```bash
luban new box                           # Luban Core, the default: shells, editors, git, compilers
luban new small --edition tiny          # 或其他 edition：nano、tiny、tiny-musl、core、agent-workspace、desktop
luban new desk --edition desktop --desktop lxqt   # 一个桌面：xdesk（自带）、labwc、compositor、lxqt、kde
luban new mine --xpkg myindex:myos      # 任何 index 的系统（02）
```

一条就够。你会看到 `▸ making box (core)`、下载过程，然后是 `✓ box is ready`、它是什么（`box  core@… · N packages ·
generation N`）以及下一步（`enter  luban enter box`）。宿主上第一次 `luban new` 可能先询问一次性设置（见下文）。
默认是 Luban Core：一个带编译器和构建工具的完整系统；`tiny` 是小的那个，`desktop` 带桌面。中途停下时，再运行同一条命令就会补全。`nano` 没有 shell：
它会这样说，下一步是 `luban run box -- …`。

## 2. 在里面工作

```bash
luban enter box               # a shell in it; the prompt says where: [luban] root@box:~#
luban run box -- uname -a     # one command in it, from outside
luban run box -- xlings install -y cmake
```

控制台先说明它是什么——系统和它的 edition、怎样运行（与本机共用内核，还是有自己内核的机器）、保留什么、
以谁登录、xlings 在这里管理什么、时间——以及怎样离开（你的语言是中文时用中文），然后才是提示符：

```
 Luban OS 2026.10.15.1 · [luban] box
 ──────────────────────────────────────────────────────────
 system    tiny 2026.10.11.1 · generation 4 · 4 packages
 runs as   sandbox: shares this host's kernel (Linux 6.8.0)
 keeps     everything
 user      root · /root
 xlings    manages this system only (R)
 time      2026-10-15 09:00 UTC
 ──────────────────────────────────────────────────────────
 exit to leave · luban status box
```

提示符的标记说明你在哪里：`[luban]` 是与本机共用内核的系统，`<luban>` 是有自己内核的系统（`--mode machine`），
保留的不是全部时加 `:temp` 或 `:home`（`[luban:temp] root@box:~#`）。主机名就是系统的名字。用 `xlings subos`
建的本机视图则标为 `(名字)`。

Luban 系统是独立的用户态 OS：它里面的 xlings 只管理它自己（里面是 `/xlings`），它不与本机的 SubOS 共享任何包文件，
里面也不出现你的用户名。它的用户是它自己的（见下）。

### 以谁登录

`luban enter` 默认以 root 登录，除非系统指定了别的用户。desktop 一定有一个普通用户：
`luban new d --edition desktop` 会询问用户名（默认 `lubanoser`）和密码（隐藏输入；留空表示不设密码、直接登录）；
`-y` 取默认值。任何 edition 都可以有：

```bash
luban new box --edition core --user alice --no-password
luban enter box --user root   # root this once
luban config box --login root # and from now on
```

密码只以哈希形式保存在系统自己的 `/etc/shadow` 里。要在里面以 root 操作，就以 root 进入：这个系统本来就归本机上的你所有。

普通用户安装软件无需 root。在机器里，他们的 `xlings` 使用自己的 `~/.xlings`，系统的包（`/xlings`）是其下一层：
系统已有的直接使用，自己安装的只属于自己。在沙箱里，安装经系统的 broker 装进系统本身。

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
