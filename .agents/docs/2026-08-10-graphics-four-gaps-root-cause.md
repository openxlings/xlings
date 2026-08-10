# 四块缺口的根因分析:A/B/C 是同一个,而且它已经被修好过一次

> 深度调研 2026-08-10。方法:受控对照(宿主 vs subos)+ `strace` 差分 + 逐层证伪。
> 机器:NVIDIA RTX 4080 / 550.144.03 / X11 / Ubuntu。客户端 xlings 2026.8.10.3。
> 前置:`2026-08-10-graphics-completeness-gap.md`(该文的 §4.1 结论被本文推翻)。

## 0. 结论

**A(EGL/GLES 到不了 GPU)、B(无显示 GPU 离线渲染)、C 的运行期一半,是同一个根因:**

> `elfpatch` 给载荷打的是 **DT_RUNPATH**,而 DT_RUNPATH **不传递**。
> 图形栈的加载链有三到四层 `dlopen`,只有可执行文件上的 **DT_RPATH** 能穿透到底。

**路径一直是对的,标签一直是错的。** 抽样 73 个已安装可执行文件:

| 标签 | 数量 | 其中含 `<subos>/lib` | 图形结果 |
|---|---|---|---|
| **DT_RPATH** | **1** | 1 | ✅ 拿到 GPU |
| **DT_RUNPATH** | **68** | **55** | ❌ 拿不到 |
| 无 | 4 | — | — |

唯一那个 DT_RPATH 是 **godot**,而 godot 是这套栈里唯一被观察到"图形真的能用"的程序。
它不是巧合:**godot 的 recipe 自己调 `patchelf --force-rpath` 把标签翻了过来**,
并逐字写下了原因——

> `config()` invokes `patchelf --force-rpath` to flip the tag elfpatch stamps in
> (DT_RUNPATH → DT_RPATH) **so godot's dlopen'd libs actually see the patched path**.

**这个机制早就被发现并修好了,只是修在一个 recipe 里,从没上升到 elfpatch。**

## 1. 证据链

五步测量,每一步都把上一步的结论往前推翻或推进一层。

### 1.1 先纠正一个我自己的错误结论

前文 §4.1 写的是"NVIDIA vendor 被加载了,然后拒绝提供 display"。**这个结论站不住**:
那次隔离测试用的是**还原后的原始 interposer**,它本来就 `dlopen` 失败。
"vendor 加载不了"和"vendor 加载了但拒绝服务"在 `eglGetDisplay` 上返回同一个
`EGL_NO_DISPLAY`,我把两者混为一谈了。

正确做法是先让 interposer 可加载,再隔离。做了之后仍然 `no EGL display`,
所以那一步的**现象**是对的,**推理**是错的——现象需要新的解释。

### 1.2 `strace` 差分:宿主 168 行,subos 23 行

受控对照:两边都只留 `10_nvidia.json`(断掉 mesa 回落)。

* 宿主:`egl|NVIDIA Corporation|NVIDIA GeForce RTX 4080/PCIe/SSE2`
* subos:`egl|ERROR|no EGL display`

差分立刻显示 subos 侧**根本没走到加载 NVIDIA EGL 核心那一步**:

```
✅ 我们的 libEGL.so.1(glvnd)      ✅ 读 10_nvidia.json
✅ 我们的 interposer               ✅ libnvidia-glsi(我们的载荷)
✅ 我们的 glibc stub               ✅ 宿主真驱动 /lib/x86_64-linux-gnu/libEGL_nvidia.so.0
   ↓ 宿主驱动接着读 EGL 外部平台配置
✅ /usr/share/egl/egl_external_platform.d/10_nvidia_wayland.json
❌ 找 libnvidia-egl-wayland.so.1 →
     /home/xlings/.xlings_data/xim/xpkgs/fromsource-x-glibc/2.44/lib/…  ENOENT
```

那个路径是**构建机的 home**——我们 ld.so 里烧死的默认搜索路径,用户机器上永不存在。
而 `libnvidia-egl-wayland.so.1` **就在我们的 nvidia 载荷里**。

**关键在层级**:宿主驱动用**裸 SONAME `dlopen`** 加载外部平台模块。这条路不走 DT_NEEDED,
只走搜索路径。此前所有修法(包括我做的三种)都在修 **DT_NEEDED 那一层**,而问题在第二层。

### 1.3 逐层推进,每一层都换一个错误

