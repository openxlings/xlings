> 更新日期：2026-10-06（含 Part 2：根呈现、系统层、Luban）
>
> 总体架构（部署形态、策略、进入与外部执行、可观测性、平台抽象）见
> `.agents/docs/2026-10-05-subos-architecture-design.md`。本文是落地后的使用与行为说明：
> 每一节写的是现在的实际行为，没有实现的部分在最后一节列出。

# SubOS 隔离模型

## 概述

一个 SubOS 实例有三件互相独立的事：

| 轴 | 回答的问题 | 怎么设 |
|---|---|---|
| **策略** | 允许做什么、隔离到什么程度 | `subos config <s> --sandbox=dev\|private\|locked`、覆盖项、策略包 |
| **后端** | 用什么实现隔离 | bwrap（默认）、`--sandbox landlock`、`--sandbox proot`；macOS / Windows 为 home 重定向 |
| **存储** | 实例的 home 放在哪里 | `subos new --storage shared\|tmpfs\|image` |

策略声明在实例上（`<home>/config/subos/<s>/policy.json`，在实例目录之外，沙箱里只读）。无论怎样进入
（`use`、`exec`、`start`、interface），都按这份声明执行。没有声明的实例按原样进入（Legacy），并带上 #640 的 S0 修复。

## 进入与外部执行

| 命令 | 作用 |
|---|---|
| `xlings subos use <s> [--sandbox]` | 交互 shell；`--cmd` 跑一条命令；agent 模式下拒绝交互（exit 2），改用 `exec` |
| `xlings subos exec <s> [--sandbox[=preset]] -- <argv>` | 跑一条命令，返回它自己的退出码；有运行中的会话就加入它 |
| `xlings subos start <s> [--ttl 10m]` / `stop <s>` | 后台会话：之后的 `exec` / `use` 都加入同一个实例（同一个 /tmp、同一组进程） |
| `xlings subos ps` / `status <s>` / `doctor [<s>]` | 运行中的会话；实例要求的与本机能给的；每个实例的健康检查 |
| `xlings subos log <s>` / `report <s>` | 审计事件；会话报告（执行过的程序、rw 映射里改动的文件） |
| `xlings subos cp <src> <s>:<dst>` | 在宿主和实例之间复制 |

退出码：命令自己的；`125` 进入前失败（包括策略要求而本机给不了）；`126/127` 命令无法执行 / 不存在；
`124` 超时；`128+n` 被信号终止；`13` `E_PERMISSION`；`75` 请求已排队等待批准；`2` agent 模式下需要确认。

`--keep` / `--ttl` 在 Linux 上是会话的空闲时间（替代原来的 keeper）；其他平台不生效，行为与之前一致。

## 策略

| 预设 | 文件系统 | 进程 | 网络 | 身份 | 获取包 |
|---|---|---|---|---|---|
| `dev` | home 只读，只挂入本实例 | pid / ipc / uts 隔离 | host | host | auto |
| `private` | 同上 | 同上，禁止嵌套 userns | `nat`（pasta） | 中性（user / 主机名=实例名 / UTC） | ask |
| `locked` | 同上，`--mount` 默认只读 | 同上 | `none`（只有 lo） | 中性 | deny |

- **覆盖项**：`--net`、`--fetch`、`--index-update`、`--observe`、`--allow <grant>`、`--mount`、`--env-pass`。
  写在 `subos config` 上是声明；写在单次调用上**只能收紧**，或者在 `grants_allowed` 范围内授权。
- **具名授权**：`display`、`audio`、`camera`、`gpu`、`ssh-agent`、`dbus`、`host-loopback`，每项只打开一样东西
  （一个 socket 文件或一组设备节点），从不暴露宿主的整个运行时目录。
- **规则**：`permissions.fetch.rules` 按顺序匹配（包名 glob、来源索引、大小），第一条匹配的生效。
- **策略包**：`subos config <s> --sandbox ns:name[@version]` 选择一个 `type = "subos-policy"` 的 xpkg
  （payload 根目录的 `policy.json`）。选择时显示它相对所基于的预设改了什么；策略复制进实例文件并锁定
  `resolved: {from, sha256, base}`，包更新不会悄悄改变策略，`--policy-upgrade` 显式升级并显示差异
  （owner 在包之上做过的修改会出现在差异里）。系统配置的 `subos_policy_sources` 可以限制来源。
