# Luban 用户态 OS 与生态设计，以及基于 Luban 的 Agent Workspace

- 日期：2026-10-09（第四稿：吸收三轮 review 意见；新增 luban CLI 的完整设计和交付方式，附自我 review）
- 状态：待 review
- 关系：
  - 承接 SubOS 设计 part 1、part 2、part 3（`2026-10-05`、`2026-10-06`、`2026-10-09-subos-architecture-design-part3.md`）；
  - 给出 xim-pkgindex #945 的重构方向；
  - 吸收 `subos new --from subos:luban-core` 实际使用中暴露的问题。
- 一句话：**Luban 是一个最小 OS 模型，也是一个可组合的 OS 概念。** 内核之上必需的只有 xlings、luban 和 luban-init，其余各层都可以选。用 xlings 组合出来的就是「基于 Luban 的发行版」。同一份组合既能在已有系统里真实使用，也能作为 ISO 或磁盘在真机上运行，两种方式由同一个 `luban` 工具管理，用法一致。

**体验原则先行：** Luban 不能让人觉得像 Docker 一样复杂。复杂度渐进式暴露；稳定、兼容；面向人和面向 agent 的交互分开处理；每一条体验承诺都有 CI 覆盖。

本文分为五部分：
- **A 技术视角**：架构、模型、工具；
- **B 用户体验视角**：习惯、便利、交互、渐进式暴露、人和 agent；
- **C** 具体场景：Agent Workspace；
- **D** CI 覆盖、计划和待定事项；
- **E** 自我 review。

---

## 0. 结论

| # | 决定 | 视角 |
|---|---|---|
| D1 | 最小层叫 **luban-nano**：xlings（静态）+ **luban**（静态，包含 luban-init 这个入口）+ 一个声明的首个程序，**不含 sh** | A |
| D2 | **用户态 edition 不带内核**。内核是一个包；edition 只声明 ABI、推荐内核和最低内核要求 | A |
| D3 | 机器（machine 视图）在需要时组合内核；R 模式下内核进入代，与用户态一起回滚 | A |
| D4 | ABI = CPU 架构 × 内核 ABI × libc 族 × libc 最低版本；静态 payload 写作「无 libc」。**模型里不写死任何 libc** | A |
| D5 | 四种交付形态（在线模板、rootfs、虚拟机磁盘、ISO）来自同一份锁定记录，版本一致 | A / B |
| D6 | **`XLINGS_HOME` 是基础模型的一部分**，在任何形态中都提供：Luban 机器默认是 `/xlings`；只在 home 下用 xlings 时仍是 `~/.xlings`。根视图在挂载命名空间里把 store 映射到 `/xlings`，根内链接写逻辑路径 | A |
| D7 | ISO 的引导器用 **limine**。`limine`、`squashfs-tools`、`xorriso` 作为 xpkg 加入 xim-pkgindex，经工具表解析 | A |
| D8 | 「基于 Luban 的发行版」 = 从 luban-nano 继承、用 xlings 组合出的任意 edition；官方维护一组高度可用的发行版（包括桌面）及其 ISO | A / B |
| D9 | **`luban` 是专门的 Luban OS 管理工具**，是一个独立的静态二进制：在 luban-nano 里，在每台 Luban 真机上，也可以装在宿主上，管理宿主上的 Luban 环境。系统信息、状态、环境、启动项、内核、导出都由它负责；**包管理仍是 xlings**。luban 经 xlings 的 NDJSON 接口驱动 SubOS 操作，不复制实现 | A / B |
| D10 | **Luban 相关的工具和包使用日期版本号** `YYYY.M.D.N`（N 从 1 开始，`.0` 保留），与 xlings 相同。包括 luban 工具、各 edition、策略包、Luban 镜像 | A |
| D11 | 根视图首选 bwrap。**proot 降低优先级但保留**，作为特殊环境的兜底：从不被悄悄选中，只在显式指定时使用，并标明「视图，不是安全边界」；私有策略拒绝 proot | A / B |
| D12 | 首次使用：**先检查宿主，再开始下载**；需要的一次性设置**当场请求 sudo 密码**，经提权的唯一入口执行并记录；一个操作一个进程、一个渲染器；模板对用户不可见 | B |
| D13 | **面向人的交互和面向 agent 的交互分开**：同一个核心，人看到进度、确认和一句话的结论；agent 拿到 JSON 和结构化错误，从不被提问 | B |
| D14 | Agent 场景的 edition 叫 **`subos:luban-agent-workspace`**。长期使用的隐私是一个**稳定、中性、与你分离的身份**；时区可配置，默认跟随代理出口 | C |
| D15 | Agent 隐私分两档：L2 共享内核的沙箱（第一版，如实报告内核和 CPU 指纹）；L3 同一 edition 在 virt 内核虚拟机里运行 | C |
| D16 | 每一条体验承诺（不挂起、一个进度、层次 0 不出现内部术语、agent 模式不提问、拒绝时有路线）都有 CI 覆盖（D 部分） | D |
| D17 | **luban CLI 用 mcpplibs.cmdline 解析，从一份声明式的命令规格生成帮助、补全和参考文档**；命令和选项分为「常用 / 更多 / 专家」三级，帮助按级别展开（B3） | B |
| D18 | Luban 的小功能都是一个动词：`luban export box box.iso` 制作镜像，`luban write box.iso /dev/sdX` 制作启动盘，`luban install /dev/sdX` 安装到磁盘，`luban try box` 在虚拟机里试运行 | B |
| D19 | 整套方案是**生态级的**，但**每个仓库只用一个 PR 实现**，用 commit 保留检查点；仓库之间通过 fixture 解耦，按顺序合入（D2） | D |

---

# A 技术视角

## A1 定位与分工

- **最小 OS 模型**：内核之上必需的只有 xlings（包管理、组合）、luban（OS 管理）和 luban-init（第一个进程）。
- **可组合 OS 概念**：libc、init、基础工具、shell、桌面，每一层都是 xlings 的包。
- **基于 Luban 的发行版**：从 luban-nano 继承、用 xlings 组合出来的任意 edition。

| | xlings | luban |
|---|---|---|
| 职责 | 包：安装、使用、移除、索引、payload、代的生成；SubOS 的引擎 | OS：系统信息与状态、环境（新建、进入、运行、列表、历史、回滚、导出、删除）、宿主设置、启动项、内核 |
| 运行在哪 | 任何宿主；每个 Luban 环境和真机里 | luban-nano 里（每个 Luban 环境和真机都有）；也可以装在宿主上（`xlings install luban`） |
| 代码 | `modules/`（`xlings.*`）、`src/` | `luban/`（`luban.*`）+ `apps/luban`（新增，独立的包） |
| 与对方的关系 | 不依赖 luban | 环境类操作经 `xlings interface`（NDJSON）驱动；OS 类操作（启动项、内核、stage 0、`/etc`）自己实现 |
| 版本 | `YYYY.M.D.N` | `YYYY.M.D.N`（D10） |

### A1.1 luban 二进制的构成
- `apps/luban` 是独立的包，只链接 `luban/` 和 `modules/`，**不链接 `src/` 前端**（与 `apps/luban-init` 相同，由 `lint_layer_deps` 保证）。
- **luban-init 成为 luban 的一个入口名**（多调用）：以 `luban-init` 被调用时执行 stage 0。stage 0 的约束不变：在它的路径上不读 Config。
  - 这样 nano 中是两个二进制：xlings 和 luban。
  - `apps/luban-init` 的代码合入 `apps/luban`，原有的 `bin/luban-init` 链接保留，以保持兼容。
