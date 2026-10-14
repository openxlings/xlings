# Luban OS part 4：基础架构（规范与组件、控制面与可观测、平台抽象）、三平台可用性与可测性

日期：2026-10-15　基线：2026.10.15.1（part 3）　状态：已确认，实施中（第三版）

前置：`2026-10-12-luban-os-architecture-design.md`（总体架构，下称"总体设计"）、
`2026-10-13-luban-os-desktop-part2-design.md`（桌面 part 2）、`2026-10-14-luban-os-part3-design.md`（part 3）、
openkal 的规范（`mcpplibs/openkal`，SPEC 0.15）以及 mcpp 仓库中 openkal 的设计记录。

第二版、第三版的变化：

- 加入 openas 的命名与含义；
- macOS、Windows 合并为一个阶段，与 CI 覆盖一起做；
- vz 改用 C++ 绑定，并给 luban 签名，不再需要单独的辅助程序；
- openhal 以最小面立起来；
- 新增 §11"可测性"与 CI 架构（含 GitHub 托管 runner 的实测依据）；
- 新增 §13 自审（第二轮）；
- 第三版：不支持 Intel Mac；Windows 的 machine 在 Luban 宿主机里用 KVM（§8.2）。

---

## 0. 起因与目标

part 1–3 把 Luban 在 Linux 上做到了"可以长期用"。下一步是同一件事的两面：

1. **基础架构**：规范、可复用组件、Luban 自己的组件各放在哪里，怎样分包；控制面和可观测性从架构上留好位置，
   让 agent（从外部、在环境里、以后作为系统服务）与人（TUI、GUI）用同一套语义。
2. **三平台可用**：Luban 系统在 macOS、Windows 上真正能用，体验与 Linux 一致；差异收敛到最小、能被说出来，
   并且**由 CI 持续证明**。

原则是**架构先行、按需实现**：架构一次定好，代码只落现在用得到的最小面。

## 1. 已确认的方向（讨论记录）

1. 还没有独立仓库的规范与抽象层放在 `luban/` 下，每个一个独立的 `mcpp.toml`，**按独立项目维护**
   （名字不带 luban、不导入 `luban.*` / `xlings.*`、自带 README 与规范文本），以后原样拆出。
2. openkal 已是独立仓库，作为外部依赖；openhal、opensal、openas 初期在仓库内按需最小面落地，不建仓库。
3. 控制面的关键是架构：模型、执行、绑定分开，换传输或换方案时只换一层。
4. Luban OS 从架构上具备可观测性和 agent 控制：agent 语义与人的语义（TUI、GUI）是同一模型的两种呈现。
5. macOS 与 Windows 的可用性一起做，同时把 CI 覆盖做到位；真机测试由维护者之后进行。
6. 不引入非 C++ 组件：Virtualization.framework 经 C++ 绑定使用。

## 2. 原则

1. **视角，不是层次。** openarch、openkal、openhal、opensal、openas 是同一台机器的不同视角（处理器、内核、设备、
   表面、主体），规范之间互不引用，实现之间可以任意连接。契约的形式由"一个程序里同时有几个实现"推出。
2. **Luban 是负责"连接"的那一方，不是规范。** openkal 不定义身份与权限，"谁能读"是启动程序那一方的责任；
   Luban 正是这一方：它决定一个环境里的程序拿到哪些能力。隔离、载体、用户、策略是 Luban 的组件，不进入任何规范。
3. **平台差异只出现在三处**：openkal 的实现（过渡期为 `modules/platform`）、设备与表面的提供端、载体的后端。
4. **能力由探测说出来。** 上层问"这里能不能做 X"，不问"这是不是 macOS"；做不到时给出原因与下一步。
   缺席是一个答案，不是运行时的意外。
5. **实现，而非模拟。** 一个假装做到、实际让上层悄悄得到错误结果的实现不允许存在；做不到就不声明。
6. **一个模型，多个呈现。** 每个操作只有一份定义；CLI、Luban OS 应用、`--json`、NDJSON 接口、以后的 MCP 都是它的渲染或绑定。
7. **机制确定，策略可换。** 写记录、判定作用域、停止是确定性代码；谁被授权、谁来回答是策略，可以是人，也可以是 agent。
8. **agent 的接口就是测试的接口。** 系统能被 agent 不看屏幕、不等人地使用，就能被测试不看屏幕、不等人地验证（§11）。

## 3. 家族与 Luban 的位置

| 规范 | 视角 | 契约形式 | 位置 | Luban 用它做什么 |
|---|---|---|---|---|
| openkal | 内核："我的"流、文件、进程、时间、网络…… | C ABI（头文件 + C++ 模块） | 独立仓库（`mcpplibs/openkal`），linux / macos / windows 三个实现 | `modules/platform` 的可移植部分按接口迁过去（§6.5） |
| openhal | 设备："哪一个"GPU、显示器、音频端点、输入设备 | concepts；闭源驱动走 C ABI 表 + adapter | `luban/specs/openhal`，**本期一个接口：设备的枚举与描述**（§4.3） | 能力报告；sandbox 与 machine 的 GPU 选择；以后的设备透传 |
| opensal | 表面：一个 surface 与它的接口，跨地址空间的线协议 | C++ 模块 + wire | `luban/specs/opensal`（现 `luban/desktop/opensal/spec`） | 桌面画面从 guest 到宿主窗口 |
| **openas** | 主体：谁、做了什么、能做什么（Open Actor Semantics） | Schema + 信封（JSON-RPC 2.0，换行分隔） | `luban/specs/openas`，**本期：信封、主体、效果类别、一致性检查** | 控制面与活动记录，OS 与桌面两个领域共用（§7） |

**openas 的含义**：主体（actor）是人、agent、程序、服务的统称。它们在模型里是同一种东西：有身份、委托链、作用域，
动作写进同一份记录。agent×agent、agent×人、人×系统都是主体之间的关系；双语义（人用图形、agent 用协议）
是同一模型的两种呈现，所以名字不偏向任何一方。

