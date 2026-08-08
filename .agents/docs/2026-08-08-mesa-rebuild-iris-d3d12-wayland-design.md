# mesa 重建方案:iris / d3d12 / Wayland

> 2026-08-08 · 承接 `2026-08-05-graphics-stack-ecosystem-closure.md` §3(libllvm)
> 目标:把矩阵里三格 `not-exercised-here` 变成真结论

---

## 0. 先纠正三处过期认知

写方案之前我把「已知」逐条验了一遍,**三条是错的**,而且都会把方案带偏。

**(1)「索引无法重建 mesa,已发布的 mesa 链的是宿主 LLVM」—— 已过期。**

```
$ readelf -d lib/libgallium-25.0.7.so | grep libLLVM
      Shared library: [libLLVM.so.20.1]
$ grep -n libllvm pkgs/m/mesa.lua
54:                "xim:libllvm@20.1.7",
```

`libllvm` 早已建好、发布(`xlings-res/libllvm` 20.1.7,2026-08-05)、进索引(`status = "stable"`、sha256 已 pin),而现役 mesa **就是**链着它构建的。那条记录写于 libllvm 存在之前。

**(2)「我们的 mesa 没编 Wayland 平台」—— 错的,我查错了库。**

```
$ strings lib/libEGL.so       | grep -ci wayland   →  0     ← glvnd 转发桩,什么都没有
$ strings lib/libEGL_mesa.so  | grep -c wl_display →  30
                              EGL_KHR_platform_wayland →  1
```

`libEGL.so` 是 glvnd 的**转发层**,对 x11/xcb/wayland 一律 0 命中。实现在 vendor 库里。`tiers.sh` 本来就传了 `-Dplatforms=x11,wayland`。**Wayland 不需要重建 mesa。**

**(3)「iris 需要 libclc」—— 半对,而且这半对决定了整个方案的形状。**见 §3。

**方法论教训**:三条都是「读了一个看起来对的地方」。glvnd 的坑 `reference_glvnd_four_entry_points`
早就写过「每个 vendor 库是独立的加载根」,我没照做。

---

## 1. 现状:真正缺的只有两样

| | 状态 | 证据 |
|---|---|---|
| `libllvm` 20.1.7 | ✅ 建好/发布/进索引/ABI pin | `pkgs/l/libllvm.lua`,`exports.runtime.libdirs = {lib}` |
| mesa 链 libllvm | ✅ 现役即是 | `libgallium → libLLVM.so.20.1` |
| Wayland 平台 | ✅ **能力已在** | `libEGL_mesa.so` 导出 `EGL_KHR_platform_wayland` |
| Wayland **验证** | ❌ **只缺 O4 probe** | `verify-stack.sh` 两路都 `na` |
| DirectX-Headers | ✅ 建好且 clean | sha256 `11ff1564…` |
| d3d12 驱动 | ❌ 需重建 | 载荷里只有 `swrast/libdril/nouveau/radeonsi/zink/kms_swrast` |
| iris 驱动 | ❌ 需重建 + 构建期编译器链 | §3 |

**所以这不是一个任务,是三个成本差两个数量级的任务。**把它们塞进一个「重建 mesa」里是这份方案要避免的第一件事。

---

## 2. 架构:按成本切,不按主题切

```
交付 A  Wayland probe          零构建    ~1 小时    本机可验证
交付 B  mesa + d3d12           1 次重建  ~3 小时    本机只能验载荷,不能验渲染
交付 C  mesa + iris            2 次重建  ~1-2 天    本机完全验不了(无 Intel GPU)
```

**每个交付独立可发布、独立可回滚。**理由不是流程洁癖:

- A 不动任何载荷,风险为零,却能把一格从「未执行」变成真结论 —— **投入产出比最高的一件事**
- B 改载荷但不改构建工具链,是一次干净的 `-Dgallium-drivers` 增量
- C 要引入一整条构建期编译器链(clang-cpp + LLVMSPIRVLib + libclc),**且交付后本机无法验证**

C 单独成一个交付的另一个理由:它是唯一一个「做完了也不知道对不对」的。混在 B 里会让 B 的验证结论也变得可疑。

---

## 3. 关键发现:iris 的 libclc 是**构建期**输入

