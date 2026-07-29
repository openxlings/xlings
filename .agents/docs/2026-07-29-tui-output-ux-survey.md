# xlings 终端输出体验调研(宽度 / 一致性 / 优雅度 / 可发现性)

> 日期: 2026-07-29
> 类型: 调研 (survey)
> 范围: `src/ui/*`(ftxui 渲染层)、`src/core/log.cppm`、`src/agent/text_renderer.cppm`,以及 `use / list / search / info / config / self doctor / --help` 的实际输出
> 方法: 源码通读 + 受控 pty 复现(40 / 60 / 100 列)+ 管道/非 TTY 对照
> 复现脚手架: `python3 runpty.py <cols> <rows> xlings ...`(见文末附录)

---

## TL;DR

窄窗口下的"换行乱"不是一个 bug,是**四个互相矛盾的宽度决策**叠加出来的:

1. `print_info_panel` 会把面板宽度**顶到超过终端宽度**,于是终端强制硬折行;
2. 同一函数算这个宽度时用了错的公式,导致**右侧被静默截断**(`xlings use gcc` 实测丢掉最后 3 个字符 `bin`);
3. 其余渲染器用 `Dimension::Full()`,超宽时 ftxui **等比压缩每一列**,把包名和版本号也一起吃掉(`xim:binutils@2`)——这已经不是难看,是**输出了错误的信息**;
4. 所有对齐都按**字节数**而不是显示宽度算。

更深的问题是:**xlings 没有"终端宽度"这个概念**。全树没有一个地方问过"这一行放得下吗、放不下怎么办",每个渲染函数各自决定,答案都不一样。

同时存在三套并行的输出通道(ftxui / `log::` / 手写 ANSI),缩进、前缀、调色板来源、是否响应浅色主题**全都不一致**;而 `--agent` 的纯文本渲染器,输出比 TUI **更完整、更对齐、更正确**。

---

## 一、宽度模型:"换行乱"的根因

### P0-1 — `print_info_panel` 主动把宽度顶超终端,必然硬折行

`src/ui/info_panel.cppm:80-83`:

```cpp
auto termDim = Dimension::Full();
int width = std::max(termDim.dimx, minWidth);   // ← 关键
auto screen = Screen::Create(Dimension::Fixed(width), ...);
```

意图是"别截断内容",实现是"内容多宽,画布就多宽"。而 ftxui 的 `Screen::ToString()` 会**把每一行都用空格填满到画布宽度**(`screen.cpp:421-443`,没有 trailing-space trim),于是 60 列终端里出现 92 字符的行,终端把每一行折成两行,第二行几乎全是空格。

实测(60 列 pty,`xlings use gcc`,已去 ANSI):

```
92 chars:   ◆ gcc versions (cu…
92 chars:   ──────────────────…
92 chars:   15.1.0          /h…
92 chars:   15.1.0-aarch64-mus…
```

**这就是用户看到的"换行乱"。** 窗口越窄越乱,而且乱得没有规律(每行折断位置取决于内容)。

> 顺带:`fit_full_height` 的注释("A report is not a viewport")的判断是对的,但它只解决了**高度**。宽度方向做了**相反**的选择——高度可以超屏(终端会滚动),宽度不可以超屏(终端会折行)。这个不对称没有被识别出来。

### P0-2 — `minWidth` 公式用固定 label 宽,导致右侧静默截断

`src/ui/info_panel.cppm:26` 只补齐、从不截断:

```cpp
while (padded.size() < 14) padded += ' ';   // label 可以超过 14
```

而 `:61` 算宽度时把 label 当成永远是 14:

```cpp
int w = 2 + 14 + 2 + static_cast<int>(f.value.size()) + 2;
```

label 长于 14 时,每多 1 个字符就少算 1 列。实测算术:

| label | 实际行宽 | minWidth(=90) | 溢出 |
|---|---|---|---|
| `15.1.0` | 69 | 90 | -21 |
| `15.1.0-aarch64-musl` | **93** | 90 | **+3** |
| `15.1.0-musl` | 74 | 90 | -16 |

溢出 3 → 末尾 3 个字符 `bin` 被丢掉,**无省略号、无任何提示**:

```
  15.1.0-aarch64-musl  /home/speak/.xlings/data/xpkgs/xim-x-aarch64-linux-musl-gcc/15.1.0/
                                                                                          ↑ 丢了 "bin"
```

