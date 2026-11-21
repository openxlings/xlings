# #724：hook 的运行时契约、carrier 的错误透传与 guest 的更新 —— 设计与实施计划

- 日期：2026-11-21（2026-11-22 按评审意见改为一次覆盖全部情形，并把此前未实测的各项逐一实测）。状态：已实施（2026.11.22.1），实施与本文不同之处见 §12。
- 范围：xlings（hook 的执行环境、carrier 的 userland 与更新、carrier 转发、系统根的合成）、
  xim-pkgindex（bun 及同类配方、索引检查）。libxpkg 不改。
- 来源：issue #724（Windows 上 `luban new agent --edition agent-workspace` 在安装 `xim:bun@1.3.11` 时失败，
  终端只显示 `subos_command failed (exit 1) with no error detail on the stream`）。
- 验证：临时 draft PR #725（已关闭，未合并），真实 WSL 2，已发布的 2026.11.21.1。
  run 44910699604（复现、现场、反事实），run 44911809766（宿主接口与 carrier 内接口对比），
  run 44919641483、44920286085、44921451722 与 44922463182（设计里的各个机制，在 Ubuntu 24.04 x64/arm、Ubuntu 18.04、Alpine x64/arm、
  WSL 2 carrier 上实测；§2.2）。
- 每个仓库一个 PR，PR 内以 commit 为检查点；不拆分，不延后。
- §10 记录四轮评审与实测所推翻与补充的内容。

## 0. 核心点

1. **缺的不是 glibc，是标准 loader 路径。** glibc 作为 bun 的 runtime 依赖已先装好，`<home>/abi/<family>/` 也已建立。
   bun 的 npm postinstall 在 hook **内部**执行刚下载的 bun；它的 `PT_INTERP` 是上游写的
   `/lib64/ld-linux-x86-64.so.2`，内核按字面读取。xlings 的改写（elfpatch）发生在 hook **返回之后**。
   这不是 carrier 独有的：Alpine x64、arm 上普通的 `xlings install xim:bun@1.3.11` 同样失败（实测）。
2. **一条契约（§4.1）**：hook 运行的根里，本 home 拥有的每个运行时家族，在它的标准 loader 路径上有一个运行时。
   根自己有的，用根自己的；根没有的，由环境的（`<home>/abi/<family>`）补上。
3. **一个根在一条标准路径上只能有一个运行时**（实测：换成环境的 loader 后，宿主的程序全部无法运行；§2.2 L7）。
   所以"宿主已有 glibc loader 时用谁的"不是可选项：用宿主的。它是契约的第一句，不是例外。
4. **一个判定，两种实现**。判定：这个根的标准路径上是否已有运行时。实现按"根归谁"分：
   - Luban 系统：根的合成给出（现状）。
   - 其它的根没有时，判定给出"链接"（标准目录不存在）或"合并"（已被占用）。根是 xlings 的且结果是"链接"：
     根自己带一个常量链接（carrier）。其余：这个包的 hook 在 worker 里运行，worker 的根是这个根的 userland
     加上这条路径。各种做法在 Alpine x64、arm 上均已实测可行，包括在私有域里再起一层。
5. **#724 由第一种实现修复**：carrier 的 userland 带上链接。
6. **guest 的更新改为本地进行**：Windows 侧把自己已有的同版本 Linux 构建的两个程序交给 guest，替换文件
   （实测可行，不联网）。guest 的 userland 由 guest 自己的 xlings 按修订号带到当前。
7. **错误透传**：接口调用转发到 carrier 时走 NDJSON，复用私有域路径已有的解码。guest 的日志文件取回到本 home。
   取消不需要额外处理：结束 `wsl.exe` 即结束 guest 里的进程（实测）。
8. **系统根里的标准 loader 与 `/xlings/abi` 已经同源**：系统的根只接受被选为运行时的那个 glibc（实测）。
   原计划的合成顺序改动撤销，只加一格检查把这个不变量写下来。
9. **包侧（xim-pkgindex）**：bun 不再运行 npm 的 postinstall，由配方选定平台包。索引发布即生效。
10. **同一个 edition 在三个平台上 hook 的环境不同**（Linux 是宿主的 userland，Windows、macOS 是 host machine 的）。
    这是 #724 在 CI 里漏掉的结构性原因。本次用测试补上：Linux 的 CI 里造一个与 host machine 同构的根来装（§7）。
11. **安装的发布失败被报告为成功（§4.5）**：实测中发现的另一个缺陷，根因在安装器的回滚分支，本次一并修。
12. **没有延后的项。** "不带宿主 userland 的 hook 根"实测可行（§2.2 L6、L9），但不能作为统一模型：
    索引里有面向宿主的配方（§6）。它被记录为不采用，并写明依据。

## 1. 场景

Windows 上 Luban 的运行方式（`docs/luban/07-host-support.md`）：

```
PowerShell: luban.exe new agent --edition agent-workspace
  └─ Windows 的 xlings：interface subos_command，argv 带 --carrier wsl2   (luban/os/service/src/service.cpp new_argv_)
       └─ forward_to_carrier_：wsl.exe -d xlings-<hash> … /xlings/bin/xlings subos new …   (src/core/subos/carriers.cpp:103)
            └─ Luban host machine（每个 home 一个私有的 WSL 2 发行版）里的 Linux xlings，home 是 /xlings
                 ├─ 安装 edition 的包：hook 在这个进程内、这台机器的根里运行      ← bun 在这里失败
                 └─ 合成系统的根（generation），系统以沙箱形式在这台机器里运行
```

- 每个 Luban 系统不是一个 WSL 发行版；一个私有发行版承载所有系统。
- 用户不进入这个发行版（文档："which you never enter"）。报告者自己的 Ubuntu WSL 没有参与，只是对照。
- 用户本应只在首次安装（WSL、Virtual Machine Platform、UAC）与 `luban status` 里看到 WSL。
  #724 中报告者为了定位问题，被迫进入发行版读日志。
- 造好的系统的根是 merged-usr 布局，`/lib64/ld-linux-x86-64.so.2` 存在；用户在系统里使用 bun、
  安装别的预编译工具不受本问题影响。

## 2. 证据

### 2.1 复现与现场

| 步骤 | 结果 | 来源 |
|---|---|---|
| 对照：有 `/lib64/ld-linux-x86-64.so.2` 的 Linux 宿主装 `xim:bun@1.3.11` | 成功，`bun --version` 为 1.3.11 | run 44910699604 |
| 真实 WSL 2 里 `luban new agent --edition agent-workspace` | 失败，终端文本与 issue 相同；hook 日志同为 `ENOENT rename … -> node_modules/@oven/bun-linux-x64` | 同上 |
| 失败后的 carrier | `/lib64`、`/lib/ld-linux*`、`/usr/lib64` 均不存在；`/xlings/abi/linux-x86_64-glibc/ld-linux-x86-64.so.2 -> …/xim-x-glibc/2.44.3/lib64/…` 存在 | 同上 |
| 反事实：carrier 的根里只加 `/lib64 -> /xlings/abi/linux-x86_64-glibc` | `xlings install xim:bun@1.3.11` 成功；`luban new agent8 --edition agent-workspace` 完成（31 个包） | 同上 |
| 代理 | 无关：runner 上代理端口无监听，失败表现相同 | 同上 |
| carrier **内**直接调用 `xlings interface subos_command` | 有结构化错误：`[bun] failed: install hook failed … npm error …`，带 hook 输出末尾 | run 44911809766 |
| 从 Windows 经接口调用 | guest 的输出成为 `[stray stdout] …` 与 `[error] …` 文本行；线上只有兜底的 `E_INTERNAL … no error detail` | 同上 |

### 2.2 机制的实测（run 44919641483、44920286085、44921451722，已发布的 2026.11.21.1）

宿主按标准路径的状况分三种：A 标准目录不存在（Alpine x64：没有 `/lib64`）；B 标准目录存在、里面没有这个 loader
（Alpine arm：`/lib` 是真实目录，84 项）；C 已有 glibc loader（Ubuntu 24.04 x64/arm：glibc 2.39；Ubuntu 18.04：2.27）。

