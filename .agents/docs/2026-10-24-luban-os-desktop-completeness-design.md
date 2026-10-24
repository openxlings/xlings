# Luban OS 桌面的完备性：总线上的显示与设备、虚拟显示器、默认可观察、本地真实场景全覆盖、xdesk 的交互与生态、machine 的可用性

日期：2026-10-24　状态：修订 2——两轮评审结论已写入（§16.0），每轮按视角自审的改动已并入（§18）；待第三轮评审　缘起：2026.10.24.1 在 Linux 宿主（X11 加输入法、NVIDIA、KVM、AppArmor 限制非特权
userns）上的实测——从 2026.10.21.4 直接升级后，`luban:desktop 2026.10.23.1` 的 machine 起不来、Luban OS 应用在启动页 abort；
改用 sandbox 后桌面能用，但 Falkon 一开网页就崩、复制粘贴不通、stack 布局与折叠有缺陷、应用没有图标、桌面过一段时间以
"the desktop ended (124)" 结束；以及之后关于"为什么要人转述输出""桌面应能在没有显示器时运行""连接桌面应走总线"的讨论。

范围：
- **规范**：openhal 0.3（显示器、输入、剪贴板、声音作为设备，虚拟设备）、opensal 0.7（表面跑在 openbus 上：stream 加 region）、
  openbus 0.2（跨 VM 的共享区域、设备的接入与授权、日志流）、opencaps（内核能力名、显示能力名）、openwx 0.2（折叠窗口的尺寸语义）。
- **服务与库**：`luban/services/xbus`（路由的生命周期、系统内路由与宿主的链路）、`luban/kits/openhal-host`、新 kit `openhal-emu`
  （虚拟显示器、虚拟输入、记录的声音、剪贴板）、`luban/kits/telemetry`。
- **luban**：`luban/os/vm`（内核与能力、virtiofs、帧通道）、`luban/os/service`、`luban/os/cli`、`luban/host/luban-os`（显示设备的客户端、
  启动页）。
- **xdesk**：`luban/xdesk/*`（总线上的接入、应用的日志与退出、stack 的越出与贴边、折叠、书签、图标、壁纸、多显示器）、新应用
  `xdesk-settings`、新应用 `xdesk-wallpaper`。
- **xlings**：`src/core/clipboard.cpp`（桌面会话里的剪贴板、错误不进内容）、`modules/subos`（sandbox 桌面会话的 GPU 授权）。
- **生态**：xim-pkgindex（`luban:falkon` 的 NSS、打包时声明 dlopen 的库、`linux-kernel-virt` 的能力、桌面版本、壁纸与设置的包）；
  `luban-os/luban-os-ci`（L4、L5 改为用户路径的场景）；mcpp-index（SVG 光栅化库）。

交付：每个仓库一个 PR，每个检查点一个 commit（§15）；本文是 xlings PR 的第一个检查点。

替换的旧决定：
- 2026-10-21 生态可用性设计：
  - §5.6a"共享区域本轮不做 ivshmem"与 §5.5 的帧通道：改由 §5 定义（帧在 openbus region 上，machine 用 ivshmem）；
  - §8.5 / §8.9a"折叠的窗口不给客户端发尺寸"之外未定义"折叠时移动"：§9.3 补上；
  - §8.4 书签的外观（右边缘的小标签）：§9.4 改为压在下面的书签；
  - §9.3 L4 的做法（machine 里手工起无头 xdesk、Falkon 只开 `file://`）：改由 §11 的用户路径场景；
  - §15 里关于 vz 扫描输出、多显示器扩展的"以后"：本轮做（§4、§9.8）。
- 2026-10-22 openbus/xbus 设计 §7"不迁移的：opensal 的帧（K7 之后再迁到 region）、声音"：本轮迁移（§5）；broker 仍按其 §7 的计划。
- 现状"`luban enter --display` 只有 `terminal`、`window`"（从未写成决定）：加 `virtual`（§4）。

兼容：本轮不考虑旧版本——Luban OS 还在内部测试，没有外部用户。旧的路径（私有 socket、home 级的内核、被写坏的折叠状态）直接
去掉，不做过渡；内部已有的系统经 `luban upgrade`（或重建）进入新版本（§13）。

## 0. 核心点

1. **桌面不需要显示器**：显示器是 openhal 的设备，窗口只是其中一种。`luban enter <名字> --display virtual[=1920x1080@1.25]`
   用虚拟显示器运行桌面；之后可以随时把 Luban OS 窗口、另一台电脑的窗口、真实显示器接进来——作为主屏、镜像或扩展屏，接上和
   拔掉都是热插拔（§4）。
2. **连接桌面一律走总线**：桌面的状态、控制、输入、剪贴板、声音、画面、应用的日志与退出，全部是 openbus 上的路径
   （`host/<id>/system/<名字>/…`）。`luban`、`xbus`、Luban OS 应用、测试与 Agent 都是总线的客户端；xdesk 的控制 socket 只留作进程内
   的后备，不再是外部的入口（§3、§5）。
3. **默认可观察**：xdesk 启动的每个应用，标准输出与错误写进日志并作为 stream 挂在总线上，退出码与信号作为事件发出，崩溃在桌面上
   提示；任何人（包括 Agent）不需要用户转述就能拿到画面、日志与状态（§6）。
4. **本地真实场景全覆盖**：以"人怎么用"为单位写场景——全新 home、默认创建、进入桌面、打开每个应用、复制粘贴、布局与折叠、
   待机一小时、升级旧 home、machine 起不来……全部经虚拟显示器与总线驱动，断言状态、截图与日志。先在本地跑通全部场景，再进
   luban-os-ci；Linux 上的可用性由它保证（§11）。
5. **四个根因先修**：
   - Falkon 打包漏了 NSS 运行时加载的模块（`libsoftokn3` 等），https 一来就终止（J59）；
   - machine 选了 virtio-fs，却没看内核支不支持（J55）；
   - stack 布局把窗口夹回屏幕内，贴边吸附的代码从未被走到（J61、J62）；
   - 折叠的窗口一移动，展开后只剩标题栏（J63）。
6. **machine 的可用性**：内核属于系统（随升级与回滚），可以按系统指定；启动前按 opencaps 校验内核能力，缺什么就退回或拒绝并
   说明修法；帧走共享内存（ivshmem），去掉 virtio-serial 的带宽上限（§7、§5.4）。
7. **sandbox 桌面也用 GPU**：desktop 会话按系统的 `gpu` 授权拿到渲染节点（J58）。
8. **剪贴板按区域分开，只有最新的一次跨区域**：宿主、每个系统各是一个区域，各有自己的剪贴板与历史（`clipboard`、`primary` 按
   Linux 习惯分开）；剪贴板是总线上的服务（`luban.clipboard@1`）。所有参与的区域里**最新的一次复制**可以跨区域、跨 OS——经显示器
   窗口，另一台 macOS/Windows/Linux 电脑的剪贴板也是一个区域；更早的条目、历史与 `primary` 不出区域。宿主侧由 Luban OS 的进程用
   SDL 完成（T2），不需要宿主装工具。终端保持 foot：右键粘贴 `clipboard`、Ctrl+Shift+C/V、中键粘贴 `primary`。错误永远不会成为
   粘贴的内容（§8）。
9. **xdesk 的交互补齐**：stack 可越出、可贴边；折叠只在 WuXian，有标识、不丢尺寸；书签改为"压在下面的书签"；启动器、标题栏、
   书签都有图标（含 SVG）；设置是一个标准应用；壁纸分两层——xdesk 做通用的基础功能（纯色、图片、每个工作区、WuXian 的平面锚定与
   镜头变换、待机规则、背景层），桌面版自带的壁纸引擎应用做更可玩的经典物理、数学、图形学渲染；多显示器扩展（§9）。
10. **打包规则**：包声明运行时 dlopen 的库，打包工具按声明收集；包的检查增加 https 页面与图标，检查的是用户实际看到的结果（§10）。
11. **命令树的改动（已评审）**：`--display virtual`、`luban display`、`luban logs`、`luban config --kernel`（§12、§16.0）。

## 1. 实测与定因

编号接着 2026-10-21 设计的 J53。位置以 2026.10.24.1（`66c5cbf6`）为准。

