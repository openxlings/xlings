# Luban OS 的生态设计：一切是 xpkg，系统、桌面与进入方式，能力契约，以及 2026.10.17 实测的问题

日期：2026-10-18　状态：修订 6，待评审　缘起：2026.10.17.1 在一台 Linux 宿主上的实测（core 的 `aabb`，desktop 的
`luban-desktop-1`、`desktop-2`、`d1`，Luban OS 应用 2026.10.15.4 与 2026.10.17.1），其间关于发行版、桌面与进入方式的讨论，
以及 §19 的复现与调研。

范围：xlings（xlings 的 xim 与 subos、luban、Luban OS 应用、`luban/kits`）、xim-pkgindex（editions、桌面组件、运行时、luban-os 的配方、
资源工厂）。前置设计：`2026-10-12-luban-os-architecture-design.md`（下称 **A**）、`2026-10-16-luban-inside-experience-and-ecosystem-design.md`
（下称 **L**）、`2026-10-16-ecosystem-implementation-plan.md`（下称 **P**）、`subos-as-xpkg-design-2026-05-16.md`、`graphics-stack-spec.md`。

**修订记录**
- 修订 6（本版）：**包的形态与演进**（§4.6）：原子包、整包、运行时、组合包都是 xpkg，名字与约定稳定、实现可换，按依据选、随需要演进，
  不走极端；本轮各组件的形态与理由；在 Luban 系统里把包的 `lib/<子目录>` 与 `libexec/` 也放到 FHS 路径；整包的私有树改放
  `libexec/<包名>/`；`luban:` 的含义改为"与 Luban OS 强相关或偏向 Luban 生态，别的系统也可以用"；Arch 快照导入列为按需添加的工具。
  评审后补充：KDE 走 opensal 的原生适配器、适配器三层可复用（§13.3）；能力词汇标准化为 `luban/specs/opencaps`（§9）；许可证未修改即给地址；
  E8 定因与 B22（转达 xlings 选用旧版本的原因）。多视角自审见"自审（修订 6）"。
- 修订 5：统一视角"系统 / 桌面 / 进入方式"；`luban:` 命名空间与保留规则、迁移；`--xpkg` 为唯一的通用入口，`luban new` 的设置项与
  `luban config` 同一套，去掉 `--with`；不新增"类型"字段；一个 desktop edition 加 `--desktop`；图形体验分级（含单应用窗口与"Luban OS 窗口作外壳"）；
  E21（xdesk 终端应用的标题）；默认简单、渐进披露、官方即示范。多视角自审的修正见 §0 之后的"自审"。
- 修订 4：范围不收窄；LXQt、KDE 的实测量级；共享运行时；E13 依赖的量级与归属。
- 修订 3：复用清单已有的 `from` 与 `subos_kind`；E13 在 GPU 上验证；能力契约建在已有的能力报告上；`enter` 的默认值沿用现有行为。
- 修订 2：E1、E12、E17 根因；E13 宿主实测。

## 0. 核心点

1. **一切是 xpkg**：系统（发行版）、桌面、工具集、运行时都是 xpkg 包。不新增包类型、不新增"类型"字段：系统由已有的 `type = "subos"` 与
   `subos_kind = "rootfs"` 认出，桌面由"装上后提供会话文件"的约定认出；`categories` 只用来找（保留值 `luban-desktop`），不决定行为。
2. **命名空间说明归属**：与 Luban OS 强相关或偏向 Luban 生态、由官方维护的放 `luban:`（`luban:core`、`luban:desktop`、`luban:kde`……），
   通用软件留在 `xim:`；`luban:` 的包别的系统也可以用。**官方短名就是 `luban:` 里的名字**，代码里不再写死任何名单（包括官方的）。
   `luban:`、`xim:` 是保留命名空间：只管谁能发布，不管谁能用。
3. **一个系统，三条互不相干的轴**：系统（它是什么：谱系、包、策略、身份；只沿谱系升级）、桌面（用什么界面操作：组件，可同时装几个，随时切换）、
   进入方式（`enter`、`mode`）。"桌面 OS"不是另一类 OS，而是默认进桌面的系统；发行版可以自带桌面，自带的桌面仍是可切换的组件。
4. **命令：`--xpkg` 是唯一的通用入口**（可重复，任何 index、任何包；第三方首次确认）。`--edition <短名>` 是官方系统的快捷方式；
   其余单独的选项**只给配置项**，且与 `luban config` 同一套（`--desktop`、`--enter`、`--mode`、`--shell`……）。去掉 `--with`。
   唯一的便利：`luban new --desktop kde` 在系统里没有该会话时自动装官方的 `luban:kde`。
5. **默认简单，细节可达**：L0 快速开始两个问题 → L1 短名与设置项 → L2 `--xpkg` 组合 → L3 进系统用 xlings → L4 写自己的包。
   **每类都有官方版，且只用公开机制，所以官方即示范**。
6. **图形体验分级**：T0 单应用窗口（`luban run X --display window -- firefox`）、T1 仅合成器（Luban OS 窗口作外壳）、T2 labwc 最简、
   T3 LXQt、T4 KDE，以及 Luban 自己的 xdesk。都是 desktop 组件，会话文件里声明外壳由谁提供。
7. **包的形态按情况选，随需要演进，不走极端**：原子包（一个上游项目一个包）、整包（预先组合好、带私有库）、运行时（被多个整包共用）、
   组合包（只有依赖）都是 xpkg。用户与其他包只依赖**名字与约定**，形态可以换：`luban:kde` 今天是"整包 + Qt 运行时"，将来有复用需求时
   拆成原子包、自己变成组合包，用户无感。本轮：宿主图形栈原子包（已有）；labwc、foot 整包；LXQt、KDE 整包共享 Qt 运行时
   （实测共用 341 MB）。架构能力先做好：约定独立于形态、两种 ABI、系统内的 FHS 投影、名字不变的切换、按需添加的工具。
8. **KDE 走 opensal**：guest 里加 KDE 原生适配器（KWin 的虚拟显示器 PipeWire 流 + `fake_input`，即 KDE 自己 krdp 的做法），宿主不变；
   适配器按画面 / 输入 / 会话获取三层拆开，PipeWire + EIS 这一族将来复用到 GNOME 等。
9. **能力契约取代版本比较**（建在已有的 `luban.service.capabilities` 上；词汇标准化为 `luban/specs/opencaps`）；**宿主读环境只经服务层**；**会开窗口的包必须接入图形栈**。
10. **E13（风扇）根因与修法已在 RTX 4080 上验证**：luban-os 没接图形栈而软件渲染；接入后 CPU 47% → 2%。依赖（下载约 72 MB、落盘约 243 MB，
   home 内共享一次）归 luban-os。
11. **交付**：xim-pkgindex 一个 PR（先合）、xlings 一个 PR（后合，随发布），commit 作检查点；资源包由 `tools/res/build.sh` 构建发布。

### 自审（修订 6，多视角）

| 视角 | 发现 | 修正 |
|---|---|---|
| 兼容 | 系统内若开始投影 `libexec/`，labwc-desktop 现在的私有树 `libexec/lib`、`libexec/<程序>` 会进 `/usr/libexec`，两个整包都有 `libexec/lib` 时冲突 | 整包的私有树改放 `libexec/<包名>/`，按"一个包的目录整体归它"投影（与 share 的规则相同）；labwc-desktop 出新版本时迁移（§4.6） |
| 兼容 | 把 `lib/<子目录>` 合并进 `/usr/lib` 会遇到 `lib/pkgconfig`、`lib/python3.x`、`lib/gcc` 等 | 按文件合并、冲突走现有的认领规则并报出；`lib/modules` 维持现状；新增测试覆盖（`LUBAN-FHS-PATHS`） |
| 用户体验 | 卡片上的大小若只算组件本身，第一次装 KDE 时实际下载远大于卡片所写 | 卡片与确认界面写"需要下载"：组件加尚未装的运行时与依赖，已装的不计 |
| 维护 | 整包内的库不随 xim 的更新而更新，安全修复要重建整包 | 工厂的来源说明列出整包内每个库的版本；选形态时把"需要频繁单独更新"作为倾向原子包的依据（§4.6） |
| 维护 | Qt 有两条线：`xim:qt`/`xim:qt-base`（官方二进制，给开发者的 SDK）与 Qt 运行时（给 LXQt、KDE） | 两者用途写进文档；KDE 用到 Qt 私有 API，必须用与它同源的 Qt，所以不混用 |
| 性能 | 原子包的 RPATH 是依赖闭包，KDE 规模时可能有几百个目录，启动时大量查找 | 只在把某组拆成原子包时才出现；拆之前实测启动时间，系统内可改用 `/usr/lib` 与库缓存（`library_cache.cpp`） |
| 许可证 | 重新分发 Alpine（以及将来的 Arch）中的 GPL 二进制要能提供对应源码 | 未修改的：来源说明给出发行版源码包与上游源码的地址即可；定制构建的：改动（APKBUILD、补丁）就在本仓库 `tools/res` 中公开，加上游地址 |
| 表述 | 修订 5 的口径把"全部原子化需要新做的包"写成了"缺多少"（labwc 一套 59 个） | labwc 整包已可用、不缺；数字只表示原子化的成本（§4.6） |

### 自审（修订 5，多视角）

| 视角 | 发现 | 修正 |
|---|---|---|
| 分发与信任 | `replaces`（新名字接替旧名字）若任何包都能声明，社区包可以"接替" `luban:desktop` 劫持升级 | 只认同一 index 内的 `replaces`（§15.1） |
| 分发与信任 | 命名空间是配方里自己写的字段，任何 index 都能写 `namespace = "luban"` | 保留命名空间由官方 index 的元数据声明，xlings 通用地执行，不认识 Luban（§4.2）；已核实 awesome、scode、d2x 都不用 `xim`/`luban` |
| 用户体验 | 修订 4 让 labwc 背上 273 MB 的运行时，"最简"不最简；默认 desktop 也会因 Xwayland 背上 mesa | 运行时只给 Qt；labwc 用 pixman 版 wlroots、Xwayland 不带 glamor、foot 独立（§13.5） |
| 用户体验 | desktop-xdesk 合并进 desktop 后，没手动设过桌面的环境升级会从画布变成经典 | 升级时预设默认值变化，把旧的有效值写进用户设置（§15.2） |
| 用户体验 | 会话名改了（`labwc-desktop` → `labwc`），已存的设置会指向不存在的会话 | 会话文件声明别名（§15.3） |
| 分层 | 保留命名空间、`replaces`、`info --json` 都落在 xlings | 三者都是通用机制（不出现 Luban 的名字）；Luban 的语义只在 luban |
| 命令设计 | 单应用窗口若开新命令需先讨论 | 用已有的 `luban run` 加已有的 `--display window`，不新增命令 |
| 社区 | 只有约定没有工具，作者难以自查 | `luban new t --xpkg local:x --dry-run -v` 给出角色、谱系、能力与约定的检查结果；运行时配套构建容器 |
| 测试 | KDE 的端到端下载大、耗时长 | 单独一条 CI 线，只在 main 与带标签的 PR 上跑（`tests/lanes.toml`） |
| 跨架构 | 运行时与桌面包只说了 x86_64 | 工厂按架构构建（x86_64、aarch64，与 labwc-desktop 相同）；Alpine 两种架构都有 Plasma 与 LXQt |

