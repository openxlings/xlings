# SubOS 总体架构设计 Part 3：内容 × 视图 × 承载 —— 跨平台承载、内核无关与 Luban 分层

- 承接：`2026-10-05-subos-architecture-design.md`（Part 1）、`2026-10-06-subos-architecture-design-part2.md`（Part 2）。
  本文**修订** Part 2 的 §3.1（呈现方式 → 视图，并新增"承载"维度）和 §13（跨平台），以及 Part 1 的 §17 与附录 A
  （平台抽象层的接口形状、macOS / Windows 路线）。其余章节不变。
- 依据：PR #641 head `24cb3ae9` 的代码 review（2026-10-09），三轮维护者讨论，以及 `mcpplibs/openkal` 0.15 的 `SPEC.md`。
- 交付：**不在 #641 中**。#641 合入后，作为后续的一组 PR，checkpoint 从 C43 开始编号（§13）。
- 状态：设计草案第一轮，本文不含代码改动。§1 是已确认的决定，§15 是本轮需要 review 的问题。

---

## 0. 一句话

**SubOS 是唯一的名词。** 一份 SubOS（内容）可以用不同的**视图**呈现（PATH 叠加 / 沙箱 / 根 / 整台机器），
也可以跑在不同的**承载**上（宿主内核 / WSL2 / 轻量 VM / Luban 自己的内核）。payload 的目标 ABI 决定它需要哪种承载。
凡是和内核相关的语义，都对齐 openkal：Linux 只是一种实现，以后的 Luban 内核是另一种。

由此：

- **各平台的体验一致，指的是用法一致**：命令、参数、事件、退出码、policy 都相同。实现上可以借助平台现成的技术
  （Windows 用 WSL2，macOS 用 VZ），但 xlings 不和某个平台的隔离机制绑定。
- 在 Windows 上，**是 xlings subos 以 WSL2 作为承载**，而不是"让用户到 WSL2 里去运行 xlings"。
  用户面对的始终是 Windows 上的 `xlings subos …`。
- **Luban OS = machine 视图 × native 承载**，也就是 R 模式，加上内核、安装器/ISO 和发布通道。
  R 模式本身已经是"以 xlings 作为系统包管理器的发行版"的雏形。
- 代码上，`luban/` 与 `modules/` 都是顶层目录：只属于"一台机器"的部分放在 `luban/`（模块名以 `luban.` 开头），
  xlings 与 Luban 共用的部分放在 `modules/`（模块名继续以 `xlings.` 开头）。

---

## 1. 本轮确认的决策（维护者，2026-10-09）

| # | 问题 | 决定 |
|---|---|---|
| E1 | 多平台体验一致的含义 | **用法一致**，不要求各平台机制相同；能用平台技术（如 WSL2）支撑的就用 |
| E2 | Windows 上的完整功能 | **xlings subos 基于 WSL2 实现**：用户不进入 WSL，也不直接在 WSL 里使用 xlings |
| E3 | 内核 | 早期使用 Linux 内核，长期可能有自己的内核；**内核相关语义尽量对齐 openkal，做到内核无关** |
| E4 | Luban 的代码归属 | 顶层新增 `luban/`，与 `modules/` 并列，模块名以 `luban.` 开头；xlings 与 Luban 共用的放在 `modules/` |
| E5 | `modules/` 的命名 | 共用包继续以 `xlings.` 开头 |
| E6 | hosted 形态是否默认带 Luban 承载 | 维护者："选最合适的设计"。本文的选择：**按 ABI 懒创建承载**（§5.6） |
| E7 | Linux 宿主上 xlings 的定位 | 维护者的修正：xlings 在 Linux 宿主上**几乎从不依赖宿主环境**，工具的运行时全部来自 xlings 生态，只是看起来"直接装在宿主上"。这和"宿主的包"不是一回事（§3.2） |

---

## 2. 现状（#641 head `24cb3ae9` 的 review 结论）

### 2.1 上层模型可以跨平台，执行层是按 Linux 设计的

| 层 | 模块 | 可移植性 | 依据 |
|---|---|---|---|
| 模型与生命周期 | model、manifest、roles、userdata、home_view | 好 | 纯数据和文件系统；`roles::check` 与平台无关 |
| 策略 | policy、policy_store | 好 | 纯数据，未知字段一律拒绝，Must/Should 降级模型通用 |
| 宿主能力 | caps、gates | 差 | `Caps` 的字段写死为 `bwrap/proot/userns/landlock_abi/pasta/seccomp`；`gates.cpp:60-73` 中 macOS/Windows 那一行是手写的 "not implemented" |
| 规格编译 | `spec::compile` | 中 | 输出 `SandboxSpec` 是 Linux 字段的并集（`unshare_*`、`MountKind::Proc/Dev`、`pasta_bin`、`landlock_rw`、`proot_root`）；按 `caps.platform == "macos"` 字符串分支（`spec.cpp:209`） |
| Provider | provider | 中 | 纯函数、可做 golden 测试；但接口是"返回 argv"，默认后端一定是外部 helper 进程 |
| 后端分派 | `src/core/subos/sandbox.cpp:858-868` | 差 | 分派写在 core 的适配层，加后端要改 core |
| Session | session | 差 | `kSessions = platform::is_linux`；使用 `SOCK_SEQPACKET`（`process.cpp:128,359`），**macOS 的 AF_UNIX 不支持**；`Launch` 里是 `int` fd |
| 两套执行路径 | `sandbox.cpp:915-948` | 差 | macOS/Windows 设置环境变量后直接 `run_shell`：没有 supervisor、broker、审计、join、timeout |

### 2.2 依赖工具

- **SubOS 不依赖 brew**（产品代码里出现 brew 的只有 `platform/unix.cppm:110` 的一处注释）。
- bwrap、proot、pasta 由 `caps::locate_*` 按 payload → runtimedir → host 的顺序查找，并记录来源。
- `root_cmd.cpp:947/951/1038` 通过 `host_tool_` 直接调用宿主的 `tar`、`mkfs.ext4`，用 `as_root_` 走 sudo。
  **工具解析有两套，没有统一入口。**

### 2.3 root 视图与 generation（review 发现，需要在本设计中解决）

