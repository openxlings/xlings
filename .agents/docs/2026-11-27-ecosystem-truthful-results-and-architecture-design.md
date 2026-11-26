# 生态收敛：结果的真实性、CPU 架构、版本、路径与 hook 通道 —— 跨仓库设计与实施计划

- 日期：2026-11-27（同日第二版：写入七项决定及理由，并按多视角自审修订，§17；第三版：索引迁移改为由工具判断，
  写入对五项风险的评审结论，§18；第四版：两处遗留问题实测后出方案，§2.8、§8.3、§9.6；
  第五版：§9.6 重写为 hook 接口的重新设计，§2.9 是它的证据，落地顺序随之改变，§12；
  第六版：落地模型、命令的列表形式、索引的模板声明、xpkg 规范化，§9.7 到 §9.9，全方案自审，§17；
  第七版：写入对第六版五项的评审结论，§15 第 19 到 22 项；第八版：实施的拆分、依赖与联调，§19）。
  状态：**各项决定已评审（§15），实施中（§19）**。
- 范围：xlings（解析、安装、doctor、规范）、libxpkg（架构身份表、hook 结果、elfpatch、hook 的接口与环境）、
  xim-pkgindex（架构声明迁移、mesa 的 aarch64 闭包、索引检查、规范文字、recipe 迁到 `spec = "3"`）、
  三个子索引 scode、awesome、d2x（模板声明，不挡发布）、mcpp-index（libxpkg 的版本）、
  mcpp（构建侧的声明式退出）。资产发布到 xlings-res。
- 来源：xlings 的 15 条开放 issue（#740、#739、#736、#721、#709、#708、#607、#594、#593、#592、#566、#540、
  #532、#425、#417）与上游登记帖 #741；两个开放的 PR（#738 作者标签、#710 macOS runner），内容并入本方案后关闭（§10.2）；
  xim-pkgindex 的 #1442（mesa aarch64）、#937、#451、#1341、#1152、#569。
- 验证：已发布的 2026.11.23.1 上的实测（§2.1）、索引 `main`（47591c3e）的普查（§2.2、§2.3）、
  xlings `main`（7f9f2fb1）与 libxpkg `main`（1485db9，0.0.67）的代码阅读（§2.4）。未实测的逐项列在 §2.5。
- 每个仓库一个 PR，PR 内以 commit 为检查点，每个检查点都能构建并通过测试；不拆 issue，修复后逐条关闭。
- libxpkg 以版本号发布；`git rev` 只在 PR 上用于联调（§9.5）。
- 临时的测试与验证可以用 draft PR 的 CI（aarch64 与 macOS 的机器、长时间的构建）；哪一项在哪里验证列在 §11。
- **自审改动了两处已定的做法**（§15 的第 2 项与第 8 项）：与架构无关的包不再新增 `noarch` 字段；
  hook 的提示不再新增 recipe 接口。两处都是找到了不需要新概念的做法，前一处已实测；评审时未提出异议，本文按改后的写法。

## 0. 核心点

1. **五个根问题，一条主线。** 15 条 issue 归结为：检查失败后 warn 继续（结果不真实）、架构声明不被执行、
   版本字符串含义不唯一、路径没有统一的归属规则、hook 与宿主之间只有文本通道。主线是第一条：
   **一步只有"做了、跳过并说明、拒绝、失败"四种结果，命令的结论由它们算出，warn 不改变结论**（§4 C1）。
2. **架构：字节说了算。** `package.archs` 是跨平台的并集，判断不了某个平台的条目：`bat` 声明两种架构，
   Linux 条目只有一个 x86_64 产物，在 aarch64 上连 warn 都没有（§2.2）。所以裁决放在两处：
   计划阶段看**条目**枚举的架构，安装阶段看**载荷**的 `e_machine`（规则 M，硬拒绝）（§5）。
3. **不需要新的 recipe 字段。** 固定 URL 加按架构的 `sha256` 表，已发布的客户端就能安装、校验、
   并在宿主不在表里时拒绝（实测 E2、E6、E7）。只有 x86_64 的包列一个键；与架构无关的包把同一个产物
   列给每种架构。旧客户端完全读得懂（§5.3）。
4. **索引今天就能让 aarch64 从"装错"变成"被拒绝"，迁移由工具完成。** 89 个单产物、只声明 x86_64 的 recipe
   机械改写；另外约 70 个"说不清是哪种架构"的，由工具读字节、在两种架构上各装一次来判断，不靠人逐个看。
   抽样里 12 个本机工具有 10 个上游就有 aarch64 的产物（§2.7、§5.6）。
5. **mesa 的 aarch64 闭包是 22 个节点里缺 18 个**，不是 #1442 请求的 3 个，也不是 #740 写的 12 个（§2.3）。
   图形栈的构建并入索引已有的 `res-build`，在 aarch64 的 runner 上进行；同一版本键 25.0.7.3 变成按架构的条目，
   x86_64 的字节不变（§5.8）。
6. **`glibc@2.44` 的根因是两处代码对同一个串的读法不同。** 索引侧按精确键，安装侧按前缀且先答。
   规则：一个串是索引里的具体键时永远精确；声明里写解析后的版本；已有的声明由一个 home 格式步骤带到新写法（§7）。
7. **`subos runtime` 说"未改动"之后照写声明**，是因为激活失败只降级为 warn、写声明在它之后无条件执行。
   这是主线的直接应用："拒绝"的定义就是什么都没改（§6.2）。
8. **`log.warn` 就是给用户看的。** 不新增接口：libxpkg 把 hook 里 warn 与 error 级别的行记为结构化的"说明"
   随结果交回，xlings 在总结之后显示。索引里现有的 122 处 `log.warn` 不改一行就能到达用户（§9.2）。
9. **libxpkg 已经知道架构不符，却把它计入"skipped"并丢掉。** `elfpatch` 对"为另一种机器构建"的文件
   一律跳过，`apply_elfpatch_auto` 只回传三个数。在 aarch64 上装 x86_64 的 mesa 就是全部跳过、报告成功（§2.4）。
10. **ELF 的检查归 xlings（复用已有的 `xlings.libs.elfread`），libxpkg 只打标签；架构的身份只有一份，在 libxpkg。**
11. **构建侧：既有设计的第 2 步不能照搬，已改写并做了原型。** 实测 #532 仍在；第 2 步对 SubOS 自己的 gcc 有效，
    但经过同一个 `ld` 的宿主编译器会得到一启动就崩的程序。改为：只有产物的 loader 不是系统路径时才写入 farm。
    九种情形的原型全部符合预期（§2.8、§8.3）。
12. **落地顺序由两个依赖决定**：规则 M 变硬之前，索引必须先在两种架构上证明自己干净；
    索引用上新的 hook 接口之后，它的最低客户端就是带这个接口的那一版，所以索引的 PR 在 xlings 发布之后合并（§5.4、§12）。
13. **先把基线恢复。** `main` 的 CI 现在有两处是红的（§2.6），"每个检查点都通过测试"以它们变绿为前提；
    两个开放的 PR 的内容也在这第一个检查点里：#738 的标签改动整体带入，#710 已被 `main` 取代、只带入它多出的一步断言（§10.2）。
14. **hook 的接口重新设计：一份模块 API，`spec = "3"` 去掉兼容层。** 查基线失败时找到的问题是：hook 里失败的调用
    不让 hook 失败（`os.mv` 失败后空载荷被记为已安装，实测 E16）。原因不在某几个函数，而在 hook 的环境是三层叠出来的：
    两套文件词汇、五种运行命令的方式、不认识的模块返回一个吞掉所有调用的桩（§2.9）。
    - 新接口里一次调用只有三种：**动作**（没做到就让 hook 失败）、**询问**（答不了才报错）、**声明**（交给宿主做）。
      容忍失败只有一种写法：Lua 自己的 `pcall`。
    - 模块只有一份，新旧 recipe 共用。`spec = "3"` 的 recipe 的环境里没有兼容层；`spec` 为 1、2 的 recipe 的环境原样冻结，
      已安装的包与第三方索引不受影响。老名字的语义一个都不改。
    - 旧客户端由两个已有的机制挡住：索引快照的最低客户端（实测 E18），以及对不认识的 `spec` 的逐包拒绝（实测 E14）（§9.6）。
15. **宿主放好，hook 只做调整。** 索引里 588 个 install hook 中有 171 个只做一件事：找到解压出的目录，搬进安装目录；
    89 个 recipe 靠改归档的文件名来猜那个目录叫什么。xlings 已经在进程内解压，没有 hook 的包今天就是由它放好的。
    `spec = "3"` 把这变成唯一的模型：`install()` 运行之前内容已经在安装目录里，布局由条目的 `strip` 声明，
    `install()` 可以不写。搬运这一步不存在了，它的失败（E16）也就不存在了（§9.7）。
16. **第三种环境用一句声明代替。** 三个子索引各有一份一百多行、内容相同的构建脚本，只为把一个模板接到每个 recipe 后面；
    libxpkg 为它另备了一套环境。改为在 `index-compat.json` 里声明模板，加载器在内存里拼接，不再改写检出的文件（§9.8）。
17. **xpkg 的规范收成一份。** 现在是两个仓库里的三份文档、三种叫法，hook 的接口没有任何一份现行规范写着。
    做法：一个名字（xpkg、recipe、`spec`）；一份规范，libxpkg 与 xlings 两处都放、互相链接、内容由检查保证相同；
    一份接口清单（libxpkg，参考文档、兼容层的对照、迁移工具都从它生成）；一套一致性夹具；
    一个检查器，命令是 `xlings index check`（§9.9）。

## 1. issue 与章节

| issue | 一句话 | 章节 | 仓库 | 关闭条件 |
|---|---|---|---|---|
| #740 §1 | 单产物 recipe 的架构检查 fail-open | §5 | 三个 | xlings 发布且索引迁移完成 |
| #740 §2 | musl flavor 留下 `gcc-ar` 等三个名字 | §7.4 | 索引 | 索引 PR |
| #740 §3 | 载荷里烙着构建机路径 | §8.2 规则 P | xlings | xlings 发布 |
| #740 §4 | host-link 的刷新 | — | — | 刷新已由 `host_facts` 解决（2026.11.23.1）；Vulkan 部分留在 xim-pkgindex#1152 |
| #740 §5 | 外部安装器包形态 | §9.4 | 规范 | 记为不采用；rust 按 xim-pkgindex#569 改 |
| #739、#607 第 1 条 | 精确版本被前缀满足 | §7 | xlings | xlings 发布 |
| #607 第 2 条 | 拒绝后仍改声明 | §6.2 | xlings | 同上 |
| #708 | `remove --older-than` 中止或静默跳过 | §6.2 | xlings | 同上 |
| #721 | hook 的警告丢失 | §9.2 | libxpkg、xlings | 同上 |
| #709 | `lts` / `stable` 别名 | §7.3 | xlings、索引 | 同上 |
| #736 | 扁平归档混在共享目录 | §8.4（`spec` 为 1、2）、§9.7（`spec = "3"` 起不再有这个目录） | xlings | 同上 |
| #532、#540 | subos 里构建的程序跑不起来 | §8.3 | 索引、mcpp | binutils 修订发布、mcpp 发布 |
| #566 | `ExecutionContext` 七处构造 | §10 | xlings | xlings 发布 |
| #594 | 环境变量恢复成空串 | §10 | xlings | 同上 |
| #593 | 新动词没有接口能力 | §10 | xlings | 同上 |
| #592 | 契约只在 bash 里测 | §11 | xlings | 同上 |
| #425 | 没有地方提示有新版 | §9.2 | xlings | 同上 |
| #417 | CN 镜像上新鲜度探测恒失败 | §10 | xlings | 同上；#741 的 UP-G1 随之关闭 |
| #741 UP-L1 | 缺补丁工具时跳过却报成功 | §9.3 | libxpkg | libxpkg 0.0.68 |
| xim-pkgindex#1442 | mesa 的 aarch64 | §5.8 | 索引 | 资产发布且 recipe 合并 |
| PR #738 | 作者标签改为两个值 | §10.2 | xlings | 已关闭，改动作为 X0 的一个 commit 带入 |
| PR #710 | macOS 的 CI 改用 macos-15 | §10.2 | xlings | 已关闭；`main` 已是 macos-15，只带入"已安装的 xlings 声明 minos 14.0"这一步 |

不在本方案内：#2（信息帖）、#741 的 UP-P1 和 UP-P2（proot 自身的缺陷）、xim-pkgindex#1341（发布前的资产清理，
本方案只提供它要用的规则定义，§8.2）、xim-pkgindex#1152 的 Vulkan 部分、xim-pkgindex#937 剩下的消费者验证。

## 2. 证据

### 2.1 实测（已发布的 2026.11.23.1，Linux x86_64，隔离 home）

| 编号 | 做了什么 | 结果 |
|---|---|---|
| E1 | 本地 recipe：`archs = {"aarch64"}`，一个条目，`spec = "1"` 与 `"2"` 各一次，在 x86_64 上安装 | 都只打印一行 warn，照常安装，退出码 0 |
| E2 | 本地 recipe：固定 `url`，`sha256 = { aarch64 = "…" }`，在 x86_64 上安装 | `E_UNSUPPORTED_TARGET: … has no linux-x86_64 artifact; supported targets: linux-aarch64`，退出码 1，没有装任何东西 |
| E3 | store 里有 glibc 2.44.2 与 2.44.3，subos 声明 2.44.3；`xlings subos runtime glibc@2.44.2 default` | 打印 `runtime activation refused … nothing was changed`，随后 `now declares glibc@2.44.2`，退出码 0；视图仍指向 2.44.3 |
| E4 | 接着 `xlings subos runtime glibc@2.44 default` | 被 2.44.2 满足；声明写成 `glibc@2.44`，store 里没有 2.44 |
| E5 | recipe 的 `config()` 调 `log.warn("…")`，输出重定向到文件，三种方式安装 | 默认：只进 hook 日志；`-v`：两处都没有；`XLINGS_HOOK_OUTPUT=inherit`：两处都没有。三次都报告成功 |
| E6 | 索引里 `libxshmfence` 的 recipe 原样，只把 `sha256 = "<h>"` 改成 `{ x86_64 = "<h>", aarch64 = "<h>" }` | 正常下载、校验、安装（7 个文件），退出码 0 |
| E7 | 同上，但 x86_64 的值改成全零 | `sha256 mismatch … got <h>, want 000…`，退出码 1，没有载荷 |

E2、E6、E7 是 §5.3 与 §5.6 的依据：迁移后的写法在已发布的客户端上既能装、也校验、也拒绝。

### 2.2 索引普查（xim-pkgindex `main` 47591c3e，602 个 recipe）

方法：用只有桩函数的 Lua 环境加载每个 recipe，读 `package` 表；取 `linux` 段里 `latest` 所指的条目，
按客户端 `compatibility.cpp` 的同一套分级归类。局限：只看 Linux 段、只看 `latest` 指向的那个条目；
"是否真的是本机二进制"无法从 recipe 得知。

| 类 | 数量 | 说明 |
|---|---|---|
| 按架构枚举（Strong） | 353 | 330 个含 aarch64；23 个只有 x86_64（含 `cmake`、`llvm` 的 `latest`、`luban-os`、`xdesk`） |
| 模板或 `res`（Open） | 5 | `musl-gcc`、`aarch64-linux-musl-gcc`、`patchelf`、`dash`、`xvm` |
| **单产物，带 URL（Weak）** | **140** | 89 个只声明 x86_64；51 个声明两种以上架构 |
| 单产物，经 `source`（Weak） | 19 | 多为 sysroot、磁盘镜像、头文件 |
| 只有 hook，没有产物（Weak） | 36 | 13 个只声明 x86_64 |
| 没有 `archs`（None） | 17 | 3 个带 URL |
| 没有 Linux 段或没有版本 | 32 | |

- **89 个"单产物、只声明 x86_64"**：在 aarch64 上得到一行 warn 后照装（E1 的情形）。其中 65 个是 spec 2。
  图形栈的库占大半（mesa、libllvm、libdrm、cairo、gtk4、harfbuzz 等）。
- **51 个"单产物、声明两种架构"**：在 aarch64 上**没有任何提示**。里面有两种东西：
  - 与架构无关的：字体、光标、SDK、sysroot、磁盘镜像、JVM 工具（`maven`、`gradle`、`kotlin`）、脚本；
  - 本机二进制：`bat`、`fd`、`fzf`、`gh`、`ripgrep`、`uv`、`zoxide`、`sing-box`、`xmake` 等。
    以 `bat` 为例：`archs = {"x86_64", "aarch64"}`，Linux 条目是 `…x86_64-unknown-linux-musl.tar.gz`；
    `aarch64` 来自它的 macOS 条目。
- 结论：`package.archs` 是跨平台的并集。用它拒绝会误拒（少写的），用它放行会漏放（并集里有但本平台没有的）。
- 索引里 hook 的日志调用：`log.warn` 122 处（51 个 recipe），`log.error` 136 处，`log.info` 233 处。
  抽样的 `log.warn` 多数是给用户的话（许可、要手动执行的步骤、与用户已有配置的冲突），少数是进度。

### 2.3 mesa 的 Linux 依赖闭包（25.0.7.3）

沿 recipe 的运行期依赖展开，共 22 个节点。

| 状态 | 节点 |
|---|---|
| 已有 aarch64（4） | `glibc` 2.44.3、`gcc-runtime` 15.1.0、`zlib` 1.3.1、`libxml2` 2.13.5-1 |
| 只有 x86_64（18） | `mesa`、`libllvm` 20.1.7、`libglvnd` 1.7.0.1、`libdrm`、`libpciaccess`、`libX11`、`libxcb`、`libXau`、`libXdmcp`、`libXext`、`libXfixes`、`libXxf86vm`、`libxshmfence`、`xorgproto`、`expat` 2.6.2、`libffi`、`elfutils`、`wayland` |

- 18 个全部是"单产物、只声明 x86_64"。
- 构建输入里同样缺 aarch64 的：`cmake`（Linux 段只有 x86_64 条目）、`meson`、`xcb-proto`、`xtrans`、
  `wayland-protocols`、`glslang`；x86_64 的 mesa 还用了 `llvm-dev`、`spirv-tools`、`directx-headers`（clc 链）。
- 已有 aarch64 的工具：`gcc` 16.2.0、`binutils` 2.47、`linux-headers`、`make`、`ninja`、`python`、`perl`、
  `zstd`、`xz`、`patchelf`，以及 `xlings`、`luban`、`mcpp`、`bwrap`、`proot`、`qemu-system`。
- 这些载荷现在由索引仓库 `.agents/tools/graphics/`（`tiers.sh`、`build-in-subos.sh`）在一台本地机器的 subos 里构建；
  脚本里写死了 `x86_64`（输出文件名、`PKG_CONFIG_LIBDIR` 的多架构目录、`gen-recipes.py` 的 `archs`）。
- 索引已有按架构构建的设施：`res-build.yml` 按 `matrix.arch` 选 `ubuntu-24.04-arm`，固定容器，
  构建与发布分离，发布在受保护的环境里，GitCode 由 CN 机器补齐并按内容校验。图形栈没有用它。
- 索引的周期性全量安装（`ci-sweep.yml`）只有 x86_64 的 Linux 与 Windows 两条腿，没有 aarch64。

### 2.4 代码事实

| 事实 | 位置 |
|---|---|
| Weak 证据固定 `supported = true`，只给 advisory | `src/core/xim/compatibility.cpp:121-152` |
| 解析器只读 `supported`，逐节点判断 | `src/core/xim/resolver.cpp:436` |
| 宿主架构取自客户端的构建架构 | `src/core/xim/installer.cpp:1419`（`platform::build_arch()`） |
| 安装期已对每个新载荷做 ELF 扫描，只 warn | `src/core/xim/installer.cpp:4070`（`closurecheck::scan_payload`） |
| 进程内的 ELF 读取器已经存在，在共享模块里 | `modules/libs/src/elfread.cppm`（`xlings.libs.elfread`） |
| recipe 读不了、归档任务失败，都只 warn 后跳过该节点 | `src/core/xim/installer.cpp:2868`、`:2913` |
| 激活失败降级为 warn | `src/core/xim/commands.cpp:1298-1301` |
| `subos runtime` 无条件写声明 | `src/core/subos/cmd.cpp:1940-1955` |
| "幂等"：`name@<前缀>` 接受任何以它开头的已激活版本 | `src/core/xim/commands.cpp:753-760` |
| 已安装集合里先精确、再前缀 | `src/core/xvm/db.cpp:424-490` |
| 索引侧先精确键、再范围 | `src/core/xim/catalog.cpp:158-172` |
| "是否已安装"只有一个回答者，三种状态 | `src/core/xim/install_state.cppm` |
| 没有日志文件的三种情形 | `src/core/xim/installer.cpp:906-915`（`hook_log_path_`） |
| hook 自己的 `print` / `io.write` 收进 `HookResult::output` | libxpkg `src/xpkg-executor.cppm:93-113` |
| `HookResult` 只有 `success`、`output`、`error`、`version` | libxpkg `src/xpkg-executor.cppm:120` |
| hook 可以在边界进程里运行，协议有版本号 | libxpkg `src/xpkg-executor.cppm`（`kHookBoundaryProtocol = 1`） |
| `log.warn` 就是带 `[WARN]` 前缀的 `io.write` | libxpkg `src/lua-stdlib/xim/libxpkg/log.lua` |
| 声明式通道的先例：`system.admin_step` 走延迟请求 | libxpkg `system.lua:37`；xlings `commands.cpp:1582` |
| `elfpatch` 读 `e_machine`，对另一种机器的文件跳过 | libxpkg `elfpatch.lua:199-296` |
| `apply_elfpatch_auto` 只回传三个数 | libxpkg `src/xpkg-executor.cppm:1429-1436` |
| 缺 `patchelf` / `install_name_tool` 时 warn 后返回，查工具在扫描之前 | libxpkg `elfpatch.lua:673`、`:794` |
| Scheme B（按架构的资源表）只认 `x86_64`、`aarch64`、`x86` 三个键 | libxpkg `src/xpkg-loader.cppm:751-753` |
| mcpp-index 里 `mcpplibs.xpkg` 最新是 0.0.61；libxpkg `main` 是 0.0.67 | mcpp-index `pkgs/x/xpkg.lua`；xlings `mcpp.toml:200` 钉的是 git rev |
| 索引声明最低客户端并保留兼容快照 | xim-pkgindex `index-compat.json`（当前 `2026.10.10.2`） |
| 安装脚本可以从指定地址取某个版本的客户端 | `tools/other/quick_install.sh`（`XLINGS_VERSION`、`XLINGS_BASE_URL`） |
| `host_facts`：声明的宿主文件变了就重跑 `config()` | `src/core/xim/host_facts.cppm`；`pkgs/n/nvidia-gl-host-link.lua` |
| 已安装载荷的 loader 与运行时经 `<home>/abi/<family>` 绑定 | `src/core/abi.cppm` |
| 索引 CI 的闭包检查只执行 D（D1、D2） | xim-pkgindex `.github/scripts/dep-closure-check.sh` |
| 规范写"fail-closed" | xim-pkgindex `docs/specs/xpackage-v2.md:205`、`xpackage-template.lua:12` |
| `main` 的标签 workflow、模板与贡献 skill 仍是三个值（`by-ai`、`human-and-ai`、`by-human`） | `.github/workflows/labels.yml:31`、`:46`；`.github/ISSUE_TEMPLATE/*.md`；`.agents/skills/xlings-contributing/SKILL.md:70` |
| GitHub 上的标签已改成两个（`by-ai` 改名为 `ai-and-human`，`human-and-ai` 已删） | 仓库的标签列表；正文写着 `Authorship: ai-and-human` 的 #740、#741 因此没有被打上标签 |
| `main` 的 macOS 任务已在 macos-15 上 | `release.yml:912`、`xlings-ci-fresh-install.yml:207`、`xlings-ci-luban-fresh.yml:99`、`xlings-ci-macos.yml:38` |
| minos 14.0 只在构建处断言，没有对已安装的二进制断言 | `xlings-ci-macos.yml:343-354`、`tools/macos_release.sh:100` |

