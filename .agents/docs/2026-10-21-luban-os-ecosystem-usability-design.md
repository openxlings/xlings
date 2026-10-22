# Luban OS 的生态级可用性：系统里的 xlings 与真机一致、两种存放方式、各规范各管一件事的设备与传输、自带软件有保证、创建流程、xdesk 交互、独立的 Luban CI

日期：2026-10-21　状态：修订 5，评审结束（§16），实施中；通信的统一见 part 2（`2026-10-22-openbus-xbus-design.md`）；§9.5 依据 2026-10 的调研（§17）；任务依赖见 §13.1　缘起：2026.10.21.4 在 Linux 宿主（X11、NVIDIA、KVM）上的实测——Luban OS 应用新建
`luban:desktop 2026.10.21.3`（`desktop-6` 中途取消、`desktop-7` 以 machine 运行，另有 LXQt 系统），以及之后关于系统里的 xlings、桌面交互、跨平台、CI 的讨论。

范围：
- **xlings**：`modules/platform`（文件系统类型）、`src/core/xim`（Lua worker 与输出）、`src/core/xvm` 与 `modules/subos`（环境变量合并、broker、存放方式）、
  `src/core/subos/root`（域内创建、取消）。
- **luban**：`luban/os/service`、`luban/os/vm`、`modules/carrier`（传输）、`luban/os/cli`。
- **Luban OS 应用**：`luban/host/luban-os` 与 `luban/os/window`（进度、取消、键盘与指针、控制台、新建页）。
- **xdesk**：`luban/xdesk/*`（切换器、覆盖书签、折叠、上手应用、可观察性）。
- **规范**：opensal 0.6（共享区域里的帧）、openhal 0.3（machine 的虚拟设备与设备模拟）、opencaps（新能力名）、openwx（折叠；书签属于建议性交互）。
- **生态**：
  - xim-pkgindex：`xim:xdesk`、`luban:falkon`、包的检查、`xim:virtiofsd`、Mesa 的 d3d12 驱动；
  - mcpp：Luban 系统里的 sysroot；
  - 新仓库 `luban-os/luban-os-ci`：独立的 Luban CI。

交付：每个仓库一个 PR，每个检查点一个 commit（§13）：xlings、xim-pkgindex、mcpp、luban-os-ci（由 Sunrisepeak 新建）；`virtiofsd` 的资源先发到 xlings-res。

替换的旧决定：
- 2026-10-20 WuXian 设计：
  - §13.2 第 1 条的实现（以 `statfs` 判断 9p，从未生效，J1）；
  - §7.3"machine 仍经 socket 发像素、共享内存移到以后"（改由 §5 定义）；
  - §10.1"阶段内一步做完即记满"（§7.1）；
  - §11"名字预留只按进程号"（§7.3）。
- 2026-10-15 part 4 设计 §6.4 的通道表：显示等通道的传输改按 §5.3 的"多跳"描述，字节流之外加上可选的共享区域。
- 现状"machine 里系统级的 xlings 只能从宿主做"（从未写成决定，§3 取代）。

## 0. 核心点

1. **处处一致**：同样的命令、同样的结果——不论系统以沙箱还是 machine 运行，不论宿主是 Linux、macOS 还是 Windows，也不论装在真机上；
   差异只在幕后，输出里不把"去宿主上做"当成常规出路（§2、§3）。
2. **两种存放方式，可以配置**：
   - `shared`（宿主上新建的系统默认用它）：包放在这台电脑上各系统共用的仓库里，系统级写入由宿主代办；
   - `own`：系统自带一个可写的 `/xlings`，和真机一样。

   两种方式下命令与结果相同；切换复用导出与导入的闭包机制（§3.1）。
3. **sudo 按系统习惯**：影响其他用户或整个系统的操作要 `sudo`，包括安装到系统、更新索引、改系统默认版本；只影响自己的不要，
   例如 `xlings use node 22`。是否属于系统级由同一个 `classify` 判断，CLI 与 broker 共用；CI 覆盖（§3.2）。
4. **两个根因先修**：
   - J1：9p 用 `statfs` 判断，而 9p2000.L 返回的是宿主的类型，所以 machine 里所有要读配方的命令都失败；
   - J28：给空的 `XDG_DATA_DIRS` 前置目录时丢掉了规范默认的 `/usr/share`，所以 nvim 等应用从启动器里消失。

   修好这两处，machine 里的普通用户就能装软件，应用也都回来了（§3.4、§6.2）。
5. **broker 所有 carrier 共用**：沙箱已有，machine、vz、WSL2 经各自 carrier 的消息流接上；每次代办都是一次 openas 的 run，
   记录"谁、替谁、做了什么"（§3.3）。
6. **各规范各管一件事**：
   - carrier 提供传输（字节流，加上可选的共享区域，可以多跳）；
   - opensal 定义表面，0.6 让帧放进共享区域；
   - openhal 定义设备，0.3 把 machine 的虚拟设备列为设备，并提供设备模拟 `openhal-emu`；
   - opencaps 说明能力；
   - openas 记录代办。

   每个平台在实现表里填自己能做到的，缺的能力有统一的退路，代码里不写平台特例（§5）。
7. **macOS 与 Windows 尽量都支持**：
   - broker、两种存放方式、消息传输加压缩：两边都做；
   - Windows：改用 hvsocket，沙箱系统经 Mesa 的 d3d12 驱动用上 GPU；
   - macOS machine 的 virtio-gpu 扫描输出、Windows 嵌套 machine 的 virgl：先做可行性验证，过了门槛就在本轮实现；
   - 做不到的项，在能力报告与 CI 报告里写明原因（§5.4）。
8. **machine 的文件共享改用 virtiofs**，9p 为后备（§4）。
9. **包自己声明检查**：沿用版本已有的 `checks` 格式，新增 `gui`、`launcher`、`sound` 三种，检查的是用户实际看到、听到的结果（§6.1）。
   桌面版自带轻量的音频播放器（建议 Audacious），并附带一段以 CC0 发布的经典音乐（建议 Open Goldberg Variations 里的《咏叹调》），声音这条路径从此有端到端的检查（§6.3）。
10. **桌面的流畅度**：先把帧的每一段测量清楚（guest 合成、读回、传输、宿主上传），再按顺序改进；目标写成预算（1080p 下拖动 ≥ 50 帧/秒等）：绝对值在参考机器上本地验收，CI 盯相对回归（先只报告，稳定后回归就算失败）（§5.6a）。
11. **创建流程**：
    - 阶段不再因为其中一步做完就记满；
    - 额外的包改走可取消的能力；
    - 取消时先 SIGTERM 让 xlings 收尾，之后把做了一半的移开；
    - 名字预留按"进程 + 任务"判断；
    - 外层能找到内层的记录（§7）。
12. **被盖住的项有书签**：工作区就是特殊的窗口，与自由窗口统一处理。完全被盖住的项，在盖住它中心点的最上层那一项的右边缘伸出书签，
    点击就浮上来；Super/Alt+Tab 能看到全部（§8.4）。
13. **xdesk 的其余交互**：
    - 上手应用（每人每台电脑自动打开一次）；
    - 切换器只高亮窗口自己的边框，选中后在原位 0.5 秒淡出；
    - 折叠（`Super+S`）、自由窗口缩放、标题栏命中的下限、越出显示区域的规则、小地图；
    - 宿主的 Super 手势不再把状态弄乱（§8）。
14. **独立的 Luban CI**：在 `luban-os/luban-os-ci` 用 QEMU 与 `openhal-emu` 模拟设备，L2–L6 放在那里，覆盖版本 × 模式 × 存放方式 × 渲染器 × 宿主平台；
    只用 GitHub 托管的 runner：macOS、Windows 上托管 runner 跑不了的部分，由 Linux 上模拟的"两跳"覆盖它们共用的逻辑，平台专属的部分在报告里写明原因；
    先只报告，稳定后把 L2 子集设为 xlings 的必需检查（§9）。

## 1. 实测与定因

