# xlings 生态三分层定位与可组合发行版:讨论纪要

**日期**: 2026-08-05
**类型**: 讨论纪要(discussion memo)—— 综合 2026-08-05 会话讨论,作为后续逐条深入的锚点,不是策略也不是实施计划
**触发**: 关于"xlings 生态究竟是什么、多 glibc 能不能做、可组合发行版如何表达、预构建 vs 源码构建如何取舍、消费者依赖硬钉能否用版本语义救"的连续追问
**关联**:
- `2026-08-05-userspace-distro-hermetic-strategy.md`(运行时边界策略,本文的**下位配套**)
- `2026-05-22-subos-sandbox-gpu-passthrough.md`(sandbox 层 GPU 设备节点透传)
- `2026-06-26-multiarch-package-description-design.md`(payload 多架构表达)
- `2026-06-21-linux-root-usability-survey.md`(权限边界)
- xim-pkgindex `docs/V2/xpackage-spec.md`(新字段/能力的 probe-first 迁移范式)

---

## 0. TL;DR

**xlings 生态定位为三分层**:
- **kernel**(宿主提供)—— 只提供 syscall 边界
- **xlings**(用户态发行版底座)—— subos 隔离 + xim-pkgindex 包管理 + 版本/binding 解析
- **mcpp**(生态主构建工具)—— C/C++ 是核心域,**不限于** C/C++;其他语言按需包裹/集成

**分发采用预构建 + 源码构建混合架构**:命中预构建即取,不命中走 mcpp 源构;主流软件通过 xlings-res 预热覆盖长尾。

**多 glibc / 可组合发行版通过 platform manifest 承载**,不做"任意组合"(Nix 模式对 xlings 团队规模不现实),而是"用户从官方 platform 里选一份"(Flatpak runtime / Homebrew bottle 模式)。

**消费者依赖从字面量硬钉演进为三轴模型**:
- `deps` 范围(装机期选择器)
- `runtime_floor`(构件期物理烙定,不可变)
- `platform_membership`(coupled-ABI 群的集合成员,短路 range 解析)

**与 hermetic 策略正交且互补**:hermetic 谈"运行时边界",本文谈"生态角色 + 分发架构"。合并后形成 xlings 生态的完整技术定位。

---

## 1. 起点与范围

会话由前一份 hermetic 策略延伸而来。hermetic 策略解决"什么 `.so` 能穿越到宿主",但一个更基础的问题从未明文写过:**xlings 究竟是什么?** 生态里的很多具体决策(compat.* 归属、xim-x-* 命名、pkgindex 与 mcpp-index 的分工、xlings-res 的作用、subos 与 mcpp sandbox 的关系)因为缺一份"我们是什么"的定位文档而反复重新推导。

本文的目标:
- **梳理**上述讨论,把散在多个 issue/PR/记忆里的决策浮到明面
- **命名**已经在做的模式(用户态发行版、三分层、混合分发、platform 化),让后续 review 有共同锚
- **枚举**已识别的阻塞项与开放问题,不给结论,留给后续逐条深入

本文**不是**策略、不是实施计划、不承诺时间表。任何进入代码的动作都需要独立的设计文档评审。

---

## 2. 三分层定位:kernel + xlings + mcpp

### 2.1 分工与职责

```
┌─────────────────────────────────────────────────────────┐
│  kernel (host)                                           │
│    syscall,  /dev/*,  /proc/*,  vendor kmod(如 nvidia.ko)│
│                    ↑ 通过 capabilities_host 白名单穿越   │
├─────────────────────────────────────────────────────────┤
│  xlings                                                  │
│    subos 隔离层 (bwrap sandbox, 独立 sysroot)            │
│    xim-pkgindex 包管理 (recipe, xvm, elfpatch)           │
│    版本/binding 解析 (xvm.add, xvm 节点绑定)             │
│    platform manifest (新: 承载"这个 subos 是哪个发行版") │
├─────────────────────────────────────────────────────────┤
│  mcpp                                                    │
│    生态主构建工具                                        │
│    C/C++ 核心域: compile, link, test, canonical build    │
│    构建 fingerprint、缓存、增量                           │
│    其他语言按需包裹/集成 (见 §2.3 scope 修正)             │
├─────────────────────────────────────────────────────────┤
│  用户项目 / 应用                                          │
└─────────────────────────────────────────────────────────┘
```

