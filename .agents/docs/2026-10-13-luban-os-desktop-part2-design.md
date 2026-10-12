# Luban OS 桌面 part 2：xdesk —— 可独立复用的桌面组件，两套桌面；开箱即用的环境；快速开始与导入

日期：2026-10-13　基线：part 1（`2026-10-12-luban-os-architecture-design.md`，实施中，同一个 PR）
状态：第二轮 review（第一轮的 1–5 已确认；本轮：组件命名与独立复用、版本选择规则、多视角自我 review）。
确认后按 §8 在同一个 PR 上分 checkpoint 实施，PR 合入与发布在全部 checkpoint 之后。

---

## 0. 背景与已确认的方向

part 1 打通了整条链：Luban OS 窗口（opensal 提供端）—opensal.wire—机器通道或沙箱管道—opensal-wlr 桥—labwc。
它证明了路径，但离"新建、进入就有好体验"还差得远。本轮讨论确认：

1. **一个合成器，两套桌面系统。** 合成器自己做，从开始就支持多种布局模式。两套桌面系统是同一个合成器上的两种
   默认组合：**preview** = 平铺布局 + 一套传统桌面系统（可配置）；**xdesk** = 无限画布布局 + xdesk 系统（探索）。
2. **niri 等外部合成器有缺陷就不用。** niri 没有可用的无头模式且硬依赖 OpenGL；labwc 能用但观感陈旧。
   外部合成器继续经 opensal-wlr 桥作为"用户自选会话"接入，不再是官方桌面。
3. **两种用户视角。** 控制台线（nano / tiny / core）：精简、透明、自己定制。桌面线（desktop）：基于 luban 给出的
   版本新建、进入即好用——完善的环境与基础设施。
4. **快速开始**：nano / tiny / core / desktop；点 desktop 再选 **preview（传统、高可用，推荐）** 或
   **xdesk（探索：无限画布）**，各有简单说明。**导入**同样是一等入口。
5. **技术选型：稳定 + 现代。** 自有部分全部 mcpp + C++23 modules。缺的包补到生态（mcpp 的 compat 包、
   xim-pkgindex 的用户包）；aarch64 麻烦的先标注，以后补。
6. **全部在这一个 PR 上按 checkpoint 实现。**
7. **组件可独立复用，命名要友好。** xdesk 的每个组件（合成器、布局、桌面外壳、绘制……）别的系统也能单独拿去用：
   它们不属于 Luban，名字也不带 luban（§2.0）。
8. **版本选择规则。** 用户直接使用的工具追求最新稳定版本（没有稳定版的，如 dsh，用最新版）；用户不直接接触的库与
   基础技术稳定即可（§5.1）。

## 1. 原则

| 原则 | 落到哪里 |
|---|---|
| 进入即好用 | 桌面 edition 自带完整环境与默认配置；第一次进入不需要任何手动步骤 |
| 稳定 + 现代 | 协议层用 wlroots（成熟、广泛使用）；布局、动画、交互、视觉是我们自己的，按现代标准做 |
| 一个实现 | 两套桌面共享一个合成器；差别是配置（布局、shell、主题），不是两份代码 |
| 原生 opensal | 自有合成器直接输出 opensal surface：不经 screencopy、不注入输入；输入法、剪贴板、尺寸、密度都是原生的 |
| 生态补齐 | 缺什么补什么：构建依赖进 mcpp 的 compat 包，用户软件进 xim-pkgindex |
| 可验证 | 合成器本身就是 opensal 消费端，`luban-os --desktop-exec xdesk` 即可无头测试；关键帧本地审美评估 |

## 2. xdesk：桌面组件家族与合成器

### 2.0 命名与独立复用

两个家族，都不属于 Luban，Luban 只是它们的第一个使用者：

| 家族 | 是什么 | mcpp 包 → C++ 模块 |
|---|---|---|
| **opensal** | 表面规范：一个 surface 与它的接口、跨地址空间的线协议 | `opensal` → `opensal`、`opensal.wire`；`opensal-wlr` → `opensal.wlr`、`opensal.wlr.wayland`（把任何 wlroots 系合成器接成 opensal 消费端的桥） |
| **xdesk** | 桌面组件 | `xdesk-base`（只需 C++ 与操作系统）→ `xdesk.os`、`xdesk.term`、`xdesk.layout`；`xdesk-compositor`（需要 C 图形库：wlroots、pixman、freetype、harfbuzz）→ `xdesk.compositor`、`xdesk.draw`、`xdesk.shell` |