| 编号 | 现象 | 定因 | 依据 |
|---|---|---|---|
| J1 | machine 里普通用户运行 `xlings install node@24`、`xlings update`，报 `metadata boundary: cannot capture recipe output in ~/.xlings/run/subos/default/lua/<id>/metadata.log: Permission denied`；`self doctor` 说 home 在 ext4 | 2026-10-20 §13.2 的绕行（在 9p 上时把 worker 的私有目录改放到 `XDG_RUNTIME_DIR`）用 `statfs` 的 `f_type` 判断是不是 9p；**9p2000.L 的 `statfs` 会转发给宿主，返回的是宿主的类型（ext4）**，所以绕行从来没有生效。worker 在 Landlock 围栏之后写 9p 上的文件时被拒绝（很可能因为 9p 在 `cache=mmap` 下不保持 inode，Landlock 的路径规则匹配不上；K1 第一步先复现确认）。另外，machine 的会话里不一定有 `XDG_RUNTIME_DIR` | `src/core/xim/lua_boundary.cpp:506-514,770-801`、`modules/platform/src/os/fs_linux.cpp:35-57`、`isolation.cpp:561` |
| J2 | machine 里 `xlings update` 先测镜像（GitHub 要 6 秒），再对 4 个索引各报一次 `Read-only file system`，最后报 `failed to rebuild catalog` | machine 把 `/xlings`（这台电脑上各 Luban 系统共用的仓库与索引）以只读方式挂入，系统级的写入没有出路：沙箱有 broker，**machine 没有**（`broker::available()` 只认 `sandbox`） | `luban/os/vm/src/host.cpp:432-433`、`modules/subos/src/session/broker.cpp:148-155`、`src/cli.cpp:2123-2140` |
| J3 | `sudo xlings install node@24` 报 `/xlings/.xlings.lock cannot be written`，没有给出出路 | 同 J2 | 同上 |
| J4 | 每条命令都报 `index repo 'local' skipped …`；加 `-v` 的 update 有十几行 | worker 一失败（J1），本地索引就整个被跳过；每次、每个索引都报一条；过程信息出现在默认输出里 | `src/core/xim/` 的索引构建 |
| J5 | `mcpp self doctor` 报 `sysroot / belongs to neither …` | mcpp 不认识 Luban 系统自己的根 | mcpp 仓库 |
| J6 | 新建时"配置"很快到 100%，之后 80 秒不动；"N 秒前"在 0 和 1 之间跳 | 阶段内任一步结束就把阶段记满；装额外的包与桌面走 `Port::terminal`，既不发事件也不能取消；"N 秒前"在跳，说明一直有输出 | `events.cppm:85`、`service.cpp:597-628`、`port.cpp:139-142` |
| J7 | 取消后再建同名失败（xlings 0.5 秒就以 exit 1 退出）；现在已经没有残留 | 取消时直接 SIGKILL xlings（`stream.cpp:226`），没有 `end`、没有清理；名字预留只按进程号判断（`names.cpp:47`），而 Luban OS 应用在一个进程里跑多个任务，第二次新建会接管第一次的预留。半成品后来是被哪个流程移走的，K8 用测试复现 | `names.cpp:33-54`、`stream.cpp:224-227`、外层 ops 日志 `e976f6af…` |
| J9 | 启动器里没有"设置" | `xim:xdesk` 的 `config()` 没有链接 `share/applications` | xim-pkgindex `pkgs/x/xdesk.lua`、`xdesk_release.sh:75` |
| J10 | 第一次进入时，底部的提示被挡住一半 | 提示的位置按启动时的尺寸计算；`resize` 时没有移动它 | `overlay.cpp:96`、`overlays.cpp:317-321`、`output.cpp:29-43` |
| J11 | machine 的桌面很卡；`--disallow gpu` 之后明显好转 | 帧挤在 agent 的 virtio-serial 上（1080p 一整帧约 8 MB，损坏区域超过 8 块就发整帧）；用 GPU 时每帧还要经 virgl 同步读回 | `host.cpp:434-436`、`output.cpp` |
| J12 | xdesk（machine）里 Falkon 以 exit 139 退出，单进程模式也一样，并报 `MESA-LOADER: failed to retrieve device information`。LXQt 里能打开但每次都慢、CPU 飙高；能进 `falkon:start`，一打开网页或新标签页就闪退 | 未定因。xdesk 下另有 J28 的影响。"一打开网页就崩"发生在新起的渲染进程上，和 2026-10-20 的 H23（缺 ICU 数据）是同一类；"每次都慢"很可能是 WebEngine 几百 MB 的文件每次都经 9p 读取，或者字体缓存每次重建。没有在桌面里打开 Falkon 的测试 | xim-pkgindex `tools/res/build.sh`、`tools/res/luban/falkon/env` |
| J13 | 不能 Ctrl+C / Ctrl+V | 待分清：foot 里是 Ctrl+Shift+C/V；剪贴板的 e2e 只测了沙箱 | `test_luban_os_xdesk.cpp:672-706` |
| J14 | WuXian 里 Super/Alt+Tab 时背景不变暗，工作区的图层压在窗口上 | 变暗只改了 `background`；工作区的底板与窗口在同一层（`window_layer`），标签在 `shell_layer` | `switchers.cpp:136-139`、`wuxian.cpp:130-132` |
| J15 | 切换器的悬停框与窗口不对齐；选中后其他窗口先变暗再变亮 | 选中框是画在窗口外的一圈（`chosen_ring`）；`kFadeMs` | `switchers.cpp:35,95-127,334-370` |
| J16 | 键盘已锁定时按住 Super+空格：空格一直重复，出现手形，拖动时宿主的窗口跟着动 | 按住 Super+空格是 xdesk 的手形拖动；宿主 GNOME（X11）的 Super+拖动是挂在窗口外框上的被动指针抓取，锁定键盘管不到指针；GNOME 移动窗口时还会抓走键盘，Luban OS 应用收不到松开事件（实施前复现确认最后这一环） | `luban/os/window/src/host.cpp:361-390`、`input.cpp:54-104` |
| J17 | WuXian 里自由窗口不能拖边缩放 | `edges_at` 在 WuXian 视图里直接返回 0 | `drag.cpp:209-213` |
| J18 | 窗口缩小后标题栏很难拖 | 标题栏的高度没有下限 | 同上 |
| J19 | 小地图只能点一下 | 只实现了 `look_at` | `input.cpp:370-374` |
| J20 | 被盖住的窗口很难发现 | 没有任何标识；切换器只列出当前范围的窗口 | openwx `SPEC.md` §3.3 |
| J21 | 窗口与工作区不能折叠 | — | — |
| J22 | 越出显示区域的规则没有定义 | — | — |
| J23 | 输入时字是红色就 CPU 高 | 很可能是 fish 每敲一个键都查一遍 PATH，而这些目录在 9p 上；Luban OS 应用的控制台每次都整屏重画 | `console.cpp:373-412` |
| J24 | 看不到 xdesk 的状态；性能测试只有两项 | — | `test_xdesk_cpu.cpp:70,99` |
| J25 | 以上问题 CI 一个都没发现 | machine 套件只启动 `tiny`；版本的检查只用 `ls` 看文件在不在 | `tests/fresh-install/luban.sh:177-212`、`pkgs/luban/desktop.lua` |
| J26 | 进度页里出现 qemu-gl 安装的详细输出 | 第一次以 GPU 启动时才安装它 | `host.cpp:243-252` |
| J27 | 特权环境变量的提示每升一个版本就出现一次 | 按 `name@version` 比较 | `installer.cpp:2060-2111` |
| J28 | xdesk 的启动器里没有 nvim（2026.10.20 时还有） | 给空的变量前置目录时只剩新目录（`shim.cpp:414`），而 `XDG_DATA_DIRS` 为空意味着规范默认的 `/usr/local/share:/usr/share`；xdesk 自 2026.10.21 起声明了这个变量，machine 的会话里原本没有设置它。同样的写法还在 `manifest.cpp:644` 与 `vm/host.cpp:273` | `src/core/xvm/shim.cpp:412-425`、`modules/subos/src/model/manifest.cpp:595-650`、`luban/os/vm/src/host.cpp:265-276`、xim-pkgindex `libs/graphics.lua:190` |
| J29 | 新建页里传统桌面卡片的 4 个小方块挨在图标的右上角 | 位置是按图片的宽度算的 | `quick_start.cpp:636-651` |
| J30 | 关掉 GPU 后桌面仍然很卡 | 帧通道是瓶颈（J11）；软件合成时，每帧在 guest 里用 pixman 合成，再整帧或成块经 virtio-serial 发出，宿主再上传成纹理，三段都没有测量。现在只有两项性能测试（J24），没有"拖动、滚动、刷屏的帧率与 CPU"预算 | `output.cpp` 的 `render()`、`test_xdesk_cpu.cpp` |
| J31 | 桌面系统里没有音频播放器，也没有可以拿来测声音的素材 | 版本只带了声音的输出（`luban:audio`），声音这条路径除了"文件在不在"没有任何端到端的检查 | xim-pkgindex `pkgs/luban/desktop.lua` 的 `desktop-sound` |
| J32 | machine 里新加的用户（`adduser`）写不了自己的 home；`luban-os-ci` L2-05 发现 | machine 的目录树是宿主上的文件：9p（`security_model=none`）与非特权 virtiofsd 都以宿主用户存每个文件，`chown` 得到 EINVAL，`/home/<新用户>` 的属主是宿主的 uid——也就是 guest 里的默认用户。多用户、多版本（§3.6）在 machine 里因此不成立；沙箱与导出后的真机不受影响。可选：9p 用 `security_model=mapped-xattr`（属主与权限存在宿主文件的 xattr 里，宿主文件仍归宿主用户，宿主管理不变，读写多一次 xattr）；或 virtiofsd 在用户命名空间里用 subuid 映射（宿主上的文件归 subuid，宿主管理要跟着改）。建议前者，作为 machine 有第二个用户时的方式，单独一个 PR（§15） | `luban/os/vm/src/host.cpp` 的 9p 参数与 virtiofs 计划 |
| J33 | 用户视图里不加 sudo 的 `xlings install` 让包对所有用户生效；之后同一个用户 `xlings remove` 被拒绝（"is the system's"）；`luban-os-ci` L2-02 发现 | 仓库里没有的版本由 `__store-add` 以管理员身份执行 `install -y` 放进仓库，而 install 在系统的 workspace 里没有激活版本时会激活它（`activate_requested_targets`、注册计划、借用成员三处）。§3.2 要求不加 sudo 只给自己装。修法：`__store-add` 只放 payload、登记版本，不在系统 workspace 里激活；用户视图再借用并激活在自己那里。三处激活逻辑一起改，单独一个 PR，并补上 L2-02 的反向断言（另一个用户看不到） | `src/core/xim/commands.cpp`（`__store-add`、`activate_requested_targets`）、`src/core/xim/installer.cpp` |
| J34 | 2026.10.22.1 的 xdesk 在任何 Luban 系统里都起不来：`GLIBCXX_3.4.35 not found`；发布后验证升级的桌面时发现 | XBus 的 node 用了 `std::jthread`，它的 stop token 在原子量上等待，GCC 16 的 libstdc++ 才有这个符号（`std::__detail::__notify_impl`）；Luban 系统绑定的 C++ 运行时是 `xim:gcc-runtime` 15.1.0（`GLIBCXX_3.4.34`）。CI 里 xdesk 的测试用的是构建工具链自己的 libstdc++，看不到。2026.10.22.2：node 改用普通线程加 `stopping` 与唤醒管道；`xdesk_release.sh` 拒绝需要更新运行时的二进制；索引先把 `luban:xdesk` 钉回 2026.10.21.3（xim-pkgindex#1024），再以新版本 `luban:xdesk`、`luban:desktop` 2026.10.22.2 让已升级的系统换上修好的 xdesk（版本号的 revision 不会让已经装好的系统换版本） | `luban/services/xbus/src/node.cpp`、`luban/tools/xdesk_release.sh` |
| J35 | 把 d7 从 desktop 2026.10.22.1 升到 2026.10.22.2 时，`luban:xdesk` 没有换成 2026.10.22.2，而是显示 `keep luban:xdesk 2026.10.22.1 (yours; ...)` | 升级装上新版本，但换下来的旧版本仍在 `configured` 里；计划把“同一个包有不止一个版本”当成用户自己动过。所以任何系统的第二次 edition 升级，都不会再移动第一次升级动过的包。2026.10.22.3：edition 记录多了 `brought`，记下升级换下来的版本，计划把它们算作 edition 的；没有 `brought` 的旧记录，比记录版本更旧的版本当作 edition 以前带来的 | `modules/subos/src/model/edition.cpp`、`src/core/subos/root/root_upgrade.cpp` |
| J36 | luban-os-ci L2-02：同一个 home 里第二个系统（desktop-machine）的用户 `xlings install -y xim:zoxide` 失败（`source scope has not opted into this release` / `not in the system's store`），第一个系统（core-machine）的用户装过同一个包 | 一个 home 里的系统共用一个存储；用户视图判断“存储里缺不缺”只看载荷目录在不在，不看这个系统有没有登记它，于是不去 `__store-add`，借用时又因为系统没登记而失败。2026.10.22.3：系统没登记的版本就算缺，照常经那一扇门加进去（载荷已在，只登记） | `src/core/xim/commands.cpp`、`tests/e2e/test_user_view.cpp` |
| J37 | luban-os-ci L2-14（只在 GitHub runner 上）：agent 工作区里 `xlings install` 排队等审批，主人在宿主上 `xlings subos requests agent` 看不到，`approve` 也找不到那条请求 | 系统在私有域（`domains/xlings/private`）里时，它的监督进程把请求记在域的 home 下；`requests`/`approve`/`deny` 读的是主人自己的 home。本机因为不能建私有命名空间，系统落在 home 自己的前缀里，看不出来。2026.10.22.3：这三条命令像 `upgrade` 一样转到系统所在的域里执行，批准的命令也在那里跑 | `src/core/subos/cmd.cpp`、`tests/e2e/test_domain_producer.cpp` |
| J38 | 同一个 home 前缀里的 d7，在 d1 升级之后 `xlings use xdesk 2026.10.22.2` 失败，而且之后对它根文件系统的每次更新都失败：`xdesk.files.3@2026.10.21.3: root scope has not opted into this release` | 共用一个 home 前缀的系统（主机建不了私有命名空间时）共享一份版本库；d1 迁入自己的用户态时以新 revision 重新登记了 xdesk 2026.10.21.3，这一版多了一个成员（files.3），d7 当初装的旧 revision 没有它。根闭包把“发布里有、本系统没选的成员”当成拒绝。2026.10.22.4：本系统没选的成员不投影，也不拒绝 | `src/core/subos/root/store_closure.cpp`、`tests/unit/test_root_store_closure.cpp` |
| J39 | luban-os-ci L2-02（GitHub runner）：desktop-machine 的用户 `xlings install -y xim:zoxide` 仍失败——`__store-add` 报 configured，机器里看到的系统存储却没有登记 | 在 runner 上 desktop-machine 住在自己的用户态（私有域，机器里的 /xlings 只有它自己），core-machine 在 home 前缀里。机器的 broker 在宿主上、主人 home 里用 `XLINGS_ACTIVE_SUBOS` 跑请求，装进的是主人那份控制副本，机器从不读它；机器里任何 `sudo xlings install` 都一样。2026.10.22.4：`__broker-serve` 对私有域里的系统在它的域里再执行一次——裁决、审计和执行都用系统自己的 home 与策略 | `src/core/subos/audit.cpp`、`tests/e2e/test_domain_producer.cpp` |

## 2. 原则

1. **处处一致**：同样的命令、同样的结果；差异只在幕后。
2. **按系统习惯**：影响他人或整个系统的要 sudo，只影响自己的不要。
3. **各规范各管一件事**：传输、表面、设备、能力、代办分别由 carrier、opensal、openhal、opencaps、openas 定义；平台差异写进实现表，缺的能力有统一的退路。
4. **自带的就要能用**：检查看的是用户实际看到的结果。
5. **一次说一件事**：同一条警告一条命令里至多一次。
6. **可追溯**：外层能找到内层的记录；取消也留下记录。
7. **宿主是宿主**：宿主的快捷键与手势不能把系统里的状态弄乱。
8. **复用胜过新增**：能用已有的机制（版本的 `checks`、导出的闭包、broker、opensal 的协商）就不另起一套。

## 3. 系统里的 xlings

### 3.0 分层：在哪里、谁、哪个仓库，以及变更去哪（修订 5）

实施 K1、K2 时发现，"这个 xlings 在哪里运行、能不能写、该不该转交"分散在五处各自判断：
- CLI 的 `home_is_ours` 加 `broker::available()`；
- `Config` 判断用户视图；
- `home::read_system_layer` 判断能借用的仓库（只认标了 `multi` 的仓库）；
- 锁文件的报错；
- `__store-add` 的拒绝。

结果是同一台 machine 里，前一处说"这是系统的仓库"，后一处说"这不是"，用户的安装在两者之间失败。所以先把这件事收成一层：

```
决定            xlings.subos.situation    在哪里（host / sandbox / machine / system）× 谁（owner / admin / user）
                                          × 哪个仓库（personal / shared / own）；系统的根记录（/etc/xlings/usr）
                                          route(situation, touch) → here / broker / admin(sudo) / owner / no-broker
命令表          xlings.subos.broker       classify：命令碰到什么（read / fetch / index / instance / home）与策略的 op；
                                          judge：供应方的裁决（策略、审批队列），各个 carrier 共用
客户端          broker::forward           请求 + 调用方的三个标准描述符；带上原样的全局参数（--agent、--json），
                                          被转交的命令发出的事件流与在本地时一致
传输（只搬字节） sandbox：会话 supervisor 直接拿到描述符
                machine：guest agent 的 B 通道 ↔ luban.vm 的 bridge（qemu、vz 共用）↔ `xlings subos __broker-serve`
                旧宿主：没有 A 帧就不开 socket，客户端只说一行
执行            `subos __broker-serve`    judge、审计（谁、替谁、经哪里）、以 owner 身份为这个系统执行
```

规则：
- **只有 situation 判断"在哪里、谁、哪个仓库"**：`Config` 的用户视图、安装器的借用（`read_system_layer`、快照的 scope）、CLI 的转交、锁的报错都读它，不再各自推断。
- **只有 broker 的命令表知道"命令碰到什么"**；`route` 是一张表，单元测试逐格覆盖（`test_subos_situation`）。
- **luban 不解释请求**：vm 的 bridge 与 guest agent 只按通道搬字节、管终端尺寸与挂断；裁决与执行都在 xlings（经 `__broker-serve` 这个 plumbing 命令，符合"luban 只经协议到达 xlings 的领域"）。
- **用户视图读系统仓库**：系统的根记录指明它读哪个仓库与哪个 scope（系统自己的实例）；不论那个仓库自己的标记是 `personal` 还是 `multi`。
- **真机与导出的镜像**：place 为 system，仓库可写即 own，命令都在本地执行；与 machine 的差别只在 situation 一处。

### 3.1 两种存放方式（D1、E6）