| 编号 | 问题 | 位置 |
|---|---|---|
| H1 | `remove` 判断 payload 是否还被引用时只看 workspace（`installer.cpp:1586`），不看 `root.gen/*`；`rollback` 前的校验 `owned_generation` 只核对链接字符串，不检查目标是否存在。payload 删掉之后再回滚，`/usr` 里的链接会悬空。**违反 Part 2 §6.3 "旧一代引用的 payload 不会被 GC"**。这是读代码得出的推断，尚未复现 | `rootfs.cpp:121` |
| H2 | `commit` / `switch_to` 没有 fsync 文件和目录。原子性只对并发读者成立，掉电后指针可能指向元数据为空的一代 | `rootfs.cpp:314-386` |
| M1 | `rootfs::prune` 在产品代码里没有调用方，generation 只增不减；`current()` 读不懂指针时，`prune` 可能删掉指针真正指向的那一代 | `rootfs.cpp:395` |
| M2 | `switch_to` 每次都完整遍历一代的树（`owned_generation`）：300 个 payload 时 9.598 ms，预算 10 ms，余量只有 4%，链接越多越不够 | — |
| — | root 视图直接使用 `fs::create_symlink` 和 `rename`，generation 的语义被写死在 POSIX 符号链接上 | `rootfs.cpp` |

---

## 3. 核心模型（修订 Part 2 §3.1）

### 3.1 三个维度

| 维度 | 取值 | 含义 |
|---|---|---|
| **内容**（SubOS） | workspace、home、policy | 一份 xlings 生态的用户态。**唯一的名词** |
| **视图**（View） | `overlay` · `sandbox` · `root` · `machine` | 进程以什么方式看到这份内容 |
| **承载**（Carrier） | `local` · `wsl2` · `vz` · `native` | 这份内容跑在哪个内核上，以及宿主 xlings 怎么到达它 |

**ABI 规则**：一个 payload 的目标 ABI（取自它的平台块：`linux` / `macosx` / `windows`，以及以后的 openkal 目标）
决定它需要哪种承载。

| payload 的 ABI | Linux 宿主 | Windows 宿主 | macOS 宿主 | Luban OS |
|---|---|---|---|---|
| linux（ELF） | local | wsl2 | vz | native |
| windows（PE） | — | local | — | — |
| macosx（Mach-O） | — | — | local | — |
| openkal | local | local | local | native |

一个 SubOS 的 ABI 由它的 workspace 决定，在创建时可以显式指定（§10）。

### 3.2 视图（Part 2 的"呈现方式"改名并补齐）

| 视图 | 进程看到的 | 现有对应 |
|---|---|---|
| `overlay` | 宿主的根；SubOS 的 `bin/` 在 PATH 前面，工具的运行时（loader、库、sysroot）来自 xlings 生态 | 默认的 shell 级进入；全局 SubOS |
| `sandbox` | overlay 加上隔离（宿主文件、网络、进程、身份） | `--sandbox` |
| `root` | SubOS 本身就是 `/`（`/usr` 是 generation 投影） | `--rootfs` 实例、导出的镜像 |
| `machine` | `root` 视图成为整台机器的根，由 stage0 启动 | R 模式、Luban OS |

**overlay 视图不是"宿主的包"**（E7）：内容来自 xlings 生态，只是投影到了宿主的 PATH 上。所以在 Linux 宿主上，
overlay 和 root 的区别只在于"谁是 `/`"，而不在于"是谁的包"。

### 3.3 用三个维度描述所有形态

| 形态 | 内容 × 视图 × 承载 |
|---|---|
| Linux 上默认的 xlings（U/C/P/S/M） | 全局 SubOS × overlay × local |
| `subos use --sandbox` | SubOS × sandbox × local |
| `subos new --rootfs` | SubOS × root × local |
| R（multi / single） | `default` × machine × local |
| Windows 上 Windows 原生工具的 SubOS | SubOS × overlay（home 重定向）× local |
| **Windows 上 Linux 的 SubOS** | SubOS × sandbox / root × **wsl2** |
| **macOS 上 Linux 的 SubOS** | SubOS × sandbox / root × **vz** |
| **Luban OS（ISO）** | `default` × machine × **native** |

**不变量**：同一份内容声明，在每一种承载上得到的 root 视图完全一致。这是 Part 2 §3.1 不变量的推广，并以 VIEW-CARRIER-EQ 进入测试（§12）。

### 3.4 术语

- **宿主（Host）**：承载者，即 Ubuntu / macOS / Windows。Luban OS 上没有宿主（或者说宿主就是内核）。
- **"嵌套的 SubOS"**：root 视图里运行的 xlings 看到的是 machine 视图（Part 2 §3.3 不变）。
- **不新增 "System" 这类名词。** "这个 SubOS 是一个根"或"这个 SubOS 是这台机器"都是视图。
  Part 2 §3.4 的 `kind` / `role` 保持不变：`kind=rootfs` 表示该 SubOS 支持 `root` 视图，`role=host` 表示它当前是 `machine` 视图。

---

## 4. 与 openkal 的关系

### 4.1 边界

openkal 0.15 规定了程序与运行环境之间的 ABI（`kal_*`），实现按平台分为不同的包（`openkal-linux` / `-macos` / `-windows`）。
与 SubOS 直接相关的几条：

- 文件系统基于能力：没有隐含的工作目录。程序只能看到调用方授予的预打开目录，数量、顺序、名字都确定，第一个就是工作目录（SPEC §7.13）。
- `kal_process_spawn` 只传调用方明确授予的三个流和目录，**连调用方自己继承来的句柄也不传**；`kal_spawn.job` 表示"一组一起结束的进程"。
- 强制隔离被明确留给环境："the confinement of one that does not [use only its preopens] is the environment's responsibility"（§7.13）。
  权限和身份**不在规范范围内**（§11 第 6 条）。平台依赖属于各个包自己（README 规则 2）。

所以分工是：

```
        SubOS（环境的构造者与守卫）
          决定：授予哪些目录、流、网络端点、环境变量，属于哪个 job
          强制：对不只使用 kal_* 的程序，由承载上的隔离机制兜底
  ──────────────────────── openkal ABI（程序收到什么）────────────────────────
        程序：只使用 kal_* 的程序，在任何实现上看到相同的环境语义
```

### 4.2 对齐方式