- 环境类操作经 `xlings interface <capability>` 完成：
  - luban 是这个接口的第一个「外部」使用者，接口因此被持续验证；
  - SubOS 的逻辑只有一份实现；
  - 版本错配用 `xlings interface --version` 握手：luban 声明最低协议版本，不满足时给出升级路线。

## A2 模型

```
内核（linux generic / linux virt / 将来 Luban 内核，经 openkal 提供同一 ABI）
└── luban-nano：xlings（静态）+ luban（静态，含 luban-init）+ 首个程序
    └── 可选层：libc · init / 服务管理 · 基础工具 · shell · 网络 · 桌面 ……
        ├── 官方 edition：tiny → core → desktop / agent-workspace
        └── 用户 edition：subos:my-os（自己的索引）
交付形态：在线模板 / rootfs / 虚拟机磁盘 / ISO（同一份锁定记录）
运行：内容 × 视图（overlay / sandbox / root / machine）× 承载（local / wsl2 / vz / native）
管理：luban（OS 与环境）+ xlings（包）
```

一个 edition 是一个 `type = "subos"` 的 xpkg。它的 `install()` 写出模板，内容包括：
- `.xlings.json`：`subos_kind`、`from`、`packages`、`abi`、`boot`、`policy`；
- `usr/share/factory/`：只补缺、不覆盖的 `/etc` 文件和 home 模板。

## A3 Arch / Alpine 参考

| | Arch | Alpine | Luban |
|---|---|---|---|
| 基础层是否带内核 | `base` 不带；`linux` 系列是包 | `alpine-base` 不带；`linux-lts`、`linux-virt` 是包 | 不带（D2） |
| 不带内核的形态 | bootstrap tarball、Docker、WSL | minirootfs | rootfs；在已有系统里直接使用 |
| 带内核的形态 | 由用户安装 | 按场景的 flavor | 虚拟机磁盘（virt）、ISO（generic） |
| ISO | archiso 的 live 环境，用 pacstrap 安装 | live 系统，用 setup-alpine 安装 | live 环境就是一台 Luban 机器，用 luban 安装 |
| 管理工具 | pacman + 一组零散的工具 | apk + setup-* 脚本 | xlings（包）+ luban（OS），宿主上和机器里都是同一个 luban |
| 一致性的来源 | pacman 和仓库 | apk，版本号贯穿所有形态 | xlings + luban、同一份锁定记录、日期版本 |
| 不足 | live 环境与装好的系统不完全相同 | 无盘模式是另一套心智模型 | live 环境与装好的系统是同一棵树 |

## A4 luban-nano

| 组成 | 说明 |
|---|---|
| xlings | 静态，不依赖 libc；包管理与组合 |
| luban | 静态，不依赖 libc；OS 管理；以 `luban-init` 被调用时执行 stage 0 |
| 首个程序 | edition 声明的 `boot.init`，nano 里可以是任意静态程序 |

nano 能否成立，取决于以下要求：
1. **xlings 和 luban 都不依赖 `/bin/sh`。** `spawn_command` 和 recipe 的 `system.exec` 现在走 `sh -c`，需要能直接 exec argv。需要 sh 的 recipe hook 把 sh 声明为依赖。
2. **stage 0 之前什么都不读**（part 2 已有约束）。
3. **静态二进制覆盖所有 CPU 架构**：x86_64、aarch64 已有；riscv64、armhf 随嵌入式需求加入。
4. **将来面向 openkal**：对内核的依赖只经过 platform 层。

tiny = nano + BusyBox + glibc + CA 证书 + busybox init。

## A5 ABI（不写死 libc）

```json
"abi": { "arch": "x86_64", "kernel": "linux", "libc": "gnu", "libc_min": "2.34" }
```
- `libc` 取值为 `gnu`、`musl`、`uclibc`、`bionic`……或 `none`（静态）。
- 简写与 mcpp 的目标三元组一致。
- 解析器只选与根的 ABI 匹配的变体，不匹配就拒绝并说明缺的是哪个变体；`none` 适用于任何根；自带 loader 的 payload 需要显式允许才能共存。
- part 3「payload 的 ABI 决定承载」原样适用。
- 支持哪些 libc，取决于索引里的变体；第一批只做 glibc。

## A6 内核

1. 内核是普通的包：`xim:linux-kernel`、`xim:linux-kernel-virt`，以及将来的 Luban 内核。每个内核包声明自己提供的 ABI 和特性。
2. edition 只给提示：`"boot": { "init": "/sbin/init", "kernel": "xim:linux-kernel-virt@<v>", "kernel_min": "5.10" }`。
3. machine 视图（`luban export --disk` / `--iso`、本地启动）把推荐内核装进导出的那一代，可以用 `--kernel` 覆盖；不满足 `kernel_min` 就拒绝。
4. R 模式：内核作为显式安装的包进入工作区，跟着代走。升级内核的流程：产生新的一代 → `luban boot --once` 试启动 → `luban boot --mark-good` 确认 → 失败自动回退。
5. 实例、容器、wsl2、vz：永远不装内核。

## A7 XLINGS_HOME 与 store 路径

| 场景 | `XLINGS_HOME` | 根内链接指向 |
|---|---|---|
| 只在 home 下用 xlings（overlay 视图） | `~/.xlings`，不变 | 不涉及 |
| 根视图（在挂载命名空间里） | 进程内看到 `/xlings`，宿主的 store 绑定到这里 | `/xlings/...` |
| Luban 机器（R 模式、ISO、磁盘）、M 形态 | `/xlings` | `/xlings/...` |

- 根视图的内容与机器形态逐字节相同，宿主路径和用户名不会出现在根里，也不会出现在导出的镜像里。
- 迁移：旧代仍指向宿主路径。新客户端刷新时产生使用逻辑路径的新一代；回滚到旧代时同时绑定两个路径，并在 `status` 中标出「这一代暴露宿主路径」；私有策略拒绝进入这样的代。

## A8 隔离后端

- **根视图首选 bwrap。** 在默认限制用户命名空间的发行版上（Ubuntu 23.10 及以后），需要一次系统级设置：root 所有的 bwrap 加一个范围很窄的 AppArmor profile。
- **proot 降低优先级，保留作兜底**（D11）：
  - 选择器**从不悄悄选中 proot**。只在用户或 agent 显式指定 `--sandbox proot` 时使用；
  - 用于特殊环境：没有用户命名空间、也无法设置的容器或 CI；
  - 输出和 `status` 都明确标出「视图，不是安全边界」；
  - `agent-private`、`no_degrade` 以及任何声明了 `needs: must` 的策略都拒绝 proot。
- **已知的不一致**（本轮发现）：
  - 旧会话里的 bwrap 带着 `busybox (unconfined)` 这个 AppArmor 标签。Ubuntu 24.04 的 busybox profile 允许用户命名空间，bwrap 从启动它的进程那里继承了这个标签；从普通 shell 启动的进程是 `unconfined`，会被拒绝。
  - 因此能否创建根取决于启动方式。
  - 处理：
    - doctor 报告探测进程的标签，并说明结论只对这个上下文成立；
    - 选择器不依赖继承来的标签；
    - 系统设置之后，所有上下文的结论一致。