**语义收敛**:每一层只对上一层负责,每一层解决一类问题。当前生态里的角色混淆(比如 recipe 里塞 build 逻辑、mcpp 反过来管 subos 状态)是**层次未分开**的症状。

### 2.2 参照系(定位判据的来源)

| 生态 | Kernel | 用户态底座 | 包管理器 | 构建工具 | 分发形态 |
|---|---|---|---|---|---|
| Debian/Fedora | Linux | glibc + rootfs | apt/dnf | (每包各带) | 预构建为主 |
| NixOS | Linux(自建) | glibc(自建) | nix | nix build | binary cache 命中即取,不中现构 |
| Homebrew | XNU(宿主) | libSystem(宿主) | brew | (每 formula) | bottle(预构)+ 源构混合 |
| Gentoo | Linux(宿主) | glibc(自建) | portage | ebuild | 源构为主,binhost 兜底 |
| **xlings 提议** | Linux(宿主) | xlings subos + xim-pkgindex | xlings CLI | **mcpp** | **预构命中即取,不中走 mcpp 源构** |

**最贴近的类比**:Homebrew 精神 + Nix 分层。
- 从 Homebrew 继承:不接管 kernel、不自建 rootfs、host 只提供 syscall 边界(呼应 hermetic 策略 non-goals)
- 从 Nix 继承:用一个构建工具(nix build ↔ mcpp)承担源→二进制的正典转换,预构建是同一 recipe 的产物缓存,不是另一套并行的东西
- **业界没有完美先例**——Homebrew 的 bottle/source 有微差(build flag 漂移),Nix 严格但要求自建整个 rootfs。用户态发行版 + host kernel + 单一构建正典的组合是空生态位

### 2.3 mcpp scope 定位(用户 2026-08-05 更正)

**mcpp 是生态的主要构建工具,C/C++ 是核心域,不限于 C/C++。**

原会话草案曾表述为"mcpp 是 C/C++ 构建规范,非 C/C++ 走各自 upstream build system"。这个表述过窄,已按用户更正修改。修正后的定位:

- mcpp 首先服务 C/C++——现有能力、canonical build、fingerprint 机制、hermetic build 环境都以 C/C++ 为一等公民
- mcpp **可以且应该**承担多语言场景下的**协调/包裹**角色——具体形态(是原生集成、是插件、还是 wrapper)是**开放问题 A**
- **不承诺 mcpp 亲自实现 cargo/npm/pip 的求解器/构建管线**;这类生态的求解和构建有其成熟工具,mcpp 侧更可能是"调用它们并统一 fingerprint/缓存/hermetic 边界"

**为什么这个边界重要**:如果不明确 scope,mcpp 会被要求覆盖所有语言生态,scope 爆炸后没人能维护;如果把 mcpp 限死为 C/C++,又切断了"xlings 生态用一套工具"的定位价值。**折中是"C/C++ 一等公民 + 多语言协调层"**,协调层的具体形态另议。

### 2.4 subos 与 mcpp sandbox 的关系

- **subos**:运行 OS 实例(bwrap-based,独立 sysroot / xvm 状态 / 包安装目录)
- **mcpp sandbox**:构建时环境(bwrap-based,提供 hermetic 构建)

现状:两者的 sandbox 实现应已复用。**长期方向**:mcpp build 应能直接消费"当前 subos"作为构建环境——subos 是 mcpp 的输入(工具链/依赖来源),构建产物落回 subos。这样 mcpp canonical build 天然遵循 subos 的 platform 约束。

**开放问题**:mcpp build 究竟应"消费 subos"(在 subos 里跑)还是"消费 platform 描述"(在临时环境里按 platform 组装工具链)?两者对 fingerprint 稳定性影响不同。留作开放问题 J 的子问题。

---

## 3. 可组合发行版:subos + platform manifest

### 3.1 subos 是容器,platform manifest 是"是哪一款"