| SubOS 概念 | openkal 语义 | Linux 实现 | 以后 Luban 内核上的实现 |
|---|---|---|---|
| 文件系统授权 | 有名字、有顺序的预打开；第一个是 cwd | bwrap bind / drvfs / virtiofs | `kal_process_spawn` 的授权由内核强制执行 |
| 进程范围 | `kal_spawn.job` | pid ns（以后加 cgroup） | 内核原生的 job |
| 网络 none / proxy | 只交出指定的端点（`openkal.net` 只认地址和端口） | netns + 中继 | 不授权就无法访问 |
| 终端 | `openkal.terminal` | 现有实现 | 同左 |
| root 视图 / generation | 不在 openkal 范围内：定义为**版本化的"逻辑路径 → payload"映射表**（§7.2） | 符号链接树 + `rename` | 内核的命名空间表 |
| 身份、exec 观测、设备、句柄传递 | openkal 刻意不定义 | 留在 `modules/platform` 的内核特性适配中 | 由那个内核的扩展提供 |

### 4.3 `modules/platform` 的演进

- **可移植的部分逐步改为调用 `kal_*`**：spawn + job、相对路径 fs、stream、net、terminal、time、random。
  每换一项，就用 openkal 的 conformance 测试替代 xlings 自己的一部分跨平台测试。
- **openkal 不覆盖的部分**（隔离、挂载、身份、文件所有权、句柄传递、ELF、Windows 硬链接 shim）
  留在 `modules/platform` 里作为"内核特性适配"，按 openkal 的风格声明：每个平台都声明，不支持的返回 unavailable。
- **接入前要先确认两点**（C53 的前置条件）：
  1. README 规则 3 要求一个镜像里只有一个 C 运行时。xlings 能否在现有 C 运行时下（Linux musl / macOS libc / Windows MSVC CRT）
     只链接 `openkal-<platform>` 的实现，还是必须整体切换到 `openkal-musl`（这在 Windows 上意味着工具链从 MSVC 换成 Cygwin 式环境）？
  2. 跨进程传递句柄（SCM_RIGHTS / `DuplicateHandle`）是否在 openkal 的规划中。不在的话，它留在内核特性适配里。

---

## 5. 承载（Carrier）

### 5.1 接口

```cpp
// modules/carrier
struct CarrierCaps { bool supported; gates::Enforced enforced; std::string reason, route, evidence; };
struct Carrier {
    std::string_view name;                                              // local | wsl2 | vz | native
    CarrierCaps (*probe)(const HomeView&);                              // 可用吗？缺什么？怎么启用
    std::expected<Endpoint, Unmet> (*ensure)(const HomeView&, Elevation&);  // 按需创建 / 启动
    std::expected<Channel, std::string> (*control)(const Endpoint&);    // 控制通道：NDJSON
    std::expected<int, std::string> (*terminal)(const Endpoint&, std::span<const std::string> argv,
                                                const Stdio&);          // 终端通道：交互或批处理
    std::expected<Grant, Unmet> (*grant)(const Endpoint&, const HostPath&, Mode, std::string_view name);
    std::expected<void, std::string> (*stop)(const Endpoint&);          // 空闲时停止
};
```

**宿主 xlings 与承载里的 xlings 之间不定义新协议**：

- **控制通道**沿用现有的 NDJSON interface（`xlings interface`）。承载里运行的是 `xlings __carrier-agent`，
  它就是那一侧的 interface 服务端。
- **终端通道**直接在承载里执行对应的子命令（例如 `xlings subos use <n>`），终端由承载的启动器负责。
- 不需要跨承载传递文件描述符。

### 5.2 `local`

即今天的行为：内容、视图、隔离都在宿主内核上完成。Linux 宿主上所有视图都由 `local` 提供；
macOS/Windows 上，本机 ABI 的 SubOS 由 `local` 提供 overlay 视图（home 重定向）。

### 5.3 `wsl2`

**形态：每个 xlings home 一个承载发行版，不是每个 SubOS 一个。**

| | 每个 SubOS 一个发行版 | **每个 home 一个（采用）** |
|---|---|---|
| store、引用计数 | 每份 ext4 各存一份，重复 | 共享，和 Linux 宿主的模型一致 |
| generation / rollback / GC | 分散 | 由承载里的 Linux xlings 统一管理 |
| SubOS 之间的隔离 | WSL 的发行版边界（所有发行版共用同一个 VM 和内核，并不更强） | Linux xlings 的 ns + policy，和 Linux 宿主一致 |
| 启动开销 | 每个都要冷启动 | 只启动一次，空闲时停止 |

需要单独导出或分发的 SubOS，可以显式指定 `--carrier-instance dedicated`，单独占一个发行版（例外情况）。

**设计细节**（WSL 的行为需要在 C49 中逐条实测）：

- **创建**：`wsl --import xlings-<home-id> <home>\carriers\wsl2 <rootfs.tar> --version 2`。
  - 按用户注册，不需要管理员；
  - rootfs 是官方 `subos:luban-tiny` 的 `export --tar` 产物，作为 xpkg 分发；
  - `<home-id>` 由 home 的路径派生，所以同一台机器上可以有多个 home。
- **承载发行版本身就是一个 machine 视图的 Luban（tiny）**，承载里的 home 位于 `/xlings`（Part 2 §4 的系统域，multi 布局）。
  这样它和官方镜像、导出镜像共用同一个前缀域，payload 可以直接复用。
  Windows 侧的 xlings 是它的远程控制端；用户在 Windows 上看到的 `subos list`，就是承载里的 SubOS 加上本机的 SubOS。
- **与 Windows 隔离**：承载里的 `/etc/wsl.conf` 写入
  `[automount] enabled=false`、`[interop] enabled=false`、`appendWindowsPath=false`。
  interop（从 Linux 里执行 `.exe`）是逃逸到 Windows 宿主的通道，**必须关闭**，并在 C49 里用负对照测试证明它确实被关掉了。
- **授权映射**：项目目录不靠自动挂载，而是按授权在承载里以 root 挂载：`mount -t drvfs 'C:\proj' /grant/<name>`。
  这和 openkal 预打开的语义一致（§6.1）。授权的生命周期与 session 相同。
- **控制通道**：`wsl.exe -d <名字> -u root --exec /xlings/bin/xlings __carrier-agent`，NDJSON 走标准输入输出。
- **终端通道**：`wsl.exe -d <名字> -u root --exec /xlings/bin/xlings subos use <n> …`。
  交互时由 wsl.exe 负责终端；退出码、Ctrl-C 透传。环境变量由协议显式传递，不依赖 `WSLENV`。
