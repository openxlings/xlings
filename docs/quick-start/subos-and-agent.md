> 更新日期：2026-10-06

# SubOS 使用指南：环境、沙箱与 Agent

SubOS 是一个有名字的环境：自己的工具版本、自己的 home、自己的策略。它可以只是一次 PATH
切换，也可以是带私有网络的无 root 沙箱。本指南按场景组织；每个场景的命令都可以直接执行。
行为细节见 [SubOS 隔离模型](../design/subos-isolation.md)，命令全集见
[命令参考](../generated/command-reference.md)。

> **平台差异。** Linux（bwrap）提供真正的隔离：文件系统、进程、网络、身份。macOS 与
> Windows 的 `--sandbox` 只重定向 home 目录，**不是安全边界**，运行不受信任的代码请用
> 操作系统级沙箱或虚拟机。`xlings subos status <name>` 显示这台机器实际给了什么。

## 速览

```bash
xlings subos new work --sandbox=private        # 创建，并声明它的隔离程度
xlings subos use work                          # 进入：就是那个沙箱，不用再加参数
xlings subos exec work -- make -j8             # 执行一条命令，返回它的退出码
xlings subos status work                       # 它要求的 vs 这台机器给的
xlings subos list                              # 所有 SubOS
xlings subos remove work                       # 删除（连同 home，会先确认）
```

**隔离程度属于实例，在创建时声明一次**（`subos new --sandbox=dev|private|locked`，之后可以用
`subos config` 修改）。声明过的实例，无论 `use`、`exec`、`start` 还是接口调用，都按声明进入沙箱。
单次调用上的 `--sandbox=locked`、`--net none` 之类**只能更严格**，不能放宽。没有声明的实例
（例如场景一那样只用来切换工具链的）保持原来的行为：不加 `--sandbox` 就不进沙箱。
`--from` 派生的实例继承源实例的声明。

## 场景一：一套与宿主互不干扰的工具链

不需要沙箱，只想让一组工具版本和宿主分开：

```bash
xlings subos new py312
xlings subos use py312                 # 新 shell，PATH 指向 py312 的工具
xlings install python@3.12 poetry      # 装进 py312，宿主不受影响
python --version
exit                                   # 回到宿主

xlings install ninja --subos py312     # 不进入也能往里装
xlings subos use py312 --cmd "python -V"   # 在里面跑一条命令
```

`--from` 从一个现成的环境派生，几乎不占额外空间（安装产物是共享的）：

```bash
xlings subos new py312-exp --from py312
xlings subos new rust-env --from subos:rust-env@1.80   # 从 subos 类型的包创建
```

## 场景二：在沙箱里运行 AI Agent

Agent 改代码、装依赖、跑测试，但不应该碰到你的 home、SSH 密钥和其他项目：

```bash
xlings subos new agent-ws --sandbox=private
xlings subos config agent-ws --mount ~/code/myproj:/work   # 只映射这个项目（可写）
xlings subos exec agent-ws -- sh -c 'cd /work && claude --task "fix the failing tests"'
```

`private` 的含义：

| 项 | 效果 |
|---|---|
| 文件系统 | xlings home 只读，只有本实例目录可写；看不到宿主 home、其他实例和审计日志 |
| 进程 | 独立的 pid / ipc / uts 命名空间；看不到宿主进程；禁止嵌套 user namespace |
| 网络 | `nat`：能上网，但访问不到宿主本机的服务（`--allow host-loopback` 才可以） |
| 身份 | 用户名 `user`、主机名 = 实例名、`TZ=UTC` |
| 环境变量 | 清空，只放行基础变量和代理变量；需要的用 `--env-pass NAME` 放行 |
| 装包 | 需要你批准（见场景五） |

Agent 应该声明自己的身份，这样任何命令都不会停下来等待输入：

```bash
export XLINGS_AGENT_MODE=1
xlings subos use agent-ws        # 交互式进入被拒绝（exit 2），提示改用 subos exec
```

通过 NDJSON 接口驱动（每行一个 JSON 事件，最后一行是结果）：

