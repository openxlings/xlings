# Luban OS 进入之后与 Luban OS 应用：终端、路径、index、用户、自检、测试矩阵、应用的视觉与交互；aarch64；与 mcpp 的生态联动

日期：2026-10-16　状态：修订 5，实现中　缘起：在 `hello-luban`（core）、`hello-luban-d`（desktop，machine）与 Luban OS 应用中的实测；
openxlings/xlings#652、#657、#672；mcpp 的设计
`mcpp-community/mcpp:.agents/docs/2026-10-16-xlings-provider-port-and-shared-store-design.md`（下称 **mcpp 设计**）；
probe PR #674 的六轮运行（38791353147、38793084640、38794275497、38798915957、38799653022、38801097759）。

范围：xlings（含 luban 与 Luban OS 应用）一个 PR，xim-pkgindex 一个 PR，mcpp 一个 PR（mcpp 设计），按 §19 的顺序发布。不分阶段。

**修订 5 的变化（实现中的修正）：**
- §6.2 第 3 条与 §6.3：会话的根视图已由 supervisor 以 `root_mount::update` 在线刷新（持有命名空间描述符，在父命名空间中操作）。
  `/usr` 与目录条目作为根视图的绑定参与同一刷新，session-init 不需要任何能力；D5 改为"不保留能力"，原安全代价随之消失。
- §4.1 的域内交互命令不改：`--new-session` 是 `TIOCSTI` 防护的一部分，Ctrl-C 由外层随父进程退出处理。
- §6.4 的规则按实现写定：认领互斥（一个链接之上与之下不再有链接）；`XDG_DATA_DIRS` 未设时各程序按规范取 `/usr/local/share:/usr/share`，不另设。
- §6.2 第 1 条：`/usr` 的形态记录于根的 `etc/xlings/usr`；tar 与目录导出由其他 init 启动，保留链接形态。

**修订 4 的变化（probe 第 4–6 轮）：**
- 终端"字太小、横向间距太大"实测确认（A6）：在显示缩放 > 1 时，单元尺寸按缩放计算而字形按未缩放的字号绘制；缩放 2 时字形仍约 9–11 px 高，行距由 16 变为 30。
  另有一个独立原因：宿主无等宽字体时以比例字体充当，窄字母被放进"M"宽的格子。§13.7 重写。
- 新问题 A11：应用界面在缩放 2 时文字放大而卡片等尺寸未放大，文字溢出、重叠。§13.3 增加"所有尺寸经 DPI 换算"与多缩放抓帧。
- O9 关闭：选 Adwaita 光标（24–96 五档，HiDPI 完整）；O10 关闭：文件拖放走 virtiofs；O11 关闭：U15 在 machine 中复现；
  O12 关闭：macOS 托盘可用，Windows 11 新图标默认收入溢出区，Linux 的 SDL 托盘需运行时加载 GTK 与 appindicator（静态构建不可用），确认 Linux 自实现 StatusNotifierItem。
- 新问题 U20：machine 的主机名为 `luban` 而不是系统名。

**修订 3 的变化：**
- 关闭对话框的选项在不悬停时看不出选中（A9）：对话框重做为两张选项卡片，并新增"控件规范"：选中状态无需悬停即可分辨，由抓帧断言（§13.13）。
- 后台运行与宿主的存在感（A10）：新增 §13.12 状态栏/托盘图标——Windows 通知区域、macOS 菜单栏、Linux StatusNotifierItem；
  图标显示状态，点击有控制菜单；"后台运行"时应用留在托盘而不是退出。
- O8、O9、O10 按建议方式关闭（§21）；NixOS 不进测试矩阵。新增 O12（托盘在各平台的实测）。

**修订 2 的变化：**
- O1–O4 由 probe 关闭；新增 O3b、O6、O7 的实测（§2）。
- 新问题：desktop 的 `/usr/share` 缺应用入口、光标主题与 terminfo（U12–U14，同一根因：payload 的 `share` 不进 generation）；
  machine 中"当前 subos"为 `default`（U15）；系统里的普通用户无法安装（U16）；同名创建的错误信息（U17）；broker 环境
  `XLINGS_NON_INTERACTIVE=1`（U18）；宿主矩阵 `edition` 场景的假设与 `bare.sh` 的发行版支持（U19）。
- 新增 §8 系统里的用户与 home、§9"当前系统"的判定。
- 新增 §13 Luban OS 应用：信息架构、像素游戏风格的视觉语言（主基调）、各界面、终端渲染、窗口与宿主的交互契约、
  响应性（选卡片 1–2 秒才反应的根因）、抓帧视觉验证。
- §5 补充 payload `share` 的投影规则与能力的保留与丢弃（O3b）。

## 0. 结论

1. **终端只有一个中继**：会话主命令、加入会话、broker、域内交互命令、carrier guest 共用。作业控制、fish、install 的确认、Ctrl-C 取消一起解决。
2. **域内命令的事件都转发**：`luban new` 有进度。
3. **系统里的路径符合常识**：`/usr` 在每种呈现中是挂载点；generation 中目录条目以绑定挂载呈现（实测 core 7 个、desktop 8 个，成本 < 4 ms）；
   payload 的 `share` 按规则投影进 `/usr/share`（应用入口、图标与光标主题、terminfo、字体、手册等）。
4. **系统里的 index 开箱可用**；**系统里的普通用户有自己的 home**，叠加在系统层之上，无需 root；**"当前系统"在沙箱与 machine 中都显式确定**。
5. **edition 补齐**：terminfo、光标主题进 luban-tiny/desktop；登录 shell 由 edition 声明（core 系为 fish）；找不到命令时提示；系统里的 mcpp 复用系统的工具链。
6. **自检**挂在 luban-init 与 session-init；`luban status --check` 按需；CI 复用同一套检查。
7. **测试矩阵**扩到主流发行版与 aarch64（实测 11 条新腿均能建系统、进入系统）。
8. **Luban OS 应用重做视觉与交互**：像素游戏风格为主基调；首页最近 3 个加字典序、类型有明显视觉差异、键盘导航跟随滚动；
   新建分两排、创建即进入、有分阶段进度；nano 用"运行命令"视图；终端用随应用发布的字体与整数网格；管理与设置重做；
   窗口与宿主的交互有契约（光标、键盘、输入法、剪贴板、尺寸、文件拖放）；后台运行与最小化时在宿主的状态栏/托盘显示图标，图标给出状态与控制菜单；
   控件的选中状态无需悬停即可分辨；关键界面逐 PR 抓帧，结构断言、基准图比对与视觉评审三道把关。
9. **aarch64 本轮完成**（#657 全部）；**#652、#672 修复**；**xim-pkgindex 的 Luban 包放到 `pkgs/luban/`**；mcpp 设计的 X1–X6 随本次 xlings 发布。

## 1. 问题与证据