| 编号 | 问题 | 结果 |
|---|---|---|
| L1 | 没有 loader 的宿主上普通安装 bun | A、B **均失败**（与 carrier 相同的 postinstall 失败）。C 三种均成功，包括 glibc 2.27 的宿主 |
| L2 / W3 | `npm install --ignore-scripts bun@1.3.11` 装了什么 | 所有环境一致：x64 上 **四个**平台包都装（`bun-linux-x64`、`-baseline`、`-musl`、`-musl-baseline`），arm 上两个；`node_modules/bun/bin/bun.exe` 是 450 字节的占位。选哪一个是 postinstall 做的事 |
| L3 | A：把 family 目录绑定到标准目录 | 未改写的 bun 运行（`1.3.11`）。只读绑定与符号链接两种都可行 |
| L4 | B（及 A 上的模拟目录，含悬空链接）：内存 overlay 加逐条绑定 | bun 运行；宿主的 `ls`、`sh` 照常；标准目录仍是宿主的 84 项加 family 的条目；没有同名冲突。绑定与符号链接都可行 |
| L5 | 私有域那样的命名空间里再起一层 worker（A：链接；B：合并） | bun 运行 |
| L6 | 与 carrier 同构的根（静态 xlings、busybox、bwrap，没有宿主的任何东西）里安装 18 个包：agent-workspace 的 claude、codex、pi、opencode、openclaw、bun、node、pnpm、esbuild，以及 python、uv、cmake、ninja、git、ripgrep、fish、go、gcc | 没有标准 loader 时 **只有 bun 失败**；补上 `标准目录 -> /xlings/abi/<family>` 后 bun 成功。五种宿主结果相同（arm 上 cmake 是索引没有 aarch64 制品，与此无关） |
| L7 | C：标准 loader 换成环境的，宿主的 userland 保留 | 保留宿主的 `ld.so.cache`：**每个程序都失败**（环境的 loader 加载了宿主的 libc：`undefined symbol __nptl_change_stack_perm, version GLIBC_PRIVATE`；2.27 上是 `Inconsistency detected by ld.so`）。遮住 cache：只有不依赖其它库的程序能运行，`ls`、`id`、`tar`、`git`、`perl`、`python3`、`curl` 全部找不到自己的库 |
| L8 | 一个系统里再装一个 glibc（`xim:glibc@2.39`，以及 `xim:glibc@2.44.2`） | 两个都一样：系统的根 **拒绝这一代**（`the root of subos 'v6' was not updated: …/glibc/<版本>/sbin/iconvconfig: ELF interpreter is outside its checked root closure`），登记被回滚。之前之后，`/lib64/ld-linux-x86-64.so.2`、`/usr/lib/libc.so.6` 与 `/xlings/abi/…` 都解析到同一个 glibc 2.44.3。原因：glibc 的 payload 自带的程序的 interpreter 是占位路径（2.44.2：`/nonexistent/xlings-use-rpath-not-default-search/…`；2.39：构建机上的路径），只有被选为运行时的那一个才被改写到自己。**同一次安装仍然报告成功**：`[1/1] installed`、`✓ 1 package(s) installed`、退出码 0，并提示 `xlings use glibc 2.44.2`，而这条命令随后回答 `version '2.44.2' not found`（§4.5） |
| L10 | musl 宿主装了 gcompat（Alpine：`/lib64/ld-linux-x86-64.so.2 -> ../lib/ld-linux-x86-64.so.2`，musl 的兼容层） | 普通安装 bun **成功**（hook 里的 bun 经兼容层运行）。把 `/lib64` 换成环境的：宿主的程序（musl，不经这个路径）照常，bun 也运行 |
| L9 | 在宿主的 home 上起一个"环境 userland"的 worker（busybox 加环境的运行时，没有宿主的 `/usr`），运行 bun 真实的 postinstall | 没有标准目录：失败。有：成功。五种宿主相同，包括 glibc 2.27 的宿主与 Alpine |
| W1 | guest 从挂载的 Windows 目录取得 xlings | `mount -t drvfs <Windows 目录> … -o ro` 成功，其中的程序可直接运行。`self install` **不可用**（要求完整的发布包目录：`cannot detect source package directory`）。把 `xlings`、`luban` 两个文件复制后改名替换：成功，上一版与当前版来回各一次，home 都能读 |
| W2 | 结束 `wsl.exe` 之后 guest 里的进程 | `Stop-Process`、`taskkill /T /F`、带管道的 `Process.Kill()` 三种，guest 里的进程及其子进程都随之结束（2 → 0） |
| W3 | carrier 里未改写的 bun | 直接运行：`not found`。经 `/xlings/abi/linux-x86_64-glibc/ld-linux-x86-64.so.2 <bun>`：运行。链接标准目录后：运行 |

索引的事实（xim-pkgindex `4fa1cbc`）：

- hook 里启动的外部程序以 `chmod`、`sh`、`ln`、`tar`、`cp` 为主，busybox 都有。
- 约 400 个配方里，hook 明确面向宿主的有 8 个：执行 `sudo` 的 5 个（configure-project-installer、git-autosync、
  gitcode-hosts、ros2-jazzy-ubuntu、sing-box-helper，其中 ros2 还调用 `apt`），`systemctl`（ollama 等）、
  `VBoxManage`（virtualbox），以及 `dsh`（node-gyp 需要宿主的 python3、make 与 C++ 工具链）。
  这是按文本查到的下限；其余配方是否隐含依赖宿主的工具，只有在没有宿主的根里逐个安装才知道（L6 测了 18 个）。
- esbuild 与 codex 的配方已经直接取 npm 平台包的归档，不经 `npm install`；openclaw 已用 `--ignore-scripts`。

仍然只由代码读出的：

- carrier 里 hook 在进程内运行（§3）。L6 与它相容：同构的根里、没有 policy，行为与 carrier 一致。
- vz（macOS）上把文件交给 guest 的方式。WSL 2 上实测的是 drvfs 挂载；vz 用它自己的目录共享，在 macOS 的 CI 上验证。

为什么 CI 没有发现：Windows 的 `luban-wsl2` 一道只造 `tiny`；agent-workspace 只在有系统 loader 的 Linux 宿主上造过；
host-matrix 的 u-alpine、u-alpine-arm 只造 nano 与 tiny。

## 3. 根因

**R1（主因）：hook 的执行环境没有统一的定义。** Linux 上 hook 在四种环境之一里运行：

| | 环境 | 何时 | hook 所见的根 | 标准 loader |
|---|---|---|---|---|
| E1 | xlings 进程内 | home 没有 policy、payload 不是借来的（`lua_boundary.cpp:983` 直接 `xpkg::create_executor`）。普通的 `xlings install`；**carrier 里造系统**（模板的 policy 在装完包之后才应用，`cmd.cpp:1503`、`:1508`） | 这台机器的根 | 机器有才有 |
| E2 | xlings 进程内，私有域的 bwrap 里 | Linux 宿主上 `luban new`（`--domain-if-possible /xlings`） | tmpfs 根，宿主 `/` 的条目逐个只读呈现（`domain_producer.cpp:392-411`） | 宿主有才有 |
| E3 | Lua worker，bwrap | 活动的 SubOS 有 policy，或 payload 是借来的 | `host_userland()`：宿主的 `/usr`、`/bin`、`/lib`、`/lib64` 等，存在才绑定 | 宿主有才有 |
| E4 | Lua worker，Landlock | Luban 系统内部 | 系统自己的根 | 有（系统的运行时） |

- 运行时模型（AGENTS.md；Luban OS part 3 §4.2）规定"运行时是环境的属性，经 `<home>/abi/<family>` 绑定"。
  它对安装完成的 payload 成立；对 hook 执行期间的外来 ELF，只有 E4 成立。
- 这与 2026-08-08 记录的 G2（"构建输入侧的宿主泄漏"）是同一个缺口在安装期的表现。
- carrier 的定位（`carrier_image.cpp` 注释）是"guest 的 userland，如同一个 Linux 所有的：一个 shell 及其工具，
  供包的 hook 使用"，全静态。它给了 shell，漏了 ABI 的入口。#724 是 E1 在 carrier 上。

**R2（掩盖）：bun 的 `install.js` 在平台二进制运行失败后走回退路径，回退路径自身报 ENOENT。**
上游包的行为，xlings 不修；包侧绕开（§5）。

**R3（诊断丢失）：接口调用经 carrier 转发时退化为终端文本。**

- `forward_to_carrier_` 一律调用 `carrier::terminal`（`carriers.cpp:121`）；外层在接口模式下把非 NDJSON 的行
  写到 stderr 并加 `[stray stdout]`（`interface.cpp:340`）；luban 只读结构化事件，`interface.cpp:387` 合成兜底错误。
- 私有域的路径没有这个问题：`run_domain_relayed_` / `relay_capability_`（`cmd.cpp:1015-1064`）
  在调用方是接口时以 `interface subos_command` 运行并转发事件。carrier 路径没有接上它。
- 错误里的日志路径在 guest 内部，Windows 上打不开。

**R4（修复到不了用户）：guest 的更新依赖 guest 自己联网，且失败后不再尝试。**
`refresh_guest` 只在探针超过十分钟时进行；版本不同时让 guest 执行 `self update`，每个客户端版本只试一次，
失败也记为已试（`modules/carrier/src/carrier.cpp:72`、`:83-91`）。guest 可以长期停在旧版本，
任何放在 guest 里的修复都到不了它。

## 4. 设计：xlings

### 4.1 hook 的运行时契约（R1）

**契约**：hook 运行的根里，本 home 拥有的每个运行时家族，在它的标准 loader 路径上有一个运行时
（loader 与它旁边的同族库）。根自己有的，用根自己的；根没有的，由环境的（`<home>/abi/<family>`）补上。

- **hook 看到的是包所安装进的那个系统。** xlings 模式下是宿主；carrier 里是 host machine；Luban 系统里是系统。
  索引里面向宿主的配方（`sudo`、`systemctl`、宿主的工具链）依赖这一点。
