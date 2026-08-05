# 图形栈详细设计:mesa(开源栈)与 NVIDIA 闭源栈

**日期**: 2026-08-05
**类型**: 详细设计(detailed design)
**上游**: `2026-08-05-subos-minimum-design.md`(slice 1,已发布 2026.8.5.1)
**范围**: 设计文档 §11 的 Task #8/#9/#10,外加原 §9.4 划给 "slice 2" 的 NVIDIA 闭源栈
**状态**: 设计待评审;所有数字均在 2026-08-05 于一台 RTX 4080 / Ubuntu 24.04 / mesa 25.2.8 实测

---

## 0. TL;DR

Slice 1 补完了 Configuration 基质:包能声明环境变量,变量能到达用户自己的进程。
**机制侧不再是阻塞项**。剩下的是把 payload 做出来,而这分两条**约束完全不同**的轨:

| | Track A:mesa | Track B:NVIDIA 闭源 |
|---|---|---|
| 能否分发 | ✅ MIT,可构建可发布 | ❌ 许可禁止重分发 |
| 版本耦合 | 与 subos 的 glibc 绑定 | 与**宿主内核模块**逐位绑定 |
| 交付形态 | 一个 xpkg,~241 MB | 一个**探测+桥接** xpkg,~0 MB payload |
| 失效模式 | 装错了跑不起来 | 宿主升级驱动后**悄悄断链** |

**架构关键**:libglvnd。它是厂商无关的 dispatch 层,两条轨在**同一个 subos 里共存**,
按进程通过环境变量选厂商。这不是"slice 1 做 mesa、slice 2 做 NVIDIA"两个割裂的世界。

**推翻了 slice 1 设计里的一个数字**:§10 的 O4 担心 payload "可能超 800MB,是否分包"。
实测 **241 MB**,其中 `libLLVM.so` 一家占 137 MB。

**分包结论**(§2.8):那 137 MB **建议**独立成 **`llvm-runtime`**(xim-pkgindex,
默认 `xim` 命名空间),照抄索引里**已经存在**的 `gcc-runtime` —— 后者正是为
"只想运行 C++ 程序、不需要 1.1 GB 编译器"而拆出来的。`llvm`(编译器)与
`libLLVM.so`(库)同源但消费者不同,这是两条轴。

---

## 1. 实测事实(设计的地基)

在 RTX 4080 / Ubuntu 24.04 / mesa 25.2.8 / NVIDIA 550.144.03 上测得。
凡与直觉冲突的,以下面的数字为准。

### 1.1 mesa 25.x 已经不是"每个驱动一个 .so"

`/usr/lib/x86_64-linux-gnu/dri/` 下 40+ 个 `*_dri.so` **全部是符号链接**,指向同一个
`libdril_dri.so`(117 KB 的 shim),真正的实现在单一 megadriver:

```
  41.4 MB  libgallium-25.2.8.so     ← 所有 GL 驱动(llvmpipe/iris/radeonsi/nouveau/zink)
   0.1 MB  dri/libdril_dri.so        ← shim
   0.3 MB  libGLX_mesa.so.0
   0.3 MB  libEGL_mesa.so.0
```

> slice 1 设计文档 §"25 个 .so 手写,新增/删除易漏"的前提**已经过时**。现在是一个
> megadriver + 一批符号链接,`xvm.files` 批量声明的形态反而更简单。

### 1.2 Vulkan 是独立的一套,每驱动一个 ICD

```
  11.4 MB  libvulkan_lvp.so       (llvmpipe,CPU)
  20.2 MB  libvulkan_intel.so     (ANV)
  14.5 MB  libvulkan_radeon.so    (RADV)
  15.2 MB  libvulkan_nouveau.so   (NVK)
```

发现走 JSON:`/usr/share/vulkan/icd.d/*.json` → `VK_DRIVER_FILES`。
与 GL 的 `LIBGL_DRIVERS_PATH` 是**两套独立协议**,必须分别声明。

### 1.3 LLVM 是最大的单项,且现有 llvm 包按设计就不含它

```
 136.8 MB  libLLVM.so.20.1        ← libgallium 的 DT_NEEDED(不是 dlopen)
```

**这一节的完整梳理见 §2「gcc / llvm / libLLVM / mesa 四者关系」** —— 结论是
xlings 索引里的 `llvm` 包**按明文规范就不包含 `libLLVM.so`**,不是遗漏。
mesa 需要的是一个索引里目前不存在的东西。

### 1.4 payload 总量

| 组成 | 大小 |
|---|---|
| mesa 本体(GL + Vulkan + glue) | 103.4 MB |
| libLLVM.so | 136.8 MB |
| 第三方(libdrm/xcb/expat/zstd/elf/…共 17 个) | ~1.2 MB |
| **合计** | **~241 MB** |

O4 关闭:不分包。

### 1.5 符号版本:hermetic 边界成立

```
libgallium 需要的最高:  GLIBC_2.38
libLLVM   需要的最高:  GLIBC_2.38  (+ GLIBCXX / CXXABI)
subos 提供:            glibc@2.39   ✅
```