**包按复用边界划分，不按文件**：一个包是别人会整体拿走的东西（"只要纯 C++ 的部分"、"要合成器"），
包里按目录分模块；不为一个文件建一个 `mcpp.toml`。Luban OS 窗口只链接 `xdesk-base`（用 `xdesk.term`），
不会因此带上图形库。

- 程序：**`xdesk`**（一个桌面会话：合成器 + 外壳 + 一份配置；`xdesk --profile classic|canvas`）。
  别的系统可以只用 `xdesk-compositor` 配自己的外壳，或只用 `xdesk.layout`、`xdesk.term`。
- 两种配置（profile）：**classic** = 平铺布局 + 传统外壳；**canvas** = 无限画布布局 + 画布外壳。
  会话文件 `xdesk-classic.desktop`、`xdesk-canvas.desktop`；配置包 `config:xdesk-classic`、`config:xdesk-canvas`。
- 面向用户的名字：快速开始里 desktop 之下是 **"经典桌面（preview，推荐）"**（classic）与
  **"xdesk 画布（探索）"**（canvas）；edition 是 `luban-desktop` 与 `luban-desktop-xdesk`。
- **独立性是检查出来的，不是约定**：`tools/lint_layer_deps.sh` 增加规则——`opensal*`、`xdesk*` 只能导入 `std`、
  opensal、xdesk 与 C 依赖（compat 包），**从不导入 `xlings.*`、`luban.*`**。系统头文件只在 `xdesk.os` 与直接调用
  C 库的实现单元里（同"桌面 backend"的例外）。这样任何一个组件都可以原样搬到自己的仓库。
- 现状的调整：`opensal-wlr` 现在用 `xlings.platform`，改用 `xdesk.os`；`luban.desktop.term` 改名 `xdesk.term`。
  Luban 专有的只剩 `luban/desktop/host`（Luban OS 窗口的 SDL 提供端）与应用本身。

### 2.1 位置与形态

```
luban/desktop/
├── opensal/
│   ├── spec/                 包 opensal       src/opensal.cppm、wire.cppm
│   └── wlr/                  包 opensal-wlr   src/wlr.cppm、wayland.cppm、bridge.cpp、session.cpp
├── xdesk/
│   ├── base/                 包 xdesk-base    src/os/、src/term/、src/layout/
│   └── compositor/           包 xdesk-compositor  src/compositor/、src/draw/、src/shell/
└── host/                     包 luban-desktop-host（Luban OS 窗口的宿主提供端，Luban 专有）
apps/xdesk                    xdesk 程序：一个桌面会话（opensal 消费端，stdio 即 opensal.wire）
apps/luban-os                 Luban OS 窗口
```

初期这些家族作为 `luban/desktop/` 下的子系统随 xlings 仓库一起演进（part 1 的决定），不单独发布；
独立性规则保证以后拆出时只是搬目录。

- **C++23 modules over wlroots 0.20**（当前稳定系列；0.19 已进入弃用。C API，经 mcpp 的 `compat.wlroots`）。wlroots 负责协议与缓冲区的正确性，
  我们负责布局、交互、渲染策略与 opensal 输出。系统头文件只在实现单元里，规则同 `modules/platform`。
- **静态链接（musl）**，与 luban 一样能在任何根里运行（glibc、musl、什么都没有的 nano）。
- **作为会话声明**：`share/wayland-sessions/xdesk-classic.desktop`、`xdesk-canvas.desktop`，
  多一个键 `X-Opensal-Native=true`：`luban __desktop` 与机器 agent 见到它就把 stdio 直接接给它（不经桥）。

### 2.2 核心

