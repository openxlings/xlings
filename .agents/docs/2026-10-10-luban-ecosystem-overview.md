# xlings × SubOS × Luban 总体架构与生态规划（overview + 自我 review，2026-10-10）

- 汇总：SubOS 架构 part 1–3（`2026-10-05`、`2026-10-06`、`2026-10-09-subos-architecture-design-part3`）、
  Luban OS 与 agent 私有工作区设计（`2026-10-09-luban-os-and-agent-private-design`）、
  PR #650 / #651 的实施与 review、xim-pkgindex #945 的包设计讨论（本文 §5）。
- 状态基线：xlings 2026.10.10.2 已发布；#653（bwrap 探测，发布为 2026.10.11.1）未合入；xim-pkgindex #945 未合入。
- 维护者已确认（2026-10-10）：
  - 包设计 1–6 全部按建议；不改 xpkg 规范，在现有规范内设计；
  - edition 用自己的发布日期作版本；
  - 启动层用 `luban export … --boot <profile>` 选择；
  - limine 预编译资源发布到 xlings-res，本地 gtc 补 GitCode；
  - aarch64 这一轮做到 tiny。

---

## 1. 一句话

**xlings 管包，SubOS 是唯一的"环境"名词，Luban 是用 xlings 和 SubOS 搭起来的、可组合的最小 OS。**

- 一个 SubOS = 内容（workspace、home、policy）× 视图（PATH 叠加 / 沙箱 / 根 / 整机）× 承载（宿主内核 / WSL2 / VZ / 自己的内核）。
- Luban = 内核 + nano（xlings + luban）+ 任意可选层。
  - 同一个 edition 可以是宿主上的一个环境、一台 VM、一张 ISO，或一台真机，用法和认知保持一致。
- 生态 = 一个客户端仓库（xlings，含 luban）+ 一个官方索引（xim-pkgindex：上游软件、edition、策略、启动层）+ 资源镜像（xlings-res：GitHub 和 GitCode）+ 任意第三方索引。

---

## 2. 架构总图

```
┌──────────────────────────────── 交互面（谁在读 × 怎么呈现，两条独立轴） ────────────────────────────────┐
│  人：cli / tui / 补全          agent：--agent / XLINGS_AGENT_MODE（从不提问，结构化错误，退出码 2）       │
│  --json                         NDJSON interface（xlings interface，协议 ≥1.6 握手）                     │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────┘
        │ xlings（包、环境）                              │ luban（OS：edition、镜像、驱动器、机器）
        │                                                 │  命令即 xlings 命令的映射，只有一份实现
┌───────┴─────────────────────────────────────────────────┴──────────────────────────────────────────────┐
│ src/（前端 + 核心，尚未拆完）   xim（解析、安装、索引）  xvm（版本、shim）  xself（init/update/doctor）    │
│                                subos/cmd、sandbox 适配、root（代、导出）、domain、carrier_image          │
├──────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ luban/   boot（启动项）  stage0（luban-init，第一个进程）  machine（/etc、sysusers）                    │
│          cli（luban 命令树）  image（GPT、FAT，进程内）            ── 只依赖 modules/，不依赖 src/        │
├──────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ modules/ subos（模型、策略、intent、persona、session、views/root）  confine（bwrap/proot/landlock/       │
│          home-redirect/fake，选择器，golden）  carrier（local/wsl2/vz）  store（GC roots、保留账本）      │
│          cli（命令模型）  ui  runtime（取消、guard、observe）  libs（json、sha256、tinyhttps）            │
├──────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ modules/platform —— 唯一的 OS 边界（系统头、系统调用、if constexpr 选平台）  ── 将来可移植部分走 openkal │
└──────────────────────────────────────────────────────────────────────────────────────────────────────┘
        │                    │                    │                     │
   宿主 Linux 内核        WSL2（Windows）       VZ（macOS）       自己的内核：qemu（L3）/ ISO / 真机（R 模式）
   （视图：叠加/沙箱/根）  （Linux SubOS 的承载）  （同左，助手待发布）  （将来：Luban 内核，语义对齐 openkal）
```

依赖方向（lint 强制）：`src → luban → modules → modules/platform`。系统头只在 platform 里。外部程序只从一个工具表取，提权只走一个入口。

### 2.1 三个正交维度