**不是规范的**（Luban 的组件）：SubOS、confine、carrier、users 与 `/etc`、策略、generation、edition、镜像格式、
设备的**授予**（"哪个设备给哪个环境"是策略；"有哪些设备"才是 openhal）。

GPU 能用的内存、推理资源的调度属于平台能力的视角，不归 openhal；本期不涉及。

## 4. `luban/` 的目录树与包划分

### 4.1 三类东西，三个目录

```
luban/
├── README.md              目录约定（本节），每类的规则
├── specs/                 规范：独立项目。只依赖 std；规范之间互不引用
│   ├── opensal/           表面 + wire                       （由 desktop/opensal/spec 移来）
│   ├── openhal/           设备的枚举与描述（本期唯一接口）
│   └── openas/            信封、主体、效果类别的 Schema；一致性检查
├── kits/                  规范的实现与可复用组件：独立项目。不导入 luban.* / xlings.*
│   ├── opensal-wlr/       wlroots 系合成器 → opensal        （由 desktop/opensal/wlr 移来）
│   ├── openhal-host/      openhal 的宿主提供端：Linux（sysfs、DRM）、macOS（IOKit、Metal）、Windows（DXGI、SetupAPI）
│   ├── xdesk-base/        桌面组件：os、term、layout、apps  （由 desktop/xdesk/base 移来）
│   └── xdesk-compositor/  合成器、绘制、外壳                （由 desktop/xdesk/compositor 移来）
└── os/                    Luban OS 自己的组件：只有这里导入 luban.*
    ├── system/            stage-0、boot、machine、archive
    ├── service/           环境与机器的生命周期、事件、能力报告、控制面的执行层
    ├── vm/                机器运行时：supervisor、终端中继、guest 会话进程、hypervisor 后端（qemu、vz）
    ├── cli/               luban 命令树与终端呈现
    └── desktop/           Luban OS 应用的窗口（SDL3 作为 opensal 的宿主提供端）与 imgui（由 desktop/host 移来）
apps/                      用户直接接触的程序（不变）：luban、luban-os、xdesk、gui、xdev
modules/                   xlings 与 luban 共用的通用库（不变）
```

**为什么这样分**：一个包是"三件事一起变化"的边界（依赖的系统库、被哪类二进制链接、以后是否一起拆出）。
`specs/` 与 `kits/` 的每个包都能原样搬进自己的仓库；`os/` 只在本仓库有意义。
按"是什么"分，而不是按"属于哪个子系统"分（现在的 `luban/desktop/` 把规范、实现、Luban 的应用库混在一起）。

### 4.2 每个独立项目包的形态

```
luban/specs/openhal/
├── mcpp.toml              namespace 不是 luban；版本独立（0.x）
├── README.md              这是什么、怎么用、谁实现了它
├── SPEC.md                编号条款，陈述句，每条可测试（规范包才有）
├── src/                   声明（规范包）或实现（kit 包）
└── conformance/           一致性检查：任何实现都能对自己运行（规范包才有）
```

- 文档与注释自成一体：不引用 xlings、Luban 的内部文档，也不引用任何内部探索项目。
- 版本：包自己的 `version`；Luban 的发布不改变它，除非它本身变了。
- 拆出仓库的时机：有第二个实现者，或者本仓库之外有第二个使用者。拆出就是 `git subtree split` 加一行依赖改动。

### 4.3 openhal 的最小面：一个接口

**设备的枚举与描述**：

- 设备的种类：GPU、显示器、音频端点、输入设备。
- 每个设备有稳定 id、名字、能力（例如 GPU 的厂商与驱动接口、显示器的分辨率与缩放）。
- 契约：一个 concept（"一个设备源能列出设备"）加一个一致性检查。检查本身也是一个 TU：编译通过即形状正确，
  运行时再查 id 稳定、读不到时报告"不可用"而不是空列表。

消费者：P4-2 的能力报告（`luban status` 的设备一节、Luban OS 应用）。`modules/subos/sandbox/gpu.cppm` 回答的是
"哪些设备节点绑进沙箱"——那是授予，属于 Luban 的策略，留在原处；openhal 回答"有哪些设备"，两者不重复。

本期不做：设备透传、热插拔事件、音频流。它们以后在同一个包里加接口。

### 4.4 规则（lint）

| 规则 | 检查 |
|---|---|
| `luban/specs/*` 只导入 `std` 与自己的模块；规范之间互不导入 | `tools/lint_layer_deps.sh` 新增 |
| `luban/kits/*` 不导入 `luban.*`、`xlings.*`；可以导入 `specs/` | 现有的 opensal、xdesk 规则推广到整个目录 |
| `luban/os/service` 不导入 `luban/os/desktop` 与 `kits/xdesk-*` | 新增：OS 不依赖任何呈现 |
| 系统头文件与平台分支只在 `modules/platform`、`kits/*` 的平台提供端、`os/vm` 的 hypervisor 绑定 | 现有规则，路径更新 |
| Objective-C++（`.mm`）只出现在绑定 TU 里，导出 `extern "C"` 函数，由一个 C++ 绑定模块导出 | 新增，与 `tools/lint_c_headers.sh` 同一形状 |
| `apps/luban` 不链接图形库 | 现有 |

### 4.5 移动

只移目录、不改模块名（`import opensal;` 等不变），放在一个单独的 checkpoint 里，`mcpp.toml` 的 workspace 成员与 path 依赖随之更新。
`luban/desktop/host` 整体移到 `os/desktop`：它的窗口与 imgui 的帧循环是一体的，现在也只有 Luban OS 应用使用。
等出现第二个使用者时，再把 SDL3 的 opensal 提供端拆成 `kits/opensal-sdl`（按需实现）。

## 5. Luban OS 的总体架构