- **一条标准路径上只有一个运行时。** loader 与 libc 必须是同一个 glibc 的；宿主的程序经 `ld.so.cache` 与发行版的
  库目录找库，环境的 loader 不读它们（L7：换掉之后宿主的程序全部无法运行）。所以根已有运行时的时候不替换它。
- xlings 自己的根里，这个运行时就是环境的：carrier 的链接、Luban 系统的合成，都指向 `<home>/abi/<family>`。

| 家族 | 标准路径 | 标准目录 |
|---|---|---|
| linux-x86_64-glibc | `/lib64/ld-linux-x86-64.so.2` | `/lib64` |
| linux-aarch64-glibc | `/lib/ld-linux-aarch64.so.1` | `/lib` |

- **到达的单位是目录。** xlings 的 glibc 带 `default-dir-follows-loader` 补丁：默认搜索目录是 `PT_INTERP`
  调用路径（字面，不解析链接）所在的目录（SubOS part 2 §2.3 实测）。经 `/lib64/ld-linux-x86-64.so.2`
  启动的进程在 `/lib64` 里找 `libc.so.6`。反事实与 L3、L4 验证的正是"family 目录的内容出现在标准目录里"。
- musl 家族不在契约内：它的 loader 不按调用路径找库（读 `/etc/ld-musl-*.path`），是另一条规则，
  也没有出现需要它的情形。

**一个判定**（纯函数，输入：对当前根的探针、本 home 的家族集合；每个包的 hook 开始前算一次）：

| 结果 | 条件 | 含义 |
|---|---|---|
| 已有 | 标准路径存在 | 不做任何事（宿主的 glibc、宿主自己装的兼容层如 gcompat、Luban 系统、带链接的 carrier 都落在这里） |
| 链接 | 标准目录不存在 | 标准目录 = family 目录 |
| 合并 | 标准目录存在，里面没有这个 loader | 标准目录 = 宿主的内容 + family 的条目，同名时宿主的保留 |
| 无可提供 | 本 home 没有这个家族 | 不做任何事；失败时说明（见下） |

**两种实现**：

**(a) 根是 xlings 的，且判定结果是"链接"：根自己带着。**

- carrier：标准目录的链接指向 `/xlings/abi/<family>`，按 guest 架构。
- 链接是常量：`<home>/abi` 是指向 `abi.gen/<n>` 的链接，运行时换代只改它。链接可以先于 glibc 的安装存在。
- **条件是标准目录不存在，不是"根是 xlings 的"本身。** 两个家族的标准目录性质不同：`/lib64` 是专用的
  （只放 loader）；`/lib` 是通用的（内核模块、固件）。WSL 2 在运行时自己建 `/lib` 并在其中挂 `/lib/modules`
  （实测，x64 的 carrier 里 `/lib` 是真实目录）。所以：
  - x86_64 的 carrier（WSL 2）：`/lib64` 不存在 → 链接。#724 的修复，反事实实测过。
  - aarch64 的 carrier：vz 的镜像里没有 `/lib` → 链接；若运行时有了 `/lib`（将来 ARM 上的 WSL 2 必然如此），
    判定给出"合并"，由 (b) 的 worker 完成，根里不建任何东西。两种情况是同一个判定的两个结果，不是两套代码。
  - 链接由 guest 自己的 xlings 在探测时按当时的状态建（§4.4 的 userland 修订），不写死在镜像里。
- Luban 系统：根的合成已经给出标准路径，且与运行时选择同源（§4.3）。

**(b) 其余情形：这个包的 hook 在 worker 里运行，worker 的根满足契约。**

- 位置：`lua_boundary.cpp` 的 `create_executor`。现在的规则是"没有 policy 且 payload 不是借来的就在进程内执行"；
  加一个条件：判定结果是"链接"或"合并"时也启动 worker。没有 policy 时用 `minimum_layer_policy`
  （已有，借来的 payload 走的就是它）。
- worker 的挂载在已有的挂载之后追加：
  - 链接：family 目录只读绑定到标准目录（先例：为 worker 自身的 loader 做的 `runtime_ro`）。L3 实测。
  - 合并：宿主的标准目录以 `TmpOverlay` 呈现在标准目录（已有的挂载种类；写入留在内存，hook 结束即消失），
    再把 family 里宿主没有的条目逐个只读绑定进去。条目数是 family 的大小（几十个），与宿主目录的大小无关。
    L4 在 Alpine arm 真实的 `/lib` 上实测；宿主目录里的悬空链接不影响（A 上的模拟目录）。
  - 两处都用只读绑定（已有的挂载种类），不新增"符号链接"这一种；实测两者等效。
- 同名时宿主的保留：不改变今天能运行的宿主程序。被遮住的 family 条目记入 hook 日志。
- 时序：worker 每个包启动一次（`installer.cpp:2996` 的循环、`:3331`），运行时包安装后已刷新 abi
  （`:3123`、`:4143`），启动时看到的是当时的那一代；一代目录被绑定后换代仍可读。
- **E1、E2、E3 是同一段代码**：判定探的是"当前进程的根"。普通宿主（E1）、私有域里看到的宿主（E2）、
  已经在 worker 里的情形（E3，它的根来自同一个宿主）得到同一个结果。私有域的命名空间允许再建子命名空间
  （`domain_producer.cpp:385-389` 的说明），worker 在其中启动（L5 实测，链接与合并两种）。
- 不改 `host_userland()`，不动 `tests/fixtures/intent-eq` 的 golden：挂载在编译之后追加，只属于 hook 的 worker
  （先例：`runtime_ro`）。golden 的规则是"沙箱的形状可审"，所以追加的挂载由一个纯函数给出
  （输入：判定结果、family 的条目、标准目录的条目），它的输出有自己的固定用例，不是散在启动代码里的拼接。

**做不到时**（如实说明，不假装）：

| 情形 | 行为 |
|---|---|
| 这台机器起不了 worker（没有可用的 bwrap 或 user namespace；合并需要的 overlay 内核不支持） | hook 照今天的方式运行。失败时错误里追加：这台机器没有 `<标准路径>`，hook 里启动的下载来的程序无法运行；以及使隔离可用的命令（`xlings self doctor --isolation --fix`） |
| 本 home 没有该家族（配方没有声明 runtime 依赖） | hook 照今天的方式运行。失败时追加：这台机器没有 `<标准路径>`，配方需要声明它的 runtime 依赖 |
| 根已有运行时，且它比环境的旧，或不是 glibc（`platform::host_glibc_version()` 为空：musl 上的兼容层） | 契约已成立，行为不变。失败时追加：hook 里启动的程序用的是这台机器的 glibc `<x>`，环境的是 `<y>`。实测 glibc 2.27 的宿主上 bun 的 hook 成功；这句话是为需要更新 glibc 的程序准备的 |

这三句说明由同一个函数给出，接在 `format_hook_failure` 之后；它只陈述测得的事实，不猜测失败的原因。

**配方需要在 hook 里运行尚未改写的程序、又不想依赖根的运行时的时候**，已有两个办法，都不需要新接口：
先用 `elfpatch.set_interpreter` 改写再运行（索引里已有 4 个配方这样做），或经
`<home>/abi/<family>/<loader> <程序>` 启动（W3 实测）。写进配方文档（X14）。

### 4.2 接口调用经 carrier 的事件转发（R3）

**规则**：调用方读的是结构化事件（`stream.structured()`）时，转发到 carrier 的调用是
`interface subos_command`，guest 的事件进入外层的事件流。调用方是终端时不变。与私有域的路径是同一条规则。

- **解码复用。** `relay_capability_` 里对 `result / data / progress / log / error / prompt` 的解码提出来，
  供私有域与 carrier 两条路径共用。`prompt` 成为错误并指出 `-y`（已有行为）；`heartbeat` 忽略；
  其余不认识的 kind 记为 debug 日志。
- **`modules/carrier` 只加一个按行读取的调用**（暂名 `relay`）：在 endpoint 上运行
  `xlings interface <capability> --args <json>`，每行交给回调，以 JSON 文本为界，不引用 `src/` 的事件类型。
  它带上 `forward_to_carrier_` 现在转发的九个环境变量（已有的 `control` 传的是空环境）。
  参数经 `wsl.exe` 的命令行传 JSON（已有的 `control` 如此）；X3 用带引号、空格与非 ASCII 路径的参数测，
  不稳就改为把参数写成文件、经 `grant` 交给 guest 的 `--args-file`（接口已有这个入口）。
- **交出终端的命令不转发为接口调用**：`exec`、`use`、`start`、`log`、`boot`、`__broker-serve` 保持
  `carrier::terminal`。私有域的路径已有同样的例外（`root_cmd.cpp:438-440`）。
- **`-y`**：作为调用的 `yes` 字段传递，与私有域的路径一致。
- **取消**：外层的取消经 `stream::run` 的 `cancelled` 判定结束子进程，结果为 130。结束 `wsl.exe`
  即结束 guest 里它启动的进程及其子进程（W2，三种结束方式），不需要另向 guest 发终止。
