# 沙箱与策略

[English](../11-sandbox-and-policy.md) | **简体中文**

一个 SubOS 隔离到什么程度，是声明在实例上的**策略**；在这台机器上由什么实现，是**后端**。
Linux 上沙箱是真正的边界（bwrap：文件系统、进程、网络、身份）；macOS 与 Windows 上只重定向 home，
**不是**安全边界——在那里运行不受信任的代码，请用操作系统级沙箱或虚拟机。

## 预设

| 预设 | 文件系统 | 进程 | 网络 | 身份 | 装包 |
|---|---|---|---|---|---|
| `dev` | xlings home 只读；只有本实例目录可写 | 独立的 pid / ipc / uts | 宿主的 | 宿主的 | 自动 |
| `private` | 同上 | 同上；禁止嵌套 user namespace | `nat` | 中性 | 询问 |
| `locked` | 同上；`--mount` 默认只读 | 同上 | `none`（只有 lo） | 中性 | 拒绝 |

任何预设都看不到宿主 home、其他实例和审计日志。环境变量清空，只保留基础变量和代理变量；
`--env-pass NAME` 再放行一个。

## 在沙箱里运行 agent

Agent 改代码、装依赖、跑测试，但不应该碰到你的 home、SSH 密钥和其他项目：

```bash
xlings subos new agent-ws --sandbox=private
xlings subos config agent-ws --mount ~/code/myproj:/work    # only this project, writable
xlings subos exec agent-ws -- sh -c 'cd /work && claude --task "fix the failing tests"'
```

Agent 应该声明自己，这样任何命令都不会停下来等待回答（[50](50-agents-and-machine-interface.md)）：

```bash
export XLINGS_AGENT_MODE=1
xlings subos use agent-ws        # an interactive entry is refused (exit 2): use subos exec
```

## 不受信任的代码

没有网络，代码只读，事后有记录：

```bash
xlings subos new quarantine --sandbox=locked
xlings subos config quarantine --observe full
xlings subos exec quarantine --mount ~/Downloads/suspicious-repo:/src:ro --timeout 5m -- sh -c 'cd /src && ./build.sh'
xlings subos report quarantine        # every program it ran, every file it changed
xlings subos exec --temp --sandbox=locked -- ./untrusted-binary   # an instance just for this; its audit stays
```

单次调用只能收紧实例的声明：在 `locked` 实例上用 `--net host` 会被拒绝（125）。要放宽，在外面修改声明：
`xlings subos config quarantine --net nat`。

## 设置项

| 设置 | 取值 | |
|---|---|---|
| `--net` | `host`、`nat`、`none`、`proxy` | 网络（见下文） |
| `--proxy` | `socks5h://host:port` | 唯一的出口，配合 `--net proxy` |
| `--tz` | `utc`、`proxy`、时区名 | 中性身份显示的时区 |
| `--fetch` | `auto`、`ask`、`deny` | 在里面装包 |
| `--index-update` | `auto`、`ask`、`deny` | 在里面更新索引 |
| `--observe` | `basic`、`standard`、`full` | 记录什么（见下文） |
| `--allow` / `--disallow` | 一项授权 | 打开一样东西（见下文） |
| `--grants-allowed` | 授权列表 | 单次调用可以自己加哪些授权 |
| `--mount` / `--unmount` | `host:inside[:ro]` | 把宿主目录映射进来 |
| `--env-pass` | `NAME` 或 `NAME*` | 放行一个环境变量 |

写在 `subos config` 上是声明；写在单次调用上只能收紧，或在 `grants_allowed` 范围内加授权。

### 规则

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

只有 owner 在沙箱外能修改策略；沙箱里的进程能读到它。策略里有本客户端不认识的值（`"net": "vpn"`）时拒绝进入，
而不是忽略（fail closed）。策略写明能执行它的最低客户端版本（`min_client`），更旧的客户端拒绝进入。

### 策略包

团队的策略可以是一个包（`type = "subos-policy"`，根目录放 `policy.json`）：

```bash
xlings subos config ci --sandbox xim:agent-confined     # shows what it changes against its preset
xlings subos config ci --policy-upgrade                 # after the package moved: explicit, with the diff
```

策略复制进实例并锁定（`resolved.from`、`sha256`）：包更新不会自己改变已有实例。管理员可以在
`/etc/xlings/config.json` 里限制来源（`{"subos_policy_sources": ["xim:*"]}`）。

## 在里面装包

沙箱里的 xlings 照常可用：读操作在里面执行；`install`、`remove`、`update` 交给外面的 broker，按实例的策略判定后在外面执行：

```bash
xlings subos config dev --fetch ask      # auto (default) / ask / deny
# inside:   xlings install jq            -> exit 75: queued for approval
xlings subos requests dev                # outside: what waits
xlings subos approve dev r1a2b3c         # or deny
```

只有 owner 能做的事（`self update`、改策略、动别的实例、切换 xlings 版本）在里面返回 13，并给出在外面执行的命令。

## 网络与身份

| `--net` | | 宿主本机的服务 | 出口 |
|---|---|---|---|
| `host` | 宿主网络 | 可达 | 宿主 |
| `nat` | 自己的 net namespace，经 pasta 接入；`--publish 8080:80` | 不可达（`--allow host-loopback` 才可以） | 宿主地址 |
| `none` | 只有 lo | 不可达 | 无 |
| `proxy` | 只有 lo；`127.0.0.1:1080` 的 SOCKS5h 入口把每个连接交给声明的代理 | 不可达 | 代理 |

