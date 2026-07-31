# xvm × subos 架构评估：为什么每个修复都长成 workaround

**日期**: 2026-07-31
**类型**: 架构评估（analysis）
**基线**: `78e7775` = `2026.7.31.1`
**范围**: `src/core/xvm/**`、`src/core/subos.cppm`、`src/core/config.cppm` 的 subos 解析、`src/core/xim/installer.cppm` 的物化路径
**不含**: 任何代码改动。本文只做分析与方向建议。

---

## 0. 摘要

过去几轮修的东西（执行期路径归一化、`subos/current` 写法、`sysroot` 布尔字段、doctor 的检测/修复基准）**每一个单独看都成立，合起来看都是 workaround**。它们绕的是同一堵墙。

这堵墙有五个面。A–D 是内部结构，E 直接暴露在命令行上：

| # | 问题 | 一句话 | 症状 |
|---|---|---|---|
| **A** | **"subos" 是三个概念共用一个名字** | 选择域 / 物化视图 / 沙箱运行时 | 讨论"某状态该不该 per-subos"时永远说不清，因为三个答案不同 |
| **B** | **"当前是哪个 subos" 没有唯一权威** | 代码里有 7 种拼法，install / use / remove 用了**不同的三种** | `-g` 装进去的东西，`remove` 去另一个 subos 找 |
| **C** | **xvm 记录存的是"物化后的结果"而不是"意图"** | 一条完整命令行、一个绝对路径 | 每出现一个与环境相关的需求，就得加一个字段或加一次字符串改写 |
| **D** | **物化逻辑三处重复** | install / use / remove 各实现一遍 | 三处对 scope 和 kind 的判据都不一致 |
| **E** | **`use` 一个动词做三件语义不同的事** | 临时/持久、隔离与否两个正交轴压进一个动词 | 用户可见；也是 `subos/current` 跟不上环境变量的成因 |

C 是**直接病因**，B 让它不可能被彻底修对，A 让人无法判断修得对不对，D 让每个修复都要做三遍。E 是独立的一条 —— 与前四条无因果关系，但它同样源于"两个正交的轴被压进一个名字"，和 A 是同一种错误的两个层面（A 在模块上，E 在命令上）。

---

## 1. 现有机制是什么（先把事实摆平）

### 1.1 存储分层（实测，非推断）

```
~/.xlings/.xlings.json         versions.<name>.versions.<ver>.{path,alias,envs,binding*}
                               → 整个 home 共享一份
~/.xlings/data/xpkgs/…         payload（二进制 + 自带配置）
                               → 整个 home 共享，引用计数
~/.xlings/subos/<n>/.xlings.json   workspace: {name: {active, installed[]}}
                               → per-subos
~/.xlings/subos/<n>/{bin,lib,usr}  shim / 库 / 头文件的物化视图
                               → per-subos
```

**版本库和 payload 在同一侧：共享。** workspace 和物化视图在另一侧：per-subos。

这条线是整篇分析的坐标轴。`VData::fileSrc/fileDst` 的注释早就把规则写清楚了：

> "A payload is shared between subos and reference-counted, so an absolute
> destination recorded against it would be right for the subos that installed
> it and wrong for every other one."

**这条规则对整个版本库成立，但当年只被应用到了 `files` 这一种 kind。**

### 1.2 四种 kind，四套物化机制

| kind | 记录什么 | 物化成什么 |
|---|---|---|
| `program` | `path` + `sourceName`/`filename` + `alias` + `envs` | `subos/<n>/bin/<name>` 的 shim |
| `lib` | `path` + `sourceName` + `destinationName` | `subos/<n>/lib/<destName>` |
| `files` | `fileSrc`（相对 payload）+ `fileDst`（相对 subos） | `subos/<n>/<fileDst>` |
| `group` | 什么都不指 | 无物化，只是一个挂 release 的名字 |

加上不属于任何 kind 的**头文件轴**（`includedir` / `bindingHeaders` → `subos/<n>/usr/include/*` 符号链接）。

### 1.3 绑定：两套表示并存

