# Luban OS：分层、home 格式框架、呈现层、运行方式与 desktop —— 总体设计

日期：2026-10-12　基线：2026.10.12.3　状态：已确认，按 §11 实施，发布为 2026.10.13.1（xlings 一个 PR，按 checkpoint 提交；跨仓库见 §17；实施时的决定见 §11.1）

来源：在一个长期使用的 home（398 个 payload）上从旧客户端升级到 2026.10.12.3 的完整记录（`self doctor --fix`、
升级提示），以及按 README 第一次体验 Luban（`luban new`、`luban try`）的记录。本文把这些问题和围绕它们的讨论
收成一个整体方案：先修能立即修的，再把结构理顺，让同类问题不再出现。

**已确认的决定**：

1. 依赖方向反转：xlings 不再导入 `luban.*`；`xlings subos boot`、可启动镜像的导出（iso / img / qcow2 / drive）移到 luban。
2. Luban 目前没有用户：命令树一次改到位，不留兼容写法。
3. 目录按职责三分：`modules/` 通用库，`luban/` Luban OS 的组件与服务，`apps/` 用户直接接触的程序。
4. home 格式只做向前迁移 + 读者宽容 N-1（§4.2）；机器模式直接启动环境本身（§8.1）；显示协议在 guest 一侧（§9）。
5. 没有过渡方案：机器模式从第一版起就保留修改（原 §8.4 删除）。
6. GUI 应用叫 **Luban OS**（包与二进制 `luban-os`），产品名一律写作 "Luban OS"（§14）。
7. 桌面与显示相关的规范属于 Luban 的桌面子系统（`luban/desktop/spec`，openxx 一族：opensal 等），
   Luban 不另造一套；不属于桌面的，先用 Luban 自己的格式（§15）。
8. opensal 是桌面与显示之间的抽象；宿主一侧的提供端本版以 SDL3（mcpp 索引的 `compat.sdl3`：Linux 上 X11，Windows、macOS 原生）实现，
   imgui 经 SDL 的渲染器绘制（§14.6）。一个后端覆盖所有平台，原生后端以后可以替换它而应用不变。
9. 官方桌面两条线：传统桌面、xdesk 实验版，底层都走 opensal（§9.3）。
10. 音频先做简单版本：virtio-snd 与 PipeWire / Pulse 的现成标准，不写新协议（§9.4）。
11. 关窗口：每次都问；"确认并设为默认" 写入**该环境**的配置（§14.4）。
12. 桌面子系统按需实现在 `luban/desktop/` 下，只放现在需要的，代码、注释与文档自成一体（§16）。

---

## 0. 问题总表

| # | 现象 | 根因（已确认，除非标注） |
|---|---|---|
| P1 | `self doctor --fix` 修复其他 subos 时出现 `[sudo] password`，无法输入，卡住 | 子进程以 `timeout 600s xlings self doctor --fix --subos X` 运行（`doctor.cpp` `repair_other_subos_walk_`）。GNU `timeout` 不带 `--foreground` 时 `setpgid(0,0)`，子进程在后台进程组，sudo 读终端收到 SIGTTIN 停住，直到超时被杀。子进程为什么需要提权：待复现（推测是 image 存储的 loop mount，`sandbox.cpp` `mount_image_`） |
| P2 | doctor 给的修复命令是空操作，同一项连跑 3 遍 | 检测与修复不一致：`android-emulator`、`qemu-*` 报 `executable '<包名>' not found`，修复是 `xlings install X`，install 回答"已安装、配置是最新的"，什么都不做，发现项不消失 |
| P3 | 升级提示 59 项，其中 30 项来自同一个 payload | 按登记项（一个包登记的 30 个短名）逐条报告，而不是按根因 |
| P4 | "xlings 2026.10.12.3 (from 0.4.40)" | 升级阶梯在这个 home 上第一次运行，基线回落到很旧的 `version` 字段；报出来的是 home 多年积累的问题，却被说成"升级后做不到的事" |
| P5 | 升级提示太长：5 条细节 + 24 条命令 + 回退 | 默认输出没有"结论 + 一条命令"的层次；细节没有只放在 `-v` 和日志里 |
| P6 | `luban try` 第一次就失败：qemu 窗口显示启动菜单后黑屏 | 三处叠加：virt boot profile 的 cmdline 只有 `console=ttyS0 console=ttyAMA0`；`linux-kernel-virt` 没有 `FRAMEBUFFER_CONSOLE` / `DRM_SIMPLEDRM` / `DRM_BOCHS` / `DRM_FBDEV_EMULATION`；tiny 的 shell 开在 `/dev/console`（串口）。有 `DISPLAY` 时 `luban try` 开图形窗口，内核和 shell 都在被藏起来的 serial0 上。`luban_image_test.sh` 只测 `-nographic`，默认路径从未被跑过。README 写"qemu, UEFI"，x86_64 上实际是 SeaBIOS |
| P7 | luban 的输出混乱 | `luban new`：4 行 "is already installed" + 4 行 "configured"、`[xlings]` 前缀、两个总结、提示的是 `xlings subos use`。`luban try`：limine `bios-install` 的 13 行原始输出（`root_cmd.cpp` `run_tool_` 继承 stdout）、正在 try 时提示"try it with `luban try`"、开机前不说控制台在哪里 |
| P8 | qemu 暴露在用户面前 | qemu 的窗口、`Ctrl-A X`、`--nographic` 这个选项名 |
| P9 | 进哪个 shell，三处各自决定 | `sandbox.cpp`（root：有 bash 用 bash，否则 sh，不认 fish）、`machine.cpp`（root 用户固定 `/bin/sh`）、tiny 的 inittab（`/bin/sh`） |
| P10 | 命令语义 | `luban new <name> [edition]` 把属性放成位置参数；`config <name> <key> <value>` 的 key 里有动作（allow、mount）；`try` 与 `enter` 是同一件事的两个动词，且 `try` 是 live 镜像，修改全部丢失 |
| P11 | 分层 | xlings 导入 `luban.stage0/boot/image/machine`（`main.cpp`、`root.cpp`、`root_cmd.cpp`）；luban 以拼 xlings 命令行、在同一终端上运行的方式工作，没有结构化接口，desktop 无从建起 |
| P12 | 缺的能力 | 有 export 没有 import；没有"每次启动都一样"的临时模式；没有自己的窗口 |
| P13 | home 格式的版本是零散的 | `.xlings-home` 的 `layout` 有时是整数 2，有时是字符串 `multi`/`single`（`home.cpp`）；迁移分散在 `main.cpp` 的自愈、`ensure_home_layout`、`self migrate`、各处 `COMPAT(...)` |

