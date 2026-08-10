# 完整可用的图形栈应该是什么样,xlings 现在在哪,还差什么

> 基于 2026-08-10 的实测:`.agents/tools/graphics/matrix.sh`(真实建上下文渲染)、
> `graphics-acceptance.sh`(记录与加载器对账),以及为回答"能不能修"做的四组针对性实验。
> 被测机器:NVIDIA RTX 4080 / 550.144.03 / X11 / Ubuntu。客户端 xlings 2026.8.10.3。

## 1. "完整"是什么意思

一个图形栈能被称为完整可用,要同时满足四类要求。它们互不蕴含——今天的测量正是靠把
它们拆开才看清问题的。

| 类别 | 要求 |
|---|---|
| **入口点** | GLX / EGL / GLESv1 / GLESv2 / Vulkan **各自**都能到 GPU。glvnd 为每一个准备独立的 vendor 库,它们是独立的加载链根,会各自失败 |
| **渲染路径** | GPU、CPU(软件)、**无显示离线**三条都可用,且**可被指定** |
| **环境** | 普通 subos、`--sandbox`、`--sandbox --gpu` 三者行为可预期 |
| **开发** | 能在环境内**编译、链接并运行** GL 程序,不只是运行别人构建的 |
| **诚实** | 任何降级都说话。这一条不是锦上添花:这套栈的失败方式就是**成功** |

## 2. xlings 现在在哪

| | 状态 | 证据 |
|---|---|---|
| GLX | ✅ **GPU**,与宿主逐字相同 | subos 与 `--sandbox --gpu` 均为 `NVIDIA GeForce RTX 4080/PCIe/SSE2` |
| Vulkan | ✅ **GPU** | `NVIDIA GeForce RTX 4080` |
| EGL | ❌ CPU llvmpipe | 宿主同一格是 GPU |
| GLESv1 / GLESv2 | ❌ CPU llvmpipe | 同上 |
| CPU 软件渲染 | ✅ 可用 | 但需**同时**指定 vendor 与 `LIBGL_ALWAYS_SOFTWARE`(见 §4.4) |
| 无显示离线 GPU | ❌ 不通 | `egl-device` 在 subos 里 `eglInitialize` 失败 |
| 无显示离线 CPU | ✅ 可用 | EGL surfaceless + 强制 mesa vendor |
| `--sandbox` | ⚠️ 纯软件,**且不声明** | 无 `/dev/nvidia*`;GLX 拿不到上下文,Vulkan 枚举为空 |
| `--sandbox --gpu` | ✅ 恢复 GPU | GLX 与 Vulkan 回到与不带沙箱相同的结果 |
| **构建 GL 程序** | ❌ 链接失败;能链接的也跑不起来 | 见 §4.3 |
| 降级可见性 | ✅ 2026.8.10.3 起 | `xlings subos info` 逐 vendor 报判定 |
| aarch64 | ✅ 诚实失败 | 不再"装一半" |
| macOS / Windows | ⚠️ 不适用,但措辞差 | → #531 |

**一句话**:GLX 和 Vulkan 这两条路是通的且与宿主等价;EGL 家族三个入口点全部静默降级;
环境内开发不可行;沙箱要靠一个不提示的标志。

## 3. 还差四块

| | 缺口 | 性质 |
|---|---|---|
| **A** | EGL / GLESv1 / GLESv2 到不了 GPU | **原因未知**(§4.1) |
| **B** | 无显示 GPU 离线渲染 | 与 A 同源 |
| **C** | 环境内构建不出可运行的 GL 程序 | 原因清楚,方案已验证(§4.3) |
| **D** | 沙箱缺 GPU 时不声明 | 缺一句话,不缺能力(§4.5) |

## 4. 每一块的"为什么"

### 4.1 A —— 不是闭包问题(四次测量)

原先的判定是 `reason=runpath-not-transitive`:interposer 带 DT_RUNPATH,不传递,
背后的宿主驱动够不到我们的载荷。**这个描述是真的**——`dlopen` 确实报
`libpthread.so.0: cannot open shared object file`。

但它**不是这一格红的原因**。三种独立的机械修法,结果完全一致:

| 试法 | egl | gles2 | egl-surfaceless |
|---|---|---|---|
| 现状 | llvmpipe | llvmpipe | **zink over NVIDIA** |
| DT_NEEDED 补三个 glibc stub(不加宽路径) | llvmpipe | llvmpipe | eglInitialize 失败 |
| 窄 RPATH(只含 nvidia 载荷目录) | llvmpipe | llvmpipe | eglInitialize 失败 |
| **本地拷贝真驱动 + 自写 RPATH**(不用 interposer) | llvmpipe | llvmpipe | eglInitialize 失败 |

最后一行是决定性的:那是宿主真驱动的一份本地副本,RPATH 直接指向我们的 glibc 与 nvidia
载荷,**完全绕开 interposer、不依赖任何传递性**。它也不工作。

断掉 mesa 的回落(`__EGL_VENDOR_LIBRARY_FILENAMES` 只留 `10_nvidia.json`),NVIDIA
自己报的是:

```
egl|ERROR|no EGL display
```

`eglGetDisplay(EGL_DEFAULT_DISPLAY)` 返回 `EGL_NO_DISPLAY`。**它被 glvnd 加载了,
然后拒绝提供 display** —— 比 `eglInitialize` 还早一步,和加载器、和搜索路径都无关。

**并且三种修法都有负收益**:它们各自把 surfaceless 从"经 zink 拿到 GPU"打成"彻底失败"。
原因是回落次数:现在 NVIDIA vendor **加载不了**,glvnd 静默落到 mesa,mesa 经 zink 拿到
GPU;修成"能加载"之后 glvnd 就**认定**了它,再失败时**没有第二次回落**。

所以当前的坏状态在 surfaceless 上**优于**半修状态。在查清"为什么拒绝给 display"之前,
不要为了让面板那一行变绿去补闭包。→ #534

### 4.2 为什么 xlings 会遇到别人不遇到的问题

这是最结构性的一条。同一个问题,三种解法:

| | 谁拥有内核模块 | 做法 |
|---|---|---|
| **NixOS** | 自己 | `hardware.nvidia` 用同一份表达式构建内核模块和用户态,放进 `/run/opengl-driver/lib`,所有 GL 程序 RPATH 点它 |
| **Flatpak** | 宿主 | 把匹配版本的用户态驱动做成 runtime 扩展,在**用户机器上**从 NVIDIA 取回并解包 |
| **Docker + nvidia-container-toolkit** | 宿主 | 把宿主驱动库 bind-mount 进容器,再 `ldconfig`,让默认搜索路径找到 |
| **xlings** | 宿主 | **interposer**:一个没有代码的转发壳,冒用 vendor 的 SONAME,用绝对路径 DT_NEEDED 指向宿主真驱动 |

前三种的共同形状是:**给宿主驱动一个它整个闭包都能解析的规范目录**——要么把宿主的世界
搬进来,要么在自己的世界里重建驱动。

xlings 选了第四条,因为前三条的前提它都不满足:

* **不拥有内核模块**(排除 NixOS 路线)。NVIDIA 用户态与内核模块锁步,
  550.144.03 的用户态只跟 550.144.03 的 `nvidia.ko` 对话。
* **INTERP 切换刻意拿掉了宿主回落**(排除 Docker 路线)。整个 subos 跑在我们的 ld.so 下,
  宿主的 `ld.so.cache` 不在链路上——这是自包含性的代价,也是它的定义。
* **EULA 不允许再分发**(排除 Flatpak 的一半)。不过 Flatpak 其实也不再分发,
  它是在用户机器上取;这条今天验过:本地拷贝真驱动同样不工作,所以这个区分暂时不是瓶颈。

代价就是今天测到的:**宿主驱动被弄成一半在我们的世界、一半在宿主的世界。**
GLX 挺过来了,EGL 没有。

**interposer 本身不是 EGL 坏掉的原因**——绕开它结果一样。它是这套约束下的合理选择,
今天的测量既没有推翻它,也没有为它开脱。

### 4.3 C —— 机制长在打包流程里

| | xim 装的程序 | 用户在 subos 里编的 |
|---|---|---|
| INTERP | 我们的 ld.so | **相同** |
| 标签 | **RPATH**(传递) | RUNPATH(不传递) |
| 内容 | 自身载荷 + glibc + **`<subos>/lib`** | glibc + gcc,**没有 `<subos>/lib`** |

