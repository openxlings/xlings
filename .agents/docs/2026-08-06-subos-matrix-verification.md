# subos × libc × 图形栈:深度验证报告

日期:2026-08-06
验证对象:xlings 2026.8.5.3 + libxpkg 0.0.50 + xim-pkgindex(当前 main)
验证方式:全部在隔离 `XLINGS_HOME` 中执行,宿主 `~/.xlings` 未被修改(见 §7)

---

## 1. 结论先说

矩阵跑通了,但**不是一开始就跑通的**。这一轮验证发现了 5 个缺陷,其中 4 个已修复并提交,1 个记录待办。

关键在于:**这 5 个缺陷全都只在隔离 home 下暴露**。在默认 `~/.xlings` 下,每一个都表现为完全正常。这不是巧合——它们共享同一个成因,见 §3。

| # | 缺陷 | 默认 home 下的表现 | 状态 |
|---|---|---|---|
| D1 | 沙箱把 xlings home 重映射到 `~/.xlings`,烘焙的绝对路径全部落空 | 两个拼写恰好相同,无症状 | 已修复 `7604bb9` |
| D2 | recipe 把我们的 `libc.so.6` 放上 `LD_LIBRARY_PATH`,宿主二进制在宿主 loader 下崩溃 | 宿主 glibc 版本号相同,恰好不崩 | 已修复(xlings `e486364` + pkgindex recipe) |
| D3 | `locate_proot_` 经 PATH 找到**另一个 home 的 shim**,整个沙箱改道到那个 home | PATH 上的 shim 就属于当前 home,无症状 | 已修复 `0bef205` |
| D4 | `subos use --sandbox` 完全跳过 subos.env 层 | 同一 subos 两种进入方式配置不同,但图形程序常在非沙箱路径使用 | 已修复 `0bef205` |
| D5 | 同一 subos 绑定同一包的两个版本,双方都在贡献环境声明 | EGL 枚举出重复设备,doctor 不报 | **未修复**,见 §5 |

附带修掉一个构建缺陷:`views::split \| ranges::to` 让 gcc 16 的模块构建以 "Bad file data" 失败并指向一个未改动的 TU(`f523219`)。发布目标用的是 gcc 15.1.0-musl,编译通过——所以它本可以带着这个问题发布。

---

## 2. 矩阵结果

### 2.1 libc 维度

同一个隔离 home 里两个 subos,各自装一套工具链,编译同一份 `hello.c`:

| subos | 编译器解析到 | 产物 INTERP | DT_NEEDED | 运行 |
|---|---|---|---|---|
| `g-world` | `subos/g-world/bin/gcc` | `<home>/data/xpkgs/xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2` | 1 | ✓ |
| `m-world` | `subos/m-world/bin/gcc` | 无(静态) | 0 | ✓,`env -i` 下也 ✓ |

两个 subos 里编译器都叫 `gcc`,各自解析到正确的那一个,没有交叉污染。

用 g-world 产物自己的 loader 展开依赖:

```
libc.so.6 => <home>/data/xpkgs/xim-x-glibc/2.44/lib64/libc.so.6
             <home>/data/xpkgs/xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2
```

INTERP 与 `libc.so.6` 来自**同一个 payload 目录**——同源不变量在用户自己编译的产物上成立,而不只是在我们安装的载荷上。除 `linux-vdso.so.1`(内核提供)外不涉及宿主。

全量扫描两个 home 的 store:

| home | 通过 | 违反 | 跳过(无 INTERP) |
|---|---|---|---|
| mx1 | 35 | **0** | 365 |
| prodhome | 71 | **0** | 329 |

musl 侧需要说明:索引里的 musl 只以工具链形式存在,产出静态二进制;整个索引中**只有 `glibc` 声明了 `exports.runtime.loader`**。也就是说 `abi` 字段目前没有第二个 ABI 可供区分——它是为将来准备的,现在还没有被真正行使。

### 2.2 图形栈维度

在 `prodhome`(mesa + libglvnd + nvidia-gl-host-link + X11/wayland 全栈)中,修复后两种进入方式结果完全一致:

| 探针 | shell 进入 | `--sandbox proot` |
|---|---|---|
| EGL device 枚举 | DEVICE_COUNT=5 | DEVICE_COUNT=5 |
| 硬件路径 | `NVIDIA GeForce RTX 4080/PCIe/SSE2` | 同 |
| 软件回退 | `llvmpipe (LLVM 20.1.7, 256 bits)` | 同 |
| `LIBGL_DRIVERS_PATH` | 2 项 | 2 项 |
| `__EGL_VENDOR_LIBRARY_DIRS` | 3 项 | 3 项 |

