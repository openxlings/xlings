# Luban OS：WuXian（无限画布）上的工作区与 WuXian 规范、Luban 的独立架构、GPU 渲染、xdesk 交互与设置的打磨、矢量指针与晃动找指针、双进度条、系统里的 xlings 与 sudo、Falkon 的 ICU

日期：2026-10-20　状态：修订 3，已评审（§22 各点已确认）　缘起：2026.10.20.1–.3 在 Linux 宿主（KVM）上的实测（`desktop-4`、`desktop-40`，
machine 模式的 `luban:desktop 2026.10.20.1`），以及之后关于 xdesk 布局模型、交互、渲染、命名与 Luban 独立的讨论。

范围：WuXian 规范 openwx（新 `luban/specs/openwx`）；指针（新 `luban/kits/cursor`）；xdesk（`luban/xdesk/*`：布局模型、两种视图、拖放、切换器、滚轮、设置、关于、复制粘贴、渲染器）；opensal（`luban/specs/opensal`：
共享内存的帧）；Luban OS 应用与窗口（`apps/luban-os`、`luban/os/window`：进度页、新建页、剪贴板默认、帧与指针）；luban
（`luban/os/service`、`luban/os/cli`：进度阶段、`luban new` 的名字、桌面系统的 GPU 授权）；machine（`luban/os/vm`：sudo、virtio-gpu）；
xlings（`src/core/xim`、`src/core/xvm`、`modules/confine`：系统里的 Lua worker、名字解析的报错、shim）；xim-pkgindex（`luban:qt-runtime`、
`luban:falkon`、`luban:cursors`、`xim:mesa`、`xim:qemu-system`、内核、`luban:xdesk`、`luban:desktop` 的新版本）。

交付：xlings 一个 PR，按 §19 的检查点逐个 commit，每个检查点跑一次完整 CI；xim-pkgindex 一个 PR（资源与配方），其中 xlings 发布前就要有的
资源（mesa、qemu、内核）先发到 xlings-res，配方的版本指针在 xlings 发布后合并。

替换的旧决定：2026-10-19 设计 §17.2–§17.3 的"三个预设"与画布上的"平铺组"、§15.2 的像素指针；2026-10-20 设计 §7.5 的"拖动时其他窗口
实时移到落下后的位置"、§13.2 第 5 条（GPU，改由本文 §7 完整定义）、G22 与 G23 的阶段与权重。

## 0. 核心点

1. **一张 WuXian，工作区是 WuXian 上的特殊窗口**：桌面只有一张 WuXian（无限画布），一直存在；工作区是 WuXian 上带编号（1–9）、名字、颜色、规则（平铺 / 层叠）
   的区域，可以移动、等比缩放、互相重叠；工作区之外是自由窗口。WuXian 不再是一种布局，它就是桌面（§3.1）。
2. **两种视图**：工作区视图（镜头锁在一个工作区、1:1，与今天的平铺 / 层叠相同，不做工作区之间的操作）与 WuXian 视图（镜头自由，
   工作区之间的一切操作在这里）。Super+O 进出（§3.2）。
3. **工作区像窗口一样有层次**；没有阴影、边框是它的颜色、左上角标签 `● 2 · 编码 · ▥`；等比缩放只改 WuXian 上的样子，进入总是 1:1（§3.3–§3.5）。
4. **删除有窗口的工作区要问**：变成自由窗口（默认），或关闭全部——那个按钮 5 秒后才能按（§3.9）。
5. **平铺拖动改为冻结布局 + 占位**；修掉起手就落到最后一列（§3.8）。
6. **状态栏显示工作区指示**：颜色是工作区自己的（身份），形状是它的规则（平铺：分栏的方块；层叠：错开的两块）；当前工作区是完整的一块
   （颜色、编号、名字、规则），其他已有的工作区是带编号的小图形，点击切换；不显示工作区之间的拖放与管理（§3.13）。
7. **WuXian 是名字，也是规范**：无限画布在界面、文档、配置、快捷键与代码里都叫 WuXian（`wuxian`、`WuXian`）；它的模型、视图、
   状态格式与基本交互写成开放桌面的规范 openwx（open WuXian，`luban/specs/openwx`）（只依赖标准库），xdesk 是它的第一个实现（§15）。
8. **为 Luban 独立出去做好架构**：Luban（`luban/`、`apps/luban*`、生态包）对 xlings 的依赖分三层——基础库、两边共读的格式、
   xlings 的领域只经协议（`Port` 与 xlings 的 JSON 协议）；lint 按允许表检查，本轮新代码遵守，现有的越层依赖列出清单与去处；
   版本、测试、文档、发布脚本、生态包（`luban:` 命名空间、它的库与资源）都按"整体可以搬走"放置（§16）。
9. **GPU 渲染本轮完成**：xdesk 有 GPU 时用 GLES 合成，没有或失败时退回 pixman 并说明原因；沙箱经 render node + 系统里的 Mesa，
   machine 经 virtio-gpu（virgl，宿主 qemu 的 GL 版本）；帧在同一台电脑上经共享内存交给窗口；关于页写实际用的渲染器与原因（§7）。
10. **矢量指针代替像素指针**：尾巴歪是两个原因叠加——画法本身是 45°（与箭头右边平行），以及在 1.25 这类非整数密度下最近邻缩放；
   改为标准比例的矢量形状，按每个实际尺寸抗锯齿生成，窗口不再重采样；**快速晃动鼠标时指针变大、变成强调色**，停下后缩回，
   便于找到它（类似 macOS，可设）（§8）。
11. **切换器与滚轮**：展开等待 500 ms 且可设；选中与悬停分开；选中后突出；Super+滚轮不弹提示、不误开启动器；滚轮慢一格一列、快则加速；
   中键交给应用（§4）。
12. **设置**：外观、三种状态、输入法搜索、间隔 4、恢复默认、"设置"在启动器里；关于页说真话（§5、§6）。
13. **复制粘贴**：自己的桌面系统默认开剪贴板；全局 Super+C / Super+V（§9）。
14. **双进度条**与正确的阶段（§10）；**`luban new` 的名字**（§11）；**新建页**：一张 xdesk 卡、传统桌面折叠、档位角标（§12）。
15. **系统里的 xlings**：根因已定——有系统层（用户视图、私有 subos）时，**来自下层的包要在 Lua worker 里读配方，而 worker 在沙箱里因
    bwrap 绑定 `/etc/ld.so.cache` 失败、在 machine 里因建不出输出捕获文件而起不来，于是已安装的 gcc 被说成"索引里没有"**；
    worker 在两处都能运行，读不了配方时说"读不了"而不是"没有"，报错不再落到模糊候选（§13.2）。
16. **machine 里的 sudo**：root 所有的副本放在 `nosuid` 的 `/run` 上，绑定继承 `nosuid`；改为专用的 tmpfs（§13.1）。
17. **Falkon**：qt-runtime 缺 ICU 数据，渲染进程对空断行器调用崩溃；带上完整 ICU 数据；有 GPU 时不再强制 `--disable-gpu`（§14）。

## 1. 实测与定因