| | `shared` | `own` |
|---|---|---|
| 包放在哪 | 这台电脑上各系统共用的仓库（宿主的域，`/xlings`） | 系统自己树里的 `/xlings` |
| 在系统里写入 | 只读挂入；系统级写入由宿主代办（broker，§3.3） | 可写，由系统里的 xlings 直接执行 |
| 索引 | 这台电脑共用一份（跟随主人的索引，#688） | 系统自己的一份 |
| 宿主管理（`xlings --subos`、`luban upgrade`） | 直接操作共用的仓库 | 系统运行时经 carrier 的消息流在系统里执行；停着时在它的树上以沙箱方式执行；都记在系统自己的日志里 |
| 磁盘 | 相同的包只存一份 | 每个系统一份（在同一个文件系统上用硬链接或 reflink 去重） |
| 默认 | 宿主上新建的系统 | 导出的镜像、装在真机上的系统 |

- 切换：`luban config <名字> --store shared|own`。
  - `shared → own`：把依赖闭包复制进系统，复用导出用的闭包机制（`src/core/subos/root/store_closure.cpp`）；
  - `own → shared`：把系统里的包并入共用仓库，复用导入机制，相同的包去重；
  - 有进度、能取消，失败时恢复原样；不碰系统的 home。
- 两种方式下，系统里的命令、输出与结果完全相同；只有 `luban status` 里的"存放"一行和磁盘占用不一样。
- macOS、Windows 的 carrier 同样支持两种方式（§5.4）。

### 3.2 sudo 与范围（D2、E1）

| 命令 | 不加 sudo | 加 sudo |
|---|---|---|
| `xlings install/remove <pkg>` | 只给自己装 / 删（用户视图 `~/.xlings`） | 给整个系统装 / 删 |
| `xlings use <pkg> <ver>` | 只切换自己的版本 | 切换系统默认的版本 |
| `xlings update` | 拒绝，只说一行：`更新索引会影响这个系统的所有用户：sudo xlings update` | 更新索引 |
| `xlings search/info/list` | 读 | 读 |
| `xlings self doctor --fix` | 只修自己的部分 | 也修系统的部分 |

- **一个判断**：某个操作是不是系统级，由 `broker::classify`（扩展后）判断；CLI 用它决定要不要 sudo，broker 用它决定怎么代办，规则只有一份。
- **只对"home 不是自己的"适用**（`src/cli.cpp:2110` 的 `home_is_ours`）：宿主上个人的 home 照旧，不需要 sudo。
- **E1**：在 `shared` 下，`sudo xlings update` 更新的是这台电脑上共用的索引。这件事在每个系统里只说一次（记下"已说过"），说法是"这台电脑上的系统共用一份索引"。
- **agent**：agent 回答不了密码。agent 工作区里的系统级操作照旧进 broker 的审批队列；agent 模式下直接报出需要人来做的那条命令，不等待输入。

### 3.3 broker：所有 carrier 共用

1. **guest 侧**：guest agent 在 `/run/xlings/broker.sock` 上监听；它启动的会话带 `XLINGS_SUBOS_MODE=machine`；`broker::available()` 同时认
   `sandbox` 与 `machine`。客户端（`broker::forward`）不变。
2. **传输**：是 carrier 消息流上的一条通道，内容是字节流加上终端尺寸与退出码。沙箱沿用 unix socket 传 fd；machine、vz、WSL2 转发 stdio 的字节。
   能力名 `broker`（opencaps），在 hello 里声明。
3. **宿主侧**：从 `modules/subos/src/session/session.cpp:660-760` 抽出 `broker::serve(request, io)`，负责 classify、decide、Ask 入队，并在 pty 上执行；
   所有 carrier 的 supervisor 共用它。每次执行记为一次 openas 的 run：actor 是系统里的那个人，via 宿主的 luban。
4. **授权**：用这个系统的 policy。从窗口新建的桌面系统允许；agent 工作区为 ask。
5. **断开**：请求方的通道断了，宿主就像终端挂断一样结束这次执行（先 SIGHUP，再 SIGTERM），并记录下来。
6. **看到结果**：virtiofs 的 `/xlings` 用 `cache=auto`；broker 回报完成之前，guest agent 先让 `/xlings` 的目录项重新验证，客户端返回时就能看到新的内容。
7. **旧宿主**：不建 socket；客户端只说一行
   `系统范围的操作需要宿主上较新的 luban；只给自己装：xlings install <pkg>`。

### 3.4 Lua worker 在系统里（J1）

实测定因（K1，本地 machine）：9p 以 `cache=mmap` 挂载时不缓存 inode，目录项一没人引用就被删掉（`always_delete_dentry`），下一次查找得到一个新的 inode；
Landlock 的规则挂在旧的 inode 上，于是允许写的目录也被拒绝（EACCES）。只要规则目录一直被打开着，写入就成功。

1. **按挂载表判断文件系统类型**：`platform::filesystem_type` 按设备号查 `/proc/self/mountinfo`，`statfs` 只作后备；9p、virtiofs、NFS、SMB、FUSE 归为"远端"（`FsCaps::remote`）；`self doctor` 用同一个函数。
2. **私有目录放在本地**：在远端文件系统上时，私有目录与输出捕获放在 `platform::local_private_dir`：依次 `XDG_RUNTIME_DIR` → `/run/user/<uid>` → `<temp>/xlings-<uid>`
   （machine 的会话里前两者都没有，用 `/tmp`，是 tmpfs），只属于这个用户；都不行时才留在原处。
3. **围栏**：Luban 系统里没有 bwrap，machine 里仍用 Landlock。Landlock 的每个规则目录在被围住的进程存活期间一直以 `O_PATH` 打开（跨 exec），
   不缓存 inode 的文件系统也不会"忘掉"规则；`O_PATH` 不能读写任何内容。

### 3.5 输出

- 读不了的索引，一条命令只说一次，细节在 `-v` 里。
- 镜像探测、artifact 回退之类的过程信息只在 `-v` 里出现。
- 特权环境变量的提示按"包名 + 变量 + 值"判断是否已经说过（J27）。
- L2 断言默认输出里没有 `[warn]`。

### 3.6 多用户与多版本

- 每个用户有自己的用户视图。
- `xlings use node` 列出系统的版本与自己的版本，标出当前在用的是哪一个、来自哪里。
- 新用户的用户视图在第一次需要时建立（2026-10-20 §13.3）。

### 3.7 mcpp（mcpp 仓库）

- Luban 系统里的 `/` 按"已声明的根"处理（依据 `/etc/os-release` 里的 Luban 标识与 `/usr/lib/xlings-abi`）。
- 第一次 `mcpp run` 的规划时间由 L2 记录。
- **实现（M1，mcpp#831，2026.10.22.1）**：provider 的布局端口加两项（mcpp 不直接写 xlings 的路径，provider 边界）：`declared_system_root`
  （`ID=luban` 加 `/usr/lib/xlings-abi`）让 sysroot `/` 不再算"外来的"；`system_home` 读根记录 `/etc/xlings/usr`
  （`<home>/subos/<name>/root/usr`），在 `/xlings` 不存在时把版本里的 `upstream = ["/xlings"]` 解释为它——
  宿主上建的系统，home 在建它的那个 home 的路径上，不在 `/xlings`（J5 的第二条警告）。实机：`mcpp self doctor` 两条警告都没了，新项目第一次 `mcpp run` 4 s。

## 4. machine 的文件共享（D3）

- **新 T1 包 `xim:virtiofsd`**：进 `xlings.subos.tools`，在第一次需要时安装（§5.7）。
- **qemu 的参数**：`-object memory-backend-memfd,share=on` 加 `-numa`；每个共享配一个 `vhost-user-fs-pci` 和一个非特权的 virtiofsd（`--sandbox namespace`）；guest 沿用现有的 tag。
- **缓存**：`/usr` 的 generation 用 `cache=always`；`/xlings`、root 与 `/home` 用 `cache=auto`。
- **后备**：virtiofsd 不可用时退回 9p，只读的共享改用 `cache=loose`，`luban status` 里说一行。
- **验收（L5）**：输入红字时的 CPU、`xlings search` 的耗时、`ls -R /usr` 的耗时、Falkon 的启动时间，9p 与 virtiofs 各测一组。

### 4.1 实现（K6）

- **设置 `files`**：`virtiofs`（默认）或 `9p`（`luban config <name> --files`）；`luban status` 与 `luban config` 的输出里各有一行 `files`。
- **规划与执行分开**：`luban::vm::virtiofs_plan` 只算参数（每个共享一个 daemon 的 argv、qemu 的 memfd、`-numa`、chardev 与设备），`nine_p_args` 算 9p 的；启动时两套都写进 state.json（`virtiofsd`、`files_virtiofs`、`files_9p`），由 supervisor 先起 daemon、等它的 socket（3 秒），再起 qemu，qemu 退出后停掉 daemon。
- **后备的层次**：`--sandbox namespace` 起不来（主机限制了用户命名空间）就以 `--sandbox none` 再试；还起不来，就把参数换成 9p、去掉内核命令行上的 `luban.fs=virtiofs`，state 里写 `9p (virtiofsd did not start: supervisor.log)`。GL qemu 退回软件渲染时 daemon 重新起一次（每个 virtiofsd 只服务一个 qemu）。
- **只在硬件虚拟化下**：没有 KVM 的（模拟的）machine 留在 9p，state 里说明。CI 上 aarch64 的 TCG machine 经 virtiofs 读程序时 init（busybox）段错误；
  virtiofs 的好处也只在 KVM 下才显著。
- **安装时机**：第一次需要时装 `xim:virtiofsd`；装不上（索引里没有、这个架构没有构建）就写 `logs/machines/<name>/virtiofsd-install-failed`，一天之内不再试——否则每次启动都要多等约 8 秒。
- **缓存与上面的不同**：所有共享都是 `cache=auto`。generation 与 `/xlings` 在同一个共享（home）里，宿主机往里安装的东西 machine 要马上看到；`always` 要到重启才看得到。9p 的挂载参数未改。
- **权限语义**：在实机上对比过，非特权 virtiofsd 与 9p `security_model=none` 一样：`chown` 得到 EINVAL，`chmod` 成功，新文件的属主是宿主机用户。
- **测得（实机 KVM，同一系统）**：`find /usr -type f`（14287 个文件）9p 3955 ms、virtiofs 359 ms；读 400 个共享库（27 MB）9p 1041 ms、virtiofs 130 ms；启动时间相同（4.4–4.6 s）。
- **测试**：`LubanVm.AMachinesFilesOverVirtiofsAreADaemonAShare`（参数，LUBAN-MACHINE-FILES）；默认、未安装、daemon 起不来三条路径在实机上验证。

## 5. 设备、表面与传输：各规范各管一件事（D4、E5）

### 5.1 问题

帧、输入、声音、剪贴板、broker、文件，现在是每个 carrier 各自接的：帧挤在 virtio-serial 上（J11），声音经 guestfwd，文件走 9p，broker 只有沙箱有（J2）。
每多一个平台就多一套实现，每多一种设备就多一处特例。

### 5.2 分工

| 关注点 | 由谁定义 | 本轮 |
|---|---|---|
| 传输：有序的字节流，加上可选的共享区域（有名字、有大小、可以封口），可以多跳 | carrier 层：`modules/carrier` 的 `Transport` 接口；每个平台一个实现 | 新增 |
| 表面：帧、输入、剪贴板、指针、文本 | opensal | 0.6 新增 `SharedFrames`：帧放在传输提供的共享区域里，消息只发损坏的矩形；没有共享区域时，`'f'` 帧做行差分加 LZ4 压缩 |
| 设备：显示器、GPU、声音、输入设备，以及它们的模拟 | openhal | 0.3：machine 的虚拟设备作为设备列出（特征 `transport=shared\|stream`、`accel=virgl\|dxg\|none`）；新 kit `openhal-emu` 提供模拟设备 |
| 能力：一方能不能依赖另一方 | opencaps | 新能力名：`shared-frames`、`broker`、`store-own`、`virtiofs` |
| 代办：谁、替谁、做了什么 | openas | broker 的每次执行记为一次 run |

依赖方向不变：规范只依赖标准库；kit 不引用 xlings 与 luban；carrier 在 `modules/carrier` 与 `luban/os/vm`。

### 5.3 多跳传输

在 macOS 与 Windows 上，系统与 Luban OS 窗口之间隔着一台 Luban 宿主机（part 4 §6.2），所以是两跳。共享区域只在单跳里有效；中间的转发进程（宿主机里的 luban）
从上一跳的共享区域里读出损坏的矩形，压缩后发往下一跳。opensal 的协商逐跳进行，双方说的都是同一套消息。

### 5.4 各平台的实现表（E5：尽量都支持）

| 平台与方式 | 跳 | 共享区域 | 消息流 | 文件 | GPU |
|---|---|---|---|---|---|
| Linux 沙箱 | 系统 ↔ 窗口 | memfd | unix socket（可传 fd） | 绑定挂载 | render node |
| Linux machine | guest ↔ 窗口 | ivshmem | virtio-serial | virtiofs，9p 后备 | virgl |
| macOS 沙箱 | 系统 ↔ 宿主机（vz） ↔ 窗口 | 第一跳 memfd，第二跳没有 | vsock | virtiofs | 没有：vz 不给 Linux guest 3D |
| macOS machine | guest（vz） ↔ 窗口 | 没有 | vsock | virtiofs | 没有（同上） |
| Windows 沙箱 | 系统 ↔ 宿主机（WSL2） ↔ 窗口 | 第一跳 memfd，第二跳没有 | **hvsocket**（取代 `wsl.exe` 的 stdio） | 发行版内的 ext4、drvfs | **dxg**：Mesa 的 d3d12 驱动，`libd3d12`、`libdxcore` 由 Windows 提供（T0） |
| Windows machine | guest（宿主机里的 qemu） ↔ 宿主机 ↔ 窗口 | 第一跳 ivshmem，第二跳没有 | virtio-serial + hvsocket | virtiofs | 待验证：virgl 跑在 d3d12 上 |
| 真机 | 本机 | 本机 | 本机 | 本机 | DRM（§15） |

