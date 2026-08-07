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

0 个不满足项。在 recipe 的 `install()` 里:

```lua
elfpatch.patch_elf_loader_rpath(pkginfo.install_dir(), {
    bins   = { "bin" },
    loader = "auto",                                   -- 解析到 xim:glibc 的 ld.so
    rpath  = elfpatch.closure_lib_paths(),
})
```

这条路径 libxpkg 早就有,和 `selfcontain.seal` 是同一套机制的 bins 侧。

**L1 — 两个 JDK(补两个包后做)**

先把 `libXtst`、`libasound` 打包(`libX11/libXext/libXi/libXrender` 图形栈这轮已经
有了),然后同样切 `bins`。**即使这两个包不做也可以切**——差别只在 AWT/声音继续
落宿主,而那是现状。建议:**先切 loader,再补包**,两件事不要互相阻塞。

**L2 — `python`(只补 `libcrypt`)**

12 个不满足项里,只有 `libcrypt.so.1` 是 CPython 标准库(`crypt` 模块)要的;
`libmpi/libucp/libfabric/libibverbs/libtbb` 全是 numpy/scipy 在有 HPC 环境时才
`dlopen` 的加速后端,没有就走通用路径。补 `libxcrypt` 一个包即可。

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
