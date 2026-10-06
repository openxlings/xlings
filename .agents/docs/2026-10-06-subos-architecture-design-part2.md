# SubOS 总体架构设计 Part 2：宿主即 SubOS —— 根呈现、系统包管理器、Luban

- 承接：`2026-10-05-subos-architecture-design.md`（下称 Part 1）。本文**修订** Part 1 的 §4 部署形态、§5 层叠 home、
  §6 目录规范、§8.2 `fetch=layer`、§20 rootfs 和 C26 / C27；其余章节不变。
- 交付：与 Part 1 同一个 PR（#641），checkpoint 接着编号（C28 起，§16）。
- 状态：设计草案第二轮（第一轮的 review 意见已并入：§1 的 D1、D3、D7、D9；§2.4 的生态实测；§4 的两种布局）。
  本文不含代码改动。
- 隐私规则同 Part 1：实测只记录计数、是/否和名字；本文的实测都在临时 home 里完成。

---

## 0. 一句话

**SubOS 是"一份环境"，宿主是其中一份。** 一个 SubOS 的内容（工作区里的包和版本、home、策略）与它的
**呈现方式**（进程怎样看到它）是两件事。现在的 shell 级进入和沙箱是两种呈现方式；本文加第三种 ——
**根呈现**：SubOS 本身就是 `/`。

- 在别人的发行版上（U / C / P / S / M），宿主是一个 xlings 不管理的"底座"；
- 在 xlings 自己就是系统包管理器的机器上（R），宿主就是 `default` 这个 SubOS 的根呈现；
- 一个 `--rootfs` 实例是以根呈现进入沙箱的 SubOS；它里面的 xlings 看到的又是 R 模式（嵌套）；
- **Luban** = kernel + xlings + （可选）LubanOS 服务；一款 Luban 发行版就是一个可以作为宿主启动的
  SubOS，官方版本（tiny / core / desktop …）是 subos 类型的 xpkg，任何人都可以 `--from` 它定制自己的发行版。

rootfs（Part 1 的 C27）、发行版模式、`fetch=layer`、多用户系统层（M）是同一个模型的四个用法，一起实现。

---

## 1. 本轮确认的决策（维护者，2026-10-06）

| # | 问题 | 决定 |
|---|---|---|
| D1 | R 模式下 xlings home 的路径（会写进所有 payload） | **两种都支持**（第二轮）：`/xlings`（多用户，推荐）和 root 安装到某个用户的 `/home/<u>/.xlings`（单用户）。见 §4 |
| D2 | 系统视图用 shim 还是直接链接 | **直接链接**；shim 只给用户层和项目层 |
| D3 | 库搜索 | **两个都支持，按消费者分，不做开关**（§7，第二轮确认） |
| D4 | 缺的基础工具 | 补到 xim-pkgindex |
| D5 | Luban 的定义 | kernel + xlings + LubanOS 服务（可选）；每个人都能基于 xlings 生态定制发行版，像 SubOS 一样；统称 Luban；官方出几个默认版本（tiny / core / desktop 一类的分级，名字待定） |
| D6 | R 模式下宿主 SubOS 的名字 | **`default`** |
| D7 | "把验证过的实例应用到系统" | 维护者的模型：**配置默认启动的 SubOS**（它带"宿主"标识，有特殊性），运行时修改配置，**重启生效**。本文据此把 Part 2 初稿的 `apply` 改成"启动项"（§8）；第二轮确认 |
| D8 | 范围 | **所有 SubOS 相关功能一起做，在本 PR 上** |
| D9 | 第一轮 §17 的待定项 | 维护者："按合适的建议，推荐的先做"。逐项结论见 §17.1，顺序见 §16 |
| D10 | "生态里很多包已经不依赖宿主" | 实测见 §2.4：**运行期成立，安装期不成立**；据此新增 C30a（自带的安装工具集）和 elfpatch 失败即报错 |

---

## 2. 现状（实测）

### 2.1 部署形态 S 的主路径不可用（main 上已有，#641 的测试没覆盖到）

把编译出的二进制放在任何 home 之外（模拟 `/usr/bin/xlings`），用一个空的临时 `HOME` 当新用户：

```
xlings install cmake -y   → 9 个包安装成功，exit 0
cmake --version           → 宿主的 3.28.3（不是刚装的 4.4.2）
xlings self init          → "init ok"，<home>/bin/ 是空的
xlings use cmake          → 显示 4.4.2 当前，subos/default/bin/ 仍为空
xlings self doctor --fix  → ✗ shim table 10 missing，修不好，也不说原因
```

`xself::sync_shim_tables()`（`src/core/xself/init.cpp:649`）在 `<home>/bin/xlings` 不存在时按"还没 `self init`"
跳过，而 S 形态下没有任何流程会放这个文件。已发布的 2026.10.4.1 行为相同。

### 2.2 其他

| 项 | 现状 |
|---|---|
| AUR 包 / Arch CI | `config/aur/PKGBUILD` 停在 `pkgver=0.4.14`；workflow 只在 `config/aur/**` 改动时触发；还在用已删除的 `self config --adduser` |
| M（系统层） | `home::system_layer()` 能识别 `/opt/xlings`（`mode: multi`）并在 doctor 报告；`install --system` 不存在；从系统层解析包（HOME-LAYER-RESOLVE）推迟 |
| rootfs | 未实现；gate `RootfsRuntime` 报 "not in this release" |
| generations | `profile::commit / rollback` 只记录**选择**（包 → 版本），回滚 = 重新跑一遍 `use`；没有可原子切换的物化树 |
| subos 类型 xpkg | 骨架 + `.xlings.json` 工作区声明；`--from` 复制它（COW 文件系统上零成本）；4 个 e2e 测试 |
| 实例目录 | 本来就是 FHS 形状的：`bin/`、`lib/`、`usr/include/`（sysroot 模型），gcc 用 `--sysroot=<subos 目录>` |
| 基础包（fixture 索引） | 有 glibc、busybox（静态 musl）、util-linux、ncurses、openssl、zlib、xz、perl、python、make、ca-certificates、linux-headers；**没有 bash、coreutils、shadow / 用户生成、init、内核** |
| `default` | 已经特殊：全局作用域；不能删除（`src/core/subos.cpp:1433`） |

### 2.3 glibc loader 的实测（临时 home，glibc 2.44.3-r1）

| 怎样调用 loader | 默认搜索目录 | `ld.so.cache` / `ld.so.preload` |
|---|---|---|
| payload 路径（受管二进制的 `PT_INTERP`） | payload 的 `lib/` | payload 的 `etc/`（不存在 → 不读） |
| **经过一个符号链接作为 `PT_INTERP`**（`<root>/lib64/ld-linux-x86-64.so.2 → payload`） | **`<root>/lib64`，即调用路径的目录** | 仍是 payload 的 `etc/` |

结论：`glibc-2.44-default-dir-follows-loader.patch` 跟随的是**调用路径**（字面），不是 realpath。所以根呈现里
`/lib64/ld-linux-x86-64.so.2` 链接到 payload 之后，外来二进制**今天就会**在 `/lib64`（合并后的 `/usr/lib64` 视图）里找库，
不需要改 glibc。改 glibc 只为 `ld.so.conf` / cache 这一半（§7）。

### 2.4 "生态里的包不依赖宿主"：实测（第二轮）