- **生命周期**：第一次需要时创建（§5.6）；按需拉起；空闲后 `wsl --terminate`；`subos remove` 删除最后一个承载里的 SubOS 时**不**自动注销承载。
- **用户数据**：承载的 vhdx 里装着 SubOS 的 home，**属于用户数据**。注销承载（`--unregister` 加删除 vhdx）只能由经过确认的删除命令触发，
  并按 AGENTS.md 的"SubOS user data"规则写入 `destructive.ndjson`。
- **前置条件**：首次启用 WSL 需要一次管理员权限（`wsl --install --no-distribution`），通过 `Elevation` 接口（§6.4）完成并明确告知用户。
  WSL1 是系统调用翻译，没有 namespace，`probe` 报告 unsupported。
- **失败时**：WSL 不可用时，按 Must/Should 规则拒绝或降级，并给出启用路线。Windows 本机 ABI 的 SubOS 不受任何影响。

### 5.4 `vz`（macOS）

形态与 `wsl2` 相同：每个 home 一台轻量 VM，里面是 machine 视图的 luban-tiny，home 位于 `/xlings`。

- 控制通道和终端通道走 vsock 上的同一套 NDJSON interface。授权通过 virtiofs 共享目录实现。
- 客户机的内核和 initrd 作为 xpkg 分发（例如 `xim:xlings-guest-kernel`），不依赖 Lima、Colima 或 brew。
- **需要验证**：Virtualization.framework 要求带 `com.apple.security.virtualization` entitlement 的签名。
  待确认 ad-hoc 签名在本机是否够用；不够的话，改为由一个带签名的小 helper（`xlings-vm`，作为 payload 分发）来承担 VMM。
- Apple Containerization（每个容器一台 VM）作为远期选项，取决于它的系统版本要求。

### 5.5 `native`（Luban OS）

machine 视图直接跑在 Luban 自己的内核上。早期就是 Linux 内核，等同于 `local`。
以后换成自有内核时，只替换 `modules/platform` 的内核特性适配和 `confine` 的实现（§4.2），上层不变。

### 5.6 选择规则（E6）

1. Linux 宿主：所有 SubOS 都用 `local`，**行为和现在完全一样**。
2. macOS / Windows：默认的 SubOS 是本机 ABI 的 overlay（`local`），**永远不会因此启动 VM**。
3. 以下任一情况第一次出现时，为这个 home 创建承载（`wsl2` / `vz`）：
   - `subos new … --rootfs`；
   - SubOS 的 ABI 是 linux；
   - 用户显式指定 `--carrier`；
   - 在 macOS/Windows 上，SubOS 的 policy 要求 Must 级别的强隔离，而 `local` 无法满足。
   之后按需拉起，空闲时停止。
4. 承载不可用时，按 Must/Should 规则处理：Must 级别拒绝（exit 125）并给出启用路线，Should 级别降级并说明原因。
   agent 模式下不会停下来提问，结构化错误中带上启用承载所需的确切命令。

---

## 6. 执行层的抽象

### 6.1 Intent IR

把 `SandboxSpec` 中"要什么"的部分拆出来，成为平台无关的中间表示。**文件系统授权按 openkal 预打开的语义定义。**

```cpp
namespace xlings::subos::intent {
struct Grant { std::string name; HostPath source; bool writable; };   // 有名字、有顺序；grants[0] 是 cwd
struct Fs    { std::vector<Grant> grants; bool hide_host; std::optional<RootRef> root; };
struct Net   { policy::Net mode; std::string proxy; bool host_loopback; std::vector<Publish> publish; };
struct Proc  { bool own_job; bool kill_on_close; std::optional<Limits> limits; };
struct Dev   { std::set<std::string, std::less<>> grants; };
struct Ident { policy::Identity mode; std::string user; std::string tz; };
struct Obs   { policy::Observe level; };
struct Intent { Fs fs; Net net; Proc proc; Dev dev; Ident id; Env env; Obs obs;
                std::map<std::string, policy::Need, std::less<>> needs; Command cmd; };
std::expected<Intent, Refusal> lower(const policy::Policy&, const Request&);   // 纯函数
}
```

- 同一份 Intent 可以翻译成 bwrap 的 bind、承载里的 drvfs/virtiofs 挂载，以及以后 `kal_process_spawn` 的授权。
- 现有 `SandboxSpec` 变成 linux-ns 这个实现自己的计划（Plan）。
- 环境变量的平台翻译（Windows 用 `USERPROFILE/APPDATA`，POSIX 用 `HOME/XDG_*`，`spec.cpp:593-613`）移到 Ident/Env 的翻译函数里。

### 6.2 Confine SPI、注册表与选择器

```cpp
// modules/confine
struct Implementation {
    std::string_view name;                                         // linux-ns | linux-landlock | linux-proot | home-redirect | …
    std::map<std::string, Capability> (*probe)(const HomeView&, const Ports&);   // 按维度声明：Kernel/Advisory/None + 证据
    std::expected<Plan, Refusal> (*plan)(const intent::Intent&, const ProbeResult&);   // 纯函数，golden 测试
    std::expected<Started, std::string> (*launch)(Plan&, LaunchCtx&);             // 唯一有副作用的一步
};
```

- 外部 helper 类的实现（bwrap、proot）在 `launch` 内部生成 argv，**golden argv 测试保留**。
  API 类的实现（进程内 namespace、以后的 `kal_spawn`）不经过 argv。
- **选择器**：按 Intent 中的 Must 维度挑第一个能满足的实现，Should 维度记入 `degraded`。`--sandbox <name>` 指定实现。
- **`gates::probe` 改为汇总各实现的 `probe()`**，`subos status` 的矩阵和文档中的矩阵都由此生成，不再手写。
- `src/core/subos/sandbox.cpp` 中的后端分派删除，core 只负责把 Request 交给选择器。

### 6.3 Session：三个接口

| 接口 | 职责 | Linux | macOS（本机 ABI） | Windows（本机 ABI） |
|---|---|---|---|---|
| `Transport` | 消息 + 句柄 | SEQPACKET + SCM_RIGHTS | STREAM + 长度前缀 + SCM_RIGHTS | 命名管道 / AF_UNIX + `DuplicateHandle` |
| `ProcessScope` | job、整树终止、等待、资源限制 | pid ns（以后 cgroup） | 进程组 + kqueue | Job Object |
| `ExecTracer` | exec 审计 | seccomp user-notify | 不提供（Advisory） | Job Object 新进程通知 |

