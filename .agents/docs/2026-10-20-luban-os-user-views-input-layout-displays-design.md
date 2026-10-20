# Luban OS：系统里的 xlings（一份存储、每个用户一个视图、sudo）、键盘锁定、xdesk 的布局与交互、两个切换器、多显示器与显示设置，以及 2026.10.19.2 实测的问题

日期：2026-10-20　状态：修订 2，已评审　缘起：2026.10.19.2 发布后在 Ubuntu 24.04 GNOME（X11 会话）宿主上的实测
（`hi-luban`、`desktop-5`），以及之后关于系统里的 xlings、xdesk 交互、多显示器与安卓版的讨论。

范围：luban（`luban/os/cli` 的进度行）；Luban OS 应用与窗口（`apps/luban-os`、`luban/os/window`：键盘锁定、显示器窗口）；
`luban/specs/opensal`（多个显示器、显示器配置、窗口大小与锁定）；xdesk（`luban/xdesk/*`：平铺、层叠、画布、切换器、多输出、设置）；
xlings（`src/core`、`modules/subos`、`modules/confine`：系统里的用户视图、提权、失败分类、补全）；xim-pkgindex（`xim:sudo`、core 与 desktop
的新版本、`luban:cursors` 的新版本）。安卓版的预研单独记录在 `2026-10-20-luban-os-android-preliminary.md`，本轮不做。

本轮（R1 + R2）在一个 PR 里完成，按 §18 的检查点逐个 commit，每个阶段跑一次完整 CI。

## 0. 核心点

1. **系统里的 xlings 只有一份存储**（`/xlings`：index、目录、包，归 root 写）；**每个用户自动有一个视图**（自己的版本选择与 shim，
   没选过的继承系统默认），像 project 模式一样叠在上面；查找顺序 project › 用户视图 › 系统默认。用户装包时只为写存储经一个入口
   提权一次，选择记在自己的视图里；`sudo xlings …` 或 `--system` 改系统默认。所有 edition、sandbox 与 machine 一致（§4）。
2. **core 起自带 sudo**（desktop、agent-workspace 跟着得到），与常见桌面系统一致：sudo 要密码（15 分钟内不重复问）；
   建系统时不设密码则默认密码 `lubanoser`（当场说明、`luban status` 提示仍在用默认密码）；`--no-password` 是明确的选择：
   无密码登录且 sudo 免密；`secure_path` 与代理变量随 sudo 保留（§4.5）。
3. **上次的连锁失败从根上消失**：用户层不再有自己的存储、不在里面跑 recipe（Landlock 白名单的缺口、重试刷屏、看不到系统层、
   重复装都不再可能）；本地错误与网络错误分开，本地错误立即停、只说一次（§4.9、§4.10）。
4. **键盘锁定以宿主的回答为准**：失败不再当成功、获得焦点时重试；标题、提示与菜单写出真实状态（§5）。
5. **xdesk 的交互规则统一**：标题栏或 Super+拖动 = 移动，Super+右键拖动 = 调整大小，背景拖动 = 平移；命中按正在画的位置（§6）。
6. **平铺**：告诉客户端它是平铺的（xdg tiled）；间隔内 4、外 0；合计不到一屏时按比例占满；多列先平分、每列低于最小宽度才滚动；
   拖动时其他窗口实时移到落下后的位置；去掉悬停的宽度条（§7）。工作区可选纵向（§7.4）。
7. **层叠**：边角拖动调整大小、处理客户端的 `request_resize`（§8）。
8. **画布**：缩放只是视角，任何缩放下都能操作；点击不再自动放大，双击或 Super+0 才进入；普通滚轮平移，Super+滚轮或捏合缩放；
   新窗口就近放；吸附、成组、小地图（§9）。
9. **两个切换器**：Super+Tab 按应用（停 1 秒展开该应用的窗口），Alt+Tab 按空间展开的总览（保留真实布局、屏幕外的窗口汇聚进来、
   选中的放大）；总览自动折行；一个总览组件三种排布共用（§10）。
10. **多显示器**：每个 Luban OS 窗口是一块显示器；**分辨率就是窗口的大小**，"锁定大小"即固定分辨率；第一个窗口是主屏，
    之后的默认**镜像主屏**（进入时窗口调成源屏大小、宽高比锁定、可等比缩放）；xdesk 自带设置（显示页：镜像或扩展、主屏、方向、
    缩放、排列、锁定、设为分辨率），配置下发到对应的窗口；偏向设置的功能都进设置窗口：显示器、布局、外观、快捷键、关于
    （系统与 xdesk 的版本、渲染器、配置位置、开源仓库与许可证、复制系统信息）（§11）。
11. **美术**：工匠的锤子按人体运动重画为 4 帧（预备、下挥、击打、回弹，时长不均）；指针尾巴改正；标题栏上两角圆角、
    占满屏幕时全直角（§14）。
12. **快捷键按 Luban 自己的设计**：只占 Super 层（例外 Alt+Tab 系、Ctrl+Alt+T、Alt+F4）；不冲突时一个功能可有多个键；Super+键去那里、Super+Shift+同键带着窗口去、
    Super+Ctrl 管工作区；方向是空间的、越过边缘到相邻显示器；单按 Super 启动器、按住出提示；快捷键服务支持多套与绑定应用和命令；
    Super+/ 打开设置的"快捷键"页（即说明）；启动器能搜到设置、各页与动作（结果旁写快捷键）（§12）。
13. **性能一起做**：缩放与总览改用缩略图合成、帧缓冲复用、同机走共享内存、遮挡时不出帧；Super+O 连按 10 次的 CPU、静止时 0 帧、
    内存都有预算与 CI（§13）。有 GPU 时 GPU 优先，失败退回 pixman 并说明。
14. **每项功能有配套文档**：桌面、快捷键（新页 + 生成的键位表）、设置（新页）、系统里的用户与 sudo（§17）。
15. **任务与依赖**：C1–C12（含 C5b、C7a、C9b）与跨仓库任务，单 PR、检查点 commit（§18）；多角度自审（§19）。

## 1. 实测与定因

| 编号 | 现象 | 定因 | 依据 |
|---|---|---|---|
| G1 | `luban new` 时进度行刷屏 | 进度行用 `\r\033[K` 原地重画，但不按终端宽度截断；最长约 115 列，80 列终端折成两行，`\r` 只回到第二行行首，每次刷新多一行；解压阶段几十毫秒刷新一次 | `luban/os/cli/src/cli.cpp:148,185`；粘贴的输出每段在第 80 列断开 |
| G2 | 工匠抡锤不自然 | 三帧里手固定在 (x9–10, y8)，手臂不动，锤子绕手转；帧序 `{举, 平, 下, 平}` 等时 0.25 s，像节拍器 | `apps/luban-os/src/art/art.cpp:97-108,137` |
| G3 | 像素指针尾巴是歪的 | `default` 第 8–11 行尾巴每行右偏 1 px（45°），与箭头右边平行；常见的尾巴约 2 行偏 1 列、沿中轴伸出；12×12 放不下比例正常的尾巴 | `xim-pkgindex/tools/res/cursors/luban.cursors` |
| G4 | 平铺时标题比窗口往右长、窗口间隔很大 | xdesk 从不发 xdg tiled 状态；foot 在不是平铺、最大化、全屏时把尺寸向下取整到字符格，内容比分配的格子小最多一格（约 9×19 px）；标题按格子宽度画；间隔 8 再加上这块余量 | `luban/xdesk/compositor/src/compositor/windows.cpp:62-69`（无 `wlr_xdg_toplevel_set_tiled`） |
| G5 | 3 个窗口时有一个在屏幕外；左 4 叠右 1 时屏幕两侧留白 | 1–2 列平分，3 列起每列 1/2 屏宽，整条带比屏幕宽而滚动；列被设过宽度、合计不到 1 时整条带居中 | `luban/xdesk/layout/src/tile.cpp:113-128` |
| G6 | 拖动时位置预览常常不准 | 预览在副本上做一次落下再排布（拖出的列合并、宽度规则重算、滚动为露出落下的窗口而变），其他窗口却停在原位；判定区按三分之一切、无迟滞；落下后客户端按字符格取整（G4） | `luban/xdesk/layout/src/drag.cpp:17-48,130-138` |
| G7 | 拖动时的 1/3 1/2 2/3 1 时有时无、移上去会变位置 | 它是"新建一列"的宽度选项，停 300 ms 才出现；要选它就得移动指针，指针一动落点按新位置重算，目标一变它就移动或消失；没有写明是什么 | `luban/xdesk/compositor/src/compositor/drag.cpp:73-109` |
| G8 | Ctrl+Alt+K 没有反应，标题不显示锁定 | `grab_keyboard` 不检查 `SDL_SetWindowKeyboardGrab` 的返回就记成已锁；X11 下窗口未可见或宿主正持有锁定时 `XGrabKeyboard` 会失败，之后状态相同不再重试；Ctrl+Alt+K 只在两个假状态间切换；标题固定为 `Luban OS (desktop-5)`，切换无反馈。宿主的 Alt+Tab 能用说明键盘从没锁住（xdesk 只绑了 Super+Tab） | `luban/os/window/src/host.cpp:419-422`、`apps/luban-os/src/app.cpp:480-491`、`state.cpp:61-70`；宿主 `x11 ubuntu:GNOME` |
| G9 | Super+Tab 与 Alt+Tab 功能重复 | xdesk 只有一个按窗口的切换器（Super+Tab）；Alt+Tab 是宿主的 | `luban/xdesk/compositor/src/compositor/keys.cpp:33`、`overlay.cpp:204` |
| G10 | 层叠模式不能调整窗口大小 | 拖动只有 `none, pan, move, gap`；客户端的 `request_resize` 没有处理 | `luban/xdesk/compositor/src/compositor/server.cppm:153` |
| G11 | 画布上点一下窗口就放大；拖动时灵时不灵 | `live()` 是 `zoom == 1`；滚轮在背景上缩放，误触一格（0.91）后点击就居中放大到 100%、输入不交给应用、不能拖；画布里 Super+拖窗口内容被当成平移（与注释相反），Alt 被当成 Super；标题命中用未缩放的 `kTitle`；命中按目标位置、不按动画中的位置；新窗口只往右放 | `server.cppm:357`、`input.cpp:232-258,268-273`、`layout/src/canvas.cpp:56-72` |
| G12 | 桌面里普通用户 `xlings install codex` 失败：`metadata boundary: cannot capture recipe output` | 系统里 bwrap 不可用，recipe worker 用 Landlock 限制写入；白名单只有 `<home>/subos/<name>`、`/dev`、`/proc`，而 worker 的输出日志在 `<home>/run/subos/<name>/lua/…`、hook 日志在 `<home>/logs/…`、包在 `<home>/data/xpkgs`；打开被拒，所有 recipe 求值失败，目录建不起来 | `src/core/xim/lua_boundary.cpp:345,448-460,506-509`、`modules/confine/src/backends/linux_landlock.cpp:44`、`modules/subos/src/model/home_view.cppm:40` |
| G13 | 失败后刷一大屏，最后建议"检查网络或代理" | 本地确定失败被当网络问题：重新同步 4 个 index → 自动装 git（index 已从 artifact 取得）→ 装 git 又要目录 → 再失败，循环 3 次 | 实测输出 |
| G14 | 系统已有 codex、opencode，`install` 仍去装 | 用户层是一个完整的 home，不看系统层 | 实测；desktop 的包列表含 `xim:codex`、`xim:opencode` |
| G15 | `xlings use opencode`：不是这个 home 认识的名字（`opencode --version` 却能运行） | `collect_version_candidates_` 只读用户自己的版本库 | `src/core/xvm/commands.cpp:798-810` |
| G16 | 安装确认的提示难读 | 先问一句很长的 `[Y/n]`，答完再打印一行更长的解释 | 实测输出 |
| G17 | fish 不能补全 | 系统给用户写的 `fish/conf.d/xlings-user.fish` 只加 PATH，没有 xlings profile 里的 `__complete` 钩子，也没有 luban 的 | `modules/subos/src/views/root/root_etc.cpp:363-385` |
| G18 | 带桌面的系统里没有 sudo | index 里没有 sudo 包；edition 不带 | `xim-pkgindex/pkgs/s/`；L §8 的用户层模型不需要它 |
| G19 | core 的自检：`xmake starts -- exit 255` | 把所有大于 128 的退出码当信号；255 是 xmake 自己的 -1（以 root 运行被拒） | 已修：#685 |
| G20 | 托盘（2026.10.19.2，Ubuntu 24.04 GNOME）：右键没有菜单、单击也没有，只有双击能打开窗口；没有办法退出，只能 kill，100% 复现 | GNOME 的 AppIndicator 扩展：单击在菜单有项时打开菜单、双击才 `Activate`，所以扩展手里的菜单是空的或它的菜单客户端失败；托盘的 D-Bus 回答用 GLib 自己的解码（`gdbus`）逐项核对过都正确，定因要看 GNOME 的日志（§3.1） | `luban/kits/tray/src/native/linux.cpp`；扩展的 `indicatorStatusIcon.js`、`dbusMenu.js` |
| G21 | 托盘进程常驻 CPU 约 4% | 主循环每 100 ms 醒一次，总线线程也每 100 ms 轮询一次；每秒读一次各系统的状态 | `apps/luban-os/src/tray/tray.cpp`、`linux.cpp` |