```
呈现      luban CLI · Luban OS 应用（TUI/GUI）· --json · NDJSON 接口 · （以后）MCP · 桌面里的面板
           └─ 同一个模型的不同渲染与绑定（§7）
控制面    模型（Schema，openas 信封）· 执行（service）· 绑定 · 活动记录
OS 组件   环境与机器的生命周期 · generation · edition · 用户与身份 · 运行时（abi）· 策略与授予
运行方式  sandbox（共享内核）· machine（自己的内核）
载体      local（Linux 命名空间）· wsl2 · vz · qemu（KVM）                    ← 平台差异
可移植层  modules/platform → openkal · openhal-host · os/desktop 的 SDL3 提供端             ← 平台差异
宿主      Linux · macOS · Windows
```

**Luban 系统里面处处相同**：无论宿主是什么，一个 Luban 系统都是同一个 Linux 用户态
（同样的包、运行时、用户、提示符、欢迎语）。宿主之间的差别只在"怎样承载它"。

## 6. 平台抽象：载体与能力

### 6.1 两种运行方式在三个平台上

| | Linux | macOS | Windows |
|---|---|---|---|
| **sandbox**（共享内核） | 本机命名空间（bwrap），毫秒级 | **Luban 宿主机**里的命名空间（§6.2），载体 vz | **Luban 宿主机**里的命名空间，载体 wsl2 |
| **machine**（自己的内核） | qemu + KVM（已有） | vz 直接启动这个环境（本期） | **Luban 宿主机里的 qemu + KVM**（WSL2 的嵌套虚拟化，Windows 11 默认开启；本期） |

"sandbox 共享谁的内核"在 macOS、Windows 上是 Luban 宿主机的内核。用户看到的语义不变：进入快，和同一台电脑上的其他环境共享内核。
只有第一次启动宿主机时多花几秒。

### 6.2 Luban 宿主机：每个 home 一台轻量 Linux 机器

wsl2 与 vz 两个载体已经按这个形状设计（"每个 xlings home 一台轻量 Linux VM，`/xlings` 是它的系统 home，用同一套 NDJSON 接口"）。
part 4 把它定为跨平台的统一模型：

- 它是一台最小的 Linux 机器：静态的 xlings 与 luban（这个版本的 Linux 构建），`/xlings` 是它的 home；
  bwrap 等其余一切像任何 Linux 宿主一样来自索引。它由载体创建、启动、停止，坏了可以重建（派生状态）。
- 环境的 payload 在大小写敏感、保留符号链接与可执行位的文件系统上，**不放在** 普通的 APFS、NTFS 上：
  - Windows：WSL 发行版自己的 ext4（vhdx）；
  - Mac：一个**大小写敏感的 APFS 稀疏卷**（系统自带的 hdiutil 创建并挂载），整个宿主机的树都在上面，
    经 virtio-fs 交给虚拟机；Mac 自己也看得见这些文件，所以 machine 可以直接从同一棵树启动一个系统。
- 宿主的目录只以授予的形式进入：一次一个目录、一个会话（vz 用 virtiofs，wsl2 用 drvfs）。
- 宿主机在第一次需要时启动，空闲后停止；`luban status` 报告它的状态。
- 在 Linux 上没有宿主机：本机就是。

### 6.3 能力报告：一张表，处处读同一份

`luban status`（不带名字）回答"这台电脑上能做什么"。每种运行方式一行，包括：

- 是否可用；
- 由哪个载体承载；
- 特性：目录共享是实时还是进入时复制，GPU，显示；
- 不可用时的原因与下一步。

设备一节来自 openhal。

```
$ luban status
Luban OS 2026.10.x -- macOS 15 (arm64)
  sandbox  ready     in the Luban host machine (stopped; starts in ~3 s)
  machine  ready     own kernel, 2 cores · 2 GB by default
  desktop  ready     in a Luban OS window
  devices  1 GPU (Apple M3) · 1 display · 2 audio endpoints
```

Linux 上同一张表、同样的词。Luban OS 应用的快速开始、欢迎语、agent 读的 `--json` 都读这一份。

### 6.4 协议：所有载体共用

| 通道 | 协议 | 传输 |
|---|---|---|
| 终端 | 现有的终端中继（NDJSON，窗口大小同步） | Linux：pty；vz：vsock；wsl2：stdio；qemu：virtio-serial |
| 显示 | opensal wire | 同上的字节流通道 |
| 控制 | openas 信封（§7） | 同上；宿主上为本地套接字 |
| 文件 | virtiofs / 9p / drvfs | 由载体选择，能力报告说出特性 |

新的载体只需提供一个字节流通道和一种文件共享方式；上面的一切不变。
这也是 §11 能在 Linux 上用 qemu 测完载体逻辑的原因。

### 6.5 可移植层与 openkal

`modules/platform` 是 port layer。迁移**按接口进行**，每一项都要有一个真正受益的功能：

| 顺序 | 接口 | 受益的功能 |
|---|---|---|
| 1 | pty | Luban OS 应用的终端在 Windows 上（ConPTY），三平台一份代码 |
| 2 | 本地 endpoint | 控制面的本地套接字：只有本用户能连 |
| 3 | process（带一组显式能力启动、进程组上限与整组终止） | 无人值守子进程、agent 的最小沙箱 |
| 4 | fs（以根为界的视图、只读绑定） | agent 沙箱的文件通道 |

openkal 今天没有 1、2，3、4 只有一部分。这几项合成**一份**上游提案，附 Luban 侧的实际需求；
接受之前实现留在 `modules/platform`，接口按提案的形状写，接受之后只换实现。

### 6.6 macOS 的虚拟化：C++ 绑定，签名的是 luban

- Virtualization.framework 是 Objective-C 接口。绑定模块 `luban.vm.native.vz`（`luban/os/vm/src/native/vz.cpp`）
  用纯 C++ 经 Objective-C 运行时调用（`objc_msgSend`，与 metal-cpp 同一做法），回调用 clang 的 blocks；
  不用 `.mm`：一个 `.mm` 在 Linux 的 gcc 上无法编译，而这个 C++ 单元在其他平台上只剩"这是 Mac 的"几个函数。