样本：在临时 home 里安装 glibc、make、xz、zlib、ncurses、openssl、curl、perl、python、cmake、util-linux、busybox、gcc
（连同依赖共 27 个 payload，1.4 GB）。"空根"指一个 bwrap：只有这个 home（同一路径）、`/proc`、`/dev`，**没有 `/usr`、
`/lib`、`/etc`**；探针用 busybox 的 `sh`、`grep`。

**运行期：成立。**

| 检查 | 结果 |
|---|---|
| payload 里的 ELF 按形态分类（Part 1 之前的闭包设计：H = 宿主 loader，X = 我们的 loader） | X 可执行 88、静态 4、共享库 356、其他 22；**形态 H 的可执行文件：0** |
| 88 个形态 X 可执行文件，在空根里用 payload loader `--list` 解析闭包 | **88 / 88 全部解析在 home 内**，没有 `not found`，没有任何 home 之外的路径 |
| 在空根里运行 | python（`import ssl, ctypes, lzma, sqlite3`；OpenSSL 3.5.5）、cmake 4.4.2、make 4.3、perl 5.44.0、openssl 3.1.5、xz 压缩往返：**全部 exit 0** |
| python 进程的 `/proc/self/maps` | home 之外的映射：**0** |
| gcc / g++ 编译并运行 hello（C、C++） | 根里有 `/bin/sh` 时通过；没有时失败（见下 G2） |
| 静态 xlings 在空根里 `self init`（只多绑一个 `/etc/resolv.conf`） | 成功：索引获取、构建都不需要宿主 |
| payload 的 `RUNPATH` / `RPATH` 里有构建机路径吗 | **没有**。构建机路径 `/home/xlings/.xlings_data` 出现在 204 个文件里，都是调试字符串、`.la`、binutils 的链接脚本（`=` 前缀，按 sysroot 解析） |

**安装期：不成立。** 用 home 在 `/xlings` 的空根执行 `xlings install xz cmake`：

| # | 发现 | 位置 | 结论 |
|---|---|---|---|
| G1 | hook 依赖宿主工具：libxpkg 用 `sh -c` 执行命令；glibc 的 install hook 用 `io.popen("find …")`，expat / fontconfig 的 config 用 `cp -a`；303 个 recipe 里 130 个调用外部命令（`chmod`、`bash`、`ls`、`tar`、`find`、`grep`……） | libxpkg + recipe | 空根里 glibc、expat、fontconfig 安装失败 |
| G1b | elfpatch 依赖 PATH 上的 `patchelf`；找不到时 `skip patching`，**包仍报告"已安装"** | installer | 一次静默成功：装出来的 payload 没有被 patch |
| G2 | 经 shim 调用 `gcc`，根里没有 `/bin/sh` 时静默返回 255；直接调用 payload 的 gcc 则报 binutils 的 `xlings-wrappers/ld`（`#!/bin/sh` 脚本，还用到 `dirname`）无法执行 | shim / binutils recipe | R 里有 `/bin/sh`，但 shim 和受管包装脚本不应依赖根里的 shell |
| G3 | perl 5.44.0 的 29 个脚本 shebang 是 `/build/stage/bin/perl`：`perldoc`、`pod2man`、`cpan`、`prove`……**在任何机器上都无法执行**（已在宿主上复现：`required file not found`） | perl recipe | 生态缺陷，与 R 无关，单独修 |
| G4 | 没有 `<home>/bin/xlings` 时 gcc 的 config 调 `<subos>/bin/gcc-specs-config`（shim）失败，gcc 安装失败 | §2.1 的同一个根因 | C28 的优先级再提高 |
| G5 | payload 的 `RUNPATH` 写死了 `<home>/subos/default/lib` | elfpatch | 在嵌套的 R 视图里 `/xlings/subos/default` 就是实例自己（§3.3），这个路径正好成立；**必须保持这个映射** |
| G6 | 脚本解释器：`/bin/sh` 27、`env python3` 28、`/bin/bash` 4、`env perl` 3、`env bash` 1 | 各包 | 根里必须有 `/bin/sh`、`/bin/bash`、`/usr/bin/env`：tiny 的最低要求 |

结论：**包本身已经可以脱离宿主运行；装包的过程还离不开宿主的工具。** 由此确定的做法（C30a，实施时第三轮修订）：

- **安装期的工具从"这个根"来，不从 xlings 自带**：在 U / S / M 机器上往一个前缀域装包时，bwrap 把宿主的 `/` 只读挂入、
  把前缀域 home 绑到它的逻辑路径（§4）—— hook 照常用宿主的 `sh`、`find`、`cp`，装出来的路径是逻辑路径；在 R 里装包，
  工具来自 R 自己的 `/usr`。所以 tiny 必须包含 `sh`、coreutils 类工具和 patchelf（busybox + 静态 patchelf），
  DOM-BOOTSTRAP 在 tiny 的容器里验证。不另做一套"安装工具集"：那会让 hook 在 U 模式下换一套工具，行为与今天不同。
- **elfpatch 找不到 patchelf 时自动安装 `xim:patchelf`**（上游预编译版本是静态的，不需要被 patch），然后继续；仍然拿不到时
  这个包**安装失败**并说明原因，不再 `skip patching` 后报告"已安装"。
- shim 的 alias 展开在进程内完成，不经过 `/bin/sh`；binutils 的 ld 包装脚本改成不依赖 `dirname`（跨仓库）。

---

## 3. 核心模型

### 3.1 SubOS = 内容 × 呈现方式

| 呈现方式 | 进程看到的 | 谁用 |
|---|---|---|
| **PATH 叠加**（已有） | 宿主的根；SubOS 的 `bin/` 在 PATH 前面 | U/C/P/S/M 下的 shell 级进入 |
| **沙箱视图**（已有） | 宿主的 `/usr` 只读挂入，叠加 SubOS 的工具、home、`etc-neutral` | `--sandbox` |
| **根呈现**（新） | SubOS 本身就是 `/`：`/usr` 是它的物化视图，`/lib64/ld-linux…` 是它的 loader | R 的宿主；`--rootfs` 实例；导出的镜像 |

内容不变，呈现方式可以换：同一个 SubOS 可以 PATH 叠加地用、沙箱里用、作为 rootfs 实例进入、导出成镜像、
作为宿主启动。**测试的核心不变量**：同一份声明，在每一种根呈现（bwrap 实例、chroot、容器、启动）下得到的树完全一致（§14）。

### 3.2 底座与分层

所有形态都有一个底座，分层规则（Part 1 §5）在所有形态下一致：

```
底座（宿主）  →  系统层 /xlings（M、R）  →  用户层 ~/.xlings  →  实例层
```

| 形态 | 底座 | 底座归谁管 |
|---|---|---|
| U / C / P / S / M | 别人的发行版 | 不管理；只读；就是现在的"宿主" |
| R | `default` 的根呈现 | xlings |

三条分层规则不变：读和激活可以跨层；获取只写自己的层（没有可写层的交给 broker）；上层永远不引用下层。

### 3.3 嵌套：`fetch=layer` 的答案

`--rootfs` 实例里的 xlings 看到的就是 R 模式：它是这个根的包管理器，`/xlings/subos/default` 就是这个实例本身。
"沙箱里的 xlings 把包装进实例自己的层"（Part 1 §8.2 的 `layer`）因此不再是单独的设计：就是在这个根的系统作用域里装包。
PERM-FETCH-LAYER 与 HOME-LAYER-RESOLVE 由同一套查找机制解决（§10）。

### 3.4 角色：`host` 是运行时事实，不是名字