---

## 1. 原则

1. **luban 只暴露 OS 的概念**：环境、机器、控制台、桌面。qemu、vz、WSL2、bwrap 是实现，用户的视觉与交互在各载体上一致；
   藏不住的差异用 OS 的话说（"这台机器没有硬件虚拟化，会慢"，而不是 "no KVM"）。
2. **默认只说结论和下一步**；`-v` 显示步骤；一切细节（含外部工具的输出）在日志里，失败时打印日志最后 20 行和路径。
3. **一个问题一个回答者**：登录 shell、home 格式版本、一个发现项的修复，各自只有一处代码决定。
4. **core 产出结构化结果和事件，前端渲染**（#640 已有原则）；luban CLI、Luban OS、xlings CLI、`--json`、interface 是同一事件流的不同渲染器。
5. **xlings 知道自己每个版本的格式**：已知格式的变化与派生状态由框架自动处理，只把真正需要用户决定的事交给用户。
6. **依赖只向下**：`apps/` → `luban/` → `modules/`；`apps/xlings` 不导入 `luban/`。

---

## 2. 分层与目录

```
modules/            通用：xlings 和 luban 都用，不知道"机器"
  libs runtime platform ui cli store     已有
  home        新：home 的布局、schema、迁移、显式 home 上下文（§4）
  subos       已有；加上 root 的 /etc（sysusers、出厂文件、登录 shell 规则 §7）
  carrier     已有（local、wsl2、vz）；加 vm（§8）
  image       从 luban/image 移来：GPT、FAT、ISO 9660 + El Torito、qcow2——格式本身通用
luban/              Luban OS 的组件与服务：知道"机器"（布局见 §16）
  system      boot、stage0、machine、archive
  service     服务层：环境与机器的生命周期、结构化事件；CLI 与 Luban OS 共用（§5）
  vm          机器运行时：supervisor、终端中继、guest 会话进程（§8）
  desktop     桌面子系统：opensal 与 wire、宿主 backend、imgui 在 opensal 上、终端模拟、opensal-wlr（§9、§14）
apps/               用户直接接触的程序
  luban         luban 命令树与终端呈现 + luban-init（stage-0），静态链接
  luban-os      Luban OS 应用，动态链接（静态 musl 没有 dlopen，打不开窗口）
  gui  xdev
```

移动清单（P11）：

| 现在 | 之后 |
|---|---|
| `luban/src/image` | `modules/image` |
| `luban/src/machine` 中 root 的 `/etc`（sysusers、出厂文件） | `modules/subos/views/root`——root 在 sandbox 里进入也需要 passwd，它属于 root view |
| xlings 二进制兼当 `xlings-init`（`main.cpp` 导入 `luban.stage0`） | 只有 `luban-init`；home 的 `boot/xlings-init` 链接由 §4 的迁移步骤改指向 luban |
| `xlings subos boot`、`subos export --iso/--drive/.img/.qcow2` | `luban boot`、`luban export`；xlings 的 `subos export` 只留目录和 tar |

规则由 lint 保证：`tools/lint_layer_deps.sh` 增加 "`apps/xlings` 与 `src/` 不导入 `luban.*`"、"`modules/` 不新增 `Config::`"。

xlings 已发布的 `subos boot`、`subos export --iso` 被移走后，xlings 对这些参数给出一行拒绝，指向对应的 luban 命令——
这是错误信息，不是兼容代码。

---

## 3. 从 src/core 抽取：显式的 home 上下文

`src/core` 约 6.8 万行，`.cpp` 中有 777 处 `Config::`——几乎每个函数都通过全局单例读 home；`modules/` 的代码显式接收
`HomeView`。所以"抽到 modules"真正的工作是**把隐式的全局读取换成显式传入的上下文**，移动文件是小事。

- `modules/home` 提供 `HomeContext`：路径、home 配置的读取（带 `upgrade-and-compatibility.md` 的故障域：读不了不当作空）、状态锁。
- 被移动的函数接收上下文；`Config` 在过渡期是 `HomeContext` 在 xlings 前端的一个适配器，最终消失。
- **按 luban 需要的顺序抽**：先 SubOS 生命周期操作与 `xim::cmd_install_step`（嵌套安装），再 xim / xvm 其余部分。
- 服务层经一个**端口**（`XlingsPort`）调用 xlings 的操作：本版由 interface（结构化 NDJSON，按需补充 capability）实现，
  抽取完成的操作换成进程内实现——服务层与它的前端都不变。不拼命令行、不解析文本；多个进程同时写 home 由已有的状态锁保证。
- NDJSON 只留给 agent，以及跨 carrier 的调用（宿主 ↔ guest）。

---

## 4. home 格式框架（`modules/home`）

### 4.1 布局版本（实施时的修正）

- 格式的版本是 `.xlings-home` 里的**整数 `layout`**，与发布版本无关。原因：2026.10.9.1 起发布的客户端按它决定能不能写这个 home
  （高于自己认识的就只读）——换一个键会让这些客户端把新 home 当作旧的来写。`schema` 是标记文件本身的文档版本，不是 home 的格式。
- 只有旧客户端写入会破坏的变化才提高 `layout`；只增加旧客户端会忽略的文件和键（本版的 instance.json 新键、`logs/ops/`、`.luban` 归档）不提高。
  本版仍是 layout 2。
- root 部署的标记里 `layout` 是 root 的形状（`single` / `multi`），这是现有的、有意的约定（导出的镜像、前缀域都这样写），本版不改。
- 兼容表（`xlings.home.format`）：2026.10.9.1 之前的客户端不读 layout（照写，看不到新布局带来的东西）；之后的客户端认识到它发布时的 layout。
  每个 layout 记录它带来了什么，切换到旧客户端时据此说明。