- **fail closed**：策略里出现本版本不认识或不能执行的值（例如 `net: "vpn"`、`fetch: "layer"`）时拒绝进入，不忽略。

## 沙箱里的 xlings 与 broker

沙箱里能看到 xlings home，但只读；只有本实例的目录可写。沙箱里的 xlings 照常可用：

- 只读命令（`list`、`info`、`--version`、shim 分发）在本地执行，**不经过 broker**；
- 改动 home 的命令（`install`、`remove`、`update`）交给宿主侧的 broker（`run/subos/<s>/broker.sock`），
  broker 用同一个 `policy.decide()` 判定，在宿主侧执行并带回退出码；
- 只有 owner 能做的事（`self update`、改策略、动别的实例）在沙箱里返回 `13`，并给出在外面执行的命令；
- `fetch=ask` 的请求进入队列（exit `75`），由 owner 在外面 `subos requests / approve / deny`。

## 后端

| 后端 | 机制 | 能给的 | 给不了的 | 选择方式 |
|---|---|---|---|---|
| **bwrap** | user / mount / pid / ipc / uts / net namespace | 全部预设 | — | 默认；查找顺序：root 拥有的 `/usr/lib/xlings/bwrap` → 系统 bwrap → xim payload |
| **landlock** | Landlock LSM（内核 ≥ 5.13），没有 namespace | 写入围栏：只有实例目录、`/dev`、`/proc` 和 rw `--mount` 可写；`TMPDIR` 在实例内 | 文件不可见、pid / net / 身份隔离；宿主的 unix socket（D-Bus、agent、其他会话）仍可连接，**不是**运行不受信代码的边界 | 只在显式 `--sandbox landlock` 时使用，从不自动替代 bwrap |
| **proot** | ptrace | 文件系统视图 | 安全边界（视图而非边界） | `--sandbox proot`，或 bwrap 不存在时回退 |
| **home 重定向** | 环境变量 | HOME / USERPROFILE 指向实例 | 其余全部（advisory） | macOS / Windows |

后端给不了的项：在 `dev` 下降级并说明原因；在 `private` / `locked`（这些项是 must）下拒绝进入（125），
逐项列出缺什么、怎么修。`xlings subos status <s>` 显示请求的与实际生效的。

Ubuntu 24.04 默认限制非特权 user namespace（AppArmor）。`xlings self doctor --isolation --fix` 用一次 sudo
安装 root 拥有的 `/usr/lib/xlings/bwrap` 和一个窄 AppArmor profile；不改 sysctl，不创建 setuid。

## 网络与身份

| 模式 | 实现 | 本机服务 | 出口 |
|---|---|---|---|
| `host` | 共享宿主网络（抽象 unix socket 也可达） | 可达 | 宿主 |
| `nat` | 预先建好的 user+net namespace + pasta 接入；`--publish 8080:80`；`--allow host-loopback` 才能访问宿主本机服务 | 不可达 | 宿主 IP |
| `none` | net namespace，只有 lo | 不可达 | 无 |
| `proxy` | 未实现（见最后一节） | — | — |

中性身份：用户名 `user`、主机名 = 实例名、`TZ=UTC`、`LANG=C.UTF-8`，`/etc/passwd` 等来自实例的 `etc-neutral/`。

## 终端与进程

- 非交互命令放进新会话，没有控制终端；交互 shell 保留终端以支持作业控制，并加载 seccomp 过滤器拦截 `TIOCSTI`（F8）。
- 会话由宿主侧的 supervisor 托管：`SIGTERM` / `SIGHUP` 结束会话；等待期间 `Ctrl-C` 属于命令。
- 环境变量默认清空，只放行 `kBaseEnvPass` 和策略的 `env_pass`（名字，或 `NAME*` 前缀）。

## 可观测性

审计写在沙箱看不到的地方：`<home>/logs/subos/<s>/events.ndjson`（按大小轮转）。