- 没有用户命名空间的宿主：根视图的路线是显式的 proot 兜底，或者 machine 视图（P4）。

## A9 交付形态与 ISO

| 形态 | 产物示例 | 内核 | 命令 |
|---|---|---|---|
| 在线模板（在已有系统里真实使用） | `subos:luban-core@2026.10.20.1` | 无 | `luban new box` |
| rootfs | `luban-core-2026.10.20.1-rootfs-x86_64.tar.zst` | 无 | `luban export box box.tar.zst` |
| 虚拟机磁盘 | `luban-core-2026.10.20.1-virt-x86_64.qcow2` | linux-kernel-virt | `luban export box box.qcow2`（或 `.img`） |
| ISO | `luban-core-2026.10.20.1-x86_64.iso` | linux-kernel | `luban export box box.iso` |

ISO 的构成：

| 组成 | 来源 |
|---|---|
| 引导器 limine（UEFI 和 BIOS） | `xim:limine` |
| 内核 | 推荐内核或 `--kernel` |
| initramfs | `luban-init` 作为第一阶段 |
| 根 | 当前代的只读 squashfs |
| 制作工具 | `xim:squashfs-tools`、`xim:xorriso`，经工具表解析 |

- live 环境的根就是只读的那一代，可写部分放在 tmpfs；live 环境与装好的系统是同一棵树。
- 安装用 `luban install <设备>`：分区、ESP、limine、写入根的代；制作启动盘用 `luban write <镜像> <设备>`（B3.5）。
- limine 只读 FAT 和 ISO9660，所以装好的机器每一代的内核由 `luban boot` 复制到 ESP。
- 第一版不做无盘模式；Secure Boot 放到后期。
- 官方 ISO 和磁盘是组合产物（edition + 内核 + 引导配置 + 锁定记录），CI 按组合矩阵测试。

## A10 版本（D10）

- luban 工具、edition（`subos:luban-*`）、策略包、Luban 镜像统一使用 `YYYY.M.D.N`，规则与 xlings 相同：`N` 从 1 开始，`.0` 保留给正式版本。
- 已发布的 `0.1.0` 保留不动；新版本从日期版本开始。
- 引用一律通过显式的 `latest` ref，不依赖版本比较。`semver::parse` 不接受四段版本号，否则会退回字典序，这条规则来自 AGENTS.md。
- 一个 Luban 发行版的「版本」就是它 edition 的日期版本；`os-release` 的 `VERSION_ID` 写日期版本。

## A11 生态

| 角色 | 提供 |
|---|---|
| xlings | 包管理、组合、代、ABI 匹配、工具表、提权的唯一入口、NDJSON 接口 |
| luban | OS 与环境管理，在宿主上和每台 Luban 机器里都可用 |
| xim-pkgindex | luban 工具、官方 edition、内核、limine、squashfs-tools、xorriso、策略包、按 ABI 区分的变体 |
| 第三方索引 | 自己的 edition（基于 Luban 的发行版）、策略、软件 |
| 官方发行版 | 长期维护的组合（命令行、桌面、agent workspace），提供 ISO 和磁盘 |
| 兼容性 | 「基于 Luban」= 继承自 luban-nano，并通过一组公开的验收（启动、`luban status`、代与回滚、一致性约束） |

---

# B 用户体验视角

## B1 不像 Docker

| Docker 让人觉得复杂的地方 | Luban 的做法 |
|---|---|
| 守护进程、权限组、socket | 没有守护进程；一次性的系统设置之后，普通用户直接使用 |
| 镜像、容器、卷、网络，一组名词 | 只有一个名词：**环境**（box）。edition、视图、承载只在需要时出现 |
| 必须先写 Dockerfile | 默认 edition 拿来就用；模板只在打造自己的发行版时才需要写 |
| 环境是一次性的，数据要另外挂卷 | 环境是长期的；home 就在里面，升级和回滚不动用户数据 |
| 在容器里装东西，重建就没了 | `xlings install` 即时生效，每次变更是一代，可以回滚 |
| 用户 ID、文件权限、端口映射要自己处理 | 默认就对：同一个用户，home 可用，网络按策略 |
| 出错信息是底层的 | 第一次就给出根本原因和一个修复办法 |

## B2 两种使用方式，同一个系统

- **在已有系统里使用**：真实的日常环境，不是试用。它和真机上的 Luban 是同一个发行版，用法相同。
- **在真机上运行**：需要独占硬件或替换宿主时，直接用 ISO 或磁盘，或者把已有环境导出过去。
- 两者由**同一个 luban 工具**管理。宿主上的 luban 和机器里的 luban 是同一个程序，只是机器里多出启动项和内核。

## B3 luban CLI 设计

### B3.1 实现方式
- **解析**：`mcpplibs.cmdline`，与 xlings 相同。
- **一份声明式的命令规格**：命令、参数、选项、别名、级别、说明。帮助、shell 补全（bash、zsh、fish、PowerShell）和参考文档（`docs/generated/luban-reference.md`）都从这份规格生成，三者不会不一致。
  - xlings 的规格、校验和补全代码在 `src/cli/`（前端），luban 不能链接它。
  - 因此把这部分**抽成共享模块** `modules/cli`（`xlings.cli.spec`、`xlings.cli.completion`），xlings 和 luban 共用。
- **规格新增 `level` 字段**：`common`（常用）、`more`（更多）、`expert`（专家）。帮助的渲染由共享模块完成，不依赖 cmdline 的帮助输出。
  - cmdline 只负责解析。如果缺少能力（例如 `--` 之后的透传、未知选项的拼写建议），给 mcpplibs/cmdline 提交 PR；实现时先核对 0.0.2 已有的能力。
- CI 检查生成的参考文档与规格一致（与 xlings 的 `command-reference.md` 相同的机制）。

### B3.2 命令

**常用（`luban --help` 只显示这一组）**

| 命令 | 作用 |
|---|---|
| `luban` | 一屏概要：这是哪里（宿主、环境内、真机），有哪些环境，下一步做什么 |
| `luban new <名字> [edition]` | 新建环境，默认 core。edition 可以写短名：`luban new agent agent-workspace` |
| `luban enter <名字>` | 进入 |
| `luban run <名字> -- <命令…>` | 运行一个命令 |
| `luban ls` | 列出环境 |
| `luban status [名字]` | 不带名字：这台宿主或这台机器的状态；带名字：这个环境的状态 |

**更多（`luban help --all`）**

| 命令 | 作用 |
|---|---|
| `luban config <名字> [键 [值]]` | 查看或设置参数，键值形式（类似 git config）：`proxy`、`tz`、`policy`、`allow`、`mount` |
| `luban history [名字]` / `luban rollback [名字] [--to <代>]` | 历史和回滚 |
| `luban export <名字> <文件>` | 制作镜像。格式按扩展名推断：`.iso`、`.img`（可启动的原始磁盘）、`.qcow2`、`.tar` / `.tar.zst`（rootfs）；`--format` 可以覆盖 |
| `luban write <镜像或名字> <设备>` | 制作启动盘（U 盘）。写入前显示设备型号和大小并要求确认；拒绝已挂载的盘和系统盘 |
| `luban try <名字或镜像>` | 在本地虚拟机里试运行（qemu，经工具表解析；有 KVM 时自动加速） |
| `luban rm <名字>` | 删除（需要确认） |
| `luban setup` | 宿主的一次性设置（隔离） |