`mesa.lua` 的注释说「iris and anv require libclc」。查 meson.build,这话**只在默认路径下成立**:

```meson
# meson.build:835
if get_option('mesa-clc') == 'system'
  prog_mesa_clc   = find_program('mesa_clc',   native : true)
  prog_vtn_bindgen = find_program('vtn_bindgen', native : true)
  with_clc = with_gallium_rusticl          # ← iris 不在其中
else
  with_clc = ... or with_gallium_iris or ...   # ← iris 拉 libclc
endif

# meson.build:850
if with_gallium_clover or with_clc
  dep_clc = dependency('libclc')
```

默认路径要三样东西,我们一样都没有:

| 需要 | 出处 | 我们有吗 |
|---|---|---|
| `libclc` | `meson.build:850` | ❌ |
| `LLVMSPIRVLib`(SPIRV-LLVM-Translator) | `meson.build:1882`,`required : true` | ❌ |
| `clang-cpp`(clang 当库用) | `meson.build:1900` | ❌ `llvm` 是 tools-only,`libllvm` 无 clang |

**但 `mesa_clc` / `vtn_bindgen` 是构建期工具** —— 它们在构建时把 OpenCL C kernel 编成 SPIR-V/NIR 塞进驱动,运行期不存在。所以:

### 方案:两趟构建,libclc 不进运行期闭包

```
pass 1   在构建 subos 里建 mesa-clc + vtn_bindgen
         需要 libclc + LLVMSPIRVLib + clang-cpp —— 只在这一趟

pass 2   -Dmesa-clc=system,指向 pass 1 的产物
         -Dgallium-drivers=...,iris
         产出的载荷 零 libclc 依赖
```

**这比「打包 libclc 并发布」优雅得多**:libclc 是编译器输入,不是运行期库。把它塞进用户的闭包,是让每个装 mesa 的人为一个自己永远不会加载的东西付磁盘和下载成本。

### 一个必须正面处理的张力

`build-in-subos.sh` 的 **G2 输入检查**(本轮刚加的)禁止构建消费宿主库。pass 1 需要 clang-cpp 和 libclc,**不能从宿主拿** —— 否则 G2 会红,而且应该红。

所以 pass 1 的输入也得是我们的包。这意味着:

- 需要一个 **build-only** 的 `llvm-dev` 类构件(clang-cpp + LLVMSPIRVLib + libclc)
- 但它**不必进索引给用户装** —— 可以是 `.agents/tools/` 里的构建产物,或一个 `status = "dev"` 的包

这是 C 真正的成本所在,也是它该独立成交付的最强理由。

### 顺带:`intel-bvh-grl` 与 iris 无关

```meson
# meson_options.txt:671
option('intel-bvh-grl', type : 'boolean', value : false)   ← 默认关

# meson.build:319
if ... and with_intel_bvh_grl
  with_intel_clc = ...
else
  with_intel_clc = false      ← 默认走这里
```

`intel-clc`(≠ `mesa-clc`)是给 anv 光追 BVH 用的,默认关,**iris 不碰**。不要被名字带偏去建 intel-clc。

---

## 4. 稳定性

### ABI 锁是资产,不是负担

`libgallium` 引用 97 个带 `@LLVM_20.1` 版本标记的 mangled `llvm::` 符号。所以:

- mesa 必须 pin **patch 级** `xim:libllvm@20.1.7`,不能用 range —— recipe 里已经这么写并注明了理由
- 不匹配是**加载期失败**,不是行为异常。这正是想要的:错了立刻炸,而不是渲染出错误画面

**推论:任何 libllvm 版本变动都强制 mesa 重建。**两个包在版本上是刚性耦合的,方案里必须成对处理。

### 编译器必须是 gcc 15.1.0

`build-libllvm.sh` 里记了两条硬约束,任何重建都得继承:

- **不能用 subos 默认的 gcc 16.1.0** —— 在 `AMDGPUAsmParser.cpp` 2212/2218 处 ICE 段错误。AMDGPU 不能砍(radeonsi 的着色器编译器要它)
- **不能用 clang** —— xlings 的 clang 默认 libc++,mesa 是 libstdc++ 构建的;两个 C++ runtime 符号名对得上、对象布局对不上。强制 `-stdlib=libstdc++` 则链接失败
- **必须走 subos 的 shim**,不能直接调 payload 里的二进制 —— 只有 shim 带 elfpatch 过的 interpreter 和 RPATH,直接调会让 cmake 的编译器探测链接失败