**这正是 issue #352 的反面**。#352 里 Fedora 44 宿主的 `GLIBC_2.43` 漏进来把 GLFW
打挂;只要 payload 针对 subos 的 glibc 2.39 构建、且由 subos 提供 loader 与 libc,
宿主 glibc 是什么就不再相关。

### 1.6 NVIDIA 闭源栈的两条硬约束

```
用户态总量:      327 MB
每个文件版本戳:  libcuda.so.550.144.03 / libGLX_nvidia.so.550.144.03 / ...
内核模块:        NVRM 550.144.03
```

1. **许可禁止重分发**。
2. **用户态必须与内核模块逐位匹配**。NVRM 的 ioctl ABI 不跨版本稳定,
   550.144.03 的 `libcuda.so` 配 555 的内核模块会直接失败。

而内核模块属于宿主,xlings 不接管(hermetic 策略的 non-goal)。所以:
**Track B 不是"发布一个包",而是"从宿主借,并把借的东西记账"。**

### 1.7 内核侧透传已经有了

`src/core/subos/gpu.cppm`(75 行)在 bwrap 下已 `--dev-bind` 了
`/dev/nvidiactl`、`/dev/nvidia-uvm`、`/dev/nvidia*`、`/dev/dri`,外加 `/sys` 只读。
**设备节点这一层不缺东西,缺的是用户态库。**

### 1.8 构建依赖:索引里只有 5/18

| 已有 | 缺 |
|---|---|
| ninja, python, rust, expat, zlib, llvm(工具链) | meson, cargo, pkg-config, libdrm, zstd, elfutils, libxcb, libx11, wayland, wayland-protocols, bison, flex |

源码构建 mesa 要先补 ~12 个 xpkg。这是 Track A 真实成本的大头,不是 mesa 本身。

---

## 2. gcc / llvm / libLLVM / mesa:四者关系梳理

这一节回答"那 137 MB 的 `libLLVM.so` 一般在哪里、xlings 生态该怎么放"。
结论会改变 mesa 的分包形态,所以放在架构之前。

### 2.1 四个东西,不是三个

最容易搞混的是 **`llvm`(编译器)** 与 **`libLLVM.so`(库)**:它们同源,但**消费者完全不同**。

| | 是什么 | 谁消费 | xlings 现状 |
|---|---|---|---|
| **gcc** | C/C++ 编译器(~1.1 GB) | 开发者 | ✅ `gcc` 包 |
| **gcc-runtime** | libstdc++ / libgcc_s 等(~25 MB) | **任何 C++ 程序**,含 mesa | ✅ **已存在**,正是本文要照抄的先例 |
| **llvm** | clang/lld 工具链 | 开发者 | ✅ `llvm` 包(Linux 实体 102 MB) |
| **libLLVM.so** | LLVM 作为**共享库**(JIT / 代码生成) | 几乎只有 **mesa** | ❌ **索引里没有** |
| **mesa** | GL/Vulkan 实现 | 图形程序 | ❌ 待做 |

依赖方向:

```
    图形程序 (Godot / GLFW)
         │
       mesa ────────────┬──────────────┐
         │              │              │
   libgallium      libvulkan_*    (GL/EGL vendor)
         │
   libLLVM.so ← llvmpipe 的 JIT + radeonsi 的着色器编译
         │
   libstdc++ / libgcc_s ← 来自 gcc-runtime 包(~25 MB,不是 1.1 GB 的 gcc)
```

**`llvm` 包不在这条链上**。mesa 不需要 clang;它需要的是 LLVM 的代码生成能力,
以共享库形态。

### 2.2 这些包落在哪个索引:xim-pkgindex,不是 mcpp-index

先划清层,因为这条线是本文初稿命名出错的原因。

| 索引 | 装什么 | 命名 |
|---|---|---|
| **xim-pkgindex** | 装进 subos 的**运行时 payload** | 默认命名空间 `xim`,包名 kebab-case **不带点**(`gcc-runtime`、`jdk-temurin`、`mingw-gcc`);另有 8 个 `config` 包 |
| **mcpp-index** | mcpp **构建**时消费的 C/C++ 库 | `compat` 命名空间(`compat.freetype`、`compat.glfw`、`compat.glx-runtime`) |

图形栈的 payload 是**给图形程序在 subos 里跑的**,不是给 mcpp 链的 →
全部进 **xim-pkgindex**,用默认 `xim` 命名空间。

> mcpp-index 那边已经有 `compat.glx-runtime` —— 一个**无 payload 的宿主适配器**,
> 通过 `mcpp.runtime.capabilities = {"x11.display", "opengl.glx.driver"}` 从宿主借 GL。
> 那是**另一层**的东西:它解决"mcpp 应用怎么声明自己需要 GL",本文解决"subos 里
> 到底有没有 GL"。两者将来会接上(有了 `mesa`,mcpp 应用就能从 subos 拿 GL 而不是
> 宿主,那正是 #352 的根治),但不是同一个包、也不在同一个索引。

### 2.3 发行版怎么放:纯运行时包,消费者只有 mesa

Ubuntu 上实测:

```
$ dpkg -S libLLVM.so.20.1
libllvm20:amd64

$ dpkg -L libllvm20
/usr/lib/x86_64-linux-gnu/libLLVM.so.20.1      ← 就这一个实体
/usr/lib/x86_64-linux-gnu/libLLVM-20.so        ← 加一个符号链接

$ apt-cache rdepends --installed libllvm20
  libosmesa6
  mesa-vulkan-drivers
  mesa-libgallium
```

`libllvm20` 是一个**只有两个文件的纯运行时包**,而它的反向依赖**全是 mesa**。
发行版早就沿"运行时库 vs 工具链"这条轴切开了,原因就是消费者不同。

### 2.4 xim-pkgindex 的 llvm 划分:2 包模型,明文排除 libLLVM

索引仓自带规范:`.agents/skills/llvm-subpackaging/SKILL.md`。要点:

- **2 包,不是 3 包**。历史上曾拆 `llvm-core` + `llvm-libcxx` + `llvm-tools`
  (commit `9db2ba94`),随后 `99b004dc` **主动把 libc++ 并回 `llvm`**,并写明
  旧资产"不再使用、不要复活"。
- 当前模型:
  - **`llvm`** = slim 自包含工具链(clang + lld + binutils + libc++ + compiler-rt)
  - **`llvm-tools`** = clang-format / clang-tidy / clangd
- **上游没有现成分包**(单体 1.4–2 GB),两个包都是 xlings-res 下游 carve 的。
- **验收红线,原文**:

  > 目标二进制静态自包含。clang 驱动、`lld`、`clang-format`/`tidy`/`clangd` 都
  > **静态链接 LLVM,只依赖系统库;无共享 `libLLVM`**。
  >
  > Linux:`ldd` 只能是系统库(**不应出现 `libLLVM.so`**)。

- 明确丢弃:全部 `.a` 静态库、`libclang-cpp`、`liblldb*`、MLIR、docs。

实测印证(Linux `llvm` payload,102 MB):`find -name "libLLVM*"` **零结果**,
`clang-20` 无任何 `libLLVM` 的 `DT_NEEDED`。

> **更正**:本文档初稿写的是"llvm 包 627 MB 只有 bin/ 和 lib/clang/"。那个 627 MB
> 是本机上一份 **Windows PE32+ payload**(27 个 `.exe`,交叉工具链残留),不是 Linux 版。
> 结论(不能复用)是对的,但依据错了 —— 真正的依据是上面这条**明文规范**,
> 比一次偶然的目录观察强得多。

### 2.5 mesa 到底用 LLVM 的什么(实测,非推测)

对 `libgallium` 做未定义符号分析:

```
$ nm -D --undefined-only libgallium-25.2.8.so | grep -oE 'LLVMInitialize[A-Za-z]+' | sort -u
LLVMInitializeAMDGPUAsmParser     LLVMInitializeAMDGPUTarget
LLVMInitializeAMDGPUAsmPrinter    LLVMInitializeAMDGPUTargetInfo
LLVMInitializeAMDGPUDisassembler  LLVMInitializeAMDGPUTargetMC
LLVMInitializeX86...              (27 处 AMDGPU 相关符号)
```

需要且只需要两个 target:

| target | 服务于 | 对应用户要的硬件 |
|---|---|---|
| **X86** | llvmpipe(GL)/ lavapipe(Vulkan)的 JIT | **CPU** |
| **AMDGPU** | radeonsi 的着色器编译 | **AMD** |

Intel(iris/ANV)和 NVIDIA 开源(nouveau/NVK)**不经过 LLVM** —— 它们用 NIR 自带后端。
所以 `LLVM_TARGETS_TO_BUILD=X86;AMDGPU` 是实测结论,不是保守猜测;
砍掉其余十几个 target 是安全的。

### 2.6 mesa 与 gcc:依赖 `gcc-runtime`,不是 `gcc`

`libgallium` 需要的最高 C++ 符号版本是 **`GLIBCXX_3.4.29`**。

索引里**已经有** `gcc-runtime`,专门为这种场景存在:

```
gcc-runtime@15.1.0  →  lib64/libstdc++.so.6.0.34   (= GLIBCXX_3.4.34)
                       libgcc_s.so.1 / libgomp / libatomic / …
                       exports.runtime.libdirs = { "lib64" }   ← elfpatch 自动接上
```

3.4.34 ≥ 3.4.29,**覆盖有余**;而 libstdc++ 是**前向兼容**的,所以一个
`gcc-runtime` 版本能同时服务针对更老 GCC 构建的一切产物。

> **O5 关闭**:走 `deps = { "xim:gcc-runtime@<ver>" }`,mesa **不自带**。
>
> **不要依赖 `xim:gcc`** —— 那是 ~1.1 GB 的完整编译器,而 mesa 在**运行期**
> 只需要那 ~25 MB 的运行时库,`cc1`/`cc1plus`/头文件/`*.a` 一样都用不上。
> 这正是 `gcc-runtime` 被单独拆出来的理由。
>
> 仍需在 recipe 里钉下限:源码构建(方案 A)用哪个 GCC 编译,就产生哪个
> `GLIBCXX_*` 下限,`deps` 里的 `gcc-runtime` 版本必须不低于它。

