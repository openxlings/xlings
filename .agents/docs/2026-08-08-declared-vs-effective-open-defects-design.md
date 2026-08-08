# 「声明了」与「生效了」:七条待办的实测结论与设计方案

> 2026-08-08 · 承接 `2026-08-08-mesa-rebuild-iris-d3d12-wayland-design.md` §9
> 目标:把七条挂着的事逐条**实测**判定真伪,再合成一份能执行的方案

---

## 0. 先说三条被测量改写的结论

写方案之前逐条验,**两条我原来的说法是错的,一条根本不是问题**。

| 原说法 | 实测 |
|---|---|
| `xlings use` 不重新声明 sysroot 资产(`xlings#507`) | **假问题,已关闭。** 同命名空间切换是好的,见 §2.6 |
| `rust` 直接跑 rustup 安装脚本(`#569`) | **不准确。** 它跑的是**已打包的** `rustup-init -v -y`;真正的问题在 uninstall,见 §2.7 |
| `#506` 是「委托安装的包」的问题 | **谓词错了。** 真正的谓词是「**没注册任何 xvm 版本**」,委托只是抵达该状态的一条路,见 §2.4 |

三条里有两条是我自己上一轮写下的。这个比例本身值得记:**本仓库的缺陷描述,不实测就有一半概率归因错。**

---

## 1. 七条的判定汇总

| # | 事项 | 判定 | 决定性证据 |
|---|---|---|---|
| 1 | `xim-pkgindex#568` libXtst + libasound | **真** | `libawt_xawt.so` → `libXtst.so.6`;`libjsound.so` → `libasound.so.2`;两者均不在索引 |
| 2 | `xim-pkgindex#569` rust 打包方式 | **真(需改写描述)** | `install()` 跑 `rustup-init -v -y`;`uninstall()` 跑 `rustup self uninstall` |
| 3 | `xlings#505` mirror 配置不生效 | **真,且已精确定位** | 仅配置 → 连 `20.205.243.166`(= github.com)@ 57.6 KB/s;加 env → 只连 `116.205.2.91` |
| 4 | `xlings#506` remove 失败 | **真,且面更大** | 4 个 **Linux** 配方委托且 `xvm.add` 为 0 |
| 5 | `xlings#508` `subos.env` 缺 `default` | **真** | `manifest.cppm:45-46` 只有 `set`/`prepend` |
| 6 | 同命名空间 `use` 切换 | **不是问题** | ca-certificates 的 files 节点跟着切了,见 §2.6 |
| 7 | `--add-xpkg` 静默跳过声明 | **真** | libxpkg 调用全部落地,pkgindex 调用全部蒸发 |

---

## 2. 逐条实测

### 2.1 #568 libXtst + libasound —— 真

```
libawt_xawt.so NEEDED: libawt.so libjava.so libdl.so.2 libm.so.6 libX11.so.6
                       libXext.so.6 libXi.so.6 libXrender.so.1 libXtst.so.6 ...
libjsound.so   NEEDED: libasound.so.2 libc.so.6
$ ls pkgs/l/libXtst.lua pkgs/a/alsa*.lua   → neither present
```

这不是「少两个库」,而是**卡住 JDK 的 loader 迁移**:切 PT_INTERP 会移除全部宿主回退(我们 glibc 编译进去的 cache 路径在任何机器上都不存在),所以闭包里剩一个不是我们的库,JDK 就起不来 —— 而且第一个死的是 AWT,报错指向 `libawt_xawt.so` 而不是缺失的那个依赖。

### 2.2 #569 rust —— 真,但我的描述错了

我原来写「跑上游 rustup 安装脚本」。实际:

```lua
deps    = {"xim:rustup", "config:rustup-mirror"}
install = os.exec("rustup-init -v -y")          -- 已打包的 rustup-init
uninstall = os.exec("rustup self uninstall")
```

所以 **不是 `curl | sh`**,rustup 本身是索引里的包。真正的问题剩两条,而且第二条更尖锐:

1. **载荷身份不由索引决定** —— `rustup-init` 在安装时去 `static.rust-lang.org` 拉工具链,索引里没有对应的 sha256。同一个 recipe 在不同日子装出不同字节。
2. **`uninstall()` 调第三方工具卸载自己** —— `rustup self uninstall` 动的是本索引不拥有的状态。一个 recipe 无法承诺「移除后 home 回到原样」。

### 2.3 #505 mirror —— 真,且是「配置说一套、行为做一套」的教科书例子

受控实验,同一个包、同一个隔离 home、唯一变量是 env:

| | 连过的外部 IP | 速率 |
|---|---|---|
| 只有 `.xlings.json` 的 `"mirror": "CN"` | 115.223.9.100、116.205.2.91、**20.205.243.166** | **57.6 KB/s** |
| 额外设 `XLINGS_MIRROR/RELEASE/INSTALL=CN` | **只有 116.205.2.91** | 秒完 |

`getent hosts github.com` → `20.205.243.166`。**配置版真的去了 GitHub,env 版一次都没去。**

代码侧对得上:`XLINGS_MIRROR` 只在 `src/core/xself/install.cppm:58` 被读(self-install 路径),而 `config.cppm:399` 的 `effective_mirror_name_(mirror, mirror_)` 确实读了配置 —— 也就是说**两条路径各自都「有道理」,只是没有汇合**。这不是「配置被忽略」,是「配置和 env 走了不同的解析链」。

### 2.4 #506 remove —— 真,而且我把谓词写窄了

我原来写「委托安装的包」。实测委托的有 7 个配方,其中 **4 个在 Linux 上委托且 `xvm.add` 为 0**:

| 配方 | type | xvm.add |
|---|---|---|
| `cpp.lua` | config | 0 |
| `mcpp-vscode-clangd.lua` | config | 0 |
| `linux-sysroot-create.lua` | script | 0 |
| `configure-project-installer.lua` | script | 0 |

所以真正的谓词是「**这个包没注册任何 xvm 版本**」,委托只是抵达该状态的一条路;`type = "config"` / `type = "script"` 天然就在该状态。

佐证:`posix-test.sh` **早就为 `type = "config"` 加了针对这条诊断的容忍**,并在注释里写「whether `remove` should succeed as a no-op there is a real question, and an xlings-side one」。也就是说这个问题项目里已知一半,只是没有被当成一个统一问题处理。

### 2.5 #508 `subos.env` 缺条件 op —— 真

```cpp
// src/core/subos/manifest.cppm:45-46
inline constexpr std::string_view OP_SET     = "set";
inline constexpr std::string_view OP_PREPEND = "prepend";
```

两个 op,没有条件式。后果是 `wsl-gl-host-link.lua` 的注释宣称「用户 export 的值会保留,所以强制是安全的」—— 那个逃生通道**只存在于注释里**。

诚实标注:**「`set` 覆盖用户值」这一条是 mcpp#382 的报告者实测的,不是我测的**(本机不是 WSL2)。我这边能独立确认的是 op 集合只有两个,以及本机 mesa 用的是 `prepend` 而非 `set`,用户预设值确实存活。

### 2.6 同命名空间 `use` 切换 —— **不是问题,正证如下**

这条此前只有「没有反证」。现在有正证了。做法:找一个**用 `xvm.files` 且索引里有两个版本**的包 —— `ca-certificates`(2025.07.15 / 2026.03.19)。

```
安装两个版本后:
  ca-certificates.files.1   2025.07.15   kind=files  dst=etc/ssl/certs/ca-certificates.crt
  ca-certificates.files.1   2026.03.19   kind=files  dst=etc/ssl/certs/ca-certificates.crt

BEFORE  → xim-x-ca-certificates/2025.07.15/cacert.pem   sha 7430e90e
$ xlings use ca-certificates 2026.03.19
AFTER   → xim-x-ca-certificates/2026.03.19/cacert.pem   sha b6e66569
```

**文件跟着切了,内容也变了。机制是对的。**