| 编号 | 现象 | 定因 | 依据 |
|---|---|---|---|
| H1 | `luban new desktop-4` 报 `output already exists: <home>/config/subos/desktop-4`，后面跟着 xlings 的 usage | luban 的重名检查（`describe`）只认完整的系统；xlings 遇到已存在的记录目录就拒绝；那条错误落到通用的用法错误提示上。名字从检查到创建之间没有预留，Luban OS 应用的快速开始也按 `desktop-N` 起名 | `luban/os/service/src/service.cpp:531`、`src/core/subos/root/root_cmd.cpp:220`、`src/core/subos/cmd.cpp:1070` |
| H2 | "配置"阶段进度不动，详细信息在滚 | configure 只有开始与结束，没有阶段内进度 | `luban/os/service/src/events.cppm:41-75` |
| H3 | 启动后"检查 ✓"一闪就进入 | machine 启动的第一步 `phase("check")` 是检查宿主，被映射为最后一段；总进度只进不退，开机期间停在约 97–100% | `luban/os/vm/src/host.cpp:225`、`events.cppm:42` |
| H4 | 装额外包、装桌面时进度不动 | `install` 映射为 -1（无段） | `events.cppm:41-49`、`service.cpp:597-628` |
| H5 | Super+Tab 展开应用的窗口要等很久 | `kHeldMs = 1000`，不可设 | `luban/xdesk/compositor/src/compositor/switchers.cpp:32` |
| H6 | 鼠标移到空白处，选中的仍是放大的；放大盖住旁边 | 指针不在窗口上时 `switched_hover` 不改变什么；选中与悬停同一视觉；1.2 倍放大不留位置、无阴影 | `switchers.cpp:34,98-109,356` |
| H7 | 选中窗口后看不出它去了哪 | 关闭切换器后所有窗口同时滑回，其他窗口的变暗立即撤掉 | `switchers.cpp:125-132` |
| H8 | Super+滚轮弹出"最常用的键"；滚不到 1 秒松开 Super 打开启动器 | 滚轮不清 `super_alone`、不取消 `hold_timer` | `input.cpp:81-86,358`、`server.cpp:219` |
| H9 | Super+滚轮只有一种速度 | 每格固定 60 px | `input.cpp:379` |
| H10 | 中键在窗口上不起作用（新标签、粘贴） | 中键在任何地方都被当成平移 | `input.cpp:335` |
| H11 | 设置左上角"设置"像能点的项 | 与页面项同色、同位置 | `luban/xdesk/compositor/src/shell/overlays.cpp:294` |
| H12 | 设置侧栏右边的圆角拼在内容上 | 侧栏四角圆角填充 | `overlays.cpp:292` |
| H13 | 不在指针下的行也高亮；"复制"、搜索框常亮 | 只有一个"键盘选中行"，没有悬停与焦点之分；唯一可选的行永远是选中的样子 | `settings.cpp:394-460`、`overlays.cpp:316-386` |
| H14 | 快捷键页搜索打不进字 | 有输入法时字以 `TextCommit` 到达，只送给启动器与客户端（很可能；实现时在中文输入法下验证） | `input.cpp:412-414` |
| H15 | 窗口间隔不是 4 | 会话配置覆盖为 8、12 | `luban/xdesk/app/share/xdesk/*.ini` |
| H16 | 不搜索时启动器里没有"设置" | 被移除，`xdesk:` 条目不搜索时隐藏 | `desktop.cpp:117`、`shell/shell.cpp:240` |
| H17 | 关于页把 machine 说成沙箱 | 以 `/run/luban/network.env` 判断，它只在宿主有代理时写 | `settings.cpp:139`、`luban/os/vm/src/host.cpp:315-326` |
| H18 | 关于页"no GPU renderer in this build" | 文字写死；xdesk 只建 pixman 渲染器；wlroots 依赖关掉了 GLES；2026-10-20 §13.2 第 5 条没有实现 | `settings.cpp:148`、`server.cpp:50`、`luban/xdesk/compositor/mcpp.toml:30-33` |
| H19 | 平铺里拖标题栏，旁边窗口立刻占了原位，预览在很远处 | 起手时指针在被拖窗口自己身上，判定排除了它 → 新建最后一列 | `luban/xdesk/layout/src/drag.cpp:31` |
| H20 | 拖动中落点难以预料 | 其他窗口已移到预览位置，判定却按移动前的布局 | `drag.cpp:17-48`、`compositor/drag.cpp:120-128` |
| H21 | 画布里不能在窗口上拖动平移 | 没有这样的手势 | `input.cpp:335,365-375` |
| H22 | 从平铺转来的窗口在画布上连在一起 | 平铺 → 画布时各列成为一个平铺组，组随任何一个窗口移动（设计如此） | `layout.cppm`、`input.cpp:348` |
| H23 | Falkon 打开网页闪退 | §14：渲染进程在 ICU 断行器为空时调用 `setText` | dmesg：`QtWebEngineProc … segfault at 0 … libQt6WebEngineCore.so.6[88595f2,…]`；该处反汇编为 `UTEXT_INITIALIZER`、`utext_openUChars_78`、`call *0x40(%r13)`，`r13 == 0` |
| H24 | machine 里 `sudo`：`effective uid is not 0 … nosuid` | sudo 的 root 所有副本放在 `/run/luban/root`，`/run` 是 `MS_NOSUID` 的 tmpfs，绑定继承源挂载的 `nosuid`；没有一步失败，console.log 里没有 sudo 的记录 | `luban/os/vm/src/guest.cpp:389-413,470,510`、`modules/platform/src/isolation/isolation.cpp:543,578`；`/usr/bin/sudo`、`/usr/sbin/sudo` 都链到 `/xlings/data/xpkgs/xim-x-sudo/1.9.17p2/bin/sudo` |
| H25 | machine 里 `gcc` 说"没装在这个 subos（desktop-40）"，`xlings list` 列出 `xim:gcc@16.1.0`；`self doctor` 报用户视图缺 `.xlings.json` | shim 在用户视图里找不到激活的版本，没有落到系统默认（2026-10-20 §4.2：project › 用户视图 › 系统默认）；新用户视图还没有清单就被当成错误。在私有 subos（一份自己的激活记录）里同一个 shim 正常，问题在"只继承、没有自己的激活"的用户视图 | `src/core/xvm/shim.cpp:880-950`；machine 里复现（§13.3） |
| H26 | `xlings install gcc`、`xlings info gcc` 说"匹配多个包"，候选里没有 `xim:gcc`；`xlings info xim:gcc` 说"索引里没有" | **已复现并定因**：索引缓存里有 `xim:gcc`（360 条之一）；解析时 `needs_layer_boundary(provider)` 对**下层已有的包**要求在 Lua worker 里读配方；worker 在私有 subos 里因 bwrap `Can't bind mount /oldroot/etc/ld.so.cache … No such file or directory` 退出（`Lua worker control channel closed`），在 machine 里因 `cannot capture recipe output` 失败；失败的包被当成"没有"，于是落到模糊候选（截到 5 个、顺序不定），并建议"`-y` 取第一个"（会装上不相干的包） | `src/core/xim/lua_boundary.cpp:56-81,440-540,1074`、`modules/confine/src/common.cpp:30`、`src/core/xim/commands.cpp:776-870`；`xlings info xim:gcc -v` 的输出 |
| H27 | `index repo 'local' skipped … metadata boundary` | 同 H26 的 worker 失败；本地索引要整体在 worker 里构建，于是整个跳过 | 同上 |
| H28 | 与电脑之间不能复制粘贴；终端里 Ctrl+C/V、右键都不行 | 剪贴板只在授予时提供，默认不授予；foot 的约定是 Ctrl+Shift+C/V | `apps/luban-os/src/views/display.cpp:282-287` |
| H29 | 新建页两张 xdesk 卡；传统桌面整组展开；档位四格在名字后面 | `kEditions` 两项；分组不可折叠；四格在 `r0.x + 52` | `apps/luban-os/src/core.cppm:361-369`、`views/quick_start.cpp:597-604` |
| H30 | 指针尾巴仍然是歪的 | 两个原因：`default` 第 10–12 行尾巴每行右移一列（45°，与箭头右边平行），注释说"每两行一列"而画的不是；窗口在非整数密度（1.25）下对设备像素图做最近邻缩小，列被吞掉或重复 | `xim-pkgindex/tools/res/cursors/luban.cursors`、`luban/os/window/src/pointer.cpp:184-190` |
| H31 | 系统里没有 GPU 可用 | xdesk 只有 pixman（H18）；machine 的 qemu 是静态构建、`--disable-opengl --disable-virglrenderer`；内核配置没有 DRM 与 virtio-gpu；`xim:mesa` 没有 virgl 驱动；沙箱的桌面会话不授予 `/dev/dri` | `xim-pkgindex/tools/res/build.sh:576-577`、`tools/res/kernel-virt.config`、`pkgs/m/mesa.lua:28` |
| H32 | 有 GPU 也用不上的 Falkon | 启动脚本默认 `--disable-gpu` | `luban:falkon` 的 `libexec/falkon/env` |
| H33 | machine 模式的系统，`luban run` 在宿主内核上执行（`uname -r` 是宿主的） | `run` 一律走 `xlings subos exec`（沙箱），不看系统按什么启动 | `luban/os/cli/src/cli.cpp` 的 `cmd_run`、`luban/os/service/src/service.cpp` 的 `run_command`；KVM 宿主上实测 |
| H34 | 进 machine 时 `/bin/sh: /etc/profile.d/xlings-complete.sh: line 8: syntax error: unexpected "("` | bash 的补全脚本放在 `profile.d`；`BASH_VERSION` 的判断挡不住解析，busybox sh、dash 读到数组语法就报错 | `modules/subos/src/views/root/root_etc.cpp` 的 `complete_sh` |
| H35 | machine 里 `poweroff` 之后，宿主问"kvmt 仍在运行，后台还是关机" | 关机时 shell 先被信号结束，会话以"退出"收场；宿主在 machine 还没停下时就问 | `luban/os/vm/src/host.cpp` 的 `session_` 与 `enter` 的结尾 |
| H36 | 沙箱里 `--mount /dev/kvm` 后打不开 `/dev/kvm` | 用户的挂载一律 `--bind`，bwrap 在新的挂载命名空间里是 `nodev`；设备节点要 `--dev-bind` | `modules/confine/src/confine.cpp` 的挂载一节 |

## 2. 原则

- 布局是纯逻辑（`xdesk.layout`），每个排布与操作都有单元测试；动画由 Spring 完成。
- 工作区视图里不做工作区之间的操作；能看到的只是"在哪一个、还有哪几个"（§3.13）。
- 一个手势在两种视图里意思相同；WuXian 视图只是多出工作区之间的操作。
- 拖动时布局不动，指针下是什么就落到哪里；松手才变。
- 读不了就说读不了：配方读取失败、旧状态丢弃、GPU 用不上，都说出原因，不当成"没有"或悄悄退回。
- 外观选项都在设置里，都能一键恢复；新的字符串都有中英两份；新的动画都服从"减少动态"。
- Luban 的新代码按 §16.2 的分层：xlings 的领域只经协议；通用的部分（WuXian、指针）做成规范与 kit，名字不说 Luban。

## 3. WuXian 上的工作区

### 3.1 模型

```
桌面   = 一张 WuXian（世界坐标，无限）
  ├ 工作区 { id 1..9, 名字, 颜色, 规则: 平铺 | 层叠, 方向(平铺), 位置(世界), 比例 s, 层次 }
  │    └ 窗口（按规则排布，坐标是屏幕逻辑像素）
  ├ 自由窗口 { 位置与大小(世界), 层次 }
  └ 与 WuXian 无关的层：置顶、在所有工作区显示、全屏
镜头   = { 锁定: 工作区 k } | { 自由: 世界点, 缩放 }
视图   = 工作区视图（镜头锁定） | WuXian 视图（镜头自由）
```

- 工作区的内容按"一块屏幕"排布：层叠工作区就是一屏；平铺工作区高一屏、宽为整条列带（展开，至少一屏）。
- 自由窗口只在 WuXian 视图里看得到。窗口属于且只属于一处：某个工作区，或 WuXian。
- **浮动**（Super+Shift+空格）是工作区里的：浮在这个工作区的排布上方，随工作区移动；对话框与弹出窗口自动浮动（同现在）。
  **自由窗口**是 WuXian 上的。两者在设置与文档里用这两个词，不混用。

### 3.2 两种视图

| | 工作区视图 | WuXian 视图 |
|---|---|---|
| 镜头 | 锁在当前工作区，1:1；平铺时沿流向滚动 | 自由平移与缩放 |
| 看到 | 当前工作区的窗口、置顶的、在所有工作区显示的 | 所有工作区（外框与标签）、所有窗口、自由窗口、小地图（可关） |
| 进入 | 默认；WuXian 视图里 Super+O（或焦点不在窗口上时 Esc）回到焦点所在的工作区；双击工作区标签或空底板；选中工作区按 Enter | Super+O、状态栏的 WuXian 按钮、"WuXian 上 N 个窗口"、在切换器里选中一个自由窗口 |
| 过渡 | 从工作区缩小出去、缩进工作区，连续的 Spring 动画（减少动态时直接到位） | |

焦点：WuXian 视图里点了哪个窗口，它所在的工作区就是"当前工作区"；焦点在自由窗口上时回到工作区视图，回到上一个工作区、焦点给它上次的窗口。

### 3.3 工作区的样子

- 底板比 WuXian 背景略亮，**没有阴影**（窗口有阴影：工作区是地面，窗口是放在上面的东西）。
- 边框 1.5 px，颜色是工作区的颜色（按 id 从一组颜色里分配，可改）；当前工作区的边框更亮。
- 左上角在外框之上的标签：颜色点、编号、名字、规则图标——`● 2 · 编码 · ▥`（平铺 ▥，层叠 ❐）；缩得很小时只剩颜色点与编号。
- 拖动窗口经过时，被命中的工作区边框变成强调色、底板淡淡着色，里面显示落点预览（§3.7）。

### 3.4 尺寸与等比缩放

- 工作区的世界尺寸 = 屏幕尺寸 × 比例 s（0.2–2，默认 1）。WuXian 视图里拖标签右下角的手柄等比缩放，或在标签菜单里选 50% / 100%；
  内容不重新排布，只是整体缩放。
- 进入工作区视图永远 1:1：内容就是按屏幕排布的，文字清楚、客户端尺寸不变。
- 屏幕的逻辑尺寸变了（窗口大小就是分辨率）：各工作区按新尺寸重新排布，左上角不动。

### 3.5 重叠与层次

- 工作区可以重叠。工作区连同它的窗口是一层，与自由窗口一起有上下次序；点工作区的标签或它里面的窗口、拖它，就提到最上面。
- WuXian 视图里，命中按层次：指针下最上面的那个。工作区视图不受重叠影响。
- 自由窗口拖进工作区后加入那一层；拖出来的窗口成为最上面的自由窗口。

### 3.6 新窗口去哪

| 情形 | 去处 |
|---|---|
| 工作区视图 | 当前工作区，按它的规则（平铺：焦点右边新的一列；层叠：中间、最上面） |
| WuXian 视图，视野中心在某个工作区里（重叠时取最上面的） | 那个工作区，按它的规则 |
| WuXian 视图，视野中心在空白处 | 自由窗口，视野中心附近的空位（2026-10-20 §9 的螺旋搜索） |

新窗口获得焦点并短暂突出（§4.3）；在视野外时平移露出它，不缩放。

### 3.7 拖放

工作区视图：只在当前工作区里（平铺按 §3.8，层叠按 2026-10-20 §8 与贴边）。不做"拖到屏幕边缘停留去相邻工作区"：平铺的屏幕边缘已是
"新建首 / 尾列"；跨工作区用键盘或 WuXian 视图。

WuXian 视图：

| 指针在 | 松手 |
|---|---|
| 某个工作区里（最上面的那个） | 按它的规则落位：平铺按 §3.8；层叠就在那里（贴边吸附照旧） |
| 空白处 | 自由窗口，世界尺寸 = 它原来的屏幕尺寸（1:1） |
| 别的自由窗口上 | 自由窗口，与它对齐吸附（参考线），不成组 |

- 被拖的窗口保持它在原处的显示比例跟着指针，松手后以缩放动画变到目标的比例。Shift：不吸附。Esc：取消，回原位。
- 缩放 < 0.5 时整个窗口都是把手（单击选中、拖动移动、双击以 100% 居中）。
- 拖工作区的标签：移动整个工作区，与别的工作区、自由窗口的边对齐吸附；可以重叠。

### 3.8 平铺的拖放：冻结布局与占位

替换 2026-10-20 §7.5 的实时重排。