- 宿主机与 machine 是同一种 Luban 机器（`luban.vm`），只是 hypervisor 不同：qemu 的 9p 换成 virtio-fs，
  virtio-serial 的 `luban.agent` 换成 virtio console 的同名端口，客户端、supervisor 的桥、guest 的 agent 都是同一份。
  agent 增加了在管道上运行命令（`"pipes": true`：不回显、不改行尾、标准错误单独成 E 帧、F 帧关闭输入），
  这正是 `luban __vm exec` 承载 NDJSON 接口所需要的。
- 调用 Virtualization.framework 的进程需要 `com.apple.security.virtualization` 权限。这不是受限权限，ad-hoc 签名即可。
  所以由 `tools/macos_release.sh` 给 **luban** 签上这个权限。vz 是 `luban/os/vm` supervisor 的一个 hypervisor 后端，**不需要单独的辅助程序**。
- xlings 的 vz 载体（`modules/carrier/src/vz.cppm` 已写好的契约）改为调用 `luban __vm <verb>`。
  这是 plumbing 命令，经工具表调用；xlings 仍不导入 luban。

## 7. 控制面与可观测性

### 7.1 三层

| 层 | 内容 | 换方案时 |
|---|---|---|
| **模型** | 实体（环境、机器、会话、作业、镜像、包、用户、策略、设备）与动词。每个动词：参数与结果的 Schema、事件、错误、所需作用域、**效果类别**（只读 / 可撤销 / 不可撤销 / 对外），以及"没有人回答时用哪个参数" | 不变；CLI 的 spec、文档、补全、MCP 工具都从它生成 |
| **执行** | `luban/os/service`：返回结构化结果、发事件、经 `Asker` 提问、写活动记录 | 内部可以随意重构 |
| **绑定** | stdio 上的 NDJSON（与 `xlings interface` 同一信封）、本地套接字上的 JSON-RPC 2.0、载体通道、（以后）MCP | 加一种绑定是加一个适配器 |

**起点是已有的东西**：

- xlings 的 NDJSON 接口：capability 注册表、`inputSchema`、事件、取消、协议版本 1.9；
- `Asker` 与 agent 模式；
- 总体设计 §5.2 的呈现事件；
- §13 的 trace id 与 actor。

本期不另起一套，只把它推广到 luban，并把信封、主体与效果类别写进 `specs/openas`。

### 7.2 模型里决定能否平滑演进的几件事

1. **长操作是作业**：返回作业 id，过程以事件推送，可以取消、重新接上、查询。人从 GUI 发起，agent 可以接着观察。
2. **提问是一等事件**：带 id，任何有权的客户端都可以回答；没有人能回答时失败，并给出对应的参数。人和 agent 的差别只在"谁来回答"。
3. **查询与订阅**：状态是结构化快照（`status --json`），变化通过订阅得到。
4. **describe**：列出动词、Schema 与版本；只加不改，新客户端先探测再调用。
5. **效果类别决定确认策略**：只读不问；不可撤销与对外，按调用者的自主级别决定是否征求同意。
   agent 模式下"需要同意"就是一个带参数的失败。

### 7.3 主体与活动记录

现在的 `actor` 只区分 cli、agent、program。扩展为**主体**：

- 种类：人、agent、程序、服务；
- 名字；
- 委托链：只能缩小；
- 所在：宿主、哪个环境、哪台机器。

主体随子进程继承，不靠自报。

活动记录是 `logs/ops` 的推广，每条记录：

- 时间、trace id、因果 id（任务 → 作业 → 步骤 → 效果）；
- 主体、动词、对象、结果；
- **来源**：观察到的，还是声明的。只有观察、没有对应声明的效果，就是要先看的异常。

规则：

- 只追加，由机制写入。
- 人的操作不记内容，agent 的操作记内容。
- 按大小与时间轮转。
- 哈希链之后再做。

upgrade、destructive、elevation、abi 四份日志保留，加上 trace id，作为活动记录的专门视图。

### 7.4 端点可以嵌套

宿主上的 luban、Luban 宿主机里的 luban、每个环境里的 luban，提供**同一个接口**。宿主经载体通道转发到里面的端点，
trace id 贯穿每一层。从外部控制、在环境里自我管理（part 3 的 R 模式）、以后的 agent 系统服务，都只是这些端点的客户端。

### 7.5 agent 在环境里

Luban 已有的东西正好是 agent 沙箱需要的：

- 派生一个环境；
- 修改落在它的 generation 里形成差异；
- 所有者审阅后合并或丢弃；
- 每一代都可以退回。

还缺的是：

- 只把指定的套接字绑进环境（控制面、显示）；
- 只读绑定指定路径（agent 的运行时与凭据）；
- 在环境里派生环境；
- 主体随沙箱继承。

这些是机制，在 part 5 实现；本期只在模型里留位置。

### 7.6 openas 是 OS 与桌面共用的

桌面子系统需要同一类东西：实体、动词、事件、主体、记录。规则只有一条：**只有一种线协议、一种信封、一份主体模型**。
openas 的核心与领域无关；OS 是一个领域（本期由 luban 的 spec 导出动词），桌面是另一个领域。
中枢属于 OS 的会话，不属于任何呈现。

## 8. 各平台的可用性

### 8.0 分级与现状

| 级别 | 含义 | Linux | macOS（现状 → 本期） | Windows（现状 → 本期） |
|---|---|---|---|---|
| L1 管理 | luban CLI、Luban OS 应用能运行，能力报告说清楚 | ✓ | CLI 已发布；应用只在 CI 编译 → **发布应用** | 同左 |
| L2 环境 | sandbox 环境：new、enter、run、config、export | ✓ | vz 载体缺辅助程序，不可用 → **可用** | wsl2 载体已有，未与 luban 一起验证 → **可用** |
| L3 机器 | 自己的内核 | ✓ | 拒绝 → **vz 直接启动环境** | 拒绝 → **宿主机里的 KVM**（Windows 11） |
| L4 桌面 | desktop edition 显示在 Luban OS 窗口里 | ✓ | ✗ → **opensal wire 走 vsock** | ✗ → **opensal wire 走 wsl2 通道** |