```bash
xlings interface subos_exec --args '{"name":"agent-ws","sandbox":true,"argv":["make","test"],"timeout":"10m"}'
xlings interface subos_events --args '{"name":"agent-ws","kind":"perm"}'
```

## 场景三：运行不受信任的代码

来路不明的脚本、要审计的仓库：不给网络，只读地看代码，事后看它做了什么：

```bash
xlings subos new quarantine --sandbox=locked
xlings subos config quarantine --observe full
xlings subos exec quarantine --mount ~/Downloads/suspicious-repo:/src:ro --timeout 5m -- sh -c 'cd /src && ./build.sh'
xlings subos report quarantine        # 执行过的每个程序、改动过的文件
```

`locked` = 没有网络（只有 lo）、`--mount` 默认只读、不能装包、`observe=full` 记录每一次
execve。一次性的任务可以不留下实例：

```bash
xlings subos exec --temp --sandbox=locked -- ./untrusted-binary   # 用完即删，审计保留
```

单次调用的参数**只能收紧**，不能放宽实例的声明：在 `locked` 实例上用 `--net host` 会被
拒绝（exit 125）。要放宽，由你在外面改声明：`xlings subos config quarantine --net nat`。

## 场景四：一个实例，多条命令（会话）

每次 `exec` 都会重新启动沙箱。要让多条命令共享同一个 `/tmp` 和进程（例如后台服务 + 测试），
就启动一个会话：

```bash
xlings subos start dev --ttl 30m              # 后台会话，空闲 30 分钟后自动结束
xlings subos exec dev -- sh -c 'redis-server --daemonize yes'
xlings subos exec dev -- make integration-test   # 加入同一个会话，几毫秒就开始
xlings subos ps                               # 正在运行的会话
xlings subos stop dev
```

`subos use dev --sandbox --keep` 也会启动会话：shell 退出后会话保留，直到 `subos stop`。

需要从外面访问沙箱里的服务（`nat` 网络）：

```bash
xlings subos exec dev --sandbox=private --publish 8080:80 -- python -m http.server 80
```

## 场景五：在沙箱里装包

沙箱里的 xlings 照常可用：读操作在本地执行，`install` / `remove` / `update` 交给宿主侧的
broker，按实例的策略判定后在外面执行：

```bash
xlings subos config dev --fetch ask      # auto（默认）/ ask / deny
# 沙箱内：
xlings install jq                        # exit 75：已排队，等待批准
# 沙箱外：
xlings subos requests dev                # 待批准的请求
xlings subos approve dev r1a2b3c         # 批准（或 deny）
```

细粒度规则写在策略文件里（`<home>/config/subos/<name>/policy.json`），按顺序匹配：

```json
{
  "extends": "private",
  "permissions": {
    "fetch": {
      "default": "ask",
      "rules": [
        { "match": "xim:*", "index": "official", "action": "auto" },
        { "match": "*", "size_gt": "2GB", "action": "deny" }
      ]
    }
  }
}
```

只有你（owner）能在沙箱外修改策略；沙箱里的进程能读到它，但改不了。只有 owner 能做的事
（`self update`、改策略、动别的实例、切换 xlings 版本）在沙箱里返回 13，并提示在外面执行的命令。

## 场景六：需要显示、声音、SSH 或 GPU

默认这些都关闭。每一项授权只打开一样东西（一个 socket 文件或一组设备节点）：

```bash
xlings subos config gui-app --allow display      # X11 / Wayland
xlings subos config gui-app --allow audio        # PulseAudio / PipeWire
xlings subos config ci --allow ssh-agent         # 只有 agent socket，不是 ~/.ssh
xlings subos config ml --allow gpu               # NVIDIA 节点 + /dev/dri
```

`--grants-allowed gpu,display` 允许单次调用自己加这些授权（`subos exec ml --allow gpu ...`）。

## 场景七：团队统一的策略（策略包）

把策略做成一个 `type = "subos-policy"` 的包（payload 根目录放 `policy.json`），团队成员选用：

```bash
xlings subos config ci --sandbox xim:policy-ci-strict@1   # 示例包名
# 选择时显示：它相对于所基于的内置预设改了什么
xlings subos config ci --policy-upgrade          # 包更新后，显式升级并显示差异
```