### 2.5 没有实测的

- aarch64 主机上的任何运行（E1 是把角色对调后在 x86_64 上做的）。
- 规则 M 在全索引上的误报率（§5.4 的门槛就是为它设的）。
- mesa 在 aarch64 上的 meson 配置：哪些驱动要 clc 链（§5.8）。
- `libllvm` 在 `ubuntu-24.04-arm` 上的构建时长。
- #736 的污染（证据是 xim-pkgindex#1273 的闭包检查失败与 #1274 的分析）。
- 索引安装测试所钉的旧客户端对迁移后条目的行为（依据是规范写的"0.4.61 起支持按架构的形态"；
  E6、E7 只在 2026.11.23.1 上做过）。
- `ld` 包装器的原型只在 x86_64、gcc 16.2.0、binutils 2.47 上跑过；aarch64、lld、musl 的工具链没有测。
- `u-ubuntu-2004` 那条腿上有没有 patchelf（决定 `set_interpreter` 是否也被跳过，§2.8）。
- 新的 hook 接口、落地模型、模板声明都没有原型：§9.6 到 §9.8 的每一条都还只是设计。E14 到 E18 测的是已发布客户端的现状。
- 索引里今天实际有多少处 hook 调用在失败、有多少个 recipe 依赖这种失败（§2.9），要全量安装才知道。
- mcpp 是否经 libxpkg 的执行器运行 mcpp-index 里 recipe 的 hook，没有查；新环境由 `spec` 选择，不改 `spec` 的 recipe 不受影响。
- 迁移工具能自动改写的比例（§9.6）；全量安装没有 macOS 的腿，只在 macOS 分支里出现的调用今天无法由它验证。

### 2.6 基线：`main` 的 CI 现状（7f9f2fb1）

| workflow | 状态 | 失败处 | 原因 |
|---|---|---|---|
| `xlings-ci-host-matrix` | 连续三次失败 | `u-ubuntu-2004` 的 `hookrt` 一格："a hook's program that needs a newer glibc fails, and says which it saw"，实际退出码 0 | 已查（§2.8）：产品行为正确，用例的夹具用了不报告失败的 `os.exec`。自 2026-11-22 这个用例加入起就失败 |
| `xlings-ci-linux` | 最近两个提交都失败 | `unit (1/6)`：`LubanDesktopBus.AnAgentDrivesADesktopOnlyWithItsGrant` | 未查 |
| `xlings-ci-fresh-install`、`xlings-ci-luban-fresh` | 通过 | | |

PR #738 的失败检查（`acceptance`、`build-and-test`、`distro`、host-matrix 的几条腿）没有逐条与基线对照；
它只改模板、一个 workflow 与一份 skill，不触及被测代码。

### 2.7 迁移的判断可以直接验证（抽样，2026-11-27）

对 §2.2 里"单产物、声明多种架构"与"经 `source` 的单产物"两组（去重后 68 个）做了三件事。

**按文件名里的架构词分**：20 个带 x86_64 的词，8 个带 aarch64 的词，38 个不带，2 个没有 URL。
文件名**不可靠**：带架构词的 28 个里有 13 个是以目标命名的 sysroot 与磁盘镜像（`x86_64-freebsd-sysroot`、
`aarch64-linux-gnu-sysroot` 等），词说的是目标不是宿主；`edk2-firmware` 的文件名写着 `linux-x86_64`，里面没有机器码。

**读字节**（不解包、不执行，只读归档成员的文件头；9 个样本）：

| 产物 | 归档里的 ELF | 结论 |
|---|---|---|
| `adwaita-cursors`、`nvm`、`terminfo`、`edk2-firmware`、`composer` | 没有 | 与架构无关，静态即可定 |
| `android-platform-tools`（今天声明两种架构） | 8 个 x86_64 的 Linux 程序 | 只有 x86_64，静态即可定 |
| `bat` 的 aarch64 产物 | 1 个 aarch64 程序 | aarch64 |
| `bat` 的 x86_64 产物 | 1 个 x86_64 的 static-pie（没有 `PT_INTERP`，静态看像库） | 要靠安装后的规则 M：登记的程序就是它 |
| `x86_64-freebsd-sysroot` | 198 个 x86_64 的库，没有 Linux 程序 | 要靠安装后的规则 M：它不向宿主开放任何文件 |

**探测上游有没有另一种架构的产物**（把文件名里的架构词替换后请求）：12 个本机工具里 10 个有
（`bat`、`cc-connect`、`fd`、`fzf`、`gh`、`github-gh`、`ripgrep`、`sing-box`、`uv`、`zoxide`），
2 个没有（`xmake` 的单文件包、`project-graph` 的 deb）。

结论：判断不需要人逐个去看。字节与两种架构上的一次安装就能给出答案，§5.6 据此改为由工具完成。

### 2.8 两处遗留问题的实测（2026-11-27，已发布的 2026.11.23.1，Linux x86_64）

**A. `hookrt` 的基线失败**

用例（`tests/host-matrix/scenarios/hookrt.sh`）在宿主 glibc 早于 2.34 时期望：hook 里启动的程序失败，
xlings 说出 hook 看到的是哪个 glibc。夹具的 `install()` 复制 glibc 载荷里的 `getconf`，用
`elfpatch.set_interpreter` 把它的 loader 指到标准路径，再 `os.exec(program .. " PAGESIZE")`，然后 `return true`。

| 编号 | 做了什么 | 结果 |
|---|---|---|
| E8 | hook 里 `os.exec("/bin/false")`，随后 `return true` | `os.exec` 返回 `nil, "exit", 1`；安装成功，退出码 0 |
| | hook 里 `os.exec` 一个不存在的程序 | 安装成功，退出码 0 |
| | hook 里 `system.exec("/bin/false")` | hook 失败，退出码 1 |
| | hook 里 `os.cp` 一个不存在的文件 | 返回 `false`；安装成功，退出码 0 |
| E9 | 用例的夹具原样，程序换成必然失败的调用（`getconf` 一个不存在的变量） | 用 `os.exec`：安装成功，退出码 0（与 CI 上看到的一样）。换成 `system.exec`：hook 失败，错误里有 `programs the hook starts use this machine's glibc 2.39 (the environment's is 2.44.3)`，正是用例要找的那句 |

- 结论：hook 的运行时契约与那句说明都工作正常；失败的是夹具。`os.exec` 是 Lua 的 `os.execute` 的包装，只返回状态；
  `system.exec` 才在失败时报错。
- 还有第二处可能叠加：`elfpatch.set_interpreter` 在找不到 patchelf 时只 warn、返回一张空的结果表。
  那条腿（`ubuntu:20.04` 加 `bare.sh`）没有装 patchelf，计划里也只有 glibc 与夹具两个包；如果 loader 没被改写，
  程序会用载荷自己的 loader 正常运行。日志里看不到 hook 的输出，这一点没有证实。
- 索引里同类调用的数量（作为语句使用、结果被丢弃的）：`os.mv` 497 处，`os.mkdir` 156 处，`os.cp` 68 处，
  `os.exec` 63 处（共 69 处里只有 2 处用了返回值）；检查了返回值的 `os.cp` 与 `os.mv` 是 0 处。
  `system.exec` 427 处，失败时报错。

**B. #532 与 `ld` 包装器**

干净的 home，装 `xim:gcc`（16.2.0）、`xim:zlib`、`xim:libglvnd`，在 `xlings subos use default --cmd` 里构建并运行。
binutils 2.47 的包装器今天只追加 `-rpath-link "$XLINGS_SUBOS_LIB"`。

| 编号 | 情形 | 链接 | 产物 | 运行 |
|---|---|---|---|---|
| E10 | SubOS 的 gcc，只用 libc | 通过 | loader 与 `RUNPATH` 都直接指向 glibc 2.44.3 与 gcc 16.2.0 两个载荷 | 通过 |
| | SubOS 的 gcc，`-lz` | 通过 | 同上 | **失败**：`libz.so.1: cannot open shared object file` |
| | SubOS 的 gcc，`-lGL` | 通过（链接期那一半确已修好） | 同上 | **失败**：`libGL.so.1` 找不到 |
| E11 | 同上两例，手工加 `-Wl,--disable-new-dtags -Wl,-rpath,$XLINGS_SUBOS_LIB` | 通过 | `RPATH`：glibc 载荷、gcc 载荷、`<subos>/lib`，按这个顺序 | 通过；libc 来自 glibc 载荷，libz 来自 `<subos>/lib` |
| E12 | **宿主的 gcc**（`/usr/bin/gcc`，它从 PATH 上取到的 `ld` 就是这个包装器），手工加同样的两个参数 | 通过 | loader 是 `/lib64/ld-linux-x86-64.so.2`，`RPATH` 只有 `<subos>/lib` | **启动即崩**：`symbol lookup error: <subos>/lib/libc.so.6: undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE` |

- E10：#532 的运行期那一半在当前客户端上原样复现。
- E12 是 2026-08-11 设计第 2 步的问题：`<subos>/lib` 里有 `libc.so.6`（指向环境的 glibc）。包装器若对经过它的每一次链接
  都追加 `-rpath`，宿主编译器（以及 cargo、go 这类调用系统 `cc` 的工具）的产物就会用宿主的 loader 加载环境的 libc。
  #540 担心的是 mcpp 一家；实际受影响的是所有不是 SubOS 自己工具链的构建，而它们不知道要去声明退出。
- 另一个事实：用户构建的程序把 glibc 载荷的版本目录写进了 loader 与搜索路径（`xim-x-glibc/2.44.3`），
  没有经过 `<home>/abi/<family>`。这是 gcc 的 `specs` 写的，不是包装器写的（§14）。

**改写后的包装器的原型**（E13；把 §8.3 的规则写进实验 home 里的包装器后再跑）：

| # | 情形 | 产物 | 运行 |
|---|---|---|---|
| 1 | SubOS 的 gcc，`-lz` | `RPATH` 末尾多了 `<subos>/lib` | 通过 |
| 2 | SubOS 的 gcc，`-lGL` | 同上 | 通过 |
| 3 | SubOS 的 gcc，只用 libc | 同上 | 通过 |
| 4 | 宿主的 gcc 经同一个 `ld`，`-lz` | 没有被改动（系统 loader，没有 `RPATH`） | 通过 |
| 5 | 宿主的 gcc，只用 libc | 没有被改动 | 通过 |
| 6 | SubOS 的 gcc，`-shared` | 没有被改动 | — |
| 7 | SubOS 的 gcc，`-static` | 没有被改动 | 通过 |
| 8 | SubOS 的 gcc，`-lz`，`XLINGS_SUBOS_LD_PATHS=0` | 标签是 `RPATH`，没有 `<subos>/lib` | 找不到 libz（预期：路径由声明退出的一方自己负责） |
| 9 | SubOS 的 g++，`-lz` | 同 1 | 通过；libc 来自 glibc 载荷，libstdc++ 来自 gcc 载荷，libz 来自 `<subos>/lib` |

### 2.9 hook 环境的现状（2026-11-27；libxpkg 1485db9 的代码，索引 47591c3e 的调用计数，2026.11.23.1 上的实测）

**环境是三层叠出来的**（`src/lua-stdlib/prelude.lua`、`xpkg-executor.cppm` 的 `register_os_funcs` 与 `register_fs_module`）：

| 层 | 内容 | 失败时 |
|---|---|---|
| Lua 5.4 的标准库，完整 | `os.execute`、`os.remove`、`os.rename`、`io.open`、`io.popen` 等 | 返回状态 |
| 补进标准表与全局的一层 | `os.mv`、`os.cp`、`os.mkdir`、`os.exec`、`os.tryrm`、`os.isfile`、`os.iorun`、`io.readfile`、`string.replace`，全局的 `path`、`format`、`raise`、`is_host`、`try`、`cprint` | 返回布尔值；`os.tryrm` 恒为 `true` |
| 模块（`import("xim.libxpkg.<名字>")`） | `pkginfo`、`xvm`、`system`、`log`、`fs`、`elfpatch`、`subos`、`json`、`utils`、`pkgmanager`、`base64` | `system.exec` 报错；`fs.*` 返回布尔值 |

- **两套文件词汇，都不报错**：`os.isfile` 与 `fs.is_file`、`os.isdir` 与 `fs.is_directory`、`os.mkdir` 与 `fs.mkdir_p`、
  `os.tryrm` 与 `fs.remove_all`、`os.dirs` 与 `fs.dirs`。
- **前缀不表达含义**：`os.mv` 与 `os.trymv` 是同一个函数。
- **运行命令有五种方式**：`system.exec`（失败报错）、`os.exec`、`os.execute`、`os.iorun`、`io.popen`（都不报错）。
  执行器只把 `os.execute` 换成了写 hook 日志的版本，`io.popen` 是 Lua 自带的那个。
- **不认识的模块返回一个桩**：对它的任何调用都返回一张表，什么也不做。

**实测**

| 编号 | 做了什么 | 结果 |
|---|---|---|
| E14 | 一条命令里装一个 `spec = "3"` 的包和一个 `spec = "2"` 的包 | 前者被拒绝：`xpackage spec "3" is newer than this xlings understands (max 2)`，下一行 `try: xlings self update`；后者照常安装。命令的退出码是 0（这是 §6 要改的：被请求的节点被拒绝，命令应失败） |
| E15 | hook 里 `import` 一个不存在的模块并调用它的函数 | 调用返回一张表；安装成功 |
| E16 | hook 里只有 `os.mv("<不存在的目录>", pkginfo.install_dir())`，随后 `return true` | `os.mv` 返回 `false`；安装成功；载荷里只有 xlings 自己写的标记文件；`xlings list` 把它列为已安装 |
| E17 | hook 里 `os.tryrm` 一个删不掉的路径 | 返回 `true` |

**索引里的调用**（`pkgs/` 与 `libs/`，次数）

| 用途 | 调用 |
|---|---|
| 文件的询问 | `os.isfile` 1186、`os.isdir` 407、`os.dirs` 52、`fs.files` 41 |
| 文件的动作 | `os.tryrm` 646、`os.mv` 508、`fs.mkdir_p` 213、`os.mkdir` 170、`io.writefile` 93、`os.cp` 74、`fs.symlink` 3、`fs.remove` 1 |
| 命令 | `system.exec` 435、`os.iorun` 101、`os.exec` 69、`io.popen` 60、`os.execute` 10 |
| 路径与文字 | `path.join` 1827、`format` 701、`path.directory` 180、`string.replace` 137、`path.filename` 132 |
| 平台 | `os.host` 169、`is_host` 156、`os.arch` 43 |
| 出错与容错 | `error` 571、`raise` 241、`try` 72、`pcall` 51 |

- `system.exec` 运行的命令里，`chmod` 74 次，`tar` 与 `unzip` 39 次；`io.popen` 的 60 次里至少 14 次是 `ls -1` 或 `dir /b`，
  用来列目录：`os.files` 在 hook 里不存在（索引的注释里记着 `attempt to call a nil value (field 'files')`）。
- `os.mv`、`os.cp`、`os.mkdir` 的 752 次调用里，用了返回值的是 2 次。
- xlings 里有一处**依赖**这种失败：安装目录在 hook 之后仍为空时，把解压出的内容整个搬进去。`installer.cpp:3752` 的注释写明
  它服务的是"归档没有顶层目录、hook 的 `os.mv` 因为找不到那个目录而什么都没做"的 recipe。

**hook 的数量与 install hook 在做什么**（602 个 recipe）

| | 数量 |
|---|---|
| 有 `install()` | 588 |
| 有 `config()`、`uninstall()`、`installed()`、`build()` | 569、578、216、0 |
| `install()` 里只有移动、删除、建目录与存在性判断（外加断言） | 171 |
| 用改归档文件名的办法推出解压目录（`install_file():replace(".tar.gz", "")` 之类） | 89 |
| `install()` 里的 `os.tryrm` 与 `os.mv` | 461 次、397 次 |

- xlings 在进程内解压（libarchive，`src/core/xim/extract.cpp`）。没有 `install()` 的包：归档解到这次安装私有的目录，
  再原样放进安装目录，顶层目录不去掉（`docs/specs/xpkg-manifest-v1.md` §6）。
- 有 `install()` 的包：归档解在下载文件的旁边，由 hook 自己找到并搬走。
- hook 运行的 673 条命令里，字面量中出现管道、重定向、`&&`、通配符等 shell 语法的约 75 条（按同一行里的字面量粗略统计）。

**索引的构建脚本**

- 官方索引没有。三个子索引（scode、awesome、d2x）各有一份 `pkgindex-build.lua`，122 到 126 行，除名字外相同：
  它的 `install()` 把仓库里的 `template.lua`（38 到 85 行，共用的 hook）接到 `pkgs/` 下每个文件的后面。
- 脚本要在两种运行时里都能跑，所以逐个探测 `pcall`、`cprint`、`raise` 是否存在。
- libxpkg 为它另备了一套环境（`register_build_sandbox`）：自带一份 `path`，`os.cd` 与 `os.execv` 是空函数。
  运行前 libxpkg 用 `git checkout -- pkgs/` 把上一次改写的文件恢复原样。

**规范文档的现状**

| 文档 | 位置 | 叫法 | 写了什么 |
|---|---|---|---|
| `xpkg-manifest-v1.md`（470 行，英文，现行） | xlings `docs/specs/` | "xpkg manifest spec v1"，`spec = "1"` | 字段、`xpm`、修订、依赖；hook 只列了 `install`、`config`、`uninstall`；模块只列了三个 |
| `xpackage-v2.md`（855 行，英文，现行） | 索引 `docs/specs/` | "XPackage Spec V2"，`spec = "2"` | 只写相对 V1 的新增；"其余见 V1" |
| `legacy/V1/xpackage-spec.md`（556 行，中文，标为废弃） | 索引 | "XPackage Spec V1" | 唯一一张 hook 接口的表 |

- hook 的接口没有现行的规范：唯一的表在一份标为废弃的文档里。
- libxpkg 的代码里有五个 hook；现行规范写了三个；`build()` 在索引里没有使用者。
- mcpp-index 的 recipe 是 `spec = "1"`；它有自己的客户端契约文件（`index.toml` 里的 `min_mcpp`）。

**已有的兼容机制**

- 索引快照带最低客户端（`index-compat.json` 的 `requires.xlings.min`，现在是 2026.10.10.2）；客户端取它满足的最新快照，
  并说明原因（2026.8.4.1 起）。指针的历史保留每一代契约的最新快照（`docs/specs/index-version-contract.md` §4）。
- **E18**（实测）：2026.10.9.2 的客户端（低于现在的下限）执行 `xlings update`，得到
  `47591c3e requires xlings >= 2026.10.10.2, this is 2026.10.9.2`，落在下限为 2026.10.8.1 的那一代里最新的快照上，
  `xlings search` 正常。规范写明提高下限的条件是"索引整体依赖新能力"，并要求历史里先有可回落的快照（同一份规范 §3、§6）。
- 客户端拒绝高于自己上限的 `spec`，只拒绝那一个包；移除不受影响（2026.7.27.1 起，早于索引现在的下限）。
- `xlings self update` 读**最新**的快照而不是它满足的那个（`xself/update.cpp`），否则被带回旧快照的客户端永远看不到新版本。
  所以 `xlings` 自己的 recipe（现在是 `spec = "1"`）必须一直是每个旧客户端都读得懂的写法。

## 3. 根因

**R1 检查发现了问题，失败分支只 warn。** 位置见 §2.4。项目有"读不了不等于空"，没有"warn 不是结果"；
每一步自己决定失败后是否继续，命令的退出码不是从各步结果算出来的。

**R2 架构有三个说法，只有最弱的那个被当作依据。** 条目枚举的架构（可信）、载荷的 `e_machine`（事实）、
`package.archs`（跨平台并集）。单产物条目没有第一种，客户端又不看第二种，只剩第三种，而它既少写也多写。

**R3 一个版本串两种读法。** `2.44` 在索引里是一个具体条目；在已安装集合里没有它时被当成前缀。
优先级是"声明 > 已激活 > 索引"，前缀那一侧先答；同一计划里的另一个节点也用同一个前缀匹配，
于是 `2.44` 的 `install()` 不运行、载荷为空，却记为已安装（#739 的同一条命令情形）。命令还把请求串原样写进声明。