| 试法 | 结果 | 说明 |
|---|---|---|
| 原状 | `no EGL display`,trace 23 行 | interposer 都加载不了 |
| + DT_NEEDED 补 glibc stub | `no EGL display`,23 行 | 能加载了,但外部平台模块找不到 |
| + 窄 RPATH(只含 nvidia 载荷) | `no EGL display`,30 行 | egl-wayland/gbm/eglcore **找到了**,但 libX11/libXext 丢了(窄过头) |
| + RPATH = nvidia 载荷 **+ farm** | **`eglInitialize failed`**,90 行 | 拿到 display 了;仍有两个模块找不到 |

第四行是转折:错误从"没有 display"变成"初始化失败",trace 从 30 涨到 90(宿主 168)。
仍然找不到的两个是 `libwayland-server.so.0` 和 `libnvidia-gpucomp.so.550.144.03`
——**两者都在我们载荷里**,但搜索**一条 RPATH 都没查**,直奔构建机路径。
因为它们是被 `dlopen` 出来的模块**再**去 `dlopen` 的,每一层 `dlopen` 开启自己的解析作用域。

### 1.4 判定性实验:一个目录对全进程可见,是否就够?

```
LD_LIBRARY_PATH=<farm>:<nvidia载荷>
```

```
egl               NVIDIA GeForce RTX 4080/PCIe/SSE2
gles2             NVIDIA GeForce RTX 4080/PCIe/SSE2
egl-surfaceless   NVIDIA GeForce RTX 4080/PCIe/SSE2   ← 无显示 GPU 离线渲染,即 B
```

**够。** 四个入口点全部到 GPU,B 也一并解决。但 `LD_LIBRARY_PATH` 有污染半径
(被所有子进程继承,包括不属于我们的宿主程序),不是答案,只是判定题的答案。

### 1.5 根因确认:标签,不是路径

同样的内容,只改标签类型,**不设任何环境变量**:

| 可执行文件的标签 | egl | gles2 | egl-surfaceless | glx |
|---|---|---|---|---|
| **DT_RPATH** | **NVIDIA** | **NVIDIA** | **NVIDIA** | **NVIDIA** |
| DT_RUNPATH | `eglInitialize failed` | — | — | — |

**DT_RPATH 在可执行文件上对进程内任意深度的 `dlopen` 都传递;DT_RUNPATH 不传递。**

**并且 interposer 一行都不用改。** 上表那一轮跑的是**还原后的原始 interposer**——
我在 §1.3 对它做的所有 DT_NEEDED / RPATH 修改全部撤销。消费者的 DT_RPATH 是传递的,
它把 interposer 自己的 DT_NEEDED 链也一并解析了。**索引侧零改动。**

现实验证:godot(store 里唯一的 DT_RPATH)自己报告

```
OpenGL API 3.3.0 NVIDIA 550.144.03 - Using Device: NVIDIA - NVIDIA GeForce RTX 4080
```

## 2. 为什么此前所有人(包括我)都找错了层面

**因为 GLX 能用。** GLX 的 vendor 由 `libGLX.so.0` 用裸 SONAME `dlopen`,**只有一层**,
`libGLX.so.0` 自己的 RPATH 就够到了。EGL 的驱动要三到四层。
"GLX 通了所以路径机制是对的"——这个推论在一层深度上成立,在三层深度上不成立。

**并且判定记录写的原因是真的,只是不完整。** `reason=runpath-not-transitive` 描述的
现象(`dlopen` 报 `libpthread.so.0`)真实存在,而且用的词就是"RUNPATH 不传递"——
**它说对了机制,指错了对象**:不是 interposer 的标签,是**消费者可执行文件**的标签。

我自己在这条路上犯了三次:
1. 把"vendor 加载不了"当成"vendor 拒绝服务"(§1.1)
2. 把搜索序列中的失败尝试当成最终失败(有 7 个库其实后来找到了)
3. 用 subos gcc 默认参数编的探针跑整个矩阵——**那个探针带 RUNPATH,而 godot 带 RPATH**,
   所以矩阵里 EGL 那几行反映的是"用户自己编的程序"看到的世界,**不是已安装应用看到的**

第 3 条要求修正前一份评估文档:矩阵**对用户构建的程序是准确的**,但不能当作"这套栈对
应用不可用"的结论。真实分布是 1 : 68。

## 3. 四块缺口的方案

### A + B —— 让 elfpatch 打 DT_RPATH

**做什么**:`elfpatch` 的 `--set-rpath` 加上 `--force-rpath`。**只此一处,索引侧零改动**
——§1.5 的验证用的是未经任何修改的原始 interposer。
patchelf 的 `--set-rpath` 默认写 DT_RUNPATH;`--force-rpath` 才是 DT_RPATH。
今天这一行已经存在于代码里(`elfpatch.lua:1587`),但只在一个特定 helper 上。

**为什么是它而不是别的**:

| 备选 | 为什么不 |
|---|---|
| `LD_LIBRARY_PATH` | 被所有子进程继承,包括宿主程序。污染半径最大 |
| `<subos>/lib` 进 `ld.so.cache` | 粒度对不上:cache 是 per-glibc-载荷,farm 是 per-subos;且我们 ld.so 的 cache 路径是构建机 home,永不存在 |
| 每个 recipe 自己 `--force-rpath` | 已经在做,一个包做对了,68 个没有。**这正是"一个问题多个回答方"** |
| 给 interposer 加标签 | 已证伪。而且不必要:消费者带 DT_RPATH 时,**原封不动的 interposer** 就能工作 |

**取舍(必须先想清楚)**:

* **DT_RPATH 优先级高于 `LD_LIBRARY_PATH`**,而 DT_RUNPATH 低于它。翻转标签会让用户
  **无法再用 `LD_LIBRARY_PATH` 覆盖**我们的库。这是行为变更,不是纯修复。
* **传递性是双向的**:farm 会进入这个进程里**每一次**查找,包括宿主驱动自己往下的查找。
  #590 就是被这一点咬的(`--force-rpath` 曾让 EGL 更糟)。但今天的测量表明,当时的
  问题在**内容**(整个 farm + glibc lib64 放在 interposer 上)而不是标签本身;
  放在**消费者**上、内容为 farm 时,四个入口点全通。
* **blast radius 是整个生态**:每个包重装才会重新打标签。需要一次全量重打或
  按需迁移策略。

**验证方法(可自动化)**:`.agents/tools/graphics/matrix.sh` 增加一行——用 **DT_RPATH**
编译的探针与用 **DT_RUNPATH** 编译的探针各跑一遍,两者不一致就是这个缺陷还在。

### C —— 工具链补一步(已验证)

```
-Wl,--disable-new-dtags -Wl,-rpath,<subos>/lib -Wl,-rpath-link,<subos>/lib
```

三个部分各管一件事,实测**缺一不可**:

| 部分 | 管什么 | 缺了会怎样 |
|---|---|---|
| `-rpath` | 运行期查找 | `libz.so.1: cannot open shared object file` |
| `-rpath-link` | 链接期解析输入 `.so` 自己的 DT_NEEDED | `libGLdispatch.so.0 not found`(常见说法"`-rpath` 会兼作 `-rpath-link`"在这里**不成立**,实测) |
| `--disable-new-dtags` | 写 DT_RPATH 而非 DT_RUNPATH | 用户构建的 GL 程序仍拿不到 GPU(§1.5) |

内容照抄 elfpatch 的既定约定(冻结条目在前、farm 兜底),**不新增回答方**。
值得再加一道 Guix 式的出口校验:产物闭包不完整就让构建失败,而不是留到运行时。

### D —— 沙箱声明(不变)

`--gpu` 有效,缺省不带也对,缺的是一句话。判据复用 `src/core/subos/graphics.cppm`。

## 4. OS 视角:标签是 OS 的 ABI 决策

把 xlings 当 OS 看,这件事的定位就变了。

**加载器是 OS 的一部分,而"用什么标签"是加载器契约的一部分,不是每个包的自由。**
现状是每个 recipe 各自决定(一个决定对了,68 个没决定),这在 OS 里相当于"每个软件包
自己决定 ABI"。godot 的 recipe 里那段注释——一个包的作者独立发现了 OS 级的问题并在
自己包里绕过去——正是这种缺失的症状。

同样地:

* **`<subos>/lib` 是 OS 的 `/usr/lib`**。farm 已经是那个目录了,它只是没有被声明为
  "加载器的默认位置"。三条路可以声明它:烧进每个二进制(RPATH,现在的路)、
  烧进加载器(cache,粒度不对)、或者**把它挂到加载器认得的位置**(bind mount,
  只有沙箱能做)。第三条在 `--sandbox` 里是可行的,而且是 Docker/Flatpak 的做法。
* **"只要是软件就没有不可能"在这里是成立的**:本轮没有任何一步需要改 NVIDIA、
  改 glvnd、或改内核。全部是我们自己打的标签。

## 5. 还没答的问题

* **翻转标签对已装生态的实际影响**没有测。需要一个差分:全量重打标签前后,
  跑一遍完整验收矩阵 + e2e,确认没有包依赖 `LD_LIBRARY_PATH` 覆盖。
* **`libwayland-server.so.0` / `libnvidia-gpucomp` 那两层**在 DT_RPATH 方案下是否
  全部解析,只在探针上验过,没在真实应用上验(godot 是 GL 不是 EGL)。
* **aarch64 / 无 GPU 机器 / CI** 上的行为未测。DT_RPATH 的翻转是全平台的。