### 2.7 ABI 耦合的实测:精确到 major,且 fail-closed

在决定分不分包之前,先量清楚 mesa 与 libLLVM 的耦合有多紧。

```
$ nm -D --undefined-only libgallium-25.2.8.so
  C API 符号 (LLVM*)        : 233
  C++ mangled 符号 (llvm::) :  97   ← 决定性
```

C++ 符号样例,注意尾部的符号版本标签:

```
llvm::DataLayout::~DataLayout()                        @LLVM_20.1
llvm::Instruction::setMetadata(unsigned, MDNode*)      @LLVM_20.1
llvm::LLVMContext::getOrInsertSyncScopeID(StringRef)   @LLVM_20.1
```

而 `libLLVM.so.20.1` **只导出一个符号版本节点**:`LLVM_20.1`。

两条结论:

1. **不是纯 C API,耦合精确到 LLVM major.minor**。用 LLVM 21 的 libLLVM 跑针对
   20.1 构建的 mesa,97 个 C++ 符号一个都解析不了。
2. **失配是加载期硬失败,不是静默损坏**。符号版本机制让它 fail-closed —— 这一点很重要,
   它决定了拆包是安全的。

Ubuntu 对此的表达方式:

```
mesa-libgallium Depends: … libc6 (>= 2.38), libstdc++6 (>= 11), libllvm20, …
                                ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^   ^^^^^^^^^
                                别的依赖都带版本区间              libllvm 没有区间
```

没有区间,是因为 **major 写在包名里**了。这就是发行版表达"精确 major、任意 patch"的办法。

### 2.8 结论:拆,但理由比初稿窄

**建议新增 `llvm-runtime`。** 但先撤回我在初稿里给的两条理由 —— 它们是错的:

| 初稿的理由 | 实际 |
|---|---|
| ~~"可独立升级"~~ | ❌ **假的**。`@LLVM_20.1` 符号版本意味着升 libllvm 必须同时重建 mesa |
| ~~"将来可复用"~~ | ⚠️ 空头支票。任何未来消费者必须用**同一个 LLVM major** 才谈得上复用 |

站得住的理由只有三条,但够了:

1. **mesa patch 升级不必重下 137 MB**。mesa 稳定分支约每月一个 patch,LLVM major 一年两次。
   拆开后 25.2.8 → 25.2.9 只动 104 MB,libllvm 原地复用。
   这对**用户**和**发布流程**都成立 —— 后者见第 3 条。
2. **与所有发行版的分法一致**。Ubuntu/Debian/Fedora/Arch 都把 libLLVM 单列,
   任何打过 mesa 包的人一眼就懂这个版本关系;埋进 mesa 反而要解释。
3. **单个资产更小**。`2026-07-15-gitcode-large-asset-mirror-runbook.md` 记录:
   **GitHub CI runner 无法向 GitCode 上传 >~8 MiB 的资产**(跨境 15–30 KB/s,必超时),
   >8 MiB 一律要人工从 CN 机器补传。241 MB 和 104+137 MB 都逃不掉这一步,
   但拆开后**每次 mesa patch 只需补传 104 MB**,而 137 MB 那份一年才动两次。

**代价**:mesa 要硬钉 `compat:libllvm@<ver>`。这不是新负担 ——
索引里所有 recipe 都是硬钉(`gcc.lua` 钉 `xim:glibc@2.39`),这就是现有范式。
而且 §2.7 已经证明钉错了是加载期硬失败,不会悄悄跑出错误结果。

**形态**:

```
llvm-runtime/<ver>/
└── lib/libLLVM.so.<major.minor>        ← 单一实体
                                           -DLLVM_TARGETS_TO_BUILD=X86;AMDGPU
                                           -DLLVM_LINK_LLVM_DYLIB=ON
```

**命名遵循索引里已有的先例,而不是 Ubuntu 的。** xim-pkgindex 里已经有
`gcc-runtime`,它存在的理由与这里逐字对应(原文注释):

> * `xim:gcc` is the full compiler (~1.1 GB)
> * The runtime libs are ~25 MB
> * Tools that only need to **run** C++ binaries don't need cc1/cc1plus/headers/`*.a`

`gcc`(编译器)/ `gcc-runtime`(运行时库)这对区分在这个索引里早就立住了,
`llvm` / `llvm-runtime` 完全平行,一眼可懂。

> **更正**:我先前写过"不要叫 `llvm-runtime`,会和 `llvm` 包混淆"。**说反了** ——
> 有 `gcc-runtime` 这个先例在,`llvm-runtime` 恰恰是最不会被误解的名字。
> 也不要用 `compat.` 前缀:那是 **mcpp-index** 的命名空间(`compat.freetype`、
> `compat.glfw`、`compat.glx-runtime`),而 xim-pkgindex 的包名**没有一个带点**,
> 命名空间只有默认的 `xim` 和 8 个 `config` 包。

它**不违反** llvm-subpackaging skill 的"2 包不是 3 包":那条规则约束的是
**工具链的拆分轴**(core / libcxx / tools 是同一批消费者的三块)。`llvm-runtime`
在**另一条轴**上 —— 消费者是 mesa 不是开发者,与 `llvm` 包零重叠。
`gcc-runtime` 就是这条轴上已经存在的那一个。