| 编号 | 现象 | 根因 |
|---|---|---|
| U1 | `luban new` 只显示"▸ making …"，数分钟无变化 | 域内建系统 `produce_domain_at_creation_`（`src/core/subos/root/root_cmd.cpp:403,419`）不转发事件；upgrade/export 有转发（`src/core/subos/cmd.cpp:2136-2156`） |
| U2 | `can't access tty`；bash `cannot set terminal process group`；fish `tcgetpgrp failed`（CI 的 luban-window 截图里同样出现） | 会话主命令继承调用者终端（`modules/subos/src/session/session.cpp:1152-1161`），PID 命名空间内看不到前台进程组；加入会话的路径已建 pty（`:1240-1262`） |
| U3 | 无法切换到 fish | 同 U2 |
| U4 | vim 退出后屏幕不恢复 | 根没有 terminfo（O1 实测确认） |
| U5 | 系统里 install 不确认 | broker 强制 `-y`（`session.cpp:697`），且环境设 `XLINGS_NON_INTERACTIVE=1`（`src/core/subos/sandbox.cpp:450`） |
| U6 | 下载中 Ctrl-C 无法取消 | `broker::forward`（`modules/subos/src/session/broker.cpp:157`）不转发信号；supervisor 在子进程启动后不再监视客户端（`session.cpp:516`） |
| U7 | `cd /usr`、`cd /lib` 显示 store 路径；`cd ..` 又回到 `/` | `/usr` 是绝对符号链接（`modules/subos/src/views/root/rootfs.cpp:607`）；generation 的目录条目是整目录链接（`rootfs.cpp:343-393`） |
| U8 | 系统里 list/info/search：`package index not available` | 系统 home 无 index；只有 install 会同步（`src/core/xim/commands.cpp:456-472` 对 `:2319`） |
| U9 | 登录 shell 是 `/bin/sh` | luban-tiny 的 passwd |
| U10 | 命令找不到时没有指引（含拼错 `xlnigs`） | 无 command-not-found 处理 |
| U11 | 系统里第一次 `mcpp build` 会再下载 gcc | mcpp 用自己的 `~/.mcpp`；见 mcpp 设计 |
| U12 | desktop：启动器 "Apps" 为空，找不到终端 | payload 的 `share` 不进 generation：`foot.desktop`、`nvim.desktop` 在 payload 中，`/usr/share` 只有 factory、luban、udhcpc、wayland-sessions、xdesk（O7 实测） |
| U13 | desktop：看不到鼠标 | 系统里**没有任何光标主题**（O7）；应用在机器画面上隐藏宿主光标（`apps/luban-os/src/app.cpp:1144`），依赖 guest 画进帧里；应用不声明 opensal 的 `Cursor` 接口（`apps/luban-os/src/display.cpp:206`） |
| U14 | terminfo 只在 python、tmux、htop 的 payload 中 | 同 U12 |
| U15 | machine 中 `vim`：not installed in this subos (default) | generation 的 `/usr/bin/vim` 是 xlings shim；沙箱设 `XLINGS_ACTIVE_SUBOS`，machine 中无此变量且无 `/etc/xlings/root.json`，shim 取 home 的 `default`（O11 在 core machine 中复现） |
| U20 | machine 的主机名为 `luban`（提示符由 `root@pcore` 变为 `root@luban`） | 沙箱取 `root_etc::hostname(etc, name)`（`src/core/subos/sandbox.cpp:752`），machine 的启动未用同一规则 |
| U16 | 系统里的普通用户 `xlings install`：`/xlings/.xlings.lock cannot be written` | 系统内所有用户都指向 `/xlings`；部署形态 R（multi）规定"用户另有 `~/.xlings`"（part 2 §5），未实现；错误不指出 `sudo` 或用户层 |
| U17 | 同名再建：`prefix-domain control registry is invalid or already claims this name` 加一行 usage | 错误未翻译为"已存在" |
| U18 | 宿主矩阵 `edition` 场景在 11 条新腿上同样两处失败 | 场景假设"agent 模式下需要一次性设置"只在 AppArmor 受限的 Ubuntu 24.04 成立 |
| U19 | `bare.sh` 不支持 Alpine；Rocky、Amazon Linux 的 `curl-minimal` 冲突 | 需 apk 分支与 `dnf --allowerasing` |
| A1 | 应用：选卡片 1–2 秒后才显示选中 | 主循环空闲时 `begin_frame(1000)`（`app.cpp:1807`）；ImGui 的点击需要第二帧才呈现 |
| A2 | 应用：新建界面 5 张卡片一排、两个分组标题与下方表单不在同一网格、"More options" 是漂浮的复选框、只有文字区分类型 | 截图 `1-quick-start`；`quick_start_`（`app.cpp:591`）；只列 5 个 edition（`kQuick`，`app.cpp:110`） |
| A3 | 应用：nano 创建后回到首页；从首页进 nano：`nano-1 is off`、`xlings: /bin/sh: No such file or directory` | `goes_in = false` 时回首页（`app.cpp:691-699`）；首页行对所有系统都调用 `enter`（`app.cpp:463`） |
| A4 | 应用：首页 ↑↓ 选择时列表不跟随滚动 | `app.cpp:1839` 只改 `selected`，不滚动 |
| A5 | 应用：首页顺序与类型无区分 | 列表按服务返回的顺序 |
| A6 | 应用：终端字太小、横向间距太大 | **主因（实测）**：`draw_console`（`apps/luban-os/src/console.cpp:210-260`）的单元宽高取 `CalcTextSize("M")` 与 `GetTextLineHeight()`，二者含 `FontScaleDpi`（`luban/os/desktop/src/host.cpp:184,250`）；字形以 `AddText(font, size)` 按未缩放的 15 px 绘制。缩放 2 时字形约 9–11 px 高、行距 30、窗口只容 64 列。**次因（实测）**：宿主无等宽字体时 `mono_font_ = uiFont`（`host.cpp:203`），窄字母放进"M"宽的格子。另：默认 15 px（`host.cppm:111`）偏小；宽字符放大 1.2 倍、粗体偏移重画、框线用字形 |
| A11 | 应用：缩放 2 时新建界面的文字溢出卡片、与说明行重叠 | 文字随 `FontScaleDpi` 放大，卡片宽高等是固定像素（`quick_start_`、`card_`，`app.cpp:565-720`），未乘缩放（截图 `quick-start-scale-2`） |
| A7 | 应用：desktop 窗口只有面板，什么都做不了 | U12、U13；CI 的 luban-window 场景截图（`3-desktop`、`4-machine`）正是空桌面，却判为通过 |
| A8 | 应用：管理与设置界面粗糙 | `admin_`（`app.cpp:1230`） |
| A9 | 应用：关闭对话框中"后台运行 / 关机"的单选圆在不悬停时看不出选中 | ImGui 默认 `RadioButton`（`app.cpp:925-929`），主题中 `FrameBg` 与弹窗背景几乎同色，只有 `FrameBgHovered` 可见 |
| A10 | 应用：选"后台运行"后宿主上看不到这台机器；最小化与后台都没有状态栏图标 | 选后台即 `a.quit = true`（`app.cpp:949-954`），应用退出；没有托盘。SDL 3.4.2 的托盘子系统已编入 `compat.sdl3`（`src/tray/{unix,cocoa,windows}`），但 Unix 实现运行时 dlopen GTK3 与 appindicator，而构建为 `-DSDL_DBUS=OFF`（O12） |
| I652 / I657 / I672 | 见各 issue | 见 §15、§16 |

## 2. probe 实测（#674）

