# subos 生态:架构级优化方案

日期:2026-08-06
输入:`.agents/docs/2026-08-06-subos-matrix-verification.md` 的 5 个缺陷
性质:设计提案,待 review 后再实施

---

## 0. 为什么需要这份文档

上一轮验证修掉的 4 个缺陷都是**症状**。把它们和 8 月 5 日那轮的 glibc 崩溃放在一起看,底下是**三个不同的架构问题**,每一个都还会继续产出新的缺陷:

| 架构问题 | 已产出的缺陷 | 还会产出什么 |
|---|---|---|
| P1 一个问题有多个回答者 | 依赖版本(8-05)、home 在哪里(8-06 四个回答者) | 每加一个"缺省即约定"的契约,就多一批 |
| P2 用进程全局机制满足单库需求 | libc 上 `LD_LIBRARY_PATH` 杀死 shell | 每个 host-link 类包都会重演,且各自答案不同 —— **已找到机制层面的出路并端到端验证,见 §2.3** |
| P3 subos 层没有"恰好一个"的执行点 | 同一 subos 绑定两个 mesa | 任何"装第二个版本"的操作 |

另有一个横切属性:**沉默成功**——"没发生"和"成功了"输出相同。它不是第四个问题,而是让上面三个都变得难以发现的原因。

下面每一节给出:现状实测 → 为什么是架构问题 → 方案与取舍 → 可观测性要求。

---

## 1. P1:一个问题有多个回答者

### 1.1 已确诊两次

**"这个依赖是哪个版本?"**(2026-08-05)四个回答者:resolver 的 plan node、libxpkg `pkginfo` 的目录扫描、xvm 的 active version、`elfpatch` 的 `{lib64, lib}` 约定探测。

**"xlings home 在哪里?"**(2026-08-06)四个回答者:`XLINGS_HOME` 环境变量、沙箱绑定目标、烘焙进产物的绝对路径、shim 从 argv0 反推的 owner home。

两次都是同一个机制:**在默认配置下所有答案恰好相同,所以从未被迫达成一致**。第二个版本 / 第二个 home 出现的那一刻,它们同时分歧。

### 1.2 什么让一个问题长出多个回答者

共同前提是**契约里写了"缺省即约定"**:

> `deps_exports` 里没有条目,意味着这个依赖什么都没声明——回退到约定。

一句"没有就自己猜"等于授权每个读端各自实现一份猜测。回答者的数量等于读端的数量,而读端会随时间增加。

`XLINGS_HOME` 是同一句话的另一种写法:"没设就用 `$HOME/.xlings`"。默认路径把四个独立计算变成了同一个字符串。

### 1.3 已经在用的五条规则,建议提升为规范

`2026-08-05-dependency-resolution-single-source.md` 里推导出的五条,实际上是通用的:

- **R1 权威记录必须是全量的** —— 每一项都记,不只是"声明了东西的那些"。值通常本来就算出来了,只是被 `break` 扔掉。
- **R2 约定只在写端应用** —— 读端永远不猜。
- **R3 删除而非调和** —— 如果改动是**增加**一条路径而不是**移除**一条,它是 workaround。
- **R4 对产物断言,不对意图断言** —— 在安装时失败,而不是在运行时。
- **R5 决策必须持久化** —— 需要复现才能查看的决策不叫可追溯。
- **R6 内部消费者绑定 payload,不绑定视图** —— 见 §1.5,这一条是本轮补上的。

**提议 A1**:把这五条写进 `xim-pkgindex/docs/V2/xpackage-spec.md` 的规范正文(目前只在 xlings 的设计文档里),并给每条配一个可执行判据,比如 R3 的判据:

> 一个修复如果只是让两个独立答案**更可能一致**,它是 workaround。只有**删掉第二个回答者**才是解决。
> libxpkg 0.0.49 没通过这条(它让扫描取最高版本而不是直接失败,仍与 `pin_target_to_active` 分歧);0.0.50 通过了。

**提议 A2**:契约文档里**禁止**"缺省即约定"式措辞。凡是"没有 X 就回退到 Y"的句子,要么改成"X 必须存在"(写端保证全量),要么改成"没有 X 是错误"。

### 1.5 R6:内部消费者绑定 payload,不绑定视图

三层模型里,前两层的**消费者不同**,而这一点从来没被写成规则:

| 层 | 是什么 | 谁应该消费它 |
|---|---|---|
| **payload** `data/xpkgs/<pkg>/<ver>/…` | 不可变、唯一确定的产物 | **xlings 与 libxpkg 自身**;RPATH / INTERP;`resolved_deps` |
| **subos sysroot / bin / PATH** | 给用户的**选择性视图**:可变、经 shim、受 `xlings use` 影响、可能属于别的 home | 用户,以及用户运行的程序 |
| 每个消费者的 RPATH/INTERP | 安装时冻结的决定 | 动态加载器 |