### 8.1 macOS

1. vz 后端（§6.6），luban 签名。
2. 内核：`linux-kernel-virt` 打开 `VIRTIO_FS`、`FUSE_FS`、`VSOCKETS`、`VIRTIO_VSOCKETS`（跨仓库）；x86_64 与 aarch64 都有。
3. Luban 宿主机：第一次需要时从 `luban-core` 创建，放在 `<home>/carriers/vz/`。
4. machine：同一个 vz 后端，启动这个环境本身；环境的根经 virtiofs 进入，可写部分与 Linux 上的机器模式规则相同。
5. Luban OS 应用：release 产出 `luban-os-*-macosx-arm64`，index 的 `luban-os` 包加 macOS 段。
6. x86_64 程序：Rosetta for Linux 作为之后的可选能力，本期不做。

### 8.2 Windows

1. wsl2 载体补齐三件事：
   - luban 的 new、enter、status 走它，作为默认路径；
   - `luban setup` 经提权入口启用 WSL，需要重启时说清楚；
   - 首次进入的进度显示。
2. Luban 宿主机：wsl2 的发行版本来就是（`/xlings`，不自动挂载 Windows 盘，不开 interop）。
3. Luban OS 应用：release 产出 `luban-os-*-windows-x86_64`；终端经 ConPTY（§6.5 第 1 项）。
4. machine：在 Luban 宿主机里用 Linux 上同一份 qemu + KVM 代码启动（WSL2 的嵌套虚拟化，Windows 11 默认开启，
   当前 WSL 内核把 KVM 编成模块）。不需要提权、不需要 Hyper-V 或 Windows Hypervisor Platform（家庭版也可用），
   终端、显示、控制走已有的通道。代价：两层虚拟化，嵌套的机器共用 WSL 的内存与 CPU 上限，能力报告说明。
   不可用时能力报告说出原因与下一步：
   - Windows 10：WSL 在 Windows 10 上关闭嵌套虚拟化，替代为 sandbox，或导出镜像交给 Hyper-V / VirtualBox；
   - `.wslconfig` 关掉了 nestedVirtualization：一条改回的命令，加 `wsl --shutdown`；
   - ARM64 的 Windows：WSL 在 ARM 上不提供 KVM；
   - 有 `/dev/kvm` 但没有权限：宿主机内部的问题，由 Luban 自己修复。

### 8.3 Linux

行为不变。只做两件事：迁目录，加上能力报告的新格式。

## 9. 迁移与无感升级

- home 格式不变：`carriers/` 已是现有布局。宿主机的磁盘是派生状态，可以重建；环境的数据在环境自己的 home 里，规则不变。
- 目录移动只影响构建。
- 能力报告的 `--json`、openas 信封、协议版本都只加不改；旧客户端读到新字段时忽略。
- macOS 上 luban 换成带权限的签名：用户无感（签名不影响运行），由发布检查保证。

## 10. 实施

### 10.1 阶段与依赖

| 阶段 | 内容 | 依赖 |
|---|---|---|
| **P4-1 目录与规则** | §4 的移动与拆分；`luban/README.md`；每个 specs、kits 包的 README；lint；docs 更新 | — |
| **P4-2 设备与能力报告** | `specs/openhal`（一个接口）与 `kits/openhal-host` 三平台提供端；service 的 `Capabilities`；`luban status` 与 `--json`；Luban OS 快速开始读同一份 | P4-1 |
| **P4-3 控制面骨架** | `specs/openas`（信封、主体、效果类别、一致性检查）；luban 动词 Schema（由 spec 导出）；`luban interface`；describe；主体与因果 id 进 `logs/ops` | P4-1 |
| **P4-4 macOS 与 Windows** | vz 绑定（先验证）、luban 签名、`luban __vm`、vz 载体端到端、宿主机、machine 经 vz；wsl2 载体与 luban 端到端、`luban setup`；两个平台的 Luban OS 应用发布；ConPTY；显示与终端经各自通道 | P4-2；内核（跨仓库） |
| **P4-5 CI 架构** | §11 的三平台矩阵：Linux 上用 qemu 后端测载体逻辑；Windows 上真实 WSL2；Apple 芯片上的 validate、拒绝路径与签名；self-hosted 真机入口 | 与 P4-2 到 P4-4 同步推进，每个阶段的测试随阶段提交 |
| **P4-6 openkal 提案** | §6.5 的一份上游提案；port layer 按提案形状 | P4-3 |
| **P4-7 文档与发布** | docs/luban 三平台快速开始（中英）；AGENTS.md 规则；release 与 index；CN 镜像；验证 | 全部 |

P4-1 到 P4-3 是骨架，Linux 上行为不变。P4-4 与 P4-5 一起做：每个平台能力都和证明它的 CI 一起提交。

### 10.2 仓库与 PR

| 仓库 | 内容 |
|---|---|
| xlings（一个 PR，按阶段提交 checkpoint） | 全部代码；release 产出 macOS（arm64）与 Windows 的 luban-os；macOS 的 luban 带权限签名；CI 矩阵 |
| xim-pkgindex（一个 PR） | `linux-kernel-virt` 新版本（virtiofs、vsock）；`luban-os` 的 macOS 与 Windows 段；xlings、luban-os、xdesk 的版本 |
| xlings-res | 内核新配置的构建（两个架构） |
| openkal | 一份上游提案（issue），不阻塞本期 |

联调顺序：内核构建 → index 的包 → xlings 的 PR 在 CI 中用 index 分支验证 → 发布与 CN 镜像 → index 合入 → 三平台验证。

## 11. 可测性与 CI 架构

### 11.1 为什么 Luban 比传统 OS 好测