顺带一个此前无人知道的事实:在做这个实验之前,**这个 home 里 0 个 files 节点拥有多于一个版本** —— 也就是说多版本 files 切换从未被任何测试或使用走过。它是对的,但此前是靠运气对的。

### 2.7 `--add-xpkg` 静默跳过 —— 真

同一次安装里:

| 调用 | 模块 | 结果 |
|---|---|---|
| `xvm.add(package.name)` | `xim.libxpkg.xvm` | **注册了** |
| `graphics.declare_dri(...)` | `xim.pkgindex.graphics` | **没有** |
| `sysroot.declare_libs(...)` | `xim.pkgindex.sysroot` | **没有** |

`--add-xpkg` 用宽容 stub 替换 `xim.pkgindex.*`,调用变成 truthy no-op 且**一声不吭**。后果:**图形类配方的本地安装测试,恰好验不到最容易错的那部分** —— sysroot 资产、驱动目录、EGL/Vulkan manifest —— 而且报成功。

这条直接制造了一个假 bug(`xlings#507`),我为此写了报告、写了 issue、还在两处文档里引用了它。

---

## 3. 一条贯穿线:声明被接受,却没有生效,而且不出声

七条里有四条是同一个形状:

```
#505    配置里写了 mirror=CN            →  下载仍去 GitHub          →  没有任何提示
#508    注释里写了「用户值优先」        →  op 集合根本不支持        →  没有任何提示
#507    recipe 调了 declare_dri         →  stub 吞掉               →  没有任何提示
#506    包安装成功                      →  移除时说版本没注册       →  安装时没有任何提示
```

**共同点不是「有 bug」,是「系统接受了一个声明,然后不履行它,并且不说」。** 这正是本仓库反复出现的 silent-success 的另一面 —— 前者是「什么都没做却报成功」,这里是「答应了却没做,也报成功」。

所以方案不是七个补丁,而是一条原则:

> **一个声明要么生效,要么在声明的那一刻就报错。永远不要沉默地接受一个不会履行的声明。**

---

## 4. 方案

### D1 — `--add-xpkg` 的 stub 必须出声(最高优先)

**为什么排第一**:它不修复任何用户可见的行为,但它是**唯一一条会让其他修复的验证失效**的。本轮它已经让我把一个不存在的产品缺陷写进了 issue 和两份文档。

**做法**:`xim.pkgindex.*` 的 stub 在被调用时记录并打印:

```
[warn] local index: xim.pkgindex.graphics.declare_dri(...) skipped
       (--add-xpkg cannot run pkgindex modules; sysroot assets are NOT verified by this install)
```

并在安装结束时汇总一行 `N pkgindex declaration(s) skipped`。

**验收**:用 `--add-xpkg` 装 mesa,输出里必须出现被跳过的 `declare_dri` / `declare_libs`;CI 的 `linux-install-test` 对图形配方必须打印该汇总行(不是失败,是可见)。

### D2 — mirror 的两条解析链合一

**做法**:确定唯一的优先级并在一处实现:

```
XLINGS_INSTALL_MIRROR > XLINGS_RELEASE_MIRROR > XLINGS_MIRROR > .xlings.json mirror > Auto
```

下载路径与 self-install 路径调用同一个函数。

**验收(必须是实测,不是单测)**:隔离 home、只设配置、`strace -e connect` 装一个包,**不得出现 github.com 的地址**。这正是 §2.3 的实验,把它固化成脚本。

**顺带**:`xlings config --mirror CN` 成功时应回显生效范围(「适用于:索引 / 发行资产 / self-install」),否则用户无从判断「我设了但没用」。

### D3 — `subos.env` 增加 `default` op

**做法**:第三个 op,仅当变量未设置时导出。

两个必须显式决定、不能靠实现继承的点:

1. **未设置 vs 空值**。`FOO=` 在某些子系统里是刻意的「不要覆盖」。把空当作未设置会踩掉它;当作已设置大概是对的,但要写进 spec。
2. **两个包都对同一个变量声明 `default`**。跟 `set` 的冲突规则保持一致,不要新发明一套。

