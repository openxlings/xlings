# SubOS

[English](../10-subos.md) | **简体中文**

SubOS 是一个有名字的环境：自己的工具版本、自己的 home、自己的策略。它可以只是一次 PATH 切换，
也可以是带私有网络的无 root 沙箱，或者一整个 Linux 根。它由三件事构成：

| | 是什么 | 章节 |
|---|---|---|
| **内容** | workspace（哪些版本）、home、策略 | 本章 |
| **视图** | 进程怎样看到它：PATH 叠加、沙箱、根、机器 | [11](11-sandbox-and-policy.md)、[12](12-roots-and-generations.md) |
| **承载** | 它在哪里运行：本机、WSL2 发行版、虚拟机 | [13](13-subos-on-macos-and-windows.md) |

## 速览

```bash
xlings subos new work --sandbox=private        # create it, and declare its isolation
xlings subos use work                          # enter: that sandbox, no flag needed
xlings subos exec work -- make -j8             # one command; its exit code
xlings subos status work                       # what it asks for vs what this machine gives
xlings subos list                              # every SubOS
xlings subos remove work                       # delete it (with its home; asks first)
```

**隔离程度属于实例，只声明一次**：创建时（`--sandbox=dev|private|locked`），或之后用 `subos config`。
无论 `use`、`exec`、`start` 还是接口，进入的都是那个沙箱。单次调用上的参数（`--net none`、`--sandbox=locked`）
只能收紧。没有声明的实例就是普通环境：只有给了 `--sandbox` 才进沙箱。`--from` 派生的实例继承源实例的声明。

## 与宿主分开的工具链

不需要沙箱，只把版本分开：

```bash
xlings subos new py312
xlings subos use py312                     # a shell whose PATH is py312's tools
xlings install python@3.12 poetry          # into py312; the host is unchanged
exit
xlings install ninja --subos py312         # install without entering
xlings subos use py312 --cmd "python -V"   # one command inside
```

`--from` 从已有环境派生，几乎不占额外空间（payload 共享）：

```bash
xlings subos new py312-exp --from py312
xlings subos new rust-env --from subos:rust-env@1.80   # from a SubOS package
```

## 会话：一个实例，多条命令

每次 `exec` 都重新启动沙箱。要让多条命令共享同一个 `/tmp` 和同一组进程（服务加它的测试），就启动一个会话：

```bash
xlings subos start dev --ttl 30m                    # in the background; ends after 30 idle minutes
xlings subos exec dev -- sh -c 'redis-server --daemonize yes'
xlings subos exec dev -- make integration-test      # joins the session in milliseconds
xlings subos ps                                     # running sessions
xlings subos stop dev
```

`xlings subos use dev --sandbox --keep` 也会启动会话：shell 退出后保留，直到 `subos stop`。从另一个终端加入的命令
有自己的终端：作业控制、Ctrl-C 以及窗口尺寸都跟随调用方。
进入同一实例的终端（`subos use dev --sandbox`，不带 `--cmd`）彼此对等：第一个在后台启动会话并像其他终端一样加入，
任何一个都能立即离开，Ctrl-C 只到达它自己的程序，最后一个离开几秒后会话结束。

沙箱里的服务可以用 `--publish` 从外面访问：

```bash
xlings subos exec dev --publish 8080:80 -- python -m http.server 80
```

## 拷入拷出文件

```bash
xlings subos cp ./data.csv dev:/tmp/           # in
xlings subos cp dev:~/results ./results        # out (~ is the instance's home)
```

只能访问实例的 home 和 `/tmp`。实例里的链接不会被跟随：实例无法借你的拷贝写到宿主上别的地方。

## 存储

| `--storage` | home 在哪里 | 说明 |
|---|---|---|
| `shared`（默认） | `subos/<name>/home/<user>` | shell 与沙箱共用同一个 home |
| `tmpfs` | 内存里，退出即消失 | 需要 bwrap |
| `image` | `home.img`，ext4 稀疏文件，loop 挂载 | 需要 bwrap 和一次管理员操作 |

存储只决定 home 在哪里，不增加边界。

## 删除

```bash
xlings subos remove dev        # asks, and says how much data its home holds
xlings subos remove dev -y     # only when the user asked for the deletion
```

SubOS 的 home 是用户数据：只有用户发起并确认的删除才会删除它（接口里 `remove_subos` 需要 `"yes": true`）。
每一次删除都记录在 `<home>/logs/destructive.ndjson`。升级、修复、清理都不会删除它。

## 退出码

| 退出码 | 含义 |
|---|---|
| 命令自己的 | 命令运行并结束 |
| 125 | 进入之前就失败（包括策略要求而这台机器给不了） |
| 126 / 127 | 命令不能执行 / 不存在 |
| 124 | `--timeout` 到期 |
| 128 + n | 被信号 n 结束 |
| 13 | `E_PERMISSION`：只有 owner 能做，或策略拒绝 |
| 75 | 请求已排队，等待批准 |
| 2 | agent 模式下需要确认，或交互式进入被拒绝 |
| 3 | Windows 上的 `subos new`：一次性设置已完成，Windows 要求先重启 |

## 不能用时

```bash
xlings subos status dev        # asked vs given; why it cannot enter, item by item, with the fix
xlings subos doctor            # every instance: its policy, a real entry, its policy package, its sessions
```

`XLINGS_TRACE=caps,provider,session` 为一条命令打印诊断信息。

## 命令

| 命令 | 作用 |
|---|---|
| `subos new <name> [--sandbox=preset] [--from <src>] [--storage …]` | 创建，并声明隔离程度 |
| `subos use <name> [--sandbox] [--cmd "…"] [--keep\|--ttl N]` | 进入；`--cmd` 执行一条命令 |
| `subos exec <name>\|--temp [--sandbox[=preset]] [--timeout D] -- argv` | 一条命令，返回它的退出码 |
| `subos start / stop / ps` | 会话 |
| `subos cp <src> <dst>` | 拷入或拷出（一端写成 `<name>:<path>`） |
| `subos config / status / doctor` | 策略、生效状态、健康检查（[11](11-sandbox-and-policy.md)） |
| `subos log / report` | 审计（[11](11-sandbox-and-policy.md)） |
| `subos upgrade <name>` | 升到 edition 的最新版（[Luban](luban/03-environments.md)） |
| `subos rollback / export / boot / diff / pack` | 根（[12](12-roots-and-generations.md)） |
| `subos list / info / remove` | 列出、详情、删除 |