```bash
xlings subos config box --sandbox private --proxy socks5h://127.0.0.1:1080
xlings subos exec box -- curl https://example.com
```

`proxy` 下域名由代理解析（socks5h），从不由宿主解析；不支持 SOCKS5h 的客户端、直接 IP 和 UDP 都过不去，也不会退回别的网络。
中性身份：用户名 `user`，实例自己的主机名和 machine-id（只生成一次并保持不变），`LANG=C.UTF-8`，时区为 UTC、
你选定的时区或代理出口所在时区（经代理查询）——从不用宿主的。

## 授权

默认全部关闭；每一项只打开一样东西（一个 socket、一组设备节点），从不暴露宿主的整个运行时目录：

```bash
xlings subos config gui-app --allow display      # X11 / Wayland
xlings subos config gui-app --allow audio        # PulseAudio / PipeWire
xlings subos config ci --allow ssh-agent         # the agent socket, not ~/.ssh
xlings subos config ml --allow gpu               # NVIDIA nodes and /dev/dri
xlings subos config work --allow clipboard       # copy into the host's clipboard
xlings subos config work --allow clipboard-paste # read the host's clipboard
```

授权有 `display`、`audio`、`camera`、`gpu`、`ssh-agent`、`dbus`、`host-loopback`、`clipboard`、`clipboard-paste`、`desktop-control`（Agent 可以经总线操作系统的 Luban 桌面：输入与显示器）。

剪贴板不等于显示。授权后，沙箱里程序本来就会调用的 `wl-copy`、`wl-paste`、`xclip`、`xsel` 经 broker 到达宿主剪贴板，
由 broker 运行宿主一侧的工具（来自索引：`wl-clipboard`、`xsel`）。没有授权时，复制经终端（OSC 52）生效于你所坐那台机器的剪贴板。
`xlings subos status <name>` 显示当前是哪一种。

## 审计

写在外面、沙箱看不到的地方：`<home>/logs/subos/<name>/events.ndjson`，每个会话另存 `sessions/<id>.ndjson`。

```bash
xlings subos log dev                  # every event
xlings subos log dev --kind perm      # permission decisions
xlings subos log dev --kind exec -f   # programs run, followed
xlings subos report dev --json        # per-session summary
```

| `--observe` | 记录 |
|---|---|
| `basic`（默认） | 会话开始与结束、策略变更、权限判定、每次 `exec` |
| `standard` | + 可写映射里改动的文件 |
| `full` | + 里面执行过的每个程序 |

记录程序名和参数个数，不记录参数值；记录环境变量名，从不记录值。审计写不进去时 `locked` 拒绝启动或结束会话，其他档位警告。

## 后端

| 后端 | 机制 | 能给的 | 给不了的 | 何时使用 |
|---|---|---|---|---|
| **bwrap** | namespace | 全部预设 | — | 默认：root 拥有的 `/usr/lib/xlings/bwrap`、发行版的、然后是索引的 |
| **landlock** | Landlock LSM（Linux ≥ 5.13），没有 namespace | 写入围栏：只有实例、`/dev`、`/proc` 和可写映射可写 | 隐藏文件、pid / net / 身份隔离；宿主 socket 仍可连接——不能用于不受信任的代码 | 只在 `--sandbox landlock` 时 |
| **proot** | ptrace | 文件系统视图 | 安全边界 | `--sandbox proot`，或没有 bwrap 时 |
| **home 重定向** | 环境变量 | home | 其余（advisory） | macOS、Windows |

后端给不了的项：`dev` 下降级并说明原因；`private` 与 `locked` 下拒绝进入（125），逐项列出缺什么、怎么修。

宿主视图（bwrap、proot）呈现 `/usr`，以及按宿主实际解析位置呈现的 `/bin`、`/sbin`、`/lib*`：usr-merge 的宿主上是指向
`/usr` 的链接，非 usr-merge 的宿主上是它们自己的目录（Alpine 的 `/lib`、openSUSE Leap 15 的 `/lib64`）。

### 叠加在系统 home 之上的 home 如何执行配方

与下层系统 home（`/xlings` 或 `XLINGS_SYSTEM_LAYER`）共享 store 的 home，在 worker 中执行配方（index 构建、借来的 payload
的配置），worker 不能写下层。能启动 bwrap 时由 bwrap 运行该 worker。Luban 系统内禁止嵌套 user namespace，bwrap 无法启动；
此时 worker 在读取配方之前建立 Landlock 写入围栏（只有它的 scratch、设备、hook 日志与正在安装的 payload 可写），pid 由系统
自身的命名空间与宿主隔开。宿主上没有 bwrap 时照旧拒绝，并给出修复命令。声明了 `upstreams` 的 home 通过上游借用（在自己
store 中的链接条目），没有系统层。

### 一次性设置（Ubuntu 23.10 及以后）

Ubuntu 用 AppArmor 限制非特权 user namespace；bwrap 会报 `setting up uid map: Permission denied`。不需要改 sysctl：

```bash
xlings self doctor --isolation          # why
xlings self doctor --isolation --fix    # one sudo: a root-owned bwrap and one AppArmor profile for it
```

需要沙箱时 `subos new` 会当场询问；agent 模式下以退出码 2 结束并给出这条命令。
