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