`gcc-runtime` 是 15.1.0,所以用 gcc 15.1.0 让 C++ ABI **由构造保证一致**,而不是靠一个 flag。

### 验收必须包含「没退化」

新增驱动最容易犯的错是把现有的搞坏。所以每次重建后跑完整 `verify-stack.sh`,基线是**本轮实测的 pass 14 / fail 0**,不是「新驱动能跑」。

---

## 5. 兼容性

### 版本号:第四段是我们的构建修订

现役是 `25.0.7.1`,上游是 `25.0.7`。**第四段就是包装修订号**,这正好解决「同版本不同字节」这个最危险的情况:

```
25.0.7.1   现役(llvmpipe softpipe radeonsi nouveau zink)
25.0.7.2   + d3d12
25.0.7.3   + iris
```

**绝对不能**用同一个版本号换字节:sha256 已 pin,已装的 home 会校验失败或悄悄不一致。

### 上游已到 26.2.0,我们在 25.0.7 —— 但不要顺手升

`archive.mesa3d.org` 最新是 **26.2.0**。诱惑是「反正要重建,一起升」。**不要**,理由是耦合:

- mesa 25.0.7 要求 `LLVM >= 15.0.0`(`meson.build:1770`),20.1.7 满足
- mesa 26.x 的 LLVM 要求未核实,若要求 > 20.1.7,**libllvm 也得重建**,而 libllvm 重建要面对 §4 那个 gcc ICE
- 一次改两个变量,失败时无法归因

**升级 mesa 是第四个独立交付**,不是这三个的搭车项。

### d3d12 对非 WSL2 用户是纯负担吗?

不是零成本,但很小:多一个 `d3d12_dri.so`,以及 DirectX-Headers 的构建期依赖(纯头文件,不进运行期闭包)。WSL2 侧真正需要的 `libdxcore` 由已有的 `wsl-gl-host-link` 哨兵从 Windows 侧接,不打进载荷。

**可接受**:一个驱动文件换一个平台,而且它在非 WSL2 机器上根本不会被 `LIBGL_DRIVERS_PATH` 选中。

---

## 6. 简洁 / 优雅

### 不要拆成 per-driver 包

诱惑:`mesa-radeonsi` / `mesa-iris` / `mesa-d3d12` 各一个包,按需装。**反对**:

- mesa 的驱动共享一个 `libgallium-<ver>.so`(33 MB)。拆包意味着要么每个包复制它,要么造一个 `mesa-core` + N 个薄包 —— 后者的收益是省几 MB 的 `.so`,代价是 N+1 个包的版本必须严格同步,而它们之间是 mesa 内部 ABI,**连版本标记都没有**
- `gcc` / `gcc-runtime` 的拆分是对的,因为那条线是**消费者不同**(编译期 vs 运行期)。mesa 的驱动线是**同一个消费者的不同硬件**,不是同一种切法

**一个 mesa 包,驱动按硬件在运行期选**,这也正是 `LIBGL_DRIVERS_PATH` + glvnd 的设计意图。

### Wayland probe 要小

O4 probe 该做的只有一件事:证明 `eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, …)` 在**我们的** vendor 库上拿到 display 并能创建 surface。

- 用 `mutter --headless --wayland` 起一个临时 compositor(宿主已有),完事杀掉
- **不要**引入一个 GUI 工具链去开真窗口 —— 那是 §5 godot 那格的职责,别重复
- 按三态契约:没有 compositor 就 `exit 3`,不是 fail

---

## 7. 交付顺序与验收

| # | 交付 | 前置 | 验收(必须是真跑出来的) |
|---|---|---|---|
| **A** | Wayland O4 probe | 无 | `verify-stack.sh` 该格从 `na` 变 `✓`;整体不低于 pass 15 / fail 0 |
| **B** | mesa 25.0.7.2 + d3d12 | DirectX-Headers(已有) | `d3d12_dri.so` 在载荷里;G2 输入检查过;**pass 不低于 14 / fail 0**;WSL2 格仍 `na`(本机不是 WSL2)但载荷已就位 |
| **C** | mesa 25.0.7.3 + iris | build-only clang-cpp + LLVMSPIRVLib + libclc;两趟构建 | `iris_dri.so` 在载荷里且**零 libclc 依赖**;pass 不低于 14 / fail 0;iris 渲染格仍 `na`(无 Intel GPU)—— 交社区 |
| D | mesa 26.x 升级 | 先核实其 LLVM 要求 | 独立评估,不搭车 |