- **guest 的版本**：§4.4 之后 guest 与客户端同版本。带不到同版本时（§4.4 的失败情形）调用被拒绝并给出原因，
  不以旧 guest 执行，也不在调用之后回退（`subos new` 执行到一半不可重跑）。
- **guest 的 stderr**：不进事件流，写入本 home 的日志；guest 没有给出 `result` 时作为失败说明的一部分。
- **进度**：guest 的 progress 事件原样转发。luban 按阶段名取单调最大值（`luban/os/service/src/events.cppm`），
  与 Linux 私有域路径上的表现相同，不重复计数。

**日志取回**：用户不进入 host machine，所以错误指向的文件要在这台电脑上。

- guest 侧：hook 失败时，除了已有的错误文本（含输出末尾与 `full log:`），再发一个 data 事件，给出日志文件的路径
  （接口只增不改）。
- 转发处：收到这个事件且来源是 carrier 时，把文件取到 `<home>/logs/carriers/<carrier>/…`
  （经 launcher 读取；只接受 guest 的 `<home>/logs/` 之下的路径），并把随后错误的 message、hint 里的
  guest 路径替换为本地路径。取不回时保留原路径并注明它在 host machine 里。
- 不新增命令。

**兜底提示不改**：对真正没有细节的情形仍然成立。

### 4.3 系统根里的标准 loader 与运行时选择同源

Luban 系统的根里有两处 loader，声明的来源不同：

- `/usr/lib/ld-linux-x86-64.so.2`（经 `/lib64 -> usr/lib64 -> lib` 成为标准路径）：来自活动 payload 的
  `lib` / `lib64` 里的共享对象，同名先到者保留（`rootfs.cpp:474-480`）。
- `/usr/lib/xlings-abi/<family>/`（系统内即 `/xlings/abi`）：来自运行时选择（`rootfs.cpp:458`）。

part 3 §4.2 要求两者"指向同一个"。前一稿担心系统里有两个 glibc 时取决于声明顺序，并计划改合成顺序。
L8 表明这个状态到不了：系统的根只接受被选为运行时的那个 glibc。glibc 的 payload 自带的程序，interpreter 是
占位路径，只有运行时选择选中的那一个才被改写到自己；别的版本进入工作区时，闭包检查
（`store_closure.cpp:281`）拒绝这一代，登记回滚，根保持原样。两处因此始终是同一个文件。
（这同时意味着一个系统里不能并存两个 glibc 版本；这是现状，本设计不改变它。）

- **合成不改。** 不变量由已有的检查保证。
- **把不变量写下来**：`luban status --check` 加一格——标准路径的 loader 与默认运行时的 loader 是同一个文件
  （`root_check.cpp` 现在只查 loader 是否存在）；闭包检查的单元测试加一条——不是运行时的 glibc payload 被拒绝。
  以后有人放宽闭包检查或改变 glibc 的改写方式时，这两处会先红。

### 4.4 guest 的更新与 userland 修订（R4）

**guest 的 xlings 由宿主交给它，不由它自己联网取。**

- 宿主本来就持有同版本的 Linux 构建：做镜像时取到 `<home>/carriers/linux-xlings/<version>/`
  （`carrier_image.cpp` 的 `download_linux_build`，经索引与镜像，按 sha256 校验）。
- 版本不同时：把这个目录只读地给 guest，guest 把其中的 `bin/xlings`、`bin/luban` 复制到 `<home>/bin` 下的
  临时名字再改名替换。这与做镜像时放这两个文件是同一个形状（镜像不经过 `self install`）。
  不经过 guest 的网络，不依赖 guest 里的代理配置。W1 实测：上一版与当前版来回替换，home 都能读。
  - 不用 `self install`：它要求完整的发布包目录（W1：`cannot detect source package directory`），
    而 Windows 的文件系统放不下发布包里的链接。
  - home 的格式由新的 xlings 第一次运行时自己带到当前（`xlings.home.format` 已有的规则）。
  - 交付的通道是 carrier 已有的 `grant`（把宿主的一个目录呈现在 guest 的 `/grant/<name>`；WSL 2 上是
    `__carrier-grant` 做的 drvfs 挂载，W1 实测的就是这种挂载；vz 有自己的实现）。不新增原语。
  - 替换是改名：正在 guest 里运行的进程继续用旧的文件直到结束，与现在的 `self update` 相同。
- 何时判断：每次 `ensure()` 之后，用已存的探针里的版本与客户端版本比较；不同就立即进行，不等十分钟。
  十分钟的间隔只用于"重新探测 KVM 与内核"。
- 失败：不记为已试。本次调用被拒绝，错误说明 guest 的版本、失败的原因与日志位置；下一次调用再试。
  替换是改名，中途失败时 guest 里仍是原来的那个完整文件。
  `luban status` 的 host machine 一行显示 guest 的版本与上一次更新的结果。
- 降级不做：guest 比客户端新时（用户换回旧客户端）不动它，接口只增不改。

**userland 修订**：carrier 的 userland（镜像里 xlings 以外的东西）有一个整数修订号。

- 修订 1：现状（busybox 的 applet 链接，`guest_probe` 已在第一次探测时做）。
- 修订 2：标准 loader 目录的链接（§4.1 (a)）：标准目录不存在时建，存在时不动。
- guest 的 xlings 在探测时把 userland 带到自己知道的修订：每一步幂等，做完记录修订号
  （记在 guest 的 home 的 `state/` 下，与宿主侧保存的探针分开）。
  新镜像直接带着当前修订。与 home 格式"向前走、一次、幂等、有记录"是同一种做法。

顺序因此是一条链：客户端版本 → 交给 guest 同版本的 xlings → guest 探测时补齐 userland → 转发调用。

### 4.5 安装的发布失败不得报告为成功

**现象**（L8）：在 Luban 系统里 `xlings install xim:glibc@2.44.2`，根拒绝这一代。输出里有一行 `[error] the root … was not updated`，
随后是 `[1/1] installed xim:glibc@2.44.2`、`installed, but 'glibc' still resolves to 2.44.3 — xlings use glibc 2.44.2 to switch`、
`✓ 1 package(s) installed`，退出码 0。按提示执行 `xlings use glibc 2.44.2` 得到 `version '2.44.2' not found`。
脚本、Agent 与 luban 都把这次安装当作成功。

**根因**：`src/core/xim/installer.cpp` 的 `process_xvm_operations_`。发布（保存登记、核对、刷新路由与根）在一个
`try` 里；`catch` 分支恢复元数据、回滚已应用的变更、再刷新一次，然后**只在回滚的刷新也失败时**返回错误
（约 `:2436-2441`）。回滚成功时控制流落到 `try` 之后，继续按成功处理并提交。原始的失败只留下
`sync_shim_tables` 自己打的那一行日志。同一函数里 `replace_entry_binary` 失败的分支（约 `:2460-2470`）是同一个形状，
而且回滚之后还会走到 `commit()`。

同一文件里另一处发布（约 `:1940-1958`）是对的：`catch` 里收集失败、回滚、`return std::unexpected(failure)`。

**修复**：

- 两个分支在回滚之后都返回原始的失败；回滚自身的失败追加在同一条消息后面（与 `:1940-1958` 同一个形状）。
  安装因此以这个包的失败结束：没有 `installed` 一行，没有"用 `xlings use` 切换"的提示，退出码非 0，
  接口模式下有一条带原因的 error 事件。
- 失败之后的状态：登记已回滚（现状如此）。store 里已下载的 payload 是否留下，在实施时核对并定为一种：
  留下则它是未登记的 store 内容，由已有的清理负责；本修复不新增清理路径。
- 这条错误的文字补上被拒绝的 interpreter 路径（闭包检查已经读到它），便于判断是包没有改写还是指向了根之外。

**范围**：这是安装器的缺陷，与 hook、carrier 无关；任何让根的刷新在发布时失败的包都会触发，不限于 glibc。
放在本 PR 里是因为它是这次实测发现的，并且它使"失败却显示成功"在 #724 之外的路径上继续存在。

## 5. 设计：xim-pkgindex

契约落地后，hook 里运行尚未改写的程序在每一种根里都有运行时可用；它用的是根的运行时（宿主的，或环境的）。
包侧因此不再需要一条"禁止"的规则。bun 仍然要改，原因是它自己的：

- **bun（P1）**：不再运行 npm 的 postinstall。
  - 现状的 postinstall 做三件事：从已装的平台包里选一个（有 AVX2 用 `bun-linux-x64`，否则 `-baseline`；
    musl 的宿主用 `-musl`），运行它验证，放到 `node_modules/bun/bin/bun.exe`。验证失败后的回退路径本身有缺陷（R2）。
  - 改为 `npm install --ignore-scripts`，由配方完成选择与放置：glibc 的平台包（xlings 的运行时是 glibc，
    不看宿主的 libc）；x86_64 上按 `/proc/cpuinfo` 有无 `avx2` 选普通版或 baseline；然后删除 `@oven`（现有逻辑）。
    L2、W3：`--ignore-scripts` 下四个平台包都在，选择所需的输入都有。
  - 安装方式变了，提升 `revision`。不给配方加 xlings 版本下限（索引的安装测试用旧客户端运行）。
  - 索引发布后，旧客户端、旧 carrier 上 bun 也能装：这条路不依赖 xlings 的发布。
  - #1215（已关闭未合并）的描述提到 aarch64 上改写后的 bun 会段错误。L6 在 arm 上安装成功，未运行改写后的程序；
    P1 的测试里运行 `bun --version`，x64 与 arm 都跑。