| 编号 | 问题 | 结果 |
|---|---|---|
| O1 | 无 terminfo 时 vim 是否发备用屏幕序列 | 同一个 vim payload：隐藏 `/etc/terminfo`、`/usr/share/terminfo` 时**不发** `\e[?1049h/l`；存在时发；`luban enter pcore` 中不发。vim 的查找路径为 `/etc/terminfo:/usr/share/terminfo:/lib/terminfo:/usr/lib/terminfo` |
| O2 | generation 的目录条目数；bwrap 绑定成本 | core：677 条，目录 7（含 2 条布局链接）；desktop：726 条，目录 8。bwrap 0.11.2：0/10/50/100/200/500 条绑定的启动中位数 2.2/3.6/10.9/23.0/60.5/278.6 ms |
| O3 | 用户命名空间中能否换挂载 | 无 `CAP_SYS_ADMIN`：一切挂载被拒；有：叠加绑定、卸上层、只读重挂、延迟卸载都成功；`mount --move` 的系统调用成功（libmount 报无法更新 utab，改用系统调用即可）；对来自父命名空间的锁定挂载可叠加 |
| O3b | 保留能力后能否对用户进程丢弃 | 只有 `CAP_SYS_ADMIN`：无法缩 bounding set、无法设 securebits，子进程 exec 后仍可挂载；同时保留 `CAP_SETPCAP`：清空 bounding set、清 ambient、设 `SECBIT_NOROOT|LOCKED` 后子进程 CapEff 为 0，挂载被拒 |
| O4 | 更多宿主能否建出沙箱 | rockylinux:9、almalinux:9、centos:stream10、amazonlinux:2023、alpine:3.20、debian:13、opensuse/leap:15.6 容器，aarch64 的 debian:12、fedora、alpine 容器，Rocky 9 KVM（SELinux、5.14 内核、普通用户）：install、terminal 场景全部通过；edition 场景的失败为 U17、U18 |
| O6 | machine 中 `/usr/bin/vim` 与当前 subos | `/usr/bin/vim → /xlings/bin/xlings`（shim）确认；`luban run` 在 mode=machine 时仍以沙箱运行，machine 中的 `default` 未复现（U15 为推断） |
| O7 | desktop 的 `/usr/share` | 见 U12–U14；`XDG_DATA_DIRS` 为空；`.desktop` 文件只在 payload 中；整个系统无光标主题 |
| A6/A7 证据 | 现有截图 | CI 的 luban-window（run 38775618605）：快速开始、控制台（含 `can't access tty`）、desktop 与 machine（空桌面） |
| A6 | 终端在缩放 1、1.5、2 下（Xvfb、`SDL_VIDEO_X11_SCALING_FACTOR`、`--console-exec /bin/bash`） | 字形墨迹高度：9–12 / 9–11 / 9–11 px；行距：16 / 23 / 30；缩放 2 时 64 列；比例字体回退时窄字母间距明显过大（截图 `scale-*`、`proportional-1`） |
| A11 | 新建界面在缩放 2 下 | 文字溢出卡片并与说明行重叠（截图 `quick-start-scale-2`） |

## 3. 原则

| 编号 | 原则 |
|---|---|
| P1 | **一个问题一个答案。** 终端、事件转发、index 可用性、路径呈现、"当前系统"各只有一个实现 |
| P2 | **系统里像一台普通的机器。** 路径、终端、确认、Ctrl-C、用户权限的行为与常见发行版一致；差异（`/xlings` store）明说 |
| P3 | **检查只有一个来源。** 产品自检与 CI 跑同一套由 edition 与包声明的检查 |
| P4 | **依赖方向不变。** Luban → mcpp → xlings；联动通过 edition 的配置文件 |
| P5 | **界面以抓帧为准。** 关键界面的样子由截图证明，不由"代码走到了"证明 |

## 4. 终端中继与 broker

### 4.1 现状

| 路径 | 现状 |
|---|---|
| 会话主命令 | 继承调用者终端（`session.cpp:1152-1161`） |
| 加入会话 | ✅ 沙箱内 pty（`session.cpp:1240-1262`、`:968-1000`） |
| broker | `setsid`、强制 `-y`、`XLINGS_NON_INTERACTIVE=1`、不转发信号、不监视客户端 |
| 域内交互命令 | `bwrap --new-session`（`domain_producer.cpp:225`）；保留（`TIOCSTI` 防护），不改 |
| carrier guest | 各自 `new_session`（`luban/os/vm/src/guest.cpp:112,190,274`） |

### 4.2 设计

`platform::terminal_relay` 是唯一实现：

- **执行端**：调用方声明 `tty` 时建 pty，子进程 `setsid`、slave 为 stdio 与控制终端；否则用调用方给的管道。
- **调用端**：stdin 是终端时置 raw，双向转发；`SIGWINCH` 转为窗口大小；退出时恢复模式。
- Ctrl-C/Z/\\ 作为字节进入 pty，由行规程向前台进程组发信号；调用端断开时，执行端向进程组发 `SIGHUP`，宽限后 `SIGKILL`。

会话主命令、加入、broker 与 carrier guest 改用它；域内交互命令保留 `--new-session`（修订 5）。会话主命令即"第一个加入者"。broker：不追加 `-y`、不设 `XLINGS_NON_INTERACTIVE`（交互时与宿主一样列出计划并确认；
非交互且无 `-y` 时拒绝并提示）；客户端连接全程监视。`TIOCSTI` 防护不变。

### 4.3 取消

下载中 Ctrl-C：SIGINT 到安装进程，删除不完整的下载，未发布的条目不出现（原子发布）；退出码 130。

## 5. 域内命令的事件转发

`domain_producer::run_relayed(domain, argv, output, stream)` 是唯一入口：交互时内层在终端中继上运行，否则以
`interface subos_command` 运行并转发事件。`produce_domain_at_creation_`、`routed` 的 upgrade/boot/export、`src/cli.cpp:866`、
`cmd.cpp:1565` 全部改用它。

## 6. 系统里的路径与 `/usr/share`

### 6.1 用户视角

- 能 `cd` 进去的每个目录都有 FHS 路径，`pwd`、`pwd -P`、`getcwd`、报错与编译产物的调试路径里都是它。
- 合并 `/usr`（`/bin → usr/bin` 等）照常：`cd /lib; pwd` 为 `/lib`，`pwd -P` 为 `/usr/lib`。
- store 路径（`/xlings/...`）只作为**文件**链接的目标出现；`/xlings` 出现在 `/` 下，文档与欢迎信息中说明。

### 6.2 `/usr` 与目录条目

1. `/usr` 是挂载点：根中为空目录，它呈现的 generation 记录于 `etc/xlings/usr`。沙箱的根视图把 `<gen>/usr` 绑定到 `/usr`；
   machine 由 stage-0 或 guest agent 在启动 init 前绑定；drive 与 ISO 同 machine；tar 与 `--rootfs` 目录由其他 init（WSL、容器）启动，`/usr` 保留为链接。
2. generation 只对**文件**建符号链接；目录条目在 generation 中是空目录，记录于 `mounts.tsv`，呈现时逐条绑定（实测 ≤ 8 条，< 4 ms）。
   同一计划且同一形态复用当前代；所有权校验把 `mounts.tsv` 的条目视为空目录。
3. 换代：沙箱中由 supervisor 的根视图刷新（`root_mount::update`）撤下旧的 `/usr` 绑定、加上新的，在线生效；machine 中在旧挂载之上叠加新的。
4. 从宿主读根内的路径（init、登录 shell、会话、设置、镜像的内核）经 `rootfs::in_root` 与 `presented_usr`，与根内看到的一致。

### 6.3 能力

session-init 与用户进程都不保留能力：`/usr` 的换挂由命名空间之外的 supervisor 完成（§6.2 第 3 条）。
O3b 的实测（保留 `CAP_SYS_ADMIN` 需同时保留 `CAP_SETPCAP` 才能对子进程丢弃）因此不再需要。

### 6.4 payload 的 `share` 投影

