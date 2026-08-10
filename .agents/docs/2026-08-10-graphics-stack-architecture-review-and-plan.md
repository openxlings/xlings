# xlings 图形栈:架构评审、可用度实测与完善方案

> 状态:**Measured** —— 下面每一个「通过/失败」都来自 2026-08-10 在真实 NVIDIA 550.144.03 /
> X11 `:1` 主机、隔离 home 上的实跑,不是从 recipe 推断的。
>
> 验收工具:`xim-pkgindex/.agents/tools/graphics/verify-stack.sh`、`verify-host-link.sh`
> (本轮起,探针按**真实消费者的默认链接参数**构建)
>
> 关联:#525(已修)、#529、mcpp-community/mcpp#401、`2026-08-10-issue-524-525-root-cause-and-fix-design.md`

---

## 0. 结论先行

**当前状态:在 x86_64 Linux + NVIDIA 专有驱动这一条路径上,栈是可用的**——真实 GUI 程序
(godot)起得来、走 NVIDIA 550.144.03、RPATH 里零个宿主目录。这是 #525 报的那个场景。

**但"可用"目前只在一条路径上被证明过**,而且栈有一处**正在静默降级**:

| 维度 | 判断 |
|---|---|
| 架构 | **健全**。分层清楚、所有权清楚,唯一的结构性缺口是 GLX 侧没有 vendor 发现机制(glvnd 上游就没有),已用「专用目录 + dispatch 自带 RPATH」补上 |
| 可用度 | **14/15 通过**,唯一失败是 EGL 不走我们的 NVIDIA vendor 而**静默退到 zink** |
| 稳定性 | **最弱的一环**。这套栈的失败模式几乎全是「照常渲染,只是换了个人的驱动」,而像素一模一样 |
| 跨平台 | **只有 linux/x86_64**。五个包全部 `archs={"x86_64"}`、只有 `linux` 段。aarch64、macOS、Windows 都没有路径 |

**最该做的三件事**,按收益排序:

1. **把「谁在渲染」变成可断言的产品能力**,而不是只有 `.agents/tools` 里的验收脚本知道(§4.1)
2. **修 EGL 的 host-vendor 闭包**,并把「静默回落到另一个 vendor」变成显式事件(§4.2)
3. **给 aarch64 一条路径**——不是移植 mesa,而是先让栈在 aarch64 上**诚实地不可用**而非装到一半(§4.4)

---

## 1. 架构:四层,以及每层的所有权

```
┌─ 消费者 ────────  godot / imgui+GLFW / 用户程序
│                   只依赖 `xim:graphics`,不认识 vendor
├─ 发现层 ────────  LIBGL_DRIVERS_PATH · __EGL_VENDOR_LIBRARY_DIRS · XDG_DATA_DIRS
│                   (subos 相对,由包 declare,xlings 写进 subos env)
│                   + GLX_VENDOR_SUBDIR  ← 本轮新增,见下
├─ 调度层 ────────  xim:libglvnd —— libGLX/libEGL/libGL/libGLdispatch
│                   一个 home 一份载荷,所有 subos 共享
├─ 驱动层 ────────  xim:mesa(软件 + AMD/Intel/zink)
│                   xim:nvidia-gl-host-link  ┐ 宿主驱动的桥:interposer
│                   xim:wsl-gl-host-link     ┘ (不搬运宿主二进制,只 DT_NEEDED 它)
└─ 装配者 ────────  xim:graphics —— 声明全部依赖 + 把 vendor 接进调度层 + 报告本机走哪条路
```

**这个分层是对的**,理由是它把「一个问题一个回答者」落在了正确的边界上:

- **驱动归宿主,其余全是我们的。** interposer 不复制宿主的 `libGLX_nvidia`,而是 `DT_NEEDED`
  它的绝对路径。宿主驱动必须与 `nvidia.ko` 版本严格配对,搬运它就是在赌两边同时升级。
- **调度层单一。** 一个 home 一份 libglvnd,每个 subos 的 farm 符号链接指向同一个文件。
- **装配由 `graphics` 做,不是由各 vendor 自己注册。** 这条是本轮改出来的:vendor 自注册看起来
  更对称,但 libglvnd 重装会 `os.tryrm` 掉自己的载荷,连带清空所有注册,而 vendor 不会因为
  依赖重装而重装 —— 栈会带着一个空 vendor 目录起来,在 llvmpipe 上渲染并报告成功。
  `graphics` 声明了全部 vendor 和 dispatch,所以它最后安装,能整体重建。

### 1.1 GLX 的结构性缺口,以及它为什么不是设计失误

实测宿主 libglvnd 1.7 的符号表:

```
libEGL.so.1   __EGL_VENDOR_LIBRARY_DIRS · __EGL_VENDOR_LIBRARY_FILENAMES · JSON 里的 library_path
libGLX.so.0   libGLX_%s.so.0 · __GLX_VENDOR_LIBRARY_NAME        ← 一个名字,从来不是路径
```

**上游就没给 GLX 一个「去哪里找」的接口。** EGL 一直是好的,是因为它走了一条 GLX 没有的路——
JSON 里可以写绝对路径。所以 GLX 的 vendor 可达性只能由**执行 dlopen 的那个对象自己的搜索路径**
提供,而那个对象是 `libGLX.so.0`,是我们的文件。

本轮的补法:libglvnd 载荷内一个专用目录(`lib/glx-vendor`),只放 `libGLX_*.so.0` 符号链接,
`libGLX.so.0` 的 RPATH 指向它的**绝对载荷路径**。

- 不用 `$ORIGIN`:glibc 按对象被**打开**的路径展开它,而 libGLX 是通过 subos farm 符号链接加载的,
  `$ORIGIN` 会变成 `<subos>/lib`,指向一个没人创建的目录(实测,LD_DEBUG 原文)。
- 不用 `<subos>/lib` farm:farm 里有 `libc.so.6` / `ld-linux` 符号链接,进 RPATH 是已知地雷。
- 不用 `LD_LIBRARY_PATH`:它是进程全局的,会把我们的载荷灌进宿主二进制。

---

## 2. 可用度:实测矩阵

`verify-stack.sh --subos default`,探针**默认 dtags**(真实消费者的形状):

```
pass 14   fail 1   这台机器覆盖不到 4
```

| 格 | 结果 | 说明 |
|---|---|---|
| 栈装进 subos | ✓ | 37 个包 |
| 发现层三个变量都是 declare 的 | ✓ | 不是硬编码 |
| 一个共享 glvnd vendor 目录 | ✓ | `10_nvidia.json` `50_mesa.json` |
| dri 模块 | ✓ | 8 个 |
| 软件渲染 llvmpipe | ✓ | LLVM 20.1.7 |
| **NVIDIA 专有(interposed)** | **✗** | 1 failed / 11 passed —— 见 §3.1 |
| Vulkan loader + 我们的 ICD | ✓ | |
| X11 / GLX | ✓ | GL_RENDERER = RTX 4080,走**我们的** interposer |
| Wayland | ✓ | EGL on Wayland,7 个对象全是我们的 |
| **真实 GUI 程序(godot)** | ✓ | OpenGL 3.3.0 NVIDIA 550.144.03,**RPATH 里 0 个宿主目录** |
| 空宿主自包含(无 `/usr`) | ✓ | S1–S4 |
| AMD radeonsi / Intel iris / nouveau / WSL2 d3d12 | · | 这台机器没有对应硬件 |

**最有分量的一格是 godot**:它是 #525 报的场景,而且 `app RPATH free of host dirs` 这条判据
比任何 renderer 字符串都硬——renderer 字符串在「我们的栈生效」和「整条链都来自 `/usr/lib`」
两种情况下**完全一样**。

---

## 3. 稳定性:这套栈的失败模式几乎全是「静默降级」

### 3.1 唯一还红着的一格:EGL 退到 zink,而且不说

```
✗ EGL did NOT load our interposer — it used no nvidia vendor at all
✓ EGL rendered (PIXEL=336699)   GL_RENDERER=zink Vulkan 1.3 (RTX 4080)
```

**根因(实测到了)**:宿主 `libEGL_nvidia.so.0` 的 `DT_NEEDED` 是

```
[libpthread.so.0] [librt.so.1] [libc.so.6] [libdl.so.2] [libnvidia-glsi...]
```

前三个是 **glibc 2.34 之前的拆分库**。我们的 glibc 2.44 载荷里有它们(存根),farm 里也有——
但宿主 vendor **自身没有任何搜索路径**,只有**传递的 DT_RPATH** 能覆盖它的闭包。这和 GLX
需要 DT_RPATH 是同一个机制。

对照实测(`verify-host-link.sh`,默认 dtags):

| interposer tag | 结果 |
|---|---|
| 全 RUNPATH(本轮之前) | 5 failed / 7 passed —— EGL 走 llvmpipe,GLX 完全没 vendor |
| 全 RPATH | 2 failed / 10 passed —— GLX 好了,EGL 的 vendor **加载了但 `eglInitialize` 失败** |
| 只有 GLX = RPATH(当前) | 1 failed / 11 passed —— EGL 退到 zink |

**必须说清楚的一点:当前这个「最好」的分数,有一部分是 EGL 静默退化换来的。**
它不是被修好了,是退到了一个在这台机器上恰好可用的 fallback(zink 背后仍是 NVIDIA Vulkan)。
一旦这台机器没有可用的 Vulkan,同样的配置就会掉到 llvmpipe,而分数不变。