| 维度 | 取值 | 由谁决定 | 用户看到的 |
|---|---|---|---|
| 内容 | workspace（包）、home（用户数据）、policy（策略） | 用户 / edition / 策略包 | "环境"及其设置 |
| 视图 | overlay（PATH）、sandbox（隔离的宿主视图）、root（自己的 `/usr`，代）、machine（整机） | `subos new` 的形态 / edition | 进入后的样子 |
| 承载 | local、wsl2、vz、native（R 模式）、qemu（`luban try`） | payload 的 ABI，加上用户的选择 | 在哪里运行（概要第一行） |

### 2.2 Luban 分层

```
                 ┌────────────── edition（日期版本，不可变快照，from 链） ──────────────┐
  第三方 edition │  your-index:your-os（from 任意官方层）                                │
                 │  luban-desktop（preview，待迁移）   luban-agent-workspace（+ 策略）     │
                 │  luban-core（GNU 工具、gcc、mcpp、git、编辑器、shell）                 │
                 │  luban-tiny（busybox、glibc、CA、busybox init）  luban-tiny-musl（新）  │
                 │  luban-nano（只有 xlings + luban；无 libc、无 shell、无 init）          │
                 └───────────────────────────────────────────────────────────────────────┘
  启动层（新）    luban-boot-generic（真机） / luban-boot-virt（VM、L3、try）
                  = 内核包 + limine + boot.json（命令行、console、init 方式）
  内核            linux-kernel（generic） / linux-kernel-virt（新） / 将来的 Luban 内核
```

交付形态：环境（宿主内核上）、rootfs（tar.zst：docker import / wsl --import）、驱动器镜像（GPT，BIOS+UEFI，ext4 根）、qcow2、live ISO（根作为 initramfs，limine）。

### 2.3 agent 私有工作区的三档

| 档 | 形态 | 网络 | 身份 | 共享内核能看到的 | 状态 |
|---|---|---|---|---|---|
| L1 | agent-confined（沙箱，宿主网络） | 宿主的 | 宿主的 | 全部 | 已实现 |
| L2 | agent-private（根视图，宿主内核） | 只有代理（socks5h，不回落） | persona（主机名、machine-id、代理出口时区） | 内核版本、CPU、绑定进来的宿主路径（mountinfo） | 已实现；宿主路径待 A7 消除 |
| L3 | 同一 edition 跑在自己的 virt 内核上 | 只有代理（guestfwd → `luban __pipe`） | persona + 虚拟硬件 | 基本没有（剩下的是 CPU 特性、时序） | `luban try --proxy` 已有；常驻形态待做 |

---

## 3. 生态总图

```
                         ┌──────────────── mcpplibs/libxpkg（xpkg 规范与执行器，不改） ───────────────┐
                         │                                                                              │
┌────────────── openxlings/xlings ─────────────┐        ┌────────────── openxlings/xim-pkgindex ───────────────┐
│ xlings + luban（同一日期版本，同一发布包）     │ 读索引 │ pkgs/：上游软件（gcc、glibc、busybox、kernel、limine…）│
│ release.yml → GitHub Release                  │◄──────┤        edition（type=subos，namespace subos）          │
│ 本地 gtc → GitCode（mirror-latest.sh）         │        │        策略（type=subos-policy）                        │
│ fresh-install CI 测"用户第一次装到的东西"       │        │        启动层（type=package，日期版本）                  │
└──────────────┬───────────────────────────────┘        │ libs/luban.lua：edition / policy 的唯一写盘实现         │
               │ bump(xlings) PR：4 平台 sha256 + latest │ CI：静态（数据驱动）、已发布不变 golden、                │
               └────────────────────────────────────────►│     定时真实验收（全部 edition × 进入 × ISO × qemu）     │
                                                         └───────────────┬──────────────────────────────────────┘
┌────────────── xlings-res（GitHub + GitCode 镜像） ──────────┐           │ url（GLOBAL / CN）
│ 预编译 payload：glibc、busybox、kernel、limine（新）…        │◄──────────┘
│ CI 上传 GitHub；大文件由本地 gtc 补 GitCode，GET 校验 sha256 │
└─────────────────────────────────────────────────────────────┘
┌────────────── 第三方索引 ──────────────┐
│ 自己的 edition（from 官方层）、策略、包 │  luban new box your-index:your-os → 导出 ISO / 驱动器
└────────────────────────────────────────┘
```

### 3.1 发布节奏与依赖顺序

