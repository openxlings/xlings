# Luban OS：分层、home 格式框架、呈现层、运行方式与 desktop —— 总体设计

日期：2026-10-12　基线：2026.10.12.3　状态：设计 checkpoint，待 review

来源：在一个长期使用的 home（398 个 payload）上从旧客户端升级到 2026.10.12.3 的完整记录（`self doctor --fix`、
升级提示），以及按 README 第一次体验 Luban（`luban new`、`luban try`）的记录。本文把这些问题和围绕它们的讨论
收成一个整体方案：先修能立即修的，再把结构理顺，让同类问题不再出现。

**已确认的决定**：

1. 依赖方向反转：xlings 不再导入 `luban.*`；`xlings subos boot`、可启动镜像的导出（iso / img / qcow2 / drive）移到 luban。
2. Luban 目前没有用户：命令树一次改到位，不留兼容写法。
3. 目录按职责三分：`modules/` 通用库，`luban/` Luban OS 的组件与服务，`apps/` 用户直接接触的程序。

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
4. **core 产出结构化结果和事件，前端渲染**（#640 已有原则）；luban CLI、luban desktop、xlings CLI、`--json`、interface 是同一事件流的不同渲染器。
5. **xlings 知道自己每个版本的格式**：已知格式的变化与派生状态由框架自动处理，只把真正需要用户决定的事交给用户。
6. **依赖只向下**：`apps/` → `luban/` → `modules/`；`apps/xlings` 不导入 `luban/`。

---

## 2. 分层与目录

```
modules/            通用：xlings 和 luban 都用，不知道"机器"
  libs runtime platform ui cli store     已有
  home        新：home 的布局、schema、迁移、显式 home 上下文（§4）
  pkg         从 src/core/xim + xvm 抽出：解析、安装、版本、注册（§3）
  subos       已有；加上从 src/core/subos 抽出的操作（new / rm / config / upgrade / rollback / generation）
              和 root 的 /etc（sysusers、出厂文件、登录 shell 规则 §7）
  carrier     已有（local、wsl2、vz）；加 vm（§8）
  image       从 luban/image 移来：GPT、FAT、ISO 9660 + El Torito、qcow2——格式本身通用
  interface   NDJSON 协议的类型与客户端（服务端仍在 xlings）
  ui-gui      从 apps/gui 抽出：imgui 的主题与组件
luban/              Luban OS 的组件与服务：知道"机器"
  boot stage0 machine        已有（machine 只留机器才有的部分）
  vm          机器运行时：每台机器一个 supervisor、控制台中继、显示通道（§8、§9）
  archive     环境归档 .luban 的格式（§10）
  service     服务层：环境与机器的生命周期、结构化事件；CLI 与 desktop 共用（§5）
apps/               用户直接接触的程序
  xlings        现在的 src/，最终只留前端
  luban         luban 命令树与终端呈现 + luban-init（stage-0），静态链接
  luban-desktop 动态链接（静态 musl 没有 dlopen，打不开窗口——apps/gui/mcpp.toml 已说明）
  xlings-gui  xdev
```

移动清单（P11）：

| 现在 | 之后 |
|---|---|
| `luban/src/image` | `modules/image` |
| `luban/src/machine` 中 root 的 `/etc`（sysusers、出厂文件） | `modules/subos/views/root`——root 在 sandbox 里进入也需要 passwd，它属于 root view |
| xlings 二进制兼当 `xlings-init`（`main.cpp` 导入 `luban.stage0`） | 只有 `luban-init`；home 的 `boot/xlings-init` 链接由 §4 的迁移步骤改指向 luban |
| `xlings subos boot`、`subos export --iso/--drive/.img/.qcow2` | `luban boot`、`luban export`；xlings 的 `subos export` 只留目录和 tar |
| `apps/gui`（库的一半） | `modules/ui-gui` + `apps/xlings-gui` |

规则由 lint 保证：`tools/lint_layer_deps.sh` 增加 "`apps/xlings` 与 `src/` 不导入 `luban.*`"、"`modules/` 不新增 `Config::`"。

xlings 已发布的 `subos boot`、`subos export --iso` 被移走后，xlings 对这些参数给出一行拒绝，指向对应的 luban 命令——
这是错误信息，不是兼容代码。

