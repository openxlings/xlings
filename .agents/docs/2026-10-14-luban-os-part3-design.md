# Luban OS part 3：分层与模式、一个环境一个默认运行时、无人值守与提权、用户与身份 —— 跨仓库生态级优化方案

日期：2026-10-14　基线：part 1（`2026-10-12-luban-os-architecture-design.md`）、桌面 part 2
（`2026-10-13-luban-os-desktop-part2-design.md`）、SubOS part 2（`2026-10-06-subos-architecture-design-part2.md`）
状态：第三轮 review（已并入：问题 6 不成立、glibc 自维护、宿主 runtime 的锁定入口、单 PR 加 commit 检查点、
首次创建的进度与镜像；本轮："一个环境只有一个默认运行时"的表述、无人值守与提权并入为统一模型 §8）。确认后按 §10 实施：每个仓库一个 PR，每个检查点一个 commit。

---

## 0. 起因：在一个用过一段时间的 home 上真实使用 Luban

2026.10.13.4 发布后，全新 home 上的矩阵全部通过；但在一个**用过一段时间的 home**（几百个 payload、
多个 SubOS、装过多个 glibc）上真实使用，暴露出八个问题。全新 home 的 CI 一个也覆盖不到。

| # | 现象 | 根因（已定位） |
|---|---|---|
| 1 | `self doctor --fix` 修复另一个 SubOS 时停在 `[sudo] password`，无法输入，Ctrl-C 无效 | 修复子进程经 `timeout` 放进了后台进程组；配方自己调用 `sudo`（绕过 `subos::elevation`）向读不到的终端要密码；父进程在 `system()` 里忽略 SIGINT |
| 2 | `luban new` 失败：`xz@5.8.3 is linked against glibc@2.44, which subos 'default' still uses` | payload 的身份不含 runtime 绑定；根闭包只允许一个 runtime；只能原地重链，而别的作用域在用（`src/core/xim/resolver.cpp:227`） |
| 3 | 根里 vim：`E1187: Failed to source defaults.vim` | 根的 `/usr/bin` 直接链接 payload 文件，只有带 alias 的程序保留 shim（`src/core/subos/root/root.cpp:160`）；配方登记的 `envs` 因此丢失。索引里 22 个配方有 `envs`（git、curl 的 CA，tmux、htop 的 terminfo，gcc、jdk……） |
| 4 | 根里 bash：`setlocale: LC_ALL: cannot change locale` | 宿主的 `LC_*` 只在 neutral 身份下被清除（`modules/confine/src/backends/linux_bwrap.cpp:101`） |
| 5 | desktop 版本：下载完整个镜像后才报同样的 runtime 冲突（多了 pnpm） | 同 2；冲突检查在下载之后 |
| 6 | 根里 `mcpp run` 之后出现一个 fish 提示符 `root@<宿主主机名>` | 已确认 `mcpp run` 本身正常，不作为问题；它暴露的"沙箱主机名是宿主的"归入问题 8 |
| 7 | tiny 里 `gcc: not found` | tiny 本来没有 gcc；缺"找不到命令时的下一步" |
| 8 | 机器模式提示符 `root@luban:~#`，看不到环境名；两种模式的起点、主机名、提示符各不相同 | 机器的主机名先读镜像里写死的 `/etc/hostname`（`luban`，`libs/luban.lua`）；沙箱的提示符靠环境变量 PS1 注入（`src/core/subos/sandbox.cpp:726`），子 shell 一换就丢 |

另外：进入系统时落在宿主路径 `/home/<user>`；系统里的 xlings 能看到并操作宿主 home（"穿透"）；
doctor 的一次修复（`xlings install <pkg>` 报"已是最新"）原地空转——都在下文归位。

这台 home 的实测：store 里有 glibc 2.39、2.44、2.44.2、2.44.3 四份——**装几份运行时本身没有问题**。问题在于同一个
环境（`default`）里的程序同时绑定在这四份上：一个环境里实际有四个"默认运行时"；gcc-runtime 只有一份，却被绑定
不同 glibc 的程序共同加载；而要让一个环境换运行时，只能改写被别人共享的 payload。

## 1. 已确认的方向（讨论记录）

| # | 决定 |
|---|---|
| D1 | **两类东西**：SubOS 视图 = 宿主上的叠加（像 venv）；**Luban 系统 = 独立的用户态 OS**（自己的 `/`、`/etc`、用户、包管理器）。同一个 Luban 系统内，视图之间共享版本 |
| D2 | **每一层只看到自己和自己的下一层**，看不到上一层与兄弟。嵌套时规则相同 |
| D3 | **Luban 系统用自己的前缀域**（系统域 `/xlings`），不与宿主的 SubOS 共享 payload；下载缓存全局共享 |
| D4 | **一个环境同一时刻只有一个默认运行时（T3）**：store 里可以有多份运行时；每个环境（作用域）每个家族选定一个默认运行时，C / C++ runtime 经一个间接点绑定到它；普通库仍按包绑定、永不原地改写；没有 ABI 契约的包显式固定绑定自己的运行时 |
| D5 | nano / tiny / core 默认以 root 进入，可配置登录用户、可单次 `--user`；**desktop 必须有普通用户**：交互或参数指定，隐藏输入、确认两次，默认无密码，不设公开默认密码；默认用户名 `lubanoser`（Luban OS + -er，社区称呼，同 oier、cpper） |
| D6 | 主机名 = 系统名；提示符用固定标识 `(name)` / `[luban]` / `<luban>`，带 16 色并兼容各种终端 |
| D7 | 欢迎语：无边框两列布局，≤ 72 列，ASCII 可退化，信息分层（提示符 / 欢迎语 / `luban status`） |
| D8 | **无人值守与提权是一个模型**（§8）：无人在场的子进程没有终端、不提问、能被中断；提权只走一扇门；配方只声明需要管理员的步骤，不自己执行 |

## 2. 原则

1. **内容与绑定分离。** payload 的内容只由（包、版本、构建）决定，安装后不可变；任何作用域的选择都不改写它。
2. **一个环境只有一个默认运行时。** 一个进程只能有一个 C 库；安全更新要一次覆盖环境里所有程序；所以默认运行时
   是环境的属性，不写进每个包。store 里有几份运行时都可以。