| 编号 | 现象 | 定因 | 位置 |
|---|---|---|---|
| J54 | 启动 Luban OS 应用后倒计时自动进入 machine 系统，启动页 abort（imgui：`SetCursorPos` 后没有项目） | 启动页在按钮前 `SetCursorPos`；"完成"分支不提交任何项目，日志区放不下时就没有项目；`done` 在函数里读两次，工作线程不加锁写它，帧中间结束时两个按钮分支都不走 | `luban/host/luban-os/src/views/launcher.cpp:242-357`、`state.cpp:96-101` |
| J55 | 从 21.4 升级后 machine 起不来：`the environment's tree (tag root) did not mount` | 22.1 起 KVM 宿主默认 virtio-fs；home 里是最早的 `linux-kernel-virt 6.12.112`（没有 `FUSE_FS`/`VIRTIO_FS`，r1 起才有）；宿主只在没有内核时安装，从不校验能力；多个内核时取目录遍历的最后一个 | `luban/os/vm/src/host.cpp:72-79`、`368-376`、`475-511`；`guest.cpp:723` |
| J56 | 每次启动 virtiofsd 先失败一次（`CleanMount … Permission denied`），再以无沙箱模式启动，file handles 被关掉 | 宿主 `kernel.apparmor_restrict_unprivileged_userns=1`（Ubuntu 24.04 默认）；先试命名空间、失败再退，每次都走一遍且日志像错误 | `host.cpp:281-300`、`1844-1881` |
| J57 | machine 的桌面很卡，sandbox 不卡 | 帧仍在 virtio-serial（J11 未解决）：只做了 LZ4 与矩形合并，共享区域没做 | 2026-10-21 设计 §5.6a；`luban/os/vm/src/host.cpp:464-466` |
| J58 | sandbox 桌面用软件合成：xdesk 报 `no render node in /dev/dri`，而系统已授权 `gpu`、宿主有 `renderD128` | 已定因（K18）：系统在私有域里，域的 bwrap 只给最小的 `--dev /dev`，宿主的 `/dev/dri` 进不了域，域里的 sandbox 便无从授予。修：域里的会话（`subos use`、`exec`、`start`）带上宿主的 GPU 设备节点，授予与否仍由系统的 `gpu` 授权决定 | `src/core/subos/domain/domain_producer.cpp`（`command`）；`modules/subos/src/sandbox/gpu.cppm` |
| J59 | Falkon 能开，一打开网页就退出；`--disable-gpu` 也一样 | `libexec/falkon/lib` 只有 `libnss3`、`libnssutil3`；NSS 运行时 dlopen 的 `libsoftokn3`、`libfreebl3`/`libfreeblpriv3`、`libnssckbi` 没收进来（打包只跟 ldd 的依赖）。Chromium 初始化 NSS 两种方式都失败后终止；`file://` 用不到 NSS，所以 L4-03 是绿的 | xim-pkgindex `tools/res/build.sh` 的 falkon 一节；luban-os-ci `scenarios/L4/03-falkon-page.toml` |
| J60 | 桌面终端里 Ctrl+V：`the host has no clipboard to read …`，而且这行错误被当成粘贴内容插进了命令行（fish 历史里有它） | 桌面会话里的 `wl-paste` 是 sandbox 的转发程序，经 broker 去宿主找 `wl-paste`/`xsel`，宿主没有；本该用会话自己的剪贴板（xdesk ↔ Luban OS 应用）；错误进了标准输出 | `src/core/clipboard.cpp:183-195`；`modules/subos/src/sandbox/tools.cpp:61-68` |
| J61 | stack 里窗口不能部分移到屏幕外 | 拖动时合成器按 §8.8 留出 64 px（`kept_on_output`），重新排列时布局又整体夹回屏幕内 | `luban/xdesk/compositor/src/compositor/drag.cpp:136-142`；`luban/xdesk/layout/src/stack.cpp:28-29` |
| J62 | stack 里拖到屏幕边缘不会半屏、四分之一屏 | stack 窗口的拖动是"实时"的，`drag_motion` 在 125 行就返回，不算落点；`drag_end` 只对非实时拖动落下；布局里的贴边从未被走到 | `compositor/drag.cpp:125`、`232`；`compositor/windows.cpp:353`、`input.cpp:471`；`layout/src/drag.cpp:66-72` |
| J63 | 折叠的窗口移动后再展开，只剩一点点 | 落下时用快照里的矩形（折叠时只有标题栏高）作尺寸，写回 `world` | `layout/src/drag.cpp:210-214`；`layout/src/layout.cpp:319`、`726` |
| J64 | 折叠后的标题栏没有标识 | 未设计 | — |
| J65 | 书签是右边缘的小标签，不像书签 | 按 2026-10-21 §8.4 实现 | `compositor/bookmarks.cpp` |
| J66 | xdesk 的应用都没有图标 | 只有 Super+Tab 的切换器画图标；启动器、标题栏、书签都不画；只认 PNG | `compositor/switchers.cpp:220`、`459-470`；`base/src/apps/apps.cpp:102-125` |
| J67 | 设置不能像窗口一样移动 | 设置是合成器画的浮层，不是窗口 | `compositor/overlay.cpp`（`settings`） |
| J68 | 没有壁纸 | 未设计 | — |
| J69 | 桌面过一段时间结束：`the desktop ended (124)` | 已定因（K18）：私有域里的每个操作都以 30 分钟为限运行（`domain_producer::run` → `run_argv_with_timeout(…, 30min)`），桌面会话（`subos use --cmd luban __desktop`）也在内，30 分钟时被结束，124；另外"无限时"（`milliseconds::max()`）换算成纳秒会溢出到过去。修：会话（use、exec、start、log）不设时限；无限时即无截止；Luban OS 应用说出原因与日志位置 | `src/core/subos/root/root_cmd.cpp`（`run_domain_operation_`）；`modules/platform/src/process/process.cpp` |
| J70 | `luban enter` 时终端出现 `imDefLkup.c,419: The application disposed a key event` | X11 宿主有输入法时，SDL 窗口丢弃了一个按键事件（XIM 的提示） | `luban/os/window` |
| J71 | 应用崩溃没有任何痕迹 | xdesk 启动应用时三个标准流都接 `/dev/null`；子进程结束只从列表删掉；telemetry 的 `recent` 是空的 | `compositor/desktop.cpp:208-221`；`compositor/server.cpp:432` |
| J72 | 没有显示器窗口就不能运行桌面；无法抓取画面 | 显示提供方只有 Luban OS 窗口；`--display` 只有 `terminal`、`window`；控制 socket 没有截图 | `luban/os/cli/src/cli.cpp:679-717`；xdesk 已在 wlroots 无头输出上渲染（`server.cpp:161`、`290`） |
| J73 | 外部只能经原始 socket 连桌面；系统的域里没有路由 | 总线上只有 xdesk 的 `state`/`control`/`events`；帧、输入、剪贴板、声音、应用日志都不在；用户系统所在域没有 `run/openbus` | 2026-10-22 设计 §7；`compositor/bus.cpp:90` |
| J74 | 多显示器只能镜像 | openwx 0.1 的显示角色只有 primary、mirror；布局只有一个输出 | `luban/specs/opensal/src/wire.cpp:296-310` |
| J75 | 欢迎信息写着"2 CPU" | 写死的；实际是宿主核数的一半（2–8） | `luban/os/vm/src/host.cpp:40-43`、`610-612` |
| J76 | `xdesk msg debug hud` 只打出用法，第一行是 `keys list` | 必须带 `on|off`；用法不按子命令给 | `luban/xdesk/app/src/main.cpp:41`、`54` |
| J77 | 桌面里的终端不能右键复制、粘贴 | foot 没有右键菜单；xdesk 只有标题栏与桌面背景的菜单 | `compositor/input.cpp:426-452` |
| J78 | 桌面系统的完备性从未在本地按用户路径测过 | L4 在 machine 里手工起无头 xdesk，Falkon 只开 `file://`；没有长时间、升级、剪贴板、布局的场景；开发用的 shell 里 bwrap 不可用，本地跑时系统退回到 home 的 prefix | luban-os-ci `scenarios/L4/*`；2026-10-21 设计 §9.6a |

## 2. 原则

1. **用户路径即测试路径**：测试走人走的路（Luban OS 应用或 `luban` 创建、进入、使用），不另起捷径；捷径只能作为补充。
2. **看得见才算有**：一个功能要能经总线观察到（状态、事件、日志、画面），否则视为没有完成。
3. **总线是唯一的外部入口**：进程间、宿主与系统之间、人与 Agent 的访问都走 openbus；私有 socket 只能是进程内部的实现细节或后备。
4. **设备是设备**：显示器、输入、剪贴板、声音都是 openhal 设备，可以是真实的、窗口的、虚拟的、远程的；桌面不关心是哪一种。
5. **能力先校验**：任何默认值的改变，先查对方能不能接住（内核、驱动、宿主的限制）；接不住就退回并说明，不让用户撞墙。
6. **不留"以后"**：本轮列出的都做；平台做不到的写进实现表与报告，带原因与替代的覆盖方式。

## 3. 架构

### 3.1 分层

```
规范（只依赖标准库）   openbus 0.2   openhal 0.3   opensal 0.7   opencaps   openwx 0.2   openas
                          │             │             │
服务（有进程、有端点）  xbus 路由 ─────┼─────────────┼──── 每个 home 一个，系统里一个，经链路相连
                          │             │             │
库（kits）              openhal-host（真实设备）  openhal-emu（虚拟设备）  telemetry  opensal-wlr
                          │
程序                    xdesk（合成器，显示设备的使用者）  xdesk-settings  xdesk-wallpaper
                        Luban OS 应用（窗口型显示设备的提供者）  luban（CLI，总线客户端）  xbus（工具）
```

依赖方向不变：规范只依赖标准库；kit 不引用 `xlings.*`、`luban.*`；`os/service` 不引用表现层。新增的 `openhal-emu` 是 kit。

### 3.2 总线上的路径

所有路径以 `host/<宿主 id>/` 开头。系统内的节点经链路出现在 `host/<id>/system/<名字>/` 下，machine 与 sandbox 一样。

| 路径 | 接口 | 交互 | 说明 |
|---|---|---|---|
| `…/device/display/<n>` | `openhal.display@0.3` | call、stream、region | 显示设备：模式、角色、截图、录制；帧在 region，事件在 stream |
| `…/device/input/<n>` | `openhal.input@0.3` | stream | 指针、键盘、文本、手势 |
| `…/service/clipboard`（宿主与每个系统各一个） | `luban.clipboard@1` | call、publish | 区域的剪贴板：读、写、变化；宿主路由上的那个还维护"最新的一次"（§8.2） |
| `…/device/audio-output/<n>`、`audio-input/<n>` | `openhal.audio@0.2` | stream、region | 声音 |
| `…/system/<名字>/service/xdesk/state` | `xdesk.state@1` | call | 已有 |
| `…/system/<名字>/service/xdesk/control` | `xdesk.control@1` | call | 已有：与控制 socket 同样的请求 |
| `…/system/<名字>/topic/xdesk/events` | — | publish | 已有；加 `app-start`、`app-exit`、`display-attach`、`display-detach` |
| `…/system/<名字>/service/xdesk/apps/<pid>/log` | `xdesk.log@1` | stream（可重放最近 1 MiB） | 每个应用的标准输出与错误 |
| `…/system/<名字>/service/session` | `luban.session@1` | stream | 终端会话（2026-10-22 §7a 已有计划） |
| `…/system/<名字>/service/desktop` | `luban.desktop@1` | call | 桌面会话的生命周期：开始、结束、原因、退出码 |

`host/<id>/device/display/*` 是宿主的显示设备（Luban OS 窗口、虚拟显示器）；系统里的 xdesk 打开它们。一台电脑上的显示器也可以
被另一台电脑的系统打开（经链路，跨不了内存时 region 退化为 inline 并计数）。

### 3.3 路由的生命周期（J73）

- **宿主**：每个 home 一个路由，socket 在 `<home>/run/openbus/router.sock`。由第一个需要它的程序启动（`luban`、Luban OS 应用、
  `xbus`），空闲 10 分钟且没有节点时退出；私有域的 home 有它自己的路由，经链路接到域外的路由。
- **sandbox 系统里不另起路由**：宿主的路由为每个系统开一个监听 socket（`<home>/run/openbus/systems/<名字>.sock`），绑进系统的
  `/run/openbus/router.sock`；从这个 socket 连上来的节点，路径前缀由路由按 socket 定为 `host/<id>/system/<名字>/`，节点自己声明的
  前缀不算——系统里的程序不能冒充宿主或别的系统的路径。region 的 memfd 经 `SCM_RIGHTS` 穿过绑定的 socket。