`BindingSource` 有两个值：`ProviderGroup`（0.4.70 起）和 `LegacyGraph`（此前的 pairwise 边，`VInfo::bindings[peer][myVer] = peerVer`）。两者同时被支持，`BindingErrorKind` 有 **15 个枚举值**，其中至少 6 个（`RootReferenceMismatch` / `GroupIdentityMismatch` / `AsymmetricEdge` / `SelfEdge` / `ProviderMetadataInLegacyGraph` / `PartialProviderMetadata`）纯粹是**为了调和这两套表示而存在**。

---

## 2. 问题 A：`subos` 是三个概念

`src/core/subos.cppm` 有 **2285 行**，同一个模块里装着：

1. **选择域** —— 哪些版本在这个环境里是活的（`workspace`）；
2. **物化视图** —— 一棵 FHS 形状的树（`usr/include`、`lib`、`bin`），编译器把它当 sysroot；
3. **沙箱运行时** —— proot / bwrap 后端、存储镜像（V6 storage mode）、GPU 透传、`use_spawn_shell`。

三者的生命周期、失效模式、正确性判据完全不同：

| | 选择域 | 物化视图 | 沙箱 |
|---|---|---|---|
| 存在于 | 一个 JSON 段 | 文件系统 | 一个进程 |
| 出错表现 | 版本不对 | 头文件/库缺失或混装 | 起不来 / 权限 |
| 能否并存多个 | 能（每 shell 一个） | 能 | 能 |
| 切换成本 | 改一个字段 | 重新链接一堆文件 | 重启进程 |

**把它们叫同一个名字的后果**：像"subos 状态该不该共享"这种问题没有唯一答案 —— 选择域必须 per-subos，物化视图必须 per-subos，而**版本库不是 subos 的一部分却被当成了 subos 的一部分**。上一轮关于 `--sysroot` 该存哪的反复，本质就是在一个没有区分这三者的词汇表里讨论问题。

### 2.1 量一下：三者各占多少，以及它们实际共享什么

按段落归属拆 `subos.cppm`：

| 行数 | 段 | 属于 |
|---:|---|---|
| 74 | `SubosInfo` / `list_all` | 选择 + 视图 |
| 134 | `create`（建目录树） | 视图（**混入 storage**） |
| 142 | `new_from`（克隆） | 视图（**混入 storage**） |
| 154 | `use_global` / `use_emit_shell` | 选择 |
| **179** | Storage mode V6 | **沙箱** |
| **56** | Image storage helpers | **沙箱** |
| **404** | bind list + bwrap 后端 + 后端探测/自动安装 | **沙箱** |
| **413** | `use_sandbox_mode_` | **沙箱** |
| 136 | `use_spawn_shell` | 选择（进程级） |
| 377 | `use` / `remove` / `info` / `run` | 命令入口 |

**沙箱侧 1052 行 ≈ 全文件的 46%。**

关键在于它们**共享什么**。统计沙箱主体（第 955–1770 行，约 800 行）对版本管理概念的引用：

```
subos_dir            ×30   ← 只是一个要 bind-mount 的路径
storage              ×33   ← 沙箱自己的概念
XLINGS_ACTIVE_SUBOS  × 3   ← 往子进程 env 里塞
Config::paths()      × 1
versions             × 1   ← 而且是出现在注释里
```

**没有共享不变量。** 沙箱不读 workspace、不碰版本库、不管物化 —— 它要的只有"这棵树在哪"和"进去之后叫什么名字"。

### 2.2 唯一真正纠缠的地方，和它已经付过的账

`subos/<n>/.xlings.json` 一个文件同时装两个概念的状态：

```json
{
  "workspace": { ... },      ← 选择：哪个版本是活的
  "storage":   "image",      ← 沙箱：存储模式
  "imageSize": "50G"         ← 沙箱
}
```

而 `create()` 的签名把沙箱参数焊进了"建一个 subos"这个动作：
`create(name, customDir, StorageMode storage, imageSize, stream)` —— 建目录树的同时可能去 `mkfs.ext4` 一个稀疏镜像。