## 1. 证据

| # | 现象 | 根因 | 依据 |
|---|---|---|---|
| E1 | 新建的 core、desktop 登录 shell 都是 `/bin/sh`，`$SHELL` 也是 | 违反已有要求 `LUBAN-SHELL`。`best_shell` 只在 root 行不存在时生效；创建时每装一个包就刷新一次根，第一次刷新时 fish 还没装，root 行写成 `/bin/sh`，之后保留。测试的 fixture 没有 fish | `src/core/xself/init.cpp:693`、`modules/subos/src/views/root/root_etc.cpp:166,201`、`tests/e2e/test_luban.cpp:376` |
| E2 | desktop 的登录用户 `lubanoser` 也是 `/bin/sh` | 新用户照抄 root 的 shell | `luban/os/service/src/service.cpp:357` |
| E3 | 改了 shell 后进入，`$SHELL` 仍是旧值 | 加入运行中的会话时环境取会话启动时的 | `modules/subos/src/session/session.cpp:864` |
| E4 | 两个终端进入同一环境，先进入的 `exit` 后挂住；Ctrl-C 两个都退出 | 先进入者是会话宿主；bwrap 留在该终端的前台进程组，Ctrl-C 杀死它 | `session.cpp:1330,326`、`modules/confine/src/backends/linux_bwrap.cpp:89` |
| E5 | `luban enter <desktop>` 不带 `--display` 进入终端控制台 | `o.display` 为空时只走终端 | `luban/os/cli/src/cli.cpp:1115`、`luban/os/vm/src/host.cpp:950` |
| E6 | 应用 2026.10.15.4 打开 2026.10.17 的环境只显示控制台，无提示 | 旧应用按旧布局读，10.16.1 起根的 `usr` 在宿主上是空挂载点，读到空即"没有桌面" | `git diff cf348cd0 84652775 -- luban/os/service/src/service.cpp` |
| E7 | 桌面失败后退回控制台，原因不显示 | 退回时 `notice` 被清空 | `apps/luban-os/src/app.cpp:1913,3239` |
| E8 | `desktop-2` 的桌面无鼠标指针 | 根里没有光标主题：它是 `subos:luban-desktop@2026.10.15.1`（早于 10.16.1 加的光标主题）。luban 不钉版本、取 index 的最新（`edition_ref`）；建时 index 尚无 10.16.x，或建它的 xlings 早于 10.16.1——10.16.x 的 `min_client` 为 2026.10.16.1，10.16.2 起的 xlings 会改用它能用的最新版本并给出一句 `passedOver` 说明（`src/core/xim/catalog.cpp:172-195`），**luban 没有把这句转给用户** | `luban status desktop-2 --json`（用户提供）；`xim-pkgindex/libs/luban_desktop.lua:56,106` |
| E9 | 应用列表的说明为 `xlings-data-xpkgs-…-foot` | 应用 ID 用 `fs::relative`，解析了软链接 | `luban/kits/xdesk-base/src/apps/apps.cpp:112` |
| E10 | 终端窗口的标题停在 `foot` | 登录 shell 是 sh（E1），不改标题 | — |
| E11 | 应用的设置页没有"桌面"一项 | 设置页自己扫 `<usr>/share/wayland-sessions`，未经 `physical` 换算 | `apps/luban-os/src/app.cpp:2092` |
| E12 | labwc 背景菜单只有 Reconfigure / Exit | labwc 0.7.2 上游缺陷：找 `menu.xml` 时第一个路径打不开就 `return` | labwc 0.7.2 `src/menu/menu.c:655-678`；§19.2 |
| E13 | labwc 桌面一动指针风扇就响 | 宿主上的应用没接图形栈，静默软件渲染；桌面视图每次指针移动重画整窗 | §19.3、§19.5 |
| E14 | `--edition labwc-desktop` 报 xlings 的内部错误 | 非官方名原样传给 `xlings subos new --from` | `luban/os/service/src/service.cpp:154` |
| E15 | 快速开始的"经典桌面"标签与实际会话不符 | 卡片写死在应用里 | `apps/luban-os/src/app.cpp:152` |
| E16 | 控制台：程序开启鼠标模式后鼠标无效；无右键、中键粘贴、双击选词 | 鼠标模式下既不选择也不转发 | `apps/luban-os/src/app.cpp:1537` |
| E17 | fish 的欢迎语压住 Luban 的欢迎信息 | `CSI ? u` 被当作恢复光标 | `luban/kits/xdesk-base/src/term/term.cpp:333`；§19.1 |
| E18 | `XLINGS_ENTER_QUIET=1` 时仍有 `▸ entering subos` | 未定 | §19.1 |
| E19 | 另一个应用窗口空闲 6–10% CPU | 违反 `LUBAN-OS-PERF`（静止的窗口不画）；软件渲染放大 | §19.3 |
| E20 | 设置的会话不存在时悄悄进控制台 | `desktop_session()` 返回空，与"没有桌面"不可区分 | `apps/luban-os/src/display.cpp:21-26` |
| E21 | xdesk 里从启动器打开的 nvim、htop 等，窗口标题与窗口 ID 都是 `foot` | xdesk 以 `foot <命令>` 启动 `Terminal=true` 的应用，不传应用名；终端写死为 foot（来自 labwc-desktop 包，隐藏的跨组件依赖） | `luban/kits/xdesk-compositor/src/compositor/server.cpp:505`、`compositor.cppm:18`、`server.cpp:430`（标题取 `title`，否则 `app_id`） |

## 2. 原则

1. **一切是 xpkg**：分发、版本、信任、更新都走 xpkg 与 index，不另建渠道，不新增包类型。
2. **官方没有特殊路径，所以官方即示范**：官方包只用公开机制；社区能做的与官方相同，官方版是每一类的参考实现。
3. **不写死**：名单、卡片、终端、会话都来自数据；官方的短名来自 `luban:` 命名空间本身。
4. **默认简单，细节可达，复杂度渐进披露**：默认路径一步得到好体验；细节控制存在，但不出现在默认路径上。
5. **复用已有的格式**：`from`、`subos_kind`、`min_client`、`categories`、`settings.json`、能力报告、`--display` 都已存在，先扩展它们。
6. **能力而非版本**：两方能否一起工作，由能力对账决定。
7. **读不到不是空，降级要说出来**（AGENTS.md）。
8. **宿主读环境只有一个入口**：服务层。第三方只有一个入口：`--xpkg`。
9. **先复现，再修**：修法带上同一个复现作为回归测试。

## 3. 统一视角：一个系统，三条轴

| 轴 | 回答 | 谁定 | 何时变 | 怎么变 | 显示 |
|---|---|---|---|---|---|
| **系统**（发行版） | 它是什么：谱系、包、策略与授权、身份（os-release）、升级线 | 创建时选的发行版 | 只在升级时，沿同一谱系 | `luban upgrade` | `system   myos 2026.11.1 (Luban desktop)` |
| **桌面**（会话） | 用什么界面操作它 | 预设给默认，用户改 | 随时；可同时装几个 | 装组件；`luban config --desktop` | `desktop  KDE Plasma 6.6 (from myos)   also: labwc` |
| **进入方式** | 先看到什么、怎么运行 | 预设给默认，用户改 | 随时 | `--enter`、`--mode` | `enter    desktop · machine` |

判断标准：换掉它之后还是同一个系统吗？谱系、升级线、策略、身份变了是**发行版**；只是界面变了是**桌面**。业界同理：Fedora 的 KDE Spin 与
Workstation 是同一个 Fedora，登录界面选的 GNOME / Plasma 是会话。

- **"桌面 OS"** = 默认进桌面的系统，不是另一类 OS。
- **发行版自带桌面**：清单的包里列上桌面组件，预设写默认的 `desktop` 与 `enter`；自带的桌面仍是组件，用户可以再装、可以切换。
  发行版自己做的桌面作为 desktop 组件发布（可与发行版同一 index），别人也能用；只能在该发行版上跑的，组件声明 `requires`，装到别处时对账拒绝。
  要锁死（信息亭）交给策略（`subos-policy` 禁止安装与切换），不进桌面模型。
- **切换**：多个桌面共存；`luban config X --desktop kde` 长期切换，`luban enter X --desktop lxqt` 只此一次；应用系统栏"桌面 ▸"列出已装的会话、
  "控制台"、"安装更多桌面…"。切换只换会话，机器不重启。前提是互不干扰：组件的默认配置只经它自己的会话脚本进入（§6.3），
  一个桌面的自启动项不会出现在另一个里。卸掉正在用的桌面：退回控制台并说明（E20 的规则）。

## 4. 分发模型

### 4.1 角色由已有字段与约定认出

| 角色 | 认出的依据（不新增字段） | 例 |
|---|---|---|
| 系统 | `type = "subos"` 且清单 `subos_kind = "rootfs"`；`from` 链到达官方 edition | `luban:core`、`luban:desktop`、社区的 `awesome:myos` |
| 桌面 | 装上后载荷里有 `share/wayland-sessions/<id>.desktop`（约定，装后核实） | `luban:labwc`、`luban:lxqt`、`luban:kde`、`luban:xdesk` |
| 工具集 | 普通的元包 | `luban:tools-dev`、`luban:tools-agent`、`luban:tools-cli` |
| 运行时 | 被组件依赖的普通包 | `luban:qt-runtime` |
| 普通包 | 其余 | `xim:ripgrep` |

`categories` 与 `keywords` 只用来**找**（多个、不校验，影响搜索与卡片）；角色用来**认**（装后核实）。桌面组件写 `categories` 含保留值
`luban-desktop`，供卡片与 `xlings search --category luban-desktop`；写了却不符合约定的，安装后的核实拦下来并说明。

### 4.2 命名空间

| 命名空间 | 放什么 |
|---|---|
| `luban:` | 与 Luban OS 强相关或偏向 Luban 生态、官方维护：edition、桌面组件、Qt 运行时、工具集元包、Luban 的策略与启动配置、为 Luban 做的整包。**别的系统也可以依赖它们** |
| `xim:` | 通用软件（fish、mesa、内核、busybox……） |
| 社区 index 的命名空间（`awesome:`、`scode:`、`d2x:`……） | 社区的系统、桌面、工具 |

- **官方短名 = `luban:` 里的名字**：`--edition desktop` 即 `luban:desktop`，`--desktop kde` 对应 `luban:kde`。应用的 `kEditions`、卡片、
  luban 的 edition 名单都改为读数据。新增官方桌面不需要发布应用。