3. **一个问题一个回答者。** 登录用户由系统的 passwd 回答；主机名由系统的 `/etc/hostname` 回答；提示符由系统的
   `/etc/profile.d` 回答；runtime 由作用域的 abi 层回答。
4. **在下载之前说清楚。** 能在规划阶段知道的冲突、下限、缺失，一律在下载前报告，附命令。
5. **可观测。** 每个派生状态都有一份可读的清单和一条变更日志，能回答"现在是什么、为什么、谁改的"。

## 3. 分层与模式

### 3.1 两类东西

| | SubOS 视图 | Luban 系统 |
|---|---|---|
| 是什么 | 宿主上的一套工具版本，叠加在宿主之上 | 独立的用户态 OS |
| 底座 | 宿主的发行版 | 自己 |
| 里面的 xlings | 宿主的 xlings（U / C / P / S / M），当前 SubOS 是它 | 这个系统自己的 xlings（R），home 是它的 `/xlings` |
| 管理入口 | `xlings subos …` | `luban …` |
| 提示符标识 | `(name)` | `[luban]` 共享内核 / `<luban>` 自己的内核 |

底层仍是同一套 SubOS 引擎（Luban 系统是 `kind = rootfs` 的实例），区别在入口与可见性。

### 3.2 xlings 在各处的模式

| 你在哪里 | xlings 模式 | home | `xlings install` 装到 | `xlings subos ls` | `luban ls` |
|---|---|---|---|---|---|
| 宿主 | U（或 C / P / S / M） | `~/.xlings` | 宿主当前的 SubOS | 宿主的视图 | 宿主的 Luban 系统 |
| 宿主的视图 `(dev)` | 同上 | 同上 | dev | 同上 | 同上 |
| Luban 系统 box 里（沙箱、机器） | R | box 的 `/xlings` | box（新 generation） | box 里的视图 | box 里的系统 |
| box 里的视图 `[luban](dev)` | R | box 的 `/xlings` | box 里的 dev | 同上 | 同上 |
| 真机启动的 Luban OS | R | `/xlings` | 这台机器 | 本机视图 | 本机系统 |

**可见性规则（D2）**：

- 宿主的 `xlings subos ls` 默认只列视图，末尾一行指引：`3 Luban systems: luban ls`（`--all` 全列）。
  `xlings subos` 的命令仍可作用于 `kind = rootfs` 的实例——它是引擎层，脚本与旧用法不被破坏；文档与提示
  引导到 `luban`。
- 系统里的 xlings 的 home 是系统自己的 `/xlings`：它看不到宿主 home、宿主的视图与其他系统。
  现状是 `XLINGS_HOME` 指向宿主 home（`linux_bwrap.cpp:116`）—— 这就是"穿透"。
- 系统里的 `/xlings` 是**组合出来的视图**：共享 store（`data/xpkgs`，只读）+ 这个系统自己的状态
  （`.xlings.json`、`subos/default` = 它自己、它里面的视图）。别的系统的状态不在其中。在系统里安装时，
  payload 经前缀域的生产者（已有的 domain producer）写入共享 store，注册只写这个系统自己的状态。
- 嵌套：系统里还能有视图和系统；沙箱里再开沙箱取决于内核是否允许嵌套 user namespace，不行时 `luban new`
  说明原因并建议机器模式。

### 3.3 复用与独立

| 关系 | payload | 下载缓存 | 版本选择与 runtime | `/etc`、用户、`/home` |
|---|---|---|---|---|
| 宿主的视图之间 | 共享（用户域） | 共享 | 各自 | 宿主的 |
| 宿主视图 ↔ Luban 系统 | **不共享**（前缀域不同） | 共享 | 独立 | 独立 |
| Luban 系统之间 | 只读共享 `/xlings` 域的 store | 共享 | 独立 | 独立 |
| 同一系统里的视图之间 | 共享这个系统的 store | 共享 | 各自（runtime 是系统的） | 系统的 |

### 3.4 前缀域：Luban 系统用系统域

- xlings 已有前缀域的机制（`subos new --domain`、`src/core/subos/domain/`）：机器上没有 `/xlings` 时，系统域的
  物理位置是 `<home>/domains/xlings/`，只在命名空间里挂在 `/xlings`。`luban new` 目前没有用它。
- **`luban new` 一律在系统域建系统**。效果：
  - 与宿主 `default` 的 runtime 选择脱钩（问题 2、5 的一半）；
  - 系统里只看得到 `/xlings`，宿主用户名不再出现（`/home/<user>` 的来历见 §6.3）；
  - 导出的镜像天然可移植、可多用户。
- 代价：第一次在系统域安装要按 `/xlings` 前缀安装一份（下载缓存复用，不重复下载），多占一份磁盘。

## 4. 一个环境一个默认运行时（T3）

### 4.1 为什么不是"每个包各自绑定 runtime"，也不是"所有库都走间接点"

| 方案 | 根里冲突（2、5） | glibc 补丁版本 | 安全更新 | 进程内混用 | 磁盘 | 复杂度 |
|---|---|---|---|---|---|---|
| T1 现状：每个包写死 runtime 的绝对路径 | ✗ | ✗ 一个环境里同时有多个运行时 | ✗ 旧程序继续用旧 glibc | ✗ 隐患（一个进程一个 libc） | 每份运行时一份 | 冲突逻辑复杂 |
| T2 绑定计入身份（Nix 式变体） | ✓ | ✗ 每次复制受影响的包 | ✗ 复制全部 | ✗ | 成倍 | 组合爆炸 |
| **T3 一个环境一个默认运行时** | ✓ | ✓ 切换一次 | ✓ 一次覆盖 | ✓ 一个环境一个默认运行时 | 每份运行时一份 | **低** |
| T4 所有 soname 都走间接点 | ✓ | ✓ | ✓ | ✓ | 最少 | 中高：要信任所有库的 soname 约定、选提供者 |

每个包各自绑定，对"单独运行的可执行文件"是对的；出问题的只有 runtime 这一层：一个环境同一时刻应当只有一个
默认运行时，它是环境的属性。
普通库（openssl、zlib……）按包绑定没有观察到问题。所以收敛到 T3。

### 4.2 模型：两条规则加一个例外

