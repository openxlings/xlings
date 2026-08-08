# xlings 生态闭环:一个进程只有一个 libc

> 2026-08-09 · 承接 `2026-08-08-three-open-items-analysis-and-plan.md` §1、
> `mcpp-community/mcpp#392`、`xim-pkgindex#578`(已回滚)
> 目标:把「我们的 loader / 我们的 libc / 宿主库」三者的关系一次性定清楚,
> 并给出**安装期就能判定**的闭环契约

---

## 0. 两个提法,都对,而且第二个更根本

> 「mcpp run 报 `GLIBC_2.42 not found`,是不是也是缺少库导致的?」

**报错文本的直接原因是版本地板**:宿主的 `/lib64/libtinfo.so.6` 一直在,宿主 glibc
2.43 也比它要的 2.42 新;是**我们的 libc 2.39 太旧**,给不出那个符号版本。

> 「难道不是 xlings 生态缺少这个库才走到 host 的吗?按理能不用 host 就不用 host」

**对,而且这是更根本的那一层。** 我第一版把它写成「不是缺库」,矫枉过正了:
如果闭包里根本没有宿主库,版本地板就永远不会被触发。实测:

```
xim 主索引:      没有 ncurses / libtinfo
子索引 scode:    有 ncurses.lua
本机 xim-x-xmake/3.0.8 的 xmake:  NEEDED libtinfo.so.6
```

所以两层要分开说:

| 层 | 问题 | 修法 |
|---|---|---|
| 表层 | 我们的 libc 满足不了宿主库 | 抬版本地板 |
| **根层** | **宿主库为什么会进闭包** | **补齐闭包,让它根本不进来** |

**只修表层是把 bug 换个版本号继续等着。** 下面的方案以根层为主。

### 0.1 而且这条契约仓库里已经有了

`xim-pkgindex/.github/scripts/dep-closure-check.sh` 的 **D2** 写的正是这件事:

```
用我们的 loader 的载荷  →  host-only soname 是 hard failure
还在宿主 loader 上的载荷 →  host-only 是 note,"the documented arrangement, not a leak"
```

理由和 §3 我测到的一致(ld.so cache 路径不存在,切过去就没有宿主回退)。
**契约是对的,不需要重新发明。** 真正的缺口是它管得到谁——见 §4.4。

---

## 1. 唯一的物理约束

一个进程**只有一个动态加载器、一个 libc**。进程里每一个被载入的 .so 都必须与那一份
libc 兼容。所有故障都是这条约束的推论。

glibc 的兼容性是**单向**的:

```
新 libc 能满足旧库的符号要求      ✔  (向后兼容)
旧 libc 不能满足新库的符号要求    ✘  (不向前兼容)
```

于是有第一条硬规则:

> **规则 A(版本地板)**:若进程里会载入任何宿主库,则
> `our_glibc >= max(host_glibc, 所有被载入宿主库要求的最高 GLIBC_x.y)`

以及第二条:

> **规则 B(同源)**:`PT_INTERP` 指向的 ld.so 与实际载入的 `libc.so.6`
> **必须来自同一份 glibc 载荷**。

---

## 2. 四种组合,三种失败,而且症状各不指向病因

全部实测,不是推演:

| # | loader | libc | 载入的库 | 结果 | 实测症状 |
|---|---|---|---|---|---|
| 1 | 宿主 | 宿主 | 宿主 | ✔ | 今天的默认 |
| 2 | **宿主** | **我们的** | — | ✘ 段错误 | `__vdso_gettimeofday` + `Segmentation fault`(`xim-pkgindex#578`,已回滚) |
| 3 | 我们的 | 我们的 | **宿主(要求更新)** | ✘ 起不来 | `libc.so.6: version 'GLIBC_2.42' not found (required by /lib64/libtinfo.so.6)`(`mcpp#392`) |
| 4 | 我们的 | 我们的 | 宿主(SONAME 查找) | ✘ 找不到 | `UnsatisfiedLinkError: libawt_xawt.so: libX11.so.6`(2026.8.8.1) |

**没有一条症状指向病因**:2 报的是段错误、3 报的是「库缺符号」、4 报的是「缺库」。
三条都会让人去补包,而只有 4 是真的和「有没有那个包」有关。

