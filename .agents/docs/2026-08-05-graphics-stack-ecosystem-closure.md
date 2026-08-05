# 图形栈生态闭环设计:让用户无感地运行图形程序

**日期**: 2026-08-05
**类型**: 详细设计(detailed design)
**上游**: `2026-08-05-graphics-stack-design.md`(初版,其 Track A 的"repack 宿主二进制"路线**已作废**)
**目标**: `xlings install <图形程序>` 后直接能跑,栈来自 xlings 生态而非宿主
**状态**: 设计待评审;所有数字均为 2026-08-05 实测

---

## 0. 这份文档为什么推翻上一版

上一版 Track A 提出"方案 B bootstrap:重打包宿主的 mesa 二进制"。**方向是错的。**

xlings 的定位是**用户态 OS**。一个 OS 的图形栈不能是"把宿主发行版装了什么抄过来" ——
那样它在没装 mesa 的宿主上就没有图形栈,在装了别的版本的宿主上就是另一个栈。
**能不依赖宿主的,就不依赖宿主**;缺的东西补进 xim-pkgindex,而不是从 `/usr/lib` 拿。

这份文档按这条原则重做,并给出**完整的包清单**和**分层依赖图**。

---

## 1. 设计原则:最小宿主表面

不是"尽量少依赖宿主",而是先划清**哪些在物理上不可能是我们的**,其余一律是我们的。

### 1.1 物理上不可能属于我们的四样

| # | 宿主表面 | 为什么不可能是我们的 |
|---|---|---|
| H1 | **内核 DRM/KMS**(`/dev/dri/*` 的 ioctl) | 它**就是内核**。hermetic 策略的 non-goal 明确不接管内核 |
| H2 | **`/sys` 只读**(设备枚举) | 同上,内核暴露的信息面 |
| H3 | **显示服务器**(X server / Wayland compositor) | 它是一个**独立进程**,我们通过 **socket 协议**跟它说话 —— 这是**服务边界**,不是 ABI 边界 |
| H4 | **NVIDIA 闭源用户态** | 与内核模块 ABI 逐位锁步(§6),且许可禁止重分发 |

H3 值得强调:依赖宿主的 X server,与依赖宿主的内核是同一性质 ——
**协议稳定的服务调用**,不是链接依赖。客户端库(`libX11`/`libxcb`)完全可以是我们的,
而且**必须**是我们的,否则就把宿主的 glibc 拖了进来。

### 1.2 因此必须是我们的

除上面四样之外的**全部**:

```
mesa(GL/Vulkan 驱动)   libLLVM        libglvnd(GL 分发层)
Vulkan loader           libdrm         libpciaccess / libxshmfence
X11 客户端库全套         Wayland 客户端库   libxkbcommon
fontconfig / freetype    glibc / libstdc++
```

**实测支撑**:`libvulkan.so.1` 的 `DT_NEEDED` 只有 `libm` / `libc` ——
Vulkan loader 完全可自带,没有任何理由借宿主的。

### 1.3 由此得到的验收判据

> 在一个**没有 `/usr` 的 bwrap 容器**里,只挂载 subos 与 `/dev/dri`,
> 一个 GL 程序必须能渲染并读回正确像素。

这不是"最好能做到",这是**定义**。做不到就说明还有宿主依赖没被识别出来。
具体断言见 §8。

---

## 2. 实测事实

在 RTX 4080 / Ubuntu 24.04 / mesa 25.2.8 / LLVM 20.1 / NVIDIA 550.144.03 上测得。

### 2.1 mesa 25.x 是单一 megadriver

`dri/` 下 40+ 个 `*_dri.so` **全是符号链接**,指向 117 KB 的 `libdril_dri.so` shim,
实现在单一 `libgallium.so`(41.4 MB,含 llvmpipe/iris/radeonsi/nouveau/zink)。

### 2.2 Vulkan 是独立一套

```
libvulkan_lvp.so 11.4 MB   libvulkan_intel.so 20.2 MB
libvulkan_radeon.so 14.5 MB   libvulkan_nouveau.so 15.2 MB
```
发现走 JSON → `VK_DRIVER_FILES`,与 GL 的 `LIBGL_DRIVERS_PATH` 是**两套独立协议**。