**能力探测**:新 op 不是新函数,`if subos.env then` 判不出来。要么暴露 `subos.env_ops`,要么让未知 op 在**安装时**硬失败 —— `manifest.cppm:336` 已经把未知 op 判为 `EnvDeclMalformed`,需要确认这条路径在旧客户端上是安装时触发而不是只在 validate 时。

**受益者**:`wsl-gl-host-link` 可以恢复「用户的值优先」,而不是像现在这样只能少强制。

### D4 — `remove` 对「没注册版本」的包成功为 no-op

**谓词是「没注册 xvm 版本」,不是「委托安装」**(§2.4)。

**做法**:`remove` 在选择版本失败前,先问「这个包注册过任何版本吗」。没有 → 执行 recipe 的 `uninstall()` 钩子,然后成功返回,并说明「该包注册无版本,已执行其 uninstall 钩子」。

**理由**:安装时它就没注册版本,而安装是成功的。移除时因为同一个事实失败,是**安装/移除不对称**,不是包的错。

**验收**:`posix-test.sh` 里那条 `type = "config"` 容忍**可以删掉** —— 容忍存在本身就是这个缺陷的证据,修好后它必须不再需要。这是最好的回归测试。

### D5 — libXtst + alsa-lib 打包(`#568`)

两个普通 autotools 构建,进 `tiers.sh`:

- `libXtst` —— 前置 `xorgproto` + `libX11` + `libXext` + `libXi`,全部已打包,T2 形状,无新前置
- `alsa-lib` —— 自包含;`--disable-python`,不要 `alsa-plugins`

**验收是运行,不是文件存在**:切 JDK 的 loader 之后,headless 起 `Toolkit.getDefaultToolkit()` 必须成功。

### D6 — rust 打包方式(`#569`)

先改 issue 描述(§2.2)。真正要解决的是两条:载荷身份、以及 `uninstall` 调第三方工具。

**两个需要你拍板的点**:

1. **切分**:完整 tarball 几百 MB。一个包还是 `rust` / `rust-std` / `rust-src` 拆开?这跟 `gcc` vs `gcc-runtime` 是同一个问题,应该显式回答。
2. **与现存 rustup 安装共存**:已有 home 上有 rustup 状态。新配方是只影响新装、检测到就拒绝、还是提供迁移?**用户可见,我不该自己决定。**

---

## 5. 建议顺序

```
D1  stub 出声          半天   —— 先做,否则后面每一条的验证都可能是假的
D2  mirror 合流         1 天   —— 用户天天踩,而且有现成的实测脚本
D4  remove 对称         1 天   —— 能删掉一条测试容忍,回归信号最干净
D3  default op          1 天   —— 解锁 wsl-gl-host-link 的「用户优先」
D5  libXtst + alsa      2 天   —— 解锁 JDK loader 迁移
D6  rust                待定   —— 先要 §4/D6 的两个决定
```

D1 排第一不是因为它最重要,而是因为**它是唯一一条不修就会让其余验证不可信的**。

---

## 6. 不在本方案内、但已知的两件

- **env 声明是否跟随 `use` 切换,仍未验证。** files 节点在 xvm DB 里(§2.6 已证跟随),而 env 声明在 `subos_info` 里、按 binding 键值组织 —— **是两个不同的存储**。我尝试测量时被 `--add-xpkg` 的 stub 污染了(D1 修好后就能干净地测)。在验证之前,不要假设它跟 files 一样是对的。
- **图形栈六个 gallium 驱动里只有 llvmpipe 真跑过。** radeonsi / nouveau / iris / d3d12 / zink 全部只是「载荷就位、闭包干净」。这不靠加包解决,只能靠别人的机器 —— 见 `collect-matrix.md`。


---

## 7. 实施后的更正:D2、D3 不存在,D1 的修复位置我写错了

方案写完就去实现了,过程中又有三处被测量改写。**这一节写在最后而不是回头改上面**,因为「当初怎么判断的」和「后来发现什么」都是证据。