- **保留命名空间**：今天命名空间是配方自己写的字段（`namespace = "subos"`），任何 index 都能写 `luban`。改为：官方 index 的元数据声明它保留的
  命名空间（`xim`、`luban`、`subos`、`config`），xlings 只从官方 index 接受这些命名空间的包，别处出现时拒绝并指出 index 与文件。
  机制是通用的，xlings 不认识 Luban。已有社区 index（awesome、scode、d2x）用的是 `awesome`、`scode`、`fromsource`，不受影响。
- 保留规则只管**谁能发布**这个命名空间，不管谁能用：任何 subos、任何系统都可以依赖 `luban:` 的包。
- 宿主上运行的程序 `luban`、`luban-os` 留在 `xim:`（用户像装其他工具一样 `xlings install luban-os`）；`xdesk` 进 `luban:`（已定）。

### 4.3 清单中的 `luban` 对象

系统包的清单（`.xlings.json`，模板载荷）里，Luban 独有的声明放在 `luban` 对象：

```json
{
  "subos_kind": "rootfs",
  "from": "luban:desktop@2026.10.19.1",
  "min_client": "2026.10.19.1",
  "packages": [ "..." ],
  "luban": {
    "settings": { "mode": "machine", "desktop": "lxqt", "enter": "desktop", "login_shell": "/usr/bin/fish" },
    "os":       { "name": "myos", "id": "myos" },
    "requires": [ "root-layout>=2", "agent-display>=1" ]
  }
}
```

- xlings 读清单是按键取值，`luban` 原样留在载荷里；luban 读它。
- `luban.edition()` 从同一张表写出清单的 `luban.settings` 与根里的 `usr/share/luban/settings.json`，一处来源。
- 没有"我是官方"的字段：官方与否只由解析出它的 index 决定（加上 §4.2 的保留命名空间）。
- 组件不写清单；它的声明在载荷里（§6.3）。

### 4.4 谱系

1. **单一上级**：`from` 是一个字符串，格式本身保证。要复用的东西做成组件。
2. **链必须到达官方 edition**：官方 edition 自身成链（xim-pkgindex 规则 3），终止于 nano。环与深度沿用 xlings 现有检查（上限 8）。
3. **每一层的引用在声明它的 index 中解析**；记录每层的 index（`edition.chain` 现在只有坐标，补 index）。
4. **Luban OS 底座**：往上遇到的第一个官方 edition，即最全的那个。
5. **直接上级**：名字、版本、来源（官方 / 社区 / 本地）、是否已确认；与底座相同时合并为一行。
6. `upgrade` 只在原 index 中找新版本；名字迁移经 `replaces`（§15.1）。

### 4.5 名字的写法与 os-release

`<发行版> <版本> (Luban <底座 edition>)`，不用 `+`。官方 edition 本身写作 `Luban desktop 2026.10.19.1`。桌面单独一行（§3），不进名字。

| 位置 | 显示 |
|---|---|
| `luban new` 完成 | `box  myos 2026.11.1 (Luban desktop) · desktop lxqt · 36 packages · generation 41` |
| 欢迎信息、`luban status` | 系统 / 桌面 / 进入方式三行；`--json`：`distribution`、`luban_base`、`parent`、`lineage[]`、`desktop`、`desktops[]`、`enter` |
| Luban OS 应用 | 主标题 myos，副标题 Luban desktop，来源标签；桌面在系统栏 |

`/etc/os-release` 由 luban 在创建末尾按谱系写入（模板文件"上层优先"，发行版本可整个覆盖它）：`NAME`/`ID` 取 `luban.os`，`ID_LIKE=luban`，
`VERSION_ID` 取发行版版本，`LUBAN_BASE="desktop 2026.10.19.1"`。官方 edition 仍为 `ID=luban`。

### 4.6 包的形态与演进

同一个东西可以有不同的打包形态。架构要支持所有形态并存、随需要转换，**不走极端**：不要求一切拆成原子，也不把一切做成整包。

| 形态 | 是什么 | 例 |
|---|---|---|
| 原子包 | 一个上游项目一个包，以 deps 组合；库经 `<home>/abi` 与 RPATH 解析 | 宿主图形栈（mesa、glvnd、libX11……，已有） |
| 整包 | 预先组合好的一整套：程序、私有库、自己的加载器，包装脚本运行 | labwc-desktop（已有） |
| 运行时 | 被多个整包共用的那部分，本身也是整包 | Qt 运行时（LXQt 与 KDE 共用 341 MB） |
| 组合包 | 只有依赖，加上 Luban 的会话脚本、配置与自检 | `luban:kde`、各 edition |

**名字与约定稳定，实现可换**——这是形态能演进的前提：
- 用户与其他包只依赖**名字**（`luban:kde`）与**约定**（会话文件、`X-Luban-Shell`、自检、默认终端），不依赖内部怎么打包。
- 形态改变 = 同一个名字出新版本（整包 → 组合包 + 原子包，或反过来）；`luban upgrade` 照常走，用户的命令、设置、已建的系统不受影响。
  名字也要变时用 `replaces`（§15.1）。

**选择的依据**（每个组件在配方注释中写明选了什么、为什么）：

| 倾向整包 | 倾向原子包 |
|---|---|
| 要快、复用者少 | 被多个东西共用 |
| 来源只有现成的二进制，或 C 库不同（musl） | 系统外也有人单独要用（宿主上的单个应用） |
| 一起更新、一起测试更省事 | 某个库需要单独、频繁地出安全更新 |
| 只在 guest 里跑，用不上宿主 GPU | 要与宿主图形栈共用，或在沙箱模式下用宿主 GPU（glibc 族、经 glvnd：链接 `libEGL` 不再背上 mesa） |

**架构能力**（与选哪种形态无关，本轮做好）：
1. **约定独立于形态**：§6.3 的会话、外壳、自检、默认终端，整包与原子包写法相同。
2. **两种 ABI**：glibc 族经 `<home>/abi` 绑定；musl 族由整包自带加载器（`<home>/abi/linux-x86_64-musl` 也已存在）。
3. **系统内的 FHS 投影**：今天系统的 `/usr` 只放 `bin`、`lib/*.so`、`lib/modules`、`share`（`modules/subos/src/views/root/rootfs.cpp`）。
   扩为：包的 `lib/<子目录>` 按文件合并进 `/usr/lib/<子目录>`，`libexec/` 进 `/usr/libexec`，冲突走现有的认领规则并报出。上游二进制里写死的
   `/usr/lib/qt6/plugins` 之类在系统内因此直接成立（通用改动，xlings 不认识 Luban）。
4. **整包的私有树**放在 `libexec/<包名>/`，整体归该包（与 share 的"一个包的目录整体归它"相同），不与别的包合并。labwc-desktop 现在用
   `libexec/lib`、`libexec/<程序>`，新版本迁移。
5. **按需添加的工具**：已有 Alpine 整包工厂（`tools/res/build.sh`）；本轮加运行时的构建与对应的构建容器；**Arch 快照导入**（从固定日期的快照
   按上游项目批量生成原子包的配方与载荷，glibc、glvnd，与宿主图形栈同族）在出现原子化需求时再做，不是本轮的前提。

**本轮各组件的形态**：

| 组件 | 形态 | 理由 |
|---|---|---|
| 宿主图形栈（luban-os 依赖的那套） | 原子包（已有） | 宿主、godot、chatgpt 共用 |
| `luban:labwc`、`luban:compositor` | 整包（已有，升到 Alpine 3.24） | 已经可用，复用者少；见 §13.5 的轻量做法 |
| `luban:foot` | 小整包（≈7 MB） | 从 labwc 里独立出来，xdesk 等共用 |
| `luban:xwayland` | 小整包 | 默认 desktop 要带，须轻 |
| `luban:qt-runtime` | 运行时 | LXQt 与 KDE 共用最大的那部分 |
| `luban:lxqt`、`luban:kde` | 整包，建在 Qt 运行时上 | 上线最快 |
| 以后 | 有需求时把 Qt 与 KDE 框架拆成原子包（Arch 快照导入） | `luban:kde` 名字不变，变成组合包 |

原子化的成本参考（§19.6，按上游项目计、粗略按名字匹配 xim 现有包）：labwc 一套 115 个项目、xim 已有约 56 个；LXQt 在其上约 123 个、已有约 24 个；
KDE 最小集在其上约 320 个、已有约 35 个。这是"全部原子化"要新做的量，不是现状缺的东西——labwc 整包现在就可用。

## 5. 命令

### 5.1 `luban new` 的选项：选系统、装包、设置

| 类 | 选项 | 说明 |
|---|---|---|
| 选系统 | `--edition <短名>` 或 `--xpkg <系统坐标>` | 二选一；都不写为 `core` |
| 装包 | `--xpkg <坐标>`（可重复） | 任何 index、任何角色：桌面、工具集、普通包；第三方首次确认 |
| 设置 | 与 `luban config` **完全同一套**：`--desktop`、`--enter`、`--mode`、`--persist`、`--shell`、`--login`、`--on-close`、`--policy`、`--proxy`、`--tz`、`--allow` | `luban new` = 选系统 + 装包 + 创建时就配置好 |

```
luban new box                                              # L0：core
luban new box --edition desktop                            # L1：默认桌面（xdesk 经典）
luban new box --edition desktop --desktop kde              # L1：KDE（自动装官方 luban:kde）
luban new box --edition core --desktop labwc --enter desktop
luban new box --xpkg awesome:myos --xpkg scode:hypr --desktop hypr     # L2：任意组合
```

- 多个 `--xpkg` 的规则：系统最多一个（不给则 core）；桌面可以多个，默认桌面由 `--desktop` 选，不给时取预设声明的，否则第一个；
  没有角色的普通包照装。角色与入口不符时指出正确的写法（例如把系统坐标当桌面装："awesome:myos 是一个系统：`--xpkg` 只能有一个系统"）。
- **`--desktop` 的便利规则**（唯一的隐式安装）：`luban new` 中会话不存在、而官方 index 里有提供它的 `luban:<名字>` 时，自动装上并在结果里写明。
  只对官方包；社区桌面先 `--xpkg` 再 `--desktop`。`luban config --desktop` 不安装，只提示 `xlings install luban:kde --subos X`——配置命令只改设置。
- `--edition` 收到坐标：失败并给出 `--xpkg <同一坐标>`（agent 模式同样给出确切选项）。
- 创建之后加组件：`xlings install <坐标> --subos X`（组件是普通包）。两个视角都成立：luban 从外面配置系统，开发者进系统里用 xlings 自己装和配。
- 帮助分层（`cli.cpp` 已有的 `Level::More`）：默认帮助只列 L0–L1；`--xpkg` 等 L2 选项在详细帮助中。

### 5.2 渐进披露的层次