### 2.3 mesa 只需要 LLVM 的两个 target

对 `libgallium` 做未定义符号分析:

| target | 服务于 | 硬件 |
|---|---|---|
| **X86** | llvmpipe(GL)/ lavapipe(Vulkan)的 JIT | CPU |
| **AMDGPU** | radeonsi 着色器编译(27 处符号) | AMD |

Intel(iris/ANV)与 NVIDIA 开源(nouveau/NVK)**不经过 LLVM**,走 NIR 自带后端。
所以 `LLVM_TARGETS_TO_BUILD=X86;AMDGPU` 是实测结论,砍掉其余十几个 target 安全。

### 2.4 符号版本:hermetic 边界成立

```
libgallium / libLLVM 最高需要:  GLIBC_2.38 ,GLIBCXX_3.4.29
subos 提供:                    glibc 2.39 ,gcc-runtime 15.1.0 → GLIBCXX_3.4.34   ✅
```

这正是 issue #352 的反面:#352 是宿主的**新** glibc(2.43)漏进来把 GLFW 打挂;
只要 payload 针对 subos 的 glibc 构建、loader 与 libc 都由 subos 提供,宿主是什么都不相关。

### 2.5 完整运行期闭包 = 40 个库

以 `LIBGL_ALWAYS_SOFTWARE=1` 跑 `glxinfo` 并 strace,实际加载 44 个 `.so`,
除 glibc/libstdc++/libgcc_s 外 **40 个**需要由生态提供。这是包清单的实测依据,
不是照着 Debian 的依赖表抄的。

---

## 3. gcc / llvm / libLLVM / mesa:四者关系

最易混的是 **`llvm`(编译器)** 与 **`libLLVM.so`(库)** —— 同源,消费者不相交。

| | 是什么 | 谁消费 | 索引现状 |
|---|---|---|---|
| **gcc** | 编译器(~1.1 GB) | 开发者 | ✅ |
| **gcc-runtime** | libstdc++ / libgcc_s(~25 MB) | 任何 C++ 程序,含 mesa | ✅ |
| **llvm** | clang/lld 工具链 | 开发者 | ✅ |
| **libllvm** | `libLLVM.so`(JIT/代码生成) | **几乎只有 mesa** | ❌ 待建 |
| **mesa** | GL/Vulkan 实现 | 图形程序 | ❌ 待建 |

索引的 `llvm` 包**按明文规范就不含 `libLLVM.so`** ——
`.agents/skills/llvm-subpackaging` 的验收红线:
"clang 驱动、lld 都静态链接 LLVM;**Linux `ldd` 不应出现 `libLLVM.so`**"。
所以这不是遗漏,`libllvm` 是索引里确实缺的一个新包。

**ABI 耦合实测**:`libgallium` 引用 233 个 C API 符号 **+ 97 个 C++ mangled 符号**,
且都带 `@LLVM_20.1` 符号版本标签;`libLLVM.so.20.1` 只导出一个版本节点。
→ 精确锁 major.minor,消费者必须**硬钉**;配错是**加载期硬失败**(fail-closed)。

**命名**:`libllvm`。索引里库包一律用上游真名加 `lib` 前缀(`libffi`/`libpng`/`libxml2`),
`gcc-runtime` 是唯一例外且有理由(七个库的 bundle,无单一上游名)。
业界也一致:Debian `libllvm20`、openSUSE `libLLVM20`。

---

## 4. 完整包清单与依赖分层

**这是本文档的核心交付物。** 实测缺 **30 个包**,已有 17 个可直接 `deps`。

### 4.1 已有(直接依赖,不重复造)

```
glibc  gcc-runtime  linux-headers
expat  zlib  libffi  libxml2  libpng  pcre2
freetype  fontconfig  pixman  glib  harfbuzz  fribidi  pango  cairo
```

构建工具已有:`ninja  python  rust  cmake  make  nasm  binutils  gcc  patchelf`

### 4.2 待建(30 个,按依赖分层)

**T0 — 构建工具(6)**,只在构建期用,不进运行期 payload:

| 包 | 用途 |
|---|---|
| `meson` | mesa / libdrm / wayland / libxkbcommon 的构建系统 |
| `pkgconf` | `pkg-config` 实现,meson 找依赖靠它 |
| `bison` `flex` | mesa 的 GLSL 编译器前端 |
| `zstd` | libLLVM 与 mesa 都链它(**同时也是运行期依赖**) |
| `elfutils` | 提供 `libelf`,radeonsi 需要(**同时也是运行期依赖**) |

**T1 — 底层协议与内核接口(5)**:

| 包 | 说明 |
|---|---|
| `xorgproto` | X11 协议头,**纯 build-time** |
| `xcb-proto` | XCB 协议描述(Python),**纯 build-time** |
| `libdrm` | 内核 DRM ioctl 的用户态封装 + `libdrm_{amdgpu,intel,nouveau,radeon}` |
| `libpciaccess` | PCI 设备枚举 |
| `libxshmfence` | DRI3 的共享 fence |

**T2 — X11 客户端栈(10)**,依赖 T1 的 proto:

```
libXau ← libXdmcp ← libxcb ← libX11 ← libXext
                                    ← libXfixes ← libXrandr
                                    ← libXrender ← libXcursor
                                    ← libXxf86vm  ← libXi
```

**T3 — Wayland 客户端栈(3)**:`wayland`(libwayland-client/server/cursor/egl)、
`wayland-protocols`(build-time)、`libxkbcommon`

**T4 — 图形核心(4)**:

| 包 | 依赖 | 说明 |
|---|---|---|
| `libllvm` | gcc-runtime, zlib, libffi, libxml2, zstd | `LLVM_TARGETS_TO_BUILD=X86;AMDGPU`,`LLVM_LINK_LLVM_DYLIB=ON` |
| `libglvnd` | libX11, libXext | GL 分发层 —— **两条驱动路线共存的前提** |
| `vulkan-loader` | (仅 libc/libm) | `libvulkan.so.1` |
| `mesa` | 上面**全部** + lm-sensors | GL/Vulkan 驱动本体 |

**T5 — 杂项(1)**:`lm-sensors`(提供 `libsensors`,libgallium 的 `DT_NEEDED`)

**T6 — 宿主桥接(1)**:`nvidia-gl-host-link`(见 §6,0 payload)

### 4.3 依赖图

```
        T0 构建工具(meson/pkgconf/bison/flex)
              ↓ 仅构建期
  ┌─────────────────────────────────────────────┐
  │ T1  xorgproto  xcb-proto  libdrm            │
  │     libpciaccess  libxshmfence              │
  └───────┬─────────────────────────────────────┘
          ▼
  ┌─────────────────────────────────────────────┐
  │ T2  libXau→libXdmcp→libxcb→libX11→libXext   │
  │     libXfixes libXrandr libXrender          │
  │     libXcursor libXxf86vm libXi             │
  └───────┬─────────────────────────────────────┘
          │      ┌── T3  wayland / wayland-protocols / libxkbcommon
          ▼      ▼
  ┌─────────────────────────────────────────────┐
  │ T4  libllvm   libglvnd   vulkan-loader      │
  │                    ↓                        │
  │                  mesa  ←── T5 lm-sensors    │
  └───────┬─────────────────────────────────────┘
          ▼
    图形程序(godot / GLFW / …)只需 deps 一个 `mesa`
```

**关键路径 = T1 → T2 → T4**。T3(Wayland)可以延后,X11 先跑通。

### 4.4 体积预算

| | 大小 |
|---|---|
| mesa 本体(GL + Vulkan + glue) | 103 MB |
| libllvm(精简至 X86;AMDGPU 后预计) | 40–60 MB |
| X11 + Wayland 客户端栈 + libdrm 等 | ~8 MB |
| **合计** | **~150–170 MB** |

初版设计文档 §10 的 O4 担心"可能超 800 MB 要分包" —— **实测不成立**,不分包。

---

## 5. 构建与发布模型

索引里**零个包有 `build()` hook** —— 全部是"预构建 tarball + 下载 + relocate"。
所以"补进生态"的完整含义是三步,缺一不可:

```
1. 构建   在受控环境里从源码构建,针对 subos 的 glibc 2.39
             ↓
2. 发布   tarball → xlings-res(GitHub GLOBAL + GitCode CN 双镜像)
             ↓
3. 接线   xim-pkgindex 加 recipe:xpm 指向资源 + deps + config()
```

### 5.1 构建环境:必须针对 subos 的 glibc

不能在宿主上随手 `meson build` —— 那样产物会链上宿主的 glibc 符号,
在别的宿主上就是 #352。构建必须在**装了 xim:glibc@2.39 + gcc 的 subos 里**进行,
这也正是 `glibc.lua` 里 `fromsource-x-glibc` 那套路径改写存在的原因:
它处理的就是"构建机路径与安装机路径不同"。

### 5.2 deps 版本语法:用范围,不要硬钉

**索引里现存的 recipe 全部是硬钉**(`"xim:glibc@2.39"`、`"xim:freetype@2.13.2"`),
一个用范围的都没有。但那是习惯,不是限制 —— **解析器完整支持区间**,
实测自 `src/core/semver.cppm` 与 `src/core/xim/resolver.cppm`:

| 写法 | 含义 |
|---|---|
| `@2.39` | 精确 |
| `@2` / `@2.39` 前缀 | 组件边界前缀匹配(`@3` 匹配 3.x.x) |
| `@^1.2.3` | `>=1.2.3 <2.0.0`(major 为 0 时退化到 minor / patch) |
| `@~1.2.3` | `>=1.2.3 <1.3.0` |
| `@>=2.35` | 下限 |
| `@>=1.0 <2.0` | 空格分隔的多约束 |
| 省略 | 任意版本 |

resolver 的注释把这件事说死了:

> Constraint satisfaction is `semver::satisfies_expr` — the same grammar
> `catalog.cppm` selects with — so `@3`, `@^1.2` and `>=1.0 <2.0` mean here
> exactly what they mean there.

**为什么这对本次的 30 个包尤其重要**:硬钉会让整个图形栈变成一块铁板 ——
升一个 `libX11` 补丁版就要改十几个 recipe,而它们本来只在乎"有这个库"。

**因此本设计的约定**:

- **默认用下限**:`"xim:zlib@>=1.2"`、`"xim:libX11@>=1.8"`。
  这些库 ABI 稳定,消费者要的是"存在且不太老"。
- **glibc 例外,硬钉 `@2.39` —— 原因是客户端缺陷,不是 ABI**。
  §2.4 测到 mesa 的实际下限是 `GLIBC_2.38`,所以 `>=2.38` 才是事实。
  但 **2026.8.5.2 之前的 xlings 把依赖的版本部分当字面量比**
  (`installer.cppm`:`depNode.version == dep_ver`),`@>=2.38` 匹配不到任何
  plan 节点 → glibc 的 `exports` 进不了 `deps_exports` → elfpatch 判定
  "no loader provider in deps" → **整个包一个 RPATH 都不写**。

  这个失败没有任何输出:包报告安装成功,自己的库还带着构建期的 RPATH,
  直到 glvnd `dlopen` mesa 的 EGL vendor、vendor 找不到 `libexpat` 才炸,
  而 EGL 把它报成"没有任何 vendor"—— 距离真正的原因隔了三层。

  glibc 是**唯一**声明 `exports.runtime.loader` 的依赖,所以只有它漏掉是致命的:
  其余依赖漏掉 exports 时,`closure_lib_paths` 会退回 `{lib64, lib}` 约定,
  RPATH 照样是对的。因此只钉这一条,就能让这批包在**已经发布的客户端**上可用。

  xlings 侧已按 `semver::satisfies_expr` 修好(2026.8.5.2);
  等到可以假定客户端不低于它时,这一条应改回 `>=2.38`。
- **只有真正锁死的才硬钉** —— 目前只有一处:`mesa` → `libllvm`。
  §3 测到 97 个带 `@LLVM_20.1` 符号版本的 C++ 符号,那是**精确到 major.minor**
  的锁,写范围会让人以为能升。

**范围不会引发无谓升级**:`pin_target_to_active` 保证 ——
已安装且满足约束的版本**优先于索引里的最新版**。所以 `>=1.2` 不会在每次
`xlings install` 时把已经装好的 1.2.11 换成 1.3.0。