### 4.2 迁移注册表

```cpp
struct Step {                                  // xlings.home.format
    std::string_view name;
    std::function<bool(const fs::path& home)> needed;                                    // 只读
    std::function<std::expected<std::string, std::string>(const fs::path& home)> apply;  // 幂等
};
Report run(const fs::path& home, std::span<const Step> steps);   // 每步一个故障域
```

步骤由拥有所需能力的一方注册（需要 Config 的步骤在 xlings 前端注册），运行器只负责排序、隔离与记录；升级阶梯与 `self doctor --fix` 运行同一组步骤。

- 有序、幂等、记录已执行的；一个步骤失败是它自己的故障域（升级阶梯已有的规则），不阻止其他步骤和用户的命令。
- **向前迁移 + 读者宽容 N-1**，不做"按版本把每个操作分派给对应版本的处理函数"——那要求永久保留每种旧格式的读写代码。
  旧格式的读取放在 `COMPAT(… drop in YYYY.M)` 里，至少 6 个月。
- 现有的分散迁移收进来：`main.cpp` 的 shell profile 自愈、旧 home 标记、`ensure_home_layout`、`self migrate`、`boot/xlings-init` 改指向。
- 升级阶梯（`xself::upgrade`）的第 2 步从"重建派生状态"扩展为"执行到当前纪元的迁移 + 重建派生状态"。

### 4.3 切换到旧客户端

`xlings use xlings <old>` 由**当前**客户端在切换前查兼容表：

```
[warn] this home is layout 2; xlings 0.4.40 knows layout 1: it does not see SubOS policies, sessions, roots and
       Luban environments (they stay as they are for a newer client); back: xlings use xlings 2026.10.12.4
```

旧客户端遇到新格式的行为不变（`home_context().writable()`：只读，并给出 home 自己的客户端）。

### 4.4 问题分类：每类有固定的处理

| 类别 | 例子 | 处理 | 说给用户吗 |
|---|---|---|---|
| 格式 / 布局变化 | `layout` 的形状 | 迁移，自动 | 否 |
| 派生状态 | 路由表、generation、索引缓存 | 重建，自动 | 否 |
| 悬空登记 | payload 已不在、无法重装的 `local:` 包 | 自动删除登记（登记是派生数据）、记入日志 | 否 |
| 可重装的缺失 | `xim:` 包的 payload 不在 | 一条命令，按根因（payload）归并 | 是，一行 |
| 检测误报 | `android-emulator` 的程序名不是包名 | 修检测器或配方，不是用户的事 | 否（`self doctor --all` 可见） |
| 需要用户决定 | 删除用户数据、提权 | 才提问；无人可问时 exit 2 + 命令 | 是 |

由此而来的规则（P2–P4）：

- **修复命令必须消除它的发现项**：每一种带修复的 `FindingKind` 有一个测试——在 fixture 上执行修复后，该项消失。
- **按根因归并**：一个 payload 只有一项，不论它登记了多少个名字。
- **基线**：home 里没有 `upgrade` 记录时，阶梯是第一次运行，报出的是"这个 home 里已有的问题"，不是"升级后做不到的事"。
- **doctor 的子进程从不提问**（P1）：以 `XLINGS_AGENT_MODE=1` 运行，需要提权或确认时 exit 2 并给出命令，由外层收集后说一次。

---

## 5. 服务层与呈现层

### 5.1 `luban/service`

环境：new / rm / ls / status / config / upgrade / rollback / history / export / import。机器：start / attach / stop / 状态。
每个操作返回结构化结果，过程中发事件；提问只通过 `Asker` 端口（`xlings.guard`，已有）。CLI 与 desktop 都只调用这一层。

### 5.2 事件与渲染

事件：`phase`（开始 / 完成 / 失败）、`progress`、`artifact`（产物与路径）、`notice`、`error`（带修复命令）、`step`（仅 `-v`）。

| 输出层次 | 内容 |
|---|---|
| 默认 | 每个阶段一行（进行中 ▸，完成 ✓）+ 结论 + 下一步 |
| `-v` | 加上每个步骤 |
| 日志 `logs/<app>/<cmd>-<时间>.log` | 全部，包括外部工具（limine、mkfs、qemu-img）的原始输出 |
| 失败 | 那个阶段的错误 + 修复命令 + 日志最后 20 行和路径（与 hook 日志一致） |

外部工具的输出**一律进日志**，从不直接到终端（`run_tool_` 一类调用改为捕获）。luban 的输出不带 `[xlings]` 前缀，
提示里只出现 luban 的命令。

### 5.3 目标输出

```
$ luban new box --edition tiny
✓ box   tiny 2026.10.11.1 · 4 个包 · 第 1 代
  进入：luban enter box

$ luban enter box --mode machine --temp
✓ 准备  内核 6.12.112 · 镜像 38 MB
▸ 启动  box（2 GB · 网络 NAT）
<shell 提示符>

$ xlings self update                       # 升级后有待办时
xlings 已升级到 2026.10.12.4。这个 home 里有 3 个已有的问题。
  修复：xlings self doctor --fix     详情：xlings self doctor -v
```

默认输出由快照测试固定（luban new / enter / export / import，升级提示）。

---

## 6. luban 命令树（不留兼容）

| 级别 | 命令 | 说明 |
|---|---|---|
| 常用 | `new <name> --edition <E>` | 默认 edition 为 core；`--proxy` |
| 常用 | `enter <name\|镜像路径> [--mode sandbox\|machine] [--temp] [--display terminal\|window]` | 进入环境；参数是镜像路径时等于 `--mode machine --temp` |
| 常用 | `run <name> -- <cmd>`、`ls`、`status [name]` | |
| 更多 | `config <name> [--policy] [--proxy] [--tz] [--shell] [--mode] [--persist] [--desktop] [--on-close] [--allow/--disallow] [--mount/--unmount] [--fetch]` | 全部是选项；隔离相关的与 `xlings subos config` 一一对应，机器相关的（mode、persist、desktop、on-close）是 luban 的 |
| 更多 | `stop <name>` | 停止一台运行中的机器或会话 |
| 更多 | `history`、`upgrade`、`rollback`、`rm`、`setup` | 不变 |
| 更多 | `export <name> <file> [--boot]`、`import <file> [--name]`、`write <source> <device>` | §10 |
| 专家 | `boot …` | 从 xlings 移来 |