### D2(mirror 合流)—— 不是缺陷,#505 已关闭

§2.3 我用 `strace -e connect` 的端点列表下结论。**方法错了** —— 日志会直接打印 URL:

```
[debug] downloading xim:xtrans@1.5.2 from
        https://gitcode.com/xlings-res/xtrans/releases/download/1.5.2/...
```

只有配置、没有 env,载荷走的就是 **gitcode**。`cmd_install` 本来就设
`dlConfig.preferredMirror = Config::mirror()`(`commands.cppm:529`)。

我看到的 GitHub 连接是**索引拉取的 GLOBAL 兜底,而且源码里写着为什么**:

```cpp
// Always include GLOBAL (github) servers as fallback for the index — even
// under CN. Unlike package binaries (where the regional mirror is
// authoritative), the index must stay reachable if the regional mirror
// lacks the asset ...
```

self-install 也保留既有设置(`if (!envMirror && !overwriteDataSubos && !existingMirror.empty()) return;`)。

**两次运行差的不止我以为的那一个变量** —— env 那次不需要拉索引,所以没有 GitHub 连接。57.6 KB/s 只是 gitcode 当时慢。

### D3(`default` op)—— 不是缺陷,#508 已关闭

`op = "set"` **本来就是** conditional,四个后端一致:

| 后端 | 代码 |
|---|---|
| POSIX | `: "${VAR:=value}"; export VAR;` |
| fish | `if not set -q VAR; set -gx VAR …; end` |
| pwsh | `if (-not $env:VAR) { … }` |
| 进程内 | `else if (existing.empty()) { set_env_variable(...) }` |

注入一个合成声明实测:

```
生成:  : "${XLINGS_SET_PROBE:=PACKAGE_VALUE}"; export XLINGS_SET_PROBE;
结果:  XLINGS_SET_PROBE=USER_VALUE          ← 用户的值赢
```

所以 `wsl-gl-host-link.lua` 那句「用户 export 的值会保留」**对 xlings 是对的**,我在 `xim-pkgindex#565` 里说它不存在,那半撤回。真正被覆盖的是 **mcpp 应用 subos_info 的实现**,报告者自己就猜到了 —— 已在 mcpp#382 说明。

**残留的真问题是命名**:一个叫 `set` 却不 set 的 op,把报告者和我先后骗了。这是文档问题,不是语义问题。

### D1 —— 结论对,位置错了两次

方案说「让 stub 出声」。实现时先改 `ctx.pkgindex_dir`,不生效;查到 libxpkg 的模块加载器:

```lua
local pkgindex_dir = _PKGINDEX_DIR or (_RUNTIME and _RUNTIME.pkgindex_dir)
```

`ctx.pkgindex_dir` 只喂 `_RUNTIME.pkgindex_dir`,而它**只在钩子调用时被查**;recipe 的 `import` 在顶层,用的是 `_PKGINDEX_DIR`,由 mcpplibs::xpkg 内部设置 —— 在第三个仓库。

**真正的修法在源头**:`cmd_add_xpkg` 造出的本地索引只有 `pkgs/`。给它 symlink 一份主索引的 `libs/` 就行,全部在 xlings 侧。

验收(同一个 recipe,真索引的同类包做基准):

| | 注册的 xvm 节点 |
|---|---|
| 真索引 `libXi` | `libXi`、`.files.1/2`、`libXi.so`、`.so.6`、`.so.6.1.0` |
| 本地 **修前** `libXtst` | `libXtst` |
| 本地 **修后** `libXtst` | `libXtst`、`.files.1/2`、`libXtst.so`、`.so.6`、`.so.6.1.0` |

### 结账

七条里 **四条是假问题,而这四条里四条都是我先前自己写的描述**。

这个比例不是运气不好,是方法问题:每一条错误都来自「读了一个看起来对的地方就下结论」——
端点列表而不是 URL 日志、op 名字而不是 op 实现、`config()` 调用而不是它是否落库。§0 里我已经
写过一次同样的教训,然后又犯了四次。