> 这一条同时是对索引现状的一个建议:现有 recipe 的硬钉大多也不是必需的,
> 但改动它们不在本设计范围内,只在新增的这 30 个包上先立规矩。

### 5.3 每个包的接线要点

- `exports.runtime.libdirs` —— 让 xlings 的 elfpatch 自动给消费者补 RPATH。
  `gcc-runtime` 已经是这个范式的样板。
- 运行期只依赖①subos 提供的(glibc/libstdc++)②同包或依赖包随附的。
  **不允许出现宿主路径** —— 这是 §8 验收的直接对象。
- vendor JSON(glvnd EGL / Vulkan ICD)里的 `library_path` 必须在 `install()`
  里改写成绝对路径。留裸名会被宿主的 ld.so cache 解析掉,而那正是要关掉的口子。

### 5.4 sysroot:头文件要合并,库要注册

subos 的 `usr/include` 和 `lib` 合起来才是一个 sysroot。这两件事各踩了一个坑。

**头文件:`xvm.files{ src = "include", dst = "usr/include" }` 是错的。**
`files` 资产由 `rename(2)` 落地,`dst` 写整个目录就意味着**整体替换**:
18 个包都这么声明,最后一个安装的包把 `usr/include` 换成指向自己 payload
的符号链接,前面 17 个连同 **glibc 的 477 个头文件**一起消失。装完 mesa 的
subos 编译 `#include <stdint.h>` 直接失败。

索引里两个现成 helper 都表达不了这件事:`declare_headers` 按直接子项声明,
`X11/` 作为一个资产整体落地(后写者赢);`install_headers` 跳过已存在的名字
(先到者赢)。而 `X11/` 由 xorgproto、libX11、libXau、libXdmcp、libXext、
libXfixes、libXxf86vm、libxshmfence **八个包共同贡献**。

所以新增 `sysroot.declare_headers_tree`:**递归到叶子,只声明文件,从不声明目录**。
xlings 会为每个资产创建父目录,于是 `usr/include/X11/` 成为真实目录,
装着八个包各自的符号链接 —— 和发行版组装 `/usr/include` 的方式一致。
整个图形栈 270 个节点。`install_headers_tree` 是给没有 `xvm.files` 的老客户端
准备的同语义降级路径。

**一个必须知道的测试盲区**:`config --add-xpkg` 把 recipe 复制进**本地索引**,
而 `import("xim.pkgindex.sysroot")` 是按 recipe 所属索引解析的。本地索引没有
`libs/`,于是 import 落到未知模块的桩上 —— **桩的每个字段都是真值且可调用**,
所以 recipe 里每一次 sysroot 调用都"成功"且什么也没做,
连 `if not sysroot.declare_headers_tree(...)` 的 else 分支都不会走。

CI 的安装测试正是用 `--add-xpkg` 注册被改动的 recipe。这就是为什么
"把整个 subos sysroot 换成指向某个包 payload 的符号链接"这样的改动能全绿通过:
**要测的那段代码在测试里根本没运行**,只有发布之后才会跑。
修法在 `.github/scripts/posix-test.sh`:注册前先把本仓库的 `libs/` 拷进本地索引。

**库:装了能跑,但链不上。** `exports.runtime.libdirs` + elfpatch 让
**消费者**的 RPATH 指向 payload,所以程序能跑;但 `<subos>/lib` 里只有 glibc 的,
`gcc -lEGL` 什么也找不到。于是 subos 处在一个奇怪的状态:
编译器找得到头文件,却找不到库。`sysroot.declare_libs` 按 zlib / glibc 已有的
`xvm.add(name, {type = "lib", ...})` 约定注册 `lib/*.so*` ——
**只取直接子项**,这样 mesa 的 `lib/dri/*.so` 十二个驱动模块不会进链接路径
(它们是靠 `LIBGL_DRIVERS_PATH` 按路径加载的,不是链接目标)。

---

## 6. NVIDIA 闭源:唯一保留的用户态宿主依赖

### 6.1 glibc 不是障碍(实测)