> **xlings 自己需要一个工具时,必须解析到 payload。视图只服务用户程序。**

内部代码去消费第二层,是层级倒置:它把自己的正确性交给一个**用户可以随时改变**的选择。而且视图与 payload 在默认状态下**恰好一致**——又是同一个陷阱。

这条规则**吞掉**了先前"借用了另一个 home 的东西"那个说法:那只是视图与 payload 分歧的**一种**方式,不是一个独立类别。

#### 违反一:`locate_proot_` 的 PATH 回退

已修,但修得不够彻底:当时只拒绝了位于 xlings home 内的候选。按 R6,**整条 PATH 步骤对内部使用都是错的**。真正的 `/usr/bin/proot` 属于**宿主依赖**,按 §2.8 应当是一条**显式声明**加一句可见的提示,而不是一个与 payload 平级的静默候选。

#### 违反二:libxpkg 的 `_find_tool` —— 这条更严重

`elfpatch` 用它找 **patchelf**,而 patchelf 正是给每一个载荷烙上 INTERP 与 RPATH 的工具。候选顺序:

```
1. subos_sysrootdir/bin/<tool>     ← 视图
2. _RUNTIME.bin_dir/<tool>          ← 视图
3. /usr/bin/, /usr/local/bin/       ← 宿主
4. which <tool>                     ← PATH
```

**payload 路径根本不在候选里。**

实测(`prodhome`):

| 候选 | 实际是什么 |
|---|---|
| payload `xim-x-patchelf/0.18.0/bin/patchelf` | 唯一确定的文件,**不在列表中** |
| 候选 1 `subos/default/bin/patchelf` | **符号链接指向 xlings 二进制**(shim),exec 时按 `XLINGS_HOME` 与活动 subos 解析 |
| 候选 3 `/usr/bin/patchelf` | 宿主的,**这台机器上存在** |

于是:**同源不变量所依赖的那把工具,身份由可变视图决定,并且带一条通往宿主的静默回退。** patchelf 各版本在动态段增长策略、`--force-rpath` 语义上都有过实际差异,所以"哪个 patchelf"不是无关紧要的细节——它决定产物长什么样。

同一个函数也用于 `readelf`,问题相同。

**提议 A3**:`_find_tool` 改为**优先且默认解析 payload**——通过 `pkginfo.resolved_dep()` / `build_dep()` 拿到工具包的载荷目录。视图与宿主降为显式、可见、需声明的回退,而不是排在最前的静默候选。这同时满足 R6 与规则 2。

### 1.4 下一个还没修的实例

**"一个 dlopen 进来的宿主文件,去哪里找它的依赖?"** 今天有三个答案(recipe 的手写表、宿主默认搜索、什么都不做),见 §2。
§2.3 给出的 shim 机制把它收敛为一个:**链接期依赖由我们拥有的对象上的 DT_RPATH 回答,运行时 dlopen 由宿主回答**,两者界线可判定。

---

## 2. P2:用进程全局机制满足单库需求

### 2.1 现状

`nvidia-gl-host-link` 的处境是真实的:NVIDIA vendor 库是宿主的文件(符号链接指向 `/lib/x86_64-linux-gnu/`),**不能**给它打 RPATH——那要改宿主的文件。所以 recipe 把依赖收拢到 `lib/xlings-deps/`,声明到 `LD_LIBRARY_PATH`。

`LD_LIBRARY_PATH` 是**进程全局、被每个子进程继承**的。需求是"这一个被 dlopen 的库要找到它的依赖",施加范围却是"这个 subos 里的每一个进程"。这个错配就是 P2。

### 2.2 实测:手写表两个方向都错

对宿主 NVIDIA 用户态全部文件求 DT_NEEDED,与 recipe 的手写表对照:

| SONAME | 被几个 nvidia 库 NEED | store 里谁提供 | 在手写表里? |
|---|---|---|---|
| `libc.so.6` | 全部 | glibc | ~~曾在~~ 已移除 |
| `ld-linux-x86-64.so.2` | 全部 | glibc | 否(正确) |
| `libpthread.so.0` / `librt.so.1` / `libdl.so.2` | 多个 | glibc | ✓ |
| **`libm.so.6`** | **16 个**,含 `libnvidia-glcore` | glibc | **✗ 漏** |
| `libX11.so.6` / `libXext.so.6` | 5 个 | libX11 / libXext | ✓ |
| **`libdrm.so.2`** | 有 | libdrm | **✗ 漏** |
| **`libgbm.so.1`** | 有 | mesa | **✗ 漏** |
| **`libgcc_s.so.1`** | 有 | gcc | **✗ 漏** |
| **`libwayland-client/server.so.0`** | 有 | wayland | **✗ 漏** |
| `libxcb.so.1` / `libXau` / `libXdmcp` | **0 个** | libxcb 等 | ✓(理由不同:DT_RUNPATH 不传递) |
| `libcrypto.so.1.1` / `libcrypto.so.3` / `libnvcuvid.so.1` | 有 | 无人提供 | — |

