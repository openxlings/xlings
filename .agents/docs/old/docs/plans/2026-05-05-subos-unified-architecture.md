# xlings subos 统一架构分析（一套架构 vs 模式专属处理）

> 系列文档之四，与以下三份配套阅读：
> - `2026-05-05-subos-mount-namespace-analysis.md` — light 方案（轻量 `/usr/local/*` 覆盖）
> - `2026-05-05-subos-full-isolation-analysis.md` — full 方案（独立 rootfs）
> - `2026-05-05-subos-tiered-mode-architecture.md` — 四档分级总览
>
> 本文回答的核心问题：**这四档（light / medium / heavy / full）能用一套架构实现吗？还是各有独立的依赖和特殊处理？**

---

## 0. 一句话答案

**能用一套架构实现。约 70% 共享骨架 + 30% 显式插件化的差异点。**

差异不是实现细节问题，而是模式语义本身的天然分歧。把分歧用明确的策略接口隔离，新增模式或后端只动叶子节点，骨架代码保持稳定。

---

## 1. 背景：四档模式回顾

| 档位 | 一句话定义 | 隔离手段 | 体积 | 启动延迟 |
|------|----------|---------|------|---------|
| **light** | `local/*` 覆盖 `/usr/local/*` | mount-ns + 5 bind | 几 MB | <50ms |
| **medium** | + 私有 `$HOME` / `/opt` / `/tmp` | mount-ns + 10 bind | 几 MB | <100ms |
| **heavy** | OverlayFS 完整视图 + pivot_root | bwrap + overlay | 几 MB（差量） | 200-500ms |
| **full** | 独立 rootfs（Alpine/Debian） | bwrap 或 nspawn | 10-500MB | 0.5-2s |

详见 `2026-05-05-subos-tiered-mode-architecture.md` §2-§3。

---

## 2. 共享骨架（4 档完全相同的部分，约 70%）

无论用户选哪一档，下列模块**逻辑完全一致**，只用一份实现：

| 模块 | 内容 |
|------|------|
| **命令层** | `subos {new, use, enter, run, exec, list, info, remove, upgrade, export, import, discard}` 命令解析与路由 |
| **配置 schema** | `.xlings.json` 统一 schema（`mode` 字段 + 各 mode 子段）；读写、版本迁移逻辑 |
| **目录所有权** | `$XLINGS_HOME/subos/<name>/` 路径解析、生命周期、引用计数（与 payload 共享机制对接） |
| **EventStream 事件** | `subos_created`、`subos_switched`、`subos_entered`、`subos_exited`、错误码分类——与现有 `cli.cppm` 一致的事件流 |
| **Hook 系统** | `pre-enter.sh`、`post-enter.sh` 钩子模板与执行 |
| **Mode 仲裁器** | 读 `.xlings.json` 的 `mode` → 探测内核能力 → 决定实际生效 mode（autodegrade）；仲裁结果是策略点的输入 |
| **平台分支** | Linux / macOS / Windows 一级分支；非 Linux 直接走 PATH 模式 |
| **错误码与诊断** | `unprivileged_userns_disabled`、`overlay_unsupported`、`bwrap_not_found` 等统一错误码与提示文本 |

这部分代码是 xlings 自有逻辑、跨档复用，不依赖任何外部容器工具。

---

## 3. 真实差异点（无法消除的部分，约 30%）

下列差异**不是实现选择**，是模式语义本身的差异。每条说明为什么必须分支处理：

