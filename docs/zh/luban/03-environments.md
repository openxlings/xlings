# 环境

[English](../../luban/03-environments.md) | **简体中文**

环境由 edition 创建，之后就是你的：设置、包、代。

## 设置

```bash
luban config box                                  # its settings
luban config box --proxy socks5h://127.0.0.1:7897   # its only network is this proxy
luban config box --tz utc                           # utc, proxy (the proxy's exit), or a zone
luban config box --policy xim:agent-confined        # another policy
luban config box --allow clipboard                  # a grant
luban config box --mount ~/code/proj:/work          # a host directory inside
```

| 设置 | |
|---|---|
| `proxy` | `socks5h://host:port`：环境的网络就是这个代理，别无其他 |
| `tz` | 它显示的时区：`utc`、`proxy` 或一个时区名——除非你选择，从不用宿主的 |
| `policy` | 一个策略包（`xim:agent-private`、`xim:agent-confined`、你自己的） |
| `allow` | 一项授权：`display`、`audio`、`gpu`、`ssh-agent`、`clipboard`、`clipboard-paste` 等 |
| `mount` | 映射进环境的宿主目录 |

每一项都是同名的 `xlings subos config` 设置（[沙箱与策略](../11-sandbox-and-policy.md)）。下面这些是 luban 自己的：

```bash
luban config box --mode machine       # luban enter starts it with a kernel of its own
luban config box --persist home       # each session starts from its baseline; /home stays
luban config box --shell fish         # its login shell
luban config box --desktop none       # its console, not its desktop, in the window
luban config box --on-close background
```

| 设置 | |
|---|---|
| `mode` | `sandbox`（本机内核）或 `machine`（本地虚拟机启动环境本身） |
| `persist` | `all`（默认）、`home`（系统部分每次恢复，`/home` 保留）或 `none`（什么都不保留） |
| `shell` | 它的登录 shell，取自它自己的 `/etc/passwd` |
| `desktop` | 窗口显示的桌面会话，或 `none`（[桌面](08-desktop.md)） |
| `on-close` | 关闭正在运行的机器的窗口时：`ask`、`background` 或 `poweroff` |
| `files` | 机器怎样共享文件：`virtiofs`（默认；做不到时用 9p）或 `9p` |

edition 可以给出这些设置的默认值；你设置的优先。

按 machine 启动的系统，`luban run` 也在 machine 里执行，用它自己的内核：machine 没开时为命令启动，其中最后一条命令结束时关机（同时执行的命令各自执行完）；正在运行的保持运行。`--user` 指它的某个用户——它没有的名字会被拒绝。
在 machine 里执行 `poweroff`，会话随之结束，machine 关机，不会再问。

## 升级

```bash
luban upgrade box --dry-run      # what would change
luban upgrade box                # the edition's newest version
luban upgrade box --to 2026.10.11.2
```

环境知道它的 edition 带来了什么。一次升级：

- 把 edition 带来、**你没动过**的包升到新 edition 的版本（之前的升级换下来的版本仍然装着，它仍属于 edition，不算你改过的）；
- **你**自己装的、或改过版本的包保持不变；
- 安装新 edition 新增的包；
- 新 edition 去掉的包保持安装，并说明；
- 新 edition 的策略只有在**不放宽任何一项**时才应用；否则保留当前策略并列出会被放宽的项——切换要你显式执行 `luban config box --policy …`。

一次升级是新的一代，所以可以回滚。

不会替你升级：系统保持创建时的样子，有可升级的内容、或它的桌面缺少新 edition 带来的东西时，`luban status box` 会说
（并给出两条路：只给它装那一个包，或者升级）。把较旧的系统迁到它自己的用户态的升级会保留迁移前的样子（它的归档，
`luban history box` 与 `luban status box` 都会写出）；回到那里只要一条命令，先确认：

```bash
luban rollback box --to relayout -y   # the system as it was before its move
```

## 代与回滚

```bash
luban history box                # its generations
luban rollback box               # back one
luban rollback box --to 3
```

环境的包每变动一次就是一代；回滚只移动指针，不碰数据。每一代也记下了它的包，所以回退后再次进入，环境仍停在回退到的那一代
（2026.10.13.2 之前产生的一代没有这份记录：它的文件会回来，而下一次进入会按现有的包重建——回滚时会说明这一点）。

## 网络

`luban status box` 用一行说出它的网络，无论它怎样运行都是同一个意思：`like the host`——宿主的 DNS 与宿主的代理
（启动它的环境里的 `HTTPS_PROXY`、`HTTP_PROXY`、`ALL_PROXY`、`NO_PROXY`）——`only its proxy`（`luban config box --proxy …`）
或 `none`（`--net none`）。

机器在自己的 NAT 上：地址与 DNS 来自内核的 DHCP，不依赖系统；宿主回环上的代理（`127.0.0.1:7890`）在里面是宿主在这个 NAT 上的地址
（`10.0.2.2:7890`）：`network  like the host · proxy http://10.0.2.2:7890 (the host's http://127.0.0.1:7890)`。它写在本次启动的
`/run/luban/network.env` 里，每个会话与每个登录 shell 都有，从不写进系统。宿主的代理变了，下次启动生效；`luban status` 会说明。