传统 OS 难测，是因为它的接口是给人的：屏幕、交互式提问、开机流程、散落各处的可变状态。
Luban 的每一条设计原则，同时也是一条可测性原则：

| 设计 | 带来的可测性 |
|---|---|
| core 返回结构化结果，前端只渲染 | 断言的是 JSON，不是屏幕上的文字；渲染另有快照测试 |
| agent 模式从不等待，缺什么就报出参数 | 每条路径都能无人值守地跑完；交互路径用 pty 测（part 3 的 `at_a_terminal`） |
| 一个模型、多个呈现；应用不能做 CLI 做不到的事 | 业务逻辑只在服务层测一次；呈现层只测渲染 |
| 隔离的 home（`Home::isolated`） | 测试互不干扰、可以并行、不碰真实 home |
| 环境是数据：generation、导出导入、golden | 一个测试可以几秒内建好、快照、恢复一个系统；sandbox 的 intent 是 golden |
| sandbox 不需要内核 | 用户、主机名、提示符、locale、运行时这些 OS 层的行为，毫秒级就能端到端验证 |
| 同一个 guest 镜像，载体只是一个字节流通道 | guest 一侧在 Linux 上测一次，就覆盖了三个平台的 guest |
| 显示是 opensal wire，输入走和人一样的协议 | 桌面可以无头运行：帧是数据（PPM golden），输入可以注入；Luban OS 应用用 SDL offscreen |
| 能力由探测说出，缺席是一个答案 | 拒绝路径（原因 + 下一步）和成功路径一样是一等测试对象，每个平台都有可断言的输出 |
| 活动记录区分观察与声明 | "没有归属的效果为 0"是一个可执行的不变量；测试断言的是"谁做了什么"，不只是结果 |
| 读不到不等于空 | 故障注入：损坏一个文件，断言只停在需要它的地方，并给出修复 |
| agent 的接口就是测试的接口 | 一个不带模型的脚本 agent 经 openas 驱动整个系统，就是 e2e；以后的真实 agent 基准用同一条路径 |

### 11.2 GitHub 托管 runner 的实测依据（2026-10）

| runner | 能做什么 | 依据 |
|---|---|---|
| Linux（ubuntu，x86_64 与 arm64） | 有 `/dev/kvm`：qemu 机器、镜像启动 | 现有的 luban 镜像测试 |
| `windows-2025` | 有嵌套虚拟化（Dadsv5），**能跑 WSL2**：用 `Vampire/setup-wsl`（`wsl-version: 2`，默认即 2）启用 | setup-wsl 的说明、runner-images #10563 的讨论；GitHub 官方不保证嵌套虚拟化 |
| `windows-2025`，`wsl-version: 1` | WSL1：静态 Linux 程序能跑，没有命名空间 | setup-wsl |
| `macos-15`（Apple M1） | **没有嵌套虚拟化**，vz 与 Hypervisor.framework 都起不了虚拟机 | GitHub 文档；M3 + macOS 15 才有嵌套虚拟化，托管 runner 未开启 |
| `macos-15-intel`（x86_64） | 能起虚拟机，但 **Luban 不支持 Intel Mac**，不为它构建，也不用它测 | —— |

### 11.3 矩阵

| lane | runner | 内容 |
|---|---|---|
| linux-unit / e2e（已有） | ubuntu x86_64、arm64 | 单元；sandbox e2e；machine；镜像启动；无头桌面；fresh-install |
| **linux-carrier**（新） | ubuntu x86_64（KVM） | 用 **qemu 后端**跑与 vz / wsl2 相同的载体逻辑：宿主机的创建、升级、启停；exec；目录授予；终端与显示通道；trace 贯通。使用同一个 `luban-core` 宿主机镜像 |
| **windows-wsl2**（新） | `windows-2025` + setup-wsl（v2） | **真实的 wsl2 载体**：`luban setup` 检测到已启用；宿主机（导入自己的发行版）；`luban new / enter / run / export`；sandbox 在里面跑；ConPTY 终端；Luban OS 应用无头测试 |
| **windows-machine-probe**（新） | `windows-2025` + setup-wsl（v2） | 宿主机里没有 `/dev/kvm`（托管 runner 是第三层虚拟化）：machine 的能力报告给出原因与下一步；machine 的启动逻辑与 Linux 同一份代码，由 Linux lane 覆盖 |
| **windows-wsl1**（新） | `windows-2025` + setup-wsl（v1） | 拒绝路径：能力报告说"sandbox 需要 WSL 2"并给出下一步；不运行 sandbox |
| macos-arm64（扩展） | `macos-15` | 单元；Luban OS 应用无头测试；vz 绑定的配置构造与 `validate`（磁盘、网络、virtiofs、vsock、显示各一项）；probe 如实报告"没有嵌套虚拟化"；发布产物的签名与权限检查。vz 之上的载体逻辑由 linux-carrier 覆盖，真正开机由 real-mac 覆盖 |
| 契约 | 三平台 | 假的 `wsl.exe`、假的 `luban __vm`：所有失败分支与"下一步"的文字 |
| 能力报告 | 三平台 | `luban status --json` 的快照，中英文 |
| **real-mac / real-win**（新，手动） | self-hosted（标签 `mac-real`、`win-real`） | 与 host-matrix 同一组场景脚本：Apple 芯片上的 vz、GPU、显示密度、真实 WSL 安装与重启。由维护者在自己的机器上触发 |

**托管 runner 上的嵌套虚拟化不是官方承诺。** 所以：

- windows-wsl2 这条 lane 的环境准备（setup-wsl、虚拟化探测）失败时，报告为**环境不可用**，并单独标注，
  不和"我们的代码错了"混在一起。
- 这两条 lane 要求在 PR 上通过；如果平台方撤销了支持，就改为在 main 上运行，并在能力报告的测试里保留拒绝路径。
  这个决定只改 workflow，不改代码。