**专家（只在 `luban help --all --expert` 和参考文档中出现）**

| 命令 | 作用 |
|---|---|
| `luban install <设备>` | 把当前系统（live ISO 或任何 Luban 机器）安装到磁盘：分区、ESP、limine、根的代 |
| `luban boot [list \| once <项> \| default <项> \| good]` | 启动项（只在真机上） |
| `luban new <名字> --view machine` / `--carrier <c>` | 指定视图和承载 |

不单独设 `luban kernel`：内核是包，用 `xlings install linux-kernel@<v>` 安装，新的一代自动成为启动候选，再用 `luban boot once` 试启动。`luban status` 显示当前内核。

**全局选项**：`--json`、`--agent`、`-y/--yes`、`-v/--verbose`、`-q/--quiet`、`-h/--help`、`--version`。语义与 xlings 相同：全局选项任何命令都接受，不能作用时忽略。

### B3.3 帮助是渐进式的
```
$ luban --help
Luban：环境与系统

常用
  new <名字> [edition]    新建环境（默认 core）
  enter <名字>            进入
  run <名字> -- <命令>    运行一个命令
  ls                      列出环境
  status [名字]           状态

更多命令：luban help --all
装软件：  xlings install <包>
```
- `luban <命令> --help` 显示这个命令的常用选项，最后一行写出等价的 xlings 命令（如果有），作为往下走一层的入口。
- 拼错的命令和选项给出建议（「你是想用 `luban enter` 吗？」）。

### B3.4 按所在位置的行为
| 位置 | `luban` / `luban status` 显示 | 不适用的命令 |
|---|---|---|
| 宿主 | 宿主的隔离状态、环境列表 | `boot`、`install`：说明「这里不是 Luban 机器」，并给出路线 |
| 环境内 | 这个环境的 edition、版本、代、隔离、身份 | `new`、`write`、`setup`：说明要在宿主上执行 |
| 真机或 live ISO | 机器的 edition、代、内核、启动项 | — |

### B3.5 小功能：镜像、启动盘、安装、试运行
```
luban export box box.iso          # 制作 ISO（可 live 运行，可安装）
luban write box.iso /dev/sdb      # 制作启动盘
luban write box /dev/sdb          # 直接从环境制作启动盘（先导出再写入）
luban try box                     # 在虚拟机里试运行这个环境（machine 视图）
luban try box.iso                 # 试运行一个镜像
luban install /dev/nvme0n1        # 在 live ISO 里：安装到磁盘
```
- 每个动词只做一件事，参数就是「从哪里」和「到哪里」。
- 危险操作（`write`、`install`）：
  - 面向人：显示设备型号、大小、分区，要求确认；
  - 面向 agent：必须同时给出 `--yes` 和 `--device <序列号>` 才执行，否则返回结构化错误；
  - 两种模式都拒绝已挂载的盘和当前系统所在的盘。
- 第一版 `write` 和 `install` 只在 Linux 上实现；其他平台给出路线（例如用平台自带的写盘工具写入 `luban export` 的镜像）。

### B3.6 退出码与错误
- 0 成功；1 失败；2 需要确认但无法询问（agent 模式，或没有终端）；124 超时。
- 错误输出：一行原因，一行修复办法。`--json` 时输出稳定的错误码和字段。

## B4 渐进式暴露

| 层次 | 用户需要知道的 | 示例 |
|---|---|---|
| 0 | 一个名字 | `luban new box` → `luban enter box` |
| 1 | 选一个 edition 或场景 | `luban new agent agent-workspace`、`luban new d desktop` |
| 2 | 调整参数，做镜像和启动盘 | `luban config box tz utc`、`luban export box box.iso`、`luban write box.iso /dev/sdb` |
| 3 | 完整的 SubOS 能力 | `xlings subos ...`：视图、承载、授权、挂载 |
| 4 | 打造自己的发行版 | 写模板，`from subos:luban-nano`，发布到自己的索引 |
| 5 | 真机 | `luban install <设备>`、`luban boot` |

- 每一层都不要求先理解下一层。
- 输出只出现当前层的概念：层次 0 和 1 的输出里不出现 template、generation、rootfs、payload、view、carrier 这些词（D 部分有 CI 检查）。

## B5 交互原则

1. **先检查，后花费。** 先检查宿主能力，再下载；问题只在开头问一次。
2. **需要 sudo 就当场请求。** 说明为什么、会做什么，然后请求密码，经提权的唯一入口执行，写入 `elevation.ndjson`。这是系统级的一次性设置。
3. **拒绝时给出路线，不悄悄降级。** 用户不同意 sudo 时，不创建根，并给出两条路线：
   - `luban setup`（之后再做）；
   - 在特殊环境下显式使用 `--sandbox proot`（视图，不是安全边界）。
4. **一个操作，一个进度。** 整条 `from` 链解析成一份计划，声明的包在同一进程里安装。
5. **模板是不可见的管道。** 用户看到的是「Luban Core 2026.10.20.1」。
6. **错误第一次就给出根本原因和一个修复办法**，用用户的说法。
7. **成功时安静。** 最后一行只说下一步。
8. **同一件事用同一个词**：luban、xlings、文档和错误信息都一样。

## B6 面向人和面向 agent（D13）

谁在读输出（人还是 agent）、怎么画（cli、tui、`--json`、NDJSON）是两个独立的维度，核心代码不关心（AGENTS.md 已有的规则，luban 同样遵守）。

| | 面向人（默认） | 面向 agent（`--agent` 或 `XLINGS_AGENT_MODE=1`，`--json`） |
|---|---|---|
| 进度 | 一个进度条，结束时一行结论 | 不画进度；需要时是 NDJSON 事件流 |
| 提问 | 只在开头问一次（sudo、删除确认） | **从不提问**：需要回答的问题变成结构化错误，附候选项和能回答它的确切参数 |
| 错误 | 根本原因 + 一个修复办法，人的说法 | 稳定的错误码 + 字段（原因、路线、命令），不依赖文案 |
| 输出 | 当前层次的概念 | 完整的结构（edition、视图、承载、代、隔离、身份），字段只追加不删除 |
| 幂等 | — | 重复执行同一个 `new` / `config` 得到同样的状态，并说明「已是这样」 |
| 退出码 | 0 成功，1 失败，2 需要确认 | 相同，另有错误码区分原因 |

## B7 稳定性与兼容性

- **升级不打断使用。** luban 和 xlings 各自升级；环境里的数据和已锁定的策略不受影响；每次变更是一代，可以回滚。
- **版本错配有握手。** luban 启动时检查 xlings 接口的协议版本；不满足时给出一条升级命令，不半途失败。
- **数据格式只追加。** `instance.json`、`status --json`、启动配置只增加字段；旧客户端忽略新字段（part 3 已有的原则）。
- **已发布的 edition 不可变。** 清单按版本查表；`latest` 移动不改变已有环境。
- **旧环境照常工作。** 旧代在新客户端下仍可进入（A7 的迁移规则）；proot 环境保留。

## B8 本次试用的问题与改法