这个混淆**已经出过一次事故**，代码注释里记着：

> "earlier V6 auto-upgrade fused the two axes and made `subos use <image-subos>`
> silently require root, bwrap, and a working mount namespace **just to switch
> shells**."

仅仅为了换个 shell 就要求 root + bwrap + mount namespace —— 因为"这个 subos 用镜像存储"（沙箱属性）被当成了"进入这个 subos"（选择动作）的前提。修法是在注释里声明"两个轴是正交的"，然后靠**纪律**维持。**纪律正是模型本该替你保证的东西。**

---

## 2A. 问题 E：`use` 一个动词做三件语义不同的事（用户可见）

A/B/C/D 都是内部结构问题，这一条不是 —— 它直接暴露在命令行上。放在这里是因为它和 A 是**同一种错误的两个层面**：A 是"两个正交的轴挤进一个模块"，E 是"两个正交的轴挤进一个动词"。但它**与是否拆模块无关**，D5 做不做它都在（`subos.cppm:2118+` 的注释自己列着这三种模式）：

| 命令 | 实际做什么 | 改持久状态 | 起子进程 |
|---|---|:---:|:---:|
| `subos use <n>` | 起一个新 shell，env 设 `XLINGS_ACTIVE_SUBOS` | ❌ | ✅ |
| `subos use --global <n>` | 写 `activeSubos` + 更新 `subos/current` 链接 | ✅ | ❌ |
| `subos use <n> --sandbox` | 起 shell **＋ 文件系统隔离** | ❌ | ✅ |

前两者的差别是**临时 vs 持久**，第三者的差别是**隔离与否** —— 两个正交的轴被压进一个动词加两个 flag。

而且默认值反直觉：`xlings subos use dev` 读起来像"我切过去了"，实际是"我进了一个子 shell，`exit` 就回来"，**持久状态没变，`subos/current` 也没动**。

> 这正是上一轮 `subos/current` 方案跟不上 `XLINGS_ACTIVE_SUBOS` 的**根源**：`current` 由 `use_global` 维护，而日常用的 `subos use <n>` 走的是另一条路，根本不更新它。当时我把这归因于"symlink 跟不上环境变量"——那是现象；成因是**同一个动词的两条路径维护着不同的状态**。

---

## 3. 问题 B：「当前是哪个 subos」有 7 种拼法

代码里回答这个问题的方式：

| # | 写法 | 位置 |
|---|---|---|
| 1 | `Config::paths().subosDir` | 构造期算一次 |
| 2 | `Config::xvm_artifact_subos_dir()` | 每次调用重算 |
| 3 | `Config::global_subos_dir()` | |
| 4 | `Config::subos_dir(name)` | 按名字 |
| 5 | `paths().homeDir / "subos" / paths().activeSubos` | 手拼（`installer.cppm:1174`） |
| 6 | `paths().binDir` / `paths().libDir` | 由 #1 派生 |
| 7 | `Config::global_subos_bin_dir()` | |

`2026.7.30.1` 的注释说 `xvm_artifact_subos_dir()` 是"唯一解析点"。**实际上不是。**

### 3.1 三条主路径用了不同的解析器

| 路径 | 用哪个 | 证据 |
|---|---|---|
| **install**（物化进去） | `xvm_artifact_subos_dir()` | `installer.cppm:1432-1437` |
| **use**（切换） | `paths().subosDir` / `paths().libDir` | `xvm/commands.cppm:408,416` |
| **remove**（物化出来） | `paths().subosDir` / `paths().libDir` | `installer.cppm:1378-1379,1395` |

### 3.2 它们在什么时候分叉

`paths_` 的解析（`config.cppm:485-501`）：

```
globalActiveSubos_  →  XLINGS_ACTIVE_SUBOS  →  项目 subos（Named/Anonymous）
```

`xvm_artifact_subos_dir()` 的解析（`config.cppm:1133-1145`）：

```
项目 subos（仅当 hasProjectConfig_ && !forceGlobalScope_）  →  global_subos_dir_()
```