1. xlings 先发布：release，然后 GitCode 镜像（gtc，GET 校验），然后索引 bump PR，最后重跑 main CI。
2. 索引后合入。新 edition 用到新客户端字段时，在 manifest 里写 `min_client`。
3. 定时验收持续检查"已发布的 xlings × 已发布的索引"。xlings 的 PR 可以把自己的构建产物交给索引验收，在合入前就跑。

### 3.2 包的分类（都在现有 xpkg 规范内）

| 类别 | type | namespace | 版本 | 例 | 谁读 |
|---|---|---|---|---|---|
| 上游软件 | package | xim | 上游版本（可加 `-rN`） | gcc、glibc、busybox、linux-kernel(-virt)、limine | 安装器 |
| edition | subos | subos | 发布日期 `YYYY.M.D.N` | luban-nano/tiny/tiny-musl/core/agent-workspace | `subos new --from`、`luban new` |
| 策略 | subos-policy | xim | 发布日期 | agent-private、agent-confined | 策略编译器（confine） |
| 启动层 | package | xim | 发布日期 | luban-boot-generic、luban-boot-virt | `export` / `try` |
| 共享库 | 索引 `libs/` | — | 跟随索引 | `xim.pkgindex.luban` | recipe |

规则：
- 已发布版本的 manifest 永不修改（golden 证明）。
- 引用一律通过 `latest` ref 或精确版本。
- 日期版本只用于 Luban 自己的包。

---

## 4. 演进路线（阶段与依赖）

```
 R0 收尾 ──► R1 包设计 v2 ──► R2 环境升级 ──► R4 官方镜像
   │            │                 │              ▲
   │            └──► R3 启动层 ────┴──► R5 L3 常驻私有机
   └──► R6 隐私补全（A7 /xlings、非 root agent）──► R5
                                                R7 生态扩展（aarch64 全线、musl core、Secure Boot、桌面、第三方指南）
```

| 阶段 | 内容 | 仓库 | 依赖 |
|---|---|---|---|
| **R0 收尾** | bwrap 探测（#653 内容）；stage-0 预算的间歇失败（§6.2）；#945 真实验收通过 | xlings、索引 | — |
| **R1 包设计 v2** | `libs/luban.lua`；edition 数据化 + golden；tiny/core 新版本（from nano，luban-init）；limine 预编译（xlings-res + gtc）；luban-tiny-musl；aarch64 到 tiny（busybox、kernel 的 aarch64 payload）；desktop 改为 preview；定时验收；验收接受 PR 构建 | 索引为主，xlings 配合（`min_client`、desktop 短名） | R0 |
| **R2 环境升级** | `instance.json.edition`（来源和 edition 层的包）；不带版本的包在创建时解析并锁定；`luban upgrade` / `subos upgrade`（新的一代，可回滚，保留用户层，策略放宽需要确认）；agent-workspace 改为不带版本的 claude | xlings → 索引 | R1 |
| **R3 启动层** | luban-boot-generic / -virt、linux-kernel-virt；manifest `boot.profile`；`luban export --boot`；`try` 默认 virt | 两边 | R1 |
| **R6 隐私补全** | 根视图的 store 改为中性的 `/xlings`（A7：mountinfo 不再露出宿主路径，镜像路径中性）；agent 改为 sysusers 的非 root 用户 | xlings | R0 |
| **R5 L3 常驻私有机** | 同一 edition 常驻在 virt 内核 VM 里：持久盘、代理是唯一出口、`luban enter/run` 透明转发（承载 = qemu），status 如实列出 | xlings | R3、R6 |
| **R4 官方镜像** | CI 产出 nano/tiny/core/agent-workspace 的 ISO 和 qcow2，随发布同步 GitCode；`luban install <设备>`（从 live 安装自己） | 两边 | R2、R3 |
| **R7 生态扩展** | aarch64 全线、musl core（musl 工具链包）、Secure Boot（需要签名的 shim）、桌面图形栈、第三方 edition 指南、vz 助手发布、openkal 接入（§4.3 前置条件） | 全部 | R4 |

每个阶段在每个仓库都是一个 PR，commit 作为检查点；版本带日期。

---

## 5. xim-pkgindex 包设计 v2（在现有 xpkg 规范内）