| # | 现象 | 原因 | 改法 |
|---|---|---|---|
| U1 | 卡住 | 子进程 `xlings install` 在后台进程组里探测终端，被 SIGTTOU 停住，父进程等到超时 | 子进程组运行期间接管终端，结束后归还（修复在 stash 中，回归测试已复现）；根本上见 U2 |
| U2 | 输出混乱 | `from` 链逐层自动安装；子进程另外输出 | B5-4：同一进程内安装 |
| U3 | 「installed, but … still resolves to … use to switch」 | 通用提示套用在模板包上 | 模板不产生这条提示 |
| U4 | `subos created` 打印两次 | 两层都报告 | 只报告一次 |
| U5 | 下载完之后 `use` 才失败 | 宿主检查放在最后 | B5-1 |
| U6 | 先报 proot，第二次才看到 AppArmor 原因 | 选择器悄悄回退到 proot，掩盖了根本原因 | D11（不悄悄选 proot）+ B5-6 |
| U7 | 下载了内核 | tiny 0.1.0 带内核 | D2 |

目标形态：
```
$ luban new box
  Luban Core 2026.10.20.1：13 个包，约 410 MB
  ! 第一次在这台机器上运行 Luban 环境，需要一次系统设置：
    Ubuntu 限制了程序创建隔离环境；将安装一个 root 所有的 bwrap 和一个范围很窄的 AppArmor 规则
    （只做一次，之后所有用户都不再需要）
  [sudo] speak 的密码：
  ✓ 系统设置完成
  [██████░░░░] 5/13 gcc 16.1.0   180/410 MB
  ✓ box 就绪
    进入：luban enter box
```

---

# C 场景：Luban Agent Workspace

## C1 场景
长期使用、高度隐私、可配置的 agent 工作环境：agent 在里面写代码、编译、联网，不能接触宿主数据；对外不暴露宿主的位置、网络和设备指纹，不能与你本人的身份关联。

## C2 威胁模型

| 要防 | 不防（写进 `PRIVACY.md` 和 `luban status`） |
|---|---|
| 远端服务通过网络和环境推断宿主的位置和身份 | 账号提供方：你的账号本身就能识别你 |
| agent 读取宿主数据、凭据、其他实例 | 宿主的管理员 |
| 任何失败后静默回退到直连或宿主配置 | 流量时序关联 |
| 不同 agent 实例之间可以互相关联 | 在 L2：共享内核和 CPU 的指纹（L3 解决） |

## C3 组成

```
subos:luban-agent-workspace（edition，from luban-core）
  × 策略包：agent-confined（默认） ← agent-private（继承它，加上只走代理和身份中性化）
  × 实例参数：代理、时区模式、授权
  × 视图：L2 = sandbox（共享内核）| L3 = machine（virt 内核虚拟机）
  × 承载：local（Linux）| wsl2（Windows）| vz（macOS）
```

| 层 | 内容 |
|---|---|
| luban-agent-workspace | 从 core 继承，加上 claude（以后加入其他 agent 工具）；中性用户 `agent`；home 模板（workspace、`.claude/settings.json`、`.mcpp/config.toml`、`PRIVACY.md`），只补缺；模板清单声明默认策略 |
| agent-confined | 保护宿主：宿主 home 和其他实例不可见；不授权设备和 socket；拉取包、更新索引前先询问；禁止嵌套 userns；能力不足就拒绝。**网络可以直连**。名字和 `status` 都说明它不隐藏身份 |
| agent-private | 继承 confined，再加：只走 SOCKS5h，失败就关闭；身份中性化；时区跟随代理 |
| 实例参数 | 在 `luban new` 或 `luban config` 时给出，不写进可分发的内容 |

策略是数据，适用于所有架构和平台；平台不能执行时按 `no_degrade` 拒绝，并给出路线。

## C4 稳定的身份
- **每个实例固定一个身份**：machine-id 和 hostname 在创建时随机一次，之后不变；用户名是 `agent`；Git 身份在实例内配置。
- **复制或导出的实例重新生成身份**，除非用户明确要求保留。
- **时区** `identity.tz = "proxy" | "utc" | "<地区名>"`：
  - `proxy` 经过代理查询出口位置，查询服务可以配置，结果按代理缓存，每次进入会话时确定；
  - 查不到时退回 UTC 并报告，**绝不退回宿主时区**；
  - **默认值：创建时给了代理就是 `proxy`，否则是 `utc`。**
- **locale** 默认 C.UTF-8，可选 `identity.locale = "proxy"`。
- **每个 agent 一个实例、一个身份**；不做每次会话随机化。

## C5 泄漏面与两档防护

| 泄漏面 | L2 私有沙箱（第一版） | L3 私有机器 |
|---|---|---|
| 出口 IP、DNS、UDP（QUIC、WebRTC）、IPv6、宿主 loopback | 只走 SOCKS5h（DNS 由代理解析），失败即关闭；阻断 UDP、IPv6 和宿主 loopback | 虚拟机只有一条通向代理的网络 |
| TZ、`/etc/localtime`、`LANG` / `LC_*` | 跟随代理或 UTC；修复 `LC_*` 漏过过滤的问题 | 同左 |
| 挂载表、链接目标、`$HOME`、配置里的路径 | 根视图用 `/xlings`；挂载表里不出现宿主路径；`$HOME=/home/agent` | 天然不存在 |
| machine-id、boot_id、hostname | 每个实例固定的随机值 | 同左 |
| DMI、MAC、磁盘 ID、`/proc/cpuinfo` | 屏蔽 DMI 和磁盘 ID，只露出 lo；cpuinfo 如实报告无法隐藏 | 通用 CPU 模型、随机 MAC、中性 SMBIOS |
| `uname`、`/proc/version`、`/proc/cmdline` | 如实报告无法隐藏 | 统一的 virt 内核 |
| Git 身份、ssh-agent、剪贴板、遥测 | 不授权；默认关闭工具的非必要流量 | 同左 |
| 宿主环境变量、凭据、socket | 白名单为空 | 同左 |

## C6 长期运行
- 升级和回滚按代进行，不动用户数据。
- 策略升级显示差异，显式应用，从不静默放宽。
- 凭据只在实例内。
- workspace 和 home 可以导出和导入；整个实例可以导出迁移（迁移时重新生成身份，除非明确保留）。
- 资源配额随会话层提供。
- 审计：会话、拉取请求、策略变更、提权全部写入 ndjson 日志，不记录秘密。

## C7 用法
```
luban new agent agent-workspace --proxy socks5h://127.0.0.1:7897
luban enter agent                 # 或：luban run agent -- claude
luban status agent                # 隔离、身份、出口地区、无法隐藏的项
luban config agent tz utc            # 键值形式；luban config agent 查看全部
luban new agent-vm agent-workspace --view machine --proxy ...   # L3
luban try agent                   # 在虚拟机里试运行这个环境
```
Windows 和 macOS 上命令相同。

## C8 验收
1. **全泄漏面探针**：宿主放置哨兵值，在环境里收集 C5 的所有泄漏面，断言输出中找不到任何哨兵值；L2 和 L3 用同一个探针。
2. **网络探针**：直连 TCP、UDP DNS、IPv6、宿主 loopback 都被阻断；经代理可以访问；代理停止后不改走直连。
3. **身份稳定**：两次会话之间身份一致；两个实例之间不同；复制出的实例身份不同。
4. **生命周期**：升级、回滚、重复配置、卸载策略包之后，用户数据和已锁定的策略都还在。
5. **拒绝路径**：能力不足、客户端太旧、策略里有未知字段、代会暴露宿主路径、选中的后端是 proot，都拒绝进入。