差别在 **`forceGlobalScope_`**（`xlings install -g`，`xim/commands.cppm:352` 设置）：

- `paths_` **完全不看** `forceGlobalScope_`，而且是**构造期算好的**，设置之后不会重算；
- `xvm_artifact_subos_dir()` 看。

于是在**项目目录里执行 `xlings install -g <pkg>`**：

```
install  →  写进 global subos          （artifact 解析器）
remove   →  去 project subos 找并清理    （paths 解析器）
use      →  在 project subos 里切换      （paths 解析器）
```

**装的和卸的不是同一个地方。** 这不是 workaround 能盖住的东西 —— 它是"同一个问题有两个权威答案"的必然结果。

> 注：本文只做静态分析，未构造该场景的运行时复现。但两个解析器对 `forceGlobalScope_` 的处理差异是代码事实，且 `paths_` 不重算也是代码事实。

---

## 4. 问题 C：记录存"结果"而不是"意图"

### 4.1 症状清单

`VData` 有 **18 个字段**，按 kind 只有子集有效：

- `program` 用 `path`/`sourceName`/`alias`/`envs`；
- `lib` 用 `path`/`sourceName`/`destinationName`；
- `files` 用 `fileSrc`/`fileDst`；
- `group` 一个都不用；
- `includedir` 跨 kind，且与 `bindingHeaders` **表示同一件事**；
- `libdir` **唯一的写入者是 JSON 反序列化器**（`db.cppm:801`）—— 也就是说它只能装下旧版本写进去的东西，当前代码里没有任何包能设置它。它是一个靠 round-trip 活着的死字段；
- `sysroot`（`2026.7.31.1` 加的）是第 18 个 —— **而它正是本文要说的那个模式的最新一例**。

tag 本身也不干净：`kind` 可缺省、回落到 `VInfo::type`，`types.cppm:170` 提供了 `effective_kind()` 让所有人口径一致 —— 然后：

| 读法 | 使用者 |
|---|---|
| `effective_kind(info, data)` | bindings、inspect、registration |
| 直接读 `type` | **`xvm/commands.cppm:487`（`use` 建 shim 的那一句）**、`xself/doctor.cppm`（4 处） |

也就是说，**决定"要不要给它建 shim"的那一行用的是回落值，不是权威值**。

### 4.2 病因

recipe 通过 `xvm.add` 能表达的只有**结果**：一个 `bindir`、一条完整的 `alias` 命令行、一组 `envs` 字符串。凡是与"当前环境"有关的东西，recipe 只能把**当时的答案**算出来写进去。

于是每出现一个环境相关的需求，就走一遍同样的循环：

```
recipe 写死一个绝对路径
  → 发现它在别的 subos 里是错的
  → 加一个执行期改写（normalize_subos_paths）
  → 发现记录本身还是脏的 → doctor 加一条 finding
  → finding 修不干净 → --fix 再加一条规则
  → 想彻底修 → 加一个字段（sysroot: bool）
  → 字段只覆盖 alias，覆盖不了 envs → 又是"已知部分解"
```

**`--sysroot` 那一轮把 GCC/Clang 的 flag 拼写写进了通用版本管理器的核心** —— 对 `-isysroot` 无效、对 `--gcc-toolchain=` 无效、对 envs 无效。它是这个循环的终点形态：当模型只能存结果，"通用"就只能靠穷举特例来近似。

### 4.3 正确的分界线在哪

> **记录应当只包含与 subos 无关的事实；一切与"当前是哪个 subos"有关的东西，必须在物化/执行时求值。**

按这条线检查现有字段：

| 字段 | 与 subos 无关？ | 结论 |
|---|:---:|---|
| `path`（payload 目录） | ✅ | 正确 |
| `sourceName` / `destinationName` | ✅ | 正确 |
| `fileSrc` / `fileDst`（都相对） | ✅ | **正确，且是唯一一开始就做对的** |
| `alias`（完整命令行，可含绝对路径） | ❌ | 病灶 |
| `envs`（值可含绝对路径） | ❌ | 病灶 |
| `includedir`（payload 内绝对路径） | ✅ | 正确但与 `bindingHeaders` 重复 |
| `sysroot: bool` | ✅ | 形式对了，但它是**特例化**的通用性 |