- **machine**：路由由 agent 承载，经 virtio-serial 的 `L` 帧与宿主的路由相连（已有）；前缀同样由宿主按链路定。
- **节点是"耐心"的**：路由后来才有也能接上（xdesk 已是）。
- `luban status <名字> --bus` 显示路由、链路与该系统的端点。

### 3.4 访问与授权

- 读状态、订阅事件、读日志：系统的所有者与被授权的 Agent。
- 注入输入、改设置、接显示器、截图与录制：所有者；Agent 需要 `desktop-control` 授权（opencaps 的新能力名），每次都记成 openas 的
  run（谁、替谁、做了什么、动作的种类）。能注入输入就能在终端里打命令，所以授权提示写明它等同于以这个用户身份运行命令。
- 宿主的剪贴板：沿用 `clipboard`、`clipboard-paste` 授权；桌面版本默认授予两者。
- 截图、录制与 `luban logs --bundle` 可能含有屏幕内容与网址：打包默认不含截图（`--with-screenshot` 才加），生成后提示"分享前请检查"。

### 3.5 控制 socket 的位置

xdesk 的控制 socket 保留，作用改为两件：同一会话里没有路由时的后备；`xdesk msg` 在系统里直接用。外部工具（`luban`、测试、Agent）
一律走总线；`xdesk msg` 有路由时也走总线。文档与帮助只写总线的用法。

## 4. 显示器：设备、虚拟显示器与热插拔（J72、J74）

### 4.1 openhal 0.3 的显示设备

- **种类**：`physical`（宿主的显示器，经 openhal-host）、`window`（Luban OS 窗口）、`virtual`（openhal-emu，任意尺寸、密度、刷新率）、
  `remote`（另一台电脑的显示设备，经链路）。
- **属性**：尺寸、缩放、刷新率、色彩格式、是否接受输入、是否在线。
- **角色**（openwx 0.2）：`primary`、`mirror <n>`、`extend <n> <left|right|above|below>`。
- **操作**：`attach`（系统的桌面打开它）、`detach`、`capture`（一张截图，PNG）、`record`（一段帧，按损坏矩形，带时间戳）、`mode`。
- **热插拔**：设备上线、下线都是事件；桌面按角色重新排布。

### 4.2 虚拟显示器

- `openhal-emu` 提供：内存里的帧缓冲（region），按设定的刷新率向使用者要帧；没有人看时照样运行，按 §9.7 的待机规则不出帧。
- 截图、录制直接从 region 读，不经过任何窗口；录制可以导出为图片序列或 y4m，供 L5 与回归比较。
- 测试与 Agent 用虚拟显示器加输入设备驱动桌面，与人用窗口走同一条路径。

### 4.3 命令（§12 讨论）

```bash
luban enter desk --display virtual                 # 虚拟显示器，默认 1920x1080@1；桌面在后台运行
luban enter desk --display virtual=2560x1440@1.5
luban display desk                                 # 列出：0 virtual 1920x1080 primary；1 window 1100x700 mirror 0
luban display desk attach window                   # 把 Luban OS 窗口接进来（默认作为镜像；--as primary|extend-right）
luban display desk capture shot.png [--display 0]
luban display desk record 10s out/ [--display 0]
luban display desk detach 1
```

Luban OS 应用的"进入"即 `attach window`：桌面已经在运行（虚拟显示器上）时，窗口接进来而不是另起一个桌面。

与已有设置的关系：
- 关窗口时按 `on-close`：`background` 时桌面不结束，自动接上一个虚拟显示器作为主屏继续运行；`poweroff` 时结束；`ask` 时问。
- `luban ls` 与 Luban OS 应用的列表标出"在运行（虚拟显示器）"，点"进入"即接上窗口。
- 最后一个显示器拔掉时桌面不结束（总有一个虚拟显示器兜底），结束桌面沿用已有的关闭方式。

### 4.4 多显示器扩展（J74）

- xdesk 的布局按输出分区：每个输出有自己的工作区视图与状态栏；WuXian 是一个平面，多个输出是平面上并排的多个镜头。
- 窗口在输出之间拖动；显示器下线时，它上面的窗口移到主屏，记住原来的输出，回来时归位。
- 镜像只是一种角色，不再是唯一的多显示器方式。

## 5. 总线上的表面、设备与传输（J57、J73）

### 5.1 opensal 0.7：表面跑在 openbus 上

- opensal 的消息不变（帧、输入、剪贴板、指针、文本、光标），载体从私有的 socket 改为 openbus 的 stream：每个显示设备一条，
  帧放进 region（`memfd`，双缓冲，按最大模式预留，`F_SEAL_SHRINK`），消息只引用损坏的矩形。
- 跨不了内存的一跳（另一台电脑、两跳的中继）自动退化为 inline，帧做行差分加 LZ4（opensal 0.6 已有），并计数。
- 显示设备与 xdesk 之间的 stream 用总线的直连（openbus §7：路由授权后交给双方一对已连接的 socket），帧的消息不经路由转发，
  路由只记统计。
- 私有 socket 的连接方式去掉，Luban OS 应用与 xdesk 只经总线相连（不考虑旧版本）。

### 5.2 输入、剪贴板、声音

- 输入是显示设备附带的 stream（`device/input/<n>`）；虚拟输入由 openhal-emu 提供，测试与 Agent 的输入走它，和人的输入同一路径。
- 剪贴板是每个区域的服务（§8）。
- 声音迁到 `device/audio-*`，sandbox 不再经 guestfwd 的特例，machine 经链路。

### 5.3 应用的日志与生命周期

见 §6。

### 5.4 machine 的帧：共享内存

- qemu 加一个 `ivshmem-plain`，内存后端是宿主上的一块 memfd；客机里的 xdesk 映射这个 PCI 设备 BAR 2 的 sysfs 资源文件
  （`/sys/bus/pci/devices/<地址>/resource2`），不需要 UIO 驱动；客机的 init 把这个文件交给桌面的用户。
- 设备在启动时按设置的上限定大小（默认能放两个 4K 显示器的双缓冲，约 130 MiB），按显示设备切分成多个 region；超出上限的显示器
  走 inline 并在状态里写明。PCI 设备不做热插拔。
- openbus 的 region 增加 kind `ivshmem`（placement 是宿主的显示设备，coherence `owned`，引用里的偏移是 BAR 内的偏移）。
- 消息（损坏矩形、输入）仍经 virtio-serial 链路，量很小；像素不再经过串口。
- 不能用 ivshmem 的 carrier（vz、WSL2 嵌套）按实现表走 inline 加压缩，并在 `luban status` 与报告里写明。
- 预算（沿用 2026-10-21 §5.6a）：1080p 拖动 ≥ 50 帧/秒、刷屏 ≥ 30 帧/秒、待机不出帧；本地参考机器上验收，CI 比较相对回归。

### 5.5 实现表（平台）

| 平台与方式 | 帧 | 输入 | 剪贴板 | 声音 | 虚拟显示器 |
|---|---|---|---|---|---|
| Linux sandbox | memfd region | stream | 设备 | stream | ✓ |
| Linux machine（KVM） | ivshmem region | 链路 | 链路 | 链路 | ✓ |
| Linux machine（TCG） | ivshmem region | 链路 | 链路 | 链路 | ✓ |
| macOS（vz，经宿主机） | 两跳：inline + LZ4（vz 的 virtio-gpu 扫描输出做可行性验证，过门槛即用） | 链路 | 链路 | 链路 | ✓ |
| Windows（WSL2 宿主机） | 两跳：inline + LZ4（hvsocket） | 链路 | 链路 | 链路 | ✓ |

## 6. 默认可观察（J71、J69、J76）

### 6.1 应用的输出与退出

- 收集不在 wayland 循环里做：一个独立的线程以非阻塞方式读所有应用的管道，限速（每个应用每秒 1 MiB，超出丢弃并计数，日志里写
  "丢了 N 字节"），满了不反压应用；wayland 循环只收到"应用退出"这一件事。
- xdesk 启动应用时，标准输出与错误接到一条管道；xdesk 把它写进系统用户的 `~/.local/state/xdesk/apps/<应用>-<pid>.log`（每个应用
  保留最近 5 份，单份上限 8 MiB），同时作为总线上的 stream 发布（新订阅者先收到最近 1 MiB）。
- 退出时发 `app-exit` 事件：应用 id、pid、退出码或信号、运行时长、日志路径、最后 20 行。
- 非正常退出（信号、非 0 且运行不到 10 秒）在桌面上提示："Falkon 意外退出（信号 6）"，点开看日志；设置里可关。
- 启动失败（找不到程序、exec 失败）同样是事件和提示，不再只是一行内部日志。

### 6.2 xdesk 与 Luban OS 应用自己的日志

- xdesk 的日志（现在的 `log(...)`）进 telemetry 的 `recent` 与文件，`telemetry recent` 不再是空的。
- Luban OS 应用：桌面会话的开始、结束、退出码、原因（谁结束了它）写进 `logs/luban-os.log`，并作为 `service/desktop` 的事件。
  "the desktop ended (124)" 这样的提示带上原因与日志位置。

### 6.3 一次收齐

`luban logs <名字> [--app falkon] [--since 10m]`（§12 讨论）：经总线收齐这个系统的桌面会话、应用、machine（console、supervisor、
virtiofsd）、Luban OS 应用的日志；`--bundle out.tar.zst` 打包，附 `luban status --json` 与当前截图。Agent 用它，不需要用户转述。

### 6.4 小处

- `xdesk msg debug hud` 不带参数时切换并说出现在的状态；`xdesk msg` 的用法按子命令列，不再总以 `keys list` 开头（J76）。
- `luban status <名字>` 增加：渲染方式（GPU 或软件，以及原因）、文件共享方式、显示设备、路由与链路。

### 6.5 124（J69）

先用 §11 的长时间场景复现（虚拟显示器上待机 2 小时，记录会话、Luban OS 应用、xdesk 的事件），定因后修；无论原因是什么，桌面
会话被结束时都要说出是谁、为什么。

## 7. machine 的可用性（J54–J57、J75）

### 7.1 内核属于系统