1. 按下标题栏开始拖：布局冻结，所有窗口不动；原位置画占位（虚线框、强调色淡填充）；被拖的窗口半透明跟着指针。
2. 判定在冻结的布局上做：

| 指针在 | 落点 | 预览 |
|---|---|---|
| 自己的占位上 | 不动 | 占位高亮 |
| 另一个窗口沿流向的中间三分之一 | 交换 | 目标与占位都半透明，各有一个指向对方的小箭头 |
| 另一个窗口沿流向的前后三分之一 | 在它前 / 后新建一列 | 两列之间一条插入线，旁边写宽度（滚轮或 1–4 选，沿用 2026-10-20 §7.6） |
| 另一个窗口横向的四分之一 | 叠在它上 / 下（同一列） | 目标半透明，里面画出切分后自己将占的那一半 |
| 列之间的间隙、屏幕边缘 24 px | 新建一列 | 插入线 |

3. 判定区切换有 12 px 迟滞（保留）。
4. 松手：一次 Spring 动画到最终布局，占位收起、旁边的补上。
5. 实现：拖动开始时取排布快照；`drop_at` 在快照上判定，起手在自己格子上为 `Drop::Kind::none`（H19）；拖动中只画占位与预览框，
   `preview_all` 只在松手时用（H20）。

### 3.9 工作区的建立、改名、删除

- **建立**：WuXian 视图空白处右键"在这里新建平铺工作区 / 层叠工作区"（标签左上角在点击处），编号取最小的空号；Super+Ctrl+N 同样取最小空号、
  放在最右边并进入；Super+数字去一个还没有的工作区时，以**这个编号**在最右边新建它。最多 9 个。
- **改名、规则、颜色、方向、比例**：标签上右键或双击名字；Super+M 改规则。名字可以为空（只显示编号）。
- **删除**：标签菜单"删除工作区"，或在 WuXian 视图里选中后按 Delete。
  - 空的：直接删除。
  - 有窗口的：对话框写明"工作区 2「编码」里有 3 个窗口"，三个按钮：**变成自由窗口**（默认，Enter）、**关闭全部窗口**（5 秒倒计时后才可按，
    按钮上显示剩余秒数）、取消（Esc）。
  - 关闭全部：给每个窗口发关闭；10 秒内没有关掉的（要保存的）留在原地成为自由窗口，并说明有几个没关。
  - 编号空出，不重排。最后一个工作区不能删。
- 首次启动有 1 个工作区。由 Super+数字**顺带新建**、没有名字、仍然空着的工作区，离开它时自动删除；明确新建的（右键菜单、Super+Ctrl+N）、
  有名字的、有过窗口的都保留。

### 3.10 与 WuXian 无关的层

- 置顶（Super+P）：在所有之上；WuXian 视图里随视野不动（2026-10-19 §18.2）。
- 在所有工作区显示（Super+Shift+P）：浮在每个工作区视图里；WuXian 视图里画在它原来所在工作区的位置。
- 全屏：工作区视图里铺满整个输出；进入 WuXian 视图时退回它在工作区里的位置，回来时恢复全屏。

### 3.11 鼠标

| 手势 | 工作区视图 | WuXian 视图 |
|---|---|---|
| 拖标题栏，或 Super+左键拖 | 移动窗口（§3.7、§3.8） | 移动窗口，可跨工作区、到 WuXian 上 |
| Super+右键拖、拖边角 | 调整大小（层叠、浮动） | 同左；自由窗口也可以 |
| 空白处左键拖 | 平铺：沿流向滚动 | 平移 |
| 按住 Super+空格时左键拖（任意处） | 平铺：沿流向滚动 | 平移；指针变成手 |
| 中键 | 交给应用 | 空白处拖动平移；窗口上交给应用 |
| 滚轮（空白处） | 平铺：滚动（§4.4） | 平移（Shift：横向） |
| Super+滚轮 | 平铺：滚动（§4.4） | 以指针为中心缩放 |
| 空白处右键 | 桌面菜单（终端、设置、WuXian 视图） | 新建工作区、整理工作区（排成一行）、显示全部 |
| 工作区标签：拖 / 拖手柄 / 右键 / 双击 | — | 移动 / 等比缩放 / 菜单 / 进入 |
| 双击自由窗口（缩小时） | — | 以 100% 居中 |

### 3.12 键盘

保持 2026-10-20 §12 的结构（Super 层；Super+键去那里、Super+Shift+同键带着窗口去、Super+Ctrl 管工作区）。

| 键 | 工作区视图 | WuXian 视图 |
|---|---|---|
| Super+O | 进入 WuXian 视图 | 回到焦点所在的工作区 |
| Esc | — | 焦点不在任何窗口上时（点了空白、选中了工作区标签）同 Super+O；焦点在窗口上时 Esc 交给那个窗口 |
| Super+1..9 | 去第 N 个工作区（没有就新建） | 镜头框住它，仍在 WuXian 视图 |
| Super+Shift+1..9 | 把窗口送到第 N 个工作区 | 同左 |
| Super+Shift+0（新） | 把窗口放到 WuXian 上（自由窗口） | 同左 |
| Super+Ctrl+←/→、PageUp/PageDown | 上 / 下一个工作区（按编号） | 镜头框住上 / 下一个 |
| Super+Ctrl+N（新） | 新建工作区并进入 | 新建，镜头框住它 |
| Super+M | 当前工作区的规则：平铺 ↔ 层叠（选择卡两张，保留预览） | 焦点所在工作区的规则 |
| Super+方向键 | 焦点移动（按规则） | 按空间移到那个方向最近的窗口（跨工作区与自由窗口），镜头跟着 |
| Super+Shift+方向键 | 移动窗口（按规则） | 工作区里的按规则；自由窗口每次移动 1/8 屏 |
| Super+= / Super+- | 平铺：列宽；层叠：大小 | 缩放 |
| Super+0 | 平铺：等宽 | 以 100% 显示焦点窗口 |
| Super+Ctrl+0（新） | — | 显示全部（框住所有东西） |
| 按住 Super+空格 | 手形：左键拖动滚动 | 手形：左键拖动平移 |
| Enter / Delete（WuXian 视图里选中工作区时） | — | 进入它 / 删除它（§3.9） |
| Super+C / Super+V（新） | 复制 / 粘贴（§9） | 同左 |

- 原来的"画布"预设从 `arrangements` 的循环里拿掉；`overview` 动作改名为"WuXian 视图"。
- 数字键按位置识别（evdev 2–11，含 0），不受 Shift 改变字符的影响（沿用 `shortcut()` 的做法，加上 0）。
- 按下 Super+空格清 `super_alone`（松开不打开启动器）。
- Super+空格可能被宿主拦（GNOME 用它切换输入法）：快捷键页写明、可改；Luban OS 窗口锁定键盘时能收到。
- `keys.json` 里旧的 `arrangements`、`overview` 设定照常读入，含义按上表。

### 3.13 状态栏：显示工作区指示

习惯上两类做法：平铺类桌面（i3、sway、Hyprland、niri 配 waybar）总是列出工作区编号，键盘用户靠它知道"还有哪几个、我在哪";
GNOME 45 起在左上角放一排小圆点、当前的拉长成药丸；Windows 与 macOS 不在任务栏列出（Task View、Mission Control 里看）。
Luban 是键盘优先、有 Super+1..9 的桌面，不显示就要靠记忆，所以显示；但只是**指示**，不在这里做工作区之间的操作（那是 WuXian 视图的）：

- **两个通道各说一件事**：颜色是工作区的身份（它自己的颜色，与 WuXian 视图里的边框、标签同色）；**形状是它的规则**——平铺是分成竖栏的
  圆角方块（▥，纵向流时栏是横的），层叠是错开叠放的两块（❐）。规则不靠颜色区分：颜色已经用来认工作区，而且色弱的人分不出。
- 左：当前工作区一块——规则形状（填它的颜色）、编号、名字（点它打开 Super+M 的选择卡）；其后是其他已有工作区的小图形（同样的规则形状、
  它们的颜色，里面是编号，不写名字），点击切换（滑动过渡），悬停提示名字、规则与窗口数。没有其他工作区时只有当前那块。
- 规则变了（Super+M）：图形以一次形变过渡到新形状。
- WuXian 视图里工作区标签上的规则图标与这里是同一组图形（§3.3）。
- 右：WuXian 按钮（WuXian 视图时为"返回"）；WuXian 上有自由窗口时显示"WuXian 上 N 个窗口"，点它进入 WuXian 视图并框住它们。
- WuXian 视图：缩放比例、显示全部、小地图开关。
- 设置"状态栏显示其他工作区"，默认开。

### 3.14 状态的保存

布局状态存进 `wuxian.json`（WuXian 规范的状态格式 v1，§15.4；与 `state.json` 同一目录，`xdesk.config` 给出路径）：工作区（id、名字、颜色、规则、方向、位置、比例、层次）、镜头与视图、
各窗口所属与位置（按 app id 与标题恢复，同现在）。`state.json` 只留 xdesk 自己的其他状态。读到旧的布局形状：改名保存为
`state.json.before-<日期>`（不删除），用默认（一个空工作区），日志与第一次的提示各说一行。不做迁移（内部研发阶段）。

### 3.15 `xdesk.layout` 的接口（要点）

```cpp
using openwx::Rule;   // tile | stack
using openwx::View;   // workspace | wuxian
// 工作区、自由窗口、镜头、视图的类型是规范模块的（openwx::Workspace 等，§15.6），这里不再定义一份。

class Layout {
    int add_workspace(Rule, std::optional<int> id = {}, std::optional<Point> world_at = {});
    void remove_workspace(int id);                 // 先把窗口移走（to_wuxian）或由 compositor 关闭，空了才删
    void rename(int id, std::string); void set_rule(int id, Rule); void set_color(int id, std::uint32_t);
    void set_scale(int id, double); void move_workspace(int id, Point world); void raise(int id);
    View view() const; void set_view(View); int current() const; void show(int id);
    void add(Id, Size preferred);                  // 按 §3.6
    void send_to(Id, int id); void to_wuxian(Id);
    DragSession begin_drag(Id) const;              // 排布快照
    Drop drop_at(const DragSession&, double sx, double sy, bool shift) const;   // 目标：工作区 + 规则内的落点，或 WuXian
    void drop(const DragSession&, const Drop&);
    std::vector<Placement> arrange(Size output, Insets reserved);
    Hit at(double sx, double sy) const;            // 窗口 | 工作区标签 | 工作区底板 | 空白，按层次
};
```

`Mode` 三选一与 `Space::groups` 去掉；`tile.cpp`、`stack.cpp` 按工作区复用；`canvas.cpp` 改名 `wuxian.cpp`，是镜头、世界坐标与工作区的变换。
数据类型（工作区、自由窗口、镜头、视图、命中次序）与状态格式来自规范模块 `openwx`（§15）；`xdesk.layout` 只加排布规则。

### 3.16 命名：WuXian

无限画布的名字是 **WuXian**（来自"无限"），是专有名词，各处一致：

| 地方 | 写法 |
|---|---|
| 界面（中英） | "WuXian"；第一次出现与设置里写"WuXian（无限画布）"/"WuXian (the infinite canvas)"；视图叫"WuXian 视图"/"WuXian view" |
| 代码 | 规范的模块与命名空间 `openwx`（open WuXian，与 opensal、openhal 同一种命名）、`xdesk::layout` 里的 `View::wuxian`、`to_wuxian()`、文件 `wuxian.cpp`；类型名 `WuXian…` 只用于专指它的类型 |
| 配置 | 个人设置的节 `[wuxian]`（读入旧的 `[canvas]`）；`keys.json` 的动作 `wuxian`（读入旧的 `overview`）；状态文件 `wuxian.json`（§15.4） |
| 文档 | "WuXian"，规范文档 `luban/specs/openwx/SPEC.md` |