subos 已经是"一个用户态 OS 实例"的物理位置。但**subos 现在没有整体身份**——每个包各自声明 `xim:glibc@2.39`,合起来隐式产生了一个"发行版",但没人拥有它、没人能整体替换它。

**可组合发行版 = subos + platform manifest**:

```
distro := subos + platform_manifest
platform_manifest := {
    id: "xlings-2026.08-el9",
    kernel_abi_floor: "3.10.0",
    glibc: "2.39",
    gcc: "16.1.0",
    libstdcxx: "gcc@16.1.0",
    binutils: "2.42",
    linux_headers: "6.6",
    ...
}
```

一个 subos 生成时选一份 platform,之后**subos 内所有包的依赖解析都以 platform 为锚**。用户"切换发行版" = "换 subos",不是"改配置"。

### 3.2 platform manifest 的形态(开放问题 B)

至少三种承载形式,尚未定:

**形式 1 —— 显式声明包**(platform 本身是 xim-pkgindex 的一个 xpkg):
```lua
-- pkgs/x/xlings-platform-2026.08.lua
package = {
    name = "xlings-platform-2026.08",
    type = "platform",
    members = {
        { "xim:glibc",         "2.39" },
        { "xim:gcc",           "16.1.0" },
        { "xim:binutils",      "2.42" },
        { "xim:linux-headers", "6.6"   },
        ...
    },
    kernel_abi_floor = "3.10.0",
}
```

**形式 2 —— subos 元数据**(subos 创建时选一份,记在 subos 目录里):
```json
// $XLINGS_HOME/subos/<name>/platform.json
{ "id": "xlings-2026.08-el9", ... }
```

**形式 3 —— `.xlings.json` 用户侧**(用户项目声明依赖的 platform):
```json
{ "platform": "xlings-2026.08-el9" }
```

三者可以共存(索引侧发布定义 + subos 侧固化实例 + 项目侧声明需求),但**谁是权威源**、**谁与谁一致性校验**需要单独设计。留作开放问题 B。

### 3.3 为什么不是"任意组合"(Nix 模式)

Nix 允许用户任意组合"glibc 2.31 + gcc 13.3 + llvm 20 + boost 1.85",代价是 nixpkgs 社区的巨大规模(数千 committer,构建集群,binary cache)。**xlings 团队规模是两个数量级之外**。

务实定位:**"用户从官方 platform 里选一份"**,不是"用户自由组合"。
- 官方发布节奏:比如季度一份 platform(2026.05 / 2026.08 / 2026.11 / ...)
- 每份 platform 是**原子集合**:内部互测通过,一起 release
- 用户"自定义发行版" = "从官方 platform 库里选" + "在 platform 之上叠加应用层包"
- 极端情况下用户可以 fork platform recipe 自建,但那是社区扩展,不是核心承诺

**对齐 Flatpak runtime `//24.08` 模型**,也匹配 hermetic 策略 §5.3 P2 #8 的"Release 套装"表述。

---

## 4. 多 glibc:能力 vs 现实

### 4.1 xvm 数据模型:已经够

事实核对(xim-pkgindex):
- `glibc.lua` 有 `xvm_enable = true` 且每个 .so 通过 `xvm.add(lib, {version=glibc_version, ...})` 版本绑定注册
- gcc 已有 5 个共存版本(9.4.0 / 11.5.0 / 13.3.0 / 15.1.0 / 16.1.0),llvm 有 2 个(20.1.7 / 22.1.8)
- **glibc 现在只有 2.39 是"没做",不是"做不到"**
- 每个 subos 有独立 `subos_sysrootdir()`,glibc payload 走 xvm 独立目录——存储上完全隔离

### 4.2 阻塞项(优先级排序)

即使 xvm 模型够,**加第二份 glibc 到索引里,今天只会产生孤儿包**,因为:

1. **消费者硬钉字面量**(最硬的阻塞)
   - `pkgs/g/gcc.lua:35`: `"xim:glibc@2.39", "xim:binutils@2.42"`
   - `pkgs/l/llvm.lua:27`: `"xim:glibc@2.39"`
   - **所有 5 个 gcc 版本、2 个 llvm 版本全部锁 2.39**——加 glibc 2.28 索引里没消费者用