哪个 SubOS 现在是 `/`，它就带 `host` 角色。`default` 是首次安装的那个、也是默认启动项，但任何根呈现的 SubOS
都可以被启动成 host（§8）。模型里实例有 `kind`（`view` / `rootfs`）和运行时的 `role`（`host` 或无），
**每个子命令对照一张允许操作表**，不在各命令里各自判断（§8.3）。

---

## 4. 前缀域（这一节决定 home 放在哪里）

recipe 会把真实的 `install_dir()` 写进装出来的文件（gcc specs、pkg-config、rpath、glibc 的 255 字节重定位前缀；
见 `src/core/xim/payload.cppm:262`），elfpatch 把 `PT_INTERP` 写成 payload 的绝对路径。所以：

> **payload 只能在同一个前缀下共享。** 把装 payload 时 home 的逻辑路径叫作它的**前缀域**。

前缀域就是 home 的逻辑路径。R 支持两种布局（D1）：

| 布局 | 系统 home | 谁拥有 | 适合 | 用户自己的包 |
|---|---|---|---|---|
| **multi**（推荐） | `/xlings`，root 拥有 | root | 多用户机器、服务器、官方 Luban 镜像 | 各自的 `~/.xlings` 叠在上面（M 的规则） |
| **single** | root 安装到 `/home/<u>/.xlings`，`<u>` 拥有 | `<u>`（**等价于 root**） | 个人机器、开发容器、WSL 里的个人发行版 | 就是系统 home 本身，不分层 |

- single 布局下 `/usr` 链接进一个用户可写的目录，那个用户改得了 root 运行的程序，所以**它就是 root**（与 Homebrew 在 macOS
  上的取舍相同）。`self doctor` 在 single 布局的机器上发现别的可登录用户时报警告，建议改用 multi。
- single 布局要求系统 home 在 stage-0 时可达：`/home` 在根分区上，或者 stage-0 按 `/etc/fstab` 先挂好它（§8.2）。
- 布局写在 `.xlings-home`：`{ "mode": "root", "layout": "multi" | "single" }`；stage-0 从 `/etc/xlings/root.json`
  （机器配置，`{ "home": "/xlings" }`，缺省就是 `/xlings`）找到它。

前缀域决定共享：

| 前缀域 | 逻辑路径 | 谁在里面 |
|---|---|---|
| 用户域 | `~/.xlings` | U/C/P/S 的包；R-multi 机器上用户自己装的包；**R-single 的整个系统** |
| 系统域 | `/xlings` | M 的系统层、R-multi 的宿主、多用户的 rootfs 实例、官方镜像 |

由此：

- **系统层（M）也放在 `/xlings`**，不再是 Part 1 的 `/opt/xlings`。`/opt/xlings` 是本 PR 的 C26 引入的、
  还没发布，直接改，不需要兼容。好处：同一台机器上 M 的系统层、rootfs 实例、R 宿主共用一个 store。
- **U 机器上的 `--rootfs` 实例默认用用户自己的 home 作前缀域**：实例里 home 挂在同一路径，用户已经装过的 payload
  直接共享，不用重新下载。`--domain /xlings` 显式选择系统域（要导出成多用户镜像时）；机器上没有 `/xlings` 时，系统域的
  物理位置是 `<home>/domains/xlings/`，只在 bwrap 里挂在 `/xlings`；有 `/xlings`（M 或 R）时只读复用它，用户私有域叠在上面。
- 导出的镜像保留它的前缀域：single 布局的镜像里系统 home 就在 `/home/<u>/.xlings`。
- **在根里面安装，不在外面拼路径**：往系统域装包，一律在 bwrap 里执行 —— 目标树绑成 `/`，域 home 绑在 `/xlings`，
  user namespace 里映射成 uid 0，宿主上这个静态 musl 的 xlings 绑到 `/xlings/bin/xlings`。装出来的路径天然就是根里的路径，
  **所有 recipe 一行不改**。hook 需要的工具来自安装工具集（C30a，§2.4），不来自宿主。没有 user namespace 的机器
  （Ubuntu 24.04 未修复）给出 Part 1 的 `self doctor --isolation --fix`。
- **store 在实例里只读可见**，与 Part 1 的沙箱一致（"沙箱里能看到 xlings home，但只读"）。只挂入闭包（看不到别的实例装了什么）
  需要逐个 payload 绑定和闭包计算，作为同一个规则的收紧放在 C35 之后（ROOT-STORE-CLOSURE，§14）。

为什么多用户用 `/xlings` 而不是 `/var/lib/xlings`（初稿的倾向）：它在任何发行版上都不和别人的 FHS 冲突（U 机器上
rootfs 实例也能用它），路径短（重定位前缀有 255 字节上限），并且不随发行版习惯改变。

---

## 5. 部署形态（修订 Part 1 §4）

| 形态 | entry | home | 前缀域 | 更新渠道 | `.xlings-home` 的 `mode` |
|---|---|---|---|---|---|
| U 用户 | `~/.xlings/bin/xlings` | `~/.xlings` | 用户域 | `self update` | `user` |
| C 自定义 | `<dir>/bin/xlings` | 任意 | 用户域 | `self update` | `user` |
| P 便携 | `<dir>/bin/xlings` | entry 所在树 | 用户域 | `self update` / `self relocate` | `portable` |
| S 系统包 | `/usr/bin/xlings`（别人的包管理器） | 每个用户的 U | 用户域 | 系统包管理器 | `user` |
| M 多用户系统层 | 同 S | S + `/xlings` | 用户域 + 系统域 | 同 S；管理员 `sudo xlings install --system` | `/xlings` 上是 `system-layer` |
| **R 根（multi）** | `/usr/bin/xlings → /xlings/bin/xlings` | `/xlings`；用户另有 `~/.xlings` | 系统域 + 用户域 | **`sudo xlings self update` = 在宿主 SubOS 里装 `xim:xlings`** | `root` / `multi` |
| **R 根（single）** | `/usr/bin/xlings → /home/<u>/.xlings/bin/xlings` | `/home/<u>/.xlings` | 用户域 | `xlings self update`（`<u>` 等价于 root） | `root` / `single` |

**S 的修复**：从系统 entry 运行、而用户 home 没有 `bin/xlings` 时，`self init` / 首次安装 / `doctor --fix` 在用户 home
建 `bin/xlings → <系统 entry>` 的**符号链接**。包管理器升级本体后所有用户跟着升级，不会出现两个 xlings；`self update`
照样拒绝（实际执行的文件仍是系统 entry）；`--user` 显式换成用户自己的副本。doctor 把"有系统 entry、home 里没有链接"报成
可修复的错误，而不是 `10 missing`。

**R 下的 `self update`**：xlings 本身就是宿主 SubOS 里的 `xim:xlings` 包，`self update` 需要 root，走宿主 SubOS 的
generation 切换（§6.3），可以回滚。`describe_entry` 识别"我是这个根的包管理器"（`/xlings/.xlings-home` 的 `mode: root`），
不再提示"交给系统包管理器"。

---

## 6. 根呈现

### 6.1 一棵根里什么归 SubOS，什么归机器