**A 先做。**它不动载荷、当天能完成、能把一格变成真结论 —— 而 B/C 做完之后,本机能新增验证的格数是 **0**(d3d12 要 WSL2,iris 要 Intel GPU)。

这句话值得停下来看一眼:**B 和 C 的收益不在本机的矩阵上,而在别人的机器上。**所以它们的真正交付物是「载荷里有这个驱动 + collect-matrix 能收回结果」,不是「我这里绿了」。

---

## 8. 实施记录:llvm-dev 已交付,以及它教了什么

交付 A(Wayland probe)和 llvm-dev 包都已完成。llvm-dev 是 §3 那个「两趟构建」方案的
第一趟前置,构建过程推翻了本文里两条我原以为确定的东西。

### 已交付

| | |
|---|---|
| 构建脚本 | `.agents/tools/graphics/build-llvm-dev.sh` |
| 产物 | `xlings-res/llvm-dev` 20.1.7,237 MB,`sha256 b0cdaaad…` |
| CN 镜像 | gitcode,**完整下载哈希与 GLOBAL 一致** |
| recipe | `pkgs/l/llvm-dev.lua`,`status = "dev"` |
| 沙箱验证 | OpenCL C → LLVM IR → SPIR-V,468 字节产出 |

最后一行是真正的验收:**不是「三个文件在不在」,而是「mesa_clc 要走的那条流水线能不能走通」**。
`pkg-config --modversion libclc` 也能解析,而那正是 `meson.build:850` 的路径。

### 推翻一:必须用 gcc 15.1.0 —— 对 libllvm 成立,对这里不成立

`build-libllvm.sh` 的约束是「16.1.0 在 `AMDGPUAsmParser.cpp` ICE,而 AMDGPU 不能砍」。
**这条是 AMDGPU 专属的**,而 llvm-dev 只建 `X86;SPIRV`,碰不到那个文件。

更要紧的是反向:**gcc 15.1.0 在这个生态里根本建不了 LLVM。**它的
`include-fixed/pthread.h` 在搜索顺序上先于 sysroot,遮蔽了我们的
`bits/pthreadtypes.h`,于是 `__gthread_cond_t` 变成 `unsigned int`,libstdc++ **自己的**
`<ext/concurrence.h>` 编不过,构建死在 13/4049:

```
ext/concurrence.h:257: cannot convert '<brace-enclosed initializer list>' to 'unsigned int'
```

已提 `xim-pkgindex#560`。这不是一个头文件的事:任何与 sysroot glibc 不一致的
fixincludes 副本都会这样遮蔽,`pthread.h` 只是 C++ 线程头最先碰到的那个。

**代价**:用 gcc 16 编出来的 llvm-dev,libstdc++ ABI 是 16 的,而索引里 `gcc-runtime`
是 15.1.0 —— 所以 recipe 只能依赖 `xim:gcc@16.1.0`(重得多)。**#560 修好后**换回
`gcc-runtime` 即可,已写在 recipe 注释里。

### 推翻二:「DirectX-Headers 已建好且 clean」—— 建过,但从没打包

原文(和我的任务清单)据此把 B 当成「只差一次 mesa 重建」。查了:

```
$ gh release list --repo xlings-res/directx-headers → 仓库不存在
$ ls pkgs/d/directx-headers.lua                      → 不存在
```

只剩上一轮的 `/tmp/xlings-gfx/directx-headers-*.log`。**产物在临时目录里,不在任何人能装到
的地方。**所以 B 多一步:先把 DirectX-Headers 做成包。

这是「把做过当成交付了」的又一次,值得作为方案的一条纪律:**前置只有在「能被 install
到」时才算就位。**

### 两个非 mesa 的发现