**R4 路径的规则散在四个执行者里。** 索引 CI 的闭包检查、libxpkg 的 `elfpatch`、xlings 的 `closure_check`、
构建侧（mcpp 与 `ld` 包装器）各有一份理解；"哪种文件允许出现哪种路径"只写在注释和设计记录里。
多个包共用一个解压目录是同一问题的另一面：文件没有归属。

**R5 hook 向宿主传话只有布尔和文本。** `log.warn` 与调试输出走同一条流，宿主分不出哪一行是给人看的；
"跳过了"只能返回成功。凡是做对了的地方都把事情结构化地交给宿主：`system.admin_step`、`subos.env{}`、`host_facts`。

**R6 hook 里一次调用失败了怎么办，由名字碰巧决定。** 环境是三层叠出来的（§2.9），同一件事有两三种写法，
其中只有 `system.exec` 在失败时让 hook 失败。recipe 的作者把 `os.mv(...)` 当语句写，等于默认它会成功；
它不成功时没有任何一层知道。这是 R1 在 recipe 的运行时里的同一个问题，只是这里连 warn 都没有。

## 4. 五条契约与四个词

- **C1 结果**：一步的结果是 `Done`、`Skipped(原因)`、`Refused(原因)`、`Failed(错误)` 之一。
  `Refused` 的定义是**什么都没改**；`Failed` 是做了一部分、已回滚到之前的状态。
  命令的结论与退出码由各步结果计算：被请求的节点有 `Refused` 或 `Failed`，命令失败；`Skipped` 列在总结里。
- **C2 架构**：计划阶段依据条目枚举的架构；安装阶段依据载荷的字节。`package.archs` 不参与放行或拒绝
  （`type = "subos"` 的模板除外，它没有产物，现状如此）。
- **C3 版本**：一个请求串先归类为"具体键、别名、范围或前缀"之一，归类只做一次，所有解析点用同一个结果。
  具体键永远精确。写进声明的是解析后的具体版本。
- **C4 路径**：载荷规则只有一份定义，各执行点引用它。一个包解压出的文件在安装结束前只属于这个包。
- **C5 通道**：recipe 要让宿主知道的事以结构化的形式到达宿主，宿主负责展示、记录与执行；输出流只是日志。
  hook 里的一次调用是动作、询问、声明三种之一：动作没做到，hook 就失败（§9.6）。

本文只用四个词描述"系统告诉人的话"，实现里也只有这四种：

| 词 | 是什么 | 谁产生 | 默认输出里 |
|---|---|---|---|
| 结果（outcome） | 一步的四种结局之一 | 核心 | 总结的各段 |
| 发现（finding） | 一条规则对一个载荷的报告，带规则编号与文件 | 检查（安装时、doctor） | 一行计数；细节在 `-v`、日志与 doctor |
| 说明（notice） | 一个包对用户说的话 | recipe 的 `log.warn` / `log.error`，以及 xlings 自己（新版本） | 总结之后，按包列出，有上限 |
| warn | 日志级别 | 任何代码 | 不改变结论；不承载以上三种 |

现有的 `advisory`（架构未说明）并入"发现"，编号 `arch-unstated`。

## 5. 设计：CPU 架构

### 5.1 模型

| 概念 | 是什么 | 来源 |
|---|---|---|
| 宿主机器 | 客户端进程的 ABI | `platform::build_arch()`；仿真下的 x86_64 客户端用 x86_64 的载荷（规范现状） |
| 条目架构 | 一个版本条目说它提供哪些架构 | recipe |
| 载荷机器 | 文件的 `e_machine` 与位宽 | 字节 |

**架构的身份只有一份，在 libxpkg**：规范名、别名、`e_machine`、位宽。使用者：加载器（名字归一、
Scheme B 认哪些键）、`elfpatch` 的 `_target_abi`、xlings 的规则 M。今天加载器的 Scheme B 只认三个键，
`elfpatch` 另有一份按名字猜的表。

各模块自己的、**按规范名索引**的属性留在各自那里：标准 loader 路径在 `xlings.subos.std_loader`，
启动文件名与 qemu 机型在 `xlings.image`。它们不搬进 libxpkg：`modules/` 不依赖 libxpkg，
这些属性也不是 recipe 格式的一部分。一个单元测试保证 `platform::build_arch()` 能返回的每个值都在身份表里。

一个 home 只有一种宿主机器。想在 aarch64 的机器上用 x86_64 的包，运行 x86_64 的客户端（仿真下），
那个 home 里一切都是 x86_64 的。

### 5.2 `package.archs` 的含义（规范改写）

`archs` 是这个包在所有平台上提供的架构的并集，供搜索与站点使用。它不用于接受或拒绝某个条目。
索引检查保证它等于各平台条目架构的并集（§5.6 K3），不再手写漂移。

规范里两处"fail-closed"改为如实描述：枚举了架构的条目在计划阶段 fail-closed；
没有枚举的由安装阶段的规则 M 裁决。

### 5.3 条目怎样说明架构

不新增字段。已有的写法足够，且已发布的客户端都认识（E2、E6、E7）：

| 情形 | 写法 | 计划阶段 | 安装阶段 |
|---|---|---|---|
| 每种架构各有产物 | 按架构的资源表，或 URL 模板加按架构的 `sha256` | 宿主不在其中则拒绝 | 规则 M |
| 只有一种架构的产物 | 固定 URL，`sha256 = { x86_64 = "<h>" }` | 同上 | 规则 M |
| **与架构无关** | 固定 URL，同一个 `<h>` 列给每种架构：`sha256 = { x86_64 = "<h>", aarch64 = "<h>" }` | 列出的都支持 | 规则 M |
| 未说明 | 一个 URL、一个 `sha256` 字符串 | 通过；记一条发现 `arch-unstated` | 规则 M |
| 无产物 | 只有 hook | 通过 | 无可检查 |

- "与架构无关"不是一个新概念，而是一句可检验的话："这同一份字节是这些架构各自的产物"。
  规则 M 在每种列出的架构上各检查一次（发布时，§5.6 K2），带机器码的产物必然在其中一种上不过。
  新增一种宿主架构时这些条目要各加一个键；这是有意的：只声明测过的。
- "未说明"保留给第三方索引与 V1 recipe。xim-pkgindex 自己通过迁移与检查清零（§5.6）。
- **只有一种受支持架构的平台**（今天的 macOS 与 Windows）上，未说明的单产物条目就是那个平台的架构，
  不记发现，检查 K1 也不要求改写。某个平台增加第二种宿主架构时，K1 扩到那个平台。

### 5.4 规则 M（载荷的机器）

**对宿主开放的面**，一个已安装载荷里宿主会直接执行或加载的文件：

1. 这个包经 xvm 登记的每个程序所指向的文件；
2. 它导出的运行时库目录（`exports.runtime.libdirs`，未声明时按约定的 `lib64`、`lib`）里直接放着的共享库，不递归；
3. 它导出的 loader。

**规则**：面里的 ELF 文件，`e_machine` 与位宽等于宿主机器的。面以外的文件不裁决：交叉工具链的目标库、
sysroot、npm 的多架构预编译件、固件都在面以外。面以外的事实由 recipe 负责，规则 M 不替它担保。

- 实现：读取用已有的 `xlings.libs.elfread`（加上机器与位宽两个字段，它已经是进程内、只读文件头的读取器）；
  规则写在 `closure_check`，在 `config()` 返回、登记发布之前执行。
- 失败走安装器已有的回滚（2026.11.22.1 的"发布失败"分支），节点结果为 `Failed`，载荷不留、不登记。
  错误码 `E_FOREIGN_MACHINE`，给出文件、它的机器、宿主的机器，以及下一步（向索引报告，附地址）。
- `xlings self doctor`：对已安装的载荷按同一规则报告 `payload-foreign-machine`，修复提示是移除或重装，
  不自动处理。覆盖今天已经装错的机器。
- 只管 ELF。macOS 与 Windows 各只有一种受支持的架构，本轮不做 Mach-O 与 PE。
- libxpkg 的 `elfpatch` 继续跳过另一种机器的文件（那是打标签的正确行为），但把数量与原因交回（§9.3）。
- **没有开关。** 误报的修法在 recipe：不要把装着另一种机器文件的目录导出给宿主（那本身就是缺陷）。
  索引发布几分钟内生效，不需要客户端发版。

**变硬的门槛**：规则 M 在客户端里是硬的，所以索引必须先证明自己干净。做法是复用索引已有的周期性全量安装
（`ci-sweep.yml`）：加一个"被测客户端"的输入（安装脚本已支持从指定地址取某个版本）和一条 aarch64 的腿，
用 xlings PR 的构建把全部分片跑一遍。有误报就修面的定义或修 recipe，直到为零，xlings 才发布。
这是整个方案唯一的跨仓库先后依赖（§12）。

### 5.5 计划阶段：一次说全

解析器收集整个闭包里不支持目标的节点，一次报告。默认输出是结论与下一步：

```
E_UNSUPPORTED_TARGET: xim:mesa@25.0.7.3 cannot be installed on linux-aarch64:
  18 of its 22 packages have no linux-aarch64 artifact (libllvm, libdrm, libX11, ... -v lists all)
  next: the coverage table shows what is planned; request a package at <index issues>
```

`-v` 与 JSON 事件给出完整列表与依赖链。`arch-unstated` 的发现在计划里只占一行计数。

### 5.6 索引的迁移与检查

| 组（Linux 段） | 数量 | 做法 |
|---|---|---|
| 单产物、只声明 x86_64 | 89 | 机械改写：`sha256 = "<h>"` 改为 `sha256 = { x86_64 = "<h>" }`。字节不变，不动 `revision` |
| 单产物、声明多种架构 | 51 | 由迁移工具判断并改写（见下）：与架构无关的，同一个 `<h>` 列给每种架构；本机二进制的，上游有 aarch64 产物就补成各自的条目，没有就只列 x86_64 |
| 经 `source` 的单产物 | 19 | 同上一行 |
| 只有 hook、只声明 x86_64 | 13 | 没有产物，不需要改条目；`archs` 由 K3 校正 |

**迁移由工具完成，判断由字节给出**（`tools/arch-migrate`，与普查脚本放在一起；依据 §2.7）：

1. **读字节**（静态，在 CI 里下载产物后进行，不解包不执行）：列出归档里每个 ELF 的机器、是程序还是库、
   是不是 Linux 的。没有机器码 → 与架构无关，直接改写。只有一种机器的 Linux 程序 → 候选"只有这种架构"。
2. **在两种架构上各装一次，过规则 M**（即 §5.4 的全量安装，用被测客户端）：
   - 候选"只有 X"且在另一种架构上规则 M 不过 → 确认只有 X；
   - 静态定不了的（static-pie、只有库、别的系统的 ELF）由这一步定：两种架构上都过 → 与架构无关；只在一种上过 → 只有那一种。
3. **找另一种架构的产物**：对确认"只有 x86_64"的，把 URL 里的架构词替换后探测上游；存在就在 aarch64 的 runner 上
   下载、算 `sha256`、安装、过规则 M，通过则写成按架构的条目；不存在就只列 x86_64，覆盖表里显示为缺口。

工具输出两样东西：recipe 的改动（一个可评审的 diff），和每个包的证据表（读到了什么、两种架构上的结果、
探测到的 URL）。**留给人的只有一种情形**：静态看到"只有 X 的 Linux 程序"，而规则 M 在另一种架构上却通过了。
那是宿主程序放在面以外的包（例如 `android-ndk` 把编译器放在 `toolchains/…/linux-x86_64/` 下、不登记任何程序），
或者以 Linux 为目标的 sysroot；工具把它们列出来，预计是个位数，每个都是一眼能定的。

迁移之后同样的判断持续生效：K2 对每个新资产做第 1 步，全量安装做第 2 步。

检查（`xpkg_ci.py`，对改动的 recipe 生效；编号用 K，以免与 §12 里 libxpkg 的检查点 L 混淆）：

- **K1** Linux 段里有产物的条目必须枚举架构（§5.3 的前三种写法）；
- **K2** 资产的准入：条目把一个产物列给架构 X，就在 X 上对它过规则 M。发布时没有登记信息，
  面取静态的近似（顶层 `bin`、`sbin` 与导出的库目录）；精确的判断由全量安装承担；
- **K3** `archs` 等于各平台条目架构的并集（提供 `--fix`）；
- **K4** 自动更新写出的新版本条目沿用 recipe 已有的形态，机器人 PR 同样过 K1。

产出一份**架构覆盖表**（由普查脚本生成，进索引站点）：每个包在每种架构上是否可装，以及它的**整个闭包**
是否可装。它回答"补哪几个包能让最多的包在 aarch64 上可用"，也是 §5.5 的报错指向的地方。

### 5.7 生态的架构支持分级

| 级别 | 架构 | 含义 | 现状 |
|---|---|---|---|
| 一级 | Linux x86_64 | 客户端、Luban、全部索引包；每个 PR 的 CI | 是 |
| 二级 | Linux aarch64 | 客户端、Luban（除桌面）、工具链；原生 CI；索引包按覆盖表逐步补齐 | 客户端与基础包已有（330 个 recipe）；图形栈缺（本方案补 mesa 闭包） |
| 目标 | riscv64、loongarch64、arm 裸机等 | 只作为交叉编译的目标，没有原生客户端 | `linux-headers`、裸机 smoke |
| 单一 | macOS arm64、Windows x86_64 | 各一种架构 | 是 |

**新增一种宿主架构要动的地方**（写进 `docs/51-supported-platforms.md`）：libxpkg 的身份表一行；
`std_loader` 与 `image` 各自的属性一行；xlings 的发布构建与一条 CI 腿；`res-build` 的 runner 或交叉路径；
基础闭包（glibc、gcc-runtime、工具链）的资产；与架构无关的 recipe 各加一个键（脚本完成）。
覆盖表自动出现新的一列。本方案不新增宿主架构。

**一个包怎样得到新的架构**：贡献者提交的是构建数据（版本、来源、参数），不是字节；
构建在 CI 里、在目标架构上进行，发布由受保护的环境完成。#1442 这类请求的落点是 `tools/res/` 的一行。

### 5.8 mesa 的 aarch64

**目标**：`xlings install xim:mesa` 在 Linux aarch64 上装到一套能渲染的栈；在那之前得到 §5.5 的拒绝，
而不是 x86_64 的库。

**构建**

- 图形栈并入已有的 `res-build`：构建脚本从索引仓库的 `.agents/tools/graphics/` 移到 `tools/res/graphics/`
  （CI 不应依赖 agent 的工作目录），`tools/res/build.sh` 增加这一类资源，复用它的计划、按架构选 runner、
  产物自检、受保护的发布与 GitCode 补齐。不新建 workflow。
- 构建仍在 subos 里进行（这是这些脚本存在的理由：不让宿主的库与头文件漏进产物），subos 由 runner 上一个
  已发布的 xlings 建出，工具链来自索引。
- `tiers.sh`、`build-in-subos.sh`、`build-libllvm.sh`、`gen-recipes.py` 去掉写死的 `x86_64`：
  架构取自构建所在的 subos，输出名是 `<name>-<version>-linux-<arch>.tar.gz`。
- 先补构建输入：`cmake` 的 Linux aarch64 条目；`meson`、`xcb-proto`、`xtrans`、`wayland-protocols`
  按 §5.6 判断（脚本与数据是与架构无关的，其余在 aarch64 上构建）。
- 构建顺序即 `tiers.sh` 的顺序：T1（`xorgproto`、`libpciaccess`、`libdrm`、`libxshmfence`）→
  T2/T3（`libXau`、`libXdmcp`、`libxcb`、`libX11`、`libXext`、`libXfixes`、`libXxf86vm`、`expat`、`libffi`、
  `wayland`、`elfutils`、`libglvnd`）→ `libllvm` 20.1.7 → T5 `mesa`。
- 每个载荷出厂前过既有的两项检查（不含宿主路径、配置所用的输入不来自宿主）与规则 M。

**驱动集合**（已定，§15 第 3 项）

- x86_64 的集合是 `llvmpipe, softpipe, radeonsi, nouveau, zink, iris, d3d12, virgl` 加 Vulkan 的 `amd`。
  其中 `iris` 与 `d3d12` 带来 clc 链（`llvm-dev`、`spirv-tools`、`directx-headers` 等五个包）。
- aarch64 第一步：`llvmpipe, softpipe, virgl, zink, radeonsi, nouveau` 加 Vulkan 的 `amd`。
  去掉 `iris`（Intel 核显只在 x86 上）与 `d3d12`，于是不需要 clc 链，构建只多一个 `libllvm`。
- ARM SoC 的驱动（`freedreno`、`panfrost`、`v3d`、`etnaviv`、`lima`、`asahi` 及对应的 Vulkan 驱动）
  放第二步：其中有的要 clc 链（以 25.0.7 源码里 `with_clc` 的条件为准，实施时核对），
  而且 CI 的 runner 没有这些硬件，需要真机验证后再作为一次修订加入。
- 同一个版本键在两种架构上驱动集合不同，写在 recipe 的注释里。

**发布与 recipe**

- 资产加到 xlings-res 各包已有的 release 里（只增不换），再补齐 CN 镜像，按内容校验。
- 18 个 recipe 的条目变成按架构的形态：x86_64 的 URL 与 `sha256` 原样，新增 aarch64。
  版本键不变（`mesa` 仍是 25.0.7.3），`revision` 不动，已安装的 x86_64 系统不受影响。
- `libglvnd`（1.7.0.1）与 `expat`（2.6.2）即 #1442 点名的两个，包含在内。

**验证**

| 在哪里 | 验证什么 |
|---|---|
| `ubuntu-24.04-arm`（无 GPU） | 干净 home 安装 `xim:mesa`：规则 M 与闭包规则 D 无发现；EGL surfaceless 由 llvmpipe 渲染（`glprobe`）；在 subos 里链接 `libgbm` / `libEGL` 的程序能运行（#1442 的场景） |
| x86_64 | 迁移后的条目与迁移前装出同样的字节 |
| 真机 | GPU 路径由有 aarch64 硬件的一方验证，结果记在 #1442 |

**本方案不含**：`qemu-system-gl` 的 aarch64（Luban 机器的 3D）、桌面 edition 的 aarch64、
`nvidia-gl-host-link` 的 aarch64。

## 6. 设计：结果契约（xlings）

### 6.1 类型与汇总

- 核心里一个 `StepOutcome`（四种，带原因）；安装、移除、切换的每个节点各有一个。
  它与 `install_state` 的三种状态是两回事：后者回答"现在装着没有"，前者回答"这一步做了什么"。
- 核心返回结构化的结果，前端渲染（现行规则）：命令行的总结、`--agent` 的事件、Luban 的界面读同一份。
- 命令的总结由节点结果生成：已完成、已跳过（附原因）、被拒绝、失败，各自成段；退出码按 C1。

### 6.2 各处的改法

| 位置 | 现在 | 改为 |
|---|---|---|
| `subos runtime`（#607 第 2 条） | 激活失败 warn，随后写声明 | 激活的结果是 `Refused` 时不写声明，命令失败，输出只有拒绝与提示 |
| 安装后的激活失败（`commands.cpp:1298`） | warn | 节点结果记为"已安装、未激活"，总结里说明；用户写了 `--use` 时命令失败（§15 第 5 项） |
| recipe 读不了、归档任务失败（`installer.cpp:2868`、`:2913`） | warn 后跳过 | `Failed` |
| 同一计划里被另一个节点"满足"（#739） | `install()` 不运行，记为已安装 | 归类按 §7.1；"已安装"的记录只由产生了载荷的那一步写 |
| `remove --older-than`（#708） | 目录不存在则内部错误中止；状态读不了则跳过 | 目录不存在：这条"已知项目"是派生状态，移除记录并在总结里说明，继续；状态读不了：停止，指出文件与修法 |
| 闭包规则 D、A、E | 逐条 warn | 仍不拒绝（§15 第 6 项），改为"发现"：默认输出一行计数，细节进 `-v`、日志与安装记录；规则 M 是硬的 |
| `elfpatch` 的结果 | 三个数，失败不可见 | 按 §9.3 的结构化结果：缺工具且有文件要补是 `Failed`，附怎么得到这个工具 |
| recipe 的 `spec` 高于客户端的上限（E14） | 报错并跳过这个包，命令的退出码是 0 | 节点结果是 `Refused`，命令失败 |
| hook 报告成功而载荷为空（E16） | 写一个标记文件，记为已安装 | 这次 hook 有失败的动作调用时是 `Failed`（§9.6）；没有时照旧（载荷在别处的包） |

### 6.3 守住它

`tools/lint_warn_continue.sh`：安装、移除、切换这几条流程的源文件里，`log::warn` 之后紧跟 `continue` 或
直接落空的，必须带 `// warn-ok: <为什么不改变结论>`。与仓库里其它 lint 同样在 CI 里跑。

## 7. 设计：版本契约

### 7.1 归类一次

`VersionRequest { kind: Exact | Alias | Range, text }`，由 catalog 对着索引的条目表给出：

- 串等于一个具体键 → `Exact`；
- 串等于一个带 `ref` 的键 → `Alias`；
- 其余 → `Range`（前缀是范围的一种，行为与今天相同）。

"幂等"的快速路径、`xvm::db` 的匹配、同一计划里"是否已被满足"的判断都接收这个归类。`Exact` 时只接受相同的版本；
想要"2.44 系列里任何一个"的写范围（`~2.44`）。#739 的两种情形与 #607 第 1 条由此解决。

### 7.2 声明里写什么，已有的声明怎么办

- 写解析后的具体版本；原始请求另存一个字段，供 `xlings update` 与 doctor 解释"为什么是这个版本"。
  旧客户端忽略多出的字段。
- **已有的 home**：一个声明写着 store 里没有的版本、而视图实际指向另一个具体版本（E4 留下的状态），
  由 `xlings.home.format` 的一个步骤改写为视图所指的那个版本：只做一次、幂等、有记录。
  它不提高布局号：旧客户端不会把它改回去。声明的版本确实装着的，不动。

### 7.3 别名（#709）

- 规范：平台表里带 `ref` 的非版本键是别名。`latest` 是其中一个；`lts`、`stable` 等由 recipe 定义，
  名字是小写字母开头、不能被解析成版本的串。