删除：`try`、`--nographic`、`new` 的位置参数 edition、luban 的 `--from`、`config` 的位置参数 key / value。

- **能力先是配置**：`luban config box --mode machine` 之后 `luban enter box` 就以机器方式运行；命令行选项只覆盖这一次。
- `enter --mode machine` 是 luban 自己的（机器是 luban 的范畴）；其余命令仍与它运行的 xlings 操作一一对应。
- 随 spec 一起更新：help 级别、补全、`docs/generated/command-reference.md`、`docs/luban/`（中英）、README、`test_luban_cli`。

---

## 7. 登录 shell：环境回答，不是 enter 决定（P9）

- root 环境的答案是它自己的 `/etc/passwd`。
- 创建时的默认值由 edition 决定：edition 声明 `shell` 则用它；否则按 edition 带的包选 fish > bash > sh。写入 root 与默认用户的 passwd 项。
- `luban config box --shell fish` 改 passwd；这是机器状态，升级不改写用户的选择。之后再装 fish **不会**自动切换（与真实发行版一致，不 chsh）。
- 非 root 的 view SubOS 用宿主用户的 `$SHELL`——那本来就是用户自己的 userland。
- sandbox 的 enter、机器里的会话、真机都读 passwd；`-c` 执行一律 `/bin/sh`（fish 不是 POSIX）。
- 实现位置：`modules/subos/views/root`（§2）；`sandbox.cpp` 的 bash/sh 判断、`machine.cpp` 的固定 `/bin/sh` 删除。

---

## 8. 运行方式：sandbox 与 machine

| `--mode` | 内核 | 载体 |
|---|---|---|
| `sandbox`（默认） | 宿主的 | local（bwrap 等）、wsl2、vz（已有） |
| `machine` | 它自己的 | 新的 `vm` carrier：Linux 与 Windows 上 qemu（KVM / WHPX）、macOS 上 vz |

### 8.1 启动环境本身，不是拷贝

现在的 `try` 导出一个 live ISO 在内存里运行，修改全部丢失；`enter` 的语义要求修改保留。机器模式因此**直接启动这个环境**：

- 内核来自 boot profile（virt）；
- 宿主 home 只读映射到 guest 的同一路径（root 的 `/usr` 是指向 payload 的链接，路径必须一致）；
- root 自己的 `/etc`、`/home`、`/var` 可写映射；
- 映射：qemu 用 9p 或 virtiofs（virtiofsd 作为 T1 包），vz 用自带的 virtiofs；guest 的 stage-0 负责挂载。

live 镜像只用于 `export`。

### 8.2 终端：luban 自己中继

```
用户终端 ⇄ luban（raw 模式，同步窗口大小）⇄ virtio-serial（unix socket）⇄ guest 中的 xlings 会话进程 ⇄ pty ⇄ 登录 shell
```

- 协议是 broker 与 carrier 已用的终端 / 控制 NDJSON（`relay_terminal`，每 100 ms 同步窗口大小），所以 sandbox、WSL2、vz、qemu 是同一套终端协议。
- virtio-serial 而非 vsock：不需要 `/dev/vhost-vsock` 的权限，Windows 上的 qemu 也可用。
- 内核 cmdline 加 `quiet`；串口全文写入 `logs/machines/<name>/console.log`，`-v` 时显示。
- 退出键是 luban 自己的，所有载体一致；shell 退出即会话结束。
- edition 的 inittab（tiny 的 `askfirst:-/bin/sh -l`）改为启动会话进程；这需要发 edition 的新日期版本（已发布的 manifest 不改）。

### 8.3 每台机器一个 supervisor

- 运行中的机器是一个会话，载体为 vm；生命周期复用已有的会话模型（`subos start/stop`、`--keep`、`--ttl`）。
- `run/machines/<name>/` 下是 `console`、`display`、`control` 三个 socket；一台机器可以同时有多个终端和一个窗口连接。
- 关窗口或离开终端只是断开；关机是 `luban stop` 或 guest 内关机。
- 下载之前先检查宿主（已有原则）：硬件虚拟化、qemu-system（T1）、固件；缺什么用 OS 的话说，并给出 `xlings install …`。

## 9. 显示、桌面与音频

### 9.1 画面从 guest 来，经 opensal 到宿主窗口

guest 里的桌面画在一个 opensal surface 上；这个 surface 的**提供端在宿主**：Luban OS 窗口。两端之间是
opensal 的跨地址空间传输（opensal wire，§15 的 X 层）；Luban 只提供它走的通道（unix socket、virtio-serial），不定义协议。

```
guest：  桌面（labwc 经 bridge，或 xdesk）── opensal wire 消费端 ──┐
         通道：sandbox 是 unix socket，machine 是 virtio-serial      │
host：   Luban OS 窗口 ── opensal wire 提供端 ── 宿主 backend（x11 / wayland / win32 / cocoa）
```

- 同一个桌面在 sandbox 与 machine 两种运行方式里一样，Luban OS 窗口不知道下面是哪一种。
- 开机阶段显示 Luban OS 自己的启动画面（§14.3）。
- 窗口大小与缩放 → `opensal.mode` / `opensal.density`；Host 键与键盘锁定 → `opensal.capture`；
  宿主输入法 → `opensal.text`；宿主剪贴板 → `opensal.clipboard`（受环境的 clipboard 授权约束）。

### 9.2 opensal 在本版需要的接口

只实现现在用到的（§16）：