结论:

1. **漏了五个**(libm、libdrm、libgbm、libgcc_s、libwayland-*)。它们今天**静默地来自宿主**——正是这个包存在的目的所要关掉的泄漏。
2. **我上一轮关于 `libm` 的判断是错的**。它不在 `libEGL_nvidia` 的直接 DT_NEEDED 上,但被 16 个 nvidia 库 NEED。上一轮报告里"libm 无人需要"这句话需要更正,已在 §6 记录。
3. **只有 `libc.so.6` 是"既无用又致命"**:它对每个进程都必然已加载(所以搜索路径永远用不上它),而放上去会杀死宿主二进制。这一条的测量结论不变。
4. 表里混了**两种理由**:DT_NEEDED 直接需要 vs. DT_RUNPATH 不传递导致的二级需要。`libGLdispatch.so.0` 是第三种(glvnd 分发,应用侧加载)。一张表承载三种语义,是它容易写错的原因。

而这个文件自己的注释说过:

> Enumerated rather than listed: … a fixed list would be a list of one driver release.

`__nvidia_entries` 遵守了这条,依赖表没有。**同一个文件里的两套标准。**

### 2.3 找到了机制层面的出路

先说为什么前一版的推荐(修正表内容 + 加护栏)不合格:按本文档 §1.3 的 **R3** 判据——"如果改动是**增加**一条路径而不是**移除**一条,它是 workaround"——那两条都没有删掉任何回答者。内容修正只是把错的表改对,护栏只是限制损害。`LD_LIBRARY_PATH` 这个进程全局机制仍在。

出路来自一条被忽略的 ELF 性质:

> **DT_RPATH 沿加载链传递,DT_RUNPATH 不传递。**

这条性质意味着:我们不必修改宿主的文件,也能决定它的依赖去哪里解析——只要在**它和加载器之间放一个我们拥有的对象**,把策略放在那个对象上。

#### 合成实验:先证伪

构造一个"我们不能修改的宿主 vendor"(无 rpath,NEED 一个只存在于我们目录里的库):

| 加载方式 | 结果 |
|---|---|
| 直接 dlopen 该 vendor | **失败** —— 依赖找不到 |
| 经过一个我们拥有的 shim,shim 带 **DT_RUNPATH** | **失败** |
| 经过同一个 shim,shim 带 **DT_RPATH** | **成功**,`LD_DEBUG` 显示依赖从我们的目录解析 |

RUNPATH 与 RPATH 的对照是关键:两者只差一个 patchelf `--force-rpath`,结果相反。**这条性质是承重的,不是巧合。**
并且 `dlsym(shim_handle, ...)` 能取到 vendor 的符号——dlsym 搜索句柄的整个依赖树,所以 glvnd 拿到的仍是真 vendor 的入口。

#### 真实 NVIDIA 栈:A/B

同一个探针二进制,同一份 `LD_LIBRARY_PATH`(只含应用自己需要的 X11 目录,**故意不含 glibc**),唯一差别是 vendor JSON 指向谁:

| JSON 指向 | 结果 |
|---|---|
| 宿主 vendor 本身(今天的机制,去掉 `xlings-deps`) | `DEVICE_COUNT=0` —— vendor 加载不了 |
| 我们的 shim(DT_RPATH 指向闭包) | `DEVICE_COUNT=1` |

`LD_DEBUG` 确认:vendor 的 `libdl / libm / libpthread / librt` 从 **我们的 glibc 载荷**解析,而全局搜索路径上没有 glibc。

#### 机制的边界(实测,不是推测)

shim 组能枚举出设备,但 `eglInitialize` 失败。`strace` 对比工作组与 shim 组打开的文件,差异是:

```
libnvidia-glsi / libnvidia-eglcore / libnvidia-egl-gbm
libnvidia-egl-wayland / libdbus-1
```

vendor 在**运行时按裸 SONAME `dlopen` 自己的兄弟库**。于是边界是:

> **DT_RPATH 的传递性覆盖链接期依赖(DT_NEEDED),不覆盖运行时 dlopen。**

运行时 dlopen 没有链接链可依附,任何 RPATH 机制都服务不了它。这是这条路线的硬边界。

#### 边界恰好落在正确的地方

那些运行时 dlopen 找的是 **NVIDIA 自己的兄弟库**——宿主的文件,而且**必须**与宿主内核模块匹配。它们本来就该来自宿主。于是问题按同一条线切开:

| 要找什么 | 由谁提供 | 为什么这是对的 |
|---|---|---|
| vendor 对**我们的**库的 DT_NEEDED | **shim 的 DT_RPATH** | 作用域是链接链,per-consumer,不向任何其他进程施加任何东西 |
| vendor 运行时 dlopen **它自己的**兄弟库 | 宿主驱动目录放在 `LD_LIBRARY_PATH` | 全是宿主文件,宿主二进制本来就能解析到同一批;我们的库一个都不在上面 |

这条线正是这个包自己已经画的那条线("`lib/` 是宿主的,`xlings-deps/` 是我们的")。

#### 端到端验证

`LD_LIBRARY_PATH` 上**只有宿主驱动目录**,`xlings-deps` 完全不参与:

```
DEV0_EGL_VENDOR   = NVIDIA
DEV0_GL_RENDERER  = NVIDIA GeForce RTX 4080/PCIe/SSE2
DEV0_GL_VERSION   = 4.6.0 NVIDIA 550.144.03
DEV2_GL_RENDERER  = llvmpipe (LLVM 20.1.7, 256 bits)     ← 软件回退同时可用
```

- 宿主 `/bin/bash` 在同样的 `LD_LIBRARY_PATH` 下正常(那目录里没有我们的任何东西)。
- shim 体积 **27KB**(对照:拷贝整套用户态 327MB)。
- 仍是符号链接指向宿主文件,**用户态/内核模块的版本耦合完整保留**。
- **不需要安装时的编译器**:已验证用 patchelf 对一个预置空 stub 做 `--add-needed` + `--set-rpath --force-rpath` 即可产出可用 shim。

### 2.4 这解决了什么,以及为什么它不是 workaround

1. **`xlings-deps` 整个消失**,两张硬编码表随之消失。shim 的 DT_RPATH 从已解析依赖推导,而 `resolved_deps` 已经是 xlings 记录的权威记录(R1 已经做过了)。§2.2 漏掉的五个库不需要"补进表里"——表没有了。
2. **我们的库永远不出现在任何进程全局搜索路径上。** libc 那一类缺陷从"被护栏挡住"变成**结构上不可能**。
3. **删掉了一个回答者**,而不是增加。这是 R3 意义上的解决。
4. xlings 侧的护栏保留,但它的角色变了:从"防止损害"变成 **R4 意义上的断言——它应当永远不触发**。触发即说明某个 recipe 又走回了老路。

### 2.5 提议

**提议 B1(替换 B1/B2 旧版)**:把这个机制做成 **libxpkg 的公共能力**,而不是 recipe 的私有代码:

```
elfpatch.host_link_shim{
    vendor  = "<宿主 vendor 的绝对路径或 SONAME>",
    deps    = <从 resolved_deps 推导的载荷 libdir 列表>,
    out     = "<我们 payload 里的 shim 路径>",
    soname  = "<需要时,例如 GLX 要求 libGLX_nvidia.so.0>",
}
```

`nvidia-gl-host-link` 与 `libcuda-host-link` 共用它——后者实测**既不收拢依赖也不声明 `LD_LIBRARY_PATH`**,今天依赖全部来自宿主,是同一个问题的第二个答案。一个实现,两个消费者。

**提议 B2**:`nvidia-gl-host-link` 的 `LD_LIBRARY_PATH` 声明收窄为**只有宿主驱动目录**,并在注释里写明它为什么是安全的(里面没有我们的任何文件)。`xlings-deps` 目录删除。

**提议 B3(不变)**:规范里写明,任何 `subos.env` 对 `LD_LIBRARY_PATH` / `LD_PRELOAD` 的声明都是特权操作,需要写明为什么 RPATH 不适用。有了 shim 机制,"RPATH 不适用"的真实场景只剩**运行时按裸 SONAME dlopen 宿主自己的文件**这一种。

**方案 B(拷贝 327MB + RPATH)正式否决**,理由写进 recipe:它打破用户态与内核模块的版本耦合,而 shim 用 27KB 拿到了同样的隔离性。

### 2.6 shim 修不了的另一半:被库自己读的搜索变量

`LD_LIBRARY_PATH` 不是唯一一个进程全局的搜索变量。`subos.env` 目前声明的四个里,有两个是**同一个形状**:

- `__EGL_VENDOR_LIBRARY_DIRS` —— libglvnd 自己读
- `LIBGL_DRIVERS_PATH` —— mesa 自己读

它们不经过动态加载器,所以 shim 的 DT_RPATH 完全够不到,xlings 侧的 libc 护栏也看不见(护栏只检查 loader 读的变量)。

**实测。** 一个**宿主**二进制(`INTERP=/lib64/ld-linux-x86-64.so.2`,宿主 loader、宿主 libc),编译时只链接宿主的 `libEGL`:

| 运行环境 | `GL_RENDERER` |
|---|---|
| 不带 subos 声明 | `NVIDIA GeForce RTX 4080/PCIe/SSE2` |
| 带 subos 声明 | `llvmpipe (LLVM 20.1.7, 256 bits)` |

`LD_DEBUG` 显示它加载进来的是**我们的** `libm.so.6`(glibc 2.39 载荷)、`libgcc_s`、`libstdc++`、`libxcb`、`libxshmfence`——全都进了一个跑在宿主 libc 上的进程。

两个后果:

1. **规则 1 违反**:宿主的东西依赖了我们的。这台机器宿主 glibc 恰好也是 2.39 所以没崩;换一台旧 glibc 的机器就是 `version 'GLIBC_2.xx' not found`。和 libc 那次是同一个形状,只是慢一拍。
2. **功能上是静默降级**:宿主程序从硬件加速掉到软件渲染,没有任何提示。用户会认为"进了 subos 之后 GL 变慢了"而查不到原因。

**这一条是可以彻底解决的,而且解法就是本文档的主线**:这两个变量存在的唯一目的,是告诉**我们的** GL 栈它自己的驱动在哪里。而 libglvnd 与 mesa **是我们自己构建的**——完全可以把路径**编进产物**(构建时的默认 vendor 目录 / DRI 目录,或 `$ORIGIN` 相对路径),不必经过环境。

一旦编进产物:

- 我们的 GL 栈自己知道去哪里找,不需要任何环境变量;
- 宿主的 libglvnd 用宿主的默认目录,拿到宿主的 vendor——**规则 1 与规则 2 同时成立**;
- `subos.env` 里只剩 `XDG_DATA_DIRS` 这类真正属于"用户可见约定"的变量。

**提议 B4**:mesa 与 libglvnd 的构建把 vendor 目录 / DRI 目录设为自身载荷路径,删除这两条 `subos.env` 声明。这比 shim 更直接——那两个库是我们的,不存在"不能修改宿主文件"的约束,当初用环境变量只是因为没有把"决定应当由产物携带"当成规则。

**提议 B5**:xlings 侧的护栏目前只检查 loader 读的变量(`LD_LIBRARY_PATH` / `LD_PRELOAD`)。扩展为:**任何 `subos.env` 声明,如果它的值指向我们的载荷目录,都要在安装时报告**——因为进程全局的环境变量没有"只对我们的进程生效"这种作用域。报告而非拒绝:`XDG_DATA_DIRS` 这类是正当的。

### 2.7 还需要验证的



诚实列出,不要当成已完成:

- **GLX 路径**:`libGLX_nvidia` 的 vendor 选择走的是按 SONAME 模式 `libGLX_%s.so.0` 查找,shim 需要顶替这个文件名。机制应当相同,但没有单独验证过。
- **Vulkan ICD**:同理,ICD JSON 指向文件路径,预期可用,未验证。
- **`dlsym` 语义**:合成实验证明句柄依赖树可见;glvnd 是否对 vendor 做过 SONAME 或路径上的额外校验,未穷尽。
- **预置 stub 的分发**:每个 arch 一个,归属 libxpkg 还是索引,未定。

## 2.8 规则 2 缺的是执行点,不是意图

> "vendor 的 libm / libdrm / libgbm / libgcc_s / libwayland 为什么要用宿主的?"

**没有人决定用宿主的。** recipe 的表没列它们,于是没有任何地方提供;动态加载器在我们提供的所有位置都找不到,就落到宿主的默认搜索(`ld.so.cache`、`/lib/x86_64-linux-gnu`)。悄无声息,因为对加载器而言"找到了"就是成功,不问来自哪里。

> **宿主是我们没能回答的任何问题的默认答案。**

这就是为什么规则 2("能不依赖宿主就不依赖")**不能靠意图成立**。它需要一个执行点:让"我们没提供"成为**硬错误**,而不是回退。

### 现状实测

对 `prodhome` 的 483 个 ELF 求 DT_NEEDED,统计有多少落到宿主:

| SONAME | 处数 | 判定 |
|---|---|---|
| `libKSC/libGB/libJIS/libCNS/libJISX0213/libISOIR165` | 20 | **假阳性** —— glibc 自己 `lib/gconv/` 下的模块,扫描没把该目录算作提供方 |
| **`libxml2.so.2`** | 1 | **真漏** —— `wayland` 的载荷 NEED 它,store 里没有 libxml2,解析到宿主的 2.9.14 |

**483 个 ELF 里只有 1 处真漏。** 我们自己构建的载荷状态其实相当好——nvidia 那五个之所以严重,是因为漏的那个文件是**宿主的 vendor 库**,它不在我们的载荷里,所以任何只扫自己载荷的检查都看不见它。