| 层 | 谁 | 入口 |
|---|---|---|
| L0 | 所有人 | 应用的快速开始：做什么（开发控制台 / 桌面）、桌面要什么样；`luban new box` |
| L1 | 进阶 | `--edition`、设置项（`--desktop kde`、`--enter`、`--mode`） |
| L2 | 玩家 | `--xpkg` 组合任意 index 的系统与组件 |
| L3 | 开发者 | 进系统用 xlings 装与配；本地 index |
| L4 | 社区作者 | 写配方与清单；以官方包为模板 |

### 5.3 校验与预览（服务层，结构化结果）

xlings 提供两项通用能力（不认识 Luban；新选项按 AGENTS.md 更新命令定义、`docs/generated/` 与测试）：
- `xlings subos new <name> --from <坐标> --dry-run --json`：安装模板包（很小），沿 `from` 解析整条链，输出每层的坐标、index、清单原文（含 `luban`）、
  合并后的包与策略；不建实例。
- 包的结构化描述走 xlings 已有的接口通道（luban 本就经它调用）：`xlings interface package_info` 的结果带 `package` 对象（命名空间、所在 index、类型、
  `categories`、版本、`passed_over`）；`search_packages` 与 `xlings search` 接受 `category`（结果另有 `packages` 数据）。不另加 `--json` 选项。

luban 的顺序：坐标解析 → 角色（§4.1）→ 链到达官方 edition → 能力满足（§9）→（第三方）确认。任一不满足即停，说明原因与下一步。
传入的是组件却当系统用时：

```
✗ labwc is a desktop, not a system
  with it   luban new box --edition core --desktop labwc
```

xlings 返回的错误由 luban 翻译；不转贴 `xlings subos` 的用法（E14）。社区作者用 `--dry-run -v` 自查：角色、谱系、能力、组件约定的检查结果一次列出。

### 5.4 确认界面与记录

一屏回答四个问题：这是什么 → 基于什么 → 建出来什么样 → 它多做了什么。标题加粗；官方部分暗色；需要注意的以黄色 `!` 置前；满足以绿色 `✓`。≤ 80 列。

```
 myos 2026.11.1 (Luban desktop)                      community · awesome
 A lightweight LXQt desktop for students

 Luban OS     desktop 2026.10.19.1                   official   ✓ runs on this luban
 Based on     campus-base 1.4                        community · scode   confirmed 2026-10-02

 It will be
   machine · enters its desktop (lxqt) · login shell fish
   34 packages over campus-base · ~620 MB · x86_64

 More than Luban desktop
 ! mounts   ~/Documents → /home/user/Documents  read-write   myos
 ! admin    installs udev rules for the lab's card reader    campus-base
   grants   clipboard, audio                                 myos
   network  private: through a SOCKS5 proxy only             myos

 Make box from it? [y/N]
   details  luban new box --xpkg awesome:myos@2026.11.1 --dry-run -v
```

- "More than" 的比较对象是 Luban OS 底座；每项标出由链上哪一层引入。排序：写宿主的挂载、管理员步骤（`!`）→ 授权 → 收紧项。
- 同时用 `--xpkg` 装的第三方组件在同一屏的"also installs"段列出（来源、大小、管理员步骤）。
- 记录：`<home>/config/luban/confirmed.json`，按坐标 + 版本；按 `xlings.home.format` 登记（不存在即未确认；读不了则停下并指出文件）。
  链上任何一层"多做的事"变化需重新确认。无人可答时失败并给出 `--yes`。

## 6. 底座、组件与预设

### 6.1 edition（预设）

| edition | 组成（`from` 之上） | 默认设置 |
|---|---|---|
| nano / tiny / tiny-musl | — | `enter: console`、`login_shell: /bin/sh` |
| core | 开发工具链、fish、nvim、git… | `enter: console`、`login_shell: /usr/bin/fish` |
| agent-workspace | core + `luban:tools-dev`、`luban:tools-agent` | `enter: console`、私有网络、fish |
| desktop | core + `luban:xdesk`、`luban:foot`、`luban:xwayland` + `luban:tools-dev`、`luban:tools-agent`、`luban:tools-cli` | `mode: machine`、`enter: desktop`、`desktop: xdesk-classic`、fish |

- **桌面不是 edition**：不出 desktop-minimal / desktop-lxqt / desktop-kde；选桌面用 `--desktop`。`desktop-xdesk` 冻结（已发布版本保留），
  今后等同于 `--edition desktop --desktop xdesk-canvas`（§15.2）。edition 从 7 个降到 5 个，新增桌面不增加 edition。
- 清单锁定组件版本以保证可复现；已发布版本的输出不变（xim-pkgindex 规则 1），以上是**新版本**的组成。

### 6.2 官方即示范

| 角色 | 官方版 |
|---|---|
| 系统 | nano（定制的起点）、tiny、tiny-musl、core、agent-workspace、desktop |
| 桌面 | T1 `luban:compositor`（labwc，无面板）、T2 `luban:labwc`、T3 `luban:lxqt`、T4 `luban:kde`、`luban:xdesk`（经典、画布） |
| 运行时 | `luban:qt-runtime` 与对应的构建容器 |
| 终端 | `luban:foot` |
| 工具集 | `luban:tools-dev`、`luban:tools-agent`、`luban:tools-cli` |
| 策略 / 启动 | 现有的官方策略（private、locked）与启动配置 |

- 卡片标"官方 · 示例"，链接到配方；文档每类一篇"从官方 X 开始做你自己的 Y"。
- 官方包的测试即组件约定的测试：社区包通过同一套检查，就符合约定。

### 6.3 桌面组件的约定

1. **会话**：`share/wayland-sessions/<id>.desktop`。附加键：`X-Luban-Shell=own|none`（外壳由组件提供，还是由 Luban OS 窗口提供，§7）、
   `X-Luban-Aliases=<旧名>`（§15.3）。
2. **默认配置**：包内 `etc/xdg`，只经自己的会话脚本以 `XDG_CONFIG_DIRS` 交给程序；用户的 `~/.config` 优先。程序读不对时（E12）由会话脚本绕开并注明。
3. **终端**：不写死，按 xdg 默认终端约定（`xdg-terminal-exec`，`share/xdg-terminals/`）找；以 `Terminal=true` 启动应用时把应用名与 ID 交给终端（E21）。
4. **资源**：光标主题、字体、应用入口（`share/applications`，按 L §6.4 投影）。
5. **自检**：`share/luban/checks.d/<id>.json`，随组件不随 edition。至少：会话能起来并画出东西、指针可见、终端里启动的应用显示自己的名字。
6. **不能**改 `enter`、`mode`、`login_shell`、策略与授权。

### 6.4 快速开始的卡片

从 index 读：edition 一组；`categories` 含 `luban-desktop` 的组件构成"桌面要什么样"的选项（文字取包的描述与会话文件的 `Name`）；
已添加 index 中的社区系统在"更多"，标明来源。默认只露出两个问题，其余在"自定义"。先修 E15（标签与实际会话一致）。

## 7. 图形体验分级

| 等级 | 内容 | 外壳 | 场景 | 入口 |
|---|---|---|---|---|
| T0 单应用窗口 | 系统里的一个图形程序，作为宿主上单独的窗口（guest 内以 cage 只跑它），窗口标题即应用 | 宿主 | 只想用某个 GUI 程序（类似 WSLg） | `luban run X --display window -- firefox` |
| T1 仅合成器 | labwc，无面板 | **Luban OS 窗口的系统栏**：从系统的应用列表启动、列出窗口、剪贴板 | 多窗口但不要桌面 | `--desktop compositor` |
| T2 最简桌面 | labwc + 面板 + 启动器 | 组件 | 轻量 | `--desktop labwc` |
| T3 LXQt | 加文件管理、设置、托盘 | 组件 | 传统桌面 | `--desktop lxqt` |
| T4 KDE | 完整桌面 | 组件 | 完整体验 | `--desktop kde` |
| Luban 自有 | xdesk 经典 / 画布 | 组件 | 默认 | `--desktop xdesk-classic` |

- 外壳由会话文件的 `X-Luban-Shell` 声明：`none` 时 Luban OS 窗口提供启动器与窗口列表，`own` 时不打扰。应用列表由服务层读系统的 `share/applications`（§10）。
- T0 不新增命令：`luban run` 已有，`--display window` 已是 `luban enter` 的选项，扩到 `run`。
- T0、T1 里 guest 不跑任何外壳程序，资源最省、门槛最低。

## 8. 进入：`enter` 与 `desktop`

| 设置 | 取值 | 含义 | 默认 |
|---|---|---|---|
| `desktop` | 会话名 \| `none` | 用哪个桌面会话 | 预设声明的，否则根里的第一个 |
| `enter`（新） | `console` \| `desktop` | 进入时先看到什么 | 预设声明的，否则**现在的规则**：能解析出会话即 `desktop` |

- 优先级：用户设置（`machine.json`）> 预设（`usr/share/luban/settings.json`）> 内置默认。组件不参与。
- 兼容：不写 `enter` 时沿用现在"能解析出会话就进桌面"（`display.cpp:21`）；core 系新预设显式声明 `console`，在 core 上装桌面不改变进入方式。
  `desktop = none` 读作 `enter = console`。旧 luban 写入时保留不认识的键、读取时忽略（`settings.cppm` 的约定）。
- 服务层给出 `EnterPlan { target: console|desktop, session, why }`，Luban OS 应用、`luban enter`（无 `--display` 时：有图形显示开窗口，没有则终端并说明，
  修 E5）、`luban status` 共用。桌面打不开（含会话不存在，E20）：`why` 显示并保留，再退回控制台（修 E7、E20）。

## 9. 能力契约

建在已有的 `luban.service.capabilities`（`luban status --json` 与快速开始在读）上：

| 方 | 新增声明 | 获取 |
|---|---|---|
| luban | `provides`：能读的根布局、机器协议、认识的清单字段 | 能力报告 |
| Luban OS 应用 | 同上（按自己的版本）；渲染后端 | 启动时自查；`luban-os __capabilities --json` |
| 机器里的 agent | 握手 `{"agent":1}` 扩为 `{"agent":1,"provides":[…]}` | 机器协议 `H` 帧（`luban/os/vm/src/guest.cpp:206`） |
| 系统 | `requires`（清单）与 `provides`（会话、光标主题、应用入口、登录 shell、根布局） | 服务层 |
| 组件 | `requires`（例如只能在某发行版上跑的桌面） | 载荷 |

能力名带整数级别（`root-layout`、`agent-display`、`desktop-cursor`、`host-gpu`），"需要"对"提供"做 `>=` 比较。

**这套词汇要标准化**：luban、Luban OS 应用、机器里的 agent、官方与社区的包，各自版本不同，却要对同一个名字有同一个理解。它不是网络协议，
是一份**数据契约**：
- 放在 `luban/specs/opencaps`（与 opensal、openhal、openas 同类：只依赖标准库）：`SPEC.md` 是登记表——每个能力名、每个级别加了什么、
  谁提供、哪个版本引入；模块提供解析（`root-layout>=2`）与比较。