- `Launch` 中的 `int` 改为 `platform::Handle`（跨平台、只能移动的句柄）。`refresh_root` 这类 Linux 专有能力，
  作为 linux-ns 实现的扩展，不放在通用的 Launch 中。
- **macOS/Windows 本机 ABI 的 SubOS 也走 supervisor**，从而获得 join、`--timeout`、统一退出码、审计和 broker。
  隔离等级仍然如实报告为 Advisory。
- 优先级低于承载（§13）：完整功能由承载提供；这一步是让本机 ABI 的 SubOS 在用法上与其他形态一致。

### 6.4 ToolResolver 与 Elevation

依赖分三级，越往上越优先：

| 级别 | 是什么 | 例子 | 规则 |
|---|---|---|---|
| T0 进程内 | 静态链接的库或 OS API | libarchive、namespace/Landlock/seccomp、Job Object、VZ | 首选 |
| T1 自带 payload | xlings 从索引安装、按 sha256 固定的工具 | bwrap、pasta、e2fsprogs、客户机内核、luban-tiny rootfs | T0 做不到时使用 |
| T2 宿主工具 | 系统自带 | `wsl.exe`、sudo / UAC | 必须声明、探测、报告，并给出替代路线；**永远不调用 brew/apt/winget 去安装** |

```cpp
struct ToolNeed { std::string_view name, package, purpose; ToolTier max_tier; };
std::expected<ResolvedTool, Unmet> resolve(const ToolNeed&, const HomeView&, const Ports&);
```

- `caps::locate_bwrap/locate_proot/locate_pasta` 和 `root_cmd` 的 `host_tool_` 统一合并到 `resolve`。
- 立即调整：`tar` 改用 libarchive（T0）；`mkfs.ext4 -d` 改用 `xim:e2fsprogs`（T1）。
- **`Elevation`**：Linux 用 sudo/polkit，macOS 用 Authorization Services，Windows 用 UAC。每次提权都写入审计。现有的 `as_root_` 并入这里。
- **lint**：在 `modules/` 和 `luban/` 下，用字面工具名启动进程必须经过 `resolve`（`tools/lint_tool_resolve.sh`）。

---

## 7. Store 与 root 视图

### 7.1 `modules/store` 与 GC roots 契约（修复 H1）

- payload store、引用图、完整性校验从 `xim` / `xvm` / doctor 中抽出，成为 `modules/store`。
- **GC roots**：所有"固定 payload"的持有者都通过登记 root 来表达：
  - workspace 的 `active` / `installed`；
  - **每个保留中的 generation**（它的 `links.tsv` 里引用的 payload）；
  - 正在运行的投影（`running_projection`）；
  - 承载里的 home（由承载自己的 store 管理，宿主不跨承载收集）；
  - 已知项目。
- `is_version_referenced_anywhere_` 改为查询 store 的引用图。读不出来的 root 一律视为"还在引用"（沿用 AGENTS.md 中"读不到不等于空"的规则）。
- **保留策略**：每个 rootfs SubOS 保留最近 N 代（默认 5，可配置），另外固定当前代、启动项引用的代和正在运行的代。
  `prune` 接入 install / use / remove 之后的路径（修复 M1）；`current()` 读不出来时，`prune` 拒绝执行。

### 7.2 root 视图：版本化映射表

```cpp
// modules/subos/views/root
struct RootMap { std::vector<Entry> entries; };     // 逻辑路径 → payload 内的目标
struct RootStore {                                    // 由 StoreLinker 实现
    std::expected<Gen, std::string> commit(const RootMap&, std::string_view reason);  // 持久化 + 原子发布
    std::expected<void, std::string> switch_to(Gen);
    std::vector<Gen> list(); std::optional<Gen> current();
    std::expected<void, std::string> validate(Gen);   // O(1)：比对提交时记录的摘要
};
```

- **Linux / POSIX 实现**：符号链接树 + `rename`，即现有的 `rootfs.cpp`。
  - **持久化**（修复 H2）：每一代的文件、目录，以及指针的父目录，在 rename 之前和之后都要 fsync。直接复用 `platform.cppm:295-316` 已有的 sync 原语。
  - **校验**（修复 M2）：`commit` 时记录树的摘要（链接数加上 `links.tsv` 的哈希），`switch_to` 时只比对摘要，并检查链接的目标是否存在（这也能挡住 H1 的悬空情况）。完整的树遍历只保留给 `doctor`。
- **Windows / 自有内核的实现**：不使用符号链接。Windows 上 root 视图只存在于承载里，宿主不需要实现。自有内核用命名空间表实现，语义相同。

---

## 8. 代码布局与依赖规则

```
modules/                       xlings 与 Luban 共用，模块名 xlings.*
├── libs/  runtime/  ui/  testkit/
├── platform/      OS 边界；逐步基于 openkal（§4.3）；src/ 下按 os/ process/ net/ isolation/ fs/ target/ 分目录
├── store/         payload store、GC roots、完整性/签名                         ← 新增
├── subos/         内容与视图
│   ├── model/     model、manifest、roles、ports、home_view、userdata
│   ├── policy/    policy、policy_store
│   ├── intent/    Intent IR、lower()
│   ├── views/     overlay、sandbox、root（RootMap / RootStore、library_cache）
│   └── session/   session、broker、Transport / ProcessScope / ExecTracer
├── confine/       linux-ns、linux-landlock、linux-proot、home-redirect；选择器      ← 从 subos 拆出
└── carrier/       local、wsl2、vz；native 的接口                                   ← 新增

luban/                         只属于"一台机器"，模块名 luban.*
├── boot/          stage0、init、boot.json（现 subos/boot、subos/stage0）
├── machine/       machine 视图：机器的 /etc、sysusers、用户、主机名、服务（现 rootfs.cpp 中的 fill_machine_etc / apply_sysusers 等）
├── image/         disk / ISO、安装器、export --disk
└── release/       官方分级（tiny/core/desktop）的基础 manifest、发布通道、签名策略、machine 视图下的 self update

apps/              xdev、gui，以及（C52）luban-init
src/               xlings 前端：索引、recipe、解析、获取、CLI / NDJSON / TUI
```