本轮在 macOS 与 Windows 上的工作：

- **都做**：broker（§3.3）、两种存放方式、消息传输加 LZ4、能力报告与 CI 的子集（§9.5）；Windows 的消息流改用 hvsocket。
- **Windows 沙箱的 GPU**：`xim:mesa` 加上 d3d12 的 Gallium 驱动；xdesk 的渲染器选择认得 `dxg`；没有 `/dev/dxg` 或驱动失败时退回软件渲染，并说明原因。
- **两项可行性验证**（E14 = B、E15 = C：CI 只用托管 runner，真机由维护者在发布后验证）：
  1. **macOS machine 的扫描输出**：移到 §15（E14 = B：托管 runner 起不了 vz，本轮没有能验证它的地方）。
  2. **Windows 嵌套 machine 的 virgl**：在宿主机里，GL 版的 qemu 用 d3d12 驱动的 surfaceless EGL。实现成"先探测、不行就软件渲染"：
     探测在 CI（WARP）上覆盖选择与退路，真 GPU 下的门槛（L4 的 GUI 场景通过，且 L5 不比软件渲染差）由维护者在发布后用自己的 Windows 电脑验证；不达标时默认关掉，写进能力报告。
- **K14 实施**：探测与退路。WSL 的 GPU 是 `/dev/dxg`（Mesa 的 d3d12 驱动在它上面画），但 qemu 的 virgl（`egl-headless,rendernode=`）
  与 wlroots 的 GLES 渲染器都要一个 DRM render node，WSL 没有；所以 machine 与 xdesk 都探测到 `/dev/dxg` 时如实说"没有 render node、用软件"，
  而不是含糊的"没有 GPU"（`luban status`、关于页、`xdesk msg state` 的渲染器一行）。真要在 d3d12 上跑 virgl，需要 qemu 的 surfaceless EGL
  路径（不经 GBM），写进 §15，由维护者在真 Windows GPU 上验证后再做。
- **做不到的**（vz 下 Linux 的 3D、第二跳的共享内存）：写进能力报告（part 4 §6.3）与 CI 报告，原因与下一步都要写明。

### 5.5 帧

- **有共享区域**：双缓冲，按最大输出预留；memfd 加 `F_SEAL_SHRINK`；消息只发损坏的矩形（`'m'` 宣告区域，`'r'` 报告矩形）。
- **没有共享区域**：`'f'` 帧只带损坏的矩形，做行差分加 LZ4。
- **沙箱**：Luban OS 应用与 xdesk 之间另开一条可以传 fd 的 unix socket。
- **区域的生命周期**：窗口关掉再打开时，区域留在系统的运行目录里，重新宣告即可；尺寸超出预留时重新协商。

### 5.6 GPU 合成的默认

- 每个平台按 L5 的预算，比较"GPU 合成 + 读回"和"软件合成"，默认用更好的那一种；关于页写明实际用的是哪种，以及原因。
- 在测出结果之前，或者 carrier 没有共享区域时，machine 的桌面默认用软件合成，并如实说明这时应用也只能用软件渲染。

### 5.6a 桌面的流畅度（J30）

先测清楚，再改；目标写成预算，进 L5（门禁按 E4：先只报告，稳定后回归就算失败）。

- **分段测量**：帧统计（§8.11）分成四段：guest 里的合成、读回（GPU 时）、传输（字节与耗时）、宿主的上传与显示，每段都有耗时；
  `xdesk msg debug hud` 在屏幕上显示各段的数值。
- **预算**（1080p，machine，软件合成；实施时实测后定最终数值）：
  - 拖动窗口与滚动：≥ 50 帧/秒；
  - 终端刷屏：≥ 30 帧/秒，宿主的 CPU 不超过一个核的 60%；
  - 待机：不发帧。
- **在哪里验收**：
  - 绝对值：在参考机器上本地验收（`xdev test --lane-config luban-perf`），参考机器写进报告；
  - CI：托管 runner 的性能波动大，只比较 CPU 时间与帧数，取多次运行的中位数，与 main 的基线比；先只报告，稳定后退步超过 25% 就算失败。
- **改进按顺序做，每一步都用预算验收**：
  1. 共享区域（§5.5）：去掉 virtio-serial 的带宽上限；
  2. 只发损坏的矩形：取消"超过 8 块就发整帧"，改为合并相邻的矩形；
  3. 宿主只上传变化的部分：SDL 纹理按矩形更新；
  4. 按显示的节奏出帧：宿主显示完上一帧再要下一帧（已有 paced，检查它在 machine 里是否生效）；
  5. GPU 合成的默认由 §5.6 的对比决定。
- 每一步做完后，记录 L5 的前后对比（数值写进 PR 描述与 luban-os-ci 的报告）。
- **实施（K7，本轮）**：
  - opensal 0.6 `Compressed`：窄通道（machine 的串口、PC 宿主机的中继）的提供方声明它，消费方（xdesk、opensal-wlr 的桥）把帧的行压成一个
    LZ4 块（`'z'`），更短才发；LZ4 块格式在规范模块里自己实现（只依赖标准库，规范仍是一个），桌面画面压到十分之一以下（单测）。
  - 损坏矩形：取消"超过 8 块就发整帧"，改为把相距最近的两块反复合并到 8 块（超过 64 块才整帧）；`xdesk.frame.compress_ns`、`bytes_saved` 进 telemetry。
  - 宿主按矩形更新纹理、按显示节奏出帧：已经如此（`Display::take` 给出矩形，paced）。
  - **共享区域**改走 openbus 的 region（part 2 §7a：Luban OS 窗口是总线上的显示设备，xdesk 以 stream 打开它，帧放进 memfd 区域、只发引用；
    跨不了内存的一跳自动退化为 inline 并计数）——XBus 的 region 已就绪（B4），显示设备的接入是下一步，本轮不做 ivshmem。

### 5.7 设备模拟与依赖

- **`openhal-emu`**：可以设定尺寸、密度、热插拔的显示器；记录样本的声音输出；输入注入；剪贴板。它通过 openhal 的一致性测试；L0、L1 与 luban-os-ci 都用它。
- **machine 层面**：另用 QEMU 模拟的硬件（virtio-gpu、声卡、USB）。
- **迁移**：现有的 agent 帧（`'D'`、`'X'`、`'O'`）映射到新的通道上；hello 里声明能力；旧的一方照旧工作。声音本轮保持现有的路径（machine 经 guestfwd 到宿主的声音服务），改走 openhal 的 audio 通道放到 §15。
- **依赖的安装时机（J26）**：`xim:qemu-system-gl`、`xim:virtiofsd` 在授予 `gpu` 时、或第一次以 machine 运行时安装；进度里只显示一行。

## 6. 自带软件

### 6.1 包自己声明检查（D9）

- 沿用版本已有的 `checks` 格式（`luban.checks`：`id`、`what`、`run`、`timeout`），放在包的配方里：`extensions.luban.checks`。
  一个版本的检查 = 它自己的 + 它依赖闭包里所有包的。
- 新增三种：
  - `gui`：在桌面里启动它，`timeout` 之内窗口映射出来，并且进程还在；
  - `launcher`：这个应用出现在启动器里，按 `xdesk msg state` 返回的应用列表判断；
  - `sound`：执行后，宿主侧录到非静音的样本（§6.3）。

  J25 里的旧检查用 `ls` 看文件在不在，所以 J28 发生时它照样通过；新的检查看的是用户实际看到的结果。

```lua
extensions = { luban = { checks = {
    { id = "falkon-runs", what = "it starts", run = "falkon --version", timeout = 20 },
    { id = "falkon-window", what = "it opens a page", gui = "falkon file:///usr/share/luban/checks/page.html", timeout = 30 },
    { id = "falkon-listed", what = "the launcher lists it", launcher = "org.kde.falkon" },
} } }
```

- **由谁执行**：`luban status --check` 执行 `run` 与 `launcher`，都很快；CI（L3）执行全部。
- **lint**：xim-pkgindex 要求 `luban:` 下有 `programs` 的包、以及被版本带上的程序包，都声明了检查。

### 6.2 修

- **J28**：给环境变量前置目录时，如果原值为空，按变量的规范默认值处理：`XDG_DATA_DIRS` 是 `/usr/local/share:/usr/share`，`XDG_CONFIG_DIRS` 是 `/etc/xdg`。
  默认值只有一张表：`platform::list_default` 与 `platform::prepend_to_list`（放在 platform，luban 的 vm 也能引用）；
  程序的 shim、SubOS 会话（`apply_subos_env_`）、`subos use` 打印的 sh 与 fish 代码、machine 的 qemu 环境都用它。
- **J9**：`xim:xdesk` 链接 `share/applications`，并升 `revision`。
- **J12（Falkon）**：先修 J28，再在 machine 里复现：xdesk 与 LXQt、GPU 开与关都要试。依次检查：
  1. 系统里 `luban:falkon` 的版本，以及 ICU 数据与 `ICU_DATA` 是否生效；
  2. `dmesg` 里 `QtWebEngineProc` 崩溃的位置；
  3. `/dev/shm` 是否挂载且可写；
  4. 字体缓存目录是否可写，是否每次都在重建；
  5. 9p 与 virtiofs 下的启动时间。

  验收：打开本地页面、新标签页，以及经测试代理打开一个外部网址，都不崩溃；启动时间纳入 L5。

  **实现（P5）**：在 KVM machine 里复现（xdesk 用 wlroots 的 headless 后端，Falkon 开在上面）：主进程 exit 139，跳到 `0x9276`；
  去掉 GPU、`--disable-gpu`、软件 GL 都一样。`QT_DEBUG_PLUGINS` 显示崩溃前最后加载的是 `vulkan`：Falkon 的树用自己的 musl 加载器，
  musl 找不到库时退到默认的 `/lib:/usr/local/lib:/usr/lib`，而 Luban 系统的 `/usr/lib` 是 glibc 的（桌面版带 vulkan-loader），
  glibc 的库进了 musl 进程。宿主上没有 libvulkan，所以宿主上不崩。修法在组件这一层：`luban_component.config` 在每个
  `libexec/<name>/lib/ld-musl-<arch>.so.1` 旁写一个空的 `libexec/<name>/etc/ld-musl-<arch>.path`（musl 在那里读它，空表示没有系统路径），
  所有带加载器的组件升 revision。修后本地页面 1.1 s 打开，两个标签页都不崩；9p、virtiofs 都一样。
- **J10**：提示还在显示时，`resize` 与 `set_scale` 之后按新的尺寸重画。

### 6.3 音频播放器与测试用的音乐（J31）

- **播放器**：桌面版自带一个轻量的音频播放器，做法和 Falkon 一样，打包在 `luban:qt-runtime` 上。建议用 **Audacious**（`luban:audacious`）：
  Qt 界面，体量小，播放列表、常见格式都有，Alpine 有现成的包（E11）。
- **音乐**：建议用 **Open Goldberg Variations**（Kimiko Ishizaka 演奏的巴赫《哥德堡变奏曲》，作者以 CC0 放弃版权）里的《咏叹调》，Ogg Vorbis，约 4–5 MB。
  - 打成数据包 `luban:sample-music`，资源放在 xlings-res；
  - 装到 `/usr/share/luban/samples/music/`，桌面版默认带上；
  - provenance 写明出处与许可（实施时以发布页上的许可原文为准）。
- **检查**（§6.1 新增一种 `sound`：播放后，宿主侧录到非静音的样本）：
  - `luban:audacious` 声明 `run`（`audacious --version`）、`launcher`、`gui`（打开示例音乐，窗口映射出来），以及 `sound`
    （`audacious -H -q <示例>`，无界面播放到结束，宿主侧记录到非静音的样本）。
  - **宿主侧怎么录**：沙箱与 machine 的声音最终都到达宿主的声音服务（machine 经 guestfwd）。CI 的 harness 在宿主上起一个只录音的声音服务（null sink，录它的 monitor），断言录到非静音样本；L0、L1 里用 `openhal-emu` 的记录器。
  - L3 执行这些检查；L4 在桌面里用播放器播放并核对录音。
  - `sound` 只在 CI 执行：真实的系统里没有宿主侧的录音，`luban status --check` 不跑它。
- **上手应用**：最后一步可以选"播放一段音乐"，顺便确认声音是通的。

### 6.4 剪贴板（J13）

L4 覆盖以下几条路径：宿主 → foot（Ctrl+Shift+V）、foot（Ctrl+Shift+C）→ 宿主、Super+C/V、Falkon 文本框里的 Ctrl+C/V。

## 7. 创建流程：进度、取消、名字、记录

### 7.1 进度（J6）

- 一个阶段只在下一个阶段开始、或者整个操作结束时记满。
- 阶段内的百分比来自 Progress 事件的 percent，最多到 99%。
- 万一出现阶段已满但操作还没结束，显示"仍在进行：<最近一步>"。

### 7.2 额外的包

`create` 与 `configure` 里的安装改走能力 `install_packages`：有事件，能取消。
- 实施：`install_packages` 加参数 `subos`（接口协议 1.17，特性 `install_into_subos`）；系统在它的域里时，同一能力在域里执行，事件经
  `relay_capability_` 原样转发（`run_domain_relayed_` 也改用它，域里出来的事件只有一条路）。
- luban 的 `Port::install` 在 xlings 声明了这个特性时用它，否则退回原来的终端方式（旧 xlings 不认识 `subos`，不能让它装错地方）。

### 7.3 取消与名字（J7）