**runtime 集合**（固定、很小）：C 库（glibc 的 loader、libc、libm、libpthread、libdl、librt、libresolv、
libnss_*……，或 musl）与 C++ 运行时（libgcc_s、libstdc++、libatomic）。由配方声明 `runtime = "glibc" | "musl"
| "gcc-runtime"`（xim-pkgindex），不由文件名猜测。

**默认运行时**：store 里可以同时有多份运行时 payload；每个环境（作用域）每个家族在同一时刻只选定一个作为
默认运行时。环境里的程序都用它，除非这个程序显式固定绑定了自己的运行时（下面的例外）。

**规则 1：runtime 经作用域的间接点绑定到默认运行时。**

```
<域>/abi/glibc-x86_64/ld-linux-x86-64.so.2 -> 某个 glibc payload 的 loader
<域>/abi/glibc-x86_64/libc.so.6 ...           同一个 glibc
<域>/abi/glibc-x86_64/libstdc++.so.6 ...      某个 gcc-runtime
<域>/abi/musl-x86_64/ld-musl-x86_64.so.1      某个 musl
```

- 安装时 elfpatch 把 `PT_INTERP` 写成 `<域>/abi/<家族>/<loader>`，RPATH 里去掉指向 runtime payload 的项。
  xlings 的 glibc 带"默认搜索目录跟随 loader 的调用路径"补丁（SubOS part 2 §2.3 实测），经间接点启动的进程
  在这个目录里找到 runtime 的库。glibc 自己的 gconv、locale、NSS 按它的重定位前缀找，那是不可变的 glibc
  payload 本身，不受影响。
- musl 的 loader 没有这个补丁（它读 `/etc/ld-musl-*.path` 或固定的默认目录），musl 家族的包在 RUNPATH 里写
  `<域>/abi/musl-x86_64`——仍然是与作用域无关的同一个路径。
- 间接点指向什么由作用域决定：

| 作用域 | runtime |
|---|---|
| 宿主 home（U / C / P / S，视图与沙箱视图） | 锁定的版本，否则每个家族已安装的最新版本（§4.6） |
| M（系统层 + 用户层） | 用户层取"用户层与系统层里最新的" |
| Luban 系统（沙箱、机器、真机） | 它的 generation：挂在 `/xlings/abi`，`/lib64/ld-linux…` 指向同一个；可锁定、可回滚 |
| 系统里的视图 | 系统的 runtime；视图只选工具版本与 sysroot |

**规则 2：普通库按包绑定，永不原地改写。** 包对 openssl 等的绑定在安装时确定，写在解析记录里；作用域使用它时
带上它记录的闭包，不让它重新绑定到作用域的选择。runtime 已不在普通库的绑定里，所以"重链"这条路径整体删除。

**例外：没有 ABI 契约的固定绑定。** 引用 `GLIBC_PRIVATE` 的，或配方声明 `abi = "exact"` 的，`PT_INTERP` 与
RPATH 用绝对路径指向那个具体的 runtime payload。这是包自身的属性，对所有使用方相同：不冲突，也没有变体。

**编译期与运行期分开。** 运行期由 abi 层决定；编译期（头文件、链接哪个 glibc 的库、`.pc`）由 SubOS 已有的
sysroot 模型决定。"视图想要旧 glibc"几乎都是编译目标问题：`xlings subos runtime` 在视图上改为选择 sysroot。
gcc 的 specs：系统域里 dynamic-linker 写标准路径 `/lib64/ld-linux-x86-64.so.2`（系统里编出的程序可拷到别的
系统运行）；用户域里写 home 的 abi 路径。

### 4.3 下限与不变式

- 安装时读出每个 ELF 需要的最高符号版本（`GLIBC_*`、`GLIBCXX_*`、`CXXABI_*`、`GCC_*`）与 loader 家族，写进解析记录：

```json
"abi": { "family": "glibc-x86_64", "floor": { "GLIBC": "2.38", "GLIBCXX": "3.4.32" }, "exact": false }
```

- xim-pkgindex 发布资源时由它自己的 CI 算出同样的摘要写进索引，规划在下载前检查；旧索引没有摘要时退回到
  "下载后、启用前"检查。
- **不变式**：作用域里每个启用的 payload，其家族的 runtime ≥ 它的下限。切换 abi 层之前检查，不成立就不切换。
- 求解按 §4.6 的规则：当前 runtime 满足就不动；不满足且未锁定时，规划里加入该家族在索引中的当前版本
  （新 generation 或 home 的 abi 层切换，写明原因）；锁定时拒绝，指出包与符号，以及解除锁定的命令。
- 配方里 `xim:glibc@2.39` 一类 runtime 依赖的含义变为"glibc 家族，至少 2.39"，不再意味着安装 2.39。

### 4.4 场景验证

| 场景 | 结果 |
|---|---|
| a ≥ 2.39、b ≥ 2.43 在同一作用域 | runtime 2.43，一份 glibc |
| 系统 A 用 2.39、B 用 2.43，都装 xz | 同一份 xz，各自的 abi 层 |
| 系统锁定 2.39，装 b（≥ 2.43） | 下载前拒绝：解除锁定（新 generation）、放到另一个系统、或确认 b 应固定绑定 |
| a 用了 2.39 的 `GLIBC_PRIVATE` | a 固定绑定 2.39；其余照常 |
| glibc 安全更新 | 切换 abi 层，一个 payload 都不改写；可回滚 |
| python 加载 C 扩展、node 加载原生模块 | 同一作用域只有一个 runtime，不会混用 |
| musl 与 glibc 程序 | 不同家族目录 |
| 静态程序、`$ORIGIN` 自带库的包 | 不经过间接点 |
| 降级到某包下限以下 | 拒绝，指出包与符号 |
| macOS / Windows | runtime 是操作系统（libSystem、UCRT），模型成立，无间接点 |

### 4.5 可观测

- `<作用域>/abi.json`（派生，可重建）：每个家族的 runtime 指向、版本、补丁状态。
- `logs/abi.ndjson`：每次切换的前后与原因（哪个包的哪个下限）。
- 规划与 `--dry-run` 显示 runtime 变化：`glibc-x86_64  2.44 -> 2.44.3  (b needs GLIBC_2.44.3)`。
- `self doctor` 检查不变式，给出结构化发现与修复命令；查看：`luban status box --abi`、`xlings info <pkg>`
  的 `-v` 输出（不新增顶层命令）。