- **同类配方（P2）**，逐个给出结论：
  - esbuild、codex：已直接取平台包归档，不经 npm；L6 在没有 loader 的根里安装成功。不改。
  - openclaw：已用 `--ignore-scripts`；L6 成功。不改。
  - pnpm、claude、opencode、pi、node、python、uv、ninja、git、ripgrep、fish、go、gcc：L6 成功。不改。
  - npm（`npm install -g npm@…`）、dsh（node-gyp，需要 python3、make、C++ 工具链）：不在 L6 的清单里。
    P2 里在同构的根中各装一次；dsh 所需的工具在索引里都有，缺的声明为依赖。
- **索引检查（P3）**：索引 CI 加两种宿主的安装测试，对象是带原生二进制的配方（上面这些）：
  Alpine 容器（没有 glibc loader，x64 与 arm）与受支持的最旧 glibc 宿主（host-matrix 里是 ubuntu:20.04）。
  前者挡"假设宿主有 loader"，后者挡"假设宿主的 glibc 够新"。P1 合入之前前者对 bun 是红的，所以排在 P1 之后。
  再加第三种：与 host machine 同构的根，对象是 luban 各 edition 列出的包。它挡"假设有宿主的工具"，
  也就是 Windows、macOS 用户实际经过的环境；它的结果同时是 G2 的数据（哪些配方离得开宿主）。

## 6. 不采用的方案

| 方案 | 为什么不 |
|---|---|
| 宿主已有 glibc loader 时，把标准路径换成环境的，宿主的 userland 保留 | 实测不可行（L7）：保留宿主的 `ld.so.cache` 时每个程序都失败（环境的 loader 加载到宿主的 libc）；遮住 cache 时宿主的程序找不到自己的库。一个根在一条标准路径上只能有一个运行时 |
| 所有 hook 一律在"环境 userland"的根里运行（busybox 加环境的运行时，没有宿主的 `/usr`；与 carrier 同构） | 机制上可行，且最统一：L9 在五种宿主上通过，L6 里 18 个包全部能装。不采用，因为 hook 的对象是包所安装进的系统：索引里至少 8 个配方的 hook 面向宿主（`sudo`、`systemctl`、`apt`、`VBoxManage`、宿主的工具链），其余约 390 个只测过 18 个。要走这条路，需要配方能声明"面向宿主"，并在没有宿主的根里把整个索引装一遍（G2）；§5 P3 的同构根测试开始积累这份数据 |
| 只在"宿主的 glibc 比环境的旧"时改用环境 userland | 环境的 glibc（2.44）比几乎所有宿主的都新，这个条件等于"所有宿主"；而且需要的是"比这个程序要求的旧"，事先无从得知。实测 glibc 2.27 的宿主上 bun 的 hook 成功 |
| hook 失败后换一种根重跑 | hook 不保证可重入；同一个包在两台机器上经过不同的路径 |
| 在"正在造的系统"的根里运行 hook | payload 属于 store，被多个系统共用，安装结果不能取决于某个系统；xlings 模式下没有系统；generation 在包装完之后才合成，顺序上循环 |
| 只在 worker 沙箱里加挂载（本文初稿） | carrier 里 hook 不在 worker 里运行，修不到 #724 |
| carrier 里也走 (b)，不带链接 | 只有一种实现、不需要 userland 修订，是更统一的形状。不采用，因为它把 Windows、macOS 上每一个包的 hook 都改为经 worker 运行，改变的是主路径上全部安装的行为，而要修的只是缺一个路径；链接是反事实实测过的。标准目录已被占用的 carrier 仍会走 (b)，所以这条路径在 carrier 上是有测试的；G2 使 worker 成为常态时，链接随之取消 |
| 链接写死在 carrier 的镜像里 | aarch64 的标准目录 `/lib` 是通用目录，WSL 2 运行时自己会建它；写死的链接在那里要么冲突，要么让别人写进运行时的目录。改为 guest 探测时按状态建 |
| Linux 的私有域也用与 carrier 同构的根，使三个平台上 hook 的环境相同 | 这是 Luban 一侧最干净的形状（Luban 的宿主只有一种）。不采用：私有域同时承载系统的运行（设备、会话的套接字），它的根是按"宿主逐项只读呈现"设计的；改它是 Luban 在 Linux 上的模型变更。本次用同构根的测试消除它带来的盲区（§7） |
| musl 宿主上标准路径被兼容层占着时，换成环境的 | 实测可行（L10：宿主的程序不经这个路径）。不采用：兼容层是这台机器自己的安排，bun 的 hook 在它上面成功；与"根有的用根的"一致。失败时的说明会指出它不是 glibc |
| 私有域启动时加一个 `--symlink`（第二稿的 E2 做法） | 只能表达"链接"，表达不了"合并"；并使同一条契约有三种实现。(b) 在私有域里同样成立 |
| 合并用"空目录 + 宿主条目逐项绑定" | 条目数随宿主目录增长（`/lib -> /usr/lib` 时上千），悬空链接会使 bwrap 中止整个沙箱。`TmpOverlay` 没有这两个问题 |
| 没有 worker 时失败整个安装 | 大多数 hook 不启动下载来的程序；这类机器上今天能装的包会装不上 |
| guest 继续联网 `self update`，只改"失败后重试" | 仍依赖 guest 的网络与代理；宿主手里已有同版本的构建 |
| guest 从交付的目录执行 `self install` | 实测被拒绝：它要求完整的发布包目录（W1） |
| 合成根时让运行时选择先声明 `usr/lib` 的条目（前一稿 §4.3） | 它要防的状态到不了（L8：根只接受被选为运行时的 glibc）；改合成顺序会改变每个根的声明记录而没有收益 |
| 包侧规定"hook 不得运行尚未改写的程序" | 契约成立后这是合法的；无法在不加开关的情况下检查。改为修 bun 自身的问题，并在两种宿主上测（§5） |
| 改 `host_userland()` | 影响所有沙箱与 golden，超出问题 |
| 只修包侧 | bun 可装，R1、R3、R4 仍在 |

## 7. 测试

需求编号（`tests/requirements.toml`，名称在实施时定）：

- `HOOK-RUNTIME-CONTRACT`：hook 运行的根里，本 home 拥有的运行时家族在标准 loader 路径上有运行时：
  根是 xlings 的由根带着（环境的）；根不是且没有时 hook 在补上它的 worker 里运行；根已有时不变；
  做不到时照旧运行并在失败时说明。
- `CARRIER-RELAY-EVENTS`：接口调用转发到 carrier 时，guest 的错误、进度作为事件到达调用方；失败时线上至少有一条
  guest 给出的错误，没有兜底的 "no error detail"；交出终端的命令不受影响；guest 的日志文件在本 home 里。
- `CARRIER-GUEST-CURRENT`（已有，改写）：guest 的 xlings 由宿主交给它（替换文件），与客户端同版本；失败不记为已试；
  userland 被带到当前修订。
- `ROOT-STD-LOADER-RUNTIME`：系统根里标准路径的 loader 与默认运行时的 loader 是同一个文件。
- `INSTALL-PUBLISH-FAILURE`：包的发布失败（登记、路由或根的刷新）时，安装以这个包的失败结束，
  登记保持安装之前的样子，不显示已安装，也不给出切换版本的提示。

| 层 | 内容 | 在哪里跑 |
|---|---|---|
| 单元 | 判定：已有 / 链接 / 合并 / 无可提供，两个架构；三句说明各自的触发条件 | 每个 PR，Linux |
| 单元 | 提出来的解码：各 kind、`prompt`、`heartbeat`、不认识的 kind；guest 路径到本地路径的替换 | 每个 PR |
| 单元 | userland 修订：标准目录不存在时建链接、存在时不动（两个架构）；步骤幂等 | 每个 PR |
| 单元 | worker 追加的挂载：链接、合并各自的固定输出 | 每个 PR |
| 单元 | 闭包检查：不是运行时的 glibc payload 被拒绝；`status --check` 的同源一格 | 每个 PR |
| e2e（C++） | 夹具配方：payload 里有一个 interpreter 指向根之外的 ELF，装进一个根：退出码非 0，有它的错误，工作区与安装前相同，没有"已安装" | 每个 PR，Linux |
| e2e（C++） | `relay` 对 `local` endpoint：一个必然失败的 `subos_command`，事件里有它的错误，没有兜底 | 每个 PR，Linux |
| e2e（WSL 2） | 夹具配方：hook 里执行一个 `PT_INTERP` 为标准路径的 glibc 程序；在 carrier 上安装成功 | `luban-wsl2` 一道（PR） |
| e2e（WSL 2） | 一个 hook 必然失败的夹具配方，经 `xlings interface` 从 Windows 调用：线上有 hook 的错误，日志文件在 Windows 的 home 里 | 同上 |
| e2e（WSL 2） | guest 被换成另一个版本、userland 退回修订 1：下一次使用时二者都被带到当前，不联网 | 同上 |
| e2e（宿主矩阵） | 同一个夹具配方：u-alpine 上普通安装（E1，链接）、经 `luban new`（E2）、有 policy 的 SubOS（E3）；u-alpine-arm（合并） | host-matrix（main 定时；实施时 dispatch） |
| e2e（宿主矩阵） | 现有的全部场景在 u-alpine、u-alpine-arm 上不退步（这两行的每个包的 hook 现在都经 worker） | 同上 |
| e2e（Linux，同构根） | 在 Linux 的 runner 上造一个与 host machine 同构的根（静态 xlings、busybox、bwrap，没有宿主的任何东西），在其中安装夹具配方与 `luban new` 一个最小的 edition | 每个 PR，Linux |
| 发布后 | `luban new --edition agent-workspace` 在 WSL 2 上 | luban-fresh |