### 提议 D4:安装期的闭包断言

`elfcheck::scan_payload` 已经在做同源断言(§R4)。同一个位置扩展一条:

> 对刚安装的载荷里每个 ELF,以及**载荷链接进来的每个宿主文件**,解析其 DT_NEEDED 传递闭包。任何会落到宿主的 SONAME,必须在 recipe 的显式清单上;不在清单上就是安装失败。

关键是**显式清单**,不是放宽:

```lua
exports = {
    runtime = {
        -- 允许落到宿主,并说明为什么。不写在这里的一律是安装错误。
        host_deps = {
            "libnvidia-*",     -- 必须与宿主内核模块匹配,见 §2.3
            "libdbus-1.so.3",  -- 驱动运行时 dlopen,RPATH 够不到
        },
    },
}
```

这样"依赖宿主"从**意外**变成**声明**:

- 五个漏掉的库会在安装时报错,而不是安静地从宿主拿——它们本来就该由 §2.3 的 shim 提供;
- `libxml2` 这处会立刻暴露(要么给 wayland 声明 libxml2 依赖,要么写进 `host_deps` 并说明理由);
- 驱动用户态那个**不可解**的洞变成一行有理由的声明,而不是一个没人知道的事实;
- 规则 2 第一次有了可执行判据:**未声明的宿主依赖 = 安装失败**。

配套的可观测性(§4 的 D2):安装结束时报告三个数——从我们载荷解析的、按声明落到宿主的、无人提供的。今天只报 "N libraries ✓",而 N 里既有真链接也有静默跳过。

---

## 3. P3:subos 层没有"恰好一个"的执行点

### 3.1 现状

你定的三层模型:

| 层 | 版本数 |
|---|---|
| xpkg store | 多个(设计允许) |
| **subos sysroot** | **恰好一个** |
| 每个消费者的 RPATH/INTERP | 各自一个 |

实测 `prodhome/default`:

```
$ xlings list | grep mesa
  ◆ xim:mesa@25.0.7.1
  ◆ xim:mesa@25.0.7
```

subos manifest 里两个 binding 都在,两者都在贡献 `__EGL_VENDOR_LIBRARY_DIRS`(3 项 = nvidia + 两个 mesa),EGL 因此枚举出重复设备。`xlings self doctor` 不报。

**中间层的"恰好一个"没有任何地方在执行。**

### 3.2 为什么这是架构问题而不是一个 bug

"这个 subos 里有什么"目前有两个记录:

1. **xvm 注册**(哪些程序/库被绑定)
2. **subos manifest 的 `envs` 段**(哪些 binding 贡献了环境变量)

安装第二个版本时,两个记录各自追加,没有任何一处执行"替换而非并列"。这是 P1 的又一个实例——只不过这次两个回答者恰好**都答"两个都在"**,所以它们一致,但一致地违反了模型。

**规则没有执行点,就不是规则,只是文档。**

### 3.3 方案

**提议 C1(执行点)**:在 subos 层引入单版本约束。安装 `pkg@B` 到已有 `pkg@A` 的 subos 时:

- 默认**替换**:解绑 A,绑定 B。store 里 A 仍然保留(store 是多版本层),只是这个 subos 不再指向它。
- 需要并存时必须显式(不同 subos,或未来的显式 flag),而不是靠安装顺序悄悄达成。

**提议 C2(单一记录)**:`envs` 段不再独立记录 binding,而是从 subos 的绑定集合**派生**。R2:约定只在写端应用。这样"这个 subos 里有什么"只有一个答案。

**提议 C3(可观测)**:doctor 增加一条检查——同一包在同一 subos 有多个绑定即报告,并给出 `--fix`(保留 xvm active 的那个)。注意 `reference_reporter_repairer_predicate_drift` 的教训:报告端和修复端必须**共用同一个谓词函数**,不是各写一份等价逻辑。

### 3.4 迁移

已有的 home 里可能已经存在多重绑定(prodhome 就是)。C1 上线前 doctor 必须先能报告并修复,否则用户会在下一次安装时遇到一个"突然开始替换"的行为变化而不知道为什么。**顺序:C3 → C1 → C2。**

---

## 4. 横切:沉默成功是这个代码库的默认失败模式

### 4.1 本轮遇到的全部实例

| 现象 | "没发生"与"成功了"如何变得不可区分 |
|---|---|
| doctor 不报双绑定 | 干净的 doctor 输出 = 没有双绑定 **或** doctor 不看这个 |
| `dep_install_dir()` 返回 nil,内层循环整个跳过 | 依赖没提供 = 依赖不需要提供 |
| 手写表漏了 libdrm/libgbm/… | 从宿主拿到了 = 我们提供了 |
| 沙箱不应用 subos.env | 变量为空 = 没有包声明过 **或** 整层被跳过 |
| 隔离 home 借用宿主 proot | 沙箱正常进入 = 用的是这个 home **或** 用的是另一个 home |
| e2e S3 的 skip 分支 | PASS = 测过了 **或** 跳过了整个特性 |