| 接口 | 用途 |
|---|---|
| `opensal.surface`（核心） | 像素与节奏（帧完成） |
| `opensal.pointer`、`opensal.keyboard` | 输入 |
| `opensal.mode`、`opensal.density` | 窗口大小 → guest 分辨率；高分屏 |
| `opensal.capture` | 聚焦时收下全部按键（含宿主的组合键），Host 键释放 |
| `opensal.cursor` | 光标形状 |
| `opensal.text` | 宿主输入法提交的文字与预编辑，消费端回报光标位置 |
| `opensal.clipboard` | 宿主剪贴板 |
| opensal wire | 以上接口跨地址空间：帧格式、接口协商（缺什么可以由一次操作得知）、脏区域、压缩 |

### 9.3 官方桌面两条线

| edition | 桌面 | 接入 | 状态 |
|---|---|---|---|
| `desktop` | 传统层叠桌面：labwc + 面板 + 启动器 + 终端 | `opensal-wlr`：guest 内的桥，用 wlroots 的标准协议取画面（screencopy）、注入输入（virtual-keyboard / virtual-pointer）、提交文字（input-method）、同步剪贴板（data-control）、调整输出（output-management），对外是 opensal wire 消费端 | 正式线 |
| `desktop-xdesk` | xdesk | 原生 opensal 消费端 | 实验线（preview） |

- 两条线同一个传输、同一个 Luban OS 窗口，输入法、剪贴板、分辨率行为一致。
- 用户在 nano / tiny / core 上自己装桌面：任何实现了上述 wlroots 协议的合成器经 `opensal-wlr` 接入。
- 桌面会话的声明用 freedesktop 的 `wayland-sessions/*.desktop`，加一个键 `X-Opensal-Interfaces=`（它用到哪些
  opensal 接口）；edition 指定默认会话，`luban config <n> --desktop <会话|none>` 切换。
- 实验线升为默认的门槛：真实程序的兼容矩阵、opensal 一致性测试、帧延迟预算、aarch64、CN 镜像。

### 9.4 音频（简单版本，现成标准）

- machine：virtio-snd（qemu `-audiodev` 接宿主的 PipeWire / Pulse / CoreAudio / Windows 音频；内核 `SND_VIRTIO`）。
- sandbox：`audio` 授权，映射宿主 PipeWire / Pulse 的 socket（能力先是配置）。
- 不写新协议；Luban OS 需要按系统控制音量时，再按 §15 判断。

### 9.5 GPU

软件渲染是基线（所有载体都能跑）；sandbox 有 GPU 授权时用 render node，machine 里 virtio-gpu。画面一致，只有性能差别。

---

## 10. persist 与 export / import

### 10.1 persist

| `persist` | 每次启动 |
|---|---|
| `all`（默认） | 修改全部保留 |
| `home` | 系统部分恢复，`/home` 保留 |
| `none` | 全部恢复（kiosk、冰点还原） |

- 基线 = 一个 generation + 机器状态（`/etc`、`/var`、需要时 `/home`）的快照；每次启动在上面叠一层临时可写层，停止时丢弃。
- 基线只通过明确的操作改变：从环境外装包（新 generation）、`luban upgrade`、`luban config`。临时模式里的修改按设计丢失。
- sandbox 由会话初始化做 overlay（root 只读 + tmpfs 上层），machine 由 guest 的 stage-0 做同样的 overlay——与载体无关。
- `luban config box --persist none` 是长期设置；`enter --temp` 是这一次按 `none`。

### 10.2 export / import

| 产物 | 内容 | 用途 |
|---|---|---|
| 环境归档 `box.luban` | `manifest.json`（edition、包坐标 + sha256、policy、`min_client`）、`state.tar.zst`（`/etc`、`/home`、`/var`，不含 persona）、可选 `payloads/`（`--with-payloads`，离线用） | `luban import` |
| 可启动镜像 iso / img / qcow2 | 内含 `/luban/manifest.json` | 启动、写盘；也可 `luban import box.iso` |
| rootfs tar.zst | 文件树 | docker、WSL |

`luban import <file> [--name <n>]`：

- policy 不被悄悄放宽：比默认宽时先显示再问（与"升级从不放宽隔离"同一原则）；
- persona 不复制：生成新的主机名与 machine-id（与 fork 的规则一致）；
- payload 按 sha256 校验；未附带的从 index 安装同一版本，index 没有时说出是哪一个；
- 名字已存在时拒绝并给出 `--name`。

---

## 11. 任务与依赖

xlings 一个 PR，每个 checkpoint 一个提交：能构建、它的测试通过、在本表里标明对应的节。跨仓库的 PR 见 §17。

```
C1 升级与 doctor ─────────────┐
C2 modules/home ──► C3 分层 ──┼──► C5 服务层 + 命令树 ──► C6 机器模式 ──► C8 Luban OS 应用 ──► C9 guest 桌面
C4 可观测性 ──────────────────┘                    ▲              │                ▲
C7 桌面子系统基础（opensal、backends、ui、term）─────┴──────────────┼────────────────┘
                                                                   └──► C10 音频
C11 文档与规范、C12 CI、C13 版本与发布：贯穿，最后收口
```