| 路径 | 归属 | 说明 |
|---|---|---|
| `/usr` | **SubOS**（只读视图） | 由宿主 SubOS 的当前 generation 物化：`/usr` → `<系统 home>/subos/<host>/root/usr` |
| `/bin` `/sbin` `/lib` `/lib64` | 固定链接（merged-usr） | `→ usr/bin`、`usr/bin`、`usr/lib`、`usr/lib64`；切换 SubOS 时不动 |
| `/etc` | **机器** | 真实目录；SubOS 只提供默认值（`/usr/share/factory/etc`）和声明（用户、组、tmpfiles） |
| `/home` `/root` `/var` `/srv` | **机器** | 用户数据；切换宿主 SubOS 时保留 |
| `/xlings`（single 布局：`/home/<u>/.xlings`） | 系统 home | store、各 SubOS、`boot.json`、审计 |
| `/etc/xlings/root.json` | 机器 | 系统 home 在哪（stage-0 的锚点） |
| `/proc` `/sys` `/dev` `/run` `/tmp` | 运行时 | stage-0 或容器运行时挂载 |

同一份内容在 rootfs 实例里呈现时，"机器"那几项来自实例自己的目录（`subos/<n>/etc`、`home/`、`var/`），规则相同。

### 6.2 物化：直接链接（D2）

- `usr/bin/<cmd>`、`usr/lib/<so>`、`usr/share/...` 是**直接指向 payload 的链接**，不经过 shim：PID 1、`/bin/sh`、每一次
  fork + exec 都不能先经过 xlings，xlings 坏了系统也要能启动。
- 视图里有什么，按普通发行版的语义：**一个包的 `bin/` 就是它放进 `/usr/bin` 的东西，`lib/` 里的 `lib*.so*` 就是它放进
  `/usr/lib` 的东西**。先放工作区里激活的程序和库（xvm 的注册，名字以注册为准），再放各激活包 payload 的 `bin/`、`lib/`、
  `lib64/` 里其余的可执行文件和共享库（busybox 的 applet 链接就是这样进入 `/usr/bin` 的）。同名时注册的优先，其次按工作区顺序
  的第一个声明者；冲突写进这一代的清单，不静默覆盖。
- 带 alias 参数的程序（例如 gcc 的 `--sysroot=${XLINGS_DYNAMIC_SUBOS_DIR}`）链接到 xlings 的 shim：参数要在执行时展开。
  启动关键路径上的程序（`sh`、init、coreutils）没有 alias，都是直接链接。
- `usr/include` 链接到这个 SubOS 的 sysroot 头文件目录（现有的 sysroot 模型）；`usr/lib64 → lib`、`usr/sbin → bin`。
- xlings 自己是静态 musl 的，不依赖 glibc：glibc 坏了，`/xlings/bin/xlings` 仍能把系统切回上一代（这条是 R 的稳定性基础，
  有专门的测试，§14）。

### 6.3 generation：可原子切换的物化树

`profile` 现在只记录选择。根呈现需要**物化树**：

```
/xlings/subos/<n>/generations/<k>/usr/...   每一代一棵完整的视图（只含链接，很小）
/xlings/subos/<n>/root/usr  →  generations/<k>/usr        当前代：一个符号链接，rename(2) 原子替换
```

- 包变动（install / remove / use）= 生成新的一代 + 原子替换指针；**宿主 SubOS 上这就是"在线升级"**，与传统发行版的 apt 一样即时生效。
- 回滚 = 指针指回上一代（`xlings subos rollback <n> [--to k]`，宿主上需要 root）。
- 旧一代引用的 payload 在这一代被回收前不会被 GC；正在运行的进程映射着的文件不受影响（文件本身没被删）。
- 现有的 `profile` 记录作为每一代的元数据保留，`rollback` 改为切指针，不再重跑 `use`。

### 6.4 `/etc`、用户与组

- 包把默认配置放在 `usr/share/factory/etc/`，用户和组声明在 `usr/lib/sysusers.d/*.conf`（systemd 的格式，不依赖 systemd）。
- 每次切换 generation / 启动时：缺的 `/etc` 文件从 factory 复制，**已有的永不覆盖**；sysusers 只追加缺的用户和组；
  然后跑 ldconfig（§7）。这一步由 xlings 执行（静态二进制，不依赖被切换的那一代）。
- 机器的 `/etc` 是用户数据，适用 AGENTS.md 的 "SubOS user data" 规则。

---

## 7. 库搜索（D3 的答复）：两个都支持，按消费者分

不是一个用户可选的开关。一个进程里有两类二进制，它们需要的东西不同：

| 消费者 | `PT_INTERP` | 搜索 | 读 `ld.so.cache` / `ld.so.conf` |
|---|---|---|---|
| **受管**（xlings 的包，elfpatch 过） | payload 的 loader | `RUNPATH` 闭包；默认目录 = payload 的 `lib/` | **永远不读**（与今天一致，hermetic） |
| **外来**（第三方预编译程序、wheel、厂商驱动、用户自己编译的） | `/lib64/ld-linux-x86-64.so.2` | 默认目录 = `/lib64` = 合并后的 `/usr/lib64` 视图（**今天已经如此**，§2.3） | **读这个根的 `/etc/ld.so.cache`**，由 `/etc/ld.so.conf(.d)` + ldconfig 生成 |

这样受管包在 U / M / R / 实例里行为完全一样；外来程序在 R 里看到的是一个普通发行版（统一视图覆盖绝大多数情况，
`ld.so.conf` 覆盖 `/opt/<vendor>/lib`、NVIDIA 驱动这类装在别处的库）。

**需要的 glibc 改动**（xlings-res，新 revision）：今天 cache 和 preload 的路径是编译进去的 payload `etc/`（§2.3）。
改成与 `default-dir-follows-loader` 同一条规则：**sysconfdir = 调用路径的目录 `/..` `/etc`（字面，不解析链接）**。

- payload 路径调用：`<payload>/lib64/../etc` = `<payload>/etc`，与今天相同（恒等变换）；
- `/lib64/ld-linux…` 调用：`/lib64/../etc` = `/etc`，即这个根自己的 `/etc/ld.so.cache` 和 `/etc/ld.so.preload`，与普通发行版的语义相同；
- U 机器上没有任何东西经 `/lib64` 调用我们的 loader（那是宿主的 glibc），不受影响；rootfs 实例里则是实例自己的 `/etc`。

ldconfig 用宿主 SubOS 当前代的 glibc 自带的 `ldconfig`，在 generation 切换和启动时运行（§6.4）。

---

## 8. 宿主 SubOS 与启动（D7）

### 8.1 启动项

```jsonc
// <系统 home>/boot.json（multi 布局即 /xlings/boot.json）—— 唯一写者：subos 模块；只有 root（single 布局：<u>）能写
{
  "default": "default",            // 默认启动的 SubOS
  "once": null,                    // 只用于下一次启动（试启动）
  "fallback": "default",           // 启动失败时回到这个
  "tries": { "core-2026.11": 1 }   // 启动计数：试启动没有被确认就回退
}
```

```bash
sudo xlings subos boot core-2026.11 --once   # 下次启动试一次；没有确认就回到 default
sudo reboot
xlings subos status                           # 显示 host = core-2026.11 (trial)
sudo xlings subos boot core-2026.11           # 确认：之后默认启动它
xlings subos list                             # default (rootfs) · core-2026.11 (rootfs, host, boot default)
```

- **运行时修改启动项，重启生效**（维护者的模型）。宿主 SubOS 自己的包变动则是在线生效的（§6.3）——两件事分开：
  "换一个发行版"在启动时发生，"在当前发行版里升级"随时发生。
- 典型用法：`subos new trial --rootfs --from default` 克隆当前系统（payload 共享，几乎不占空间）→ 在 trial 里升级、
  作为 rootfs 实例跑你的工作负载 → `subos boot trial --once` → 重启验证 → 确认。这就是初稿里的 `apply`，但不复制、不替换
  正在运行的系统。