## 2. 原则

1. **系统里只有一份事实**：包与 index 只在系统的存储里；用户只做选择，不复制存储。
2. **写系统只走一个入口**（AGENTS.md）：提权经 `xlings.subos.elevation`，有记录；agent 模式不等输入，给出确切的命令。
3. **本地错误不当网络错误**：确定会失败的不重试；错误说出原因与修法，只说一次。
4. **交互规则在所有排布里一致**：同一个手势在平铺、层叠、画布里是同一个意思。
5. **视角不是模式**：缩放、平移、总览都不改变能不能操作。
6. **看到的就是会发生的**：拖动时的预览就是落下后的样子；锁定状态看得见。
7. **一个概念一个机制**：分辨率只由窗口大小决定；总览只有一个组件。
8. **美术不突兀**：符合整体的像素风与视觉，联系表随 PR 附上。

## 3. 命令行进度行（G1）

- 每次重画前取终端列数（`platform::terminal_size(1)`，取不到按 80）；整行按**显示宽度**截到"列数 − 1"（`▸`、中文按显示宽度），
  超出时先截明细、再截标题，末尾加 `…`；留出最后一列（有的终端写满即折行）。
- 不是终端时照旧每个阶段一行。
- 测试：在 40 列的伪终端里运行 `luban new`，断言屏幕上始终只有一行 `▸`。

### 3.1 托盘（G20、G21）

- **菜单**：按 GNOME 日志里扩展说出的原因修；同时把已知的脆弱处一并去掉：根节点总带 `children-display`（菜单为空时也带），
  `GetLayout` 按请求的 `parentId` 回答子树，`GetGroupProperties` 只回答请求的编号，实现 `org.freedesktop.DBus.Peer`；
  菜单第一次被读之前就有内容（先 `update` 再注册）。
- **退出不依赖菜单**：`luban-os --quit`（与托盘菜单的"退出"同一条路：有窗口时由窗口确认，没有时直接结束托盘，系统照常运行）；
  中键（`SecondaryActivate`）打开窗口里的同一个菜单；窗口的主机菜单里也有"退出 Luban OS"。
- **CPU**：主循环在套接字与总线上阻塞等待（托盘图标的事件经一个本地 socket 对唤醒主循环），不再定时醒；各系统的状态
  按变化（窗口告诉托盘）加上每 5 秒一次兜底读取。静止时托盘的 CPU 接近 0（测试：10 秒内 CPU 时间 < 50 ms）。
- **测试**：托盘的测试在有 `gdbus` 时用 GLib 自己的解码读图标、菜单（带属性过滤）与分组属性；`luban-os --quit` 结束托盘。

## 4. 系统里的 xlings：一份存储、每个用户一个视图

### 4.1 现状（被替换的 L §8 模型）

普通用户的 `~/.xlings` 是一个完整的 home，叠在系统的 `/xlings` 之上：自己的 index、目录与包；第一次写入前确认一次。
实测的问题都由此而来（G12–G16）：它从头拉 index、在 Landlock 下跑 recipe、看不到系统层、重复装系统已有的包。

### 4.2 模型

| 层 | 内容 | 谁写 | 位置 |
|---|---|---|---|
| 存储 | index、目录、包（payload）、系统默认的版本库与 shim | root | `/xlings` |
| 用户视图 | 自己的版本选择、shim、个人配置、日志 | 用户自己 | `~/.xlings`（只有视图，没有 index 与 `data/xpkgs`） |
| project | `.xlings.json` | 项目 | 项目目录 |

查找顺序：project › 用户视图 › 系统默认。视图里没选过的名字一律继承系统默认；系统默认变了，没选过的用户跟着变。
视图沿用 xlings 已有的 view 类型（共享存储、自己的 PATH 与版本库），区别是存储属于另一个 home（`/xlings`）。

所有 edition、sandbox 与 machine 都是这一个模型（root 登录时视图就是系统默认）。

### 4.3 命令的语义

| 普通用户执行 | 结果 |
|---|---|
| 运行程序、`search`、`info`、`list`、`use`（不带版本） | 直接；`list` 与 `info` 合并两层并标出来源：`node 22.1 (yours)`、`node 20.3 (system)` |
| `use node 22`（存储里已有） | 只写自己的视图，不提权，不影响别人 |
| `install node@22`（存储里没有） | 包写进存储时提权一次（§4.4）；选择记在自己的视图里 |
| `install codex`（系统已提供） | 不装："codex 2.1 已由系统提供"；要别的版本写 `codex@<版本>` |
| `remove node@22` | 只是撤销自己视图里的选择；包是否从存储删除由 root 的 `remove` 决定 |
| `sudo xlings …` 或 `--system` | root 身份：改系统默认与存储，等同系统管理 |
| `update` | 视图没有 index；更新 index 是系统的事，提权一次 |

### 4.4 提权

- 有终端：一行说明加一个问题，例如

  ```
  node 22.1 要装进系统的存储（需要管理员权限）
  用 sudo 继续？[Y/n]
  ```

  确认后经 `xlings.subos.elevation` 以 sudo 运行一个只写存储的子命令（`__store-add <包>@<版本>`，plumbing，不在帮助与补全里），
  完成后在用户视图里记下选择；记录写进 elevation 日志。`__store-add` 不改变系统已有的默认版本（安装从不擅自切换）；
  系统里原本没有的名字第一次加入时成为系统里可用的版本。
- agent 模式与没有终端：不问，失败并给出确切命令（`sudo xlings install node@22`），退出码与错误码见 §4.12。
- 回答记住：问过一次后，同一会话不再重复说明。

### 4.5 sudo

- 新包 `xim:sudo`（index）：从源码构建，不用 PAM（读系统的 `/etc/shadow`）；setuid root。
- core 的新版本起带 sudo；desktop、agent-workspace 跟着 core 得到；nano、tiny 不带。
- 生成系统时 luban 写 `/etc/sudoers.d/luban`：
  - `%wheel ALL=(ALL:ALL) ALL`，sudo 要密码，`timestamp_timeout` 15 分钟（与 Ubuntu、Fedora、Debian 的默认一致；
    它们都不默认免密，免密只见于云镜像与 live 会话）；建系统时的登录用户加入 wheel。
  - 密码：`luban new` 问密码时直接回车（或 `-y`）用默认密码 `lubanoser`，当场说明"默认密码 lubanoser，用 passwd 修改"；
    `luban status <系统>` 在仍用默认密码时给一条提示；系统里的 ssh（若有）不允许密码登录。
  - `--no-password`：明确选择无密码登录，这个用户的 sudo 免密（`NOPASSWD`），当场说明。
  - `Defaults secure_path` 含 xlings 的程序目录（`sudo xlings` 能找到）。
  - `Defaults env_keep += "http_proxy https_proxy all_proxy no_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY LANG"`
    （否则 network.env 的代理在 sudo 下丢失）。
- **setuid 要真生效**（C6 第一步的验证结果）：machine 的根树是 9p 共享（`security_model=none`），文件显示宿主用户的属主，sudo 拒绝
  非 root 属主的程序与规则。做法：machine 的 init 在每次启动时把 sudo 程序（4755）、`/etc/sudoers` 与 `sudoers.d` 复制到本次启动的
  `/run`，改为 root 属主，绑定挂载回原位；`/usr/bin/sudo` 若不是 sudo 程序本身（经 shim）则不绑定并说明。payload 由 recipe 设 4755。
- sandbox 模式：user namespace 里 setuid 不起作用；写存储照旧经系统的 broker；自检不检查 sudo。
- agent-workspace 的 `agent-private` 策略：不给 agent 用 sudo（策略禁止 setuid 执行，或不把 agent 加入 wheel）。

### 4.6 实现要点

- 用户第一次运行 xlings 时生成视图（`~/.xlings`，只有 `config/`、版本库、`bin/` shim、`logs/`）；index、目录与包的读取指向 `/xlings`（只读）。
- 视图里没有 recipe 运行：安装与配置只在提权后的存储子命令里发生（root，不需要用户层的 Landlock 写入隔离）。
- `xlings-user.{sh,fish}`：PATH 依次为用户视图的 `bin`、系统的 `bin`。

### 4.7 引用与回收

root 删除或清理旧版本时，读每个用户视图的选择（`/etc/passwd` 里的各用户 home 的 `~/.xlings` 版本库，root 可读），
不删除任何用户正在选择的版本；被挡住时说出是哪个用户的哪个选择。（没有共享的可写登记目录：避免多一个用户可写的系统位置。）

### 4.8 补全（G17）