**依赖规则**（`tools/lint_layer_deps.sh`，加入 `xlings-ci-linux` 的 lint 套件）：

1. `luban/ → modules/`；**`modules/` 不能 import `luban.*`**。
2. `modules/` 和 `luban/` 都不能 import `xlings.core.*`，沿用现有 Ports 的做法。
3. `src/` 只在与机器相关的命令中（`subos boot`、`export --disk`、machine 视图下的 `self update`）通过一个窄接口使用 `luban.*`。
   非 Linux 平台的构建不链接 `luban/`。
4. `modules/store` 只依赖 libs、runtime、platform。

**划分标准**：一个东西是否只在 SubOS 成为"一台机器"时才存在。root 视图和 generation 在 Linux 宿主的 `--rootfs` 和各承载里都要用，
所以留在 `modules/subos/views/root`；stage0、启动项、机器的 `/etc`、镜像、发布属于 `luban/`。

**`luban-init` 独立二进制**（C52）：stage0 拆成一个小的静态二进制。AGENTS.md 规定"stage0 执行路径上不能使用 Config"，拆分后由构建本身保证。
代价：要调整现有"stage0 字节和 inode 不变"的测试，以及 machine 视图下的自升级流程。

---

## 9. 契约

| 契约 | 内容 | 现在的雏形 | 版本号 |
|---|---|---|---|
| Env manifest | 一个 SubOS = workspace + policy + 授权（按 openkal 预打开语义）+ ABI + 承载 | `policy.json` + `instance.json` | `schema` 字段；读取时遇到未知字段拒绝 |
| Root map | 一代 = 映射表 + 摘要 + 元数据 | `links.tsv` + `generation.json` | 同上 |
| Store GC roots | 谁固定了哪些 payload | 无 | 同上 |
| Session 协议 | supervisor ↔ 客户端 | SEQPACKET 上的 JSON | 消息里带 `v` |
| Carrier 协议 | 宿主 xlings ↔ 承载里的 xlings | NDJSON interface | 沿用 interface 的版本协商 |

宿主、承载、发行版三方只通过这五份契约交互。每份都有兼容测试：上一个 release 写出的文件，新版本必须能读。

---

## 10. 使用面

**不新增顶层名词，只在现有命令上加选项。**

| 命令 | 变化 |
|---|---|
| `subos new <n> [--carrier auto\|local\|wsl2\|vz] [--abi native\|linux]` | 默认 `auto`，按 §5.6 选择 |
| `subos new <n> --rootfs` | 在 macOS/Windows 上自动使用承载，不再报 unavailable（修订 Part 2 §13） |
| `subos list` / `subos status` | 每一行显示视图、承载、ABI；矩阵按承载显示 Enforced 等级 |
| `subos use` / `exec` / `start` / `stop` / `rollback` / `export` / `remove` | 用法不变，由承载路由 |
| `self doctor --carrier` | 检查承载：是否可用、版本、interop 是否关闭、vhdx 大小、承载里的 xlings 版本 |
| `subos carrier stop\|status` | 运维用，不是日常命令 |

agent 模式：承载需要提权或首次启用时，不会提问，返回结构化错误，里面带上确切的命令。

---

## 11. 安全与用户数据

- **承载的磁盘（vhdx / VM 磁盘）是用户数据**，适用 AGENTS.md 的"SubOS user data"规则：只有确认过的删除命令才能注销或删除它，并写入 `destructive.ndjson`。
- **wsl2 承载的 interop 和自动挂载必须关闭**，用负对照测试证明（CARRIER-WSL-INTEROP-OFF）。
- **授权的生命周期与 session 相同**：session 结束时卸载 drvfs / virtiofs 映射。
- 每次提权都经过 `Elevation`，写入审计：谁、为了什么、执行了什么命令。
- 承载里的 xlings 与宿主 xlings 的版本差异由 `self doctor --carrier` 报告。宿主执行 `self update` 时，在承载里也安装同一个 release
  （承载是一台 machine 视图的 Luban，走 Part 2 §5 R 模式下的 `self update`，所以可以回滚）。

---

## 12. 需求与测试

| 需求 ID | 内容 | 车道 |
|---|---|---|
| LAYOUT-DEPS | §8 的依赖规则由 lint 强制 | lint |
| TOOL-RESOLVE | 用字面工具名启动进程必须经过 `resolve`；`subos status` 列出工具及其来源 | lint + linux |
| ROOT-GC-ROOT | 回滚到旧的一代后，链接的目标都存在（H1 的负对照：先 remove 再 rollback） | linux |
| ROOT-GEN-DURABLE | commit 过程中被 kill 或模拟掉电后，指针指向一个完整的代（用 fault injection 截断写入） | linux |
| ROOT-GEN-RETAIN | 超过保留数量的代会被回收；读不懂指针时 prune 拒绝执行 | linux |
| ROOT-SWITCH-SCALE | checked switch 在 300 个和 3000 个 payload 时都不超过 10 ms | perf |
| INTENT-EQ | Linux 上引入 Intent 和 Confine SPI 前后，golden argv 逐字节相同 | linux |
| GATE-CONFORM-* | 每个维度一套 conformance 测试；实现要么通过，要么在 `probe` 中声明 Advisory/None；矩阵由测试结果生成 | 全平台 |
| VIEW-CARRIER-EQ | 同一份内容声明在 local 与 wsl2（以及后续的 vz）上的 root 视图完全一致 | linux + windows |
| CARRIER-WSL-LIFECYCLE | 创建、拉起、空闲停止、`subos remove`，以及经过确认的注销 | windows |
| CARRIER-WSL-INTEROP-OFF | 承载里执行 `cmd.exe` 失败；看不到 `/mnt/c` | windows |
| CARRIER-WSL-GRANT | 只有被授权的目录可见；session 结束后映射被卸载 | windows |
| CARRIER-UNAVAILABLE | 没有 WSL 或只有 WSL1 时，Must 级别拒绝并给出路线，本机 SubOS 不受影响 | windows |
| SESSION-NATIVE-SUPERVISED | macOS/Windows 本机 ABI 的 SubOS 支持 join、timeout，退出码与 Linux 一致 | macos + windows |

Windows runner 只有 WSL1 的问题（Part 2 §17.1 第 5 条）：CARRIER-WSL-* 在找到 WSL2 runner 之前，先在本地或自托管机器上执行，
CI 中只作为尽力项报告。**不会因为测不了就把它写成已验证。**