规范叫 **openwx**（open WuXian）：它是关于 WuXian 的规范，名字与其他规范一致；WuXian 是这张平面本身的名字，用在界面、文档与实现里（xdesk 的 WuXian 视图、`View::wuxian`）。状态文件仍叫 `wuxian.json`，版本键 `"wuxian"`：它记的是一张 WuXian。

"画布"只在说历史（旧的画布预设）时出现；代码里的 `canvas` 标识符本轮全部改为 `wuxian`。

## 4. 切换器、突出与滚轮

### 4.1 Super+Tab 的等待

`kHeldMs` 改为设置项"停留多久展开应用的窗口"：250 / 500（默认）/ 750 / 1000 ms。

### 4.2 选中与悬停

- 选中：强调色 3 px 描边 + 阴影；松开 Super 选它；指针离开所有窗口时保持。
- 悬停：指针下的窗口浮起 1.04 倍（Spring），离开即收；悬停也改变选中（同现在）。
- 不再放大 1.2 倍；`layout::spread` 按 1.04 留出间距。未选中的暗到 0.6（保留）。
- 范围：工作区视图里是当前工作区的窗口；WuXian 视图里是全部（含自由窗口）。

### 4.3 选中后突出

选中后其他窗口保持暗着，直到所有窗口的 Spring 停下，再停"突出时长"，然后 300 ms 淡回。设置：突出时长 关 / 0.5 s（默认）/ 1 s；
暗度 0.3 / 0.5（默认）/ 0.7。新窗口出现、WuXian 视图里 Super+方向键跳到别处也用它。透明度逐帧变化：`View` 加一个透明度 Spring。

### 4.4 滚轮

- 滚轮、按键、指针移动超过 4 px 都清 `super_alone` 并取消 `hold_timer`（H8）。
- 速度曲线（纯函数，可测）：与上一格间隔 ≥ 150 ms 为"慢"，平铺里每格一列并对齐到列边；更快时步长 = 60 px × min(6, 1 + 速度 / 基准)，
  目标交给 Spring（惯性），停下吸附到最近的列边。触控板的连续滚动按原量、不吸附。
- 设置：滚动速度 慢 / 中（默认）/ 快；对齐到列 开（默认）/ 关。WuXian 视图的平移用同一条曲线，不吸附。

### 4.5 中键

只在空白处（或按住 Super 时）平移；在窗口上交给应用（H10）。

## 5. 设置

### 5.1 外观

- 左上角是应用标题：设置图标 + "设置"，加粗、大一号，不可点击的样子。
- 侧栏与内容区同底色，中间 1 px 分隔线；页面项选中时是圆角高亮块，四周留边距，不碰卡片边。

### 5.2 三种状态

| 状态 | 什么时候 | 样子 |
|---|---|---|
| 选中的值 | 选项的当前值（芯片、开关） | 强调色填充，常驻 |
| 悬停 | 指针在一行、一个按钮、一个芯片上 | 浅色底，离开即消失 |
| 键盘焦点 | 最近一次输入是键盘 | 强调色 2 px 焦点环；一用指针就隐藏 |

- 按钮平时是普通按钮，悬停变浅、按下变深，不因为是唯一可选的行而常亮（H13）。
- 搜索框在快捷键页一直接收输入：显示闪烁的光标；只有键盘焦点在它上时才有焦点环。点击一行只执行它，不把它变成"选中行"。

### 5.3 搜索与输入法

设置打开时，`TextCommit` 在快捷键页送进搜索（与启动器同一个处理），其他页忽略；`Preedit` 画在搜索框里（下划线）。

### 5.4 新的设置项与默认值

| 页 | 项 | 默认 |
|---|---|---|
| 布局 | 窗口间隔 | **4**（会话配置一起改，§12.2 合并后只剩一份） |
| 布局 | 新建工作区的规则；状态栏显示其他工作区（§3.13） | 平铺；开 |
| 布局 | 小地图、吸附（去掉"成组"） | 开 |
| 切换 | 停留多久展开（§4.1）、突出时长与暗度（§4.3） | 500 ms、0.5 s、0.5 |
| 切换 | 滚动速度、对齐到列（§4.4） | 中、开 |
| 外观 | 减少动态 | 关 |
| 外观 | 渲染器：自动 / 软件（§7.2） | 自动 |

### 5.5 恢复默认

- 每页底部"恢复本页默认"：删掉个人配置文件里这一页的键，回到会话配置的值，立即生效；提示"已恢复 · 撤销"（10 秒内可撤销）。
- 关于页"全部恢复默认"：确认对话框列出会恢复的页与快捷键，确定后同上，并清掉 `keys.json` 里的改动。

### 5.6 启动器里的"设置"

不搜索时"设置"作为一个应用与终端、浏览器一起列出；各页与动作仍只在搜索时出现。

## 6. 关于页

- 运行方式：`/proc/cmdline` 含 `luban.name=` 为 machine（"一台机器：自己的内核"），否则为沙箱（"这台电脑的内核"）。
- 渲染器：实际用的与原因（§7.4），如"GPU（GLES 3.2 · Mesa iris · Intel UHD 770）"、"GPU（virgl · 宿主 NVIDIA）"、"软件（pixman）：
  这个系统没有可用的 GPU（machine 的显示器不是 3D 的：宿主的 qemu 不带 GL）"。
- "复制"带上渲染器那一行。

## 7. GPU 渲染

### 7.1 目标与范围

- xdesk 在有 GPU 时用 GPU 合成：窗口内容是纹理，缩放（WuXian、切换器）、透明度、圆角、阴影在 GPU 上；GL 应用（Falkon、Qt 程序）经 linux-dmabuf
  直接交出 GPU 上的缓冲区，不再读回到内存。
- 没有 GPU 或失败时退回 pixman，画面不中断，原因写进关于页与日志。
- 覆盖：沙箱（宿主的 GPU：Intel、AMD、NVIDIA）与 machine（virtio-gpu 3D，宿主是 Linux 时）。macOS（Virtualization.framework）、
  Windows 宿主的 machine 本轮只说明"软件渲染"。
- 传统桌面（labwc、LXQt、KDE）拿到同样的 `gpu` 授权与 Mesa 时，labwc 自己会用 GLES；它们的帧仍经 opensal-wlr 的截屏交给窗口，本轮不改那条路。
- 不在本轮：帧经 dmabuf 零拷贝交给宿主窗口（宿主窗口跨 X11 / Wayland / macOS / Windows 的 GL 互通要单独设计）；Venus（Vulkan）。

### 7.2 选择渲染器

1. 设置"渲染器"为软件、或环境变量 `XDESK_RENDERER=pixman`：用 pixman。
2. 否则按顺序找 render node（`/dev/dri/renderD128` 起）：打开它，GBM 建设备，EGL 初始化（glvnd → Mesa，或 NVIDIA 经 interposer），
   GLES 2 上下文（wlroots 的渲染器要求），画一帧 64×64 并读回校验。都成功用 GLES（wlroots 的 `wlr_gles2_renderer_create_with_drm_fd`，GBM 分配器）。
3. 任何一步失败：pixman，记下失败在哪一步与错误。
4. 运行中 GPU 复位、上下文丢失：重建为 pixman，所有窗口重画一次，提示一行"GPU 不可用了，已改为软件渲染"。

### 7.3 帧交给窗口

- 合成到 GBM 缓冲区后，读回被改动的矩形（`wlr_texture_read_pixels`）。
- **同一台电脑（沙箱）**：帧缓冲区在共享内存里（memfd，连接建立时经 socket 以 `SCM_RIGHTS` 传一次；尺寸变化时重传），socket 只发
  "哪些矩形变了"（新的 opensal.wire 消息 `'m'` 共享内存、`'r'` 变化的矩形；不认识的一方仍收原来的 `'f'`）。读回直接写进共享内存。
  memfd 加 `F_SEAL_SHRINK`（对方缩小缓冲区不会让读的一方 SIGBUS），尺寸变化时换一块新的。
  这同时实现 2026-10-20 §13.2 第 3 条，pixman 也走它。连接中间有转发进程时，它原样转交 fd（`SCM_RIGHTS`）；转交不了就协商回 `'f'`。
- **machine**：仍经 socket 发像素（`'f'`），加按行差分压缩（§13.2 第 3 条后半）。
- **实施时的修正（本轮不做共享内存）**：窗口与系统之间的连接是 stdio 管道（中间有 bwrap 与转发进程），管道传不了 fd；共享内存需要宿主与系统之间
  另开可传 fd 的 socket 或共享目录，牵动 opensal、窗口、xdesk 与沙箱挂载四处。它是性能优化，不影响 GPU 合成本身，移到以后（§21）；本轮 GPU
  下只读回变化的矩形，经原来的 `'f'` 发出。
- 窗口端：从共享内存更新 SDL 纹理（只更新变化的矩形），节流照旧（paced）。每个显示器窗口（镜像）各有自己的共享缓冲区。

### 7.4 沙箱：设备与驱动

- 有桌面的系统，桌面会话默认得到 `gpu` 授权：只绑 render node（`/dev/dri/renderD*`）与 NVIDIA 的设备节点（`xlings.subos.gpu` 已有），
  不绑 `card*`（不做显示模式设置）。系统设置里可关；Agent 用的系统、没有桌面的系统默认不授予。
- 驱动来自系统里的包：桌面 edition 带 `xim:graphics`（glvnd + `xim:mesa` + NVIDIA interposer，见 `graphics-stack-spec.md`），宿主只提供内核驱动
  与 NVIDIA 用户态（T0）。体积：Mesa 与 LLVM 约 +150 MB，写进新建页的大小。
- xdesk 的进程按 `graphics-stack-spec.md` §5 的加载器契约（DT_RPATH）找到 glvnd 与 vendor。
- 打开 GLES 后 xdesk 链接 libEGL、libGLESv2、libgbm、libdrm：没有它们 xdesk 起不来，即使最后用 pixman。所以 `luban:xdesk` 自己依赖
  `xim:graphics`（不只靠 edition 带上）；xdesk 启动时这些库缺失的报错写明缺什么、`xlings install` 哪个包。

### 7.5 machine：virtio-gpu

- 宿主：`xim:qemu-system` 增加 GL 版本 `xim:qemu-system-gl`（动态链接，`--enable-opengl --enable-virglrenderer`，`-display egl-headless`，
  经宿主的 render node 与 `xim:graphics` 渲染）；machine 启动时宿主有 render node 且有这个包就用 `-device virtio-gpu-gl-pci` 与
  `egl-headless`，否则用静态版本、没有 3D。第一次需要时与 qemu 一样按 T1 安装（`luban start` 说明）。
- 内核：`kernel-virt.config` 加 `CONFIG_DRM`、`CONFIG_DRM_VIRTIO_GPU`（模块或内建），新版本 `xim:linux-kernel-virt`。
- 系统里：`xim:mesa` 增加 gallium `virgl` 驱动（新 revision）。
- 选择同 §7.2：系统里的 render node 是 virtio-gpu 的。
- 关于页写"GPU（virgl · 宿主的 GPU 名）"；宿主没有 GPU、或 macOS / Windows：写原因。
- **实施（2026.10.21.3）**：
  - `xim:qemu-system-gl` 9.2.4-r1（`tools/res/build.sh qemu-system-gl`，Debian 12 上动态链接，virglrenderer 1.1.1 放在包的 `lib/`，
    只有 x86_64：`xim:mesa` 只有 x86_64）；依赖 `xim:graphics` 与 `xim:nvidia-gl-host-link@>=0.1.4`。
  - 是否用 virgl 跟沙箱同一个开关：系统有 `gpu` 授权（桌面默认有）。第一次需要时 luban 经 Port 的 `install_packages` 安装它（T1）。
  - qemu 要找到图形栈（EGL vendor、GBM 后端、DRI 驱动）：xlings 加 Port 能力 `subos_env`（协议 1.16），返回 SubOS 声明的变量，
    luban 存进机器状态（`gpu_env`），supervisor 启动 qemu 时按 `subos use` 的规则合并。
  - NVIDIA：egl-headless 经 GBM 打开 render node，libgbm 按驱动名找 `nvidia-drm_gbm.so`；`nvidia-gl-host-link` 0.1.4 把宿主的
    GBM 后端（libnvidia-allocator）链进 SubOS 的 `usr/lib/gbm`，与 Mesa 的 `dri_gbm.so` 并列。
  - GL 版 qemu 在 render node 上起不来（驱动拒绝）时，supervisor 在 agent 连上之前看到它退出，改用静态 qemu、`-display none`
    重新启动同一台机器，状态写原因；不会因为 GPU 让机器起不来。
  - 系统里：`xim:mesa` 25.0.7.3 加 virgl（gfxbuild subos 里按 `tiers.sh` T5 重编，与 25.0.7.2 只差 `virtio_gpu_dri.so`），
    `xim:xdesk` 依赖 `xim:mesa@>=25.0.7.3`；机器的 init 把 `/dev/dri/renderD*` 设成 0666（没有 udev / logind 交给座席）。
  - 显示：`luban status <名字>` 的 `gpu` 行、进入时的欢迎行（`GPU (virgl)`）、xdesk 关于页"GPU（GLES · virgl, the host's GPU）"。

