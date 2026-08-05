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
| P2 用进程全局机制满足单库需求 | libc 上 `LD_LIBRARY_PATH` 杀死 shell | 每个 host-link 类包都会重演,且各自答案不同 |
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

**提议 A1**:把这五条写进 `xim-pkgindex/docs/V2/xpackage-spec.md` 的规范正文(目前只在 xlings 的设计文档里),并给每条配一个可执行判据,比如 R3 的判据:

> 一个修复如果只是让两个独立答案**更可能一致**,它是 workaround。只有**删掉第二个回答者**才是解决。
> libxpkg 0.0.49 没通过这条(它让扫描取最高版本而不是直接失败,仍与 `pin_target_to_active` 分歧);0.0.50 通过了。

**提议 A2**:契约文档里**禁止**"缺省即约定"式措辞。凡是"没有 X 就回退到 Y"的句子,要么改成"X 必须存在"(写端保证全量),要么改成"没有 X 是错误"。

### 1.4 下一个还没修的实例

**"一个 dlopen 进来的宿主文件,去哪里找它的依赖?"** 目前有三个答案,见 §2。

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

### 2.3 三个方案

#### 方案 A:推导闭包,保留机制

安装时读 vendor 的实际 DT_NEEDED,求**传递闭包**,在已解析依赖的 payload 里查找,减去"每个进程必然已加载的那一对"(`libc.so.6` + loader)。

```
需要提供的集合
  = 传递闭包(所有 nvidia 入口的 DT_NEEDED)
  ∩ 我们的依赖载荷能提供的
  − {libc.so.6, ld-linux*/ld-musl*}
```

- 消灭两张手写表(SONAME 列表、包→文件名映射)。
- 换驱动版本自动跟上——这正是 `__nvidia_entries` 已经在做的事,只是把它贯彻到依赖侧。
- 排除规则从"一张清单"降为**一条有理由的规则**:凡是每个动态进程在到达任何 dlopen 之前必然已绑定的,搜索路径上放它没有意义;而它恰好也是放上去会致命的那一个。
- 顺带补上五个泄漏。

代价:安装时要跑 N 次 `patchelf --print-needed`(recipe 已可直接调用 patchelf,`godot.lua` 有先例)。NVIDIA 用户态约 80 个文件,一次安装内可接受。

**不解决 P2 本身**——`LD_LIBRARY_PATH` 仍是进程全局的,只是内容正确了。

#### 方案 B:拷贝 + RPATH,消灭机制

把 vendor 库**拷贝**进我们的 payload,我们就拥有副本,可以 patchelf 打 RPATH。于是完全不需要 `LD_LIBRARY_PATH`,每个库只找自己的依赖,不对任何其他进程施加任何东西。

**不推荐**,两个理由:

1. **327MB**(实测宿主 NVIDIA 用户态体积)。
2. 更关键:**打破用户态与内核模块的版本耦合**。NVIDIA 用户态必须与正在运行的内核驱动严格匹配。符号链接总是跟随宿主;拷贝会在宿主更新驱动后失配,表现为运行时错误而不是安装时错误。这个包叫 `host-link` 正是因为这个耦合是它的设计核心。

方案 B 用"驱动更新后的正确性"换"隔离性"。这个交换在这里不划算,**但结论应当写进 recipe**,否则下一个人会重新讨论一遍。

#### 方案 C:承认是结构性妥协,收窄爆炸半径

`xlings-deps` 上 `LD_LIBRARY_PATH` 是**结构性妥协,不是实现细节**。既然妥协要保留,就必须把它的爆炸半径限定住并且可见:

- xlings 侧拒绝把含 libc 的目录放上全局搜索路径(已实现,commit `e486364`)。这不是第二个回答者——它不**决定**任何事,它**拒绝**。属于 R4"对产物断言"。
- 目录粒度是诚实边界:`LD_LIBRARY_PATH` 只能整目录取舍。要做到文件级就得物化一个过滤后的镜像目录,那会成为"vendor 的依赖在哪里"的**第三个回答者**——正是 P1 在拆的东西。

### 2.4 推荐

**A + C 落地,B 记录为不采纳及理由。**

**提议 B1**:`nvidia-gl-host-link.lua` 的依赖收拢改为闭包推导(方案 A),排除规则表述为一条规则而非一张清单。

**提议 B2**:把闭包推导做成 **libxpkg 的公共能力**而不是 recipe 的私有代码。`libcuda-host-link` 是同一模式的第二个实例——实测它**既不收拢依赖也不声明 `LD_LIBRARY_PATH`**,也就是说它的依赖今天全部来自宿主。两个 sentinel,同一个问题,两个不同答案:这已经是 P1 在这一层的实例。一个 `elfpatch.host_link_closure(opts)` 让两者共用一个实现。

**提议 B3**:规范里写明——**任何 `subos.env` 对 `LD_LIBRARY_PATH` / `LD_PRELOAD` 的声明都是特权操作**,需要在 recipe 里写明为什么 RPATH 不适用。目前全索引只有 1 处这样的声明(实测:`LD_LIBRARY_PATH` × 1,其余是 `LIBGL_DRIVERS_PATH`、`__EGL_VENDOR_LIBRARY_DIRS` × 2、`XDG_DATA_DIRS`),现在立规则的成本最低。

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

## 7. 落地顺序

依赖关系决定顺序,不是优先级:

```
D3 (host-link 解析结果持久化)  ─┐
                                ├─→ B1/B2 (闭包推导)  ─→  补齐五个泄漏
D2 (三个数的报告)              ─┘

C3 (doctor 报双绑定)  ─→  C1 (单版本执行点)  ─→  C2 (envs 派生)

E1/E2 (隔离 home + 契约断言)  ─→  独立,应当最先做,因为它决定后面所有验证是否可信

A1/A2 (规范化五条规则)  ─→  独立,成本最低,防止新缺陷
E3 (双工具链门禁)      ─→  独立
```

**建议先做 E1/E2 和 A1/A2**:前者让后续所有验证可信,后者阻止新的回答者被引入。B 和 C 两条线可以并行。