**与 `gcc-runtime` 的一处关键差异,不能照抄**:`gcc-runtime` 的注释说
"libstdc++ is forward-compatible; one gcc-runtime version covers every binary
built against an equal-or-older GCC"。**libLLVM 没有这个性质** —— §2.7 测到的
`@LLVM_20.1` 符号版本让它精确锁 major.minor。所以包的**形状**照抄 `gcc-runtime`,
**版本策略**不能照抄:消费者必须硬钉,不能像 libstdc++ 那样"装个新的就都覆盖了"。

> **如果不拆**:也不是错的选择,把 137 MB vendored 进 mesa 一样能跑,
> 少一个包要维护。分界线是"会不会跟 mesa 的 patch 版本走" —— 若 xlings 只固定一个
> mesa 版本、几乎不升级,拆包的三条理由全部失效,那就别拆。

---

## 3. 架构关键:libglvnd 让两条轨共存

宿主上 GL 的实际结构:

```
         应用 (glxinfo / GLFW / Godot)
                    │
         libGL.so.1 / libGLX.so.0 / libEGL.so.1     ← libglvnd,厂商无关 dispatch
                    │
        ┌───────────┴───────────┐
libGLX_mesa.so.0        libGLX_nvidia.so.550.144.03  ← 厂商实现,可并存
        │                       │
   libgallium              (闭源栈)
        │
   DRI 驱动 / DRM
```

**这决定了整个设计**:

- mesa 只需提供 **glvnd + mesa vendor**,不必也不应该覆盖 `libGL.so.1` 的语义。
- NVIDIA 桥接只需让 **`libGLX_nvidia` / `libEGL_nvidia` 对同一个 glvnd 可见**。
- 选谁由**环境变量按进程决定** —— 这正是 slice 1 交付的能力。

选择协议:

| 子系统 | 发现机制 | 强制选择 |
|---|---|---|
| GLX | X server 的 `GLX_EXT_libglvnd` 自动匹配 | `__GLX_VENDOR_LIBRARY_NAME=mesa\|nvidia` |
| EGL | `__EGL_VENDOR_LIBRARY_DIRS` 下的 JSON,按文件名数字排序 | `__EGL_VENDOR_LIBRARY_FILENAMES` |
| GL 驱动 | `LIBGL_DRIVERS_PATH` | `MESA_LOADER_DRIVER_OVERRIDE` |
| Vulkan | `VK_DRIVER_FILES`(旧名 `VK_ICD_FILENAMES`) | 同左,只列一个 |

> EGL 的 JSON 按**文件名**排序,宿主用 `10_nvidia.json` / `50_mesa.json` 表达优先级。
> 我们的目录必须复刻这个命名约定,否则优先级是随机的。

---

## 4. Track A:mesa

### 4.1 构建方法论(Task #8)

两条路,结论是**分两阶段**:

| | 方案 A:源码构建 | 方案 B:重打包发行版二进制 |
|---|---|---|
| 符号版本 | 可锁到 subos 的 glibc 2.39 | 继承发行版的(Ubuntu 24.04 恰好也是 2.39) |
| 前置成本 | 先补 ~12 个 xpkg | 无 |
| 可复现 | ✅ | ❌ 绑死某个发行版某次构建 |
| 许可 | MIT,无问题 | MIT,无问题 |

**决策:B 作为限时 bootstrap,A 作为最终交付物。**

理由是把两类风险分开:B 能在几天内把**机制**端到端跑通(env 声明 → glvnd 选中 →
llvmpipe 出图),从而先证明 slice 1 的设计对图形栈真的成立;而 A 要先补 12 个包,
如果同时验证机制和构建,失败时分不清是哪一头的问题。

**B 绝不能作为最终交付**:它把 payload 悄悄钉死在 Ubuntu 24.04 的 ABI 上,
在别的宿主上"能装、跑起来才炸" —— 正是 slice 1 最反对的那种失效形态。
必须在 recipe 里写明这是临时形态,并且 A 落地时整个替换。

**方案 A 的构建参数**(待实测校准):

```
meson setup build \
  -Dgallium-drivers=llvmpipe,iris,radeonsi,nouveau,zink \
  -Dvulkan-drivers=swrast,intel,amd,nouveau \
  -Dglvnd=enabled \
  -Dplatforms=x11,wayland \
  -Dllvm=enabled -Dshared-llvm=enabled \
  -Dbuildtype=release -Db_ndebug=true
```

驱动集正对应用户要的四种硬件:CPU(llvmpipe/lvp)、Intel(iris/ANV)、
AMD(radeonsi/RADV)、NVIDIA 开源(nouveau/NVK)。

**LLVM 的处置**见 §2.8:独立成 `llvm-runtime` 包,用
`-DLLVM_TARGETS_TO_BUILD=X86;AMDGPU -DLLVM_LINK_LLVM_DYLIB=ON` 构建,
预计 137 → 40~60 MB。

不能走的两条路,记下来免得再被提起:

- **`-Dllvm=disabled`** —— 省 137 MB,但 §2.5 已经证明这会**同时失去 llvmpipe 和
  radeonsi**,即失去"CPU"和"AMD"两种目标硬件。不可接受。
- **把 libLLVM 加回 `llvm` 包** —— 违反索引的 llvm-subpackaging 验收红线,
  且让每个只想要 clang 的人多背 137 MB。

### 4.2 payload 布局

```
mesa/<ver>/
├── lib/
│   ├── libGLX_mesa.so.0        libEGL_mesa.so.0        libgbm.so.1
│   ├── libgallium-<ver>.so     libLLVM.so.<ver>
│   ├── libvulkan_lvp.so        libvulkan_intel.so
│   ├── libvulkan_radeon.so     libvulkan_nouveau.so
│   ├── dri/                     ← libdril_dri.so + 全部 *_dri.so 符号链接
│   └── (第三方:libdrm* libxcb* libexpat libzstd libelf …)
└── share/
    ├── glvnd/egl_vendor.d/50_mesa.json
    ├── vulkan/icd.d/{lvp,intel,radeon,nouveau}_icd.json
    └── drirc.d/
```

**JSON 里的 `library_path` 必须是绝对路径或可被 RPATH 解析的裸名**。宿主用裸名
(`libEGL_mesa.so.0`)靠 ld.so 缓存;subos 里没有那个缓存,所以 recipe 在 install
时要把 JSON 里的路径改写成 `${pkgdir}` 展开后的绝对路径。**这是最容易漏的一步**,
漏了的表现是 EGL 静默回落到别的 vendor —— 又一个 silent-success。

### 4.3 recipe 骨架(Task #9)

```lua
package = {
    spec = "2", name = "mesa", namespace = "compat",
    description = "Mesa 3D — GL/EGL/Vulkan for CPU, Intel, AMD and NVIDIA-open",
    licenses = {"MIT"}, type = "package", archs = {"x86_64", "aarch64"},
    categories = {"graphics"},
    xpm = { linux = { ["latest"] = { ref = "25.2.8" },
                      ["25.2.8"] = { res = true, sha256 = { x86_64 = "…" } } } },
}

import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
import("xim.libxpkg.subos")

function install()
    -- 解包;把 JSON 里的 library_path 改写为绝对路径(见 §4.2)
    __rewrite_vendor_jsons(pkginfo.install_dir())
    return true
end

function config()
    local d   = pkginfo.install_dir()
    local tag = package.name .. "@" .. pkginfo.version()

    xvm.add(package.name)
    -- megadriver + 符号链接一次性声明,不逐个手写(§1.1)
    if xvm.files then
        xvm.files{ src = "lib",   dst = "usr/lib",   binding = tag }
        xvm.files{ src = "share", dst = "usr/share", binding = tag }
    end

    -- 配置基质:PATH/RPATH 供不了的那三套发现协议
    if type(subos.env) == "function" then
        subos.env{ var = "LIBGL_DRIVERS_PATH", op = "set",
                   value = "${pkgdir}/lib/dri", binding = tag }
        subos.env{ var = "__EGL_VENDOR_LIBRARY_DIRS", op = "set",
                   value = "${pkgdir}/share/glvnd/egl_vendor.d", binding = tag }
        subos.env{ var = "VK_DRIVER_FILES", op = "prepend",
                   value = "${pkgdir}/share/vulkan/icd.d", binding = tag }
        subos.env{ var = "XDG_DATA_DIRS", op = "prepend",
                   value = "${pkgdir}/share", binding = tag }
    end
    return true
end
```

**注意**:`__EGL_VENDOR_LIBRARY_DIRS` 用 `set` 而不是 `prepend`。这是刻意的 ——
prepend 会把宿主的 `/usr/share/glvnd/egl_vendor.d` 留在搜索路径里,于是宿主的
`10_nvidia.json` 排在我们的 `50_mesa.json` 前面,hermetic 边界当场失效。
需要同时看到两者的场景由 Track B 显式合并目录来表达(§5.3),不靠 prepend 碰运气。

### 4.4 dlopen 闭包审计

`readelf -d` 只能看到 `DT_NEEDED`。mesa 还会 dlopen:

- `libdril_dri.so` → `libgallium.so`(通过 `LIBGL_DRIVERS_PATH`)
- Vulkan loader → ICD JSON 里的 `library_path`
- `libgallium` → `libLLVM.so`(这个在 DT_NEEDED 里,已覆盖)
- VA-API / VDPAU(可选,不在 slice 范围)

审计方法(一次性,写进 recipe 的注释而不是靠记忆):

```bash
# 在空 host 容器里跑一个真实 GL 客户端,记录所有成功/失败的 open
strace -f -e trace=openat -o /tmp/trace glxinfo -B
grep -E '\.so' /tmp/trace | grep -v ENOENT   # 实际加载了什么
grep -E '\.so' /tmp/trace | grep    ENOENT   # 找了但没找到 ← 缺的就是这些
```

`ENOENT` 那一列就是闭包缺口。这个命令要进 Task #10 的测试脚本,
让它成为**每次构建都跑的断言**,而不是一次性的人工审计。

---