| # | 内容 | 依赖 | 主要视角 | 测试 |
|---|---|---|---|---|
| C1 | P1–P5：doctor 子进程不提问（agent 模式 + 前台）；修复必须消除发现项；按根因归并；基线；三行升级提示 + `-v` + 日志 | — | 稳定性、体验 | 每种带修复的发现项"修复后消失"；后台进程组的子进程不读终端；升级提示快照 |
| C2 | `modules/home`：schema 纪元、迁移注册表、`HomeContext`、兼容表、问题分类；`layout` 拆为 `mode` + `root_layout`；现有自愈收编 | — | 无感升级、兼容 | 每个迁移的 check / apply / 幂等；旧 home 升级；降级切换前的提示 |
| C3 | 分层：`modules/image`、root 的 `/etc` 进 `modules/subos`、stage-0 只在 luban、`subos boot` 与可启动导出移到 luban；lint | C2 | 架构、一致性 | `lint_layer_deps` 新规则；镜像启动测试不变通过 |
| C4 | 可观测性：trace id 贯穿进程、span、按操作的日志、外部工具输出进日志、`xlings self log` | — | 可观测、隐私 | 跨子进程的 trace 还原；`--bundle` 去除 home 路径 |
| C5 | `luban/service` + 呈现层 + 命令树（§6）+ 登录 shell（§7）+ `.luban` 归档 + sandbox 的 persist + on-close 配置 | C3、C4 | 体验、一致性 | 命令树快照、help 级别、export → import 往返、persist 三档、默认输出快照 |
| C6 | 机器模式：`vm` carrier（qemu）、9p 映射环境本身、guest 会话进程与终端中继、supervisor、machine 的 persist、主机检查 | C3、C5 | 稳定性、跨平台 | qemu 下 进入 → 修改 → 重进仍在；`--temp` 不留；窗口大小同步；没有 KVM 时的提示 |
| C7 | `luban/desktop`：opensal（§9.2）与 wire、宿主 backend、mem、imgui 在 opensal 上、终端模拟 | — | 架构、跨平台 | 一致性：同一消费者经 mem 与经 wire 像素相同（带反向对照）；终端序列测试 |
| C8 | `apps/luban-os`：窗口即显示器、启动器、快速开始、启动画面、控制台、关窗口、Host 键与标题栏、管理面板、多实例、桌面入口 | C5、C6、C7 | 用户体验、视觉 | mem backend 下的界面快照；快速开始的完整流程；关窗口三种选择 |
| C9 | guest 桌面：`opensal-wlr`、xdesk 实验版、两个 edition | C6、C7 | 生态、一致性 | 桥的一致性测试；两个 edition 各启动一次 |
| C10 | 音频：virtio-snd、`audio` 授权 | C6 | 跨平台 | 授权映射；qemu 参数 |
| C11 | 文档与规范：docs/luban（中英）、docs/specs、命令参考、AGENTS.md 的规则、README | 全部 | 一致性 | `tests/scripts/test_docs.py` |
| C12 | CI：新 lint、新测试、host matrix 场景 | 全部 | 覆盖度 | 三平台 CI 全绿 |
| C13 | 版本号三处（mcpp.toml、config.cppm、luban kVersion）、发布、CN 镜像、index | 全部 | 发布 | fresh-install、真实升级 |

收口的规则：一个 checkpoint 只有在它的测试通过后才提交；PR 合并要求全部 checkpoint 完成、CI 全绿、自我 review 通过。

### 11.1 实施时的决定（2026-10-13）

- **C9 的桥在 `luban/desktop/wlr`（opensal-wlr）**：自己实现 Wayland wire（guest 里的 luban 是静态的，不带 libwayland），只用
  screencopy、virtual-keyboard（evdev 键码、US keymap，由合成器用它自己的 xkeyboard-config 编译）、virtual-pointer、
  output-management。会话按 freedesktop 声明（`share/wayland-sessions/<id>.desktop` + `X-Opensal-Interfaces=`），
  headless 后端 + pixman 软件渲染。机器里由 agent 在一个通道上桥接；沙箱里是 `xlings subos use --cmd 'luban __desktop'`
  的两条管道——窗口两边都是 opensal.wire 的提供端。opensal.wire 增加了 `s`（消费端实际绘制的大小）。
- **桌面栈是一个可重定位的包 `labwc-desktop`**：资源工厂把 Alpine 3.20 的构建（labwc、yambar、fuzzel、foot、swaybg、grim、
  Xwayland、字体）连同 musl loader 与依赖库打成一棵树；库放在 `libexec/lib`（根会把 payload 的 `lib/` 链进 `/usr/lib`），
  并把每个 ELF 的 PT_INTERP 改成 PT_NULL（根检查每个 ELF 的 loader 在闭包内；这些程序只由树自己的 loader 运行）。
  工厂在 Debian 12 里跑一个 headless 会话，画出背景与面板才算构建成功。
- **edition 的默认设置**是根里的 `usr/share/luban/settings.json`（`mode`、`persist`、`desktop`、`on_close`），用户设置的优先，
  随升级更新；desktop edition 默认以机器运行并启动 labwc-desktop。
- **机器的 `/run`、`/tmp` 是 tmpfs**，在挂 home 之前挂（home 可能就在 `/tmp` 下）：否则会话留下的 socket 归宿主用户所有，
  下一个会话被拒绝。
- **luban-os 的发布形态**：glibc + X11（SDL 直接链接 X11）的普通 payload，loader 用系统路径、无 RPATH，`xim:luban-os`
  依赖索引的 glibc 与 X11 库，安装时由 elfpatch 接上——与其他 glibc 包相同。索引的 X11 库只有 x86_64，所以本版 luban-os 只有
  x86_64；aarch64 上桌面本身可以运行（机器、桥、`luban __desktop`），窗口待 X11 库有 aarch64 版本后再发布。
- **实验线 `desktop-xdesk` 不在本版**（§12）：本版建立了它需要的接入点——会话声明、`X-Opensal-Interfaces`、opensal.wire 通道；
  原生 opensal 消费端的桌面另立版本。
- **CI 的"被测索引"此前没有生效**：`self install` 同步过的索引目录在改 `index_repos` 之后仍然留着，工具一直来自已发布的索引
  （aarch64 因此拿到没有 9p 的 qemu r1）。`tools/ci/xlings_tools.sh` 与镜像测试的 home 现在删掉它再 `xlings update`，并核对
  正在用的就是被测索引。客户端本身"改了默认索引的 url 却继续用旧目录"是另一个问题，记在这里，不在本版修。

---

## 12. 待定与风险

- P1 中子 subos 为什么需要提权：C1 中复现后决定去掉该步骤的提权还是只变成待办。
- 9p 的性能；以后需要时换 virtiofs（virtiofsd 作为 T1 包，内核要 `FUSE_FS` + `VIRTIO_FS`）。
- Windows 上的机器模式：qemu + WHPX 的性能。
- opensal wire 在 virtio-serial 上的带宽：脏区域 + 压缩；同机（sandbox）用共享内存。
- 宿主 backend 的多窗口、换显示器时的缩放、Wayland 上的键盘锁定（keyboard-shortcuts-inhibit）、macOS 不拦截 Cmd+Tab。
- imgui 不支持屏幕阅读器：完整的键盘导航与对比度，文档中写明。
- 抽取的规模（`src/core` 中 777 处 `Config::`）：本 PR 的服务层经一个端口调用 xlings（§5.1），端口的进程内实现随抽取替换。
- xlings 移走 `subos boot`、`subos export --iso`：发布说明写明；xlings 对这些参数给出一行指向 luban 的拒绝。
- 不在本版：RFB 兼容档（非 wlroots 系的第三方桌面）、opends 的 agent 桌面操作、xdesk 在真机上原生运行。

