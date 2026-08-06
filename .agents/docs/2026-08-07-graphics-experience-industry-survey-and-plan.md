# 图形栈体验:工业界实践调研与落地方案

> 2026-08-07。前置:`2026-08-05-graphics-stack-ecosystem-closure.md`(设计)、
> `2026-08-06-deferred-work-plan.md` §7.6–§7.8(B 线落地实测)。
>
> 那两份文档解决的是"栈能不能自持"。这一份解决的是**用户碰不碰得到**:
> 22 个包已经装得上、NVIDIA 闭源已经打通、空 host 判据已经全绿,而
> **索引里没有任何一个包依赖 `mesa`**,发现层的环境变量**只在 `subos use`
> 子 shell 里存在**。栈是好的,接线是断的。
>
> 约束(明确):能在包侧(recipe / `libs/*.lua`)做的不动 xlings 本体;
> 需要 libxpkg 的单列;只有根本问题或 bug 才改 xlings 本体。本文按这个分组。

---

## 0. 结论先行

调研六个生态之后,得到的核心判断只有一条:

> **图形栈的配置必须绑在"对象"或"程序"上,不能绑在"shell"上。**
> 六个生态里没有一个把驱动发现挂在登录 shell 上;xlings 目前**只**挂在那里。

由此得到的方案里,**9 项纯 recipe 改动**、1 项 libxpkg、**1 行 xlings 本体**
(`/dev/dxg`,WSL2 沙箱唯一必需的,见 §C1)。
最贵的两项(Intel iris、Vulkan)是构建工作,不是设计工作 —— 而且调研推翻了
closure §4.2 里"iris 需要 libclc"的判断:`intel-clc` 在 mesa 里**默认 disabled**,
只为光追所需。

核实这些机制的过程中额外撞到两件事,都写在下面:一个**不合规的 `files` 目标会静默
不放置**(§C,本体 bug 候选,独立开 issue),以及 closure 里三处需要按实测订正的
旧结论(§5)。

---

## 1. 调研:六个生态怎么解同一个问题

问题都是同一个:**应用来自包管理器,驱动必须来自宿主(与内核模块锁步)**。

| 生态 | 驱动来源 | 让应用找到驱动的机制 | 作用域 |
|---|---|---|---|
| **Steam / pressure-vessel** | 宿主。`capsule-capture-libs` 按 SONAME 抓取宿主 GL/Vulkan 驱动及其闭包,落到容器内 `/overrides`,宿主文件另挂 `/run/host` | 容器内固定路径 + 生成的 `ld.so.conf` | **容器** |
| **NixOS** | 宿主(`hardware.graphics` 模块填) | **`/run/opengl-driver/lib` 符号链接农场** —— 一个故意不纯的固定路径,nixpkgs 里所有 GL 包都编译期指向它 | **固定路径(全局)** |
| **nixGL** | 宿主,构建期探测版本 | 生成 wrapper,注入 `LD_LIBRARY_PATH` | **进程 + 所有子进程** |
| **nix-system-graphics** | 宿主 | 在**非 NixOS** 上也把 `/run/opengl-driver` 建出来,回到 NixOS 的路子 | **固定路径** |
| **Snap `gpu-2404`** | 宿主(`mesa-2404` content snap,同时支持宿主 deb 装的 NVIDIA) | `command-chain:` 里的 wrapper 脚本,启动时组装库路径与环境变量 | **单个程序** |
| **Flatpak** | 打包(`org.freedesktop.Platform.GL.nvidia-<ver>`,每个驱动版本一个 extension) | 沙箱内 `%{lib}/GL`;`enable-if: active-gl-driver`,驱动版本从 `/sys/module/nvidia/version` 读 | **沙箱** |
| **conda / rattler** | 不管驱动 | `__cuda` / `__glibc` **虚拟包**注入 solver,依赖图按宿主能力自动裁剪 | **依赖图** |

### 1.1 三条共识

1. **驱动必须借宿主,而且这不是将就。** 六个里五个借;唯一不借的 Flatpak 的代价是
   *每个驱动版本发一份 extension*。closure §6.2 的结论与工业界一致,不需要重新论证。
2. **让应用找到驱动的正解是"稳定的间接层",不是"环境变量"。** NixOS 的
   `/run/opengl-driver`、pressure-vessel 的 `/overrides`、Snap 的 `$SNAP/gpu-2404`
   都是同一个东西:**一个名字不随版本变化的目录,内容由安装期填**。
3. **需要环境变量的时候,把它绑在程序上。** Snap 用 `command-chain` wrapper,
   Nix 用 `makeWrapper`。没有人写进 `~/.bashrc`。

### 1.2 一条反共识,而且我们已经踩过

`nix-system-graphics` 的 README 用来解释"为什么不用 nixGL"的那句话,与 xlings
2026-08-05 那次崩溃是同一件事:

> "If your application running via nixGL calls another application, that second
> application also needs to support nixGL's specific versions of those graphics
> libraries, as these get propagated down through the environment variables."
> —— 结果是系统程序 "crash or behave unpredictably"。

xlings 这边的同一句话写在 `elfpatch.lua:1434`:

> "That is how `xlings subos use` once returned a /bin/bash that died of SIGSEGV
> before printing a character."

**两个生态独立得出同一个结论。** interposer(对象级 RPATH)这个选择因此不是权宜,
它比工业界主流(农场 + wrapper)还要窄一档 —— 这是 xlings 现在**领先**的地方,
应该守住并推广到发现层。

---

## 2. 判据:作用域越窄越好

把上面翻译成一条可以逐条检查的规则。一个配置有三种可能的作用域,能用窄的就不许用宽的:

| # | 作用域 | 机制 | 泄漏给谁 | xlings 现状 |
|---|---|---|---|---|
| S1 | **对象** | `DT_RPATH` / interposer | 只有这条载入链 | ✅ 库解析已经全用这个 |
| S2 | **程序** | `xvm.add{ envs = ... }` shim 注入 | 只有这个程序及其子进程 | ⚠️ 机制齐备,**图形栈一个都没用** |
| S3 | **shell** | `subos.env{}` | subos 里每一个进程,**包括跑在宿主 loader 上的宿主二进制** | ❌ 图形栈**只**用了这个 |

**S3 对图形栈是错的作用域**,理由不是洁癖:

- `LIBGL_DRIVERS_PATH` 指向我们的 `lib/dri`。subos 里跑一个**宿主**的 `glxinfo`,
  它会 dlopen 我们的 `swrast_dri.so`,而那个模块 `NEEDED libgallium-25.0.7.so` ——
  宿主 loader 找不到,宿主 GL 于是坏在一个与它无关的地方。这与
  `LD_LIBRARY_PATH` 的 `/bin/bash` SIGSEGV 是同一形状,只是轻一档。
- 反方向更要命:**S3 出不了 subos**。`profile_resources.cppm` v10 的 profile 片段
  只写 `XLINGS_HOME` / `XLINGS_BIN` / `PATH` / `PS1`;`subos_env_for_()` 全仓只有
  两个调用点(`subos.cppm:995` 的 `apply_subos_env_` 和 `--shell` 发射器)。
  所以 closure §7 的终局形态
  ```
  xlings install godot
  godot          # 直接出图
  ```
  在当前实现下**第二行拿不到发现层**。

### 2.1 S2 已经存在,而且比我预期的完整

`xvm.add{ envs = {...} }`(`libxpkg/xvm.lua:24,48`)不是新东西:索引里
`glibc / gcc / git / jdk-* / brew / musl-gcc / claude / virtualbox` 共 10 个 recipe
在用。shim 侧(`xvm/shim.cppm:398 setup_envs`)做了三件正好对得上的事:

1. **列表值前插 + 去重**(`merge_shim_env_value`)—— 三个图形变量都是 `:` 分隔列表,
   语义正确;而且这个去重函数是 #378 那次 `GIT_SSL_CAINFO="x:x"` 的修复,已经被真实
   bug 打磨过。
2. **`${XLINGS_HOME}` 与 `${XLINGS_DYNAMIC_SUBOS_DIR}` 占位符展开**
   (`xvm/db.cppm:624,642`)。
3. **`normalize_subos_paths` 在 dispatch 时把安装期写死的 subos 路径重指到当前
   活动 subos** —— 这正好治了 interposer RPATH 里 `subos/default/lib` 写死的那个毛病
   的同类问题。

**结论:发现层从 S3 搬到 S2 是纯 recipe 改动,零核心改动,而且用的是已经被验证过的路径。**

---

## 3. 方案

### A. 纯 recipe / `libs/*.lua`(不动 xlings,不动 libxpkg)

#### A1 —— 共享 vendor 目录:P8,一次改完双栈优先级

**现状**:`mesa` 与 `nvidia-gl-host-link` 各自 `prepend` 自己载荷里的
`share/glvnd/egl_vendor.d`。实测 manifest:

```json
"mesa@25.0.7.1":               [{"op":"prepend","var":"__EGL_VENDOR_LIBRARY_DIRS", ...}],
"nvidia-gl-host-link@0.1.0":   [{"op":"prepend","var":"__EGL_VENDOR_LIBRARY_DIRS", ...}]
```