2. **elfpatch loader 单值**
   - `glibc.exports.runtime.loader = "lib64/ld-linux-x86-64.so.2"` 无版本轴
   - 预设"当前 subos 只有一个 glibc"——PT_INTERP 该写哪份必须由 subos platform 决定,不是 recipe 决定

3. **sysroot include 平面**
   - `glibc.lua:__config_header` 塞 130 项进 `subos_sysrootdir()/usr/include`,`declare_headers` first-claimant-wins
   - 两份 glibc 会在同一平面 include 目录打架
   - 需要 `usr/include/glibc-2.28/` 版本分片 + gcc `--sysroot`/`-isysroot` 联动

4. **binding 是 pin 不是 range**
   - 无法表达"我兼容 glibc >= 2.28"
   - manylinux 那种"编译对老、运行对新"的复用能力现在无法表达

5. **libstdc++/libc++ 与 glibc 隐式耦合**
   - gcc-15 的 libstdc++ 依赖新 glibc 符号
   - 换 glibc 就得换 gcc → 佐证"platform 是原子单位"的判断

6. **kernel syscall 版本地板**
   - 最新 glibc 会隐式要求某个 kernel(2.39 要 3.2+,2.42 要 3.10+)
   - platform 应显式声明 `kernel_abi_floor`,首装时探 `uname -r` 拒绝不合规宿主

**前三条不修,加第二份 glibc 到索引里就是纯技术债。**

### 4.3 platform 化 = 多 glibc 的最小可行形式

多 glibc 应该表现为**多 platform**,而不是"任意 glibc 组合"。用户不定义"glibc 2.31 + gcc 13.3.0 + llvm 20"这种随意组合(nixpkgs 规模),而是从 xlings 官方发布的一组 platform 里选。

**渐进路径**:
1. 定义 `xlings-platform-2026.08 = {glibc=2.39, gcc=16.1.0, ...}`,让当前 recipe 全都隐式属这一个 platform。**零功能变化,只是给现状命名**
2. 补 recipe DSL 里的 `${platform.*}` 变量,仅新写 recipe 用
3. 做第二份 platform 当作压力测试(如 `xlings-platform-legacy-el7 = {glibc=2.28, gcc=11.5.0, ...}`)——此时同时暴露阻塞项 1/2/3/6
4. compat.mesa 等新 payload 一律标 `platform: 2026.08`,不试图跨 platform 复用

---

## 5. 分发架构:预构建 + 源码构建混合

### 5.1 三 tier 分层

用户提议的 "预构建不中就源构" 有一个例外:**底座层没有源构选项**(bootstrap 循环:构 glibc 需要 gcc,构 gcc 需要 glibc)。

```
Tier 0 — Bootstrap 底座 (永远预构建,不允许源构)
  glibc, binutils, gcc(至少一份能自举的), linux-headers, libstdc++
  → 就是 §3 的 xim-platform-YYYY.MM
  → 份数由 xlings 官方发布节奏决定 (季度一份 × 3 arch × 3 年 ≈ 36 份底座)

Tier 1 — 工具链 / 主流库 (优先预构建,可源构兜底)
  llvm, cmake, ninja, boost, freetype, libpng, mesa, ...
  → 预构命中率应 > 95% (受众都是主流版本)
  → 不中时 mcpp 本地构

Tier 2 — 长尾 / 用户项目 (源构常态,预构增值)
  用户 mcpp 项目、compat.* 库、私域包
  → 预构建是可选优化
  → 组织可架私有 substituter 加速团队
```

**关键性质**:Tier 0 的存在恰好是"xlings 是发行版"最硬的一条证据——发行版的定义就是"我发布一组同步过的底座包"。

### 5.2 Canonical build 与 fingerprint

用户提议的"预构不中就源构"要 sound,必须解决"等价性"问题。

**Canonical build**:一个包在 mcpp 侧只有一套 build 描述(compile 命令 + flags + 依赖版本),预构建 tarball 与本机源构走**同一份 recipe**。这样:
- 预构 tarball = canonical build 的 CI 产物
- 源构产物 = 用户机上跑同一 canonical build
- 两者行为等价(不承诺字节等价——那是未来目标)