| # | 差异点 | 为什么不能合并 |
|---|--------|---------------|
| 1 | **Bootstrap I/O** | full 需要下载 + 校验 + 解压 base image（curl + sha256 + tar），其他 mode 只 `mkdir`。工作量差 100×，必须独立模块 |
| 2 | **MountPlan 内容** | 4 档的挂载列表、根策略、tmpfs 数量、`/etc` 处理完全不同。不能用一个模板参数化——每档逻辑都需要自己的 builder |
| 3 | **后端能力差异** | unshare 不能做 unprivileged OverlayFS；bwrap 不能做 PID-ns 1（除非 `--as-pid2`）；nspawn 强依赖 systemd。后端各有命令语法和能力盲区，必须有 backend trait 抽象 |
| 4 | **`/etc` 策略表** | heavy / full 需要一份**精心维护**的 `/etc` 路径清单（哪些 A、哪些 H、哪些 stub）。light / medium 完全不碰。这是数据，不是代码——以资源文件存在，每档独立 |
| 5 | **内核能力检测** | light/medium 只查 `unprivileged_userns_clone`；heavy 还要查 OverlayFS 是否支持 unprivileged mount；full(nspawn) 查 systemd 版本。检测项是 mode 的**输入条件**，每档独立 |
| 6 | **Autodegrade 链** | `full → heavy` 没有自然语义（rootfs 不能退成 overlay）；`heavy → medium` 有（丢弃 overlay，留 home/local）；`medium → light` 有；`light → PATH-only` 有。降级矩阵需要逐对定义，不是单一函数 |
| 7 | **Export 格式** | light/medium：tar 整个 subos 目录；heavy：upper 含 character device whiteout，要转 OCI 标准；full：tar `rootfs/`。三种格式，三套打包流水线 |
| 8 | **平台降级路径** | light/medium 在 macOS/Windows 退到 PATH-only 仍可用；heavy/full 没有降级路径——只能拒绝并提示用 Lima/WSL2 |

---

## 4. 推荐架构：共享骨架 + 三类策略点

```
┌────────────────────────────────────────────────────────────┐
│  命令层  subos {new, enter, run, …}                        │  ← 共享
├────────────────────────────────────────────────────────────┤
│  Mode 仲裁  (read .xlings.json → probe → autodegrade)      │  ← 共享
├────────────────────────────────────────────────────────────┤
│  ┌─ Bootstrap 策略 ────┐  ┌─ MountPlan 构建 ──┐            │  ← 策略点
│  │ NoOp (light/med/hv)│  │ LightPlan          │            │
│  │ ImageFetch (full)   │  │ MediumPlan         │            │
│  └────────────────────┘  │ HeavyPlan          │            │
│                          │ FullPlan           │            │
│                          └────────────────────┘            │
├────────────────────────────────────────────────────────────┤
│  Backend Driver 接口  execute(MountPlan)                    │  ← 策略点
│  ┌──────────────┐  ┌───────────┐  ┌──────────────┐          │
│  │ UnshareDriver│  │ BwrapDrv  │  │ NspawnDriver │          │
│  └──────────────┘  └───────────┘  └──────────────┘          │
├────────────────────────────────────────────────────────────┤
│  Export 策略  ┌─ TarPlain ─┐ ┌─ OverlayToOCI ─┐ ┌─ Rootfs ─┐│  ← 策略点
└────────────────────────────────────────────────────────────┘
```

骨架代码只处理共享逻辑；策略点是 trait（C++ concept / 接口类）+ 多个具体实现。

---

## 5. 关键抽象：MountPlan

把 4 档差异收敛到一个**声明式数据结构**，让骨架代码只处理数据、后端只消费数据：

```cpp
struct MountPlan {
    enum class Root { Passthrough, Overlay, Rootfs };
    Root root_strategy;

    // overlay
    fs::path overlay_lower;     // 仅 heavy: 通常 "/"
    fs::path overlay_upper;
    fs::path overlay_work;

    // rootfs
    fs::path rootfs_dir;        // 仅 full

    // 通用
    std::vector<BindMount>   binds;     // 源、目标、ro/rw、可选 missing
    std::vector<TmpfsMount>  tmpfs;     // 目标、size、mode
    std::vector<KernelMount> kernel;    // proc / sysfs / devfs / devpts
    std::map<std::string, std::string> env;
    std::optional<std::string> hostname;
    std::vector<std::string> shell_cmd;

    // 后端能力声明（驱动用来判断能不能跑）
    struct Caps {
        bool need_user_ns = true;
        bool need_overlay = false;     // heavy
        bool need_pivot_root = false;  // heavy / full
        bool need_pid_ns = false;
        bool need_uts_ns = false;
    } caps;
};
```

每档实现 `MountPlanBuilder::build(SubosConfig) → MountPlan`：
- `LightPlan::build`：5 个 BindMount，`root_strategy = Passthrough`
- `MediumPlan::build`：调 LightPlan + 4 个新 bind/tmpfs
- `HeavyPlan::build`：`root_strategy = Overlay` + procfs/sysfs/devfs + `/etc` 策略表展开
- `FullPlan::build`：`root_strategy = Rootfs` + 完整 mount 编排

