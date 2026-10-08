# SubOS 总体架构设计：部署与运行、控制与配置、使用面、隔离、可观测性、模块化

- 起因：#640（SubOS 隐私隔离）及其评论（Ubuntu 24.04 bwrap 探针失败的根因）
- 基线：`v2026.10.4.1`
- 整合并取代以下设计中与 SubOS 相关的部分：
  - `docs/design/subos-isolation.md`
  - `old/agents-docs/sandbox-cross-platform-design.md`（L0–L6）
  - `old/docs/plans/2026-05-05-subos-tiered-mode-architecture.md`（light / medium / heavy / full）
  - `old/agents-docs/sandbox-v5-dual-backend-design.md`、`sandbox-v6-storage-isolation-design.md`
  - `subos-as-xpkg-design-2026-05-16.md`（`--from`、keeper、D8 一次性实例）
  - `2026-05-22-subos-sandbox-gpu-passthrough.md`
  - 相关：`2026-09-06-issue-583-relocation-design.md`、`src/core/home_identity.cppm`、`src/core/uimode.cppm`
- 状态：设计草案第七轮，一份完整方案，不拆分。附录 B 是自我 review（R1–R16 是第六轮的，R17 起是第七轮的）。本文不含代码改动

> **隐私规则**：本文所有实测只记录计数、是/否和变量**名**，不记录任何值。issue、PR、CI 日志、doctor 输出、审计日志的默认脱敏都按这个规则。

---

## 0. 一句话与文档地图

**核心功能与交互面分离；控制完全由声明驱动；用户面多平台一致，技术面可以替换。**

| 问题 | 唯一回答者 | 章节 |
|---|---|---|
| xlings 装在哪、谁拥有、怎么更新 | `.xlings-home` 标记 | §3–§4 |
| 这次运行是谁、用哪些 home、作用在哪 | xlings 核心的 `HomeContext` | §5 |
| 允许做什么（获取、授权、网络……） | **策略**：预设 → 覆盖 → 规则 → 策略包；由 `policy.decide()` 判定 | §7–§9 |
| 人和 agent 怎样使用 | 交互面：受众契约（human / agent）× 渲染（cli / tui / json / ndjson） | §12–§13 |
| 实际给了什么隔离 | `Policy + Caps → SandboxSpec`，由 provider 执行，由 supervisor 托管 | §15–§21 |
| 发生了什么 | 统一事件模型；审计由 supervisor 写到沙箱够不到的地方 | §22 |

```
Part I    现状 ............ §1 实测  §2 缺口
Part II   部署与运行 ...... §3 概念  §4 部署形态  §5 运行形态与层叠 home  §6 目录规范
Part III  控制与配置 ...... §7 策略模型  §8 SubOS 内的 xlings  §9 权限执行
Part IV   使用面 .......... §10 隔离的渐进式披露  §11 目录映射  §12 进入与外部执行  §13 人与 agent 两套语义  §14 平台一致性
Part V    技术架构 ........ §15 轴  §16 编译器与会话模型  §17 平台抽象层  §18 能力探测  §19 网络与身份  §20 特权组件与 rootfs  §21 性能与能力不缺失
Part VI   可观测性 ........ §22
Part VII  代码架构 ........ §23
Part VIII 质量与交付 ...... §24 测试、CI 与开发工具（testkit / xdev）  §25 稳定性规范  §26 交付 checkpoint  §27 决策
附录 A 其他平台的技术路线参考　　附录 B 自我 review
```

---

# Part I 现状

## 1. 实测（在真实的 xlings bwrap 沙箱里）

环境：Ubuntu 24.04、kernel 6.8、bwrap 0.11.2（xim 包）。测量直接在一个真实的 `subos use <s> --sandbox bwrap` 会话里进行，并按 `build_bwrap_argv_()` 嵌套复现了一次，结论一致。

| # | 维度 | 实测 | 根因 | 严重度 |
|---|---|---|---|---|
| F1 | **宿主代码执行面可写** | `config/shell/xlings-profile.sh`、`bin/xlings`、`data/xpkgs/`、`.xlings.json` 全部可写 | `$XLINGS_HOME` 以 `--bind`（读写）挂入 | **S0 逃逸** |
| F2 | **X11** | 抽象 socket 握手返回 Success（完全访问）：可截屏、记录键盘、注入输入 | 没有 `--unshare-net`；加上桌面默认的 `si:localuser` 授权 | **S0 隐私** |
| F3 | 环境变量 | 继承了 103 个，其中有 `*_TOKEN`、`SSH_AUTH_SOCK`、`DBUS_SESSION_BUS_ADDRESS`、`XAUTHORITY` | 没有 clearenv | S1 |
| F4 | 进程 | 宿主约 830 个进程可见，374 个的 `cmdline` 可读（`/proc/<pid>/root` 被拒是 userns 的副作用） | 没有 `--unshare-pid` | S1 |
| F5 | 网络身份 | 出口、DNS、IPv6 与宿主相同；能看到 6 条 ARP 邻居（网关 MAC 可以作为位置指纹） | 没有 `--unshare-net` | S1 |
| F6 | 跨实例 | 能读写其他 22 个 SubOS 的 `home/` | 同 F1 | S1 |
| F7 | 主机标识 | hostname、localtime、TZ、LANG 都继承宿主 | 没有 `--unshare-uts` | S2 |
| F8 | 终端注入 | 没有 `--new-session`，也没有 seccomp；本机 `legacy_tiocsti=0` 挡住了 | — | S2 |
| F9 | D-Bus / 设备 | 按路径都不可达，最小 /dev，`CapEff=0`（**issue 这部分判断正确**） | — | 已隔离 |
| F10 | backend | recipe 和文档都写 setuid，实测属主是用户、0755，setuid 静默失败，所以撞上 AppArmor | install hook 里执行 sudo | 文档与事实不符 |
| F11 | keeper | `--keep` / `--ttl` 没有任何作用；原设计只 `nsenter --mount` | — | 潜在旁路 |
| F12 | 探针文案 | 原因判断错误，并建议全局 `sysctl …=0` | `classify_bwrap_probe_error_` | 误导 |
| F13 | CI | 拿不到 backend 时 e2e 直接 skip | — | 从来没有被正向验证 |
| F14 | AUR 包 | 停在 0.4.14；`/usr/share/xlings/.xlings.json` 是 766 | 系统安装形态没有定义 | 安全隐患 |
| F15 | 可观测性 | 进入沙箱用 `execvp` 替换了 xlings 进程，之后没有任何进程在沙箱外观察它；仅有的记录都在沙箱可写的范围内 | 没有 supervisor | 不可审查 |
| F16 | 外部执行 | 只有 `subos use <s> --cmd '<shell 字符串>'`：需要 shell 转义，每次都冷启动；interface 里**没有**在 SubOS 中执行命令的 capability | — | 不方便 agent 使用 |

## 2. 架构缺口

部署形态没有定义；主体和权限没有定义；隔离模型混杂；没有能力探测；没有可观测性；**没有面向 agent 的执行面**；**交互语义（人 / agent）没有正式的契约**；模块之间双向依赖；平台差异没有抽象层。

---

# Part II 部署与运行（已确认）

## 3. 核心概念

| 概念 | 定义 |
|---|---|
| Entry | xlings 可执行文件；所有 shim 都指向它 |
| Home | 数据根（payload 池、索引、配置、实例），由 `.xlings-home` 声明 |
| Layer | home 在层叠关系中的位置，每层只有一个所有者 |
| Principal | 这次运行的主体：某一层的 owner，或者某个实例 |
| Scope | 激活和配置作用在哪：SubOS / 项目 |
| Instance | 具名环境：工具视图 + 策略 + 可选的 rootfs |
| **Policy** | 实例的控制声明（§7） |
| **Session** | 一个实例当前正在运行的环境（一组 namespace + 一个 supervisor）；多次进入和外部执行可以共享同一个会话（§16） |
| **Supervisor** | 宿主侧托管会话的进程：负责 broker、审计、网络桥接、清理 |
| **Broker** | supervisor 上的服务：替沙箱里的 xlings 执行它无权执行的操作 |
| **Surface（交互面）** | CLI（human / agent）、`--json`、interface（NDJSON）、GUI；核心功能与交互面分离（§13） |

## 4. 部署形态

| 形态 | entry | home | 更新渠道 |
|---|---|---|---|
| **U 用户模式（默认）** | `~/.xlings/bin/xlings` | `~/.xlings` | `self update` |
| **C 自定义目录** | `<dir>/bin/xlings` | 任意目录（路径里可以不出现登录名） | `self update` |
| **P 自包含 / 便携** | `<dir>/bin/xlings` | entry 所在的目录树 | `self update`；移动后 `self relocate` |
| **S 系统安装** | `/usr/bin/xlings` | 每个用户仍是 U 或 C；系统只提供 root 拥有的组件 | 系统包管理器 |
| **M 系统包管理器（多用户）** | 同 S | S + root 拥有的系统层 `/opt/xlings` | 同 S；管理员 `sudo xlings install --system` |

- 形态写在 `.xlings-home` 里：`{ "layout": 2, "mode": "user|portable|system-layer", "upper": "..." }`。推断只作为老 home 的迁移路径。
- 系统组件（S / M，或 U / C 用户执行 `self doctor --isolation --fix` 时安装）：