| 级别 | 记录 |
|---|---|
| `basic` | 生命周期（会话开始 / 结束、策略变更及差异）、权限判定、每次 `exec` 加入及其退出码 |
| `standard` | + rw 映射里改动的文件 |
| `full` | + 沙箱里执行过的每个程序（seccomp 用户态通知，由 session-init 安装；bwrap 后端） |

只记录环境变量的**名字**，不记录值；看起来像密钥的值一律不落盘。`XLINGS_TRACE=caps,provider,...` 打印诊断。

## 存储

| 存储 | 沙箱里的 home | 说明 |
|---|---|---|
| `shared`（默认） | `subos/<s>/home/<user>` | 与 shell 级进入共用 |
| `tmpfs` | tmpfs，退出即消失 | 需要 bwrap |
| `image` | `home.img`（ext4 稀疏文件，loop 挂载，需要 sudo） | 需要 bwrap |

存储只改变 home 放在哪里，不增加边界。实例的 home、`home.img` 和 xlings 无法证明归自己所有的文件是**用户数据**，
只有用户发起并确认过的删除（`subos remove` 等）才能删除它们（见 AGENTS.md "SubOS user data"）。

## 部署形态

- **用户 / 自定义 / 便携**：每个用户一个 home，`self update` 更新 entry。
- **系统包（S）**：`/usr/bin/xlings` 归包管理器所有。`self update` 不碰它，提示包管理器命令；
  `self update --user` 显式在 home 里装一个用户级 xlings。`/etc/xlings/config.json` 提供 `mirror` / `lang` 默认值
  和 `subos_policy_sources`，home 自己的 `.xlings.json` 覆盖它。`self doctor` 报告实际在用的 entry 和系统文件。
- **系统包（S）的 home**：用户第一次执行写命令（或 `self init`、`doctor --fix`）时，home 的 `bin/xlings` 被建成
  指向系统 entry 的链接——没有它，shim 表不会建立，装的包就不在 PATH 上。包管理器升级本体，所有用户跟着升级。
- **系统层（M）**：`sudo xlings install --system <pkg>` 装进 `/xlings`（root 拥有，`.xlings-home` 声明 `mode: multi`；
  `XLINGS_SYSTEM_LAYER` 可指定别处）。用户的 shell profile 把它的程序放在用户自己的之后、宿主之前：
  不复制，用户自己的版本优先，用户写不了它。
- **根（R）**：一台机器的整个用户态就是一个 SubOS（见下一节）。系统 home 是 `/xlings`（multi）或构建它的
  home 的路径（single，这个用户等价于 root）；`/etc/xlings/root.json` 是 stage-0 找到它的锚点。

## 根呈现（Part 2）

设计：`.agents/docs/2026-10-06-subos-architecture-design-part2.md`。一个 SubOS 的内容（工作区、home、策略）
与它的呈现方式无关；第三种呈现方式是**根**：SubOS 本身就是 `/`。

| 部分 | 行为 |
|---|---|
| 种类与角色 | `subos new --rootfs` 声明 `kind=rootfs`（`config/subos/<n>/instance.json`，沙箱里只读）；正在作为 `/` 的是 `host`，`boot.json` 里的是启动项。删除、策略、拷贝、启动、导出、回滚都问同一张表（`modules/subos/src/roles.cppm`） |
| `/usr` | 一代 = 一棵链接树：已注册的程序和库优先，再是各激活包 payload 的 `bin/`、`lib/`（普通发行版的语义，busybox 的 applet 就这样进 `/usr/bin`）；带 alias 的程序保留 shim；`usr/lib/modules/<ver>` 来自内核包 |
| 代 | `<subos>/root -> root.gen/<k>`，一次 rename 切换；工作区的每次变动（install / use / remove）产生新的一代；`subos rollback` 只切指针 |
| 机器状态 | `/etc` 只补缺（factory：`usr/share/factory/etc` 和 sysroot 的 `etc/`），sysusers 只追加；`/home`、`/var`、`/root` 不随代变化 |
| 进入实例 | bwrap：实例的树作为 `/`，uid 0，home 只读挂在原路径，实例本身同时是嵌套根的 `default`——里面的 xlings 就是这个根的包管理器；安装经 broker，立即出现在 `/usr` |
| `fetch=layer` | 沙箱里装的包进这个根自己的作用域；视图实例（没有根）拒绝进入 |
| 导出 | `subos export --rootfs/--tar/--disk`：实例成为镜像的 `default`，带上闭包里的 payload（工作区、已安装、每个 ELF 的 loader 与搜索路径）、静态 xlings、`root.json`、`boot/xlings-init` |
| 启动 | 内核 `init=<home>/boot/xlings-init`：stage-0 挂载内核文件系统，按 `boot.json`（试启动 once、默认、计数用尽后 fallback）选 SubOS，把 `/usr` 指过去，exec 它的 init；init 起不来就试下一个。`subos boot <n> --now`：busybox init 的 restart 重新执行 stage-0，不重启内核 |
| 库搜索 | xlings 的包按自己的 RUNPATH 闭包找库，从不读 cache；外来程序（`PT_INTERP=/lib64/ld-linux…`）的默认目录是 `/lib64` = 根的 `/usr/lib`（实测） |