root_etc 给所有用户写补全钩子：`fish/conf.d/xlings-complete.fish`、`profile.d/xlings-complete.sh`，调用 `xlings __complete` 与
`luban __complete`（与 xlings profile 里的钩子同一份）。若 fish 自身的补全数据目录在系统里找不到，另行修复（C6 里确认）。

### 4.9 失败分类与重试（G13）

- 目录构建、recipe 求值、沙盒启动失败是**本地错误**：立即停止，一条错误写原因与修法，不重新同步、不装 git。
- 只有拉取 index 或包的网络错误才重试，并且只在 index 来自 git 时才需要 git；来自 artifact 时从不装 git。
- 错误文字不再一律提示"检查网络或代理"：只在网络错误时说。

### 4.10 G12 的直接修复

即使新模型下用户层不再跑 recipe，Landlock 的写入白名单仍要按 worker 实际写的目录给全（scratch、logs、hooks、xpkgs），
因为宿主上的 system layer 也会走这条路径；补一个测试（系统内、没有 bwrap、普通用户安装）。

### 4.11 删除与保留

- 删除：系统里的"装进你自己的 home"提示与切换；用户层的完整 home（index、目录、`data/xpkgs`）。
- 保留：宿主上的 system layer（deployment M）；sandbox 的 broker。
- Luban OS 尚未对外发布，旧系统不做迁移；读取方容忍旧的用户层（当作视图读，忽略其中的存储），不写入（AGENTS.md：home 的格式只往前走）。

### 4.12 接口

- 新错误码 `E_NEEDS_ROOT`（提示里是确切的 sudo 命令）；`list` 里系统提供的行状态为 `system`（payload 状态里已有的 `origin`
  说的是 payload 的来处，不再复用这个词）。
- 协议 1.15（只增加）；`--version` 的握手加 feature `user_views`、`needs_root`。发布前按记忆核对 main 上的版本与协议号是否已被占用。

### 4.13 实现：建立在 L §8 已有的借用之上

调研发现 L §8 的用户 home 已经有"视图"的大半：系统层（`/xlings`）的快照、按包借用系统的 payload（`home::layers::plan_borrow`、
`plan_borrow_package`）、借用的登记与在用户自己 subos 里的物化（头文件、库）。所以不另起一套，而是把用户 home 收窄成视图：

| 任务 | 内容 | 依赖 |
|---|---|---|
| V1 | 视图的 index 与存储就是系统的：`Config::global_data_dir()` 在视图里指向系统层的 `data`（目录、仓库、`xpkgs`）；`start_own_home` 不再复制 index；视图里的 `update` 需要 root | — |
| V2 | `install`：不带版本且系统已有 → 说"已由系统提供"，不装；存储里没有 → 经 elevation 运行 `sudo xlings __store-add <包>@<版本>`（root 只写存储、不改系统默认）；之后照常安装，所有节点都借用；仍有要在视图里装的节点时报错（视图里从不跑 recipe） | V1 |
| V3 | `use`、`list`、`info` 看得见系统层并标出来源（G15） | V1 |
| V4 | `remove` 在视图里只撤销自己的选择与借用的登记，从不删除存储 | V1 |
| V5 | root 删除或清理时读各用户 home 的借用登记，不删正在被选择的版本，并说出是谁 | V2 |
| V6 | 提权的问法（G16）与 agent 模式的 `E_NEEDS_ROOT` | V2 |
| V7 | 本地错误不重试（G13）：目录构建的失败按原因分；只有 index 缺失或损坏才重新同步 | — |
| V8 | Landlock 的写入白名单（G12）：worker 的私有目录、recipe 与 hook 日志、本 home 的存储 | — |
| V9 | 补全钩子写给所有用户（G17） | — |
| V10 | 接口 1.15 | V2、V3、V6 |
| V11 | sudo：`xim:sudo`、core 的新版本、sudoers、默认密码、`--no-password`、setuid 验证 | 跨仓库 |
| V12 | 系统里读得到 index（2026.10.20.2，实测发现：桌面系统里 root 与 lubanoser 的 `xlings search/info/list` 都说"读不到"或"找不到"）。两个原因：① 目录缓存（`.xlings-index-cache.json`）按绝对路径记 recipe，随 index 带进系统的仍指向主人的 home，里面便要重建——沙箱的 `/xlings` 连 root 也只读；生产者在建系统时、以及每次经它进入系统时（`domain_producer::run`），把主人的缓存改写成系统里看到的路径（`logicalHome/data/...`），内容相同就不写，只对同一快照（index 版本标记相同）；② 有策略的系统（桌面有声音的授权）里读 recipe 又起一个受该策略约束的 worker，它要写 home——系统里该策略本就包在整个进程外，读 recipe（metadata、建目录）就按无策略的 home 读（`lua_boundary::read_policy`）；装包的 worker 不变。`__root check`（非 fast）多一项"package-index"。同批：主机没有声音服务时每次进入都警告 `audio`——默认授权、无可修复，改为 `-v` 与 status 里说；升级时托管分发器的版本问的是名为 `source` 的副本（多入口程序不答），每次升级都报"DOWNGRADED X -> "，改问原文件 | V1 |

## 5. 键盘锁定（G8）

### 5.1 修法

- 锁定结果以 SDL 的返回为准（X11：`XGrabKeyboard`；Wayland：keyboard-shortcuts-inhibit；Windows：低级键盘钩子；macOS：限制写明）。
- 没成功：获得焦点时与之后每帧重试，直到成功或用户释放；不再把失败记成已锁。
- Ctrl+Alt+K 在"想锁 / 不想锁"之间切换；真实状态单独记录。

### 5.2 反馈

- 标题：`Luban OS (desktop-5) · 键盘已锁定（Ctrl+Alt+K 释放）`；没锁时只有 `Luban OS (desktop-5)`。
- 每次切换在顶部中间显示 1.5 秒："键盘：已锁定"、"已释放"、"宿主未允许锁定（原因）"。
- 菜单的键盘项显示同一状态；状态快照（`window.keyboard_grabbed`）写真实值。

### 5.3 平台

| 平台 | 机制 | 限制 |
|---|---|---|
| Linux X11 | `XGrabKeyboard` | 宿主正持有锁定（菜单、宿主的 Alt+Tab）时失败，重试 |
| Linux Wayland | keyboard-shortcuts-inhibit | 宿主可能弹框询问；compat.sdl3 目前只有 X11，Wayland 宿主上经 XWayland，锁定受宿主设置限制（写明） |
| Windows | SDL 的键盘钩子（`SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED` 设为 0） | Ctrl+Alt+Del 不可锁 |
| macOS | SDL 的锁定 | Cmd+Tab 等系统快捷键不可锁（写明） |

### 5.4 测试

Xvfb 加一个窗口管理器：锁定后 Alt+Tab 交给 xdesk；宿主持有锁定时失败并在其释放后成功；标题与提示文字。

## 6. xdesk 交互的共同规则

| 动作 | 结果（平铺、层叠、画布一致） |
|---|---|
| 拖标题栏，或 Super+拖窗口任意位置 | 移动窗口（平铺：重新排布；画布：组里的窗口被拖出） |
| Super+右键拖动、拖边或角（层叠与画布） | 调整大小 |
| 拖背景、中键拖任意位置 | 平移（平铺：横向或纵向滚动；画布：平移画布） |
| 单击 | 焦点；不改变视角 |
| 双击标题 | 最大化或还原；画布上缩放不是 100% 时：进入（回到 100% 并居中） |

- Alt 不再当成 Super 用（应用常用 Alt+点击）。
- 命中按**正在画的位置**判断（动画中的窗口也是）；标题栏区域随缩放调整。
- 拖动时看得见：落点的预览、吸附的参考线、成组时高亮的边；按住 Shift 关闭吸附与成组。

## 7. 平铺

### 7.1 告诉客户端它是平铺的（G4）

- 平铺、最大化时对四条边设 `wlr_xdg_toplevel_set_tiled`（最大化另设 maximized），客户端不再按字符格取整。
- 客户端仍提交比要求小的尺寸时：标题按实际提交的宽度画，剩下的部分用边框色填满，标题与内容不错位。

### 7.2 间隔

- `gap` 拆为 `inner_gap`（默认 4）与 `outer_gap`（默认 0），逻辑像素，可配置；旧的 `gap` 读作 `inner_gap`。

### 7.3 宽度规则（G5）

1. 列数 n 时先平分（1/n），直到每列低于 `min_column_width`（默认 480 逻辑像素），之后每列取最小宽度并开始滚动。
2. 列被设过宽度时：合计不到一屏 → 按比例放大到占满（保留比例）；超过一屏 → 滚动，焦点列保持在视野里。
3. 单列占满。

### 7.4 方向

工作区可设 `direction = horizontal | vertical`（默认横向）：纵向时列变成行、往下滚动，适合竖屏；规则与横向对称。

### 7.5 拖放（G6）

- 拖动时其他窗口实时动画到落下后的位置（预览就是结果）；被拖的窗口半透明跟着指针。
- 判定区：目标窗口的左半 / 右半 = 在它前 / 后新建一列；上 / 下四分之一 = 叠在它上 / 下；中心 = 交换；屏幕边缘 24 px = 首 / 尾新建一列。
- 判定区切换有迟滞（新区域要越过边界 12 px 才生效）。

### 7.6 宽度选择（G7）

去掉悬停的宽度条。新列的宽度按 §7.3 自动决定；拖动中滚轮或数字键 1–4 在 1/3、1/2、2/3、1 之间切换，预览框里写
"1/2 · 滚轮切换"；落下后拖列间的缝隙调整，或 Super+R 循环宽度。

## 8. 层叠：调整大小（G10）

- 窗口边与角各 6 px 的范围可拖，指针换成对应形状（opensal 已有 `n-resize` 等）；Super+右键在任意位置拖动。
- 处理客户端的 `request_resize`（客户端自绘装饰）；遵守 `min_size`、`max_size`。
- 平铺里同一列上下窗口之间的缝隙也可以拖。

## 9. 画布（G11）

场景：平时在 100% 下工作（一张无限大的普通桌面）；窗口很多时先找再进入；把相关窗口成组；像白板一样按空间摆放；
只用鼠标、用触控板、只用键盘。

1. **缩放只是视角**：任何缩放下窗口都能操作（输入坐标按缩放换算）；缩到 50% 以下窗口成为缩略图，单击 = 选中并获得焦点，不自动放大。
   进入一个窗口是明确的动作：双击或 Super+0（回到 100% 并以焦点窗口居中；Super+Enter 仍是终端，见 §12.2）。
2. **缩放只来自明确的手势**：Super+滚轮、触控板捏合、Super+= / Super+-、Super+0（100%）。背景上的普通滚轮平移
   （竖直滚动上下、Shift+滚轮左右）；触控板双指平移。