- 规则：名字不复用；级别只升不降；每升一级写明新增了什么；新增能力或级别只需在登记表加一行并实现提供方。
- **不认识的能力名**按"不满足"处理并说明（"这个 luban 不认识能力 X：`xlings update luban`"）——"太新"由此有了明确的答复。
- xlings 不 import 它（`src` 不 import `luban`）：xlings 只记录自己的事实（根布局的整数），由 luban 映射到 `root-layout`。根布局级别由根本身的事实读出
（`xlings.subos.rootfs::layout_of`：`/usr` 是链接为 1，即 10.16.1 之前；是挂载点为 2），不另存记录——记录可能与树不一致，树不会。应用读到更高的级别：停下，"应用太旧：`xlings update luban-os`"。

| 时机 | 内容 | 不满足时 |
|---|---|---|
| `luban new` / 导入 / 装组件 | 系统与组件需要的 vs luban 与 agent 提供的 | 停，写明缺项与下一步 |
| 应用启动、`--display window` 前 | 应用能读的根布局与协议 vs 系统与 luban | 停，写明是应用太旧还是系统太旧 |
| 打开桌面前 | 会话可找到、agent 支持显示、光标主题、应用入口、宿主 GPU | 前两项不打开并说明；其余打开并提示 |

同一份检查进入 `luban status X --check`。luban-os 与 luban 互相声明最低要求；`xlings update luban` 时提示一并更新 luban-os。

## 10. 宿主读环境：一个入口

从宿主读环境里的路径（会话、应用入口、设置、os-release、passwd），一律经服务层（`service::describe`、`usr_of` + `physical`）。设置页的会话列表改用
`Environment::desktops`（修 E11）；xdesk 的应用 ID 用 `lexically_relative`（修 E9）；新增 lint：`apps/luban-os` 中不得对 `usr_of(...)` 的结果直接遍历
（`// env-read-ok: <why>` 豁免）。

## 11. 登录 shell

1. **xlings（通用）**：`subos new` 装完全部包后的刷新（`src/core/subos/root/root_cmd.cpp:606`）之后，按最终的根重选 root 的 shell（`best_shell` +
   `root_etc::set_login_shell`）。恢复 `LUBAN-SHELL`，对不经 luban 的创建同样成立。
2. **luban**：预设声明了 `login_shell` 时，在 xlings 返回后设定（与 `luban config --shell` 同一函数，`service.cpp:402`）；根里没有则不写并提示。
3. **顺序**：先 root 的 shell，再建登录用户（照抄 root，修 E2），再写 os-release。
4. 升级不改用户的选择；已建的环境不变。运行中改 shell 提示"`luban stop X` 后生效"；加入会话时 `SHELL` 取当前 passwd（修 E3）。

## 12. 会话与终端

- **多个终端对等**（修 E4）：交互式 `enter` 总以后台方式启动会话，第一个终端也作为加入者；任何终端退出立即返回；Ctrl-C 只到达各自终端的程序；
  bwrap 交互时也进入新会话（pty 已取代"保留终端以便作业控制"），`TIOCSTI` 防护不变。
- **控制台**：
  - E17：终端模拟器按"前缀 + 中间字节 + 结尾字节"分派；`CSI ? u` 不回应。回归测试喂 fish 4.8.1 的启动字节（§19.1）。
  - E18：在根的沙箱路径复现后再修。
  - 鼠标（E16）：鼠标模式下按 SGR / X10 转发，Shift 时本地选择；PRIMARY、中键粘贴、右键菜单、双击选词、三击选行。

## 13. 桌面

### 13.1 现有会话的修正

- **labwc**（E12）：0.7.2 的启动脚本在没有 `~/.config/labwc` 时加 `-C "$here/etc/xdg/labwc"`（§19.2 已验证），菜单补"关闭桌面"（labwc 的 Exit 改名）
  与"关机"（machine，`poweroff`）；随后换 Alpine 3.24 的 labwc 0.20（已修复），去掉绕法。
- **光标**：wlroots 默认搜索正确，代码无需改动；缺主题的旧 edition 由 §9 的检查提示。
- **标题**（E21、E10）：xdesk 以终端启动应用时传 `--title=<Name>`、`--app-id=<应用 ID>`（foot 支持），终端改为按默认终端约定查找，
  `compositor.cppm:18` 的写死去掉；labwc 会话里的 fuzzel 同样配置；E1 修好后 fish 也会更新标题。foot 是开源软件（MIT），问题在 Luban 的 xdesk。

### 13.2 性能（E13、E19）

主因：宿主上的 luban-os 以 xlings 的 glibc 加载器运行，看不到宿主的 GL；包不依赖 `graphics`，SDL 静默退回软件渲染。次因：桌面视图每次指针移动重画整窗。

本机验证（RTX 4080，§19.5）：同一 labwc 桌面、300 次指针移动——现发布版 CPU 23.8 s（47%）、present p50 21.3 ms；接入图形栈 0.41 s（2%）、0.07 ms；
只有 mesa（无 GPU 的机器）present 1.9 ms。接入需要三件事，缺一即再次静默退回软件渲染：RPATH 含 vendor 桥的库目录、进程有图形栈的发现变量、
宿主菜单的启动项经 shim。

修法：
- **0（xim-pkgindex）`luban-os` 接入图形栈**：依赖 `xim:graphics@>=0.1`，并**直接**依赖程序自己加载的 `xim:mesa`、`xim:libglvnd`、
  `xim:nvidia-gl-host-link`、`xim:wsl-gl-host-link`（elfpatch 只把包自己的直接依赖的库目录放进 RPATH——实测只依赖 `graphics` 时找不到
  `libGL.so.1`）；`xvm.add` 带 `graphics.consumer_envs()`；`.desktop` 经 shim（`system.bindir()`）；`revision` 加一。实测（隔离 home）：经 shim
  present p50 0.21 ms、加载 `libnvidia-eglcore`；直接运行载荷 26.9 ms（软件渲染）。
- **a–e（xlings）**：指针移动只转发、新帧才画；软件渲染时帧率上限 30；记录渲染后端并在软件渲染时提示原因与 `xlings install graphics`（`host-gpu`）；
  空闲不画（E19）；`LUBAN-OS-PERF` 加无脚本的 CPU 采样，CI 断言渲染后端不是 `software`。

**依赖的量级与归属**：`graphics` 给 luban-os 多拉进 home 共享包库的依赖约 72 MB 下载、243 MB 落盘（libllvm 124 MB、mesa 77 MB，其余小包约 40 MB，
NVIDIA 桥 ≈0）。每个 home 只装一次，与 godot、chatgpt 等共用。mesa 在 AMD / Intel 上就是驱动（xlings 不借宿主的 mesa，mcpp#352），在 WSL 走 d3d12，
处处是退路（llvmpipe 比 SDL 软件渲染快约 14 倍），不为 NVIDIA 单独裁掉。**归属 luban-os**——"谁在宿主上开窗口，谁依赖图形栈"：
luban 命令不画东西；系统里的桌面在 guest 渲染，用自己的栈；xlings 不开窗口。xim-pkgindex 加检查：写菜单项（`[Desktop Entry]`）的 Linux 包须依赖
`xim:graphics`，否则以 `graphics-ok: <why>` 注明（`categories` 的 `gui` 也用于 gtk3、qt-base 这类库，它们自己不开窗口，不作依据）。

### 13.3 LXQt 与 KDE

来源：Alpine 3.24 的二进制，沿用 labwc-desktop 的工厂做法（容器里装包，带 musl 加载器与私有库，`bin/<程序>` 包装运行）。实测（§19.6）：

| | labwc 0.20 | LXQt 2.4 | KDE Plasma 6.6.6（KWin、shell、Konsole、Dolphin、系统设置） |
|---|---|---|---|
| 整包时的库（`ldd` 闭包） | 112 个 273 MB（mesa 与 LLVM 占 235 MB） | 192 个 352 MB | 1088 个 687 MB |
| 建在 Qt 运行时上时自己的库 | 不用运行时（§13.5） | ≈11 MB | ≈346 MB |
| 另需的数据 | xkb、字体 ≈20 MB | 主题等 ≈50 MB | 图标 140 MB、壁纸 39 MB、QML 47 MB 等 ≈250 MB |
| 无 GPU | 流畅（pixman） | 流畅（Qt Widgets） | 可用（KWin 6.6 的 QPainter；QtQuick 走 llvmpipe，偏慢） |
| 会话脚本 | 已有 | Alpine 未打包 `lxqt-wayland-session`，按上游 0.4 自带 | `startplasma-wayland`（无 systemd 时传统方式启动）+ 自带 D-Bus 会话总线 |

**KDE 的接入：走 opensal，在 guest 里加一个 KDE 原生的适配器**（KWin、krdp 源码核实，§19.7）。

宿主一侧不变：Luban OS 应用只说 opensal 的传输协议。问题只在 guest 里由谁把 KDE 的画面与输入变成 opensal：

| 方案 | 做法 | 性能 | 代价 |
|---|---|---|---|
| (a) 嵌套 | 桥接用的 wlroots 合成器（headless）里，KWin 以 `wayland` 后端作为一个全屏客户端；现有 opensal-wlr 不改 | 两层合成（guest 内多一份 CPU） | 零新代码；KWin 6.8 起无 QPainter，要 `/dev/udmabuf` |
| **(b) KDE 原生适配器（目标）** | KWin 以 `virtual` 后端运行；适配器作为 KWin 的 Wayland 客户端，用 `zkde_screencast_unstable_v1` 建一个**与窗口同大小的虚拟显示器**的 PipeWire 流（`createVirtualMonitorStream`，窗口改大小即换尺寸），用 `org_kde_kwin_fake_input` 注入输入；指针以流的元数据给出（宿主画指针，延迟低）；剪贴板走数据控制协议 | 一层合成；共享内存帧（KWin 的 screencast 支持 MemFd，无 GPU 可用），带损伤区域 | 新 kit；依赖 libpipewire |
| (c) KWin 插件 | 在 KWin 进程内取帧、注入输入 | 最好 | 绑死 KWin 内部 API，每个 KWin 版本都要跟 |

- **(b) 是 KDE 自己的远程桌面 krdp 的做法**：krdp 的 `PlasmaScreencastV1Session` 正是 `zkde_screencast` + `fake_input`，并以其 `.desktop` 中的
  `X-KDE-Wayland-Interfaces=org_kde_kwin_fake_input,zkde_screencast_unstable_v1` 获得 KWin 的授权（这两个是特权协议），无需弹窗。适配器照此声明。
  krdp 另有 `PortalSession`（xdg-desktop-portal 的 ScreenCast / RemoteDesktop + EIS），是跨桌面的通用路径。
- 先用 (a) 验证 KDE 的打包与会话（零新代码），同时做 (b)；(b) 通过后作为默认，(a) 留作没有适配器的合成器的通用退路。(c) 不做。
- `kernel-virt` 加 `CONFIG_UDMABUF=y`（KWin 6.8 的软件渲染设备；6.6、6.7 的 virtual 后端仍有 QPainter）。