**这正是本仓库反复出现的形态**:「没发生」和「成功了」产生相同的输出。

### 3.2 这套栈已经踩过的静默降级,以及各自的守卫

| 形态 | 症状 | 现在的守卫 |
|---|---|---|
| vendor 目录空 | 在 llvmpipe 上渲染并报成功 | `graphics` 整体重建 + `wire_glx_vendors` 返回 0 时告警 |
| RPATH 写入没发生 | 装干净,GL 在软件上跑 | libglvnd/nvidia 写完**断言**,失败即 fail install |
| `dst = "lib/dri"` | 目标被白名单拒绝**不报错**,只是不发生 | `DRI_DIR` 固定在 `usr/` 下并写了注释 |
| vendor JSON 用裸 SONAME | 解析到**宿主的** libEGL_mesa | recipe 改写成绝对路径 |
| 探针加 `--disable-new-dtags` | 每一条 GLX 绿灯都是假的 | 探针改用默认 dtags(本轮) |
| interposer 少一个 entry point | EGL 过而 GLX 从 `/usr/lib` 拉整条闭包 | 四个 entry point 全部 interpose |

**共同点**:每一条都是「照常渲染,只是换了个人的驱动」,而 llvmpipe 和 RTX 4080 画出来的像素一样。
所以这套栈的稳定性投入,**几乎全部应该花在"让降级可见"上,而不是花在"防止降级"上**。

---

## 4. 完善方案

### 4.1 P0 —— 把「谁在渲染」变成产品能力

**问题**:今天唯一能回答「我的 GL 是不是真的走了我的栈」的东西,是 `.agents/tools/graphics/`
里的验收脚本。用户手上没有。而 `glxinfo` 的 renderer 字符串**结构性地不能回答这个问题**。

**做法**:`xlings graphics status`(或 `xlings doctor --scope graphics`),读的是**载荷与状态文件**,
不重新探测:

```
GL dispatch   xim:libglvnd@1.7.0.1        <home>/data/xpkgs/...
GLX vendor    libGLX_nvidia.so.0 -> nvidia-gl-host-link@0.1.2   (host driver 550.144.03)
              libGLX_mesa.so.0   -> mesa@25.0.7.2
EGL vendor    10_nvidia.json  50_mesa.json
渲染路径      NVIDIA 专有(GPU)     ← 与 graphics 安装时打印的那句同源
未接线        (无)
```

**判据**:在一台把 `lib/glx-vendor` 清空的机器上,这条命令必须报「未接线」,而不是照常打印 vendor 列表。

**为什么是 P0**:它把 §3 里六种形态**全部**从"两层之外的怪症状"变成"一条命令说得出的事实"。

### 4.1b 实测修正:安装期告警**不是**一条通道

写 §4.2 的探测时测到的,它改变了 §4.1 的定位:

**config hook 的 log 输出在成功路径上不显示。** 不只是新加的告警——连这个栈原有的
「graphics stack installed / GL renders on the GPU」横幅也不显示。安装什么都不打印,
而状态文件照常写出来了。

所以 §4.1 的 status 读取方**不是锦上添花,是唯一的通道**。任何"在安装时提醒用户"的方案
都建立在一个不成立的前提上。这也反过来确认了「装配者记录、读取方不重新探测」是对的:
记录是唯一活下来的东西。

### 4.2 P0 —— 修 EGL 的 host-vendor 闭包,并让回落变成事件

两步,**顺序不能反**:

1. **先让回落可见。** glvnd 静默吞掉 vendor 的 dlopen 失败。我们控制不了 glvnd,但控制得了
   `graphics` 的 config:它可以在安装时对每个已注册 vendor 做一次 `dlopen` + 入口点 `dlsym`
   探测(GLX 用 `__glx_Main`,EGL 用 `__egl_Main`),**探测失败就报出来**。
   本轮实测证明这个探测有效且便宜:`dlopen FAILED: libpthread.so.0: cannot open shared object file`
   一句话就定位了根因,而 verify-stack 只能说「did NOT load our interposer」。
2. **再修闭包。** EGL 与 GLX 是同一个机制,所以 EGL 的 interposer 也需要 DT_RPATH ——
   但实测那样会让 `eglInitialize` 失败,说明**后面还有第二个缺陷**。
   在第 1 步落地之前不要动 tag:否则又是一次「分数变了,不知道为什么」。

**判据**:EGL 走我们的 NVIDIA vendor 并渲染(`verify-host-link.sh` 12/12);
或者,在确实无法走通的机器上,**明确报告它退到了 zink/llvmpipe**。

### 4.3 P1 —— 让验收矩阵可汇总