### 7.6 应用

- `luban:falkon` 的启动脚本：有可用的 render node 时不加 `--disable-gpu`（H32）；没有时照旧。
- Qt、GTK 程序经 Mesa 自动使用 GPU；foot 不用 GL，不受影响。

### 7.7 测试与验收

- CI（无 GPU）：选择逻辑的每一步失败都退回 pixman 且原因正确（注入失败）；共享内存的帧协议（编码、解码、尺寸变化、旧消息兼容）；
  渲染结果与 pixman 的像素对比（Mesa llvmpipe 经 vgem 的 render node，CI 机器有 `vgem` 时；没有则跳过并说明）。
- 本地验收矩阵：Intel（iris）、AMD（radeonsi）、NVIDIA（interposer）沙箱；Linux 宿主 machine（virgl）；macOS、Windows machine 退回软件并写明原因。
- 预算（与 2026-10-20 §13.3 同一组指标）：Super+O 连按 10 次 CPU、静止 0 帧、WuXian 缩放时帧率；GPU 下 CPU 不高于 pixman 的一半。

## 8. 矢量指针与晃动找指针

### 8.1 一个源、一个光栅化器

- 放弃 12×12 像素格。指针的源是矢量形状（每个形状一组闭合路径：直线与二次曲线，单位是 24 px 的逻辑格，加热点），通用的部分是新的 kit
  `luban/kits/cursor`（名字不说 Luban，只依赖标准库）：路径格式、光栅化（8×8 超采样抗锯齿，直通 alpha 的 ARGB）、任意尺寸的图像与热点、
  晃动检测（§8.3）。Luban 的指针主题是数据，放在 Luban OS 自己那里：`luban/os/window/cursors/*.path`。
- 只有这一个光栅化器：Luban OS 窗口运行时用它（任意尺寸，§8.3 的放大要用）；它的命令行工具 `cursor-make` 生成 Xcursor 主题
  （24、30、36、48、60、72 px，即密度 1、1.25、1.5、2、2.5、3）。主题由本仓库的发布脚本用它生成，作为 `luban:cursors` 的资源发布
  （与 xdesk 同一条发布流程）；源从 xim-pkgindex 移到这个 kit，`tools/res/cursors`（`luban.cursors`、`make.py`）删除。
- 形状按常见比例重画：箭头左边竖直、右边约 45°，尾巴从箭头底边中部伸出、与竖直约 27°（约 2 行偏 1 列），深色描边、白色填充；保留 Luban
  的一处强调色（尾巴末端）。15 个形状全部重画。
- 路径的画法（K8 实施时定下）：`body` 先以描边色画整个形状、再以填充色画内缩一个描边宽度的形状，所以一个形状只写一次、描边处处等宽，
  放大并变色时描边加粗只是改宽度；`accent` 只画在上一个 body 的填充里；`outline`、`fill` 原样一种颜色（问号、手指的分隔）。
  主题文件包在 C++ 原始字符串里（首尾两行解析器跳过），窗口直接 `#include` 带上它，不再从磁盘找主题。
- 窗口（`luban/os/window/src/pointer.cpp`）按实际密度向 kit 要正好那个尺寸，不再重采样；客户端自带的指针图像（`'C'`）在非整数密度下
  用面积平均缩小，不用最近邻。
- 新版本 `luban:cursors`（新的资源）；`luban:xdesk`、`luban:labwc` 随之更新。

### 8.2 验收

1、1.25、1.5、2 密度下截图对比；测试：每个尺寸都生成、热点按比例、尾巴的斜率（按像素统计）在 25°–30° 之间、同一形状不同尺寸的
覆盖率一致（光栅化正确）。

### 8.3 晃动找指针

- **检测**（纯函数，可测，在 `cursor` kit 里）：最近 0.6 s 的指针移动里，水平或竖直方向反转至少 4 次，平均速度超过 1800 逻辑 px/s，
  路径长度超过 700 逻辑 px，视为"在找指针"。
- **反应**：指针以 Spring 放大，持续晃动时继续放大到最大 4 倍（可设 2 / 3 / 4 倍）；放大时填充变为强调色、描边加粗（可设：只放大、
  放大并变色、关）；停止晃动 0.3 s 后以 0.4 s 缩回原样。放大期间热点按比例，指向的位置不变。
- **在哪里做**：谁画指针谁做。Luban OS 窗口画宿主上的指针（SDL 光标）：它最先看到真实的移动，没有延迟；指针图像每变化一档（0.25 倍）
  向 kit 要一次新尺寸、建一次 SDL 光标。xdesk 在不提供指针接口的输出上自己画指针时，用同一个 kit 的检测与图像。
- **设置**：在 xdesk 设置的"外观"页（"晃动鼠标时放大指针"：关 / 放大 / 放大并变色，默认放大并变色；最大倍数）。xdesk 经 opensal
  告诉窗口：`Cursor` 接口加一个偏好（新的 wire 消息 `'p'`：`locate <off|grow|grow-color> <max> <rgb>`；不认识的一方忽略，
  opensal 小版本加一）。窗口没收到偏好时用默认。
- 不触发：按着鼠标键拖动时、指针被锁定（游戏、`PointerLock`）时、指针隐藏时。
- 宿主对光标尺寸有上限（部分 X11 服务器、Windows 只接受较小的光标）：建大光标失败时，窗口隐藏系统光标，把放大的指针作为一层图画在窗口里，
  缩回后恢复系统光标。
- 减少动态打开时：直接放大到目标大小、不做 Spring。

## 9. 复制粘贴

- 与电脑之间：从窗口新建的、有桌面的系统，默认授予 `clipboard` 与 `clipboard-paste`（新建时说明一行；系统设置里可关）。已有的系统不变，
  "复制"失败时的提示给出打开的方法（保留）。Agent 用的系统、没有桌面的系统默认不授予（系统里的任何程序都能读到桌面的剪贴板）。
- 全局 Super+C / Super+V（新动作，可改）：焦点窗口的 app id 是终端（`foot`、`org.codeberg.dnkl.foot`、`kitty`、`Alacritty`、
  `org.wezfurlong.wezterm`、`qterminal`、`konsole`，以及 `keys.json` 的 `terminals`）时发 Ctrl+Shift+C/V，否则发 Ctrl+C/V
  （合成按键经 `wlr_seat_keyboard_notify_key`，带上对应的修饰键状态，之后恢复）。
- 终端里 Ctrl+C 仍是中断；快捷键页与欢迎提示写明 Super+C/V 与中键粘贴（§4.5）。

## 10. 进度：双进度条与阶段

### 10.1 阶段

| 阶段 | 包含的 phase | 有的系统 | 本阶段的度量 |
|---|---|---|---|
| 获取 | prepare、plan、resolve、download | 全部 | 字节 |
| 解包 | extract、unpack | 全部 | 文件数（xlings 给出时），否则不定 |
| 配置 | config*、hook、install（额外包、桌面）、用户、设置 | 全部 | 包 i/N（有钩子的包与额外包） |
| 启动 | preflight（原 vm 的 `check`）、vmm、kernel、boot、agent、会话 | machine | 子步骤 i/N |
| 自检 | verify（原建好后的 `check`） | 有自检的 | 检查项 i/N |

- 两个 `check` 改名：`preflight`（属于启动）与 `verify`；旧名照常读入：`check` 出现在 `boot` 之前按 `preflight`（H3）。
- 去掉"准备"（并入获取）与"完成"两格；只显示这个系统有的阶段。

### 10.2 两条进度

```
总进度  ███████████░░░░░░░░  58%   获取 ✓ · 解包 ✓ · 配置 ● · 启动 · 自检
本阶段  ██████░░░░░░░░░░░░  3/7 个包 · fontconfig 的钩子 · 已用 42 s
        └ Running fc-cache…                                   2 s 前
```

- 总进度只进不退。各阶段权重先按预估（获取 55、解包 15、配置 15、启动 10、自检 5），知道总字节与包数后按实际调整；调整时显示值取
  `max(原值, 新值)`，不回退。
- 本阶段：有度量写数字与进度；没有度量画不定进度动画并写已用时间。最近一行日志与"几秒前"始终显示。
- 服务给出 `overall`、`stage`、`stage_percent`、`stage_detail`（`Event` 新字段）；命令行与应用同一份计算。命令行两行（按终端宽度截断，
  沿用 G1 的处理）；`--json` 多出这几个字段。
- 成功：停 0.7 s 显示"就绪 ✓"再进入；整个操作不到 2 s 的直接进入。

## 11. `luban new` 的名字

- 检查：完整的系统 → "已存在"（保留）；只有记录（`<home>/config/subos/<名字>`）、没有根目录、没有注册 → "上次没建完的 `<名字>` 留下了记录"，
  提示 `luban rm <名字>`（`rm` 能清只有记录的残留：记录是派生的，不是用户数据）。
- 名字原子预留：开始时在 `<home>/run/luban/names/<名字>` 以独占方式建文件（写入进程号与开始时间），结束或失败时删除；撞上时说
  "`<名字>` 正在被创建"；进程已不在的预留视为过期、直接接管。Luban OS 应用自动起名时跳过已预留的。
- xlings 返回的错误由 luban 用自己的话说，不透出 `usage: xlings subos …`；xlings 那边内部错误不再落到用法错误的提示。

## 12. 新建页（Luban OS 应用）

### 12.1 卡片

- xdesk 一张卡"Luban 桌面"（xdesk：工作区与 WuXian），去掉"xdesk 画布 · 探索"（`kEditions` 的 `desktop-xdesk`）。
- 传统桌面一组默认折叠："传统桌面 · N 个 ▸"，点开或按 → 展开；键盘 ↑↓ 经过折叠组时停在标题上。
- 档位四格画在图标右上角作为角标（半透明圆角底），所有有档位的卡一致。
- 大小写实际（含 §7.4 的 Mesa）。

### 12.2 会话与配置

- 会话只剩 `xdesk`；`xdesk-classic`、`xdesk-canvas` 的会话文件保留为别名（已有的系统照常启动），都按新模型。
- 会话配置合并为 `xdesk.ini`；`classic.ini`、`canvas.ini` 删除（别名会话读 `xdesk.ini`）。

## 13. 系统里的 sudo 与 xlings

### 13.1 sudo（H24）

`root_owned_sudo_()` 先在 `/run/luban/root` 挂专用的 tmpfs：`MS_NODEV`，**不加** `MS_NOSUID`，mode 0755（`platform` 加一个带挂载参数的
`mount_tmpfs`）；副本放在那里再绑定覆盖 sudo 的真实程序（`fs::canonical`），`/usr/bin/sudo`、`/usr/sbin/sudo` 等链接都看到 root 所有的副本。
任何一步失败写到 console，并在 `luban enter` 的欢迎信息里说一行"这个系统的 sudo 不可用：…"。