---

## 3. 从 src/core 抽取：显式的 home 上下文

`src/core` 约 6.8 万行，`.cpp` 中有 777 处 `Config::`——几乎每个函数都通过全局单例读 home；`modules/` 的代码显式接收
`HomeView`。所以"抽到 modules"真正的工作是**把隐式的全局读取换成显式传入的上下文**，移动文件是小事。

- `modules/home` 提供 `HomeContext`：路径、home 配置的读取（带 `upgrade-and-compatibility.md` 的故障域：读不了不当作空）、状态锁。
- 被移动的函数接收上下文；`Config` 在过渡期是 `HomeContext` 在 xlings 前端的一个适配器，最终消失。
- **按 luban 需要的顺序抽**：先 SubOS 生命周期操作与 `xim::cmd_install_step`（嵌套安装，§5 服务层要用），再 xim / xvm 其余部分进 `modules/pkg`。
- 抽出后 luban 服务层**进程内调用**，不再拼命令行、不解析文本；多个进程同时写 home 由已有的状态锁保证（与多个 xlings 并发相同）。
- NDJSON 只留给 agent，以及跨 carrier 的调用（宿主 ↔ guest）。

---

## 4. home 格式框架（`modules/home`）

### 4.1 schema 纪元

- 格式的版本是 `.xlings-home` 里的整数 `schema`（已有，值为 1），**与发布版本无关**。每次不兼容的布局或格式变化 +1。
- 拆掉被重载的 `layout`：部署形态是 `mode`（user / root）+ `root_layout`（single / multi），由迁移步骤改写。
- 发布版本只用来回答"哪个客户端认识哪个 schema"：兼容表 `client → max_schema`，随客户端发布。

### 4.2 迁移注册表

```cpp
struct Migration {
    int from, to;                              // schema 纪元
    std::string_view name;
    Applies (*check)(const HomeContext&);      // 只读：这个 home 需要吗
    Result  (*apply)(HomeContext&);            // 幂等；结果写入 upgrade 记录与 logs/upgrade.ndjson
};
```

- 有序、幂等、记录已执行的；一个步骤失败是它自己的故障域（升级阶梯已有的规则），不阻止其他步骤和用户的命令。
- **向前迁移 + 读者宽容 N-1**，不做"按版本把每个操作分派给对应版本的处理函数"——那要求永久保留每种旧格式的读写代码。
  旧格式的读取放在 `COMPAT(… drop in YYYY.M)` 里，至少 6 个月。
- 现有的分散迁移收进来：`main.cpp` 的 shell profile 自愈、旧 home 标记、`ensure_home_layout`、`self migrate`、`boot/xlings-init` 改指向。
- 升级阶梯（`xself::upgrade`）的第 2 步从"重建派生状态"扩展为"执行到当前纪元的迁移 + 重建派生状态"。

### 4.3 切换到旧客户端

`xlings use xlings <old>` 由**当前**客户端在切换前查兼容表：