| 能力 | 做法 |
|---|---|
| 渲染与输出 | wlroots scene graph；无头后端 + pixman 软件渲染（机器与沙箱都没有 GPU 也能跑）；场景按损伤区域渲染到自己的缓冲区，直接编码成 opensal.wire 的帧 |
| 尺寸与密度 | opensal.mode / density → 输出的自定义模式与 scale（wp-fractional-scale） |
| 输入 | opensal 的键盘、指针、滚轮直接成为 wlroots 输入设备。keymap：xkbcommon 编译**编进程序的完整 keymap 文本**（US 起步，按宿主布局扩展），运行时不依赖 xkeyboard-config 数据 |
| 输入法 | 宿主输入法的提交与预编辑（opensal.text）→ text-input-v3 交给客户端；光标位置回报给宿主 |
| 剪贴板 | data-device / primary-selection ↔ opensal.clipboard（受环境的 clipboard 授权约束） |
| 应用 | xdg-shell（服务端装饰：我们画标题栏）、Xwayland、layer-shell（第三方面板也能用） |
| 协议 | xdg-decoration、foreign-toplevel、screencopy/ext-image-copy（截图工具）、output-management、viewporter、presentation-time、cursor-shape、idle-inhibit、pointer-constraints / relative-pointer |
| 动画 | 所有几何变化（打开、关闭、移动、缩放、切工作区、画布平移缩放）走统一的弹簧动画；可关闭（无障碍） |

### 2.3 布局模式（同一个引擎，按工作区可切换）

| 模式 | 行为 |
|---|---|
| **tile** | 滚动平铺：窗口是一列列横向排开，可滚动；列内上下分；任意窗口可浮动 |
| **float** | 传统层叠：自由移动、缩放、吸附 |
| **canvas** | 无限画布：窗口放在二维平面上；平移、缩放、总览、书签视图 |

布局引擎是纯逻辑（输入：窗口与事件；输出：几何与动画目标），单独测试。

### 2.4 两套桌面系统 = 两份默认配置（profile）

| | **classic**（"经典桌面"，preview，推荐） | **canvas**（"xdesk 画布"，探索） |
|---|---|---|
| 默认布局 | tile（可切 float、canvas） | canvas（可切 tile、float） |
| 桌面系统 | 顶栏（工作区、窗口、时钟、托盘状态）+ 启动器（应用搜索）+ 通知 | 画布导航（总览、缩放、书签、小地图）+ 启动器 + 通知 |
| 状态 | preview | 探索（可能变化） |

- 面板、启动器、通知**画在合成器里**（同一渲染路径、同一动画与主题），不依赖 yambar / fuzzel；
  文字用 `xdesk.draw`（pixman + freetype + harfbuzz；字体按路径加载，不经 fontconfig；CJK 字体随 `xdesk-essentials`）。
- 配置是 `config:` 包（xlings 已支持 `type = "config"`）：`config:xdesk-classic`、`config:xdesk-canvas`
  给出布局、快捷键、主题、面板、启动器条目；用户的 `~/.config/xdesk/` 覆盖。
- 选会话：`luban config <n> --desktop xdesk-classic|xdesk-canvas|<其它会话>|none`。

### 2.5 与 part 1 的关系

- opensal-wlr 桥保留：第三方合成器（labwc 等，用户在 nano / tiny / core 上自建）照旧接入。
- `labwc-desktop` 不再是官方桌面，作为可选会话保留在索引里。它带的 foot、Xwayland、字体重新打包为
  `xdesk-essentials`（§3）。

## 3. 开箱即用的环境

### 3.1 desktop edition 的组成

```
nano → tiny → core → desktop = core + 富环境层 + xdesk + xdesk-essentials + 默认配置
                       ├─ luban-desktop       （会话 xdesk-classic，"经典桌面"，preview）
                       └─ luban-desktop-xdesk （会话 xdesk-canvas，"xdesk 画布"，探索）
```

| 类别 | 内容 | 状态 |
|---|---|---|
| 工具链 | gcc 16.1、mcpp、xlings、luban | ✓（来自 core） |
| 语言 | node、python + uv、bun | 已有（node 24、python 3.13）→ 升到最新稳定（§5.1） |
| AI agent | claude、codex、pi、dsh、opencode | claude / codex / pi 已有，升到最新；**dsh 用最新版，仅 x86_64**（aarch64 先标注）；**opencode 补包** |
| 命令行 | git、curl、ripgrep、fd、fzf、nvim、vim、fish | 已有，升到最新稳定 |
| 常用 | tmux、htop、jq | **补包**（最新稳定） |
| 桌面应用 | 终端（foot）、Xwayland、字体（含 CJK） | `xdesk-essentials`（工厂，两种架构） |
| 浏览器、文件管理器 | — | 后续（体积与依赖大，单独评估） |

一个包在某个架构上没有时，edition 清单按架构声明成员（`"packages_x86_64": [...]`），另一架构上不装并在
`luban status` 里说明——不让整个 edition 在 aarch64 上失败。

### 3.2 默认配置（"装好了"与"好用"的差别）