### 13.2 Lua worker 与名字解析（H26、H27）

1. **worker 在系统里能运行**：
   - 沙箱 / 私有 subos：`host_userland` 只在目标能建挂载点时绑定 `/etc` 下的文件（`ld.so.cache`、`resolv.conf` 等）；worker 的 `/etc` 用它自己的
     身份目录（已有的 `passwd`、`group`、`hosts`），需要的文件先在那里建好占位再绑定。
   - machine：worker 的私有目录与输出捕获放在 machine 自己的 tmpfs（`/run`），不放在 9p 上；围栏（Landlock 或 bwrap）的可写路径随之调整。
2. **读不了不是没有**：配方读不了时，解析结果是"`xim:gcc` 的配方读不了：<原因>（`xlings self doctor`）"，不是"索引里没有"；
   本地索引读不了时同样说原因，并指出受影响的是哪个索引。
3. **报错**：精确名字存在但不可用时直接说原因（没有这个平台的构建，或配方读不了）；模糊候选按接近程度排序（完全包含 › 前缀 › 其他）、稳定，
   超过 10 个写"还有 N 个，`xlings search <名字>`"；没人可问时不建议"`-y` 取第一个"，只建议完整坐标。
4. 测试：私有 subos 与 machine 里 `xlings info xim:gcc`、`xlings install`（需要执行配方的包）成功；worker 起不来时报"读不了"。

### 13.3 shim 与用户视图（H25）

- 复现：machine 里新用户视图（没有自己的激活记录）运行系统默认里有的程序（`gcc --version`）。
- 修：shim 与 `xlings info/use` 在用户视图里没有激活时落到系统默认；用户视图的清单在第一次需要时写出，缺失不算错误（派生状态，`self doctor` 不报）。
- 一并看：`self doctor` 在系统里报的 `index copy`（`/xlings/data/scode`）若是系统层的正常形态则不报。

### 13.4 machine 的实测（KVM，H33–H36）

在有 `/dev/kvm` 的宿主上，用隔离的 home 新建 `luban:tiny` 的 machine：4 秒起来，内核 `6.12.112-virt`，dmesg 有 `Hypervisor detected: KVM`。由此改：

- **`luban run` 进 machine**（H33）：系统按 machine 启动、或正以 machine 运行时，命令在 machine 里执行（`vm::run`：有终端时在 pty 上，
  否则管道，stdout 与 stderr 分开，退出码原样）；本来没开的 machine 为它启动、执行完关掉；Windows 照旧（经 carrier 另行设计）。
- **补全脚本**（H34）：bash 的语法作为文本读入、只由 bash `eval`，任何 sh 都能解析这个文件；2026.10.20.1–.3 写下且没改过的那份
  在下一次换 generation 时替换，改过的保留（机器状态）。
- **自己关机**（H35）：guest 报告 shell 是被信号结束的（`X` 帧带 `signal`）；宿主在这种情况与连接断开时先等 machine 停下（至多 10 秒），
  停了就说"已关机"，不再问。旧的 guest 不带这个字段，行为不变。
- **设备的挂载**（H36）：挂载源是字符或块设备时用 `--dev-bind`（`Intent::device`，与 `exists` 同一个可注入的接缝）。

## 14. Falkon（H23、H32）

- 定因：`luban:qt-runtime` 的 `libicudata.so.78` 是 Alpine 拆包后的空壳（9 KB），数据在 `share/icu/78.1/icudt78l.dat`，qt-runtime 与 falkon
  都没带（`luban:kde` 带了）。Blink 创建断行器得到空指针，`setText` 时崩溃；渲染进程一个接一个崩，Falkon 退出。
- 修（xim-pkgindex）：`luban:falkon` 26.04.2-r1 带上 `icu-data-full` 的 `icudt78l.dat`（中文的断词与排序要完整数据），放在
  `libexec/falkon/share/icu/78.1/`，`libexec/falkon/env` 设 `ICU_DATA`。实施时的修正：不放进 qt-runtime——升 qt-runtime 的版本要连带重建
  LXQt 与 KDE（它们按 runtime 的版本构建），而需要完整数据的是 Blink 的断行器。
- 启动脚本：有可用的 render node 时不加 `--disable-gpu`（§7.6）；没有 `WAYLAND_DISPLAY` 与 `DISPLAY` 时说"在桌面里打开它"，不落到 Qt 的 xcb 报错。
- 验证：machine 与沙箱里打开中文页面、搜索结果页；`dmesg` 无 `QtWebEngineProc` 崩溃。

## 15. WuXian 规范

### 15.1 为什么是规范、规范什么

WuXian 要成为开放桌面的一种布局：别的合成器（以及以后别的平台上的 xdesk）可以实现同一个模型、读写同一份状态、给出一致的基本交互，
面板与工具不必关心是谁实现的。规范定义**模型、视图与镜头、行为的不变量、状态格式、基本交互与一致性测试**；不定义平铺与层叠内部的排布算法
（列宽、吸附细节是实现的事），也不定义绘制风格（颜色、阴影是建议）。

位置：`luban/specs/openwx`（`README.md`、`SPEC.md`、`src/`、`conformance/`），与 opensal 等同为规范：只依赖标准库与它自己，名字不说 Luban，
文档不指向本仓库，版本自己走；有第二个实现者或外部使用者时整体搬走（`luban/README.md` 的规则）。

### 15.2 模型（规范性）

- **平面**：无限的世界坐标（逻辑像素，双精度）。
- **工作区**：`id`（正整数，唯一；实现可以设上限，键盘可达的是 1–9，xdesk 上限 9）、`name`（可空）、`color`（sRGB）、`rule`（`tile` | `stack`）、`flow`（`horizontal` | `vertical`，仅 tile）、
  `origin`（世界坐标的左上角）、`scale`（0.2–2）、`z`（层次）。世界尺寸 = 内容尺寸 × `scale`；内容尺寸由输出的逻辑尺寸与规则决定
  （stack：一屏；tile：高一屏、宽为列带，至少一屏）。
- **窗口**：属于且只属于一处——某个工作区（`workspace: id`，坐标是工作区内容坐标），或平面（`free`，世界坐标的矩形与 `z`）。
  `floating`（工作区内浮动）、`pinned`、`sticky`、`fullscreen` 是窗口的属性。
- **镜头**：`locked(id)`（工作区视图：工作区内容以 1:1 铺满输出，只沿流向滚动）或 `free(point, zoom)`（WuXian 视图，zoom 0.05–4）。
- **视图**：`workspace` | `wuxian`。

### 15.3 行为（规范性）

1. 进入工作区视图时工作区内容 1:1，与 `scale` 无关；`scale` 只影响 WuXian 视图。
2. 工作区视图只呈现当前工作区（加 pinned、sticky 的窗口）；工作区之间的操作在 WuXian 视图。
3. 命中次序：pinned › 按 `z` 从高到低（一个工作区与它的窗口为一层，自由窗口各为一层）› 平面。
4. 新窗口：工作区视图 → 当前工作区；WuXian 视图 → 视野中心所在的最上层工作区，否则成为视野中心附近的自由窗口。
5. 拖放的目标是指针下最上层的工作区（按它的规则落位）或平面（成为自由窗口）；拖动期间不改变其他窗口的位置（预览不重排）。
6. 删除工作区：空的直接删除；有窗口的必须由人选择 `to_plane`（默认）或 `close`，`close` 必须有防误触的确认；最后一个不能删；编号不重排。
7. 编号：新建取最小空号；按编号跳转到不存在的工作区时以该编号新建。
8. 输出尺寸变化：工作区按新尺寸重新排布，`origin` 不变。

### 15.4 状态格式 `wuxian.json` v1

```json
{ "wuxian": 1,
  "view": "workspace", "current": 2,
  "camera": { "x": -120, "y": 40, "zoom": 0.5 },
  "workspaces": [ { "id": 2, "name": "编码", "color": "#5e9cff", "rule": "tile", "flow": "horizontal",
                    "origin": [0, 0], "scale": 1, "z": 3, "layout": { … 实现的排布状态，不透明 … } } ],
  "windows": [ { "match": { "app_id": "foot", "title": "…" }, "in": 2, "flags": ["floating"] },
               { "match": { "app_id": "org.kde.falkon" }, "free": [1800, 200, 1200, 800], "z": 4 } ] }
```

- `flags` 取 `floating`、`pinned`、`sticky`、`fullscreen`。读者容忍未知字段与未知的 flag；`layout` 是实现自己的（不同实现之间不保证恢复同样的列）；`wuxian` 版本只在不兼容时升。
- 读不了（坏的、版本更新）：不覆盖，改名保存，说明一行，用默认（"读不了不是空"）。

### 15.5 基本交互（建议性）

§3.11、§3.12 的手势与键位是推荐的基线（Super 层；Super+O 进出 WuXian 视图；Super+数字；Super+Shift+数字送走窗口；Super+M 改规则），
实现可改键，但应提供每一个操作。状态栏的指示（颜色 = 身份，形状 = 规则，§3.13）是建议。

### 15.6 模块 `openwx`

只依赖标准库：类型（`Workspace`、`FreeWindow`、`Camera`、`View`、`Rule`、`Flow`、`Placement` 的公共部分）、镜头变换（世界 ↔ 屏幕）、
命中次序、新窗口去处与编号规则（纯函数）、状态的读写（标准库实现的 JSON 子集，只覆盖 §15.4 的形状）、`validate(state)`（不变量检查）。

### 15.7 一致性测试

`conformance/` 定义一个适配接口（创建工作区、加窗口、拖放、删除、切换视图、读状态），用例覆盖 §15.3 每一条；`xdesk.layout` 通过它，
CI 里作为 xdesk 的测试运行。

### 15.8 与已有协议

面板与工具需要工作区信息时，WuXian 的工作区映射到 Wayland 的 `ext-workspace-v1`（编号、名字、当前、激活请求）；规则、颜色、世界位置与视图
等 WuXian 特有的信息以后以一个小扩展 `wuxian-v1` 提供（本轮只在规范里预留章节，不实现）。

## 16. Luban 的架构：为独立做准备

### 16.1 目标

Luban（`luban/`、宿主上的 `luban` 与 Luban OS 程序、它的测试与文档、它的生态包）以后搬到自己的仓库时，是**一次目录拆分加依赖行的改变**，
不改逻辑。本轮把边界定清、用 lint 守住、新代码遵守，现有的越界依赖列出清单与去处；能机械完成的搬动本轮做。

### 16.2 依赖分层（规范性，lint 检查）

| 层 | 是什么 | Luban 怎么用 | 以后 |
|---|---|---|---|
| L0 | 标准库、`luban/specs/*`、`luban/kits/*`、`luban/xdesk/*` | 任意 | 已是独立项目 |
| L1 基础 | 与包管理无关的通用库：`xlings.platform*`、`xlings.libs.*`（json、sha）、`xlings.i18n`、`xlings.observe`、`xlings.cli.model`、`xlings.ui`、`xlings.testkit` | 直接导入 | 成为独立的基础包，两边依赖它 |
| L2 共有 | 两边都读的格式与唯一入口：系统根的格式（`xlings.subos.root_etc`、`rootfs`、`boot`、`root_check`、`library_cache`、`edition`）、home 的布局（`home_view`、`located`）、镜像（`xlings.image`）、载体（`xlings.carrier`）、外部程序表（`subos.tools`、`subos.ports`）、提权的唯一入口（`subos.elevation`） | 直接导入；每个都有格式版本与说明 | 成为两边共享的"系统格式"包 |
| L3 xlings 的领域 | 包、索引、存储、隔离策略、会话：`xlings.store`、`xlings.subos.policy`、`policy_store`、`session`，以及 `src/` 的一切 | **只经协议**：`Port`（`capability`、`terminal`：同进程时直接调用，分开后是进程与 JSON 协议）与 xlings 的 JSON 事件协议；兼容性由 opencaps 的 needs/provides 与 `min_client` 表达 | 不变 |