3. **拖动**按 §6。组有可见的外框与标签，拖组的标题或手柄整组移动。
4. **吸附与成组**：移动时离别的窗口边约 16 px 内吸附对齐并显示参考线；拖到别的窗口的边缘带，那条边高亮，松手成组；Shift 关闭。
5. **新窗口就近放**：在焦点窗口附近找最近的空位（右、下、左、上螺旋搜索），尽量在当前视野里；放不下就平滑平移露出它；从不自动缩放。
6. **知道自己在哪**：角落的小地图（可关）显示整张画布与视野框，点一下跳过去；Super+o 总览（§10.3），Esc 回到原来的视角。
7. **键盘**：Super+方向键焦点按空间移到那个方向最近的窗口并平移露出；Super+Shift+方向键移动窗口。

## 10. 切换器（G9）

### 10.1 Super+Tab：按应用

- 一排应用图标（最近使用的在前），有多个窗口的显示窗口数；Tab / Shift+Tab 移动。
- 在一个应用上停 1 秒（或按 ↓），下面展开它各窗口的实时预览，可继续 Tab 或用鼠标点；松开 Super 选定（默认该应用最近用过的窗口）。
- Super+` 在当前应用的窗口间切换。
- 应用的身份：Wayland 按 xdg `app_id`，X11 窗口按 class；图标从 `.desktop` 的 `Icon=` 在图标主题里找 PNG，找不到用首字母色块。

### 10.2 Alt+Tab：按空间展开的总览

- 当前显示器上按真实相对位置展示当前工作区的全部窗口（缩小到放得下）；屏幕外的窗口从它所在的方向汇聚进来（约 200 ms 缓动，减少动态时直接显示）。
- 默认选中当前焦点窗口；没有焦点时选当前显示器上的第一个（左上起）。
- Tab 的顺序按最近使用（来回两个窗口最快）；方向键按空间移动；选中的放大约 1.2 倍、浮在上面并带阴影，其余略暗；鼠标悬停与点击。
- 快按（150 ms 内松开 Alt）直接切到上一个窗口，不展开。
- 只有键盘锁定时 Alt+Tab 才交给 xdesk（没锁时是宿主的）。

### 10.3 总览组件

一个总览组件：Alt+Tab、画布的 Super+o、排布切换浮层共用。一条带超过约 2 个屏宽时自动折成多行，整体接近屏幕比例；
预览直接引用窗口的 buffer（实时，不截图）。

### 10.4 快捷键

`switch-apps` = Super+Tab、`windows` = Alt+Tab、`app-windows` = Super+`，加 Shift 反向；键位总表见 §12.2。
`keys.json` 的动作名：启动器沿用 `apps`（2026.10.19.2 起就是它），`windows` 仍是"按窗口切换"，只是键从 Super+Tab 改到 Alt+Tab，
所以旧的改键文件意思不变。

实现（C7）：总览组件是 `xdesk.layout` 的纯函数 `spread(窗口矩形, 区域)`——保留相对位置、只缩小、一条带超过 2 个区域宽时
按最接近区域比例的行数折行；Alt+Tab、Super+Tab 展开的应用窗口、Super+m 的卡片都用它。显示时移动的是窗口本身
（场景节点的位置加每个窗口自己的缩放，客户端尺寸不变、内容实时），选中的放大 1.2 倍（放不下就不放大）、其余透明度 0.6、
背景变暗；关闭时弹簧动画回到原位。Alt+Tab 按下 150 ms 后才显示，在此之前松开 Alt 直接切到上一个窗口。
Super+Tab 的图标：桌面项的 `Icon=`，按图标主题规范找 PNG（hicolor、breeze、Adwaita、Papirus，目标尺寸及以上优先，
再 pixmaps），libpng 解码（`xdesk.native.png`），按应用缓存；找不到用首字母色块（按应用 id 固定取 8 种颜色之一）。

## 11. 多显示器与显示设置

### 11.1 模型

- 每个 Luban OS 窗口是桌面的一块**显示器**；它在宿主的哪块屏上、同一块宿主屏上开了几个，都与桌面里的排列无关。
- 第一个进入桌面的窗口是**主屏**（面板在主屏）；之后进入的窗口默认**镜像主屏**，不询问。
- 主屏关闭时下一块成为主屏，原来镜像主屏的改为镜像新的主屏。

### 11.2 分辨率就是窗口的大小

| 情况 | 分辨率 | 行为 |
|---|---|---|
| 主屏与扩展屏 | 窗口大小 × 所在宿主屏的密度 | 缩放窗口即改分辨率；缩放停下约 300 ms 后桌面才重排，过程中拉伸显示旧画面 |
| 锁定大小（每个窗口的开关） | 锁定时的大小 | 窗口不能缩放；最大化不可用；全屏时固定的画面在宿主屏上居中等比显示 |
| 镜像屏 | 永远是源屏的分辨率 | 进入时窗口调成源屏的大小（像素对等）；宽高比锁定（`SDL_SetWindowAspectRatio`），可等比缩放，不影响源屏与其他镜像屏 |

- 镜像窗口初始大小超过它所在宿主屏的可用区域：等比缩小，内容缩放显示。
- 源屏大小变了：镜像窗口保持宽度、高度按新宽高比调整；最大化或全屏的镜像窗口不调，内容等比居中。
- 密度不同：源屏按自己的密度渲染；镜像整数倍用最近邻，非整数倍用线性过滤。

### 11.3 扩展

- 每块屏一个 headless 输出，加 `wlr_output_layout`；每块屏有自己的工作区、自己的 Alt+Tab 总览；面板只在主屏（可设每块屏都有）。
- 默认排列：按加入顺序从左到右，顶边对齐。
- 跨屏拖动：按住鼠标时 SDL 把事件一直交给最初的窗口，坐标可越出边界；越出部分按**桌面自己的排列**换算到相邻的屏。
- 指针锁定时越过桌面里的屏幕边界：锁定转到对应的窗口。
- 关掉一块扩展屏：上面的窗口移回主屏，排列保存不变。

### 11.4 设置 app

- xdesk 的内置窗口（xdesk.draw 绘制，与快捷键速查、菜单同一套），在布局里像普通窗口一样；从面板、启动器或 Super+I 打开（与 Windows 的 Win+I 一致；Super+I 目前未占用）。
  理由：只有合成器知道有哪些输出；compat.sdl3 在 Linux 上没有 Wayland，独立客户端要先补工具包；不引入新依赖。
- 显示页：
  - "已连接的显示器"：每个 Luban OS 窗口一项（"显示器 2 · my-pc · 1600×900 @2x"）。
  - 每块屏：模式（镜像某块 / 扩展）、主屏、方向（0/90/180/270）、缩放（跟随窗口或固定 1、1.5、2）、锁定大小、
    设为分辨率…（预设 1280×720、1920×1080 或输入宽高）。
  - 排列画布（扩展屏）：拖动矩形，靠边对齐。
  - "识别"：每个窗口闪出自己的编号。
  - "应用"后 15 秒内不确认就还原。
- **配置下发到窗口**：锁定大小、设为分辨率、宽高比由对应的 Luban OS 窗口执行（经 opensal 的显示器配置消息，宿主调整窗口并锁定），
  窗口菜单与标题同步显示。
- 实现标准的 wlr-output-management（`wlr-randr`、kanshi 可用）；内置设置与它用同一份内部模型。
- 保存：`state.json` 的 `displays`，按窗口身份（宿主 + 窗口编号）保存，同一个窗口再进来时恢复；窗口编号由托盘分配（最小的空闲号，关闭后复用），所以稳定。
- 偏向设置的功能都放进设置窗口（标题栏菜单与快捷键仍是快速入口），本轮的页：

| 页 | 内容 |
|---|---|
| 显示器 | 上面的显示页 |
| 布局 | 每个工作区的排布（平铺、层叠、画布）与方向；内外间隔；列的最小宽度；新窗口的放法（画布）；吸附与成组的开关；小地图；工作区数量 |
| 外观 | 标题栏圆角、动画与"减少动态"、配色（跟随 Luban 的主题）、字体大小 |
| 快捷键 | §12.4 |
| 关于 | 系统的基本信息：Luban OS（系统的 edition 与版本、generation、运行方式 sandbox / machine）、xdesk 的版本、内核、用户、渲染器（GPU 名称或"CPU（pixman）"及原因）、已连接的显示器、CPU 与内存；配置文件的位置（`keys.json`、`state.json`、profile）；开源项目与仓库（openxlings/xlings：xlings、luban、Luban OS、xdesk；openxlings/xim-pkgindex；openxlings/libxpkg）、许可证（Apache-2.0）与第三方许可（wlroots、SDL、Dear ImGui……）；"复制系统信息"（报问题时贴上）；升级的入口说明（在宿主上 `luban upgrade <系统>`） |

- 每页的设置都写进 xdesk 的配置（`state.json`、profile 之上的个人设置），与标题栏菜单、快捷键改的是同一份；改动立即生效，显示页另有 15 秒还原。

### 11.5 opensal

- 一个会话可以有多个显示器：`hello` 带显示器编号、名称与角色（主屏、镜像某块、扩展）。
- 新消息：显示器配置（宿主 → 合成器：窗口大小、密度、锁定；合成器 → 宿主：设为分辨率、锁定、宽高比、方向）。
- 镜像：同一块输出的帧发给多个连接，只渲染一次；同机时共用同一份共享内存。
- 版本：opensal 加一级（能力位 `Displays`）；旧宿主只有一块显示器，行为不变（compat 登记）。

### 11.6 machine 模式

显示通道支持多个连接（每个窗口一条）；先确认现有通道的连接模型，必要时在 agent 里复用。

### 11.7 实现（C8，第一步：镜像）

- **会合**：`opensal-wlr` 的 `run_session` 先连 `<runtime>/luban/displays-<会话>.sock`；连得上说明这个会话已在运行，本窗口的流就接到它上面
  （双向转发），不再启动第二个桌面。沙箱里第二个 `subos use --cmd` 加入同一个会话（同一个 /tmp、/run），machine 里由同一个 agent
  在同一个系统里运行，所以三种运行方式都是同一条路。
- **xdesk**：`OPENSAL_DISPLAYS` 给出 socket 路径，xdesk 监听；每个连接是一个 `Link`（各自的解码器、hello、节流、可见与否、
  欠一整帧）。只渲染一次，按每个窗口的状态发：没显示完上一帧的镜像跳过、之后补整帧；隐藏的不发。主屏的 Resized/Density 改输出，
  镜像的不改。主屏的流结束而还有镜像时，下一个成为主屏（它重发自己的大小与密度），`luban __desktop` 看到自己的输入断开而 xdesk
  仍在运行，就自己退出、留下 xdesk——会话因还有加入的命令而继续。
- **桥接会话**（labwc、KDE、LXQt）：桥接接受镜像（同一帧、输入合并、光标与 Shell 列表也发给镜像），主屏关闭时桌面结束（桥在
  `__desktop` 进程里，接管需要把桥移出它，留到扩展模式一起做）。