## 5. Track B:NVIDIA 闭源栈

### 5.1 为什么形态完全不同

不能发布(§1.6),所以唯一可行的是:**在安装时探测宿主,把宿主的用户态桥接进 subos,
并把"桥接了哪个版本"记账**。

这与 hermetic 策略文档里的 `capabilities_host` 白名单是同一件事的具体化:
GPU 是一个必须穿透 hermetic 边界的宿主资源,因为它的用户态与宿主内核绑定。

### 5.2 `nvidia-runtime` recipe

命名用 `host.` 前缀,明确它**不携带 payload**,只描述一次对宿主的借用。

```lua
package = { name = "nvidia-runtime", namespace = "host", type = "package", … }

function installed()
    -- 探测:内核模块在不在,版本是多少
    local v = io.readfile("/proc/driver/nvidia/version")
    return v and v:match("([%d%.]+)") or nil
end

function install()
    local ver = __host_driver_version()     -- "550.144.03"
    if not ver then
        raise("no NVIDIA kernel module on this host (/proc/driver/nvidia/version absent)")
    end
    -- 在 payload 目录里造符号链接,指向宿主的实体
    -- 只链 GL/EGL/Vulkan 三套所需,不链整个 327 MB
    __link_host_libs(ver, pkginfo.install_dir())
    __write_vendor_jsons(pkginfo.install_dir(), ver)
    return true
end

function config()
    local d, tag = pkginfo.install_dir(), "nvidia-runtime@" .. pkginfo.version()
    if type(subos.env) == "function" then
        subos.env{ var = "__EGL_VENDOR_LIBRARY_DIRS", op = "set",
                   value = "${pkgdir}/share/glvnd/egl_vendor.d", binding = tag }
        subos.env{ var = "VK_DRIVER_FILES", op = "prepend",
                   value = "${pkgdir}/share/vulkan/icd.d", binding = tag }
        subos.env{ var = "__GLX_VENDOR_LIBRARY_NAME", op = "set",
                   value = "nvidia", binding = tag }
    end
    return true
end
```

**需要桥接的最小集**(不是全部 327 MB):

```
libGLX_nvidia.so.<ver>       libEGL_nvidia.so.<ver>
libGLESv2_nvidia.so.<ver>    libGLESv1_CM_nvidia.so.<ver>
libnvidia-glcore.so.<ver>    libnvidia-eglcore.so.<ver>
libnvidia-glsi.so.<ver>      libnvidia-tls.so.<ver>
libnvidia-glvkspirv.so.<ver> libnvidia-rtcore.so.<ver>
libcuda.so.<ver>             (Vulkan/CUDA 互操作需要)
libnvidia-ptxjitcompiler.so.<ver>
+ nvidia_icd.json
```

精确清单由 §4.4 的 strace 方法在真机上导出,**不靠这份列表**。

### 5.3 与 mesa 共存

两个包都用 `set` 声明 `__EGL_VENDOR_LIBRARY_DIRS` —— 按 slice 1 的冲突规则,
doctor 会报 warning,且 binding 序后者胜出。**这是对的行为,但对用户不够好**。

正确做法:引入一个**合并目录**。当两个包都在时,由 xlings 侧把双方的 vendor JSON
按宿主的命名约定(`10_nvidia.json` / `50_mesa.json`)链进
`${subosdir}/share/glvnd/egl_vendor.d`,两个包都 `set` 到那个目录。

> 这需要 slice 1 之外的一点新机制(一个"多提供者共享目录"的概念)。
> **本设计不预先实现它** —— 先让两个包各自能单独工作,冲突由 doctor 显式报出来,
> 等真的有人要同机双栈时再做。列为 §7 的 O3。

### 5.4 宿主驱动升级 = 悄悄断链

**这是 Track B 唯一的严重失效模式,必须设计对策。**

宿主 `apt upgrade` 把驱动从 550.144.03 换成 555.x 之后:

- 符号链接指向 `libGLX_nvidia.so.550.144.03` —— 文件没了,链接悬空
- 悬空链接**通过所有 `[ -e ]` 检查**(这正是既有 `SysrootDangling` 检查存在的原因)
- 表现:GL 程序报一个不提 xlings 的加载错误

对策,三层:

1. **记账**:`nvidia-runtime` 把探测到的版本写进自己的 payload(一个 `HOST_DRIVER` 文件),
   并作为包版本的一部分(`nvidia-runtime@550.144.03`)。
2. **doctor 新检查**:比对 `/proc/driver/nvidia/version` 与记账值,不一致就报
   `xlings install nvidia-runtime` 重建链接。**这条要新写**,既有的 `SysrootDangling`
   只看链接是否悬空,看不出"版本漂移但恰好新版本也有同名文件"的情况。
3. **不缓存 payload**:`nvidia-runtime` 的 payload 是纯符号链接,重装成本 <1s,
   所以修复动作可以无脑推荐重装。

---

## 6. bwrap 空 host 冒烟测试(Task #10)

**这是 mesa 自洽性的唯一可证伪断言**,也是这台开发机上唯一能做的
mesa 验证(RTX 4080 + 闭源驱动不属于 Track A 的四种目标硬件)。