- 版本的 `boot` 声明 virt profile 的内核（`boot = { virt = "xim:linux-kernel-virt@6.12.112-r3", … }`）；内核进系统的 generation，
  随 `luban upgrade` 升级、随 `luban rollback` 回滚。
- `luban config <名字> --kernel <版本>` 按系统覆盖；没有声明内核的系统不能以 machine 启动，并提示 `luban upgrade <名字>`。
- 系统的 machine 不再用 home 级的内核：`kernel_()` 这类"在 home 里找一个 vmlinuz"的代码去掉。macOS、Windows 上的 Luban 宿主机
  不是一个系统，它的内核由 carrier 的包声明（`luban` 的 carrier 依赖），同样走 §7.2 的能力校验。
- `linux-kernel-virt` 各版本的 release 都是 `6.12.112-virt`，`uname` 区分不了：内核的身份以 xpkg 版本为准，`luban status` 显示它。

### 7.2 能力校验（opencaps）

- **内核提供什么**：读内核旁边的 `config`（每个版本都有，包括最早那一版），映射为能力名：`virtio-fs`（`FUSE_FS` 与 `VIRTIO_FS`）、
  `virtio-9p`、`virtio-gpu`、`udmabuf`、`pci-sysfs-mmap`（`PCI` 与 `SYSFS`，所有版本都有）、`virtio-vsock`。
- **启动要什么**：按设置与授权算出来：files=virtiofs 要 `virtio-fs`；virgl 要 `virtio-gpu`；KWin 软件渲染要 `udmabuf`；共享内存帧要
  `pci-sysfs-mmap`。
- **对不上**：有退路的退回（virtio-fs → 9p，GPU → 软件，共享内存 → inline），`luban status` 写明"9p（这个内核没有 virtio-fs：
  luban upgrade desk）"；没有退路的拒绝启动并给出确切修法；都进 `self doctor` 的报告。
- **原则**：任何默认值的改变先过能力校验（本次 22.1 的 virtio-fs 就缺了这一步）。

### 7.3 virtiofsd 与宿主的限制（J56）

启动前探测宿主是否允许非特权命名空间（`/proc/sys/kernel/apparmor_restrict_unprivileged_userns`、试建 userns），不允许时直接以
`--sandbox none` 启动，`luban status` 写"virtiofs（无沙箱：这台电脑限制非特权命名空间）"；日志不再出现看似失败的 ERROR。

### 7.4 启动页（J54）

- 函数开头读一次 `done`，后面都用这个值；工作线程写 `done` 用原子操作并在锁内写结果。
- 位置设定之后总有一个项目（`Dummy`），日志区放不下时也一样。
- 测试：窗口很矮时，成功、失败、取消三种结束都不 abort。

### 7.5 其它

- 欢迎信息的 CPU 数用实际值（J75）。
- 失败的启动在 Luban OS 应用里给出 console 的最后几行与修法（与 CLI 一致）。

## 8. 剪贴板与输入法（J60、J70、J77）

### 8.1 剪贴板是总线上的服务，按区域分开

- **区域**：宿主（每台电脑一个）、每个系统（sandbox 的桌面会话、machine）各是一个区域。每个区域有自己的剪贴板（`clipboard` 与
  `primary` 两种选区，按 Linux 的习惯保持分开）和自己的历史；历史永远不出区域。
- **服务**：每个区域在总线上有 `…/service/clipboard`（`luban.clipboard@1`）：
  - 系统里由 xdesk 提供（它就是会话的剪贴板，实现 `wlr-data-control`，真正的 `wl-clipboard` 可以直接工作）；
  - 宿主上由 Luban OS 的进程用 SDL 读写与监听宿主的剪贴板，不需要宿主装任何工具（T2）：X11（XFixes）、macOS（changeCount）、
    Windows（剪贴板监听）上托盘进程就能做，宿主一复制就看到；Wayland 宿主只把选区交给有焦点的表面：宿主合成器提供
    `ext-data-control-v1`（KDE、wlroots 系）时由托盘经它即时看到，否则由 Luban OS 窗口的进程在**获得焦点的那一刻**看到并排序。
    跨 OS 的交互必须经过 Luban OS 窗口（点进窗口、在窗口里复制或粘贴），而获得焦点先于窗口里的任何操作，所以人的每一次复制都按
    真实的先后排序——"焦点时才看到"不改变结果；
  - 焦点进入时 Wayland 每次都给一个新的 offer，是否"新的复制"按内容指纹判断（文字全量，其它类型取类型集合加前 8 MiB），同一份
    内容不会重复成为"最新的一次"；
  - 只有终端会话、没有 Luban OS 进程时，broker 的 `__clipboard` 退回宿主工具（T1，提示 `xlings install xsel` 或 `wl-clipboard`），再退回
    OSC 52。
- **桌面会话里的命令行工具**：`wl-copy`/`wl-paste`/`xsel`/`xclip` 的转发程序先看 `WAYLAND_DISPLAY` 是不是会话的 xdesk：是，就用会话
  的剪贴板（fish 的 Ctrl+V 由此可用）；不在桌面会话里（终端里的 TUI），才经总线去宿主的剪贴板服务。
- **失败时**：转发程序只写标准错误、返回非 0，标准输出为空——错误不会成为粘贴的内容（J60）。

### 8.2 跨区域、跨 OS：只有最新的一次

- **规则**：区域之间默认分开，只有**最新的一次复制**（所有参与的区域里最近的那一次 `clipboard` 选区）可以跨区域。在某个区域粘贴时：
  如果别的区域在这个区域自己最近一次复制之后又复制过，粘贴得到的是那一次；否则是本区域自己的。更早的条目、历史、`primary`
  选区都不跨区域。
- **谁说了算**：系统所在电脑的宿主路由上的剪贴板服务维护"最新的一次"——只记来源区域、类型与序号（序号由这个路由分配，不用各处的
  时钟），内容在粘贴时才从来源区域取（惰性）。
- **经显示器窗口跨 OS**：接进来的显示设备若是另一台电脑上的 Luban OS 窗口，那台电脑的剪贴板就作为一个区域经链路参与——在那台电脑
  上复制，再到窗口里的桌面粘贴，得到的就是它；反过来也一样。宿主与系统、两个系统之间、两台电脑之间都是同一个规则。
- **关窗口与断开**：来源区域离开（窗口关闭、链路断开、machine 关机）前，若"最新的一次"是它的，文字与图片先取回一份交给权威服务，
  粘贴不会落空。
- **类型**：`text/plain;charset=utf-8`、`text/html`、`image/png`（其它图片在来源侧转成 PNG）、`text/uri-list` 表示的文件——文件按内容
  传送，落到目标的 `~/Downloads/Clipboard/`（单次上限 256 MiB，超出时提示），再以新路径交给粘贴的程序。
- **只有用户操作时的复制才跨区域**：一次复制要成为"最新的一次"，它所在区域的显示设备在复制时必须有输入焦点（人或经
  `desktop-control` 的 Agent 正在操作它）；没有焦点时的复制（系统里后台的脚本、定时任务）只留在本区域。这样各平台的结果一致，后台
  程序也不能悄悄把内容推进宿主的剪贴板。宿主这一侧，人在宿主程序里的复制总算作操作。
- **只在用户操作时读**：一个区域读取别处的内容，只在它的显示设备有输入焦点时才被应答（与 Wayland 的焦点规则相同）——系统里的程序
  不能在后台随时读走宿主的剪贴板。
- **几个权威**：一台电脑上的系统接着另一台电脑的窗口时，那台电脑的剪贴板同时参与两边的权威服务；它的每次复制通知所有相连的权威。
- **授权**：一个系统要参与，读入需要 `clipboard-paste`，导出需要 `clipboard`（桌面版本默认授予）；没有授权的系统是孤立的区域，它的
  复制不出去、别处的复制也进不来，第一次碰到时提示授权的命令。
- **上限与隐私**：单次文字与图片 64 MiB（沿用 `kLimit`）；不做持续镜像；剪贴板内容不进日志与 telemetry，`xbus monitor` 只看得到类型
  与大小。

### 8.3 终端：保持 foot（J77，评审结论 C）

- 选区按 Linux 的习惯分开：选中进 `primary`，中键粘贴；Ctrl+Shift+C/V 用 `clipboard`。
- **右键粘贴 `clipboard`**：`luban:foot` 自带的 `/etc/xdg/foot/foot.ini`（xim-pkgindex `tools/res/luban/foot`，发 `1.27.0-r2`）在
  `[mouse-bindings]` 里把右键绑到 `clipboard-paste`——K18a 已验证 foot 1.27 的鼠标绑定接受它（`foot --check-config`，构建时也检查）；
  原来右键的"扩展选区"改到 Ctrl+右键（Shift 是 foot 的"越过程序的鼠标"修饰键，Shift+左键与它冲突）。
- fish 的 Ctrl+V 经 `wl-paste` 走会话的剪贴板（§8.1）；粘贴多行时，fish 与 bash 的 bracketed paste 不会直接执行。
- xdesk 的标题栏与桌面背景的菜单保持；不做通用的右键菜单。

### 8.4 输入法（J70）

Luban OS 窗口在 X11 有输入法时按 SDL 的方式把按键交给 XIM 过滤（`SDL_HINT_IME_IMPLEMENTED_UI` 等），不再丢弃按键；终端上不再出现
`imDefLkup.c` 的提示。中文输入经文本输入设备进入桌面（opensal 的文本消息已有）。

## 9. xdesk 的交互与外观（J61–J68、J74）

### 9.1 stack 的越出（J61）

- 规则沿用 2026-10-21 §8.8：标题栏至少留 64 px 或宽度的 1/4（取较小者），不高过顶边，不压到状态栏下面。
- 规则只在一处：布局的 `stack_rect_` 用同一个函数，合成器拖动时调用它；不再有两套。

### 9.2 stack 的贴边（J62）

- 实时拖动时也计算边缘：指针进入 24 px 的边带就显示半屏、四分之一屏或最大化的预览；松手吸附；离开边带取消。
- 吸附后的窗口记住吸附前的位置与大小，向外拖动即恢复（和现在的 Super+Shift+方向键一致）。
- 点标题栏的快速布局保留。

### 9.3 折叠（J63、J64；只在 WuXian）