- 客户端：范围匹配排除所有别名键（今天只特判 `latest`）；安装记录所用的别名；`xlings update <pkg>`
  沿记录的别名移动，没有记录的沿 `latest`。
- 索引：guard 放行别名键。旧客户端今天已经能跟随 `ref`，所以别名对它们可用，只是不会被记住。
- 首个使用者：`node` 的 `lts`（§15 第 7 项）。

### 7.4 flavor（#740 §2）

`gcc-ar`、`gcc-nm`、`gcc-ranlib` 是带着各自 gcc 的 LTO 插件的包装，与另一个版本的 `gcc` 混用是真实的风险，
所以 doctor 的一致性检查是对的。决定：**flavor 作为同一组名字整体切换**。改动只在索引：musl flavor 把这三个
名字也发布出来，`revision` 加一。客户端不改。

## 8. 设计：路径契约

### 8.1 三种路径

| 种类 | 例子 | 谁可以写进文件 |
|---|---|---|
| 环境的运行时与载荷直连 | `<home>/abi/<family>/…`、`<store>/<pkg>/<ver>/lib` | 安装时由 `elfpatch` 写进载荷自己的 ELF、`specs` |
| subos 视图 | `<subos>/lib` | 只出现在登记（别名、环境）与用户自己构建的产物里，不进共享载荷 |
| 构建机路径 | 构建时的临时目录、另一个 home | 任何已发布的资产里都不应出现 |

### 8.2 一份规则

`docs/specs/payload-rules.md`（规范，英文），列出规则及其编号、适用的文件、在每个执行点的效力：

| 规则 | 内容 | 发布（索引 CI） | 安装（xlings） | 构建（mcpp、`ld`） | doctor |
|---|---|---|---|---|---|
| A | glibc 载荷不低于宿主记录的 glibc | — | 发现（现状是 warn） | 拒绝（mcpp 现状） | 报告 |
| B | loader 与 libc 来自同一载荷 | 加入 | 拒绝（现状） | 拒绝（mcpp 现状） | 报告 |
| D | 每个 `DT_NEEDED` 有提供者 | 拒绝（现状，D1、D2） | 发现（现状是 warn） | 不适用 | 报告 |
| E | 可执行文件的搜索路径用 `DT_RPATH` | 加入，先报告 | 发现（现状是 warn） | 由 §8.3 保证 | 报告 |
| **M** | 对宿主开放的面是宿主的机器 | 拒绝（新，K2） | **拒绝（新）** | 不适用 | 报告 |
| **P** | `RPATH`/`RUNPATH`、`.pc`、`.la`、编译器的 sysroot 里没有构建机路径 | 拒绝（新；xim-pkgindex#1341 的范围） | 发现（新） | `mcpp pack` 去除（现状） | 报告 |

- 规则 P 在安装侧只报告：今天已发布的资产里有违反的（`glibc@2.44` 的 `DT_RUNPATH`、gcc 16.1.0 的 sysroot）。
  它覆盖 #740 §3 与已并入的 #458（subos 路径烙在共享载荷的 `specs` 里是 P 的一种）。
- **实现两份，规范一份。** xlings 的实现在 `closure_check`（读取用 `xlings.libs.elfread`）；
  索引 CI 经 `xlings` 的一个 `__` 开头的内部命令调用它，不再维护 shell 版本。mcpp 有自己的实现（规则 A、B）。
  规范带一组夹具（由 testkit 已有的 ELF 生成器产出的小文件与期望的发现），两份实现都跑。
- **doctor 不重扫。** 安装时的发现写进这个载荷的安装记录；`self doctor` 读记录，`--deep` 才重新扫描。
  规则 M 只读面里文件的文件头，doctor 里总是现算。

### 8.3 构建侧（#532、#540）

2026-08-11 设计的第 2 步（包装器对每次链接追加 `-rpath <farm>`）不照搬：实测它会让经过这个 `ld` 的宿主编译器
产出启动即崩的程序（§2.8 的 E12）。那份设计把"谁不想要路径"交给各家去声明；实际上不该得到路径的是一大类，
而且它们不知道自己经过了这个包装器。改为由包装器自己判断。

**规则**：farm 只写进**运行在环境运行时上的可执行文件**。包装器从链接参数里读 `-dynamic-linker`：

| 这次链接 | 包装器追加 |
|---|---|
| 没有 `-dynamic-linker`（静态、`-shared`、`-r`） | 只有现有的 `-rpath-link` |
| loader 是系统路径（`/lib`、`/lib64`、`/usr/lib`、`/usr/lib64` 等之下） | 只有现有的 `-rpath-link` |
| loader 不是系统路径（即环境的 glibc 或 musl） | `-rpath-link`、`--disable-new-dtags`、`-rpath "$XLINGS_SUBOS_LIB"` |
| 同上，且声明了 `XLINGS_SUBOS_LD_PATHS=0` | `-rpath-link`、`--disable-new-dtags`（标签在，路径不加） |

- 这个判断与闭包规则用的是同一个：一个产物是不是"我们的"，看它的 loader 在不在系统路径上。
  它不依赖是哪个 home，所以对借用的载荷、项目自己的 home 都成立；Luban 系统里的 gcc 用系统路径的 loader，
  包装器不加东西，那里的库由系统的根自己解析。
- 追加的 `-rpath` 排在 `specs` 写的运行时路径之后，所以 libc 与 C++ 运行时仍来自各自的载荷，
  farm 只补上其余的库（原型第 9 行）。farm 里那个 `libc.so.6` 不会被选中。
- 共享库不加：库上的传递标签已测得有害。
- `XLINGS_SUBOS_LD_PATHS=0` 保留给 #540：mcpp 的链接经过 SubOS 的 gcc，会被判为"我们的"；它要标签、路径自己管，
  在链接步骤与 `mcpp pack` 里声明这个变量（mcpp 的 PR）。
- binutils 的 `revision` 加一。Windows 不适用（包装器是 POSIX 的），写明。
- 判据就是 §2.8 原型的九行；其中第 4、5 行（宿主编译器不被改动）是这次改写新增的。
  第 8 行改正了旧设计的一条判据：声明退出时产物不含**包装器追加的**路径；`specs` 写的运行时路径照旧，
  由 `mcpp pack` 去除。
- 实现放在 binutils 的 recipe 里（它生成这个包装器）。原型用的脚本约二十行 POSIX shell，没有外部命令。

### 8.4 解压目录（#736）

- 每个节点解压到自己的目录：`<data>/runtimedir/<namespace>-<name>-<version>/`，归档以硬链接的形式也放在里面
  （不能硬链接的文件系统上复制），`install_file()` 指向它。
- 这样两种写法都继续成立：带外层目录的归档，`install_file():replace(".tar.gz", "")` 仍是解出的目录；
  扁平归档，`path.directory(install_file())` 里只有这个包的文件。recipe 不需要改。
- 节点结束（成功或失败）后这个目录被删除，不给下一个包留东西。
- 门槛：索引里约二十个提到扁平布局的 recipe 用这个构建装一遍（在 §5.4 的全量安装里一并完成）。
- 这一节服务于 `spec` 为 1、2 的 recipe。`spec = "3"` 的包不经过归档旁边的目录（§9.7），没有这个问题。

## 9. 设计：hook 通道（libxpkg 与 xlings）

### 9.1 结果里多出的两样东西

- `HookResult` 增加 `notices`：这次 hook 里每一次 `log.warn` 与 `log.error` 的文本，按顺序。
  输出流照旧写（日志不变）。边界进程的协议带上它，协议号加一。
- `apply_elfpatch_auto` 返回结构化的结果（§9.3），其中有 `outcome`（`Done`、`Skipped`、`Failed`）与原因。
- **结果这一侧不新增 recipe 可见的接口。** 布尔返回值的映射不变；今天产生"跳过"的只有 `elfpatch`，
  等有 recipe 需要声明"跳过"时再加调用。recipe 可见的新名字都在 §9.6。
- `HookResult` 再增加一项：兼容层里失败的动作调用（§9.6），同样过边界协议。

### 9.2 说明（#721、#425）

- **语义**：`log.warn` 与 `log.error` 是 recipe 对用户说的话；`log.info` 在 `-v` 下可见；`log.debug` 只进日志。
  recipe 的作者本来就是这样用的（§2.2 的抽样），#721 的标题说的也是这件事。
- **展示**：核心把各节点的说明放进结果；命令行在总结之后按包列出，每个包有行数上限，超出的指向日志。
  `--agent` 与接口收到事件。全文写进 `<home>/logs/`。
- **日志的直接修复**：没有 hook 日志文件的三种情形下，xlings 把 `HookResult::output` 打印出来。
  这一处改动就让 E5 的后两种不再丢行。
- **旧 recipe 立即受益**：索引里 122 处 `log.warn` 不需要改。少数把 `log.warn` 用作进度的，
  在索引 PR 里改成 `log.info`。
- **新版本提示（#425）** 用同一个位置，由 xlings 自己产生：索引同步后若 `xim:xlings` 的 `latest` 高于当前版本，
  命令结束时说一行，带 `xlings self update`。每天至多一次；非终端、`--agent` 与 JSON 模式下不打印，只作为事件；
  可用配置关闭。

### 9.3 `elfpatch`（#741 UP-L1）

- 先扫描，后查工具。有文件需要补而工具不存在：结果是失败，原因写明缺哪个工具。没有文件要补：照常通过。
- 结构化结果：`scanned`、`patched`、`failed`、`skipped_foreign`（另一种机器）、`skipped_static`、`tool_missing`。
- 怎么得到工具由 xlings 说，出处是工具表：`xlings.subos.tools` 增加 `patchelf`（T1，索引里的包）与
  `install_name_tool`（T0，macOS 的命令行工具）两行，提示从表里来。
- recipe 显式调用的打标签函数（`set_interpreter`、`set_rpath`、`patch_elf_loader_rpath`）同理：
  指定了文件而工具不存在，就是失败，不是一张空的结果表（§2.8 的第二处叠加）。
- `elfpatch.lua` 里属于检查的部分改为只返回事实，裁决移到 xlings；打标签的部分不动。
  具体函数的清单在 libxpkg PR 的第一个检查点里列出。

### 9.4 不采用"外部安装器"包形态（#740 §5）

在安装时运行另一个安装器的 recipe，它的字节不受索引的 `sha256` 约束，移除也触及索引不拥有的状态。
规范里写明这不是一种受支持的形态；现有的 `rust` 按 xim-pkgindex#569 改成 tarball 包。本方案不改 `rust`。

### 9.5 libxpkg 的版本

- libxpkg 发布 0.0.68：打 tag，在 mcpp-index 里登记（它在那里停在 0.0.61）。
- xlings 的 `main` 用版本号依赖它。PR 上联调时可以临时用 `git rev`；`release.yml` 拒绝带 git 依赖的发布。
- 兼容规则：已有的名字不改含义。这一版给模块加了名字、加了 `spec = "3"` 的环境（§9.6）；
  `spec` 为 1、2 的 recipe 的环境不变，例外是 `fs.symlink` 与 `fs.remove` 两个函数（§9.6"兼容层"）。
- 索引用上新名字之后要提高最低客户端（§9.6"旧客户端"）。

### 9.6 hook 的接口：一份模块 API，`spec = "3"` 去掉兼容层

**问题**（§2.9、R6）：hook 的环境是三层叠出来的，同一件事有两三种写法，失败怎么报由名字碰巧决定。
`os.mv` 是把解出的文件搬进安装目录的主要方式；它失败时 hook 照样返回成功，空载荷被记为已安装（E16）。
逐个把老名字改成"失败即报错"能堵住这一处，但留下两个问题：同一段 recipe 在新旧客户端上含义不同；
两套词汇与那个桩还在。所以这一节不改任何老名字的含义，而是给出一份接口，并让 recipe 自己声明它只用这一份。

**五条规则**

1. **一次调用是三种之一。**
   - 动作：改变机器上的东西。它说明结束时成立的事；不成立就报错，hook 失败。没有需要检查的返回值。
   - 询问：不改变任何东西，返回答案。答不了（读不了、不是目录）时报错；"没有"是一个答案，不是错误。
   - 声明：把一件事交给宿主做（`xvm.add`、`subos.env`、`system.admin_step`、`log.warn` 的说明）。写错了当场报错；做不做成是宿主那一步的结果。
2. **容忍失败只有一种写法**：Lua 自己的 `pcall`。没有带 `try` 前缀的名字。它出现在 recipe 的文字里，读的人与检查都看得见。
3. **环境是标准 Lua 的纯计算部分，加上 `import` 进来的模块。** libxpkg 不再往 `os`、`io` 与全局里补名字；
   碰文件、起进程的事只经过模块。
4. **模块只有一份**，`spec` 为 1、2、3 的 recipe 用的是同一份实现；索引的 `libs/` 只写一遍。
5. **recipe 用 `spec` 声明它的环境。** `spec = "3"` 的环境里没有兼容层，用到老名字就报错，不靠检查脚本来保证。

**两种环境**

| | `spec` 为 1、2 | `spec = "3"` |
|---|---|---|
| 模块 | 同一份 | 同一份 |
| 兼容层（下文） | 有，原样冻结 | 没有 |
| Lua 的标准库 | 完整 | 基础函数、`string`、`table`、`math`、`utf8`，以及 `os.time`、`os.date`、`os.clock`、`os.getenv`；没有 `io`，没有 `os` 的其余部分 |
| `import` 一个不认识的模块 | 返回桩 | 报错，并说明这个 recipe 需要更新的 xlings |
| `string` 上的 `split`、`trim`、`replace` | 有 | 有（纯函数，没有失败的情形） |
| 文件的顶层（读 `package` 表时也会执行） | 对模块的调用被桩吞掉 | 只能有 `package` 表、`import` 与函数定义；顶层调用模块的函数是加载错误 |
| hook 的返回值、`package` 表的规则 | 不变 | 与 `spec = "2"` 相同 |

- 执行器在准备环境之前就知道 `spec`：xlings 的目录里已有，随上下文给它；单独使用 libxpkg 时由加载器读出。
- 加载器读 `package` 表的方式不变；它记下顶层对模块函数的调用，`spec = "3"` 的 recipe 有这样的调用就报错。
- 新环境不是安全边界：hook 仍然可以经 `system.exec` 运行任何程序。约束 hook 能碰什么是沙箱（`xlings.confine`）的事；
  这里保证的只是"每一次调用的结果都有人知道"。

**模块**（✦ 是新增的名字，其余是现有的）

| 模块 | 询问 | 动作 | 声明 |
|---|---|---|---|
| `fs` | `exists`、`is_file`、`is_dir` ✦、`is_symlink`、`readlink`、`read` ✦、`list` ✦ | `mkdir` ✦、`remove`、`move` ✦、`copy` ✦、`symlink`、`write` ✦、`chmod` ✦、`extract` ✦（§9.7） | |
| `system` | `run` ✦、`rundir`、`xpkgdir`、`bindir`、`subos_sysrootdir`、`xpkg_args` | `exec` | `admin_step` |
| `pkginfo` | `name`、`version`、`install_dir`、`install_file`、`platform` ✦、`arch` ✦、依赖的各项查询 | | |
| `path` ✦（现在是全局表） | `join`、`filename`、`directory`、`is_absolute` | | |
| `log` | | | `debug`、`info`、`warn`、`error`（§9.2） |
| `xvm`、`subos`、`pkgmanager` | `xvm.has`、`xvm.info` | | 其余各项 |
| `elfpatch`、`json`、`base64` | 不变 | 不变（`elfpatch` 按 §9.3） | |

`fs` 的动作各自说明结束时成立的事：

| 调用 | 结束时 | 说明 |
|---|---|---|
| `fs.mkdir(p)` | `p` 是目录 | 上级一并建出；已经存在不是失败 |
| `fs.remove(p)` | `p` 不存在 | 文件、链接或整个目录；本来就不存在不是失败 |
| `fs.move(src, dst)` | `dst` 是原来的 `src`，`src` 不存在 | `dst` 是最终路径，不是"放进去的目录"；`dst` 已存在是错误，除非写 `{ replace = true }`；跨设备时复制后删除 |
| `fs.copy(src, dst)` | `dst` 是 `src` 的副本 | 目录递归，链接保持为链接；`dst` 的规则同上 |
| `fs.symlink(target, link)` | `link` 是指向 `target` 的符号链接 | |
| `fs.write(p, text)` | `p` 的内容是 `text` | 上级一并建出 |
| `fs.chmod(p, mode)` | `p` 的权限是 `mode`（`"755"` 或 `"+x"`） | Windows 上不做事 |

- 错误里有动作名、路径、系统给出的原因，以及 recipe 里的行号。
- `fs.list(dir)` 返回按名字排序的 `{ name, path, type }`；`dir` 不是目录时报错。`fs.read(p)` 读不了时报错。
  `exists` 与 `is_*` 不报错。
- 旧的 `os.mv(src, dst)` 在 `dst` 是已有目录时把 `src` 放进去，否则改名：结果取决于调用时磁盘上有什么。
  `fs.move` 只有一种含义；要放进目录就写出最终路径。
- `system.exec(cmd, opt)`：命令必须成功，否则报错。`opt` 有 `cwd`、`env`（加进子进程的环境）、`retry`、`tty`、
  `capture`（为真时返回标准输出）。取代 `os.cd`、`os.setenv` 这两个改全局状态的名字。
- `system.run(cmd, opt)`：询问"这条命令的结果是什么"，返回 `{ ok, code, out }`，命令失败不报错。
  用于探测（`<程序> --version` 能不能跑）。
- **`cmd` 有两种写法**，`exec` 与 `run` 相同：
  - 列表：`system.exec({ "tar", "-xf", archive, "-C", dir })`。第一个元素是程序，其余每个元素是一个参数，
    不经过 shell，不需要引号。程序按 PATH 查找。这是规范推荐的写法，与 `system.admin_step` 的写法一致。
  - 字符串：一行 shell（`sh -c` 或 `cmd /c`），用在确实需要管道、重定向的地方。
  - 索引里 hook 运行的命令约九成不含 shell 语法（§2.9）。引号是 recipe 里跨平台缺陷的来源之一
    （libxpkg#52；索引为此有 `winpath` 这个 lib 与一条引号检查）；列表形式下这类问题不存在。
  - 进程层（`xpkg-shell.cppm`）增加按参数列表启动的入口：POSIX 上直接 `posix_spawnp`；Windows 上按系统的规则
    把列表拼成命令行。Windows 上程序是 `.bat` 或 `.cmd` 时列表形式报错并提示改用字符串：
    批处理的参数由 `cmd` 再解析一遍，没有安全的通用转义。
- `pkginfo.platform()` 与 `pkginfo.arch()`：这次安装所用的平台与架构，即 §5.1 的宿主机器。
  它是这次安装的一个事实，由 xlings 给出，不是 hook 对运行环境的又一次探测。取代 `os.host()`、`is_host()`、`os.arch()`。

**兼容层**

- 内容：补进 `os`、`io` 的名字，全局的 `path`、`format`、`raise`、`is_host`、`try`、`cprint`，模块里的旧名
  （`fs.mkdir_p`、`fs.remove_all`、`fs.copy_file`、`fs.is_directory`、`fs.files`、`fs.dirs`、`fs.entries`、
  `system.run_in_script`、`system.unix_api`、整个 `utils`），以及返回桩的 `import`。
- 它是一个文件（`prelude.lua` 改名为 `compat.lua`）加上 `register_os_funcs` 里对应的函数，行为原样冻结，不再加名字。
  文件里说明每个名字做什么；关于这些名字出处的注释删掉（`prelude.lua` 8 处、`json.lua` 1 处），
  libxpkg 的 README、`build.mcpp` 与两份 skill 里过时的构建说明一并更正。
- 为什么留着：已安装的包的载荷里存着安装时的 recipe（`.xpkg.lua`）；第三方索引与本地 recipe 也不由我们迁移。
  不设删除日期，保留它的成本就是这一个文件。
- **对旧 recipe 只有两处变化**：
  1. `fs.symlink` 与 `fs.remove` 在两种环境里是同一个函数，失败时报错，`fs.remove` 删除整个目录。
     索引里共 4 处调用，都在 `libs/`，在 P10 里逐处看。
  2. 兼容层的动作（`os.mv`、`os.cp`、`os.mkdir`、`os.exec`）失败时记一条（函数、参数、原因），随 `HookResult` 交回；
     行为不变。xlings 把它列为发现 `hook-call-failed`。

**旧 recipe 立即得到的一条硬规则**（xlings 一侧，不动 Lua 的语义）：hook 报告成功，这次 hook 里有失败的动作调用，
并且安装目录在 xlings 的补救（把解压出的内容搬进去，§2.9）之后仍然是空的，这一步就是 `Failed`，
错误里列出那几次调用。它正好是 E16。以全量安装零误报为门槛，与规则 M 同一次。

**旧客户端**

- 索引的 `libs/` 改用新名字之后，旧客户端运行任何用到 `libs/` 的 recipe 都会出错，所以含这次改动的索引快照
  把最低客户端提高到带新接口的那一版（下称 N）。更早的客户端继续用上一个快照，并被告知原因（已有机制，§2.9）。
- 单个 `spec = "3"` 的包到了旧客户端手里（本地 recipe、第三方索引、钉住的快照）：被逐包拒绝并提示更新（E14）。
- **`xlings` 自己的 recipe 不迁移**，保持每个旧客户端都读得懂的写法：`self update` 读的是最新快照。
  这是索引里唯一的长期例外，由检查按名字放行。
- 索引下限保证了 N，`spec = "3"` 的 recipe 里对 N 之前的能力的探针（`if xvm.files then … else <旧路径> end`，索引里 11 处）
  连同旧路径一起删掉。不认识的模块在新环境里报错，"新模块要用 `type()` 探测"这条规则对 `spec = "3"` 不再需要。

**迁移**（索引；`libs/` 15 个文件 3786 行，recipe 602 个）

1. `libs/` 先迁：它们被新旧 recipe 共同使用，只能用模块里的名字。
2. 迁移工具按下表改写，改不了的列出来由人处理。工具放在 libxpkg（`tools/`），读接口清单里的对照（§9.9），
   子索引与第三方索引用同一个。