- 同构根的用例是为了让"只在 host machine 里才出现的问题"在每个 PR 的 Linux CI 里出现：#724 在 L6 的同构根里
  用已发布的版本就能复现，不需要 Windows。
- 夹具配方不依赖 bun：索引修复后 bun 不再执行未改写的二进制，无法再充当用例。
- 夹具里"标准路径的 glibc 程序"的来源在实施时取较轻的一种：每架构一个极小的预编译 ELF 作为测试资源，
  或在 hook 内把依赖里的一个程序的 `PT_INTERP` 改回标准路径。
- 反向对照：每条 e2e 在修复前的构建上应当失败（第一个 commit 只加用例，记录它的失败）。

## 8. 实施计划与依赖

### xlings（一个 PR）

| 编号 | 任务 | 依赖 | 完成的判据 |
|---|---|---|---|
| X1 | 夹具配方与失败用例（WSL 2 一道、Linux 的同构根、C++ e2e），需求编号 | — | 在未修复的构建上按预期失败 |
| X2 | 解码从 `relay_capability_` 提出，两条路径共用 | — | 私有域的现有用例不变 |
| X3 | `carrier::relay`（按行、带环境、可取消） | — | 对 `local` endpoint 的 e2e 通过 |
| X4 | `forward_to_carrier_`：结构化调用方走 `relay`；例外清单、`yes`、stderr | X2、X3 | X1 的错误用例在 WSL 2 上通过；终端模式的现有用例不变 |
| X5 | 日志取回：guest 的 data 事件、转发处取回与路径替换 | X4 | 错误里的路径在 Windows 上存在 |
| X6 | 契约的判定（纯函数）、家族到标准目录的表、三句说明 | — | 单元通过 |
| X7 | 实现 (a)：guest 探测时按判定建链接（wsl2、vz） | X6 | 新建的 carrier 上 X1 的安装用例通过 |
| X8 | guest 的更新：经已有的 `grant` 交付、替换两个程序、立即比较版本、失败不记为已试、`luban status` 的一行 | — | 换版本的用例通过，不联网 |
| X9 | userland 修订：修订号、步骤、探测时补齐 | X7、X8 | 旧镜像的用例通过 |
| X10 | 实现 (b)：`create_executor` 的条件、worker 的"链接"挂载 | X6 | u-alpine 上三条路径的夹具用例通过；现有场景不退步 |
| X11 | 实现 (b)："合并"（`TmpOverlay` 加逐条绑定） | X10 | u-alpine-arm 上通过 |
| X12 | 做不到时的行为与说明接入 hook 失败的错误 | X6、X10 | 没有 worker 的机器上，安装照旧，失败时有说明 |
| X13 | 系统根：`status --check` 的同源一格；闭包检查拒绝非运行时 glibc 的单元测试（合成本身不改） | — | 单元通过；现有 edition 的根不变 |
| X15 | 安装器：发布失败在回滚后返回原始失败（两个分支）；错误里带被拒绝的 interpreter | — | `INSTALL-PUBLISH-FAILURE` 的用例在修复前失败、修复后通过 |
| X14 | 文档与记录（§11 的清单） | 全部 | 文档检查通过 |

```
X1 ─┬───────────────> X4 ──> X5 ──────────┐
X2 ─┤                                     │
X3 ─┘                                     │
X6 ─┬─> X7 ──┬─> X9 ──────────────────────┤
    │   X8 ──┘                            ├─> X14
    ├─> X10 ──> X11 ──┐                   │
    └────────────────>└─> X12 ────────────┤
X13 ──────────────────────────────────────┤
X15 ──────────────────────────────────────┘
```

- 三条线互不依赖：透传（X2–X5）、契约（X6、X7、X10–X12）、guest（X8、X9，X9 需要 X7 的链接作为修订 2 的内容）。
  X13、X15 各自独立。
- 建议顺序：X1；X2–X5（之后 WSL 2 上的任何失败都能直接读到原因）；X6–X9（#724 至此修复，含已有的 carrier）；
  X10–X12；X13；X15；X14。

### xim-pkgindex（一个 PR）

| 编号 | 任务 | 依赖 | 完成的判据 |
|---|---|---|---|
| P1 | bun：`--ignore-scripts`，配方选定并放置平台包，`revision`；测试运行改写后的 `bun --version`（x64、arm） | — | 索引的安装测试通过；Alpine 容器与同构的根里安装成功 |
| P2 | npm、dsh 在同构的根里安装；dsh 缺的工具声明为依赖（其余配方的结论见 §5） | — | 两个配方有结论 |
| P3 | 索引 CI：Alpine（x64、arm）、最旧 glibc 宿主、同构根（luban 各 edition 的包）上的安装测试 | P1、P2（否则它先红） | 检查在 CI 中运行并通过 |

### 跨仓库与发布

| 编号 | 任务 | 依赖 |
|---|---|---|
| R1 | xlings 发布（版本、镜像、索引 bump 按 AGENTS.md） | xlings PR |
| R2 | luban-fresh 增加 WSL 2 上的 agent-workspace | R1（或 P1 先到） |

- xlings PR 与索引 PR 之间没有依赖，可并行。
- 用户侧：索引 PR 发布后 bun 立即可装。xlings 发布后，Windows 侧更新、下一次使用时 guest 被带到同版本、
  userland 补上链接，全程不需要 guest 联网。
- **发布当次**：把 guest 带到新版本的是新客户端，所以已有的 carrier 在这一次也由新的方式更新，
  不依赖旧 guest 里的任何东西（挂载与替换文件用的是 guest 的 shell；W1 用上一版的 guest 实测）。
- edition 不需要新的 `min_client`：两边的修复都不改变 edition 的内容。

## 9. 已定的取舍与重新设计的条件

两处取舍已确认（2026-11-22）。它们是在当前前提下最好的，前提变了就该重看；这里写明前提与信号。

**取舍 1：根已有 glibc loader 时，hook 用根自己的运行时。**

- 为什么是现在最好的：三个候选里，"换成环境的、保留宿主 userland"实测不可行（L7）；"所有 hook 进环境 userland"
  会使索引里面向宿主的配方失效（已知 8 个，其余未测）；"按宿主 glibc 的新旧决定"的条件无法事先求值（§6）。剩下的这一个不改变任何
  今天能运行的东西，实测最旧到 glibc 2.27。
- 它的代价：hook 里启动的程序若要求比宿主更新的 glibc，会失败，并得到 §4.1 的那句说明。
- 什么时候重看：
  - 索引完成 G2（配方能声明"面向宿主"或改为声明式的步骤如 `system.admin_step`，且 P3 的同构根测试覆盖了整个索引）。
    届时"环境 userland"成为可用的统一模型，L6、L9 是它的可行性依据，worker 的根换一种组成即可。
  - 出现真实的失败报告：某个包的 hook 在旧 glibc 的宿主上因 glibc 版本失败。先用 §4.1 末尾的两个办法改配方；
    同类报告成批出现才是模型的信号。
  - xlings 不再支持 glibc 早于某个版本的宿主时，代价本身缩小。

**取舍 2：没有 glibc loader 的宿主上，每个包的 hook 都经 worker。**

- 为什么是现在最好的：判定是按根做的，不是按包做的——xlings 无法事先知道哪个 hook 会启动下载来的程序。
  只给"需要的包"开 worker 就得让配方声明，而这类宿主上以前这些包本来就装不上。worker 是已有的、
  有 policy 的 SubOS 每天在走的路径。
- 它的代价：每个包多一次 worker 启动；hook 在一个 user namespace 里（`sudo` 一类不可用，而配方本就不得运行它）。
  hook 看到的文件系统不变（§12：worker 呈现这台机器本来的样子）。