后端实现 `BackendDriver::execute(MountPlan)`：
- 收到 plan 先 `caps_check`，能力不够返回错误（仲裁器据此 autodegrade）
- 能力够则把 plan 翻译成自己的命令格式（unshare 拼 shell、bwrap 拼参数、nspawn 拼参数）
- 启动子进程 + 等待 + 清理

**这一层抽象的核心收益：** 新增 mode 只写 `XxxPlanBuilder`，新增后端只写 `XxxDriver`。骨架不动。

---

## 6. 模块划分（代码组织建议）

```
src/core/subos/
├── subos.cppm                  (现有) — 命令 facade
├── mode.cppm                   (新)   — Mode enum、字符串解析
├── config_v2.cppm              (改)   — .xlings.json 新 schema
├── arbiter.cppm                (新)   — 仲裁器：probe → autodegrade
├── capability.cppm             (新)   — 内核/平台能力探测
├── plan/
│   ├── plan.cppm               (新)   — MountPlan 数据结构
│   ├── plan_light.cppm         (新)
│   ├── plan_medium.cppm        (新)
│   ├── plan_heavy.cppm         (新)
│   └── plan_full.cppm          (新)
├── backend/
│   ├── backend.cppm            (新)   — BackendDriver 接口
│   ├── backend_unshare.cppm    (新)
│   ├── backend_bwrap.cppm      (新)
│   └── backend_nspawn.cppm     (新)   — 可选/晚期
├── bootstrap/
│   ├── bootstrap.cppm          (新)   — Bootstrap 策略接口
│   ├── bootstrap_noop.cppm     (新)   — light/medium/heavy
│   └── bootstrap_image.cppm    (新)   — full（fetch + verify + extract）
├── export/
│   ├── export.cppm             (新)   — Export 策略接口
│   ├── export_tar.cppm         (新)
│   ├── export_overlay.cppm     (新)
│   └── export_rootfs.cppm      (新)
└── etc_policy/
    ├── heavy.toml              (新)   — heavy 模式 `/etc` 处理清单（数据）
    └── full_alpine.toml        (新)   — full+alpine 模式 `/etc` 处理清单
```

---

## 7. 各模式专属依赖一览

| 依赖 | light | medium | heavy | full(bwrap) | full(nspawn) |
|------|:----:|:------:|:-----:|:----------:|:----------:|
| `unshare(2)` | ✓ | ✓ | ✓ | — | — |
| `bubblewrap` 二进制 | — | — | ✓ | ✓ | — |
| `systemd-nspawn` | — | — | — | — | ✓ |
| `curl` / `wget` | — | — | — | ✓（fetch） | ✓ |
| `tar` + `zstd`/`gzip` | — | — | — | ✓ | ✓ |
| `sha256sum` | — | — | — | ✓ | ✓ |
| Linux 内核 ≥ 5.11 | — | — | ✓ | (推荐) | (推荐) |
| Linux 内核 ≥ 3.8 | ✓ | ✓ | — | ✓ | ✓ |
| systemd ≥ 252 | — | — | — | — | ✓ |

xlings 启动时探测可用项，配置 `.xlings.json` 时 mode 选择面随之收窄。

---

## 8. 渐进引入策略（不要一开始就建全套）

| 阶段 | 抽象层引入程度 | 理由 |
|------|-------------|------|
| **M1**（仅 light） | 只有简单 `EnterContext` 结构 + 一个函数；不引入 MountPlan / BackendDriver 抽象 | 一个 mode 不需要插件化 |
| **M2**（+ medium） | 抽出 `MountPlan` 数据结构；两个 PlanBuilder；仍单后端（unshare） | 两个 mode 已经能看出共性，正好抽数据结构 |
| **M3**（+ heavy） | 引入 `BackendDriver` 接口；新增 BwrapDriver；引入 `Capability` 探测 | 出现第二种后端时，自然需要 trait |
| **M4**（+ full） | 引入 `BootstrapStrategy`、`ExportStrategy`；可能加 NspawnDriver | 这是最复杂的 mode，所有抽象在此被复用 |