- 不重启的切换（不换内核、只重启用户态，相当于 systemd 的 soft-reboot）依赖 init 的支持，作为 `subos boot <n> --now`
  在 init 支持时提供，否则拒绝并说明（C42，§17.1）。

### 8.2 stage-0：xlings 作为最早的用户态

内核参数 `init=/xlings/boot/init`（`→ /xlings/bin/xlings`，多调用名 `init` 进入 stage-0）。xlings 是静态的，
不依赖任何一代 SubOS：

1. 挂载 `/proc` `/sys` `/dev` `/run`；读 `/etc/xlings/root.json` 找到系统 home（缺省 `/xlings`）；single 布局且 home 不在根分区上时，
   按 `/etc/fstab` 先挂好它；
2. 读 `<home>/boot.json`，按 `once` / 计数 / `fallback` 选出宿主；
3. 把 `/usr` 指向它的当前代（§6.1）；factory `/etc`、sysusers、ldconfig（§6.4）；
4. 记录 `boot` 事件到 `/xlings/logs/boot.ndjson`；
5. `exec` 这个 SubOS 声明的 init（`boot.init`：busybox init、systemd、LubanOS 的服务管理器……）。

启动成功由服务管理器在启动完成后执行 `xlings subos boot --mark-good` 确认；没有确认就按计数回退。
容器（docker、WSL）里没有 stage-0：导出时就把 `/usr` 指好（§11），也可以把 `/xlings/boot/init` 作为容器的入口。

### 8.3 `host` 角色的特殊规则（强制执行）

| 操作 | 普通实例 | `host`（当前是 `/`） | 启动项（`default` / `fallback` / `once`） |
|---|---|---|---|
| `use` / `exec` 进入 | 可以 | `use` = 回到宿主；`exec` = 在宿主上执行 | 可以（作为 rootfs 实例） |
| 隔离策略 | dev / private / locked | 不适用（最外层；它给子实例授权） | 作为实例时适用 |
| `remove` / 重置 | 确认后可以 | **永远拒绝** | **拒绝**，先改启动项 |
| 包变动 | owner 或 broker | 只有 root；新的一代 + 原子切换 | 同普通实例 |
| 身份 | 中性（`user` / 实例名） | 真实用户与主机名 | 作为实例时中性 |
| 用户数据 | 实例的 `home/` | 机器的 `/etc` `/home` `/var` `/root` `/srv` | — |
| GC | 只回收不被引用的 | 也不回收**任何一代**、任何启动项引用的 payload | 同左 |

这张表就是代码里的允许操作表；测试对每个 subos 子命令、每种角色各跑一遍（§14 的 ROOT-ROLE-TABLE）。

---

## 9. Luban（D5）

**Luban = kernel + xlings + LubanOS 服务（可选）。** 一款 Luban 发行版是一个可以作为宿主启动的 SubOS：

- **版本即 subos 类型的 xpkg**：`subos:luban-tiny`、`subos:luban-core`、`subos:luban-desktop`（分级名字待定）。
  包里是工作区声明（基础工具和运行时）、`boot` 声明（内核包、init）、平台锚点（glibc / gcc 等一组版本，
  沿用 `2026-08-05-ecosystem-three-tier-and-composable-distro.md` 的 platform manifest）。
- **分级叠加**：desktop `from` core，core `from` tiny。subos 类型 xpkg 增加 `from` 字段，fork 时按链合并工作区，
  下层的版本被上层覆盖。
- **定制自己的发行版**，和定制一个 SubOS 是同一组命令：

```bash
xlings subos new mydistro --rootfs --from subos:luban-core@2026.10
xlings subos exec mydistro -- xlings install nginx postgresql
xlings subos export mydistro --oci mydistro.tar      # 容器镜像
xlings subos export mydistro --disk mydistro.img     # 可启动的磁盘镜像（ext4，内核 + init= 已就位）
xlings subos pack mydistro --as myns:mydistro@1.0    # 打成 subos 类型 xpkg，发布到你自己的索引
# 别人：xlings subos new x --rootfs --from myns:mydistro@1.0
```

- **第一版官方分级**（D4：缺的包补到 xim-pkgindex）：

| 分级 | 内容 | 本 PR 的验证 |
|---|---|---|
| tiny | 内核、xlings、glibc、bash、coreutils、util-linux（最小）、ca-certificates、用户生成、一个最小 init | 容器、chroot、rootfs 实例、qemu 启动到 shell |
| core | tiny + 服务管理、网络、ssh、sudo / shadow、编译工具链（gcc / binutils / make） | 容器、rootfs 实例、qemu 启动后网络可用、在里面编译程序 |
| desktop | core + 图形栈（已有的 mesa / wayland / X 相关包）、字体、音频 | 容器里冒烟 + 无头离屏渲染（llvmpipe 渲染一帧并校验像素）；图形会话放到之后（§17.1） |

---

## 10. M：多用户系统层（修订 C26）

- `sudo xlings install --system <pkg>`：装进 `/xlings`（系统域），在系统层的 `default` 里激活。
- 用户解析：读和激活可以跨层 —— 用户 home 的工作区可以选择系统层的 payload，**不复制**（HOME-LAYER-RESOLVE）。
  用户自己装的同名包优先；用户写不了系统层（root 拥有，0755）。
- 前缀域的约束：系统层 payload 的路径前缀是 `/xlings`，用户层是 `~/.xlings`，两者各自成立；激活只是链接，不涉及重定位。
- `fetch=layer`（沙箱里的 xlings 装进实例自己的层）与这一节用同一个查找函数：实例层 → 用户层 → 系统层。
- R 机器上的普通用户就是 M 的用户：系统是 `/xlings`，自己的是 `~/.xlings`。

---

## 11. 使用面（新增 / 修改的命令）

| 命令 | 作用 |
|---|---|
| `subos new <n> --rootfs [--from <subos 或 xpkg>]` | 建一个根呈现的实例；`--from default` 克隆当前系统 |
| `subos new <n> --rootfs --domain /xlings` | 用系统域（要导出成多用户镜像时）；缺省是当前 home 的前缀域，共享已经装过的 payload |
| `sudo xlings self install --root [--layout multi\|single]` | 把一台机器（或一棵根）初始化成 R：写 `/etc/xlings/root.json`、系统 home、`default` 的根呈现 |
| `subos use/exec <n>`（rootfs 实例） | 以它为根进入；策略、broker、审计照常；里面 `id -u` = 0（user namespace 映射） |
| `subos export <n> --rootfs <dir> \| --tar <f> \| --oci <f> \| --wsl <f> \| --disk <img>` | 导出：闭包里的 payload 复制到 `/xlings`；`--disk` 生成 ext4 镜像（`mkfs.ext4 -d`，不需要 root） |
| `subos pack <n> --as <ns:name@ver>` | 打成 subos 类型 xpkg |
| `subos rollback <n> [--to k]` | 切回某一代 |
| `subos boot <n> [--once] [--now]` / `subos boot --mark-good` | 启动项 |
| `subos diff <a> <b>` | 两个 SubOS 差在哪些包和版本 |
| `sudo xlings install --system <pkg>` | M：装进系统层 |
| `subos list` / `status` | 标注 `kind`、`role`、启动项、当前代 |

interface 同步：`subos_new` 的 `rootfs`、`subos_export`、`subos_boot`、`subos_rollback`；agent 模式下不会停下来等输入。