---

## 13. 交付（C43 起，#641 合入之后）

| C | 内容 | 完成标准 | 前置 |
|---|---|---|---|
| C43 | 布局：新增 `luban/` 顶层；抽出 `modules/store`；拆出 `modules/confine` / `carrier` 的空骨架；`subos` 与 `platform` 内部按目录分层；依赖方向 lint | **只移动文件，行为零变化**，全平台 CI 通过；更新 AGENTS.md 和写死路径的测试（`subos_runtime_*_test.sh`、`lint_platform_headers.sh` 等） | #641 |
| C44 | Store GC roots 契约；generation 登记为 root；保留策略和 prune 接入 | ROOT-GC-ROOT、ROOT-GEN-RETAIN | C43 |
| C45 | RootMap / RootStore 抽象；fsync；O(1) 校验 | ROOT-GEN-DURABLE、ROOT-SWITCH-SCALE | C44 |
| C46 | ToolResolver + Elevation；tar 改用 libarchive；mkfs 改用 e2fsprogs payload | TOOL-RESOLVE | C43 |
| C47 | Intent IR + Confine SPI + 选择器；gates 由 probe 生成；后端分派移出 core | INTENT-EQ，**Linux 零变化** | C43 |
| C48 | Carrier SPI + `local`；`__carrier-agent`（复用 NDJSON interface） | Linux 上通过 local 承载跑完现有 e2e | C47 |
| C49 | `wsl2` 承载 | CARRIER-WSL-*、VIEW-CARRIER-EQ（windows） | C48、C45 |
| C50 | Session 三接口；macOS/Windows 本机 ABI 的 SubOS 走 supervisor | SESSION-NATIVE-SUPERVISED | C47 |
| C51 | `vz` 承载 | VIEW-CARRIER-EQ（macos） | C48、签名验证 |
| C52 | `luban/` 归位：boot、stage0（`luban-init` 独立二进制）、machine、image、release | 现有的 rootfs_instance / rootfs_image / boot 场景全部通过 | C43 |
| C53 | `modules/platform` 分项接入 openkal | 每一项都有 openkal conformance 测试加上 xlings 自己的回归；§4.3 的两点已确认 | §4.3 |

C44 / C45 修复的是已合入代码中的问题（H1 / H2 / M1 / M2），**优先级最高**，可以紧跟在 #641 之后单独发版。

---

## 14. 风险

| 风险 | 应对 |
|---|---|
| C43 的大规模文件移动干扰进行中的分支 | 只移动文件、一个提交完成，并在 #641 合入后立即做 |
| WSL 的行为随 Windows 版本变化（Store 版与内置版、wsl.conf 的选项） | `probe` 记录 WSL 版本；CARRIER-WSL-* 按版本跑；不满足最低版本时报告 unsupported |
| drvfs 性能差 | 授权只用于项目目录；store 和 generation 一律放在承载的 ext4 里 |
| VZ 的签名要求 | C51 前先做一次签名验证；不行就改用签名的 helper |
| openkal 与 xlings 现有 C 运行时的冲突（README 规则 3） | C53 之前不承诺接入；先确认 §4.3 的两点 |
| Confine SPI 抽象过度 | C47 的完成标准是 Linux golden argv 逐字节不变；没有第二个实现需要的接口一律不加 |
| 承载里的 xlings 版本漂移 | `self update` 同步更新承载；`doctor --carrier` 报告差异 |

---

## 15. 本轮需要 review 的问题

1. **§3 的模型**：SubOS 作为唯一的名词，加上视图（overlay / sandbox / root / machine）和承载（local / wsl2 / vz / native）两个维度，以及 ABI 规则。
   这会把 Part 2 的"呈现方式"改名为"视图"，并新增 `machine` 这个取值，是否认可？
2. **§5.3 每个 home 一个承载发行版**，并且承载本身就是 machine 视图的 luban-tiny，home 位于 `/xlings`，是否认可？
3. **§5.6 的懒创建规则**，特别是第 3 条中"在 macOS/Windows 上，Must 级强隔离的 policy 自动使用承载"，是否合适？
4. **§8 的划分**：root 视图和 generation 留在 `modules/subos`，boot、stage0、machine、image、release 放到 `luban/`；`confine` 和 `carrier` 成为独立的包。
5. **C52 把 stage0 拆成独立的 `luban-init` 二进制**，是否接受由此带来的测试和自升级流程调整？
6. **C44 / C45（修复 H1 / H2 / M1 / M2）是否先于其他所有工作，在 #641 之后单独发版？**
7. **§7.1 的保留策略默认值**（最近 5 代，加上当前代、启动项和正在运行的代）。

---

## 16. 实施计划（维护者确认 §15 全部，2026-10-09；6 改为：全部在 #641 内，以 checkpoint commit 交付）

### 16.1 多角度拆分

| 角度 | 本轮要守住的 | 落在哪些 C |
|---|---|---|
| 架构 | 依赖方向 `luban → modules`，`modules ↛ xlings.core`；后端、承载、工具都是注册的实现，core 不分派 | C43 C46 C47 C48 C52 |
| 稳定性 | GC roots 覆盖 generation；generation 持久化；读不到不是空；prune 拒绝未知指针 | C44 C45 |
| 优雅简洁 | 不新增顶层名词；不新造协议（承载复用 NDJSON interface）；没有第二个实现需要的接口不加 | C47 C48 |
| 用户体验 | 用法不变；`subos status/list` 显示视图、承载、ABI；承载不可用时给出确切路线；agent 模式不提问 | C48 C49 C50 |
| 兼容性 | 旧 home、旧 generation（无摘要）可读；`links.tsv` 格式不变；`kind=rootfs` / `role=host` 不变 | C44 C45 |
| 跨平台 | Linux 零行为变化（golden argv 逐字节）；macOS/Windows 本机 SubOS 进 supervisor；WSL2 承载；VZ 承载以 probe + 路线交付 | C47 C49 C50 C51 |
| 一致性 | 矩阵由 probe 生成；需求 ID 全部进 `tests/requirements.toml` 并有 `covers` | C47 全程 |
| 无感升级 | 新字段只追加；旧 generation 第一次切换时补写摘要；旧 client 读新文件不受影响 | C44 C45 C48 |