- 折叠只用于 WuXian 的自由窗口与工作区（已定）。
- **尺寸语义**（openwx 0.2）：折叠时的矩形只是显示用；移动折叠的窗口只改位置，宽高保持折叠前的值；展开回到折叠前的宽高。
- **标识**：折叠后的标题栏左侧有 ▸，展开时是 ▾，点它切换；折叠后的标题栏用较深的底色和一条下边线，标题后标出"已折叠"。
- 双击标题栏、右键菜单、`Super+S` 都能切换（WuXian 里）。

### 9.4 书签：压在下面的书签（J65）

- 位置：盖住它中心点的最上层那一项的右边缘（不变）；最多 5 个，多了显示"+N"，"+N"是菜单（不变）。
- 外观：书签从盖住它的那一项**下面**伸出来——在 z 序上画在盖住它的项之下、被盖住的项之上，根部被压住约 1/3；多个书签沿右边缘
  上下错开排列，每个都有自己的颜色（窗口：应用图标的主色；工作区：它的颜色），上面是图标与简短标题（工作区：编号与名字）。
- 悬停：高亮这个书签（提亮并加粗边线），同时画出被盖住那一项的轮廓；点击浮到最上面并聚焦（不变）。
- 拖动、平移、缩放时隐藏，停下 250 ms 后再显示（不变）。

### 9.5 图标（J66）

- 启动器、标题栏、书签、切换器、桌面上的提示都显示应用图标。
- 查找：`Icon=` → 主题（hicolor、breeze、Adwaita、Papirus）的 PNG → `scalable` 的 SVG（用 mcpp-index 的 SVG 光栅化库，按需要的尺寸
  渲染并缓存）→ `pixmaps` → 首字母。
- `luban status` 与包的检查报告没有图标的应用。

### 9.6 设置是一个标准应用（J67）

- 设置是一个普通的窗口：`xdesk --panel settings` 是一个 Wayland 客户端，只负责把窗口开着（xdg toplevel，app id
  `xdesk-settings`），窗口里的内容由 xdesk 画、输入由 xdesk 接——画法与原来的设置页相同，外观与桌面一致。（实现时修订：评审定的
  "imkit 加 SDL" 不可行——mcpp-index 的 SDL3 在 Linux 上只有 X11 后端，而 xdesk 不提供 X 显示；改为这个做法，见 §18.3。）
- 读写设置走的就是 xdesk 自己的设置逻辑；Agent 与自动化经总线的 `xdesk.control@1`（`{"settings": "<页>"}` 打开，改设置的请求照旧）。
- 它是普通窗口：能移动、平铺、在 WuXian 里折叠、出书签；崩溃不影响桌面；`xdesk msg settings <页>` 打开它到那一页。
- 现在的浮层去掉（找不到自己的可执行文件时才退回浮层）；设置的内容与分页不变（显示器、布局、外观、按键、关于），加"壁纸"与
  "应用"（每个应用的日志与崩溃记录、意外结束时是否提示）。页名只有一张表（`settings_page_named`）。

### 9.7 壁纸：xdesk 的基础功能与自带的壁纸引擎应用（J68）

**xdesk 里的基础功能**（通用、便宜，任何桌面都有）：
- 纯色、渐变、图片（适应、填充、平铺、居中），每个工作区可以单独设定；
- 背景层：xdesk 实现 `wlr-layer-shell` 的 `background` 层，任何标准的壁纸客户端（例如 swaybg）都能用，按屏幕锚定；声明自己懂镜头
  的客户端（壁纸引擎）还可以占有某个工作区或 WuXian 平面的背景，xdesk 按镜头变换它的缓冲；
- 一处设定：`xdesk-settings` 的"壁纸"页列出基础功能与"壁纸引擎"，选中引擎就打开它的窗口；每个输出、工作区以最后一次的选择为准，
  两边不会互相覆盖；
- WuXian：平面有一张总的壁纸，锚定在平面上（世界坐标），工作区可以有自己的壁纸（锚在它的底板上）；平移、缩放时由 xdesk 按镜头
  对壁纸做变换（平滑、不等客户端），镜头停下后再通知引擎按新的缩放重新渲染（分形放大能看到新细节）；图片壁纸可以分视差层；
  工作区视图里壁纸锚定在屏幕上；
- 镜头与可见区域作为总线上的话题（`…/topic/xdesk/camera`，QoS `latest`）发布给背景客户端；
- 待机与预算：有窗口最大化、全屏、桌面待机（5 分钟无输入）、machine 的窄通道时，背景客户端收到"暂停"；待机时不出帧。

**壁纸引擎 `xdesk-wallpaper`**（桌面版自带的应用，更可玩）：
- 经典的程序生成渲染：
  - 数学：Mandelbrot/Julia 慢速缩放、Lorenz 吸引子、Gray-Scott 反应扩散、彭罗斯铺砌；
  - 物理：双摆、N 体、稳定流体、布料；
  - 图形学：SDF 光线步进场景、Perlin 流场、Boids、生命游戏。
- 可以玩：指针与壁纸互动（流体被拖动、Boids 躲开指针、点一下在反应扩散里滴一滴），参数可调（速度、配色、密度），可以暂停、截图、
  把当前一帧设为静态壁纸；
- 每个自带的渲染都有 GLSL（GPU）与 C++（CPU，低分辨率渲染再放大）两种实现；软件渲染时默认静态（每 10 分钟重新生成一帧）；
- 第三方壁纸是一个小包，只能是 GLSL 着色器加一张静态图，不接受二进制插件；着色器的参数：时间、分辨率、镜头、指针、种子；
- 帧率上限 30；默认用计算便宜、安静的一种（慢速的反应扩散）；
- 它是普通应用：从启动器打开是它的设置窗口（选择、参数、预览、每个工作区单独设定），背景渲染在后台进程里。

### 9.8 多显示器

见 §4.4。

## 10. 自带软件与打包（J59、J66）

### 10.1 Falkon

- 打包时收集 NSS 运行时加载的模块（`libsoftokn3.so`、`libfreebl3.so`、`libfreeblpriv3.so`、`libnssckbi.so`）及其 `.chk`；
  发 `26.04.2-r2`（版本号要变，只升 revision 不会让已装的系统移动），桌面版本发新版本引用它。
- 构建时的测试：经测试代理打开一个 https 页面与一个新标签页，Falkon 不退出。

### 10.2 打包规则：声明 dlopen

- 包在配方里声明运行时 dlopen 的库（`bundle = { dlopen = { "libsoftokn3.so", … } }`），打包工具按声明收集并校验存在；
  `tools/res/luban/bundle` 的检查在没有声明而 `strings` 里出现常见的 dlopen 名字（NSS、GL/Vulkan 加载器、GIO 模块、GStreamer 插件）时
  报警。

### 10.3 包的检查

沿用 2026-10-21 §6.1 的 `checks`：
- `gui` 支持 URL 列表（`file://`、经测试代理的 `https://`）；
- 新增 `icon`：启动器里能找到并画出它的图标；
- 新增 `exits-clean`：运行指定时间后仍在运行，或者以 0 退出；崩溃时附上 §6.1 的日志。

## 11. 本地真实场景全覆盖（J78）

### 11.1 驱动方式

- **环境**：隔离 home（`Home::isolated`），被测的 `xlings`、`luban`、Luban OS 应用、xdesk 由本次构建提供；`xim:bwrap` 装进测试 home
  （`XLINGS_TOOLS_SEARCH=home`），所以开发机的 shell 里没有可用的 bwrap 也能建出真正的私有命名空间——测试走用户的路径，不退回
  prefix。
- **显示**：虚拟显示器（§4.2），可以同时接一个无头的 Luban OS 窗口（`luban-os --headless`），覆盖"窗口作为显示设备"的路径。
- **输入与观察**：全部经总线：`device/input` 注入、`xdesk.state` 断言、`device/display capture` 截图、`apps/*/log` 与 `app-exit` 事件。
- **断言**：状态（布局、窗口、书签、折叠、渲染方式）、截图（与审过的基线比较，允许小差异，按区域掩码）、日志（没有崩溃、没有
  `[error]`）、性能（L5 的预算）。
- **场景文件**：数据驱动（与 luban-os-ci 同一种格式），一个场景是"一个人做一件事"；同一份场景在本地（`xdev test --lane-config
  luban-desktop`）与 luban-os-ci 运行。
- **确定性**：测试经 `xdesk.control@1` 设定固定的时钟、壁纸的种子与"静态"、关掉动画（减少动态）；截图基线按渲染器（pixman、GLES）
  各一份，状态栏的时间等区域按掩码排除，比较用感知差异的阈值；字体随系统固定版本。
- **开发环境**：先在 K18 验证开发用的 shell 能否在测试 home 里建出私有命名空间（装进测试 home 的 `xim:bwrap`）；不能时（外层沙箱
  不允许嵌套 userns），本地 lane 经 broker 在宿主上运行（一次审批），结果与日志回到开发环境——测试永远走用户的路径，不退回 prefix。

### 11.2 场景