### 4.6 runtime 什么时候变、谁能锁定

可选的做法：

| 做法 | 稳定 | 安全更新 | 简单 | 问题 |
|---|---|---|---|---|
| 永远跟随索引的最新版本 | 差：索引一更新，所有程序换 runtime | 好 | 好 | 没有人请求时也在变 |
| 永远取满足下限的最低版本 | 好 | 差：停在旧版本 | 中 | 环境停在旧版本，安全更新进不来 |
| 每个视图各自锁定 | — | — | 差 | PATH 叠加没有命名空间，做不到 |
| **跟随作用域里已安装的最新版本；只在需要或升级时前进；可在作用域上锁定** | 好 | 好：升级即覆盖 | 好 | — |

选最后一种，规则对所有作用域相同：

- **runtime = 锁定的版本，否则作用域里已安装的该家族最新版本。** 宿主 home 的"已安装"是 home 里的；Luban
  系统的是它 generation 里的。
- **只在两种时刻前进**：一个包的下限高于现有 runtime（规划里加入索引中的当前版本并写明原因）；用户升级
  （宿主上 `xlings update glibc` 或安装更新的版本，系统里 `luban upgrade box`）。索引更新本身不改变任何作用域。
- **锁定是配置**（能力先做成配置）：宿主 home `xlings config --runtime glibc@2.44.3`，`--runtime glibc@latest`
  解除；Luban 系统 `luban config box --runtime …`，在系统里 `xlings config --runtime …` 写的是同一处
  （系统的 `/xlings/.xlings.json`）——一个问题一个回答者。锁定低于某个已启用包的下限时拒绝，指出包与符号。
- **回滚**：宿主 home 的 abi 层保留前几次切换，`xlings config --runtime glibc@<上一个版本>` 即回到它（同样
  检查下限）；Luban 系统用 generation 回滚。
- 视图上的 `xlings subos runtime` 改为选择 sysroot（编译目标），帮助与输出说明这一点，并指向
  `xlings config --runtime`。

## 5. 根里程序的运行环境

- **登记了 `envs` 的程序在根里保留 shim**，与带 alias 的程序同样处理（问题 3）：`root.cpp` 的判断从
  `!data.alias.empty()` 扩展到 `!data.alias.empty() || !data.envs.empty()`。shim 在进程内注入环境变量，不经 `/bin/sh`。
- **语言环境**：根模式一律 `LANG=C.UTF-8`，清除宿主 `LC_*`，与机器模式、neutral 身份一致（问题 4）。
- **找不到命令**：根里的 shell 配置提供 `command_not_found_handle`，在索引里有对应程序时提示
  `xlings install <pkg>`，或说明哪个 edition 自带（问题 7，可选）。

## 6. 用户、登录与起点

### 6.1 由 edition 声明，由 passwd 回答

| edition | 默认登录 | 普通用户 |
|---|---|---|
| nano | 不登录（`luban run`） | — |
| tiny | root | 可选：`luban new box --user <名>` |
| core | root | 可选，同上 |
| desktop | **普通用户（必须）** | 交互或 `--user`；默认 `lubanoser` |

- edition 的 `usr/share/luban/settings.json` 增加 `user`（`none` / `root` / `required`）。
- 创建：`luban new d --edition desktop`

```
▸ desktop needs a user to log in as
  user name [lubanoser]:
  password (empty: none, log in without one):
  again:
```

  经 Asker 端口提问；密码关闭回显、确认两次，写成 `/etc/shadow` 的 SHA-512-crypt（`$6$`）哈希，明文不落盘、
  不进日志。哈希在进程内计算（算法有公开规范，放在 `modules/platform` 之外的纯 C++ 模块），不调用外部程序；
  yescrypt 以后随 libxcrypt 进入索引再换。参数：`--user <名>`、`--password-file <文件>`、`--no-password`；`-y` 取默认（`lubanoser`、无密码）；
  agent 模式不提问，缺什么报对应参数。
- 默认无密码：本地自动登录（控制台与窗口只有宿主上的这个用户能经 luban 打开）；远程登录在设密码前关闭；
  用户在 `wheel`，无密码时 `sudo` 免密（edition 带 `sudo` 或 `doas`），设密码后需要密码。
- 进入以谁：`luban config box --login <名>` 写入系统（机器状态）；单次 `luban enter box --user root`；
  系统里 `su`、`login` 照常。机器模式与沙箱读同一份 passwd。
- 名字的官方解释：`lubanoser` = Luban OS + -er，"Luban OS 的使用者"。

### 6.2 主机名

- 主机名 = 系统名，沙箱与机器一致：沙箱进入时开 UTS 命名空间；创建时 luban 把名字写进 `/etc/hostname`。
- 已有系统：`/etc/hostname` 等于 edition 的出厂值 `luban` 时视为未设置，用系统名；用户改过的不动。
- neutral 身份例外：主机名是 persona 的随机值，提示符的标识照常。
- 改名：`luban config box --hostname <名>` 或直接编辑文件；升级不改写。

### 6.3 起点与宿主共享

- 现状：沙箱以宿主的当前目录为起点（`linux_bwrap.cpp:119`）；宿主 home 只读挂在原路径，于是系统里出现
  `/home/<user>`（只是挂载点的父目录），写在那里的文件进入系统自己的 `/home`。
- 改为：起点是登录用户的 home（root 为 `/root`），两种模式一致；宿主目录默认不共享，显式共享
  `luban config box --share <dir>` 挂在宿主同一路径；在已共享的目录里 `luban enter` 时从那里开始。
- 系统域之后（§3.4）宿主 home 不再挂入系统，`/home/<user>` 自然消失。

## 7. 身份标识

### 7.1 三层披露

| 层 | 内容 | 时机 |
|---|---|---|
| 提示符 | 在什么里面、保留策略（非默认时）、用户、主机名、目录 | 一直 |
| 欢迎语 | 版本、edition、名字、内核关系、保留策略、用户、xlings 角色、时间、网络（非默认时） | 进入时一次 |
| `luban status` | 全部，含 runtime、前缀域、generation | 需要时 |

### 7.2 提示符