---

## 13. 可观测性

一个事件模型，同时用于显示、日志、调试与评估。

1. **trace id 向下传递**：顶层命令生成，经环境变量 `XLINGS_TRACE_ID` 传给子进程、hook、会话，经 carrier 的 control 通道进 guest，interface 请求带同名字段。
2. **span + event**：`phase` 是 span（开始、结束、耗时、结果），其余是 event；§5 的呈现事件就是它们，屏幕显示是按级别过滤后的结果。
3. **按操作的日志** `logs/ops/<日期>/<trace-id>.ndjson`：每条带客户端版本、home schema、平台、参数、环境变量的名字（不记值）；外部工具的原始输出放在旁边的文件里，由日志引用。upgrade、destructive、elevation 三个日志保留，带 trace id。
4. **机器的日志** `logs/machines/<name>/`：串口、supervisor、guest 会话事件，同一个 trace id。
5. **操作者**：`actor` 取 person（座位上的人）、cli、agent、program、system。
6. **保留**：日志是派生数据，按大小与时间轮转（已有的 journal 轮转）。
7. **调试**：`XLINGS_TRACE=<分类>` 写入同一事件流，开启时也到 stderr。
8. **评估**：性能预算（`test_home_perf`）、host matrix、`xdev report` 读同一份 span；"点击到 shell 出现" 这类体验指标也从这里来。
9. **查看与报告**：`xlings self log [trace-id] [--bundle]`（`self` 下，不新增顶层命令），`luban status <n> --log`，Luban OS 管理面板的 "活动" 页；`--bundle` 按隐私规则去除 home 路径与用户名。日志只在本地。

---

## 14. Luban OS 应用

### 14.1 窗口即显示器

- 一个窗口只显示一个系统：控制台型整个窗口是它的控制台，桌面型整个窗口是它的桌面；没有标签页、侧栏、工具栏。
- 窗口大小即分辨率：控制台的行列、桌面的分辨率随窗口变化。标题：`<名字> — Luban OS`。
- 可以开多个窗口，每个是独立进程；系统由 supervisor 运行，窗口崩溃不影响系统。
- 两个窗口选同一个系统：控制台型开一个新的登录会话（像切到 tty2）；桌面型把已打开的窗口调到前面。
- 系统里关机后：窗口显示 "已关机"，Enter 重新启动，Esc 回到列表。

### 14.2 启动器

```
┌──────────────────────────────────────────────┐
│                                            ⚙ │
│                   Luban OS                   │
│                                              │
│        ▸ box        core                 ●   │
│          agent      agent-workspace          │
│          studio     desktop                  │
│          ＋ 新系统                            │
│                                              │
│               8 秒后进入 box                   │
└──────────────────────────────────────────────┘
```

- 只有一个列表；⚙ 或 F2 进管理面板；● 表示运行中；"＋ 新系统" 是列表最后一行。
- 倒计时 10 秒（可设置，0 为不自动进入），任何鼠标移动或按键取消。
- 顺序：用户设定的默认系统在第一位，没有设定时按最近使用。
- 已经运行的系统：选中即连上。主机条件不满足时：检查清单，"设置" / "安装" 按钮，提权弹系统自带的框。

### 14.3 快速开始与进度

- 没有任何系统时：nano、tiny、core、desktop 四张卡片，说明与大小来自 index 中 edition 的 `title` / `summary` / `size`。
- 名字自动生成（`core-1`），"更多选项" 收起：shell、运行方式、保留修改、代理。
- 点击后同一窗口显示进度：检查主机 → 下载 → 安装 → 生成系统 → 启动；进度条按阶段加权（下载按字节，安装按包数）；
  "详情" 是这次操作日志的实时滚动；"取消" 不留下半个系统（`new` 是事务性的）。完成后直接进入。
- 启动画面：超过约 300 ms 才显示；logo、系统名、阶段；Esc 显示控制台日志。

### 14.4 关窗口

```
┌ 关闭 box ────────────────────────────────────┐
│ box 里正在运行：mcpp build                    │
│   ◉ 在后台继续运行                            │
│   ○ 关机                                     │
│        [取消]   [确认]   [确认并设为默认]       │
└──────────────────────────────────────────────┘
```

- 每次都问；"确认并设为默认" 写入**该环境**的配置：`luban config <n> --on-close ask|background|poweroff`。
- 临时环境的关机写作 "关机（修改将丢弃）"。已设默认时第一次这样关，发一次系统通知。
- 宿主注销或关机：supervisor 收到 SIGTERM 让 guest 正常关机，超时后再强制。

### 14.5 Host 键与标题栏

- 单独按下再松开一组修饰键（Linux / Windows `Ctrl+Alt`，macOS `Ctrl+Option`，可设置）：键盘被锁定时释放，否则打开 Host 菜单（回到列表、管理、全屏、新会话、关机）。
- 标题栏始终写着现在按它会发生什么：`box — Luban OS · Ctrl+Alt 菜单` / `box — Luban OS · Ctrl+Alt 释放键盘`。
- 全屏没有标题栏：进入全屏的头 3 秒叠加提示，鼠标到顶边滑出细工具条。
- 控制台模式不锁定键盘；桌面模式点进窗口后锁定。

### 14.6 管理面板与界面技术