- **宿主窗口**：hello 带 `Displays`；收到 `'d'` 镜像时按源屏像素大小调整窗口（放不下等比缩小，最大化或全屏时不调），用
  `SDL_SetWindowAspectRatio` 锁宽高比，不再把自己的大小发给桌面；改为主屏时解锁并重发大小与密度。标题加"显示器 2（镜像显示器 1）"。
  第一个窗口关闭时最多等 3 秒它的进程结束，之后不再等（桌面可能还在为别的窗口服务）。
- **下一步**（C8b）：扩展模式——每块屏一个 headless 输出、各自的布局与工作区、跨屏拖动与指针、设置里的显示页与排列画布、
  `wlr-output-management`、按窗口身份保存；桥接会话的主屏接管。

## 12. 快捷键：Luban 键位、快捷键服务与设置页

### 12.1 原则

Luban 的键位按需求与交互设计来定，不照搬某个系统：

1. **只占 Super 层**：应用自己的快捷键（Ctrl、Alt 组合）一律不拦；例外只有三组肌肉记忆：Alt+Tab 系（窗口切换）、Ctrl+Alt+T（终端）、
   Alt+F4（关闭窗口）。打字时不会误触任何 Luban 的键。
2. **一个功能可以有多个键**：不冲突时允许别名（Super+Enter 与 Ctrl+Alt+T 都开终端，方向键与 hjkl，Super+Ctrl+←→ 与 Super+PageUp/PageDown），
   更自然、各种习惯都能用；快捷键页列出全部，按住提示只显示第一个。
3. **按层分工**：Super+键 = 去那里 / 做这件事；Super+Shift+同一个键 = **带着窗口**去那里；Super+Ctrl+键 = 工作区与显示器一级。
4. **方向是空间的**：Super+方向键在所有排布里都是"焦点移到那个方向最近的窗口"，越过屏幕边缘就到相邻的显示器；
   Super+Shift+方向键是"把窗口往那边移"（层叠里移到边缘即半屏贴靠、向上即最大化、向下即还原，同一个意思的自然延伸）。
5. **最常用的离 Super 最近**：单按 Super、Tab、方向键、Enter、Space。
6. **三种排布同名同键**：同一个操作在平铺、层叠、画布里是同一个键（§6），只是效果按排布解释。
7. **单按、按住、组合三种**：单按 Super = 启动器（搜索应用、设置页、窗口、命令）；按住约 1 秒 = 浮出常用键提示；与别的键一起 = 快捷键。
8. **宿主一级的键不在这里**：Ctrl+Alt+K（锁定键盘）、单按 Ctrl+Alt（窗口菜单）属于 Luban OS 窗口，不可改到 xdesk 里。

### 12.2 Luban 键位（默认）

| 分组 | 操作 | 键 | 与 2026.10.19.2 相比 |
|---|---|---|---|
| 找 | 启动器 | 单按 Super、Super+Space | 去掉 Super+D |
| | 按应用切换 / 同一应用的窗口间切换（`` ` `` 是 Tab 上方、1 左边的反引号键：开了两个终端时，在这两个之间切） | Super+Tab / Super+` | Super+Tab 原为按窗口 |
| | 按窗口切换（空间总览） | Alt+Tab（Shift 反向） | 新 |
| | 总览 | Super+O | |
| 焦点与移动 | 焦点到某个方向 | Super+←↑→↓（或 hjkl） | 越过边缘到相邻显示器 |
| | 把窗口移到某个方向 | Super+Shift+←↑→↓（或 hjkl） | 同上 |
| | 并进左边一列 / 拆出成单独一列 | Super+[ / Super+] | |
| 窗口 | 最大化 / 还原 | Super+F | 原为浮动；三种排布一致 |
| | 全屏 | Super+Shift+F | 新 |
| | 浮动 / 放回 | Super+Shift+Space | 原为 Super+F |
| | 置顶 / 所有工作区显示 | Super+P / Super+Shift+P | 原为 Super+Shift+T；后者新 |
| | 关闭 | Super+Q、Alt+F4 | Alt+F4 新 |
| 大小 | 列宽循环 / 列内高度循环 | Super+R / Super+Shift+R | |
| | 变宽 / 变窄（画布：放大 / 缩小） | Super+= / Super+- | |
| | 平分（画布：回到 100%） | Super+0 | 新 |
| 排布 | 平铺、层叠、画布 | Super+M | |
| 工作区 | 去第 n 个 / 带窗口去第 n 个 | Super+1…9 / Super+Shift+1…9 | 原为 1…4 |
| | 相邻的工作区 / 带窗口去 | Super+Ctrl+← →、Super+PageUp/PageDown / 再加 Shift | 新（纵向工作区用 ↑ ↓） |
| 打开 | 终端 | Super+Enter、Ctrl+Alt+T | 两个都保留 |
| | 设置 / 快捷键页 | Super+I / Super+/ | Super+/ 原为只读速查 |
| 鼠标 | 移动 / 调整大小 | Super+左键拖 / Super+右键拖 | 原 Alt 也算 Super，去掉 |
| | 缩放（画布）/ 滚动（平铺） | Super+滚轮 | |

键名、动作名沿用 `keys.cppm` 的动作表；改动写进发布说明与用户文档（C11）。工作区按需增加，最多 9 个（面板只显示有窗口的与当前的）。

### 12.3 快捷键服务

- 在 xdesk 里（按键必先经过合成器），扩展 `keys.cppm`：动作表（名字、中英说明、分组、默认键）、组合键解析、绑定、冲突检测。
- **多套**：内置一套 Luban（另有它的变体"Luban 不带 hjkl"）；用户可复制成自己的套（命名、切换、导入、导出）；
  `keys.json` 只存"所用的套 + 修改"，内置套更新时用户的修改保留。
- 绑定的对象：动作、启动一个应用（`.desktop` 名）、运行一条命令。
- 控制 socket：`$XDG_RUNTIME_DIR/xdesk-<WAYLAND_DISPLAY>.sock`（目录 0700，只有本会话的用户能连），NDJSON；`xdesk msg <命令>` 是它的客户端。
- `xdesk msg keys list|bind|unbind|set`、`xdesk msg settings <页>`（JSON），脚本与 agent 可用。
- 以后再做：前导键后再按一键的连续按键、模式（如调整大小模式）、触控板手势、指定应用不拦截、鼠标绑定的编辑。

### 12.4 设置里的"快捷键"页（也是说明）

- Super+/ 打开它，取代只读的速查表：按 §12.2 的分组列出全部，每项中英说明，可搜索；点一项再按新组合即修改；
  冲突当场提示（替换或交换）；单项或全部恢复；切换、复制、导入、导出套。
- 键盘没锁定时页顶写"未锁定：Super 等键由宿主处理"并给"锁定"按钮。
- 按住 Super 约 1 秒浮出 8 个最常用的键，松开消失。
- 第一次进入桌面显示一张可关闭的卡片：5 个最常用的键与"Super+/ 查看和修改全部快捷键"。
- 用户文档里的快捷键表从动作表**生成**到 `docs/generated/`（与命令参考一样），文档与行为一致。

### 12.5 启动器里的设置

启动器加入内置条目"设置"及其每一页（显示器、布局、外观、快捷键、关于），带中英关键词（显示器：分辨率、镜像、monitor；快捷键：按键、shortcut；关于：版本、about），
直接打开对应页；另写 `xdesk-settings.desktop`（经 `xdesk msg settings <页>`），其他启动器也找得到。

启动器同时能搜**动作**（"最大化""画布""工作区 3""锁定大小"……）：结果旁写它的快捷键，回车即执行。搜一次就学会一个键；
不记得键的人也能用全部功能。动作、快捷键页、按住提示、文档用同一张动作表。

### 12.6 实现（C9、C9b）

- **设置是一张覆盖层**，不是布局里的窗口：与 Super+m、Alt+Tab 同一套（xdesk.shell 画、合成器路由键与点击），模态、Esc 关闭。
  理由：窗口形态要么是内置视图（`View` 处处假定有 `wlr_xdg_toplevel`），要么是另一个 Wayland 客户端进程（要补键盘映射、输入法、
  缩放）；覆盖层零新依赖、与其他浮层行为一致。以后需要"设置开着同时用桌面"时再做成客户端（`xdesk msg` 已是它要的接口）。
- 页：显示器（每个窗口一行与说明）、布局（本工作区排布、列方向、内外间隔、最窄列、画布小地图与吸附）、外观（强调色 6 种、背景 4 种、
  减少动态）、快捷键、关于（`/etc/os-release`、运行方式、xdesk 版本取自安装包的 `README.provenance`、内核、用户、渲染器、显示器、
  CPU、内存、文件位置、开源项目；"复制"经剪贴板授权发到宿主）。修改立即生效：间隔与列宽走 `Layout::set_spacing`，方向
  `set_flow`，排布 `set_mode` 并记进 state.json；其余写个人设置文件（`config::set_value` 只改那一行，注释保留）。
- **键位**（§12.2 落地）：Super+F 最大化（三种排布一致）、Super+Shift+F 全屏（布局新增 `fullscreen`：整块输出、无标题栏、面板隐藏；
  客户端请求全屏也照办）、Super+Shift+Space 浮动、Super+P / Super+Shift+P 置顶 / 所有工作区、Alt+F4、Super+1…9（面板固定显示
  1–4，5–9 有窗口或当前时出现）、Super+Ctrl+← →（及 PageUp/PageDown，加 Shift 带窗口）、Super+I 设置、Super+/ 快捷键页、
  Super+O 在平铺与层叠里是常驻的空间总览（选中焦点窗口，直到选一个或 Esc）。去掉 Super+D。
- **套**：`luban`、`luban-no-hjkl`；`keys.json` 存 `"set"` 与改过的动作（旧文件照读）。
- **快捷键页**：搜索框（输入即过滤，英中说明、动作名、键都可搜）、套的选择、"全部恢复"、按分组的每个动作；回车进入录入，按下的组合
  （单独的修饰键不算）即生效，原属别的动作的从那里移走并提示；Delete 恢复该项。
- **按住 Super 一秒**浮出最常用的 9 个键；首次进入底部一行说最先要会的五个。
- **启动器**：设置的每页与每个动作都是条目（只在输入后出现），旁边写它的键；`xdesk-settings.desktop` 给其他启动器。
- **控制 socket**：`$XDG_RUNTIME_DIR/xdesk-<WAYLAND_DISPLAY>.sock`，一行一个 JSON；`xdesk msg keys list|bind|unbind|set`、
  `xdesk msg settings <页>`。
- **生成的键位表**：`xdesk --keys-markdown en|zh` → `docs/generated/xdesk-keys{,.zh}.md`，e2e 测试比对（`XDESK_KEYS_WRITE=1` 重写）。

## 13. 性能：渲染、传输与内存

### 13.1 定因（Super+O 连按时风扇起来）