`verify-stack.sh` 已经设计成「不同人在不同硬件上跑、把 summary 贴进 issue」。现在缺的是**汇总处**:
四格(AMD / Intel / nouveau / WSL2)在这台机器上永远是灰的,而没有任何地方记录**别人跑出来的结果**。

**做法**:`--json` 输出已经有了;在 xim-pkgindex 建一个 `.agents/docs/graphics-matrix.md`,
每次有人贴 summary 就并进去,注明硬件、驱动版本、日期。**不要做成自动上报**——这是几个人的
生态,一张手工维护的表比一套遥测更可信也更省。

### 4.4 P1 —— aarch64:先诚实,再支持

五个包全部 `archs={"x86_64"}`。今天在 aarch64 上 `xlings install graphics` 的行为是
**依赖解析不到**,而不是一句「这个平台还没有图形栈」。

**做法(便宜且现在就能做)**:给 `graphics` 加一个 aarch64 段,`install()` 里直接 `raise`
并说明现状与替代(用宿主 GL)。**不要**为了"看起来支持"而让它装一半。

**做法(真支持,更大)**:mesa 的 aarch64 构建是可行的(`build-in-subos.sh` 已经是交叉友好的),
真正的阻塞是 vendor 桥:NVIDIA aarch64 (Jetson/Orin) 的驱动布局与 x86_64 不同,
`hostlib.dir_of("libGLX_nvidia.so.0")` 那套探测需要在真机上验。**这一条不该在没有硬件的情况下设计。**

### 4.5 P1 —— macOS / Windows:明确"不适用",而不是"未实现"

glvnd 是 Linux 的机制;macOS 走 Metal/OpenGL framework,Windows 走 WGL/DXGI。
**这不是移植问题,是这套架构不适用。** 应当在文档里写死:

> `xim:graphics` 是 Linux 专有。macOS 与 Windows 上,GL 由系统提供,
> mcpp 的 `compat.*` 直接链系统库。

**判据**:在 macOS 上 `xlings install graphics` 给出这句话,而不是一个解析失败。

### 4.6 P2 —— 收掉两个环境变量

`__EGL_VENDOR_LIBRARY_DIRS` 与 `LIBGL_DRIVERS_PATH` 都是**进程全局**的,xim 自己在安装时就会
警告它们「能把我们的载荷加载进我们不拥有的进程」。GLX 侧本轮已经证明了「专用目录 + 消费者
自带 RPATH」是可行的替代。

**EGL 可以照做**:JSON 里已经是绝对路径,所以 `__EGL_VENDOR_LIBRARY_FILENAMES`(指定文件而非目录)
比 `_DIRS` 更窄;更进一步,如果 libEGL 的 RPATH 能覆盖,连变量都不需要。
`LIBGL_DRIVERS_PATH` 更难——mesa 从环境读它,这是 mesa 的接口,不是我们的选择。

**这条排 P2**:收益是缩小 blast radius,但没有已知的真实事故推动它。

---

## 5. 不建议做的事

- **不要把宿主驱动搬进载荷。** 它必须与 `nvidia.ko` 严格配对,搬运等于赌两边同时升级。
  interposer 的 `DT_NEEDED` 绝对路径是对的。
- **不要用 `LD_LIBRARY_PATH` 补任何可达性。** 进程全局,会杀死在同一棵进程树下的宿主二进制
  (`timeout: symbol lookup error ... __pointer_chk_guard`)。
- **不要给四个 entry point 一视同仁地打 DT_RPATH。** 实测:GLX 需要,EGL 被它害。
  「每个 entry point 都要 interpose」是关于**拦截**的规则,不是关于**打什么 tag** 的规则。
- **不要用 renderer 字符串做判据。** 它在两种相反的情况下输出相同。用 `/proc/self/maps`
  里 GL 对象的路径,或 `app RPATH free of host dirs`。

---

## 6. 优先级汇总

| | 项 | 收益 | 成本 |
|---|---|---|---|
| **P0** | `graphics status`:谁在渲染,一条命令说得出 | 六种静默形态一次性变可见 | 小 |
| **P0** | vendor 入口点探测 + 回落变事件;然后修 EGL 闭包 | 唯一还红的一格;且顺序保证可归因 | 中 |
| P1 | 验收矩阵汇总表 | 四格未覆盖硬件有处可记 | 很小 |
| P1 | aarch64 先诚实报错 | 消除"装一半" | 很小 |
| P1 | macOS/Windows 写死"不适用" | 同上 | 很小 |
| P2 | 收窄 EGL 的发现变量 | 缩小 blast radius | 中 |

**如果只做一件**:P0 的 `graphics status`。这套栈的核心风险不是不能用,是**用坏了看不出来**。
