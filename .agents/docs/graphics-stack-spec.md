# xlings 图形栈:实现与规范

> 规范文档,描述**当前实现**及其契约。与之相对,`2026-08-10-graphics-*.md` 是某一次
> 调研与方案,会过期;本文随实现变更。
>
> 适用平台:Linux / x86_64。macOS 与 Windows 不适用(见 §8)。
> 最后核对:2026-08-10,xlings 2026.8.10.4 / libxpkg 0.0.57。

## 1. 一句话

**GL 程序在 subos 里走的是我们自己的 glvnd 派发和我们自己的 mesa;唯一留在宿主的是
NVIDIA 专有驱动,通过一个无代码的转发壳(interposer)接进来。**

之所以只有这一处例外,有两条无法用打包消除的理由:NVIDIA 用户态与内核模块**锁步**
(550.144.03 的用户态只跟 550.144.03 的 `nvidia.ko` 对话),而我们不拥有内核模块;
以及 NVIDIA 的 EULA **不允许再分发**。

## 2. 分层

```
  应用            任何链接 libGL / libEGL 的程序
    │  DT_NEEDED
  派发层          libglvnd —— libGL.so.1 / libEGL.so.1 / libGLX.so.0 / libGLdispatch.so.0
    │  按 vendor 名 dlopen(四个入口点各自独立)
  vendor 层       libGLX_mesa / libEGL_mesa      ← 我们构建
                  libGLX_nvidia / libEGL_nvidia  ← interposer,转发到宿主
    │
  驱动层          mesa(iris / llvmpipe / zink / d3d12)   ← 我们的载荷
                  宿主 NVIDIA 用户态                        ← 宿主的文件,符号链接
```

**四个入口点是独立的加载链根,会各自失败**:`libGLX_%s.so.0`、`libEGL_%s.so.0`、
`libGLESv1_CM_%s.so.1`、`libGLESv2_%s.so.2`。实测过 GLX 在 GPU 上而 EGL 同时静默退到
zink 的状态。任何只覆盖一个入口点的探测或结论都是不完整的。

## 3. 包图

```
graphics(装配者)
├── xim:mesa                      我们构建的 GL/EGL 实现
├── xim:nvidia-gl-host-link@>=0.1.2   NVIDIA interposer(仅在有 NVIDIA 的宿主上有内容)
├── xim:wsl-gl-host-link@>=0.1        WSL2 的 d3d12 路线
└── xim:libglvnd(经上述各包)         派发层
```

`libglvnd` ← `libX11`、`libXext`、`glibc`、`patchelf`(build)
`nvidia-gl-host-link` ← `libglvnd@>=1.7`、`libX11`、`libXext`、`glibc@>=2.39`、
`interposer-stub`、`patchelf`(build)
`mesa` ← `libllvm@20.1.7`、`libglvnd@>=1.7`、`libdrm`、`libX11`、`libxcb`、`libXext`…

**`graphics` 是唯一的装配者。** 它声明**全部** vendor 和 dispatch,因此严格在它们之后
安装,重跑时从头重建整套接线。这不是风格问题:vendor 目录在 libglvnd 的载荷里,而
libglvnd 重装会 `os.tryrm(install_dir)` —— 若每个 vendor 自行注册,libglvnd 一重装
注册就全没了,vendor 不会因为依赖重装而重装,栈会带着空 vendor 目录起来、退到软件渲染、
并报告成功。**一个写入者,一个时刻,不依赖任何顺序假设。**

## 4. vendor 发现:四条各不相同的路

| 入口点 | 发现方式 | 谁提供 |
|---|---|---|
| **GLX** | `libGLX.so.0` 的 **RPATH** 指向 `<libglvnd载荷>/lib/glx-vendor/`,glvnd 在其中按 `libGLX_%s.so.0` dlopen | `graphics` 的 `wire_glx_vendors` 重建该目录 |
| **EGL** | `__EGL_VENDOR_LIBRARY_DIRS` 指向 `<subos>/share/glvnd/egl_vendor.d/`,JSON 里 `library_path` 是**绝对路径** | 各 vendor 包写自己的 JSON |
| **GLESv1/v2** | 经 glvnd 的 GLES 派发,按 SONAME 解析 | 同 EGL vendor |
| **Vulkan** | ICD JSON(`VK_ICD_FILENAMES` / 默认目录) | 独立于 GL 栈 |

**GLX 的 vendor 目录是重建而非合并的**:`wire_glx_vendors` 每次清空重写。一个被删掉的
vendor 若靠追加就会永远留在那里。

