# xlings 图形栈可用性评估:实测矩阵(2026-08-10)

> 工具:`.agents/tools/graphics/matrix.sh` + `probe.c`(真实建上下文渲染,不是查文件)
> 原始输出:`.agents/docs/graphics-matrix-2026-08-10.txt`
> 被测机器:NVIDIA RTX 4080 / 驱动 550.144.03 / X11(DISPLAY=:1)/ Ubuntu
> 客户端:xlings 2026.8.10.3;栈:libglvnd 1.7.0.1 + mesa 25.0.7.2 + nvidia-gl-host-link 0.1.2

## 0. 一句话结论

**GLX 和 Vulkan 走 GPU,与宿主等价;EGL / GLESv1 / GLESv2 全部静默降级到 CPU;
离线 GPU 路径不可用;沙箱内 GLX 完全拿不到上下文;并且用户无法在 subos 里构建 GL 程序。**

12 格 × 3 环境 = 36 格,全部有测量结果(没有一格是"没测到")。

## 1. 矩阵

家族标注是**实际渲染者**,由 `glGetString(GL_RENDERER)` 在一个真实当前上下文里返回。

| 探针 | 覆盖变量 | 宿主(基线) | subos | subos --sandbox |
|---|---|---|---|---|
| glx | — | GPU nvidia | **GPU nvidia** ✅ | **拿不到上下文** ❌ |
| glx | `__GLX_VENDOR_LIBRARY_NAME=nvidia` | GPU nvidia | **GPU nvidia** ✅ | 拿不到上下文 ❌ |
| glx | `…=mesa` + `LIBGL_ALWAYS_SOFTWARE=1` | 拿不到上下文 | 拿不到上下文 | 拿不到上下文 |
| glx | `DISPLAY` unset | 无 X display(**预期**) | 无 X display ✅ | 无 X display ✅ |
| egl | — | GPU nvidia | **CPU llvmpipe** ❌ | CPU llvmpipe |
| egl-surfaceless | `DISPLAY` unset | GPU nvidia | **CPU llvmpipe** ❌ | CPU llvmpipe |
| egl-surfaceless | + `LIBGL_ALWAYS_SOFTWARE=1` | GPU nvidia(**变量无效**) | CPU llvmpipe | CPU llvmpipe |
| egl-surfaceless | + 强制 mesa vendor JSON | **CPU llvmpipe** ✅ | CPU llvmpipe ✅ | CPU llvmpipe ✅ |
| egl-device | `DISPLAY` unset | GPU nvidia | **eglInitialize 失败** ❌ | CPU llvmpipe |
| gles1 | — | GPU nvidia | **CPU llvmpipe** ❌ | CPU llvmpipe |
| gles2 | — | GPU nvidia | **CPU llvmpipe** ❌ | CPU llvmpipe |
| gles2-surfaceless | `DISPLAY` unset | GPU nvidia | **CPU llvmpipe** ❌ | CPU llvmpipe |
| vulkan | — | GPU nvidia | **GPU nvidia** ✅ | **ICD 枚举失败** ❌ |
| **构建 GL 程序** | `gcc -lGL` | (宿主可以) | **链接失败** ❌ | 链接失败 ❌ |

## 2. 分维度评估

### 2.1 GPU 渲染:GLX 与 Vulkan 完好,其余全失

subos 里 GLX 拿到的是 `NVIDIA GeForce RTX 4080/PCIe/SSE2`,**与宿主逐字相同**;Vulkan 拿到
`NVIDIA GeForce RTX 4080`。这两条是这个栈真正做到的部分。

EGL / GLESv1 / GLESv2 三个入口点在 subos 里全部返回 llvmpipe——**宿主上同样三个入口点返回
GPU**。这不是"这台机器没有 GPU",是同一台机器、同一个驱动,只有走我们的栈时降级了。

它与 `xlings subos info` 记录的判定**逐条吻合**:

```
nvidia GLX     ok
nvidia EGL     BROKEN — runpath-not-transitive
nvidia GLESv1  BROKEN — runpath-not-transitive
nvidia GLESv2  BROKEN — runpath-not-transitive
```

也就是说,记录不是猜的,渲染结果证实了它。这是这轮最有价值的一件事:**判定与现实对齐,
所以判定可以被当作事实使用**。

### 2.2 CPU 渲染:可用,但"强制软件"的通行做法在这个栈上是错的

`LIBGL_ALWAYS_SOFTWARE=1` **单独使用完全无效**——宿主上加了它仍然返回 NVIDIA。
原因是结构性的:glvnd 先选 vendor,选中 NVIDIA 之后,这个变量是 mesa 的,NVIDIA 驱动
根本不读它。要真的落到 llvmpipe,必须**同时**把 vendor 选成 mesa:

```
LIBGL_ALWAYS_SOFTWARE=1 __EGL_VENDOR_LIBRARY_FILENAMES=<…>/50_mesa.json   # EGL
__GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1                    # GLX
```