## C9 #945 的重构方向

| #945 中的内容 | 方向 |
|---|---|
| GCC、binutils、openssl、xz 的构建依赖修正 | 单独合入 |
| luban-core 0.2.0 | 改为日期版本；不含 claude；清单按版本查表；0.1.0 保留 |
| luban-tiny 0.2.0 去掉内核 | 日期版本；等 xlings 完成 A6 后再合入，同时加上 `boot.kernel` 提示 |
| `agent-private` 策略 | 拆成 agent-confined 和 agent-private 两级，适用于所有架构，使用日期版本 |
| `agent-workspace-private`（shell 入口） | 改为 edition `subos:luban-agent-workspace`，加上模板清单里的默认策略和 `--proxy`；创建完成时就已经是私有的 |
| 验收脚本和网络探针 | 保留并扩展为 C8 |

---

# D CI 覆盖、计划与待定

## D1 CI 覆盖（每条体验承诺都有测试）

| 承诺 | 测试 | 车道 |
|---|---|---|
| 不挂起：子进程不会因为终端被停住 | 伪终端单元测试（`test_process_terminal`，已写好）；`luban new` 在伪终端下的 e2e，带超时 | linux、macos |
| 一个操作只有一个计划、一个进度、一个结论 | 伪终端 e2e：输出快照，「计划」「完成」各出现一次 | linux |
| 层次 0、1 的输出不出现内部术语 | 词汇检查：在 `luban new / enter / ls / status` 的输出快照上，禁止出现 template、generation、rootfs、payload、view、carrier | linux、macos、windows |
| 先检查后花费 | 用一个模拟「宿主不能创建根」的探测接缝运行 `luban new`：断言在任何下载之前就停下，并且给出 `luban setup` | linux |
| agent 模式从不提问 | 没有终端、`--agent`：每个需要确认的路径都返回结构化错误（错误码、候选项、参数），无一等待输入 | linux、macos、windows |
| `--json` 结构稳定 | `luban status --json` 和 `info --json` 的 schema golden，只允许追加字段 | linux |
| luban 只有一个实现 | 对每个 luban 命令，断言 `--help` 中写出的等价 xlings 命令真实存在；luban 的 SubOS 操作经接口完成（`lint_layer_deps`：`apps/luban` 不链接 `src/`） | linux |
| 帮助按级别展开 | `luban --help` 只列常用命令的 golden；`help --all` 和 `--expert` 各自的 golden；参考文档和补全与规格一致 | linux |
| 拼写建议和位置感知 | 拼错的命令给出建议；宿主上执行 `luban boot`、环境内执行 `luban new` 都说明原因和路线 | linux |
| `export` 的格式推断 | 每个扩展名产出对应格式；未知扩展名拒绝并列出支持的格式 | linux |
| `write` / `install` 不会写错盘 | 在 loop 设备上执行：拒绝已挂载的盘和系统盘；agent 模式缺 `--yes` 或序列号时拒绝；写入后能在 qemu 中启动 | linux（root 车道、qemu） |
| `luban try` | 用 qemu 启动一个环境，`luban status` 在虚拟机内报告 machine 视图 | linux distro（qemu） |
| 版本错配有路线 | luban 遇到较旧的 xlings 接口版本时，给出升级命令 | linux |
| proot 不被悄悄选中 | 在 bwrap 不可用的宿主上，没有显式 `--sandbox proot` 时拒绝并说明；有时创建视图并标出「不是安全边界」；私有策略总是拒绝 | linux（isolation-fix 车道） |
| 已发布的 edition 不可变 | xim-pkgindex：每个已发布版本的清单 golden | xim-pkgindex |
| 交付形态一致 | 同一个 edition 的实例、rootfs（容器）、磁盘（qemu 启动）三者的树哈希相同（扩展已有的 ROOT-SAME-TREE） | linux distro（qemu） |
| ISO 能启动和安装 | qemu 启动 ISO（UEFI 和 BIOS），live 环境 `luban info`，安装到虚拟磁盘后再启动 | linux distro（qemu） |
| Agent 隐私 | 全泄漏面探针、网络探针、身份稳定性（C8） | linux（L2）、qemu（L3） |
| 首次安装 | fresh-install 从索引安装 luban 和 edition，`luban new` 到 `luban enter` 全流程 | fresh-install（三平台） |
| Windows / macOS 用法一致 | 假 wsl.exe 和假 vz 助手下的 `luban new / enter / status` | windows、macos |

需求 ID 加入 `tests/requirements.toml`，例如 `LUBAN-CLI-*`、`UX-NO-HANG`、`UX-ONE-PLAN`、`UX-VOCAB-L0`、`UX-PREFLIGHT`、`UX-AGENT-NO-PROMPT`、`PRIVACY-LEAK-PROBE`。

## D2 交付方式：每个仓库一个 PR，用 commit 保留检查点（D19）

| 仓库 | PR | 内容 |
|---|---|---|
| mcpplibs/cmdline | 一个（只在需要时） | 补齐 luban CLI 需要而 0.0.2 没有的解析能力 |
| xlings | **一个** | P0 到 P5 的 xlings 侧全部内容 |
| xim-pkgindex | **一个**（由 #945 重构而来，或新开一个 PR 取代它） | Luban 相关的所有包 |

### D2.1 解耦与合入顺序
- **xlings 的 PR 不依赖 xim-pkgindex 的 PR。** xlings 的测试用自己的 fixture recipe（`tests/fixtures/` 里的 edition、策略、内核、limine 等最小 recipe），不使用真实索引。
- **xim-pkgindex 的 PR 依赖 xlings 的发布。** 新包的 `min_client` 写 xlings 新版本号；在 xlings 发布之前，它的 CI 用 xlings PR 的构建产物验证。
- 合入顺序：
  1. cmdline（如有）；
  2. xlings，并发布 `YYYY.M.D.1`；
  3. 补全 GitCode 镜像，合入 xim-pkgindex 的 xlings bump PR；
  4. 合入 xim-pkgindex 的 Luban PR；
  5. 在真实生态中验证：fresh-install、`luban new` 到 `luban enter`、ISO 在 qemu 中启动，以及 agent workspace 的探针。

### D2.2 xlings PR 的检查点（每个 commit 都能编译、CI 全绿）