放在 edition 的 factory `/etc` 与 `config:` 包里，用户自己的配置优先：

- **shell**：登录 shell fish，提示符显示所在环境；`EDITOR=nvim`；UTF-8 locale。
- **git**：`init.defaultBranch main`、`pull.rebase false` 等常用默认。
- **区域镜像感知**：npm / pip / uv / bun 的源跟随 `xlings config --mirror`（CN 用户默认走 CN 源）。
- **agent**：经环境的代理与授权工作；密钥不进镜像，首次运行引导配置。
- **桌面**：启动器里有终端与各 agent 的入口；字体覆盖 CJK。

### 3.3 版本语义

- edition 是日期版本，发布后不变；`latest` 是引用；`min_client` 说明需要的客户端。
- **基础成员固定版本**（glibc、gcc、mcpp、xlings、xdesk、xdesk-essentials）；**快速变化的成员创建时取最新并记录**
  （node、python、agent，写成不带版本的 `xim:node`），`luban upgrade` 移动它们。
- CI 定期（每周）检查固定成员是否有新版本，有则测试并发布新的日期版本。

## 4. 快速开始与导入（体验）

### 4.1 快速开始

```
欢迎使用 Luban OS
┌ 控制台：精简，自己定制 ─────────────────────────┐   ┌ 桌面：开箱即用 ─────────┐
│  nano          tiny           core              │   │  desktop                 │
│  只有 xlings   shell 与基础   开发控制台        │   │  完整环境与桌面          │
└────────────────────────────────────────────────┘   └──────────────────────────┘
点 desktop →  ┌ preview（推荐）──────────┐  ┌ xdesk（探索）────────────┐
              │ 平铺 + 传统桌面，高可用   │  │ 无限画布，opensal 原生    │
              └───────────────────────────┘  └───────────────────────────┘
名字 [desk-1]        ▸ 更多选项（运行方式、持久化）
[ 创建并进入 ]                         从文件导入（.luban）…
```

- 每张卡：标题、一句话、状态（stable / preview / 探索）、运行方式（沙箱 / 机器）、大小、需要什么（qemu、KVM）。
  选中后展开详情：里面有什么、适合谁。
- **卡片内容来自索引**：edition 包加 `title` / `summary`（中英）、`size`、`status`、`audience`（console / desktop）、
  `runs`（sandbox / machine）、`requires`；应用内有离线兜底。新增 edition 或改描述只改索引。
- **创建过程**：阶段 + 大小 + 速度 + 剩余时间；缺 qemu 等依赖时给出一键安装（普通的 `xlings install`），
  不让用户回终端。

### 4.2 导入

三个入口，同一个流程：

1. 快速开始底部："从文件导入（.luban）…"；
2. 启动列表最后两行："＋ 新系统"、"↓ 导入…"；
3. 任何界面把 `.luban` 文件拖进窗口。

流程：选文件（系统文件对话框，SDL3）→ 预览（edition、大小、包、策略；**策略比默认宽时先显示再确认**）→ 名字
（重名给出建议）→ 导入并进入。管理面板里同样有"导入"。

## 5. 生态与跨仓库

| 仓库 | 内容 | 架构 |
|---|---|---|
| mcpp 的包索引（compat） | `compat.wayland`（server、client、scanner）、`compat.wayland-protocols`、`compat.xkbcommon`、`compat.wlroots`（无头 + pixman；drm、libinput 后端先关；**提供静态库**）；已有：pixman、libdrm（头文件）、libffi、freetype、harfbuzz | x86_64、aarch64 |
| xim-pkgindex | `xdesk`（随 xlings 发布）、`xdesk-essentials`（foot、Xwayland、字体；工厂）、`config:xdesk-classic`、`config:xdesk-canvas`、edition `luban-desktop` 与 `luban-desktop-xdesk` 新日期版本（含元数据与按架构成员）、补包 `opencode`、`tmux`、`htop`、`jq`，已有的用户工具升到最新稳定；`dsh` aarch64 标注后补 | 两种架构（标注例外） |
| xlings-res / GitCode | 新的大文件，发布当天本地 gtc 补齐 | — |

### 5.1 版本选择规则

**用户直接使用的工具：最新稳定版本**（没有稳定版的，用最新版）。**用户不直接接触的库与基础技术：稳定即可**
（成熟、广泛使用、在维护的系列；不追最新）。实施时（D1）按当时的最新再核对一次：