| 编号 | 现象 | 定因 | 依据 |
|---|---|---|---|
| P1 | 总览、画布缩放时 CPU 满载 | xdesk 只用 pixman（CPU）渲染；缩放时每个窗口每帧都按比例重采样（`zoom_surfaces` 设 dest size），全是软件缩放 | `luban/xdesk/compositor/src/compositor/server.cpp:43`、`windows.cpp:89-101` |
| P2 | 动画期间每帧都是整屏 | 缩放与平移让整屏都是 damage；超过 8 个矩形也按整屏发 | `output.cpp:107-112` |
| P3 | 每帧整屏被复制 3–4 次 | 每帧新分配一个整屏大小的 string、逐行复制、再逐像素改 alpha；经 socket（内核复制）；宿主解析后再 `SDL_UpdateTexture` 上传。2560×1440 一帧约 14 MB，60 帧每秒约 0.9 GB/s 的复制 | `output.cpp:114-125`、`luban/os/window/src/host.cpp:471-478` |
| P4 | 连按会叠加 | 每次切换都启动 16 ms 一帧的弹簧动画，直到停稳 | `windows.cpp:104-108` |

### 13.2 做法

1. **缩略图代替实时缩放**：总览、切换器、画布缩到 50% 以下时，用每个窗口的缩略图（长边最多 512 px，窗口提交新内容时更新、最多每秒 10 次）合成；
   停稳后再按需要显示真实内容。动画期间用最近邻，停稳后线性过滤。
2. **少传**：XRGB 直接按 `SDL_PIXELFORMAT_XRGB8888` 上传，去掉逐像素改 alpha；帧缓冲复用，不每帧分配。
3. **同机零拷贝一半**：xdesk 与宿主在同一台机器上（sandbox）时，帧写进共享内存（memfd 经 socket 传一次），socket 只发"哪块变了"；
   宿主从共享内存直接更新纹理。machine 模式（虚拟机）仍走 socket，加简单压缩（全帧时按行差分）。
4. **节流**：宿主窗口被遮住或最小化时告诉 xdesk，不再出帧；动画按宿主的刷新率出帧，宿主没显示完上一帧不发下一帧（已有的 paced 机制覆盖到动画）。
5. **有 GPU 时 GPU 优先**：系统里能用 render node 时（sandbox 经宿主的图形授权拿到 `/dev/dri`；machine 有 virtio-gpu 的 3D 时），
   用 `wlr_renderer_autocreate` 得到 GLES 渲染器（合成、缩放、圆角、阴影都在 GPU 上），读回经共享内存交给宿主；以后再考虑 dmabuf 直接交给宿主。
   没有 GPU（machine 无 3D、CI）时退到 pixman，加上 1–4 也要达标。选了哪个、为什么，写进设置的"关于"页与 `XDESK_TRACE=timing`；
   GPU 初始化或运行中失败时退回 pixman 并说明，不黑屏。
6. **内存**：缩略图总量有上限（按窗口数 × 512 px）；切换器与总览关闭后释放；宿主纹理按窗口大小复用。

### 13.3 预算与测试

| 项目 | 预算（CI 的软件渲染 lane） |
|---|---|
| Super+O 连按 10 次，xdesk 加宿主的 CPU 时间 | ≤ 2 秒（改前先测出基线写进记录） |
| 静止的桌面 | 0 帧、CPU 接近 0 |
| 5 个窗口时 xdesk 常驻内存 | 基线 + 不超过 64 MB 的缩略图 |
| 总览、快捷键页第一帧 | ≤ 100 ms |

用上一轮的延时测试框架（`cpu` 资源，单独运行）；`XDESK_TRACE=timing` 写出每帧的合成、复制、发送时长。

## 14. 美术（G2、G3）

### 14.1 工匠抡锤

依据：转轴在肩、肘配合弯曲，锤柄大致与小臂一线、只在最高点手腕后扣；预备、下挥、击打、回弹四个阶段，时长不均。

| 帧 | 姿势 | 时长 |
|---|---|---|
| 预备 | 手臂高举过头，锤头在头后上方 | 320 ms |
| 下挥 | 手臂约 45° 前下，锤与小臂一线；可加 1–2 个拖影像素 | 70 ms |
| 击打 | 手臂前伸向下，锤头落在右下角的小砧上；1–2 个火花像素；身体下沉 1 px | 200 ms |
| 回弹 | 锤头弹起 1–2 px | 120 ms |

- `imkit::beat(4, 0.25)` 改为每帧各自的时长；减少动态时显示静止帧。
- 评审决定：美术由实现者定稿（符合视觉、不突兀即可），联系表随 PR 附上。
- 静态图标（托盘、`luban-os.svg`）用新的预备帧；`tools/luban_os_icon.py --check` 随之更新。
- 联系表（每帧放大并排 + GIF）随 PR 附上。

### 14.2 指针

- 尾巴约 2 行偏 1 列、沿箭头中轴伸出；12×12 放不下，或缩短箭身，或源图改为 12×16（热点不变）。画 2–3 个候选，选不突兀的一个。
- 同一主题的 `pointer`、`grab` 等形状一并检查；`luban:cursors` 出新版本（index PR 与资源构建）。

### 14.3 标题栏的圆角

- 窗口标题栏的左上、右上两个角为圆角（半径 8 逻辑像素，按密度缩放，抗锯齿）；下面两个角（内容区）是直角。
- 窗口占满屏幕（最大化、全屏、平铺里只有它一个且外间隔为 0）时四个角都是直角。
- 角外透明，露出背景；阴影与边框跟随圆角。

### 14.4 实现（C10）

- 工匠：四帧（预备：锤在头后上方；下挥：手臂前伸、锤向前上方、两点拖影；击打：锤落在右下角的砧上、三点火花、身体下沉 1 px；
  回弹：锤头抬起 2 px），时长 320 / 70 / 200 / 120 ms（`imkit::beats`，每帧自己的时长；减少动态时静止）；人物整体下移 2 行，
  给举起的锤留出空间；砧在每一帧都在。图标与托盘用预备帧（`tools/luban_os_icon.py` 重新生成）。
- 指针：源格式允许 12 宽、最多 16 高；`default`、`progress`、`help` 的箭头改为 12×14，尾巴每两行偏一列、沿中轴伸出，热点不变；
  `luban:cursors` 2026.10.20。
- 标题栏：上两个角半径 8 逻辑像素（按密度，抗锯齿：角上每个像素按圆的覆盖率保留），最大化、全屏、平铺里占满整宽（外间隔 0）时为直角。

## 15. 平台

- xdesk 只在 Linux（系统里）；宿主窗口的新能力（锁定反馈、宽高比、设为分辨率、多显示器）三平台都由 SDL 提供，限制写进 §5.3 与用户文档。
- sudo 与用户视图只在 Linux 系统里；Windows（WSL2 carrier）与 macOS（vz carrier）里的系统同样是 Linux，一致。

## 16. 测试与需求

新增需求 ID（实现时与 `tests/requirements.toml` 的命名对齐）：用户视图与提权、sudo、失败分类、补全、进度行宽度、键盘锁定、
平铺尺寸与宽度规则、拖放预览、层叠调整大小、画布交互、两个切换器、多显示器（镜像、扩展、跨屏拖动）、显示设置、美术。

| 测试 | 断言 |
|---|---|
| 系统内普通用户 `xlings install` | 只提权一次；选择进视图；别的用户不受影响；agent 模式给确切命令；已有的包不装 |
| `xlings use` / `list` / `info` | 合并两层并标来源 |
| root 回收 | 不删用户正在选择的版本，并说出是谁 |
| 本地错误 | 一条错误、不重试、不装 git |
| 进度行 | 40 列伪终端只有一行 `▸` |
| 键盘锁定（Xvfb + WM） | 锁定后 Alt+Tab 到 xdesk；失败重试；标题与提示 |
| 平铺（按字符格取整的客户端，或 foot） | 标题宽 = 内容宽；间隔 = inner_gap；3 列平分；合计不足时占满 |
| 拖放 | 预览 = 落下后的布局；判定区迟滞 |
| 层叠 | 边角与 Super+右键调整大小；`request_resize` |
| 画布 | 缩放 0.9 下点击后仍 0.9 且应用收到点击；Super+拖动移动窗口；新窗口在视野内且不重叠；吸附与成组 |
| 切换器 | 3 个客户端（两个同 `app_id`）：分组、展开、选中；空间总览选中与快按 |
| 多显示器（两个假宿主） | 默认镜像、宽高比锁定、扩展后跨屏拖动、主屏关闭、配置下发与锁定 |
| 设置 | 脚本命令打开显示页、改模式与排列、15 秒还原；布局页与外观页的改动与标题栏菜单、快捷键改的是同一份并立即生效；关于页的版本、渲染器、配置位置与"复制系统信息" |
| 渲染器 | 有 render node 时选 GLES；强制 GLES 初始化失败时退回 pixman、关于页写出原因、不黑屏 |
| 快捷键 | 默认键位逐项生效；页内修改、冲突的替换与交换、恢复；复制并切换套；绑定应用与命令；`xdesk msg keys` 的 JSON；按住 Super 出提示、单按开启动器 |
| 启动器 | 搜"分辨率""shortcut"直接打开对应设置页 |
| 文档 | 生成的快捷键表与动作表一致（与命令参考同样的检查） |
| 延时 | Alt+Tab 总览、快捷键页第一帧 ≤ 100 ms |

截图基线：平铺、画布、两个切换器、设置页（按发布的字体录制）。

## 17. 文档

每一项用户看得到的功能都有配套文档（中英两份，`docs/` 与 `docs/zh/`），在同一个检查点里写：

| 文档 | 内容 |
|---|---|
| `docs/luban/08-desktop.md` | 三种排布与统一的交互规则、拖放、调整大小、画布、两个切换器、总览、圆角 |
| `docs/luban/09-desktop-keys.md`（新） | Luban 键位的原则、按住提示、快捷键页、多套、别名、绑定应用与命令、`xdesk msg keys`、各宿主不能锁定的键 |
| `docs/luban/10-desktop-settings.md`（新） | 设置窗口：显示器页（镜像与扩展、主屏、方向、缩放、排列、锁定大小、设为分辨率、15 秒还原）、布局页、外观页、快捷键页、关于页、从启动器打开 |
| `docs/generated/xdesk-keys.md`（生成） | 键位总表，由动作表生成，CI 检查一致 |
| `docs/luban/03-environments.md` | 系统里的用户：视图、`install` 与提权、sudo 与默认密码、`--no-password` |
| `docs/luban/01-quick-start.md` | 第一次进入桌面要知道的 5 个键 |
| `luban/xdesk/README.md`、`luban/specs/opensal/SPEC.md` | 多显示器、显示器配置消息、控制 socket |
| `docs/specs/interface-ndjson-v1.md` | 1.15：`E_NEEDS_ROOT`、`origin` |

## 18. 实施拆分与依赖

一个 PR，按检查点逐个 commit；每个检查点编译通过、相关测试与文档随之更新；每个阶段结束跑一次完整 CI。