**xim-pkgindex 现状** vs **迁移目标**:
- 现状:100+ 个 recipe 的 `install()` 里塞满 build 逻辑(makefile 参数、cmake flags、post-build patch)
- 目标:recipe 只做 metadata,build 逻辑交给 mcpp canonical build 描述
- **这是多年重构**。务实做法:**新包 mcpp-first,老包按需迁移**(参照 xpackage-spec V2 是 V1 严格超集的模式)

**Fingerprint**:mcpp 已有 fingerprint 概念(memory 里 `mcpp fingerprint stale binary` 提到 `target/<fp>/bin/xlings` 路径结构)。扩展方向:

```
fingerprint = hash(
    source_tarball_sha256,
    toolchain_version + toolchain_fp,     # 当前 subos 的 gcc/glibc 版本
    build_flags,
    target_platform,                       # §3 的 platform id
    canonical_build_recipe_version
)
```

查询 xlings-res:`<pkg>/<version>/<fp>.tar.gz` 存在 → 下载;不存在 → mcpp 本地构 → 落 `target/<fp>/`。

**Fingerprint 定义的字段列表与稳定性**是开放问题 J。

### 5.3 "命中 = 相等"的定义

两级选项:
- **字节级重现**(reproducible builds):同源 + 同工具链 + 同 flags → bit-identical。Nix/Bazel 承诺。工程代价高,签名可从 hash 派生
- **行为等价**(functional equivalence):跑起来一样,字节不必一样。apt/brew 现状。代价低,但预构与源构结果不同时 bug 只在其中一条路径复现,定位困难

**对 xlings 现状**:行为等价起步,字节重现作为未来目标。

### 5.4 迁移代价与现实约束

1. **本地源构对宿主要求上升**:用户机装 xlings 几百 MB,但源构 mcpp 项目要 gcc + headers + 磁盘 + CPU。教育场景下源构兜底可能是"永远打不开的开关"——**xlings-res 预构建覆盖率必须 >> 95%**。这不推翻架构,但决定了 **xlings-res 预构建 CI 是生态的关键基础设施**。开放问题 F
2. **Fingerprint 传染性**:一个包的 fingerprint 依赖工具链 fingerprint,工具链依赖 libc fingerprint。**libc 换一次,fingerprint 全部失效,tier 1 所有包需重构**。对策:libc/toolchain 变更走 platform 原子发布,平时不动
3. **信任模型统一**:预构建来自 xlings-res(CA/signing 假设),源构建来自本机(信任 upstream sha256)。两者语义等价意味着"不介意从哪拿"——已经是 Nix binary cache 信任模型

---

## 6. 版本语义:三轴模型

### 6.1 现在的硬钉字符串同时承担四种语义

`"xim:glibc@2.39"` 现在暗含四件事,没显式拆开:

| 语义 | 含义 | 生命周期 |
|---|---|---|
| **构建期符号地板** | "gcc 这份预构件是拿 glibc 2.39 的 headers/loader 编的" | 不可变,tarball 出厂就烙死 |
| **装机期选择器** | "装 gcc 时,给我把 glibc 2.39 也装上/找到" | 可由 policy 改 |
| **兼容承诺** | "我保证这份 gcc 在 glibc 2.39 上能跑" | 人工断言,断言错了用户炸 |
| **平台成员** | "我是 xlings-platform-2026.08 的一员" | 集合成员,不是比较 |

**Semver 范围解决的是第 2 和第 3 类**。但 glibc 的向前兼容是**单向的**:
- 对 2.28 编译的二进制 → 在 2.39 上跑得动
- 对 2.39 编译的二进制 → 在 2.28 上跑不动(缺 GLIBC_2.30+)

写 `"xim:glibc@>=2.28"` 只有在**这份 tarball 真的是拿 2.28 编的**时才是真话。tarball 拿 2.39 编,再声明 `>=2.28`——resolver 满心欢喜给 2.28 环境,运行时炸——**范围声明与物理事实脱节比硬钉更危险**,因为默认"绿"实际"红"。

**对 glibc 这类的正确模型是最小版本地板,不是范围**。

### 6.2 拆开:三轴模型