这条路径同时也解释了用户贴的原始输出里那一行为什么和别的行不齐——**长 label 既破坏对齐又触发截断**,两个症状同源。

这又是 xlings 反复出现的那个 bug 形态:**截断了和没截断,输出形态一模一样**。

### P0-3 — `Dimension::Full()` + hbox 等比压缩 = 把标识符也截掉

其余 11 处渲染器用 `Dimension::Full()`。超宽时 ftxui 的 `hbox` 对**所有子元素等比压缩**,它不知道哪一列是"标识符不能动"、哪一列是"描述可以删"。

`xlings search gcc` @ 60 列实测:

```
  ◆xim:aarch64-linu  Cross GCC toolchain: host -> aarch64-li
  ◆ xim:gcc  GCC, the GNU Compiler Collection
  xim:g GCC runtime libraries (libstdc++, libgcc_s, libgomp,
  ◆xim:mingw-c  MinGW-w64 GCC cross toolchain (Linux host →
```

- 包名被截成 `xim:g` / `xim:mingw-c` —— **用户无法把它复制去 install**;
- marker `◆` 后面的空格都被吃掉(`◆xim:`),甚至整个 marker 消失(第 3 行);
- 每行截断量不同,看起来像渲染故障。

`xlings list` @ 60 列更严重,截的是**版本号**:

```
  ◆ xim:binutils@2       ← 真实版本是 2.42
  ◆ xim:bun@1.3          ← 真实版本是 1.3.x
  ◆ xim:cairo@1.
```

`xim:binutils@2` 是一个**看起来完全合法、但是错的**版本号。这比截断更糟。

`xlings --help` @ 40/60 列则表现为**逐行对不齐**(描述长的行把 name 列的 padding 吃掉了):

```
   install  Install packages (e.g. xling      ← name 列 8 宽
    remove        Remove a package            ← name 列 14 宽
   update    Update package index or a s
    search        Search for packages
```

第一次接触 xlings 的用户看到的第一屏就是这个。

### P0-4 — `self doctor` 单行 230 字符

@ 60 列,每行折 4 次:

```
  ✗ binding state  'node' is active at 24.15.0 but 'npm' of the same release is active at 11.2.0 [npm@node-24.15.0] — xvm-active-group-incoherent — run `xlings use node 24.15.0` to bring the whole release back in step
```

`self doctor` 是**最需要被读懂**的输出(它的存在意义就是告诉用户哪里坏了、怎么修),却是折行最惨的一个。remedy 在行尾,恰好是折行后最难找到的位置。

### P1-5 — 下载进度条的光标回退按逻辑行记账

`src/ui/progress.cppm:352` 返回 `screen.dimy()`(文档行数),`:345` 用它做 `\033[NA` 回退。但终端折行后**物理行数 > dimy**,回退量不足 → 上一帧残留,逐帧堆积。

阈值可算:每行宽度 = icon(6) + `nameWidth` + 1 + status(8),而 `nameWidth = max(20, 最长包名) + 2`(`downloader.cppm:874-878`)。装 `xim:aarch64-linux-musl-gcc@15.1.0`(33 字符)时 nameWidth=35 → 行宽 50。**终端窄于 ~50 列,安装进度就会糊。** 分屏 / 手机 SSH 场景可达。

### P1-6 — 所有 padding 按字节数,不是显示宽度

`info_panel.cppm:26`、`banner.cppm:15`、`selector.cppm:84/142`、`progress.cppm:83` 全部用 `std::string::size()`。中文描述(每字 3 字节 / 2 列)必然错列。`src/core/utf8.cppm` 只有 `safe_truncate`,**没有 display-width 函数**;ftxui 自带的 `string_width()` 从未被 `src/ui/` 调用过。

考虑到 xlings 的用户基本盘,这条的实际影响比排位显示的要大。

### P2-7 — 分隔线硬编码 40 个 `─`

`info_panel.cppm:54/72`。面板实际渲染到 90 或 230 列时,底下压着一条 40 列的短横线,视觉上像没画完:

```
  ◆ xlings self doctor                                    ...(230 列)
  ────────────────────────────────────────                          ← 40 列
```

---

## 二、非 TTY / 管道:输出不可脚本化

### P0-8 — 管道输出仍带 truecolor、行尾 `\r`、行尾空格、结尾 NUL

`xlings use gcc | cat -A` 实测:

```
^[[38;2;168;85;247m  ◆ ^[[1mgcc versions (current subos)^[[22m ... (空格填充) ^M$
                                                                  ^@$
```

- **ANSI 色码**:`Screen::ToString()` 只看 `COLORTERM`/`TERM`,不看 isatty;
- **`\r`**:ftxui 行间用 `"\r\n"`,`grep`/`awk` 会看到 CR;
- **行尾空格**:填充到画布宽度;
- **NUL 字节**:`Screen::Print()` 是 `std::cout << ToString() << '\0'`(`screen.cpp:453`)。

xlings **自己知道**怎么判断 TTY——`main.cpp:28` 和 `platform/linux.cppm:201` 都在用 `isatty`——但整条 ftxui 渲染链一次都没问过。

### P0-9 — 不认 `NO_COLOR`,而 xlings 自己生成的 shell profile 认

`NO_COLOR=1 xlings use gcc` 实测仍输出 truecolor。同时 `src/core/xself/profile_resources.cppm:113/166/209` **为三种 shell 都写了 `NO_COLOR` 判断**。xlings 要求自己生成的脚本尊重 `NO_COLOR`,自己却不尊重。

### P1-10 — 非 TTY 时 fallback 80×24,报告被裁到 80 列

ftxui `Terminal::Size()` 在非 TTY 下返回 80×24(`terminal.cpp:41-42`)。CI 日志、`| less`、重定向到文件时,所有 `Dimension::Full()` 的输出都按 80 列裁切。`fit_full_height` 已经处理过高度方向的同一个坑(注释里写得很清楚),宽度方向没有。

---

## 三、一致性:三套输出通道,四套规则

### 三条并行通道

| 通道 | 缩进 | 前缀 | 颜色来源 | 响应浅色主题 | 响应 `--agent` |
|---|---|---|---|---|---|
| `src/ui/*`(ftxui) | 2 空格 | 无 | `theme::` | ✅ | ✅ |
| `log::info/warn/error` | 0 | `[xlings]` / `[warn]` | `log.cppm:68` 硬编码 | ❌ | ✅(`enable_color(false)`) |
| `subos_ansi_`(`info_panel.cppm:208`) | 2 空格 | 无 | 硬编码 | ❌ | ❌ |
| `confirm()` / `read_line()`(`selector.cppm:204/219`) | **0** | 无 | 硬编码 | ❌ | ❌ |

一次 `xlings install` 会同时经过这四条,用户看到的缩进在 0 和 2 之间反复跳。

### P0-11 — 浅色主题只覆盖了一半的输出

`theme.cppm` 用 92 行注释 + OSC-11 探测 + `COLORFGBG` 回退实现了明暗自适应,理由写得很明确("near-white text became invisible on a light terminal background")。但:

- `log.cppm:73-77` 硬编码 dark 调色板;
- `info_panel.cppm:210-215`(subos 消息)硬编码 dark 调色板;
- `selector.cppm:204/219`(输入提示、y/n 确认)硬编码 `38;2;34;211;238` 和 `38;2;148;163;184`。

于是在浅色终端上,**主题要修的那个低对比问题原封不动地留在了确认提示和状态日志上**——而确认提示恰恰是必须看清的那一行。

### P0-12 — "当前激活"有三种编码,其中一种只用颜色

| 命令 | 编码方式 |
|---|---|
| `xlings use <t>` | **仅绿色**(`info_panel.cppm:29`) |
| `xlings info <t>` | 尾部 `*`(`versions  ... 16.1.0 *`) |
| `xlings subos list` | `▸` marker + 青色 |

`use` 那条是纯颜色编码。实测 100 列输出里只有一段绿色转义 —— 一旦 `NO_COLOR`、管道、`| less -R` 失效、或用户有色觉障碍,**"哪个版本是当前激活的"这条信息完全消失**,而这正是 `xlings use <target>` 唯一要回答的问题。

### P1-13 — `theme::icon` 定义了"安全字形集",`doctor.cppm` 绕过它

`theme.cppm:153-176` 花了 20 行注释论证为什么只用某几个 BMP 字形,并点名 "Avoid: U+27D0 ⟐, U+2699 ⚙, U+27F0 ⟰ — spotty in older console fonts"。