```
(dev) user@host:~/proj$          ( )  宿主上的 SubOS 视图；名字在括号里（主机名是宿主的）
[luban] root@box:~#              [ ]  Luban 系统，共享宿主内核（沙箱）
<luban> root@box:~#              < >  Luban 系统，自己的内核（机器）
[luban:temp] root@box:~#         :temp / :home  保留策略非"全部"时
<luban:home> lubanoser@box:~$
[luban](dev) root@box:~#         系统里的视图
```

- 名字只出现一次：系统的名字在主机名里，视图的名字在括号里。括号种类 = 与宿主的关系；`luban` = xlings 在此是 R。
- 由系统的 `/etc/profile.d/luban.sh` 生成，读 luban 进入时设置的 `LUBAN_KIND`、`LUBAN_PERSIST`；fish、zsh 的片段
  随 edition 提供。不再用环境变量 PS1 注入。已有系统：作为出厂文件在缺失时补上，不覆盖用户的修改。
- 宿主上的视图：宿主的 shell 与它的配置是用户自己的，xlings 不改写。`xlings subos use` 设置 `XLINGS_SUBOS`，
  `(name)` 由 shell 集成片段显示（bash / zsh / fish 由 `xlings self init` 的集成提供，PowerShell 随后）；
  没有集成时欢迎语照常说明。
- 颜色：只用 ANSI 16 色（SGR 30–37、90–97）。`(name)` 黄、`[luban]` 青、`<luban>` 品红、`:temp` 红、`:home` 黄、
  root 的 `#` 红，其余默认色。上色白名单：`TERM` 为 `xterm*`、`screen*`、`tmux*`、`linux`、`rxvt*`、`vt220`、
  `*color*`、`alacritty`、`foot`、`kitty`、`wezterm`；`NO_COLOR` 或 `LUBAN_COLOR=0` 关闭；其余（`dumb`、`vt100`、
  未设置）不上色。bash 与 busybox ash 用 `\[ \]` 包住颜色码，zsh 用 `%{ %}`，fish 用 `set_color`。
  不上色时括号与后缀已表达全部含义。

### 7.3 欢迎语

规则：不画边框（CJK 与歧义宽度字符会让右边框错位）；两列"标签 值"，标签宽 9；每行 ≤ 72 列，过长的值中间截断；
只用 ASCII 与 `·`、`─`，非 UTF-8 语言环境退化为 `-`；颜色只用于标题与标签，规则同提示符；只在交互进入时显示；
由宿主侧的同一个渲染器输出（终端里的 `luban enter` 与 Luban OS 窗口的控制台一致），内核版本、时间由系统里的
agent 回报。

```
 Luban OS 2026.10.14.1 · [luban] box
 ──────────────────────────────────────────────────────────
 system    core 2026.10.11.1 · generation 3 · 412 packages
 runs as   sandbox: shares this host's kernel (Linux 6.8.0)
 keeps     everything
 user      root · /root
 xlings    manages this system only (R)
 time      2026-10-14 12:05 UTC
 ──────────────────────────────────────────────────────────
 exit to leave · luban status box
```

```
 Luban OS 2026.10.14.1 · <luban:temp> box
 ──────────────────────────────────────────────────────────
 system    tiny 2026.10.11.1 · generation 1 · 38 packages
 runs as   machine: its own kernel (Linux 6.12.4, KVM · 2 CPU · 2 GiB)
 keeps     nothing: what you write is gone when it stops
 user      lubanoser · /home/lubanoser
 xlings    manages this system only (R)
 time      2026-10-14 12:05 UTC (the system's zone) · up 4 s
 network   through the host's proxy
 ──────────────────────────────────────────────────────────
 exit to leave · Ctrl-] d leaves it running · Ctrl-] p powers it off
```

宿主上的视图（`xlings subos use dev`）两行：

```
 (dev) a view on this host: its tools first on PATH, the host's system stays
       xlings manages dev · exit to leave
```

`luban ls`、`luban status` 用与欢迎语相同的词（sandbox / machine、keeps everything / only /home / nothing）。

## 8. 无人值守与提权：一个模型（问题 1）

### 8.1 现状：同一类问题出现在不止一处

| 位置 | 子进程怎样运行 | 遇到配方里的 `sudo` |
|---|---|---|
| `self doctor --fix` 的修复子进程（`xself::run_unattended`） | `system()` + `timeout`（后台进程组），父进程忽略 SIGINT | 向读不到的终端要密码，最长挂 30 分钟，Ctrl-C 无效（问题 1） |
| luban 服务取 JSON 的子进程（`platform::stream::run`，10 处调用；`luban/service/src/port.cpp`） | 自己的进程组、输入 `/dev/null`、输出被截获，**仍有控制终端** | 打开 `/dev/tty` 读密码，后台组读终端被内核停住，无期限挂起；提示被截获，没人看得到 |
| Luban OS 窗口的后台任务 | 经 luban 服务 | 同上 |
| 配方自己 | 索引里 11 个配方出现 `sudo`（如 `code.lua` 的 chrome-sandbox；其中有的只是注释，A5 逐个确认） | 绕过 `subos::elevation`：不记录、不判断有没有人能回答 |

根因是同一个：**"有人在场"与"无人在场"的子进程没有分开**，提权也不只一扇门。

### 8.2 模型

**两类子进程，各一个原语（`modules/platform`）：**

| | 有人在场 | 无人在场 |
|---|---|---|
| 例子 | `luban enter`、`xlings subos exec`、交互安装 | doctor 的修复、截获输出的 JSON 调用、服务与窗口的后台任务、前缀域生产者 |
| 终端 | 把终端交给子进程组（已有：`run_argv_with_timeout`） | **新会话，没有控制终端**，输入 `/dev/null` |
| 提问 | 经 Asker 问在场的人 | 从不提问：`XLINGS_AGENT_MODE=1`，需要回答时退出 2 并给出命令 |
| 中断 | Ctrl-C 送到子进程 | 调用者收到 Ctrl-C / 终止时，先结束子进程的整个会话，再照常处理；调用者意外退出时子进程随之结束（Linux：`PR_SET_PDEATHSIG`） |
| 期限 | 可选 | 可选，进程内计时（不再依赖外部 `timeout`） |
| 输出 | 直接到终端 | 截获或写入日志 `logs/<来源>/<名字>.log`，失败时提示指向它（替代现在的"run it directly to see why"） |

`run_shell_unattended`（已实现）与 `stream::run` 都归入"无人在场"：后者加上新会话即可，调用方不变。