---

## 12. 安全与用户数据

- 构建与进入 rootfs 都在 user namespace 里映射 uid 0（多个 uid 用 `newuidmap` + `/etc/subuid`，没有就只映射一个），
  **不需要 setuid，不需要 root**（Part 1 §20 已确认的前提）。
- 宿主角色的拒绝规则（§8.3）由允许操作表强制；`subos remove` 遇到启动项拒绝。
- 机器状态（`/etc` `/home` `/var` `/root` `/srv`）和 rootfs 实例的同名目录都是用户数据；`subos export` 默认**不**包含
  `home/` 与 `var/`（`--with-data` 显式要求时才包含，并提示）。
- `boot.json`、每一代、stage-0 的选择都写审计事件；stage-0 的日志在 `/xlings/logs/boot.ndjson`。

---

## 13. 跨平台

| 平台 | 根呈现 |
|---|---|
| Linux | 本文全部 |
| Windows | `subos export --wsl` → `wsl --import`：Luban 跑在 WSL 里就是 R 模式；本机上仍是 home 重定向（Part 1） |
| macOS | 不提供；gate `RootfsRuntime` 报 unavailable，路线是轻量 VM（Part 1 附录 A） |

---

## 14. 需求与测试

新增需求 ID（进 `tests/requirements.toml`，全部 `required`）：

| ID | 内容 | 车道 |
|---|---|---|
| DEPLOY-S-ENTRY | 系统 entry：用户装包后能直接运行；包管理器升级后跟随；`self update` 拒绝、`--user` 放行 | isolation-fix（sudo） |
| DEPLOY-S-AUR | 真实 AUR 包（当前构建）：两个用户各自装包、运行 | arch（容器，PR 触发） |
| DEPLOY-M-SYSTEM | `install --system`；用户不复制地使用；用户覆盖；用户写不了系统层 | isolation-fix（sudo） |
| HOME-LAYER-RESOLVE / PERM-FETCH-LAYER | 由 deferred 改为 required | L3 + rootfs |
| DOM-PREFIX | 被搜索的位置（`PT_INTERP`、`RPATH`/`RUNPATH`、`.pc`、`.la`、无 `=` 前缀的链接脚本、shebang）里只有本前缀域的路径；构建机路径一个也没有（G3 的 perl 就是反例） | rootfs + L1（扫描器） |
| DOM-BUILD-INSIDE | 装进系统域一律在根里执行；不改 recipe | rootfs |
| DOM-BOOTSTRAP | 空根（没有 `/usr`、`/etc`）里 `install` tiny 的全部包成功；hook 只用安装工具集 | rootfs |
| DOM-ELFPATCH-FAILCLOSED | elfpatch 不可用时安装失败并说明原因，不报告"已安装" | L3 |
| DOM-LAYOUTS | multi（`/xlings`）与 single（`/home/<u>/.xlings`）两种布局各构建、导出、启动一次 | container + boot |
| ECO-HOST-INDEPENDENT | §2.4 的检查变成测试：tiny / core 的每个形态 X 可执行文件在空根里闭包完整；代表程序在空根里运行，`/proc/self/maps` 里没有根外路径 | rootfs |
| SHIM-NO-SHELL | shim（含 alias 展开）在没有 `/bin/sh` 的根里也能分发 | L3 + rootfs |
| ROOT-PROJECT | 根呈现的树：merged-usr 链接、`/usr` 指向当前代、直接链接无 shim | rootfs |
| ROOT-SAME-TREE | 同一份声明在 bwrap 实例、chroot、容器、启动下的树完全一致 | rootfs + container + boot |
| ROOT-GEN-ATOMIC | 切换是一次 rename；中途杀掉进程，指针只会是旧的或新的 | rootfs |
| ROOT-ROLLBACK | 回滚切指针，不重跑 `use` | rootfs |
| ROOT-SURVIVES-GLIBC | glibc 被破坏后，静态 xlings 仍能回滚并恢复 | container |
| ROOT-ETC-FACTORY | factory 只补缺；已有 `/etc` 永不覆盖；sysusers 只追加 | rootfs |
| ROOT-LIBSEARCH | 受管二进制不读 cache；外来二进制经 `/lib64` 用统一视图和本根的 `ld.so.cache` | container |
| ROOT-NO-HOST | 根里进程的 `/proc/self/maps` 没有任何宿主路径 | rootfs + container |
| ROOT-ROLE-TABLE | 每个 subos 子命令 × 每种角色，按允许操作表接受或拒绝 | L3 |
| ROOT-SELF-UPDATE | R 下 `self update` = 宿主 SubOS 的新一代，可回滚 | container |
| INST-ROOTFS | `--rootfs` 实例：uid 0 映射、看不到宿主 `/usr`、策略 / broker / 审计照常、只看到自己的闭包 | rootfs |
| EXPORT-OCI / EXPORT-WSL / EXPORT-DISK | 导出物能被 docker / `wsl --import` / qemu 使用 | container / windows / boot |
| BOOT-STAGE0 | qemu 启动：stage-0 选择宿主、`/usr` 就位、exec init、打出标记 | boot |
| BOOT-ONCE-FALLBACK | 试启动未确认 → 下次回到 fallback；确认后成为默认 | boot |
| BOOT-NOW | `subos boot --now` 在声明支持的 init 下切换用户态；其余 init 拒绝并说明 | boot |
| LUBAN-DESKTOP-RENDER | desktop 在容器里离屏渲染一帧，像素校验通过 | container |
| LUBAN-TINY / LUBAN-CORE | 两个分级从 xpkg 创建、导出、启动、在里面装包 | container + boot |
| LUBAN-FROM-CHAIN | `from` 链合并工作区，上层覆盖下层 | L3 |

CI 车道（Part 1 §24.5 的架构，新增三条）：

| 车道 | 环境 | 内容 |
|---|---|---|
| **rootfs** | ubuntu runner + bwrap（C21 修复后） | 构建 tiny / core 的 rootfs 实例；ROOT-* 里不需要容器和启动的部分 |
| **container** | ubuntu runner + docker | `export --oci` → `docker import` → `docker run --network none` 里跑 ROOT-* / LUBAN-* |
| **boot** | ubuntu runner + qemu（有 `/dev/kvm` 就用，否则 TCG） | `export --disk` + 内核包 → `qemu -kernel … init=/xlings/boot/init`，串口里断言标记；试启动与回退 |
| isolation-fix（已有） | sudo | S、M |
| arch（修复） | archlinux 容器，PR 触发 | AUR 包 |
| windows（已有，加一步） | `wsl --import`（runner 只有 WSL1 时作为尽力项：只报告不阻塞，§17.1） | EXPORT-WSL |

网络与确定性：基础包的 payload 用 actions/cache 按索引提交缓存；PR 车道不在测试里联网（Part 1 §24.5 的规则），
索引固定在 fixture 的提交上（固定的是索引，不是 xlings 版本，不违反 "Never pin a released xlings version into CI"）。

---

## 15. 性能预算（进 Part 1 §21.1 的断言）

| 项 | 预算 |
|---|---|
| 生成一代（core，约 300 个包的链接） | ≤ 1 s |
| 切换一代（rename） | ≤ 10 ms |
| 进入 rootfs 实例（含闭包绑定） | ≤ Part 1 沙箱进入预算 + 100 ms |
| stage-0（启动到 exec init） | ≤ 200 ms |

---

## 16. 交付：C28 起（同一个 PR）