修复前:沙箱内三个变量**全为空**,GL 程序静默回退到宿主能提供的任何东西(D4)。

`MESA: error: ZINK: failed to load libvulkan.so.1` 在两侧都出现,是已知缺口——vulkan-loader 尚未打包(任务 #34),zink 后端因此不可用,不影响 llvmpipe 与 NVIDIA 路径。

### 2.3 沙箱后端与时延

| 项 | 结果 |
|---|---|
| proot 进入 | 59ms 均值,5/5 成功 |
| bwrap 进入 | 0/5,不可用 |
| 非沙箱 shell 进入 | 52ms 均值 |

沙箱只比普通进入多约 7ms。

bwrap 不可用有**两个独立原因**,任一都足以致其失败:

1. `kernel.apparmor_restrict_unprivileged_userns=1`(Ubuntu 24+ 默认);
2. 隔离 home 里的 bwrap 是 `-rwxr-xr-x`,而真实 home 里是 `-rwsr-xr-x`。setuid 位需要 root 才能设置,安装钩子在没有 sudo 时设不上。

这意味着**在没有 sudo 的机器上,新建的隔离 home 只能用 proot**。proot 基于 ptrace,功能上够用(上面所有测量都是它跑的),但性能与隔离强度都弱于 bwrap。这不是缺陷,是需要写进文档的既有约束。

### 2.4 生命周期

| 操作 | 耗时 | 结果 |
|---|---|---|
| `subos new` | 80ms | ✓ |
| `subos use --global` | 81ms | ✓ |
| `install`(已缓存) | 166ms | ✓ |
| 来回切换 | 99 / 101ms | ✓ |
| `subos remove`(活动中) | 34ms | ✗ rc=1 —— **正确行为** |
| `subos remove`(切走后) | — | ✓ |

删除活动 subos 被拒绝,并给出补救命令(`switch first: xlings subos use default`)。这是本轮唯一一个"拒绝得恰到好处"的地方,值得记一笔:它同时报告了规则和出路。

---

## 3. 五个缺陷的同一个成因

D1、D2、D3、D5 是同一个形状的四个实例,也就是
`.agents/docs/2026-08-05-dependency-resolution-single-source.md` 里那个"一个问题、多个回答者"——只不过这次问的不是"依赖是哪个版本",而是:

> **xlings home 在哪里?**

回答者有四个:

1. `XLINGS_HOME` 环境变量;
2. 沙箱绑定的目标路径(原来是 `<user_home>/.xlings`);
3. 安装时烘焙进产物的绝对路径(xvm alias target、RPATH、INTERP);
4. shim 从 argv0 反推出的 owner home。

**在默认配置下这四个答案是同一个字符串。** 所以它们从未被迫达成一致——它们只是恰好一致。隔离 home 是第一个让它们分开的配置,一分开就同时暴露四个缺陷。

这解释了为什么这些缺陷能活到现在:整个测试体系(包括 e2e)几乎都在默认 home 或与默认 home 同形的路径下运行。`subos_sandbox_test.sh` 的 S8 甚至**把错误模型写成了断言**——"`~/.xlings` 能看到宿主内容"——所以它不但没发现 D1,还会阻止别人修。

### 修法

按同一份设计文档的规则:**删掉多余的回答者,而不是让它们更容易一致。**

- D1:home 永远绑定在**它自己的绝对路径**上。内外只有一个拼写,烘焙的绝对路径原样有效。
  曾试过"两个路径都绑"——更糟:bind mount 不会把两条路径合并成一个,`weakly_canonical` 仍看到两个 home,shim 于是报告"与自己冲突"。
- D3:PATH 回退拒绝任何位于 xlings home 内的候选。真正的 `/usr/bin/proot` 仍然可用。
- D4:两条进入路径共用同一个 applier,而不是各写一份。

D2 是另一个形状,见下节。

---

## 4. D2:libc 不是可以放上搜索路径的依赖

这个值得单独讲,因为它是**同源分裂从设计断言看不见的方向**打进来的。

`nvidia-gl-host-link` 的处境是真实的:NVIDIA vendor 库是宿主的文件(符号链接指向 `/lib/x86_64-linux-gnu/`),我们**不能**给它打 RPATH——那要改宿主的文件。所以它需要一个搜索路径,recipe 把依赖收拢到 `lib/xlings-deps/` 并声明到 `LD_LIBRARY_PATH`。

问题是 `LD_LIBRARY_PATH` 是**进程全局且被每个子进程继承**的,而 subos 里绝大多数子进程是**宿主二进制、跑在宿主 loader 下**。目录里有我们的 `libc.so.6`,于是:

```
$ xlings subos use default --cmd 'echo HI'
  ▸ entering subos default
   1229155:	__vdso_time
$ echo $?
139
```

`/bin/bash` 在打印任何字符之前就 SIGSEGV 了。宿主 glibc 是 2.39,我们的也是 2.39——**同一个上游版本,只是不同构建**,GLIBC_PRIVATE 就已经对不上。

同源断言看不见它,因为**我们安装的东西没有一个是错的**。错的是我们让宿主的东西去用我们的一半。

### 逐库测量,而不是逐库推理

我第一版修复删掉了整个 glibc 行。那是错的,测量推翻了它:

| `xlings-deps` 内容 | EGL 枚举出 NVIDIA | `/bin/bash` |
|---|---|---|
| 什么都不放 | ✗ 消失 | ✓ |
| 只放 `libc.so.6` | ✗ 消失 | ✗ SIGSEGV |
| `libm/libpthread/libdl/librt`,不放 libc | ✓ | ✓ |
| `libpthread/librt/libdl`(vendor 的 DT_NEEDED 减去 libc) | ✓ RTX 4080 | ✓ |

vendor 的 DT_NEEDED 是 `libpthread.so.0, librt.so.1, libc.so.6, libdl.so.2`(外加自家 `libnvidia-glsi`)。`libm` 根本不在上面。

- `libc.so.6` **既无用又致命**:vendor 是被 dlopen 进一个已在运行的进程的,它的 libc 早就绑定好了,已加载的 SONAME 不会再去搜索。所以这一项永远不会被用于它声称的目的。
- `libpthread/librt/libdl` **必需且无害**:自 glibc 2.34 起它们是兼容桩,实现都搬进了 `libc.so.6`,分别只剩 27、13、9 个定义符号。
- `libm` 有 1203 个符号(真正的 ABI 面)。**这里原本写的是「没人要它」,那是错的**——
  它不在 `libEGL_nvidia` 的直接 DT_NEEDED 上,但被 16 个 nvidia 库 NEED,含核心渲染器
  `libnvidia-glcore`。我当时只取了闭包的一个入口。更正与影响见
  `2026-08-06-subos-architecture-proposal.md` §6。

最终 recipe 保留 glibc 行,只列这三个桩。

### 防线也按测量收窄

xlings 侧加了一道通用防线:构建环境时,拒绝把含 libc 的目录放上 `LD_LIBRARY_PATH`/`LD_PRELOAD`,点名目录和声明它的包,并保留该变量的其余部分。

我最初把整个 glibc 集合都列进"耦合"名单——**那会打断一个刚被测量证明必需的配置**。收窄到 `libc.so.6` 和 `ld-linux*`/`ld-musl*`:这一对的失败方式是"在 main 之前崩溃且不指名任何文件"。不匹配的 `libm` 会响亮地报 `version 'GLIBC_2.38' not found` 并指出文件名——那是可诊断的,不是这道防线要防的东西。

E2E-63 覆盖的是这一**类**,不是这个 recipe,并且专门断言桩库不能被误伤。

### 一个诚实的局限

`LD_LIBRARY_PATH` 只能按**目录**取舍,不能说"这个目录除了某个文件"。所以在尚未更新的载荷上,防线丢掉整个目录,连带丢掉 libX11 等本该保留的库,图形栈降级——直到 recipe 重新发布。

这是安全的方向(降级而非崩溃),而且有明确的报告。要做到文件级,就得让 xlings 物化一个过滤后的镜像目录——那会成为"vendor 的依赖在哪里"的第二个回答者,正是本轮在拆的东西。所以按目录取舍是这里的诚实边界,recipe 才是正确的修复位置。

---

## 5. D5:未修复 —— 一个 subos 绑定同一个包的两个版本

`prodhome` 的 `default` subos 里:

```
$ xlings list | grep mesa
  ◆ xim:mesa@25.0.7.1
  ◆ xim:mesa@25.0.7
```

subos manifest 里两个 binding 都在,两者都在贡献 `__EGL_VENDOR_LIBRARY_DIRS`(3 项 = nvidia + 两个 mesa)。EGL 因此枚举出重复的 llvmpipe 设备。

按你定的三层模型:**store 可以有多个版本,subos sysroot 恰好一个**。这里 subos 层拿到了两个,环境层把 store 的"多版本"泄漏进了 subos 的"恰好一个"。

目前的实际后果有限(两个 mesa 版本兼容,枚举出重复设备而已)。但如果两个版本的 DRI ABI 不同,选中哪个驱动将取决于目录顺序——一个由安装顺序决定的、没有任何东西报告的结果。

**`xlings self doctor` 不报这个。** 又是一次"从未发生"和"成功了"输出相同。

没有在本轮修复:它需要在 subos 层强制单版本(安装第二个版本时替换而非并列),牵涉 xvm 注册与 env manifest 两处语义,不适合塞进这批修复里。已作为独立任务记录。

---

## 6. 已确认成立的性质

这些跑通了,并且是**用会失败的方式**验证的:

- **同源不变量**:两个 store 共 106 个带 INTERP 的 ELF,0 违反;用户自己编译的产物同样成立。
- **home 隔离**:修复 D3 后,隔离 home 缺少沙箱后端时**拒绝执行**并给出 `xlings install proot`,而不是借用宿主的。
- **路径同一性**:沙箱内外 home 只有一个拼写,`XLINGS_HOME`、`PATH[0]`、烘焙路径三者一致。E2E 里用 `env -i` 净环境断言,且被测 home 位于 `/tmp` 下——这是绑定顺序最难的情形(`/tmp` 先被私有化,再挂 home),它通过了。
- **两条进入路径等价**:shell 与 sandbox 的 subos.env 现在逐项一致。
- **libc 矩阵**:glibc 动态与 musl 静态在同一 home 中共存,互不干扰;musl 产物在 `env -i` 下运行正常。
- **生命周期**:create/use/install/switch/remove/doctor 全程无残留,删除活动 subos 被正确拒绝。
- **沙箱私有 /tmp**:宿主 `/tmp` 下的文件在沙箱内不可见(这是预期行为,验证时踩到过一次)。

---

## 7. 宿主 xlings home 未被破坏

按约束核对:

- 我的第一个提交是 03:07。
- 宿主 home 的实质性改动(packages、index、`subos/default`,共 1000+ 项)时间戳在 **01:52–02:32**,全部早于此。
- 我的工作窗口(03:00 之后)内,宿主 home 只有一个文件被动过:`data/xim-pkgindex-local/.xlings-index-cache.json`(一个索引**缓存**)。
- `data/xpkgs`、`subos/`、`.xlings.json` 三处在 03:00 后改动数为 **0**。

即:宿主的包、subos 与配置状态未被本轮验证改动。

需要说明的是,`--version` 与 `list` 单独执行不会写入宿主 home(已实测),那个缓存文件的写入没能复现出来,来源未确定。

---

## 8. 测试体系上的教训

三条,都值得改进流程而不只是改代码:

1. **e2e 把错误模型写成了断言。** `subos_sandbox_test.sh` 的 S8 断言 `~/.xlings` 在沙箱内能看到宿主内容——那正是 D1 的错误行为。一个测试不仅可能漏掉缺陷,还可能把缺陷钉死。已改写为契约断言(`XLINGS_HOME` 即宿主路径、`PATH[0]` 与之一致、不存在第二个拼写)。

2. **默认 home 让四个答案恒等。** 只要测试都在默认 home 或同形路径下跑,这一整类缺陷就不可见。**隔离 home 应当成为沙箱与 subos 测试的默认环境,而不是特例。**

3. **发布目标编译通过 ≠ 代码没问题。** `views::split | ranges::to` 在 gcc 15.1.0-musl(发布目标)下编译通过,在 gcc 16.1.0(默认工具链)下让整个模块以 "Bad file data" 失败,并且指向一个未改动的 TU。只跑发布目标的构建会让它直接发出去。**改动核心模块后必须两个工具链都构建。**

---

## 9. 提交

xlings(分支 `fix/sandbox-xlings-home`):

| commit | 内容 |
|---|---|
| `7604bb9` | D1 —— home 绑定在自己的绝对路径上;rc 模板三份重复收敛为一个写入器;S8 改写 |
| `e486364` | D2 防线 —— 拒绝把 libc 放上全局搜索路径;新增 E2E-63 |
| `0bef205` | D3 + D4 —— 拒绝另一个 home 的 shim;两条进入路径共用 env applier;防线按测量收窄 |
| `f523219` | 构建 —— 去掉让 gcc 16 模块构建失败的 C++23 ranges |

xim-pkgindex(分支 `docs/resolved-deps-spec`):`nvidia-gl-host-link.lua` —— `xlings-deps` 只保留 `libpthread/librt/libdl`,去掉 `libc.so.6`(致命且无用)与 `libm.so.6`(无人需要且 ABI 面最大)。

测试:单测 35 passed / 0 failed(55 个用例);E2E-63 六条断言全过。