这么做之后 subos 与宿主都稳定返回 `llvmpipe (LLVM 20.1.x)`。**CPU 路径本身是好的**,
坏的是文档/直觉:几乎所有教程只说 `LIBGL_ALWAYS_SOFTWARE=1`,在 glvnd 环境下它会静悄悄
给你 GPU。

GLX 的 CPU 路径在这台机器上拿不到上下文(mesa 的 GLX vendor 对着 NVIDIA 的 X server
`glXCreatePbuffer` 失败)——**宿主上也一样**,所以这是宿主环境属性,不是 xlings 的退化。

### 2.3 离线 / 无显示渲染:能跑,但只有软件

`DISPLAY` 完全 unset 时:

- GLX:三个环境一致地报"无 X display"。**这是正确的**——GLX 本来就没有无显示模式,
  离线故事必须走 EGL。这一格存在的意义就是证明这一点。
- EGL surfaceless:三个环境都能拿到上下文。**宿主是 GPU,subos 是 CPU**。
- EGL device platform(无显示访问 GPU 的正规路线):宿主 GPU;**subos `eglInitialize` 直接失败**。

结论:**xlings 的离线渲染可用,但只能用 CPU**。要在无显示环境下用 GPU 跑离线渲染
(渲染农场、CI 里的 GPU 测试、服务端推理配套的可视化),这个栈目前做不到。

### 2.4 沙箱:GLX 完全不可用,Vulkan 不可用

`--sandbox` 下 GLX 连上下文都建不出来(X 协议 `BadValue` on `X_GLXCreateNewContext`),
Vulkan 的 ICD 枚举返回 0 个设备。EGL 系列仍能跑,但都是 llvmpipe。

bwrap 重建 `/dev` 与挂载命名空间,`/dev/nvidia*` 默认不在里面——`subos use --sandbox --gpu`
存在正是为此。**没有 `--gpu` 时沙箱是纯软件渲染环境**,这本身合理;不合理的是**它不说**:
命令照常成功,程序照常出画面。

### 2.5 开发体验:能跑,不能建

`gcc -lGL` 在 subos 里**链接失败**:

```
warning: libGLdispatch.so.0, needed by <subos>/lib/libGL.so, not found (try using -rpath or -rpath-link)
warning: libGLX.so.0,        needed by <subos>/lib/libGL.so, not found
undefined reference to `__glDispatchInit' …
```

两个库就在同一个目录里。链接器解析 `libGL.so` **自己的** DT_NEEDED 时不搜 `-L`,
也不搜 libGL 的 DT_RUNPATH——**RUNPATH 对 ld 同样不传递**,与 #525 在运行期咬人的
是同一条属性,只是换了个工具。

加 `-Wl,-rpath-link` 能链上,产物却**跑不起来**:subos 工具链只给产物写了
`glibc` 和 `gcc` 两个载荷的 RUNPATH,产物找不到 `libGL.so.1`。

已安装的 GL 程序不受影响,因为 elfpatch 在打包时给它们写过 RPATH。所以这个栈是
**"能运行别人构建的 GL 程序,但用户自己构建不出来"**。

## 3. 按优先级的缺口

| # | 缺口 | 影响 | 备注 |
|---|---|---|---|
| **A** | EGL / GLESv1 / GLESv2 静默降级到 CPU | 任何用 EGL 的程序(现代引擎、Wayland、无显示)拿不到 GPU | 已定位:interposer 带 DT_RUNPATH,不传递。`subos info` 已能报出来 |
| **B** | 无法在 subos 里构建 GL 程序 | GL 开发在 subos 内不可行 | 根因同 A(RUNPATH 不传递)+ 工具链只写两个载荷的 RUNPATH |
| **C** | 离线 GPU 路径(EGL device)不可用 | 无显示环境只能软件渲染 | subos 里 `eglInitialize` 失败 |
| **D** | 沙箱无 GPU 时不声明 | 用户以为在用 GPU,实际全软件 | `--gpu` 存在但缺省静默 |
| **E** | "强制软件渲染"的通行做法在此栈无效 | 用户以为在测 CPU 路径,实际测的是 GPU | 属文档缺口,机制正确 |

A 与 B 同源,修好 A 的传递性大概率同时松动 B。

## 4. 这份评估自己的可信度

三件事保证它不是自我印证:

1. **每格都有结果**,没有"没测到"。`UNMEASURED` 在这个 harness 里被当作失败——第一版
   有两格 `(no output)`,追下去发现是 Xlib 默认错误处理器 `exit()` 把一个真实的
   X 协议错误吃掉了,测量工具自己在隐藏发现。
2. **有宿主基线**。每一格都能和"不经过 xlings"的同一格对照,所以"这台机器就是这样"
   和"经过我们的栈才这样"能分开。
3. **与另一条独立证据交叉**。`.wiring` 记录(安装期 `readelf` 静态判定)和这份矩阵
   (运行期真实渲染)是两套完全不同的方法,结论逐条一致。