3. recipe 分批迁移，每批一个 commit，门槛是全量安装（两种架构的 Linux、Windows、macOS，被测客户端）通过。
   全量安装装不到的 recipe 留在旧写法，列在覆盖表里（与架构覆盖表同一张，多一列）。
4. 一个 recipe 只动一次：改 `spec`、换名字、删掉落地用的那段 hook（§9.7）、按 §5.3 枚举架构，在同一次改动里完成，装一次验证。
5. 模板与规范改为 `spec = "3"`；新增的 recipe 必须是 `spec = "3"`，由检查器保证（§9.9）。

| 旧 | 新 | 工具 |
|---|---|---|
| `install()` 里的 `os.tryrm(安装目录)` 加 `os.mv(解压目录, 安装目录)`，以及推出解压目录名的那几行 | 删掉；条目写 `strip`（§9.7） | 认得出固定写法的直接改写（171 个只做这件事的 hook 整个删掉或只剩断言）；其余由人看 |
| `os.isfile`、`os.isdir`、`fs.is_directory` | `fs.is_file`、`fs.is_dir` | 直接改写 |
| `os.tryrm`、`fs.remove_all` | `fs.remove` | 直接改写 |
| `os.mkdir`、`fs.mkdir_p` | `fs.mkdir` | 直接改写 |
| 其它位置的 `os.mv`、`os.trymv`、`os.cp` | `fs.move`、`fs.copy` | 直接改写；`dst` 是已有目录的那些会在全量安装里报"目标已存在"，逐个改成最终路径 |
| `io.readfile`、`io.writefile` | `fs.read`、`fs.write` | 直接改写 |
| `os.dirs`、`fs.files`、`fs.dirs`、`fs.entries`、`io.popen("ls …")` | `fs.list` | 前四个直接改写；`io.popen` 由人看 |
| `system.exec("chmod …")` | `fs.chmod` | 直接改写 |
| 运行 `tar`、`unzip` 的命令（39 处） | `fs.extract`（§9.7） | 认得出参数的直接改写 |
| `os.exec`、`os.execute` | 当语句用的是 `system.exec`；用了返回值的是 `system.run` | 直接改写 |
| `os.iorun`、`io.popen` | `system.exec(…, { capture = true })`；包在 `try` 里的是 `system.run` | 部分；其余由人看 |
| 不含 shell 语法的命令字符串 | 列表形式 | 字面量拼成的直接改写；其余保持字符串 |
| `os.host()`、`is_host(x)`、`os.arch()` | `pkginfo.platform()`、`pkginfo.arch()` | 直接改写 |
| `path.*`（全局） | 加一行 `import("xim.libxpkg.path")` | 直接改写 |
| `format`、`raise`、`cprint` | `string.format`、`error`、`log.info` | 直接改写 |
| `try { f, catch { g } }`（72 处） | `pcall` | 由人看 |
| `os.cd`、`os.setenv`（10 处） | `exec` 的 `cwd`、`env` | 由人看 |
| `system.run_in_script(…, true)`、recipe 里的 `sudo` | `system.admin_step` | 由人看 |
| `utils.*`（7 个 recipe） | 没有对应的模块；参数解析的二十行并进索引的一个 lib | 由人看 |

- 改写后含义变了的地方不会静默：以前失败后继续的调用现在报错。这正是迁移要暴露的东西，每一处要么是藏着的缺陷，
  要么是有意的容错（改成 `pcall`）。
- 迁移不改变 recipe 做的事，不提高 `revision`；改了行为的那几个（修缺陷的）各自提高。
- 迁移工具还没有写，能自动改写的比例未知。**先做一次探路**：libxpkg 的 draft PR 里实现 L5 到 L8，
  取十个有代表性的 recipe（只落地的、带 `chmod` 的、带 `tar` 的、Windows 分支多的、用 `try` 的）手工迁移并在 CI 里安装，
  用结果校正上表与规则里最可能要调的两条（`fs.move` 的目标规则、`strip` 的严格程度），再写工具。
- **探路的结果**（2026-11-27，索引 draft PR #1477，xlings PR #742 的构建）：cmake、dash、fish、gh、go、kotlin、
  kubectl、make、shfmt、zig 十个 recipe 迁到 `spec = "3"`，在 Linux x86_64 上安装、运行、移除通过。两条规则都不用调：
  带顶层目录的归档都是一层（`strip = 1`），扁平的（fish）与不是归档的下载（kubectl、shfmt）是 0；`fs.move` 只有 shfmt
  用到，把落下的上游文件名改成程序名，写最终路径是自然的写法。改写的量是减少：十个 recipe 增 132 行、删 279 行，
  install hook 里的搬运、`tar`、对工作目录的依赖与 kotlin 里"两种落地方式都要找一遍"的代码全部消失。
  gh 顶层按 `os.arch()` 拼 URL 的代码由条目的 `arch_alias` 取代；fish 顶层删条目的代码由 `sha256 = { x86_64 = … }` 取代。
  其余平台与 aarch64 的结果在 #1477 的 CI 上（§19.6）。

**基线的夹具**：`tests/host-matrix/scenarios/hookrt.sh` 的修复（X0）不等这一节：夹具改用 `system.exec`、把 `xim:patchelf`
声明为构建依赖并检查 `set_interpreter` 确实改了一个文件，失败时把 hook 的日志打进 CI 的输出。

### 9.7 落地：宿主放好，hook 只做调整

**问题**（§2.9）：588 个 install hook 里 171 个只做"找到解压出的目录，搬进安装目录"；89 个 recipe 用改归档文件名的办法
推出那个目录叫什么。这是同一个约定被读的一方各自实现了一遍。xlings 自己就会解压，没有 hook 的包今天就是它放好的；
有 hook 的包却要 hook 来搬，搬不成时再由 xlings 在 hook 之后补救。

**规则**（`spec = "3"`）

1. **`install()` 运行之前，下载的内容已经在 `pkginfo.install_dir()` 里。** `install()` 只做调整（改名、改权限、
   生成包装脚本、打补丁），可以不写。没有"归档旁边的解压目录"这个概念。
2. **布局由条目声明。** `strip = n`：去掉每个成员路径的前 n 段。默认是 0，即归档自己的布局，与今天没有 hook 的包相同。
   - 某个成员不足 n 段是错误（声明与归档不符），不是悄悄丢掉那个文件。
   - 写在版本条目里；一个平台的各个版本都一样时写在平台段（与 `source` 的位置规则相同）。
3. **不是归档的下载**（单个可执行文件、安装器）原样放进安装目录，名字不变。
4. **`fs.extract(archive, dir, { strip = n })`** 是同一个解压器，作为动作给 hook 用：归档里套着归档的（`.deb` 里的
   `data.tar`）、要解到别处的。`pkginfo.install_file()` 仍然给出下载文件的路径，只读。
5. 解压先进这次安装私有的目录，再放进安装目录，就是今天没有 hook 的包走的那段代码。
   `spec = "3"` 的包因此没有 #736 的问题；§8.4 的每节点解压目录服务于 `spec` 为 1、2 的 recipe。
6. `install()` 失败时这一步是 `Failed`，安装目录按现有的失败处理清掉。hook 之后的补救（§2.9）对 `spec = "3"` 不再需要，也不执行。

**怎么实现**

- libxpkg 不带解压的实现。`ExecutionContext` 增加一个宿主给出的端口（一个函数）；`fs.extract` 调用它，
  没有给端口时报错。hook 在边界进程里运行时，那个进程就是 xlings，端口同样可用。
- `strip` 由加载器读出，随 `resolve_resource` 的结果交给 xlings；xlings 的解压器增加"去掉前 n 段"这一个选项。
- xlings 对 `spec = "3"` 的包把"解压并放好"从 hook 之后的补救挪到 hook 之前。

**得到什么**

- E16 那一类问题对 `spec = "3"` 不可能发生：没有搬运这一步。
- 171 个 hook 整个删掉或只剩断言；89 处猜目录名的代码删掉；`install()` 里的 `os.tryrm` 与 `os.mv`（461 次、397 次）大部分消失。
  迁移的量因此是减少而不是增加。
- hook 里不再需要宿主上的 `tar` 与 `unzip`（39 处）；Windows 上不必再绝对路径地去找系统的 `tar.exe`（`libs/payload.lua`）。
- `libs/payload.lua` 的 `unpack` 不再需要；`require` 与 `present`（断言落下了哪些文件）保留。

### 9.8 索引的模板：声明代替构建脚本

**问题**（§2.9）：三个子索引各有一份内容相同的构建脚本，只为把一个模板接到每个 recipe 后面。为了它：libxpkg 维护第三套环境；
脚本为两种运行时逐个探测函数；每次构建改写检出的文件，下一次构建前再用 `git checkout` 恢复。

**设计**

- `index-compat.json` 增加一项：`"recipes": { "template": "template.lua" }`。这个文件已经是"关于整个索引的声明"
  （最低客户端、保留的命名空间），并且随索引的制品与指针走。
- 有这项声明时，加载器把 `pkgs/` 下的每个文件读作"文件的内容，后面接模板的内容"，在内存里完成；磁盘上的文件不动，
  也就不需要恢复。`spec` 由文件里的 `package` 表给出，对拼接后的整体生效。
- 有声明时不运行 `pkgindex-build.lua`。旧客户端不认识这项声明，照旧运行脚本，所以两者在过渡期并存；
  子索引把自己的最低客户端提高到 N 之后删掉脚本。
- 构建脚本的环境（`register_build_sandbox`）冻结，不加名字；它与兼容层是同一种处理。

官方索引没有模板，不受影响。三个子索引各一个 PR，在 N 发布之后，不挡发布（§12）。它们的模板用的是兼容层的名字，
可以在同一个 PR 里迁到 `spec = "3"`，也可以以后再迁。

### 9.9 xpkg 的规范化

**问题**（§2.9）：一种格式，两个仓库里三份文档、三种叫法；hook 的接口不在任何一份现行规范里；
规范写着三个 hook，代码里有五个。recipe 的作者（官方索引、子索引、第三方、mcpp-index）没有一个地方能查到"什么是被接受的"。

**五样东西**

1. **一个名字。** 格式叫 xpkg；一个文件叫 recipe；格式的版本就是 `spec` 的值（1、2、3）。
   "xpkg manifest v1"与"XPackage V2"这两个标题不再使用。
2. **一份规范，两处都放，互相链接**（英文，按 `docs/specs/` 现有的惯例：现行与否、自哪个版本、在哪里实现；
   每一项标明自哪个 `spec`、哪个客户端版本起）。它取代 `xpkg-manifest-v1.md`，并入索引 `xpackage-v2.md` 里属于格式的部分。
   - libxpkg 的 `docs/spec/xpkg.md`：随 libxpkg 的版本走，与接口清单、一致性夹具在同一个仓库、同一个 tag 上。
   - xlings 的 `docs/specs/xpkg.md`：站点上发布的那一份，recipe 的作者看的地方。
   - 两份的正文相同，各自在顶部写明另一份的地址与所对应的 libxpkg 版本。
   - **不让它们各走各的**：文字只在 libxpkg 那一份里改；xlings 提高 libxpkg 的版本时把它带过来（与生成接口参考是同一步），
     xlings 的检查保证这一份与所依赖的 libxpkg 版本里的那一份逐字相同。方向与依赖一致：xlings 依赖 libxpkg，反过来没有。
   - "两处都放并互相链接"是评审的决定；"在哪一份里改、怎样保证相同"是本文补的做法。

   | 章 | 内容 |
   |---|---|
   | 版本 | `spec` 的含义；每一版相对上一版的差别；客户端的上限与逐包拒绝 |
   | `package` 表 | 字段、类型、`type` 的取值 |
   | `xpm` | 平台、版本条目的各种形态、架构的名字与枚举、`strip`、修订与下限、别名 |
   | 生命周期 | 哪个 hook 在什么时候、在哪个目录、带着什么上下文运行；返回值；宿主在 hook 之前之后做什么（落地、登记、检查） |
   | hook 的环境 | 三种调用；标准库的哪一部分；模块（参考由接口清单生成）；兼容层的内容与它冻结的事实 |
   | 声明 | `xvm`、`subos.env`、`system.admin_step`、说明（`log`） |
   | 索引一级 | `index-compat.json`（下限、保留的命名空间、模板）、`libs/`、子索引的列表 |
   | 演进 | 下面第 5 条 |

   - `spec = "3"` 规定四个 hook：`installed`、`install`、`config`、`uninstall`。`build` 在代码里保留，规范不定义（索引里没有使用者）。
   - `spec = "3"` 把 §5.3 的"有产物的条目必须枚举架构"写成格式的规则（范围与 §5.3 相同：有两种宿主架构的平台），
     由检查器与客户端共同执行；于是对 `spec = "3"` 的 recipe，§2.2 里"证据弱"与"没有证据"两类不存在，计划阶段的判断只剩一种。
   - 索引的 `xpackage-v2.md` 留下属于官方索引的约定（七条规则、怎样采用新能力、评审要求），改名为索引的约定；
     旧的 V1、V0 文档保留，顶部指向规范。xlings 的 `docs/30-authoring-a-package.md` 是教程，按 `spec = "3"` 重写。
   - 索引的文档、子索引与 mcpp-index 的文档链接到 xlings 站点上的那一份。
3. **一份接口清单**（libxpkg，数据文件）：每个名字一行，写明模块、种类（动作、询问、声明）、参数、自哪个 `spec` 与哪个版本起、
   适用的平台、它取代的旧名字。三处从它来，所以不会各说各的：
   - 测试：`spec = "3"` 的环境里有且只有清单里的名字；兼容层里有且只有清单里"被取代"的名字；
   - 参考文档：xlings 的 `docs/generated/xpkg-hook-api.md`，与命令参考同样由生成器产出、由检查保证没有过期；
   - 迁移工具与检查器用的新旧对照。
4. **一套一致性夹具**（libxpkg `tests/conformance/`）：一组 recipe，加上"加载后的 `package` 表是什么"
   "每个 hook 调用之后成立什么"的期望。libxpkg 的 CI 在三个平台上跑；xlings 的 CI 经真实的安装再跑一遍；
   别的实现（mcpp）可以直接取用。§8.2 的载荷规则夹具是同一种做法。
5. **演进的规则**
   - 加一个名字、加一个字段：不提高 `spec`。标明生效的客户端版本；索引要么用探针，要么提高下限
     （`docs/specs/index-version-contract.md` §3 的那张表）。
   - 改一个名字的含义、删一个名字：提高 `spec`。一个 `spec` 的环境发布之后只加不改。
   - 弃用：旧名字进兼容层，在清单里记下取代它的名字；兼容层只存在于它所属的那些 `spec`。
   - 一个客户端认识的最高 `spec` 是它的上限；更高的逐包拒绝并提示更新。

**一个检查器**：libxpkg 提供"检查一个 recipe"的函数，返回发现（字段的类型、条目的形态、架构是否枚举、
用到的名字是否属于它声明的 `spec`、`strip` 的取值）。两处使用：

- xlings 增加 `xlings index check [路径]`，检查一个 recipe 或一个索引目录。这是命令树的新增（已确认），
  按规则同时改命令规范、生成的参考与测试；它放在已有的 `index` 之下，不是新的顶层命令。
- 官方索引的检查（`xpkg_ci.py`）里属于格式的部分（§5.6 的 K1、K3）改为调用它；属于索引的部分（K2 资产准入、K4 机器人）留在索引。
  xlings 的 `tools/check_index_compat.py`（给出"这个索引需要的最低客户端"）把 `spec` 与用到的名字算进去。

**方向，不在本轮**：`config()` 与 `uninstall()` 各有约 570 个，内容多是登记与注销。登记改为声明之后，
多数 recipe 可以没有 hook。本轮的落地模型是这个方向的第一步，接口的三种调用里"声明"就是为它留的位置。

## 10. 设计：结构、小修与并入的两个 PR

### 10.1 结构与小修

- **#566**：一个构造函数 `make_execution_context(节点, 作用域)`，七处调用点都改用它；
  `subos_sysrootdir` 与 `hook_log` 的填法只剩一种。它是 xlings PR 的第一个改动，后面的检查点都要动上下文。
- **#594**：恢复 `XLINGS_LOCK_TIMEOUT` 时，原先未设置的就取消设置。
- **#417**：新鲜度探测在 `HEAD` 得到 401、403 或 405 时改用 `GET` 加 `Range: bytes=0-0`。#741 的 UP-G1 随之关闭。
- **#593**：`remove` 的 `--all`、`--all-subos`、`--subos`，覆盖层的列出、移除、清理，doctor 的范围参数，
  在 `src/capabilities.cpp` 里各有对应能力。C1 之后这些命令已经返回结构化结果，能力只是把它们暴露出来。

### 10.2 并入的两个 PR

**PR #738（作者标签改为两个值）**：整体带入，作为 X0 的一个 commit。

- 内容：`Authorship:` 只有 `ai-and-human`（AI 完成大部分，人指挥并少量参与）与 `by-human`（人主导并决定，AI 协助）。
  改 issue 与 PR 模板、`labels.yml`（列表、正则、文件头）、贡献 skill，共七个文件。
- 为什么不能拖：GitHub 上的标签已经改了，`main` 的 workflow 却还在匹配旧的三个值，
  所以今天写 `Authorship: ai-and-human` 的 issue 不会被打标签（#740、#741）；
  写 `Authorship: by-ai` 的会让 workflow 去加一个已经不存在的标签。
- 与本方案的关系：本方案要在每个仓库各开一个 PR，它们的正文都要写这一行；先让规则与标签一致。
- xim-pkgindex 一侧（AGENTS.md 与报告流程的文档里同样的两个值）在索引 PR 的 P9 里核对。

**PR #710（macOS 任务改用 macos-15）**：已被 `main` 取代，只带入多出的一步。

- `main` 已经把 `release.yml`、fresh 与 luban-fresh 的 macOS 任务改到 macos-15，所以这个 PR 的三处 runner 改动不再需要，
  它与 `main` 也已冲突。
- 它多出的一步 `main` 没有：macOS 的 fresh 任务对**已安装的** xlings 断言 `minos 14.0`。
  支持的下限仍是 macOS 14，而 CI 里已经没有 macOS 14 的机器；下限现在只在构建处被断言。
  对装出来的二进制再断言一次，是下限在"用户拿到的东西"上的证明。带入 X0。
- 与本方案的关系：§5.7 的分级表把 macOS arm64 列为"单一架构"平台，它的下限是那张表的一部分。

## 11. 测试

需求编号（名称在实施时定，写进 `tests/requirements.toml`）：

| 编号 | 内容 |
|---|---|
| `RESULT-OUTCOME` | 被请求的节点被拒绝或失败时命令失败；跳过的列在总结里；E3 的场景不改声明；没有运行 `install()` 的节点不被记为已安装 |
| `ARCH-PLAN-REFUSAL` | 枚举条目不含宿主时在计划阶段拒绝，一次给出闭包里全部缺失的节点 |
| `ARCH-MACHINE-RULE` | 面里有另一种机器的 ELF 时安装失败、不留载荷、不登记；面以外的不影响 |
| `ARCH-DOCTOR` | 已安装的错误架构载荷被报告 |
| `VERSION-EXACT-KEY` | 具体键不被前缀满足（E4 与 #739 的两种情形）；范围仍按范围 |
| `VERSION-DECLARED` | 声明里是解析后的版本；旧声明被带到新写法一次 |
| `VERSION-ALIAS` | 别名可安装、被记住、`update` 沿它移动 |
| `HOOK-NOTICE` | `log.warn` 的行出现在总结之后、日志与事件里；重定向输出时不丢（E5 三种方式） |
| `ELFPATCH-OUTCOME` | 缺补丁工具且有文件要补时失败并说明；没有文件要补时通过；显式调用指定了文件而缺工具时失败 |
| `HOOK-ACTION-FAILS` | `spec = "3"` 的 hook 里，`fs` 的每个动作与 `system.exec` 没做到时 hook 失败，错误里有动作、路径与原因；包在 `pcall` 里时不影响；`fs.move` 的目标已存在时报错 |
| `HOOK-ENV-SPEC3` | `spec = "3"` 的环境里没有兼容层的名字，没有 `io` 与 `os.execute`；不认识的模块报错；`spec = "2"` 的同一段文字照旧运行（E15 的情形在新环境里失败） |
| `HOOK-COMPAT-FROZEN` | `spec` 为 1、2 的 recipe：E8、E16、E17 里各个老名字的返回值与今天逐一相同；失败的动作调用出现在结果里 |
| `HOOK-EMPTY-PAYLOAD` | E16：hook 报告成功、有失败的动作调用、安装目录为空时这一步失败，不登记；没有失败调用的空目录包（载荷在别处的）照常 |
| `SPEC-GATE` | 高于上限的 `spec` 被逐包拒绝，命令的退出码非零（E14；现有的 E2E-35 改为同时断言退出码） |
| `LAND-BEFORE-INSTALL` | `spec = "3"`：`install()` 开始时内容已在安装目录；没有 `install()` 的包同样；`install()` 失败时不留载荷；`spec = "2"` 的同一个归档仍走旧路径 |
| `LAND-STRIP` | `strip = 1` 去掉顶层目录；成员不足 n 段时失败并指出那个成员；tar 与 zip 结果相同 |
| `FS-EXTRACT` | `fs.extract` 在三个平台上解 tar.gz、tar.xz、zip；宿主没有给端口时报错 |
| `EXEC-ARGV` | 列表形式：带空格、引号、`%`、`&` 的参数原样到达子进程，三个平台；Windows 上 `.bat` 被拒绝并提示 |
| `INDEX-TEMPLATE` | 有模板声明时每个 recipe 带上模板的 hook，检出的文件不被改写，构建脚本不运行；没有声明时照旧 |
| `XPKG-API-MANIFEST` | `spec = "3"` 的环境与接口清单逐名相同；兼容层与清单里被取代的名字逐名相同；生成的参考没有过期；xlings 里的规范与所依赖的 libxpkg 版本里的那一份相同 |
| `XPKG-CONFORMANCE` | 一致性夹具在 libxpkg 与 xlings 两处的结果相同 |
| `INDEX-CHECK` | `xlings index check` 对每一类错误的 recipe 给出对应的发现，对规范的模板没有发现 |
| `BUILD-FARM-RPATH` | §2.8 原型的九行：SubOS 的工具链得到 farm，宿主的工具链、共享库、静态链接不被改动，声明退出时只有标签 |
| `EXTRACT-OWN-DIR` | 两个扁平归档先后安装，后者的目录里没有前者的文件 |
| `REMOVE-OLDER-THAN-STATE` | 目录不存在的项目被清理并说明；状态读不了时停止 |
| `FRESHNESS-GET` | `HEAD` 被拒时用 `GET` 探测 |
| `UPDATE-NOTICE` | 有新版时提示一次；非终端不打印 |
| `MACOS-FLOOR-INSTALLED` | macOS 上装出来的 xlings 声明 `minos 14.0`（fresh 任务里的一步） |