farm 把所有载荷的库软链进同一个目录,`<subos>/lib` 那一条就是全部秘密。而**写这条的是
elfpatch —— 一个打包步骤**。用户构建完全绕开它;编译器知道 libc 和 libgcc 是因为它非知道
不可,它对 subos 一无所知。

**根因不是漏了一条路径,是 subos 内的构建从来没有对应的一步。**

实测的修法(两条 flag,缺一不可):

```
-Wl,-rpath,<subos>/lib  -Wl,-rpath-link,<subos>/lib
```

前者管运行,后者管链接期解析输入 `.so` **自己的** DT_NEEDED。常见说法"`-rpath` 会兼作
`-rpath-link`"在这里**不成立**(实测:单给 `-rpath`,`libz` 通过而 `libGL` 仍报
`libGLdispatch.so.0 not found`)。

内容照抄 elfpatch 已经在写的那份(冻结条目在前、farm 兜底),**不新增回答方**。
值得再偷一招:构建产物做闭包校验,不完整就让构建失败(Guix 的 `validate-runpath`),
而不是留到运行时。→ #532

**为什么不改成"把 `<subos>/lib` 放进默认搜索路径"**:我们 ld.so 里烧死的 cache 路径是
`/home/xlings/.xlings_data/.../etc/ld.so.cache`——构建机的 home,用户机器上永远不存在。
要用它,要么在用户机器外造出那个绝对路径(需要 root,而且**全局一份**),要么重编 glibc
换 prefix(能 per-home,**做不到 per-subos**:一份 glibc 载荷服务这个 home 里所有 subos)。
**`<subos>/lib` 是 per-subos 状态,`ld.so.cache` 是 per-glibc-载荷 状态,粒度对不上。**

顺带:NixOS 也没把 store 放进 `ld.so.conf`,那是刻意的——全局 cache 正是环境隔离要消灭的
那种全局可变状态。两边结论一致:**路径长在二进制上,不长在机器上。**

### 4.4 "强制软件渲染"的通行做法在这套栈上无效

`LIBGL_ALWAYS_SOFTWARE=1` **单独使用完全无效**——宿主上加了它仍然返回 NVIDIA。
glvnd 先选 vendor,选中 NVIDIA 之后这个变量属于 mesa,NVIDIA 驱动根本不读。要真的落到
llvmpipe,必须**同时**把 vendor 选成 mesa:

```
__GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1                     # GLX
LIBGL_ALWAYS_SOFTWARE=1 __EGL_VENDOR_LIBRARY_FILENAMES=<…>/50_mesa.json    # EGL
```

机制是好的,坏的是文档/直觉:几乎所有教程只写一半,另一半会静悄悄给你 GPU。
**以为自己在测 CPU 路径的人,测的是 GPU。**

### 4.5 D —— 缺的是一句话

`--gpu` 是有效的:带上它 `/dev/nvidia0`、`/dev/nvidiactl`、`/dev/nvidia-modeset` 回到沙箱,
GLX 与 Vulkan 恢复到与不带沙箱完全相同的 GPU 结果。**缺省不带它也是对的**——设备直通
应当是显式决定。

问题在**沉默**:不带 `--gpu` 时用户拿到"能跑、有画面、exit 0",与 GPU 可用时观感完全相同,
唯一差别是帧率。判据可以直接复用 2026.8.10.3 的接线记录读取方
(`src/core/subos/graphics.cppm`)——那里已经知道这个 subos 有没有图形栈。→ #533

## 5. 到"完整"还有多远

| 缺口 | 距离 |
|---|---|
| **C**(构建) | **近**。方案已验证,是补一个缺失的构建步骤 |
| **D**(沙箱声明) | **近**。判据已存在,缺一句话 |
| **A / B**(EGL 家族 + 离线 GPU) | **未知**。今天把三个看似合理的方案证伪,并把问题从"路径够不着"移到"驱动拒绝初始化"。在有人查清后者之前,方案讨论都在错的层面上 |

**可见性这一块已经完成**(2026.8.10.3)。这很重要:三个月来这套栈的核心风险是
"用坏了看不出来",那一半已经解决——现在它会说自己坏了。剩下的是老老实实的"确实坏了"。