`src/core/xself/doctor.cppm` 用了 4 个不在集合里的字形:`⚠`(U+26A0)、`ⓘ`(U+24D8)、`·`(U+00B7)、`→`(U+2192)。其中 `ⓘ` 是 Enclosed Alphanumerics,和注释里点名回避的 U+2699 属于同一风险类(旧 console 字体覆盖不全)。两个都不在 ftxui 的全宽表里(`string.cpp:32` `g_full_width_characters`,`0x26a0` 恰好落在 `0x026a1` 之前),ftxui 按 1 列算;在把 East-Asian-Ambiguous 当双宽渲染的终端上会产生逐行右移。

问题不在这几个字形本身,在于**有一个显式的字形治理约定,而它没有强制力**。

### P1-14 — `xlings info` 一个面板里 "versions" 出现两次,含义不同

```
  versions        latest -> 16.1.0, 15.1.0, 11.5.0, 16.1.0, 13.3.0, 9.4.0   ← 索引里可装的
  installed       yes
  ────────────────────────────────────────                                   ← 无标题分隔
  active          16.1.0
  versions        15.1.0, 15.1.0-aarch64-musl, 15.1.0-musl, 16.1.0 *         ← 本地已装的
```

- 同名不同义,中间只有一条无标题的分隔线,读者无法知道上半区是"包(索引)"、下半区是"本地状态";
- `latest -> 16.1.0, 15.1.0, 11.5.0, **16.1.0**, ...` —— 16.1.0 出现两次;
- `bindings  xim-aarch64-musl-gnu-gcc, xim-gnu-gcc -> 16.1.0, xim-musl-gnu-gcc` —— 逗号分隔的列表里嵌了一个 `->`,无法判断它作用于哪一项。

### P1-15 — 路径缩写策略不一致

`Config::display_path()`(`config.cppm:872`)会把路径缩成 `@xlings/data/...`。`xlings config` 和 `self doctor` 用了它,`xlings use` / `xlings info` 没用:

```
xlings config:   XLINGS_DATA     @xlings/data
xlings use gcc:  15.1.0          /home/speak/.xlings/data/xpkgs/xim-x-gcc/15.1.0/bin
```

而 `use` 恰恰是被路径长度撑爆的那个命令 —— **缩写函数已经存在,用上它就能让 P0-1/P0-2 的溢出直接消失**。

### P1-16 — `--agent` 的纯文本输出比 TUI 更正确

同一条命令:

```
$ xlings --agent use gcc
gcc versions (current subos)
  15.1.0: /home/speak/.xlings/data/xpkgs/xim-x-gcc/15.1.0/bin
  15.1.0-aarch64-musl: /home/speak/.xlings/data/xpkgs/xim-x-aarch64-linux-musl-gcc/15.1.0/bin   ← 完整
  15.1.0-musl: /home/speak/.xlings/data/xpkgs/xim-x-musl-gcc/15.1.0/bin
  16.1.0: /home/speak/.xlings/data/xpkgs/xim-x-gcc/16.1.0/bin
```

不截断、不折行、无转义污染。**TUI 在这里没有增加任何信息,只减少了信息。**

附带风险:`cli.cppm:47+` 和 `agent/text_renderer.cppm:22+` 是两份手写的 kind 分发,必须手工保持同步;`text_renderer` 漏掉一个 kind 就静默丢输出(注释里已经承认 "Silently skips event kinds")。

---

## 四、可发现性与"方便"

### P1-17 — `xlings use gcc` 列了版本,却不告诉你下一步

成功路径只打面板,没有 `xlings use gcc 16.1.0` 这样的下一步提示。而**失败**路径(`xvm/commands.cppm:520`)反而给了完整 hint。提示的密度和用户的困惑程度成反比。

### P1-18 — 交互选择器已经写好,但没接上

`src/ui/selector.cppm` 里的 `select_version()`(第 16 行)完全实现了"上下选版本 + 回车确认",**零调用者**。同样零调用者的还有 `select_option()`、`read_line()`、`print_progress()`。

也就是说,`xlings use gcc` 最自然的行为——弹一个选择器——**代码已经在仓库里躺着了**。

### P2-19 — 确认提示不缩进

`selector.cppm:219`:

```cpp
std::print("\033[...m{}\033[0m{}...", std::string(theme::icon::arrow) + " ", message, prompt);
```

没有前导空格,`▸ Continue? [Y/n]` 从第 0 列开始,而它上面的 remove plan 是 2/4 空格缩进(`info_panel.cppm:326-334`)。视觉上确认行"掉出"了面板。