| 编号 | 场景 | 断言 |
|---|---|---|
| S1 | 全新 home，Luban OS 应用快速开始，默认创建 desktop（sandbox），进入 | 进度阶段正确；桌面出现；渲染方式为 GPU（有渲染节点时）或说明原因；启动器列出全部应用且都有图标 |
| S2 | 依次打开每个自带应用，各操作一次后关闭 | 每个应用窗口出现、`exits-clean`；Falkon 打开 https 页面与新标签页；Audacious 播放并录到声音 |
| S3 | 复制粘贴（宿主分别为 X11 与 Wayland，Wayland 分有无 `ext-data-control-v1`）：宿主 → 终端、终端 → 宿主、终端 → Falkon、Falkon → 终端；foot 的右键粘贴、Ctrl+Shift+C/V、中键粘贴 `primary`；fish 的 Ctrl+V；图片与文件；系统 A → 系统 B（两个窗口）；在 A 里连复制三次再到 B 粘贴；来源窗口关闭后粘贴 | 得到的是最新的一次，更早的两次与 `primary` 不跨区域；宿主没有剪贴板工具时也成立；没授权的系统是孤立的并提示；失败时粘贴内容为空；内容不出现在日志与 `xbus monitor` 里；同一份内容重新获得焦点不算新的复制；系统里后台的 `wl-copy`（区域没有焦点）不跨区域，X11 与 Wayland 宿主上结果相同 |
| S3b | 跨 OS：luban-os-ci 的 macOS、Windows 托管 runner 上，桌面跑在 Luban 宿主机里的 Linux 系统中，Luban OS 窗口在 runner 的 OS 上，两向复制粘贴（托管 runner 之间不能互连，所以在一台 runner 上覆盖"不同 OS 经窗口"） | 同 S3；跑不了的平台在报告里写明原因 |
| S4 | stack：拖到左右边、四角、顶边；拖到屏幕外；向外拖恢复 | 半屏、四分之一屏、最大化；越出留出标题栏；恢复到原来的位置与大小 |
| S5 | WuXian：折叠、移动、展开；双击、菜单、Super+S | 尺寸不变；标识出现与消失 |
| S6 | 书签：盖住一个、三个、七个 | 书签数量、"+N"、悬停高亮与轮廓、点击浮上来；截图与基线一致 |
| S7 | 设置：打开、移动、平铺、改一项、关闭再打开 | 是普通窗口；设置被保存 |
| S8 | 壁纸：xdesk 的纯色、图片、每个工作区；壁纸引擎的几种渲染与指针互动；WuXian 平移与缩放；待机 5 分钟 | 跟随镜头（平移时无延迟，停下后重新渲染）；互动生效；待机时帧数为 0 |
| S9 | 显示器：虚拟显示器运行，接入窗口（镜像、主屏、扩展），拔掉，再接 | 布局重排；窗口归位；截图来自正确的设备 |
| S10 | 长时间：待机 2 小时，期间每 10 分钟操作一次 | 桌面不结束；内存与帧数在预算内（J69） |
| S11 | machine：同 S1–S3、S8 以 machine 运行；拖动与刷屏 | 共享内存帧；L5 预算 |
| S12 | 升级：先装上一个发布版，建 sandbox 与 machine 系统，再升级到被测版本（客户端与 `luban upgrade`），进入 | 系统带上自己的内核，machine 用 virtio-fs 起来；sandbox 桌面进入；之前装的包与设置还在；启动页不 abort（J54、J55） |
| S13 | 失败路径：machine 起不来（含 `--kernel` 指定一个没有 virtio-fs 的内核）、桌面程序崩溃、Falkon 崩溃 | 启动页给出原因与修法；崩溃提示与日志；`luban logs` 收齐 |
| S14 | Agent：只经总线完成 S2、S4、S9 | 与人的操作结果相同；openas 里有记录 |
| S15 | 多用户：第二个用户进入同一系统的桌面 | 各自的会话、剪贴板与设置互不影响 |

### 11.3 层次与门禁

- xlings 仓库：L0（布局、书签、折叠尺寸、能力映射、打包声明的单元测试）、L1（S1、S3、S4、S5、S13 的 sandbox 版本，作为 e2e）。
- luban-os-ci：全部场景，矩阵为版本 × 模式 × 渲染器；先只报告，稳定后 S1–S5、S12、S13 设为 xlings 的必需检查。
  - 每个 PR：S1–S9、S12、S13 与 S10 的 10 分钟版；
  - 每晚：全部场景，S10 跑满 2 小时。
- 发布前：在参考机器上本地跑全部场景（`xdev test --lane-config luban-desktop`），报告附在发布的 PR 里。

### 11.4 需求编号

`luban/tests/requirements.toml` 新增：`LUBAN-DISPLAY-VIRTUAL`、`LUBAN-DISPLAY-HOTPLUG`、`LUBAN-BUS-DESKTOP`、`LUBAN-DESKTOP-APP-LOGS`、
`LUBAN-DESKTOP-SESSION-END`、`LUBAN-DESKTOP-SCENARIOS`、`LUBAN-MACHINE-KERNEL-CAPS`、`LUBAN-MACHINE-SHARED-FRAMES`、`LUBAN-CLIPBOARD-SESSION`、
`LUBAN-DESKTOP-GPU-SANDBOX`、`LUBAN-OS-JOB-PAGE`、`XDESK-STACK-OFFSCREEN`、`XDESK-STACK-EDGE-SNAP`、`XDESK-COLLAPSE-KEEPS-SIZE`、
`XDESK-BOOKMARK-TABS`、`XDESK-ICONS`、`XDESK-SETTINGS-APP`、`XDESK-WALLPAPER`、`XDESK-MULTI-OUTPUT`。

## 12. 命令树的改动（评审结论 R1：A）

| 改动 | 位置 | 说明 |
|---|---|---|
| `luban enter --display virtual[=WxH@S]` | `luban.cli` 的 `enter` | 已同意 |
| `luban display <名字> [attach\|detach\|capture\|record\|mode]` | `luban` 新的子命令 | 显示设备的管理 |
| `luban logs <名字> [--app] [--since] [--bundle]` | `luban` 新的子命令 | §6.3 |
| `luban config <名字> --kernel <版本>` | `luban config` | §7.1 |
| `luban status <名字> --bus` | 已有 | 增加显示设备与渲染方式 |
| `xdesk msg debug hud` 无参数 | xdesk | §6.4 |

每一项更新 `luban.cli` 的规格、`docs/generated/` 与测试；Luban OS 应用里有对应的入口（显示器的接入在系统的菜单里，日志在"关于"
与设置的"应用"页）。

## 13. 兼容

不考虑旧版本（内部测试阶段，没有外部用户）：
- 去掉的旧路径：opensal 的私有 socket 连接、home 级的内核、设置的浮层；不写读取旧状态的特例（被 J63 写坏的折叠状态不修复，
  重新设定即可）。
- 内部已有的系统：`luban upgrade <名字>` 进入新的桌面版本（带内核、新 Falkon、`foot.ini`）；升级不了的重建。
- 规范的版本照常前进（openhal 0.3、opensal 0.7、openbus 0.2、openwx 0.2），不保留旧版本的协商分支。
- home 的格式：`run/openbus/systems/`、应用日志都是派生状态，不升格式版本。
- 仍要处理的只有 CI 的机制：xim-pkgindex 的安装测试会用旧客户端读新快照，新配方字段用 `min_client` 让旧客户端跳过——这是索引
  CI 的需要，不是对用户的兼容。

## 14. 文档

`docs/luban`（中英两版）：显示器与虚拟显示器、多显示器、总线上的桌面（路径、`xbus` 的用法）、日志与排错（`luban logs`）、
machine 的内核与能力、剪贴板、书签与折叠、设置应用、壁纸；`docs/generated/` 随命令树更新。

## 15. 任务与检查点

xlings（一个 PR，每个检查点一个 commit，每个都跑一次完整 CI）：

| 检查点 | 内容 |
|---|---|
| K17 | 本设计（本 commit） |
| K18 | 测试的地基：隔离 home 里装 `xim:bwrap` 的 fixture（或经 broker 在宿主上跑，§11.1）；场景格式与 runner（本地 lane `luban-desktop`）、确定性的钩子；用现有手段先跑通 S1、S12、S13，并复现 J58、J69 |
| K18a | 不依赖总线的修复先落：启动页（J54）、内核的能力校验与退回（J55）、virtiofsd 的探测（J56）、CPU 数（J75）、`xdesk msg` 的用法（J76）、stack 越出与贴边（J61、J62）、折叠的尺寸与标识（J63、J64）、剪贴板转发程序的错误不进内容（J60 的一半）；对应场景转绿 |
| K19 | 规范：openhal 0.3（显示、输入、剪贴板设备，角色）、opensal 0.7（stream + region）、openbus 0.2（`ivshmem` kind、`desktop-control`、日志流）、opencaps（内核与显示能力名）、openwx 0.2；一致性检查 |
| K20 | `openhal-emu`（虚拟显示器、输入、剪贴板、记录的声音）；xbus 路由的生命周期（宿主、sandbox、machine）、`luban status --bus` |
| K21 | xdesk 上总线：显示设备的使用者、输入、剪贴板、`apps/*/log`、`app-exit`、热插拔、多输出布局；Luban OS 应用作为窗口型显示设备与宿主剪贴板设备 |
| K22 | 命令：`--display virtual`、`luban display`、`luban logs`；桌面会话的结束原因（J69 在 S10 复现后修） |
| K23 | machine：内核属于系统（读 `boot.virt`、`--kernel`）、ivshmem 帧（J57） |
| K24 | sandbox 桌面的 GPU（J58）；剪贴板服务、按区域分开与"最新的一次"跨区域与跨 OS（J60 的另一半）；foot 的右键粘贴；输入法（J70） |
| K25 | xdesk 外观：书签、图标与 SVG（J65、J66） |
| K26 | `xdesk-settings` 应用；xdesk 的壁纸基础功能与背景层、镜头话题；壁纸引擎应用 `xdesk-wallpaper` 与自带渲染（J67、J68） |
| K27 | 全部场景在本地跑通；L0、L1 的补充；需求编号；文档；版本号（三处一起改，先查是否已被占用） |

xim-pkgindex（一个 PR）：Falkon `26.04.2-r2`（NSS 与 https 测试）、打包的 dlopen 声明与检查、`linux-kernel-virt` 的能力与各版本的
`config`、桌面版本声明 virt 内核并引用新 Falkon、桌面包的 `foot.ini`（§8.3）、`xdesk-settings`、`xdesk-wallpaper` 与壁纸包、`checks` 的
`icon` 与 `exits-clean`。

跨仓库的顺序：
1. Falkon `26.04.2-r2` 与 `foot.ini` 不依赖新客户端，先合并、先发布（内部的系统 `luban upgrade` 即得）；
2. xlings 发布后，再合并依赖它的部分（`boot.virt`、`checks` 的新种类、`xdesk-settings`、`xdesk-wallpaper`）；
3. luban-os-ci 的场景随 xlings 的检查点推进，未发布的修复以 `known` 标出。

luban-os-ci（一个 PR）：§11.2 的全部场景替换现有 L4；L5 改为在虚拟显示器上测量；L6 加"从上一版升级"（S12）。

mcpp-index（如需要）：SVG 光栅化库。

## 16. 评审

### 16.0 结论（第一轮）

| 项 | 结论 |
|---|---|
| R1 命令树 | A：`luban display`、`luban logs`、`luban config --kernel` 加进 `luban` |
| R2 内核 | B：属于系统，保留 `--kernel` 与 home 默认 |
| R3 终端右键 | C：保持 foot，右键粘贴；复制粘贴要完整可用，并经显示器窗口跨 OS 粘贴（§8.2、§8.3） |
| R4 machine 的帧 | A：ivshmem 为统一路径，virtio-gpu 扫描输出由 L5 对比决定是否作为加速 |
| R5 WuXian 的壁纸 | C，默认等于 A |
| R6 设置的工具包 | A：imkit + SDL（实现时修订为 C 的变体：占位客户端加 xdesk 绘制，原因见 §18.3） |
| R7 授权 | A：`desktop-control` 一个授权，记录细到动作种类 |
| R8 控制 socket | A：保留为系统内的后备 |