```lua
package = {
    name = "gcc",
    version = "16.1.0",

    -- 装机期(resolver 看这个,可以是范围/平台变量)
    deps = {
        { "xim:glibc",    version = ">=2.39",  reason = "gcc 16 build-time floor" },
        { "xim:binutils", version = "^2.42",   reason = "ld 兼容" },
    },

    -- 构件期(不可变,elfpatch/loader-选择/CI hermetic 都读这里)
    runtime_floor = {
        loader     = "glibc@2.39",   -- PT_INTERP 烙定
        libc       = "glibc@2.39",   -- 编译时 headers
        libstdcxx  = "gcc@16.1.0",   -- 自带 libstdc++.so.6
    },

    -- 平台成员(若属某个原子发布集合)
    platform_membership = "xlings-2026.08",
}
```

**关键性质**:
- `deps` 里的范围**只在 mcpp 源构或纯 header-only 包时是自由的**。对预构 tarball,`deps` 的范围下界必须 == `runtime_floor` 对应字段——**CI 强校验**
- `runtime_floor` 是 tarball 元数据,发布时确定,**从此再不能改**——就像 PT_INTERP 一样物理烙定
- `platform_membership` 存在时,resolver **短路**——直接锁 platform 定义的版本,不走 `deps` 范围解析。这条路径吃掉 80% 的现实场景

### 6.3 按包类型分级采用

| 包类型 | 举例 | 版本语义 | 理由 |
|---|---|---|---|
| **coupled-ABI 底座** | glibc, gcc, libstdc++, libc++ | 只用 platform 成员 | 三者符号 ABI 相互烙定,任意范围都是幻觉 |
| **系统运行时** | libpng, freetype, openssl | 最小版本地板 (`>=X.Y`) | 通常向前兼容,反向不成立;上界几乎不需要 |
| **纯 header-only C/C++ 库** | fmt, spdlog, catch2 | 完整语义范围 (`^X.Y.Z`) | 无 ABI,只在 build-time 解析 |
| **应用层预构件** | gh, ollama, godot | 单版本 pin (现状) | 上游各自发一个,没有多版本选择必要 |
| **mcpp 源构包** (未来) | 用户项目、compat.mesa 源版 | 完整语义范围 | 源构环境已知,范围能真被 solver 利用 |

**推论**:"引入 semver 范围"不是全局决策,是按类型渐进的政策。

### 6.4 Silent-success 陷阱

xlings 生态踩过多次"从不发生和已经成功产生一样的日志"(memory: silent-success pattern)。版本范围的天然坑就是这一族:
- 声明 `>=2.28`,预构建实际拿 2.39 编——CI 里全都是 2.39,永远绿
- 用户在 2.28 环境装,报 `GLIBC_2.30 not found`——错误信息不指向 recipe 的 range 声明,而是指向 loader

**防坑必须的两件事**:
1. `runtime_floor` 与 `deps` 下界一致性,**CI 强校验**——范围声明与物理事实必须联动
2. 范围声明的**每个"点"都必须 CI 覆盖**——`>=2.28, <3.0` 意味着 CI 至少跑过 2.28/2.35/2.39 三个采样点的 install + smoke test

只做第一件,范围诚实但受限;只做第二件,CI 矩阵爆炸。**两件必须同时做**,而**平台成员机制**恰好把 CI 矩阵砍下来:成员天然把"跑几个采样点"变成"跑几个 platform",数量可控。CI 采样点矩阵设计是开放问题 H。

### 6.5 迁移(V2 spec probe pattern 复用)

xpackage-spec V2 §"Adopting a capability" 已写清:新字段/新语法必须通过 probe 让老 client 走 legacy 分支,不能靠 `min_xlings` 挡门。三轴模型同样适用:

```lua
if pkgindex.version_constraints then
    deps = { { "xim:glibc", version = ">=2.39" } }
else
    deps = { "xim:glibc@2.39" }   -- 老 client 退化到硬钉最低点
end
```

代价:每次 range 变更都要维护 legacy 侧硬钉,直到 dropping 老 client 那天。

---

## 7. 与 hermetic 策略的关系

正交、互补:

|  | hermetic 策略 | 三分层 + 分发架构 |
|---|---|---|
| **谈的是** | 运行时**边界**(什么 `.so` 能穿越到宿主) | 生态**角色** + 分发**架构** |
| **主要产物** | `capabilities_host` 元数据 + bwrap 空 host CI | mcpp canonical build + fingerprint 分发协议 + platform manifest |
| **单独价值** | 收敛"跨越宿主"的口径 | 收敛"我们是什么 + 如何来到用户机器上"的口径 |
| **联动价值** | mcpp 本地构出的二进制**天然遵循 hermetic**(build 环境本身 hermetic);预构建 tarball CI 用 bwrap 空 host 验证 hermetic 合规 |

**建议**:把三分层定位写成上位文档,hermetic 策略作为其下位的**运行时边界章节**;分发架构作为下位的**分发章节**。合并后生态里所有决策(新增包、新增依赖、新增 CI check)都能找到锚。

---

## 8. 阻塞项汇总(优先级排序)

从"要做多 glibc / 可组合发行版"角度倒推,阻塞项排序:

| # | 阻塞项 | 影响 | 相关章节 |
|---|---|---|---|
| 1 | 消费者硬钉字面量 | 加第二份 glibc 无人使用 | §4.2 |
| 2 | 无 platform manifest 概念 | subos 无整体身份,无从"选一份" | §3 |
| 3 | elfpatch loader 单值 | PT_INTERP 无法按 subos platform 选择 | §4.2 |
| 4 | sysroot include 平面 | 多 glibc headers 打架 | §4.2 |
| 5 | recipe 里塞 build 逻辑 | 无法做 canonical build → 预构/源构等价性无从谈起 | §5.2 |
| 6 | 无 fingerprint 分发协议 | "命中即取,不中现构"无判定 | §5.2 |
| 7 | binding 是 pin 不是 range | 无法表达跨版本兼容 | §6 |
| 8 | 无 CI 一致性校验(range vs runtime_floor) | Silent-success 陷阱 | §6.4 |
| 9 | libstdc++ 与 glibc 隐式耦合无声明 | 换任一即换另一,但没人显式表达这个约束 | §4.2 §6.3 |
| 10 | kernel syscall 地板无声明 | 最新 glibc 隐式要求某 kernel,无人校验 | §4.2 |

---

## 9. 最小可行下一步(仅建议顺序,不承诺时间)

1. **写定位文档**(本文的继任者):把三分层 + hermetic 合成一份 `docs/design/ecosystem-positioning.md`。**零代码,当天可做**
2. **命名现状为 platform-2026.08**:定义 `xlings-platform-2026.08 = {现有 gcc/glibc/binutils/...}`,让当前 recipe 隐式属这份 platform。**零功能变化,零迁移**
3. **加 `runtime_floor` 字段**(spec V3):走 V2 spec 的 probe pattern 引入,recipe 侧填充,尚不强制。**逐包填,老 client 兼容**
4. **加 `deps` range 语法**(spec V3):同上,同样 probe 引入。CI 强校验 `runtime_floor` 与 `deps` 下界一致性
5. **mcpp canonical build 探索**:选 1~2 个包做 pilot(不要选 glibc/gcc 这种硬骨头,选 libpng/fmt 这类简单库),验证 fingerprint 稳定性 + 预构/源构等价性
6. **第二份 platform 作为压力测试**:当上面 5 步走通,做一份 `xlings-platform-legacy-el7`,一次性暴露多 glibc 剩余阻塞项(elfpatch / sysroot / libstdc++ 耦合)
7. **compat.mesa 等 hermetic 策略新 payload**:严格标 `platform_membership`,不跨 platform 复用

---

## 10. 开放问题(留待后续逐条讨论)