```
libGLX_nvidia.so.550.144.03      max GLIBC_2.4
libEGL_nvidia.so.550.144.03      max GLIBC_2.9
libnvidia-glcore.so.550.144.03   max GLIBC_2.10
                    subos 提供:  glibc 2.39   ✅ 绰绰有余
```

NVIDIA 刻意针对极老 glibc 构建。会坏的是反方向(subos glibc 比驱动要求还老),
实践中不存在。额外只需 `libX11` / `libXext`,而 T2 正好提供。

### 6.2 那为什么还是不自持

glibc 排除后剩两道,第一道是硬的:

1. **与内核模块 ABI 逐位锁步**。发 550.144.03 的用户态、宿主内核是 555 就失败。
   自持就得**每个驱动版本发一份**(327 MB × N)。
2. **重分发许可**。`libcuda-host-link` 的 recipe 已表明项目立场:
   "The NVIDIA Driver EULA forbids third-party redistribution"。

### 6.3 各生态怎么解:除 Flatpak 外全部选"借"

| 方案 | 做法 | 自持 |
|---|---|---|
| **NixOS** | `/run/opengl-driver/lib` —— 故意**不纯**的路径,由系统模块填入匹配当前内核模块的驱动 | ❌ |
| **nixGL / nixglhost** | 探测宿主驱动版本,把宿主库拷进缓存目录 + `LD_LIBRARY_PATH` | ❌ |
| **nvidia-container-toolkit** | `libnvidia-container` 把宿主驱动库**挂载**进容器 | ❌ |
| **Flatpak** | `org.freedesktop.Platform.GL.nvidia-<ver>`,每个驱动版本一个 extension | ✅ |

Nix 的经验最贴切:**驱动库必须匹配内核模块,所以不可能钉进包集**。
这不是我们的将就,是这个问题的主流解。

### 6.4 `nvidia-gl-host-link`:照抄已有的 sentinel 模式

索引里 `libcuda-host-link` 已经实现了同一模式,并独立得出同样结论。照搬:

| 它做的 | 为什么 |
|---|---|
| 只装指向宿主库的符号链接 | "宿主库在哪"的**单一真相源** |
| 宿主没驱动时**故意留悬空** | 用户后装驱动,重装本包即可恢复 |
| 消费者链到**本包的链接**而非直连宿主 | 重装一次,全下游生效 |

**唯一严重失效模式**:宿主 `apt upgrade` 换驱动 → 符号链接悬空,
而悬空链接**通过所有 `[ -e ]` 检查**。对策:包记账宿主驱动版本,
doctor 比对 `/proc/driver/nvidia/version`,不一致就提示重装。

#### 实现后补录:照抄 sentinel 模式不够,还有三件事

**一、必须链**整套**,不是两个 vendor 入口。**
`libGLX_nvidia.so.0` 的 DT_NEEDED 原文是
`libnvidia-glsi.so.550.144.03  libnvidia-tls.so.550.144.03  libnvidia-glcore.so.550.144.03`
—— 私有半边**把驱动版本写进 SONAME**,只有同名符号链接才解析得了。
只链两个入口做出来的东西在宿主上能加载(靠 ld.so cache),
在我们关心的任何地方都不能。所以按前缀枚举整个目录(本机 50 个文件)。

**二、vendor JSON 要重写,不能链。**
宿主的 `10_nvidia.json` 写的是裸 SONAME `libEGL_nvidia.so.0` ——
在有 cache 的宿主上正确,在别处就是**给宿主栈留的一扇门**。
和 mesa 重写 `50_mesa.json` 是同一件事、同一个理由。

**三、这是全栈唯一需要"搜索路径"而不是 RPATH 的地方。**
glvnd 用绝对路径 dlopen vendor,vendor 的依赖随后按**进程的搜索路径**解析;
而 vendor 是宿主的文件,我们**不能给不属于自己的文件写 RPATH**。
所以只能给 `LD_LIBRARY_PATH`,问题变成"指哪个目录":

- **不指 `${subosdir}/lib`**。全栈其余部分都通过 payload 目录解析,
  payload 目录由依赖图钉死;subos 的 lib 目录装的是"这个 subos 恰好装了什么"。
  装进一个没有 libX11 的 subos,`${subosdir}/lib` 就会安静地缺一块。