`files` 那一对是全表唯一从设计上就守住这条线的 —— 而且它守住的方式恰恰是**两端都相对**，也就是"存事实，不存答案"。

---

## 5. 问题 D：物化逻辑三处重复

`install_headers` / `remove_headers` / `place_library` / `remove_library` / `place_asset` / `remove_asset` —— 这六个动作被 **install（`installer.cppm`）和 use（`xvm/commands.cppm`）两个模块各自调度一遍**，remove 是第三处（`detach_current_subos_`）。`create_shim` 有 **9 个调用点**，散落在 installer、xvm/commands、xself/init、xself/doctor、common、libxpkg/types/script。

后果不是"代码重复"这种表面问题，而是：

1. **三处对"哪个 subos"意见不一致**（§3.1）；
2. **三处对 kind 的判据不一致**（§4.1）；
3. 一个 kind 新增了物化语义，得改三处 —— `files` 加进来时 `use` 侧就漏过一次，`lib` 侧则因为读了没有写入者的 `libdir` 而**整条静默失效**（`switch_plan.cppm:33` 的注释记着这件事）。

**install / use / remove 本质是同一个函数的三个方向**：

```
install:  ∅            → release R      （物化 R）
use:      release R₁   → release R₂      （撤 R₁ 物化 R₂）
remove:   release R    → ∅              （撤 R）
```

现在它们是三份独立实现，共享的只有最底层的六个动作函数。

---

## 6. 优化方向

按"能独立落地"排序，每条都注明它消掉的是哪个问题。

### D1. 把"作用域"变成显式参数（消 B）

现状是所有人向全局单例提问"当前是哪个 subos"，问法还有 7 种。

方向：定义一个显式值类型，比如

```
SubosScope { name, root }            // root = 那棵 FHS 树的绝对路径
```

由 CLI 入口解析**一次**（项目 / 环境变量 / 持久字段 / `-g`），然后**作为参数往下传**。`install(plan, scope)`、`use(target, ver, scope)`、`remove(target, ver, scope)`。

收益：
- `-g` 的分叉不可能发生 —— 只有一个 scope 值；
- 多 subos 操作（doctor 扫全部 subos）从"改全局状态再调用"变成"传另一个 scope"，不需要 `XLINGS_ACTIVE_SUBOS=<n>` 重新起进程；
- 可测试性：现在这三条路径都需要 Config 单例 + 真实文件系统才能测。

代价：签名扩散。可以先只在 xvm 三条主路径上做，Config 的 getter 保留为"默认 scope"。

### D2. 记录只存事实，环境相关的值用**核心自有的占位符**（消 C）

`expand_path()` 已经有先例：`${XLINGS_HOME}` 是核心自己的 marker，存进记录、执行时展开。把同一机制下推一层：

```
存：  alias = "g++ --sysroot=${XLINGS_SUBOS}"
     envs  = { PKG_CONFIG_PATH = "${XLINGS_SUBOS}/usr/lib/pkgconfig" }
执行： 用当前 scope 展开
```

关键性质：
- **核心只认识自己的 marker，不认识任何编译器 flag** —— `-isysroot`、`--gcc-toolchain=`、纯环境变量全都自动可用，因为语法归 recipe 管；
- **alias 和 envs 用同一条规则** —— `sysroot: bool` 覆盖不了 envs 的缺口消失；
- 写入端可以由核心在注册时**自动完成**（把 `<home>/subos/<n>` 换成 marker），recipe 不必改，也不需要 capability probe；
- doctor 的判据退化成一句话："记录里还有具体 subos 路径吗"，`--fix` 就是做同一次替换 —— 检测与修复同源，不会漂移。

代价：老客户端读到 marker 会拿到字面量。这是**任何**"记录里存意图"的方案都有的代价（`sysroot: bool` 版本是"丢掉 flag"，本质同类）。要么接受一个版本的地板，要么双写（marker + 具体路径），由新客户端优先取 marker。