**能立刻用的一条规则**:凡是断言「X 没有生效」,先找出系统里**打印 X 实际做了什么**的那一行;
找不到就先加上它,再下结论。

---

## 8. 收尾:D3 的残留、§6 的答案、D5 的真实状态,以及 aarch64 红格

§7 之后又做了四件事。**其中三件推翻了「已经好了」的默认假设** —— 两件是我的,一件是 CI 的。

### 8.1 aarch64 CI:四次「修复」全错,错在同一个地方

这一格红了 16 次,期间提交了四个「修复」——pin mcpp、pin 索引、去掉源码构建 mcpp、
去掉载荷缓存。**四个全都没改变任何东西**,失败输出一字不差:

```
build.mcpp compiling / build.mcpp running
error: posix_spawnp('.../xpkg@0.0.54/build.mcpp.bin') failed (error 2)
```

四次都是**在 runner 之外推断**的。没有一次去 runner 上量。加了一个 `if: failure()`
的诊断步骤之后,一轮就出来了:

```
build.mcpp.bin	287360 bytes        ← 它被产出来了,编译从来没失败
interp = /home/xlings/.xlings_data/subos/linux/lib/ld-linux-x86-64.so.2
interp MISSING -> this is the ENOENT. The loader, not the binary.
```

**`execve` 在 PT_INTERP 不存在时返回 ENOENT,而且报的是「二进制」不是「缺的 loader」。**
所以这条错误读起来像「构建脚本没产出」,实际上它好好地躺在那里。四次修复全在追一个
根本不存在的编译失败。

那个 interp 是**打包机器的路径**:用户 `xlings`、已废弃的 `.xlings_data` 家目录布局、
已废弃的 `subos/linux` 命名。确认它来自发布的载荷本身 —— 下载
`gcc-16.1.0-linux-x86_64.tar.gz`,它就写在

```
lib/gcc/x86_64-linux-gnu/16.1.0/specs
```

里,用这个未经改动的载荷编译 hello.c 能一比一复现。**tarball 里不含 stamp**,
所以 `gcc.lua` 的 stamp 闸门不是原因。

真正的原因:**CI 的 bootstrap 客户端是 `v0.4.69`** —— 旧版本号体系的产物,比整个
loader/INTERP 系列早了几个月。`gcc.lua` 的 `config()` 本来就是干这件事的
(`__rewrite_specs_linux`,注释写着「this is mandatory」),旧客户端上它没有发生。
把七个 workflow 的 `BOOTSTRAP_XLINGS_VERSION` 换成 `v2026.8.8.1` 之后这一格**绿了**,
并且是真绿:交叉构建、`file` 断言 ARM/static、qemu 跑 `--version`、产物上传、
以及依赖它的原生 aarch64 contracts job 全部通过。

**mcpp 不背这个锅**,两点都要说清楚:它为项目目标**在构建时生成链接参数**而不是读
gcc 的 specs;而且该 job 用的已经是最新的 mcpp(2026.8.8.4,日志可查)。唯一吃 specs
的是宿主侧的 `build.mcpp` 编译 —— 恰好就是失败的那一步。

### 8.2 顺带挖出来的第二个缺陷:`gcc-specs-config` 不幂等

本机复现时撞到另一个:

```
INTERP  = .../xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
RUNPATH = .../xim-x-glibc/2.44/lib64:...
→ libc.so.6: undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
```

`gcc-specs-config.lua` 每次运行:

- **rpath 段是前插**(`*link:\n` → rpath + `*link:\n`),所以**每跑一次就多一条**;
  本机 specs 里已经堆了几十条,包括早就删掉的 `/tmp/tmp.*/mcpphome/...`
- **dynamic-linker 是一次性替换**:它找的是原始字面量 `/lib64/ld-linux-x86-64.so.2`,
  第一次替换后这个字面量就没了,**之后永远不再更新**

净效果:rpath 漂到最新的 glibc,interp 冻结在第一次配置的那个,两者迟早对不上,
报出来是 `GLIBC_PRIVATE` 符号错误 —— 一个完全不指向病因的现象。这是本文档主线的
同一族:一个动作被执行了、被记为成功,而它没有做到它声称的事。