---

## 五、建议:引入一个宽度感知的渲染契约

单点修 bug 会继续漏,因为缺的是**约定**。建议加一个 `src/ui/layout.cppm`,把下面五件事变成唯一入口:

### 1. 单一宽度源

```cpp
// 终端宽度;非 TTY 返回 nullopt = "不裁切、不填充"
std::optional<int> term_width();
```

规则:**任何画布宽度不得超过 `term_width()`**。P0-1 的 `std::max(term, minWidth)` 改成 `std::min(natural, term)`。

### 2. 按列语义分级的收缩策略

超宽时不能等比压缩,要按语义:

| 列类型 | 策略 |
|---|---|
| 标识符(包名 / 版本 / subos 名) | **永不截断**,宽度优先分配 |
| 路径 | 先 `Config::display_path()` 缩写,再中段省略 `…` |
| 描述 / remedy | 末尾截断 + `…`,或换行续行(缩进对齐) |

`self doctor` 的 remedy 建议直接改成独立续行,不参与列宽竞争。

### 3. 显示宽度函数

```cpp
int display_width(std::string_view);   // 包装 ftxui::string_width
std::string pad_to_width(std::string, int);
std::string truncate_to_width(std::string_view, int);  // 带 … 且不切断 UTF-8
```

替换 `src/ui/` 里全部 `.size()` 形式的 padding(5 处)。

### 4. 输出模式判定 + 自有 print

```cpp
enum class OutMode { Rich, Plain };   // isatty && !NO_COLOR && TERM!=dumb && !--agent
void print_doc(ftxui::Element);       // 逐行 rtrim、用 "\n"、不写 '\0'
```

`Plain` 模式直接复用现成的 `agent::render_data_event` —— 它的输出已经被验证是更好的(P1-16)。同时这条修掉 P0-8 / P0-9 / P1-10。

### 5. 进度条按物理行记账

`render_download_progress` 返回值改为 `Σ ceil(行宽 / term_width)`;或更简单:把行宽 clamp 到 `term_width()-1`,让物理行数恒等于逻辑行数。

### 另外两条约定

- **`theme::icon` 是唯一字形来源**,`doctor.cppm` 的 `⚠ ⓘ · →` 收编进去(补 `warn` / `note` / `bullet` / `remedy`),加一条 grep 测试防回归;
- **"激活/当前"必须有非颜色编码**(统一用 `▸` 或 `*` 之一),颜色只做增强。

---

## 六、优先级

| 编号 | 问题 | 影响 | 成本 |
|---|---|---|---|
| P0-1 | info_panel 画布顶超终端宽 → 硬折行 | 用户直接报告的症状 | 小(一行) |
| P0-2 | minWidth 用固定 14 → 静默截断 | 输出丢字符,无提示 | 小 |
| P0-3 | hbox 等比压缩截掉包名/版本号 | **输出错误信息** | 中 |
| P0-4 | `self doctor` 单行 230 字符 | 最该读懂的输出最难读 | 中 |
| P0-8 | 管道输出带色/CR/NUL/尾空格 | 不可脚本化 | 中 |
| P0-9 | 不认 `NO_COLOR`(自己生成的脚本却认) | 自相矛盾 | 小 |
| P0-11 | 浅色主题只覆盖一半输出 | 确认提示看不清 | 小 |
| P0-12 | "激活版本"仅用颜色编码 | 无障碍 + 管道下信息全失 | 小 |
| P1-5 | 进度条按逻辑行回退光标 | 窄窗口安装画面糊 | 小 |
| P1-6 | 按字节 padding | 中文必然错列 | 中 |
| P1-13 | doctor 绕过 `theme::icon` | 字形治理失效 | 小 |
| P1-14 | `info` 面板 versions 二义 + 重复 | 读者误解 | 小 |
| P1-15 | 路径缩写不一致 | 顺手修掉 P0-1 溢出 | 小 |
| P1-17 | `use` 成功路径无下一步提示 | 可发现性 | 小 |
| P1-18 | 交互选择器是死代码 | 已有实现未接线 | 小 |
| P2-7 | 分隔线硬编码 40 列 | 观感 | 小 |
| P2-19 | 确认提示不缩进 | 观感 | 小 |

建议的第一批(一个 PR 内可完成,风险低、收益最直接):
**P0-1 + P0-2 + P1-15**(三处都在 `info_panel.cppm`,合起来直接消灭用户贴出来的那个症状)。