组合 2 的存在尤其要记:它是**只声明依赖、不切 loader** 的直接产物——把我们的
glibc 写进 RPATH 而 `PT_INTERP` 仍是宿主的。**「安全的中间态」不存在。**

---

## 3. 一个根:载荷里带着打包机的绝对路径

三处独立故障,同一台机器:

```
gcc specs      /home/xlings/.xlings_data/subos/linux/lib/ld-linux-x86-64.so.2
ld.so cache    /home/xlings/.xlings_data/xim/xpkgs/fromsource-x-glibc/<ver>/etc/ld.so.cache
llvm clang.cfg …/xim-x-glibc/2.39/…            (mcpp#392 记录)
```

- 第一条造成 `xlings#509` 的 aarch64 ENOENT(16 轮 CI、4 个错误修复)
- 第二条造成组合 4:**我们的 ld.so 没有可用的 cache**,切过去之后宿主库
  **只能靠 RPATH/LD_LIBRARY_PATH 找到,SONAME 查找整体失效**
- 第三条把 2.39 焊死进工具链配置

> **规则 C**:载荷中任何指向绝对路径的东西(specs、cfg、ld.so cache、RPATH)
> 都必须在**安装机**上重新生成或验证,不能相信打包机写下的值。

---

## 4. 闭环契约

### 4.1 定义边界:core 与 leaf

```
core  = ld.so + libc + libstdc++/libgcc_s + 直接被它们约束的运行时
        → 必须整套来自我们,不可混用
leaf  = 一切其他库(X11、ALSA、freetype、fontconfig、mesa、ncurses…)
        → 可以来自我们,也可以来自宿主,但受规则 A 约束
```

判据不是「重不重要」,而是**「换掉它会不会改变 libc 契约」**。

### 4.2 两种合法形态,不存在第三种

**形态 H(host-anchored)**——今天的默认,也是最省事的:

```
PT_INTERP = 宿主 ld.so
libc      = 宿主
leaf      = 宿主为主;我们的 leaf 可用,但 RPATH 里绝不能出现我们的 glibc
```

**形态 X(xlings-anchored)**——目标形态:

```
PT_INTERP = 我们的 ld.so
libc      = 我们的(同一份载荷)
leaf      = 我们的优先;允许宿主 leaf,但必须满足规则 A
```

**任何二进制必须整体处于 H 或 X,不能一半一半。** 组合 2 就是「一半一半」。

**2026-08-09 定调(见 C5)**:**X 是目标形态,H 降级为过渡态。** 已有的 H 可以留着,
但新东西不得停在 H,且 X 下的宿主库只能经 `*-host-link` 进入。

### 4.3 形态 X 的三个前置条件

1. **版本地板**:`our_glibc >= host_glibc`。今天默认 runtime 是 `glibc@2.39`,
   而 Ubuntu 24.04 就是 2.39、更新的发行版已到 2.43 —— **默认值等于「在任何比
   打包机新的系统上,形态 X 都会踩规则 A」**。这不是个别用户的环境问题,是默认值选错了。
2. **cache 可达**:我们的 ld.so 必须能找到一份真实的 `ld.so.cache`,否则宿主 leaf
   只能靠 RPATH 逐个点名(组合 4)。
3. **闭包已知**:切之前必须知道这个二进制会载入哪些库——**包括 dlopen 的**。
   DT_NEEDED 不够:fontconfig 就是被 dlopen 的,任何只看 NEEDED 的清单都会漏掉它。

### 4.4 缺口:D2 管的是索引包,而 mcpp 把**用户二进制**放进了形态 X

这是 `mcpp#392` 真正的位置,也是 §0 那个问题问出来的东西。

D2 的判据是对的,但它只在 **xim-pkgindex 的 CI** 上、对**索引里的包**运行。
而 `mcpp#392` 里崩掉的不是索引包,是**用户自己的项目产物**:

```
llvm@22.1.8/bin/clang.cfg  硬编码
    -Wl,--dynamic-linker=…/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
    -Wl,-rpath,…/xim-x-glibc/2.39/lib64
```

**默认工具链把每一个用户二进制都编进形态 X** —— 也就是契约最严格的那一侧 ——
而那一侧:

- 没有 D2 检查(它不是索引包,CI 看不到)
- 没有闭包保证(`xim` 主索引里根本没有 ncurses/libtinfo)
- 没有宿主回退(ld.so cache 路径不存在)