| 组件 | Linux | macOS | Windows |
|---|---|---|---|
| 特权辅助程序 | `/usr/lib/xlings/bwrap` | Seatbelt profile 目录 | AppContainer 辅助程序 |
| 安全策略 | `/etc/apparmor.d/xlings-bwrap` | — | — |
| 系统配置 | `/etc/xlings/config.json` | `/etc/xlings/` | `%ProgramData%\xlings\` |
| 系统层（M） | `/opt/xlings` | `/opt/xlings` | `%ProgramData%\xlings\home` |

- S / M 下 `self update` 不碰 root 拥有的 entry，只提示系统包管理器的命令；用户可以用 `self install --user` 装一个用户层的 entry；`self doctor` 报告实际生效的 entry。

## 5. 运行形态与层叠 home

- **HomeContext**（属于 xlings 核心）：保留现有的解析顺序，结果收敛为 `{ entry, mode, layers[], principal, scope }`，所有读者都从它取值。SubOS 模块只接收纯数据视图 `HomeView`。
- 沙箱里的 `principal = instance` 只用于给出友好提示；**安全判定在 broker（宿主侧）和内核**。
- **层叠 home**：系统层（root）→ 用户层（用户）→ 实例私有层（实例）。三条规则：读和激活可以跨层；获取只写自己的层，没有可写层的交给 broker；上层永远不引用下层。
- 项目模式是作用域，与部署形态正交；一个项目可以绑定一个实例。

## 6. 目录规范

### 6.1 用户层 home

| 路径 | 唯一写者 | 类别 | 沙箱内可见性 |
|---|---|---|---|
| `.xlings-home` | `self init` / migrate | 派生 | 只读 |
| `.xlings.json` | config | 用户配置 | 只读 |
| `bin/xlings` | `entry_binary` | 派生 | 只读（始终挂入） |
| `config/shell/` | `self init` | 派生 | 只读 |
| **`config/subos/<name>/policy.json`（新）** | **owner（subos 模块）** | **声明** | **只读**（沙箱能读到自己的策略，但改不了） |
| `data/xpkgs/`、`data/xim-*`、`data/runtimedir/` | owner 侧的 xim / broker | 派生 | 只读 |
| `state/`（新：caps 缓存、会话登记） | 各模块 | 派生 | 不可见 |
| `logs/`（hooks、destructive、ops） | 日志模块 | 审计 | 不可见 |
| `logs/subos/<name>/`（新） | **supervisor** | 审计 | **不可见** |
| `subos/<name>/` | subos | 见 6.2 | 只挂入自己 |

**策略放在实例目录之外**（`config/subos/<name>/policy.json`）。这回答了上一轮留下的问题：实例的 `.xlings.json`（`workspace` / `configured` / `storage`）在沙箱里保持可写，版本切换照常在本地执行，不需要经过 broker；策略文件位于只读的 `config/` 下，沙箱从结构上就改不了它。fork（`--from`）、导出、删除时，subos 模块会一起处理这两个文件。

### 6.2 实例目录

| 路径 | 唯一写者 | 类别 |
|---|---|---|
| `.xlings.json` | subos manifest（沙箱内可写） | 声明（工作区） |
| `bin/`、`lib/`、`usr/`、`.shim-view/`、`generations/` | xvm / installer | 派生 |
| `home/<user>/`、`home.img`、`tmp/` | 实例内的进程 | **用户数据** |
| `etc/` | spec 编译器 | 派生 |
| `sandbox-root/`、`subos/`（空标记） | sandbox 初始化 | 派生 |
| `.xlings-layer/`（新，`fetch=layer`） | 实例内的 xlings | 派生 |
| `rootfs/`（新） | 实例内的包管理器 | **用户数据** |
| `run/`（新：`broker.sock`、`exec.sock`） | supervisor | 派生 |
| `logs/`（新：沙箱内的调试日志） | 实例内的 xlings | 派生（**不作为审计依据**） |

### 6.3 规范

每个文件只有一个写者，上表就是清单；数据类别沿用 AGENTS.md "SubOS user data"；**审计与被审计的对象物理分离**；**策略与被约束的对象物理分离**。

---

# Part III 控制与配置

## 7. 策略模型：预设 → 覆盖 → 规则 → 策略包

### 7.1 四个层次（与使用上的分级一一对应）

| 层次 | 给谁 | 写法 | 例子 |
|---|---|---|---|
| ① 预设 | 所有人 | 一个词 | `--sandbox=private` |
| ② 覆盖 | 常用调整 | 命令行参数或 `subos config` | `--fetch ask --net proxy=… --allow gpu` |
| ③ 规则 | 细粒度控制 | 策略文件里的有序规则 | 按包、按索引、按大小、配额、超时 |
| ④ 策略包 | 组织、团队、复用 | **xpkg** | `--sandbox=xim:policy-ci-strict@1` |

内置的 `dev` / `private` / `locked` **在概念上就是内置的策略包**，所以四个层次是同一套数据模型，只是来源不同。

### 7.2 策略文件的结构

```jsonc
// <home>/config/subos/<name>/policy.json
{
  "extends": "private",                         // 内置预设或策略包：xim:policy-ci-strict@1
  "resolved": { "from": "xim:policy-ci-strict@1.2.0", "sha256": "…" },   // 锁定：包更新时不会悄悄改变策略
  "isolation": { "net": "proxy", "proxy": "socks5h://127.0.0.1:1080",
                 "identity": { "tz": "UTC" }, "grants_allowed": ["gpu", "display"] },
  "mounts": [ { "src": "~/proj", "dst": "/work", "mode": "rw" } ],
  "permissions": {
    "fetch": {
      "default": "ask",
      "rules": [                                 // 有序，第一条匹配的生效
        { "match": "xim:*",        "index": "official", "action": "auto" },
        { "match": "*",            "size_gt": "2GB",    "action": "deny" },
        { "match": "community:*",                       "action": "ask"  }
      ],
      "quota":   { "requests_per_session": 50, "bytes": "20GB" },
      "ask":     { "timeout": "10m", "on_timeout": "deny" }
    },
    "index_update": "auto",                      // auto | ask | deny
    "exec_from_outside": "owner"                 // 谁可以在外部往这个实例里执行命令（§12）
  },
  "observe": { "level": "standard", "retention": "30d", "redact": true }
}
```

### 7.3 规则

- **合并顺序**：内置默认值 → `extends` 链 → 本文件 → 单次调用。**单次调用只能收紧，或者在 `grants_allowed` 列出的范围内授权**，不能放宽。
- **谁能修改**：只有 owner。每次修改都写一条 `lifecycle` 审计事件，附带修改前后的差异。
- **策略包**：
  - 是一种 xpkg 类型（`type = "subos-policy"`）；
  - 由 owner 选择，选择时显示**与内置预设相比放宽了哪些项**；
  - 安装后锁定版本和 sha256，升级要显式执行 `subos config <s> policy upgrade`；
  - S / M 形态下，系统配置可以限制允许的策略包来源。

  环境模板（`--from subos:xxx`，已有的 subos-as-xpkg）可以携带一份策略，规则相同。
- **校验（fail closed）**：策略里出现**本版本不认识的、与安全相关的字段**（例如 `net: "vpn"`）时，**拒绝进入**，而不是忽略。这与 manifest 写入时"保留不认识的键"不矛盾：写入时保留，是为了不破坏数据；执行时拒绝，是为了不悄悄放宽。

## 8. SubOS 内的 xlings：一定存在，按权限提供全部功能（已确认）

### 8.1 操作分类

| 类别 | 例子 | 沙箱内的执行方式 |
|---|---|---|
| 只读查询 | `list` / `info` / `search` / `which` / `status` / `self doctor` | 始终本地执行 |
| 作用域内的状态 | `use`（版本切换）、激活已有 payload、`remove`（只从本实例摘掉）、实例 `.xlings.json` | 始终本地执行 |
| 获取 payload、同步索引 | `install` 一个还没装的包、`update` | 按 `permissions.fetch` / `index_update` 的规则 |
| home 级操作、其他实例、策略 | `self update`、xlings 自身版本、全局配置、`subos new/remove`、`subos config` | 只有 owner 能做：返回 `E_PERMISSION` 并给出宿主侧命令 |

### 8.2 获取动作

| 动作 | 行为 | 写到哪里 | 默认用于 |
|---|---|---|---|
| `auto` | broker 自动批准：校验 sha256；hook 在受限的 hook 沙箱里运行 | 用户层（共享） | dev |
| `ask` | 生成待审批请求（human：等待并提示；agent：见 §13.3） | 用户层 | private |
| `layer` | 沙箱里的 xlings 自己获取 | 实例私有层 | rootfs |
| `deny` | `E_PERMISSION` + `xlings install X --subos <s>` | — | locked |

审批只走沙箱无法伪造的通道（沙箱外的命令或 interface 事件），不在沙箱共享的终端里提示。broker 复用 interface 的 NDJSON 协议，通过 `run/broker.sock` 通信。

## 9. 权限执行：三层，一个判定函数

1. **客户端**：沙箱里的 xlings 读取只读的 `policy.json`，调用 `policy.decide(op)` 提前给出友好提示。
2. **broker**：在宿主侧对同一个请求**用同一个 `policy.decide(op)` 再判定一次**，这是可信的判定。
3. **内核**：只读挂载 / Landlock 兜底。macOS / Windows 目前没有这一层，标为 `advisory`。

新增 `xlings install ... --subos <name>`（与 `remove --subos` 对称），以及 `subos requests / approve / deny`。**实现前必须先盘点**沙箱里激活时实际会写哪些文件（inotify e2e），用来核对 §6 的写者表。

---

# Part IV 使用面

## 10. 隔离的渐进式披露（已确认）

| 层 | 用户需要了解的 |
|---|---|
| 0 | 什么都不用了解：`subos new work && subos use work` |
| 1 | `--sandbox[=dev\|private\|locked]`，默认 dev |
| 2 | `--mount`、`--allow`、`--net`、`--fetch` |
| 3 | `subos config` 设置细项 / 编辑 policy.json 的规则 |
| 4 | 策略包、`--rootfs`、`--storage` |

| 档位 | 一句话 | fetch | net | 观测 |
|---|---|---|---|---|
| `dev` | 看不到宿主文件和其他 SubOS，不能篡改 xlings，网络照常；做不到的项只提示 | auto | host | basic |
| `private` | + 网络、桌面、身份隔离；必须项做不到就不进入 | ask | nat / proxy | standard |
| `locked` | + 断网，映射默认只读 | deny | none | full |

两种提示：`✗ 无法进入：缺少必须项…`，`! 已进入：X 未生效…`；CI 用 `--no-degrade`。安全默认落在实例上：声明过的实例以任何方式进入都执行策略。命令行兼容：`--sandbox=<档位>` 没有歧义；有歧义时报错。

## 11. 目录映射（已确认）

`--mount <宿主>[:<沙箱内>][:ro|rw]`，与 docker `-v` 一致；省略目标时映射到相同路径（第二段恰好是 `ro` 或 `rw` 时按模式解析，例如 `~/.gitconfig:ro`）；默认 rw，locked 下默认 ro；宿主路径不存在就报错；不能覆盖系统路径；不能映射 `$XLINGS_HOME` 或它的上层目录；rw 映射标为"宿主代码执行面"。各平台：bwrap 用 bind；Landlock 用访问规则 + 链接；macOS / Windows 用链接或 junction（advisory）。

## 12. 进入与外部执行

### 12.1 两种使用方式，一个会话模型

| 方式 | 命令 | 适合 |
|---|---|---|
| **进入**（交互） | `xlings subos use <s>` | 人在里面工作 |
| **外部执行**（一条命令） | `xlings subos exec <s> [选项] -- <argv...>` | 脚本、agent、CI |
| **启动 / 停止会话** | `xlings subos start <s> [--ttl 30m]` / `subos stop <s>` | agent 需要连续执行很多条命令时（热会话） |
| **一次性实例** | `xlings subos exec --temp [--from <s\|xpkg>] [--sandbox=locked] -- <argv>` | agent 试跑、测试；结束后自动删除，**审计和报告保留** |
| 传文件 | `xlings subos cp <src> <s>:<dst>` / `<s>:<src> <dst>` | 不想设置映射时 |
| 端口转发 | `--publish 8080:80`（nat / none 模式） | 在隔离网络里测试服务 |

- **同一个实例同一时刻只有一个会话**。`use` 和 `exec` 在会话已经运行时都**加入**这个会话（共享 /tmp、进程空间、网络），没有运行时就新建。所以人在一个终端里 `use`，agent 在另一处 `exec`，看到的是**同一个环境**。
- 单次调用如果改变了 spec（例如运行中的会话是 `nat`，这次要求 `--net none`），不能加入，而是**另开一个临时会话**并说明原因。不会悄悄地以不一致的隔离执行。
- `exec` 使用 **argv 形式**（`--` 之后原样传递），不需要 shell 转义；`--cmd '<字符串>'` 保留为兼容写法（等价于 `-- sh -c '<字符串>'`）。
- `exec` 的选项：`--cwd`、`--env K=V`（只能在策略的 `env_pass` 范围内）、`--mount`（单次）、`--timeout`、`--stdin`、`--json`（结束时输出结构化结果）。

### 12.2 退出码（参考 docker / timeout 的约定）

| 退出码 | 含义 |
|---|---|
| 命令自己的退出码 | 命令已经启动并结束 |
| 125 | **命令启动之前**失败（隔离必须项不满足、实例不存在、策略校验失败……）；`--json` 里有 `phase: "setup"` 和具体的 code |
| 126 / 127 | 命令无法执行 / 找不到 |
| 124 | `--timeout` 超时 |
| 128 + n | 命令被信号 n 终止 |

非 exec 命令的退出码沿用现有规则：`E_PERMISSION` 用 13，等待审批用 75（EX_TEMPFAIL），在 §13 的契约里统一列出。

### 12.3 agent 的典型用法

```bash
xlings --agent subos exec --temp --from subos:py-ds@1 --sandbox=locked --json -- pytest -q
# 或者需要连续多步时：
xlings --agent subos start t1 --sandbox=private --ttl 20m
xlings --agent subos exec t1 -- pip install -e .         # 获取由 fetch 规则管
xlings --agent subos exec t1 --json -- pytest -q
xlings --agent subos report t1 --json                    # 行为报告
xlings --agent subos stop t1
```

interface 新增 capability：`subos_exec`（流式输出 stdout / stderr 片段和最终结果）、`subos_start` / `subos_stop`、`subos_status`、`subos_events`、`subos_requests` / `subos_approve`、`subos_report`。

## 13. 人与 agent：两套交互语义

### 13.1 两个正交的轴

- **受众（语义契约）**：`human` | `agent`。决定问不问、怎么报错、文案稳定性、等待时的行为。
- **渲染**：`cli` | `tui`（`uimode` 已有），加上 `--json`（单条命令的结构化结果）、`interface`（NDJSON 会话）。

现有的 `uimode` 刻意只有两种渲染模式，并写明"交互方式是配置，不是模式"。本方案沿着这个方向，把**受众**也做成一项声明配置，而不是新增一种模式。

### 13.2 怎样声明（不推断）

| 方式 | 作用范围 |
|---|---|
| `--agent`（已有） | 单条命令 |
| `XLINGS_AGENT_MODE=1`（新增） | 整个进程树。**会被允许传入沙箱**，所以 agent 在 SubOS 里调用的 xlings 也遵守 agent 契约 |
| `--ui-mode` / `--json` / `interface` | 渲染，与受众正交 |

不根据 tty 推断受众（`uimode` 的注释已经说明：有 pty 不代表有人会按键）。没有声明时就是 `human`。

### 13.3 契约

| 方面 | human | agent |
|---|---|---|
| 确认（有默认值） | 有终端就问；`-y` 跳过 | **从不提问**：没有 `-y` / `"yes": true` 就不执行，退出码 2，并说明需要什么 |
| 选择（没有默认值） | 交互式选择 | 报错 + **结构化候选列表** |
| 等待审批（`fetch=ask`） | 提示并等待（有超时） | 默认**不阻塞**：返回请求 id，退出码 75；`--wait <时长>` 才阻塞 |
| 错误 | 解释 + 建议，本地化 | 稳定的英文文案 + **错误码** + 机器可读的 `fix`（命令数组） |
| 降级提示 | `! X 未生效 …` | 结构化的 `missing[]{dimension, reason, fix}` |
| 进度 | 进度条 / TUI | 没有重绘；需要时输出 NDJSON 事件 |
| 帮助 | `--help` | `--help --json`（已有 `help_json`）、`xlings agent skills`（已有） |
| 退出码 | 同一张表 | 同一张表（§12.2），写进文档并加测试固定下来 |

### 13.4 核心与交互面分离（适用于整个 xlings，不只是 SubOS）

- **核心只产出结果和事件**（`EventStream` 已经存在），**不提问、不判断受众、不拼文案**。
- **提问通过端口**：核心需要确认时调用交互面提供的 `Asker`（已有 `confirm::ask()` 两层提问的设计，迁到 `modules/guard`）。human 交互面问用户；agent 交互面按契约直接给出答案（有 `-y` 就是 yes，否则拒绝）。
- 行为上的差异（例如审批时阻塞还是不阻塞）由交互面以**显式参数**传给核心（`wait: none|until(t)`），核心里**不出现** `if (agent)` 这种分支。
- 文案和 i18n 只在交互面里；错误码、`fix` 和事件 schema 是核心的契约，两种受众共用。
- 这条规则在 SubOS 模块里已经落地（§23：模块只返回结构化诊断），并逐步推广到 xim / xvm / xself（不在本 PR 的范围内，作为方向写进 AGENTS.md）。

## 14. 平台一致性（已确认）

✓ 内核强制　◐ 部分支持或仅提示　○ 暂未实现（计划中，路线见附录 A）　— 不适用

| 功能 | Linux | macOS | Windows |
|---|---|---|---|
| 切换工具链、SubOS 内 xlings 全功能、进入 / exec / start / temp / cp | ✓ | ✓ | ✓ |
| 私有 home / tmp | ✓ | ◐ 环境变量 | ◐ 环境变量 |
| 宿主文件不可见 | ✓ | ○ Seatbelt | ○ AppContainer |
| xlings 写保护 / 权限 | ✓ | ◐ 客户端 + broker | ◐ 客户端 + broker |
| 目录映射 | ✓ | ◐ 链接 | ◐ junction |
| 进程隔离 / 会话内进程管理 | ✓ | ◐ 进程组 | ◐ Job Object |
| 网络：断网 / 只走代理 | ✓ | ○ Seatbelt（断网） | ○ AppContainer（断网） |
| 桌面、设备控制 | ✓ | ○ | ○ |
| 身份 | ✓ | ◐ 环境变量 | ◐ 环境变量 |
| 观测：ops / lifecycle / perm / fs | ✓ | ✓ | ✓ |
| 观测：exec | ✓ seccomp | ○ | ○ Job Object 通知 |
| 观测：net | ✓（nat / proxy） | ○ | ○ |
| 独立 rootfs | ○ bwrap | ○ VM | ○ WSL2 |
| 部署形态 U / C / P | ✓ | ✓ | ✓ |
| 部署形态 S / M | ○ | ○ | ○ |

提示规范：**暂未实现**（`—— <平台>上暂未实现（计划中）` + 能换用的做法）/ **环境限制**（原因 + 修复命令）/ **权限**（`E_PERMISSION` + 宿主侧命令或待审批 id）/ **用户配置**（只在 `status` 里列出）。由同一个函数判定，交互输出、`--json`、interface 共用。

---

# Part V 技术架构

## 15. 三条轴 + 一个输出

| 轴 | 取值 | 对应已有设计 |
|---|---|---|
| 隔离档 | 无 / dev / private / locked + 覆盖 + 规则 | 取代 light / medium |
| 系统根 | host / rootfs | 四档设计里的 full；heavy 不再单独做 |
| 存储 | shared / tmpfs / image | 已有；修正"image = L3" |
| 输出：实际达到的等级 | L1–L6 | 只出现在技术报告里 |

## 16. 编译器与会话模型

```
Policy + HomeView + Caps → SandboxSpec（纯数据）→ Provider（平台抽象层，§17）→ Session（由 Supervisor 托管）
```

- **spec**：挂载、unshare、env 白名单、设备、身份、网络、Landlock 规则、seccomp（隔离用的规则 + 可选的 exec 通知）。单元测试只测 spec；探针 = 同一份 spec + `/bin/true`。
- **`$XLINGS_HOME` 的挂载拓扑**：`--ro-bind $H $H ; --tmpfs $H/subos ; --bind $H/subos/<self> … ; --tmpfs $H/logs ; --tmpfs $H/state`。`config/`（含 policy）保持只读可见。
- **会话的结构**：

```
宿主侧：supervisor（xlings）── broker.sock / exec.sock ── 审计写入 / 网络桥接 / seccomp 监听 fd
           │ fork + 托管（不再 execvp）