**`libclc.pc` 带构建机绝对路径。**pkg-config 把 `includedir`/`libexecdir` 直接交给 mesa,
不重写就指向不存在的 `/tmp/.../gfxwork/dist`,而失败会表现为 mesa 报缺位码文件 —— 离
原因三层远。recipe 里重写并**断言重写生效**(gsub 没匹配上也会「成功」写回原文)。

**`.xlings.json` 里的 `"mirror": "CN"` 不被下载路径采纳。**同一个 home、同一个 CN URL:

| | 速率 | 实连 |
|---|---|---|
| 只有配置文件 | 40 KB/s | `185.199.109.133`(GitHub CDN) |
| 加 `XLINGS_MIRROR=CN` 等环境变量 | **6.7 MB/s** | gitcode |

差 160 倍。任何靠配置文件的隔离环境或 CI 都在静默跨境下载。这是「配置说一套、行为做
一套」,和本轮其它几个同源。

---

## 9. 实施记录二:真去跑 mesa 构建,推翻了 §8 的一条和本文的两条

`xim-pkgindex#563`。**§8 的「推翻一」本身被推翻了**,而且方向相反。

### 已交付(两个新包 + 一个 gcc 修复)

| | |
|---|---|
| `pkgs/g/gcc.lua` | 剪掉 fixincludes 冻结的头 —— **#560 的根因修复** |
| `pkgs/d/directx-headers.lua` | 新,1.614.1,432K,两区已发布 |
| `pkgs/w/wayland-protocols.lua` | 加 1.45(**1.38 早就在**,见下) |
| `build-directx-headers.sh` / `build-wayland-protocols.sh` | 新,各带输入审计 |
| `build-in-subos.sh` | meson 解析、python 模块、dep bin 上 PATH、build-only pkgconfig |
| `tiers.sh` | T5 的 deps 字段改对(原来那条跑不通) |

两个包都装进隔离 home,**从已安装的 payload** 过 pkg-config 验过:`${pcfiledir}` 落在
payload 里、两种拼写都解析、`pkgdatadir` 里 57 个 XML。

### 推翻 §8 的「推翻一」:gcc 15.1.0 不是建不了,是被一个能删的文件挡住

§8 说「gcc 15.1.0 在这个生态里根本建不了 LLVM」,并据此让 llvm-dev 依赖
`xim:gcc@16.1.0`。**症状是真的,归因只差最后一步。**

那个 `include-fixed/pthread.h` 不是「随便哪台机器的 glibc」,banner 写着它来自
**我们自己的 sysroot**:

```
It has been auto-edited by fixincludes from:
    "/home/xlings/.xlings_data/subos/linux/usr/include/pthread.h"
```

即 gcc 构建时的 glibc **2.39**。现在 sysroot 是 **2.44**,而 2.44 改了
`pthread_cond_t` 的布局。所以这不是宿主泄漏,是**对我们自己 glibc 的版本漂移** ——
每次 glibc 升级都会重演。

`include-fixed` 里只有两个文件(`README` 和这个 `pthread.h`),删掉即可。已在
`gcc.lua` 的 `install()` 里按 **fixincludes banner** 判定剪除(不按文件名白名单:gcc
真正生成的 `limits.h`/`syslimits.h` 没有 banner,必须留)。

> 中间我还错过一次:第一次测「删掉有没有用」时用的是 subos 的 shim,而它当时指向
> gcc 16 —— **等于用 gcc 16 证明了 gcc 15 没问题**。选对 15.1.0 重测才拿到双向证据:
> 有该文件 1 个错误、`-H` 显示 include-fixed 胜出;删掉 0 个错误、sysroot 胜出。

### 推翻二:llvm-dev 必须带 AMDGPU —— libllvm 根本不能当构建输入

§3 把两个包的分工写成「运行时库」与「构建期工具」。**分错了。**

```
$ ls <libllvm payload>/
lib
```

没有头文件、没有 `lib/cmake/llvm/LLVMConfig.cmake`、没有 `bin/llvm-config`。meson 找
LLVM 只有两条路(config tool 或 cmake 包),libllvm **一条都不提供**。

后果不是「找不到就报错」,而是**静默换人**:

```
llvm-config found: YES (/usr/bin/llvm-config) 18.1.3
```