## 5. 加载器契约 —— 本栈最重要的一条规范

### 5.1 标签

> **form-X 可执行文件的搜索路径必须是 DT_RPATH,不能是 DT_RUNPATH。**
> **库保持 DT_RUNPATH。**

DT_RUNPATH 只对携带它的对象生效;**DT_RPATH 对进程内任意深度的 dlopen 都生效**。
GL 程序要穿三到四层 dlopen(glvnd → vendor → EGL 外部平台模块 → 它自己的依赖),
只有传递性标签能到底。

实测:同样的路径内容,DT_RPATH 让四个入口点全部到 GPU(含无显示 surfaceless),
DT_RUNPATH 则是 llvmpipe。

**库不能强制 RPATH**:传递性向下也成立,库的 RPATH 会进入它下方每一次查找;
NVIDIA 的 EGL interposer 被这样打标签后 `eglInitialize` 直接失败(xim-pkgindex#593)。

判据是 **PT_INTERP**,与决定是否 `--set-interpreter` 的是同一个。

### 5.2 谁来打标签

**唯一写入者:`elfpatch`**(libxpkg ≥ 0.0.57,`_rpath_flags`)。
`patchelf --set-rpath` 默认写 DT_RUNPATH;`--force-rpath` 才是 DT_RPATH。

**recipe 不应自己动标签。** 曾经有一个包(godot)必须这么做,因为写入者当时是错的;
写入者修好之后,那种覆盖应当逐步撤除。真要覆盖,必须显式声明,不能靠隐式行为。

### 5.3 谁来验证

**`closure_check` rule E**,在**用户机器上、每次安装时**评估:

> form-X 可执行文件带 DT_RUNPATH → 报出,并说明代价。

warn-only(与 rule A/D 一致)。标签由**原生解析 ELF dynamic section** 读取,因为没有
命令可用:`patchelf --print-rpath` 打印"存在的那个"的值却不说是哪个,`readelf` 不保证
载荷里有。

**读整个 dynamic section,不是第一个命中**:两个标签同时存在时加载器**忽略 DT_RPATH**,
而 DT_RPATH 在前是常见布局。

### 5.4 已知缺口

**用户在 subos 里构建的程序仍带 DT_RUNPATH。** elfpatch 是打包步骤,构建绕开它。
修法已验证但未落地:链接器包装注入
`-Wl,--disable-new-dtags -Wl,-rpath,<subos>/lib -Wl,-rpath-link,<subos>/lib`
(三条缺一不可)。→ openxlings/xlings#532

**不要用 gcc specs 承载这个契约**:它是共享可变状态(实测三个 gcc 载荷三种内容,
一个把 per-subos 路径写死进共享载荷,一个 `-rpath` 里 glibc 出现两次),而且
`-specs=` 是替换不是追加,mcpp 正靠这一点生产宿主链接的产物。

## 6. 状态记录

### 6.1 接线记录 `.wiring`

位置:`<libglvnd载荷>/lib/glx-vendor/.wiring`,由 `graphics` 的 config hook 写。

```
dispatch=<libglvnd 载荷绝对路径>
vendor=libGLX_nvidia.so.0 state=ok
vendor=libEGL_nvidia.so.0 state=broken reason=runpath-not-transitive
vendor=libGLESv2_nvidia.so.2 state=broken missing=libpthread.so.0,librt.so.1
vendor=libGLX_mesa.so.0 state=native
```

| state | 含义 |
|---|---|
| `ok` | 背后有宿主驱动,且它的闭包可达 |
| `native` | 我们自己构建的,背后没有宿主驱动 —— **与 `ok` 是不同的事实** |
| `needs-transitive-consumer` | **已安装程序可用,用户自建的程序不可用**。判定取决于谁打开它:消费者的 DT_RPATH 是传递的,elfpatch 给已安装可执行文件打的正是它;用户构建的产物仍是 DT_RUNPATH(#532) |
| `broken` | 任何消费者都加载失败;`reason=` 或 `missing=` 说明为什么 |
| `unverified` | 安装时无法读取该库(readelf/patchelf 不可用) |

**为什么需要第三种状态**:`vendor_closure_gaps` 判的是 interposer **孤立看**的可达性,
那对 interposer 是对的、对进程不对。libxpkg 0.0.57 之前几乎所有可执行文件都是
DT_RUNPATH,所以 `broken` 事实上准确;之后已安装程序都是 DT_RPATH,再报 `broken` 就是
**低报**。低报比高报隐蔽:它派人去修一个不存在的问题,并掩盖真正坏着的那个。→ #537

**格式是纯 key=value**,因为它跨仓库、由另一种语言读取:**需要解析器的格式就是会解析
失败的格式**。未知的键必须被跳过而不是拒绝 —— 索引独立于客户端发布,新字段一定会到达
旧客户端。

**读取方不得重新探测。** 安装器手里有 readelf/patchelf 并已作答;第二个回答方是这个
仓库产生矛盾的方式。`xlings subos info` 只读不测。

**判定以消费者标签为条件,而格式必须表达这个条件** —— 这正是
`needs-transitive-consumer` 存在的理由(见上表)。graphics 0.1.3 起产出它。

### 6.2 驱动版本戳

`<nvidia载荷>/.host-driver-version` —— 安装时从 `/sys/module/nvidia/version` 记录。
漂移判据:两者都非空**且不相等**。任一侧未知都不是漂移(模块没加载的机器不是换了驱动)。

## 7. 可观测性:唯一的通道

**config hook 的 log 在成功路径上不显示。** 不只是新加的告警,连这个栈原有的
"GL renders on the GPU" 横幅也不显示。实测确认。

所以 `xlings subos info` 是这些判定**唯一**能到达用户的地方:

```
    GL dispatch    <home>/data/xpkgs/xim-x-libglvnd/1.7.0.1
    mesa GLX       ok (built by xlings — no host driver behind it)
  ⚠ nvidia EGL     BROKEN — it carries DT_RUNPATH, which is not transitive, …
    nvidia GLX     ok (the host driver behind it is reachable)
```

**锚点是 `<subos>/lib/libGLX.so.0`** —— GL 程序真正加载的那一份,glvnd 又通过它的 RPATH
找 vendor。所以读取方走的是**加载器同一条边**,只是用 `readlink` 代替 `dlopen`。
必须用 `symlink_status` 而非 `exists`:载荷被删后留下的悬空软链,`exists` 会答"否",
于是一个**接线到已消失载荷**的坏栈被读成"这个 subos 不做图形"。

**四种状态,不是两种**:

| 状态 | 含义 | 显示 |
|---|---|---|
| `NoDispatch` | farm 里没有 libGLX.so.0 | 整节不显示 |
| `NoVendors` | dispatch 在,vendor 目录空 | ⚠ 每个 GL 程序都退到软件渲染 |
| `Unrecorded` | vendor 在,无记录 | ⚠ 旧版 graphics 接的线,没人量过 |
| `Recorded` | 逐 vendor 判定 | 四个入口点各一行 |

沙箱缺 GPU 时另有一句提示,**三个条件**:subos 有 GL dispatch、宿主确实有 GPU 设备节点、
且没传 `--gpu`。窄是必需的——同文件里另一条"每次都响"的提示已因噪音被停用。

## 8. 平台

| 平台 | 状态 |
|---|---|
| Linux x86_64 | 支持 |
| Linux aarch64 | **诚实失败** —— 声明 `archs = {"x86_64"}`,不装一半 |
| macOS / Windows | 不适用:GL 栈是 ELF + glvnd 的,这两个平台的图形栈不是这个形状 |

`archs` + 不声明平台**已经是准确的声明**;`xlings` 对"此包不适用于此平台"的措辞不佳,
那是 xlings 的问题(#531),不是靠在 recipe 里加平台块来解决。

## 9. 环境变量

栈声明的(经 `subos_info.envs`,per-subos):

| 变量 | 声明者 | 用途 |
|---|---|---|
| `__EGL_VENDOR_LIBRARY_DIRS` | mesa、nvidia-gl-host-link | EGL vendor JSON 目录 |
| `LIBGL_DRIVERS_PATH` | mesa、nvidia-gl-host-link | mesa DRI 驱动目录 |

**这类变量会把我们载荷里的代码加载进不属于我们的进程**(包括在宿主加载器下运行的宿主
程序)。**优先在消费者上用 RPATH;只有 RPATH 到不了的地方**(库自己 dlopen 插件)
才用它,并在 recipe 里写明为什么。

### 9.1 强制软件渲染

`LIBGL_ALWAYS_SOFTWARE=1` **单独使用无效**。glvnd 先选 vendor,选中 NVIDIA 之后这个
变量属于 mesa,NVIDIA 驱动根本不读。必须**同时**把 vendor 选成 mesa:

```sh
__GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1                    # GLX
LIBGL_ALWAYS_SOFTWARE=1 __EGL_VENDOR_LIBRARY_FILENAMES=<…>/50_mesa.json   # EGL
```

几乎所有教程只写一半,另一半会静悄悄给你 GPU。

## 10. 验收工具

| 工具 | 回答什么 |
|---|---|
| `.agents/tools/graphics/matrix.sh` | **谁在渲染** —— 真实建上下文,12 格 × 三种 xlings 环境 + 宿主基线 |
| `.agents/tools/graphics-acceptance.sh` | **记录与加载器是否一致** —— 两个方向的分歧都是发现;**两种标签各测一遍**,因为一个 vendor 在 DT_RPATH 下可用、DT_RUNPATH 下不可用,既不是 ok 也不是 broken |
| `xlings-gl-doctor`(nvidia 包) | 驱动版本漂移、interposer 覆盖了几个入口点 |

三条硬规矩,都是这些工具自己踩出来的:

1. **待测集由声明驱动,不由目录列举。** 按 `glx-vendor/` 列举只覆盖 GLX,曾经 6 个只测
   了 2 个**却打印 PASS**。
2. **`not-measured` 一律算失败。** 沉默与同意在结果表里是同一个字符,除非你让它们不同。
3. **探针必须两种标签各编一遍。** 只用默认 dtags 的探针会与记录**共享同一个假设**,
   于是一致地错 —— 这是同一陷阱在这个文件里的第三种形态。
4. **探针按解析出的载荷真实路径 dlopen。** 裸 SONAME 不在搜索路径上(假象与真失败输出
   一样);软链会让 `$ORIGIN` 锚到别处。

## 11. recipe 作者须知

**必须**

- vendor 包写自己的 EGL vendor JSON,`library_path` 用**绝对路径**
- 需要宿主库时,通过 `*-host-link` 包以符号链接引入,并链接**整套**私有依赖
  ——NVIDIA 的私有半边把驱动版本写进 SONAME,只链两个入口点会得到一个"在宿主上能加载、
  在 subos 里失败"的库
- 声明 `programs = {...}`,否则 xlings 不会物化 shim,装后校验也无从读起

**不得**

- 自己动 ELF 标签(§5.2);真要动必须显式声明
- 每个 vendor 自行注册进 GLX vendor 目录(§3)
- 用 `LD_LIBRARY_PATH` 解决查找问题——它被所有子进程继承,包括宿主程序
- 在 recipe 沙箱里用 `os.ln` / `os.files` / `os.arch`(**不存在**,调用会从 hook 内抛出);
  用 `fs.*`

**注意**

- config hook 的 log 不显示(§7);要让用户看见,写进状态文件
- `os.iorun` 失败返回 `""` 而非 nil,且吞掉 stderr——空结果是"工具没跑成"的**唯一**信号,
  不是"结果为空"的证据
- build dep **在 store 里但不在 PATH 上**;用 `pkginfo.build_dep(name).bin`

## 12. 已知缺口

| # | 缺口 | 追踪 |
|---|---|---|
| ~~1~~ | ~~面板低报~~ | **已修**:graphics 0.1.3 + xlings 2026.8.10.5 |
| 2 | subos 内构建的程序带 DT_RUNPATH,拿不到 GPU;链接非 glibc/gcc 载荷的库也跑不起来 | #532 |
| 3 | 无显示 GPU 离线渲染(`egl-device` 在 subos 里 `eglInitialize` 失败) | #534 |
| 4 | 沙箱缺 `--gpu` 时已有提示,但 `--gpu` 仍需用户显式给出 | #533 |

**缺口 1 与缺口 2 是同一个标签的两面。** 实测:同一个 home、同一个 interposer,
只改**消费者**的标签 ——

| 消费者标签 | libEGL_nvidia | libGLESv2_nvidia | 真实渲染 egl / gles2 |
|---|---|---|---|
| DT_RUNPATH | 打不开 | 打不开 | llvmpipe |
| **DT_RPATH** | **LOADED** | **LOADED** | **NVIDIA** |

所以 E1a 之后,**已安装程序的 EGL/GLES 是可用的**;仍然不可用的是**用户自己构建的
程序**(缺口 2)。而 `.wiring` 的判定以消费者标签为条件却没有表达这个条件,于是面板
对已安装程序低报(缺口 1)。

一个报"坏"却其实能用的检查,比高报更隐蔽:它会让人去修一个不存在的问题,并掩盖真正
还坏着的那部分。