于是用户链上任何一个我们没打包的库,都会在**运行时**炸,报一个指向 glibc 版本的错误。

**这才是「能不用 host 就不用 host」这句话真正的落点**:不是「多打几个包」,
而是——

> **谁把二进制放进形态 X,谁就要为那个二进制的完整闭包负责。**

今天是 mcpp 的默认配置把它们放进去的,但负责闭包的机制只覆盖索引包。**责任和能力
不在同一处**,这是本文档全部四种故障里唯一的结构性问题,其余都是它的表现。

两个方向,必须选一个(见 §5 的 C5):

- **X-完整**:凡是工具链可能链进去的库,索引必须全有(ncurses 是当前已知缺口),
  并对用户产物也跑一次 D2
- **H-默认**:用户二进制默认编成形态 H(宿主 loader + 宿主 libc),
  形态 X 变成显式 opt-in,并只在闭包被证明完整时才允许

**倾向 H-默认**:形态 X 的价值是可复现和可移植,而它今天既不可复现(依赖宿主
有没有那个库)也不可移植(依赖宿主 glibc 够不够新)。在闭包补齐之前,
它拿到的是两者的缺点。

---

---

## 5. 方案

### C1 — 把默认 runtime 的版本地板提上来(最高优先)

`DEFAULT_RUNTIME = "glibc@2.39"`(`src/core/subos/manifest.cppm`)。
2.39 是 2024 年的版本;宿主一旦更新,形态 X 必然失败,而失败信息完全不指向版本。

**做法**:默认改为当前打包的最高版本(今天是 2.44),并在 subos 创建时
**记录当时的宿主 glibc 版本**,供 C2 判定。

**不是简单改常量**:已有 home 的 `runtime` 字段已经写死,需要迁移路径;
`mcpp#392` 的报告者是手工改 `.xlings.json` + 删 2.39 + 全量重建才走通的,
这条路不能要求用户走。

### C2 — 安装期判定,而不是运行期崩溃

三种失败今天都是**装完报成功、跑起来才炸**。全部可以在安装期判出来:

```
对每个即将写入 RPATH / 切换 INTERP 的 ELF:
  1. 规则 B:INTERP 的载荷 == RPATH 里提供 libc 的载荷?否 → 拒绝
  2. 规则 A:our_glibc >= host_glibc?否 → 拒绝进入形态 X
  3. 形态一致性:同一个包内所有 ELF 必须同形态,不得混合
```