- 什么时候重看：
  - host-matrix 的 u-alpine、u-alpine-arm 出现成批的退步，且原因是 worker 的限制而不是配方的假设。
  - 配方有了声明"hook 里要运行自己的 payload"的字段：那时可以只给声明了的包开 worker。
  - 取舍 1 走到"环境 userland"时，这一条随之并入（所有宿主都经 worker）。

**已确认本次不做、只作记录的两个形状**（2026-11-22；内容与代价在 §6）：

- carrier 里也一律经 worker、不带链接。重看的时机：G2 使 worker 成为常态；或 userland 修订的维护成本变得明显。
  动它之前先在真实的 carrier 上把 worker 路径测通。
- Linux 的私有域改用与 host machine 同构的根。重看的时机：同构根的测试仍然漏掉只在某个平台出现的问题；
  或私有域的"构造"与"运行"被拆成两个根。

实测中留下的、本设计不改变的现状（记录在此，供以后查）：

- 一个 Luban 系统里不能并存两个 glibc 版本（L8）。按 `abi = "exact"` 固定旧 glibc 的包在系统里是否因此装不上，
  没有测；有这样的包出现时从这里查起。

## 10. 评审记录

第一轮（对照代码的审阅）推翻与补充的：

| 初稿的说法 | 实际 | 处理 |
|---|---|---|
| carrier 里 hook 在 bwrap 的 worker 里运行，往沙箱加一条挂载即可修复 #724 | 没有 policy 时 hook 在进程内运行（`lua_boundary.cpp:983`）；carrier 里造系统正是这种情形 | §3 的四种环境；carrier 的链接 |
| carrier 的根里加链接要维护随 abi 变化的持久状态 | 链接指向 `<home>/abi/<family>`，是常量 | 采用 |
| 新写事件解码与流式转发 | 私有域的路径已有（`relay_capability_`） | 复用 |
| 接口调用方一律走转发 | `exec`、`use`、`start`、`log`、`boot`、`__broker-serve` 交出终端 | 例外清单 |
| guest 没有给出 `result` 时退回终端转发 | 会重跑执行到一半的 `subos new` | 调用之前决定，之后不回退 |
| 旧 guest 会先被带到当前版本 | 十分钟的间隔，每个版本只试一次，失败也记为已试 | §4.4 |
| 判定经 `Intent` 的接缝注入 | 该接缝在 `lua_boundary.cpp` 里不可达 | 纯函数自带探针参数 |

第二轮（评审意见：统一模型、覆盖全部情形、本地方案、不延后）改动的：

| 第二稿 | 本稿 |
|---|---|
| 三处各自的做法（镜像链接、私有域 `--symlink`、worker 绑定） | 一条契约、一个判定、两种实现（根自己带着 / worker） |
| 根不是 xlings 的宿主上的普通安装：只说明 | 由 (b) 覆盖 |
| 标准目录已被占用：只说明 | "合并"（`TmpOverlay`） |
| 已有的 carrier 由宿主侧补链接，绕开 guest 的更新 | 修 guest 的更新本身（宿主交付），userland 修订由 guest 补齐 |
| `refresh_guest` 失败也记为已试：另行处理 | 并入 §4.4 |
| 从 Windows 读 guest 的日志：另议 | 日志取回，不新增命令 |
| 系统里两处 loader：只加检查 | 按构造同源，再加检查 |

第三轮（评审意见：未实测的各项用临时 PR 查清，"宿主已有 loader"给出架构级的结论）改动的：

| 第三稿 | 本稿 | 依据 |
|---|---|---|
| 契约："经标准路径到达环境的运行时"；宿主已有 loader 是"唯一不并入的" | 契约："标准路径上有一个运行时，根有的用根的，没有的由环境补上"；宿主已有 loader 是契约的第一句 | L7、索引里面向宿主的配方、Ubuntu 18.04 |
| 没有 loader 的宿主会失败：推断 | 实测失败（Alpine x64、arm） | L1 |
| 链接、合并、私有域里再起 worker：未实测 | 均可行；统一用只读绑定 | L3、L4、L5 |
| guest 经交付的目录执行 `self install` | 替换两个文件；endpoint 加"只读呈现宿主目录" | W1 |
| 取消：可能要向 guest 发终止 | 不需要 | W2 |
| 系统根：改合成顺序使两处 loader 同源 | 合成不改；加检查与单元测试 | L8 |
| L8 的第一次解读："第二个 glibc 被闭包检查拒绝" | 被拒绝的原因是非运行时 glibc 的程序没有被改写（2.39 与 2.44.2 都如此）；结论不变，理由更正 | run 44921451722 |
| 安装报告成功而根没有更新：列为另议 | 根因定位在安装器的回滚分支，并入本设计（§4.5、X15） | L8、`installer.cpp` |
| bun：`--ignore-scripts` 后把平台包放到位 | 配方还要在四个平台包里选一个（glibc；AVX2 或 baseline） | L2、W3 |
| 包侧规则"hook 不执行未改写的 ELF"，索引 CI 用没有 loader 的环境检查 | 不设禁止规则；索引 CI 在 Alpine 与最旧 glibc 宿主上测 | 契约成立后前者无法检查，也不再必要 |
| 同类配方"待核对" | 17 个配方有实测结论，剩 npm、dsh | L6 |
| "不带宿主 userland 的根"未讨论 | 实测可行，记为不采用并写明前提（G2） | L6、L9 |

第四轮（从 xlings、Luban、操作系统三个架构的角度自审）改动的：

| 角度 | 发现 | 处理 |
|---|---|---|
| 操作系统 | aarch64 的标准目录 `/lib` 是通用目录，WSL 2 运行时自己建它并挂 `/lib/modules`；"carrier 的根都没有 `/lib`"不成立 | (a) 的条件改为"标准目录不存在"；链接由 guest 探测时按状态建，不写死在镜像里；已被占用时落到 (b) |
| 操作系统 | musl 宿主上 gcompat 占着标准路径时，判定是"已有" | 实测（L10）bun 的 hook 成功；保持"根有的用根的"，说明里指出它不是 glibc |
| 操作系统 | 替换 guest 的程序时有进程在运行 | 改名替换，运行中的进程不受影响，与现在的 `self update` 相同；写明 |
| xlings | 上一稿要给 carrier 的 endpoint 加"呈现宿主目录"的调用 | 已有 `grant`（`/grant/<name>`），直接用 |
| xlings | worker 的挂载在编译之后追加，绕开了"沙箱的形状是 golden"的规则 | 追加的挂载由纯函数给出，有自己的固定用例 |
| xlings | 接口的参数经 `wsl.exe` 的命令行传 JSON | X3 用难处理的参数测；不稳就走 `--args-file` 与 `grant` |
| xlings | "15 个配方用到 `sudo`"是按文本数的，把依赖 `sudo` 这个包的 edition 也算了进去 | 更正为 hook 面向宿主的 8 个；"环境 userland 不能统一"的依据改为"需要声明字段与全索引的数据" |
| Luban | 同一个 edition 的 hook，在 Linux 上看到宿主的 userland，在 Windows、macOS 上看到 host machine 的；Linux 的 CI 因此看不到 host machine 才有的问题 | 每个 PR 的 Linux CI 加同构根的用例；索引 CI 加同构根一道 |
| Luban | "carrier 也走 worker"与"私有域用同构根"是两个更统一的形状 | 都记为不采用并写明代价与重看的条件（§6） |
| Luban | luban 一侧是否要改 | 不改：luban 只读结构化事件，转发之后 Windows 上得到的事件与 Linux 私有域路径相同；层次规则（`luban/` → `modules/`，`modules/carrier` 不引用 `src/`）不受影响 |

多视角检查：