- `luban/DEPENDENCIES.toml` 列出每个 Luban 包允许的 L1、L2 模块；`tools/lint_layer_deps.sh` 按它检查：不在表里的 `xlings.*` 导入失败。
- 现有的 L3 导入记在同一个文件的 `migrating` 段（模块、在哪里用、去处、期限），lint 允许它们但不允许新增：

| 现有 L3 导入 | 在哪里 | 去处 |
|---|---|---|
| `xlings.subos.policy`、`policy_store` | `luban/os/service`（`configure`：授权、隔离设置）、`apps/luban-os` | `Port` 能力 `policy_get` / `policy_set`（xlings 侧实现） |
| `xlings.subos.session` | `luban/os/service` | `Port` 能力 `session_*` |
| `xlings.store` | `luban/os/service` | `Port` 能力 `store_query` |

  本轮新增的桌面系统默认授权（GPU、剪贴板）直接走 `Port` 的新能力，不再导入 policy 模块；上表的其余迁移在独立前完成（§21）。

### 16.3 放置（本轮搬动）

| 现在 | 搬到 | 说明 |
|---|---|---|
| `apps/luban`、`apps/luban-os` | `luban/host/luban`、`luban/host/luban-os` | 宿主上的 Luban 程序；顶层 `apps/` 只留 xlings 的（gui、xdev） |
| `tests/unit/test_luban_*.cpp`、Luban 的 e2e | `luban/tests/unit`、`luban/tests/e2e`，需求编号在 `luban/tests/requirements.toml` | `xdev test` 读两份需求文件、`tests/lanes.toml` 收集两处的测试；`covers` 的检查对两份都生效 |
| `docs/luban`（已在一个子目录） | 不动；新增的 Luban 文档只进这里（含 `docs/zh/luban`） | |
| 发布：`tools/{linux,macos}_release.sh`、`windows_release.ps1` 里 Luban 的部分 | `luban/tools/release.*`，顶层脚本调用它 | luban、luban-os、xdesk、luban:cursors 的资源 |

搬动是机械的（路径、`mcpp.toml` 的 workspace 成员、CI 的路径过滤），单独一个检查点（K0），不夹带逻辑改动。

### 16.4 版本与兼容

- Luban 的版本已有自己的来源：`luban/os/service/src/service.cppm` 的 `kVersion`（在 `luban/` 里）。独立前仍与 xlings 同号发布（AGENTS 的三处
  一起改，不变）；独立后它自己走。
- Luban 与 xlings 的配合靠协议而不是同号：Luban 声明它需要的 xlings 能力（opencaps needs），xlings 声明它提供的；不满足时说出要升级哪一个
  （已有的 `luban_provides` 机制扩展到 xlings 一侧）。

### 16.5 生态包

| 规则 | 现状 | 处理 |
|---|---|---|
| Luban 的包都在 `luban:` 命名空间 | `xim:xdesk`（xdesk 程序）在 `xim:` | 新的 Luban 包只进 `luban:`；`xim:xdesk` 的改名要动已装的路径，留到拆分时（§21） |
| `xim:` 的包不依赖 `luban:` 与它的库 | `xim:agent-private`、`xim:agent-confined` 导入 `xim.pkgindex.luban`；`xim:sudo` 的说明写 Luban 的规则 | 两个 agent 包的策略函数移到通用库 `libs/policy.lua`（它与 Luban 无关）；sudo 的说明改为通用措辞 |
| Luban 的库与资源在一处 | 库 `libs/luban*.lua` 与资源 `tools/res/{qt-runtime,falkon,lxqt,kde,labwc,labwc-desktop,audio,foot,xwayland,kernel-virt,bundle}` 与通用的混在一起 | 资源搬到 `tools/res/luban/`（只改 `build.sh` 的路径）；库的名字已带 `luban` 前缀，搬目录要改加载器，留到拆分时 |
| 依赖写完整坐标 | 已是（`xim:qt…`、`luban:…`） | lint 检查 `pkgs/luban/` 里没有不带命名空间的依赖 |
| 以后 | — | `luban:` 成为子索引（像 awesome、d2x、scode 一样由主索引声明），搬动是 `pkgs/luban`、`libs/luban`、`tools/res/luban`、`docs/12-luban-packages.md` 四处 |

通用的包（`xim:mesa`、`xim:graphics`、`xim:qemu-system`、`xim:qemu-system-gl`、`xim:linux-kernel-virt`）留在 `xim:`：它们不只为 Luban。

## 17. 文档

- `docs/luban/` 与 `docs/zh/luban/`：桌面页重写"工作区与 WuXian"；设置页（新的项、恢复默认、渲染器、晃动找指针）；复制粘贴；
  GPU（沙箱与 machine 各需要什么、怎么确认）；`luban new` 的进度。`docs/generated/` 的键位表随快捷键更新。
- `luban/specs/openwx/`：`README.md`、`SPEC.md`（§15 的规范性内容）；`luban/kits/cursor/README.md`（源的格式、工具）。
- `luban/specs/opensal/SPEC.md`：`'m'`、`'r'`（共享内存的帧）、`'p'`（指针偏好）三个消息，opensal 小版本加一。
- `Port` 的新能力（桌面系统的默认授权）写进 Luban 与 xlings 之间的能力表（`docs/luban/` 的架构页，§16.2）。
- AGENTS.md 的发布一节：推到 CN 镜像的项目加上 `luban:cursors` 的资源（`tools/mirror-latest.sh` 加这个项目）。
- `luban/README.md`：加 `host/`、`tests/`、`DEPENDENCIES.toml` 与 §16.2 的分层；AGENTS.md 的布局一节（`luban/host/`、`luban/tests/`）。
- `graphics-stack-spec.md` 加 xdesk 的渲染器选择与 virgl 一节。
- 实现后把模型（§3.1–§3.5）提升到 `docs/design/`。

## 18. 测试

| 范围 | 测试 |
|---|---|
| `wuxian` 规范 | 镜头变换、命中次序、新窗口去处与编号规则、状态读写往返、`validate`；一致性用例（§15.7）对 `xdesk.layout` 全部通过 |
| `cursor` kit | 光栅化（尺寸、热点、尾巴斜率、覆盖率一致）；晃动检测（反转次数、速度、路径长度的边界，正常移动不触发） |
| 架构 | `lint_layer_deps.sh` 按 `luban/DEPENDENCIES.toml` 检查：表外的 `xlings.*` 失败、`migrating` 段不许新增；`pkgs/luban/` 的依赖带命名空间、`xim:` 不依赖 `luban:` |
| `xdesk.layout` | 工作区增删改、编号（空号与指定号）、比例不改排布、重叠的命中按层次、新窗口去处（§3.6 每行）、跨工作区与到 WuXian 的拖放、删除的两种处理、视图切换后的焦点；平铺拖动：起手在自己格子上为 `none`、判定在快照上、交换 / 切分 / 新列 |
| compositor | Super+滚轮与 Super+空格清 `super_alone`；中键在窗口上转发；切换器悬停离开后选中不变；设置的悬停与焦点、`TextCommit` 进搜索、恢复默认；Super+C/V 对终端与非终端；状态栏指示 |
| 渲染 | 选择逻辑每一步失败退回 pixman 且原因正确；共享内存帧协议；有 vgem 时 GLES 与 pixman 的像素对比 |
| 纯函数 | 滚轮速度曲线；进度阶段映射（`check` 在 `boot` 前按 `preflight`、`install` 属于配置、权重调整不回退） |
| luban | 只有记录的残留给出 `luban rm`；名字预留冲突与过期接管；错误不含 xlings 的 usage |
| Luban OS 应用 | 新建页：一张 xdesk 卡、传统桌面折叠、角标；进度页只列有的阶段；指针按实际密度取尺寸、收到 `'p'` 偏好后晃动放大 |
| xlings | 私有 subos 里 `xlings info xim:gcc` 成功；worker 起不来时报"读不了"；不出现"-y 取第一个"；模糊候选稳定；用户视图落到系统默认 |
| machine e2e | sudo 可用；worker 能执行配方；有 virgl 的宿主上渲染器为 GPU（本地验收） |
| xim-pkgindex | qt-runtime 带 `icudt78l.dat`、env 设 `ICU_DATA`；cursors 每个尺寸都生成；内核配置含 virtio-gpu；mesa 含 virgl（静态检查） |

改了输出文字的地方（进度两行、`luban new` 与 xlings 的报错）同时更新 `tests/fresh-install/` 里断言已发布文字的用例。

每个测试 `covers` 新的需求编号（前缀 `WUXIAN-`、`CURSOR-`、`GPU-`、`LUBAN-ARCH-`、`PROGRESS-`、`SUDO-`、`RECIPE-WORKER-`）：xlings 的在 `tests/requirements.toml`，Luban 的在 `luban/tests/requirements.toml`（§16.3）。

## 19. 任务与检查点

xlings（一个 PR，每个检查点一个 commit，各跑一次完整 CI）：

| 检查点 | 内容 |
|---|---|
| K0 | 搬动（§16.3）：`apps/luban*` → `luban/host/`、Luban 的测试 → `luban/tests/`、发布脚本的 Luban 部分 → `luban/tools/`；`luban/DEPENDENCIES.toml` 与 lint。发布产物的文件清单与搬动前逐一相同（对比两次构建的 tarball 列表）。不夹带逻辑改动 |
| K1a | `luban/specs/openwx`：SPEC、模块、一致性测试 |
| K1 | `xdesk.layout`：在 `wuxian` 类型上实现工作区、镜头、两种视图、比例、层次、拖放快照与判定；去掉三选一与旧的组；`canvas` 全部改名 `wuxian`；通过一致性测试 |
| K2 | compositor：两种视图与过渡、工作区的绘制、命中、跨工作区与到 WuXian 的拖放、平铺的占位预览、删除对话框 |
| K3 | 快捷键、状态栏指示、右键菜单、Super+空格、中键 |
| K4 | 切换器（等待、选中与悬停、突出）、滚轮 |
| K5 | 设置（外观、三种状态、输入法搜索、新的项、恢复默认、启动器里的"设置"、间隔 4）、关于页、Super+C/V |
| K6 | 会话与配置合并、`wuxian.json`、设置的 `[wuxian]` 节与旧名读入 |
| K7 | 渲染：wlroots 打开 GLES、选择与退回、共享内存的帧（opensal.wire、窗口端）、关于页的渲染器 |
| K8 | 指针：`luban/kits/cursor`（源、光栅化、`cursor-make`、晃动检测）、窗口按实际密度取尺寸与晃动放大、opensal `'p'` 偏好、设置项、发布脚本生成主题 |
| K9 | luban：阶段与双进度、`luban new` 的名字、桌面系统默认的剪贴板与 GPU 授权（经 `Port` 的新能力，§16.2）、machine 选 GL 版 qemu 与 virtio-gpu |
| K10 | Luban OS 应用：进度页、新建页 |
| K11 | machine：sudo；xlings：worker 在系统里运行、读不了不是没有、名字解析的报错、用户视图落到系统默认 |
| K12 | 发布准备：新的资源仓库（`qemu-system-gl`、`luban-cursors`）在 GitCode 上先提交一次内容再建 release；文档（`docs/luban` 中英、`docs/generated/`、`graphics-stack-spec.md`、规范与 kit 的 README、`luban/README.md`、AGENTS.md）、需求编号、发布版本号（三处一起改；先查版本号是否已被占用） |

xim-pkgindex（一个 PR）：

| 检查点 | 内容 | 时机 |
|---|---|---|
| P1 | qt-runtime 的 ICU 数据、falkon 的 env 与启动脚本 | 随时 |
| P2 | 删除 `tools/res/cursors`；`luban:cursors` 新版本用本仓库发布的主题资源 | xlings 发布后（随 P4） |
| P3 | 内核加 virtio-gpu、`xim:mesa` 加 virgl、`xim:qemu-system-gl` | 资源先发到 xlings-res，K9 的 e2e 依赖它 |
| P3a | 生态的边界（§16.5）：`tools/res/luban/`、agent 包的策略函数移到 `libs/policy.lua`、sudo 的说明、依赖命名空间与 `xim:` 不依赖 `luban:` 的 lint | 随时 |
| P4 | `luban:xdesk`、`luban:desktop`（会话 `xdesk`、带 `xim:graphics`）新版本；`xlings`、`luban`、`luban-os`、`xdesk` 的版本指针 | xlings 发布并推到 CN 镜像后 |

