# 自我管理

[English](../06-self-management.md) | **简体中文**

`xlings self` 管理 xlings 自身：升级客户端、检查并修复 home、清理缓存。日常几乎用不到——升级之后，
或者某个命令开始报奇怪的错时，从这里开始。

## 升级

```bash
xlings self update
```

它刷新索引，然后把最新的 xlings 作为一个包安装。新版本第一条修改 home 的命令成功之后，随即自己完成 home 的升级：检查 home
配置、重建命令路由，并像 `self doctor` 一样检查 home。都成功时什么也不说；自己做不到的，说一次：有几个问题（按原因计——
一个不在了的 payload 是一个问题，不论它登记了多少个名字）、修复它们的一条命令、详情在哪里：

```text
[warn] xlings 2026.10.13.1 (from 2026.10.12.3): 1 problem(s) in this home it could not fix by itself
         fix          xlings self doctor --fix
         details      xlings self doctor   (log: @xlings/logs/upgrade.ndjson)
         or go back   xlings use xlings 2026.10.12.3
```

每一项及其命令都在日志和 `xlings self doctor` 里；命令加 `-v` 时也在这里列出。从没有升级记录的 home 会说这些问题在这个版本之前就已存在。

上一个版本还在时，回去只要一条命令：

```bash
xlings use xlings 2026.10.11.1
```

`self update` 本身无法完成时，它会说该试什么：另一个镜像（`xlings config --mirror CN`），或者安装脚本——它覆盖安装，
保留 home 里的一切。规则见[升级与兼容](../specs/upgrade-and-compatibility.md)。

## 体检

```bash
xlings self doctor
```

只读。它检查以下几层是否一致：

| 层 | 检查什么 |
|---|---|
| workspace | 当前 SubOS 声明的活跃版本 |
| 版本数据库 | 每个 `包@版本` 的注册记录 |
| shim | `bin/` 下的转发链接 |
| payload | 记录指向的目录及其中的程序 |
| sysroot | `lib/`、`lib64/`、`usr/`、`etc/`、`share/` 下的链接 |

退出码 0 表示当前 SubOS 健康；只有告警不会让它失败。

```bash
xlings self doctor --show-ok          # every check, the good ones too
xlings self doctor --subos work       # another SubOS
xlings self doctor --isolation        # what this machine can sandbox with (see 11)
```

## 修复

```bash
xlings self doctor --fix
```

一次跑完，把能修的都修掉：

- 补上缺失的 shim，删除多余的；
- 丢掉指向不存在版本的绑定；
- 指向未注册版本的活跃项取消激活——包括其他 SubOS 的；
- payload 坏掉的包重新注册，不行就重装；
- 断掉的 sysroot 链接按版本数据库**重新指回去**，没有任何记录说明它该指向哪里时才删除；
- 应该在却不在的链接放回去；
- 被整体挪动过的 home，记录重新指向当前位置（见下文）；
- 任何东西都救不回来、也没有索引提供的记录被清除。

结束后会再检查一遍：你看到的报告是修复**之后**的状态。修复是否成功，看发现项是否真的消失，而不是看命令的退出码；
重装之后发现项仍在，报告为检查本身的问题。

修复另一个 SubOS 时执行的 `self doctor --fix` 没有终端：它启动的任何程序都不会停下来等密码，Ctrl-C 能结束它，
它的输出在 `logs/doctor/<subos>.log`。包声明了需要管理员权限的步骤（`system.admin_step`）而当时没人能确认的，
`self doctor` 每次都会列出，在终端里执行 `--fix` 时补做。

### 运行时

```bash
xlings config --runtime glibc@2.44.3   # pin this home's default runtime
xlings config --runtime glibc@latest   # follow the newest installed again
```

这个 home 里绑定 glibc 的包都运行在同一个运行时上：锁定的版本，否则是已安装的最新版本。改变它只是切换链接
（`<home>/abi`），不改写任何包；`abi.json` 说明当前是什么，`logs/abi.ndjson` 记录何时、为何改变。

```bash
xlings self doctor --fix --dry-run    # what it would do, changing nothing
```

### 清除记录

只有同时满足这些条件，`--fix` 才会删除一条注册记录：payload 已经不在、重新注册和重装都失败、没有索引提供它、
没有别的 SubOS 在用它。这会丢失一个事实（"它曾经装过"），所以这样的运行不会以 `OK` 结束，而是说明清除了几条。
删除 sysroot 链接同样如此。退出码仍是 0：它回答的是"这个 home 还需不需要人来处理"。

### 挪动过的 home

用 `mv` 搬动 home 不受支持：payload 里的程序把旧路径写在 `PT_INTERP` 和 `RPATH` 里。`self doctor` 会说明：

```text
✗ home relocated   this home is at <new path>, but its records were written for <old path>
  → run            xlings self doctor --fix
  • note           re-points the versions database and the sysroot links;
                   binaries that baked the old path into PT_INTERP/RPATH are
                   NOT repaired — re-provision the home if the toolchain fails
```

`--fix` 把 xlings 自己的记录和所有 SubOS 的 sysroot 链接指向当前根。它不会因此宣称工具链能用了。

### 建议的命令一定能跑

报告里 `→ run` 后面是可以直接粘贴的完整命令。它给出的是**包**，不是程序名（`nm` 由 llvm 提供，没有叫 `nm` 的包）。
没有任何东西能提供某个条目时，它会直说，而不是给一条跑不通的命令。

### 重置元数据

```bash
xlings self doctor --reset-metadata
```

它和 `--fix` 分开，因为它会丢信息：读不出来的发布元数据被丢弃，条目退回成一个可单独切换的版本。
只在元数据已经损坏、重装也救不回来时使用。

## 清理

```bash
xlings self clean            # caches, and payloads nothing references
xlings self clean --dry-run  # what it would delete
```

SubOS 的 home 是用户数据，从不被清理：只有用户发起并确认的删除（`subos remove`）才会删除它。

## 其他命令

```bash
xlings self config     # the home, the current SubOS, the mirror
xlings self install    # install from a release tarball (what the installer runs)
xlings self init       # create the home's directories
xlings self migrate    # move an old layout into subos/default
xlings self uninstall  # remove xlings, asking first
```

## 一条命令做了什么

每条命令写一份操作日志 `logs/ops/<日期>/<trace>.ndjson`；它运行的每个进程（hook、修复、SubOS 里的命令）按继承的 trace
写进同一个文件。里面有各个阶段、耗时和失败；工具自己的输出（limine、mkfs 的）放在旁边的文件里。

```bash
xlings self log                      # the latest operations
xlings self log 4ed6cd2b             # one of them (a prefix of its trace)
xlings self log 4ed6cd2b --bundle    # one file for a bug report, without your paths or name
```

日志只留在本机；两周前的会被删除。格式见[操作日志](../specs/operation-journal.md)。

## 出问题时

```bash
xlings self doctor                   # what is wrong
xlings self doctor --fix --dry-run   # what it would do
xlings self doctor --fix             # repair
xlings self doctor                   # exit 0: clean
```

还有剩余问题时，报告逐条说明原因。属于其他 SubOS 的 payload 问题不在这里修——那会把包拉进当前环境——报告会指出是哪个 SubOS。