- **取消**：
  - 先发 SIGTERM。xlings 收到后完成当前这个原子步骤，记下状态，写 `end`（`cancelled`），然后以 130 退出；
  - 10 秒内没有退出才发 SIGKILL；
  - 之后把做了一半的移到 `.unfinished-<名字>-<pid>`，名字立即可以再用。
- **名字预留**：记"进程号 + 任务号 + 开始时间"；同一个进程里的其他任务撞上时，说"`<名字>` 正在被创建"；只有进程已经不在了才算过期。
- **占用的判断要一致**：完整的系统、只有记录的残留、域里的半成品、名字预留，luban 和 xlings 认同同一组，每种都有明确的说法和对应的命令。
- **清理**：`luban ls --all` 列出半成品；`luban rm <名字>`、`luban rm --unfinished` 确认后删除；`self doctor` 只报告不删除。
- **测试**：在每个阶段取消，然后用同一个名字重建。
- 实施：`platform::stream::run` 取消时先对进程组发 SIGTERM，10 秒后才 SIGKILL；`xlings interface` 把 SIGTERM 变成调用的取消，
  并置进程级的 `stop_requested`：域里不再启动新的进程，不再改到别处去做，半成品移到 `.unfinished-<名字>-<pid>`，说一次 `E_CANCELLED`，以 130 退出。
  名字预留另记本进程正持有的名字，同一进程里的第二个任务得到"正在被创建"。

### 7.4 记录

- 内层操作继承外层的 `XLINGS_TRACE_ID`，外层日志写一行 `link` 指向内层；`luban status <名字> --log` 两边都读。
- 被取消的操作写 `end`，带 `cancelled` 与当时的阶段。

## 8. xdesk 交互

### 8.1 上手应用（D8）

- **形式**：`xdesk-tour`，xdesk 自带的小客户端，用 xdesk 的绘制库与 `wl_shm` 画。
- **何时自动打开**：在新建的工作区里打开，**每个人在每台电脑上只自动打开一次**。完成或跳过的记录写在 Luban OS 应用的设置里，经 opensal 的 hello 告诉 xdesk；
  在真机上没有 Luban OS 应用，按每个用户记录。之后随时可以从应用里再打开。
- **怎么推进**：每一步都要用户真的做一次，合成器通过 `xdesk msg subscribe` 推送事件来推进；每一步都可以跳过。
- **步骤**：Super → Super+Enter → Super+Tab、Alt+Tab → Super+数字 → Super+O → 把窗口拖进工作区 → 点书签 → `Super+S` 折叠 → Super+/ → "设置"。
- 一次性的提示保留为后备。
- **实施（K13）**：导览由合成器自己画（右下角一张卡片），不另起 Wayland 客户端：它要看的正是合成器自己发出的事件（`launcher`、`action`、
  `view`、`drop`、`bookmark`、`collapse`……），放在合成器里没有进程间的同步，也不必为一个客户端再带一套 Wayland 客户端库。
  记录在每个人自己的 `state.json`（`toured`）：一个系统里的一个人一次；经 opensal hello 由 Luban OS 应用转达"这台电脑已看过"留到以后
  （要先给 opensal 的 hello 加字段）。结束得早，就退回原来那一行按键提示。`xdesk msg tour start|skip|end`，状态里的 `tour`。

### 8.2 切换器的突出（J15）

- 去掉外圈；悬停与选中都直接高亮窗口自己的边框（包括标题栏），悬停用浅色，选中用强调色。
- 去掉其他窗口"先暗后亮"的过程；选中后，那个窗口的边框高亮在原位 0.5 秒内淡出。
- 开了"减少动态效果"时直接消失。

### 8.3 WuXian 里的切换器（J14、J20）

- 新增 `spread_layer`，铺一块全屏的变暗矩形；聚合时把窗口移进这一层。
- 列出所有窗口，并标明每个窗口属于哪个工作区，或者是自由窗口。
- Alt+Tab 按最近使用排序，Super+Tab 按应用分组。

### 8.4 被盖住的项：书签（D5、E2）

- **统一**：工作区就是特殊的窗口。平面上的工作区与自由窗口共用一个 z 序；层叠工作区里的窗口有自己的 z 序。两层用同一套规则。
- **"完全被盖住"**：这一项的边框（包括标题栏或标签）在屏幕上没有一处露出来。
- **书签挂在哪**：盖住它中心点的那些项里，最上面的一项在右边缘伸出书签。
  - 窗口的书签：图标 + 简短的标题；
  - 工作区的书签：颜色 + 编号 + 名字；
  - 纵向排列，最多 5 个，其余合成一个"+N"，点开是列表。
- **交互**：
  - 悬停书签：画出被盖住那一项的轮廓；
  - 点击书签：把它浮到最上面并聚焦；
  - 书签不能拖动；
  - 拖动、缩放镜头的过程中先隐藏书签，停下后再显示；
  - 设置里可以关掉书签。
- **计算**：每次排布后重算。用 pixman 的区域做遮挡运算，n 个项的开销是 O(n²)；n 通常在一百以内，每次 `arrange` 花在这上面的时间放进 L5 的预算。
- **规范**：书签写进 openwx 的"基本交互（建议性）"一节，命中次序不变。

### 8.5 折叠（J21，D6）

- **方式**：双击标题栏或标签、右键菜单，或者 **`Super+S`**（修订 1 提议的 `Super+Shift+H` 已用于"向左移动"，`Super+S` 没有被占用），也列在 Super+/ 里。
- **折叠后**：工作区只占标签那么大，里面的窗口不显示、不参与命中；折叠的窗口不接收输入，焦点跳过它。
- **openwx**：窗口加 flag `collapsed`，工作区加字段 `collapsed`，不升格式版本。

### 8.6 自由窗口缩放（J17）

去掉 WuXian 视图里的这项排除；边框按屏幕像素至少有 6 px 可以命中。

### 8.7 标题栏命中（J18）

标题栏至少有 24 px（屏幕像素）可以命中；缩放低于 0.4 时，整个窗口都可以拿来拖动，双击放大回 1:1。

### 8.8 越出显示区域（J22，D7）

- **层叠**：标题栏至少留出 64 px 或宽度的 1/4（取较小者），不能高过顶边，也不能压到状态栏下面。
- **WuXian**：进出工作区以松开时指针的位置为准；放进层叠工作区后，超出工作区边框的部分被裁掉。

### 8.9 小地图（J19）

拖动视口框可以平移；点窗口或工作区，就聚焦并居中；拖窗口可以移动它，落点规则同 §8.8。

### 8.9a 实施（K11）

- **覆盖的计算**放在 `xdesk.layout`（纯逻辑、可单测）：`layout::covered(从后到前的矩形, 屏幕)`，逐项减去其上各项（矩形差，O(n²)，
  一百项在一帧以内，单测断言）；不足 4 像素的细条不算"露出"（实测：层叠窗口在最大化窗口下方露出 1 像素，人无法抓住它）。
  合成器只取各项的屏幕矩形（WuXian：平面上的层按 z；工作区视图：层叠工作区的窗口按前后）并画书签；拖动、平移、缩放中隐藏，
  静止 250 ms 后再算。`xdesk msg state` 的 `bookmarks`，以及 `xdesk msg bookmark N`（Agent 无法点击，所以有同样效果的请求）。
- **折叠**：openwx 的工作区字段 `collapsed` 与自由窗口的 flag `collapsed`（不升格式版本）；布局里折叠的工作区只出标签条、不放它的窗口，
  折叠的自由窗口只有标题栏高、不给客户端发尺寸。双击标签仍是"进入"（已有的、文档写过的行为不改），所以工作区的折叠走菜单与 Super+S；
  折叠后的标签条双击是展开；自由窗口在 WuXian 里双击标题栏是折叠。
- **图片模式的阈值**沿用已有的 `live()`（缩放 < 0.5 即为图片），不另设 0.4，避免两个相近的阈值。
- **越出**：工作区视图里层叠、浮动窗口拖动时保留标题栏。WuXian 里层叠工作区窗口超出边框的部分裁掉——**本轮未做**：wlroots 的
  `wlr_scene_subsurface_tree_set_clip` 会重设每个缓冲的目标尺寸，与 WuXian 的缩放（逐缓冲设目标尺寸）冲突；需要先把缩放改为整树变换，
  记在 §15。

### 8.10 宿主的键盘与指针（J16）

- **抓指针**：键盘已锁定、Super 按下、指针在窗口里时，抓住指针（X11 用 `XGrabPointer`），Super 松开就放开。
  只在这三个条件同时成立时抓，不影响平时把指针移出窗口。
- **核对按键**：获得焦点、抓取状态变化、或者抓取被宿主打断时，读出宿主当前按着的键（X11 用 `XQueryKeymap`，Wayland 用 `wl_keyboard.enter`），对系统里多出来的按键补发松开。
- **提示**：没有锁定键盘时第一次按 Super 组合键，提示一次 Ctrl+Alt+K。

### 8.11 可观察性（J24）

- **`xdesk msg state --json`**，包含：
  - 输出与渲染器（以及选它的原因）、镜头；
  - 工作区、窗口（包括是否被盖住）、书签、浮层、启动器里的应用；
  - 通道（传输、有没有共享区域、节奏）；
  - 近 10 秒的帧统计。
- **`xdesk msg debug hud on|off`**：在屏幕上显示调试信息。
- **宿主侧**：经 openbus 取回同一份数据（part 2 §7：`…/service/xdesk/state`，`luban status <名字> --bus`），不再另开 `--desktop`。
- **实施**：帧的分段测量（合成、读回、发送、字节、整帧）在 `render()` 里记录最近 10 秒，`state` 的 `frames` 与 HUD 都读它（§5.6a 的第一步）。
- e2e 能断言状态的地方就断言状态。

### 8.12 控制台（J23）

只在内容变化时重画，最多 60 帧/秒。

### 8.13 新建页的档位角标（J29）

4 个小方块放在卡片的右上角，与卡片边缘留一个内边距；图标仍在左上角；卡片被选中抬起时，角标跟着一起移动。

## 9. 独立的 Luban CI：`luban-os/luban-os-ci`（D10、E3、E4）

### 9.1 仓库与分工

- **新建**：由 Sunrisepeak 在 `github.com/luban-os` 下新建（这个组织已经存在，目前只有 `wlroots`）。
- **luban-os-ci 里放**：
  - 场景文件（数据驱动）；
  - harness：驱动被测版本的 `luban`、`luban-os --headless --script`、`xlings`、`xdesk msg`，不链接 xlings 的模块；
  - 矩阵、设备模拟的配置、性能预算、报告；
  - 它自己的 `requirements.toml`。
- **留在 xlings**：L0、L1，以及贴近代码的 e2e。

### 9.2 设备模拟

| 测什么 | 怎么模拟 |
|---|---|
| CPU 与启动 | `/dev/kvm`（加 udev 规则让 runner 用户可用）；aarch64 用 TCG |
| 显示 | `openhal-emu` 的显示器，用 `luban-os --headless` 截图；machine 再用 QMP `screendump` |
| GPU | 宿主的 llvmpipe 上跑 virgl；Windows 上用 WARP 跑 d3d12 |
| 声音 | 宿主上只录音的声音服务（null sink 的 monitor），沙箱与 machine 共用；L0、L1 用 `openhal-emu` 的记录器 |
| 输入 | 脚本的 `key`、`type`、`click`、`commit`；QMP `input-send-event` |
| 剪贴板、网络、磁盘、USB | 脚本；用户态 NAT 与 `restrict=on`；测试代理；virtio-blk、`usb-storage`、`usb-tablet` |

### 9.3 层次

| 层 | 内容 | 在哪 |
|---|---|---|
| L0 | 单元测试：挂载表 fixture、classify、环境变量默认值、进度、按键核对、书签计算、opensal 与 openhal 的一致性 | xlings |
| L1 | 沙箱 e2e | xlings |
| L2 | 系统里的 xlings（§9.4） | luban-os-ci |
| L3 | 包的检查（§6.1）全部执行 | luban-os-ci |
| L4 | 桌面 GUI：启动器与"设置"、foot、Falkon 的页面与新标签页、剪贴板、Super/Alt+Tab、书签、折叠、上手应用；软件渲染与 virgl 各跑一遍 | luban-os-ci |
| L5 | 性能预算：待机、刷屏、拖动、输入红字、帧通道的流量、9p 与 virtiofs、Falkon 启动、书签计算；CI 用 CPU 时间与中位数做相对比较，绝对值在参考机器上本地验收（§5.6a） | luban-os-ci（托管 runner） |
| L6 | 已发布的版本：fresh 安装、从上一版升级、导出 ISO 后启动并跑 L2 | luban-os-ci |

### 9.4 L2 的场景

用 `desktop`、`core` 两个版本，`shared`、`own` 两种存放方式运行 machine，以默认用户登录：

1. `xlings --version`；`xlings search node`。
2. `xlings install -y xim:jq` 后可用；`remove` 后不可用。
3. 不加 sudo 的 `xlings update` 被拒绝，并给出 `sudo xlings update`；`sudo xlings update` 成功；默认输出里都没有 `[warn]`。
4. `sudo xlings install -y xim:tree`，另一个用户也能用。
5. 多用户、多版本：
   - alice 用 node@22，bob 用 node@24；
   - alice 不加 sudo 执行 `xlings use node 24`，只有她自己变了；
   - `sudo xlings use` 只影响没有自己版本的用户；
   - 第三个用户看到的是系统默认。
6. `mcpp new demo && mcpp run`；`mcpp self doctor` 没有警告。
7. `xlings self doctor` 干净。
8. 宿主上 `xlings install --subos <名字> xim:xz`，系统里马上可用。
9. persist 为 `home` / `none` 时重启后的结果符合设定。
10. 断网时只说一行；配置代理后能装。
11. fish、bash 的 PATH 正确；`XDG_DATA_DIRS` 里有 `/usr/share`。
12. `luban upgrade` 之后，之前装的包还能用。
13. `--store own` 后再切回 `shared`，1–5 的结果都不变。
14. agent 工作区：3、4 进入审批队列，宿主 approve 之后完成。