**可复用性：opensal 的适配器按"画面来源 + 输入 + 会话获取"三层拆开**：

| 层 | 实现 | 复用到 |
|---|---|---|
| 画面 | wlroots 的 screencopy（opensal-wlr，已有）；**PipeWire 流 → opensal 帧**（新，含损伤与指针元数据） | PipeWire 这层被 KDE、GNOME、任何经门户（portal）提供 ScreenCast 的桌面共用 |
| 输入 | wlr 虚拟指针 / 键盘（已有）；KDE 的 `fake_input`；**EIS（libei）**（新） | EIS 是正在成为跨桌面标准的输入注入方式（KWin、Mutter 都支持） |
| 会话获取 | KDE 直连（`zkde_screencast`，免弹窗）；门户（通用，可能弹窗）；GNOME Mutter 的 D-Bus（将来） | 新增一种桌面 = 多一种会话获取，画面与输入不重写 |

于是 Linux 桌面基本被两族适配器覆盖：wlroots 族（labwc、sway……）与 PipeWire + EIS 族（KDE、GNOME、COSMIC……）。kit 按规则不 import
`xlings.*`/`luban.*`，C 头文件只在其绑定模块中（AGENTS.md）。
- 路径重定位是主要风险：编进二进制的 `/usr` 路径很少（KIOCore 4 处、KAuth 2 处），Qt 可用 `qt.conf` 与环境变量重定位，35 个 D-Bus 服务文件安装时改写。
  试验第一步验证；不行时用 Alpine 的 APKBUILD 以 Luban 的前缀重建少数几个 KF6 包。
  这一风险只属于整包形态（私有树不在 `/usr`）；原子形态在系统内经 FHS 投影（§4.6）直接成立——若整包的重定位代价过高，这本身就是把 KDE
  原子化的理由。
- 量级：KDE 组件约 0.6 GB 落盘、约 0.25 GB 下载（另加 Qt 运行时），可选，不作默认。

### 13.4 xdesk

Luban 自己的 glibc 合成器与外壳，走 xlings 的包，不用 Qt 运行时；本轮修 E9、E21，终端与 labwc 的组件化解耦（不再从 labwc-desktop 包取 foot 与 Xwayland）。

### 13.5 整包形态下的桌面：轻量与共享（§4.6 的具体做法）

guest（machine）没有 GPU：wlroots 用 pixman 渲染（桥接与工厂测试都设 `WLR_RENDERER=pixman`），Xwayland 的 glamor 用不上。但 Alpine 3.24 的 mesa 26
把 gallium 直接链进 `libEGL`/`libgbm`，于是 labwc 与 Xwayland 只要链接 EGL 就背上 mesa 与 LLVM（235 MB）。实测各程序的 `ldd` 闭包（§19.6）：

| 程序 | 库 |
|---|---|
| foot | 21 个，7 MB（不链 EGL） |
| yambar / swaybg / fuzzel | 10 / 14 / 24 MB |
| Xwayland | 48 个，248 MB（glamor 经 EGL 拉进 mesa、LLVM） |
| labwc | 含 `libEGL`、`libgbm`、`libgallium`、`libLLVM` |
| LXQt 与 KDE 共用 | 162 个，341 MB（Qt6 链接 `libEGL`/`libGL`，mesa 在其中） |

做法（都是整包形态内的选择；换成原子形态时由 glvnd 自然解决，见 §4.6）：
- **每个整包带自己的闭包**（labwc-desktop 现在的做法），轻量的就该轻量：`luban:foot`（≈7 MB）。
- **去掉用不上的 GPU 依赖**：工厂以 Alpine 的 APKBUILD 构建 `wlroots`（只要 pixman 渲染器）与 `Xwayland`（不带 glamor），labwc、Xwayland 因此不背 mesa。
  这是两项定制构建，换来"最简桌面"与默认 desktop 都轻；做不成时接受它们背上 mesa（或改建在 Qt 运行时上），以实测大小在卡片上如实写出。
- **只有 Qt 共享运行时**：`luban:qt-runtime`（musl 加载器、mesa、LLVM、Qt6 base / wayland / declarative / svg、glib、xkb），按 Alpine 版本发布；
  `luban:lxqt`、`luban:kde` 依赖其确切版本，包装脚本以 `--library-path <自己>/libexec/<包名>/lib:<运行时>/libexec/qt-runtime/lib` 运行，
  运行时路径由 `config()` 从依赖解析写入。
- 运行时与整包的库都私有（`libexec/<包名>/`），整体归该包，不进根的 `/usr/lib`（musl 与 glibc 的根不混）。
- 维护：升 Alpine = 重建运行时与各组件；社区用同一个构建容器做 Qt 桌面，只打包自己的程序。

## 14. 缺陷清单

| # | 缺陷 | 修法 | 节 | 仓库 |
|---|---|---|---|---|
| B1 | 登录 shell 默认值（E1、E2） | 按最终的根重选；预设设定；先 root 后用户 | §11 | xlings、xim-pkgindex |
| B2 | 加入会话的 `SHELL`；运行中改 shell 无提示（E3） | §11 | §11 | xlings |
| B3 | 多终端挂住与 Ctrl-C 波及（E4） | 会话对等 | §12 | xlings |
| B4 | `luban enter` 默认显示方式（E5） | `EnterPlan` | §8 | xlings |
| B5 | 桌面失败原因被吞（E7）；会话不存在时无声（E20） | `EnterPlan.why` | §8 | xlings |
| B6 | 应用 ID 解析链接（E9） | `lexically_relative` | §10 | xlings |
| B7 | 设置页无桌面项（E11） | `Environment::desktops` | §10 | xlings |
| B8 | `--edition` 不校验，xlings 错误外泄（E14） | §5.3 | §5 | xlings |
| B9 | 卡片写死、标签不符（E15） | 卡片从 index 读 | §6.4 | xlings |
| B10 | labwc 0.7.2 读不到 Luban 菜单（E12） | `-C` 绕法；换 0.20 | §13.1 | xim-pkgindex |
| B12 | 控制台鼠标（E16） | §12 | §12 | xlings |
| B13 | `CSI ? u` 当恢复光标（E17） | 按前缀分派 | §12 | xlings |
| B14 | 旧应用悄悄误读新布局（E6） | 能力契约、根布局级别 | §9 | xlings |
| B15 | luban-os 没接图形栈（E13 主因） | §13.2 的 0 | §13.2 | xim-pkgindex |
| B16 | `XLINGS_ENTER_QUIET` 时仍有提示（E18） | 先复现 | §12 | xlings |
| B17 | 指针移动重画整窗；空闲仍在画（E13、E19） | §13.2 的 a–e | §13.2 | xlings |
| B18 | 会开窗口的包可以不接图形栈 | xim-pkgindex 检查 | §13.2 | xim-pkgindex |
| B19 | 新 Alpine 下 labwc、Xwayland 背上 mesa 与 LLVM | pixman 版 wlroots、无 glamor 的 Xwayland、Qt 运行时 | §13.5 | xim-pkgindex |
| B20 | xdesk 的终端应用标题与 ID 都是 foot；终端写死（E21） | 传标题与 ID；按默认终端约定 | §13.1 | xlings |
| B21 | 名单与卡片写死在代码里 | 短名来自 `luban:` 命名空间 | §4.2 | xlings |
| B22 | 用了比 index 最新版本旧的 edition 时，luban 不转达 xlings 的原因（E8） | `luban new` 的结果转达 `passedOver`（"用了 10.15.1：10.16.2 需要 xlings ≥ 2026.10.16.1，`xlings self update`"） | §5.3 | xlings |

（B11 撤回：光标代码无误。）

## 15. 兼容与迁移

### 15.1 命名空间迁移

- 旧名字（`subos:luban-*`、`xim:labwc-desktop`）**冻结**：已发布版本保留、照常解析（旧清单的 `from`、已建环境的记录都引用它们）；新版本只发在 `luban:`。
- 新包在清单（`.xlings.json`）中声明 `"replaces": "subos:luban-core"`（xlings 按键读清单，libxpkg 的包模型不变）；xlings 的 `subos upgrade` 据此从旧名字过渡（通用机制）。**只认同一 index 内的 `replaces`**，社区包不能接替官方包。
- 旧 luban 的 `--edition core` 仍解析到冻结的 `subos:luban-core`，能用、不再更新；新 luban 解析到 `luban:core`。
- 旧 xlings 不认识保留命名空间：照常解析 `luban:`（命名空间是通用的），只是不做保留检查。

### 15.2 edition 合并

- `desktop-xdesk` 冻结；`luban upgrade` 时以 `replaces` 过渡到 `luban:desktop`。
- 预设的默认值变化时（desktop-xdesk 的 `xdesk-canvas` → desktop 的 `xdesk-classic`），升级把**旧的有效值**写进用户设置，用户看到的不变；升级计划里列出这一条。

### 15.3 会话改名

- `labwc-desktop` → `labwc`：新组件的会话文件写 `X-Luban-Aliases=labwc-desktop`；读设置时别名解析到新名；`luban status` 显示新名。

### 15.4 其他

- 没有 `luban` 对象的旧 edition：按官方 index 与名字识别，默认设置从根里的 `settings.json` 读；`min_client` 仍生效。
- 没有 `luban` 对象的社区系统包：可以创建，确认界面的"建出来什么样"显示"未声明"。
- `--edition <坐标>`（含测试的 `fixture:my-os@1.0.0`）：失败并给出 `--xpkg`；现有测试改用 `--xpkg … --yes`。
- 不写 `enter`：沿用现在的规则；旧 luban 保留并忽略 `enter`。
- 实例记录中的每层 index、根布局级别为新增字段；读不到时按"只有一层""按事实推断"，不报错。
- 已建环境不改登录 shell；luban-os 的新 `revision` 对旧客户端只是多出依赖。

## 16. 测试与需求

先扩展已有的要求，新概念才开新 ID：

