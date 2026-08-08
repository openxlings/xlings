# xlings 生态闭环:一个进程只有一个 libc

> 2026-08-09 · 承接 `2026-08-08-three-open-items-analysis-and-plan.md` §1、
> `mcpp-community/mcpp#392`、`xim-pkgindex#578`(已回滚)
> 目标:把「我们的 loader / 我们的 libc / 宿主库」三者的关系一次性定清楚,
> 并给出**安装期就能判定**的闭环契约

---

## 0. 先纠正一个提法

> 「mcpp run 报 `GLIBC_2.42 not found`,是不是也是缺少库导致的?」

**不是缺库。** 那个库(宿主的 `/lib64/libtinfo.so.6`)一直在,而且宿主的 glibc 2.43
比它要的 2.42 还新。**是我们的 libc 2.39 太旧,提供不了它要的符号版本。**

这个区分决定方案:缺库 → 补包;**版本地板不足 → 补包解决不了**,再打十个包也一样。

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

### 4.3 形态 X 的三个前置条件

1. **版本地板**:`our_glibc >= host_glibc`。今天默认 runtime 是 `glibc@2.39`,
   而 Ubuntu 24.04 就是 2.39、更新的发行版已到 2.43 —— **默认值等于「在任何比
   打包机新的系统上,形态 X 都会踩规则 A」**。这不是个别用户的环境问题,是默认值选错了。
2. **cache 可达**:我们的 ld.so 必须能找到一份真实的 `ld.so.cache`,否则宿主 leaf
   只能靠 RPATH 逐个点名(组合 4)。
3. **闭包已知**:切之前必须知道这个二进制会载入哪些库——**包括 dlopen 的**。
   DT_NEEDED 不够:fontconfig 就是被 dlopen 的,任何只看 NEEDED 的清单都会漏掉它。

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
C2  安装期守卫(客户端)     ← 先做。它让后面每一步的失败都变成可诊断的
C1  默认 runtime 版本地板    ← 用户可见,需要迁移路径
C3a ld.so cache 就地生成     ← 解掉组合 4 的根
C4  provenance 验收固化      ← 把「装完能不能跑」变成可证伪的断言
然后才谈 JDK 切 loader(D5-4)
```

**顺序不能换。** 先切 loader 再补这些,就是 2026.8.8.1 和 `#578` 已经付过两次的学费。

---

## 7. 验收(全部可证伪,今天跑都会失败)

| 项 | 断言 | 今天的结果 |
|---|---|---|
| 规则 B | 任一 ELF 的 INTERP 载荷 == 其 libc 载荷 | `#578` 的产物会失败 |
| 规则 A | `our_glibc >= host_glibc` | 默认 2.39 在 2.43 宿主上失败 |
| 组合 4 | 我们的 ld.so 能按 SONAME 找到宿主 leaf | 失败(cache 路径不存在) |
| C4 | `LD_DEBUG` provenance 无宿主对象 | JDK 15 个宿主对象,失败 |

---

## 8. 一条贯穿线

这份文档里四种失败、三处绝对路径、两条规则,底下是同一件事:

> **系统允许一个二进制处于「一半我们、一半宿主」的状态,不在任何环节拦截它,
> 并把随之而来的崩溃报成一个完全不相关的现象。**

和 `2026-08-08-declared-vs-effective-open-defects-design.md` §3 是同一个形状,
只是这次不出声的不是「声明」,而是**运行时契约**。

所以修法也是同一条:**把契约变成安装期就能判定的谓词,判不过就拒绝,而不是让它跑起来再炸。**
