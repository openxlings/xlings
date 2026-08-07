# 宿主 loader 迁移 + 生态守卫方案

> 2026-08-08 · 承接 `2026-08-07-graphics-experience-industry-survey-and-plan.md` §11
> 三块:**L** 宿主 loader 迁移 · **G** 静默成功守卫 · **E** 环境覆盖

---

## 0. 这份方案的由来

上一轮结束时我给出一个数字:「真实 home 里 205 个动态可执行文件,67 个(33%)
用宿主的 `/lib64/ld-linux-x86-64.so.2`」,并据此说"mcpp#352 在图形栈之外没被解决"。

那个数字对,**但结论下得太粗**。按包一分组,67 个集中在 **5 个包**里:

| 包 | 宿主-loader 二进制数 |
|---|---|
| `jdk-temurin` | 30 |
| `jdk-zulu` | 30 |
| `llvm-tools` | 3 |
| `code` | 2 |
| `python` | 2 |

**60/67 是两个 JDK。**这不是"生态三分之一坏了",是五个包没迁移完。下面的方案基于
实测,不是推断。

**一处必须先更正的测量偏差:`python` 那两个不是缺陷,是陈旧载荷。**
`xim:python` 在 2026-08-05 就修好了(pkgindex#507,已在无 `/usr` 的 bwrap 里验证);
本机两个 python 载荷装于 3 月和 6 月,**早于修复**。对真实 home 的静态扫描测的是
"装的时候是什么样",不是"现在装会是什么样"——这两者在这个生态里不是一回事。
python 这一格只需 `xlings install python` 重装。

同一个 commit 已经把这个类识别出来了:「索引 106 个 linux 载荷 recipe 里,94 个没有
声明 glibc,因此同样触发不了 predicate」。**所以下面的 L 部分不是新发现,是 #507
那句话的执行**。

---

## R. 这份方案要改哪些仓库

按「能在 xpkg 包侧做的就不动 xlings 本体」这条约束逐项核对过源码,结论:

| 项 | xim-pkgindex | libxpkg | xlings 本体 | 说明 |
|---|:---:|:---:|:---:|---|
| L0 `llvm-tools` | ✅ | — | — | 加 `deps = { "xim:glibc" }`,predicate 自动生效 |
| L1 两个 JDK | ✅ | — | — | 同上 + `elfpatch.set({extra_rpath=...})` |
| L2 `python` | — | — | — | **已修好**,只需重装复测 |
| L3 `code` | ✅ | — | — | recipe 注释定性为"有意依赖宿主" |
| NSS 覆盖告警 | ✅ | — | — | `glibc.lua` 的 `install()` 里比对 nsswitch |
| G1 deps↔DT_NEEDED | ✅ | — | — | CI workflow + 脚本 |
| G2 构建输入泄漏 | ✅ | — | — | `.agents/tools/graphics/build-in-subos.sh` |
| G3 三态退出码 | ✅ | — | — | `.agents/tools/` 规范 + 现有脚本 |
| G5 裸依赖名 | ✅ | — | — | CI 检查 + 62 处 recipe |
| E1 回收矩阵 | ✅ | — | — | 文档 + 汇总页 |
| **G4 `remove` 反向依赖** | — | — | **必须** | `src/core/xvm/removal.cppm`,包侧无法拦截 |
| **doctor `getpwuid` 格** | — | — | **必须** | 诊断命令在 `src/core/xself/` |

**11 项里 9 项纯 pkgindex,2 项必须动 xlings 本体。**这两项都不是绕不开包侧才去动
本体——它们本身就是本体的职责:一个是卸载命令的语义,一个是诊断命令的一格。

`libxpkg` 一行都不用改:loader 切换所需的 predicate、`closure_lib_paths()`、
`extra_rpath` 全都已经在 `elfpatch.lua` 里了。

---

## L. 宿主 loader 迁移

### L.1 判定切换风险的四个条件

把 `PT_INTERP` 从宿主 ld.so 换成 xlings 自己的,要同时满足:

| # | 条件 | 为什么 |
|---|---|---|
| C1 | 二进制要求的最高 `GLIBC_x.y` ≤ 我们的 glibc | glibc **向后**兼容,新的能跑旧的;反过来不行 |
| C2 | 硬依赖(启动即加载的 `DT_NEEDED`)载荷能满足 | 否则起不来 |
| C3 | NSS 能工作 | glibc 会 `dlopen libnss_*.so.2`,**必须和 libc 同源**;缺模块 → `getpwnam` 静默失败 |
| C4 | locale / gconv 可用 | 否则字符集退化成 C |

**C3 是唯一真正会咬人的**,而且它的失败方式是安静的——用户名查不到、DNS 解析不了,
不报错只是行为不对。这也是发行版从不让人乱换 loader 的原因。

### L.2 实测结果

我们的 `glibc@2.44`,NSS 模块有 `compat db dns files hesiod`;
宿主 `nsswitch.conf` 点名 `db dns files mdns4_minimal nis systemd`
→ **`systemd` / `mdns4_minimal` / `nis` 我们没有**。

但真跑起来是通的,因为普通用户走 `files`:

```
$ <我们的 ld.so> --library-path <jdk>:<glibc> java -XshowSettings:properties -version
    os.name   = Linux
    user.name = speak          ← getpwuid 通过 libnss_files,成功
    user.home = /home/speak
```

四个包的完整测量(全部 ELF,不只启动器):

| 包 | 最高 GLIBC | 包外 `DT_NEEDED` 载荷不满足 | 不满足的是谁需要的 | 判定 |
|---|---|---|---|---|
| `llvm-tools` | 2.34 | **0** | — | **L0 可直接切** |
| `jdk-temurin` / `jdk-zulu` | **2.17** | 6 | 全在 `libawt_xawt.so` / `libsplashscreen.so` / `libjsound.so` | **L1 可切** |
| `python` | 2.28 | 12 | `libcrypt` + MPI/RDMA/TBB 等可选加速库 | **L2 有条件** |
| `code` | 2.34 | **27** | GTK / dbus / gbm / atk —— 整个桌面栈 | **L3 不切** |

关键在 JDK 那一行:`bin/java` 的 `DT_NEEDED` 只有 `libjli.so` + `libc.so.6`,两个都
满足;缺的 6 个(`libX11 libXext libXi libXrender libXtst libasound`)属于 **AWT、
启动画面、声音**——都是按需 `dlopen` 的组件。**ld.so 只在真正加载那个 `.so` 时才
会失败**,所以无头/服务端用法完全不受影响,GUI 用法的降级程度和今天一模一样
(切换后它们仍会经 `/etc/ld.so.cache` 找到宿主的 libX11,而我们的 2.44 ≥ 它们要的
版本,所以照样能加载)。

实测三个都跑通:

```
java 25.0.4      ✓   user.name/user.home 正确
python 3.12.13   ✓   pwd.getpwuid 正确,locale = utf-8(没有退化成 C)
clangd/clang-format/clang-tidy  ✓
```

### L.3 分级切换方案

**L0 — `llvm-tools`(零风险,先做)**

0 个不满足项。**不需要写任何 `install()` 代码**——加一行依赖声明就够了:

```lua
xpm = {
    linux = {
        deps = { "xim:glibc" },   -- ← 全部改动
        ...
```

机制在 `libxpkg/elfpatch.lua`:`_resolve_predicate()` 扫描本包的 runtime deps,
只要其中有包 export 了 `runtime.loader`(`glibc.lua` 正是这么声明的:
`exports.runtime.loader = "lib64/ld-linux-x86-64.so.2"`),xlings 就在 post-install
自动改写 `PT_INTERP` 并把 `closure_lib_paths()` 写成 RPATH。**这是 xlings 已有的
默认行为,recipe 侧不声明依赖才是它不生效的原因。**

> 我上一版这里写的是 `elfpatch.patch_elf_loader_rpath(..., loader = "auto")` —— **错的**。
> `_resolve_loader()` 只认 `"system"` / `"subos"`,其它值原样当作绝对路径回传,
> 于是会执行 `patchelf --set-interpreter auto`,**把二进制打坏而且不报错**
> (`opts.loader and not loader` 为假,警告分支进不去)。低层 API 在这里既不必要
> 也危险,声明式路径才是对的。

**L1 — 两个 JDK(补两个包后做)**

同样是加 `deps = { "xim:glibc" }`,但**多一个必须做的动作**:

```lua
elfpatch.set({ extra_rpath = { "$ORIGIN", "$ORIGIN/../lib" } })
```

原因:predicate 路径最终走 `patchelf --set-rpath`,是**替换**不是追加,而
`bin/java` 出厂就带 `$ORIGIN:$ORIGIN/../lib`(实测),`libjli.so` 靠它找到。
`closure_lib_paths()` 只 push `<install_dir>/lib` 这一层绝对路径,**不含 `lib/server`**
(`libjvm.so` 在那里,靠 libjli 显式拼路径加载,所以不致命,但把 `$ORIGIN` 留住是
零成本的正确做法)。

`libXtst`、`libasound` 打包是**独立的、可以不做**的事——不做的差别只是 AWT/声音继续
落宿主,而那正是现状。**两件事不要互相阻塞。**

**L2 — `python`(已修好,只需重装)**

见 §0 的更正:recipe 侧 2026-08-05 已经加了 `deps = { "xim:glibc@>=2.39" }`,
本机测到的是 3 月/6 月的旧载荷。**本项无代码改动**,只需 `xlings install python`
重装并复测 `readelf -p .interp`。

(原先根据旧载荷得出的"12 个不满足项、需补 `libxcrypt`"仍然成立于那份载荷,但
其中只有 `libcrypt.so.1` 是 CPython 标准库要的,`libmpi/libucp/libfabric/libibverbs/
libtbb` 都是 numpy/scipy 在 HPC 环境才 `dlopen` 的加速后端。补 `libxcrypt` 是
**可选增强**,不是 loader 迁移的前置。)

**L3 — `code`(明确不切,并写进 recipe 注释)**

VS Code 是 Electron,27 个不满足项覆盖 GTK/atk/dbus/gbm——它**按设计**就要用宿主
桌面栈。强行自持等于把半个桌面环境打进包里。这一格应该在文档里标成
**"有意依赖宿主"**,而不是留着当未完成项。

### L.4 必须同时做的守卫(否则这条路会重演)

切 loader 会让 NSS 从"用宿主的"变成"用我们的",而 C3 的失败是安静的。所以:

1. **glibc 包补 `libnss_*` 覆盖检查**:安装后比对 `/etc/nsswitch.conf` 点名的模块
   与载荷里有的,缺的**告警**(不是失败——`systemd`/`nis` 缺失在绝大多数机器上无害)
2. **`xlings doctor` 加一格**:`getpwuid(geteuid())` 能否返回当前用户名。一行调用,
   直接把 C3 从"安静失败"变成"一句话报出来"

---

## G. 静默成功守卫(对应上一轮 §二)

上一轮一个 session、一个子系统,同一类缺陷出现 **8 次**:声明了没人消费、
`skip()` 返回 0、空载荷返回 true、CI 17 秒绿、构建输入用宿主库、
`remove` 无反向依赖检查、62 处裸依赖名、探针缺 `-rpath-link` 报错指错子系统。

**这不是八个 bug,是一个 bug 出现八次。**共同形态:**「没发生」和「成功了」输出相同。**

按"能挡住多少种"排序,而不是按实现难度:

### G1. declared deps ⟷ 真实 `DT_NEEDED` 差分(挡住 #1 #7)

CI 装完一个包后,枚举其载荷所有 `.so`/可执行文件的 `DT_NEEDED`,映射回提供它的
包,与 recipe 声明的直接 runtime deps 求差:

- **多出来的**(实际需要但没声明)→ **fail**。这正是 libxcb 缺 libXau 那一类
- 少的(声明了但没用到)→ 告警

这一条如果早存在,上一轮的核心缺陷在提交时就会被挡住。

### G2. 构建输入侧的宿主泄漏检查(挡住 #5)

`build-in-subos.sh` 现在查**产物**有没有宿主引用,不查**构建输入**。已发布的 mesa
就是这样在"no host references"通过的同时,链了宿主的 LLVM;我这次构建 LLVM 时
cmake 又静默链了宿主的 `libzstd`(home 里根本没有 zstd 包)。

两种做法,建议前者:

- **把构建放进 bwrap,不挂 `/usr`**——和 `selfcontained-check.sh` 对运行期做的
  完全同构。过不了就是真的有宿主输入
- 或解析 `meson-log.txt` / `config.log`,任何 `/usr/` 命中即 fail

### G3. 三态退出码规范(挡住 #2 #4)

`skip() { exit 0; }` 让"没跑"打印成 `✓ S1-S4 pass`,这已经修了;但规范没写下来,
下一个脚本会再犯。**约定并写进 `.agents/tools/` 的 README**:

```
0 = 证明通过    1 = 证明失败    2 = 不可判定    3 = 本机无法执行
```

任何验证脚本的 `skip` 一律 **3**,调用方必须把 3 映射成"未执行",**不得**并入通过。

配套:CI 里同名 job 出现两次(一个真跑、一个 17 秒空转)必须在摘要里区分,
否则"绿"没有信息量。

### G4. 卸载的反向依赖检查(挡住 #6)

`xlings remove libxml2` 在 `llvm` 依赖它时成功了,`ld.lld` 随即起不来。
`remove` 应当在有已安装包依赖目标时**拒绝并列出依赖方**,`--force` 才继续。

### G5. 索引裸依赖名(挡住 #7)

62 处 `deps = { "expat@2.6.2" }` 这类没有命名空间前缀的声明。CI 加一条:
索引里存在同名包的裸依赖名 → fail。这条几乎零成本。

---

## E. 环境覆盖(对应上一轮 §三)

现状(实测,不是估计):

| 环境 | 状态 | 差什么 |
|---|---|---|
| Linux/glibc + NVIDIA + X11 | 已证明自持 | — |
| 软件渲染 | 已证明 | — |
| AMD radeonsi / nouveau | **载荷完整**,零验证 | 只差硬件 |
| Intel iris | 静默落 llvmpipe | clang 开发件 + SPIRV + libclc(LLVM 前置已解决) |
| WSL2 d3d12 | 静默落 llvmpipe | DirectX-Headers 已建好,mesa 重建进行中 |
| Wayland | 未知 | 索引里没有任何 compositor |
| 老发行版 / musl | **风险实测存在** | 见 L 部分 |
| macOS / Windows / aarch64 | 本轮未碰 | — |

### E1. 把"不可测"变成可收集

`verify-stack.sh --json` 已经能输出整张表。缺的是**回收**:一个
`.agents/tools/graphics/collect-matrix.md` 说明"在你的机器上跑这一条命令,把 JSON
贴到 issue",再加一个汇总页。三格硬件缺口靠社区补,而不是等我们买卡。

### E2. 优先级建议

**G1 + G2 应该排在任何新驱动之前。** 理由很直接:上一轮所有"通过"的证据,都是在
这两个守卫不存在的前提下取得的。在补上之前,增加覆盖面只是增加**未被验证的**覆盖面。

顺序建议:

```
G3(规范,最便宜) → G1(差分,挡住核心缺陷类) → G5(裸依赖名)
   → L0/L1(loader 迁移,收益最直接) → G2(构建输入) → G4
   → E1(回收矩阵) → iris / Wayland
```

---

## 附:本方案里每个数字的来源

| 数字 | 怎么来的 |
|---|---|
| 205 / 67 / 33% | 遍历 `data/xpkgs/*/*/bin/*`,`patchelf --print-interpreter`,抽样上限 400 |
| 各包最高 `GLIBC_x.y` | `readelf -V` 全量 ELF 取 max |
| 载荷不满足的 `DT_NEEDED` | 建 SONAME→载荷映射表后求差,排除包内自给 |
| 「缺的库属于哪个组件」 | 反查哪个 `.so` 的 `DT_NEEDED` 含它 |
| java/python/clangd 可跑 | `<我们的 ld.so> --library-path … <binary>` 实跑 |
| NSS 可用 | `java -XshowSettings:properties` 的 `user.name`;`pwd.getpwuid()` |
| 索引 seal 覆盖率 | `declare_libs` 26 个包中 22 个已 seal |