`rootfs.cpp` 的认领规则增加 payload 的 `share` 子树，按目录的性质处理：

| `share` 下 | 处理 |
|---|---|
| 多个包会写入同一目录的（`applications`、`icons`、`icons/hicolor/*`、`fonts/*`、`man`、`man/<节>`、`locale/<语言>/LC_MESSAGES`、`wayland-sessions`、`xsessions`、`dbus-1/*`、`bash-completion/completions`、`fish/vendor_*`、`zsh/site-functions`、`pkgconfig`、`terminfo/*`、`metainfo`、`mime/packages`、`glib-2.0/schemas`） | generation 中为真实目录，逐文件链接合并；同名冲突按工作区顺序取第一个，写入这一代的清单 |
| 只属于一个包的目录（如 `vim`、`nvim`、`icons/<主题>`） | 目录条目，以绑定挂载呈现（§6.2） |

认领互斥：已认领路径的祖先或后代再被认领时记为冲突，一个链接之上与之下不再有链接。sysroot 的 `share` 按同一规则参与。
`XDG_DATA_DIRS` 未设时各程序按规范取 `/usr/local/share:/usr/share`，不另设。

## 7. 系统里的 index

- `xim::ensure_catalog(stream, need)`：catalog 未加载则同步，失败发带 hint 的结构化错误；install、list、info、search、remove、update 全部经过它。
- 建系统时带入所用 index 的快照（artifact 形式），系统里第一次需要时离线可用。

## 8. 系统里的用户与 home

部署形态 R（multi）的规定落实到 Luban 系统中：

| 谁 | home | `xlings install` 的结果 |
|---|---|---|
| root（或 `sudo`） | `/xlings`（系统层） | 装进系统，所有用户可见；系统换代 |
| 普通用户 | `~/.xlings`（用户层，system layer 为 `/xlings`） | 装进自己的 home，无需 root；系统已有的包经 borrow 使用，不重复下载 |

- 普通用户第一次运行 xlings 时自动建立用户层（`mode = user`，`XLINGS_SYSTEM_LAYER=/xlings`），说明一次。
- 用户想装进系统：`sudo xlings install --system <pkg>`；系统里没有 `sudo` 的 edition（tiny）提示 `su`。
- 今天的错误"`/xlings/.xlings.lock cannot be written`"改为：说明这是系统层、给出两种做法。

## 9. "当前系统"的判定

系统里的 xlings 判断"这个 subos 是谁"不依赖环境变量：

- 系统的 home 记录自己的身份：沙箱与 machine 都在根的 `/etc/xlings/root.json` 写明 `{"home": "/xlings", "subos": "<名字>"}`
  （stage-0 已经读这个文件，`luban/os/system/src/stage0/stage0.cppm` 的步骤 2）。
- xlings 在根里运行时，`root.json` 的 `subos` 优先于 home 的 `activeSubos`；`XLINGS_ACTIVE_SUBOS` 只在宿主上起作用。
- shim 因此在沙箱与 machine 中找到同一个工作区（U15，O11 复现）。测试格 E（machine）断言 edition 的每个程序都能运行（§12）。
- 主机名同理：沙箱与 machine 都取 `root_etc::hostname(etc, name)`，默认为系统名（U20）。

## 10. edition 内容（xim-pkgindex）

| 项 | 内容 |
|---|---|
| terminfo | 新包 `terminfo`（ncurses 数据库：xterm*、screen*、tmux*、linux、vt100、dumb、alacritty、kitty、foot、wezterm、ghostty）进 luban-tiny 新版本；`luban enter` 发现 `$TERM` 无条目时换为 `xterm-256color` 并说明一次 |
| 光标主题 | 新包 `adwaita-cursors`（O9：24–96 五档，HiDPI 完整，10.5 MB，LGPL-3 或 CC-BY-SA-3.0）进 desktop 系 edition，`/usr/share/icons/default/index.theme` 指向它；xdesk 与 labwc 的 `XCURSOR_THEME`、`XCURSOR_SIZE` 有默认值 |
| 应用入口 | 由 §6.4 的投影自然得到；desktop 的检查断言 `/usr/share/applications` 含 foot、nvim |
| 登录 shell | edition 声明 `login_shell`：core、agent-workspace、desktop 为 `/usr/bin/fish`，tiny 为 `/bin/sh`；`luban run`/`exec` 仍用 `/bin/sh -c` |
| 找不到命令 | fish 的 `fish_command_not_found`、bash 的 `command_not_found_handle`：本地 index 有同名程序时提示 `xlings install <pkg>`；编辑距离 ≤ 2 的已安装命令提示"是不是 …"（`xlnigs` → `xlings`） |
| mcpp | core、agent-workspace、desktop 的 factory `/etc/mcpp/config.toml`：`[xlings] upstream = ["/xlings"]`；默认工具链为 edition 已带的 glibc 版 gcc，写明 `<arch>-linux-gnu`（mcpp 在 aarch64 上把裸写的 `gcc@16.1.0` 解析为 musl 版，见 mcpp 设计 §2.2） |
| 检查声明 | edition 与包的检查（§11.2） |

## 11. 系统自检

### 11.1 时机

| 时机 | 执行者 | 失败时 |
|---|---|---|
| 建成 | xlings 在新系统中 | 不报 ready；详情在 `luban status <n> --check` |
| 升级 | 新 generation，切换前 | 不切换 |
| 启动 / 进入 | luban-init（machine）、session-init（沙箱）：快速子集，按 generation 摘要缓存 | luban-init 试下一个候选（`boot.json` 已有）；session-init 关键项失败拒绝进入并提示 `luban rollback <n>`，非关键项列在欢迎信息中 |
| 按需 | `luban status <n> --check`（宿主）与系统里的 `luban status --check` | 报告 |

`init` 一词保留给 PID 1 的 luban-init；检查是 `status` 的选项，不新增命令。

### 11.2 检查项

- **系统级**（edition 的 `checks`）：generation 指针、`/usr` 挂载、passwd/group/shadow、`/etc/shells`、hostname、machine-id、时区、locale、CA 证书、
  `XDG_DATA_DIRS` 下有应用入口（desktop）、光标主题（desktop）、terminfo 有 `$TERM` 的条目；**运行库闭包**（`/usr/bin`、`/usr/lib` 的每个 ELF 的解释器与 `NEEDED`）；
  **用户进程无能力**（§6.3）。
- **包级**（`check`）：gcc 编译运行 hello、`vim -es`、`nvim --headless +q`、`fish -c true`、`git init`、`mcpp new && mcpp build`。
- **默认**：其余包登记的每个程序 `--version` 退出 0。

## 12. 测试矩阵

### 12.1 维度

| 维度 | 取值 |
|---|---|
| 宿主 K | ubuntu-22.04、ubuntu-24.04（受限/开放）、ubuntu-24.04-arm；KVM：fedora、debian-12、debian-13、arch、**rocky-9**、opensuse-leap-15.6 |
| 宿主 U | fedora、archlinux、tumbleweed、debian:testing/12/13、ubuntu:20.04、**rockylinux:9、almalinux:9、centos:stream10、amazonlinux:2023、alpine:3.20、opensuse/leap:15.6**；aarch64：**debian:12、fedora、alpine** |
| 宿主（其他） | Windows（WSL2）、macOS（vz） |
| edition | nano、tiny、tiny-musl、core、agent-workspace、desktop、desktop-xdesk |
| 形态 | 沙箱；machine（x86_64 KVM、aarch64 TCG）；`--drive`/`--iso` + `try`；WSL2；vz |
| 用户 / 保留 / 网络 | root、普通用户；all、home、none；直连、`--proxy` |