1. **edition 写成数据。** recipe 只有 `package` 表、`editions[version] = {manifest, files}` 数据，以及一行 `luban.edition("Core", editions)`。os-release、factory `/etc` 由 `libs/luban.lua` 统一生成；策略用 `luban.policy(json)`。第一个 commit 只做迁移：已发布版本写出的字节不变（golden 证明）。
2. **环境升级。** xpkg 侧不用加任何东西（多版本并存 + `latest`），由客户端记录来源、提供 `upgrade`（§4 R2）。
3. **agent 工具。** 不带版本，创建时解析并锁定到实例。edition 不可变，它的含义就是"创建时最新的 claude"。换别的 agent 就在环境里 `xlings install`，或者写一个 `from` 它的薄 edition。
4. **启动层成包。** `type=package`，deps 是内核和 limine，带一个 `share/luban/boot.json`；manifest 里写 `boot.profile`。旧的 `boot.kernel` 继续兼容。
5. **limine 预编译。** 索引 CI 合成一个 tarball（启动文件 + 静态编译的工具），发布到 xlings-res（GitHub）；GitCode 由本地 gtc 补。用户机器不再需要 cc。
6. **一致性。**
   - tiny 改为 from nano；新 edition 用 luban-init；
   - 新增 luban-tiny-musl；
   - aarch64 做到 tiny；
   - desktop 改为 preview；
   - agent 策略的 macOS/Windows 声明写明"在 wsl2/vz 承载里"。

---

## 6. 自我 review

每条写结论、风险和应对。"⚠"表示需要在路线里专门处理。

### 6.1 架构
- **结论：** 分层成立。
  - 依赖方向由 lint 强制；
  - luban 是 xlings 之上的一层而不是第二套实现；
  - 内容 × 视图 × 承载三维正交，新形态（L3、真机）都是"同一份内容换一个承载"，不需要新名词。
- **⚠ `src/` 还没有拆完。** luban 需要的能力只能经过 xlings 进程（命令映射）。目前可行，但 `luban upgrade` 这类新命令会让 `src/core/subos/cmd.cpp` 继续变大。
  - 应对：R2 起，新逻辑放进 `modules/subos`（可测、可复用），cmd 只做解析。
- **openkal 对齐只是方向，不是承诺。** 前置条件（单一 C 运行时、句柄传递）没有落实之前，不进入路线的具体阶段。这一点合理，保持不变。

### 6.2 稳定性
- **⚠ 宿主差异是最大的不稳定源。** 2026.10.10 一天发了 .1/.2/.3 三个版本，三次修的都是宿主差异：AppArmor 限制下 supervisor 自己 unshare 没有 caps、setuid 的 bwrap 不支持 `--disable-userns`、终端前台进程组。共同点是 CI 的宿主太"干净"（关掉限制、免密 sudo），而真实用户的机器千差万别。
  - 应对：建立宿主矩阵 CI：
    - Ubuntu 24.04（受限）；
    - Ubuntu 22.04；
    - Debian；
    - Fedora（SELinux）；
    - Arch；
    - setuid 和非 setuid 的 bwrap。
  
  每个都跑"创建 → 进入 → 每个预设策略"。探测要测策略真正用到的能力，而不只是"能不能进入"。
- **一个 bug 让 xlings 发两个版本，因为验收在发布之后才跑。** 应对：索引验收接受 PR 构建（R1）。
- 一次性设置只走一个提权入口；在下载之前检查；读不出来的不当作空。这些规则在新代码里继续适用。
- **⚠ 已知的间歇失败：** `rootfs_boot_test.sh` 的 stage-0 预算是 200 ms。#650 期间测到过 1.6 s，#653 测到 2.2 s，都出现在 distro 车道的 qemu 里，与改动无关。可能的原因有两个：没有 KVM、只能软件模拟（TCG）时这个预算没有意义；或者某一次启动里有慢的一步。
  - 应对（R0）：输出 stage-0 各阶段的耗时；预算只在有 KVM 时强制执行，没有 KVM 时只报告数字。查明是哪一步之后再定预算。

### 6.3 优雅简洁
- **用户面对的名词是 4 个：** 环境、edition、策略、镜像。`luban --help` 只列 5 个常用命令。
- **包设计 v2 减少重复：** 4 份写盘代码合成 1 份。
- **启动层新增两个包，但用户默认看不到**：导出时自动选择，`--boot` 只是高级选项。
- **⚠ xlings 和 luban 是两个工具，用户可能分不清该用哪个。**
  - 规则：环境和 OS 用 luban，包用 xlings，一句话就能说清。
  - `luban <cmd> --help` 写出对应的 xlings 命令。
  - 需要在文档首页反复强调这一条。