### 16.2 依赖

```
C43 布局 ──┬─► C44 GC roots/保留 ─► C45 持久化/O(1) ─┐
           ├─► C46 ToolResolver/Elevation            ├─► C49 wsl2 ─┐
           ├─► C47 Intent/Confine ─► C48 Carrier/local┘             ├─► 自我 review ─► 合入/发布
           │                    └─► C50 Session 三接口 ─► C51 vz ───┘
           └─► C52 luban/ 归位（luban-init）
C53 openkal：§4.3 两点确认后分项接入；确认不了则记录为结论，不阻塞
```

### 16.3 每个 checkpoint 的完成标准

- 一个 commit（或一组以 `(C4x)` 结尾的 commit），本地 `mcpp build` + 受影响的 `mcpp test` 通过；
- 新需求 ID 写进 `tests/requirements.toml`，测试带 `covers`；
- 涉及跨平台的改动在 macOS / Windows CI 上实际跑过，不以"Linux 通过"代替；
- 文档（AGENTS.md、Part 3 §19 实施记录）随 commit 更新。

---

## 19. 实施记录（2026-10-09，PR #641）

| C | 提交 | 结果 | 验证 |
|---|---|---|---|
| C43 | `f91c481` | 目录分层（platform `os/ process/ net/ isolation/ fs/ triple/`；subos `model/ policy/ sandbox/ views/root/ session/`；core `subos/cmd.cpp root/ domain/`）；接口单元不含系统头；`src/` 中全部平台 `#if` 改为 `if constexpr`（剩余 14 处为编译器差异或 runtime 层，带 `platform-if-ok`）；新增 `lint_platform_branches`、`lint_layer_deps`；CI 运行全部 `tools/lint_*.sh`（`lint_platform_headers` 此前没有 CI 步骤） | 三平台 CI 编译通过 |
| C44 | `02816a8` | `modules/store`；generation 成为 GC root（H1）；保留 5 代 + 释放（M1）；读不懂的指针不 prune | ROOT-GC-ROOT、ROOT-GEN-RETAIN（CLI 级：remove 后回滚找得到目标；prune 释放） |
| C45 | `0aacae7` `94041bd` `b7040ad` | 持久化（H2，fsync 顺序可追踪）；切换校验改为目录 change stamp + payload 存在性（M2）：300 payload 0.2 ms、3000 payload 1.9 ms（优化）/ 3.0 ms（dev）；发布新代只校验树（静态车道发现：导出镜像的链接指向逻辑路径） | ROOT-GEN-DURABLE、ROOT-SWITCH-SCALE；静态车道 RootExport |
| C46 | `f36ec0e` | 工具表、提权入口；export/pack 的 tar 改为进程内（libarchive，root-owned，无需 user namespace）；`priv_prefix` 删除；`std::system` 清零 | TOOL-RESOLVE；distro 车道 docker import / qemu boot 通过 |
| C47 | `58d0e0c` `86527fa` | Intent IR + `modules/confine` 注册表；core 不再分派后端；矩阵由实现声明汇总 | INTENT-EQ：872 个录制用例逐字节一致；GATE-CONFORM |
| C48 | `898b67d` | `modules/carrier`、`choose`、`subos new --carrier --abi`、按名转发、`list` 统一（interface 的 list_subos 原先缺 kind） | CARRIER-LOCAL、CARRIER-UNAVAILABLE |
| C49 | `a8f6262` | wsl2 承载：每个 home 一个发行版，镜像 = 本版本 Linux 构建 + 声明的机器文件（关闭 automount/interop），`__carrier-env` / `__carrier-grant` | 假 wsl.exe 跑通整个生命周期（发现 guest 无 env(1)）；Windows 车道 E2E-08（有 WSL2 则走生命周期与 interop-off，无则拒绝并给路线）；VIEW-CARRIER-EQ deferred（需要真实 guest 内核） |
| C50 | `482b54c` | macOS：消息 socket 为分帧的 SOCK_STREAM，会话在 macOS 上与 Linux 同一条路径；Windows：Job Object（整树、超时 124） | SESSION-NATIVE-SUPERVISED 由 macOS / Windows 工作流把关；Windows 的 join 需要 CreateProcess supervisor，未声称 |
| C51 | `43ab70f` | vz 承载，经 `xlings-vm` 助手的契约 | CARRIER-VZ-LIFECYCLE（假助手）；CARRIER-VZ-HELPER deferred（签名的 Swift 包，托管 macOS 无法嵌套虚拟化） |
| C52 | `f1dd2e2` | `luban/`（boot、stage0、machine）；`apps/luban-init` 独立包：0 个前端符号（作为根包目标时 6812 个）；发现 `inline constexpr` 模块变量仅由前端发射的潜在缺陷 | LUBAN-INIT（release tarball 上检查） |
| C53 | 本提交 | 见下 | OPENKAL-COEXIST（CI 步骤） |

### C53：§4.3 两个问题的结论

1. **openkal 能否在 xlings 现有 C 运行时下链接：能（Linux 实测）。** `openkal-linux` 0.16.1 直接基于系统调用，与 libstdc++ 在 glibc
   动态构建和 xlings 发布用的 x86_64-linux-musl 静态目标下都能链接、运行（`tests/openkal`，CI 每个 PR 构建并运行两种目标）。
   一个互操作事实：`kal::write` 绕过 C 库的 stdio 缓冲，混用前必须 flush。`openkal-windows` 声明"不使用任何 C 运行时符号"，
   `openkal-macos` 直接基于内核调用；两者在 xlings 接入时由各自平台 CI 验证。
2. **跨进程传递句柄：openkal 0.15 不定义**（SPEC §11 第 9 条："A general mechanism for passing a handle to a context in another
   space is not defined by this version"）。因此 `Transport`（SCM_RIGHTS / `DuplicateHandle` / 分帧流）留在 `modules/platform`。

**本 PR 不把 openkal 接入 xlings 的产品代码**：可移植的部分（spawn、fs、stream）目前没有一项能因换成 `kal_*` 而让 xlings 的行为变好，
为接入而接入只会把一个新依赖放进三平台的发布路径。第一项有意义的接入是 confine 中"按授权启动 openkal 程序"的实现
（Intent 的 grants → `kal_process_spawn` 的预打开）——它与 Luban 自有内核一起做，那时授权由内核强制执行（§4.2）。