沙箱内：session-init（xlings 的一个内部模式，作为沙箱里的第一个进程）
           ├── 用户的交互 shell（use）
           └── exec 请求派生出的进程（exec / 再次 use）
```

- **加入会话**：后来的 `use` / `exec` 连接 `exec.sock`，**通过 `SCM_RIGHTS` 把自己的 stdin / stdout / stderr（或终端）fd 直接交给 session-init**，由它在沙箱里派生进程。进程直接拿到真实的终端，**不经过任何中继**（零拷贝，作业控制和窗口大小都正常）。退出码通过 socket 返回。
- **信号**：supervisor 在等待期间忽略 SIGINT / SIGQUIT（与 `system()` 的做法相同，Ctrl-C 直接送给前台进程组），把 SIGTERM / SIGHUP 转发进会话，退出码按 §12.2 返回。
- **终端注入**：非交互（`exec`）时加 `--new-session`；交互时 `--new-session` 会破坏作业控制，所以改用 seccomp 按参数过滤 `ioctl(TIOCSTI / TIOCLINUX)`。
- **keeper** 被会话模型取代：`--keep` / `--ttl` / `start --ttl` 的含义是"会话空闲多久后结束"。
- **hook 沙箱**：broker 代为安装时，recipe 的 hook 在同一个编译器生成的受限 spec 里运行。
- **没有沙箱的实例**（shell 模式）：没有 supervisor；`exec` 直接执行；`ops` 事件由 xlings 自己写入。

## 17. 平台抽象层

`modules/subos` 里定义与平台无关的接口；每个平台提供实现，或者明确返回"暂未实现"：

| 接口 | 职责 | Linux（现在） | macOS / Windows（现在 → 路线见附录 A） |
|---|---|---|---|
| `FsGate` | 宿主文件不可见、写保护、映射 | bwrap mount ns / Landlock / proot | home-redirect + 链接 → Seatbelt / AppContainer |
| `ProcessScope` | 会话内的进程分组、清理、资源限制 | pid ns；以后加 cgroup | 进程组 → Job Object |
| `NetGate` | host / nat / none / proxy | netns + pasta + 内置转发器 | 未实现 → Seatbelt / AppContainer 断网；proxy 走系统级过滤 |
| `DeviceGate` | 显示、音频、摄像头、GPU | /dev 白名单 + socket bind | 未实现 → Seatbelt 设备操作 / AppContainer capability |
| `IdentityShim` | 时区、语言、主机名、用户 | uts ns + passwd 模板 + env | env 级 |
| `ExecTracer` | exec 审计 | seccomp 用户态通知 | 未实现 → Windows Job Object 新进程通知；macOS Endpoint Security |
| `SessionHost` | supervisor 的托管、socket、fd 传递 | fork + unix socket + SCM_RIGHTS | Windows：CreateProcess + Job Object + AF_UNIX / 命名管道 + `DuplicateHandle` |
| `RootfsRuntime` | 独立用户态 | bwrap userns | 未实现 → Lima 类 VM / WSL2 `--import` |

- 每个接口都有 `probe()`，返回 `{supported, enforced: kernel|advisory, reason}`。§14 的矩阵**就是这些 probe 的结果**，不是手写的表。
- 用户面（命令、配置、提示格式）只依赖接口，不依赖实现。

## 18. 能力探测

运行时实测，不预设；缓存键是 `kernel release + boot_id + provider 的 hash + xlings 版本`，存在 `state/isolation-caps.json`；在满足必须项的前提下，选能满足最多尽量项的组合；探针失败时输出原始 stderr、AppArmor label、相关 sysctl 的值、按最小权限排序的修复办法。

## 19. 网络与身份

| 模式 | 实现 | 本地边界 | 出口 | 默认用于 |
|---|---|---|---|---|
| `host` | 现状（kernel ≥ 6.12 叠加 Landlock 抽象 socket 作用域） | ✗ | 宿主 | dev |
| `nat` | netns + pasta（支持 `--publish`；`--allow host-loopback` 可以访问宿主本机的服务） | ✓ | 宿主 IP | private |
| `none` | netns，只有 lo | ✓ | 无 | locked |
| `proxy` | netns + "无路由 + 一扇门"：内置转发器 → supervisor 里的桥接 → 声明的代理；DNS 只走代理 | ✓ | 代理 | 设置了代理的 private |

身份：private / locked 默认中性化（`user`、`/home/user`、hostname = 实例名、UTC、C.UTF-8、每个实例一个 machine-id）。路径中出现登录名，由部署形态决定（C 形态可以避免）。

## 20. 特权组件与 rootfs（已确认）

- root 拥有的 `/usr/lib/xlings/bwrap` + `/etc/apparmor.d/xlings-bwrap`（**profile 不能指向用户可写的路径**）；由系统包提供，或者 `self doctor --isolation --fix` 安装（一次 sudo）。查找顺序：root 拥有的路径 → 系统 bwrap（探测通过就用，用了就报告）→ xim payload。不用 sysctl，不自动使用 busybox，不再创建 setuid；private / locked 加 `--disable-userns`。
- rootfs：userns + uid 0 映射（setuid 做不到）；多 uid 映射用 `newuidmap` + `/etc/subuid`；xlings 挂在 `/usr/local/bin/xlings`；默认 `fetch=layer`；rootfs 来源是 subos 类型的 xpkg。与隔离用的是同一个前提，一次解决。

## 21. 性能与能力不缺失

### 21.1 性能预算（作为 CI 基准测试的断言）

| 路径 | 预算 | 做法 |
|---|---|---|
| **shim 分发**（gcc、clang 这类，每次构建可能上千次） | **与现在相同**（2026.10.4.1 已经降到个位数毫秒） | 沙箱里的 shim 分发**不经过 supervisor 和 broker**，不读策略文件；只有需要提权的 xlings 管理命令才走 socket |
| `subos exec`，热会话 | 目标 ≤ 5 ms 的额外开销 | socket + fd 传递，不启动 bwrap |
| `subos exec`，冷启动 / `use` | 目标比现在多 ≤ 10 ms | caps 缓存命中时不跑探针；supervisor 只是 fork + wait |
| 交互 I/O | 零开销 | 终端 fd 直接交给会话，没有中继 |
| `observe=full` 的 exec 审计 | 每次 execve 一次往返（微秒级） | 只有 locked 默认开启；过滤器只拦 execve / execveat，其他系统调用只多一次 BPF 判断 |
| `net=nat` | 接近原生 | pasta 对 TCP 使用 splice |
| `fs` 变更清单 | 与映射目录的大小成正比 | 按 mtime 扫描；超过阈值时只记录摘要（可以配置） |
| supervisor 常驻 | 空闲时 0 CPU，几 MB 内存 | — |

基准测试放在 `tests/perf/subos_*`，与 #639 的 shim 分发基准放在一起。超出预算就算回归。

### 21.2 能力不缺失：每一项默认受限的能力，都有一个具名的授权

| 默认受限的 | 授权方式 | 实现 |
|---|---|---|
| 显示（X11 / Wayland） | `--allow display` | bind socket 文件 + xauth cookie（不暴露抽象 socket） |
| 音频 | `--allow audio` | PipeWire / Pulse socket |
| 摄像头 | `--allow camera` | `/dev/video*` |
| GPU | `--allow gpu`（取代 `--gpu`，旧参数保留为别名） | 已有的 passthrough |
| SSH agent | `--allow ssh-agent` | bind `SSH_AUTH_SOCK` |
| D-Bus | `--allow dbus`（以后用过滤代理） | — |
| 宿主本机服务（数据库等） | `--allow host-loopback` | pasta 的映射选项 |
| git 配置、凭据 | `--mount ~/.gitconfig:ro`；`env_pass` | — |
| 环境变量 | `env_pass` 白名单 | — |
| 服务端口 | `--publish` | pasta |

只要策略的 `grants_allowed` 允许，任何一项都可以单次授权，或者写进策略。**不存在"隔离之后就做不到"的死路**，只有"需要显式授权"。

---

# Part VI 可观测性（已确认）

## 22. 可观测性

- **原则**：只有一种事件模型（与 interface 同源）；审计由 supervisor 写入 `logs/subos/<name>/`，沙箱不可见；只在本地；默认脱敏；审计写不进去时 locked 结束会话，其他档位警告。
- **事件类别**：

| 类别 | 内容 | 平台 |
|---|---|---|
| `ops` | 每次 xlings 命令 | 全部 |
| `lifecycle` | 实例、会话、策略的变化（附带隔离快照） | 全部 |
| `perm` | 权限判定、请求、审批 | 全部 |
| `exec` | 沙箱里每次执行程序 | Linux（seccomp 通知） |
| `net` | 每个出站连接 | Linux（nat / proxy） |
| `fs` | rw 映射目录里改动过的文件 | 全部 |
| `destructive` | 现有的破坏性操作记录 | 全部 |
| `trace` | 开发调试 | 全部 |

- **观测级别**：`off`（只保留 destructive）/ `basic` / `standard` / `full`，默认值跟随档位，可以在策略里配置。
- **用户面命令**：`subos ps`、`subos log [-f] [--kind] [--session] [--json]`、`subos sessions`、`subos report`、`subos requests / approve / deny`、`interface subos_events`、`XLINGS_TRACE=home,caps,spec,provider,broker,session`、`subos status --verbose`。
- **存储**：`events.ndjson` + `sessions/<id>.ndjson`；按大小和天数轮转；实例删除时**默认保留**审计（`--purge-logs` 才删除）。
- **exec 审计的实现链路**：bwrap 的 `--seccomp` 拿不到监听 fd，所以由 supervisor fork 出的子进程用 `SECCOMP_FILTER_FLAG_NEW_LISTENER` 安装过滤器，通过 `SCM_RIGHTS` 把 fd 传回 supervisor，再 exec provider。过滤器跨 exec 继承，覆盖沙箱里的所有进程。

---

# Part VII 代码架构（已确认，本轮补充）

## 23. 模块化

### 23.1 归属

| 东西 | 归属 |
|---|---|
| home、部署形态、层、HomeContext | **xlings 核心**（`src/core/home/`） |
| 实例、策略、编译器、provider、平台抽象层、会话 / supervisor / broker 服务端、userdata、status | **SubOS 核心**（`modules/subos`） |
| `UserConfirmed` 令牌 + `Asker` 端口（`ask()`）、路径守卫 | 公共：`modules/guard` |
| 事件 schema、journal（含 destructive）、sink、脱敏、trace 类别 | 公共：`modules/observe` |
| 各自的安全策略（删除 home、GC / `delete_subos`、用户数据分类） | 各自的核心模块 |
| 交互面（CLI human / agent、`--json`、interface、文案、i18n） | `src/`（cli、interface、ui、agent），以及 `src/core/subos/` 适配层 |

### 23.2 目录结构

```
modules/guard/      xlings.guard     UserConfirmed、Asker 端口、路径守卫
modules/observe/    xlings.observe   事件、journal、sink、脱敏、trace
modules/subos/      xlings.subos
  model / view / layout / manifest / policy（含 decide、规则、策略包解析）/ caps / spec
  platform/  fs_gate · process_scope · net_gate · device_gate · identity · exec_tracer · session_host · rootfs   ← §17 的接口
  provider/  linux_bwrap · linux_landlock · linux_proot · home_redirect   （以后 macos_seatbelt · win_appcontainer）
  session/   supervisor · session_init · broker（服务端）· exec 服务
  net/       forwarder · bridge
  userdata / status / ports / gpu / graphics