> **这一条应当取代 `2026.7.31.1` 里的 `VData::sysroot`。** 那个字段是同一思路的特例化版本：思路对，粒度错。

### D3. 把物化收敛成一个方向可逆的 planner（消 D）

已经有半个了：`switch_plan.cppm` 把 `use` 的决策与执行分开了。把它推广：

```
plan_materialization(entry, scope, direction)  →  [Effect]
Effect = LinkHeaders | PlaceLib | PlaceFile | CreateShim | …（各自有反向）
apply(effects) / revert(effects)
```

install = `apply(plan(R, scope, Forward))`
remove = `apply(plan(R, scope, Reverse))`
use = `apply(plan(R₁, scope, Reverse) ++ plan(R₂, scope, Forward))`（已有的头文件去重/顺序逻辑保留）

收益：kind 的物化语义只写一次；三条路径不可能对 scope 或 kind 判据产生分歧；新增 kind 的成本从"改三处 + 记得别漏"变成"加一个 Effect"。

### D4. 用带 tag 的和类型取代宽 struct（消 C 的一半）

`VData` 现在是"18 个字段 + 一个可选 tag + 两种读 tag 的方式"。目标形状：

```
Entry {
    payload: PayloadRef          // 与 subos 无关的事实
    binding: Option<GroupRef>
    body: Program{…} | Lib{…} | Files{…} | Group
}
```

收益：`effective_kind` 的两种读法问题消失（tag 是构造出来的，不是推断的）；每个 kind 只能触及自己的字段；`libdir` 这种"只有反序列化器写"的死字段在迁移时自然暴露。

代价：一次 schema 迁移。`bindingUnreadable` 的无损 round-trip 机制正好是为这种事准备的。

### D5. 把 sandbox 从 subos 里拆出来（消 A）—— **实现层的拆，CLI 一个字不改**

> **先明确边界**：D5 是**模块拆分**，不是命令行拆分。用户看到的命令面保持原样。
> 命令行那边确实另有问题（问题 E），但那是**独立决策**，见 D7 —— 两者可以任意顺序做，也可以只做其中一个。

依据见 §2.1：沙箱侧 1052 行（46%）与版本管理模型的耦合只有**一个目录路径 + 一个环境变量名**，没有共享不变量。所以这不是重构，是**把两团本来就不粘的东西挪开**。

```
现在   subos.cppm   ──►  选择 + 视图 + 沙箱      （2285 行）
之后   subos.cppm   ──►  选择 + 视图            （~1200 行）
       sandbox.cppm ──►  后端探测 / bind list / storage / GPU / 进入
       CLI 层        ──►  按 flag 分派到两者      （命令面不变）

接口   sandbox::enter(scope: SubosScope, opts) -> int
       // scope 只需要 { name, root }
```

`create` 相应拆成 `subos::create(name, dir)` + `sandbox::prepare_storage(dir, mode, size)`，命令入口按用户给没给 `--storage` 决定要不要调第二个。

**为什么 CLI 不该跟着拆**：沙箱**永远是某个 subos 的沙箱**。提成顶层命令（`xlings sandbox …`）只会让用户每次重复指定 subos，把内部结构泄露成用户负担。用户心智里"subos 是一个环境，`--sandbox` 是进入它的一种方式"本身是对的。

**收益**主要是词汇上的，但它是前提：拆开之前，"这个状态该不该 per-subos"没有唯一答案（选择域要、视图要、沙箱另说），于是 D1（scope 显式化）和 D3（统一 planner）**没法判定自己做对了没有**。

**代价**：
- `subos/<n>/.xlings.json` 里两组字段的归属要定。**建议第一步不动文件格式**，只拆模块，让 `sandbox` 读它自己那几个 key；格式的合并/拆分留到有版本地板时再说。
- `create` 的签名要改 —— 内部 API 的 breaking change，外部 CLI 不变。

**风险很低**：没有共享不变量意味着拆错了会**立刻编译不过**，而不是运行时静默出错。这与 D2/D3 那种"改语义"的方向性质完全不同。