| 要求 | 变化 |
|---|---|
| `LUBAN-SHELL` | 创建途中才装上的 fish 被选中；预设声明的 shell；登录用户与 root 一致；加入会话的 `SHELL` |
| `LUBAN-TERM` | 带前缀的 CSI 不当作无前缀的同名序列；鼠标转发与选择 |
| `LUBAN-SETTINGS` | `enter`；`luban new` 接受与 `luban config` 相同的设置项 |
| `LUBAN-OS-PERF` | 指针移动不逐次重画；无脚本的 CPU 上限；渲染后端被记录并提示 |
| `LUBAN-OS-QUICK-START` | 卡片从 index 读，与实际会话一致 |
| `LUBAN-FHS-PATHS`（已有） | 包的 `lib/<子目录>`、`libexec/` 在系统内出现在 FHS 路径；整包的 `libexec/<包名>/` 整体归它；冲突被报出 |
| `LUBAN-EDITION-MIN-CLIENT`（已有） | 选用了较旧的 edition 时，`luban new` 说明原因与下一步 |
| `LUBAN-FROM-CHAIN` | 预览给出整条链与每层清单；`replaces` 只认同一 index |
| `LUBAN-DIST-XPKG`（新） | `--xpkg` 的角色、校验、确认、记录、`--yes`；角色与入口不符的提示 |
| `LUBAN-DIST-LINEAGE`（新） | 底座与上级、名字写法、os-release |
| `LUBAN-NAMESPACE`（新） | 保留命名空间只认官方 index；短名来自 `luban:` |
| `LUBAN-CAPS`（新） | 能力对账，应用太旧 / 系统太旧，根布局级别；不认识的能力名按不满足并说明 |
| `LUBAN-DESKTOP-KDE-ADAPTER`（新） | KDE 经原生适配器打开：画面、输入、指针、剪贴板、改窗口大小 |
| `LUBAN-SESSION-PEERS`（新） | 多终端对等 |
| `LUBAN-DESKTOP-SWITCH`（新） | 多桌面共存、切换、互不干扰、卸掉正在用的桌面 |
| `LUBAN-DESKTOP-CLASSIC`（新） | labwc、LXQt、KDE 在 machine 与沙箱中打开、可操作 |
| `LUBAN-APP-WINDOW`（新） | `luban run --display window` 的单应用窗口 |
| `LUBAN-PACKAGE-FORM`（新） | 同一名字从整包换成组合包 + 原子包（或反过来）后，已建的系统升级、设置与会话不变（以 fixture 包测试） |

测试一律用隔离的 home；应用的用例用 `--headless --script`。xim-pkgindex：luban-os 的 RPATH、发现变量、`.desktop` 经 shim；组件约定的自检；
各工厂在 Debian 容器里起一次无头会话并截图断言（labwc-desktop 现有的做法）；"窗口程序须接图形栈"检查的测试。KDE 的端到端单独一条 CI 线，
只在 main 与带标签的 PR 上跑。

## 17. 交付：每个仓库一个 PR，commit 作检查点

### 17.1 约定

- xim-pkgindex 一个 PR、xlings 一个 PR。每个 commit 是检查点：能构建、涉及的测试通过、提交信息写明关闭的 E / B 或节号；squash 的正文列出各检查点。
- 资源包（`kernel-virt`、`luban:foot`、`wlroots`/labwc、Xwayland、`luban:qt-runtime`、LXQt、KDE）由 `tools/res/build.sh` 在 CI 按架构构建，发布到
  `xlings-res/<名字>`（GitCode 新仓库需先有一次提交）；配方的 sha256 由对应检查点写入。
- 发布后的版本跟进按 AGENTS.md 的发布流程，不并入功能 PR。
- 可切分点只作退路：后面的检查点受阻时，可在最后一个绿色检查点合并，其余开新 PR。
- 分支：xlings 从 `feat/luban-release-asset` 合入后的 main 开出；xim-pkgindex 等本地的 `add(luban)` 提交合入后再开。

### 17.2 xim-pkgindex PR（先合；每一项对已发布的客户端都安全）

| 检查点 | 内容 | 关闭 |
|---|---|---|
| K1 `fix(luban-os)` | 接入图形栈 | B15 |
| K2 `fix(labwc-desktop)` | 0.7.2 的 `-C` 绕法、菜单项、自检 | B10 第 1 步 |
| K3 `test(index)` | 窗口程序须接图形栈的检查 | B18 |
| K4 `feat(index)` | 官方 index 声明保留命名空间；`luban:` 命名空间的约定写入 `docs/` | §4.2 |
| K5 `feat(res)` | `kernel-virt` 加 `CONFIG_UDMABUF=y`；`luban:foot`；pixman 版 wlroots 与 `luban:labwc`、`luban:compositor`（0.20，带别名，私有树迁到 `libexec/<包名>/`）；无 glamor 的 `luban:xwayland` | B10 第 2 步、B19、§7 |
| K6 `feat(res)` | `luban:qt-runtime` 与构建容器；`luban:lxqt` | §13.3、§13.5 |
| K7 `feat(res)` | `luban:kde`（先做路径重定位与 (a) 方案的试验，`status = "preview"`） | §13.3 |
| K8 `feat(luban)` | 每个组件配方注明形态与理由（§4.6）；`luban.edition()` 写清单的 `luban` 与 `settings.json`；工具集元包；`luban:` 下的新 edition（nano…desktop），声明 `replaces`、`enter`、`login_shell`、`os`；desktop 按 §6.1 组成；`luban:xdesk`；`docs/12-luban-packages.md` | B1、§6 |

新 edition 的字段对旧 luban 都是"忽略亦安全"。可切分点：K3 之后。

### 17.3 xlings PR（后合，随一次发布）

| 检查点 | 内容 | 关闭 |
|---|---|---|
| C0 `docs` | 本设计；`requirements.toml` 的扩展与新 ID | — |
| C1 `fix(term)` | CSI 按前缀分派 | B13 |
| C2 `fix(subos,luban)` | 登录 shell 的两层修法；加入会话的 `SHELL` | B1、B2 |
| C3 `fix(luban-os,xdesk)` | 设置页经服务层；应用 ID；失败原因保留；终端应用的标题与 ID；终端不写死 | B5–B7、B20 |
| C4 `perf(luban-os)` | 渲染后端与提示；新帧才画；30 帧上限；空闲不画；CPU 测试 | B17 |
| C5 `fix(luban)` | `--edition` 只收官方；角色不符的提示与翻译；转达 `passedOver` | B8、B22 |
| C5a `refactor(luban-os)` | `app.cpp`（3345 行）按职责拆为 `app.cpp`（主循环、托盘）、`common.cpp`（文字、设置、页框与浮层）、`launcher.cpp`（列表、快速开始、导入、作业）、`system_views.cpp`（控制台、运行、桌面）、`admin.cpp`（管理面板）、`script.cpp`（状态与测试脚本），共享的状态与声明在分区 `luban.os:core`；单个文件不超过 2000 行。§1 的行号指 2026.10.17.1 的单文件 | — |
| — 可切分点 — | | |
| C6 `feat(xim)` | 保留命名空间；`replaces`；`info --json`、`search --category --json` | §4.2、§15.1 |
| C7 `feat(subos)` | `subos new --dry-run`（数据事件 `subos_plan`）；实例记录补每层 index；根布局级别由树读出；系统内投影 `lib/<子目录>` 与 `libexec/`，整包私有树整体认领 | §5.3、§9、§4.6 |
| C8 `feat(luban)` | `--xpkg`（可重复）、角色、校验、确认与记录、谱系显示、os-release；短名来自 `luban:`；`luban new` 接受全部设置项；`--desktop` 的便利规则；系统 / 桌面 / 进入方式三行显示 | §3–§5、B21 |
| C9 `feat(luban)` | `enter` 与 `EnterPlan`；切换桌面（`luban enter --desktop`、应用系统栏）；升级时预设默认值变化的处理；会话别名 | B4、§3、§15 |
| C10 `feat(luban)` | 能力契约；`luban/specs/opencaps` | B14 |
| C11 `feat(luban-os)` | 卡片从 index 读；T1 的外壳（`X-Luban-Shell=none`） | B9、§7 |
| — 可切分点 — | | |
| C12 `feat(luban)` | T0：`luban run --display window` | §7 |
| C13 `feat(subos)` | 会话对等 | B3 |
| C14 `feat(luban-os)` | 控制台鼠标 | B12 |
| C15 `fix` | E18 | B16 |
| C16 `feat(kits)` | opensal 的 PipeWire 画面层、`fake_input` / EIS 输入层、KDE 直连的会话获取（新 kit）；KDE 会话默认用它 | §13.3 |
| C17 `test(e2e)` | 三种传统桌面与切换的端到端 | §13.3 |
| C18 `release` | 版本号与 `docs/`（en、zh） | — |

C17 依赖 K5–K7 已发布；C16 与 K7 配合（KDE 整包的会话脚本启动适配器）。

### 17.4 依赖与执行顺序

```
xlings                                               xim-pkgindex
C0 ─┬─ C1 C2 C3 C4 C5 C13 C14 C15   （互不依赖）       K1 K2 K4      （互不依赖）
    ├─ C10 ──► C8 ──► C9 ──► C11 C12                   K1 ──► K3
    ├─ C6 ──┘  ▲                                       K5 ──► K6 ──► K7 ◄── C16（适配器的 guest 程序）
    └─ C7 ─────┘                                       K5 ──► K8 ◄── C18（新 edition 的 min_client）
C16 ─────────────────────────────────────────────────► K7
K5–K7 已发布 ──► C17 ──► C18
```

- **能力契约先于 `--xpkg`**（C10 → C8）：`luban new` 的校验要用它。`--xpkg` 用 C6 的 `info --json` 与 C7 的 `subos new --dry-run --json`。
- **跨仓库**：xlings 的测试用 fixture index，不等 xim-pkgindex 合入；xim-pkgindex 的新 edition 写 `min_client` 为本次发布的版本，旧客户端经
  `passedOver` 继续用旧版本，所以两个 PR 的合入顺序只受"资源已发布"约束。实际顺序：K1–K7 的资源先由工厂构建发布 → xlings PR 合入并发布 →
  xim-pkgindex PR 补上发布版本的跟进后合入 → 真实环境验证。
- **KDE 的 (b)**：KWin 6.6 没有 ext-image-copy-capture，帧只经 `zkde_screencast` 的 PipeWire 流给出（已核实 `libkwin.so.6.6.6` 的协议表）。
  guest 的 luban 是静态程序、不带 libpipewire，所以 (b) 分两半：会话获取（`zkde_screencast`）与输入（`fake_input`）在静态的 bridge 里说 Wayland；
  PipeWire 的消费者是随 `luban:kde` 一起构建的小程序（musl、链接 Alpine 的 libpipewire），把帧以共享内存交给 bridge。KDE 整包因此要带 pipewire 与
  wireplumber。(a) 不依赖这些，先上。

## 18. 待定

1. 何时把 Qt 与 KDE 框架原子化（Arch 快照导入）：出现系统外单独使用、或安全更新频率要求时；届时先实测长 RPATH 的启动时间。
2. KDE：(a) 嵌套先验证打包与会话，(b) 原生适配器的帧率、CPU 与 (a) 对比；路径重定位是否需要重建 KF6 的少数包。K7 与 C16 的第一步。
3. 两项定制构建（pixman 版 wlroots、无 glamor 的 Xwayland）在 Alpine 的 APKBUILD 上的改动量；不可行时接受变大，如实写在卡片上。
4. 确认记录是否按 index 的信任级别区分（例如本地 index 免确认）。
5. （已定）能力词汇作为 `luban/specs/opencaps` 标准化（§9）。
6. 需要宿主上的数据：`luban status d1 --json` 的 edition（应用新建时为何解析到旧版本）；guest 内 labwc 与 foot 的 RSS。

