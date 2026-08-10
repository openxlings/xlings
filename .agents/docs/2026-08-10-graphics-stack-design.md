# xlings 图形栈:完整 / 稳定 / 可观测 的设计方案

> 综合 2026-08-10 全天的实测。前置阅读(结论,不必重读过程):
> `2026-08-10-graphics-four-gaps-root-cause.md`(根因与证据链)、
> `2026-08-10-graphics-availability-assessment.md`(48 格渲染矩阵)。
> 本文是**设计**,不是报告:它规定要建什么、按什么顺序、用什么判据验收。
>
> 所有标注"已验证"的结论,均在 NVIDIA RTX 4080 / 550.144.03 / X11 / x86_64 /
> Ubuntu 上实测。**这是一台机器的结论**,跨硬件覆盖是 E5。

## 0. 一句话

四块缺口里有三块是**同一个标签**;那个标签今天由 73 个 recipe 各自决定,1 个决定对了。
方案的重心不是把标签改对,是**把"谁来决定"从 73 个地方收回到 1 个地方,并让偏离在
用户机器上装机时就被说出来**。

## 1. 目标:"完整 / 稳定 / 可观测"的可测判据

不给形容词,给能跑的判据。

| 维度 | 判据 | 怎么验 |
|---|---|---|
| **完整** | GLX / EGL / GLESv1 / GLESv2 / Vulkan **各自**在 subos 内到 GPU;CPU 与无显示离线两条路径可用**且可被指定**;环境内能构建并运行 GL 程序 | `matrix.sh` 全绿,`report_build_defect` 报 OK |
| **稳定** | 同一台机器上,重装、`xlings use` 切换、宿主驱动升级之后,结论不变或**明确报出变化** | `graphics-acceptance.sh` 记录与加载器一致;`gl-doctor` 漂移检测 |
| **可观测** | 任何降级都有一条用户看得到的通道说出来,且**不需要用户先怀疑** | `xlings subos info` 一条命令说清 |
| **完备性** | 上述判据由**系统在装机时评估**,而不是靠人记得跑工具 | `closure_check` rule E |

第四行是这套方案与之前所有尝试的分界:**前三行是结果,第四行是让结果不再退化的机制。**

## 2. 根因(一段)

`elfpatch` 给载荷打的是 **DT_RUNPATH**,而 DT_RUNPATH **不传递**。图形栈的加载链有三到
四层 `dlopen`(glvnd → vendor → EGL 外部平台模块 → 它自己的依赖),只有**可执行文件上的
DT_RPATH** 对进程内任意深度的 `dlopen` 生效。

**路径一直是对的,标签一直是错的。** 已验证:同样的路径内容,只改标签类型,
**索引零改动、interposer 零改动、无环境变量**:

| 可执行文件标签 | egl | gles2 | egl-surfaceless | glx |
|---|---|---|---|---|
| **DT_RPATH** | **NVIDIA** | **NVIDIA** | **NVIDIA** | **NVIDIA** |
| DT_RUNPATH | llvmpipe | llvmpipe | llvmpipe | NVIDIA |

抽样 73 个已安装可执行文件:**1 个 DT_RPATH(godot),68 个 DT_RUNPATH,其中 55 个路径
里已经有 farm**。godot 的 recipe 自己调 `patchelf --force-rpath` 并写下了准确的原因——
**这个知识没有任何机制把它带给另外 68 个包**。

## 3. 架构:四层,一个契约

```
   ┌─────────────────────────────────────────────────────────┐
   │  可观测层    subos info(接线记录 + 驱动漂移 + 沙箱状态)  │  E3
   ├─────────────────────────────────────────────────────────┤
   │  验证层      closure_check rule E:装机时评估标签不变式   │  E1b ← 基石
   ├─────────────────────────────────────────────────────────┤
   │  写入层      elfpatch(打包侧) │ 链接器包装(构建侧)      │  E1a / E2
   ├─────────────────────────────────────────────────────────┤
   │  契约层      subos_info.envs 声明 XLINGS_SUBOS_LIB        │  E2a
   └─────────────────────────────────────────────────────────┘
```

**契约层是唯一的事实来源**;写入层有两个实现(打包侧与构建侧),但**读同一份声明**;
验证层保证两个实现的结果一致,且不被第三方悄悄覆盖。

### 3.1 为什么契约不能放在 gcc specs

已验证的三条,每条都足够:

* **它是共享可变状态。** 一份 gcc 载荷被这个 home 里每次安装反复 patch。实测三个载荷
  三种内容:`gcc@11.5.0` 把 `<subos>/default/lib` **写死**进了共享载荷,`gcc@16.1.0` 的
  `-rpath` 里 glibc **出现两次**。
* **`-specs=` 是替换不是追加**(除非定义以 `+` 开头)。任何构建工具都能悄无声息地丢掉它。
* **mcpp 已经这么做了,而且理由正当。** `mcpp-clean-link.specs` 重定义 `*link:`,剥掉
  `-rpath`,注释写明:"`-rpath` accumulates … that file has been patched by every home
  that ever installed against this shared payload — one stale entry per run, forever."

**这不是 mcpp 与 xlings 打架,是 specs 这个位置不适合承载契约。**

## 4. 分项设计

### E1 — 标签契约:唯一写入者 + 装机验证 【基石】 —— **已落地**

> libxpkg 0.0.57(openxlings/libxpkg#41)+ xlings 2026.8.10.4(#536)。
> 端到端实测:强制重装后可执行文件 `(RPATH)`、同包的库 `(RUNPATH)`,正是设计要求的分界。

#### E1a 修正唯一写入者

`libxpkg/src/lua-stdlib/xim/libxpkg/elfpatch.lua`,五个 `--set-rpath` 调用点只有
`host_link_interposer`(1587)带 `--force-rpath`。patchelf 的 `--set-rpath` **默认写
DT_RUNPATH**。

**只对可执行文件强制 RPATH,不要全翻。** `#593` 已实测:在**库**(interposer)上强制
RPATH 有害——传递性会把 farm 推进那个库往下的每一次查找,`eglInitialize` 直接失败。
而消费者可执行文件带 RPATH + interposer 原封不动,四个入口点全通。

区分谓词**已存在**:460 行下方的 `_has_pt_interp(filepath, patch_tool)`(现用于决定是否
`--set-interpreter`)就是"这是可执行文件"。

**取舍(实测,非推理)**

| 顾虑 | 实测结论 |
|---|---|
| DT_RPATH 优先级高于 `LD_LIBRARY_PATH`,用户无法再覆盖 | subos 里 `LD_LIBRARY_PATH` **根本没设置**;recipe 注释明写设计目标是"让这套栈不需要任何人设它"。**今天不破坏任何现存行为**,代价在未来的覆盖能力 |
| 传递性双向,会污染 | DT_RPATH 作用域**严格限于这一个二进制的进程,不传播给子进程**——这正是它比 `LD_LIBRARY_PATH` 安全的地方 |
| blast radius | 标签在**安装时**打,已装包不会自动变。**这是唯一真实成本**:需要一次全量重打或迁移策略 |

#### E1b 装机验证 —— rule E 【本方案的核心】 —— **已落地**

`src/core/closure_check.cppm` 已经是正确的强制点,而且是**为解决同一类问题**建的。
它的模块注释自己写着:

> That predicate is right, and **it runs in exactly one place — a repository workflow
> the user's machine never sees**. … This module is the same predicate, **evaluated
> where the payload actually lands**.

**标签契约现在正处在被搬运之前的那个状态:它活在 73 个 recipe 作者的脑子里。**

它已具备:用户机器上每次安装时运行、rule A/D、**WARN-first 的既定推进节奏**
("collect real gaps before turning the gate hard")、form-X 豁免概念。
它缺的:`RPATH|RUNPATH` 在整个文件里出现 **0 次**——它读 `--print-interpreter` 和
`--print-needed`,**不读标签**。

**rule E**:

> form-X **可执行文件**的搜索路径必须在 `dlopen` 深度可达——即必须是 DT_RPATH。
> 带 DT_RUNPATH 的,它 `dlopen` 出来的库看不见它的路径。

这一条把"每个作者都要记得"换成"系统在每台机器上检查一次"。

| | 现在 | 加 rule E 之后 |
|---|---|---|
| 谁决定标签 | 73 个 recipe(1 个对) | elfpatch,一个写入者 |
| 谁验证 | 无人 | 装机时,每次 |
| 偏离怎么被发现 | 用户发现帧率低 | 装的时候就说 |
| 合法例外 | 隐形(godot 那样自己 patch) | 必须显式声明才通过 |

**顺序不能反**:先 E1a 再 E1b。不先修写入者,rule E 会对 68 个包同时报警。

#### E1c 显式退出

**豁免不能靠推断。** 我原以为 mcpp 那种刻意链宿主的产物会因"非 form-X"自动豁免——
**实测不成立**:mcpp 构建出的 xlings 二进制,INTERP 指向 mcpp 自己 store 里的 glibc,
**是 form-X**。

所以契约不只要规定默认值,还要规定**怎么合法地不遵守它**。退出必须是声明出来的
(包描述里的一个字段,或环境契约里的一个显式关闭),而不是靠某个工具重写 spec 去
对抗另一个工具的默认值。

### E2 — 构建侧:契约 + 工具链无关的实现

#### E2a 契约层:声明一个环境变量

`subos_info.envs` 已经是那一层——per-subos、有版本、任何工具可读,现在里面已经有
`LIBGL_DRIVERS_PATH`、`__EGL_VENDOR_LIBRARY_DIRS` 这类由包声明的变量。加一个**由 subos
自己声明**的:

```
XLINGS_SUBOS_LIB = ${subosdir}/lib
```

比蹭 `XLINGS_BIN` + `/../lib` 干净,而且是**被声明的契约**而不是巧合可用的变量。

#### E2b 默认实现:链接器包装

```sh
exec <real-ld> "$@" -rpath "$XLINGS_SUBOS_LIB" -rpath-link "$XLINGS_SUBOS_LIB" --disable-new-dtags
```

`ld` 在我们的 binutils 载荷里,**任何调用它的驱动都受覆盖**(gcc / clang / rustc / 自定义)。

**已验证**:标签 `(RPATH)`,egl / gles2 / egl-surfaceless **全部 NVIDIA GPU**;
`gcc -lGL` 与 `gcc -lz` 在**用户零 flag** 下链接+运行 OK。

**参数必须在 `"$@"` 之后**——specs 会传 `--enable-new-dtags`,后出现者胜出。第一版
包装器把参数放在前面,结果标签仍是 RUNPATH,而当时测的是 GLX(本来就通)所以没暴露。

三个部分各自不可省(已验证):

| 部分 | 管什么 | 缺了会怎样 |
|---|---|---|
| `-rpath` | 运行期查找 | `libz.so.1: cannot open shared object file` |
| `-rpath-link` | 链接期解析输入 `.so` 自己的 DT_NEEDED | `libGLdispatch.so.0 not found`(「`-rpath` 兼作 `-rpath-link`」的常见说法在这里**不成立**) |
| `--disable-new-dtags` | 深层 dlopen 能否穿透 | 产物"能跑但图形静默走软件"——**比跑不起来更难发现** |

`LD_RUN_PATH` 不是备选:已验证无效,因为 ld 只在**没给 `-rpath`** 时才读它,而我们自己的
specs 总是给 `-rpath`。

### E3 — 可观测性补完 —— **已落地**

**E3a 驱动漂移接入通道。** `xlings-gl-doctor` 已经能判断且**实测有效**:

```
built for driver   550.144.03
host driver now    550.144.03
interposers        4/4 entry points
```

缺的是它要用户自己想起来跑。应与接线记录一样,在 `subos info` 里自动说话——
读状态、不启动子进程,沿用 2026.8.10.3 立的即时查询契约。

**E3b 沙箱缺 GPU 时声明。** → #533。判据两个条件都已存在:
`read_graphics_wiring(subosDir).has_dispatch()` + `subos/gpu.cppm` 已知的设备节点。
**不做**:默认打开 `--gpu`,设备直通该是显式决定。

### E4 — 验收矩阵制度化

`.agents/tools/graphics/matrix.sh` 已有 48 格 + 构建缺陷行 + 标签差分行。要做的是把它
从"人想起来才跑"变成**有硬件的机器上定期跑**,并把结果与 `.wiring` 记录对账
(`graphics-acceptance.sh` 已实现该对账)。

harness 自身的两条硬规矩(都是它自己踩过的):**待测集必须由声明驱动而非目录列举**;
**`not-measured` 一律算失败**。

### E5 — 跨硬件 / 跨平台覆盖

当前全部结论来自一台 NVIDIA / X11 / x86_64。未覆盖:AMD、Intel、Wayland、aarch64、
无 GPU 机器、CI。矩阵工具已经是可跑的,缺的是**在别的机器上跑一遍**。

E1 的标签翻转是**全平台**行为变更,E5 是它的前置风险评估而不是收尾工作。

## 5. 依赖关系与顺序

```
E2a 契约声明 ──┬─→ E1a elfpatch --force-rpath ──→ E1b rule E(WARN)──→ E1b(硬门禁)
               └─→ E2b 链接器包装                      ↑
                                             E1c 显式退出 ─┘

E3a/E3b 可观测   独立,可并行,收益立即
E4 矩阵制度化    独立
E5 跨硬件覆盖    E1b 转硬门禁之前必须完成
```

**关键顺序**:E1a 必须在 E1b 之前(否则 68 个包同时报警);E5 必须在 E1b 转硬门禁之前
(否则在未验证的硬件上把安装拦死)。

## 6. 验收判据(可执行)

| 项 | 判据 |
|---|---|
| E1a | ✅ 已验:强制重装 libxkbcommon / wayland 后可执行文件 `(RPATH)`、同包的库仍 `(RUNPATH)` |
| E1b | ✅ 已验:`closure_guard_differential` 的钻机用**不带 `--force-rpath` 的 `--set-rpath`** 复现「来自旧客户端的载荷」,rule E 在真实安装中发声并说出代价。见 §5.1 |
| E2 | 用户零 flag:`gcc -lGL` 链接+运行 OK;新建程序四个入口点到 GPU |
| E3a | ✅ 已验:版本一致时无该行、伪造不一致时有、还原后又无 |
| E3b | ✅ 已验:不带 `--gpu` 响;带 `--gpu` 不响;无图形栈的 home 不响 |
| E4 | 矩阵 48 格无 `UNMEASURED`;记录与加载器逐条一致 |
| E5 | 至少一台非 NVIDIA、一台无 GPU、一台 aarch64 上跑过矩阵 |

每条判据都必须**先证伪再采信**——把实现故意改坏,确认对应判据变红。

### 5.1 落地后新增的一条经验:一致的写入者会让它的验证器难以观察

rule E 落地后,**装任何东西都产出 DT_RPATH**,于是「rule E 会发声」这一幕**无法用安装
来构造** —— 它只出现在两种情况:被更旧的 elfpatch 打过标签的载荷,以及自己覆盖标签的
recipe。仓库里每个有 `bin/` 的包都有 `lib/`,所以 elfpatch 总会打标签。

这不是缺陷,但有一个必须处理的后果:**一个永远不发声的检查,和一个坏掉的检查,输出
完全相同。**

所以 rule E 的证据不能是「装了很多包都没报警」,必须是一个**故意造出违规载荷**的钻机。
`closure_guard_differential` 正是它:真 gcc 编译、PT_INTERP 指向载荷 glibc(form X)、
经 fixture recipe 真实安装,加一条不带 `--force-rpath` 的 `--set-rpath` 就复现了旧客户端
的产物。钻机**先断言自己的形态**(`readelf` 必须显示 RUNPATH),否则将来 patchelf 行为
一变它就静默空转。

同一形态本轮出现三次,值得单列:**要验证「不该发生的事没发生」,必须让它发生一次。**

## 7. 不做什么

| | 为什么 |
|---|---|
| `LD_LIBRARY_PATH` | 被所有子进程继承,包括不属于我们的宿主程序。污染半径最大 |
| `<subos>/lib` 进 `ld.so.cache` | 粒度对不上:cache 是 per-glibc-载荷,farm 是 per-subos;且我们 ld.so 的 cache 路径是构建机 home,永不存在 |
| 逐包 `--force-rpath` | 已在做,实测通过率 **1/73**。判据("需不需要传递标签")**在二进制上看不见**——真实 GL 程序的 `dlopen` 引用数是 **0**,是 `libGLX.so.0` 在调 |
| 改 gcc specs 承载契约 | §3.1 |
| 默认打开 `--gpu` | 设备直通应当是用户的显式决定 |
| 为了让面板变绿去补 interposer 闭包 | 已验证是负收益:它把 surfaceless 从"经 zink 拿到 GPU"打成彻底失败 |

## 8. 这套方案的边界

**能做到的**:在已验证的这类机器上,四个 GL 入口点 + Vulkan + 无显示离线全部到 GPU,
环境内能构建 GL 程序,任何降级都有通道说出来,且偏离在装机时被系统发现。

**做不到 / 未证的**:
* 一台机器的结论;E5 之前不应把 rule E 转成硬门禁。
* 标签翻转对已装生态的实际影响没测——需要全量重打前后各跑一遍矩阵 + e2e。
* 宿主驱动大版本升级后的行为只有检测,没有自动修复(`gl-doctor` 会说,不会修)。

**不需要的**:改 NVIDIA、改 glvnd、改内核、绕开 EULA、拥有内核模块。今天的全部路径
都只动我们自己的标签和自己的链接器。