## 离线使用

系统自己的 home 从宿主的 index 拷贝开始：它装什么都能不联网地从中读到，在里面 `xlings update` 更新的是它自己的那份。
导出的系统带着它的包与这份 index，因此不联网也能重新建出来：

```bash
luban export box box.luban
luban import box.luban
```

## 用户、sudo 与软件包

系统的用户随系统建出（`--user`，桌面默认 `lubanoser`）。不设密码时用默认密码 `lubanoser`：`luban new` 当场说明，
`luban status` 在用 `passwd` 修改之前一直提示。`--no-password` 是明确的选择：完全没有密码，`sudo` 也不问。
其余时候 `sudo` 与 Ubuntu、Fedora 一样：用户在 `wheel` 组里，sudo 问他的密码，15 分钟内不再问，找得到 `xlings`，
并保留宿主的代理（`/etc/sudoers.d/luban`，用 `visudo` 修改）。core、desktop、agent-workspace 带 `sudo`；
沙箱里不需要（它的写入经系统的 broker）。

```bash
luban new box --user alice                  # asks for a password; empty: lubanoser
luban new box --user alice --no-password    # no password, a sudo that asks nothing
luban run box --user alice -- passwd        # change it
```

系统里只有一份软件包存储，即系统的（`/xlings`）。用户的 `~/.xlings` 是它的一个视图：自己选择的版本、自己的 shim 与日志——
没有自己的 index，也没有自己的包。自己没选过的就是系统的选择。`xlings list`、`info`、`use` 也显示系统的包，标为 `system`。

| 普通用户 | 结果 |
|---|---|
| `xlings install codex`（系统已有） | 不安装："codex 2.1 is provided by the system" |
| `xlings use node 22`（存储里已有） | 自己的选择，不影响别人 |
| `xlings install node@22`（存储里没有） | 一行说明加一个问题：经 `sudo` 装进系统的存储，之后成为自己的选择——存储里人人都有，只在你这里启用 |
| `xlings remove node` | 撤销自己的选择；存储里的包保留（root 为所有人删除） |
| `sudo xlings ...` | 系统自己的：所有用户的默认与存储 |

没有人回答时（agent、脚本）不问：命令以 `E_NEEDS_ROOT` 失败，并写出要运行的确切 `sudo xlings install ...`。
root 删除某个用户正选择着的版本时会保留它，并说出是谁的选择。

## 删除

```bash
luban rm box                     # asks first, and says how much its home holds
```

环境的 home 是你的数据：只有你发起并确认的删除才会删除它。

## 检查

系统在创建、升级、启动和进入时检查自身，也可以随时要求检查：

```bash
luban status box --check          # every check, and what failed
luban status box --check --json   # {"ok": ..., "failed": [...], "items": [...]}
```

检查分关键与非关键。关键检查失败会中止正在进行的操作：新系统不就绪，升级不切换，机器不启动，进入不开始，并说明是哪一项失败。
其余失败只报告，系统照常使用。检查项是任何 Linux 用户空间都需要的：`/usr` 是系统的这一代，`/usr/bin` 中每个程序都有其加载器、
能找到它声明的库，每个包的程序都能启动，用户、登录 shell、主机名与 machine id、时区、证书机构、UTF-8 locale、终端描述、
系统里 xlings 读的包索引（它的用户也读这一份），桌面系统还有应用列表和光标主题；再加上 edition 声明的检查。每个包的程序在没有显示的情况下启动
（图形程序用 offscreen），声明了自有检查的包改由它自己的检查来查。系统在创建时（或升级后第一次进入时）得到 machine id，
并用这台电脑的时区：沙箱里绑定进去，机器启动时写进去——你在里面自己设的时区不会被覆盖。edition 在 `/usr/share/luban/checks.d/*.json` 中声明自己的检查：

```json
{"checks": [{"id": "desk-session", "what": "its desktop session starts", "run": "xdesk --version", "critical": false, "timeout": 30}]}
```

`run` 是在系统内执行的 `/bin/sh -c` 命令；在 `timeout` 秒（默认 120）内以 0 退出即通过。进入时只运行快速检查：只读文件，不启动程序。

## 使用剪贴板

剪贴板不等于显示：

- 默认情况下，复制经终端（OSC 52）生效于你所坐那台机器的剪贴板；粘贴就是终端的粘贴；
- `allow clipboard`（复制到宿主）和 `allow clipboard-paste`（读取宿主）是两项独立的授权：授权后环境里的 `wl-copy`、`wl-paste`、
  `xclip`、`xsel` 经 broker 到达宿主的剪贴板；
- `luban status box` 显示当前是哪一种。

从另一个终端加入正在运行的环境的命令（`luban enter`、`luban run`）有自己的终端：作业控制、信号以及窗口尺寸都跟随你的。
同一环境里的终端彼此对等：先进入的可以先离开而其他的留下，一个终端里的 Ctrl-C 只到达它自己的程序，最后一个终端离开几秒后环境的会话结束。