## 20. 自审

| 视角 | 发现 | 处理 |
|---|---|---|
| 一致性 | 初稿说"工作区视图不显示其他工作区"，与 Super+1..9 的可发现性冲突 | §3.13 改为显示指示、不做操作；可关 |
| 一致性 | "编号取最小空号"与"Super+5 去没有的工作区"矛盾 | §3.9：菜单与 Super+Ctrl+N 取最小空号，Super+数字用那个编号 |
| 一致性 | 初稿键盘表把 Super+Shift+0 同时用于"放到 WuXian"与"显示全部" | 显示全部改为 Super+Ctrl+0 |
| 一致性 | "浮动"与"自由窗口"两个概念容易混 | §3.1 定义两个词、设置与文档不混用 |
| 规则：读不了不是没有 | 旧 `state.json` 直接丢弃是悄悄丢数据 | §3.14 改名保存、说明一行 |
| 规则：读不了不是没有 | worker 失败被报成"索引里没有"（H26 的根） | §13.2 第 2 条 |
| 规则：系统调用在 `modules/platform` | sudo 的专用 tmpfs 需要新的挂载参数 | §13.1 在 `platform` 加带参数的 `mount_tmpfs` |
| 规则：外部程序一张表、宿主依赖分层 | GL 版 qemu、Mesa 是新的依赖 | §7.4、§7.5：T1 包（`xim:graphics`、`xim:qemu-system-gl`），宿主只提供内核驱动与 NVIDIA 用户态（T0）；不调用宿主包管理器 |
| 规则：配方改了作用要升 revision | qt-runtime、falkon、cursors、mesa、内核 | P1–P3 各升 revision 或新版本 |
| 规则：层次的依赖方向 | Super+C/V 的终端表、渲染器选择在 xdesk（kit 不引用 xlings / luban）；GPU 与剪贴板的默认授权在 luban service | §7、§9 按此放置 |
| 安全 | 默认给桌面系统 GPU 与剪贴板：系统里的程序都能用 | 只对从窗口新建、有桌面的系统默认开启，Agent 系统默认关；新建时说明，设置里可关 |
| 兼容 | 已有系统的会话名、`keys.json` 的旧动作、旧 opensal 的 `'f'` 帧 | 别名会话（§12.2）、旧设定照常读入（§3.12）、新消息可协商（§7.3） |
| 交互边界 | 重叠的工作区里拖放、全屏窗口进 WuXian 视图、在所有工作区显示的窗口在 WuXian 视图的位置 | §3.5、§3.10 写明 |
| 交互边界 | 关闭全部时有窗口不肯关 | §3.9：10 秒后留下为自由窗口并说明 |
| 交互边界 | 非整数密度下的指针 | §8：每个密度正好的尺寸，窗口不重采样 |
| 性能 | WuXian 视图里很多缩小的工作区 | 沿用 2026-10-20 §13.2 的缩略图；GPU 下缩放在 GPU 上 |
| 性能 | GPU 读回的代价 | 只读回变化的矩形、直接写进共享内存；预算见 §7.7 |
| 可测性 | CI 没有 GPU | 选择与退回的每一步可注入失败；有 vgem 时做像素对比；真机矩阵本地验收（§7.7） |
| 风险 | 本轮最大：K1–K2（布局模型重写）、K7 与 P3（GPU 的两条路径） | 检查点顺序让 K1 的单元测试先稳住；P3 的资源先发，K9 的 machine e2e 才依赖它；GPU 任何失败都有软件路径 |
| 风险 | 体积：桌面 edition 带 Mesa 约 +150 MB | 新建页写实际大小（§12.1） |
| 遗漏 | 多显示器扩展、框选自由窗口、应用规则 | §21 列为以后；模型已留口（每块屏一个镜头） |
| 修订 2：一致性 | 规则若用颜色区分，会与"颜色 = 工作区身份"冲突，色弱也分不出 | §3.13：颜色只表示身份，形状表示规则；状态栏与 WuXian 视图的标签同一组图形 |
| 修订 2：层次 | 指针 kit 名字不说 Luban，但主题（形状、强调色）是 Luban 的 | §8.1：kit 只有格式、光栅化与检测；主题数据在 `luban/os/window/cursors/` |
| 修订 2：单一来源 | Python 的 `make.py` 与运行时各一个光栅化器会不一致 | §8.1：只留 C++ 的一个，主题由发布脚本用 `cursor-make` 生成，`tools/res/cursors` 删除 |
| 修订 2：交互边界 | 晃动检测误触（拖窗口时快速移动、游戏锁定指针） | §8.3：按键按着、指针锁定或隐藏时不触发 |
| 修订 2：平台 | 宿主对光标尺寸有上限 | §8.3：建不出大光标时在窗口里画放大的指针 |
| 修订 2：规范的独立 | 规范只依赖标准库，状态读写不能用 nlohmann | §15.6：规范模块自带只覆盖 §15.4 形状的 JSON 子集；实现可以另用自己的 JSON 库，一致性测试保证结果相同 |
| 修订 2：规范的边界 | 规范若定义平铺的列宽算法，第二个实现很难符合 | §15.1：只定义模型、不变量、状态与基线交互；排布算法与绘制是实现的事，`layout` 字段不透明 |
| 修订 2：命名 | 旧配置与旧动作名（`[canvas]`、`overview`）、旧会话名（`xdesk-canvas`） | §3.16：读入旧名；会话别名保留；代码里的 `canvas` 标识符全部改名 |
| 修订 2：架构 | 一次把 L3 依赖全迁到 `Port` 会让本轮失控 | §16.2：本轮定分层、加 lint 与 `migrating` 清单、新代码遵守；迁移在拆分前完成 |
| 修订 2：架构 | 搬动目录可能悄悄改了发布产物 | K0：对比搬动前后的 tarball 文件清单；搬动单独一个检查点 |
| 修订 2：架构 | 新加 `luban/VERSION` 要构建时注入，得不偿失 | §16.4：`kVersion` 本来就在 `luban/` 里，是 Luban 自己的来源，不加文件 |
| 修订 2：生态 | `xim:xdesk` 改名、`libs/luban*` 搬目录都要动已装路径或加载器 | §16.5：留到拆分时；本轮只搬资源、去掉 `xim:` 对 `luban:` 的依赖、加 lint |
| 修订 3：交互 | WuXian 视图里窗口照常可操作，Esc 一律回工作区视图会抢走应用的 Esc | §3.12：只在焦点不在窗口上时 Esc 回去；Super+O 总是可用 |
| 修订 3：交互 | "没名字的空工作区离开即删"会删掉刚在 WuXian 视图里明确新建、还没放窗口的工作区 | §3.9：只自动删除 Super+数字顺带建出的 |
| 修订 3：规范 | 规范把编号写死成 1–9，是 xdesk 的产品选择 | §15.2：编号是正整数，上限由实现定（xdesk 为 9） |
| 修订 3：规范 | 状态格式没有窗口的浮动、置顶等属性，恢复后会丢 | §15.4：`flags`，读者容忍未知值 |
| 修订 3：重复定义 | `xdesk.layout` 的 `WorkspaceInfo` 与规范的类型重复 | §3.15：直接用 `openwx::` 的类型 |
| 修订 3：依赖 | xdesk 打开 GLES 后链接 EGL / GBM，没有 Mesa 的系统里会起不来 | §7.4：`luban:xdesk` 自己依赖 `xim:graphics`；缺库时的报错写明装什么 |
| 修订 3：传输 | memfd 被对方缩小会 SIGBUS；中间有转发进程时 fd 过不去 | §7.3：`F_SEAL_SHRINK`、尺寸变化换新块；转发进程转交 fd，不行就回到 `'f'` |
| 修订 3：测试 | 搬测试后 `covers` 只认 `tests/requirements.toml` | §16.3：`xdev test` 读两份需求文件 |
| 修订 3：测试 | 改了已发布的输出文字（进度、报错），main 上的 fresh 套件会失败 | §18：同时更新 `tests/fresh-install/` |
| 修订 3：发布 | 新的资源仓库（`qemu-system-gl`、`luban-cursors`）在 GitCode 上没有提交时建不了 release；CN 镜像要多推一个项目 | K12、§17：先提交一次内容；`mirror-latest.sh` 与 AGENTS 加 `luban:cursors` |
| 修订 4：命名 | 规范与平面同名，与其他规范（opensal、openhal…）不一致 | §3.16：规范叫 openwx（`luban/specs/openwx`、模块 `openwx`），平面与实现里仍叫 WuXian |
| 修订 3：范围 | 传统桌面是否也走 GPU | §7.1：拿到授权与 Mesa 时 labwc 自己用 GLES；它们的帧路径本轮不改 |

## 21. 不做的与以后

- 帧经 dmabuf 零拷贝交给宿主窗口；帧经共享内存交给窗口（§7.3 的修正）；Venus（Vulkan）；macOS、Windows 宿主 machine 的 3D。
- 多显示器扩展（每块屏一个镜头、各锁一个工作区）；本轮仍只镜像。
- WuXian 上框选多个自由窗口、"平铺这些"；应用规则"总在某工作区打开"。
- WuXian 的 `wuxian-v1` 扩展协议与 `ext-workspace-v1` 的实现（规范里预留，§15.8）。
- machine 的 3D 已在 2026.10.21.3 实现（§7.5 的"实施"）。仍不做：aarch64 宿主与 macOS、Windows 宿主 machine 的 3D（Mesa 只有
  x86_64；后两者没有 virgl 的宿主路径）；Venus（Vulkan）。沙箱的 `gpu` 授权仍绑定整个 `/dev/dri`（§7.4 的"只绑 render node"以后做）。
- xdesk 自己画指针时（宿主不提供 `Cursor`：2026.10.19.2 之前的 Luban OS）的晃动放大：这时的指针图像已经来自 kit 生成的
  `luban:cursors`，检测留到那条路径还有用户时再接（K8 实施时的取舍）。
- Luban 拆分时一并做：L3 依赖迁到 `Port`（§16.2 的 `migrating` 段）、`xim:xdesk` 改名、`libs/luban*` 搬目录、`luban:` 成为子索引。

## 22. 评审结论

已确认（2026-10-20，三轮评审）：

1. 一张 WuXian、工作区是它上面的特殊窗口；两种视图；比例 0.2–2，进入总是 1:1；可以重叠，按层次命中。
2. 删除有窗口的工作区：默认变成自由窗口；关闭全部 5 秒后可按。
3. 编号与自动删除（§3.9）；平铺拖动改为冻结 + 占位；不做拖到屏幕边缘换工作区。
4. 状态栏：颜色 = 身份，形状 = 规则；当前一块 + 其他的小图形，可关。
5. 新键：Super+O、Super+Shift+0、Super+Ctrl+N、Super+Ctrl+0、Super+C/V、按住 Super+空格。
6. GPU 本轮完成（沙箱与 Linux 宿主的 machine），桌面带 Mesa；帧零拷贝、Venus、macOS / Windows 的 3D 不在本轮。
7. 桌面系统默认开剪贴板与 GPU。
8. 矢量指针；晃动找指针默认"放大并变色"、最大 4 倍；指针主题的源搬到本仓库。
9. WuXian 的命名范围与规范范围。
10. Luban 独立的本轮范围：分层与 lint、搬目录、新代码走协议；L3 迁移、`xim:xdesk` 改名、子索引留到拆分时。

修订 3 的自审改动（§20 末尾）不改变以上结论，实施按 §19 开始。