```
这个 home 的格式是 3，xlings 0.4.40 只认识到 1：它在这个 home 上只读，root、edition 升级等功能不可用。
继续：xlings use xlings 0.4.40 -y
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
| 更多 | `config <name> [--policy] [--proxy] [--tz] [--shell] [--mode] [--persist] [--allow/--disallow] [--mount/--unmount] [--fetch]` | 全部是选项，与 `xlings subos config` 一一对应 |
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

### 8.4 在 vm carrier 之前（P6 的过渡）

先发布的 CLI 中，`--mode machine --temp` 由现有的 live 镜像路径实现，控制台按 §8.2 由 luban 中继（第一版可直接中继串口）；
`--mode machine` 不带 `--temp` 时明确拒绝并说明"保留修改的机器模式尚未提供"，而不是悄悄变成临时的。

---

## 9. 显示与 luban desktop

### 9.1 帧从 guest 来

| 做法 | 一致性 | 取舍 |
|---|---|---|
| 各载体自己的窗口（qemu gtk、VZVirtualMachineView、WSLg） | 差 | 正是要避免的 |
| **luban 自己的窗口 + guest 内输出帧** | 好 | 采用 |
| 每个应用一个宿主原生窗口 | 最好 | 以后再评估（macOS / Windows 上要自建 Wayland 桥） |

- 桌面 edition 在 guest 里运行合成器，以 RFB（VNC）协议经 virtio-serial / vsock 输出（wayvnc 或 weston 的 VNC 后端）。
  显示协议在 guest 一侧（OS 这一侧，归 luban 管），所以 qemu、vz、WSL2、网络上的真机看起来一样。
- 开机阶段显示 luban 自己的启动画面；需要看固件和 limine 时（调试），qemu 载体用 `-vnc unix:` 接到同一个窗口（`-v`）。
- 每台运行中的机器一个窗口；窗口外框（名字、状态）由 luban 画，多个窗口一致。
- RFB 先行（客户端在进程内一两千行）；RDP（音频、剪贴板、带宽）以后再换。剪贴板走已有的 clipboard 授权通道。
  GPU 加速各载体不同（virgl / venus、vz 半虚拟化 GPU、dxgkrnl），画面一致，只有性能差别。
- `--display` 的默认值：edition 声明了桌面且宿主有显示环境 → window；否则 terminal。

### 9.2 desktop 是服务层的 GUI

- `apps/luban-desktop`：GLFW + imgui（`modules/ui-gui`）+ `luban/service`；index 中的 T1 包 `luban-desktop`。
- 功能：环境列表、新建 / 配置 / 升级 / 回滚 / 导出 / 导入、启动 / 停止、状态、每台机器的窗口。
- **规则：desktop 没有 CLI 或 interface 做不到的管理功能**；它独有的只是"显示机器的画面"。agent 与命令行因此始终完整，测试针对服务层。
- CLI 需要窗口时启动 `luban-desktop --attach <name>`；没装时提示 `xlings install luban-desktop`。

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

## 11. 分阶段

| 阶段 | 内容 | 可单独发布 | 测试要点 |
|---|---|---|---|
| 0 | 本文 | — | — |
| 1 | P1–P5：doctor 子进程不提问、修复必须消除发现项、按根因归并、基线、三行升级提示 + `-v` + 日志 | 是 | 每种带修复的 FindingKind 的"修复后消失"；子进程在后台进程组不读终端；升级提示快照 |
| 2 | `modules/home`：schema 纪元、迁移注册表、`HomeContext`、兼容表、问题分类；现有迁移收编 | 是 | 每个迁移的 check / apply / 幂等；旧 home fixture 升级（host matrix `oldhome`） |
| 3 | 分层：image → modules、root 的 `/etc` → subos、stage-0 / boot / 可启动 export → luban、ui-gui；lint | 是 | `lint_layer_deps` 新规则；luban_image_test 不变通过 |
| 4 | 抽取：SubOS 生命周期操作与 install step 进 modules，接收上下文 | 是 | 现有 subos / install 测试不变通过 |
| 5 | `luban/service` + 呈现层 + 新命令树 + shell 规则 + `.luban` 归档 + sandbox 的 persist；`--mode machine --temp` 过渡实现（§8.4） | 是 | 命令树快照、help 级别、export→import 往返、persist 三档、默认 `--display` 路径的启动测试（修 P6 的盲区） |
| 6 | `vm` carrier：启动环境本身、终端中继、supervisor、machine 的 persist；edition 新版本 | 是 | qemu 下 enter → 修改 → 重进仍在；`--temp` 不留；窗口大小同步 |
| 7 | luban-desktop：RFB 客户端、启动画面、环境管理、每机器一窗口 | 是 | 服务层的测试覆盖它的每个操作 |

每个阶段一个 PR；需求 ID 在实现时加入 `tests/requirements.toml`。

---

## 12. 待定与风险

- P1 中子 subos 为什么需要提权：待复现后决定是去掉该步骤的提权，还是只把它变成待办。
- 9p 与 virtiofs 的性能；rootless virtiofsd 的可用性。
- Windows 上的机器模式：qemu + WHPX 的性能；Hyper-V 以后再评估。
- RFB 经 virtio-serial 的带宽；有 vsock 时改用 vsock。
- 抽取的规模：777 处 `Config::`；阶段 4 只抽 luban 需要的部分，其余随后。
- xlings 移走 `subos boot`、`subos export --iso` 影响 2026.10 已发布的用法：发布说明写明，xlings 给出一行指向 luban 的拒绝。