**提权只有一扇门：`subos::elevation`。** 它记录到 `logs/elevation.ndjson`；能问时问（有人在场），不能问时只用
不需要提问的权限（已是 root、sudo 的凭据仍有效），否则不执行并返回 77，调用者说出要手工执行的命令。

**配方不提权，只声明。** libxpkg（独立仓库）给配方一个只声明、不执行的接口：

```lua
-- 在 install() 或 config() 里
system.admin_step({"chown", "root:root", sandbox}, "Chromium's sandbox helper must be root's")
system.admin_step({"chmod", "4755", sandbox}, "and setuid")
```

- xlings 收集这些步骤，安装结束后经 `subos::elevation` **一次性**执行：有人在场时列出全部步骤与理由，问一次；
  无人在场时不执行，记入 `<home>/state/admin-steps.json`，安装照常完成，输出一行"还需要管理员执行的步骤"与
  命令（`xlings self doctor --fix` 在有人在场时补做）。
- 包的功能因此缺少的部分由配方说明（例如 VS Code 在步骤补做之前用 `--no-sandbox` 启动的提示）。
- xim-pkgindex 的静态检查禁止配方里出现 `sudo`（`-- elevation-ok: <why>` 例外），直接调用 `sudo` 的配方改用 `admin_step`。

**doctor 的修复梯级不空转。** 修复子进程报告"已是最新"而发现项仍在时，直接进入下一梯级（R3 重装），
修复后重新检查发现项，仍在则报告并给出原因与日志位置；跨 SubOS 修复的子进程同样只做一轮。

### 8.3 已完成与待做

- **已完成（本地，A1）**：`platform::run_shell_unattended` 与 `xself::run_unattended` 改用它；测试：伪终端里子进程
  打不开 `/dev/tty`、输入不是终端；SIGINT 在 5 秒内结束整链；去掉 `setsid` 两个测试都失败（已验证）。
- **待做**：`stream::run` 改为无人在场原语（A1b）；进程内期限取代外部 `timeout`、`PR_SET_PDEATHSIG`、子进程日志
  （A1c）；libxpkg 的 `admin_step` 与 xlings 的收集执行（A5）；xim-pkgindex 的改写与静态检查（A5）；修复梯级（A6）。

## 9. 迁移与无感升级

| 已有的东西 | 处理 |
|---|---|
| 用户域里的 Luban 系统（如 luban-tiny-3） | 继续可用；`luban status` 标注"旧布局"；`luban upgrade box` 的规划里包含迁移到系统域（安装到系统域、新 generation，机器状态 `/etc`、`/home`、`/root` 保留） |
| 绝对路径绑定 runtime 的 payload | 照常运行（路径仍在）；home 格式步骤建出 `<home>/abi`；`self doctor --fix` 把它们改到间接点（对所有使用方相同，共享也安全；改前检查所有使用者的下限），幂等、有记录 |
| 视图上的 `runtime` 绑定 | 迁移为 sysroot 选择，告知一次 |
| 系统的 `/etc/hostname = luban` | 视为未设置 |
| 系统缺 `/etc/profile.d/luban.sh` | 作为出厂文件补上 |
| 旧客户端 | 新 payload 只是指向一个普通路径，旧客户端照样运行；home 的 `layout` 整数不升（旧客户端不会撤销这些改动） |

## 10. 实施：阶段、依赖与仓库

```
A 独立修复 ───────────────────────────────┐
B 分层与前缀域 ──► C 默认运行时 ──────────┼──► E 测试矩阵与 CI ──► 发布
           └────► D 用户与身份 ─────────────┘
```

| 阶段 | 内容 | 仓库 |
|---|---|---|
| A1 | doctor 修复子进程无终端、可中断（已完成） | xlings |
| A1b | `stream::run` 归入无人在场：新会话 | xlings |
| A1c | 无人在场原语：进程内期限、`PR_SET_PDEATHSIG`、子进程日志与失败提示 | xlings |
| A2 | 根里登记 `envs` 的程序保留 shim | xlings |
| A3 | 根模式 `C.UTF-8`、清除宿主 `LC_*` | xlings |
| A4 | 规划阶段的冲突与下限检查提到下载之前 | xlings |
| A5 | 配方声明管理员步骤：libxpkg 的 `admin_step`；xlings 收集并经 elevation 一次执行、无人时记入待办；直接调用 `sudo` 的配方改写与静态检查 | libxpkg / xlings / xim-pkgindex |
| A6 | doctor 修复梯级在"已是最新"时进入下一梯级，并验证发现项消失 | xlings |
| B1 | `luban new` 在系统域建系统；下载缓存全局共享；首次创建的进度与镜像（§10.2） | xlings |
| B2 | 可见性：宿主 `subos ls` 不列系统；系统里 xlings 的 home 是 `/xlings` | xlings |
| B3 | 旧布局系统经 `luban upgrade` 迁移 | xlings |
| C1 | abi 层：间接点目录、`abi.json`、`abi.ndjson`、原子切换与回滚 | xlings |
| C2 | elfpatch 写间接点、读下限、识别 `GLIBC_PRIVATE` | xlings |
| C3 | 求解：runtime 依赖为下限；删除重链与 `resolver.cpp:227` 的冲突；固定绑定 | xlings |
| C4 | generation 的 runtime 链接与不变式（取代根闭包检查里关于 runtime 的规则） | xlings |
| C5 | 迁移：home 格式步骤、`doctor --fix` 改绑 | xlings |
| C7 | runtime 的锁定与回滚：`xlings config --runtime`、`luban config --runtime`；`subos runtime` 改为 sysroot | xlings |
| C6 | 配方声明 `runtime`、`abi = "exact"`；glibc 补丁检查；发布时算 ABI 摘要写入索引；gcc specs | xim-pkgindex |
| D1 | 主机名 = 系统名（UTS 命名空间、机器优先级） | xlings |
| D2 | 用户：settings `user`、sysusers、`--user` / `--password-file` / `--no-password`、Asker、shadow 哈希、`--login`、`enter --user` | xlings |
| D3 | edition：`profile.d/luban.sh`（提示符、颜色）、fish / zsh 片段、`wheel` 与 `sudo` / `doas`、desktop 的 `user = required` | xim-pkgindex |
| D4 | 起点与 `--share` | xlings |
| D5 | 欢迎语渲染器（终端与窗口共用）、`luban ls` / `status` 用词 | xlings（luban、apps/luban-os） |
| D6 | 桌面会话以普通用户运行 | xlings（xdesk） |
| E | 测试与 CI（§11） | xlings / xim-pkgindex |