| 视角 | 结论 |
|---|---|
| 运行时模型 | payload 不依赖某个环境：只改变 hook 执行时所见的根；payload 的 `PT_INTERP` 仍由 elfpatch 写为 abi 路径。契约把"运行时是环境的属性"延伸到安装期 |
| 文件系统布局 | Luban 系统：不变。carrier：多一个常量链接，仍然全静态、不带任何库。宿主：不写任何东西 |
| 一致性 | carrier、Luban 系统、补上运行时的 worker 三处，标准路径都指向环境的运行时；根已有运行时的宿主用它自己的；E1–E3 由同一段代码判定 |
| 层次 | `relay` 在 `modules/carrier`，以 JSON 文本为界；解码、判定、家族表在 `src/`；镜像的链接条目在 `src/core/subos/carrier_image.cpp`（busybox、bwrap 的条目同在此处），`modules/carrier` 不需要知道家族 |
| 平台规则 | 不新增 `#if`；不使用宿主包管理器；"宿主有 glibc loader"这个未声明的假设，在契约覆盖的地方由索引里的 glibc 满足 |
| 结构化输出 | carrier 与私有域两条转发路径行为一致；兜底只在真正没有细节时出现 |
| Agent 模式 | 转发的接口调用没有终端；`prompt` 成为带选项的错误；guest 更新失败是带原因的错误，不等待 |
| 安全 | carrier 的链接属于 root，指向同属 root 的 home。worker 追加的是只读绑定与内存里的 overlay，hook 结束即消失，没有新的可写的宿主路径。日志取回只读 guest 的 `<home>/logs/` 之下。交给 guest 的构建是宿主按 sha256 校验过的那一份，只读授予 |
| 兼容 | 宿主已有 loader：一切不变。没有的宿主：以前这类 hook 必然失败；其余 hook 现在经 worker（待定 2）。起不了 worker：与今天相同，多一句说明 |
| 升级 | 新客户端对任何旧 guest：先交付同版本的 xlings 再调用（只用 guest 的 shell，上一版实测）。guest 比客户端新：不动 |
| 可观测 | 失败时用户看到 guest 的错误、hook 输出末尾、本地的日志路径；`luban status` 显示 guest 的版本与上次更新的结果；被遮住的 family 条目在 hook 日志里 |
| 性能 | 标准路径上已有运行时的机器没有任何额外开销。其余：每个包一次 worker 启动；合并时几十条只读绑定 |
| 测试 | 判定与合成是纯函数；`relay` 对 `local` endpoint；真实环境在 WSL 2 一道（PR）与 u-alpine、u-alpine-arm（dispatch） |
| 未来演进 | G2（hook 的根 = 声明的依赖闭包）在同一条轴上：届时外来的根不再作为 hook 的 userland，worker 的根换成环境 userland（L6、L9 已测得它可行），判定与 (a) 不变 |
| 仍不确定 | carrier 里 hook 在进程内运行是读代码得到的（L6 与之相容）；vz 上把目录呈现给 guest 未实测（macOS 的 CI）；改写后的 bun 在 aarch64 上是否正常未实测（P1 的测试）；npm、dsh 在没有宿主工具的根里未实测（P2）。实测用的是容器里的 Alpine，内核是 runner 的 |

## 11. 记录在哪里

这些决定要在以后查得到、重新设计时用得上。随实施的 PR 一起落下（X14）：

| 位置 | 写什么 | 给谁看 |
|---|---|---|
| 本文 | 证据（§2）、契约（§4.1）、不采用的方案与各自的依据（§6）、取舍的前提与重看的信号（§9）。实施后状态改为"已实施"，并补上实施时与本文不同之处 | 重新设计的人 |
| `AGENTS.md`，"The runtime is the environment's" 一条 | 加一句：hook 运行的根在标准 loader 路径上有运行时——根自己的，没有时是环境的；指向本文 | 改代码的人与 agent |
| `.agents/docs/2026-08-08-host-loader-migration-and-ecosystem-guards.md` 的 G2 | 加一行指向本文 §9：安装期的同一个缺口、"环境 userland"的实测结果、它的前提 | 接着做 G2 的人 |
| `.agents/docs/2026-10-14-luban-os-part3-design.md` §4.2 | 加一行：系统里标准路径与 `/xlings/abi` 同源由闭包检查保证，一个系统一个 glibc（本文 §4.3） | 改系统根的人 |
| `docs/30-authoring-a-package.md`（及 zh） | hook 的运行时：hook 看到的是包所安装进的系统；标准路径上的运行时是谁的；在 hook 里运行自己的 payload 的两个办法；旧 glibc 宿主的限制 | 写配方的人 |
| `docs/13-subos-on-macos-and-windows.md`、`docs/luban/07-host-support.md`（及 zh） | host machine 里装包与 Linux 宿主的差别（没有宿主的工具，只有 busybox 与环境的运行时）；错误与日志在本机哪里；guest 如何保持与客户端同版本 | 用户 |
| `docs/51-supported-platforms.md`（及 zh） | 没有 glibc loader 的宿主（Alpine 一类）上 hook 经 worker 运行，需要可用的 user namespace；做不到时的表现 | 用户 |
| xim-pkgindex 的 `docs/contributing.md` | 带原生二进制的配方：优先直接取平台归档（esbuild、codex、bun 为例）；索引 CI 的两种宿主各挡什么 | 写配方的人 |
| 代码注释 | 判定函数处写契约的一句话与本文的文件名；`carrier_image.cpp` 的链接条目处写它为什么是常量；`installer.cpp` 两个回滚分支处写"回滚成功不等于安装成功" | 读代码的人 |

## 12. 实施记录（2026-11-22，2026.11.22.1）

实施与上文不同之处，及其原因：

| 上文 | 实施 | 原因 |
|---|---|---|
| guest 在 hook 失败时多发一个 data 事件给出日志路径 | 不新增事件：转发处从 guest 的错误文本里读 `full log: <路径>`（xlings 自己写的格式），经 guest 的 busybox 读出，存到 `<home>/logs/carriers/<carrier>/`，并替换消息里的路径 | 对已发布的旧 guest 同样成立；接口不加东西 |
| guest 用自己的 shell 复制、改名 | 交付的新构建自己完成替换：`<grant>/xlings __carrier-adopt /xlings/bin`（先 luban 后 xlings，各自复制到旁边再改名） | 不依赖旧 guest 里有什么工具；旧 guest 只需要已有的 `__carrier-grant` 与 `__carrier-probe` |
| "链接"把 family 目录只读绑定到标准目录 | 标准目录建为空目录，family 的每个条目解析到它的文件后逐个只读绑定（"合并"同样逐条） | family 里的条目是指向 store 的绝对链接，worker 里 home 不一定可见；链接与合并成为同一个纯函数的两个分支（`xlings.subos.std_loader::mounts`） |
| 判定四种结果 | 同；标准目录是指向不存在之处的链接时为"无可提供" | 这种目录既不能挂上也不能挂进 |
| 做不到时的提示 `xlings self doctor --isolation --fix` | `xlings self doctor --isolation`（说明这台机器缺什么） | `--fix` 只处理 AppArmor 的情形；Alpine 上缺的是 bwrap（`xlings install bwrap`） |
| guest 的构建版本即客户端版本 | 取到的构建旁记 `.xlings-release`；客户端版本不在索引里时（候选、开发构建）取到的是最新发布的版本，与它相同的 guest 视为当前，记 `current_for` | 否则开发构建会每次都替换 guest |
| Linux 的同构根用例另造一个根 | 用 carrier 真实的镜像：桩 `wsl.exe` 导入到目录，bwrap 进入（`e2e/test_hook_runtime`，acceptance 一道，静态的发布构建）；(a) 链接、(b) 去掉链接后经 worker。(b) 只在这个根里能再起 bwrap 时断言（Ubuntu runner 的 AppArmor 不一定允许），(b) 的真实覆盖是每个 PR 的 u-alpine 一道 | 同构即同一个镜像；不在测试里复制镜像的组成 |
| (b) 的 worker 用 `minimum_layer_policy` 编译出的沙箱（home 只读、私有的 `HOME` 与 `/tmp`），再追加挂载 | 只为补运行时而起的 worker 看到的是这台机器本来的样子：`/` 的每个条目在原处、按这个用户原有的权限可写，再加 `/dev`、`/proc`、`/sys` 与标准路径的挂载；有 policy 的 SubOS（E3）仍是它编译出的沙箱加这条路径。取舍 2 的代价（hook 的环境变了）因此不存在 | PR 的 u-alpine 一道实测：按 policy 编译的 worker 里，ca-certificates 的 config hook 写不进正在造的系统的 sysroot（`luban new` 失败）。契约只要求"这个根的 userland 加上这条路径"，不要求隔离 |
| bun 的配方只改为不跑 postinstall | 另外：aarch64 上只写 interpreter、不写 RPATH（`elfpatch.set_interpreter` 后 `elfpatch.skip()`） | 索引 CI 在 aarch64 的 host machine 根里运行改写后的 bun：SIGSEGV；本地 qemu-aarch64 复现，只写 interpreter 时运行、加 RPATH（任一顺序）时段错误（patchelf 移动了 `.dynamic`）。这是已有的问题（xim-pkgindex#1215 也遇到），与 postinstall 无关 |
| u-alpine、u-alpine-arm 只在定时与手动的全矩阵里 | u-alpine 加入每个 PR 的 smoke；新场景 `hookrt`（普通 home、有 policy 的 SubOS；glibc 早于 2.34 的宿主断言失败的说明） | 没有 glibc loader 的宿主每个 PR 都测到 |
| `luban status` 的 host machine 一行 | sandbox 一行的 how 带上 guest 的 xlings 版本与没有成功的更新；JSON 的 traits 有 `guest`、`guest_update` | 报告只有这一处说 host machine |
| 安装的发布失败返回原始失败 | 消息为 `<包>: not published: <原因>`，回滚自身的失败接在后面；闭包检查的错误带上被拒绝的 interpreter | 同上文 |

代码位置：判定、挂载与说明 `modules/subos/src/sandbox/std_loader.*`；worker 的接入 `src/core/xim/lua_boundary.cpp`
（`hook_runtime_`）；guest 的更新、userland 修订与 `relay` `modules/carrier/src/carrier.cpp`；转发与日志取回
`src/core/subos/carriers.cpp`；共用的解码 `src/core/subos/cmd.cpp`（`relay_line_`）；安装器 `process_xvm_operations_`；
系统根的检查 `modules/subos/src/views/root/root_check.cpp`（`runtime` 一格）。