| 用户直接使用（最新稳定） | 版本（2026-10 调研） | 库与基础技术（稳定即可） | 版本 |
|---|---|---|---|
| node | 26.x（26.11.1；10 月 28 日转 LTS） | wlroots | 0.20.x（当前稳定系列） |
| python | 3.15.0 | wayland（libwayland） | 1.24 系列（各发行版的稳定基线） |
| opencode | 1.19.0 | wayland-protocols | 1.49 |
| tmux | 3.8 | libxkbcommon | 1.13.2 |
| htop | 3.5.3 | pixman、freetype、harfbuzz、libffi | compat 已有的版本 |
| jq | 1.8.2 | foot、Xwayland（`xdesk-essentials` 里） | 工厂所用发行版的稳定包 |
| dsh | 最新版（尚无稳定版） | | |
| claude、codex、pi、uv、bun、git、nvim、fish、ripgrep、fd、fzf | 实施时的最新稳定 | | |

## 6. 测试与验证

- **布局引擎**：纯逻辑单元测试（tile 滚动与浮动、canvas 平移缩放、动画目标）。
- **组件独立性**：lint 规则（§2.0）；`xdesk.layout`、`xdesk.draw`、`xdesk.term` 各自的单元测试不链接任何 Luban 模块。
- **合成器**：`luban-os --desktop-exec xdesk --profile classic` 无头运行；脚本打开终端、切布局、平移画布，
  断言像素与状态；测试客户端（Wayland）验证 xdg-shell、text-input、剪贴板。
- **端到端**：镜像通道（x86_64、aarch64）各启动 preview 与 xdesk 一次，机器与沙箱各一次，打字并确认在环境里运行。
- **关键帧**：两套桌面 × 三种尺寸 × 中英文，CI 产物 + `tools/luban_os_keyframes.py`，合入前本地审美评估。

## 7. 风险与待定

- **工作量最大的是合成器（§8 D2–D5）。** wlroots 每个小版本都会改 API：锁定 0.20 系列，升级作为单独的变更；
  静态链接 wlroots 需要 compat 包提供静态库（meson `default_library=static`，含 libffi、xkbcommon）。
- GPU 加速、真机（DRM、libinput、seatd）后续：先无头 + pixman，所有载体一致。
- 浏览器、文件管理器后续单独评估（体积、依赖、沙箱）。
- aarch64：compat 包与 xdesk-essentials 两种架构一起构建；dsh 等用户包先标注。

## 8. Checkpoint 与依赖（全部在本 PR）

```
D0 家族与独立性 ─┐
D1 生态补齐 ──────┴► D2 合成器核心 ──► D3 布局引擎 ──► D4 桌面系统（shell） ──► D6 配置与会话 ──► D7 环境与 edition
                     │                                                                              │
                     └──► D5 输入法与剪贴板                       D8 快速开始与导入 ◄────────────────┘
D9 测试与关键帧、D10 文档 / 版本 / 发布：贯穿，最后收口
```

| # | 内容 | 依赖 | 验收 |
|---|---|---|---|
| D0 | 家族与独立性：目录与包（opensal、opensal-wlr、xdesk-base）；`xdesk.os`；`opensal-wlr` 脱离 `xlings.platform`；`xdesk.term` 改名；lint（layer deps 的家族规则、headers 与 branches 覆盖 luban/） | — | lint 通过；现有测试不变 |
| D1 | compat 包（wayland、wayland-protocols、xkbcommon、wlroots）；用户包补齐与升级（§5.1）；`xdesk-essentials` 工厂 | — | 两种架构构建通过；索引静态测试 |
| D2 | `xdesk-compositor` + `xdesk` 程序核心：scene、无头 + pixman、opensal 原生输出 / 输入 / 尺寸、xdg-shell + 服务端装饰、Xwayland、layer-shell；`X-Opensal-Native` 会话 | D1 | 无头运行：终端打开、打字执行、截图像素正确 |
| D3 | 布局引擎：tile / float / canvas、工作区、动画 | D2 | 单元测试；关键帧 |
| D4 | `xdesk.draw` 与 `xdesk.shell`：面板、启动器、通知、画布导航；文字渲染；主题 | D3 | 无头脚本：启动器打开应用；关键帧审美评估 |
| D5 | 输入法（text-input-v3 ↔ opensal.text）、剪贴板（↔ opensal.clipboard，授权） | D2 | 测试客户端收到提交文字；剪贴板往返 |
| D6 | `config:xdesk-classic` / `config:xdesk-canvas`、用户配置、`luban config --desktop` | D4 | 两种默认组合可切换 |
| D7 | edition：desktop（preview）与 desktop-xdesk 新日期版本；富环境层；默认配置；区域镜像；按架构成员；元数据 | D1、D6 | 新建进入即可用；aarch64 缺的包被说明而不是失败 |
| D8 | Luban OS 应用：快速开始两组 + desktop 二选一 + 索引元数据 + 导入（三入口、拖放、预览确认） | D7 | 无头脚本走完新建与导入；关键帧 |
| D9 | 测试：引擎、合成器、端到端两版、关键帧 | 全部 | CI 全绿；关键帧评估通过 |
| D10 | 文档（docs/luban 中英、规范）、版本、发布、CN 镜像、索引 PR 合入、真实验证 | 全部 | 发布后 fresh install 走通桌面 |