| # | commit | 对应 |
|---|---|---|
| X1 | 子进程组接管终端，加伪终端回归测试 | P0、U1 |
| X2 | 声明的包在同一进程内安装，`from` 链合成一份计划 | P0、U2 |
| X3 | 先检查宿主；当场请求 sudo 的一次性设置；选择器不悄悄选 proot；doctor 报告 AppArmor 标签 | P0、U5、U6 |
| X4 | 模板相关的文案（U3、U4） | P0 |
| X5 | 把 `src/cli` 的规格、校验、补全抽成 `modules/cli`，加入 `level` 字段 | P1 |
| X6 | `apps/luban` 骨架：luban-init 并入成为入口名，接口握手，人和 agent 两种输出 | P1 |
| X7 | luban 常用命令（new、enter、run、ls、status、概要） | P1 |
| X8 | luban 更多和专家命令（config、history、rollback、export、rm、setup、boot） | P1 |
| X9 | 根视图使用 `/xlings` 逻辑路径，加迁移 | P2 |
| X10 | 实例身份（machine-id、hostname 固定，复制时重新生成）、`identity.tz`、`LC_*` 修复、屏蔽 DMI 和磁盘 ID | P2 |
| X11 | 模板清单的 `policy` 字段、`--proxy`、home 模板、sysusers 用户 | P2 |
| X12 | ABI 描述与匹配 | P3 |
| X13 | `boot.kernel` / `kernel_min`，machine 视图组合内核，`rootfs_boot_test` 改为组合内核 | P3 |
| X14 | 不依赖 `/bin/sh`；luban-nano 的 fixture | P3 |
| X15 | `export` 按扩展名推断格式；ISO（limine、squashfs、initramfs）；`write`；`install`；ESP 上的内核 | P4 |
| X16 | `luban try`；`--view machine` 本地启动（L3 的基础） | P4、P5 |
| X17 | CI 车道和需求 ID（D1 全部条目） | 全部 |
| X18 | 文档：AGENTS.md、用户文档、生成的参考文档、本设计的实施记录 | 全部 |

### D2.3 xim-pkgindex PR 的检查点

| # | commit |
|---|---|
| I1 | GCC、binutils、openssl、xz 的构建依赖修正（来自 #945） |
| I2 | `xim:limine`、`xim:squashfs-tools`、`xim:xorriso`、`xim:qemu`（如果还没有）、`xim:linux-kernel-virt` |
| I3 | `xim:luban`（luban 工具，日期版本） |
| I4 | edition 改为日期版本，清单按版本查表：nano、tiny（不带内核，带 `boot.kernel`）、core（不含 claude）；0.1.0 保留 |
| I5 | `subos:luban-agent-workspace`、`agent-confined`、`agent-private` |
| I6 | 验收脚本、泄漏探针、网络探针、CI |

### D2.4 阶段（内容视角）

| 阶段 | 内容 | 依赖 |
|---|---|---|
| **P0 首次使用** | X1 到 X4 | — |
| **P1 luban 工具** | X5 到 X8，I3 | P0 |
| **P2 Agent Workspace L2** | X9 到 X11，I5 | P0 |
| **P3 Luban 模型** | X12 到 X14，I4 | P1 |
| **P4 交付形态** | X15、X16，I2 | P3 |
| **P5 Agent L3** | X16 加上 virt 内核和只通向代理的虚拟网络 | P3、P4 |
| **P6 生态** | musl 变体、第三方 edition 指南、桌面 ISO（不在本次 PR 范围内） | P4 |

## D3 已定
- claude 放在 luban-agent-workspace。
- 声明的包在同一进程内安装。
- 一次性设置：`luban setup`，对应 xlings 的 `self doctor --isolation --fix`。
- 短名映射只对官方 edition 生效。
- proot 降低优先级、保留作兜底、只在显式指定时使用。
- luban 是独立的 OS 管理工具，存在于 nano、真机和宿主上；Luban 相关的工具和包使用日期版本。
- **luban-init 随 P1 一起并入 luban。**
- **时区：给了代理时默认查询代理出口，否则用 UTC、不查询。** 查询服务可以配置（`identity.geo_lookup`），默认是一个只需 HTTPS GET、返回时区名的公开服务。候选是 `https://ipinfo.io/timezone`，实现时核对它的可用性和条款。查询失败就用 UTC 并报告。
- **安装与写盘分成两个动词**：`luban install <设备>`（分区、ESP、引导、根的代）和 `luban write <镜像> <设备>`（写启动盘）；`luban export` 只产出文件。
- **luban CLI 用 cmdline 和声明式规格**，按三级渐进展开（B3）。
- **每个仓库一个 PR，commit 作为检查点**（D2）。

## D4 待定
1. 默认的出口查询服务是否接受 `ipinfo.io/timezone`，还是由 xlings 自建一个只返回时区的服务（更可控，但要维护）？
2. `luban new <名字> [edition]` 用位置参数指定 edition（更短），同时保留 `--from` 作为别名，可以吗？
3. 本次 PR 是否包含 P5（L3：virt 内核虚拟机里的 agent workspace），还是只包含 L3 的基础（`luban try`、`--view machine`），完整的 L3 隐私留到下一个 PR？

---

# E 自我 review

| # | 发现 | 处理 |
|---|---|---|
| E1 | 统一用 `/xlings` 会破坏只在 home 下使用 xlings 的场景 | 只在根视图和机器形态使用；`XLINGS_HOME` 在任何形态都提供（A7） |
| E2 | `/xlings` 依赖挂载命名空间 | 只用于根视图；proot 兜底下如实标出这一项不可用 |
| E3 | 已有根的旧代指向宿主路径 | A7 迁移规则；私有策略拒绝这样的代 |
| E4 | 复制或导出的实例共享同一身份 | C4 重新生成；C8-3 验收 |
| E5 | nano 不含 sh，但大量 hook 需要 sh | hook 把 sh 声明为依赖（A4） |
| E6 | limine 读不到 ext4 上每一代的内核 | `luban boot` 把每一代的内核复制到 ESP（A9） |
| E7 | 能否创建根取决于 xlings 的启动方式（继承的 AppArmor 标签） | doctor 报告标签；选择器不依赖它；系统设置后结论一致（A8） |
| E8 | luban 需要 SubOS 能力，但分层规则不允许它链接 `src/` 前端 | luban 经 NDJSON 接口驱动 xlings（A1.1），同时保证只有一份实现 |
| E9 | luban 与 xlings 各自升级，会出现版本错配 | 接口握手和升级路线（B7）；CI 覆盖（D1） |
| E10 | 两个工具可能分化成两套词汇 | 职责按「包」和「OS」划分；`--help` 写出对应的 xlings 命令；CI 检查对应命令存在 |
| E11 | luban 进入 nano 会增加最小层的体积 | luban 只链接 `luban/` 和 `modules/`，与 luban-init 合并成一个二进制，nano 仍然只有两个文件 |
| E12 | 日期版本与已发布的 `0.1.0` 混在一起，比较顺序不可靠 | 一律通过 `latest` ref 引用；不依赖版本比较（A10） |
| E13 | proot 保留但不能重蹈 U6（悄悄选中、掩盖原因） | 从不自动选中；显式指定才用；私有策略拒绝；CI 覆盖 |
| E14 | `agent-confined` 允许直连，可能被误以为是「私有」 | 名字和 `status` 都说明它只保护宿主 |
| E15 | 时区跟随代理需要联网查询，增加延迟和依赖 | 按代理缓存；查不到就用 UTC；未给代理时不查询 |
| E16 | U1 的修复只是兜底，新的子进程调用会再次遇到 | 同一操作内去掉子进程（B5-4）；伪终端测试守住兜底 |
| E17 | 「不像 Docker」只是口号，容易在实现中流失 | 落到可测的检查：层次 0 词汇检查、一个计划、先检查后花费、agent 不提问（D1） |
| E18 | 在已有系统里使用与真机，内核可能不同，用户态行为可能不同 | `kernel_min`、ABI 描述；官方组合按矩阵测试 |
| E19 | luban 需要的命令规格、补全代码在 `src/cli`，luban 不能链接 | 抽成共享模块 `modules/cli`（X5），两边共用，参考文档和补全都从规格生成 |
| E20 | cmdline 的帮助输出不支持分级 | 帮助由共享模块按 `level` 渲染，cmdline 只负责解析；缺少的解析能力补到上游 |
| E21 | `export` 按扩展名推断格式，`.img` 可能有歧义 | `.img` 固定表示可启动的原始磁盘；`--format` 可以覆盖；无法识别的扩展名直接拒绝并列出支持的格式 |
| E22 | `write` 和 `install` 可能毁掉错误的磁盘 | 显示型号、大小和分区并要求确认；拒绝已挂载的盘和系统所在的盘；agent 模式必须同时给出 `--yes` 和设备序列号 |
| E23 | `luban try` 依赖 qemu，没有 KVM 时很慢 | qemu 经工具表解析（`xim:qemu`）；没有 KVM 时提示速度，并说明原因 |
| E24 | 同一个命令在宿主、环境内和真机上含义不同，可能让人困惑 | 位置写在概要的第一行；不适用的命令说明原因并给出在哪里执行（B3.4） |
| E25 | xlings 单 PR 的范围很大，审查和回退困难 | 18 个检查点 commit，每个都能编译、CI 全绿、可以单独审查；仓库之间用 fixture 解耦，避免循环等待 |
| E26 | xlings 的 PR 如果直接依赖 xim-pkgindex 的新包，会形成循环（索引的包要求新客户端，客户端的测试需要新包） | xlings 用 fixture recipe 测试；索引的 PR 在 xlings 发布后才合入（D2.1） |
| E27 | 「默认查询代理出口」会让每个带代理的实例在进入时联网 | 结果按代理缓存，只有代理变化或缓存过期时才查询；查询本身经过代理 |