src/core/home/      HomeContext、.xlings-home、层（xlings 核心）
src/core/subos/     适配层：CLI 参数解析、Ports 的实现（xim / xvm）、broker 请求的执行、渲染
modules/testkit/    xlings.testkit   e2e 测试库（§24.3），只作为 dev-dependency
apps/xdev/          开发工具：test / ci plan / report / lint / release / doctor（§24.3），不随产品发布
```

### 23.3 依赖规则

```
platform, json, cancellation, sha256, i18n  ←  guard, observe  ←  modules/subos  ←  src/core（含 home）←  src/core/subos 适配层  ←  cli / interface / ui / agent
guard, observe, platform, json  ←  testkit  ←  tests/**、apps/xdev      （测试侧：只依赖公共模块，通过产物二进制驱动 xlings）
```

`modules/subos` 是独立包，import `xlings.core.*` 编译就会失败。反向依赖通过 `Ports` 注入（`ensure_backend`、`is_foreign_shim`、`execute(BrokerRequest)`）。模块只返回结构化结果，不产生文案。将来可以作为 `xlings-subos` mcpp 包单独发布（有自己的 semver），可选再做一个 `apps/xsubos`。

---

# Part VIII 质量与交付

## 24. 测试、CI 与开发工具架构

### 24.1 现状（实测：最近成功运行的中位数，样本不超过 9 次）

| workflow | 中位 | 最长 | 主要耗时 |
|---|---|---|---|
| xlings-ci-linux | 28.5 min | 52.3 min | asan 单元测试 26.6 min；单元测试 9.3 min（大部分是编译）；release 构建 7.0 min |
| xlings-ci-linux-e2e | 16.9 min | 39.5 min | release 构建 6.2 min；E2E-00 从源码**再构建一次** 4.2 min；**e2e 套件本身只有 3.6 min** |
| xlings-ci-linux-root | 17.6 min | 40.2 min | 同样包含一次独立构建 |
| xlings-ci-windows | 16.3 min | 43.0 min | 单元测试 9.3 min；release 构建 5.3 min |
| xlings-ci-macos | 10.3 min | 36.0 min | — |
| xlings-ci-aarch64 | 9.6 min | 33.8 min | — |

结论与问题：

1. **时间几乎都花在构建上**，而且同一个 commit 在多个 workflow 里**重复构建**。测试本身并不慢。
2. **测试用了四种语言**：C++ gtest（`tests/unit`，57 个文件）、bash（`tests/e2e`，161 个脚本）、PowerShell（Windows 的 e2e，和 bash 版本部分重复）、Python（`tests/scripts` 12 个，加上 `tools/` 里的若干脚本）。
3. e2e 由 `run_all.sh` **串行**执行；拿不到 backend 时 **skip**（F13）；依赖网络镜像（AGENTS.md 里专门写了 `XLINGS_TEST_MIRROR=CN` 的坑）。
4. 没有统一的报告：哪些需求被覆盖了、哪些测试不稳定、耗时趋势，都无法回答。`tests/README.md` 已经过时（还写着"51 个 gtest"）。

### 24.2 目标

| 目标 | 指标 |
|---|---|
| PR 必过车道 | 墙钟时间 **≤ 15 min**（现在 Linux 中位 28.5 min） |
| 主干合并车道 | ≤ 30 min，全量 L0–L5 |
| nightly | ≤ 90 min，全部内容（asan、覆盖率、性能趋势、完整环境矩阵） |
| 覆盖 | 每一个必须项、每一个退出码、每一条 agent 契约、每一个 F 发现（F1–F16 作为回归测试）、每一个平台 probe，**都有至少一个测试声明覆盖它**；没有覆盖的 CI 直接失败 |
| 统一 | 一种语言（C++）、一个工具链（mcpp）、一个入口（`xdev`）；本地和 CI 跑的是同样的命令 |

### 24.3 工具：`modules/testkit` + `apps/xdev`（C++ / mcpp）

**为什么用 C++ + mcpp**：
- 与项目使用同一种语言、同一个工具链；
- 一份 e2e 代码就能跑三个平台，取代 bash 和 PowerShell 的重复实现；
- 可以直接复用 xlings 自己的模块（`observe` 的事件 schema、`platform`、`json`）；
- mcpp 已经提供了测试发现、按模式过滤、`--message-format json`（每个测试一条 NDJSON）、超时控制、按 workspace 成员运行，这些能力可以直接使用。

**`modules/testkit`（`xlings.testkit`，e2e 测试库，构建在已有的 gtest dev-dependency 之上）**

```cpp
import xlings.testkit;
using namespace xlings::testkit;

XTEST(subos_isolation, home_is_read_only,
      meta{ .area = "subos.isolation", .cost = Cost::Fast,
            .covers = {"ISO-HOME-RO", "F1"}, .requires = {cap::userns} }) {
    auto home = Home::isolated({ .index = Fixture::minimal });   // 临时 XLINGS_HOME + 进程内的 fixture 镜像
    home.xlings({"subos", "new", "t", "--sandbox=dev"}).expect_ok();
    auto r = home.xlings({"subos", "exec", "t", "--", "sh", "-c",
                          "test -w \"$XLINGS_HOME/config/shell/xlings-profile.sh\""});
    r.expect_exit(1);
    home.audit("t").expect_event(kind("lifecycle").has("effective", "xlings_home_ro"));
}
```

| 能力 | 说明 |
|---|---|
| `Home::isolated()` | 临时 home，测试结束后清理；默认使用**进程内的 fixture HTTP 镜像**（从 `tests/fixtures` 提供索引和 payload），**不访问网络**，与镜像所在地区无关 |
| 运行 xlings | 支持 argv、env、stdin、可选 pty、超时；结果对象提供 exit / stdout / stderr / json / ndjson 的解析 |
| 断言 | `expect_exit`、`expect_json`、`expect_event`（审计事件）、`expect_no_prompt`（agent 契约）、`expect_missing(dimension, reason)` |
| 元数据 | `area`、`cost`（Fast < 2s / Medium < 30s / Slow）、`covers`（需求 ID）、`requires`（能力）、`resources`（需要独占的资源，例如端口、真实沙箱），`network` 标记 |
| 能力要求 | 车道**声明**自己具备哪些能力（`XDEV_LANE_CAPS=userns,landlock`）。车道声明了但实际缺失 → **失败**；开发机上缺失 → skip 并写明原因（修复 F13） |
| 失败现场 | 失败时把临时 home 里的 hook 日志、审计、`destructive.ndjson` **脱敏后**保存为 artifact |
| 事件 | 每个测试的耗时、结果、skip 原因、重试次数都按 `modules/observe` 的 schema 输出 |

**`apps/xdev`（开发工具，不随产品发布）**

| 子命令 | 作用 |
|---|---|
| `xdev test [unit\|contract\|e2e\|perf] [--area] [--tag] [--changed <base>] [--shard i/n] [--lane pr\|main\|nightly] [-j N]` | 统一入口：底层调用 `mcpp test --message-format json`，加上选择、分片、并行和资源锁 |
| `xdev ci plan [--changed <base>..<head>]` | 输出 GitHub Actions 的动态矩阵 JSON（车道、分片），**测试选择逻辑只写在 C++ 里，不写在 YAML 里** |
| `xdev report [--format md\|json] [--coverage-map]` | 汇总 NDJSON：结果、最慢的测试、不稳定的测试、**未被覆盖的需求 ID**、平台能力矩阵（来自 `subos doctor --json`，同时校验 §14 的矩阵）、性能预算、覆盖率 |
| `xdev lint` | 结构性检查：`lint_subos_remove_all`、禁止固定 xlings 版本、Windows 头文件卫生、文档示例、CLI 与文档一致、i18n 覆盖（**移植现有的 Python 脚本**） |
| `xdev release build\|verify\|finish` | 统一 `linux_release.sh` / `macos_release.sh` / `windows_release.ps1` / `verify-release.sh`。`finish` 把 AGENTS.md 里"release.yml 变绿之后还要做的两步"（补 CN 镜像、用 **GET 而不是 HEAD** 验证、更新 xim-pkgindex）做成带检查的命令。先包装现有脚本，再逐步移植 |
| `xdev doctor` | 检查开发环境：工具链、镜像（是否需要 `XLINGS_TEST_MIRROR=CN`）、bwrap profile、本机的隔离能力 |

**迁移不需要一次重写**：现有的 sh / ps1 / py 测试通过 `xdev` 的**兼容适配器**照常运行（xdev 知道它们的清单，负责计时、捕获结果、写入同一份 NDJSON），**从第一天起报告就是统一的**。之后按优先级逐步移植成 C++：先移植 sh + ps1 成对重复的，再移植不稳定的、慢的；**新测试只用 C++ 写**。

### 24.4 测试分层与覆盖矩阵

| 层 | 内容 | 单个耗时 | 工具 |
|---|---|---|---|
| L0 静态 | lint、结构检查、schema 校验 | 秒 | `xdev lint` |
| L1 单元 | `policy.decide()` 和规则匹配；spec 编译器（**golden 快照**：档位 × 覆盖 × caps 组合，表驱动）；manifest 往返（保留不认识的键）；HomeContext 解析矩阵；退出码映射；脱敏；受众渲染器 | 毫秒 | gtest |
| L2 契约 | CLI spec ↔ 文档；interface schema；退出码表；**agent 契约扫描**：从 `CommandSpec` 遍历每一条命令，在 `XLINGS_AGENT_MODE=1`、挂 pty、不给输入的条件下运行，断言不会阻塞等待输入；事件 schema 兼容 | 秒 | testkit |
| L3 组件 e2e（fake provider） | 用 `fake` provider（记录 spec，直接执行命令，不做隔离）跑 CLI、策略、broker、审批、exec、会话的流程；**三个平台都能跑，速度快** | 秒 | testkit |
| L4 真实隔离 e2e | 真实的 bwrap / Landlock：每个档位的每个必须项；**F1–F16 的安全回归探针**；`subos doctor` 本身 | 十秒级 | testkit，隔离车道 |
| L5 部署与升级 | U / C / P、relocate；**N-1 版本兼容**：上一个 release 的客户端操作新布局，新客户端操作老布局，都不能损坏数据；release 产物测试 | 十秒到分钟 | testkit + 产物 |
| L6 环境矩阵 | 24.04（装 profile 前后）、22.04、`container:` job、aarch64、archlinux、macOS、Windows、fresh-install | 分钟 | CI |
| L7 性能 | §21.1 的预算；shim 分发（#639） | — | `xdev test perf` |
| L8 故障注入 | 磁盘满、supervisor 被杀、broker 不可用、caps 缓存过期、AppArmor 拒绝（由 fake 模拟）、审计写入失败 | 秒 | testkit 的故障点 |

- **fake provider 只证明流程，不证明隔离**。覆盖图里区分 `proves: flow` 和 `proves: isolation`，**隔离类的需求 ID 只有 L4 的测试才算覆盖**。
- **故障点只在测试构建里存在**：用 mcpp 的 `testing` feature 编译进去；release 构建的检查会断言故障点不存在。
- **需求可追溯**：本文的必须项、F 发现、退出码、契约规则、平台 probe 都有 ID（例如 `ISO-HOME-RO`、`F2`、`EXIT-125`、`AGENT-NO-PROMPT`）。`xdev report --coverage-map` 列出没有被覆盖的 ID，**有就让 CI 失败**。代码覆盖率（llvm-cov）在 nightly 运行，新模块（`modules/subos` 的 policy / spec）设门槛（例如 90%）。

### 24.5 CI 架构

```
plan ──► build（每个 OS × profile 只构建一次；mcpp 全局缓存）──► 上传产物（dev 二进制、测试二进制、release 包）
                                       │
             ┌─────────────────────────┼──────────────────────────────┐
             ▼                         ▼                              ▼
       L0 + L1 + L2               L3 分片 × N（3 个平台）       L4 隔离车道（Linux：装 profile / 容器 / 22.04）
             └────────────── 汇总：xdev report → step summary + report.json ─────────────┘
```

| 手段 | 说明 | 节省 |
|---|---|---|
| **每个 commit 每个平台只构建一次** | 可复用的 `build.yml` 产出所有产物，测试 job 只下载。现在 release 构建在 linux / e2e / root 三个 workflow 里各做一次；E2E-00"mcpp 从源码构建"本身就是 build job，不再单独重复 | 每个 PR 节省 10+ 分钟的机器时间，以及关键路径上的若干分钟 |
| **mcpp 缓存** | 用 `mcpp --print-fingerprint` + `mcpp.lock` 作为缓存键，缓存 BMI 和目标文件；PR 做增量编译 | 构建从 6–9 min 降到增量的几分钟（需要实测） |
| **动态矩阵与分片** | `xdev ci plan` 根据主干的历史耗时（`ci-timings.json`）装箱分片 | 关键路径由最慢的分片决定 |
| **按影响选择** | `tests/areas.toml` 把路径映射到 area，测试声明自己的 area。PR 车道：L0–L3 **全部**（本来就快）+ 冒烟集 + 受影响 area 的 L4 / L5；主干：L0–L5 全部；nightly：全部 | 避免每个 PR 都跑全量的慢测试 |
| **作业内并行** | 每个测试用自己的临时 home，天然可以并行（`-j`）；用到真实沙箱、端口、挂载的测试通过资源锁串行 | — |
| **不依赖网络** | PR 车道使用 fixture 镜像；带 `network` 标记的测试只在主干和 nightly 跑，失败时附带镜像归因 | 消除镜像导致的不稳定 |
| **asan** | 从每个 PR 移到：主干合并后 + nightly + 打了 `ci:asan` 标签或触及指定 area 的 PR | PR 关键路径减少约 27 min |
| **不稳定测试** | CI 里自动重试一次；重试才通过的记为 flaky；隔离清单（`tests/quarantine.toml`）要写明负责人和到期时间 | — |

预计 PR 关键路径：构建（缓存增量）3–6 min + 测试分片 ≤ 5 min + 汇总 ≤ 1 min，**目标 ≤ 15 min**。这个数字需要在 T3 完成后实测。

### 24.6 报告与可观测性

- 每个测试的事件都使用 `modules/observe` 的 schema（与产品的审计事件是同一套），`xdev report` 合并所有分片和平台的结果。
- **GitHub step summary**：车道结果、最慢的 10 个测试、flaky 列表、**未被覆盖的需求 ID**、平台能力矩阵（实测）、性能预算表、覆盖率变化。
- artifact：`report.json`；失败测试的脱敏现场。
- 趋势：主干和 nightly 把耗时、flaky、覆盖率追加到一个数据分支（或长期保留的 artifact），`xdev report --trend` 可以查看。
- 本地执行 `xdev test` / `xdev report` 得到的输出与 CI 相同。

### 24.7 测试基础设施自己的 checkpoint（放在 C0 之后、C1 之前，后面所有功能都用它写测试）

| # | checkpoint | 验证 |
|---|---|---|
| T1 | `modules/testkit` 骨架：`XTEST` + 元数据、`Home::isolated`、进程内的 fixture 镜像、断言、失败现场。用一个 sh + ps1 成对重复的用例证明它能跨平台 | testkit 自己的单元测试 + 3 个平台 |
| T2 | `apps/xdev test / report`：调用 mcpp 测试的 JSON 输出 + 现有 sh / ps1 / py 测试的兼容适配器 → **统一报告** | 本地和 CI 输出一致 |
| T3 | CI 重构：`plan` + 只构建一次的 `build.yml` + 分片 + mcpp 缓存 + asan 调整 + step summary | 实测 PR 关键路径的耗时 |
| T4 | 契约扫描：agent 不提问扫描、退出码表、CLI spec 一致性（把 Python 脚本移植过来） | L2 |
| T5 | 需求 ID 覆盖图 + "有未覆盖的 ID 就失败"；车道能力声明（修复 F13） | `xdev report --coverage-map` |
| T6 | `xdev lint` 移植 Python 检查；`xdev release` 包装现有脚本（可以放在本 PR 之后） | L0 |

## 25. 稳定性规范

| 规范 | 内容 |
|---|---|
| 一个问题只有一个回答者 | 部署：`.xlings-home`；运行：`HomeContext`；能不能做：`policy.decide()`（客户端和 broker 共用）；实际拿到什么：`SandboxSpec + Caps`；平台能做什么：各接口的 `probe()`；测试选择：`xdev ci plan` |
| 声明而不是推断 | 形态、策略、受众、车道能力都是声明；推断只作为老数据的迁移路径 |
| 核心与交互面分离 | 核心里不出现受众分支，不拼文案；提问只通过 `Asker` |
| schema | 写入时保留不认识的键；**执行时遇到不认识的安全相关字段就拒绝进入** |
| 布局版本 | `.xlings-home.layout` 单向迁移；从引入这条规则的版本开始，遇到更高的 layout 只读不写（layout 2 只增加内容，对现有客户端无害） |
| 物理分离 | 策略与被约束的对象分离（`config/subos/`）；审计与被审计的对象分离（`logs/subos/`） |
| 写者唯一 | §6 的表就是清单 |
| 用户数据 | 沿用 AGENTS.md；`rootfs/`、映射目录、审计日志都按用户数据处理 |
| 协议 | 事件只增加字段；broker 复用 interface；退出码表写进文档并加测试固定 |
| 性能 | §21.1 的预算是 CI 断言 |
| 可验证 | 每个需求 ID 至少有一个测试；隔离类的 ID 只有真实 provider 的测试才算覆盖 |
| 无感升级 | 没有声明的实例行为不变；单次 `--sandbox` 变为 dev 档（`fetch=auto`）；`--gpu`、`--cmd` 保留为别名；N-1 兼容测试守住升级和降级 |

## 26. 交付：单 PR，多个 commit checkpoint（已确认）

每个 commit 都是一个 checkpoint：能编译，已有测试全部通过，本 commit 的新测试通过，可以单独 revert。顺序：**C0 → T1–T5 → C1–C27**（T6 可以放在本 PR 之后）。

| # | checkpoint | 验证 |
|---|---|---|
| C0 | 本设计文档；修正现有文档（setuid、image = L3、隔离矩阵）；AGENTS.md 补充"核心与交互面分离"和 `xdev` 的使用 | 文档 |
| T1–T6 | 测试基础设施（§24.7） | 见 §24.7 |
| C1 | `modules/guard`、`modules/observe`；`confirm`、`destructive_log` 迁入（行为不变） | 现有测试 |
| C2 | `modules/subos` 骨架；`gpu`、`graphics`、`manifest` 迁入（行为不变） | 现有测试 |
| C3 | `Ports`、`HomeView`；`userdata`、`sandbox`、`keeper` 迁入（行为不变） | 现有测试 |
| C4 | 拆分 `subos.cpp`：核心与适配层（行为不变） | 现有测试 |
| C5 | `src/core/home/`：HomeContext、带形态的 `.xlings-home`、layout 规则 | L1 + L5 |
| C6 | 写者规范：保留不认识的键；策略文件放在 `config/subos/` | L1 |
| C7 | 写入面盘点（inotify） | L4 基线 |
| C8 | 平台抽象层接口 + `probe()`；bwrap / proot / home-redirect 移植成 provider；**fake provider** | L1 golden |
| C9 | 会话模型：supervisor + session-init + `exec.sock` 和 fd 传递；`lifecycle` / `ops` 事件；`subos ps / log` | L3 + L4 + L7 |
| C10 | S0 修复①：挂载拓扑 | L4（F1、F6） |
| C11 | S0 修复②：env 清理、pid / ipc / uts、`--die-with-parent`、`--new-session` + TIOCSTI seccomp；探针文案 | L4（F3、F4、F7、F8、F12） |
| C12 | `subos exec` / `start` / `stop` / `--temp` / `cp`；interface 的 `subos_exec` 等 | L2（退出码）+ L3 |
| C13 | 受众契约：`XLINGS_AGENT_MODE`、`Asker` 端口、agent 下的错误码 / 候选列表 / 非阻塞 | L2（agent 契约扫描） |
| C14 | 策略模型：预设、覆盖、合并、`policy.decide()`、`--sandbox=<档位>`、`subos config / status`、拒绝不认识的安全字段 | L1 + L3 |
| C15 | 权限：客户端 + broker；auto / ask / deny；`install --subos`；`requests / approve`；hook 沙箱；`perm` 事件 | L3 + L4 |
| C16 | 细粒度规则；策略包 | L1 + L3 |
| C17 | 能力探测 + 缓存 + 必须项 / 尽量项 + 提示规范 + `--no-degrade` | L3（fake 能力）+ L6 |
| C18 | Landlock provider | L4（容器车道） |
| C19 | `--mount`；`--allow` 授权族 | L3 + L4 |
| C20 | 身份中性化、`net=none`、`--disable-userns` | L4（F5、F7） |
| C21 | `self doctor --isolation --fix`；bwrap recipe 在 xim-pkgindex 另提 PR | L6（F10） |
| C22 | 观测完整版：exec、fs、脱敏、`report`、`subos_events`、`XLINGS_TRACE` | L3 + L4 + L8 |
| C23 | 网络：`nat`（pasta、`--publish`）、`proxy`；`net` 事件 | L4（F2、F5） |
| C24 | `fetch=layer` | L3 + L4 |
| C25 | `subos doctor`；完整环境矩阵；性能预算作为断言 | L6 + L7 |
| C26 | 部署形态 S；系统层（M）的读取路径 | L5 |
| C27 | rootfs（是否放进本 PR 由维护者决定） | L4 |

## 27. 决策

### 已确认（全部）
权限模型；Landlock；`--mount`；渐进式披露；平台矩阵和提示规范；root 拥有的 bwrap + 窄 profile + rootfs；五种部署形态；层叠 home；目录规范；supervisor；可观测性；模块归属；单 PR 多 checkpoint；控制与配置的四个层次；策略文件放在实例目录之外；进入与外部执行（会话模型、退出码）；人与 agent 两套语义（`XLINGS_AGENT_MODE`），核心与交互面分离；性能预算与具名授权；平台抽象层和附录 A；C0–C27。

### 本轮需要 review
1. **testkit + xdev**：用 C++ / mcpp 统一单元测试、e2e、lint、发布、报告；现有的 sh / ps1 / py 测试走兼容适配器，不需要一次重写；新测试只用 C++（§24.3）。
2. **L0–L8 分层**；fake provider 只证明流程；**需求 ID 覆盖图，有未覆盖的就让 CI 失败**；车道能力声明取代 skip（§24.4）。
3. **CI 架构**：每个平台只构建一次 + mcpp 缓存 + 动态矩阵 / 分片 + 按影响选择 + PR 不依赖网络 + asan 移出 PR 关键路径；PR 目标 ≤ 15 min（§24.5）。
4. **报告**：统一事件 schema、step summary、趋势（§24.6）。
5. T1–T6 放在 C0 之后（§24.7、§26）。

---

## 附录 A：其他平台的技术路线参考（供以后实现时参考，需要逐项验证）

| 接口 | macOS 候选 | Windows 候选 | 备注 |
|---|---|---|---|
| FsGate | **Seatbelt**（`sandbox-exec` / `sandbox_init` 的 SBPL profile，可以 `(deny file-read* (subpath …))`）。API 已标记为废弃，但 Chromium、Bazel 的 sandboxed 策略、Nix 仍在使用；不需要特权 | **AppContainer**（低完整性 + 能力 SID；允许访问的目录需要给 AppContainer SID 加 ACL，这些目录归用户所有，所以可以做到）；**Win32 app isolation**（Windows 11 的新机制，处于预览阶段） | macOS 上不能重映射路径，映射继续用链接 |
| ProcessScope | 进程组 + Seatbelt；没有 pid namespace | **Job Object**（kill-on-close、资源限制、进程计数），不需要特权 | — |
| NetGate | Seatbelt `(deny network*)` 可以做到 `none`；`proxy` 需要 Network Extension（需要签名 entitlement）或者 pf（需要 root） | AppContainer 不授予 `internetClient` 即可做到 `none`；`proxy` 需要 WFP（需要管理员）或 loopback 豁免 + 代理 | `nat` 在这两个平台上没有直接的对应 |
| DeviceGate | Seatbelt 有 `device-camera`、`device-microphone` 操作；另外还受系统 TCC 管控 | AppContainer capability（`webcam`、`microphone`），与 `--allow` 的模型天然对应 | — |
| ExecTracer | Endpoint Security（需要系统扩展和 entitlement）；`eslogger`（macOS 13+，需要 root 和完全磁盘访问权限）；不需要特权的退路：轮询 Seatbelt 会话内的进程表 | **Job Object 的 `JOB_OBJECT_MSG_NEW_PROCESS`**（通过 IO 完成端口，**不需要特权**），再查询映像路径和命令行；ETW 进程事件需要管理员 | Windows 这一项比较容易实现 |
| SessionHost | 与 Linux 相同（unix socket + SCM_RIGHTS） | CreateProcess + Job Object；AF_UNIX（Windows 10+）或命名管道；用 `DuplicateHandle` 传递控制台 / 管道句柄 | — |
| RootfsRuntime | 轻量 VM：Virtualization.framework（Lima 类）；Apple 公开的 Linux 容器框架（需要评估版本要求） | **WSL2**：`wsl --import <name> <dir> <rootfs.tar>` 原生支持导入 rootfs，与 subos 类型 xpkg 的分发方式契合 | 两个平台的 rootfs 都意味着"Linux 用户态跑在 VM 里"；隔离和观测在 VM 内部复用 Linux 的实现 |
| 特权组件 | Seatbelt profile 随系统包安装；不需要 root 授权 | AppContainer profile 的注册不需要管理员 | — |

## 附录 B：自我 review（本轮发现的问题与处理）

| # | 问题 | 处理 |
|---|---|---|
| R1 | 上一轮把 `E_PERMISSION=13`、"必须项不满足 = 2"和 `--cmd` 的命令退出码放在同一个空间里，`exec` 无法区分"命令失败"和"xlings 失败" | 采用 docker 的约定：exec 在命令启动之前失败返回 125，`--json` 里有 `phase` 字段；非 exec 命令保持原有的退出码表（§12.2） |
| R2 | 实例策略放在实例的 `.xlings.json` 里：要么整个文件只读（版本切换要经过 broker，热路径变慢），要么沙箱能修改自己的策略 | 策略移到 `config/subos/<name>/policy.json`，从结构上分离；实例的 `.xlings.json` 保持可写（§6.1）。"策略被老客户端丢掉"的问题也随之消失（老客户端根本不会碰这个文件） |
| R3 | 上一轮说"老客户端遇到更高的 layout 只读不写"，但**现有的老客户端并不认识这条规则** | 写明这条规则从引入它的版本开始生效；layout 2 只增加内容，现有客户端照常读写不会损坏它（§25） |
| R4 | 老客户端进入一个声明了策略的实例时，会忽略策略（没有隔离） | 无法阻止老客户端。在文档里说明；`self doctor` 检测到 entry 版本低于策略要求的最低版本时报 Error；策略文件里写 `min_client` 字段 |
| R5 | `XLINGS_SUBOS_POLICY` 如果放完整的 JSON，env 会很大，而且 env 清理时要特别放行 | 只放策略文件的路径和指纹；内容从只读的 `policy.json` 读取 |
| R6 | supervisor 从 `execvp` 改成 fork + wait 以后，Ctrl-C 会同时发给 supervisor | supervisor 在等待期间忽略 SIGINT / SIGQUIT，转发 SIGTERM / SIGHUP（§16） |
| R7 | 交互会话加 `--new-session` 会破坏作业控制，但不加又有 TIOCSTI 风险 | 交互时改用 seccomp 按参数过滤 `ioctl`；非交互时用 `--new-session`（§16） |
| R8 | 多次 `use` / `exec` 加入同一个会话时，如果 tty 要经过中继，会有性能损失和兼容问题 | 用 `SCM_RIGHTS` 直接传递 fd，由 session-init 派生进程，没有中继（§16） |
| R9 | 单次调用的覆盖参数和运行中会话的 spec 不一致 | 能改变 spec 的单次调用不加入现有会话，而是另开临时会话并说明原因（§12.1） |
| R10 | `fetch=auto` 在 macOS / Windows 上没有 hook 沙箱 | 在这两个平台上，auto 的信任等级等同于 owner 自己执行 install（dev 档在这两个平台上本来就是 advisory），并在 `status` 里标明 |
| R11 | 第三方策略包可能悄悄放宽隔离 | 只有 owner 能选择策略包；选择时显示与内置预设相比放宽的项；锁定版本和 sha256；S / M 下由系统配置限制来源（§7.3） |
| R12 | 观测里 argv 可能包含密钥 | 默认脱敏；`--observe-raw` 关闭脱敏时在事件里标明；审计只在本地 |
| R13 | `--gpu`、`--cmd`、`--keep`、`--ttl` 这些现有参数的去留 | `--gpu` → `--allow gpu` 的别名；`--cmd` → `exec -- sh -c` 的等价写法；`--keep` / `--ttl` → 会话的空闲时长。全部保留，不破坏现有脚本 |
| R14 | §14 的平台矩阵如果是手写的，会和实现脱节 | 矩阵由平台抽象层各接口的 `probe()` 生成，文档里的表是某个版本的快照（§17） |
| R15 | 本 PR 的 checkpoint 数量多，评审负担大 | 前 9 个是行为不变的重构和基础设施，可以按 commit 逐个评审；每个 commit 都可以单独 revert；评审时可以按 Part 分组 |
| R16 | 仍然待验证的事实 | 沙箱激活时的写入面（C7）；setuid 模式下 `/proc/<pid>/root`；Ubuntu 系统 bwrap 是否带 profile；GH runner 的 userns；容器里 Landlock 是否放行；nl80211 BSSID；附录 A 的每一项。**其中环境相关的几项由 L6 车道直接回答，结果进入 `xdev report` 的能力矩阵** |
| R17 | 新增一个 e2e 框架会不会再引入一套依赖 | testkit 构建在已有的 gtest dev-dependency（`[dev-dependencies.compat] gtest`）之上，三个平台都已经在用，不引入新依赖 |
| R18 | fake provider 测得快，但可能给人"隔离已验证"的错觉 | 覆盖图区分 `proves: flow` 和 `proves: isolation`，隔离类的需求 ID 只认 L4 的真实测试（§24.4） |
| R19 | 按影响选择测试可能漏选 | L0–L3 每个 PR 都全量跑；主干全量跑 L0–L5；主干上出现了 PR 车道没选中的失败时，`xdev report` 标记为"area 映射缺口"，需要修 `tests/areas.toml` |
| R20 | asan 移出 PR 关键路径，内存错误可能先合进主干 | 主干合并后立即跑 asan + nightly；`ci:asan` 标签和指定 area 强制在 PR 里跑；主干上 asan 失败按回归处理（立即 revert 或修复） |
| R21 | 重写 161 个 shell 测试成本太高 | 不要求重写：兼容适配器先统一报告，按优先级逐步移植，新测试只用 C++ |
| R22 | 故障注入点混进 release 二进制 | 只在 mcpp 的 `testing` feature 下编译；release 构建检查会断言故障点不存在 |
| R23 | fixture 镜像与真实索引格式脱节 | fixture 用真实的 xpkg 格式（`mcpp xpkg parse` 校验），并经过现有的索引兼容检查；nightly 用真实镜像跑带 `network` 标记的测试 |
| R24 | 作业内并行时，真实沙箱、端口、挂载类的测试互相干扰 | 用 `resources` 元数据加锁串行；其他测试按临时 home 并行 |
| R25 | `xdev` 自身出问题会拖垮整个 CI | xdev 有自己的 L1 测试，并且在 build job 里构建；`mcpp test` 仍然可以直接运行（xdev 只是在它上面增加选择和报告） |
| R26 | 本节的耗时数据来自最近的少量运行（每个 workflow 最多 9 次成功运行的中位数） | 15 min 的目标需要在 T3 之后实测确认；`xdev report --trend` 持续跟踪 |
| R27 | 环境变量命名 | 统一为 `XLINGS_AGENT_MODE`（取值 `1` / `0`），与 `--agent` 等价；`XDEV_*` 只用于开发工具，不进入产品 |

---

## 实施记录（2026-10-06，PR #641）

实现与本文有出入的地方，以实现为准，并在这里说明原因：

| 本文 | 实现 | 原因 |
|---|---|---|
| 审计按 kind 分文件 | 每个实例一个 `logs/subos/<s>/events.ndjson`，事件带 `kind`，`subos log --kind` 过滤 | 一次追加、一个轮转点；读者按 kind 过滤即可 |
| exec 审计的过滤器由 supervisor 安装 | 由 session-init 在沙箱内安装，listener 经 ctl socket 传给 supervisor | 在 bwrap 之前设置 no_new_privs 会触发 AppArmor 对 bwrap 的限制 |
| `net=nat`：pasta 接入 bwrap 的 netns | 子进程先 unshare user+net，pasta 按 pid 接入，再 exec bwrap | 非特权的 pasta 无法加入 bwrap 创建的 netns |
| interface `subos_exec` 流式输出 | 结束后返回输出与退出码 | 流式需要 interface 协议的增量事件，留到下一版 |
| broker 执行安装时 hook 也在沙箱里 | hook 在宿主侧按 owner 权限运行 | 需要一个能写 payload 的 hook 沙箱，单独做 |
| 加入会话的交互 shell 有作业控制 | 加入时提示作业控制留在第一个 shell | 终端只能属于一个会话首进程 |
| Landlock 作为 bwrap 不可用时的回退 | 只在 `--sandbox landlock` 时使用 | 它只限制写入、不隐藏文件，自动替换会悄悄降低隔离 |
| `fetch=layer`、系统层（M）解析包 | 识别系统层并报告；`layer` 在任何位置都拒绝 | 两者是同一个分层 payload 查找机制，单独实现（`PERM-FETCH-LAYER`、`HOME-LAYER-RESOLVE`） |
| `net=proxy`、独立 rootfs（C27） | 拒绝 / 不在本 PR | 见计划 §4 |
| 单次调用 `--sandbox=xim:pack` | 只在 `subos config` 上选择策略包 | 单次调用只能收紧，策略包可能放宽 |
| `self install --user` | `self update --user` | 系统包的 `/usr/bin/xlings` 不是发布包目录，`self install` 无从安装；`self update --user` 走现有的安装路径 |

需求与测试的对应关系在 `tests/requirements.toml`，`xdev report --requirements --fail-uncovered` 在 CI 中校验。