**libglvnd 的真实语义**(`src/EGL/libeglvendor.c`):目录**按列表顺序**扫,
每个目录内部 `scandir` + `CompareFilenames`(即 `strcmp`)**分目录**排序。
所以 `10_nvidia.json < 50_mesa.json` 这个宿主约定**只在同一个目录内生效**;
跨目录的优先级 100% 由 `__EGL_VENDOR_LIBRARY_DIRS` 的顺序决定。

xlings 这边的顺序由 `manifest.cppm:400` 按 binding 字符串排序 + resolve 的
"later providers land nearer the front" 决定 —— `mesa@` < `nvidia-gl-host-link@`,
所以 nvidia 在前。**恰好是想要的,但它取决于包名字母序。** 把包改名、或加入第三个
vendor(未来的 `nvidia-open` / `intel` sentinel),这个巧合就没了。

**做法**:两个包都用 `xvm.files{}` 把自己的 JSON 放进 subos 的**同一个**目录,
两边都声明同一个值(值相同,`prepend` 天然去重):

```lua
-- mesa.lua / nvidia-gl-host-link.lua 各自:
xvm.files{ src = "share/glvnd/egl_vendor.d/50_mesa.json",     -- 各自的文件名
           dst = "share/glvnd/egl_vendor.d/50_mesa.json",
           binding = tag }
subos.env{ var = "__EGL_VENDOR_LIBRARY_DIRS", op = "prepend",
           value = "${subosdir}/share/glvnd/egl_vendor.d", binding = tag }
```

优先级回到**文件名**(`10_nvidia` < `50_mesa`),与宿主完全一致 —— 这正是
closure §7.1 说"现在它是必须的"的那件事,而它不需要任何新机制:
`xvm.files` 的 `src`/`dst` 语义(payload 相对 → subos 相对)本来就是为这个存在的,
`sysroot.declare_libs` 就建在它上面。

**判据**:装完两个包,`ls <subos>/share/glvnd/egl_vendor.d` 有两个文件;
`__EGL_VENDOR_LIBRARY_DIRS` 只有一个目录;删掉 nvidia 包后只剩一个文件。

**已核实的两条前置**(不核实就会写出静默不生效的 recipe):

1. **目标目录必须以 `usr` / `etc` / `share` 开头。** `is_permitted_file_destination`
   (`xvm/bindings.cppm:688`)只放行这三个首段,`lib/` 被排除(那是 shim 的地盘的邻居,
   注释里写的是 `bin/` 归 shim)。**不合规的目标不报错,直接不放置** ——
   "A recipe that trips this gets no placement rather than a surprising one"。
   `share/glvnd/...` 合规。
2. **放置是符号链接,不是移动。** `place_asset`(`xvm/commands.cppm:189`)在 POSIX 上走
   `fs::create_symlink`,staging + `rename(2)` 只是为了原子替换;载荷不动,
   而且 `fs::equivalent` 短路使重复放置是幂等的 stat。两个包放**不同文件名**
   (它们本来就是 `10_nvidia.json` / `50_mesa.json`)。

#### A2 —— 发现层挂到程序上:`libs/graphics.lua` + `xvm.add{envs}`

**做法**:新增索引共享模块 `libs/graphics.lua`(→ `import("xim.pkgindex.graphics")`,
解析规则见 `libxpkg/src/xpkg-executor.cppm:1016`,`<index>/libs/<name>.lua`):

```lua
-- 一处定义,所有图形消费者共用。全部 subos 相对,所以:
--   * 每个消费者拿到的是同一张表,不需要查依赖的载荷路径;
--   * shim 在 dispatch 时把它重指到"当前活动的" subos,而不是安装时那个。
function graphics.consumer_envs()
    return {
        LIBGL_DRIVERS_PATH        = "${XLINGS_DYNAMIC_SUBOS_DIR}/usr/lib/dri",
        __EGL_VENDOR_LIBRARY_DIRS = "${XLINGS_DYNAMIC_SUBOS_DIR}/share/glvnd/egl_vendor.d",
        XDG_DATA_DIRS             = "${XLINGS_DYNAMIC_SUBOS_DIR}/share",
    }
end
```

消费者侧一行:

```lua
xvm.add("godot", { envs = graphics.consumer_envs(), ... })
```

前置:`mesa` 要把 `lib/dri` 放进 subos(现在故意没放,因为
`sysroot.declare_libs` 只取 `lib/*.so*`)。**目标是 `usr/lib/dri`,不是 `lib/dri`** ——
A1 的前置 1:`lib` 不在放行名单里,写 `lib/dri` 会**静默不放置**,而
`LIBGL_DRIVERS_PATH` 指向一个不存在的目录时 mesa 只是安静地回落 llvmpipe,
于是"没生效"与"生效了"输出一致。`usr/` 合规,而且与 `<subos>/usr/include`
(头文件已经在那)是同一套布局:

```lua
xvm.files{ src = "lib/dri", dst = "usr/lib/dri", binding = tag }
```

mesa 是 `lib/dri` 的唯一 owner,而放置是目录符号链接(A1 前置 2),所以整目录声明
安全且幂等。**被否决的备选**:`declare_libs(dir, "lib/dri", ...)` 会把 6 个驱动模块
平铺进 `<subos>/lib` —— 能用,但把非链接目标塞进链接目录,mesa recipe 现在那句
"those are loaded by path ... and are not link targets" 的顾虑是对的。

**顺带收一个好处**:`subos.env{}` 那三条也改成同样的 `${subosdir}` 相对值之后,
**S2 与 S3 的值由构造相同**,而不是靠两处各写一遍碰巧一致。

**判据 —— 已实测,而且结果推翻了这里原本写的那条**(详见 §10.2 第一条):

原本写的是"经 shim 与直接跑载荷二进制,GL_RENDERER 必须**不同**"。实测在这台宿主上
**完全相同**:mesa 25.x 相对自身位置找 dri 模块,EGL vendor 又从宿主自己的
`/usr/share/glvnd/egl_vendor.d` 读到(其中的 bare SONAME 经我们的 RPATH 解析到我们的
interposer)。所以这两个变量在**有自己 glvnd 配置的宿主上不是决定性的**。

因此 A2 的正确判据是**记录而非渲染**:

```
# 1. 消费者的节点确实带上了 subos 相对的三条(这是 S2 成立的定义)
#    → godot 的 envs 记录 ${XLINGS_DYNAMIC_SUBOS_DIR}/{usr/lib/dri,share/...,share}
# 2. subos shell 侧解析出同样的值,且没有残留 ${pkgdir}、没有重复项
# 3. 它成为必需的场景是密闭的那个:空 host 里没有宿主 glvnd 配置可读
```

1 与 2 已通过。3 见 §10.2 第二条 —— 空 host 判据当前是红的,且**不是本次改动引入的**。

**已知边界**(要写进文档,不是缺陷):`.desktop` 启动器的 `Exec=` 必须指向 shim;
用户拿绝对路径直接跑载荷二进制不走 shim,拿不到环境 —— 与 Nix wrapper 同样的边界。