- 独立窗口、全局一个；功能：系统列表与状态、启动停止、设置、历史与回滚、软件、隔离、存储、日志、新建、导入导出、写盘、主机、活动（span 树）、应用设置。
- 每个操作显示 "等价命令"，可复制；没有 CLI 做不到的管理功能。
- 多个系统的控制台并排、分离窗口只在这里（imgui docking）。
- 界面：imgui 经 SDL 渲染器绘制，窗口、输入、输入法、剪贴板、密度来自宿主提供端（`luban/desktop/host`，SDL3）；事件驱动重绘（空闲时 CPU 接近 0）；
  测试用 SDL 的 offscreen 驱动无头渲染并截图；Linux 上本版输入法组合（IBus / fcitx）不可用（`compat.sdl3` 未带 dbus），Windows、macOS 可用；
  深色默认、浅色可选，颜色取 `ui/theme`；界面字体与等宽字体都带中文；高分屏按显示器缩放。

### 14.7 安装与入口

`xlings install luban-os` 安装应用；安装器按 freedesktop Desktop Entry（Linux）、`.app`（macOS）、开始菜单（Windows）生成入口与图标，卸载时删除。CLI 的 `luban enter <n> --display window` 启动 `luban-os --attach <n>`。

---

## 15. 规范放在哪里

按顺序判断：

1. 已有现成标准 → 直接用（virtio-snd、freedesktop 的会话与桌面入口、PipeWire / Pulse、Wayland 协议）。
2. 属于桌面与显示（surface、输入、文字、剪贴板、窗口、桌面语义）→ 桌面子系统的规范，`luban/desktop/spec`（openxx 一族），
   一个共享的能力放在它的所有提供者都够得着的那一份里。
3. 不属于桌面、只有 Luban 用 → Luban 自己的格式，写在 `docs/specs/`（终端中继、`.luban` 归档、home schema、操作日志）；
   以后有第二个实现者时再提升。

opensal 的边界分三层：S（构建时选定）、L（加载时、同一地址空间）、X（另一个地址空间或权限级别）。核心接口都能跨 X；
X 部署经 opensal wire 转发，而不是经提供者的函数表。

---

## 16. `luban/` 的布局与包划分

一个包（一个 `mcpp.toml`）对应三件事一起变化的边界：依赖的系统库、被哪一类二进制链接（静态 CLI、动态 GUI、guest 程序）、以后要不要一起拆出去。包内按职责分模块与分区；不会一个文件一个包。

```
luban/
├── system/      luban-system：boot、stage0、machine、archive          静态
├── service/     luban-service：环境与机器的生命周期、事件、呈现事件    静态
├── vm/          luban-vm：supervisor、终端中继、通道、guest 会话进程    静态
└── desktop/     桌面子系统（以后可整体拆出）
    ├── spec/        opensal（含 wire）
    ├── backends/    host/（x11、wayland、win32、cocoa）、virtual/（mem、wire）   按依赖分包
    ├── ui/          imgui 在 opensal 上：光栅化、输入、字体
    ├── term/        终端模拟：VT 解析、屏幕、选择
    └── bridge/      opensal-wlr（guest 内）
apps/
├── luban        静态：CLI + luban-init；不链接 desktop
├── luban-os     动态：service + vm 客户端 + desktop
```

规则（lint）：

- `luban/desktop/**` 只依赖 `modules/` 与它自己，不导入 `luban.system` / `luban.service` / `luban.vm`；把它们接起来的只有 `apps/luban-os`。
- 系统头文件：`modules/platform` 之外，只有 `luban/desktop/backends` 与 `luban/desktop/bridge`（图形一侧的平台边界）。
- `apps/luban` 不链接 `luban/desktop`；`src/` 与 `apps/xlings` 不导入 `luban.*`。
- 基础能力（日志、进程、pty、配置）只有一套：`modules/`。
- 桌面子系统按需新写：只放现在需要的接口，代码、注释、文档自成一体，不引用任何内部探索项目。

---

## 17. 跨仓库与联调

| 仓库 | 内容 | 与 xlings 的先后 |
|---|---|---|
| xlings | 本文全部（一个 PR） | — |
| xlings-res/linux-kernel-virt | 新版本：`OVERLAY_FS`（persist）、`SND_VIRTIO`（音频）；boot profile 加 `quiet` | 先于 C6 的验证 |
| xlings-res（桌面栈） | wlroots、labwc、foot、fuzzel 等的构建，两个架构 | 先于 C9 的验证 |
| xim-pkgindex（一个 PR） | 内核与 boot profile 新版本；edition 新日期版本（`title/summary/size`、`shell`、`display`、会话进程）；`desktop` 与 `desktop-xdesk`；桌面栈的包；`luban-os` 包与桌面入口；PipeWire；xlings 的新版本与 `latest` | 包可先合；依赖新客户端的 edition 带 `min_client`，在 xlings 发布后合 |
| CN 镜像 | 新的大文件（内核、桌面栈、字体、luban-os） | 发布当天，本地 `gtc` |

联调顺序：内核与桌面栈的构建 → index PR（包）→ xlings PR 在 CI 中用 index PR 的分支验证 → xlings 发布与镜像 → index PR 合入（edition、`latest`）→ fresh-install 与真实升级验证。

---

## 18. 多视角检查

每个 checkpoint 提交前按下表过一遍，结果写在提交说明里。

| 视角 | 要问的 |
|---|---|
| 第一次使用的人 | 每一步有反馈吗？出错时知道下一步吗？ |
| 日常开发者 | 快吗？会打断工作吗？修改会丢吗？ |
| agent | 没有终端、不能提问时，每个操作完成或给出结构化的错误吗？ |
| 发行版作者 | edition 能声明 shell、桌面、persist 的默认值吗？已发布的 manifest 不变吗？ |
| 安全 | 隔离会被悄悄放宽吗？提权走一个入口吗（GUI 里是系统自带的提权框）？import 的 policy 被检查了吗？ |
| 平台 | Linux / macOS / Windows，x86_64 / aarch64，有无 GPU，Wayland / X11 |
| 网络 | CN 镜像、离线、只有代理 |
| 规模 | 大 home、同时运行多个系统 |
| 故障 | 一个系统、一个文件坏了，影响会扩散吗？ |
| 升级 | 旧 home、旧客户端、降级 |
| 可及性 | 只用键盘、中英文、对比度、中文输入法 |
| 可观测 | 能从日志还原发生了什么吗？ |