| 层 | 在哪里跑 |
|---|---|
| 单元（归类、规则 M 的面、汇总、身份表与 `build_arch()` 的一致） | 三个平台 |
| 规范的夹具（§8.2） | xlings 与 mcpp 各自的 CI |
| C++ e2e（XTEST、隔离 home、夹具 recipe） | Linux x86_64 与 `ubuntu-24.04-arm`；按 `tests/lanes.toml` 分片 |
| #592：E2E-100 到 110 的契约 | 改写为 C++ e2e，三个平台都跑；旧的 bash 版删除 |
| 索引 | 全量安装（两种架构，被测客户端）；改动 recipe 的安装测试；mesa 的 aarch64 验证（§5.8） |
| libxpkg | 说明的记录、`elfpatch` 的顺序与结果、模块的每个动作与询问、两种环境，Linux、macOS、Windows |
| 发布后 | fresh 套件增加一格：aarch64 上安装 `xim:mesa` |

**临时验证用 draft PR 的 CI**（§2.5 里没有测过的各项在哪里得到答案）：

| 没有测过的 | 在哪里 |
|---|---|
| 新接口、落地模型的可行性；迁移工具能自动改写的比例 | libxpkg 的 draft PR（探路，§9.6"迁移"末尾）与 xlings 的 draft PR |
| aarch64 主机上的行为（规则 M、计划阶段的拒绝、`ld` 包装器的原型） | xlings 的 draft PR，`ubuntu-24.04-arm` |
| mesa 在 aarch64 上的 meson 配置、`libllvm` 的构建时长 | 索引的 draft PR 上触发 `res-build` |
| 规则 M 与空载荷规则在全索引上的误报 | 索引的 draft PR 上的全量安装，用 xlings draft PR 的构建 |
| macOS 分支里的调用 | 全量安装新增的 macOS 腿 |
| `ld` 包装器在 lld、musl 工具链下 | 索引的 draft PR 里 P6 的检查 |

draft PR 只用来取得结果；结论写回本文后，正式的 PR 仍然是每个仓库一个。

每个新用例先在未修改的构建上按预期失败。总结的文字变了，fresh 套件（`tests/fresh-install/`）里断言旧文字的地方
在同一个 PR 里更新。

## 12. 实施计划与依赖

### libxpkg（一个 PR，发布为 0.0.68）

| 编号 | 任务 | 依赖 | 完成的判据 |
|---|---|---|---|
| L1 | 架构身份表；加载器与 `elfpatch` 改用它；Scheme B 认表里的每个架构 | — | 现有测试不变；新架构键被解析 |
| L2 | 架构证据的归类函数，导出（xlings 的 `compatibility.cpp` 说它"镜像"的那个判断） | L1 | 单元通过；与 xlings 现有归类逐例一致 |
| L3 | `HookResult::notices`；边界协议带上它，协议号加一 | — | 布尔返回的映射不变；旧形态的 recipe 不变 |
| L4 | `elfpatch`：先扫描后查工具；结构化的自动结果；显式调用缺工具即失败；检查改为返回事实（清单在本检查点列出） | L1 | 缺工具的各种情形符合 §9.3 |
| L5 | 模块里的新名字（§9.6 的表）：`fs` 的询问与动作、`system.run` 与 `exec` 的选项、`pkginfo.platform` 与 `arch`、`path` 模块；进程层按参数列表启动；`fs.extract` 与宿主端口 | — | 每个动作的"结束时成立"与失败各有单元测试，三个平台；`EXEC-ARGV` |
| L6 | 兼容层分出来：`prelude.lua` 改为 `compat.lua`，模块里的旧名移进去；行为冻结；失败的动作调用进 `HookResult`；删去出处的注释，更正 README、`build.mcpp` 与 skill | L3、L5 | 现有的 recipe 测试一个不改地通过；E8、E16、E17 的返回值不变 |
| L7 | `spec = "3"` 的环境：不装兼容层、标准库只留纯计算部分、`import` 不认识的模块报错 | L6 | `HOOK-ENV-SPEC3` 在 libxpkg 一侧的用例 |
| L8 | 条目的 `strip`（加载器读出，随解析结果交回）；索引的模板声明（加载器在内存里拼接，有声明时不运行构建脚本） | L1 | `INDEX-TEMPLATE` 在 libxpkg 一侧的用例；现有的子索引夹具不变 |
| L9 | 接口清单；一致性夹具；检查器；迁移工具（探路之后写） | L5 到 L8 | `XPKG-API-MANIFEST`；工具在探路的十个 recipe 上的改写与人工改写一致 |
| L10 | 规范的正文 `docs/spec/xpkg.md`（按章对照三份旧文档合并）；其余文档、版本号、tag | 全部 | CI 三个平台通过；一致性夹具覆盖规范的每一章 |

随后 mcpp-index 一个 PR（M1）：登记 `mcpplibs.xpkg` 0.0.68。

### xlings（一个 PR）

| 编号 | 任务 | 依赖 | 完成的判据 |
|---|---|---|---|
| X0 | 基线：`hookrt` 的夹具按 §9.6 末尾修复；`unit (1/6)` 的失败查明并修复；#738 的七个文件；#710 的 minos 断言 | — | `main` 的各 workflow 在这个 commit 上通过（含 `u-ubuntu-2004`）；`Authorship: ai-and-human` 的 issue 被打上标签 |
| X1 | 夹具 recipe、失败用例、需求编号 | X0 | 在未修改的构建上按预期失败 |
| X2 | #566：上下文的单一构造 | — | 现有测试不变 |
| X3 | libxpkg 0.0.68（联调期用 git rev）；身份表与归类改用 libxpkg 的；`elfread` 加机器与位宽 | L 全部 | 行为不变 |
| X4 | C1：`StepOutcome`、汇总、退出码；§6.2 的各处；lint | X2 | `RESULT-OUTCOME`、`REMOVE-OLDER-THAN-STATE` 通过 |
| X5 | C3：归类、精确键、声明写解析结果与 home 格式步骤、别名 | X4 | `VERSION-*` 通过 |
| X6 | C2：计划阶段的汇总报告、规则 M、doctor、规则 P 的发现与安装记录 | X3、X4 | `ARCH-*` 通过；**P4 为零** |
| X7 | C5：说明的收集与展示、hook 输出的直接修复、`elfpatch` 结果的消费、工具表两行、新版本提示 | X3、X4 | `HOOK-NOTICE`、`ELFPATCH-OUTCOME`、`UPDATE-NOTICE` 通过 |
| X8 | #736：每节点的解压目录 | X2 | `EXTRACT-OWN-DIR` 通过；扁平 recipe 的安装通过 |
| X9 | #417、#594 | — | `FRESHNESS-GET` 通过 |
| X10 | #593：接口能力 | X4 | 能力清单与命令规范一致 |
| X11 | #592：C++ e2e，删 bash 版 | X4 | 三个平台通过 |
| X12 | hook 的接口（§9.6）：`spec` 的上限改为 3；发现 `hook-call-failed`；空载荷的硬规则；被拒绝的 `spec` 让命令失败；夹具改用 `spec = "3"` | X3、X4、X7 | `HOOK-*`、`SPEC-GATE` 通过；空载荷规则在 P4 里零误报 |
| X13 | 落地（§9.7）：`spec = "3"` 的包在 hook 之前解压并放好；解压器的 `strip`；给执行器的解压端口 | X8、X12 | `LAND-*`、`FS-EXTRACT` 通过 |
| X14 | 规范化（§9.9）：`docs/specs/xpkg.md` 从所依赖的 libxpkg 版本带入，取代 `xpkg-manifest-v1.md`，检查保证两份相同；由接口清单生成 `docs/generated/xpkg-hook-api.md`；`xlings index check`；一致性夹具进 CI；`tools/check_index_compat.py` 认识 `spec` 与名字 | X12、X13 | `XPKG-*`、`INDEX-CHECK` 通过；文档检查通过 |
| X15 | `docs/specs/payload-rules.md` 与夹具、`docs/51`、`docs/30` 编写 recipe 的章节（按 `spec = "3"` 重写）、生成的命令参考、升级与兼容规范里的行为变化、fresh 套件的断言 | 全部 | 文档检查通过 |
| X16 | 依赖改为版本号；发布 | 全部、M1 | `release.yml` 通过 |

```
X0 ─> X1 ─> X2 ─┬─> X4 ─┬─> X5
                │       ├─> X6 <── X3 <── L*        X6 还等 P4
                │       ├─> X7 <── X3
                │       │    └─> X12 ─> X13 ─> X14  X12 的空载荷规则还等 P4；X13 还要 X8
                │       ├─> X10
                │       └─> X11
                └─> X8
X9（独立）                            全部 ─> X15 ─> X16 <── M1
```

**行为变化**（写进发布说明与 `docs/specs/upgrade-and-compatibility.md`）：

- `subos runtime` 被拒绝时退出码非零，声明不变；
- recipe 读不了或归档失败的节点算失败，不再静默跳过；
- `name@<索引里的具体键>` 不再被更长的已安装版本满足；
- 面里有另一种机器的文件的包安装失败；
- 缺补丁工具且有文件要补时安装失败；
- `remove --older-than` 遇到读不了的项目状态时停止；
- hook 的 `log.warn` 出现在总结之后；
- hook 报告成功、有失败的动作调用、安装目录为空的包安装失败（§9.6；`spec` 为 1、2 的 recipe 的其它行为不变）；
- `fs.symlink` 与 `fs.remove` 失败时 hook 失败，`fs.remove` 删除整个目录（§9.6）；
- 一条命令里有包因 `spec` 被拒绝时退出码非零；
- `docs/specs/xpkg-manifest-v1.md` 由 `docs/specs/xpkg.md` 取代（原地址留一个指向它的页面）；
- recipe 显式要求改写的文件，缺补丁工具时安装失败。

### xim-pkgindex（一个 PR）

| 编号 | 任务 | 依赖 | 完成的判据 |
|---|---|---|---|
| P1 | 普查脚本、迁移工具（§5.6 的三步）、覆盖表、检查 K1 与 K3 | — | 在当前 `main` 上给出 §2.2 的数字；工具在 §2.7 的样本上给出同样的结论 |
| P2 | 89 个 recipe 的机械迁移 | P1 | 安装测试通过；x86_64 上字节不变 |
| P3 | 用迁移工具处理 51 加 19 个：静态能定的直接改写，其余等 P4 的两次安装；工具列出的少数几个由人定；K3 的校正；把用作进度的 `log.warn` 改成 `log.info` | P1 | K1 对全索引的 Linux 段通过；每个改动有证据表里的一行 |
| P4 | 全量安装增加"被测客户端"输入、aarch64 的腿与 macOS 的腿；用 xlings PR 的构建跑完全部分片（同时给出迁移工具第 2 步的结果，以及 `hook-call-failed` 在全索引上的清单）；准入检查 K2 | P2、P3 的静态部分、X6 与 X12 的实现 | 规则 M 的误报为零；空载荷规则的误报为零；P3 余下的条目据此改完 |
| P5 | 图形构建并入 `res-build`；构建输入的 aarch64；mesa 闭包 18 个包的 aarch64 资产与 recipe | P1 | §5.8 的验证表通过 |
| P6 | binutils 的 `ld` 包装器按 §8.3 的规则改写，`revision`；把原型的九行做成索引 CI 里的检查 | — | §2.8 原型的九行 |
| P7 | musl flavor 发布 `gcc-ar`、`gcc-nm`、`gcc-ranlib`，`revision` | — | doctor 不再报五个不一致 |
| P8 | 自动更新沿用条目形态（K4）；guard 放行别名键；`node` 的 `lts` | P1 | 机器人 PR 过 K1；`xlings install node@lts` 可装 |
| P9 | 规范文字：`archs` 的含义、条目怎样说明架构、别名、`log` 各级别的含义、不采用外部安装器；模板；作者标签的两个值在索引的文档里一致 | 全部 | 站点检查通过 |
| P10 | `libs/` 的 15 个文件改用模块里的名字，`payload.unpack` 删去；模板改为 `spec = "3"`；`xpackage-v2.md` 留下索引的约定并指向规范；检查改为调用检查器（K1、K3），新增的 recipe 必须是 `spec = "3"`，`xlings` 按名字放行 | L 全部、X12 到 X14 的实现 | 全量安装通过 |
| P11 | recipe 分批迁到 `spec = "3"`，每批一个 commit，一个 recipe 只动一次（名字、落地、架构枚举）；先迁 P4 清单里有失败调用的与 mesa 闭包的；删去 N 之前能力的探针 | P4、P10 | 每批的全量安装通过；覆盖表列出仍是旧写法的 recipe 与原因 |
| P12 | `index-compat.json` 的最低客户端改为 N；索引各 workflow 钉的客户端改为 N | xlings 发布并镜像 | 用 2026.11.23.1 取索引时被带回上一个快照并说明原因；`self update` 从那里能到 N |

P5 的资产构建耗时长，在 PR 分支上触发 `res-build`；资产发布后 recipe 的检查点才能变绿。
P10 与 P11 在 PR 分支上用 xlings PR 的构建验证；P12 是合并前的最后一个 commit。

### 子索引 scode、awesome、d2x（各一个 PR，不挡发布）

| 编号 | 任务 | 依赖 | 完成的判据 |
|---|---|---|---|
| S1 | `index-compat.json` 声明模板；构建脚本保留给旧客户端 | xlings 发布 N | N 的客户端不运行脚本、检出的文件不被改写；旧客户端照旧 |
| S2 | 模板迁到 `spec = "3"`；下限提高到 N 后删掉构建脚本 | S1；各自决定时间 | 该子索引的安装测试通过 |

### mcpp（一个 PR）

| 编号 | 任务 | 依赖 | 完成的判据 |
|---|---|---|---|
| C1 | 链接步骤与 `mcpp pack` 声明 `XLINGS_SUBOS_LD_PATHS=0`；断言产物不含 store 路径；跑规范的夹具 | P6 发布（或用索引分支） | mcpp 的 e2e 通过 |

### 跨仓库的顺序与发布

```
libxpkg PR ─> 0.0.68 ─> mcpp-index M1 ──────────────────────┐
                                                             ├─> xlings 发布 N ─> 镜像 ─> 索引 PR 合并（P12：下限 = N）─> 索引里的版本 bump
xim-pkgindex PR: P1 ─> P2、P3 ─> P4（要 xlings PR 的构建）────┘
                 P5 到 P11 在分支上完成并验证
mcpp PR: 在 P6 之后
子索引的 PR: 在 xlings 发布 N 之后，各自进行
```

- **两个硬依赖**：
  1. xlings 发布之前，P4 必须在索引的 PR 分支上为零（规则 M 与空载荷规则的误报）。验证不需要合并。
  2. 索引的 PR 在 xlings 发布并镜像之后合并：它的 `libs/` 与 recipe 用了新接口，最低客户端是 N，N 必须已经存在。
- 与上一版的不同：上一版里索引的 PR 可以先合并，因为它不要求新客户端。现在架构的迁移（P1 到 P3）仍然对已发布的客户端
  生效（E2、E6、E7），但它与 hook 接口的迁移在同一个 PR 里，随 PR 一起在发布之后合并。
  代价：在那之前，已发布的客户端在 aarch64 上的行为与今天相同（单产物的包仍会装错）；
  这项改善原本可以早一次发布的时间到达。
- xlings 发布到索引合并之间，N 的用户看到的是未迁移的索引：`spec` 为 1、2 的 recipe 行为不变；
  规则 M 对未迁移的错误架构载荷如实拒绝。这段时间应当很短（发布、镜像、合并是连续的三步）。
- 低于 N 的客户端在索引合并后留在上一个快照，行为与今天相同；`xlings self update` 把它们带到 N（§9.6"旧客户端"）。
- 发布按 AGENTS.md 的流程：版本号、镜像、索引 bump。

## 13. 不采用的方案

| 方案 | 为什么不 |
|---|---|
| spec 2 的单产物按 `archs` 拒绝 | `archs` 是并集：会误拒少写的，也放过 `bat` 这样多写的 |
| 载荷里任何一个 ELF 不是宿主机器就拒绝 | 交叉工具链、sysroot、多架构预编译件会全部误报 |
| 新增 `noarch = true` 字段（第一版的做法） | 已有的写法能表达同一件事且可检验（E6）；新字段要改加载器与规范，旧客户端只是"忽略"而不是"理解" |
| 用 `archs = {"any"}` 或 `sha256 = { any = … }` | 旧客户端会打印多余的 warn，或把 `any` 当成一种架构而拒绝所有宿主 |
| 新增 `notices` 字段与 `system.notice()`（第一版的做法） | `log.warn` 已经是作者表达"给用户看"的方式；新接口要守卫、要迁移 122 处调用，旧 recipe 不受益 |
| 只修日志路由来解决 #721 | 默认情形下用户仍看不到；说明与调试输出仍混在一条流里 |
| recipe 侧的"跳过"调用 | 今天没有 recipe 需要；先不加 |
| 把架构的全部属性放进 libxpkg 的一张表 | loader 路径、启动文件名不是 recipe 格式的一部分，而且 `modules/` 不依赖 libxpkg |
| 把 ELF 检查留在 libxpkg | 检查要用到登记、导出与 store 的全貌，这些在 xlings；xlings 已有进程内的读取器 |
| `ld` 包装器对每次链接都追加 `-rpath <farm>`，不想要的一方声明退出（2026-08-11 设计的第 2 步） | 实测会让宿主编译器的产物启动即崩（E12）；不该得到路径的一方不知道自己经过了包装器 |
| 把 farm 写进 gcc 的 `specs` | `specs` 在共享的载荷里，不能指名某个 subos；用环境变量的写法在变量未定义时会让 gcc 直接报错 |
| 只修 `hookrt` 的夹具 | 夹具只是第一个撞上的；索引里 752 处不看结果的动作调用还在 |
| 把 `os.mv`、`os.cp`、`os.exec`、`os.mkdir` 改成失败即报错（第四版的做法） | 同一段 recipe 在新旧客户端上含义不同；已安装的包与第三方索引的 recipe 行为被改变；两套文件词汇、五种运行命令的方式与那个桩都还在；"试"仍然靠名字前缀表达 |
| 不动 libxpkg，在索引的 `libs/` 里包一层会报错的函数 | 接口的含义有了两个出处，第三方索引得不到；recipe 仍然可以绕过它直接调老名字 |
| 新旧名字并存于一个环境，靠检查脚本禁止老名字 | 只管得到官方索引；"这个 recipe 只用新接口"不是 recipe 自己说的，客户端也无从保证 |
| 给 hook 的接口单独设一个版本字段 | `spec` 已经是 recipe 声明自己写法的地方，客户端对不认识的 `spec` 已经会逐包拒绝（E14） |
| 新环境里保留完整的 Lua 标准库 | `os.execute`、`io.popen`、`os.rename` 不报错也不全进 hook 日志，等于留着一条绕过三种调用的路 |
| 给模块的新动作另起一套不与旧名相撞的名字（例如 `fs.delete`） | 相撞的只有 `fs.remove` 与 `fs.symlink`，索引里共 4 处调用；为它们让出最自然的名字不值得 |
| 这一版就删掉兼容层 | 已安装的包带着安装时的 recipe；本地与第三方的 recipe 不由我们迁移 |
| 只给 hook 一个 `fs.extract`，不改落地的时机 | hook 仍然要"先解到一处、再搬进安装目录"，搬运与猜目录名都还在；171 个只做落地的 hook 一个也删不掉 |
| 落地时自动去掉唯一的顶层目录 | 结果取决于归档里碰巧有几个顶层成员；xlings 的代码里删过这样一个分支，注释写明了原因。声明 `strip` 是 recipe 自己说的 |
| `strip` 不足时像 `tar` 那样跳过那个成员 | 少了文件却报告成功，正是本方案要去掉的那一类 |
| 命令只保留字符串形式 | 引号问题留在每个 recipe 里；索引已经为它写了一个 lib 与一条检查 |
| 命令只保留列表形式 | 约一成的命令确实用管道与重定向 |
| 把构建脚本搬到新的 hook 环境里运行 | 三份脚本做的是同一件事，可以用一句声明表达；保留脚本就要保留"改写检出的文件再恢复" |
| 用索引的一个 lib 代替模板（每个 recipe 自己 `import` 并转调） | hook 是文件顶层的函数，lib 定义不了它们；每个 recipe 要多写四段转调 |
| 规范只放在一个仓库（第六版写的是只放 xlings） | 评审决定两处都放并互相链接：libxpkg 那份与清单、夹具同版本，xlings 那份在站点上 |
| 两处各自维护一份规范 | 两份迟早不同；文字只在一处改，另一处由检查保证相同 |
| 规范继续分在两个仓库，各写各的增量 | 现状就是这样：hook 的接口落在两份之间，没有人写 |
| 检查器写在索引的 Python 里 | 那是加载器之外的第二份"什么是合法的 recipe"；子索引与第三方用不上 |
| 本轮把登记也改成声明 | 方向是对的（§9.9 末尾），但它要重新设计 `config()` 的职责，量与风险都超过本轮；先由落地模型验证"宿主做、recipe 声明"这条路 |
| 只靠逐包拒绝 `spec = "3"`，不提高索引的最低客户端 | `libs/` 被新旧 recipe 共用，它改用新名字后旧客户端上所有用到它的包都会出错，而且出错的方式不是一句说明 |
| 规则 M 提供环境变量开关 | 开关会让"硬"变成约定；误报的修法在 recipe，发布即生效 |
| 为图形栈新建 workflow | `res-build` 已有计划、按架构构建、自检、受保护的发布与镜像补齐 |
| mesa 的 aarch64 在本地机器上构建 | 与 x86_64 当初一样不可复现 |
| mesa aarch64 第一步就做全部驱动 | clc 链多五个包，SoC 驱动在 CI 上无法验证 |
| 把 15 条 issue 拆成多个 PR | 已定：每个仓库一个 PR，commit 为检查点 |

