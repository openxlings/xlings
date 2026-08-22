# 交互范围、主题配置与目录归属 —— 设计方案

> 上游:`2026-08-22-cli-diagnostics-experience-design.md` · `2026-08-22-multi-frontend-architecture-design.md`
> 已发布:2026.8.22.1(#556)、2026.8.22.2(#557)
> 本文覆盖四件事:`use <name>` 的语义、`--interactive` 的覆盖范围、主题配置文档与回退、`apps/` 与 `modules/` 的目录归属

---

## 0. 结论先行

四件事里,有三件不是偏好问题,是**文档与实现不一致**或**契约未被处理**,因此不需要在方案上做取舍:

| | 判定 | 依据 |
|---|---|---|
| §1 `use <name>` 单候选自动切换 | **缺陷** | `--help` 写的是 "omit to list installed versions",代码在单候选时切换 |
| §2 `select_package` 未处理 `kCannotAsk` | **缺陷** | 实测:`xlings install gc` 打印 "cancelled" 且**退出码 0**,用户从未被问过 |
| §3 `--theme` 不校验取值 | **缺陷** | 同一函数里 `--ui-mode` 和 `--interactive` 都校验并 exit 2,只有 `--theme` 不校验 |
| §3.3 内置主题存成 C++ 字符串字面量 | **设计错位** | 一份内容以源码形式存在,只为了在运行前变回文件;且"主题如何到达 home"因此有三个回答者,可互相覆盖 |
| §4 目录归属 | 偏好 | 无功能影响,仅命名与一致性 |
| §4a `platform`/`cancellation` 独立成 module | 可行 | 依赖图无环且只有一层;platform 的非 `std` 依赖仅 `cancellation` |
| §4b i18n 的 zh 不可达 | **缺陷** | 36 条译文齐备,`tr()` 的产品调用点为 **0** |

需要你裁决的集中在 §5.1 的 D-a…D-f 六项。其余条目要么是已判定的缺陷(修法无分歧),要么是已确认的偏好(目录、模块化)。

---

## 1. `xlings use <name>` 的语义

### 1.1 你观察到的行为是真的,但不是这一轮引入的

```
$ xlings use gcc
[xlings] gcc -> 16.1.0  (xim:gcc 16.1.0)
exit=0
```

三次语义变更的时间线:

| 版本 | 单候选 | 多候选 |
|---|---|---|
| 2026.7.30.2 之前 | 有 TTY → 阻塞式选择器;无 TTY → 列表 (0) | 同左 |
| **2026.7.30.2** (#455) | **切换 (0)** | 带候选拒绝 (**2**) |
| **2026.7.31.3** (#463) | 切换 (0) | **列表 (0)** —— "一个列表不是错误" |
| 2026.8.22.1 (#556) | 切换 (0) | 开了 `interactive` → 内联选择器;否则列表 (0) |

你记忆里的"输出已安装版本"对应 2026.7.30.2 之前,以及现在的多候选分支。变的是**单候选**那一支,来自 2026.7.30.2,当时的理由是:

> 人是否在键盘前不可检测,而命令是否只有一个正确结果可以检测。

这个理由本身成立。问题出在别处。

### 1.2 真正的缺陷:同一条命令是查询还是变更,取决于用户看不见的计数

`src/cli.cpp:1492` 的参数帮助写着:

```cpp
.arg("version").help("Version to switch to (omit to list installed versions)")
```

**省略版本 = 列出已安装版本。** 这是文档承诺。而 `cmd_use_by_name` 在候选恰好为 1 时执行切换。于是:

- 当前 subos 里 gcc 只装了一个 → `use gcc` **写状态**
- 装了两个 → `use gcc` **只读**

用户无法在敲命令前知道自己会落到哪一支,因为候选数是"当前 subos 已 opt-in 的版本数",不是他心里的"我装过几个 gcc"。你这次就撞上了这个:`use gcc --all` 显示 5 个版本,`use gcc` 却只有 1 个候选。

这不是"该不该自动切换"的口味问题,是**声明的契约与实现不一致**。

### 1.3 方案:省略版本恒为查询

```
xlings use <name>                 列出当前 subos 已安装的版本 + 当前活跃的那个    (0)
xlings use <name>          (interactive=true 且有 tty)  同上,并允许直接选中    (0)
xlings use <name> <ver>           切换                                          (0)
xlings use <name>@<ver>           同上
xlings use <name> --all           跨 subos 的全局视图                            (0)
```

即删除 `cmd_use_by_name` 里的单候选自动切换分支。

**代价与补偿。** 2026.7.30.2 的注释记录了单候选切换顺带承担的一个用途:

> This also covers the documented sysroot-repair use of `use` -- switching to the version that is already active re-materializes headers and libraries.

这个用途不该寄生在"候选恰好为 1"上 —— 它在候选为 2 时就失效了,所以它从来只是偶然可用。给它一个显式拼写:

```
xlings use <name> <ver>           已经活跃时同样重新物化 sysroot(现有行为)
```

即用户仍然可以 `xlings use gcc 16.1.0` 完成修复,而且这条在任何候选数下都成立。文档里把它写成 sysroot 修复的正式方法。

**为什么不选"保留切换但说得更清楚"。** 例如让单候选时输出 `gcc -> 16.1.0 (already active)`。这只修了措辞,没修"一条命令两种语义"。用户下次在另一台候选数不同的机器上仍然会被意外写状态。

### 1.4 影响面

- 退出码:全部保持 0,脚本不受影响(2026.7.31.3 已经确立"列表不是错误")
- 唯一行为变化:单候选时不再写 workspace。依赖 `xlings use <name>` 做激活的脚本需要补版本号 —— 而它们本来就应该补,因为多候选时那条命令什么也没做过
- E2E-51 `install_use_semantics_test.sh` 需要更新;新增一条断言:**同一条 `use <name>`,在候选为 1 和为 2 时产生同一种结果类型**

---

## 2. `--interactive` 的覆盖范围

### 2.1 现状:一处

`tui.interactive`(bool,缺省 false)→ `uimode.cpp:78` 的三条件与门:

```cpp
c.interactive = interactivePreference && !agentMode && env.stdoutIsTerminal;
```

→ `cli.cpp:1098` 唯一一根线 `stream.set_interactive(...)` → 下游四个 prompt 调用点:

| 调用点 | id | 受控? | 状态 |
|---|---|---|---|
| `xvm/commands.cpp:777` `use <name>` 版本选择 | `select_version` | 是 | 正确,`kCannotAsk` 回落到面板 |
| `xim/commands.cpp:86` 安装/卸载确认 | `confirm_*` | 否(设计如此) | 正确,由 `confirmed_or_refused_` 的每调用方策略决定 |
| `cli.cpp:1790` 第三处确认 | — | 否 | 同上 |
| `xim/commands.cpp:443` 模糊包名消歧 | `select_package` | **两者都不是** | **缺陷,见 2.2** |

所以准确的现状是:**`--interactive` 当前只改变 `xlings use <name>` 一处的行为。**

### 2.2 缺陷:`select_package` 把"没人能答"读成了"用户取消了"

```cpp
auto chosen = stream.prompt(std::move(req));
if (chosen.empty()) { log::println("cancelled"); return 0; }
```

`kCannotAsk` 是 `"\x01cannot-ask"`,**不是空串**。于是非交互时:跳过取消分支 → 候选里匹配不到 → `if (!match)` → 打印 "cancelled" → **return 0**。

在已发布的 2026.8.22.2 上实测(sandbox 内):

```
xlings install gc     → cancelled   exit=0
xlings install pyth   → cancelled   exit=0
xlings install nod    → cancelled   exit=0
```

触发条件:`install` 一个产生 2–5 个模糊匹配的名字,不带 `-y`,`interactive` 为缺省的 false —— 也就是绝大多数调用。用户从未被问过,也从未取消过,读退出码的脚本被告知成功。

这与 `confirmed_or_refused_` 当初(2026.8.22.1)为另外三个点消除的是同一个缺陷,第四个点被漏掉了。

### 2.3 修法

消歧属于**装错比不装更糟**的一类,策略取 Refuse:

```
[error] 'gc' matches more than one package
  candidates   gcc, gcc-musl, gccgo, ...
  name one     xlings install gcc
  or accept the first   xlings install gc -y
```

退出码 2(与 2026.7.30.2 确立的"需要用户指明"同码)。开了 interactive 且有 tty 时,同一处改为内联选择器,回落仍是上面这块。

### 2.4 覆盖范围:凡是用户必须做选择的地方

**原则(已确认):只要一个动作要求用户做选择,交互就必须能覆盖到它** —— 包括 y/n,不只是列表。

#### 2.4.1 现状:三套互不相干的问法

| 机制 | 走 EventStream | 受 `--interactive` 控制 | 受 `--agent` 控制 | 对 `xlings interface` 可见 |
|---|---|---|---|---|
| `confirmed_or_refused_`(`xim/commands.cpp:77`) | 是 | 是(但见下) | 是 | 是 |
| `stream.prompt` 直接调用 | 是 | S1 是,S2 否 | 是 | 是 |
| `utils::ask_yes_no`(`core/utils.cpp:58`) | **否,直读 `std::cin`** | **否** | **否** | **否** |

第三套在 EOF(管道)时 `return defaultYes` —— 正是 `kCannotAsk` 当初要消除的"猜一个答案"。`install.cpp:650` 的默认值还随方向变(`!downgrade`),于是管道里升级自动同意、降级自动拒绝,都不出声。

#### 2.4.2 全部决策点(实测)

| # | 决策点 | 机制 | 默认配置下的实际行为 | 判定 |
|---|---|---|---|---|
| C1 | `confirm_install` "Proceed with installation?" | EventStream + Proceed | **不问,直接装** | 该问未问 |
| C2 | `confirm_remove` "Remove X from subos S?" | EventStream + Refuse | **不问,报 `there is nobody to ask`,exit 2** | 用户就在键盘前 |
| C3 | `confirm_update` "Upgrade X from A to B?" | EventStream + Proceed | **不问,直接升** | 该问未问 |
| S1 | `select_version`(`use <name>` 多候选) | EventStream,受门控 | 回落到候选面板 | 正确 |
| S2 | `select_package`(install 模糊匹配 2–5 项) | EventStream,**未门控** | **`cancelled` + exit 0** | 静默成功(§2.2) |
| Y1 | `self install` 同版本重装 | `ask_yes_no` | 管道时取默认 false | 绕过全部门控 |
| Y2 | `self install` 覆盖 data/subos | `ask_yes_no` | 同上 | 同上 |
| Y3 | `self install` 升级/降级 | `ask_yes_no`,默认值随方向 | 管道时升级自动是、降级自动否 | 同上,且方向不同答案不同 |
| L1 | `subos use` 无参 | 无 | 列出全部 subos,要求重打名字 | 可覆盖 |
| L2 | `config --theme list` | 无 | 列出主题,要求重打名字 | 可覆盖 |
| L3 | shim `no_active_version` | 无 | `pick one  xlings use X <ver>` | 可覆盖 |
| L4 | `use <name>` 多候选面板 | S1 的回落 | — | 已是 S1 的一部分 |

C2 的实测原文(终端上,默认配置):

```
[error] this needs confirmation, and there is nobody to ask
          what it would do   remove xim:mcpp-short-cmd@0.0.1 from subos 'eco-2026-8-22-1'
          to proceed         re-run with -y
          nothing was changed
exit=2
```

#### 2.4.3 `-y` 目前名不副实

`--yes` 的语义是"别问我"。而默认配置下**没有任何一个问题会被问出来**,所以 `-y` 实际只在"没人可答"的分支里选择 Proceed/Refuse。一个记录着"存在一个提问"的旗标,而那个提问从不发生。

这是把覆盖范围定下来的最强理由:不是"要不要更交互",而是**已声明的契约当前不成立**。

#### 2.4.4 方案:两层门控

把现在被混在一起的两个问题拆开 —— "**有没有人可问**"与"**没人时怎么办**"。后者是 `WhenNobodyCanAnswer`,保持不变;前者要分成两档,因为两类提问的阻塞风险不同:

| 层 | 覆盖 | 门控 | 无人可答时 |
|---|---|---|---|
| **确认**(y/n,**有默认值**) | C1 C2 C3 Y1 Y2 Y3 | `stdin` 是终端 **且** 非 `--agent` | `WhenNobodyCanAnswer` 策略(现状) |
| **选择**(列表,**无默认值**) | S1 S2 L1 L2 L3 | 上述条件 **且** `tui.interactive = true` | 完整候选面板(不是错误) |

两点关键:

**其一,门控看 `stdin` 而不是 `stdout`。** 现在是 `env.stdoutIsTerminal`(`uimode.cpp:78`)。能不能回答取决于输入端:`xlings install foo | tee log` 的 stdout 是管道而 stdin 仍是终端,用户完全答得了;反之 stdout 是终端也不代表 stdin 有人。`platform::stdin_is_terminal()` 已存在(2026.7.30.2 用过)。

**其二,确认层不再由 `tui.interactive` 控制。** 因为它不是外观偏好,而是 `-y` 的文档契约。`tui.interactive` 继续只管选择层 —— 那一层才是 E2E-48 N3 当初禁止的东西:**无默认值、会无限阻塞**的选择器。这样两个约束同时成立:用户的原则(凡需选择皆可交互)与 2026.7.30 的决定(默认不阻塞在选择器上)。

#### 2.4.5 风险与代价

**风险:分配了 pty 但不回答的调用方会阻塞在确认上。** 这正是 2026.7.30 拒绝 TTY 门控的理由,现在被限制到了确认层。缓解:`--agent` 关闭它;agent 技能文档已经写明"非交互场景一律传 `--yes`"(`test_prompt_refusal.cpp` 的注释引用了这条)。**不引入超时** —— 超时后套用默认值就是"猜一个答案",本轮反复在消除的正是它。

因此 e2e 必须用**关闭的 stdin**而不是 pty 来断言:EOF → `kCannotAsk` → 策略分支,这条可测且确定。"pty 但永不回答"不可测,只能靠契约。

**代价:`xlings install foo` 恢复成会问一次。** 这是 `-y` 一直声明的行为,也是 apt/dnf 的常态。脚本本来就该传 `-y`。

**Y1–Y3 需要迁移到 EventStream。** `ask_yes_no` 随之删除。这三处目前对 `xlings interface` 完全不可见,mcpp 驱动 `self install` 时看不到任何提问 —— 迁移后它们成为普通的 `PromptEvent`,与其他确认同一条线。

| I7 | `config --lang` / `--mirror` / `--theme` / `--ui-mode` **不带值** | 无(报缺少参数) | **纳入** | 取值是**有限枚举**,而用户往往正是「想看看有哪些」才这样敲;带值时直接生效,不打断脚本 |

#### 2.4.6 不纳入

| # | 决策点 | 理由 |
|---|---|---|
| N1 | `remove <name>` 选哪个版本 | 已确定性解析(优先当前活跃版本),没有要问的问题 |
| N2 | 首次运行选镜像 | 此时用户没有判断依据,探测优于提问 |
| N3 | `--force`(依赖仍在时强删) | `cli.cpp:1409` 的注释写明它与 `-y` 是**不同的决定**:`-y` 是"别问我",`--force` 是"接受破坏链接"。一个脚本化的 `remove -y` 不应顺带获得后者 |

### 2.5 让契约变成类型 —— 需要你裁决

`select_package` 的缺陷不是粗心,是**接口允许粗心**:`prompt()` 返回 `std::string`,三种结局(选中 / 用户取消 / 没人可问)挤在一个字符串里,其中两种都可能"看起来像正常值"。调用方漏掉一种,编译器无话可说。

建议改成:

```cpp
struct Chosen      { std::string value; };
struct Cancelled   {};                      // 人在,选择了不做
struct NobodyToAsk {};                      // 没有人可问

using PromptOutcome = std::variant<Chosen, Cancelled, NobodyToAsk>;
```

并让 `EventStream::prompt` 返回它。调用方用 `std::visit` 配一组重载,**少写一个 alternative 就是编译错误** —— 不是警告。这一点需要 variant 而不是 `enum + string`:本仓库 `cxxflags` 里没有 `-Werror`(实测,只有三个 `-D`),枚举 `switch` 漏分支只会是警告,在这条日志量下等于没有。

`kCannotAsk` 这个哨兵随之删除。它是"用字符串编码控制流"的产物 —— 三种结局挤进一个 `std::string`,其中两种看起来都像正常值,而 §2.2 的漏判正是这样发生的。

代价:四个调用点各改约 10 行;`test_prompt_refusal.cpp` 的 5 个用例改断言形式,断言内容不变。

不做的话,§2.4 每纳入一个新决策点,就多一次漏判 `NobodyToAsk` 的机会,而这种漏判的表现是**退出码 0 且什么也没发生**。

### 2.6 `remove` 借用了一条为 `use` 写的诊断,动作正好相反

分析交互覆盖时顺带发现,与交互无关但同族。

`xim/commands.cpp:844`(2026.8.22.1 / #556 引入,是我这轮加的)在"要删的包不在当前 subos、但别的 subos 有"时复用了 `xvm::not_in_subos`,覆盖了 `.level` 与 `.summary`,却**继承了它的 actions** —— 那些 actions 是为"你想用这个包"写的:

```
[warn] mcpp-short-cmd is not installed in this subos (eco-2026-8-22-1), so there is nothing to remove
         installed in subos   gfxbuild
         switch there         xlings subos use gfxbuild        ← 去一个你没问的 subos
         install it here      xlings install mcpp-short-cmd    ← 你要删,它让你装
```

最后一条动作是请求的**反面**。第二条把用户引向一个他没有过问的 subos,而且到了那里也不会删掉任何东西。

同一状态还有第二个回答者:`commands.cpp:952` 的 `xim.remove_absent`,措辞相近而动作正确(`see what is here → xlings list`)。两者的分界是"是否有别的 subos 引用它",而这个分界与"该给什么动作"无关。

**删除本身的作用域是对的**(已核代码,未改动任何真实 subos):`Installer::uninstall` 经 `is_version_referenced_anywhere_` 判定,若他处仍引用则走 `detach_current_subos_`,只摘除当前 subos 的 opt-in,保留 payload。出问题的只是报告。

修法:`remove` 不复用 `not_in_subos`,合并到 `xim.remove_absent` 一个回答者;`installed in subos` 这条事实保留(知道它在哪儿是有用的),动作换成对删除有意义的两条:

```
[warn] mcpp-short-cmd is not installed in this subos (eco-2026-8-22-1), so there is nothing to remove
         installed in subos   gfxbuild
         see what is here     xlings list
         remove it there      xlings subos use gfxbuild && xlings remove mcpp-short-cmd
```

第二条是否给,取决于是否认为跨 subos 的删除值得一条现成命令。建议给 —— 它是用户此刻唯一可能想要的下一步,而且它是删除,不是安装。

---

## 3. 主题配置:文档与回退

### 3.1 现有回退行为(已实现,`src/cli.cpp:855` `load_configured_theme_`)

| 情形 | 行为 |
|---|---|
| `theme` 未设 / 为 `default` | 用编译进二进制的默认主题,不读盘 |
| 文件不存在 | `[warn] theme.not_found`,回退默认,给出 `--theme list` 与 `--theme default` |
| 文件不可读 | `[warn] theme.unreadable`,回退默认 |
| JSON 非法 | `[warn] theme.bad_json`,回退默认 |
| 槽位名拼错 | `[warn] theme.unknown_slot` + 近似建议(有距离阈值) |
| 颜色值非法 | `[warn] theme.bad_color`,该槽保持默认 |
| 部分槽位缺失 | 静默继承默认 —— 这是**设计**,不是回退 |

关键性质:**局部错误只影响出错的槽位**,解析成功的部分照常生效。整份丢弃会把"一行写错"放大成"整个主题不生效",那正好是这套诊断要消除的形状。

### 3.2 两个缺口

**缺口 A:`--theme` 不校验取值。** 同一个函数里:

```cpp
if (value != "auto" && !ui::parse_mode(value)) { ...; return 2; }   // --ui-mode 校验
if (value != "true" && value != "false")       { ...; return 2; }   // --interactive 校验
edits.push_back([value](json& j) { j["theme"] = value; });          // --theme 不校验
```

于是 `xlings config --theme mnoo` 回显 `theme = mnoo` 并成功退出,错误要到**下一条命令**才以警告形式出现,而且从此每条命令都出现一次。设置动作报告成功,而它设的值不可用。

修法:与两个兄弟一致 —— 在 `--theme` 落盘前解析一次(内置名 / 已装文件名 / 路径),解析不出就 exit 2 并列出可用项。校验用的是 `resolve_theme_path` 加一次 `is_regular_file`,与运行期读的是同一条路径,不引入第二个判断者。

**缺口 B:配置指向不存在的文件时,警告每条命令重复一次。** 这是真实误配,不该静默;但"每次都说"和"说一次"之间还有第三种:**在 `--theme list` 与 `self info` 里把失效状态标出来**,并让警告在同一进程内只出一次(`show_interactive_hint_once_` 用的 `hint_seen` 是按 id 持久化的,这里不合适 —— 误配是持续状态,不是一次性提示)。

建议:保持每次警告(它是当前状态的真实描述),但缺口 A 修好后这种状态的产生途径基本被堵死 —— 你上次遇到它,正是因为 `--theme` 收下了一个当时不存在的值。

### 3.3 内置主题应当是数据,不是代码

`src/core/xself/theme_resources.cppm` 把 `mono.json` 与 `high-contrast.json` 存成 C++ 原始字符串字面量,打包时由 `self init` 写到盘上,进 tarball,再由索引配方拷进 home,运行时读回来解析。**一份内容以源码形式存在,只为了在运行前变回文件。**

`default` 内置是对的 —— 它是"配置缺失时仍然有颜色"的保证,住在 `modules/theme` 的 `builtin_default()`,不在本节讨论范围。另外两份的定位是**配置格式的参考样例**,样例本就该是配置文件。

#### 3.3.1 现状:一个问题三个回答者

| 回答者 | 触发条件 | 内容来源 |
|---|---|---|
| `ensure_shipped_themes_`(`init.cpp:369`,经 `ensure_home_layout`) | 每次 `self init` / `self install` | **内嵌字符串** |
| `self install` 的目录拷贝(`install.cpp:753`) | **仅当 `config/` 不存在** —— 升级时跳过 | tarball 文件 |
| 索引配方 `__install_home_config()`(pkgindex#672) | 索引安装/升级 | payload 文件 |

三者可以给出不同答案:被配方升级过的 home,再被一个旧二进制执行一次 `self init`,`_version` 标记不匹配就会把**旧的内嵌副本写回,覆盖较新的文件**。这是本仓库反复出现的"一个问题多个回答者"。

#### 3.3.2 方案

```
源码      config/themes/{mono,high-contrast}.json      新增,真实文件
打包      release 脚本拷 config/ 进 tarball,并断言两份都在
交付      解包方拷贝(self install 与索引配方,各自都是纯文件拷贝)
运行      --theme list 枚举目录                        已实现
```

随之删除:`theme_resources.cppm`(119 行)、`ensure_shipped_themes_`、`kVersion` 标记机制。升级语义由"按文件名覆盖"承担,与 `config/shell` 的 profile 一致。

`self install` 的条件从"`config/` 不存在时整目录拷"改为"按文件覆盖 `config/themes/` 下的两个已知文件名",这样升级路径不再跳过,同时用户自己放进去的文件不受影响 —— 所有权是**按文件**的,不是按目录。

#### 3.3.3 代价

本地构建(`mcpp build`)的开发 home 不再有这两份样例,`--theme list` 只列 `default`。这是可接受的:样例随发行包交付,而开发者手边就有源码树。`--theme list` 的末行应当说明这一点。

#### 3.3.4 必须同时补的检查

`tools/linux_release.sh:173` 现在断言 `config/shell` 存在,**不断言 `config/themes`**。也就是说今天 tarball 带着主题这件事没有任何检查在看。改成数据文件后这条更关键 —— 少拷一个目录就是静默少交付。三个 release 脚本各加一条。

### 3.4 用户放置的主题已经可被发现(实测)

`--theme list` 枚举的是目录而非编译期表(`cli.cpp:665`,`platform::dir_entries` 过滤 `.json`)。实测在隔离 home 放入 `my-own.json`:

```
themes:
  default            (built in)
  my-own             @xlings/config/themes/my-own.json
  mono               @xlings/config/themes/mono.json
  high-contrast      @xlings/config/themes/high-contrast.json
```

且 `xlings config --theme my-own` 按裸名选中成功。此项无需改动。

两处可改进:列出顺序是文件系统顺序(建议改为 `default` 在前、其余按名排序,便于比对);末行"to customise"只提到了路径写法,未提"放进 `config/themes/` 后可用裸名",而后者更简短。

### 3.5 文档:`docs/theme.md`(新增)

面向用户,陈述句,不含网络用语。与既有的 `docs/spec/themes.md` 分工:后者是**规约**(给实现者,说明为什么这么设计),前者是**说明**(给使用者,说明怎么用)。

大纲:

1. **取值的三种形式** —— 内置名 `default`;随发行版附带的名字 `mono` / `high-contrast`;路径 `./my-theme.json`
2. **解析规则** —— 裸名只在 `$XLINGS_HOME/config/themes/` 下查找,不搜索工作目录(一个随 cwd 变化的配置值不可复现);路径相对于**声明它的那份配置**所在目录
3. **九个角色槽** —— `accent` `alt` `success` `warn` `error` `text` `muted` `border` `surface`,逐个说明用途,而不是逐个说明颜色
4. **覆盖语义** —— 未提及的槽继承内置默认,因此一份主题只需写它真正要改的部分;`dark` 与 `light` 各自独立回退,只写 `dark` 不会把值镜像到浅色终端
5. **失效时的行为** —— §3.1 那张表,以用户视角复述
6. **自定义步骤** —— 复制一份 → 改 → `xlings config --theme ./my.json`;并说明 `config/themes/` 归 xlings 所有,升级会覆盖,所以要改的是副本

---

## 4. 目录归属:`apps/` 与 `modules/`

### 4.1 现状与动机

```
apps/gui/main.cpp          GUI 二进制入口
ui/gui/                    GUI 前端库(mcpp 成员包 xlings:ui-gui)
libs/theme/                主题库(mcpp 成员包 xlings:theme)
src/libs/json|sha256|...   源码内的第三方与工具单元
```

两个问题:根目录 `libs/` 与 `src/libs/` 同名不同义;`apps/gui/main.cpp` 与 `ui/gui/` 是同一个产物的两半却分居两处。

### 4.2 方案

```
apps/gui/          main.cpp + src/ + mcpp.toml     ← ui/gui 并入
modules/theme/     src/ + mcpp.toml                ← libs/theme 改名
src/libs/          保持不动
```

`ui/` 目录随之消失。`src/ui/`(ftxui 的 CLI/TUI 渲染)不动 —— 它还在主二进制里,按 `2026-08-22-multi-frontend-architecture-design.md` 的记录,拆成成员包要等 `core/` 先拆(mcpp 工作区成员不能依赖根包)。

### 4.3 需要改的地方(全量)

实际引用只有 5 处,其余都在 `.agents/docs/` 的历史记录里:

| 文件 | 行 | 改动 |
|---|---|---|
| `mcpp.toml` | 5 | `members = ["modules/theme", "apps/gui"]` |
| `mcpp.toml` | 34 | `main = "apps/gui/main.cpp"` — 不变 |
| `mcpp.toml` | 42 | `ui-gui = { path = "apps/gui" }` |
| `mcpp.toml` | 70 | `theme = { path = "modules/theme" }` |
| `apps/gui/main.cpp` | 12 | 注释里的 `ui/gui` 改成 `apps/gui` |

`modules/theme/mcpp.toml` 的 `include_dirs = ["../../src/libs/json"]` 深度不变,无需修改。

包名 `xlings:ui-gui` 是否随目录改成 `xlings:gui`:**建议改**,并在 `mcpp.toml` 里同步。理由是包名与目录不一致时,`mcpp` 的报错信息会指向一个仓库里不存在的名字。代价是一次性的,现在只有一个消费者。

### 4.4 验证

目录移动的风险不在编译 —— 编译失败会立刻暴露。风险在**只在某个平台或某个 feature 下才被引用的路径**:`apps/gui` 只在 `--features gui` 时进入依赖图,而 CI 的常规矩阵不开这个 feature。

因此验证必须包含:

```
mcpp build                        # 默认,不含 gui
mcpp build --features gui         # 含 gui,三平台
mcpp test
```

第二条在改动前后各跑一次并比对产物清单;只跑第一条会绿着什么也没验。

---

## 4a. `platform` 与 `cancellation` 独立成 module

### 4a.1 可行性(实测)

| | 行数 | 非 `std` 依赖 | 被多少 TU import |
|---|---|---|---|
| `runtime/cancellation` | 115 | **无** | 19 |
| `platform`(`platform.cppm/cpp` + `platform/*`) | 3230 | **仅 `xlings.runtime.cancellation`** | **81 / 211(38%)** |

依赖图无环且只有一层:

```
modules/cancellation   叶子,只 import std
        ↑
modules/platform       3230 行
        ↑
modules/i18n           §4b
modules/theme          叶子,已存在
apps/gui               已存在
        ↑
xlings(根包)          依赖以上全部
```

`platform` 有 81 个 import 方,是本仓库依赖最广的模块;但它自己几乎不依赖任何东西,所以抽取的是**图中最容易摘下的那个节点** —— 广度在上游,深度为零。

### 4a.2 `[build]` 不被继承,必须逐项核对

mcpp 只把 `[toolchain]` 与 `[target.<triple>]` 自动继承给成员(`docs/06-workspace.md` §4),**`[build]` 不在其列**。根包的 `cxxflags` 是:

```toml
cxxflags = ["-DLIBARCHIVE_STATIC", "-DUNICODE", "-D_UNICODE"]
```

一个成员少了它需要的宏,不会报错,只会编出另一份语义 —— Windows 上 `-DUNICODE` 缺失会让通用名 Win32 API 解析到 ANSI 版本,非 ASCII 路径行为改变而三平台 CI 只有一个会红。

**实测结论:`platform` 一个都不需要。**

| flag | platform 是否依赖 | 依据 |
|---|---|---|
| `-DUNICODE` / `-D_UNICODE` | 否 | `platform/windows.cpp` 用的全是显式 `W` 后缀 API(10 处),`TCHAR`/`_T()`/`LPCTSTR` 出现 **0** 次 |
| `-DLIBARCHIVE_STATIC` | 否 | platform 全域不出现 `archive` |
| `include_dirs` | 不需要 | platform 的 `#include` 全是尖括号系统头,无第三方相对头 |

这条核对必须写进任务,不能靠"编过了"作结论:少一个宏照样编过。

### 4a.3 真正的风险是机械性的,不是架构性的

81 个 TU 跨越一条新的包边界。本仓库有两条相关记录:模块体的迁移会移动模板实例化点,失败信息指向的常常是**无关的 TU**;而 mcpp 的 BMI 缓存在跨包重组后容易失配。

因此验证必须包含:

```
rm -rf target/*/*/gcm.cache        # 不是冷构建,但足以排除 BMI 失配
mcpp build                          # 默认
mcpp build --features gui           # gui 成员也要重编
mcpp test
mcpp build --toolchain llvm@20.1.7  # 四工具链门,复现 macOS/Windows 的 clang 语义
```

以及三平台 CI 全绿 —— 这是唯一能覆盖 `platform/{windows,macos,unix,linux}.cpp` 全部四份实现的手段,本机只编得到其中一份。

### 4a.4 与 §4 目录方案合并后的最终布局

```
apps/gui/              GUI 二进制(main.cpp + src/)
modules/json/          叶子;内含 nlohmann 的 json.hpp 与其 LICENSE
modules/sha256/        叶子,仅 std
modules/cancellation/  叶子,仅 std
modules/platform/      → cancellation
modules/i18n/          → platform
modules/tinyhttps/     → mcpplibs.tinyhttps(包名 `xhttp`,见下)
modules/theme/         → json 的头目录
src/                   尚未拆出的部分:core / cli / ui / runtime
```

`src/libs/` 一并并入 `modules/`。原本的理由是「它们是随源码携带的第三方单元」,但**这个项目构建并导出的模块就是这个项目的模块**,无论里面的代码是谁写的;而 `libs/` 与 `src/libs/` 两个名字指同一件事,本身就是要消除的歧义。

抽取过程中被迫解决的两处真实耦合:

**其一,`tinyhttps` 依赖根包。** 它 `import xlings.core.log`,全模块只用了一次 `log::debug`(报告选中的代理)。成员不能依赖根包,而这一行就是唯一的阻碍 —— 改为把代理记在 `last_proxy()` 里,由关心的人用自己的口径去报告。一个会写日志的库,是一个对调用方输出去向有意见的库。

**其二,包名冲突。** 根包同时声明了 `mcpplibs.tinyhttps = "0.2.9"` 和 `tinyhttps = { path = ... }`,mcpp 在同一个命名空间里解析依赖名,于是直接报错「同一个名字既是版本依赖又是路径依赖」。成员改名为 `xhttp`,并把 `mcpplibs.tinyhttps` 从根包移到该成员 —— 它本来也只有这一个使用者。

## 4b. i18n:资源内置,模块独立

### 4b.1 现状(实测)

| 项 | 数量 |
|---|---|
| `Msg` 枚举条目 | 36 |
| 双语表里 zh 已填 | 36(抽样核对,非空) |
| **`tr()` 的产品调用点** | **0** |
| 用户可见输出点(`log::` 非 debug + `diag::emit` + `ui::print_` + `std::println`) | ≈ 593 |

**36 条中文译文全部存在,而没有任何代码读它们。** 这是本轮反复出现的同一形状第五次:抽象在仓库里,调用点绕过它。所以"zh 好像没有"是准确的观察 —— 不是译文缺失,是译文不可达。

### 4b.2 内置是对的,与主题相反 —— 因为二者性质不同

| | 主题 | i18n |
|---|---|---|
| 用户会不会自己写一份 | **会** —— 这正是 `mono`/`high-contrast` 存在的意义 | 不会 |
| 因此该以什么形式存在 | 配置文件(§3.3) | 编译进二进制 |
| 缺失时的后果 | 回退内置 `default`,可接受 | 无译文即无法输出,不可接受 |

判据是"用户是否会作为作者去编辑它"。主题是配置格式的样例,样例必须是文件;译文不是任何人要编辑的配置,内置省掉一整条交付链路(打包 → 拷贝 → 读盘 → 解析),也就省掉了这条链路上的每一种失效方式。

### 4b.3 `modules/i18n`:直接依赖 `modules/platform`

早先设想让 i18n 成为**叶子**(仿 `theme`),为此要删掉 `language()` 里那条惰性 `platform::get_system_language()` 回退,改由调用方注入。**在 `platform` 也独立成 module 之后,这一步不再必要。**

mcpp 文档明确:*"Members reference one another through `path` dependencies"*(`docs/06-workspace.md` §2.3)。成员之间可依赖,被禁止的只有成员依赖**根包** —— 这才是 `ui/cli`、`ui/tui` 至今出不去的原因。

于是 `modules/i18n` 保持现有形态,只是搬家:

```toml
# modules/i18n/mcpp.toml
[dependencies]
platform = { path = "../platform" }
```

`language()` 的惰性回退保留。它不构成"多个回答者":解析点只有 `language()` 一处,`set_language` 是覆盖而非第二个解析者。

### 4b.4 翻译的单位:用 `diag` 的 `code`,不要扩 `Msg` 枚举

593 个输出点不可能各配一个枚举常量 —— 枚举会变成一份千行的合并冲突源,而且它对 `std::format` 拼出的诊断摘要无能为力。

但本轮已经建成了一个现成的 ID 空间:**每个 `Diagnostic` 都带一个稳定、可搜索的 `code`,且 `validate()` 强制它非空**。当前 15 个:

```
cli.bad_interactive      theme.bad_color       xvm.no_active_version
cli.bad_ui_mode          theme.bad_json        xvm.not_in_subos
cli.needs_confirmation   theme.not_found       xvm.pinned_version_missing
ui.interactive_first_run theme.unknown_slot    xvm.unknown_target
ui.mode_degraded         theme.unreadable      xim.remove_absent
```

**`code` 就是 msgid。** zh 目录按 `code` 索引,提供 summary 模板与 fact/action 的标签;`std::format` 的参数不翻译(它们是版本号、路径、包名)。新增诊断时 `code` 本来就必须写,于是"加了诊断忘了加 msgid"这件事不会发生 —— 缺 zh 条目就回退英文,与缺失槽位回退默认主题同构。

### 4b.5 覆盖范围包含结构化输出的**标签**

诊断正文只是一半。`xlings config` 这类命令的输出里,左列全是标签,它们同样是面向用户的散文:

```
    XLINGS_HOME   /home/speak/.xlings          ← 环境变量名,专有名词,保持英文
    XLINGS_DATA   @xlings/data                 ← 同上
  ▸ active subos  default                      ← 「当前子系统」
    bin           @xlings/subos/default/bin    ← 「可执行目录」
    mirror        CN                           ← 「镜像」
    ui mode       tui                          ← 「界面模式」
    theme         high-contrast                ← 「主题」
    index-repo    xim : https://...            ← 「索引仓库」
```

判据:**专有名词保持英文,描述性词汇翻译。** 具体地——

| 保持英文 | 翻译 |
|---|---|
| 环境变量名(`XLINGS_HOME` 等) | 字段标签(`mirror` `theme` `ui mode` `index-repo` `active subos`) |
| 命令名与子命令名(`install` `subos use`) | `diag` 的 `Fact` / `Action` 标签(`installed`、`from`、`see what is here`) |
| 配置键名(JSON 里的 `theme`、`uiMode`) | 面板表头与状态词 |
| 版本号、路径、包名(即 `std::format` 的参数) | 诊断 summary |
| 通用技术词(`SubOS` 作为产品概念名) | — |

`SubOS` 一词两义:作为**产品概念名**时保持 `SubOS`(它是 xlings 造的词,译名会削弱可检索性);作为**普通名词出现在句子里**时用「子系统」。例如标签写「当前子系统」,而 `xlings subos --help` 的标题保持 `SubOS`。

这条把 §4b.4 的 msgid 空间从「诊断 code」扩到三类:

| 类别 | msgid 形式 | 数量 |
|---|---|---|
| 诊断 | `diag.code`(已有,稳定) | 15 |
| 字段标签 | 标签英文原文本身作键 | ≈ 40 |
| 面板/表头 | 同上 | ≈ 30 |

标签用原文作键而非新造 ID,因为它们短、唯一、且不含 `std::format` 参数;新造一层 ID 只会带来「加了标签忘了加 ID」。缺条目回退英文,与诊断一致。

### 4b.6 帮助文本中的固定描述同样覆盖,命令名不覆盖

```
  USAGE                          → 用法
    xlings [OPTIONS] [SUBCOMMAND]  ← 命令与占位符原样保留

  SUBCOMMANDS                    → 子命令
    install    Install packages    ← 左列 `install` 不翻;右列描述翻
    subos      Manage sub-OS ...   ← 同上

  OPTIONS                        → 选项
    -y, --yes  Skip confirmation prompts   ← 旗标不翻;描述翻
```

判据与 §4b.5 一致,在帮助文本里的具体落法:**能敲进终端的东西不翻,解释它的话翻。** 章节标题(`USAGE` / `SUBCOMMANDS` / `OPTIONS` / `ARGS`)、子命令描述、选项描述、示例后的说明文字都翻;命令名、子命令名、旗标名、占位符(`<version>`)、示例命令行原样保留。

`FOR AI AGENTS` 那段整体翻译 —— 它是给人读的说明,不是给 agent 解析的协议。

### 4b.7 形态:每语言一个 `.cppm`,en 是基底,其余是覆盖

与主题同构,而这不是巧合 —— 两者都是「一份内置的完整定义 + 若干只写差异的覆盖」:

| | 主题 | i18n |
|---|---|---|
| 基底 | `builtin_default()`,编译进二进制 | `en`,编译进二进制 |
| 覆盖 | `mono.json` / `high-contrast.json` | `zh.cppm`,以后 `ja.cppm` … |
| 未提及的条目 | 继承基底那一槽 | 回退 en 那一条 |
| 整份缺失 | 仍有颜色 | 仍有文字 |

目录:

```
modules/i18n/
    mcpp.toml
    src/
        i18n.cppm            接口:set_language / language / tr
        i18n.cpp             查表与回退
        i18n/
            en.cppm          基底,必须完整
            zh.cppm          只写与 en 不同的条目
```

一语言一文件,而不是一张双语大表:加一门语言是**新增一个文件**,而不是在几百行表里插一列 —— 后者每加一门语言都要改动所有行,review 与合并都会变成灾难。

`en.cppm` 必须完整,由单测钉住:**每个 msgid 在 en 下非空**。`zh.cppm` 不必完整,缺的自动回退,这让翻译能增量推进而不是全有全无。

现阶段只出 `en` + `zh`。

### 4b.8 覆盖顺序

| 阶段 | 范围 | 数量 | 理由 |
|---|---|---|---|
| 1 | 全部 `diag::emit` | 15 个 code | 本轮建成的用户面,结构化、有 ID、最高价值 |
| 2 | `ui::print_*` | 30 | 面板与表头,复用率高 |
| 3 | `log::error` / `log::warn` 的高频项 | 按实测频次取前 N | 逐条评估,不追求全量 |
| — | `log::debug`(106 处) | **不翻译** | 它的读者是开发者与 issue 报告,英文更利于检索 |

`Msg` 枚举的 36 条与阶段 1 合并后即可删除 —— 它们本就无人调用。

### 4b.9 `xlings config` 不显示 lang

`xself/config.cpp:26` 是 `if (!lang.empty()) addField("lang", lang)`,即**仅在显式设置过时显示**。而 `ui mode` 一行永远显示生效值。于是同一张表里混了两种语义:「配置了什么」与「实际是什么」,读者无法得知当前到底在用哪种语言。

修法:与 `ui mode` 一致,永远显示,并把两者都说出来:

```
    lang          auto (zh)
    lang          zh          ← 显式钉住时
```

同样的问题也在 `theme`(仅设置时显示)与 `tui.interactive`(仅设置时显示)上。`mirror` 不受影响,因为它有非空默认值。

## 5. 任务拆分与依赖关系

### 5.1 被裁决阻塞的任务

下列任务在对应决定作出前不能开工。其余任务不受影响,可并行。

| 决定 | 阻塞 | 若否决则 |
|---|---|---|
| D-a §2.4.4 两层门控 | T4 T5 T6 T7 | 只做 T3(修 S2 的静默成功),其余维持现状 |
| D-b §2.5 `PromptOutcome` variant | T4(强依赖) T5 T6 T7(弱依赖) | T4–T7 改用 `enum + string`,漏判风险留存 |
| D-c §3.3 主题改数据文件 | T9 T10 | 保留 `theme_resources.cppm`,仅做 T8 |
| D-d §1.3 `use <name>` 恒为查询 | T2 | 保留自动切换,仅改 `--help` 措辞使其与实现一致 |
| D-e §4b.4 以 `diag` 的 `code` 作 msgid(而非扩 `Msg` 枚举) | T13 T14 | 沿用 `Msg` 枚举,逐条新增,合并冲突面变大 |
| D-f §4a 抽取 `platform` + `cancellation` | T15 T16 | 只做 §4 的目录改名,`i18n` 留在 `src/core` |

### 5.2 任务表

仓库列:**X** = openxlings/xlings,**I** = openxlings/xim-pkgindex。

| # | 任务 | 仓库 | 依赖 | 验证方式 |
|---|---|---|---|---|
| T1 | §2.6 `remove` 不再复用 `not_in_subos`,合并到 `xim.remove_absent` | X | — | 新 e2e:删一个只存在于别处的包,断言动作里**没有** `xlings install` |
| T2 | §1.3 删除单候选自动切换 | X | D-d | E2E-51 更新 + 新断言:候选为 1 与为 2 产生同一结果类型 |
| T3 | §2.2 `select_package` 处理"没人可问",Refuse + exit 2 | X | — | 新 e2e:`install <2–5 项模糊名>` 无 `-y`,断言 exit≠0 且未安装 |
| T4 | §2.5 `prompt()` 返回 `std::variant`,删 `kCannotAsk` | X | D-b | `test_prompt_refusal.cpp` 改断言形式;编译期即为守卫 |
| T5 | §2.4.4 确认层门控改 `stdin_is_terminal` 且脱离 `tui.interactive` | X | D-a, T4 | 新 e2e:**关闭 stdin** 跑 `remove` 无 `-y`,断言走策略分支而非阻塞 |
| T6 | Y1–Y3 从 `ask_yes_no` 迁到 EventStream,删 `ask_yes_no` | X | D-a, T4 | `xlings interface` 下断言 `self install` 的提问可见(此前完全不可见) |
| T7 | 选择层纳入 L1 L2 L3(`subos use` / `--theme` / shim) | X | D-a, T4 | 各自 e2e:非交互时回落面板完整且 exit 0 |
| T8 | §3.2 缺口 A `--theme` 落盘前校验 | X | — | e2e:`config --theme <不存在>` 断言 exit 2 且**未写入配置** |
| T9 | §3.3 主题改数据文件,删 `theme_resources.cppm` | X | D-c | 三个 release 脚本各加 `config/themes` 断言(§3.3.4) |
| T10 | 配方 `__install_home_config` 与 T9 的文件来源对齐 | **I** | T9 | 隔离 home 真实安装,前后对比 `config/themes/` |
| T11 | §3.5 `docs/theme.md` | X | T8 T9 | 文档,无自动验证 |
| T12 | §4 目录移动 `ui/gui`→`apps/gui`,`libs/`→`modules/` | X | — | `mcpp build` **与** `mcpp build --features gui` 前后各跑,比对产物清单 |
| T13 | §4b.4 zh 目录按 `diag.code` 索引,15 个 code 全覆盖 | X | D-e | 单测:每个 code 在 zh 下非空;e2e:`--lang zh` 跑出中文诊断 |
| T14 | §4b.9 `xlings config` 恒显示 lang/theme/interactive 的**生效值** | X | — | e2e:未设 lang 时断言输出含 `auto (` |
| T17 | §4b.5 字段标签与面板表头纳入 i18n(约 70 条),专有名词保持英文 | X | D-e, T13 | e2e:`--lang zh` 下 `xlings config` 断言标签为中文且 `XLINGS_HOME` 仍为英文 |
| T18 | §4b.6 帮助文本的章节标题与描述纳入 i18n;命令名/旗标名保持英文 | X | D-e, T13 | e2e:`--lang zh` 下 `xlings -h` 断言出现「用法」且 `install` 仍为英文 |
| T19 | §4b.7 `modules/i18n/src/i18n/{en,zh}.cppm` 一语言一文件 | X | D-e, D-f | 单测:每个 msgid 在 en 下非空;zh 缺条目回退 en |
| T20 | I7:`config --lang/--mirror/--theme/--ui-mode` 不带值时进入选择 | X | D-a, T4 | e2e:非交互时断言列出取值并 exit 2,而非静默 |
| T15 | §4a 抽 `modules/cancellation` | X | D-f | 编译 + `mcpp test` |
| T16 | §4a 抽 `modules/platform`;§4b.3 `modules/i18n` 依赖它 | X | D-f, T15, T12 | §4a.3 的五条构建 + 三平台 CI;`[build]` flag 逐项核对表 |

### 5.3 并行分组

```
组 A(无阻塞,可立即并行)     T1  T3  T8  T12
组 B(D-b 决定后)             T4  →  T5  T6  T7      三者互相独立,可并行
组 C(D-d 决定后)             T2
组 D(D-c 决定后)             T9  →  T10(跨仓库,必须在 T9 发布后)
组 E(D-e 决定后)             T19  →  T13  →  T17  T18
组 H(D-a 决定后)             T20
组 F(D-f 决定后)             T15  →  T16        T16 依赖 T12 先落地目录
组 G(收尾)                   T11  T14
```

T10 是唯一的跨仓库项。它**必须在 T9 所在的 xlings 版本发布之后**才能合并 —— 配方引用的是 payload 里的文件,payload 不存在时配方是空操作,而空操作与成功在 CI 里看起来一样(pkgindex#672 就是这么被发现的)。

### 5.4 各维度的对应关系

原目标要求从若干维度检视,逐条对应到上面的任务:

| 维度 | 由哪些任务承担 |
|---|---|
| 架构 | T4(契约进类型)、T9(数据与代码分离)、T12(目录归属) |
| 稳定性 | T1 T3 T5 —— 三者都是"报告成功而什么也没做"的实例 |
| 优雅简洁 | T9 删 119 行内嵌 + `kVersion` 机制;T6 删第三套问法;T4 删哨兵值 |
| 用户体验 | T2 T5 T6 T7 —— 恢复 `-y` 的文档语义,凡需选择皆可交互 |
| 兼容性 | T2 唯一的行为变化(单候选不再写状态);T5 使 `install` 恢复询问,脚本需带 `-y`,而这本就是文档要求 |
| 跨平台 | T5 的 `stdin_is_terminal` 三平台实现;T12 的 `--features gui` 三平台构建 |
| 一致性 | T1(两个回答者合一)、T6(三套问法合一)、T9(三个交付者合一) |
| 无感升级 | T9/T10 的顺序约束;T8 堵住产生失效配置的入口 |

### 5.5 发布切分

| 版本 | 内容 | 理由 |
|---|---|---|
| 2026.8.22.3 | T1 T3 T8 | 三者皆为"报告成功而什么也没做",与 2026.8.22.1/.2 同族,无需裁决即可开工 |
| 次版本 | T2 T4 T5 T6 T7 | 交互契约整体变更,应当一次到位而非分批,否则 `-y` 的语义在中间态更混乱 |
| 再次版本 | T9 T10 T11 T12 T14 | T10 跨仓库且必须后置;T12 T14 无行为变化 |
| 再次版本 | T19 T13 T17 T18 T20 T15 T16 | T16 触及 81 个 TU 的包边界,应独占一个版本以便定位回归 |

单 PR 与否:2026.8.22.3 那批适合单 PR。交互契约那批涉及 `prompt()` 签名变更并波及全部调用点,同样适合单 PR —— 拆开会出现"一半调用点用新类型"的中间态。

---

## 6. 当前状态

设计待 review。六项裁决(D-a…D-f)未定,组 B–F 未开工。

组 A 的三项 **T1 T3 T8** 不依赖任何裁决 —— 三者都是已发布代码中的缺陷,修法在本仓库已有先例,不引入新抽象:T1 是 #556 引入的回归(`remove` 的动作是请求的反面),T3 是 `install <模糊名>` 报成功而未安装,T8 是 `--theme` 不校验而其两个兄弟都校验。批准后可立即并行开始,合为 2026.8.22.3 一个 PR。
