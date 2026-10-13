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

edition 可以给出这些设置的默认值；你设置的优先。

## 升级

```bash
luban upgrade box --dry-run      # what would change
luban upgrade box                # the edition's newest version
luban upgrade box --to 2026.10.11.2
```

环境知道它的 edition 带来了什么。一次升级：

- 把 edition 带来、**你没动过**的包升到新 edition 的版本；
- **你**自己装的、或改过版本的包保持不变；
- 安装新 edition 新增的包；
- 新 edition 去掉的包保持安装，并说明；
- 新 edition 的策略只有在**不放宽任何一项**时才应用；否则保留当前策略并列出会被放宽的项——切换要你显式执行 `luban config box --policy …`。

一次升级是新的一代，所以可以回滚。

## 代与回滚

```bash
luban history box                # its generations
luban rollback box               # back one
luban rollback box --to 3
```

环境的包每变动一次就是一代；回滚只移动指针，不碰数据。每一代也记下了它的包，所以回退后再次进入，环境仍停在回退到的那一代
（2026.10.13.2 之前产生的一代没有这份记录：它的文件会回来，而下一次进入会按现有的包重建——回滚时会说明这一点）。

## 删除

```bash
luban rm box                     # asks first, and says how much its home holds
```

环境的 home 是你的数据：只有你发起并确认的删除才会删除它。

## 使用剪贴板

剪贴板不等于显示：

- 默认情况下，复制经终端（OSC 52）生效于你所坐那台机器的剪贴板；粘贴就是终端的粘贴；
- `allow clipboard`（复制到宿主）和 `allow clipboard-paste`（读取宿主）是两项独立的授权：授权后环境里的 `wl-copy`、`wl-paste`、
  `xclip`、`xsel` 经 broker 到达宿主的剪贴板；
- `luban status box` 显示当前是哪一种。

从另一个终端加入正在运行的环境的命令（`luban enter`、`luban run`）有自己的终端：作业控制、信号以及窗口尺寸都跟随你的。