- **A. mcpp scope 到底延伸到多远?** C/C++ 一等公民 + 多语言协调层——协调层是原生集成、插件、还是 wrapper?对 cargo/npm/pip 的态度分别是什么?
- **B. platform manifest 的 CLI/配置形态**:形式 1(xpkg)/形式 2(subos 元数据)/形式 3(`.xlings.json`)如何选?权威源是谁?一致性校验谁做?
- **C. compat.* vs xim-x-* 命名归属**:上一份分析建议归 xim-pkgindex,但保留 `compat.` 前缀会打破现有命名分层
- **D. macOS / Windows 上 hermetic 与 platform 的对应**:macOS 有 system frameworks 与 libSystem,Windows 有 UCRT。platform 概念如何跨平台一致?
- **E. xlings-res 存储 / 带宽预算**:多 platform × 多 arch × tier 1 全预构 → 存储和 CDN 成本量级?
- **F. 源构建 UX 在教育场景的可行性**:mcpp 本地构对宿主 CPU/磁盘/网络的最低要求,教育场景能否兜底?
- **G. 老 recipe 迁移政策**:canonical build 迁移是强制(deadline)还是渐进(新包新政策)?
- **H. Range 的 CI 采样点矩阵设计**:每个 `>=X.Y` 声明至少覆盖哪几个点?矩阵如何砍?
- **I. Bootstrap 层如何独立更新**:比如 glibc 出安全补丁 2.39.1,能不能不动 platform 集合的其它成员单独发?
- **J. Fingerprint 定义的字段列表与稳定性**:哪些字段进 fingerprint?工具链版本进入的话,platform 微调 fingerprint 全炸,如何权衡?
- **K. mcpp 消费 subos vs 消费 platform 描述**:mcpp build 究竟在 subos 内跑,还是在临时环境按 platform 组装工具链?
- **L. 私有 substituter 支持**:组织内的私域预构建缓存,如何在信任模型 + fingerprint 协议里表达?
- **M. platform 弃用政策**:一份 platform release 后多久 EOL?EOL 后旧 subos 是不可迁移的?

---

## 11. 决策未定与假设标注

本文所有具体形态(字段名、目录布局、CLI 语法)都是**示例**,不是决策。真正的决策在后续 §10 各开放问题单独讨论时才作出。

**已经作出的假设(可讨论)**:
- xlings 定位为"用户态发行版"(呼应 hermetic 策略 TL;DR)——如果这个定位本身要改,本文整体重来
- mcpp 是生态主要构建工具(用户 2026-08-05 更正确认)
- kernel 由宿主提供,不接管(呼应 hermetic 策略 non-goals)
- Bootstrap 底座必须预构建(bootstrap 循环的技术必然)
- coupled-ABI 群走 platform 而非 range(basd on glibc symver 单向兼容的物理事实)

**故意不作的假设**(留待后续):
- mcpp 对非 C/C++ 语言的具体覆盖形态
- platform manifest 承载的确切数据结构
- fingerprint 具体字段
- range 语法(采纳 semver 还是自定义)
- 迁移 deadline

---

## 12. 一句话总括

> **xlings 是"用户态 Linux 发行版",三分层 = kernel(host) + xlings(底座) + mcpp(主构建工具)。分发采用预构 + 源构混合(canonical build 保等价性,fingerprint 判命中)。多 glibc / 可组合发行版通过 platform manifest 承载,不做自由组合。消费者依赖从硬钉演进为 deps range + runtime_floor + platform_membership 三轴模型。与 hermetic 策略正交互补,合并后形成完整生态定位。**

---

## 13. 落地进展(2026-08-05 更新)

本备忘录的 13 个开放问题里,只有一个已被实施推翻或确认:

**Configuration 基质已落地**(slice 1,xlings **2026.8.5.1** + libxpkg **0.0.48**)。
`subos_info.envs` 让包声明其 subos 必须导出的环境变量,补完了三层基质里唯一缺失的
一层。这是"用户态 OS"从概念变成可检查对象的第一步:一个 subos 现在能**描述自己**
(runtime + envs),而不只是"装了什么"的清单。

详见 `2026-08-05-subos-minimum-design.md` §13 与
`2026-08-05-subos-slice1-landing-plan.md` §7。

**由此确认的一件事**:`runtime` 字段的自描述形态(`glibc@2.39` 即 Linux/glibc,
家族由包名前缀派生而非独立存储)在实现中站得住,且天然为多 runtime 预留了位置 ——
本备忘录 §"多 glibc" 讨论的数据模型层面已经就位,剩下的仍是索引一致性与消费者绑定
两层(硬钉依赖尚未改造)。

**未被触及的**:platform manifest、canonical build、fingerprint、deps range 三轴
模型 —— 全部仍是开放问题,slice 1 明确不碰。