### 9.5 矩阵与宿主平台（E5）

- **x86_64**：版本（`tiny`、`core`、`desktop`、`agent-workspace`）× 模式（`sandbox`、`machine`）× 存放方式 × 渲染器。
- **aarch64**：TCG，只跑启动与 L2 的 1–3。
- **原则**（E7）：只用 GitHub 托管的 runner；luban-os-ci 是公开仓库，标准 runner 不花钱。每个平台在托管 runner 上能真实跑到哪一层就跑到哪一层；
  跑不到的地方，用 carrier 抽象换一种能跑的方式，覆盖同样的逻辑；还剩下跑不了的，在报告里写明原因与对应的覆盖方式。
  每个依赖环境能力的作业，第一步先探测（`/dev/kvm` 存在、`kvm-ok`、`qemu -accel kvm` 能起来、`VZVirtualMachine.isSupported` 等），
  探测失败就**明确失败**，不能让一次什么都没测的运行显示成通过（§17 的教训）。

| 平台 | runner | 能真实跑的 | 换一种方式覆盖的 | 跑不了的 |
|---|---|---|---|---|
| Linux x86_64 | `ubuntu-24.04`（有 `/dev/kvm`） | 全矩阵：sandbox、machine、两种存放方式、virgl（llvmpipe）、L2–L5 | 两跳：用 qemu 起一台 Luban 宿主机，系统在里面以沙箱方式运行，窗口在 Linux 宿主上 | 真 GPU 的性能（E15） |
| Linux aarch64 | `ubuntu-24.04-arm` | 构建、单元测试、沙箱 | machine 用 TCG，只跑启动与 L2 的 1–3 | KVM |
| Windows x86_64 | `windows-2025` + `Vampire/setup-wsl@v7`（WSL2） | **wsl2 carrier 的真实路径**：Luban 宿主机就是 WSL2 发行版，系统在里面以沙箱运行；**嵌套 machine**：WSL2 里有 `/dev/kvm`（第二层嵌套在托管 runner 上实测可用），qemu + KVM 启动 machine；hvsocket；Windows 原生的 Luban OS 应用（无头脚本）与 `luban`；L2 的 1–8、L3、L4 子集 | GPU：先探测 `/dev/dxg` 与 d3d12（WARP）；没有就用 llvmpipe 跑功能，`dxg` 的选择逻辑用单元测试 | 真 GPU 下 d3d12 与嵌套 virgl 的性能（E15） |
| macOS arm64 | `macos-15` | 构建、单元测试、原生的 Luban OS 应用（无头脚本）与 `luban`；vz 的配置对象（构建并 `validate()`，不启动） | **测试用的 qemu carrier**：托管的 macOS runner 本身是虚拟机，不能启动 vz 虚拟机，也没有 Hypervisor.framework；改用 qemu（TCG）起一台小的 Luban 宿主机，覆盖 macOS 原生一侧的两跳、只走消息的传输、broker、两种存放方式、能力报告 | vz 的真实启动、virtiofs 经 vz、vz 扫描输出（E14） |

- 测试用的 qemu carrier 只在 CI 与本地测试里启用（`LUBAN_TEST_CARRIER=qemu`），不出现在用户的能力报告里；它与 vz carrier 共用除"启动虚拟机"以外的全部代码，
  所以覆盖的是同一份逻辑，而不是另写一套。
- WSL 的已知风险：2026-10 的 Windows 累积更新 KB5127648 在 AMD 主机上破坏了嵌套虚拟化（§17）。托管 runner 在 Azure 上，是否受影响尚未有报告；
  探测步骤会让它明确失败，报告里写明。

### 9.6 触发、回报与门禁（E4）

- **xlings 的 PR 与 main**：构建完成后 `repository_dispatch` 到 luban-os-ci（带上 run id）；luban-os-ci 下载产物，跑按改动路径选出的子集，把结果作为 commit status 写回 xlings。
- **xim-pkgindex**：改到 Luban 用到的包时，用该分支的索引触发 L2、L3 的子集。
- **每天**：用已发布的版本加上 xim-pkgindex main 跑全矩阵，失败时开 issue。**发布前**：跑 L6。
- **权限**：Sunrisepeak 在组织下建一个 GitHub App，只能读两个仓库的产物、写 commit status；密钥只放在 luban-os-ci。
- **门禁**：先只报告；连续两周稳定后，把 L2 子集设为 xlings 的必需检查；发布必须通过 L6。

### 9.6a 实现（C1–C5 的第一版）

- **仓库**：`luban-os/luban-os-ci`（Sunrisepeak 新建）。harness 是一个只用 Python 标准库的程序（`harness/lci.py`），`./run.sh <层> [场景…]`
  在一台有 KVM 的 Linux 上与 CI 跑出同样的结果；被测的是 `--release <版本>`（下载发布包）或 `--bin <目录>`（某次构建，例如 xlings CI 的
  `xlings-linux-x86_64` 产物），索引可以是一个 checkout 或 xim-pkgindex 的分支（`--index`，按 §13 的规矩放进 home 里）。
- **场景**是数据：`scenarios/<层>/<nn>-<名>.toml`，每步 `run`、`in`（system、host、luban-os）、`user`、`exit`、`has`、`lacks`、`matches`、
  `max_lines`、`measure`、`read` + `limits`/`floors`（JSON 里的值，如 `perf.frames`）、`known`（有编号的已知失败：报告为 known，不算失败；
  它不再失败时报告提醒去掉）；`pending` 说明为什么还不能跑。`fixtures.toml` 定义系统（`luban new` 的参数与之后的 `then`）。
  `tests/test_scenarios.py` 检查格式、需求覆盖与 fixture 名。
- **探测**：`needs`（kvm、luban-os、xdesk、network、proxy）先查，查不过就失败并说明，绝不显示为通过。场景要代理时 harness 自带一个
  SOCKS5h 代理（这台电脑有 HTTP 代理时经它出去）。
- **第一版的覆盖**：L2 的 15 个场景（13 为 pending：K5；新增 15：同时执行的命令），L3 的检查（外面与里面），L4（窗口画出桌面、启动器、
  Falkon 打开页面；Audacious 等桌面版加入后），L5（virtio-fs 与 9p、空闲桌面的帧），L6（被测版本建系统、导出 ISO 并启动）。
- **它在本地第一次跑就找到的**（都已在本 PR 修好或记录）：两个 `luban run` 同时执行时，先结束的那个把 machine 关掉、另一个没有任何输出地结束；
  `--user` 写了系统里没有的用户时以 root 执行；系统的会话里 xlings 自己设的 `LUBAN_KIND` 等被当成 `--env` 检查、每次都 `[warn]`；
  `/etc` 里某个文件在挂载下已被删除时 bwrap 整个失败、目录随之读不出；machine 里新用户的 home 不归他（J32，记录，L2-05 标 known）；
  mcpp 的两条警告（mcpp#831，L2-06 标 known）。
- **触发**：`layers.yml`：dispatch（层、发布版本或 xlings 的 CI run、索引分支）、每天（全部层，失败开 issue）、本仓库的 PR（场景格式 + L2）。
  xlings 的 Linux CI 在发布候选构建好之后 dispatch L2（`luban-os-ci` 作业）；跨组织 dispatch 要维护者的 token（`LUBAN_OS_CI_TOKEN`），
  没有时作业说明并通过。回报用 GitHub App 写 commit status 留待维护者建 App（E3）；第一版的结果在 luban-os-ci 的运行与报告里。
- **还没有的**（下一步）：aarch64（TCG）、Windows（WSL2）、macOS（测试用 qemu carrier）各行；L6 的"从上一版升级"；L4 的剪贴板、
  Super/Alt+Tab、书签与折叠、上手应用；按改动路径选子集。

### 9.7 成本与缓存

payload 按"索引快照 + 版本 + 存放方式"缓存；PR 子集控制在 25 分钟以内。

## 10. 文档

- **`docs/luban`**（中英两版）：存放方式、sudo 与范围、多用户、各平台能做到什么（§5.4 的表）、machine 的文件与 3D、上手应用、书签、折叠、小地图、调试。
- **`docs/generated/`**：`luban config --store`、`luban status --desktop|--log`、`luban ls --all`、`luban rm --unfinished`、`xdesk msg state|debug|subscribe`。
- **规范**：opensal 0.6、openhal 0.3、opencaps 的新能力名、openwx。
- **part 4 设计 §6.4**：在旁边注明"由本文 §5 取代"。
- **luban-os-ci 的 README**。
- **AGENTS.md** 加三条：
  - 处处一致；
  - 系统级的 xlings 操作要 sudo；
  - 包要声明 `extensions.luban.checks`。

## 11. 测试

| 范围 | 测试 |
|---|---|
| platform | 挂载表解析（9p、virtiofs、bind、overlay、带转义的路径）；`statfs` 后备 |
| xvm、manifest | 空变量前置后保留规范默认值；三处结果一致 |
| xim | worker 的 tmpfs 链；特权环境变量去重 |
| broker | classify（root、home 是否可写、命令）；`serve` 的两种 io；断开时结束执行；旧宿主时只说一行 |
| 存放方式 | `shared ↔ own`：闭包完整、去重、取消后恢复原样、不碰 home |
| 传输 | 多跳转发：共享区域 → 压缩消息；无损；损坏矩形正确 |
| opensal、openhal | `SharedFrames` 在有共享区域与只有消息两种情况下结果相同；`openhal-emu` 通过一致性测试 |
| luban | 阶段进度；可取消的安装；SIGTERM 后收尾；每个阶段取消后同名重建；名字预留；`link` 与 `end` |
| vm | broker 通道；virtiofs 与后备；ivshmem 帧；依赖的安装时机 |
| Luban OS 应用 | 按键核对；抓指针的三个条件；控制台合并重画；角标的位置 |
| xdesk | 切换器；`spread_layer`；书签（判定、挂靠、"+N"、悬停、点击、拖动时隐藏、开关）；折叠与 `Super+S`；缩放；标题栏命中；越出；小地图；`state`；`subscribe`；提示的位置 |
| openwx | `collapsed` 的往返 |
| xdesk-tour | 由事件推进；可以跳过；每人只自动打开一次 |
| luban-os-ci | L2–L6 |
| xim-pkgindex | checks 的 lint；`xim:xdesk`；`xim:virtiofsd`；mesa 的 d3d12 |
| mcpp | sysroot |

改了已发布的输出文字时，同时更新 `tests/fresh-install/`。

## 12. 兼容

- **旧宿主 + 新系统**：不建 broker socket，只说一行。
- **新宿主 + 旧系统**：照旧工作。
- **opensal、openhal、opencaps**：新能力都经协商；旧的一方用原来的帧与 agent 消息。
- **openwx**：`collapsed` 放在旧读者会保留或忽略的地方。
- **存放方式**：现有的系统都是 `shared`，不需要迁移。
- **virtiofs**：下次启动时生效，系统本身不用改。
- **环境变量默认值**：宿主上的 shim 也一样受影响。没有设置 `XDG_DATA_DIRS` 的会话（比如 ssh 登录），以前会丢掉 `/usr/share`，现在按规范保留；原来有值的会话不变。
- **名字预留文件**：旧格式只有进程号，读入时按"进程不在就算过期"处理。
- **Windows**：hvsocket 取代 `wsl.exe` 的 stdio，旧的宿主机照旧走 stdio。

## 13. 任务与检查点

xlings（一个 PR，每个检查点一个 commit，每个都跑一次完整 CI）：

| 检查点 | 内容 |
|---|---|
| K1 | 两个根因：J1（挂载表、tmpfs 链、machine 里用 bwrap）与 J28（环境变量默认值）。先在 machine 里复现 J1 |
| K2 | sudo 与范围：classify、`update` 与 `use` 的规则、输出（J2–J4、J27） |
| K3 | 规范：opensal 0.6、openhal 0.3 加 `openhal-emu`、opencaps 的能力名；carrier 的 `Transport` 接口 |
| K4 | broker：抽出 `serve`；guest 侧；Linux machine、vz、WSL2 的实现；记成 openas 的 run；测试用的 qemu carrier（`LUBAN_TEST_CARRIER=qemu`，与 vz 共用除启动以外的代码） |
| K5 | 存放方式：`shared`、`own`、切换（复用闭包机制）、`own` 下的宿主管理 |
| K6 | machine 的文件：virtiofs 与 9p 后备；依赖的安装时机 |
| K7 | 帧与传输：分段测量与 HUD（先做），共享区域（memfd、ivshmem），只发损坏矩形并合并，宿主按矩形上传，检查节奏，多跳转发，LZ4，Windows 的 hvsocket，渲染器认得 `dxg`；每一步对照 §5.6a 的预算；GPU 合成的默认由 L5 决定 |
| K8 | 创建流程：进度、`install_packages`、SIGTERM 与收尾、`.unfinished`、名字预留、`link` 与 `end` |
| K9 | Luban OS 应用：按键核对、抓指针、控制台、角标 |
| K10 | xdesk 切换器（J14、J15） |
| K11 | xdesk 与 openwx：书签、折叠、缩放、标题栏命中、越出、小地图 |
| K12 | 可观察性 |
| K13 | 上手应用；提示的位置 |
| K14 | Windows 嵌套 machine 的 virgl：探测与退路（§5.4）；vz 扫描输出移到 §15 |
| K15 | L0、L1 的补充；dispatch 的 workflow |
| K16 | 文档、需求编号、版本号（三处一起改，先查是否已被占用）、`tests/fresh-install/` |