**反模式**：在 M1 阶段就把 MountPlan / BackendDriver / BootstrapStrategy / ExportStrategy 全部抽出来——这是为不存在的需求过度设计。**先简单再演化**，每个抽象都被两个以上具体场景验证后再固化。

---

## 9. 跨档生命周期一致性

虽然各档实现不同，**用户视角的命令行为应当一致**：

| 命令 | 各档行为差异 | 一致性保证 |
|------|------------|----------|
| `subos new` | 仅 full 需 bootstrap | 错误码、进度事件、终态都用同一套 EventStream |
| `subos enter` | 启动复杂度 1×～10× | 用户感知的"进入"语义一致：spawn 子 shell，`exit` 退出 |
| `subos run` | 同 enter | 退出码、stdio 语义一致 |
| `subos info` | 显示信息项不同（heavy 多 overlay 状态、full 多 base 信息） | 公共字段格式一致；扩展字段在 mode 段下 |
| `subos remove` | full 删除大量文件 | 进度事件统一；引用计数一致 |
| `subos export` | 格式不同 | 输出统一为 `<name>.xlings-subos` 容器，内含 `mode.txt` 标识 |
| `subos upgrade` | 升档语义因 mode 对而异 | 失败时事务回滚；成功统一回报 |

骨架定义的 EventStream 事件类型 + 错误码即是"一致性合同"。

---

## 10. 关键决策与风险

### 10.1 决策：MountPlan 是后端无关，还是后端感知？

**选择：后端无关 + 能力声明**。

MountPlan 描述"想达到的状态"，不描述"如何达到"。每个 plan 带 `Caps` 字段，驱动检查能力即可。

> 反例：在 plan 里写 `bwrap_args` 字段。这会让 plan 与 bwrap 绑死，新增 nspawn 后端就要重写 plan。

### 10.2 决策：autodegrade 默认开还是关？

**选择：默认开，环境变量 `XLINGS_NO_AUTODEGRADE=1` 关闭**。

降级时必须打印明显警告，避免用户误以为 heavy 在跑实则只是 medium。

### 10.3 决策：后端选择硬编码还是用户可配？

**选择：mode → 默认后端硬编码；用户可在 `.xlings.json` 显式覆盖**。

| mode | 默认后端 | 可选后端 |
|------|--------|--------|
| light | unshare | bwrap |
| medium | unshare | bwrap |
| heavy | bwrap | (无) |
| full | bwrap | nspawn |

避免用户配置爆炸，但保留灵活性。

### 10.4 风险：抽象层过早固化

C++ 概念/接口一旦上线，跨进程/跨版本兼容压力大。建议：
- 内部接口先用 `std::variant` + 静态分发（无 vtable）
- M3-M4 阶段稳定后再考虑动态多态
- 配置 schema 走版本号 + 迁移函数（`config_v2`）

### 10.5 风险：`/etc` 策略表维护成本

heavy / full 的 `/etc` 处理清单本质上是"人工维护的运行时元数据"。新发行版、新内核版本可能引入新的 `/etc` 文件。建议：
- 清单写成 TOML，与 xlings 二进制解耦
- 提供 `xlings subos doctor` 检测当前 subos 缺失的关键 `/etc` 条目
- 长期看可能演化为社区维护的策略包

---

## 11. 总结

**问：能用一套架构做吗？**
**答：能。共享骨架占七成、插件化差异占三成；插件点不是为了优雅，而是模式语义的天然分歧线；骨架统一让新增 mode/后端不动主干。**

**共享：**
命令层、配置 schema、目录约定、EventStream、Hook、平台检测、Mode 仲裁、Autodegrade 框架、生命周期合同。

**插件化：**
- MountPlan 构建（light / medium / heavy / full）
- Backend Driver（unshare / bwrap / nspawn）
- Bootstrap 策略（noop / image-fetch）
- Export 策略（tar / overlay-to-OCI / rootfs）
- `/etc` 处理清单（数据文件）

**实施节奏：**
M1 不抽象、M2 抽 MountPlan、M3 抽 BackendDriver、M4 抽 Bootstrap/Export。每个抽象至少被 2 个具体场景验证后再固化，避免过度设计。

**xlings 的核心价值不在写容器运行时，而在用一套统一抽象覆盖"工具版本切换→开发盒→可丢弃实验环境→可复现交付"全谱系，让用户按需付费。**