顺序按"推荐的先做"（D9）：先修已经影响用户的（S），再把最小的 tiny 在 rootfs 车道里真正跑通（风险最大的一段），
然后才往上加启动、导出和分级。

| # | checkpoint | 依赖 | 验证 |
|---|---|---|---|
| C28 | S：用户 home 的 entry 链接；doctor 报告与修复（同时修好 G4：gcc 在这种 home 里装不上） | — | DEPLOY-S-ENTRY |
| C29 | AUR 包更新到当前 release 流程；Arch CI 在 PR 上触发 | — | DEPLOY-S-AUR |
| C30 | 前缀域：两种布局、`--domain`、用户私有系统域、在根里安装的执行器 | — | DOM-PREFIX / BUILD-INSIDE / LAYOUTS（构建部分） |
| C30a | 在前缀域里安装（宿主 `/` 只读 + 域 home 在逻辑路径）；缺 patchelf 自动安装 `xim:patchelf`，仍失败则包安装失败；shim 的 alias 在进程内展开 | C30 | DOM-BOOTSTRAP、DOM-ELFPATCH-FAILCLOSED、SHIM-NO-SHELL |
| C31 | 模型：`kind` / `role`、允许操作表 | — | ROOT-ROLE-TABLE |
| C32 | 根呈现物化：merged-usr、直接链接、generation 树、原子切换、回滚改为切指针 | C30 C31 | ROOT-PROJECT / GEN-ATOMIC / ROLLBACK |
| C33 | `/etc` factory、sysusers、ldconfig 钩子 | C32 | ROOT-ETC-FACTORY |
| C34 | 库搜索：`/lib64` loader 链接；glibc 新 revision 的接入（sysconfdir 跟随调用路径） | C32 + xlings-res | ROOT-LIBSEARCH |
| C35 | `--rootfs` 实例：bwrap 以根呈现进入、闭包绑定、嵌套的 R 视图、uid 0 映射 | C32 C30a | INST-ROOTFS、ROOT-NO-HOST、ECO-HOST-INDEPENDENT |
| **里程碑 A** | **tiny 在 rootfs 车道里从空根构建、进入、在里面装包、跑通 §2.4 的全部检查** | C30–C35 + xim-pkgindex 的 tiny 包 | ROOT-SAME-TREE（rootfs 部分） |
| C36 | 分层查找：M 的 `install --system`、HOME-LAYER-RESOLVE、`fetch=layer` | C30 | DEPLOY-M-SYSTEM、两个原 deferred |
| C37 | `export`（rootfs / tar / oci / wsl / disk）、`pack`、`diff` | C35 | EXPORT-* |
| C38 | R：`mode: root`（两种布局）、`self update` 走新一代、宿主角色规则 | C32 | ROOT-SELF-UPDATE、ROOT-SURVIVES-GLIBC、DOM-LAYOUTS |
| C39 | 启动：`root.json`、`boot.json`、`subos boot`、stage-0、计数与回退 | C38 + 内核包 | BOOT-* |
| C40 | Luban：subos 类型 xpkg 的 `from` 链与 `boot` 声明；tiny、core、desktop | C37 C39 + xim-pkgindex | LUBAN-* |
| C41 | CI 车道 rootfs / container / boot；文档（使用指南加"发行版"场景、隔离模型、命令参考） | 全部 | ROOT-SAME-TREE |
| C42 | `subos boot --now`（不重启内核的用户态切换），只对声明支持它的 init 开放 | C39 | BOOT-NOW |

**跨仓库前置**（先于 #641 合入并发布）：

| 仓库 | 内容 | 阻塞 |
|---|---|---|
| xim-pkgindex | bash、coreutils（形态 X，链接 xlings 的 glibc）；shadow 或等价的用户工具；一个最小 init；静态 patchelf 进安装工具集；**perl 的 shebang 修复（G3）**；binutils 的 ld 包装脚本去掉 `dirname`（G2） | 里程碑 A |
| xim-pkgindex | 内核包（第一版重新打包上游预编译内核，§17.1）；`subos:luban-tiny` / `luban-core` / `luban-desktop` | C39、C40 |
| xlings-res | 上述包的构建；glibc 新 revision（sysconfdir 跟随调用路径，§7） | C34 |

---

## 17. 待定项的结论与风险

### 17.1 第一轮待定项（D9：按推荐，推荐的先做）

| # | 问题 | 结论 | 理由 |
|---|---|---|---|
| 1 | 分级名字 | **tiny / core / desktop**，包名 `subos:luban-tiny` 等 | 维护者的原话；三个词各自说明用途，不需要再解释 |
| 2 | desktop 的验证深度 | **容器里冒烟 + 无头的离屏渲染**（mesa llvmpipe 渲染一帧并校验像素）；qemu 里起图形会话放到之后 | 离屏渲染已经能证明图形栈的闭包完整；图形会话要 virtio-gpu 和长得多的 CI 时间，收益主要在 compositor，不在 xlings |
| 3 | 不重启的切换 `subos boot --now` | **放在本 PR 最后（C42）**，只对声明支持的 init 开放，其余拒绝并说明 | 重启生效的路径（C39）先完整；`--now` 是锦上添花，依赖 init |
| 4 | 内核包 | **第一版重新打包上游的预编译内核**（选一个带 virtio 驱动、模块内置的通用内核）；自己构建作为后续 | 内核与 xlings 的 glibc 无关，自己构建对本 PR 的目标没有增益，却是最长的一条构建链 |
| 5 | Windows runner 只有 WSL1 | **接受作为尽力项**：WSL1 上导入成功、在里面跑 tiny 的检查；失败只报告不阻塞，直到有 WSL2 的 runner | WSL1 是系统调用翻译，不能代表 WSL2 的行为，硬性阻塞会测出与产品无关的问题 |
| 6 | 引导器 / 装到真实硬盘 | **Luban 的安装器负责**；本 PR 做到 `export --disk` + qemu `-kernel` 直接启动 | 引导器与 SubOS 模型无关，属于发行版的安装体验 |

### 17.2 风险（第二轮按 §2.4 的实测更新）

1. **运行期的宿主独立性已经实测成立**（§2.4：88 / 88 闭包完整，代表程序在空根里运行，没有根外映射）。第一轮担心的
   "基础包能否全部链接 xlings 的 glibc 并且自洽"因此降为**新增包的工作量**问题（bash、coreutils 等按现有的形态 X 规则构建），
   不再是模型风险。
2. **新的头号风险是安装期**：hook 依赖宿主工具（G1），而 R 里没有宿主。C30a 用自带的工具集解决，但 130 个调用外部命令的
   recipe 里可能还有工具集没覆盖的命令（例如 `sudo`、`systemctl`、`powershell`，它们属于宿主集成类的包，不进 R）。
   DOM-BOOTSTRAP 先覆盖 tiny / core 的全部包，其余包在 xim-pkgindex 的 CI 里加同一个空根检查，逐步收敛。
3. `/usr` 是符号链接：大多数工具没问题（merged-usr 已经让 `/bin` 是链接），个别服务管理器可能检查 `/usr` 是否是挂载点。
   boot 车道会验证；不行的话 stage-0 改为 bind mount，容器仍用链接。
4. single 布局把一个用户变成等价于 root。只在明确选择时使用；doctor 发现多个可登录用户时警告。
5. PR 规模：#641 现在 182 个文件、+16.4k 行，这一部分估计再加 6k–10k 行，外加两个外部仓库的发布。按 checkpoint 推进，
   每个都能单独 revert；里程碑 A 之前不开始启动和分级的工作。