- 每次运行把实测到的能力（WSL 版本、是否嵌套虚拟化、CPU）写进 lane 的 meta，便于发现环境变化。

### 11.4 需求编号

在 `tests/requirements.toml` 新增：

- `LAYOUT-SPECS`
- `HAL-DEVICES`
- `PLATFORM-CAPS`
- `CARRIER-QEMU-LOGIC`
- `CARRIER-VZ`
- `CARRIER-WSL2-LUBAN`
- `CONTROL-DESCRIBE`
- `CONTROL-PRINCIPAL`
- `APP-PLATFORMS`

## 12. 风险与待定

1. **Windows 10 与关闭嵌套虚拟化的机器上的 machine**：本期在能力报告里说明并给出替代；qemu + WHPX（提权、重启、大包）
   作为备选，等确实有用户需要时再做。
2. **托管 runner 的嵌套虚拟化**：不是官方承诺，可能变化（§11.3 的处理）。托管的 Apple 芯片 runner 以后若换成 M3 以上
   并开启嵌套虚拟化，就加一条托管的 macos-vz lane。
3. **macOS 只支持 Apple 芯片**：托管的 Apple 芯片 runner 没有嵌套虚拟化，vz 的真正开机只在 real-mac（self-hosted）上验证；
   托管 runner 测到 `validate` 为止，vz 之上的载体逻辑由 linux-carrier 用 qemu 后端覆盖。
4. **`.mm` 与 C++ 模块的组合**：P4-4 的第一步验证；不行就用 Objective-C 运行时的方案（§6.6）。
5. **宿主机的资源**：默认 2 核、2 GB，可配置；空闲停止的时长待实测。
6. **目录移动的范围**：path 依赖与文档链接一起变；放在单独的 checkpoint 里。
7. **openas 的名字**：撞名检查过（OpenAS2 属于 EDI 领域，不冲突）。

## 13. 自审（第二轮，多视角）

| 视角 | 检查 | 结论与调整 |
|---|---|---|
| 架构 | 规范、kit、OS 组件三类边界是否可检查？openas 与 openhal 有没有在真空里设计？ | lint 覆盖三类边界；openhal 只有一个接口，消费者就是能力报告；openas 本期只有信封、主体、效果类别，动词来自已有的 luban spec。都不是空包 |
| 稳定性 | 新载体会不会让 Linux 退化？托管 runner 变化会不会让 CI 不可信？ | Linux 行为不变，原有 lane 全保留；runner 环境问题单独标注，不和代码错误混在一起 |
| 优雅简洁 | 有没有新增概念或组件？ | 去掉了单独的辅助程序与 Swift；宿主机就是一个 Luban 系统；能力报告就是 `luban status`；控制面就是推广后的 `interface` |
| 用户体验 | 三平台的第一次体验一样吗？ | 同一套命令、欢迎语、提示符；差异只以能力出现，带原因与下一步；宿主机第一次启动有进度 |
| 兼容性 | 已有用户受影响吗？ | home 格式不变；协议、信封、`--json` 只加不改；macOS 的签名变化对用户无感 |
| 跨平台 | 差异真的只在三处吗？ | vz 与 wsl2 后端、openhal 与 opensal 的提供端、`modules/platform`。载体逻辑在 Linux 上用 qemu 后端测，证明它与平台无关 |
| 一致性 | CLI、应用、agent 是否读同一份？ | 能力报告、Schema、活动记录各只有一份 |
| 无感升级 | 宿主机和内核怎样升级？ | 随 index 升级，宿主机是派生状态，可以重建；环境的数据不动 |
| 可测性 | 每个新能力都有证明它的测试吗？ | §11.3：Linux（KVM）与 Windows（WSL2）的真实虚拟化有托管 lane；vz 在托管 runner 上测到 validate，开机与 GPU 留给 real-mac |
| agent | agent 能否不看屏幕、不等人地完成全部操作？ | describe、效果类别、提问事件、主体与记录；脚本 agent 同时就是 e2e |
| 安全 | 隔离会不会被放宽？ | 宿主目录只以授予进入；WSL 不挂载 Windows 盘、不开 interop；设备的授予仍是 Luban 的策略；签名只加 virtualization 一项权限 |
| 可观测 | 能否从记录还原跨宿主、宿主机、环境的一次操作？ | 一个 trace id 贯穿；嵌套端点转发时带上同一个 id |

**第二轮自审发现并已改进的地方：**

1. 第一版把 vz 交给一个 Swift 辅助程序。这既多一个包、多一种语言，又让虚拟机不归 luban 的 supervisor 管。改为 C++ 绑定，并给 luban 签名。
2. 第一版的 CI 默认"托管 runner 跑不了虚拟机，只能靠真机"。实测依据表明 Windows 2025 能跑 WSL2，
   因此 Windows 的真实虚拟化进了 CI；macOS 只支持 Apple 芯片，vz 的开机留给真机，其余由 validate 与 linux-carrier 覆盖。
3. 第一版对 openhal 只留位置。但能力报告本身就需要"有哪些设备"，不立 openhal，枚举代码就会继续散在 Linux 专用的模块里。
   所以以一个接口立起来。
4. 托管 runner 的嵌套虚拟化不是官方承诺，第一版没有处理。现在规定环境不可用要单独标注，并写下 lane 降级的办法。

## 14. 实现记录（2026-10-15）

与 part 3 同在 PR #667，按阶段提交 checkpoint：