---

## 实施记录(2026.7.29.1)

全部 17 项在一个 PR 内落地。核心是新增三个「单一来源」模块,把原先散落各处的决策收敛:

| 模块 | 收敛了什么 |
|---|---|
| `src/ui/layout.cppm` | 宽度与输出契约:`term_width()` / `fit_width()` / `display_width()` / `truncate_to_width()` / `wrap_to_width()` / `plan_two_column()` / `render_to_string()` |
| `src/core/glyph.cppm` | 字形表(放在 core,因为 `self doctor` 在 core 里拼标签) |
| `src/core/palette.cppm` | 调色板 + 背景探测 + `colors_enabled()`(同上,`log::` 在 core 里写 SGR) |

`ui::theme` 变成前两者的 ftxui 适配层。规则:

1. 画布**永不宽于终端**;放不下由**调用方**决定裁剪方式,不交给 ftxui。
2. 非终端(管道/文件)**没有宽度上限** —— 拿到完整未截断的文本。
3. 宽度一律按显示列,不按字节。
4. 输出是纯文本 + 可选 SGR:无 NUL、无 CR、无行尾空格;非交互终端或用户拒绝时无颜色。
5. **标识符(包名/版本/subos 名)永不截断** —— 放不下就换行;描述才加 `…`。

新增 `XLINGS_TERM_WIDTH`(整数,`0` = 不限)覆盖宽度探测,测试与用户逃生口共用。

### 实施中发现的三个额外问题

- **第三张字形表**:`src/platform/{linux,macos,windows}.cppm` 各有一份 `Icon`,**零调用者**,而且正好包含注释点名要避免的 `⚙`(U+2699)和 `⟐`(U+27D0)。已删除 —— 与规则矛盾的死代码正是规则失效的方式。
- **stacked 模式仍在裁包名**:名字宽于终端时改成整行,但没让它换行,ftxui 照样在画布边缘剪掉。已改为换行,一个字符不丢。
- **测试本身是静默通过的**:`python3 - <<'PY'` 会把 heredoc 当成 python 的**程序**,管道进来的数据永远读不到,`sys.stdin.read()` 返回 `""` —— 宽度检查因此对任何输入都通过。改为独立脚本文件,并加了 S0 自检(空输入直接报错 + 一条故意超宽的行必须被抓到)。这正是本文反复讲的那类 bug 出现在了检查它的工具里。

### 覆盖

- `tests/unit/test_ui_layout.cpp` —— 25 个用例:显示宽度、UTF-8 边界截断、路径优先按 `/` 换行、列规划、`fit_width` 钳制。
- `tests/e2e/tui_output_contract_test.sh`(E2E-44)—— S1 无行超宽(40/60/100/200 列 × 7 条命令)、S2 包名不截断且 help 列对齐、S3 管道干净、S4 真 pty 下 `NO_COLOR` 生效、S5 激活项有非颜色标记 + 下一步提示、S6 字形只来自字形表。

对 2026.7.29.0 的二进制跑同一个 e2e,**在 S1 立刻失败**(40 列终端里出现 81 列的填充行)—— 说明它确实是回归护栏,不是摆设。

---

## 附录:复现脚手架

```python
# runpty.py — 在指定尺寸的 pty 里跑命令,原样 dump 输出
import os, sys, pty, fcntl, termios, struct, select, subprocess
cols, rows, cmd = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3:]
mfd, sfd = pty.openpty()
fcntl.ioctl(sfd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
p = subprocess.Popen(cmd, stdin=sfd, stdout=sfd, stderr=sfd, close_fds=True)
os.close(sfd)
out = b""
while True:
    r, _, _ = select.select([mfd], [], [], 20)
    if not r: break
    try: d = os.read(mfd, 65536)
    except OSError: break
    if not d: break
    out += d
p.wait(); sys.stdout.buffer.write(out)
```

用法:

```bash
python3 runpty.py 60 24 xlings use gcc | sed 's/\x1b\[[0-9;]*m//g' | awk '{printf "%d: %s\n", length($0), $0}'
python3 runpty.py 40 24 xlings --help
xlings use gcc | cat -A          # 管道行为
NO_COLOR=1 xlings use gcc | cat -v
```

> 注意:文件名不能叫 `pty.py`,会 shadow 标准库。