策略会复制进实例并锁定 `resolved.from` 和 `sha256`：包更新不会悄悄改变已有实例。系统管理员
可以在 `/etc/xlings/config.json` 里限制允许的来源：`{"subos_policy_sources": ["xim:*"]}`。

## 场景八：在宿主和实例之间传文件

```bash
xlings subos cp ./data.csv dev:/tmp/            # 拷入
xlings subos cp dev:~/results ./results         # 拷出（~ 是实例的 home）
```

只能访问实例自己的 home 和 `/tmp`。实例里的符号链接不会被跟随：实例无法借你的拷贝改写
宿主上的文件。

## 场景九：审计——它做了什么

审计由宿主侧的 supervisor 写在沙箱看不到的地方（`<home>/logs/subos/<name>/events.ndjson`）：

```bash
xlings subos log dev                       # 全部事件
xlings subos log dev --kind perm           # 权限判定（装包、被拒绝的操作）
xlings subos log dev --kind exec -f        # 执行过的程序，持续跟随
xlings subos report dev --json             # 每个会话的汇总
```

| `--observe` | 记录 |
|---|---|
| `basic`（默认） | 会话开始 / 结束、策略变更、权限判定、每次 exec |
| `standard` | + rw 映射里改动的文件 |
| `full` | + 沙箱里执行过的每个程序 |

只记录环境变量的名字，从不记录值。

## 场景十：Ubuntu 24.04 上沙箱进不去

Ubuntu 24.04 默认通过 AppArmor 限制非特权 user namespace，bwrap 会报
`setting up uid map: Permission denied`。不需要改系统 sysctl：

```bash
xlings self doctor --isolation          # 说明原因
xlings self doctor --isolation --fix    # 一次 sudo：root 拥有的 bwrap + 一个只授权它的 AppArmor profile
```

在这之前，xlings 会退回到 proot 并明确提示（proot 只是视图，不是安全边界）。也可以用
`--sandbox landlock`：内核限制写入（只能写实例目录），但宿主文件可见、本机 socket 可连接——
它防误写，不防恶意代码。

## 场景十一：一个完全由 xlings 构成的根（Luban）

SubOS 也可以**作为 `/`** 呈现：它的 `/usr` 是包的链接视图，`/etc`、`/home`、`/var` 是这台"机器"
自己的状态。同一份声明可以作为沙箱实例进入、导出成容器镜像，或者由内核直接启动——
这就是 **Luban**（kernel + xlings + 可选的 LubanOS 服务）。官方分级是 subos 类型的包：
`luban-tiny`（busybox、glibc、内核）、`luban-core`（+ bash、coreutils、gcc、curl……）、
`luban-desktop`（+ Mesa、Wayland、X11、字体、音频），上一级 `from` 下一级。

```bash
xlings subos new box --rootfs --from subos:luban-core   # 一个根；它声明的包装进去
xlings subos use box                                    # 进入：uid 0，自己的 /usr、/etc
xlings subos exec box -- sh -c 'gcc hello.c && ./a.out'
xlings subos exec box -- xlings install -y nginx        # 在里面装包：立即出现在它的 /usr/bin
xlings subos status box                                 # 当前代、各代、名字冲突
xlings subos rollback box                               # 回到上一代（只切指针）

xlings subos export box --tar box.tar.gz                # docker import / podman import / wsl --import
xlings subos export box --disk box.img --size 8G        # ext4 磁盘镜像
qemu-system-x86_64 -m 1G -nographic \
  -kernel ~/.xlings/subos/box/root/usr/lib/modules/*/vmlinuz \
  -drive file=box.img,format=raw,if=virtio \
  -append "root=/dev/vda rw console=ttyS0 init=$HOME/.xlings/boot/xlings-init"
```

- 包的每一次变动都是一个新的**代**（generation）：`/usr` 指向当前代，切换是一次 rename，
  回滚只是把指针切回去。glibc 坏了，静态的 xlings 照样能回滚。
- 镜像里的 xlings 就是唯一的包管理器：`xlings install` 装进系统；`xlings subos boot <n> --once`
  试启动另一个 SubOS（没确认就自动回到默认），`xlings subos boot <n>` 设为默认，
  `--now` 在 init 支持时不重启内核直接切换用户态。