NixOS 不在矩阵中。

### 12.2 检查层

| 层 | 内容 | 来源 |
|---|---|---|
| L0 系统状态 | §11.2 系统级 | 产品自检 |
| L1 终端 | `tty`；`sleep 30 & jobs; kill %1`；`/etc/shells` 的每个 shell 交互启动且无 `can't access tty`/`no job control`/`tcgetpgrp`；`stty size`；Ctrl-C；提示符 | CI（pty 驱动） |
| L1t | `infocmp $TERM`；vim、nvim 在 pty 中发 `\e[?1049h`/`\e[?1049l` | CI |
| L2 环境 | PATH、HOME、USER、LANG、TERM、`XDG_DATA_DIRS`；无宿主 `XLINGS_*`；`cd /usr/bin; pwd -P` 为 `/usr/bin` | 自检 + CI |
| L3 自带工具 | §11.2 包级与默认，**在每种形态中**（U15） | 自检 |
| L4 包管理 | update/search/info/list；无 `-y` 的 install 出现确认、回答 `n` 无变化；下载中 Ctrl-C 退出 130 无残留；普通用户 install 进自己的 home、无需 root；同名创建的错误为"已存在" | CI |
| L5 网络 | DNS、HTTPS（直连与代理） | CI |
| L6 生命周期 | `luban new` 在 pty 中有百分比、`--json` 有 progress；运行中的会话看到升级后的新一代；rollback、stop、rm | CI |
| L7 mcpp | core 中 `mcpp new && mcpp build && mcpp run` 无下载 | 自检 + CI |
| L8 桌面 | 经 opensal-wlr 抓帧：指针移动后帧中可见光标；启动器列出 foot；快捷键打开终端后帧中出现终端窗口；面板、壁纸、字体渲染（§13.8） | CI |
| LA 应用 | §13.8 的抓帧验证 | CI |

### 12.3 分配

L0–L4 每格都跑；其余按配对组合：

| 格 | 宿主 | edition | 形态 | 用户 / 保留 / 网络 | 额外层 |
|---|---|---|---|---|---|
| A | ubuntu-24.04 受限 | core | 沙箱 | root / all / 直连 | L5 L6 L7 |
| B | ubuntu-24.04-arm | core | 沙箱 | 普通 / home / 代理 | L5 L7 |
| C | fedora（KVM） | agent-workspace | 沙箱 | 普通 / none / 直连 | L6 |
| D | rocky-9（KVM） | tiny | 沙箱 | root / all / 代理 | L5 |
| E | ubuntu-24.04 | desktop | machine（KVM） | 普通 / all / 直连 | L3（所有程序）L8 LA |
| F | ubuntu-24.04-arm | tiny | `--drive` + `try`（TCG） | root / all / 直连 | — |
| G | ubuntu-24.04 | desktop-xdesk | 沙箱 | 普通 / all / 直连 | L8 |
| H | alpine（容器） | tiny-musl | 沙箱 | root / all / 直连 | — |
| I | debian-12 arm（容器） | tiny-musl | 沙箱 | root / all / 直连 | — |
| J | windows | core | WSL2 | root / all / 直连 | L7 LA |
| K | 其余 U 容器 | tiny | 沙箱 | root / all / 直连 | — |

`smoke`（每个 PR）：A、B、E、H；`full`（nightly 与发布）：全部。`edition` 场景按宿主是否需要一次性设置分支断言（U18）；
`bare.sh` 增加 apk 分支与 `dnf --allowerasing`（U19）。需求登记于 `tests/requirements.toml`：LUBAN-TTY、LUBAN-TERMINFO、LUBAN-FHS-PATHS、
LUBAN-SHARE-PROJECTION、LUBAN-INDEX-INSIDE、LUBAN-INSTALL-CONFIRM、LUBAN-INSTALL-CANCEL、LUBAN-USER-HOME、LUBAN-CURRENT-SYSTEM、
LUBAN-EDITION-SMOKE、LUBAN-NEW-PROGRESS、LUBAN-SELF-CHECK、LUBAN-MCPP-REUSE、LUBAN-DESKTOP-USABLE、LUBAN-APP-VISUAL。

### 12.4 实现

`tests/host-matrix/pty.py`（probe 的 `pty_run.py` 转正）；L0、L2、L3、L7 读 `luban status --check --json`；
`tests/host-matrix/scenarios/inside.sh` 参数化 edition、形态、用户、保留、网络。

## 13. Luban OS 应用

### 13.1 方向

**像素游戏风格为主基调**。Luban（鲁班）是造物的工匠：一个可以自己搭的系统，用"像素搭建"的视觉语言表达，
同时给各 edition 一个可识别的形象。可读性不让步：正文保持清晰，像素化体现在图形、面板、按钮、图标、动效与标题字。

**适用边界：** 像素风格只属于 **Luban OS 应用自身的界面**——首页、新建与创建进度、运行命令视图的外框、管理、设置、对话框、
Host 键菜单、托盘图标与菜单。**进入系统后的内容不受影响**：控制台视图里的终端是系统的终端，按标准终端忠实呈现（§13.7）；
桌面视图里是 guest 自己的桌面（xdesk、labwc 等），应用只负责把它的画面与输入原样传递（§13.9），不叠加任何风格。
两者之间只有一条应用自己的细顶栏（系统名、类型徽记、形态、Host 键提示），它属于应用界面。

### 13.2 视觉语言

| 项 | 规则 |
|---|---|
| 像素单位 | 界面以"像素单位"为网格（默认 1 单位 = 3 设备像素，按 DPI 取整数倍）；所有元素对齐到单位网格；纹理用最近邻采样，绝不模糊 |
| 调色板 | 有限调色板（32 色）：底色、面板、文字三档、强调色；**每种类型一个主色**：nano 石灰、tiny 草绿、tiny-musl 青、core 铁蓝、agent-workspace 紫、desktop 橙、desktop-xdesk 品红 |
| 类型形象 | 每个 edition 一个 32×32 像素徽记（nano：一块方砖；tiny：小木屋；tiny-musl：带齿轮的小屋；core：工作台与锤子；agent-workspace：机器人；desktop：显示器与窗；desktop-xdesk：无限画布与星点），带 2–4 帧待机动画 |
| 面板与按钮 | 9 宫格像素贴图：凸起（默认）、悬停（亮一级并上移 1 单位）、按下（下沉 1 单位、阴影消失）、禁用（去饱和）、焦点（2 单位强调色描边，脉动） |
| 字体 | 应用界面的标题：像素字体（Fusion Pixel Font，OFL-1.1，覆盖 CJK）；应用界面的正文：清晰的无衬线（Inter + Noto Sans SC 子集）；控制台中的终端：常规等宽字体（JetBrains Mono + Sarasa Mono SC，CJK 为严格 2 格宽），不使用像素字体。字体随应用发布（新包 `luban-fonts`，`luban-os` 依赖它），不再按路径在宿主上找 |
| 背景 | 低饱和像素纹理（砖、木纹或星空，按主题）加轻微视差；可在设置中关闭 |
| 动效 | 选中：徽记跳一下（2 帧）；创建中：工匠搭砖的循环动画；转场：像素溶解（120 ms）；"减少动效"设置下全部关闭 |
| 可达性 | 文字与背景对比度 ≥ 4.5:1；所有操作可用键盘完成；焦点始终可见；中英文布局都不截断 |