第二轮：

| 项 | 结论 |
|---|---|
| 剪贴板的模型 | 不合并选区；剪贴板是总线上的服务，宿主与各系统默认分开，只有最新的一次可以跨区域、跨 OS（§8.1、§8.2） |
| 壁纸 | xdesk 做通用的基础功能；桌面版自带的壁纸引擎应用做更可玩的渲染（§9.7） |
| 兼容 | 不考虑旧版本：内部测试阶段，旧路径直接去掉（§13） |
| 自审第一轮的其余改动（sandbox 不另起路由、文件按内容传送、第三方壁纸只能是 GLSL、检查点顺序、经 broker 在宿主上跑本地 lane） | 第二轮未提出异议，按建议执行 |

以下保留第一轮的选项与理由。

### R1 命令树：`luban display`、`luban logs`、`luban config --kernel`

**为什么是问题**：按规则，新的子命令要先讨论；它们会进帮助、补全、`docs/generated/` 与测试，改名以后会影响已发布的测试文本。
**选项**：
- A. 三个都按 §12 加进 `luban`：人与 Agent 用同一套命令，帮助里看得到；代价是 `luban` 的顶层多两个子命令。
- B. 显示与日志只经 `xbus` 工具（`xbus call host/…/device/display/0 capture`）：不加命令，但人要知道路径，Luban OS 应用之外没有友好的入口。
- C. 合进已有命令：`luban status <名字> --displays`、`luban status <名字> --logs`：不加子命令，但 `status` 承担了动作（接入、截图），语义变混。
**建议**：A。显示器与日志是系统级的常用动作，值得有自己的名字；内部都只是总线客户端，`xbus` 的路径用法写进文档作为底层入口。

### R2 内核属于系统

**为什么是问题**：现在内核是 home 级的工具（像 qemu），改为属于系统会动版本配方、generation 的内容与升级回滚的语义，涉及
xlings 与 xim-pkgindex 两个仓库。
**选项**：
- A. 属于 home，加 `--kernel` 覆盖：只改 xlings；但升级内核影响所有机器，回滚系统时内核不跟着回滚。
- B. 属于系统（§7.1），保留 `--kernel` 覆盖与 home 默认作为后备：和"运行时属于环境"一致，镜像与本地 machine 启动同一个内核，坏内核
  能随系统回滚；代价是版本配方补 `boot.virt`，每个系统的 generation 多一个内核（存储里共享，不重复占空间）。
**建议**：B。

### R3 终端的右键

**为什么是问题**：foot 设计上没有右键菜单，用户期望右键复制、粘贴。
**选项**：
- A. 桌面版的默认终端换成有右键菜单的终端（例如基于 Qt 的 QTerminal，复用已有的 `luban:qt-runtime`）：体验最接近期望；包更大，启动稍慢。
- B. xdesk 提供通用的右键菜单：客户端没有处理右键时，xdesk 弹出复制（当前选区）、粘贴；所有没有菜单的应用都受益；需要知道客户端
  "没处理"（以 `wl_pointer` 事件后 300 ms 内没有弹出 popup 为准），有误判的可能。
- C. foot 里右键直接粘贴（不弹菜单）：最简单；但和其它桌面习惯不同。
**第一轮的建议**：A 加 B 的有限版本。**结论**：C（§8.3），并要求复制粘贴完整可用、经显示器窗口跨 OS（§8.2）。

### R4 machine 的帧：ivshmem 还是 virtio-gpu 扫描输出

**为什么是问题**：两种都能去掉串口的带宽上限，代价不同。
**选项**：
- A. ivshmem（§5.4）：宿主与客机共享一块 memfd，xdesk 照旧在客机里合成，帧格式与 sandbox 完全一样；客机经 sysfs 映射 PCI BAR，
  不需要新的内核选项（自审 §18 修正了第一版"要 `UIO_PCI_GENERIC`"的说法）。
- B. virtio-gpu 的扫描输出（客机把合成结果交给虚拟显卡，qemu 在宿主侧拿到 dmabuf）：零拷贝、GPU 时最快；但帧不经过 opensal，
  损坏矩形、多显示器、虚拟显示器都要另做，且 macOS/Windows 的 carrier 不通用。
**建议**：A 作为统一路径；B 作为 GPU 时的加速在 L5 对比后决定是否默认。

### R5 壁纸在 WuXian 里的行为

**为什么是问题**：WuXian 是无限平面加镜头，壁纸可以锚在屏幕上（像普通桌面），也可以锚在平面上。
**选项**：
- A. 锚在平面上（§9.7）：平移、缩放时壁纸跟着动，空间感最强，程序生成的壁纸在缩放时有新细节；代价是平移时整屏都要重画（GPU 时便宜，
  软件渲染时需要降级为静态加视差位移）。
- B. 锚在屏幕上：最省；但在平面上移动时没有空间感，工作区与平面的区分也弱。
- C. 平面一张（锚在平面）、工作区各自一张（锚在工作区底板）：最丰富，设置也更多。
**建议**：C 的默认配置等于 A（工作区不单独设定时用平面的那张）。

### R6 设置应用的工具包

**为什么是问题**：设置要成为独立应用，需要一个能在系统里运行的 GUI 工具包。
**选项**：
- A. imkit 加 SDL（Wayland 客户端）：与 Luban OS 应用同一套外观与代码；系统里多一个 SDL 的依赖。
- B. Qt（复用 `luban:qt-runtime`）：控件齐全、可访问性好；外观与 xdesk 不一致，包大。
- C. xdesk 自己的绘制库做一个客户端：外观一致、依赖最少；但要补文本输入、滚动、可访问性等客户端能力。
**建议**：A。

### R7 总线上的授权

**为什么是问题**：桌面上总线后，注入输入、截图、接显示器都是敏感动作，Agent 也会用到。
**选项**：
- A. 所有者全部可做，Agent 需要 `desktop-control` 授权，每次记成 openas 的 run（§3.4）。
- B. 再细分 `desktop-input`、`desktop-capture`、`desktop-display` 三个授权：更细，但授权提示更多。
**建议**：A，记录足够细（每次动作的种类都写进 run），以后需要再拆。

### R8 控制 socket 的去留

**为什么是问题**：总线成为唯一的外部入口后，控制 socket 是否还要保留。
**选项**：
- A. 保留为系统内的后备（§3.5），文档只写总线：系统里没有路由时 `xdesk msg` 仍可用。
- B. 去掉：只剩一条路，但没有路由时 `xdesk msg` 不可用。
**建议**：A。

## 17. 不做的

无。平台做不到的项见 §5.5 的实现表，在 `luban status` 与 CI 报告里写明原因与替代的覆盖方式。

## 18. 自审（按视角）

每条写"看到了什么问题、改了什么"；改动已并入正文。

### 18.1 修订 1

| 视角 | 问题 | 改动 |
|---|---|---|
| 架构：总线的边界 | 第一版让 sandbox 里另起一个路由，再与宿主相连：多一个进程，而且系统里的节点可以自称任意前缀 | sandbox 不另起路由；宿主为每个系统开一个 socket，前缀按 socket 定（§3.3） |
| 架构：帧的路径 | 帧的消息若经路由转发，多一跳延迟，路由线程成瓶颈 | 显示 stream 用总线的直连，路由只记统计（§5.1） |
| 架构：ivshmem | 第一版写"要 `UIO_PCI_GENERIC`"，会多逼一次内核升级；也没说多个显示器怎么分内存、要不要热插拔 PCI | 经 sysfs 映射 BAR，不要新内核选项；启动时定大小、按显示设备切分、不热插拔，超出走 inline（§5.4、§7.2） |
| 生态：壁纸 | 第三方壁纸若带 CPU 实现，就是任意二进制插件进合成器旁边 | 自带的有 CPU 实现；第三方只能是 GLSL 加静态图（§9.7） |
| 生态：发布顺序 | 一次改四个仓库，用户等最久的反而是最简单的 Falkon 修复 | Falkon 与 `foot.ini` 先发；依赖新客户端的配方带 `min_client` 后发（§15） |
| 安全：输入注入 | `desktop-control` 能打字就能在终端执行命令，第一版没说 | 授权提示写明等同于以该用户运行命令；记录细到动作种类（§3.4） |
| 安全：日志与截图 | `luban logs --bundle` 默认带截图，可能泄露屏幕内容 | 默认不带，`--with-screenshot` 才加，生成后提示检查（§3.4） |
| 安全：剪贴板 | 跨 OS 若持续镜像，会把宿主的剪贴板一直送进系统 | 只在复制、粘贴时交换，惰性取内容；分方向授权；内容不进日志（§8.2） |
| 兼容：内核 | 能力不足只会退回，用户永远停在 9p | 先补：联网时装最新内核（修订 2 取消：内核属于系统，不再有 home 级内核） |
| 兼容：旧 xdesk | "保留一个发布周期"说不清到哪天 | 保留到 `.0`（修订 2 取消：不考虑旧版本，私有 socket 去掉） |
| 性能：应用日志 | 应用刷屏时，在 wayland 循环里读管道会拖慢桌面 | 独立线程、非阻塞、限速并计数，不反压应用（§6.1） |
| 性能：壁纸 | WuXian 平移时整屏重画，软件渲染吃不消 | 软件渲染时静态加视差位移；待机不出帧（§9.7，原文已有，核对过） |
| 测试：确定性 | 截图基线会被时钟、动画、壁纸、渲染器差异弄得不稳定 | 固定时钟与种子、关动画、按渲染器分基线、掩码与感知阈值（§11.1） |
| 测试：耗时 | 2 小时待机放进每个 PR 不现实 | 每个 PR 跑 10 分钟版，每晚跑满（§11.3） |
| 测试：开发环境 | "测试 home 装 bwrap 就能走用户路径"未必成立：开发用的 shell 可能不允许嵌套 userns | K18 先验证；不行就经 broker 在宿主上跑本地 lane（§11.1） |
| 体验：关窗口 | 有了虚拟显示器，关窗口后桌面的去向没说 | 按已有的 `on-close`：`background` 时转到虚拟显示器继续运行；列表里标出（§4.3） |
| 体验：剪贴板 | 两种选区（主选区、剪贴板）让"选中了却粘不上"很常见 | 主选区跟随剪贴板（修订 2 改为：选区按 Linux 习惯分开，右键粘贴 `clipboard`，§8.3） |
| 体验：关窗口时的剪贴板 | 系统里复制、关窗口后在宿主上粘贴，内容已随窗口消失 | 来源离开前把"最新的一次"的文字与图片取回交给权威服务（§8.2） |
| 交付：检查点的顺序 | 第一版把用户最痛的 bug 修复排在总线与规范之后 | 新增 K18a：不依赖总线的修复紧跟测试地基先落（§15） |
| 范围 | 本轮很大（12 个检查点、4 个仓库） | 保持单个 PR、每个检查点一次完整 CI；每个检查点都让一组场景转绿，可以单独回看 |