## 跨平台

| 平台 | 后端 | 内核强制 |
|---|---|---|
| Linux | bwrap / landlock / proot | 是（proot 除外） |
| macOS 14+ | home 重定向；会话（`start`）不可用 | 否（advisory，路线见设计附录 A：Seatbelt） |
| Windows | USERPROFILE 重定向；会话不可用 | 否（advisory，路线：AppContainer） |

`subos status` / `subos doctor` 的平台矩阵来自实际探测（gates），不是手写的表。

## GPU 透传（`--gpu` / `--allow gpu`）

bwrap 后端默认只暴露最小 `/dev`。`--gpu`（等价于 `--allow gpu`）对宿主存在的 NVIDIA 节点、`/dev/dri` 做
`--dev-bind`，并只读绑定 `/sys`（libcuda / nvml 枚举 PCI 设备需要）。proot 下 `/dev`、`/sys` 原样透传，
`--gpu` 不改变什么。实现：`modules/subos/src/gpu.cppm`。

## 尚未实现

| 项 | 现状 |
|---|---|
| `net=proxy` | 拒绝（fail closed）；需要沙箱内转发器和 supervisor 桥接 |
| 根自己的 `/etc/ld.so.cache` | loader 从它 payload 的 `etc/` 读 cache；让它跟随调用路径需要 glibc 新 revision（xlings-res）。`/lib64 = /usr/lib` 已覆盖 cache 的用途 |
| 在 macOS / Windows 上呈现根 | 返回 unavailable；Windows 用 `subos export --tar` + `wsl --import` |
| 通过 broker 安装时的 hook 沙箱 | hook 在宿主侧按 owner 权限运行 |
| 加入会话的交互 shell 的作业控制 | 加入时提示：作业控制留在第一个 shell |
| interface 的 `subos_exec` 实时流式输出 | 结束后返回输出与退出码 |

## 代码在哪里

| 位置 | 内容 |
|---|---|
| `modules/subos/src/` | SubOS 核心：`model`、`policy` / `policy_store`、`spec`（策略 + 主机能力 → 沙箱描述）、`provider`（bwrap / proot 参数）、`caps` / `gates`（能力探测与平台矩阵）、`session`（supervisor 与 session-init）、`broker`、`manifest`、`userdata` |
| `modules/platform/src/platform/` | 所有系统调用：`process`（进程、信号、带描述符的本地 socket）、`isolation`（namespace、Landlock、seccomp、beneath 拷贝）以及各操作系统的实现 |
| `modules/runtime/src/` | `guard`（删除前的确认令牌）、`observe`（审计日志、脱敏） |
| `src/core/subos.cpp` 与 `src/core/subos/` | `xlings subos` 命令：生命周期；`run.cpp`（exec / start / cp）；`configure.cpp`（config / status / doctor）；`audit.cpp`（requests / report / ps / log）；`root_cmd.cpp`（new --rootfs / rollback / boot / export / diff / pack）；`root.cpp`（工作区 → 根的投影）；`sandbox.cpp`（把策略和主机能力接到会话上） |
| `modules/subos/src/rootfs`、`boot`、`roles`、`stage0` | 根投影与代、启动项选择、种类 × 角色的允许操作表、stage-0 |
| `tests/requirements.toml` | 每一项行为的需求 ID；`tests/unit`、`tests/e2e` 中的测试用 `covers` 认领 |