`subos_sandbox_test.sh` 的 S3 分支里已经有人意识到了这个问题并写了注释("Reporting PASS while silently skipping the entire feature under test is how a real regression would reach a release looking exactly like an unattended laptop")——但那是一个人在一个地方的自觉,不是机制。

### 4.2 提议

**提议 D1(规则)**:凡是"因为条件不满足所以没做"的分支,输出必须与"做了"不同。这条已经在 `project_silent_success_pattern` 里记录,建议提升为**代码评审清单项**:任何新增的 `if (...) continue;` / `if not X then return end`,评审时必须回答"跳过时用户看到什么"。

**提议 D2(机制)**:host-link 类包安装结束时,报告**三个数**:命中我们载荷的、落回宿主的、无人提供的。今天用户只看到 "N libraries ✓",而 N 里既有真链接也有静默跳过。

**提议 D3(判据)**:`§1.3 R5` 的持久化已经有了 `.xlings-resolution.json` 和 `xlings why`。建议把 host-link 的解析结果也写进同一个文件——"哪个 SONAME 来自哪里"是一个事后必然会被问到的问题,现在需要重建 store 状态才能回答。

---

## 5. 测试架构

三条,都来自本轮实测,都是流程问题不是代码问题。

**提议 E1:隔离 home 成为 subos/沙箱测试的默认环境。**
上述四个 home 相关缺陷在默认 `~/.xlings` 下全部无症状。只要测试都在默认 home 或同形路径下跑,这一整类缺陷不可见。**并且被测 home 应当放在一个与 `$HOME` 无共同前缀的路径下**——本轮把它放在 `/tmp` 下,恰好命中了绑定顺序最难的情形(`/tmp` 先被私有化再挂 home),这个偶然应当变成故意。

**提议 E2:测试断言写契约,不写实现。**
`subos_sandbox_test.sh` 的 S8 断言 "`~/.xlings` 在沙箱内能看到宿主内容"——那正是 D1 的错误行为。**一个测试不仅可能漏掉缺陷,还可能把缺陷钉死**:修 D1 必须同时改这条断言,而改测试断言在评审里天然可疑。已改写为契约断言(`XLINGS_HOME` 即宿主路径、`PATH[0]` 与之一致、不存在第二个拼写)。

**提议 E3:核心模块改动后必须双工具链构建。**
`views::split | ranges::to` 在发布目标 gcc 15.1.0-musl 下编译通过,在默认 gcc 16.1.0 下让整个模块以 "Bad file data" 失败,并指向一个未改动的 TU(`cli.cppm`)。只跑发布目标的构建会让它直接发出去;只跑默认目标则会以为是自己刚改的文件坏了。CI 已有两个目标,但**本地开发循环没有门禁**。

---

## 6. 对上一轮报告的更正

`2026-08-06-subos-matrix-verification.md` §4 里的这句话是错的:

> `libm` 有 1203 个符号(真正的 ABI 面),但**没人要它**。

`libm.so.6` 被 16 个 nvidia 库 NEED,包括核心渲染器 `libnvidia-glcore`。正确的说法是:它不在 `libEGL_nvidia` 的**直接** DT_NEEDED 上,而我当时只看了那一个文件。

这不改变 §4 的其余结论(`libc.so.6` 既无用又致命;三个桩库必需),但它改变**方法论上的结论**:逐库测量比逐库推理好,而我做的逐库测量本身取样不足——只取了闭包的一个入口。这正是 §2.2 提议改用闭包推导的直接理由。

对 xlings 侧防线的判断不变:防线只排除 `libc.so.6` 和 loader,`libm` 不在其中,所以补上 libm **不需要**改 xlings。

---

## 6.5 架构决策记录(由 sunrisepeak 定)

以下是 review 中定下的决策,连同它们否掉的我的错误判断。

### AD-1:subos 同时是编译期与运行期概念,优先级规则是"能直连 payload 就直连"

我曾把它当成二义性("对二进制是编译期概念,对 alias 是运行期概念")。不是二义性,是**优先级**:

> 能直接走 payload 的就直接走 payload;走不了的,subos 正常映射。
> **优先走 payload 是为了稳定性。**

推论,并且解释了 doctor 那条告警为什么两半都对:

- 已编译产物的 RPATH/INTERP 冻结在 payload 上 —— 用户切 subos 不改变一个已经构建好的二进制的行为。这就是"稳定性"。
- alias / shim 在 exec 时跟随活动 subos —— 这是视图按设计工作。
- 所以 `x86_64-linux-gnu-gcc@16.1.0 records an install-time subos path` 这条告警里,**"执行跟随活动 subos"是正确行为**,而**"记录里存着安装期的 subos 路径"是缺陷** —— 记录本应存 payload 路径或什么都不存。

与 §1.5 的 R6 一致:R6 说的是"内部消费者"这一侧,AD-1 说的是**通用优先级**,R6 是它在 xlings 自身代码上的特例。

### AD-2:refcount 是删除判据 —— 有引用不删,强制删除必须告警

不需要更复杂的机制。

剩余的真实边界要写明:**refcount 只覆盖包对包的引用,覆盖不了用户自己编译的产物**——那些二进制的 RPATH 指向 payload,但它们不在 store 里,没有任何计数会知道它们。所以"没有包引用它"不等于"删了安全"。这正是强制删除必须告警的原因,而告警文案应当说清这一点。

### AD-3:`XDG_DATA_DIRS` 类变量不属于问题域

subos 提供默认值、用户可覆盖,这就是 Linux 的常规做法,没有问题。

我把它和 `LD_LIBRARY_PATH` 归为一类是**过度概括**。正确的分界线不是"是不是进程全局",而是:

| 类别 | 例子 | 为什么危险 / 不危险 |
|---|---|---|
| **导致代码被载入进程** | `LD_LIBRARY_PATH`、`LD_PRELOAD`、`__EGL_VENDOR_LIBRARY_DIRS`、`LIBGL_DRIVERS_PATH` | 把我们的库塞进宿主进程 → ABI 耦合 → 崩溃或静默降级 |
| **导致数据被找到** | `XDG_DATA_DIRS`、`PATH`(某种程度) | 最坏是宿主程序看到我们的一个 `.desktop` 或图标。没有 ABI 面 |

§2.6 的提议 B5(护栏扩展到"任何指向我们载荷的声明")按这条重写:**只管第一类**。

### AD-4:更正 —— glibc 默认搜索路径不是"靠意外维持的承重属性"

我曾把 ld.so 里烙着 `/home/xlings/.xlings_data/...` 当成"一个承重属性靠构建参数的副作用维持着"。**这个判断是错的**,两点:

1. 它不是旧产物遗留。这一季刚发布的 **glibc 2.44 烙的是同一个前缀**,是当前构建约定。
2. 更重要:对一个**可重定位**的包,构建期 `--prefix` 永远不可能等于运行时安装路径。默认搜索路径**必然**指向不存在的位置——这是结构性保证,不是运气。"一切必须靠 RPATH"因此是结构决定的。

真正剩下的是**可追溯性问题**:那个字符串泄漏了构建机的 home 布局(`.xlings_data` 是早已废弃的运行时布局)。与任务 #35(libxml2 的 `.pc` 写着构建机)同类,应当统一处理为"产物里不得出现构建机路径,除非是刻意保留的占位前缀"。

## 7. 落地顺序

依赖关系决定顺序,不是优先级:

```
E1/E2 (隔离 home + 契约断言)  ─→  独立,应当最先做:它决定后面所有验证是否可信
A1/A2 (规范化五条规则)        ─→  独立,成本最低,防止新回答者被引入
E3    (双工具链门禁)          ─→  独立

§2.6 的四项验证 (GLX / Vulkan / dlsym 语义 / stub 分发)
        │
        ▼
B1 (libxpkg 的 host_link_shim 能力)
        │
        ├─→ B2 (nvidia-gl-host-link 切换到 shim,删除 xlings-deps)
        └─→ B2' (libcuda-host-link 用同一能力,关掉它今天的全量宿主泄漏)
        │
        ▼
B3 (规范:LD_LIBRARY_PATH 声明是特权操作)   ← 有了 shim 才写得出"什么时候才真的需要它"

C3 (doctor 报双绑定)  ─→  C1 (单版本执行点)  ─→  C2 (envs 派生)

D1/D2/D3 (可观测性)   ─→  D3 可与 B1 一起做(shim 的解析结果正是要持久化的东西)
```

**先做 E1/E2 与 A1/A2**:前者让后续所有验证可信,后者阻止新的回答者被引入。

**B 线在 §2.6 四项验证完成前不要动代码**——GLX 与 Vulkan 两条路径没验证过,现在实现等于把一个未经检验的假设写进公共能力里。这正是上一轮"逐库测量但取样不足"的教训:机制在 EGL 上成立,不等于在 GLX 上成立。

**C 线与 B 线可并行**,内部顺序不可换:先能报告,再改行为。

**已落地的四个修复(D1–D4)保持不变**,它们与本提案不冲突:§2 的 shim 机制会让 xlings 侧的 libc 护栏永远不触发,但护栏本身作为 R4 断言应当保留。