第 1 条已经在 `xim-pkgindex` 的 `posix-test.sh` 里补上了(#581),
但那是 **CI 的检查**,不是**客户端的守卫**——用户本地装的时候没人拦。

### C3 — 让我们的 ld.so 有一份真实的 cache

三个可选,按侵入性排序:

- **C3a** 安装 glibc 时用我们的 `ldconfig` 在**安装机**生成 cache 到载荷内,
  并确保 ld.so 编译时的 cache 路径指向该位置(即规则 C 应用到 cache)。
- **C3b** 不用 cache,改为在切形态 X 时把**宿主库目录**显式写进 RPATH 尾部。
  简单,但 RPATH 会变长,且宿主目录因发行版而异。
- **C3c** 完全闭包:形态 X 下不允许任何宿主 leaf。最干净,但要打的包最多,
  而且 §1 的 D5 测量已表明**并不需要**——我们的 freetype 只依赖 libc,
  brotli/bz2/png 会自动离开闭包。

**倾向 C3a**,因为它同时消掉规则 C 的这一处实例;C3b 作为过渡。

### C5 — 决定:**X-完整**,唯一例外是宿主闭源/硬件库(已拍板 2026-08-09)

> loader / libc / 库 **都该是我们的**,除了极少数宿主闭源硬件库。

这把形态 H 从「另一种合法形态」降级为**过渡态**:允许存在,但不是目标,
而且任何新东西不得停在那里。

**例外走且只走一个通道:`*-host-link` 哨兵包。** 这个机制已经存在,不需要发明:

```
pkgs/n/nvidia-gl-host-link.lua    NVIDIA 专有 GL/EGL
pkgs/w/wsl-gl-host-link.lua       WSL2 的 GL
pkgs/l/libcuda-host-link.lua      libcuda
```

它们**无载荷**,做的是「把宿主库以稳定符号链接接进我们的 sysroot 并声明为 sysroot
资产」,于是由**我们控制的 RPATH** 解析。

这带来一个重要简化:**宿主库不再需要 ld.so.cache 就能被找到**,因为它们是以
显式路径进入我们的 sysroot 的,不走 SONAME 全局查找。**C3 因此退出关键路径。**

判据也随之变成一条可机械判定的规则:

> **规则 D**:形态 X 的二进制,其闭包里的每一个 soname 必须由**索引里的包**提供;
> 唯一豁免是由**已声明的 `*-host-link` 依赖**提供的那些。
> 没有第三种来源。

#### 这个决定没有取消版本地板,反而让它永久化

`*-host-link` 接进来的是**宿主编译的**库(NVIDIA 的 `libGLX_nvidia.so` 等),
它们需要**宿主 glibc 的符号**。所以只要硬件例外存在:

> **`our_glibc >= host_glibc` 是永久约束,不是过渡期措施。**

这是本决定最重要的推论:**C1 留在关键路径上**,而且性质从「修一个 bug」变成
「我们的 glibc 必须持续跟进所支持发行版的最高 glibc」。默认值停在 2.39 就是
今天 `mcpp#392` 的原因,而它会随每个新发行版复发。

#### 已知要补的闭包缺口

```
ncurses / libtinfo    xim 主索引没有(scode 有);xmake、llvm 工具链会链到
```

这是**当前已知的一个**,不是全部。规则 D 落地后,缺口会由 CI 逐个报出来——
这正是它比人工清单可靠的地方。

### C4 — 闭包必须包含 dlopen

`DT_NEEDED` 不是闭包。fontconfig 由 libfontmanager `dlopen`,任何静态清单都看不到。

**做法**:形态 X 的验收统一用 **运行期 provenance**:

```
LD_DEBUG=libs <cmd> 2>&1 | grep "calling init:"
```

断言里面没有宿主路径(或只剩明确允许的 leaf)。这条**今天跑 JDK 会失败**
(15 个宿主对象),这正是它有价值的证据。

---

## 6. 落地顺序

```
C5  ✔ 已定:X-完整 + host-link 例外
C1  默认 runtime 版本地板     ← 现在是永久约束,不是一次性修复。最高优先
C2  规则 D 的守卫,且要覆盖用户产物,不只索引包
C6  补 ncurses(已知缺口),之后由 CI 报出其余
C4  provenance 验收固化       ← 唯一能看见 dlopen 的验收
C3  ld.so cache               ← 退出关键路径(host-link 用显式路径,不走 cache)
然后才谈 JDK 切 loader(D5-4)
```

顺序变了:C5 定了 X-完整之后,**C1 从「可选」变成「永久且最高优先」**——因为
硬件例外会一直把宿主库带进来,而它们要宿主 glibc 的符号。C3 相反,因为
host-link 机制不依赖 cache,降级为「以后再说」。

**顺序不能换。** 先切 loader 再补这些,就是 2026.8.8.1 和 `#578` 已经付过两次的学费。

---

## 7. 验收(全部可证伪,今天跑都会失败)

| 项 | 断言 | 今天的结果 |
|---|---|---|
| 规则 B | 任一 ELF 的 INTERP 载荷 == 其 libc 载荷 | `#578` 的产物会失败 |
| 规则 A | `our_glibc >= host_glibc` | 默认 2.39 在 2.43 宿主上失败 |
| **规则 D** | **形态 X 的闭包里每个 soname 由索引包或已声明的 host-link 提供** | **xmake 的 libtinfo 无提供者,失败** |
| C4 | `LD_DEBUG` provenance 除 host-link 外无宿主对象 | JDK 15 个宿主对象,失败 |

组合 4(SONAME 找不到宿主库)不再列为验收项:X-完整下宿主库只经 host-link 的
显式符号链接进入,本就不走 SONAME 全局查找。

---

## 8. 一条贯穿线

这份文档里四种失败、三处绝对路径、两条规则,底下是同一件事:

> **系统允许一个二进制处于「一半我们、一半宿主」的状态,不在任何环节拦截它,
> 并把随之而来的崩溃报成一个完全不相关的现象。**

和 `2026-08-08-declared-vs-effective-open-defects-design.md` §3 是同一个形状,
只是这次不出声的不是「声明」,而是**运行时契约**。

所以修法也是同一条:**把契约变成安装期就能判定的谓词,判不过就拒绝,而不是让它跑起来再炸。**