资产（徽记、9 宫格、背景、光标）在 `apps/luban-os/res/` 中以 PNG 维护，构建时打成图集；全部为项目自绘，许可随仓库。

### 13.3 响应性与缩放（A1、A11）

主循环改为"有事就连续画"：任何输入、窗口事件、后台任务的事件、动画进行中，都立即连续绘制（至少 3 帧，动画期间 60 fps）；
只有静止时才进入长等待。点击到选中呈现的延迟目标 ≤ 1 帧（16 ms）；LA 断言：脚本点击后下一帧的状态与截图已是选中。

所有界面尺寸以"逻辑像素"书写，经一个函数（`ui::px(v) = round(v × 显示缩放)`）换算为设备像素；字号、间距、卡片尺寸、贴图缩放都经过它，
不再有直接写死的设备像素。像素贴图按整数倍放大（缩放 1.5 时取 1× 或 2× 中较接近者并居中），保持清晰。LA 在缩放 1、1.5、2 下抓取每个关键界面。

### 13.4 首页（系统列表）

- 顺序：**最近打开的 3 个**在上（单独一组，标"最近"），其余**按名字的字典序**。
- 每行：徽记（类型主色）、名字、edition 与版本、形态（沙箱 / machine）、状态（运行中 / 已停止）、上次打开时间；类型差异一眼可辨。
- 键盘：↑↓ 移动且**列表跟随滚动**（A4）；Enter 进入；F2 管理；直接键入即筛选；Delete 删除（确认）。
- nano 行显示"无 shell · 运行命令"，Enter 打开"运行命令"视图（§13.6），不再尝试进入 shell（A3）。
- 空列表：居中的工匠形象与"新建一个系统"。

### 13.5 新建与创建

- 布局：两排卡片——**控制台**（nano、tiny、tiny-musl、core、agent-workspace）与**桌面**（desktop、desktop-xdesk）；每排至多 4–5 张，
  卡片纵向（徽记、名字、一句话、下载体积估计、包含什么的前 4 项），统一网格。
- 选中卡片后，右侧（宽窗口）或下方（窄窗口）是**一块**设置面板：名字、形态（沙箱 / machine，desktop 默认 machine）、"更多"折叠区（代理、保留模式、登录用户与密码）；
  主按钮"创建并进入"。键盘：←→↑↓ 在卡片间移动，Tab 到面板，Enter 创建。
- 创建中：全屏进度视图，分阶段（解析、下载、解压、配置、自检）与百分比（事件来自 §5），工匠动画，可取消（§4.3）。
- **创建即进入**：控制台类进入控制台，桌面类进入窗口，nano 进入"运行命令"视图；失败时停在进度视图，给出原因、hint 与"重试"。

### 13.6 系统视图

| 视图 | 内容 |
|---|---|
| 控制台 | 终端（§13.7）；顶部细条：系统名、类型徽记、形态、Host 键提示 |
| 运行命令（nano） | 一行输入、输出区、历史；每条命令经 `luban run`；说明"nano 没有 shell" |
| 桌面 | guest 的画面原样呈现，不叠加应用的风格；窗口与宿主的交互按 §13.9 |

### 13.7 终端渲染（A6；系统的内容，不属于像素风格）

- 目标是一个标准、忠实的终端：配色取终端序列指定的颜色与一套常规的默认调色板，不套用应用的像素调色板、贴图或动效。
- **一个字号贯穿度量与绘制（A6 的主因）**：`PushFont` 后取有效像素字号 `px = ImGui::GetFontSize()`（已含显示缩放），单元宽高与 `AddText` 都用 `px`；
  缩放为 2 时字形与格子同时放大，列数与缩放 1 时一致。
- **默认字号与缩放**：默认 16 逻辑像素（今天 15）；Ctrl+= / Ctrl+- / Ctrl+0 放大、缩小、复位，按系统记住；设置页可改默认字号。
- 字体随应用发布（§13.2），不再按路径在宿主上查找、不再以比例字体充当等宽字体（A6 的次因）；单元尺寸取字体度量并**取整到设备像素**：
  宽 = 字形前进宽度取整，高 = ascent + descent + line gap 取整；所有字形放在整数坐标上。
- LA：在缩放 1、1.5、2 下抓同一段文本，断言字形墨迹高度与行距之比在固定区间内、列数随窗口宽度而非缩放变化、等宽字母（`iiii` 与 `WWWW`）占相同宽度。
- CJK 用 Sarasa Mono SC 的字形，严格占 2 格，不缩放。
- 粗体用真正的粗体字重；框线与块元素（U+2500–U+259F）按单元程序化绘制，连续无缝。
- 内边距 1 个像素单位；光标样式随终端序列（块、竖线、下划线）；回滚与鼠标选择复制；OSC 52 在剪贴板授权下可用；窗口尺寸变化同步到 pty。

### 13.8 抓帧视觉验证（LA）

现有基础：`luban-os --headless --script` 与 `write_screenshot_`（`app.cpp:1472-1731`）。在此之上：

1. **覆盖**：首页（空、1 个、12 个含滚动）、新建（每张卡片选中、窄窗口）、创建进度（各阶段、失败）、控制台（含 CJK、框线、粗体、颜色）、
   运行命令、desktop 窗口（光标、启动器、终端已打开）、Host 键菜单、关闭对话框（两种选择各一帧）、管理的每个页签、设置；× 三种尺寸（960×600、1280×800、1920×1080）× 中英文。
2. **结构断言**：脚本从 `state_` 读每个控件的矩形：不越出窗口、彼此不重叠、对齐到像素网格、文字不截断、焦点可见。
3. **基准图比对**：`tests/luban-os/baseline/` 存经批准的截图；逐像素比对（容差 0.5%），差异图与对照页作为 artifact；
   设计变更在同一 PR 中更新基准图，评审者看差异图批准。
4. **视觉评审**：每个 PR 生成一张对照页（全部关键帧缩略图）贴在 job summary；agent 按清单（网格、对比度、类型区分、留白、截断、焦点、一致性）逐帧评审并写入 PR；
   基准图的更新经评审批准。
5. **托盘**：菜单结构与状态以脚本读取的状态断言（`state_` 增加托盘的菜单树与图标状态）；Windows 与 macOS runner 上另抓整屏，含通知区域或菜单栏；Linux 以测试用的 StatusNotifierWatcher 断言注册与菜单。
6. **desktop 帧**：经 opensal-wlr 的 screencopy 抓 guest 帧：指针移动后光标像素存在；启动器列出 foot；快捷键打开终端后出现窗口。

### 13.9 窗口与宿主的交互契约

| 项 | 规则 |
|---|---|
| 光标 | 应用声明 opensal 的 `Cursor` 接口：guest 的光标图像与热点由 opensal-wlr 送到宿主，宿主用原生光标呈现（低延迟，不依赖帧）；guest 未送光标时显示宿主箭头（不再无条件隐藏，`app.cpp:1144`） |
| 键盘 | 窗口聚焦即捕获键盘（Super、Alt+Tab 进 guest）；Host 键（Ctrl+Alt）释放并打开菜单；释放状态在顶部细条可见 |
| 输入法 | 宿主输入法的组合文本经 `Text` 接口送入 guest 的光标处（已有） |
| 剪贴板 | 按 `clipboard`、`clipboard-paste` 授权双向（已有）；未授权时菜单中可一次性粘贴 |
| 尺寸与 DPI | 窗口尺寸变化同步为 guest 输出模式；DPI 变化同步密度（已有） |
| 文件拖放 | 拖入窗口的文件经 virtiofs 共享目录（O10：Linux 以普通用户运行 virtiofsd，内核已含 virtio-fs；macOS 用 vz 的 virtio-fs）复制到 guest 用户的 `~/Downloads`，guest 弹出提示；方向反过来经菜单"保存到宿主" |
| 焦点与关闭 | 关闭窗口时按 `on-close`（询问 / 保持运行 / 关机），已有 |