### 10.1 PR 与检查点

- **每个仓库一个 PR**：xlings、xim-pkgindex、libxpkg（只有 `admin_step`，很小）各一个。**每个检查点是 PR 上的一个 commit**（A1、A2……E），
  commit 标题以检查点编号开头，正文写它满足的需求 ID 与测试；PR 描述里有检查点清单与各自的 commit。
  PR 以 squash 合入，main 上一个 commit；检查点在 PR 的 commit 历史里可追溯。
- 每个 commit 都能独立构建、通过它涉及的测试；CI 在 PR 上对最新 commit 全量运行。
- 跨仓库的顺序：libxpkg 先合入并打 tag，xlings 的 `mcpp.toml` 指向它；xim-pkgindex 的 A5、C6、D3 以"新客户端
  可用、旧客户端无害"的方式合入（新字段旧客户端忽略；用 `admin_step` 的配方在旧客户端上退化为提示），
  xlings 的 PR 用 `XIM_PKGINDEX_REF` 指向它联调；xlings 发布后再合入依赖新客户端的 edition 修订。
- 版本：xlings 发布一个 `2026.10.14.N`（三处版本号同步）；xim-pkgindex 的 edition 与配方按 revision 规则递增。
- 命令树的每个新增（`--user`、`--password-file`、`--no-password`、`--login`、`--hostname`、`--share`、
  `--runtime`、`status --abi`）同步更新 `src/cli/spec.cpp` / `luban.cli`、`docs/generated/` 与测试。

### 10.2 首次创建的进度与镜像

- 系统域的首次安装经前缀域生产者完成。进度按阶段输出结构化事件（解析 → 下载 → 解包 → 绑定 → generation），
  每个阶段给出计数与字节数；默认输出一行刷新的进度与当前阶段，`-v` 逐包，agent 模式为 NDJSON；Luban OS 窗口
  显示同一组事件。完成后一行总结：用时、下载量（其中多少来自缓存）。
- 镜像：生产者已从所属 home 继承 `mirror`、`xim`、`index_repos`、`resource_servers`
  （`src/core/subos/domain/domain_producer.cpp:135`）。补上：下载缓存与镜像无关地按 sha256 命中；系统里的
  xlings 读系统自己的 `.xlings.json`，创建时从宿主继承镜像设置，之后 `xlings config --mirror` 在系统里单独修改。

## 11. 测试矩阵

| 需求 ID（新增） | 测试 |
|---|---|
| ELEVATION-NOBODY-TO-ASK（扩展） | 伪终端无 `/dev/tty`；Ctrl-C 结束整链（已有）；`stream::run` 的子进程无 `/dev/tty`；调用者被杀时子进程随之结束 |
| RECIPE-ADMIN-STEP | 夹具配方声明两个步骤：有人在场时问一次并执行、记录到 `elevation.ndjson`；无人在场时安装成功、步骤记入待办、输出命令；`doctor --fix` 补做 |
| DOCTOR-LADDER | 修复子进程"已是最新"而发现项仍在：进入下一梯级；修复后复查；失败时指向子进程日志 |
| ROOT-PROGRAM-ENV | 根里运行登记了 `envs` 的程序，变量生效（vim 的 `$VIM`、curl 的 CA） |
| ROOT-LOCALE | 宿主 `LC_ALL=en_US.UTF-8` 进入根，bash 无警告 |
| LAYER-VISIBILITY | 宿主 `subos ls` 不列系统；系统里 `subos ls` 不见宿主视图；`xlings install` 只改系统 |
| DOMAIN-LUBAN-SYSTEM | 系统里没有 `/home/<宿主用户>`；导出的归档不含宿主用户名 |
| RUNTIME-SCOPE | §4.4 每一行一个用例（夹具 glibc 两个版本、夹具包带不同下限、`GLIBC_PRIVATE` 夹具、musl 夹具） |
| RUNTIME-SWITCH-ATOMIC | 切换中途失败，旧 runtime 仍完整；回滚恢复 |
| RUNTIME-MIGRATE | 旧绝对绑定的 payload 经 `doctor --fix` 改绑，所有使用者仍可运行 |
| ID-HOSTNAME / ID-PROMPT | 沙箱与机器里 `hostname` = 系统名；`sh -lc` / `bash -l` / su 后提示符一致；`TERM=dumb`、`NO_COLOR` 无颜色码 |
| ID-WELCOME | 欢迎语快照（宽 72、ASCII 退化、无颜色）；`luban run` 不输出欢迎语 |
| RUNTIME-PIN | 锁定后不前进；锁定低于下限被拒绝；`@latest` 解除；回滚到上一个版本 |
| DOMAIN-PROGRESS-MIRROR | `luban new` 在 CN 镜像设置下从 CN 资源下载；第二个系统的下载全部命中缓存；agent 模式的进度事件快照 |
| USER-DESKTOP / USER-LOGIN | desktop 无 `--user` 时交互提问、`-y` 得 `lubanoser`、agent 模式报参数；密码不回显、只有哈希落盘；`--login` 与 `enter --user` |

CI：

- **luban-fresh 增加"用过一段时间的 home"一腿**：先用上一个发布在 home 里装若干绑定旧 glibc 的包，再
  `self update`，然后 `luban new` 各 edition、进入、运行 vim / `git clone https://` / tmux，检查提示符与主机名。
- **非 root 一腿**：desktop 的默认用户在桌面会话里运行浏览器类应用（Electron 夹具）。
- unit-asan 只在 main 与 `ci:asan` 标签上运行：C 阶段的 PR 打上该标签。

## 12. 风险与待定

1. **loader 补丁**：glibc 由 xim-pkgindex 自己打包维护，"默认搜索目录跟随 loader"的补丁随它走；索引 CI 对
   runtime 类配方检查补丁仍在（C6），以防升级 glibc 时丢失。