- 两种布局：镜像里的系统 home 就是构建它的 home 的路径（single，一个用户等价于 root），
  或者在 `/xlings` 里构建（multi，推荐多用户；`/xlings` 由 root 拥有）。
- 定制自己的发行版：`subos new mydistro --rootfs --from subos:luban-core`，装包，
  `subos pack mydistro --as myns:mydistro@1.0` 打成包，发布到你自己的索引。

多用户机器上，管理员可以把包装进**系统层**，每个用户都能用、不用复制，自己装的版本优先：

```bash
sudo xlings install --system gcc cmake      # 装进 /xlings（root 拥有）
cmake --version                              # 普通用户：shell 里在自己的包之后、宿主之前找到它
```

## 排错

```bash
xlings subos status dev        # 要求的 vs 生效的；不能进入时逐项列出原因和修法
xlings subos doctor            # 检查每个实例：策略、能否在这台机器进入（真实进入一次）、策略包、会话
XLINGS_TRACE=caps,provider,session xlings subos exec dev --sandbox -- true   # 诊断输出
```

| 退出码 | 含义 |
|---|---|
| 命令自己的 | 命令正常结束 |
| 125 | 进入之前就失败（包括策略要求而这台机器给不了） |
| 126 / 127 | 命令找到了但不能执行 / 没找到 |
| 124 | `--timeout` 到期 |
| 128 + n | 被信号 n 结束 |
| 13 | `E_PERMISSION`：只有 owner 能做，或策略拒绝 |
| 75 | 请求已排队，等待批准 |
| 2 | agent 模式下需要确认（例如删除），或交互式进入被拒绝 |

## 删除

```bash
xlings subos remove dev        # 会先确认，并列出 home 里有多少数据
xlings subos remove dev -y     # 只在用户明确要求删除时使用
```

SubOS 的 home 是用户数据：只有用户发起并确认的删除才会删除它（接口里 `remove_subos` 需要
`"yes": true`）。每一次删除都记录在 `<home>/logs/destructive.ndjson`。

## 命令速查

| 命令 | 说明 |
|---|---|
| `subos new <name> [--sandbox=preset] [--from <src>] [--storage shared\|tmpfs\|image]` | 创建，并声明隔离程度 |
| `subos use <name> [--sandbox] [--cmd "..."] [--keep\|--ttl N]` | 进入；`--cmd` 执行一条命令 |
| `subos exec <name>\|--temp [--sandbox[=preset]] [--timeout D] -- argv` | 执行一条命令，返回它的退出码 |
| `subos config <name> [--sandbox=preset\|ns:pkg] [--net] [--fetch] [--allow] [--mount] ...` | 声明策略 |
| `subos status <name>` / `subos doctor [<name>]` | 生效的隔离 / 健康检查 |
| `subos start <name> [--ttl D]` / `subos stop <name>` / `subos ps` | 会话 |
| `subos requests / approve / deny <name> [id]` | 处理沙箱里的请求 |
| `subos log <name> [--kind K] [-f]` / `subos report <name>` | 审计 |
| `subos cp <src> <dst>` | 拷入 / 拷出（一端写成 `<name>:<path>`） |
| `subos list` / `subos info <name>` / `subos remove <name>` | 列出 / 详情 / 删除 |
| `self doctor --isolation [--fix]` | 这台机器能怎样隔离，以及修复 |
| `subos new <name> --rootfs [--from subos:luban-*]` | 一个根：作为 `/` 进入、导出、启动 |
| `subos rollback <name> [--to N] [--list]` | 根回到某一代 |
| `subos export <name> --rootfs <dir>\|--tar <file>\|--disk <file>` | 导出为目录 / tar 包 / 磁盘镜像 |
| `subos boot [<name>] [--once\|--fallback\|--now\|--mark-good]` | 机器启动哪个 SubOS |
| `subos diff <a> <b>` / `subos pack <name> --as ns:pkg@ver` | 比较两个 SubOS / 打成 subos 类型的包 |
| `install --system <pkg>` | 装进系统层（多用户，root） |