### 8.3 §6 的答案:env 声明**确实**跟随 `use`

§6 说这条未验证,并且提醒不要假设它跟 files 一样对(两个不同的存储)。现在量了:
一个 fixture 包、两个版本、同一个变量声明为 `${pkgdir}/marker`:

```
use envswitch 1.0.0  ->  .../envswitch/1.0.0/marker
use envswitch 2.0.0  ->  .../envswitch/2.0.0/marker
```

**跟随。** 已固化为 `tests/e2e/subos_env_use_switch_test.sh`,并带两个防空转断言:
切换前先断言两个版本各自都注册了 env 段(只注册一个的话后面全部空过),以及两次切换
的结果必须不同。

写它时踩了一个值得记的坑:`subos_info` 是 **subos 级**状态,在
`subos/<name>/.xlings.json`,不在家目录的 manifest。读错文件会得到「0 个 env 段」——
和「声明被静默丢弃」长得一模一样。

### 8.4 D3 的残留是真的,而且比「命名」更严重

§7 判定 `op = "set"` 本来就是条件式,#508 关闭,残留只是命名问题。**命名确实是问题,
但底下还压着一个真缺陷**:四个后端对「空值算不算已设置」不一致。

| 后端 | `export FOO=` 时 |
|------|------------------|
| POSIX `${VAR:=v}` | **覆盖**(`:=` 连空值一起赋) |
| pwsh `-not $env:VAR` | **覆盖**(`-not` 对空串为真) |
| 进程内 `existing.empty()` | **覆盖**(根本分不出未设置与空) |
| fish `set -q` | 保留 |

同一个声明,进 subos 的方式不同,环境就不同,而且不出声 —— 正是 §3 那条主线。
已统一为**空值算已设置,用户的值赢**:`export FOO=` 是用户做的选择,不是「没设置」。
新增 `utils::env_is_set`(`get_env_or_default` 结构上答不了这个问题,它对两种情况
都返回 `""`)。e2e 加了空值用例,并**验证过可证伪**:对上一个构建跑,它失败。

命名维持 `set`:未知 op 在安装期即 `EnvDeclMalformed`,改名会让任何用新名字的 recipe
在所有已发布的旧客户端上装不上。语义写进了 `docs/spec/xlings-json-schema.md`。

### 8.5 D5:包对了,验收还不成立

`xim-pkgindex#570` 已合并,`libXtst` 与 `alsa-lib` 都在 subos sysroot 里,而且
**闭包自洽** —— 两个库的 NEEDED 全部能在 subos 内解析。

但 §4/D5 写的验收是「切 JDK 的 loader 之后,headless 起 `Toolkit.getDefaultToolkit()`
必须成功」,**这一条现在还测不了,而且天真地测会得到一个假通过**:

```
sandbox: LOAD_OK awt / awt_xawt / jsound        ← 看起来全好
sandbox: MAPPED /lib/x86_64-linux-gnu/libXtst.so.6.1.0
         MAPPED /lib/x86_64-linux-gnu/libasound.so.2.0.0   ← 来自宿主,不是我们的包
```

JDK 的 `java` 现在 PT_INTERP 仍是 `/lib64/ld-linux-x86-64.so.2`,**loader 还没切**,
所以宿主回退还在,那两个库是宿主提供的。`libawt_xawt.so` 的 RPATH 只有 `$ORIGIN`,
指不到 subos。把 `LD_LIBRARY_PATH` 指向 subos 强行让它用我们的库 → **段错误**,
这是两套 libc 混用的预期结果,不是包的缺陷。

**结论:D5 的打包这一半完成且正确;验收这一半阻塞在 JDK 的 loader 切换上**,那是
`2026-08-08-host-loader-migration-and-ecosystem-guards.md` 的工作。在切换之前,任何
「AWT 起来了」的验证都是宿主在兜底 —— 也就是说,**它验的恰好不是它要验的东西**。