## 14. 前提与重看的条件

- **规则 M 的面只有三项。** 前提是"宿主直接执行或加载的"就是这三项。全量安装若发现面以外仍有被宿主加载的
  东西（例如某个包的插件目录），面的定义要扩，而不是给包加例外。
- **闭包规则 D、A、E 仍不拒绝。** 前提是索引里还有未清理的发现。结构化之后可以统计；
  全量安装连续两轮为零时收紧（§15 第 6 项）。
- **与架构无关用"同一产物列给每种架构"表达。** 前提是宿主架构很少（今天两种）。到四五种时
  这种写法变得啰嗦，那时再考虑简写；新增架构时由脚本补键。
- **只管 ELF。** macOS 或 Windows 增加第二种架构时，规则 M 要有 Mach-O 与 PE 的实现，K1 扩到那个平台。
- **`log.warn` 等于说明。** 前提是 recipe 不滥用它。说明有每包的行数上限；索引检查可以对单个 hook 里
  `log.warn` 的条数给出提醒。
- **mesa 的驱动集合在两种架构上不同。** 有真机验证后补 SoC 驱动，作为同一版本的一次修订。
- **包装器把"系统路径上的 loader"当作"不是我们的"。** 前提是环境的 loader 从不放在系统路径上。
  Luban 系统里它在 `/lib64`，那里包装器不加东西是对的（库由系统的根解析）；出现第三种布局时重看。
- **用户构建的程序把 glibc 载荷的版本目录写进了自己**（`specs` 写的，§2.8）。移除那个版本的 glibc，这些程序就不能运行；
  `subos runtime` 已经为此打印一行提醒。让 `specs` 改指 `<home>/abi/<family>` 是另一个决定，本方案不做。
- **`spec` 既管 `package` 表的写法也管 hook 的环境。** 前提是两者一起演进。以后只想动其中一个时，
  `spec` 仍然加一，另一半写明"与上一版相同"。
- **兼容层不删。** 前提是它冻结、只有一个文件。它开始需要改动（例如某个老名字在新平台上要重写）时，
  重看的是"还有多少 recipe 在用它"，数字来自覆盖表与已安装载荷里的 `.xpkg.lua`。
- **`xlings` 的 recipe 永远用旧写法。** 前提是 `self update` 经索引里的 recipe 安装新版本。
  自更新改为不经 recipe 时，这个例外可以去掉。
- **动作的目标已存在就报错。** 前提是"先删再放"两行比一个会猜的函数更值得。迁移后若 `{ replace = true }`
  占了调用的多数，把它改成默认值是一次 `spec` 的事，不是悄悄改含义。
- **落地之后再调整，能覆盖绝大多数 recipe。** 前提是 hook 需要的只是"内容在安装目录里"。探路与迁移若发现一类 recipe
  需要在落地之前做事（例如先校验再解压），给条目加声明，而不是恢复"hook 自己搬"。在那之前这类 recipe 留在 `spec = "2"`。
- **`strip` 是唯一的布局声明。** 前提是"去掉前 n 段"说得清绝大多数归档。出现"只取某个子目录"的需要时再加一个键。
- **模板是"接在后面"。** 前提是三个子索引都只这样用。有索引需要按包选择模板时，声明从一个文件名变成一张表。
- **规范两处都放，在 libxpkg 那一份里改。** 前提是 xlings 每次提高 libxpkg 的版本都会带过来，站点上那一份不会落后
  超过一次版本提高。规范需要在两次 libxpkg 发布之间修订文字时，发一个只改文档的 libxpkg 版本。
- **一个串是否精确取决于索引里有没有这个键。** 索引以后新增一个短键（例如 `15`）会改变 `@15` 的含义。
  索引检查对"新增的键是另一个键的前缀"给出提醒。

## 15. 已定的决定与理由

| # | 决定 | 理由 |
|---|---|---|
| 1 | 规则 M 的面是三项：登记的程序、导出库目录里直接放着的共享库、导出的 loader；库目录不递归 | 这三项正是宿主会执行或加载的；递归会把交叉编译器 `lib/gcc/<目标>/` 下的目标库算进来。误报的修法在 recipe，漏报由全量安装与 doctor 兜底 |
| 2 | 与架构无关的包：**原定**包级 `noarch = true`（理由：加载器忽略未知的包级字段，放在平台段会被旧加载器当成版本键）。**自审后改为**不加字段，把同一个产物列给每种架构（§5.3） | 实测可行（E6、E7）；旧客户端不是"忽略"而是"理解"；这句话可以在每种架构上被规则 M 检验；规范、加载器、客户端都不用改。与原决定不同；评审时未提出异议 |
| 3 | mesa aarch64 第一步的驱动不带 clc 链，SoC 驱动第二步 | 少构建五个包，CI 能验证的（llvmpipe、virgl）都在；SoC 驱动在 runner 上无法验证，没有真机结果就发布等于未经测试 |
| 4 | #593 与 #592 留在 xlings 的 PR 里 | 它们消费的正是 C1 给出的结构化结果；排在最后、互不依赖，不会挡住前面的检查点；分开做要再走一次发布 |
| 5 | 安装后激活失败记为"已安装、未激活"，不算命令失败；用户写了 `--use` 时算失败 | `install` 承诺的是载荷；不抢已激活的版本是现有的有意行为；`--use` 是用户明确的请求，没做到就是没做到 |
| 6 | 闭包规则 D、A、E 本次不收紧，只做到结构化与可统计；全量安装连续两轮无发现后收紧 | 现在收紧会让已发布的资产在用户机器上装不上，而修法在资产一侧；先有数字再定日期 |
| 7 | 别名的首个使用者是 `node` 的 `lts` | 上游自己定义并公布哪条线是 LTS，别名有客观依据；`python` 等没有这个概念。自动维护别名留作后续 |
| 8 | hook 的提示：**原定**新增声明式接口（`notices` 字段与 `system.notice()`）与三态返回。**自审后改为**把 `log.warn` / `log.error` 记为说明，不新增 recipe 接口；三态只留在执行器的结果里（§9.1、§9.2） | 现有 122 处调用立即受益；不需要守卫，索引不需要等新客户端；"warn 级别就是给用户看的"本来就是作者的理解。与原决定不同；评审时未提出异议 |
| 9 | 索引迁移里"这个产物是哪种架构"由工具判断，不由人逐个看（§5.6） | 这正是本方案的原则：字节说了算。抽样里静态读字节定了 9 个中的 7 个，其余由两种架构上的安装定（§2.7）；人只看工具列出的少数几个 |
| 10 | `ld` 包装器只对"loader 不是系统路径的可执行文件"写入 farm，不照搬 2026-08-11 的第 2 步（§8.3） | 实测旧做法让宿主编译器的产物启动即崩（E12）；新规则的九种情形原型全部符合预期（E13）；判断标准与闭包规则一致 |
| 11 | hook 的接口重新设计：一份模块 API，调用分为动作、询问、声明，容错只用 `pcall`；`spec = "3"` 的环境没有兼容层；老名字的含义一个不改（§9.6）。`fs.move` 与 `fs.copy` 的目标已存在即报错。**第四版原定**逐个把 `os.exec`、`os.cp`、`os.mv`、`os.mkdir` 改成失败即报错 | 评审意见：这些名字的出处早已不用，不必以它为参照；接口可以重新设计，要符合 xpkg 的架构与演进，尽量兼容。改名字的含义会让同一段 recipe 在新旧客户端上不同，也留下两套词汇与那个桩；新接口由 recipe 用 `spec` 声明，客户端保证，旧 recipe 原样冻结。已确认 |
| 12 | 索引用上新接口的那个快照把最低客户端提高到 N；索引的 PR 在 xlings 发布之后合并；`xlings` 的 recipe 保持旧写法 | 评审意见：已有索引兼容机制可以避免旧客户端遇到新的索引（实测 E18）。`libs/` 被新旧 recipe 共用，只能整体切换；`self update` 读最新快照，所以它经过的 recipe 不能变。已确认 |
| 13 | `libs/` 本轮迁完；recipe 的迁移由工具分批做，以全量安装为门槛；装不到的留在旧写法并列出；全量安装增加 macOS 的腿 | 改写后含义变了的地方都会报错而不是静默，适合由安装来验证；没有被安装验证过的改写不合并。已确认 |
| 14 | 落地由宿主完成：`spec = "3"` 的包在 `install()` 之前已经放好，布局由 `strip` 声明（§9.7） | 评审意见：解压作为宿主的能力要做。只给一个解压函数留不住好处：搬运与猜目录名还在。把时机挪到 hook 之前，171 个 hook 可以删掉，E16 那一类问题不再可能 |
| 15 | 命令有列表与字符串两种写法，规范推荐列表（§9.6） | 评审意见：要做。约九成命令不需要 shell；列表形式下没有引号问题，与 `system.admin_step` 一致 |
| 16 | 索引的模板用 `index-compat.json` 里的声明表达，构建脚本的环境冻结（§9.8） | 评审意见：索引构建脚本的环境要处理。三份脚本做的是同一件事；声明之后不需要第三套环境，也不再改写检出的文件 |
| 17 | 规范化：一个名字、一份规范、libxpkg 里一份接口清单与一套一致性夹具、一个检查器（§9.9） | 评审意见：xpkg 的规范化要做好。现状是三份文档、三种叫法，hook 的接口没有现行规范；清单是参考文档、兼容对照与工具共同的出处 |
| 18 | 临时的测试与验证用 draft PR 的 CI；先做一次探路再写迁移工具（§9.6、§11） | 评审意见：可以用 draft PR 的 CI 资源。新接口与落地模型都没有原型，十个 recipe 的手工迁移能在写工具之前校正规则 |

| 19 | 增加 `xlings index check [路径]`，放在已有的 `index` 之下 | 已确认。没有它，检查器只能在 CI 里用，recipe 的作者在本地没有入口 |
| 20 | 规范在 libxpkg 与 xlings 两处都放并互相链接（§9.9）。**第六版原写**只放 xlings | 评审意见：可以都放以及链接。本文补的做法：文字只在 libxpkg 那一份里改，xlings 的那一份随版本带过来并由检查保证相同 |
| 21 | `strip` 不足时报错：某个成员的路径不足 n 段就失败并指出它 | 已确认。悄悄跳过会少了文件却报告成功 |
| 22 | 三个仓库的 PR 体量按现状接受（libxpkg 10 个检查点，xlings 17 个，索引 12 个）；不挡发布的排在最后 | 已确认。以 commit 为检查点推进；时间不够时 #593、#592、规范文档、`index check` 可以整块留到下一版 |

此前讨论中已定、本文直接采用的：结果契约作为主线；`e_machine` 作为硬检查；具体键永远精确、声明写解析结果；
ELF 检查归 xlings；#566 作为第一个改动；不拆分 #740；不改 issue 标题，修复后关闭；libxpkg 以版本号发布。

## 16. 记录在哪里

| 内容 | 位置 |
|---|---|
| 载荷规则与夹具 | `docs/specs/payload-rules.md`（新；按规范的惯例标明自哪个客户端版本生效） |
| 架构分级、新增架构的清单、一个包怎样得到新架构 | `docs/51-supported-platforms.md` 及中文镜像 |
| `archs` 的含义、条目怎样说明架构、别名、`log` 各级别 | xim-pkgindex `docs/specs/xpackage-v2.md`、模板；libxpkg 的文档 |
| xpkg 的规范（格式、生命周期、落地、hook 的环境、索引一级的文件、演进规则） | libxpkg `docs/spec/xpkg.md`（在这里改）；xlings `docs/specs/xpkg.md`（相同的正文，站点上发布；取代 `xpkg-manifest-v1.md`）；两份互相链接 |
| hook 接口的每个名字、种类、生效版本、取代了谁 | libxpkg 的接口清单（出处）；xlings `docs/generated/xpkg-hook-api.md`（生成） |
| 怎样写一个 recipe（教程） | xlings `docs/30-authoring-a-package.md` 及中文镜像；xim-pkgindex 的模板 |
| 官方索引自己的约定（七条规则、采用新能力、评审） | xim-pkgindex `docs/specs/`（由 `xpackage-v2.md` 改成，指向规范） |
| 模板声明、提高下限的条件 | xlings `docs/specs/index-version-contract.md` 加一节 |
| 哪些 recipe 还是旧写法、为什么 | xim-pkgindex 的覆盖表（与架构覆盖同一处） |
| "hook 里动作没做到就失败" | AGENTS.md 的结果契约那一条里 |
| 结果契约与四个词 | AGENTS.md"Rules the code is checked against"加一条 |
| libxpkg 的版本规则 | AGENTS.md"Versions and releases"加一条 |
| 行为变化 | `docs/specs/upgrade-and-compatibility.md`、发布说明 |
| 上游缺陷 | #741 |
| 普查脚本与覆盖表 | xim-pkgindex `tools/` 与站点 |

## 17. 自审记录（多视角，第二版）

每一行是一个视角下发现的问题与处置。"改"指本文已按它修改。

| 视角 | 发现 | 处置 |
|---|---|---|
| 简洁 | `noarch` 是一个新概念，而"同一产物列给每种架构"用已有的写法说了同一件事 | 改（§5.3）；实测 E6、E7；需确认 |
| 简洁 | 提示设计了两个新接口（字段与调用），而作者已经在用 `log.warn` 表达它 | 改（§9.2）；需确认 |
| 简洁 | recipe 侧的"跳过"调用没有使用者 | 删（§9.1） |
| 兼容 | 第一版里旧客户端对新字段是"忽略"；用到 `system.notice` 的 recipe 要守卫 | 两个新接口都去掉后，本方案不给 recipe 增加任何旧客户端不认识的东西（第五版起不再成立：§9.6 增加了接口，由索引下限与 `spec` 挡住旧客户端） |
| 兼容 | 精确键规则生效后，已有的声明（写着 store 里没有的 `2.44`）会变成"声明的版本没装" | 加 home 格式步骤（§7.2） |
| 兼容 | hook 可以在边界进程里运行，结果的新字段要过协议 | 协议号加一（§9.1、L3） |
| 兼容 | 多条命令的退出码与输出会变 | 列出行为变化，写进升级规范与发布说明；fresh 套件的断言同 PR 更新（§11、§12） |
| 语义 | 只归类了请求串，没有覆盖"同一计划里被另一个节点满足"（#739 的第二种情形） | 归类用于计划内的判断；"已安装"只由产生载荷的那一步写（§6.2、§7.1） |
| 语义 | "拒绝"与"失败"没有区分 | `Refused` 定义为什么都没改，`Failed` 为已回滚（§4） |
| 一致性 | 文中混用 advisory、warn、提示、发现 | 定为四个词，`advisory` 并入发现（§4） |
| 一致性 | 缺工具时的提示若由 libxpkg 自己写，就与"外部程序出自一张表"不一致 | 工具表加两行，提示从表里来（§9.3） |
| xlings 规范 | 默认输出应是结论与下一步；第一版把 18 个节点与逐条发现都放进默认输出 | 默认只给计数与下一步，细节在 `-v`、日志、doctor（§5.5、§6.2、§9.2） |
| xlings 规范 | "核心返回结构化结果，前端渲染"；第一版写的是"xlings 打印" | 说明、结果都由核心返回（§6.1、§9.2） |
| xlings 规范 | home 的格式要向前走且有记录 | 声明的改写走 `xlings.home.format`，不提高布局号（§7.2） |
| xlings 规范 | 改变 `config` 行为的 recipe 要提高 `revision` | binutils 与 musl-gcc 都写明（§7.4、§8.3） |
| 架构与结构划分 | 第一版把 loader 路径也放进 libxpkg 的架构表，而 `modules/` 不依赖 libxpkg | 只有身份在 libxpkg，属性留在各自的模块（§5.1） |
| 复用 | 第一版要在 `closure_check` 里再读一遍 ELF；仓库已有 `xlings.libs.elfread` | 用它（§5.4） |
| 复用 | 第一版为图形栈新建 workflow；`res-build` 已有全部环节 | 并入（§5.8） |
| 复用 | 第一版要在索引里新写一个全索引测量；`ci-sweep` 已经在做全量安装 | 给它加被测客户端的输入与 aarch64 的腿（§5.4） |
| 项目结构 | 索引的 CI 不应依赖 `.agents/` 下的脚本 | 构建脚本移到 `tools/res/graphics/`（§5.8） |
| 项目结构 | 规范一份，实现其实是两份（xlings、mcpp） | 如实写；用共同的夹具对齐（§8.2） |
| 通用性 | 架构检查只考虑了 Linux 段 | 写明只有一种受支持架构的平台上未说明即该架构，K1 只管 Linux，条件变了再扩（§5.3） |
| 跨平台 | 每节点的解压目录用链接放归档，Windows 上要能成立 | 用硬链接，不行则复制（§8.4） |
| 跨平台 | `ld` 包装器、规则 M 都不适用于 Windows 与 macOS | 各自写明范围（§5.4、§8.3） |
| 稳定性 | 规则 M 是硬的，误报会让包装不上 | 发布前的全量安装门槛；误报的修法在 recipe、发布即生效；不设开关（§5.4） |
| 稳定性 | §8.3 引用的设计早于 `<home>/abi/<family>` | 落地前重测，前提不符就重写这一节（§8.3） |
| 稳定性 | 一个 PR 里十三个检查点 | 每个检查点都能构建并通过测试；与主线关系最弱的排在最后 |
| 稳定性 | "每个检查点都通过测试"以基线是绿的为前提，而 `main` 现在有两处失败 | 加 X0：先恢复基线（§2.6） |
| 一致性 | 标签在 GitHub 上已改、`main` 的 workflow 未改，本方案的三个 PR 都要写 `Authorship:` | #738 带入 X0（§10.2） |
| 跨平台 | macOS 14 的下限在 CI 里已经没有对应的机器 | #710 多出的断言带入 X0（§10.2） |
| 性能 | doctor 对每个载荷重跑规则 P 要读全部 ELF 与文本文件 | 发现写进安装记录，doctor 读记录，`--deep` 才重扫（§8.2） |
| 性能 | 规则 M 的开销 | 只读面里文件的文件头，可忽略；不改 |
| 性能 | 不能硬链接时复制归档 | 只发生在少数文件系统上；写明 |
| 用户体验 | 被拒绝的用户不知道下一步 | 报错带下一步：覆盖表与请求的入口（§5.5、§5.4） |
| 用户体验 | 说明可能很长 | 每包有上限，超出的指向日志（§9.2） |
| 生态发展 | 第三方怎样为一个包补架构没有写 | 提交构建数据而不是字节（§5.7） |
| 生态发展 | 想用另一种机器的包的用户 | 一个 home 一种机器；用那种机器的客户端（§5.1） |
| 生态规范 | 规范的新增内容要标明生效的客户端版本 | 写进 §16 |
| 优雅 | 第一版的条目状态有五种，其中一种是新增的 | 现在是已有写法的五种情形，没有新增的 |

第五版（§9.6）的自审：

| 视角 | 发现 | 处置 |
|---|---|---|
| 语义 | 第四版改老名字的含义，同一段 recipe 在新旧客户端上读法不同 | 改：老名字不动，新接口由 recipe 用 `spec` 声明 |
| 复用 | 新旧环境各有一份模块，`libs/` 就得写两遍 | 模块只有一份，两种环境的差别只是有没有兼容层 |
| 一致性 | 模块里 `fs.remove`、`fs.symlink` 两个名字因此在旧环境里也变严 | 如实列为对旧 recipe 仅有的两处变化；索引里 4 处调用逐处看 |
| 兼容 | `self update` 读最新快照，它经过的 recipe 不能迁 | `xlings` 的 recipe 是唯一的长期例外，写进检查 |
| 兼容 | 已安装的包带着安装时的 recipe | 兼容层冻结保留，不设删除日期 |
| 稳定性 | 有 recipe 依赖"`os.mv` 静默失败后由 xlings 搬入解压内容" | 兼容层不动，它们不受影响；迁移时由安装暴露，逐个改 |
| 简洁 | 起草时还想过：字符串的三个函数挪进模块、命令的列表形式、解压接口 | 第一项留在原处（纯函数，挪了只增加改动）；后两项不在这一轮 |
| xlings 规范 | "读不了不等于空" | 询问答不了时报错（`fs.read`、`fs.list`） |
| xlings 规范 | recipe 不运行 `sudo` | `system.run_in_script(…, true)` 只留在兼容层；新接口里只有 `system.admin_step` |
| 用户体验 | 动作失败时作者要能直接定位 | 错误带动作名、路径、系统的原因与 recipe 的行号 |
| 跨平台 | `chmod` 是索引里 hook 运行得最多的命令，Windows 上没有 | `fs.chmod` 在 Windows 上不做事，recipe 不必再分支 |

第六版：对整个方案的自审。"改"指这一版已按它修改；"记"指如实写下、不在本轮解决。