## 19. 复现与调研记录

### 19.1 E17、E18：控制台（本机，2026-10-18）

- 方法：本地构建 `luban-os`，临时让 `--console-exec` 带入一段欢迎信息（实验改动，已撤回），在 `--headless --script` 下运行 shell，读取状态中的控制台行。
- 结果：`sh`、`bash`、fish 3.7.0 均不复现；**fish 4.8.1 复现**——欢迎语与提示符写在第 0 行，欢迎信息前三行被盖。
- fish 4.8.1 启动时发出的字节（以 pty 录取）：
  `ESC[?u  ESC[>0q  ESC]11;?ESC\  ESC[?1049h  ESC P+q…ESC\  ESC[?1049l  ESC[0c  …Welcome…  ESC[6n  ESC[0c  ESC[?2004h  ESC[>4;1m …`
  第一个 `ESC[?u` 被 `term.cpp:333` 的 `case 'u'` 当作恢复光标。
- 修法验证：`CSI s` / `CSI u` 仅在无前缀、无中间字节时生效后，fish 4.8.1 的输出接在欢迎信息之后（实验改动，已撤回）。
- E18：以工具视图 SubOS 在应用控制台中运行，只出现 `[xlings] … env var(s)`，未出现 `▸ entering subos`；根 SubOS 的路径本机无法运行（不能嵌套命名空间）。

### 19.2 E12：labwc 菜单（本机隔离 home）

- 方法：隔离 home 中 `xlings install labwc-desktop`（0.7.2）；`luban-os --headless --desktop-exec luban __desktop labwc-desktop`，脚本点击背景后截图。
- 结果：面板与时钟正常；背景菜单为内置的 Reconfigure / Exit。包内 `etc/xdg/labwc` 拷到 `~/.config/labwc` 后为 Luban 菜单；
  启动脚本加 `-C <包>/etc/xdg/labwc` 后同样为 Luban 菜单。
- 原因：labwc 0.7.2 `src/menu/menu.c` 的 `parse_xml` 在第一个路径 `fopen` 失败时 `return`；上游 master 为 `continue`。

### 19.3 E13：宿主实测与加载器核实

本机（无 GPU 时，release 构建）：应用真实空闲约 4%；present（软件渲染）约 23–27 ms；桥接与 labwc 约 0%。

宿主（Linux，RTX 4080，`d1`：desktop、labwc 会话、machine；`top -b -d 1`）：

| 场景 | d1 的应用窗口 | 另一个应用窗口（已开 32 分钟） | qemu | guest 内 labwc / foot / agent |
|---|---|---|---|---|
| 空闲 | 2.5–6% | 9–11% | ≈0% | 0% / – / 0% |
| 指针在桌面里持续移动 | **92%** | 5–8% | 3–5% | 0% / – / 0% |
| 指针在应用窗口外移动 | 2–5% | 10–30% | 0–1% | – |

两个应用进程的内存映射中均无 libGLX、libEGL、nvidia、vulkan。重启修好 NVIDIA 驱动后 CPU 仍随指针移动飙升——与驱动无关。

加载器核实：`<home>/abi/linux-x86_64-glibc/ld-linux-x86-64.so.2 --help` 的搜索路径只有 xlings 的 glibc 目录与它的 `ld.so.cache`；
`readelf -d luban-os`：DT_RPATH 只含 abi 目录与 7 个 X11 库目录；`SDL_RENDER_DRIVER=opengles2`：`Could not initialize OpenGL / GLES library`。
`compat.sdl3` 3.4.2 编入了 OGL、GLES2、Vulkan、GPU 渲染器。另记：滚动时 guest 内 labwc 与 foot 的 VSZ 升至约 1 GB（RSS 待查）。

### 19.4 调研来源

- wlroots 0.18：`xcursor/xcursor.c`、`xcursor/wlr_xcursor.c`。labwc：0.7.2 与 master 的 `src/menu/menu.c`、0.7.2 的 `src/common/dir.c`；最新 0.20.2。
- KWin master：`src/backends/`、`src/wayland/CMakeLists.txt`、`src/plugins/eis`。plasma-workspace master：`startkde/startplasma.cpp`。
- lxqt-wayland-session 0.4.1 README 与 `configurations/`。
- 本仓库：`luban/os/service/src/capabilities.cppm`、`settings.cppm`、`modules/subos/src/model/edition.cppm`、`src/core/subos/root/root_cmd.cpp:313-360`
  （`from` 链）。xim-pkgindex：`libs/luban.lua`（`luban.edition`）、`docs/12-luban-packages.md`（edition 规则）、`pkgs/c/chatgpt.lua`、`pkgs/g/godot.lua`、
  `libs/graphics.lua`（`consumer_envs`）。

### 19.5 E13 修法的本机验证（RTX 4080，驱动 580.178.04，2026-10-18）

- 隔离 home：`xlings install graphics mesa luban-os labwc-desktop`（38 个包；`data/xpkgs` 共 564 MB，其中 mesa 77 MB、libllvm 124 MB、glibc 122 MB、
  luban-fonts 65 MB、luban-os 12 MB）。`nvidia-gl-host-link` 0.1.3 识别出宿主驱动，`share/glvnd/egl_vendor.d` 有 `10_nvidia.json` 与 `50_mesa.json`。
- 以 luban-os 2026.10.17.1 的副本，用 patchelf 模拟配方改动，`SDL_VIDEO_DRIVER=offscreen`：

| 变体 | 结果 |
|---|---|
| 原样 | `SDL_RENDER_DRIVER=opengles2`：`Could not initialize OpenGL / GLES library`；默认：软件渲染，present 27 ms |
| RPATH + mesa、libglvnd | 不带变量：`Could not find a valid EGL device`；带发现变量：加载 `libgallium`，NVIDIA 的 `libEGL_nvidia` 找不到 `libnvidia-glsi.so.580.178.04`（不在搜索路径） |
| RPATH 再加 `nvidia-gl-host-link/lib` | 带发现变量：默认选 NVIDIA，present 0.23 ms（启动器视图）；只给 mesa vendor：llvmpipe，1.9 ms；**不带发现变量：软件渲染，27 ms** |

- labwc 桌面（`--desktop-exec luban __desktop labwc-desktop`，1280×800），脚本先等桌面就绪，再 300 次 `move` + `frames 1`，以 `/proc/<pid>/stat` 采样应用进程的 CPU：
  现发布版 50.9 s 内 CPU 23.77 s（47%），1201 帧，present p50 21.27 ms；接入图形栈后 27.3 s 内 0.41 s（2%），1201 帧，present p50 0.07 ms。
  （脚本模式每帧至少 30 ms，真实使用时软件渲染不受此限，所以宿主上是 92%。）

### 19.6 LXQt、KDE 与桌面运行时的量级（本机，2026-10-18）

- 方法：Alpine 3.24.2 的 APKINDEX（main、community）计算依赖闭包；再以 Alpine minirootfs + proot（`xim:proot`）实际 `apk add` labwc 会话集、
  `lxqt-desktop`、`kwin plasma-workspace konsole dolphin systemsettings breeze dbus`，对主程序、`/usr/lib/qt6/plugins` 与 `/usr/lib/qt6/qml` 下的
  `.so`、KDE 的 libexec 程序取 `ldd` 闭包，按真实文件去重计大小（labwc-desktop 工厂打包时取的正是 `ldd` 闭包）。
- 版本：labwc 0.20.0、wlroots 0.20.2、lxqt-session 2.4.0、Qt 6.11.1、Plasma 6.6.6、KF6 6.26.0、mesa 26.1.6、LLVM 22.1。
- `ldd /usr/bin/labwc` 含 `libEGL.so.1`、`libgbm.so.1`、`libgallium-26.1.6.so`、`libLLVM.so.22.1`。
- 结果：labwc 112 个库 273 MB；LXQt 192 个 352 MB；KDE 1088 个 687 MB。被两个及以上桌面共用的库 177 个 350 MB；各自独有：labwc ≈1 MB、
  LXQt ≈5 MB、KDE ≈343 MB。KDE 的数据：`usr/share/icons` 140 MB、`wallpapers` 39 MB、`qt6/qml` 47 MB、`qt6/plugins` 34 MB。
- 编进二进制的 `/usr/...` 路径（`strings`）：`libKF6KIOCore` 4 处（`/usr/lib/libexec/kf6`、`/usr/lib/qt6/plugins/kf6/kio/`）、`libKF6AuthCore` 2 处，
  `plasmashell`、`kwin_wayland`、`startplasma-wayland` 无；`usr/share/dbus-1/services` 35 个文件全部 `Exec=/usr/…`。
- KWin 源码（GitHub 的 KDE/kwin 镜像）：`Plasma/6.6`、`Plasma/6.7` 的 `src/backends/virtual` 与 `wayland` 有 `*_qpainter_backend.cpp`；
  `Plasma/6.8` 与 master 没有，`src/core/gpumanager.cpp` 以 `/dev/udmabuf` 建软件渲染设备。
- Alpine 3.24 没有 `lxqt-wayland-session` 包，也没有 `startlxqtwayland`。
- 单个程序的 `ldd` 闭包（同一 rootfs）：foot 21 个库 7 MB；Xwayland 48 个 248 MB（`libLLVM` 191 MB、`libgallium` 44 MB）；fuzzel 51 个 24 MB；
  yambar 39 个 10 MB；swaybg 37 个 14 MB。LXQt 与 KDE 的交集 162 个库 341 MB；LXQt 独有 11 MB，KDE 独有 346 MB。
- 社区 index 的命名空间（本机 `data/xim-index-repos` 中的 awesome、scode、d2x）：`awesome` 9 个、`scode` 31 个、`fromsource` 1 个；无 `xim`、`luban`。

### 19.7 KDE 适配器的依据（2026-10-18）

- KWin 分支 `Plasma/6.6` 与 `Plasma/6.8` 的 `src/plugins/screencast/screencaststream.cpp`：缓冲类型为 DmaBuf 或 MemFd（`SPA_DATA_MemFd`），
  共享内存格式 ARGB8888——无 GPU 时可用。
- krdp（KDE 的远程桌面服务）`src/`：`PlasmaScreencastV1Session`（`zkde-screencast-unstable-v1.xml`、`fake-input.xml`）与 `PortalSession`
  （xdg-desktop-portal ScreenCast / RemoteDesktop、`EiConnection`），以及 `Clipboard`、`Cursor`、`DisplayControl`；
  `createVirtualMonitorStream(name, size, dpr)`、`createOutputStream`、`createWorkspaceStream`。
- `server/org.kde.krdpserver.desktop.cmake`：`X-KDE-Wayland-Interfaces=org_kde_kwin_fake_input,zkde_screencast_unstable_v1`。
- luban 不钉 edition 版本（`edition_ref`，v2026.10.15.4 与 v2026.10.17.1 相同）；`passedOver` 说明自 v2026.10.16.2 起（`src/core/xim/catalog.cpp`）。