尚未覆盖、留待后续：
- 桌面 edition 在根视图中的图形栈；
- Secure Boot；
- 非 x86_64 的 ISO；
- 多用户 Luban 机器上 agent workspace 的归属和配额；
- luban 的 TUI；
- Windows 和 macOS 上的 `write` 与 `install`。

---

# F 实施记录（2026-10-10，xlings PR #650 / xim-pkgindex PR #945，版本 2026.10.10.1）

## F1 已实现（按检查点）

| 检查点 | 内容 | 验证 |
|---|---|---|
| X1 | 子进程组在运行期间接管终端（`run_argv_with_timeout`） | `test_process_terminal`：修复前停在截止时间，修复后 40 ms |
| X2 | `subos new --from` 一个进程、一份计划（`xim::cmd_install_step`），"created" 只报一次；"still resolves to" 的命名空间误报 | `test_subos_new_template`（伪终端） |
| X3 | 先检查宿主再下载（`prepare_root_host`），当场询问一次性设置；根的拒绝先说根本原因；doctor 报告 AppArmor 标签 | `test_subos_new_template`；INTENT-EQ golden 1 例 |
| X5 | `modules/cli`：命令规格、校验、分级帮助、拼写建议、补全 | `test_cli_model`、参考文档与 parity 脚本 |
| X6–X8 | `luban`：一个二进制两个名字（luban / luban-init），命令映射到 xlings，位置感知、概要、JSON、接口握手；随 xlings 发布、self update 一起更新、每个根和镜像都带 | `test_luban_cli`、`test_luban`、`luban_init_test.sh` |
| X10 | 中性身份 = persona（主机名、machine-id，复制时重新生成）；时区 utc / 名字 / proxy（经代理查询，缓存，失败用 UTC）；根视图丢弃 `LC_*`；`subos status` 报告无法隐藏的项 | `test_persona`、`test_agent_privacy`（CI）；INTENT-EQ golden 22 例 |
| X11 | 模板声明 `policy`、`subos new --proxy`，在第一次进入前选定并锁定 | `test_luban`（AWorkspaceIsPrivateWhenItIsMadeNotAfter） |
| X12–X13 | edition 的 `abi`（架构 × 内核 × libc × 下限）在读取模板时检查；`boot.kernel` / `kernel_min` 记录为提示，导出时使用 | `test_subos_new_template`（ABI） |
| X14 | shim 的别名是普通单词时不经 shell 执行（nano 没有 sh） | `test_shell_command` |
| X15 | `export --iso`（根作为 initramfs，limine；xorriso 时 BIOS+UEFI，否则进程内 BIOS）；`--drive`（GPT + FAT + ext4，进程内 `luban.image`，BIOS+UEFI）；`--qcow2`；`luban write`（驱动器安全检查、提权入口）；`luban try`（qemu；`--proxy` 时网络只有代理，`luban __pipe` 每个连接一条） | `test_luban_image`、`test_archive_formats`、`luban_image_test.sh`（qemu：ISO 与驱动器在 SeaBIOS 和 OVMF 下都启动到 init） |
| I1–I6 | 索引：日期版本的 edition（nano、tiny 不带内核、core 不含 claude、agent-workspace）、agent-private / agent-confined、limine、验收脚本改用 luban | xim-pkgindex 静态测试；`luban-agent-workspace.yml` 真实验收 |

## F2 与设计的差异（如实记录）

1. **没有单独的 `xim:luban` 包。** luban 随 xlings 的发布包一起分发（同一版本），每个根和镜像都由投影带上它；单独一个包只会是同一文件的第二个来源。
2. **ISO 不用 squashfs。** 根整个作为 initramfs 由内核解压到内存：不需要挂载、不需要内核模块，也不需要 mksquashfs。代价是内存：live 系统需要大约根大小两倍的内存（`luban try` 默认 2 GB）。
3. **没有 `luban install <设备>`。** 把一个环境装到驱动器上是 `luban write <环境> <驱动器>`（导出驱动器镜像再写入，持久）。从 live 系统安装它自己，需要在没有 bwrap 的机器上以 root 走完导出流程，留到下一步。
4. **根视图的 store 仍是宿主路径，不是 `/xlings`（A7 未做）。** 根内链接和导出的镜像带着构建者的 home 路径。要做出路径中性的镜像，现有的路线是私有前缀域（`subos new --rootfs --domain /xlings`）。L2 下 `/proc/self/mountinfo` 会露出绑定进来的宿主路径，`subos status` 已如实列出。
5. **根视图中 agent 仍以 root（在用户命名空间内）运行**，没有改为 sysusers 的 `agent` 用户；persona 的主机名与 machine-id 已经与宿主无关。
6. **L3 没有 `--view machine` 的 `new`。** 私有机器是 `luban try <环境> --proxy`（同一个 edition 在自己的内核上，网络只有代理）。没有加入 linux-kernel-virt，用的是现有的 generic 内核。
7. **DMI 和磁盘 ID 不需要额外屏蔽**：bwrap 视图本来就没有 `/sys`，`/dev` 是最小集合；全泄漏面探针验证这一点。
8. **proot**：降低优先级，自动回退时总会警告（与 part 2 兼容）；私有策略和根视图从不使用它。
9. **nano 与 shell**：xlings 和 luban 运行自己的路径不需要 sh（shim 别名直接 exec）；recipe 的 hook 如果用 shell，在 nano 根里需要该 edition 带上 sh——没有自动推导这个依赖。
10. **出口位置查询**默认 `https://ipinfo.io/timezone`，可由 `identity.geo_lookup` 配置；只有给了代理且没选时区时才查询。