### D6. 绑定表示单一化（消 §1.3 的成本）

`LegacyGraph` 与 `ProviderGroup` 并存的成本是 15 个错误类型里的至少 6 个，外加每条查询都要走两遍。方向：`doctor --fix` 做一次性迁移（能力已具备），设一个版本地板，然后删掉 legacy 分支。

### D7. 拆开 `use` 的三种含义（消 E）—— **产品决策，不是架构决策**

与 D5 无关：不拆模块它也在，拆了模块它也不会自己消失。三个选项，按侵入性排序：

| | 做法 | 评价 |
|---|---|---|
| **A** | 不动行为，只把三种模式写进 `--help` 和文档 | 成本最低。承认超载，至少让人知道 |
| **B** | 分动词：`subos enter <n>`（子 shell，`--sandbox` 作它的正交修饰符）/ `subos switch <n>`（持久）；`use` 保留为兼容别名 | **推荐**。两个动词对应两个正交事实，`use` 不破坏任何脚本 |
| **C** | 把 `subos use <n>` 的默认语义改成持久切换 | **不推荐**。breaking change，而且"起子 shell"本身是好设计——可嵌套、可 `exit` 退出 |

选 B 的话，`subos/current` 该由谁维护也随之清楚：它是 `switch` 的产物，`enter` 不碰它 —— 这正是当前那个不一致的显式化。

---

## 7. 不建议的方向

- **不要继续加"某某需求"的专用字段**（`sysroot` 之后是 `rpath`?`pkgconfig`?）。每一个都会重现同一个循环，而且都覆盖不了 envs。
- **不要把 `subos/current` 当解法**。它只跟全局选择，跟不上 `XLINGS_ACTIVE_SUBOS` 和项目 subos —— 它是给**老客户端**的兜底，不是模型的一部分。
- **不要为了消除 doctor 的告警而改 recipe**。告警是模型不干净的**指示器**；先修模型，告警自己消失。反过来做，就是把温度计藏起来。
- **不要一次性重构**。D1/D2 可以独立落地并立刻减少缺陷面；D3/D4 需要迁移，应当排在有版本地板之后。

---

## 8. 建议的顺序

```
内部结构
  D1 (scope 显式化)  ──┐
                       ├─► D3 (统一 planner)  ──► D4 (和类型 + schema 迁移)
  D2 (占位符取代特例字段) ┘
  D5 (拆 sandbox)   —— 独立，随时可做，收益是词汇；D1/D3 的判定前提
  D6 (绑定单一化)   —— 需要版本地板，排最后

用户面（与上面无因果关系，可任意顺序）
  D7 (拆 `use` 的三种含义)  —— 产品决策；选 B 的话 `subos/current` 的归属随之明确
```

D1 与 D2 互不依赖，都能在一个版本内落地，且各自都能**独立减少**当前的缺陷面。D3 之后，"install 和 remove 作用在不同 subos"这类问题在类型层面就不可表达。

**D5 与 D7 容易被当成一回事，它们不是**：D5 换的是模块边界（用户无感），D7 换的是命令语义（用户可感）。只做 D5，命令行的超载原样保留；只做 D7，内部仍然是一个 2285 行的混合模块。

---

## 9. 与既有 issue 的关系

| | 本文定位 |
|---|---|
| [#458](https://github.com/openxlings/xlings/issues/458)（payload 内烧死路径不可见） | 同一条分界线的**另一侧**：payload 也必须与 subos 无关。D2 管不到它，需要单独的可见性机制 |
| [#408](https://github.com/openxlings/xlings/issues/408)（sysroot/bin/lib 多版本共存模型） | D3/D4 是它的前置：物化语义统一之后才谈得上共存模型 |
| [pkgindex#459](https://github.com/openxlings/xim-pkgindex/pull/459)（gcc 写 `subos/current`） | 按 §7 第二条，它是老客户端兜底，不应被当成方案本体 |
| `2026.7.31.1` 的 `VData::sysroot` | 按 D2 应当被占位符取代；它是"思路对、粒度错"的一例 |