```bash
# 容器里没有任何宿主 GL:不 bind /usr/lib、不 bind /usr/share/glvnd
bwrap \
  --unshare-all --die-with-parent \
  --ro-bind "$XLINGS_HOME" "$XLINGS_HOME" \
  --ro-bind /usr/bin/env /usr/bin/env \
  --proc /proc --dev /dev --tmpfs /tmp \
  --setenv XLINGS_HOME "$XLINGS_HOME" \
  -- "$XLINGS_BIN" subos use mesa-test --cmd 'eglinfo -B'
```

断言:

| # | 断言 | 失败意味着 |
|---|---|---|
| S1 | 进程不因加载器错误退出 | 闭包缺库(§4.4 的 ENOENT 列) |
| S2 | renderer 字符串含 `llvmpipe` | 选中的是软件渲染,即 payload 自洽 |
| S3 | renderer **不含** `NVIDIA` | 宿主栈漏进来了,hermetic 边界破了 |
| S4 | 真的画出东西(读回像素) | 只报字符串不证明能渲染 |

S3 是最重要的一条:一个能跑通但其实用了宿主 libGL 的测试,会给出与真正成功
**完全相同的输出**。必须显式否证。

S4 用一个最小 EGL surfaceless 程序读回 framebuffer,比对期望颜色 —— 
`eglinfo` 打印正常但 `eglMakeCurrent` 失败是常见形态。

**在 CI 里的位置**:pkgindex 的 build-sanity,每次 mesa 版本变更时跑。
不进 xlings 的 e2e —— 它测的是 payload,不是 xlings。

---

## 7. 任务拆分与依赖

```
A1 补 12 个构建依赖 xpkg ──┐
   (meson/pkgconf/libdrm/  │
    zstd/elfutils/libxcb/  │
    libx11/wayland/…)      │
                           ▼
                    A3 mesa 源码构建(方案 A)
A2 方案 B bootstrap ──┐         ▲
   重打包 Ubuntu 二进制 │         │ 替换
   +env 声明          ▼         │
                 A4 机制验证 ────┘
                 (S1~S4 冒烟)
                       │
                       ▼
                 A5 精简 LLVM 构建
                 (X86;AMDGPU,137→~50MB)

B1 nvidia-runtime 探测+桥接 ──▶ B2 doctor 版本漂移检查 ──▶ B3 真机验证(本机可做)

O3 vendor JSON 合并目录 ── 仅当出现同机双栈需求
```

**关键路径是 A1**(12 个包),不是 mesa 本身。A2 可以完全并行,并且应该先做 ——
它用几天时间买到"机制是否成立"的答案。

B 轨与 A 轨**完全独立**,可并行。而且 B 轨在这台机器上**当天就能验证**,
A 轨在这台机器上永远只能验到 llvmpipe。

估算:A2 约 3~5 天;A1 约 2~3 周;A3+A5 约 1 周;B 轨约 1 周。

---

## 8. 开放问题

| # | 问题 | 阻塞谁 |
|---|---|---|
| O1 | 精简 LLVM 到底能降到多少?需实测 `LLVM_TARGETS_TO_BUILD=X86;AMDGPU` | A0 的收益,以及 §1.4 的总量 |
| O2 | aarch64 要不要一并出?驱动集不同(无 iris/radeonsi,有 panfrost/freedreno) | A3 的构建矩阵 |
| O3 | 同机双栈(mesa + NVIDIA 闭源)是否是真需求?若是,vendor JSON 合并目录怎么做 | §5.3 |
| O4 | Wayland 要不要进 slice?目前只列了 x11+wayland 两个 platform,但 wayland 的
      客户端库也要进闭包 | payload 组成 |
| ~~O5~~ | ~~`libstdc++` 从哪来~~ **已由 §2.6 关闭**:走 `deps`(`xim:gcc`),不自带;
        需在 recipe 钉 `GLIBCXX_3.4.29` 下限 | — |
| O6 | `llvm-runtime` 的版本策略:跟随 mesa 的 LLVM 需求,还是独立 major 线? | A0 |

O6 是新的:mesa 每个大版本对 LLVM 的最低要求会前移(25.x 要 ≥15),而
`llvm-runtime` 一旦有第二个消费者就不能只跟着 mesa 走。倾向按 LLVM 自己的 major
线发版(`llvm-runtime@20`),由 mesa 的 recipe 声明区间。

---

## 9. 一句话总括

> **两条轨约束相反:mesa 能发布但要自己构建(241 MB,其中 `libLLVM.so` 137 MB 独立成
> `llvm-runtime`,按 X86;AMDGPU 两个 target 精简后预计 ~150 MB 总量);
> NVIDIA 闭源不能发布只能从宿主借(0 MB payload,但宿主升级会悄悄断链,需要记账 +
> doctor)。libglvnd 让两者在同一个 subos 里共存,选谁由 slice 1 已交付的环境变量机制
> 按进程决定。关键路径不是 mesa,是它的 12 个构建依赖;而唯一能证伪 payload 自洽性的
> 是 bwrap 空 host 测试里"renderer 不含 NVIDIA"那一条断言。**