2. **`PT_INTERP` 变长**：间接点路径可能比原解释器段长，elfpatch 需要扩展段（patchelf 能做）；静态检查长度上限。
3. **宿主 home 的 runtime 前进会影响宿主上所有视图**：按 §4.6 只在需要或升级时前进，写进规划与日志，可锁定、
   可回滚。
4. **一个 PR 的体量**：xlings 的 PR 会很大。检查点逐个 commit、逐个可构建可测试，review 可以按 commit 进行；
   B 与 C 在同一个 PR 里，系统域的 payload 一开始就用新绑定，不产生需要迁移的系统域 payload。
5. **首次建系统更慢**：系统域要按 `/xlings` 前缀安装一份；下载按 sha256 命中缓存，增加的是解包与绑定的时间。
   用进度与总结让等待可见（§10.2），在性能车道里给 `luban new core` 定预算并测量。

## 13. 自我 review（多视角）

| 视角 | 检查 | 结论与本文的调整 |
|---|---|---|
| 架构 | 每个决定只有一个回答者吗 | runtime → abi 层；登录 → passwd；主机名 → `/etc/hostname`；提示符 → profile.d。系统里的 `/xlings` 是组合视图，补写了它的构成与写入路径（§3.2） |
| 简洁 | 有没有为罕见情况引入通用机制 | 从 T4 收敛到 T3：只有 runtime 走间接点；没有变体；固定绑定是包的属性 |
| 稳定性 | 切换能不能半途失败 | abi 层与 generation 同样一次 rename 切换、保留回滚；切换前检查不变式 |
| 兼容性 | 现有脚本与 home 会不会坏 | 初稿让 `xlings subos` 拒绝操作系统实例——会破坏旧用法，改为默认不列出、仍可操作（§3.2）；旧绑定照常运行；`layout` 不升 |
| 无感升级 | 用户需要做什么 | 什么都不用做：旧系统可用并被标注；迁移在 `luban upgrade` 的规划里；改绑由 doctor 完成 |
| 跨平台 | macOS、Windows | Luban 系统只在 Linux；T3 在 macOS / Windows 上 runtime 就是操作系统，无间接点；宿主视图的提示符靠 shell 集成，PowerShell 后续 |
| 安全 | 默认无密码、免密 sudo | 只在本地自动登录；远程登录在设密码前关闭；沙箱里的 root 不是宿主 root；哈希进程内计算，不经外部程序；配方不提权 |
| 隐私 | 宿主信息是否进入系统与镜像 | 系统域后宿主用户名不再出现；导出测试检查归档不含宿主用户名；默认用户名不含个人信息 |
| 体验 | 用户随时知道自己在哪吗 | 提示符的括号种类 + 主机名 + 后缀；欢迎语一次讲全；`luban status` 兜底；三处同一套词 |
| 可观测 | 出问题能不能分析 | `abi.json`、`abi.ndjson`、规划里的 runtime 变化、doctor 的不变式发现、`elevation.ndjson` |
| 一致性 | 两种模式是否同一套行为 | 主机名、起点、语言环境、提示符、欢迎语、passwd 在沙箱与机器里一致 |
| 生态 | 跨仓库的契约是否明确 | xim-pkgindex：`runtime` / `abi = "exact"` 声明、ABI 摘要、glibc 补丁检查、配方不提权、edition 的 profile.d 与用户（问题 6 已确认不涉及 mcpp） |
| 测试 | 这次的问题能否被 CI 抓住 | 新增"用过一段时间的 home"与"非 root"两腿；§4.4 每个场景一个用例；C 阶段 PR 打 `ci:asan` |
| musl | 间接点对 musl 成立吗 | musl 的 loader 不跟随调用路径，改用 RUNPATH 指向间接点（§4.2） |

第二轮 review：

| 视角 | 检查 | 结论与调整 |
|---|---|---|
| 一致性 | §4.2 写"取已安装的最新"，§4.3 初稿写"升到满足的最低版本"，两处矛盾 | 统一为 §4.6 的一条规则：锁定的版本，否则已安装的最新；需要时补入索引的当前版本 |
| 简洁 | 锁定入口要不要新命令 | 不要：做成配置 `--runtime`，宿主 `xlings config`、系统 `luban config`，系统里 `xlings config` 写同一处 |
| 稳定性 | 索引更新会不会悄悄改变 runtime | 不会：只在包需要或用户升级时前进 |
| 安全 | 固守旧 runtime 的风险 | 升级即前进到索引的当前版本；锁定是显式的、可见的（`luban status`、`abi.json`） |
| 可追溯 | 单 PR 如何 review | 每个检查点一个 commit，标题带编号、正文带需求 ID；PR 描述列出清单 |
| 跨仓库 | 两个 PR 谁先 | xim-pkgindex 的新字段对旧客户端无害，先合入；xlings 用 `XIM_PKGINDEX_REF` 联调 |
| 体验 | 首次创建的等待 | 分阶段进度、缓存命中量、结束总结；镜像沿用 home 的设置 |
| 兼容 | `xlings subos runtime` 改语义 | 输出说明它现在选择编译目标，并指向 `xlings config --runtime`；原有绑定由迁移步骤转换并告知一次 |

第三轮 review：

| 视角 | 检查 | 结论与调整 |
|---|---|---|
| 准确性 | "多份 glibc 是问题"的表述 | 改正：store 里多份没有问题；问题是一个环境里同时有多个默认运行时（§0、§2、§4） |
| 架构 | 问题 1 是个例还是一类 | 一类：`stream::run` 同样会挂；统一为"有人在场 / 无人在场"两个原语（§8.2） |
| 安全 | 配方提权 | 只声明、不执行；一扇门、一次询问、有记录；无人在场时绝不提权 |
| 体验 | 无人在场时缺管理员步骤 | 安装照常完成，一行说明还差什么与命令；doctor 在有人时补做 |
| 可观测 | 修复失败只说"run it directly" | 子进程输出写入日志，失败提示指向它 |
| 稳定性 | 外部 `timeout` 把子进程放进别的进程组 | 进程内计时；调用者退出时子进程随之结束 |
| 兼容 | 旧客户端遇到用 `admin_step` 的配方 | libxpkg 的旧版本没有这个函数：配方用 `if system.admin_step then … else` 退化为提示，不调用 `sudo` |
| 跨仓库 | 多了 libxpkg | 改动小、先合入、打 tag；xlings 固定到该版本 |