xim-pkgindex（一个 PR）：

| 检查点 | 内容 | 时机 |
|---|---|---|
| P1 | `xim:xdesk` 链接 `share/applications`，升 `revision` | 随时 |
| P2 | 各包的 `extensions.luban.checks` 与 lint | 随时 |
| P3 | `xim:virtiofsd`（加 xlings-res 的资源） | K6 之前 |
| P4 | `xim:mesa` 加 d3d12 驱动（Windows）——已在：25.0.7.2 起带 `d3d12_dri.so`（25.0.7.3 核对过）；wlroots 的 GLES 在 WSL 里仍要 GBM 设备，见 K14 与 §15 | K7 之前 |
| P5 | Falkon 的修复 | 能复现之后 |
| P5a | `luban:audacious`（打包在 qt-runtime 上，带检查）与 `luban:sample-music`（CC0 音乐，资源放 xlings-res，provenance 写明许可）；把二者加进 `luban:desktop` 的新版本 | 随时；版本指针随 P7 |
| P6 | dispatch 到 luban-os-ci | 跟 C5 一起 |
| P7 | 版本指针 | xlings 发布并推到 CN 镜像之后 |

mcpp（一个 PR）：M1 sysroot。

luban-os-ci（新仓库，一个初始 PR）：

| 检查点 | 内容 |
|---|---|
| C1 | 骨架、场景格式、harness、GitHub App |
| C2 | L2 |
| C3 | L3、L4 |
| C4 | L5、L6 |
| C5 | workflow、矩阵（§9.5 的表）；每个作业的能力探测、探测失败明确失败；Windows 的 WSL2 与嵌套 machine；macOS 的测试用 qemu carrier；报告与回报 |

**顺序**：
1. K1 最先；
2. P3、P4 的资源先于 K6、K7；
3. C1、C2 与 K2–K5 并行，用 xlings 分支的产物来验证；
4. 发布：xlings-res → xlings → CN 镜像 → P7 → luban-os-ci 的每日任务切到新版本。

### 13.1 依赖、阶段与联调

```
阶段 1 可用性        K1 ──► K2 ──► K4 ──► K5
                     │
                     └► P1、P5（Falkon 在 J28 修好后复现）
                     K8、K9、K10 互不依赖
阶段 2 架构          K3 ──► K7 ──► K14
                     P3 ──► K6          P4 ──► K7 的 dxg
阶段 3 xdesk         K3 的 openwx ──► K11      K12 ──► K13
                                       K12 ──► P2 的 launcher、gui 检查
阶段 4 覆盖与发布    K12、P2 ──► C1–C5 ──► K15 的 dispatch ──► K16
                     发布：xlings-res → xlings → CN 镜像 → P7 → luban-os-ci 的每日任务
```

- **为什么这样排**：
  - 阶段 1 先让系统里的 xlings 能用（J1–J4、J28），这是这次实测里最影响使用的；K4 的 broker 是 K5 `own` 下宿主管理的通道。
  - K3 的 `Transport` 是帧（K7）与多跳的前提，先定接口再实现。
  - `xdesk msg state`（K12）是 L3 的 `launcher`、`gui` 检查与 L4 断言的数据来源，所以先于 luban-os-ci 的 C3。
- **跨仓库联调**：
  - xim-pkgindex 的分支索引：xlings 分支的产物读该分支的索引，P1、P2、P5、P5a 在合入前就能在 L3 里验证；
  - 新的检查放在 `extensions.luban.checks`，旧的 luban 不读这个字段，所以 P2 可以先于 xlings 发布合入；
  - 资源（virtiofsd、audacious、sample-music、falkon 的新构建）先发 xlings-res 与 GitCode，再合入引用它们的配方；
  - P7 只在 xlings 发布、CN 镜像推完之后。
- **无感升级**（§12 的落地）：
  - 新宿主 + 旧系统：hello 里没有 `broker`、`shared-frames` 就走原来的路径；
  - 旧宿主 + 新系统：guest 的 broker socket 不存在时，客户端只说一行；
  - 现有的系统都是 `shared`，不迁移；名字预留的旧格式按"进程不在即过期"读；
  - `XDG_DATA_DIRS` 的默认值只影响原来为空的会话。

  每一条都有一个 L1 或 L2 的测试；从上一版升级的测试（`tests/fresh-install/`）在发布前跑。
- **本地即 CI**（E16）：luban-os-ci 的 harness 只调用程序，不依赖 GitHub 的环境变量；`./run.sh` 在有 KVM 的 Linux 上跑 Linux 的全部层次。
- **合入与发布的顺序**：xlings PR 的 CI 全绿 → 自审 → squash 合入（Sunrisepeak）→ 取消 main 的 CI，先跑 release → 资源一出来就在本地推 GitCode →
  重新触发 main 的 CI → 合入 xim-pkgindex 的 PR（含 P7）→ 用发布的版本真实验证全部生态。

### 13.2 发布后的验证轮（2026.10.22.2–.4）

2026.10.22.1 发布后，用发布的版本、发布的索引、真实的桌面（d1、d7）与托管 runner 上的 luban-os-ci 验证，
每发现一处就定因、修、测、发，直到全部生态通过。依赖链（每一步都要前一步发布并进了索引，才能被下一步验证）：

```
22.1 发布 ──► d7 升级桌面 ──► J34（xdesk 需要 GLIBCXX_3.4.35）
               ├─► xim-pkgindex#1024：新装钉回 xdesk 2026.10.21.3（只保护新系统）
               └─► xlings#694 = 22.2 ──► xim-pkgindex#1026：新版本 luban:xdesk、luban:desktop 2026.10.22.2
                                         （只升 revision 移不动已装的系统：要发新版本）
22.2 ──► d7 升级 ──► J35（第二次升级把包当成用户的）
     ──► luban-os-ci L2-02（本地）──► J36（第二个系统的用户装不了）
     ──► luban-os-ci L2-14（只在 runner）──► J37（私有域的请求主人看不到）
     ──► main 的 asan ──► 测试自身的越界读、空路径
         └─► xlings#695、#696 = 22.3 ──► xim-pkgindex#1029
22.3 ──► d1 升级、d7 切换 ──► J38（重新登记的发布多出成员，锁住别的系统的根）
     ──► luban-os-ci L2-02（只在 runner）──► J39（私有域 machine 的 broker 装错了 home）
         └─► xlings#697 = 22.4 ──► xim-pkgindex#1030 ──► luban-os-ci 全绿、d7 可用 ──► luban-os-ci#1 合入
```

- **跨仓库联动**：
  - 每个版本的顺序：xlings PR 全绿 → squash 合入 → 取消 main 的 CI → release → 本地 gtc 补 GitCode（xlings、luban 先推；luban-os、xdesk 等 CI 的镜像作业建好 GitHub 端的 release 再推）→ 重新触发 main 的 CI → xim-pkgindex PR（指针，必要时新的组合包版本）→ 发布后等 CDN → 真实验证。
  - 一个修复若还没发布就又发现了下一个问题（J37），取消尚未发布的 release，把修复并进同一个版本号，省一整轮镜像与索引。
  - luban-os-ci 的场景只在发现"它测的不是它说的"时改（L2-02 的 known 已失效；L2-14 与 L2-02 在失败时打印看到了什么）。
- **为什么本地没发现 J37、J39**：这台机器建不了私有命名空间，系统都落在 home 自己的前缀里；runner 上 agent 工作区与桌面在私有域里。
  本地 `./run.sh` 覆盖前缀路径，私有域路径只有 runner 覆盖——所以 luban-os-ci 在托管 runner 上跑是必要的，而不是重复。

### 13.3 验证记录（2026.10.22.4）

| 什么 | 在哪里 | 结果 |
|---|---|---|
| xlings 的全部 CI（Linux 含 asan 四片、macOS、Windows、aarch64、host matrix） | main（e88c2638） | 通过 |
| 从发布的版本全新安装、luban 的 fresh 套件 | `xlings-ci-fresh-install`、`xlings-ci-luban-fresh`（22.3、22.4） | 通过 |
| luban-os-ci L2（15 个场景，三类系统） | GitHub 托管 runner，最新发布 | 13 通过、0 失败、5 known（J32、J33、mcpp#831）、1 pending（K5） |
| 升级旧桌面：edition、xdesk 自动切换、xdesk 在系统绑定的 C++ 运行时上运行 | 本机 d1（desktop 2026.10.21.3 → 2026.10.22.2） | 通过（`xdesk --probe` 出帧） |
| 二次升级、手动降级后的恢复、`luban status --check` | 本机 d7 | 通过（可用，70 项 66 过；4 条非关键提示见 §15） |
| 一个用户在两台系统里装同一个包 | 本机 L2-02（前缀）、runner L2-02（私有域） | 通过 |
| agent 工作区的审批 | runner L2-14（私有域） | 通过 |

## 14. 自审（修订 2，按视角）