| 视角 | 发现 | 处置 |
|---|---|---|
| 主线 | 方案从"结果要真实"出发，新增的四节是否还在这条线上 | 在。接口的三种调用、落地、规范，回答的都是"一步做没做到，谁知道"。根因多了 R6，契约仍是五条（C5 多一句） |
| 语义 | `pkginfo.arch()` 第五版写成"目标，不是运行 hook 的机器"，与 §5.1"一个 home 只有一种宿主机器"不一致 | 改：它就是 §5.1 的宿主机器，是这次安装的事实 |
| 语义 | `spec = "3"` 的环境没有约束文件的顶层：读 `package` 表时顶层对模块的调用今天被桩吞掉 | 改：顶层调用模块的函数是加载错误（§9.6 的表） |
| 语义 | 新环境容易被读成安全边界 | 改：写明不是；约束 hook 能碰什么是沙箱的事（§9.6） |
| 一致性 | 架构的规则在 §5.3 是索引的检查，对 `spec = "3"` 应当是格式的规则 | 改：写进规范，检查器与客户端共同执行，范围与 §5.3 相同（§9.9） |
| 一致性 | §8.4 的每节点解压目录与 §9.7 的落地是两个答案 | 改：前者服务 `spec` 为 1、2，后者之后不再有那个目录；#736 的行写明两处 |
| 一致性 | 索引的检查与 libxpkg 的检查点都叫 L1 到 L4 | 改：索引的检查改叫 K1 到 K4 |
| 一致性 | 第五版把迁移工具放在索引、把接口的文档写成索引的 `xpackage-v3.md` | 改：工具与清单在 libxpkg，规范在 libxpkg 与 xlings 两处（第七版，§15 第 20 项），索引只留自己的约定 |
| 复用 | 架构迁移与 `spec = "3"` 迁移各有工具与覆盖表 | 改：覆盖表一张、两列；一个 recipe 只动一次、装一次。两个工具不合并：一个要下载与安装，一个只改文字 |
| 复用 | 检查器若写在索引里，就是加载器之外第二份"什么是合法的 recipe" | 改：在 libxpkg，xlings 与索引都调用它（§9.9） |
| 复用 | 落地要不要新写一条路径 | 不用：没有 hook 的包今天走的就是这段代码，只是把它挪到 hook 之前（§9.7） |
| 简洁 | 新增的概念有没有可以不要的 | 新增的词只有三个：动作、询问、声明；新增的字段只有 `strip` 与模板声明；新增的命令只有 `index check`。`replace` 选项与 `capture` 选项是为了不另起两个函数 |
| 简洁 | 一次给 `spec = "3"` 装了三样东西（环境、落地、架构枚举） | 保留：三样都是"这个 recipe 承诺什么"，合在一起每个 recipe 只迁一次；分成三个 `spec` 要迁三次 |
| 兼容 | 发布后发现新环境有缺陷怎么办 | 记：兼容层还在，出问题的 recipe 可以单独改回 `spec = "2"`，不必回退索引；这是保留兼容层的第二个用处（§18） |
| 兼容 | 子索引用的是兼容层的名字与构建脚本 | 不受影响；各一个 PR，过渡期声明与脚本并存（§9.8、§12） |
| 兼容 | 规范换了文件名，外面的链接会断 | 改：原地址留一个指向新规范的页面（行为变化里写了） |
| 生态 | mcpp-index 的 recipe 是 `spec = "1"`，另有自己的契约文件（`index.toml`） | 记：不受影响。`index-compat.json` 的 `requires` 本来就按使用者分键，两份契约可以合成一种；不在本轮 |
| 生态 | 第三方的 recipe 作者得到什么 | 一份规范、生成的接口参考、检查器、迁移工具、一致性夹具；以前这五样一样都没有 |
| xlings 规范 | "命令树是设计出来的" | `xlings index check` 放在已有的 `index` 之下，随规范、生成的参考与测试一起加；已确认（§15 第 19 项） |
| xlings 规范 | "外部程序出自一张表" | hook 里的命令是 recipe 自己的，不进表；`fs.extract` 让 39 处 `tar` 与 `unzip` 变成进程内完成，hook 对宿主工具的依赖减少 |
| xlings 规范 | "默认输出是结论与下一步" | `hook-call-failed` 默认只有一行计数；动作失败的错误带动作、路径、原因与行号 |
| xlings 规范 | 规范用英文 | `docs/specs/xpkg.md` 用英文；本文是设计记录，用中文 |
| 结构划分 | 每个仓库各管什么是否清楚 | libxpkg：格式的读取、hook 的环境、规范的文字、清单、夹具、检查器、迁移工具。xlings：生命周期、落地、结果、命令，以及规范在站点上的那一份。索引：约定、recipe、`libs/`。规范有两处存放、一处修改；其余没有一样东西有两个出处 |
| 跨平台 | 列表形式在 Windows 上遇到批处理 | 报错并提示用字符串；不提供"看起来能用"的转义 |
| 跨平台 | `fs.symlink` 在 Windows 上可能没有权限 | 是一次失败，错误里带系统的原因；recipe 需要时用 `pcall` 并改为复制 |
| 性能 | 落地、模板拼接、检查器的开销 | 落地比现在少一次搬运；拼接在内存里；`spec` 随上下文给执行器，不重复读文件 |
| 稳定性 | 三个仓库的 PR 都变大了（libxpkg 10 个检查点，xlings 17 个，索引 12 个），发布仍是一次 | 记为风险（§18 第 12 项）；检查点的顺序保证不挡发布的排在后面 |
| 稳定性 | 没有原型的东西变多了（接口、落地、模板、检查器） | 改：先探路（§9.6"迁移"末尾），用 draft PR 的 CI；探路的结果先于工具 |
| 稳定性 | 基线的 `unit (1/6)` 仍未分析 | 记：留在 X0，按此前的结论 |
| 用户体验 | 旧客户端的用户看到什么 | 实测 E18：一行说明索引需要哪一版、自己是哪一版，然后在旧快照上照常工作；`self update` 带它到 N |
| 优雅 | 整个方案能不能用一句话说完 | 能：一步的结果只有四种；recipe 里的一次调用只有三种；宿主做事、recipe 声明；每条规则只写在一个地方 |

## 18. 风险与评审结论（第三版起）

| # | 风险 | 评审结论 | 文中的落点 |
|---|---|---|---|
| 1 | 规则 M 的面是否足够，只有全量安装能回答 | 接受。发布前以全量安装为门槛 | §5.4、P4 |
| 2 | 51 加 19 个 recipe 的判断原写作"人工逐个" | 不需要人工：可以直接验证。改为工具三步（读字节、两种架构上安装过规则 M、探测上游的另一种架构），人只看工具列出的少数几个 | §2.7、§5.6、P1、P3 |
| 3 | xlings 的 PR 体量大（现在是十七个检查点、十二处行为变化），发布是一次性的 | 接受。以 commit 为检查点推进，每个检查点带自己的单元与 e2e 测试，CI 通过即可 | §11、§12 |
| 4 | mesa 在 aarch64 上的配置、`libllvm` 的构建时长没有测过 | 接受。在 P5 这个检查点里测，结果决定后续 | §5.8、P5 |
| 5 | 基线的两处失败原因未查 | `hookrt` 已查：是夹具，X0 里是小改动（§2.8）。`unit (1/6)` 那一处留到实施时在 X0 里分析 | §2.6、§2.8、X0 |
| 6 | `ld` 包装器那一节引用的设计可能过时 | 已重测并改写，原型九行通过 | §2.8、§8.3 |
| 7 | 新发现：hook 里失败的调用不让 hook 失败 | 第四版提出逐个改老名字的含义；评审后改为重新设计接口（第五版），老名字不动 | §2.9、§9.6、L5 到 L7、X12、P10 到 P12 |
| 8 | recipe 迁移的量：602 个 recipe 与 15 个 lib，约五千处调用；迁移工具还没有写，能自动改写的比例未知 | 接受。`libs/` 本轮迁完；recipe 分批、以安装为门槛，没迁完的留在旧写法照常工作，不挡发布。落地模型让 171 个 hook 直接删掉，量是减少的 | §9.6"迁移"、§9.7、P10、P11 |
| 9 | 全量安装没有 macOS 的腿 | 接受：给全量安装加 macOS 的腿；它装不到的 recipe 留在旧写法 | P4、P11 |
| 10 | 提高索引下限后，低于 N 的客户端停在上一个快照 | 接受。已有机制的预期用法，实测 E18；`self update` 的路径由 P12 的判据验证 | §2.9、§9.6"旧客户端"、P12 |
| 11 | 新接口、落地模型、模板声明、检查器都没有原型 | 已确认：用 draft PR 的 CI 探路：十个 recipe 手工迁移并安装，结果先于迁移工具。最可能被调整的三条：`fs.move` 的目标规则、`strip` 的严格程度、落地之后再调整是否够用 | §9.6"迁移"末尾、§11、§14 |
| 12 | 三个仓库的 PR 都变大了：libxpkg 10 个检查点，xlings 17 个，索引 12 个 | 接受（§15 第 22 项）。每个检查点能独立构建并通过测试；不挡发布的（#593、#592、规范文档、`index check`）排在最后，时间不够时可以整块留到下一版而不影响前面的 | §12 |
| 13 | xlings 发布到索引合并之间，N 的用户面对的是未迁移的索引 | 评审时未单独讨论，按文中做法。`spec` 为 1、2 的 recipe 行为不变，规则 M 如实拒绝；发布、镜像、合并连续进行 | §12 |
| 14 | 发布后发现新环境或落地有缺陷 | 评审时未单独讨论，按文中做法。出问题的 recipe 单独改回 `spec = "2"`（兼容层与旧的落地路径都在），不需要回退索引或客户端 | §9.6、§9.7 |
| 15 | 规范合并时漏掉旧文档里的某条规则 | 评审时未单独讨论，按文中做法。合并按章对照三份旧文档，一致性夹具覆盖每一章；旧文档保留并指向新规范 | §9.9、X14 |
| 16 | 子索引不在我们一次能合并的范围里 | 评审时未单独讨论，按文中做法。它们不改也照常工作（兼容层与构建脚本都冻结保留）；声明与脚本并存到它们各自提高下限 | §9.8、S1、S2 |

## 19. 实施：拆分、依赖与联调（第八版）

§12 是任务清单。这一节回答三件事：每个检查点在各个视角下必须守住什么；跨仓库怎样在发布之前一起验证；
从开始实施到生态验证完毕的顺序。

### 19.1 各视角的约束

每一行是一条约束，后两列是它落在哪些检查点、怎样被验证。一个检查点合入 PR 的条件是它涉及的行都满足。

| 视角 | 约束 | 检查点 | 验证 |
|---|---|---|---|
| 架构 | 依赖只向下：libxpkg 不知道 xlings；xlings 的 `modules/` 不依赖 libxpkg；解压、ELF 检查、工具表都在 xlings，libxpkg 只经端口调用 | L5、L8、X3、X13 | `tools/lint_layer_deps.sh`；libxpkg 单独构建与测试 |
| 架构 | recipe 的环境与模块的实现分开：`spec = "3"` 的 recipe 与它导入的索引 lib 在自己的环境表里运行，只看得到清单里的名字；模块的实现在全局环境里，内部仍可用兼容层的函数（实现细节，可以逐步收掉） | L6、L7 | `HOOK-ENV-SPEC3`、`XPKG-API-MANIFEST` |
| 架构 | 一个名字只有一个出处：架构身份在 libxpkg；接口清单在 libxpkg；规范在 libxpkg 修改、xlings 带入 | L1、L9、L10、X14 | 单元：`build_arch()` 的每个值在身份表里；清单与环境逐名相同；两份规范逐字相同 |
| 稳定性 | 每个检查点单独构建、单独通过它那一层的测试；新用例先在未修改的构建上失败 | 全部 | PR 的 CI；`X1` 在改动之前提交 |
| 稳定性 | 硬规则（规则 M、空载荷规则）变硬之前，全量安装在两种架构上零误报 | X6、X12、P4 | 索引 draft PR 上的全量安装，用 xlings PR 的构建 |
| 稳定性 | 兼容层的行为逐个名字冻结：E8、E16、E17 的返回值不变 | L6 | `HOOK-COMPAT-FROZEN` |
| 优雅简洁 | 新增的概念只有 §17 第六版自审列出的那几个；其余都复用已有机制（`spec`、`index-compat.json`、`xlings_run`、`index_ref`、命令参考的生成方式） | 全部 | 自审（§19.6） |
| 用户体验 | 默认输出是结论与下一步；失败说出动作、路径、原因与行号；说明在总结之后；发现只占一行计数 | X4、X6、X7、X12 | fresh 套件的断言；`HOOK-NOTICE`、`HOOK-ACTION-FAILS` |
| 兼容性 | 老名字的含义不变；`spec` 为 1、2 的 recipe 只有 §9.6 列出的两处变化；已安装的包（载荷里的 `.xpkg.lua`）照常移除 | L6、X12 | `HOOK-COMPAT-FROZEN`；已安装包在新客户端上的移除用例 |
| 兼容性 | 旧客户端遇到新索引：回到上一代快照并说明原因；单个 `spec = "3"` 的包：逐包拒绝并提示更新 | P12、X12 | E14、E18 的情形写成用例；`SPEC-GATE` |
| 跨平台 | 每个新动作与命令的两种写法在 Linux、macOS、Windows 上各有用例；Windows 上的差别（`chmod` 不做事、批处理拒绝列表形式）写进清单的"平台"一列 | L5、X13 | libxpkg 三平台 CI；xlings 三平台 CI |
| 跨平台 | aarch64 上的行为在原生机器上验证，不靠角色对调 | X6、P4、P5 | `ubuntu-24.04-arm` 的 xlings CI 与全量安装腿 |
| 一致性 | 四个词（结果、发现、说明、warn）与三种调用（动作、询问、声明）在代码、规范、文档里用同一套名字 | 全部 | 规范与文档的检查；自审 |
| 一致性 | 规范、生成的接口参考、命令参考与代码同 PR 更新，由检查保证没有过期 | L10、X14、X15 | `docs/generated/` 的过期检查；规范的逐字检查 |
| 无感升级 | 见 §19.4 | X5、X12、P12 | §19.4 的每一行有一个用例或一次真实验证 |

### 19.2 依赖与车道

```
车道 A（libxpkg）  L1 ─ L2                L5 ─ L6 ─ L7 ─ L8 ─ 探路 ─ L9 ─ L10 ─ 0.0.68 ─ M1
                   L3      L4 (L1)
车道 B（xlings）   X0 ─ X1 ─ X2 ─ X4 ─ X5 / X7 / X10 / X11      X3（跟随 A 的 git rev）─ X6 ─ X12 ─ X13 ─ X14 ─ X15 ─ X16
                         X8、X9 独立
车道 C（索引）     P1 ─ P2、P3 ─ P6、P7、P8 ─ P5（res-build，长）        P4（要 B 的构建）─ P10 ─ P11 ─ P9 ─ P12
车道 D（其余）     mcpp C1（在 P6 之后）；子索引 S1（在 N 之后）
```

- A 与 B 的前半段互不依赖，并行推进；X3 起 xlings 的 PR 用 libxpkg PR 头的 `git rev`。
- 探路（§9.6"迁移"末尾）横跨三条车道：A 的 L5 到 L8、B 的 X12 与 X13 的最小实现、C 的十个 recipe。它的结果先写回本文，再写迁移工具。
- 关键路径：A → X3 → X12/X13 → P4 → P10/P11 → 发布 → P12。P5 的资产构建耗时最长，尽早在 C 的分支上触发，不在关键路径上等待。

### 19.3 跨仓库的联调

| 方向 | 怎样做 | 已有的机制 |
|---|---|---|
| libxpkg → xlings | xlings 的 PR 在 `mcpp.toml` 里临时用 libxpkg PR 头的 `git rev`；libxpkg 发布后换成版本号 | 已定（§9.5）；`release.yml` 拒绝带 git 依赖的发布 |
| xlings → 索引 | 索引的全量安装与改动 recipe 的安装测试增加输入 `xlings_sha`：每条腿按这个提交找到 xlings 对应 workflow 的运行，下载它上传的构建（`xlings-linux-x86_64`、`xlings-linux-aarch64-candidate`、`xlings-macosx-arm64`、`xlings-windows-x86_64`） | 索引的 `luban-agent-workspace.yml` 已用 `gh run download … -R openxlings/xlings` 取 xlings 的构建，默认的令牌即可 |
| 索引 → xlings | xlings 的 fresh 套件用 `index_ref` 指向索引的 PR 分支，验证新客户端与迁移后的索引 | `xlings-ci-fresh-install.yml` 的 `index_ref` 输入 |
| 索引 → mcpp | mcpp 的 PR 用索引分支上的 binutils 修订验证 `XLINGS_SUBOS_LD_PATHS=0` | mcpp 的 CI 读索引的方式（实施时核对） |
| 索引 → 子索引 | 子索引的 PR 在 N 发布之后用 N 验证模板声明；旧客户端的行为用 2026.11.23.1 验证 | 子索引各自的 CI |

draft PR 只用来取得结果（§11）；结论写回本文，正式的 PR 仍是每个仓库一个。

### 19.4 无感升级：每一类用户看到什么

| 用户 | 升级前后 | 保证它的机制 | 验证 |
|---|---|---|---|
| 用 N 的用户，索引还没合并 | `spec` 为 1、2 的 recipe 行为不变；说明开始出现在总结之后；架构不符的载荷被规则 M 拒绝 | 兼容层冻结；规则 M 在 P4 里零误报 | fresh 套件在 PR 上的运行 |
| 用 N 的用户，索引合并之后 | 迁移过的 recipe 走新环境与落地模型；已安装的包不重装（`revision` 不变）；移除照常 | 迁移不提高 `revision`；移除读载荷里安装时的 recipe | 索引合并后的真实验证（§19.5 的最后一步） |
| 低于 N 的客户端 | `xlings update` 回到上一代快照并说一句为什么；`xlings self update` 把它带到 N | 快照下限（E18）；`self update` 读最新快照，`xlings` 的 recipe 保持旧写法 | P12 的判据：用 2026.11.23.1 实测 |
| 已有声明写着 store 里没有的版本（E4 留下的） | 第一次运行 N 时被改写为视图所指的版本，只改一次、有记录 | `xlings.home.format` 的一个步骤，不提高布局号 | `VERSION-DECLARED` |
| 本地 recipe 与第三方索引 | 不改也照常工作；写成 `spec = "3"` 的在旧客户端上被逐包拒绝并提示更新 | 兼容层与构建脚本的环境冻结；`spec` 的上限 | E14 的用例；子索引的 PR |
| 脚本与 CI 里调用 xlings 的人 | 被请求的节点被拒绝或失败时退出码非零（以前可能是 0）；其余输出不变 | §12 的行为变化列表写进发布说明与升级规范 | fresh 套件的断言同 PR 更新 |

没有一项要求用户做事：升级就是 `xlings self update`，home 的改写自动完成，旧快照自动回落。

### 19.5 顺序与收尾

1. **基线（X0）**：xlings PR 的第一个 commit，`main` 的两处失败修好，标签与 minos 断言带入。
2. **车道 A 与 B 并行**：libxpkg 的 draft PR（L1 到 L8，三平台 CI）；xlings 的 PR（X1、X2、X4、X5、X7 到 X11）。
3. **探路**：X3 换上 libxpkg PR 的 `git rev`，X12、X13 的最小实现；索引的 draft PR 里十个 recipe 手工迁移，用 xlings PR 的构建安装。结论写回 §9.6、§9.7 与本节。
4. **补全**：L9、L10；X6、X12 到 X15；索引的 P1 到 P11（P5 提前触发）。P4 在两种架构上零误报。
5. **自审**：按 §17 的视角对三个 PR 再审一遍（§19.6），结果写进本文。
6. **libxpkg 发布 0.0.68，mcpp-index 登记**；xlings 的依赖换成版本号（X16）。
7. **合入与发布**：xlings 的 PR squash 合入（bypass），取消 `main` 上排队的 CI，先跑 `release.yml`；资产一出来就在本机把 GitCode 的镜像补齐并逐个校验；然后重新触发 `main` 的 CI。
8. **索引**：P12 把下限改为 N，PR 合入；版本 bump 按 AGENTS.md 的流程。子索引与 mcpp 的 PR 随后。
9. **真实验证**：干净的 home 上用 N 安装迁移过的 recipe、旧客户端的回落、`self update` 到 N、aarch64 上的 mesa（`ubuntu-24.04-arm`）。
10. **关闭 issue**：发布之后用 speak-agent 逐条评论（改了什么、在哪个版本、怎样验证）并关闭；#741 的对应条目同步。

### 19.6 实施中的记录

实施中发现与本文不符的地方，在这里记一行，并改正文中对应的地方。

| 日期 | 检查点 | 发现 | 处置 |
|---|---|---|---|
| 2026-11-27 | X2 | hook 的上下文从未填 `arch`（`_RUNTIME.arch` 为空），`os.arch` 在 hook 里没有绑定 | 单一构造函数填 `arch`（`platform::build_arch()`）；`pkginfo.arch()` 取代 `os.arch` |
| 2026-11-27 | X2 | `system.xpkgdir()` 对包的 hook 是 recipe 所在目录，对脚本是载荷的 store：同一个名字两种含义 | 保持原样（兼容层冻结），`HookScope::xpkg_dir` 写明；规范里写明 |
| 2026-11-27 | L6 | 兼容层的 `try` 读 `catch[1]`，索引里写的是 `catch = function`：出错时 `try` 自己报错 | 冻结不改；迁移把 `try` 换成 `pcall`（§9.6 的表） |
| 2026-11-27 | L7 | 模块的实现运行在全局环境，可以用兼容层的名字 | 按 §19.1：实现细节，可以逐步收掉 |
| 2026-11-27 | L5 | `fs` 的动作返回 `true`：旧 recipe 里 `if not fs.symlink(...)` 的写法仍然成立 | 规范写明动作的返回值没有含义 |
| 2026-11-27 | X3 | 边界协议没有带条目的 `floor`：在 worker 里读的 recipe 丢了下限 | X3 补上，连同 `strip` 与 hook 的说明、失败调用、`elfpatch` 结果 |
| 2026-11-27 | L8 | clang 拒绝 `"\x01absent"`（十六进制转义吃掉后面的字母），gcc 接受：L8 之后 libxpkg 的 macOS 与 Windows 任务没有真正跑过 | 改正；三平台通过（`406f3b2`） |
| 2026-11-27 | X13 | 载荷里保存的 recipe（`.xpkg.lua`）是文件的拷贝：有模板声明的子索引，移除时没有模板里的 hook | 保存 `recipe_text`（文件加模板） |
| 2026-11-27 | 探路 | 结果见 §9.6"探路的结果"；`strip` 与 `fs.move` 的规则不变 | — |
| 2026-11-27 | 探路 | XLINGS_RES 的字符串条目（`["4.0.2"] = "XLINGS_RES"`）没有校验和，也不说明架构，检查器报 `arch-unstated` | P2、P3 取 res 资产的校验和写成按架构的形式 |
| 2026-11-27 | 探路 | `spec = "3"` 的 `install()` 的工作目录仍是下载目录 | 规范不承诺工作目录，hook 只用 `pkginfo` 给出的路径 |