| # | 检查点 | 依赖 | 仓库 |
|---|---|---|---|
| C1 | 进度行按终端宽度（§3）；托盘的菜单、退出与 CPU（§3.1） | — | xlings |
| C2 | 键盘锁定以回答为准、重试、反馈（§5） | — | xlings |
| C3 | 平铺：xdg tiled、间隔、宽度规则、纵向（§7.1–7.4） | — | xlings |
| C4 | 交互共同规则、拖放预览、宽度选择（§6、§7.5–7.6） | C3 | xlings |
| C5 | 层叠调整大小、列内缝隙（§8） | C4 | xlings |
| C5b | 画布交互（§9） | C4 | xlings |
| C6 | 系统里的 xlings：存储与视图、提权、sudo、失败分类、补全、接口 1.15（§4） | — | xlings；xim-pkgindex（`xim:sudo`、core 与 desktop 新版本） |
| C7a | 性能：缩略图、帧缓冲复用、去掉逐像素 alpha、同机共享内存、遮挡时停帧、预算与基线（§13） | C5b | xlings |
| C7 | 总览组件、两个切换器（§10），用 C7a 的缩略图 | C4、C5b、C7a | xlings |
| C8 | opensal 多显示器、xdesk 多输出、镜像与扩展、跨屏（§11.1–11.3、§11.5–11.6） | C7 | xlings |
| C9 | 设置窗口：显示器页（配置下发、wlr-output-management）、布局页、外观页、关于页（§11.4） | C8 | xlings |
| C9b | 快捷键：Luban 键位、快捷键服务（多套、绑定应用与命令、控制 socket）、快捷键页、按住提示、启动器里的设置、文档生成（§12） | C9 | xlings |
| C10 | 美术：工匠 4 帧、指针、标题栏圆角（§14） | C3（圆角的直角规则） | xlings；xim-pkgindex（`luban:cursors`） |
| C11 | 文档：§17 的各页（中英）、规范、生成的参考（含键位表）、fresh 套件的文字；各检查点已写的部分在此对齐 | 以上 | xlings |
| C12 | 发布：版本号、镜像、index 的 latest | C11 与 CI | 全部 |

阶段与 CI：C1–C2（一）、C3–C5b（二）、C6（三，跨仓库联调）、C7a–C7（四）、C8–C9b（五）、C10–C11（六）、C12（发布）。

跨仓库顺序：`xim:sudo` 与 core 新版本先在 index 的分支上构建与测试，xlings CI 用该分支的 index；发布后合入 index，再把 latest 提上去。

## 19. 自审（修订 3，多角度）

| 角度 | 发现 | 处理 |
|---|---|---|
| 架构 | 用户视图与宿主的 system layer（deployment M）是两种叠法 | 宿主上保留 M（用户有完整 home）；系统里只用视图。§4.11 写明边界，代码不共用到互相牵制 |
| 架构 | 总览原本有三处（Alt+Tab、画布 Super+o、排布浮层） | 合为一个组件（§10.3） |
| 安全 | 免密 sudo 让以该用户身份运行的任何程序都能成为 root | 评审改为与常见桌面一致：sudo 要密码，不设时默认 `lubanoser` 并提示；免密只在明确的 `--no-password`；agent-workspace 的策略不给 agent 用 sudo（§4.5） |
| 安全 | 默认密码人人知道（树莓派 2022 年因此取消了 pi/raspberry） | 只在本机：系统由宿主拥有、ssh 不允许密码登录；`luban status` 持续提示直到修改 |
| 安全 | 最初设想的共享登记目录（`/xlings/var/views`）是一个用户可写的系统位置 | 改为 root 回收时读各用户视图（§4.7） |
| 稳定性 | setuid 可能被 nosuid 挂载吃掉 | C6 第一步验证，退路写明（§4.5） |
| 稳定性 | 键盘锁定每帧重试可能刷 X 请求 | 失败后按焦点事件与最多每 500 ms 一次重试 |
| 简洁 | 分辨率原有"策略 × 显示方式"两组选项 | 收成"窗口大小 + 锁定"（§11.2） |
| 简洁 | 宽度条、判定区三分之一切 | 去掉宽度条；判定区简化并加迟滞（§7.5–7.6） |
| 体验 | 画布误触一格滚轮就进入"不可操作" | 视角不是模式（§9） |
| 体验 | 用户第一次装包要先拉 4 个 index | 视图没有 index（§4.2） |
| 兼容 | 旧的 `gap` 配置、旧宿主（一块显示器）、旧的用户层 | `gap` 读作 `inner_gap`；opensal 能力位与 compat 登记；旧用户层当视图读、不写 |
| 跨平台 | 锁定在 macOS、Wayland 上有不可锁的键 | §5.3 写明，用户文档同步 |
| 一致性 | 三种排布的手势不同 | §6 统一 |
| 无感升级 | 已有系统得到 sudo 与新的交互 | `luban upgrade` 移到 core 新版本即得到；可 `rollback`；xdesk 新版本随 desktop 的新 edition |
| 性能 | Alt+Tab 实时预览全部窗口；镜像发多份帧 | 预览引用 buffer、只缩放；镜像只渲染一次、同机共用共享内存 |
| 测试 | 现有假客户端接受任意尺寸，测不出 G4；offscreen 驱动的锁定总成功，测不出 G8 | 字符格取整的客户端；Xvfb + WM |
| 范围 | C1–C12 在一个 PR 里很大 | 检查点 commit、阶段 CI；美术由实现者定稿，不阻塞 |
| 文档 | fresh 套件断言已发布的文字（记忆） | 改输出时同步 `tests/fresh-install`（C11） |
| 版本 | 接口 1.15、发布号可能被并行的 PR 占用 | 发布前核对 `gh release list` 与 main 的协议号（记忆） |
| 一致性 | 快捷键原本有一半像 niri、一半像 GNOME，Super+F 在平铺是浮动、在层叠是最大化 | §12.1 的四条规则定键；Super+F 三种排布都是最大化 |
| 兼容 | 2026.10.19.2 已发布的键（Super+Tab、Super+F、Super+Shift+T、Super+D）改了意思 | §12.2 的对照列写明；发布说明与快捷键页首次进入的卡片说一次；用户文档的键表由动作表生成 |
| 体验 | 快捷键要背 | 单按 Super 搜索一切、按住出提示、Super+/ 即说明、首次卡片；方向与 Shift 的规则让大部分键可以推出来 |
| 体验 | 宿主拿走 Super 时快捷键"没反应" | 快捷键页与按住提示都写出"未锁定"并给锁定按钮（与 §5 的反馈一致） |
| 安全 | 控制 socket 能改快捷键、运行命令 | 在 `XDG_RUNTIME_DIR`（0700），只有本会话用户；绑定命令与在终端里运行同权 |
| 稳定性 | 显示器配置按窗口身份保存，窗口 pid 每次不同 | 托盘分配稳定的窗口编号（§11.4） |
| 性能 | 新的浮层与设置页 | 延时预算加两项：Alt+Tab 总览与快捷键页第一帧 ≤ 100 ms（沿用上一轮的延时测试框架）；减少动态时不做动画 |
| i18n | 新的文字（设置、快捷键说明、提示） | xdesk 的 catalogue 中英两份；`tools/lint_i18n.sh` 覆盖 |

### 19.1 修订 3 的复审（按维度）

| 维度 | 检查 | 发现与处理 |
|---|---|---|
| 多平台一致性 | 同一套键在三种宿主上是否都能按到 | macOS 宿主上 Cmd 即 Super，系统保留 Cmd+Tab、Cmd+Space（单按 Cmd 不保留，启动器照常）；Windows 宿主保留 Win+L。处理：Alt+Tab 三平台都能按到，作为切换的保底；每个被宿主保留的组合给一个别名（实现时在三平台实测选定，写进键位表），快捷键页与按住提示按宿主标出"这台宿主按不到"及其别名 |
| 多平台一致性 | 显示器、锁定大小、宽高比 | 都经 SDL，三平台同一行为；Wayland 宿主经 XWayland 时锁定受宿主设置限制（§5.3） |
| 交互 | 单按 Super 在没锁定时归宿主 | 窗口提示条与快捷键页写出"未锁定"；Luban OS 进入桌面默认锁定（设置可改） |
| 交互 | 拖动、缩放、切换的手势在三种排布是否同义 | §6 与 §12.1 第 6 条；测试逐排布验证同一手势 |
| 用户体验 | 第一次进入桌面的人知道什么 | 首次卡片 5 个键、单按 Super 搜一切（含动作）、按住提示、Super+/ |
| 易用性 | 不记键的人能否用全部功能 | 启动器能搜动作并执行（§12.5）；标题栏菜单覆盖窗口操作；设置窗口覆盖显示与键位 |
| 可玩性 | 能不能按自己的方式用 | 多套键位、别名、绑定应用与命令、`xdesk msg` 脚本；画布的组、吸附、小地图；以后：连续按键、模式、手势 |
| 稳定性 | 配置写坏、显示器断开、宿主拒绝锁定 | `keys.json` 读不了时说出文件与原因并用内置套（"读不到不是空"）；显示设置 15 秒还原；显示器窗口关闭时窗口回到主屏；锁定失败有状态与重试 |
| 稳定性 | 用户视图与存储并发 | 存储写只在 root 的子命令里，沿用现有的存储锁；视图只写用户自己的文件 |
| 性能 | 新功能会不会再让风扇起来 | §13：缩略图、帧复用、共享内存、遮挡停帧，CI 预算；Alt+Tab 与总览用同一缩略图缓存 |
| 易用性 | 设置散在菜单、快捷键与配置文件里 | 偏向设置的都进设置窗口，菜单与快捷键是快速入口，改的是同一份配置（§11.4） |
| 性能 | GPU 与 CPU 两条渲染路径 | GPU 优先；同一套缩略图与传输改进两条路径都用；GPU 失败自动退回并说明；CI 在 pixman 上守住预算 |
| 优雅 | 机制有没有重复 | 一张动作表驱动快捷键、快捷键页、按住提示、启动器的动作搜索与生成的文档；一个总览组件；分辨率一条规则；一个控制 socket |
| 视觉 | 新浮层与窗口是否协调 | 标题栏上两角圆角、占满时直角；总览选中放大 1.2 倍带阴影、其余变暗；动画 120–200 ms 缓出，减少动态时直接显示；镜像窗口的留边用桌面背景色而不是纯黑；提示、设置、切换器沿用 xdesk 的字体与配色，像素风的工匠与指针只在 Luban OS 窗口与光标上 |
| 兼容 | 改了意思的已发布键 | §12.2 的对照列、发布说明、首次卡片；旧 `keys.json` 的自定义按动作名保留 |
| 测试 | 新机制是否都有可失败的测试 | §16 逐项；性能有基线与预算；文档生成有一致性检查 |

## 20. 桌面 edition 的第二轮实测（G22–G33）

用户在 2026.10.19.2 的 LXQt、KDE 与 xdesk 系统里的实测：