### 17.3 本轮需要 review

1. §4 两种布局的定义，尤其是 single 布局"那个用户等价于 root"的取舍，以及 U 机器上 `--rootfs` 实例默认用用户自己的 home 作前缀域。
2. §2.4 的结论"运行期成立、安装期不成立"，以及 C30a 的做法：自带安装工具集 + elfpatch 失败即报错 + shim 不依赖 shell。
3. §17.1 六个待定项的结论。
4. §16 的顺序，以及里程碑 A 作为后半部分的闸门。

---

## 18. 实施计划（第三轮：维护者确认 §17.1 与 §17.3 的 1–4，开始实现）

### 18.1 按角度拆分

| 角度 | 任务 | 落在哪个 checkpoint |
|---|---|---|
| 架构 | 呈现方式与 `kind`/`role` 进 `modules/subos`（纯逻辑：投影计算、启动项选择、允许操作表），文件系统动作进适配层，系统调用（mount、switch 等）进 `xlings.platform` | C31 C32 C39 |
| 稳定性 | 一代 = 一次 rename；启动计数与 fallback；静态 xlings 能在 glibc 损坏时回滚；elfpatch 不再静默跳过 | C30a C32 C38 C39 |
| 优雅简洁 | 不新增"rootfs build"概念：`new --rootfs` + `export`；投影规则就是普通发行版的语义（包的 `bin/` 进 `/usr/bin`） | C32 C37 |
| 用户体验 | 每个新命令有 agent 模式与 interface；`subos list/status` 标出 kind、role、启动项、当前代；拒绝时给出可执行的下一步 | C31 C35 C37 C39 |
| 兼容性 | 老实例默认 `kind=view`，行为不变；`subos_info.kind` 是新增键，老客户端忽略；系统层从 `/opt/xlings` 改 `/xlings` 只涉及本 PR 未发布的代码 | C30 C31 C36 |
| 跨平台 | 根呈现只在 Linux；macOS / Windows 的 `--rootfs`、`boot` 返回 unavailable 并给出路线（Windows：`export --wsl`）；每个分支用 `if constexpr` 编译 | C35 C37 C39 |
| 一致性 | 根呈现的树在 bwrap 实例、容器、启动三处同一棵（ROOT-SAME-TREE）；需求 ID 覆盖图不允许未覆盖 | C35 C37 C41 |
| 无感升级 | S 的 entry 链接在下一次任何命令时自动建立；`self update` 在 R 下是新一代；版本号 2026.10.6.1（发布时） | C28 C38 |

### 18.2 依赖

```
C28 ─┐
C29  │
C30 ─┼─ C30a ─┐
C31 ─┴─────── C32 ─ C33 ─ C34 ─ C35 ═ 里程碑 A ─┬─ C37 ─┐
              │                                  ├─ C36  ├─ C40 ─ C41 ─ C42
              └──────────────── C38 ─ C39 ───────┘        │
xim-pkgindex：patchelf 已有；busybox applet 链接；perl shebang（G3）；luban-tiny/core/desktop；内核包 ─┘
xlings-res：bash、coreutils；glibc sysconfdir revision（C34）；内核（重新打包）
```

### 18.3 每个 checkpoint 的完成标准

能编译；已有测试全部通过；本 checkpoint 的需求 ID 有测试覆盖（`tests/requirements.toml`，不允许未覆盖）；
文档同步（使用指南、隔离模型、命令参考自动生成）；CI 全绿后再进入下一个依赖它的 checkpoint。

---

## 19. 实施记录（2026-10-06，PR #641；xim-pkgindex #929）

| checkpoint | 落地 | 与设计的差别 |
|---|---|---|
| C28 S | 首个写命令 / `self init` / `doctor --fix` 把 home 的 entry 链到系统本体并铺好 home；shim 的属主按"自身位置、逐跳"找 | — |
| C29 AUR | PKGBUILD 只装 `/usr/bin/xlings`，去掉以 root 跑 `self uninstall` 的 remove hook；`arch-package` 每个 PR 用本次构建打包、两个用户使用 | — |
| C30 / C30a | 两种布局（multi `/xlings`、single 构建者 home 的路径）；缺 patchelf 先装 `xim:patchelf`，仍没有则失败；纯词的 alias 不经 shell | **不另做 `--domain` 的 bwrap 构建器**：multi 布局在 `/xlings` 里构建（CI 用 sudo），single 用当前 home；装包始终在 home 的逻辑路径上进行，DOM-BUILD-INSIDE 由此成立 |
| C31 | `roles`：kind × role → 一张表；remove / config / cp / boot / export / rollback 都问它 | — |
| C32 / C33 | `rootfs`：投影规则、代、rename 切换、回滚、prune、merged-usr、factory `/etc`、sysusers | 冲突只在两个目标不是同一个文件时记录 |
| C34 库搜索 | 外来程序经 `/lib64` = 根的 `/usr/lib`（实测，无需改 glibc） | **根自己的 ld.so.cache 推迟**（ROOT-LDCACHE）：需要 glibc 新 revision；`/lib64 = /usr/lib` 已覆盖它的用途 |
| C35 | rootfs 实例：树为 `/`、uid 0、home 只读、实例同时是嵌套根的 `default`；里面的安装经 broker、即时出现在 `/usr` | store 在实例里整体只读可见（与 Part 1 一致），闭包级绑定未做 |
| C36 M | `sudo xlings install --system`；profile v13 把系统层放在用户自己之后、宿主之前 | 解析经 PATH（不复制），不把系统层条目写进用户的版本库 |
| C37 导出 | `--rootfs` / `--tar` / `--disk`（`--tar` 即 docker / podman / `wsl --import` 的输入）；`diff`、`pack` | `--oci`、`--wsl` 合并为 `--tar` |
| C38 R | `self update` 在 R 下是新一代（entry 保持为 stage-0），可回滚 | — |
| C39 启动 | stage-0、`boot.json`（once、计数、fallback、mark-good）、起不来的试启动被消耗 | — |
| C40 Luban | `subos:luban-tiny / core / desktop`（模板由 install() 写出，`from` 链合并，上层优先）；bash 5.2.37、coreutils 9.5（xlings glibc 构建）、linux-kernel 6.8.0-71（Ubuntu 预编译重新打包）；busybox applet 链接、perl shebang、binutils ld 包装脚本修复 | 根里编译的程序默认不在 `/usr/lib` 找库（gcc specs 用 payload loader），需要 `-Wl,-rpath,/usr/lib` |
| C41 CI | `distro` 车道（bwrap + docker + qemu/kvm + sudo）：rootfs_instance、rootfs_image、rootfs_boot；`isolation-fix` 加 system_layer；`wsl-import`（WSL1，报告不阻塞） | 场景脚本用 `# xtest:` 头进入需求覆盖图 |
| C42 | `subos boot <n> --now`：busybox init 的 `::restart:` 重新执行 stage-0，不重启内核 | — |

本地实测（CI 之外）：luban-tiny 从导出的 ext4 镜像由 Ubuntu 6.8 内核启动三次——跳过没有 init 的试启动、试启动一次、
`--now` 切回、未确认的试启动不成为默认；bwrap 实例与启动后的机器 `/usr` 树哈希一致；luban-core 里 144 个形态 X
可执行文件的闭包在空根里完整；luban-desktop 里编译的 GL 程序用 llvmpipe 离屏渲染出期望的像素。