- **指本包自己的目录**,并由 `install()` 把它补成自足的:
  `lib/` 只放宿主的 NVIDIA 文件,`lib/xlings-deps/` 放我们这边它需要的 12 个。
  两者分开,因为 `exports.runtime.libdirs` 只应该把宿主那一半给消费者。
- **必须补到传递闭包**。`DT_RUNPATH` **不传递**:libX11 一旦从
  `LD_LIBRARY_PATH` 找到,它自己的 libxcb 就按同一条路径找,而不是按
  加载者的 RUNPATH。少补一层,剩下的就从宿主解析 —— 而且是静默的,
  因为宿主有 libxcb 的机器看起来一切正常。

为什么必须"集中"而不是把依赖的 payload 目录直接列进 `LD_LIBRARY_PATH`:
`subos.env` 的 `${pkgdir}` 只能解析**声明者本身**的目录,
没有"某个依赖的 payload 目录"这种写法;把绝对路径写进 value 则正好放弃了
占位符存在的意义 —— manifest 要能描述不止写它的那台机器。

**实测(RTX 4080 / 550.144.03)**:`EGL_DEVICE_COUNT` 2 → 3,
`EGL_VENDOR=NVIDIA`,`GL_RENDERER=NVIDIA GeForce RTX 4080/PCIe/SSE2`,
`GL_VERSION=4.6.0 NVIDIA 550.144.03`,像素回读正确。

**探针要另写一个。** `glprobe` 走 `EGL_PLATFORM_SURFACELESS_MESA`,
那是 mesa 的扩展,NVIDIA 不提供 —— 拿它测 NVIDIA 会在第一步失败,
且**对 vendor 是否可用什么都没说**。NVIDIA 的无头路径是
`EGL_EXT_platform_device`,所以有 `nvprobe.c`。

---

## 7. 用户无感:目标形态

终局是这个,别的都是手段:

```bash
xlings install godot
godot                      # 直接出图,不需要任何环境变量、不需要懂 GL
```

怎么做到:

1. **图形程序只 deps 一个 `mesa`**。整棵 T1–T5 由依赖解析自动带出来。
2. **`mesa` 的 `config()` 用 `subos.env{}` 声明发现协议** ——
   `LIBGL_DRIVERS_PATH` / `__EGL_VENDOR_LIBRARY_DIRS` / `VK_DRIVER_FILES` /
   `XDG_DATA_DIRS`。这是 slice 1(2026.8.5.1)已经交付的能力,用户零操作。
3. **硬件加速自动生效**:mesa 一个包同时覆盖 CPU(llvmpipe)、Intel(iris)、
   AMD(radeonsi)、NVIDIA 开源(nouveau/NVK)。选哪个由 mesa 按 `/dev/dri` 探测,
   用户不用选。
4. **NVIDIA 闭源按需叠加**:宿主有闭源驱动时装 `nvidia-gl-host-link`,
   libglvnd 按 vendor JSON 优先级(`10_nvidia` < `50_mesa`)选中它 ——
   与宿主上的行为一致。

### 7.1 由此升级的一个未决问题

§4 里 `mesa` 与 `nvidia-gl-host-link` 都要 `set`
`__EGL_VENDOR_LIBRARY_DIRS`。按 slice 1 的冲突规则这会 doctor 报 warning、
binding 序决胜 —— **对"无感"来说不够好**。

需要一个**多提供者共享目录**:两个包各自把自己的 vendor JSON 链进
`${subosdir}/share/glvnd/egl_vendor.d/`,都 `set` 到那个目录,
由文件名(`10_nvidia.json` / `50_mesa.json`)表达优先级 —— 正是宿主的做法。

初版设计把它列为"仅当出现同机双栈需求再做"。**现在它是必须的**:
一台有 NVIDIA 闭源驱动的机器上,用户既要闭源驱动跑游戏,
也要 llvmpipe 兜底跑无 GPU 的场景,两者必须共存。

---

## 8. 验收:bwrap 空 host

**§1.3 的判据的可执行形式**,也是唯一能证伪"自包含"的手段。