### 13.10 管理与设置（A8）

- **系统管理**（每个系统，页签）：概览（类型、版本、generation、形态、大小、运行状态、自检结果）、版本（generation 列表、升级预览与执行、回滚）、
  设置（形态、保留模式、shell、桌面会话、网络与代理、剪贴板授权、关闭行为）、导出与删除（危险区，二次确认）。
- **应用设置**：语言、主题与像素单位、减少动效、镜像与代理、更新。
- 所有设置项与 `luban config` 一一对应，界面是它的呈现，不另存一份。

### 13.11 关闭窗口

- 对话框重做为**两张选项卡片**：「后台运行——系统继续运行，Luban OS 留在状态栏」与「关机——停止系统」（`temp` 时注明"修改将丢弃"）；
  每张卡片有像素图标、标题、一句后果说明；选中的卡片为强调色描边加勾选标记与轻微抬起，未选中为平面；默认选中上次的选择。
- 一个"记住我的选择"复选（替代第三个按钮），主按钮"确定"、次按钮"取消"；←→ 切换，Enter 确定，Esc 取消。
- 选"后台运行"：窗口关闭，应用进入托盘（§13.12），不退出；选"关机"：停止该系统，若无其他运行中的系统且未设置"常驻托盘"则退出。

### 13.12 状态栏 / 托盘图标

| 项 | 规则 |
|---|---|
| 何时出现 | 有系统在后台运行、窗口最小化（设置"最小化到托盘"时）、或设置了"常驻托盘"时；三者都不成立时消失 |
| 图标 | 像素鲁班徽记；角标给出运行中的系统数；颜色：空闲灰、运行绿、进行中（创建、升级、下载）为两帧动画、出错红；macOS 用单色模板图标以适配深浅色菜单栏 |
| 提示 | 悬停显示"Luban OS · 2 个系统运行中"及进行中的任务与百分比 |
| 左键 | 打开或聚焦主窗口（停在首页；只有一个运行中的系统时直接打开它） |
| 菜单 | 运行中的系统逐个列出（类型徽记、名字、形态、运行时长），子菜单：打开窗口 / 打开控制台、暂停（machine）、重启、关机、管理；分隔线；新建系统…、打开 Luban OS、设置、退出（有运行中的系统时询问：保持运行退出界面，或全部关机） |
| 通知 | 创建完成、升级完成、自检失败、machine 意外停止：系统通知（Windows Toast、macOS 通知中心、Linux `org.freedesktop.Notifications`），点击打开对应系统 |
| 实现 | Windows、macOS：SDL3 的 `SDL_CreateTray`（原生通知区域与菜单栏；O12 实测可用）。Windows 11 把新图标收入溢出区：第一次进入后台运行时，应用以通知说明如何把图标固定到任务栏角落（程序不能自行固定）。Linux：SDL 的托盘需运行时加载 GTK 与 appindicator（O12 实测：缺少时创建失败），静态构建无法使用；因此 `platform::status_item` 自行实现 StatusNotifierItem 与 DBusMenu（经 D-Bus，不依赖 GTK，静态构建可用；KDE、XFCE、Cinnamon、Ubuntu 的 GNOME、waybar 等可显示）；没有 StatusNotifierWatcher 的桌面（原生 GNOME）退回为"最小化的窗口加系统通知"，设置页说明原因 |
| 状态来源 | 与 `luban ls --json` 同源（服务层的 `describe`），加上后台任务的事件；不另存一份 |
| 设置 | 最小化到托盘（默认开）、常驻托盘（默认关）、登录时启动到托盘（默认关） |

### 13.13 控件规范

- 不直接使用 ImGui 的默认控件外观：按钮、单选、复选、开关、输入框、下拉、卡片、页签、列表行由像素控件库（`apps/luban-os/src/ui/`）统一绘制，状态（默认、悬停、按下、选中、焦点、禁用）各有贴图。
- **选中状态无需悬停即可分辨**：选中与未选中的指示部分对比度 ≥ 3:1，且不只靠颜色（勾选、填充或描边形状不同）。
- LA 断言：每个可选控件在"未选中、选中、焦点"三种状态下各抓一帧，指示区域的像素差异超过阈值；悬停不参与判定。

## 14. aarch64（#657 全部）

工具表修正（qemu-x86、qemu-arm）并测试工具表的包都存在；每架构一张表（UEFI 加载器、BIOS、qemu 程序与参数、固件名）；
aarch64 的 `--drive` 只有 UEFI；内置 ISO 写入器增加 UEFI El Torito 引导项（x86_64 同时 BIOS 与 UEFI、不再依赖 xorriso；aarch64 只有 UEFI）；
`try` 在 aarch64 用 `-M virt` 与 edk2；tiny-musl 在 xim-pkgindex 原生构建 aarch64 musl，宿主矩阵断言改为"建成、进入、musl 程序运行"（格 I）；提示给出本机三元组。

## 15. #652、#672

- **#652**：`process_xvm_operations_` 返回 `std::expected<void, std::string>`，原因进入 `InstallStatus.message`；测试断言消息含资产名。mcpp 由此在失败时有原因可显示。
- **#672**：去掉 `setup-wsl@v5`，用例并入 Windows 工作流的 WSL 2 job；`wsl -d luban --exec /bin/sh -c '…'`；去掉 `continue-on-error`；按检查结果记录。

## 16. xim-pkgindex：Luban 的包放到 `pkgs/luban/`

libxpkg 的 loader 遍历 `pkgs/` 下任意子目录（`libxpkg/src/xpkg-loader-boundary.cpp:67`，自 2026-03-01 起），`_PKGINDEX_DIR` 按上三级推导；
包的身份来自文件内容。需改：`version-bump.yml:126`（按首字母拼路径）、测试中的路径字面量、`luban-agent-workspace.yml` 的路径过滤。
归属：只为 Luban 存在的包（各 edition、luban-boot-*、luban-os、xdesk、labwc-desktop、luban-fonts、检查与 factory 配置包）；通用包留在字母目录。

## 17. 与 mcpp 的生态联动

| 联动 | 位置 |
|---|---|
| mcpp 设计的 X1–X6 | 本次 xlings 发布 |
| mcpp 设计的 X7（Luban 宿主与系统内部的 mcpp 配置） | xim-pkgindex 的 edition 新版本（§10） |
| 系统里的 mcpp 复用系统工具链 | §10、L7 |
| mcpp 收到安装失败原因 | #652 |
| 系统里的普通用户使用 mcpp | §8：用户层 home；mcpp 的上游仍为 `/xlings`（root 所有，满足 mcpp 设计 §8.2） |

## 18. 诊断