### 6.4 用户体验
- **一致性承诺：** 宿主上的环境、VM、ISO、真机，命令和认知相同。真机上用的也是同一个 `luban` 程序。
- **⚠ "环境升级"是用户会自然期待、但现在没有的能力。** 用户在一个 edition 快照上停留越久，安全债越大。R2 是用户体验上最高的优先级。
- **人和 agent 分开：** agent 模式从不提问，问题都变成结构化错误。写驱动器时 agent 必须给出序列号。

### 6.5 兼容性与无感升级
- **只追加：**
  - `instance.json` 只追加字段（`root_abi`、`boot`，将来的 `edition`）；
  - 已发布的 edition 不变；
  - `0.1.0` 继续能用。
- **⚠ `min_client` 的缺口。** 2026.10.10.2 及更早的客户端不认识 edition manifest 里的 `min_client`。新 edition 在旧客户端上会退化（没有启动层；agent 装的是最新版而不是锁定的版本），但不会损坏。
  - 应对：新字段一律设计成"旧客户端忽略后仍然安全"，这一条写进索引的规范说明。
- **⚠ 策略升级的方向。** 收紧可以随升级生效；放宽必须列出并确认，否则"无感升级"会变成"无感降低隔离"。在 R2 里实现，并写进 requirement。
- **版本比较已经支持数字多段**，`2026.10.10.1` 和 `0.1.0` 的顺序正确。引用仍然通过 `latest` ref，不依赖比较。

### 6.6 跨平台
- **用法一致，实现借用平台：** Windows 用 wsl2，macOS 用 vz。
- **⚠ vz 助手还没有发布。** macOS 上 Luban edition 目前只能如实拒绝。R7 之前不应在文档里暗示 macOS 可用。
- 镜像和写驱动器只在 Linux 上提供；其他平台给出路线说明。

### 6.7 一致性
- 日期版本覆盖 xlings、luban、edition、策略、启动层；上游软件保持上游版本。规则只有一条。
- **⚠ 遗留名字。** xlings-init 和 luban-init 并存（兼容所需）。新 edition 只用 luban-init；文档只提 luban-init。

### 6.8 安全与隐私
- **L2 的边界写得很诚实：** 共享内核无法隐藏的东西，status 如实列出。
- **⚠ A7（宿主路径通过 mountinfo 露出）是 L2 剩下的最大泄漏点。** 非 root 的 agent 是纵深防御。R6 应排在 R5 之前或与它并行。
- **代理是唯一出口，不回落。** 时区查询经过代理，结果按代理缓存。
- **⚠ 供应链。** edition 和策略是"决定隔离强度"的包。recipe 改成纯数据之后，审查面变小；"已发布不变"的 golden 防止被悄悄修改。下一步可以考虑给策略包签名，这是规范层面的事，留到以后。

### 6.9 生态与治理
- 官方索引同时承载上游软件和 Luban 层，好处是一次解析、一个镜像。代价是 PR 范围容易混在一起（#945 里夹带了 gcc/binutils 的改动）。
  - 应对：PR 描述里分区列出；跨领域的改动用独立的 commit。
- **第三方 edition** 只需要会写一个 `from` 官方层的数据 recipe，加上 `libs/luban.lua` 的用法。R7 出指南。
- **⚠ 资源镜像的人工步骤**（gtc 补 GitCode）仍然是发布的一部分。它已经写进 AGENTS.md。limine 等新资源沿用同一流程。

### 6.10 演进合理性
- **每个阶段都可以单独交付，并且在当时有用：**
  - R2 让已有用户受益；
  - R3 让镜像和 L3 共用一份配置；
  - R6 不依赖任何新包。
- **没有为远期目标提前抽象**（Luban 内核、openkal 只停留在边界说明）。
- **⚠ 范围风险：** R1 同时包含 musl、aarch64、limine 资源、定时验收，是一个大的索引 PR。
  - 应对：按 commit 检查点推进。aarch64 的 payload 构建（busybox、kernel 交叉构建）如果卡住，可以在 R1 内部降级为"nano + tiny 的 x86_64 先行"，但 commit 顺序保证可以单独审查。

---

## 7. 下一步

按路线从 R0 + R1 开始：
- **xlings 单 PR**：并入 #653，加上 `min_client`、desktop 短名、R2 的前置字段，版本带日期。
- **索引**：重排 #945，按 §5 的 commit 顺序推进。

两边 CI 全绿后，按既定流程发布、镜像、bump，再合入索引，最后做真实验证。