## 9. 多视角自我 review

| 视角 | 问题 | 结论 / 处理 |
|---|---|---|
| 架构 | 两套桌面会不会分叉成两份代码？ | 一个合成器、一个布局引擎、一个外壳框架，差别只在 profile（配置）；新增布局或外壳组件两边都可用 |
| 独立复用 | 组件会不会悄悄依赖 Luban？ | lint 规则禁止 `opensal*`/`xdesk*` 导入 `xlings.*`/`luban.*`；需要的系统调用在 `xdesk.os`；以后拆仓只是搬目录 |
| 稳定性 | 自研合成器的风险？ | 协议正确性交给 wlroots（成熟）；我们只做布局、交互、渲染策略与 opensal 输出；wlroots 锁系列；无头 + pixman 在所有载体一致 |
| 优雅简洁 | 是否引入了多余的进程或协议？ | 合成器直接输出 opensal（省掉桥、screencopy、输入注入）；面板与启动器在同一进程同一渲染路径；没有新增 xlings 命令，选择桌面是 `luban config --desktop` |
| 用户体验 | 第一次进入是否就好用？ | edition 带完整环境与默认配置；快速开始两组卡片 + desktop 二选一 + 说明与大小；缺 qemu 一键安装；导入三入口；关键帧审美评估是合入前提 |
| 兼容性 | 与 part 1 已做的东西冲突吗？ | opensal 线协议不变（只增）；opensal-wlr 桥保留给第三方合成器；labwc-desktop 降为可选会话；part 1 的 desktop edition 2026.10.13.1 尚未发布，改为 xdesk 版本不留兼容负担 |
| 跨平台 | 在哪里能用？ | xdesk 运行在 Linux 根里（x86_64、aarch64），由机器或沙箱承载；Luban OS 窗口本身跨平台（Linux 先发布，aarch64 的窗口待索引 X11 库） |
| 一致性 | 名字、配置、命令是否一套规则？ | 家族名不带 luban；会话、配置包、profile 同名（`xdesk-classic` / `config:xdesk-classic` / `--profile classic`）；edition 名带 luban（`luban-desktop*`），因为 edition 是 Luban 的 |
| 无感升级 | 升级会不会打断用户？ | edition 日期版本 + 固定/最新两类成员；`luban upgrade` 只移动用户没动过的；用户配置（`~/.config/xdesk`）永远优先于 `config:` 包 |
| 安全 | 新组件放宽了什么？ | 合成器在机器或沙箱内运行；剪贴板与输入法受环境授权约束；导入时策略更宽必须确认 |
| 可观测 | 出问题怎么查？ | 合成器日志写环境内 `/var/log/xdesk/`（机器）或会话日志；`luban-os` 关键帧与状态快照；xlings 操作日志不变 |
| 生态 | 缺的东西怎么来？ | 构建依赖进 mcpp compat（两种架构，静态库）；用户软件进 xim-pkgindex（最新稳定）；大文件发布当天补 GitCode |
| 许可 | 能否打包与静态链接？ | wlroots、wayland、xkbcommon、pixman、harfbuzz、foot、Xwayland 均为 MIT 类；freetype 用 FTL；允许静态链接与再分发，provenance 记录来源 |
| 工作量与进度 | 同一个 PR 是否过大？ | D0–D10 每个 checkpoint 一个提交、各自可构建可测试；发布在最后；某个 checkpoint 受阻（如静态 wlroots）时先在 PR 里说明并给出替代（动态链接 + 随包库） |