| 情形 | 等级 |
|---|---|
| 进入时关键自检失败 | 拒绝进入，提示 `luban rollback <n>` |
| 进入时非关键自检失败 | 欢迎信息列出 |
| 升级的新 generation 自检失败 | 不切换 |
| `$TERM` 无条目 | 换为 `xterm-256color`，说明一次 |
| 非交互且无 `-y` 的 install | 拒绝并提示 `-y` |
| 普通用户首次使用 xlings | 建立用户层，说明一次 |
| 同名创建 | "已存在"，给出 `luban rm` 或换名 |
| index 无法同步 | 结构化错误与 hint |

## 19. 提交与发布顺序

1. **xim-pkgindex PR**：`terminfo`、光标主题、`luban-fonts` 包；edition 新版本（terminfo、光标、login_shell、checks、mcpp 配置）；`pkgs/luban/`；aarch64 musl 与 tiny-musl。
2. **xlings PR**：§4–§9、§11–§15，Luban OS 应用（§13），mcpp 设计的 X1–X6；`XLINGS_INDEX_UNDER_TEST` 指向 1 的分支。
3. 1 合并 → 2 合并并发布 xlings → edition 的 `min_client` 指向该版本。
4. **mcpp PR** 钉该 xlings 版本，发布 mcpp 2026.10.16.1；xim-pkgindex bump mcpp，core 的下一版本带它。

## 20. 决策

| 编号 | 决策 |
|---|---|
| D1 | 一个 `terminal_relay`；主命令即第一个加入者 |
| D2 | broker 不强制 `-y`、不设非交互；客户端断开即结束被调用方 |
| D3 | 域内命令一个 `run_relayed` 入口 |
| D4 | `/usr` 是挂载点；目录条目绑定挂载；换代由根视图刷新在线生效 |
| D5 | session-init 与用户进程不保留能力（修订 5） |
| D6 | payload 的 `share` 投影：多来源目录逐文件合并，单来源目录绑定挂载；认领互斥 |
| D7 | `ensure_catalog` 一个入口；建系统带入 index 快照 |
| D8 | 系统里普通用户有自己的用户层 home；系统安装需 `sudo … --system` |
| D9 | "当前系统"由根的 `/etc/xlings/root.json` 决定 |
| D10 | terminfo、光标主题、字体包；edition 声明登录 shell |
| D11 | 自检挂在 luban-init 与 session-init，`luban status --check` 按需；CI 复用 |
| D12 | 宿主矩阵扩到 §12.1 的腿 |
| D13 | aarch64 全部，ISO 内置双引导项 |
| D14 | Luban 专属包移到 `pkgs/luban/` |
| D15 | Luban OS 应用：像素游戏风格主基调（§13.2），只用于应用自身的界面，不影响系统里的终端与桌面；信息架构与界面按 §13.4–§13.6、§13.10 |
| D16 | 应用的主循环"有事就连续画" |
| D17 | 终端字体随应用发布，单元整数网格，框线程序化绘制 |
| D18 | 窗口与宿主的交互契约（§13.9），光标经 opensal `Cursor` 接口 |
| D19 | 关键界面逐 PR 抓帧：结构断言、基准图比对、视觉评审 |
| D20 | 关闭对话框为两张选项卡片；"后台运行"时应用进入托盘而不退出 |
| D21 | 状态栏/托盘图标：Windows、macOS 用 SDL3 托盘；Linux 自实现 StatusNotifierItem 与 DBusMenu，无 watcher 时退回最小化窗口加通知 |
| D22 | 像素控件库统一所有控件；选中状态无需悬停可分辨，由抓帧断言 |
| D23 | NixOS 不进测试矩阵 |
| D24 | 像素风格的取向在第一版抓帧对照页上选定（O8） |
| D25 | 终端：一个有效字号贯穿度量与绘制；默认 16 逻辑像素，Ctrl+= / - / 0 缩放并记住；字体随应用发布 |
| D26 | 应用界面所有尺寸经 `ui::px` 换算；LA 在缩放 1、1.5、2 下抓帧 |
| D27 | 光标主题 Adwaita；文件拖放走 virtiofs |
| D28 | machine 与沙箱的主机名同一规则 |

## 21. 开放问题

O1–O4、O6、O7、O9–O12 已由 probe 关闭（§2）。

| 编号 | 问题 | 方式 |
|---|---|---|
| O8 | 像素风格的具体取向：调色板（暖色木作 / 冷色科技 / 两套主题）、背景纹理（砖、木纹、星空）、徽记形象 | 已确认方式：第一版实现后，在抓帧对照页上经评审选定，选定的写入 §13.2 并成为基准图 |

## 22. 自我 review

| 角度 | 发现与处理 |
|---|---|
| 测试的有效性 | 现有 luban-window 场景截到了空桌面与 `can't access tty` 却判为通过：只看"代码走到了"。P5 与 §13.8 把截图内容本身作为断言对象 |
| 根因合并 | U12、U13、U14 与 U4 都来自"payload 的 `share` 不进系统"或"系统缺基础数据包"；以 §6.4 的投影规则加 §10 的数据包一次解决，不逐个补 |
| 一致性 | U2、U5、U6、U18 与域内交互命令同属终端与 broker；D1、D2 一次解决 |
| 安全 | 修订 5 起不保留能力，`/usr` 的换挂在命名空间之外完成，攻击面不增加；broker 恢复确认后，确认发生在宿主侧执行端，沙箱内进程无法代答；上游信任边界见 mcpp 设计 §8.2 |
| 用户与权限 | 修订 1 没有处理系统里的普通用户；§8 按已有的部署形态设计补上 |
| 风格边界 | 像素风格只属于应用界面；终端内容与 guest 桌面原样呈现。§13.1、§13.7 写明边界，LA 的基准图分别归属，避免风格改动波及系统内容 |
| 视觉与可读性 | 像素风格只用于图形与标题，正文与终端保持清晰；对比度、键盘可达、减少动效写入规则 |
| 响应性 | A1 是渲染循环的策略问题，不是性能问题；D16 修复并由 LA 断言 |
| 范围 | 内容多，但每项都属于"进入 Luban 之后的可用性"或其直接依赖；按 §19 的顺序在各仓库一个 PR 内完成 |
| 缩放（修订 4） | A6 与 A11 是同一类问题：界面与终端在缩放 ≠ 1 时没有统一换算。不分别修补，以 `ui::px` 与"一个有效字号"统一，并让 LA 覆盖多个缩放；CI 原先只在缩放 1 下截图，所以从未发现 |
| 实测纠正（修订 4） | 修订 3 把终端问题归因于字体回退与浮点字宽；实测显示主因是缩放下的度量与绘制不一致，回退是次因。§13.7 以实测为准 |
| 平台（修订 4） | Windows 11 的托盘图标默认在溢出区，程序无法自行固定；以首次提示说明，而不是假设图标可见 |
| 控件状态（修订 3） | A9 说明默认控件在自定义主题下会失去状态区分；不逐个调色，改为像素控件库加"选中可分辨"的抓帧断言，同类问题由测试拦截 |
| 后台语义（修订 3） | "后台运行"今天等于退出界面，用户失去对运行中机器的感知与控制；托盘让后台运行可见、可控，退出时对运行中的系统给出明确选择 |
| 平台差异（修订 3） | Linux 的托盘生态分裂（SNI、XEmbed、原生 GNOME 无托盘）；只实现 SNI 并对无 watcher 的桌面明确退回，不实现 XEmbed（X11 专属且已被各桌面弃用） |
| 跨仓库 | aarch64 上 mcpp 的 gcc 指 musl 版而 core 带 glibc 版：edition 的 mcpp 配置写明目标；发布顺序保证 edition 只在客户端可用后被选为 latest |