拿了宿主的 LLVM 18,而索引发布的是 20.1.7。这样链出来的 libgallium 需要
`libLLVM.so.18.1` —— 索引里没有,**payload 根本载不起来**。

所以两条轴是「用户加载的那个 .so」和「构建需要的一切」,后者必须覆盖前者的每个
target。llvm-dev 改为 `X86;AMDGPU;SPIRV`,而 AMDGPU 正是让 gcc 16 ICE 的那个文件 ——
**于是必须用 gcc 15,而这恰好由上面的 #560 修复解锁**。两个 LLVM 脚本终于对编译器口径
一致了,它们本来就没有理由不一致。

### 推翻三:wayland-protocols「不在索引里」—— 在,而且没被用

我先断言它不在索引里,并且**用 Write 覆盖了一个已发布的 recipe**(已完整恢复:
`spec = "2"`、`status = "stable"`、1.38 条目保留,1.45 追加)。

真相比我原来的说法更值得记:**它 1.38 早在 2026-08-05 就发布到两区了,而现役 mesa
仍然是拿宿主那份 XML 生成的。**

```
$ ls /home/speak/.xlings/data/xpkgs/*wayland-protocols*   → 什么都没有
```

recipe 没错、payload 没错、O4 probe 也确实证明运行时 7/7 全是我们的。缺的只有**接线**:
它从没在构建那个 home 里装过,T5 那行也既没 `--deps` 也没给 pkgconfig 路径。

**没接线的构建期输入,和已满足的输入长得一模一样。**运行时自包含和构建期自包含是两个
性质,而只有前者有守卫。这条比「缺个包」有价值得多,已写进 recipe、脚本和 tiers.sh。

### 顺带发现的四个「记录与事实不符」

| | 事实 |
|---|---|
| 索引里没有 `meson` | `build-in-subos.sh` 却无条件调 `$SUBOS/bin/meson` —— 这条路径在任何 home 都不存在,**mesa 在内的每次 meson 构建都是宿主 meson 驱动的**(`#562`) |
| mesa 报「缺 mako」 | 真凶是 `packaging`:它的探测先 `import packaging.version`,回退 `distutils.version` —— 而 **Python 3.12 删掉了 distutils**,3.13 上回退分支同样抛异常,于是 mako 装好了也报缺 mako |
| glslang 装了却「找不到」 | payload 里 `bin/glslangValidator` 和 `bin/glslang` 并排,recipe 只注册了 anchor,**mesa 要的那个名字不可达** |
| T5 的 `deps` 是 `libxml2 expat` | 照着重跑直接死在 `ERROR: Dependency "libglvnd" not found`。**记录下来的命令不是当初那条命令。**已补全 18 个包 |

最后一条值得单独说:**一份跑不通的构建记录比没有记录更糟** —— 它让下一个人付同样的代价,
而且看起来权威。

---

## 附:每条结论的证据

| 结论 | 怎么来的 |
|---|---|
| mesa 现役链 libllvm | `readelf -d libgallium-25.0.7.so` → `libLLVM.so.20.1` |
| libllvm 已发布 | `gh release list --repo xlings-res/libllvm` → 20.1.7 |
| Wayland 已支持 | `strings libEGL_mesa.so` → `EGL_KHR_platform_wayland` ×1、`wl_display` ×30 |
| `libEGL.so` 不能用来判断 | 同样 grep 对 x11/xcb/wayland 全 0 |
| iris 可绕开 libclc | `meson.build:835` `mesa-clc == 'system'` 分支 |
| iris 默认要三样 | `meson.build:850`、`:1882`(`required : true`)、`:1900` |
| `intel-bvh-grl` 默认关 | `meson_options.txt:671-674` |
| d3d12 只要 DirectX-Headers | `meson.build:603-609` |
| LLVM ≥ 15.0.0 | `meson.build:1770` |
| 上游 26.2.0 | `archive.mesa3d.org` 目录列表 |
| 现役驱动清单 | `ls lib/dri/` → swrast libdril nouveau radeonsi zink kms_swrast |
| gcc 16.1.0 ICE / clang 不可用 | `.agents/tools/graphics/build-libllvm.sh` 头部注释 |
| 基线 pass 14 / fail 0 | 2026-08-08 用发布二进制 + 已发布索引实跑 |