| 编号 | 现象 | 定因与做法 |
|---|---|---|
| G22 | 建桌面系统时每个阶段的进度都从 0 到 100% | 进度按阶段各自计算。改为**整个操作一个 0–100%**：各阶段按预估权重（准备 2、下载 55、解压 18、配置 15、启动与初始化 7、自检 3）占一段，阶段内的进度映射到它那一段；阶段名照旧写在旁边。命令行与窗口同一份计算（服务给出 `overall`） |
| G23 | 配置完成后"自检"显示 0% 与"准备"，等约 1 分钟才进桌面，像是退回了开头 | 自检要在系统里跑，machine 先要启动；启动的进度与文字沿用了"准备"。加一个阶段**"启动与初始化"**（在自检之前）：显示启动的进展（内核、网络（NAT）、服务、会话），文字来自 machine 的启动日志；整体进度继续往前走，不回退 |
| G24 | LXQt 桌面右键"打开终端"：`failed to execute child process "qterminal"`（应用菜单里打开没问题） | pcmanfm-qt 的终端设置默认是 qterminal，系统里没有。luban:lxqt 带上 qterminal（LXQt 自己的终端，有右键菜单），并把 pcmanfm-qt 与 LXQt 的终端设置都指向它 |
| G25 | LXQt 桌面上的"计算机"打开显示不支持 | `computer:///` 要 gvfs。默认桌面图标去掉"计算机"，留"主目录""回收站" |
| G26 | 面板右下角"Drop application icons here"两行显示不完整 | 是 quicklaunch 插件的空白提示。默认 quicklaunch 放上常用应用（终端、文件管理器、浏览器），不再是空的 |
| G27 | foot 与 qterminal 里复制粘贴不行（konsole 可以） | 终端的复制是 Ctrl+Shift+C/V（Ctrl+C 是中断）；foot 没有右键菜单。LXQt 用 qterminal（有右键菜单）；foot 的默认配置写明 Ctrl+Shift+C/V 并在快捷键文档里说明；同时核对 opensal-wlr 在桌面内与宿主之间的剪贴板（wlr-data-control），补测试 |
| G28 | LXQt 很卡；KDE 稍好但也不流畅 | 没有 GPU：labwc 与其中的应用都是软件渲染，经 opensal-wlr 截屏传给窗口。做法：§13 的窗口端优化（整块复制、遮挡停帧）对所有桌面生效；opensal-wlr 只传变化的区域（screencopy 的 damage）；machine 默认 vCPU 与内存按宿主调高；有 GPU 时的路径同 §13.2 第 5 条 |
| G29 | KDE 里不显示鼠标指针（能点击）；新建文件夹后一直显示文字光标"\|" | 嵌套的 KWin 设置的指针形状没有传到窗口：opensal-wlr 的指针会话只在进入时取一次形状。做法：opensal-wlr 跟随 labwc 的指针形状变化（光标表面的提交与 `cursor-shape` 请求）每次都转交；没有指针会话时把指针画进画面；补一个嵌套合成器改变指针形状的测试 |
| G30 | KDE 的 camera indicator 显示 `main.qml: pipewire.monitor is not installed` | 隐私指示器小部件需要 kpipewire 的 QML 模块。没有摄像头时默认面板不放它；音频方案（§20.1）带上 PipeWire 后，luban:kde 再带 kpipewire |
| G31 | 有图形界面的系统应当默认带浏览器 | 见 §20.2：所有带桌面的 edition 默认 Falkon |
| G32 | 桌面系统应当有声音 | 见 §20.1：openhal 的 audio |
| G33 | LXQt、KDE 进入后是 root | 只有 desktop edition 被认为"需要用户"；core 加 `--desktop lxqt` 也是桌面。做法：带任何桌面（edition 的、`--desktop`、`--xpkg` 里的桌面包）的系统都建默认用户 `lubanoser`，密码同 §4.5（不设则默认 `lubanoser`） |

### 20.1 声音：openhal 的 audio

- **分层**：openhal 加一个 `Audio` 能力（spec 只定数据：PCM 流的格式、开关、音量、延迟），宿主一侧的实现与平台相关，系统一侧的服务与平台无关。
- **宿主**：Luban OS 的托盘进程用 SDL3 的音频（Linux：PipeWire / PulseAudio / ALSA；macOS：Core Audio；Windows：WASAPI，均由 SDL 选择），
  播放与录音都经它；系统不再直接连宿主的 PipeWire（现在的做法只在 Linux 宿主上可用）。
- **系统**：带桌面的 edition 带 PipeWire（加 wireplumber 与 pipewire-pulse），默认输出是一个 `luban` sink：PCM 写进一个 unix socket，
  luban 的 agent 经 machine 的通道（sandbox 经管道）转给宿主；麦克风反向。系统里的应用只看到普通的 PipeWire / Pulse。
- **授权**：沿用现有的 `audio` 授权（`luban config <系统> --allow audio`）；录音单独授权（`microphone`）。
- **测试**：系统里播放一段已知波形，宿主的假音频设备收到同样的样本；没有授权时不传。

**实现（C18，第一步：机器的声音输出）**：

- openhal 0.2：`openhal.audio`（Open、Samples、Volume、Close、Ended；kind + 长度 + payload），SPEC §5。
- 系统一侧：`luban:audio`（Alpine 3.24 的 PipeWire 1.6.8、WirePlumber 0.5.15、pipewire-pulse，按 Alpine 的 /usr 布局放
  `lib/pipewire-0.3`、`lib/spa-0.2`、`lib/wireplumber-0.5`、`share/pipewire`；私有库与 musl loader 在 libexec/audio）；
  `luban` sink 是 `module-pipe-tunnel`，写进 `/tmp/luban-audio.pcm`（48 kHz 双声道 S16LE），优先级最高即默认输出；
  `bin/luban-audio` 为一个会话启动三者。构建时在 Debian 里播放一段噪声，确认默认输出是 luban、管道里收到数据。
- 启动：`opensal-wlr` 的 `run_session`（所有桌面——xdesk、labwc、LXQt、KDE——都由它启动）在 PATH 里有 `luban-audio`
  时随会话启动它，会话结束时连同它的进程组一起结束；会话已有宿主的声音服务（沙箱的 `audio` 授权给了 `PULSE_SERVER`）时不启动。
  一个系统的 /etc 不投影包的 etc，所以不用 XDG autostart。
- 转发：`luban __audio [FIFO]` 读管道（读写方式打开，声音服务重启时不断），按整帧发 openhal 流到标准输出；machine 里由 agent
  在声音通道（3）上以登录用户运行（agent 把 `luban` 换成它自己的程序），窗口（`Display`）收到后经 `host::Speaker`
  （SDL3 音频：Linux 的 PipeWire/PulseAudio/ALSA，macOS 的 Core Audio，Windows 的 WASAPI）播放；落后超过半秒丢弃旧的。
- 授权：带桌面建出的系统授予 `audio`；窗口只在有这个授权（或系统没有策略文件）时打开声音通道。
- 沙箱：保持现有的直连（`audio` 授权传 `PULSE_SERVER`），延迟最低。
- 测试：openhal 流的往返；`luban __audio` 把管道里的已知波形原样发出；窗口的扬声器经 SDL 的 disk 驱动写出同样的样本。
- 以后：麦克风（`microphone` 授权、input 方向）、KDE 带 kpipewire 后放回相机指示器、托盘进程在没有窗口时继续播放。

### 20.2 默认浏览器

| 候选 | 引擎与依赖 | 评价 |
|---|---|---|
| Falkon | QtWebEngine（Chromium），Qt | KDE 的项目，LXQt 推荐；与 Qt 桌面共用 qt-runtime，现代网页兼容好 |
| Epiphany | WebKitGTK，GTK4 + libadwaita | GNOME 的；依赖一整套 GTK |
| NetSurf | 自己的引擎 | 最轻，但几乎不支持现代 JavaScript，日常网页打不开 |
| Luakit、surf | WebKitGTK，GTK3 | 键盘为主的极简浏览器，不适合作默认 |
| Firefox | Gecko | 各发行版最常见的默认，但最重 |

结论：所有带桌面的 edition 默认 **Falkon**（LXQt、KDE 与之同为 Qt；xdesk、labwc 的系统随 Falkon 带上 qt-runtime）——
轻于 Firefox、现代网页可用、符合 Qt 桌面社区的习惯；一个浏览器在所有桌面上一致。Firefox 留作 `xlings install firefox` 的选择。
没有 GPU 时 Falkon 用软件合成（QtWebEngine 的 `--disable-gpu`），由它的启动器设置。

### 20.3 实施

| # | 检查点 | 仓库 |
|---|---|---|
| C13 | 进度：整体 0–100%、"启动与初始化"阶段（G22、G23） | xlings |
| C14 | 默认用户：带桌面即建 `lubanoser`（G33） | xlings |
| C15 | LXQt 与 KDE 的配置：qterminal、桌面图标、quicklaunch、相机指示器、foot 的复制说明（G24–G27、G30） | xim-pkgindex（luban:lxqt、luban:kde、luban:foot 新版本） |
| C16 | opensal-wlr：指针形状跟随、只传变化区域、剪贴板（G28、G29） | xlings |
| C17 | 浏览器：luban:falkon，各桌面 edition 带上（G31） | xim-pkgindex |
| C18 | 声音：openhal audio、宿主 SDL 音频、系统的 PipeWire 与 `luban` sink（G32） | xlings；xim-pkgindex |

## 22. 2026.10.20.1 的实施状态

| 检查点 | 状态 |
|---|---|
| C1 进度行、C2 键盘锁定、托盘（G20、G21） | 完成 |
| C3–C5b 平铺、拖放、调整大小、画布、性能（C7a） | 完成 |
| C6 系统里的 xlings（视图、sudo、补全） | 完成 |
| C7 两个切换器与总览组件 | 完成（§10.4 实现说明） |
| C8 多显示器 | 镜像完成（§11.7）；**扩展模式（C8b）留到下一版**：每块屏一个输出、各自的布局与工作区、跨屏、排列画布、wlr-output-management、桥接会话的主屏接管 |
| C9、C9b 设置与快捷键 | 完成（§12.6）；设置是覆盖层，不是窗口 |
| C10 美术 | 完成（§14.4） |
| C11 文档 | 08-desktop（中英）、生成的键位表、03-environments；设计记录 |
| C13–C17 第二轮实测 | 完成：进度、默认用户、LXQt r4、KDE r2、Falkon、桥接会话的剪贴板（wlr-data-control） |
| C18 声音 | 机器的声音输出完成（§20.1 实现）；麦克风、托盘在没有窗口时播放留到下一版 |
| 系统里读 index（§4.13 V12） | 2026.10.20.2 修复 |
| GPU 优先（§13.2 第 5 条） | **留到下一版**：要 index 里有 Mesa、mcpp 里的 wlroots 带 GLES2 渲染器；软件渲染的预算（§13.3）已达标 |

## 21. 安卓版

见 `2026-10-20-luban-os-android-preliminary.md`：`import std`、imgui 已在本地对 Android 验证；中间层三处（mcpp 的 API level、
compat.sdl3 的 Android 分支、platform 的 Android 分支）；有 root 用 chroot、无 root 用 proot；自己发布 APK。未来再做。