**一个真缺口,S2 覆盖不到,owner 不在包侧**:`mcpp run` 直接 exec 构建产物,不经
xlings shim(见 §8 的 mcpp#352)。对 mcpp 构建的 GUI 程序:
- **库解析这一半自动成立** —— mcpp 已经把依赖的 runtime_dirs 烘进 RUNPATH;
- **发现层这一半不成立** —— `LIBGL_DRIVERS_PATH` / `__EGL_VENDOR_LIBRARY_DIRS`
  是 RPATH 带不了的东西(closure §7 的"配置层"就是为这个存在的),而 shim 不参与。

所以 mcpp 必须在自己的 `run`/启动路径上注入同一张表。**这不是包侧能补的**,
也不该在包侧补(否则每个消费者索引各写一遍)。可行做法:`libs/graphics.lua` 的表
同时以数据形式导出(例如 mesa 载荷里放一个 `share/xlings/graphics-env.json`),
mcpp 读它 —— 一处定义,两种启动器共用。

#### A3 —— `graphics` 元包:一条命令,自动适应宿主

**现状**:closure O3 的开放问题;用户得自己知道要装 `mesa`,还要自己知道宿主有
NVIDIA 时另外装 `nvidia-gl-host-link`。

**做法**:新增 `pkgs/g/graphics.lua`,零载荷,`deps = { runtime = { "xim:mesa",
"xim:nvidia-gl-host-link" } }`。

**不需要任何条件逻辑** —— sentinel 的设计已经把"这台机器没有 NVIDIA"当作正常状态
(`install()` 什么都不链、返回 true、打印一句 warn)。所以同一条
`xlings install graphics` 在四类宿主上分别得到:

| 宿主 | 结果 | 靠谁 |
|---|---|---|
| NVIDIA 闭源 | 四入口点 interpose,GPU 渲染 | `nvidia-gl-host-link` |
| AMD | radeonsi | mesa 自带 |
| Intel | **目前 llvmpipe** → A5 后 iris | mesa 自带(待重建) |
| **WSL2** | **目前 llvmpipe** → A9 后 d3d12 | `wsl-gl-host-link` |
| 无 GPU / 容器 / 空 host | llvmpipe(已验证 S1–S4) | mesa 自带 |

三个 sentinel(mesa 自身 + nvidia + wsl)按宿主状态**互斥地**生效,一条命令覆盖五类环境,
**没有一处条件语法** —— 因为"这台机器没有这个东西"在 sentinel 模式里是正常返回值,
不是分支。

这就是 conda `__cuda` 虚拟包在做的事(依赖图按宿主能力裁剪),用 sentinel 模式
在包侧实现,**不需要 solver 改动**。

**判据**:`xlings install graphics` 在本机(RTX 4080)之后 `verify-host-link.sh`
12/12;`XLINGS_GFX_SUBOS` 里 `selfcontained-check.sh` S1–S4 全绿。

#### A4 —— Vulkan:补一个 `vulkan-loader`,发现层零新增

**现状**:`mesa` 载荷里有 `lib/libvulkan_radeon.so` 和路径已改好的
`share/vulkan/icd.d/radeon_icd.x86_64.json`,但**索引里没有 `vulkan-loader`**
(没有 `libvulkan.so.1`),全索引也**没有任何地方声明 `VK_DRIVER_FILES`**。
连带 `zink`(GL over Vulkan)也是死的。

**调研得到的关键事实**:Vulkan loader 的驱动发现走
**`$XDG_DATA_DIRS/vulkan/icd.d`**(`Vulkan-Loader/docs/LoaderDriverInterface.md`)。
而 `mesa` 的 `config()` **已经** `prepend` 了 `XDG_DATA_DIRS = ${pkgdir}/share`。

**所以**:补一个 `vulkan-loader` 包(纯 CMake,只依赖 libc/libm,不需要 glslang ——
那个是 mesa 构建期要的)之后,**Vulkan ICD 的发现不需要任何新声明**。
closure §7 把 `VK_DRIVER_FILES` 写进了目标形态,那是多余的:`VK_DRIVER_FILES` 是
*override*(它会屏蔽系统发现),不是我们要的。

**判据**:`vulkaninfo --summary` 在 subos 内列出 `RADV`;AMD 机器上跑
`vkcube` 出图。本机(NVIDIA)判据不同:`libGLX_nvidia.so.0` 同时是 Vulkan ICD,
所以本机应看到 NVIDIA 的 ICD —— 前提是 sentinel 也把宿主的
`/usr/share/vulkan/icd.d/nvidia_icd.json` 处理掉(它内含 SONAME,与
`10_nvidia.json` 同样的问题;**这是 A4 的第二半,不要漏**)。

#### A5 —— Intel iris:调研推翻了"需要 libclc"

**现状**:载荷 `lib/dri/` 实测只有
`swrast / kms_swrast / libdril / nouveau / radeonsi / zink`。**没有 iris**。
Intel 是 Linux 上最常见的 GPU,现在静默落 llvmpipe。closure §4.2 记的理由是
"iris 和 anv 需要 libclc,而 libclc 需要 clang 和 SPIR-V translator"。

**调研结果**:mesa 的 `meson_options.txt` 里 `intel-clc` 是
`combo{enabled,disabled,system}`,**默认 `disabled`**;libclc 只有在
`-Dintel-clc=enabled`(为 Intel **光追**)时才是构建+运行依赖。
也就是说 `-Dgallium-drivers=...,iris,crocus` 很可能只是**加两个构建 flag**,
`anv` 同理(代价是 Gen12.5+ 没光追)。

**做法**:mesa 重建为 `25.0.7.2`,`-Dgallium-drivers` 加 `iris,crocus`、
`-Dvulkan-drivers` 加 `intel`,`intel-clc` 保持默认。

**判据**:**先跑构建,再改文档**。构建成功且 `lib/dri/iris_dri.so` 存在 →
closure §4.2 那句话订正。构建失败 → 记下真实的失败原因(可能是 `intel_clc`
在某些子特性上非可选),那句话也要订正成真实理由。**两个结果都要求改文档**,
因为现在那句话的依据是没验证过的。

无 Intel 硬件时的替代判据:`iris_dri.so` 存在 + `patchelf --print-needed` 闭包
在 `<subos>/lib` 内全部可解析 + `MESA_LOADER_DRIVER_OVERRIDE=iris` 在无 Intel 机器上
报的是"设备不匹配"而不是"模块加载失败"。

#### A6 —— sentinel 的驱动版本 stamp 与自检入口

**现状**:sentinel 安装期适应宿主,之后**永不复查**。日志里那个 `4/4` 是当时那个驱动
算出来的。宿主驱动 550→560 之后:载荷里 `*.550.144.03` 那批符号链接**全部悬空**。

**推理**(未实测,要标明):interposer 的绝对 `DT_NEEDED` 指向无版本名
`libEGL_nvidia.so.0`,仍有效;vendor 自己的私有库按 bare SONAME 找,载荷里那批
悬空链接名字不对因此不命中,最后落到宿主 `/etc/ld.so.cache` —— **在普通 subos 里大概
还能用,在 bwrap 空 host / 沙箱里(无 `/etc`)就不行**。

**做法**(全部包侧,借 Flatpak 的 `active-gl-driver` 思路):

1. `install()` 把探到的驱动版本写成 stamp:`<install_dir>/.host-driver-version`
   (来源:`/sys/module/nvidia/version`,与 Flatpak 同源;比文件名解析可靠)。
2. sentinel 注册一个 `program` 节点 `xlings-gl-doctor`(载荷里的 sh 脚本,
   `xvm.add{ type="program" }`),做三件事:比对 stamp 与当前
   `/sys/module/nvidia/version`、列出载荷里的悬空链接、报 interposer 分数。
3. 日志里那句 `interposers: 4/4` 后面带上驱动版本。

这样"驱动升级了"从**不可见**变成**跑一条命令可见**,而且不需要 doctor 改动。

**判据**:改 stamp 内容为一个假版本,`xlings-gl-doctor` 必须非零退出并指名
`xlings install graphics` 作为修复动作。

#### A7 —— `godot` 接进新栈,并订正它的注释

**现状**:`godot.lua:257,295` 仍在探测宿主 GUI 目录(`/lib/x86_64-linux-gnu` 等)
并 `patchelf` 进自己的 RPATH;`deps.runtime` 只有 `glibc/freetype/expat`;
`godot.lua:112` 还写着:

> "Shipping a full X11/mesa/wayland stack via xim is infeasible"

**这句话已经被 22 个包反证了**,而注释和 deps 都没动。这就是 closure P9
(真实图形程序端到端)没做的全部内容 —— 旗舰 GUI 包一个字节都没用上新栈。

**做法**:`deps.runtime` 加 `xim:graphics`(A3),`envs = graphics.consumer_envs()`
(A2),宿主目录探测**降级为兜底**:仅当 `graphics` 不在闭包里时才走老路
(用 `pkginfo.dep_install_dir("mesa")` 判定,不是猜)。注释改成真实理由。

**判据**:两条路都要测。装了 `graphics` → RPATH **不含** `/usr/lib`,GL 正常;
没装 → 回到今天的行为。第二条不测,就是给现有用户埋一个回归。

---

#### A8 / A9 —— 见 §9

这两项也在包侧,但它们的论证需要发行版视角,所以规格写在那里:

- **A8 `libs/hostlib.lua`**(§9.2):一处实现"像 loader 那样解析一个宿主 SONAME"。
  这条规则现在有四个答案,三个错;#352 的问题 2 是其中一个。
- **A9 WSL2**(§9.5):`d3d12` gallium 驱动 + `wsl-gl-host-link` sentinel。
  借用点是叶子而非载入链的根,**所以不需要 interposer**。

---

### B. 需要 libxpkg(包侧基础库,不是 xlings 本体)

#### B1 —— interposer RPATH:把稳定的农场放到第一位,并断言 vendor 闭包

**现状**(实测本机载荷):

```
.../nvidia-gl-host-link/0.1.0/lib : .../libglvnd/1.7.0/lib : .../libX11/1.8.10/lib
: .../libXext/1.3.6/lib : .../glibc/2.44/lib64 : .../interposer-stub/0.1.0/lib
: <home>/subos/default/lib          ← 安装时那个 subos 的名字,写死在 ELF 里
```

两个问题,严重度不同:

- **末段写死 subos 名。** 从另一个 subos 用这个载荷时,RPATH 指向的是**别的
  subos** 的 lib 农场。前面的载荷段仍然对,所以大概不出错 —— 但它是
  `xvm` 的 `envs`/`alias` 早就规定不许有的东西
  (`db.cppm:600` 那段注释:"a value naming one of them is right for the subos that
  installed the package and wrong for all the others")。**ELF 里没有 dispatch 时机
  可以重写,所以这里只能写占位符解析不了的东西 —— 也就是必须写成一个稳定路径。**
- **前面各段钉死依赖版本目录。** 升级 `libX11` 之后旧段悬空;因为末段是自更新的
  `<subos>/lib` 农场,解析大概会落到那里 —— **这正是 NixOS `/run/opengl-driver`
  的机制,我们已经无意中有了**。

**做法**:让 `elfpatch.host_link_interposer` 的 `libdirs` 默认值里,subos 那一段用
`${XLINGS_DYNAMIC_SUBOS_DIR}` 是不行的(ELF 不展开)。真正的做法是**把顺序反过来**:
把 `<subos>/lib` 放**第一位**,载荷段留在后面作为兜底。理由:

- `<subos>/lib` 是唯一"名字不随版本变"的目录 —— 农场语义,与 NixOS
  `/run/opengl-driver`、pressure-vessel `/overrides`、Snap `$SNAP/gpu-2404` 同构;
- 载荷段留着,是为了 recipe 注释里那个真实顾虑("装进一个没有 libX11 的 subos,
  `<subos>/lib` 会静默缺件")—— 顺序改了之后它是**兜底**而不是**唯一来源**,
  两个顾虑同时满足。

**并且**:`host_link_interposer` 应在写完之后**断言 vendor 的 `DT_NEEDED` 闭包在
最终 RPATH 里逐个可解析**,缺件时报名字。现在的断言只检查 interposer 自己的三个
形状属性(soname / needed / rpath 存在),不检查**它服务的那个 vendor 能不能真的
加载** —— 这正好是本项目那个反复出现的形状:没发生与成功输出一致。

**为什么这条必须在 libxpkg 而不是 recipe**:`libdirs` 是可以从 recipe 传的
(`opt.libdirs`),但文档里明确禁止手写(R7:上一版手写表漏了 libm/libdrm/libgbm/
libgcc_s/libwayland-*)。**顺序策略属于机制,不属于调用点** —— 让每个 recipe 自己
拼一遍就是 R7 复发。

**影响面**:`host_link_interposer` 全索引只有 `nvidia-gl-host-link` 一个调用者
(已核实),所以改默认顺序的爆炸半径是一个包。

---

### C. xlings 本体:一处必改(WSL2 带来的),其余不改

#### C1 —— `subos/gpu.cppm` 缺 `/dev/dxg`(**WSL2 支持的唯一本体改动**)

`passthrough_args()` 绑了 `/dev/nvidiactl`、`/dev/nvidia-uvm`、`/dev/nvidia-modeset`、
`/dev/nvidia0..15`、`/dev/dri`,**没有 `/dev/dxg`**。而 WSL2 的 GPU 就在那个字符设备上
—— `libdxcore.so` 通过 `/dev/dxg` 桥到 Windows 的 DirectX 内核。

所以 `xlings subos use --sandbox --gpu` 在 WSL2 上**拿不到 GPU**,而且是静默的
(`add_dev` 对不存在的节点直接跳过,注释写的是 "--gpu on a host with no GPU is
allowed and just degrades to a /sys-only addition" —— 那句话在 WSL2 上从"没 GPU"变成了
"有 GPU 但我们没绑")。

**改动**:`add_dev("/dev/dxg");` 一行,与 `/dev/dri` 并列。
**为什么这条算根本问题而不是特例**:这个函数的契约是"把宿主的 GPU 字符设备暴露进沙箱",
少一个平台的设备节点是契约没兑现,不是策略选择。**判据**:单测里注入一个
`exists_fn` 只认 `/dev/dxg` 的假宿主,断言 argv 里有它(现有单测已经是这个形状)。

非沙箱路径不受影响(`/dev/dxg` 本来就可见),所以这一行只影响 `--sandbox --gpu`。

#### 其余:调研之后,**这一轮不需要改**

逐条核对过想改的地方,结论都是"包侧已经够了":

| 想改的 | 为什么不改 |
|---|---|
| profile 里注入图形环境 | **这是错的方向**。见 §1.2:全局环境正是 nixGL 的教训。S2 才对 |
| doctor 加图形检查 | A6 的 `xlings-gl-doctor` 在包侧做同一件事,而且能跟着驱动知识一起演进;doctor 里放一个只对一个包成立的规则会制造第 4 对 reporter/repairer 漂移 |
| solver 支持虚拟包 | sentinel 模式(A3)已经等价,零核心改动 |
| 沙箱默认 `--gpu` | 是个真问题,但**不属于图形栈**:它是沙箱契约(见 §6 O3),而且默认打开会把 `/dev/dri` 递给所有沙箱,是个安全决定,不该夹在这一轮里 |

**原本以为需要本体改动的那一项已经核实不需要**:`xvm.files` 放目录是可行的
—— `place_asset` 在 POSIX 上是 `fs::create_symlink`,目录 src 得到一个目录符号链接,
载荷不动,`fs::equivalent` 短路让重复放置幂等(`xvm/commands.cppm:189`)。
所以 A1/A2 不需要 `_tree` 变体。

**但由此发现一个应当在本体修的 bug 候选**:`is_permitted_file_destination` 否决一个
目标时,`file_placement` 返回空,**recipe 侧什么也看不到**。这是本项目那个反复出现的
形状(`project_silent_success_pattern`)的一个新实例:声明照写、安装照样成功、
文件没放。**修法应该是在注册期(而不是放置期)对不合规的 `dst` 报错** ——
注册期是 recipe 还在场、能被点名的唯一时机。这条独立于图形栈,值得单开一个 issue,
不要夹在这一轮里。

---

### D. 明确不做

| 不做 | 理由 |
|---|---|
| 每个驱动版本发一份 NVIDIA 包(Flatpak 模式) | 327 MB × N,且 EULA 禁止再分发。closure §6.2 已定,工业界 6 选 5 同意 |
| `LD_LIBRARY_PATH` 方案回归 | §1.2,两个生态各自实测过它坏在哪 |
| aarch64 图形栈 | 驱动集完全不同(panfrost/freedreno/v3d),是独立一轮,不是这一轮的尾巴 |
| NVK | 需要 Rust + bindgen 进构建链;闭源路已通,收益低 |

---

## 4. 顺序与判据

```
A8 libs/hostlib.lua ─→ 修 compat.glx-runtime / godot / verify-host-link
   (最先,且独立于全部:它是 #352 用户能立刻感知的那一半)

A1 共享 vendor 目录 ──┐            (A2 的值依赖它,且能独立验证)
A2 shim 携带发现环境 ─┴─→ A3 graphics 元包 ──┐
                                             ├─→ A7 godot 接线
A5+A9 mesa 一次重建:iris/crocus/anv + d3d12 ─┘   (A5/A9 是 A7 的前置)
        └─→ A9 wsl-gl-host-link sentinel
                                    │
B1 interposer RPATH 顺序 + vendor 闭包断言   (与 A 并行,爆炸半径 1 个包)
A6 stamp + xlings-gl-doctor                  (与 A 并行,纯新增)
A4 vulkan-loader(含宿主 nvidia_icd 处理)    (最后,独立)
```

**A5 与 A9 合并成一次 mesa 重建。** 两者都是加 gallium 驱动、都要重建载荷、都要走
"四仓发布链",分两次做等于把最贵的一步付两遍。合并后的构建目标:

```
-Dgallium-drivers=swrast,llvmpipe,radeonsi,nouveau,zink,iris,crocus,d3d12
-Dvulkan-drivers=amd,intel            (anv;dozen 视 A4 决定)
-Dintel-clc=disabled                  (默认值,只影响光追)
DirectX-Headers 走 meson subproject fallback
```

**若 iris 因故建不出来,d3d12 仍要单独出一版** —— 两者的用户群不重叠,不能互相拖。

**A1→A2→A3→A7 是一条链,做完它"用户无感"才第一次成立。**

**A5 从"覆盖面"升级为 A7 的前置**(2026-08-07 修订,理由来自 mcpp#352,见 §8):
把一个现在**借宿主 GL 栈**的消费者迁到我们的栈上,在 Intel 机器上是
**硬件加速 → llvmpipe 的退步** —— 因为我们的载荷里没有 `iris`。对 NVIDIA 和 AMD
是持平或改善,对 Intel 是回归。所以顺序不能是"先迁移,再补 Intel"。

若 A5 的构建结果证明 iris 确实需要 libclc(即调研判断错了),那么 A7 必须改成
**按宿主 GPU 分流**:探到 Intel 时保留宿主兜底路径 —— 这也是 A7 判据里"两条路都要测"
那一条存在的理由,不是保守,是那条路还有唯一的用户。

**每一步的差分判据**(不做差分就等于没测,这是本项目反复出现的教训):

| 步 | 差分 |
|---|---|
| A1 | 删掉 nvidia 包后共享目录里少一个文件,且 `__EGL_VENDOR_LIBRARY_DIRS` 仍只有一个目录 |
| A2 | 经 shim 跑 vs 直接跑载荷绝对路径 → GL_RENDERER 必须不同 |
| A3 | 同一条命令在有/无 NVIDIA 的宿主上都成功,且 renderer 不同 |
| A4 | 装 loader 前 `vulkaninfo` 不存在;装后列出 ICD。ICD 的 `library_path` 必须**不含** `/usr/` |
| A5 | `lib/dri/iris_dri.so` 存在,且其 `DT_NEEDED` 闭包在 `<subos>/lib` 内全解析 |
| A6 | 改假 stamp → 自检必须非零退出 |
| B1 | 删掉一个旧版本载荷目录后,GL 仍在 4080 上渲染(证明落到了农场兜底) |
| A7 | 装 graphics → RPATH 不含 `/usr/lib`;不装 → 回到今天行为 |
| A8 | 造一个 `/usr/lib` 放 32 位 stub 的假 sysroot,四个站点必须都拿到 64 位路径 |
| A9 | W1–W5(见 §A9)。W2 是它的 S3:宿主 mesa 也报 D3D12,只有解析路径能分辨 |
| C1 | 假宿主只有 `/dev/dxg` 时,argv 里必须有它 |

**CI**:`selfcontained-check.sh` 不需要 GPU(bwrap + llvmpipe),**应该进 CI**;
`verify-host-link.sh` 需要 NVIDIA,留手工。现在两个都不在任何 workflow 里 ——
这条独立于上面所有项,而且最便宜。

---

## 5. 三处需要订正的旧结论

closure §4.2:"iris 和 anv 需要 libclc,需要 clang 和 SPIR-V translator"。
调研表明 `intel-clc` 默认 `disabled`、只为光追。**这句话在 A5 构建跑完之后必须
按实测结果改写**,无论结果是哪个方向 —— 现在它的依据是推断。

closure §4.2 的 T5 `lm-sensors`:已实测不需要
(`libgallium-25.0.7.so` 的 `DT_NEEDED` 里没有 `libsensors`),可以划掉。

closure §7:`VK_DRIVER_FILES` 不该在目标形态里(A4)。

---

## 6. 开放问题

| # | 问题 | 阻塞谁 |
|---|---|---|
| ~~O1~~ | ~~`xvm.files{src=<目录>}` 的放置语义~~ —— **已核实**:符号链接,目录可用,幂等 | 已解除 |
| O2 | 宿主 `nvidia_icd.json` 里的 SONAME 要不要和 `10_nvidia.json` 同样重写?ICD 的搜索路径与 EGL vendor 不同(`XDG_DATA_DIRS` vs `__EGL_VENDOR_LIBRARY_DIRS`) | A4 后半 |
| O3 | 沙箱里的图形:`/dev/dri` 要显式 `--gpu`,X11 socket / `XAUTHORITY` / `XDG_RUNTIME_DIR` **全仓零处理**。沙箱里的 GUI 程序两头都可能静默降级 | 独立一轮(沙箱契约) |
| O4 | Wayland 路径从没被探过(`libEGL_mesa` 确实 `NEEDED libwayland-client`)。要不要加第三个探针 | 纯 Wayland 桌面的可用性 |
| O5 | AMD / nouveau 无硬件可验。用 `MESA_LOADER_DRIVER_OVERRIDE` + 期望的失败信息做替代判据够不够 | 覆盖面声明的可信度 |
| O6 | `/usr/lib/wsl/lib/*.so` 的实测 glibc floor(§A9 的成立前提)。**需要一台 WSL2 机器**,本机是原生 Linux | A9 是"借用"还是"追版本" |
| O7 | WSL2 上是否也需要处理 Vulkan:`dozen`(`-Dvulkan-drivers=microsoft-experimental`)是 D3D12 上的 Vulkan 实现,与 A4 的 loader 一起才有意义 | A4 + A9 的交叉 |
| O8 | WSLg 的显示端:`XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir`、X socket 由 WSLg 挂入。非沙箱路径继承即可;沙箱路径与 O3 是同一个洞 | O3 |

---

## 7. 一句话

> **驱动借宿主是工业界共识,xlings 已经做对了,而且用 interposer 做得比主流更窄
> 一档。缺的不是驱动层 —— 是发现层还挂在 shell 上(六个生态没有一个这么做),
> 以及索引里没有任何包依赖 `mesa`。把发现层从 S3 搬到 S2、把 vendor JSON 收进一个
> 共享目录、加一个 `graphics` 元包、让 `godot` 依赖它 —— 四步全在包侧,做完
> `xlings install godot && godot` 才第一次成立。**
>
> **换成发行版视角还得到两条:一条规则(库布局有两套都正确的标准,所以只能问
> loader,而这条规则现在有四个答案、三个错),和一个漏掉的平台(WSL2 里 GPU 只经
> `d3d12` gallium 驱动到达,我们没建,所以今天是静默的 llvmpipe)。Intel 与 WSL2
> 合并成一次 mesa 重建:`intel-clc` 默认关闭让 iris 只是两个 flag,
> DirectX-Headers 有 meson subproject fallback 让 d3d12 连新包都可能不需要。
> WSL2 那条借用点是叶子而不是载入链的根,所以它比 NVIDIA 那条简单 ——
> 不需要 interposer,只需要 sentinel 把库放进 `<subos>/lib`。**

---

## 8. 对照真实 case:mcpp#352(Fedora 44,GLFW 窗口不出现)

<https://github.com/mcpp-community/mcpp/issues/352>(2026-08-04,**仍 open**)。
这是本方案唯一的外部检验样本,值得逐条对。issue 里三个叠加问题:

### 8.1 问题 2:`compat.glx-runtime` 链到 32 位库 —— 未修,而且真因比 issue 写的更具体

`mcpp-index/pkgs/c/compat.glx-runtime.lua` 最后一次改动 2026-07-25,在 issue 之前,
之后没动。issue 归因为"按 Debian 布局假设生成",**但候选目录里 `/usr/lib64` 已经在
`/usr/lib` 之前**:

```lua
add("/lib/x86_64-linux-gnu"); add("/usr/lib/x86_64-linux-gnu")
add("/lib64"); add("/usr/lib64"); add("/usr/lib")
...
for _, dir in ipairs(candidate_dirs()) do ... ln -sf "$lib" outdir/$(basename) ...
```

真因是 **`ln -sf` 最后一个赢**:Fedora 上先链了正确的 `/usr/lib64/libGLX.so.0`,
下一轮 `/usr/lib` 又把它覆盖成 32 位那个。`libOpenGL.so.0` 幸存,只因为那台机器的
32 位 glvnd 不带这个文件 —— 所以 issue 里"只有这个是 64 位"不是巧合,是证据。

**而它的自检恰好抓不住**:`required` 只断言 `libGLX.so.0` / `libGL.so.1` **存在**,
不看 ELF class,两个链接都是 32 位时照样通过。silent success 的又一例。

**最小修法(纯 recipe,3 行)**:首次命中即跳过(first-wins),`required` 加一条
ELF class 断言。这条**不依赖本方案**,可以立刻单独修。

### 8.2 问题 3:glibc 2.39 < 宿主 Mesa 要求的 2.43 —— 版本层面已被动缓解,机制层面未解决

`xim-pkgindex` 现在有 glibc **2.44**,而 `llvm` 的依赖是 `xim:glibc@>=2.39`(范围),
所以今天在干净机器上装,拿到的是 2.44 ≥ 2.43,那台 Fedora 44 大概不再复现。

**但这只是赢了这一轮版本竞赛。** issue 自己的判断是对的:宿主 Mesa 只会越来越新,
自带 glibc 不升就必然再断。而且范围化本身正在制造新问题(xim-pkgindex#531 是它的
一个 open 回归)。

### 8.3 方案对它的回答:取消借用,而不是追版本

问题 3 的根因是**借宿主的 Mesa**。图形栈的全部意义就是取消这次借用:mesa 由 xim 提供、
按我们的 glibc 构建。那时进程的 glibc 是我们的、GL 栈是我们的,**宿主 glibc 版本变成
无关变量** —— 不是把 2.39 追到 2.43,是让这个比较不再存在。

空 host bwrap S1–S4 全绿正是这条的证明:**没有 `/usr` 也能渲染,所以宿主是什么发行版、
什么 glibc、`/usr/lib` 是 32 位还是 64 位,全部无关。** 问题 2 因此被**一并删除**:
`compat.glx-runtime` 这个包本身不该存在 —— 它做的正是 closure §11 说的"抄宿主装了什么"。

**为什么借 NVIDIA 安全、借 Mesa 不安全**,这个 case 把判据给出来了,而且它不是
"驱动 vs 非驱动":

| 借谁 | 它的 glibc floor | 结果 |
|---|---|---|
| NVIDIA 闭源 vendor | 实测 max `GLIBC_2.10`(故意针对极老 glibc 构建,closure §6.1) | **安全** —— 任何我们的 glibc 都满足 |
| 宿主 Mesa | 跟着发行版走(#352 那台是 `GLIBC_2.43`) | **必然断** —— 只是时间问题 |

> **能不能借,取决于对方的 glibc floor,不取决于它是不是驱动。**

而且 `nvidia-gl-host-link` 的探测**就是 issue 修复建议 #1 要的东西**,已经实现:
`ldconfig -p` 且带 `x86-64` 过滤,再按 `/usr/lib/x86_64-linux-gnu` → `/usr/lib64`
→ `/usr/lib` 兜底,**first-wins**(`if os.isfile(...) then return d end`)。
同一个坑,新包躲过了,老包没有。

### 8.4 这个 case 暴露的、方案里原本没有的三条

1. **A5 是 A7 的前置,不是后续。** 已按此修订 §4。理由见那里。
2. **`mcpp run` 不走 shim,S2 覆盖不到。** 已补进 A2 的"真缺口"一段。
3. **`verify-host-link.sh` 第 4 步硬编码 `/usr/lib/x86_64-linux-gnu`**,而
   `[[ -e "$p" ]] || continue` —— 在 Fedora 上那条"宿主驱动文件未被改动"的检查会
   **静默跳过并计入 pass**。与 8.1 是同一个"假定 Debian 布局"的 bug,只是发生在
   验证器里,后果更重:它是用来证明别的东西的。**修法与 8.1 相同**:用探测到的
   目录,而不是写死的目录;跳过时必须计入一个显式的 skip 计数并打印。

### 8.5 仍然回答不了它的部分

- **问题 1(GLFW 初始化失败完全静默)**:eui-neo/mcpp 侧,方案不涉及。但它是同一个
  bug class,而且方案对"程序落到 llvmpipe 了"同样只有 A6 一个入口 —— 消费者侧的
  可观测性是这个方案的薄弱处,应当在 A7 里带一条(消费者启动时能报出 renderer)。
- **纯 Wayland 会话**:#352 那台是 Wayland + XWayland(`DISPLAY=:0`),走 X11 路径,
  我们的栈可以;纯 Wayland 从没探过(O4)。

---

## 9. 从发行版视角看:xlings 多出来的三条边界

§1 调研的六个是**打包/容器生态**。但 xlings 是一个**用户态发行版**,同类应该是
Debian / Fedora / Arch / Gentoo。换成这个视角,结论变了:

### 9.1 发行版根本没有这个问题,因为它拥有 `/usr`

一个发行版的 GL 栈里,glibc 和 Mesa 是**同一次发布、同一个构建系统、同一套
glibc 头**出来的。Debian 的 `libc6` 和 `libgl1-mesa-dri` 来自同一个 suite。
所以 #352 那个 "Mesa 要求 GLIBC_2.43 而我只有 2.39" 在任何发行版里**不可能发生**:
没有两个 glibc,也没有"宿主"。

发行版真正依赖的 ABI 契约只有**一条**:内核 uAPI(DRM ioctl),而那条由
"don't break userspace" 保着。

xlings 既不拥有 `/usr`,也不拥有内核。所以同一件事上,xlings 有**四条**边界:

| 边界 | 发行版怎么处理 | xlings 的处境 |
|---|---|---|
| 内核 uAPI(DRM ioctl) | 依赖它,稳定 | **相同** —— 这条不是问题 |
| 内核模块 ↔ 驱动用户态 | **同一个包版本,内核升级就重建**(DKMS / akmods / `nvidia-dkms`)。包管理器把它变成**安装期不变量** | 不拥有内核 → 只能**运行期探测**(sentinel),漂移不可预防,只能可见化 |
| glibc ↔ GL 栈 | 同一次构建,不存在版本比较 | 自带 glibc → **借来的东西必须 glibc floor 足够低**(§8.3 那张表) |
| 库布局(`lib` / `lib64` / 三元组) | 自己定一套,内部一致 | **必须同时容纳两套互不兼容的标准**(下条) |

**第 2、4 条是发行版用"拥有权"消掉、xlings 只能用机制顶住的**。这就是多出来的工作量的来源,
不是设计没做好。

### 9.2 库布局:两套都"正确"的标准,所以硬编码列表永远错

- **FHS 的 biarch 条款**:`/usr/lib` = 32 位,`/usr/lib64` = 64 位。Red Hat / SUSE 采纳。
- **Debian / Ubuntu 明确拒绝了这一条**,改用 multiarch:`/usr/lib/<triplet>`,
  理由是"为 x86_64 特例化会污染打包工具,收益不成比例"。
- Arch / Gentoo 又是第三种:`/usr/lib64` 是指向 `/usr/lib` 的符号链接,64 位在 `/usr/lib`。

**三种布局,没有一个是错的。** 所以任何"候选目录列表"都不是保守做法,而是**必然在某个
发行版上错**。#352 的问题 2 就是这条的实例:在 Fedora 上 `/usr/lib` 是 32 位目录。

发行版自己怎么找库?**问 loader** —— `ld.so.cache`,由 `ldconfig` 从
`/etc/ld.so.conf.d/*.conf` 生成,而且缓存里**带 ELF class / ABI 标记**
(`ldconfig -p` 输出里的 `libc6,x86-64`)。这是唯一权威的答案,因为它就是 loader
自己要用的那份数据。

**由此得出一条本项目现在有四个答案的规则**(memory: `one-question-many-answerers`):

| 站点 | 现在怎么找宿主库 | 对不对 |
|---|---|---|
| `nvidia-gl-host-link.lua:124` | `ldconfig -p` + `x86-64` 过滤 + FHS 兜底 + **first-wins** | ✅ |
| `mcpp-index compat.glx-runtime.lua` | 硬编码列表 + `ln -sf` **last-wins** + 不查 ELF class | ❌ Fedora 上链到 32 位(#352) |
| `godot.lua:264` | 硬编码列表,收集**所有**命中目录按序上 RPATH,不查 ELF class | ⚠️ 顺序碰巧对(`/usr/lib64` 在 `/usr/lib` 前),但没有判据保证 |
| `verify-host-link.sh` 第 4 步 | 写死 `/usr/lib/x86_64-linux-gnu`,不存在就 `continue` | ❌ Fedora 上静默跳过并计入 pass |

#### A8(新增)—— `libs/hostlib.lua`:一处实现"像 loader 那样解析一个宿主 SONAME"

三条规则,一个函数:**`ldconfig -p` 优先 → 按 ELF class/arch 过滤 → first-wins**,
找不到时按三种布局兜底(而不是当成 `/usr/lib`)。`nvidia-gl-host-link` 的
`__probe_nvidia_dir` 就是它的原型,把它提到 `libs/` 并让另外三个站点调用。

**判据**:在 Fedora 容器(或造一个 `/usr/lib` 放 32 位 stub 的假 sysroot)里,
四个站点必须都拿到 64 位路径;`verify-host-link.sh` 跳过任何检查时必须打印并计入
一个显式的 skip 计数,不能计入 pass。

**这条的价值高于它的体积**:它是 #352 用户唯一能立刻感知的那一半,而且它把一条
规则从"四个地方各写一遍、三个写错"变成一个。

### 9.3 vendor 切换:发行版走过两个时代,xlings 现在在 1.5 期

| 时代 | 机制 | 后果 |
|---|---|---|
| **Era 1(~2016 前)** | **换掉整个 `libGL.so.1`**。Debian:`update-alternatives` / `update-glx` + `dpkg-divert`(`mesa-diverted`);Gentoo:`eselect opengl`;Fedora:`/etc/ld.so.conf.d/` + alternatives | **同一时刻只能有一个 vendor**;混合 GPU 做不到 |
| **Era 2(glvnd,NVIDIA ≥ 361.16,现已全员)** | 一个 dispatch(`libGL.so.1` / `libEGL.so.1` 由 libglvnd 提供)+ per-vendor `libGLX_${VENDOR}.so.0` + **一个共享目录里的 vendor JSON**,文件名定优先级 | Mesa 与第三方驱动**并存且都能用** —— Fedora 的变更页把这条写成了目的;Gentoo 为此**删掉了 eselect-opengl** |

xlings 有 glvnd(Era 2 的库),但 vendor JSON 在**两个目录**里,优先级由 xlings 自己的
binding 字母序决定(§A1)。**这在架构上是 Era 1.5**:拿到了 Era 2 的机制,却用自己的
协议代替了发行版通用的协议。

**A1 因此不只是"消掉一个巧合"** —— 它是"与宿主行为一致"这条目标的**定义**:
用户在宿主上懂的那套优先级(`10_nvidia` 在 `50_mesa` 前),在 subos 里必须还成立。

### 9.4 三个真同类:不拥有 `/usr` 的用户态发行版

§1 里最接近 xlings 的其实不是 Nix,是这三个:

| | 怎么解 GL | 教训 |
|---|---|---|
| **WSLg** | **只借一个 gallium 驱动**。Linux 侧完整拥有 Mesa/libGL;`d3d12` gallium 驱动 + `/usr/lib/wsl/lib` 的 `libd3d12core.so` / `libdxcore.so` 桥到宿主 | **借用点可以比 vendor 更窄** —— 见 9.5,这给 xlings 提了一个真缺口 |
| **conda-forge** | **不发 GL provider**。文档直接叫用户 `apt install libgl1-mesa-dri` / `dnf install mesa-libGL` | 和 `compat.glx-runtime` 同一个政策、同一个失败模式,而且是长期的 support 负担(多个 open issue)。**这条正是 xlings 决定不走的路,而且事实证明是对的** |
| **Termux** | **完全不借**,自带 mesa,走 zink / virgl | 等价于我们的 llvmpipe 路线:没有宿主驱动时的正确形态 |

三个里唯一"借"得成功的是 WSLg,而它借的是**最窄的那一层**。xlings 借 vendor
(`libGLX_nvidia`),而自己拥有 dispatch(`libGL.so.1`)—— **和 WSLg 是同一个架构选择,
只是切在 glvnd 给的那条缝上,因为 NVIDIA 闭源不是 gallium 驱动。** 这条是对
closure §6 的独立验证。

### 9.5 由 WSLg 引出的平台缺口:WSL2 里只有 llvmpipe(已决定本轮支持,规格见 §A9)

实测:mesa 载荷的 `lib/dri` 是
`kms_swrast / libdril / nouveau / radeonsi / swrast / zink`,recipe 与
`tiers.sh` 里**没有任何 `d3d12` 字样**。

在 WSL2 里,宿主 GPU **只能**通过 `d3d12` gallium 驱动 + `/usr/lib/wsl/lib` 到达 ——
没有 `/dev/dri` 上的原生驱动可用。所以:

> **今天在 WSL2 里装 xlings 图形栈,得到的是 llvmpipe,而且没有任何提示。**

这与 Intel 缺 iris 是同一形状(A5),但用户群不同:WSL2 是国内开发者的主流环境之一。
做法与 A5 同类,而且**借用面比 NVIDIA 还小**:mesa 加 `-Dgallium-drivers=...,d3d12`,
再加一个 `wsl-gl-host-link` sentinel 按 `nvidia-gl-host-link` 的模式链
`/usr/lib/wsl/lib` 里的两三个 `libd3d12core.so` / `libdxcore.so`(它们的 glibc floor
需要实测,判据同 §8.3 那张表)。

**已定:本轮支持 WSL2。** 完整规格见下。

#### A9 —— WSL2:`d3d12` gallium 驱动 + `wsl-gl-host-link` sentinel

**为什么它比 NVIDIA 那条简单:借用点是叶子,不是载入链的根。**

| | NVIDIA 闭源 | WSL2 |
|---|---|---|
| 借的是什么 | **glvnd vendor**(`libGLX_nvidia.so.0`)—— 由 glvnd 按名字 dlopen,**自己就是一条载入链的根**,我们无法给它 RPATH | **一个叶子库**(`libd3d12core.so`)—— 由**我们的** `d3d12_dri.so` dlopen |
| 需要 interposer 吗 | **需要**。根没有我们的 RPATH,只能造一个我们拥有的中间对象 | **不需要**。调用方是我们的文件,DT_RPATH 沿载入链向下传递,`dlopen` 由调用方的搜索路径服务 |
| GL 分发层 | 借来的 vendor 与我们的 mesa vendor 并存 → 需要共享 vendor 目录(A1) | d3d12 是 **gallium 驱动**,由 `libGLX_mesa` / `libEGL_mesa` 经 `LIBGL_DRIVERS_PATH` 载入 → **不需要新的 vendor JSON,A1 一个字都不用改** |

**已实测的关键前提**:mesa 载荷里的 dri 模块**带完整 RPATH**,末段是 `<subos>/lib`:

```
radeonsi_dri.so → …/xim-x-mesa/…/lib : …/libllvm/… : … : …/glibc/2.44/lib64 : SUBOS/default/lib
```

所以 sentinel 只要照 `nvidia-gl-host-link` 的做法把 WSL 的库 `sysroot.declare_libs`
进 `<subos>/lib`,`d3d12_dri.so` 对 `libd3d12core.so` 的 bare-SONAME dlopen 就能解析
—— **没有 `LD_LIBRARY_PATH`,没有 interposer**。

**为什么必须由我们放进去、不能靠宿主 ld.so.cache**:`/usr/lib/wsl/lib` 是 WSL 通过
`/etc/ld.so.conf.d/` 让**宿主** loader 看见的,而我们的进程跑在**我们的** glibc 上,
用的是我们自己那份 hermetic ld.so.cache(`godot.lua` 的注释已经记过这一点:
"xim's ld.so.cache is hermetic and does NOT see /lib/x86_64-linux-gnu")。

**要做的四件事**:

1. **mesa 加 d3d12**(与 A5 同一次重建):`-Dgallium-drivers=…,d3d12`。
   构建依赖只有 **DirectX-Headers**,而且 mesa 的 meson 自带 subproject fallback:
   `dependency('DirectX-Headers', fallback: ['DirectX-Headers', 'dep_dxheaders'],
   required: with_gallium_d3d12)` —— 纯头文件、MIT、**可能连新 xim 包都不需要**。
   与 iris 的 libclc 完全不是一个量级。
2. **新包 `wsl-gl-host-link`**(0 payload,照抄 sentinel 模式):
   探到 `/usr/lib/wsl/lib` 就把整个目录的 `.so` 链进载荷并 `declare_libs`;
   探不到就什么都不做并返回 true(非 WSL 宿主上是正常状态)。
   **整套链,不是挑几个** —— 与 NVIDIA 同样的 R7 教训:`libd3d12core.so` 会再
   dlopen `libdxcore.so`,而它是宿主文件、没有我们的 RPATH,只能靠 `<subos>/lib` 命中。
3. **驱动选择**:实测报告 mesa 在 WSL2 上**不总是自动选中 d3d12**
   (wslg#1332 "D3D12 not used unless GALLIUM_DRIVER set")。所以 sentinel 的
   `config()` 在**探到 WSL 时**声明 `GALLIUM_DRIVER=d3d12`(`subos.env` + A2 的
   `xvm.add{envs}` 两处,值一致)。非 WSL 宿主上这个包什么都不声明 —— 这正是
   sentinel 模式的好处:**按宿主分流不需要任何条件语法**。
   *判断*:`set` 会被用户已导出的值让路(UC-1),所以逃逸口天然存在
   (`GALLIUM_DRIVER=llvmpipe` 可以退回软件渲染)。这一条要写进包描述。
4. **`graphics` 元包(A3)deps 加上它**。三个 sentinel(mesa / nvidia / wsl)互斥地
   按宿主状态生效,`xlings install graphics` 一条命令覆盖五类环境。

**必须实测、不能推断的一条**:`/usr/lib/wsl/lib/*.so` 的 **glibc floor**。
微软的文档说这些库"compatible with Ubuntu, Debian, Fedora, CentOS, SUSE and other
**Glibc-based** distributions"(而且有一个 open issue 专门说 musl 不行),
说明它们瞄的是一个低 floor —— 但**这是推断**。判据与 §8.3 那张表相同:

```bash
llvm-readelf -V /usr/lib/wsl/lib/libd3d12core.so | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1
```

floor ≤ 我们的 glibc → 借用安全,A9 成立。floor 高于我们的 glibc → **A9 退化成
"追版本"**,与 #352 同一个陷阱,那时的正确回答是承认它并记录,而不是把 glibc 钉高。

**判据(WSL2 上跑)**:

| # | 断言 | 失败意味着 |
|---|---|---|
| W1 | `glprobe` 的 `GL_RENDERER` 含 `D3D12` 且不含 `llvmpipe` | 驱动没被选中,或库没解析 |
| W2 | `LD_DEBUG=libs` 里 `libd3d12core.so` 的解析路径是 `<subos>/lib/...` | 走的是宿主 loader / 宿主 cache,这次证明不了任何事 |
| W3 | `LD_LIBRARY_PATH` 为空 | 同 `verify-host-link.sh` 第 3 步 |
| W4 | 同一台机器 `GALLIUM_DRIVER=llvmpipe` 时回落软件渲染且像素仍对 | 逃逸口有效 |
| W5 | **非 WSL 宿主**装 `wsl-gl-host-link` 成功、零声明、GL 不受影响 | sentinel 在不适用的宿主上有副作用 |

W2 是这一条的 S3:**"渲染器名字对"在借对了和借错了时可以完全一致**——
WSL2 的宿主 mesa 也会报 D3D12。

**W5 不能省。** 这个包会进 `graphics` 的 deps,也就是**每一台**装图形栈的机器都会装它。

---

## 10. 执行结果(2026-08-07,同日)

四仓落地。**真实验证在一台 RTX 4080 / 驱动 550.144.03 的隔离 home 里跑**,用的是
本次构建的 xlings。逐条列出,包括**三条被实测推翻的**。

### 10.1 已落地并实测通过

| 项 | 证据 |
|---|---|
| **A8** `libs/hostlib.lua` | 10/10 断言,跑在**伪造的 biarch 宿主**上(`/usr/lib` 放 32 位、`/usr/lib64` 放真的),即 #352 的形状。779 静态测试通过 |
| **A1** 共享 vendor 目录 | `<subos>/share/glvnd/egl_vendor.d/` 里**同时**有 `10_nvidia.json` 与 `50_mesa.json`,而 `__EGL_VENDOR_LIBRARY_DIRS` **只有一个目录** |
| **A2** dri 进 subos | `<subos>/usr/lib/dri/` 六个驱动模块齐;`LIBGL_DRIVERS_PATH` 解析为单一 subos 路径 |
| **A2** shim 携带 env | godot 的节点记录了三条 `${XLINGS_DYNAMIC_SUBOS_DIR}` 相对值 |
| **A3** `graphics` 元包 | `xlings install graphics` → 26 个包,报 `NVIDIA proprietary driver 550.144.03 — GL renders on the GPU` |
| **A9r** WSL sentinel(W5) | 非 WSL 宿主上:`not a WSL2 host with D3D12 userspace — nothing to link`,零声明、零影响 |
| **A6** stamp + 自检 | `xlings-gl-doctor` 经 shim 运行,报 `built for 550.144.03 / host now 550.144.03 / 4/4 / ok`。**改假 stamp → 退出码 1 并给出修复命令**(否证方向成立) |
| **A7** godot 上新栈 | 日志 `GL comes from the xlings graphics stack; not adding host library directories`,RPATH 里宿主目录数 = **0** |
| **A8m** mcpp#352 问题 2 | 在伪造 biarch 宿主上,两个 required 链接都落到 `/usr/lib64` 且都是 ELF64;旧的 `ln -sf` 循环在同一 fixture 上落到 `/usr/lib` |
| **B1** interposer | libxpkg 55 个 executor 测试通过,新增一个用「对 vendor 与对产物返回不同 `--print-needed`」的假 patchelf,警告带分数 |
| **C1** `/dev/dxg` | xlings 35 个测试二进制通过,`SubosGpu` 6 个 |
| **核心两处修复** | `SubosManifestEnv` 7 个测试;端到端确认无残留 `${pkgdir}`、无重复项 |
| **verify-host-link** | **12/12**,check 4 现在打印**探测到的**目录(`/lib/x86_64-linux-gnu`),不再是写死的那个 |

### 10.2 被实测推翻的三条

**一、A2 的两个变量在这台宿主上都不是决定性的。**
`glprobe` 同一个二进制,带与不带 `LIBGL_DRIVERS_PATH` / `__EGL_VENDOR_LIBRARY_DIRS`,
**渲染器完全相同**。原因两条:mesa 25.x 相对自身位置找 dri 模块;而 EGL vendor 在
这台宿主上是从**宿主自己的** `/usr/share/glvnd/egl_vendor.d` 读到的,里面的 bare
SONAME 又经我们的 RPATH 解析到**我们的** interposer。

所以 A2 正确的说法**不是**"修好了一个坏掉的场景",而是:把声明从 shell 搬到程序,
使它**在 subos shell 之外也成立**;而它成为必需的场景是密闭的那个(空 host 无
`/usr/share/glvnd`)。§A2 里"没有它就静默落 llvmpipe"这句话对这台宿主不成立,
已在此更正。

**二、空 host 判据 S1 失败 —— 而查下去发现,它现在证明不了它被造出来要证明的事。**

`selfcontained-check.sh` 报 `RESULT=fail:no-display`、`egl error 0x300c`、
`EGL_CLIENT_EXTENSIONS=` 为空(即**一个 vendor 都没载入**)。逐步排除:

| 测的是什么 | 结果 |
|---|---|
| 改动前的 env 形式(mesa 载荷目录) | **失败完全相同** → 与 A1/A2 无关 |
| 容器内 vendor 的 DT_NEEDED 闭包(用我们的 loader `--list`) | **0 个未解析** → 闭包是完整的 |
| 容器内 vendor JSON 与 vendor `.so` 是否可读 | **都可读**,内容正确 |
| **不进任何容器**,直接跑 subos 里编出来的同一个探针 | **`GL_RENDERER=llvmpipe (LLVM 20.1.7)`、`RESULT=ok`**,`EGL_MESA_platform_surfaceless` 在客户端扩展里 |
| 逐个 namespace(不 unshare / net / user / all) | **四种全部同样失败** |
| 额外挂上 `/usr` + `/etc` + `/lib` 符号链接 | **仍然失败** |

也就是说:**栈本身在容器外是好的,而容器内的失败既不来自 namespace,也不来自
"没有 `/usr`"** —— 后者正是这个测试被设计来隔离的那一个变量。

**结论要比"S1 红了"更准确:`selfcontained-check.sh` 当前的失败是无信息的。**
它无法区分"栈不自持"和"探针在 bwrap 里跑不起来",而这正是 S1–S4 存在的全部意义。
所以本轮**既没有重新验证 S1–S4,也不能把这次失败当作反面证据** —— 要先修测试,
再用它判栈。

(顺带踩到并确认了脚本自己注释里那条:`--tmpfs /tmp` 必须排在把一个位于 `/tmp` 下的
home 绑进去之前,否则 tmpfs 会盖掉它。我第一版 bisect 就是这么错的,四种 namespace
全部"无输出",看起来像是崩溃,实际是 home 消失了。)

**三、`>=` 范围无法匹配 mesa 的四段版本号。**
`xim:mesa@>=25.0.7` 与 `@>=25.0.7.1` 都报 **package not found**;
`@25.0.7.1` 与裸 `xim:mesa` 都能解析。其他范围(`@>=0.1`、`@>=1.8`、`@>=2.39`)
全部正常,所以问题只在**四段版本**。mesa 的第四段是本项目自己加的(见那个 recipe),
于是"用下界不要钉死"这条对 mesa **不可能**做到。`graphics` 与 `wsl-gl-host-link`
因此对 mesa 用**裸名**,并在 recipe 里记下测得的原因。

### 10.3 顺带发现的两件事(与图形栈无关)

1. **`files` 资产目标不合规时静默不放置** —— 已在 §C 记为本体 bug 候选。
   本轮踩到:`dst = "lib/dri"` 会安装成功而什么都不放。改成 `usr/lib/dri`。
2. **一个 home 的 `bin/xlings` 缺失时,所有包的 shim 都不生成,而报错指向别处** ——
   本轮先把它误判成"`gcc-specs-config` script 型包的缺陷";实际是我复制测试 home 时
   没带 `bin/xlings`(`slice-real-home.sh` 会带)。补上后 gcc 与
   `xlings-gl-doctor` 的 shim 立刻都出现。**这条是我的误判,记下来是因为报错
   ("installed but registered none of the programs it declares")指向包而不是指向
   home,下一个人会走同一条弯路。**

### 10.4 本轮未做

| 项 | 为什么 |
|---|---|
| **A5+A9b mesa 重建**(iris/crocus/d3d12) | **不是"时间不够",而是构建环境无法从已安装的载荷重建**,见下 |
| **A4 vulkan-loader** | 同上,需要一次构建。顺带实测到一条相关证据:强制 mesa vendor 时 `MESA: error: ZINK: failed to load libvulkan.so.1` —— zink 确实因为没有 loader 而是死的,与 §A4 的判断一致 |
| **发布链** | libxpkg 0.0.54 → mcpp-index → xlings pin → 发布 → xim-pkgindex bump。libxpkg 与 xlings 均已提交并测试通过,尚未 tag |
| **空 host S1–S4 复测** | 见 §10.2 第二条 |

#### 为什么 mesa 重建不能在一个装好图形栈的 home 里做(实测)

装了全部 22 个包的 subos 里,`build-in-subos.sh` 需要的东西**几乎都不在**:

```
PKG_CONFIG_LIBDIR=<subos>/usr/lib/pkgconfig
  expat ok | libdrm zlib libxcb x11 wayland-client libelf 全部 MISSING
<subos>/usr/bin/glslangValidator            MISSING
<subos>/bin/{ninja,python3}                 MISSING
```

原因是结构性的:`sysroot.declare_libs` 只取 `lib/*.so*`,`declare_headers_tree`
只取头文件,**没有任何机制把 `pkgconfig/` 声明进 sysroot**。运行期需要的东西齐了,
构建期需要的 `.pc` 一个都没有。而 `build-in-subos.sh` 故意把 `PKG_CONFIG_LIBDIR`
钉在 subos 上(注释写明:让缺的依赖在 configure 阶段大声失败,而不是被宿主悄悄满足)
—— 所以它现在正确地失败了。

上一轮之所以能构建,是因为那个 `gfxbuild` subos 里每一层都是**由构建脚本自己
`make install` 进 `$SUBOS/usr` 的**,`.pc` 是构建产物的一部分。要重建 mesa 就得
重跑整条 `tiers.sh` T1→T4,那是几小时的连续构建,不是本轮能挤进来的一步。

**这条本身是一个可以修的缺口**:如果 `sysroot` 增加一个 `declare_pkgconfig`
(与 `declare_headers_tree` 同形),一个装了栈的 home 就同时是一个能构建栈的 home。
值得单开。**A9 的 recipe 侧已全部完成并验证**,缺的只是载荷里的
`d3d12_dri.so` / `iris_dri.so`。

---

## 参考

- Steam Runtime / pressure-vessel 容器运行时:<https://gitlab.steamos.cloud/steamrt/steam-runtime-tools>(`/overrides`、`capsule-capture-libs`)
- Fedora 的 glvnd 迁移(目的写得最清楚:并存):<https://fedoraproject.org/wiki/Changes/Vendor_Neutral_libGL>
- Debian multiarch 与 FHS biarch 的分歧:<https://wiki.debian.org/Multiarch/TheCaseForMultiarch>、<https://lwn.net/Articles/844446/>
- Gentoo 因 glvnd 删掉 `eselect-opengl`:<https://fitzcarraldoblog.wordpress.com/2020/08/16/migrating-to-libglvnd-in-gentoo-linux-on-a-laptop-with-nvidia-optimus/>
- WSLg 的 d3d12 gallium 驱动:<https://docs.mesa3d.org/drivers/d3d12.html>、<https://github.com/microsoft/wslg/wiki/GPU-selection-in-WSLg>
- conda-forge 的 GL provider 政策(交给系统包管理器):<https://conda-forge.org/docs/maintainer/maintainer_faq/>
- NixOS OpenGL(`/run/opengl-driver` 符号链接农场):<https://wiki.nixos.org/wiki/OpenGL>
- nix-system-graphics(非 NixOS 上重建农场;nixGL 环境变量传播问题):<https://github.com/soupglasses/nix-system-graphics>
- Canonical `gpu-2404` / `graphics-core22`:<https://canonical.com/mir/docs/the-graphics-core22-snap-interface>、<https://github.com/canonical/gpu-snap>
- Flatpak Extensions(`active-gl-driver`、`/sys/module/nvidia/version`):<https://docs.flatpak.org/en/latest/extension.html>
- conda 虚拟包(`__cuda` / `__glibc`):<https://docs.conda.io/projects/conda/en/latest/user-guide/tasks/manage-virtual.html>
- Vulkan Loader 驱动发现(`XDG_DATA_DIRS/vulkan/icd.d`):<https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md>
- libglvnd EGL vendor 加载顺序:`src/EGL/libeglvendor.c`(<https://github.com/NVIDIA/libglvnd>)
- mesa `intel-clc` 选项默认值:mesa `meson_options.txt`、<https://docs.mesa3d.org>