| 视角 | 发现 | 处理 |
|---|---|---|
| **架构**：归类 | 修订 1 把显示、输入、剪贴板、broker 都塞进 openhal，而表面早就由 opensal 定义，broker 也不是设备 | §5.2 按"各规范各管一件事"重新分工：传输归 carrier，表面归 opensal，设备与模拟归 openhal，能力归 opencaps，代办归 openas |
| 架构：多跳 | macOS 与 Windows 上隔着一台宿主机，"每个 carrier 一种传输"的说法不成立 | §5.3：共享区域按跳生效，转发进程负责转换；opensal 逐跳协商 |
| 架构：单一来源 | "要不要 sudo"和"broker 怎么代办"如果各判断一次，迟早会分叉 | §3.2：一个 `classify`，两边共用 |
| 架构：复用 | 存放方式的切换容易写成一套新的复制逻辑 | 复用导出与导入的闭包机制（`store_closure.cpp`） |
| 架构：复用 | 修订 1 新提出的 `smoke` 与版本已有的 `checks` 是两套格式 | §6.1 统一成 `checks`，只新增三种（`gui`、`launcher`、`sound`） |
| 架构：依赖方向 | `openhal-emu` 与 harness 的位置 | emu 是 kit，只实现 openhal；harness 在 luban-os-ci，只调用程序 |
| **用户体验**：一致 | macOS、Windows 有些能力做不到 | 功能一致，性能按平台设预算；做不到的写进能力报告，说明原因与下一步（§5.4） |
| 用户体验：说话 | `shared` 下更新索引会影响整台电脑，如果每次都提醒就太吵 | 每个系统只说一次（§3.2） |
| 用户体验：上手 | 每建一个新系统都自动弹出教学，会让老用户厌烦 | 每个人在每台电脑上只自动打开一次（§8.1） |
| 用户体验：sudo | agent 回答不了密码 | 走审批队列；agent 模式下直接报出命令（§3.2） |
| **交互**：键位 | `Super+Shift+H` 已经用于"向左移动" | 折叠改用 `Super+S`（§8.5） |
| 交互：书签太杂 | 书签太多会显得乱 | 最多 5 个，再多合成"+N"；拖动、缩放时隐藏；可以关（§8.4） |
| 交互：抓指针 | 每次按 Super 都抓指针，会影响平时使用 | 只在"键盘已锁定 + Super 按下 + 指针在窗口内"时抓（§8.10） |
| **兼容** | 环境变量默认值的修复也会影响宿主上的 shim | 符合规范，原来有值的不变（§12） |
| 兼容 | 新旧宿主、系统、规范、预留文件、Windows 的通道 | §12 |
| **性能** | 只有消息时，压缩要花 CPU | 只压损坏的矩形，按行差分后再压；L5 设预算 |
| 性能 | 书签的遮挡计算 | O(n²)，n 很小；时间计入 L5 |
| 性能 | "很卡"的原因没有分段的数据，直接改可能改错地方 | §5.6a：先分段测量，再按顺序改；每一步都对照预算 |
| 性能 | 1080p 拖动 ≥ 50 帧/秒的预算可能定得过高或过低 | 实施时在 Linux machine 上实测后定最终数值，写进 L5；其他平台各自设预算 |
| 生态：许可 | 测试音乐的许可如果出错，就是分发侵权 | 只用作者明确以 CC0 发布的录音；provenance 写明来源与许可原文的链接；实施时以发布页为准再核对一遍 |
| 生态：体量 | 播放器与音乐会让桌面版变大 | Audacious 共用 qt-runtime，音乐约 5 MB；新建页写出实际大小 |
| 性能 | 每个共享一个 virtiofsd，每台 machine 有三个进程 | 进程很轻，随 machine 退出 |
| **稳定性** | SIGKILL 会把 xlings 停在不一致的状态 | 先 SIGTERM，收尾后再退出（§7.3） |
| 稳定性 | 请求方断开后，broker 的执行可能会成为孤儿 | 像终端挂断一样结束它（§3.3） |
| 稳定性 | 宿主写入后，`cache=auto` 有一段时间看不到 | broker 回报完成之前，先让目录项重新验证（§3.3） |
| 稳定性 | 共享区域在窗口重开、尺寸变化时的生命周期 | 区域留在运行目录里，重新宣告；超出预留就重新协商（§5.5） |
| **跨平台** | vz 不给 Linux guest 3D；第二跳没有共享内存 | 写明，不假装；两项可行性验证有门槛（§5.4） |
| 跨平台 | Windows 的消息流 | hvsocket；旧宿主机仍然走 stdio |
| 跨平台 | CI 的 runner 不一定能嵌套虚拟化 | C5 实测；只用托管 runner（E7），公共逻辑在 Linux 上覆盖，跑不了的写明原因（§9.5） |
| **简洁优雅** | 新概念的数量 | 新增的只有：两种存放方式、一条 sudo 规则、一种书签、一种传输接口；其余都是扩展已有的东西（checks、broker、opensal 的协商、openhal 的 Source） |
| 简洁优雅 | 平台特例散落在代码里 | 只出现在 §5.4 的实现表与 carrier 里 |
| **安全** | broker、`own` 下的宿主管理、ivshmem、virtiofsd | 都按 policy 判断并审计；broker 只执行 xlings 的子命令；ivshmem 只放帧，只交给 guest 里的 root；virtiofsd 非特权 |
| **规则** | 没人回答时不问；home 是用户数据；系统调用放在 platform；外部程序一张表；改了作用要升 revision；读不了不等于没有 | §3.3、§7.3、§5、§4、P1、§3.4 |
| **隐私** | 路径与名字 | 只用 `~/.xlings`、`/home/user`、默认用户名；lint 通过 |
| 修订 3：一致性 | §5.6a 写"回归即失败"，与 E4"先只报告"冲突 | 统一为：先只报告，稳定后回归才算失败 |
| 修订 3：可测性 | 只用托管 runner（E7），性能数据波动大，绝对预算在 CI 上不可信 | 绝对值在参考机器上本地验收；CI 只比较 CPU 时间与帧数的中位数，与 main 的基线做相对比较（§5.6a） |
| 修订 3：可测性 | 只用托管 runner，macOS、Windows 的嵌套虚拟化不一定可用 | 两跳的公共逻辑在 Linux 上用 qemu 起一台宿主机来覆盖；平台专属的薄层用单元测试与能力实测；跑不了的写明原因（§9.5） |
| 修订 3：可行性 | E10 的两项验证需要真实的 Mac 或 Windows 电脑，与 E7 冲突 | 列为 E14 请你定：用你自己的电脑手动验证一次，或者移到 §15 |
| 修订 3：正确性 | 修订 2 写 machine 的声音用 `-audiodev wav` 录，可是 machine 的声音经 guestfwd 直接到宿主的声音服务，不经过 qemu 的声卡 | 改为在宿主上起一个只录音的声音服务；声音改走 openhal 通道放到 §15（§6.3、§9.2） |
| 修订 3：正确性 | `sound` 检查要靠宿主侧录音，真实的系统里没有 | `sound` 只在 CI 执行，`luban status --check` 不跑它（§6.3） |
| 修订 3：范围 | 修订 2 顺带把声音的 guestfwd 迁到新通道，扩大了本轮风险，对用户也没有直接收益 | 本轮不迁，放到 §15 |
| 修订 4：可测性 | 调研发现 Windows 托管 runner 上 WSL2 与第二层嵌套的 KVM 都可用 | Windows 从"尽量"变成真实路径全覆盖：wsl2 carrier、嵌套 machine、hvsocket（§9.5） |
| 修订 4：可测性 | macOS 托管 runner 不能启动 vz 虚拟机，GitHub 不打算支持 | 测试用的 qemu carrier 覆盖 vz 以外的全部逻辑；vz 的真实运行列为 E14（按需租裸机 Mac 或不做） |
| 修订 4：架构 | 测试用的 qemu carrier 会不会成为特例 | 它是 carrier 接口的又一个实现，与 vz 共用除"启动虚拟机"以外的代码；只在测试里启用，不进用户的能力报告 |
| 修订 4：稳定性 | 依赖环境能力的作业，可能"什么都没测却显示通过"（调研里别人踩过） | 每个作业第一步探测，探测失败就明确失败（§9.5） |
| 修订 4：成本 | 真 GPU 与裸机 Mac 都要花钱，luban-os 组织是 Free 套餐 | E14、E15 列出费用与替代方案；默认不花钱也能覆盖全部功能 |
| 修订 6：架构 | J36、J38 同源：一个 home 前缀里的系统共用一份版本库与存储，"存储里有"与"本系统选了"被混为一谈 | 两处都改为以本系统的登记为准：用户视图缺什么按登记判断（J36），根闭包只投影本系统选过的成员（J38）；K5 的 `own` 会从根上分开（§15） |
| 修订 6：架构 | J37、J39 同源：私有域里的系统，有的命令在主人的 home 里执行 | 与 `upgrade`、`boot`、`export` 一样转到系统所在的域：`requests`/`approve`/`deny` 与 machine 的 `__broker-serve`；`log`/`report` 待查（§15） |
| 修订 6：稳定性 | J38 让一个系统的每次根更新都失败，连 `self doctor` 的建议也用不了 | 不再拒绝而是不投影；单元测试先复现拒绝 |
| 修订 6：稳定性 | `enqueue` 不检查写入是否成功，J37 里请求号照常返回 | 本轮修了写的位置；写失败要报错，列入 §15 |
| 修订 6：用户体验 | 修之前，升过一次的桌面再升级会说"keep … (yours)"，用户并没有动过 | edition 记录 `brought`；`--dry-run` 能看到要换的包 |
| 修订 6：用户体验 | 提示里的命令（`xlings subos approve agent <id>`）对私有域的系统不起作用 | J37 后提示与行为一致 |
| 修订 6：兼容 | `brought` 是记录里的新字段 | 旧客户端忽略它；没有它的旧记录把"比记录版本旧的已装版本"当作 edition 以前带来的（edition 只往前走） |
| 修订 6：无感升级 | revision 不能让已装的系统换版本（J34 的修复一开始只保护新系统） | 要让已装系统换版本，发新的组合包版本与 edition 版本；d1、d7 用发布的版本实测换上 |
| 修订 6：跨平台 | J37、J39 只在能建私有命名空间的 Linux 上出现，本机测不到 | luban-os-ci 在托管 runner 上跑（§13.2）；DomainProducer 的 e2e 需要静态候选，在 PR CI 上跳过——覆盖靠 L2 |
| 修订 6：一致性 | 同一件事（"系统有没有这个包"）在三处判断：用户视图、借用、根闭包 | 三处都以系统 scope 的登记为准 |
| 修订 6：简洁优雅 | 修复都在原有的路径上：不加新命令、新配置 | 新增的只有 `brought` 一个字段 |
| **风险** | 本轮的范围很大 | K1–K2 先交付可用性；每个检查点都跑完整 CI；每条新路径都有退路 |

## 15. 不做的与以后

- macOS machine 的 virtio-gpu 扫描输出（E14 = B）；按需租用裸机 Mac 的 workflow。
- 真机上 xdesk 的 DRM 后端：实现表里已经留了这一行；本轮在 L6 里，ISO 只测控制台与 L2。
- 第二跳的共享内存（要等平台提供机制）；vz 上 Linux 的 3D；Venus。
- 声音改走 openhal 0.2 的 audio 通道（取代 machine 的 guestfwd），让声音也走 §5 的传输。
- QEMU 的 D-Bus 显示。
- 系统里 `xlings self update` 的语义。
- aarch64 上用 KVM 跑全矩阵。
- 上手应用支持英文、中文以外的语言。
- WuXian 里层叠工作区窗口越出边框部分的裁切（§8.9a：先把窗口缩放改为整树变换）。
- d3d12 上的嵌套 virgl：qemu 的 surfaceless EGL（不经 GBM 与 render node）；本轮只探测并说明（K14）。
- 用户视图的安装只给自己（J33）：`__store-add` 只放 payload；本轮只记录，L2-02 标 known。
- machine 里多用户的属主（J32）：9p 的 `mapped-xattr`，或 virtiofsd 的 subuid 映射；本轮只记录，luban-os-ci 的 L2-05 标为 known。
- 存放方式 `own`（K5）：宿主上的系统都是单一布局，链接写的是建它的那个 home 的路径；`own` 要一种新的域（私有 home 在同一个逻辑路径上）、
  把系统的实例、闭包、ABI、版本登记在两个 home 之间搬动，并守住用户数据的规则；本轮不做，单独一个 PR（luban-os-ci 的 L2-13 为 pending）。
- 私有域里系统的 `subos log`、`subos report`：事件日志也在域里，可能与 J37 同样读错 home；先确认再转发。
- `broker::enqueue` 写请求失败时仍返回请求号：应报错。
- 通用的"程序能启动"检查对 Qt 图形程序（falkon）没有显示时退出 134：对图形程序设 `QT_QPA_PLATFORM=offscreen`，或对声明了自有检查的程序跳过；目前是非关键提示。
- DomainProducer 的 e2e 只在有私有命名空间且是静态候选的环境里跑：给 CI 一个这样的作业。
- xim-pkgindex 的 gitee-sync 只在 push 时运行：GitHub 漏掉 push 事件时（2026-10-22 的 #1030）要等下一次 push 才同步。
- mcpp 的正式发布（会通知 AUR、Homebrew、Nix）由维护者触发；core edition 里 mcpp 的版本随之更新。

## 16. 评审

### 16.1 结论

| 编号 | 结论 |
|---|---|
| D1 | 两种存放方式，可以配置；宿主上默认 `shared`；体验处处一致 |
| D2 | 系统级的操作要 sudo；只影响自己的不要（例如 `xlings use`）；CI 覆盖 |
| D3 | virtiofs，9p 作为后备 |
| D4 | 从架构上做，不打补丁（§5） |
| D5 | 书签；工作区就是特殊的窗口；Super/Alt+Tab 能看到全部 |
| D6–D9 | 按建议（折叠的键位改为 `Super+S`，见 E8） |
| D10 | 新仓库 `luban-os/luban-os-ci` |
| E1 | `shared` 下照常更新，并说明一次 |
| E2 | 书签的判定与挂靠如 §8.4 |
| E3 | 由 Sunrisepeak 新建，回报用 GitHub App |
| E4 | 先只报告，稳定后设为必需；发布必须通过 L6 |
| E5 | macOS、Windows 尽量都支持（§5.4） |
| E6 | `own` 下的宿主管理如 §3.1 |
| E7 | 只用 GitHub 托管的 runner；跑不了的项写明原因；两跳的公共逻辑在 Linux 上覆盖（§9.5） |
| E8 | 折叠用 `Super+S` |
| E9 | 统一用 `checks`，新增 `gui`、`launcher`、`sound` |
| E10 | 两项可行性验证的门槛如 §5.4（受 E7 限制的处理见 §16.2 的 E14） |
| E11 | 音频播放器用 Audacious |
| E12 | 测试音乐用 Open Goldberg Variations 的《咏叹调》（CC0），随桌面版分发 |
| E13 | 流畅度的预算如 §5.6a |

| E14 | B：不租裸机 Mac；vz 的真实运行由维护者在发布后用自己的 Mac 验证，CI 用测试用的 qemu carrier 与 `validate()`；扫描输出移到 §15 |
| E15 | C：不用真 GPU；CI 测功能（llvmpipe、WARP），性能的绝对值在本地验收 |
| E16 | CI 尽量覆盖；**本地 Linux 上能跑 Linux 的全部覆盖**：luban-os-ci 的 harness 与场景不依赖 GitHub，`./run.sh <层> <场景>` 在一台有 KVM 的 Linux 上跑出与 CI 相同的结果（§13.1） |

## 17. 调研来源（2026-10）

| 结论 | 来源 |
|---|---|
| GitHub 托管的 macOS runner 不支持嵌套虚拟化（Apple Virtualization Framework 的限制）；GPU larger runner 是一张 Tesla T4、Ubuntu 与 Windows | [Larger runners reference](https://docs.github.com/en/actions/reference/runners/larger-runners)、[GitHub-hosted runners](https://docs.github.com/en/actions/concepts/runners/github-hosted-runners) |
| 托管的 macOS arm64 runner 本身是虚拟机，`VZVirtualMachine.isSupported` 返回 false；支持 Hypervisor.framework 的请求被关闭为不计划 | [microsoft/mxc #927](https://github.com/microsoft/mxc/issues/927)、[actions/runner-images #13505](https://github.com/actions/runner-images/issues/13505) |
| Apple 的嵌套虚拟化只在 M3/M4 + macOS 15 上、只给 Linux 客户机；要在作业里启动 vz 虚拟机，runner 必须直接跑在 Mac 上 | [simongdavies/github-runner-macos](https://github.com/simongdavies/github-runner-macos) |
| 托管的 Windows runner 自 2024 年起支持嵌套虚拟化，`Vampire/setup-wsl@v7` 默认 WSL2 | [actions/runner-images #10563](https://github.com/actions/runner-images/issues/10563)、[Setup WSL](https://github.com/marketplace/actions/setup-wsl) |
| `windows-2025` 的 WSL2 里有 `/dev/kvm`，`kvm-ok` 通过，`qemu -accel kvm` 能起来（第二层嵌套）；那次实测还发现"什么都没执行也显示通过"的陷阱 | [Lullabot/sandbar #149](https://github.com/Lullabot/sandbar/pull/149) |
| KB5127648 在 AMD 主机上破坏 WSL2 的嵌套虚拟化 | [microsoft/WSL #41955](https://github.com/microsoft/WSL/issues/41955) |
| Mesa d3d12 的设备选择（`MESA_D3D12_DEFAULT_ADAPTER_NAME`），失败时不会自动退回 | [GPU selection in WSLg](https://github.com/microsoft/wslg/wiki/GPU-selection-in-WSLg)、[WSLg Architecture](https://devblogs.microsoft.com/commandline/wslg-architecture/) |
| GPU runner 的价格与套餐要求 | [Actions runner pricing](https://docs.github.com/en/billing/reference/actions-runner-pricing) |
| 裸机 Mac 的按需租用：Scaleway M4 €0.22/小时、最少 24 小时；AWS `mac-m4.metal` $1.23/小时、最少 24 小时 | [Scaleway Apple silicon pricing](https://www.scaleway.com/en/pricing/apple-silicon/)、[AWS EC2 Mac FAQ](https://aws.amazon.com/ec2/instance-types/mac/faqs/) |

luban-os 组织目前是 Free 套餐（2026-10-22 查询）。