### 18.2 修订 2

| 视角 | 问题 | 改动 |
|---|---|---|
| 架构：剪贴板的权威 | "最新的一次"若按各处的时钟比较，电脑与虚拟机的时钟不一致会排错 | 序号由系统所在电脑的宿主路由分配；内容惰性地从来源取（§8.2） |
| 架构：多个权威 | 一台电脑的系统接着另一台电脑的窗口、反过来也接着时，有两个权威 | 窗口所在电脑的剪贴板同时参与两边，每次复制通知所有相连的权威（§8.2） |
| 平台：宿主剪贴板 | 第一版写"托盘常驻提供"；Wayland 宿主只把选区交给有焦点的表面，托盘做不到 | X11、macOS、Windows 由托盘即时看到；Wayland 有 `ext-data-control-v1` 时托盘即时看到，否则在窗口获得焦点的那一刻看到——跨 OS 的交互必经窗口、焦点先于操作，所以人的复制顺序不受影响；按内容指纹去重（§8.1） |
| 平台：后台的复制 | 不经窗口的复制（系统里后台的 `wl-copy`）若参与排序：X11 宿主上后台那次胜出，Wayland 宿主上（焦点时才看到宿主的复制）宿主那次胜出——平台之间结果不一致；后台程序还能悄悄把内容推到宿主 | 只有区域有输入焦点时的复制才跨区域，后台的复制只留在本区域：各平台一致，也防止剪贴板被后台程序劫持（§8.2） |
| 安全：读取别处的剪贴板 | 系统里的程序若能随时向权威要内容，就能在后台读走宿主的剪贴板 | 只在该区域的显示设备有输入焦点时应答（§8.2） |
| 隐私：选区 | `primary` 每次选中都变，若跨区域等于把所有选中的文字送出去 | 只有 `clipboard` 的最新一次跨区域，`primary` 与历史不出区域（§8.2） |
| 生态：壁纸 | 只为自家引擎设计背景层，标准的 Wayland 壁纸客户端不能用 | xdesk 实现 `wlr-layer-shell` 的背景层（swaybg 等可用，按屏幕锚定），懂镜头的客户端再占有工作区或平面（§9.7） |
| 体验：壁纸的设定 | xdesk 与引擎各有设定，可能互相覆盖 | `xdesk-settings` 的"壁纸"页是唯一入口，按输出、工作区以最后一次选择为准（§9.7） |
| 架构：内核 | "不再有 home 级内核"会误伤 macOS、Windows 的 Luban 宿主机（它不是系统，却要内核） | 宿主机的内核由 carrier 的包声明，同样做能力校验（§7.1） |
| 测试：跨 OS | 第一版的 S3b 要两台托管 runner 互连，做不到 | 在 macOS、Windows runner 上经 Luban 宿主机跑 Linux 系统，窗口在 runner 的 OS 上，一台机器上覆盖"不同 OS 经窗口"（§11.2） |
| 测试：剪贴板 | 第一版的 S3 断言不到"只有最新的一次" | 连复制三次再跨区域粘贴、来源窗口关闭后粘贴、内容不进日志（§11.2） |
| 兼容 | 不考虑旧版本后，第一版的过渡路径都成了负担 | 去掉私有 socket、home 级内核、折叠状态的修复特例；只留索引 CI 需要的 `min_client`（§13） |
| 右键粘贴的依赖 | foot 的鼠标绑定是否接受 `clipboard-paste` 未验证 | K18a 先验证，不接受时给 `luban:foot` 打补丁（§8.3） |

## 19. 实施计划与依赖

按视角拆成的任务、它们的先后，以及跨仓库的联动与联调。检查点仍是 §15 的编号，这里给出顺序与依据。

### 19.1 依赖图

```
K18a 修复（无依赖，先落） ──────────────────────────────────────────────┐
K18 测试地基（场景格式、runner、确定性钩子） ─┬─────────────────────────┤
K19 规范（openhal 0.3 / opensal 0.7 / openbus 0.2 / opencaps / openwx 0.2）│
  ├─ K20 openhal-emu + 路由生命周期 ─┬─ K21 xdesk 上总线 ─┬─ K22 命令（display、logs、virtual）
  │                                  │                    ├─ K24 剪贴板服务、sandbox GPU、foot、输入法
  │                                  │                    └─ K26 设置应用、壁纸（经 xdesk.control@1）
  └─ K23 machine：内核属于系统（boot.virt）+ ivshmem 帧 ── 依赖 xim-pkgindex 的 boot.virt
K25 书签与图标（只依赖 K18 的截图基线）
K27 全部场景、文档、版本号 ── 依赖以上全部
```

### 19.2 每个视角看什么

| 视角 | 落在哪里 | 怎么验证 |
|---|---|---|
| 架构 | 总线是唯一入口（§3）；规范只依赖标准库；kit 不引用 xlings | `tools/lint_layer_deps.sh`；规范的一致性用例 |
| 稳定性 | 启动页不 abort（J54）；能力先校验（J55）；崩溃可见（§6） | e2e：短窗口三种结束；单元：能力映射；S13 |
| 优雅简洁 | 一条规则只有一处（`kept_on` 同时给布局与拖动用）；控制 socket 退为后备 | 代码审阅；没有重复实现的同名逻辑 |
| 用户体验 | 贴边、折叠标识、书签、图标、设置是窗口、壁纸 | 场景 S4–S8 的截图基线与状态断言 |
| 兼容性 | 不考虑旧版本（§13）；只有索引 CI 的 `min_client` | 索引的安装测试 |
| 跨平台 | §5.5 实现表；Windows/macOS 上的 Luban 宿主机 | CI 三平台；S3b |
| 一致性 | 人与 Agent 同一条路径（虚拟输入）；命令树、`docs/generated/` 同步 | `test_generated_command_reference.py`；S14 |
| 无感升级 | 系统带内核随 `luban upgrade` 前进；新 Falkon、`foot.ini` 经版本号 | S12：装上一版、升级、进入 |

### 19.3 跨仓库的联动与联调

1. **xim-pkgindex 第一部分（不依赖新客户端）**：Falkon `26.04.2-r2`（NSS）；桌面包的 `foot.ini`；各版本新增 `boot.virt`（旧客户端读到
   `boot` 里不认识的键会忽略，不需要 `min_client`）。先合并，xlings 的 CI 才能在真实索引上测 machine 的内核。
2. **xlings PR**：K18a → K27；每个检查点跑一次完整 CI。
3. **xlings 发布**后：xim-pkgindex 第二部分（`xdesk-settings`、`xdesk-wallpaper`、新 xdesk 版本、`checks` 的新种类，带 `min_client`）。
4. **luban-os-ci**：场景随检查点推进；未发布的修复以 `known` 标出，发布后去掉。
5. 联调：本地隔离 home 里 `xlings config --index-repo` 指向索引分支，跑 S1–S3、S11、S12，再进 CI。

### 19.4 本地资源

本地构建与测试不超过机器的 80%（`-j 24`，同时只跑一组重的测试）；磁盘不足时删除项目的临时与过期缓存（`/tmp/xtest-*`、
`mcpp clean --stale`）。

### 18.3 实现中的修订

| 视角 | 问题 | 改动 |
|---|---|---|
| 可行性：设置应用（R6） | mcpp-index 的 `compat.sdl3` 在 Linux 只有 X11 后端；xdesk 不启动 Xwayland，桌面里没有 X 显示——基于 SDL 的设置应用显示不出来；给 SDL 加 Wayland 后端要改 mcpp-index 的 SDL 包（协议生成、wayland-scanner），代价远大于收益 | 设置是 `xdesk --panel settings` 持有的普通窗口，内容与输入由 xdesk 负责：窗口行为（移动、平铺、折叠、书签、焦点）与其它窗口一样；外观与桌面一致；不增加任何依赖（§9.6） |
| 一致性：桌面接显示器 | 设计写"xdesk 打开显示设备"；实际由系统里的 `luban __desktop --display <路径>` 打开总线上的流，再接给会话（xdesk 或经 opensal-wlr 的其它合成器） | 所有合成器同一条路径；xdesk 的传输不变（§5.1 的意图不变：显示设备与桌面之间只有总线上的流） |
| 根因：J69、J58 | 两个"未定因"的问题都在私有域：域内操作 30 分钟时限、域的 `/dev` 没有 GPU 节点 | 会话不限时、会话带上 GPU 设备节点（§1） |
| 安全：`desktop-control`（R7） | 虚拟显示器原本挂在宿主的 `device/display/<n>`，路由按路径判定授权时分不出它属于哪个系统；Agent 的身份只能取它声明的 actor | 虚拟显示器与它的输入挂到系统下（`system/<名字>/device/display/<n>`）；路由按系统的 `desktop-control` 授权判定 Agent 对显示器、输入、xdesk 控制的调用，桌面服务自己判定 attach/detach/end（status 不需要）；Luban OS 窗口这个宿主显示器只属于坐在它前面的人，Agent 不能用；每次判定记在 `logs/openbus/desktop-control.ndjson`。授权是显式的（不随预设授予），持有它的策略文件要求 `2026.10.24.2` 起的客户端 |
| 完整：machine 的虚拟显示器 | machine 系统 `--display virtual` 原先被拒绝 | 监督进程启动（或沿用）machine，经它的 agent 在系统里运行 `luban __desktop --display`；帧经 ivshmem（机内从 PCI 设备 `1af4:1110` 的 `resource2` 自动找到） |