| 提交 | 阶段 | 内容 |
|---|---|---|
| P4-1 | 目录与规则 | `luban/{specs,kits,os}`；lint 按三类检查（变异测试过）；specs、kits 的 README，opensal 的 SPEC.md，去掉它们对内部设计文档的引用 |
| P4-2 | 设备与能力报告 | `specs/openhal`（一个接口：设备的枚举与描述，concept + 一致性检查与反向对照）、`kits/openhal-host`（Linux、macOS、Windows）；`luban.service.capabilities`；`luban status` 与 `--json`；Luban OS 应用的快速开始与导入读同一份 |
| P4-3 | 控制面骨架 | `specs/openas`（主体与委托、效果类别、信封、一致性检查）；`CommandSpec.effect`；`luban interface --describe` 与 `luban interface <verb> --args`；操作日志的 `principal`（经 `XLINGS_ACTOR` 继承）与 `parent`（经 `XLINGS_OPS_PARENT`，因果链） |
| P4-4a | Windows | 宿主机（wsl2）的镜像带 Linux luban；`xlings __carrier-probe` 与 `probe.json`；machine 在宿主机的 KVM 上（`enter_in_carrier_`）；luban 描述载体上的系统时问那里的 luban；luban 认得自己所在的 home；`subos new --carrier` 转发每个选项的值（之前丢掉 `--from` 的值，是真 bug）；Luban OS 应用为 macOS、Windows 打包发布 |
| P4-4b | macOS | `luban.vm.native.vz`（Objective-C 运行时）；`luban __vm` 是 vz 载体的辅助程序；宿主机的树在大小写敏感的 APFS 卷上；guest 的 virtio-fs 与宿主机模式；agent 在管道上运行命令；qemu 与 vz 共用一个桥；luban 带 virtualization 权限签名；machine 经 vz 从同一棵树启动 |
| docs | 文档 | 宿主支持（能力报告与三平台表）、架构（目录与包）、agent 一章（`luban interface`）、操作日志规范、各平台表、SubOS 在 macOS 上 |

**与设计不同、实现时决定的：**

1. `luban/desktop/host` 整体移到 `os/desktop`，SDL3 的 opensal 提供端等有第二个使用者再拆（§4.5）。
2. 宿主机不是从 `luban-core` 创建的 Luban 系统，而是最小的 Linux 机器：静态的 xlings 与 luban，其余按需来自索引（§6.2）。
3. vz 不用 `.mm`：Linux 上的 gcc 编译不了 `.mm`；纯 C++ 经 Objective-C 运行时在所有平台都能编译（§6.6）。
4. agent 的 `"pipes": true`、`E`、`F`、`S` 帧：宿主机承载 NDJSON 接口所需，都是协议的增量。
5. CI 的 linux-carrier lane 是 `tests/e2e/luban_carrier_test.sh`：发布包的静态构建经 wsl2 载体的替身走完 luban 的全路径
   （创建、由那里的 luban 描述、运行命令、删除），而不是在 Linux 上用 qemu 再起一台宿主机；
   宿主机里运行命令的协议由 `test_luban_agent` 覆盖，vz 的配置在 macOS CI 上由 `test_luban_vz` 交给框架验证。
6. Windows 的 WSL1 拒绝路径沿用已有的 E2E-08（托管 runner 默认没有 WSL2）与 `wsl-import`，不另开 lane。

**跨仓库：**

- xim-pkgindex：`linux-kernel-virt` 6.12.112-r1（`FUSE_FS`、`VIRTIO_FS`；build.sh 支持配置修订）由 res-build 构建；
  `luban-os` 的 macOS 与 Windows 段，xlings、luban-os、xdesk 的版本，随发布更新。
- openkal：需求聚合为 mcpplibs/openkal#47（`2026-10-15-openkal-proposals-from-luban.md`），只记录，不期望现在改动；Luban 按需使用 openkal。

**Luban OS 应用的全平台（实现时补充）：**

- 一个双工子进程（`xlings.platform.duplex`：POSIX 管道 + poll，Windows 管道 + 超时读取）让 `luban.vm` 的 Client
  可以经一个中继说同一种协议；
- Windows：沙箱的控制台与桌面经宿主机里的 agent（`wsl.exe ... luban __agent-stdio`：命令在终端或管道上运行），
  机器经 `luban __pipe-unix` 到宿主机里的 supervisor，启动经 `luban __machine-start`（事件中继回来）；
- macOS：机器经 vz，窗口直接连本机的 supervisor；
- 系统的桌面会话随描述一起给出（`Environment::desktops`），载体上的由那里的 luban 回答。

**需要真机验证的（一台 Apple 芯片的 Mac、一台 Windows 11 的 PC）：**

- Apple 芯片上宿主机的创建、启动、`luban new / enter / run`、machine；
- Windows 11 上 machine 经宿主机的 KVM 启动；
- GPU 与显示。

## 15. 自审（第三轮：实现之后）

| 视角 | 检查 | 结论 |
|---|---|---|
| 架构 | 规范、kit、OS 三类边界是否被代码遵守？ | lint 逐条检查，变异测试证明能抓到；openhal、openas 都有消费者与一致性检查 |
| 稳定性 | Linux 的机器与沙箱有没有被重构波及？ | supervisor 的桥抽成一个函数，qemu 一侧行为不变；luban 与机器的单元、e2e 在本机全过 |
| 一致性 | 载体上的系统与本机系统，`luban status` 的说法一样吗？ | 一样：载体上的由那里的 luban 回答，同一个接口 |
| 兼容性 | 协议、日志、`--json` 是否只加？ | 是：O 的 `argv`/`pipes`，E/F/S 帧，`principal`/`parent`，`modes`/`devices` |
| 跨平台 | macOS 与 Windows 的代码能在没有它们的 CI 上验证多少？ | 载体逻辑经替身在 Linux 跑通（含静态发布包）；agent 的管道模式在 Linux 测；vz 配置在 macOS 由框架判定；真机开机留给维护者 |
| 安全 | 宿主机会不会放宽隔离？ | WSL 不挂 Windows 盘、不开 interop；Mac 的目录只经 `S` 授予到 `/grant`；签名只加 virtualization 一项权限 |
| 可观测 | 一次跨宿主机的操作能还原吗？ | trace、actor、parent 经 `__carrier-env` 与 `WSLENV` 进入宿主机 |
| 发现并修复 | — | `subos new --carrier` 丢掉选项的值；luban 在宿主机里找不到自己的 home；`status` 的 JSON 是包在 `result` 里的值 |