```bash
bwrap --unshare-all --die-with-parent \
  --ro-bind "$XLINGS_HOME" "$XLINGS_HOME" \
  --dev-bind /dev/dri /dev/dri \
  --ro-bind /sys /sys \
  --proc /proc --tmpfs /tmp \
  -- "$SUBOS/bin/glprobe"
#  注意:没有 --ro-bind /usr,没有 /lib
```

| # | 断言 | 失败意味着 |
|---|---|---|
| S1 | 进程不因加载器错误退出 | 闭包缺库 |
| S2 | `GL_RENDERER` 含 `llvmpipe` | 走的是我们的软件渲染 |
| S3 | `GL_RENDERER` **不含** `NVIDIA` | **宿主栈漏进来了,边界破了** |
| S4 | `glReadPixels` 读回的像素 == 期望颜色 | 真的渲染了,不只是字符串好看 |

**S3 是最重要的一条**:一个"能跑通但其实用了宿主 libGL"的测试,
输出与真正成功**完全相同**。必须显式否证。

S4 的载体是 `glprobe.c`(EGL surfaceless + `glClear` + `glReadPixels` 比色),
不依赖任何显示服务器 —— 空 host 里本来也没有。

---

## 9. 任务拆分

```
P0  验收先行:glprobe.c + bwrap 空 host 脚本
    (先有判据,再有实现;否则无法判断某一步是否真的前进了)
         ↓
P1  T0 构建工具:meson pkgconf bison flex zstd elfutils        6 个
         ↓
P2  T1 协议与内核接口:xorgproto xcb-proto libdrm
       libpciaccess libxshmfence                              5 个
         ↓
P3  T2 X11 客户端栈                                          10 个
         ↓
P4  T4 图形核心:libllvm → libglvnd → vulkan-loader → mesa     4 个 + lm-sensors
         ↓
P5  空 host 验收(S1–S4 全绿)= CPU/llvmpipe 路线打通
         ↓
    ┌────┴────────────────────────────┐
P6  nvidia-gl-host-link(本机可验)   P7  T3 Wayland(3 个)
         ↓                                    ↓
P8  vendor JSON 共享目录(§7.1)—— 双栈共存
         ↓
P9  真实图形程序端到端(godot 等)
```

**P0 必须在 P1 之前。** 先把"怎么算成功"钉死,否则每加一个包都只能靠"看起来对"判断。

估算:P1–P4 每个包 0.5–1 天(多数是标准 autotools/meson,机械但量大),
共 **3–4 周**;P5–P9 约 1 周。

---

## 10. 开放问题

| # | 问题 | 阻塞谁 |
|---|---|---|
| O1 | 精简 LLVM 实测能降到多少?`LLVM_TARGETS_TO_BUILD=X86;AMDGPU` | libllvm 体积 |
| O2 | aarch64 是否同期出?驱动集不同(无 iris/radeonsi,有 panfrost/freedreno) | 构建矩阵 |
| O3 | 30 个包是否要一个"图形栈"元包,让用户 `xlings install graphics` | UX |
| O4 | Wayland 是否进首版?X11 先跑通更快,但纯 Wayland 桌面会没图 | P7 优先级 |
| O5 | 构建产物如何可复现?索引现在无 `build()` hook,构建脚本放哪、怎么审计 | 长期可维护性 |

O5 是最长期的一个:这次要新增 30 个预构建包,如果构建过程只存在于某个人的机器上,
下次升级 mesa 就得重新摸索一遍。构建脚本应当与 recipe 一起进版本库
(参照 `.agents/tools/build-llvm-subpkg.sh` 的做法)。

---

## 11. 一句话总括

> **xlings 的图形栈必须来自 xlings 生态,不能是"抄宿主装了什么"。物理上不可能属于
> 我们的只有四样:内核 DRM、`/sys`、显示服务器(服务边界而非 ABI 边界)、NVIDIA 闭源
> 用户态(与内核模块锁步)。其余 30 个包全部补进 xim-pkgindex,分 T0–T6 六层,
> 关键路径是 T1→T2→T4,总体积约 150–170 MB。判据先行:一个没有 `/usr` 的 bwrap
> 容器里 GL 程序必须渲染出正确像素,且 renderer 里不能出现 NVIDIA —— 后者是唯一能
> 分辨"真自包含"和"悄悄用了宿主"的断言。**
