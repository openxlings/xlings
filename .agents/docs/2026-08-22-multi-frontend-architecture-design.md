# xlings 多前端架构与体验设计(i18n / cli / tui / gui)

> 日期: 2026-08-22
> 类型: 设计 (design)
> 范围: `src/runtime/{event,event_stream}.cppm`、`src/ui/*`、`src/agent/text_renderer.*`、`src/interface.*`、`src/capabilities.cpp`、`src/cli.cpp` 的事件消费者、`src/core/log.*`、`src/core/palette.cppm`、`src/core/i18n.*`、`src/platform.cpp` 的语言探测、`mcpp.toml`
> 方法: 源码统计(HEAD `2f12f45`)+ 对 `2026.8.17.2` 实测 + musl/glibc locale 对照实验
> 姊妹文档: [`2026-08-22-cli-diagnostics-experience-design.md`](2026-08-22-cli-diagnostics-experience-design.md) —— 那篇解决"一条消息长什么样",本篇解决"谁来决定它长什么样"

---

## 已定的方向(2026-08-22)

1. **`ui/` 下三个独立 mcpp 模块:`cli` / `tui` / `gui`。先都同进程**,以后要拆进程比较简单。
2. **不做全屏 TUI。** `tui` 只做行内渲染 + 行内交互部件,全屏形态整个从设计里去掉。
3. **i18n 默认跟随系统语言(`auto`),配置可覆盖;测试一律配 `en`。**
4. **`--agent` 只是一个"尽量不要阻塞"的标识**,不是一种渲染。`ui/cli` 的原始文本本身就对 agent 友好 —— **一个渲染器,不分家**;`--agent` 下**打印 msgid**。
5. **GUI 是独立的二进制 `xlings-gui`。** 它独立之后,**`--ui-mode gui` 就不需要了** —— 想要 GUI 就运行 `xlings-gui`,没有第二条入口。
6. **`--ui-mode` 只分 `cli` / `tui`。** 配色、是否交互等做成**主题 / 配置驱动**,不是新的模式。
7. **主题先只做配色,独立成叶子模块**;`default` 在代码里,主题文件只写要覆盖的槽,`.xlings.json` 里存路径引用,自带的放 `config/themes/`。
8. **`tui.interactive` 默认 `true`,首次提示只提示一次。**
9. **`Document::id` 表放 `core/view.cppm` 的编译期表。**
10. **自带主题三个**:`default`(编译进去)+ `mono` + `high-contrast`。
11. **删掉 `config/i18n/{en,zh}.json`**(0.0.4 遗留)。
12. **取舍标准:架构 / 未来扩展性 / 多平台通用 / 稳定性。**

**两处已实测、会让功能变成摆设的现状**(都不是推断):

- 第 3 条:系统语言探测**永远只能返回 `en`**,且失败无声(§3.4)。
- 第 7 条:配色的语义层已经存在,但**调用点 119:7 绕过它** —— 94% 的取色在点名"什么颜色"而不是"什么角色"(§3.8.1)。不先做那次改名,主题文件改不动任何东西。

> **更正**:本文初稿写过"Linux 用户拿不到 GUI",**这是错的**。静态 musl 无法 `dlopen` 是真的(实测),但它约束的是 **`xlings` 这个引导二进制**,不是 Linux 平台。`xlings-gui` 独立之后不再受这条约束 —— 它就是一个普通的动态链接程序,依赖 xlings 生态已经闭环的图形栈。索引里 `pkgs/g/godot.lua:138` 就是现成模板。详见 §3.6.2。

---

## TL;DR

**好消息:模块边界是干净的。** `import xlings.ui` 全树只有一处——`src/cli.cpp:19`。core 从不认识 ftxui。多前端在**编译依赖**上没有障碍。

**坏消息:协议不干净。** core 通过**五条**通道向外说话,只有一条是结构化的(`DataEvent`),而它:

- 承载的是**渲染结果**而不是数据 —— `addField(fields, "runtime deps", join_deps(...))`:英文标签 + 已经拼成一行的值,共 19 个标签、36 个 addField 点。前端拿到 `{"label":"runtime deps","value":"xim:glibc@>=2.39 xim:binutils@2.42"}`,既不能翻译,也不能重新布局,更不能知道那是一个列表。
- **26 个 kind 发出,19 个有消费者,9 个是孤儿,2 个渲染器是死的**。`xlings subos new foo --from bar` 的整条成功路径只发一个 `subos_forked`,没有任何终端渲染器认识它 —— 加了 `--from` 这一个 flag,同一个命令就从"打印创建面板"变成"一声不吭"。
- 把**光标记账**塞进了数据层:`download_progress` 的 payload 里有 `prevLines`,而且发出方 `commands.cpp:552` 返回的是 `state.size() + 2` 这个**猜测值** —— 真正渲染器算出的行数在 `cli.cpp:262` 被丢弃了。

**代价量化**:今天加一个前端,要重新实现 26 个命令专用渲染函数,接住 **437 个 `log::` 调用点**(361 个 info/warn/error + 76 个无标签 `println`,它们完全绕过事件流),并且自己猜那 9 个没人渲染的 kind。这不是接线,是重写。

**结论**:`--ui-mode` 不是一个 flag,是一个**契约**。先把契约立起来(core 只发语义文档,前端负责渲染 + 翻译 + 交互),三个 UI 模块才可能是三个 ~300 行的适配器,而不是三份 xlings。

**而 `--ui-mode` 这个 flag 本身几乎不用设计** —— 它和 `--mirror` / `--lang` 是同一件事,`xlings config --mirror` 那条链路(CLI 选项 → 锁下写 `~/.xlings.json` → 全局/项目读取 → `xlings config` 回显 → schema 文档 → e2e 断言)已经修好并有测试覆盖,照抄即可。**i18n 也一样**:`xlings config --lang` 整条链路都在,连 `auto` 回退都写好了,缺的只有两行接线(`set_language(Config::lang())` + 调用点用 `tr()`)。所以这件事的成本在协议,不在配置。

---

## 一、现状测量

### 1.1 五条并行输出通道

| 通道 | 调用点 | CLI(Rich) | `--agent` | `interface`(NDJSON) | GUI 能接吗 |
|---|---|---|---|---|---|
| `log::{info,warn,error}` | 361 | ✅ 直写 stdout/stderr | ✅ **完全相同** | ❌ 被 `is_tui_mode` 整体静音 | ❌ 没有 stdout |
| `log::{print,println}` | 76 | ✅ 无标签裸输出 | ✅ | ❌ 同上 | ❌ |
| `EventStream` / `DataEvent` | 36 emit(26 kind) | 19 kind | 18 kind | 全部透传 | 需重写 26 个渲染器 |
| `EventStream` / Error·Log·Prompt·Progress | 65 · 5 · 0 · 1 | ✅ | ✅ | ✅ | ✅ |
| 索引侧 `\r…\e[K`(**不在 `src/` 内**) | — | 泄漏进管道 | 泄漏 | 泄漏 | 不可控 |

(另有 109 个 `log::debug`,默认不可见,不计入。)

读法:**结构化通道只承担了约 19% 的用户可见输出**(101 个事件 emit vs 437 个 `log::`/`println` 调用点)。`interface` 模式之所以"能用",是因为它把 log 整体静音——它不是渲染了这些内容,是**放弃**了这些内容。

`PromptEvent` emit = 0、`CompletedEvent` emit = 0:两个协议里定义了的事件,没有任何地方发出。`prompt` 走的是 `EventStream::prompt()`(4 个调用点,全在 `xim/commands.cpp`),它内部 emit;`CompletedEvent` 则是**纯死变体** —— `interface.cpp:50` 专门处理它并丢弃,而没人发。

### 1.2 mode 有八个旋钮,零个所有者

| 旋钮 | 位置 | 影响 |
|---|---|---|
| `stdout_is_terminal()` / `stderr_is_terminal()` | `palette` | 颜色、宽度钳制、`\e[K` |
| `NO_COLOR` / `TERM=dumb` | `palette::opted_out_` | 只关颜色,不关光标控制(有意的,注释里论证过) |
| `XLINGS_THEME=dark\|light\|auto` | `palette` | 调色板 + OSC-11 探测 |
| `XLINGS_TERM_WIDTH` | `ui::layout::term_width` | 宽度钳制;`0` = 不限 |
| `--agent` | `cli.cpp:770` | 换 EventStream 消费者 + `enable_color(false)` + `layout::set_plain(true)` |
| `platform::set_tui_mode(true)` | 只有 `interface.cpp:62` | **整体静音 `log::`** |
| `-v` / `-q` | `log::set_level` | 只影响 debug/info/warn;`error()` 里**没有级别判断**,所以 `-q` 对失败输出是空操作 |
| `Config::lang()` | 配置读到了,5 个调用点全是**回显** | 无 |

八个旋钮,没有一个地方能回答"当前是什么模式"。`--agent` 和 `interface` 各自拼装自己那一套,新增第三种就要拼第三套。

### 1.3 `is_tui_mode()` 名字是反的

全树唯一的 setter 是 `interface.cpp:62`。它的语义是"**NDJSON 模式,别往终端写任何东西**",和"TUI"正好相反 —— 真正的 TUI 恰恰是最需要写终端的模式。`log.cppm:22` 的注释("Terminal output is suppressed when platform::is_tui_mode() is true")老实说出了实情。

这个名字如果不改,`--ui-mode tui` 会和它直接冲突。

### 1.4 协议完整性:没有任何保证

```
emit 26 kinds:  download_progress env help index_versions info_panel install_plan
                install_summary remove_blocked remove_plan remove_summary repo_list
                styled_list subos_already_in subos_candidates subos_created
                subos_entering subos_forked subos_list subos_nesting subos_removed
                subos_shims subos_switched system_info tip update_plan update_summary

CLI  消费 19    agent 消费 18    interface 泛型透传全部
```

**9 个孤儿**(emit 了,没有任何终端渲染器):

| kind | 发出点 | CLI 路径? | 后果 |
|---|---|---|---|
| `subos_forked` | `subos.cpp:897` | **是** | `subos new --from` 成功后**零输出**(见下) |
| `remove_blocked` | `xim/commands.cpp:1115` | **是** | 有 `log::println` 兜底,DataEvent 纯浪费 |
| `update_plan` | `xim/commands.cpp:1687` | **是** | 同上 |
| `update_summary` | `xim/commands.cpp:1730` | **是** | 同上 |
| `system_info` / `index_versions` / `subos_shims` / `repo_list` / `env` | `capabilities.cpp` | 否 | 只走 capability 路径,NDJSON 泛型接住 |

**2 个死渲染器**:`search_results`、`table` —— CLI 和 agent 都实现了,没有任何地方 emit。

`subos_forked` 值得单独看,因为它把问题压缩成了两行代码(`subos.cpp:1527-1530`):

```cpp
if (!fromSpec.empty()) {
    return new_from(name, {}, storage, imageSize, fromSpec, runtime, stream);
    //   → emit DataEvent{"subos_forked"}   ← 零消费者
}
return create(name, {}, storage, imageSize, runtime, stream);
//   → emit DataEvent{"subos_created"}     ← cli.cpp 有渲染器
```

同一个命令,差一个 `--from`,一条分支打印创建面板,另一条一声不吭。`new_from` 的整条成功路径没有任何 `log::` 兜底(只有两处失败分支的 warn),所以这不是"输出少了",是"输出没了"。

> 证据级别:代码级确定(单一 emit 点、零消费者、成功路径无其它输出),未做运行时复现 —— 复现需要在真实 home 里 fork 一个 subos。



兜底逻辑是 `cli.cpp:263`:

```cpp
else { log::debug("unhandled DataEvent kind: {}", e.kind); }
```

**debug 级**。也就是说协议漏了一个屏幕,默认日志级别下**一个字都不会说**。这正是 xlings 反复出现的那个形态:没发生和成功了,输出完全一样。

### 1.5 协议里装的是渲染结果,不是数据

`xlings info gcc` 的面板由 36 个 `addField()` 拼出,19 个不同的英文标签:

```
active | aliases | available | bindings | build deps | categories | deps | description
homepage | installed | licenses | repair | repo | runtime deps | selected installed
selected version | shim | subos | xpkg path
```

值也已经渲染完了:`join_deps(rtIt->second)` 把依赖列表拼成一行;`Config::display_path()` 把路径缩写成 `@xlings/...`;版本列表拼成 `"15.1.0, 15.1.0-musl, 16.1.0 (active)"` —— **"哪个是活跃的"这个语义被编码进了字符串里的括号**。

后果,按前端逐个说:

- **i18n**:前端拿到 `"runtime deps"` 这个字符串,没有 id,没法查表。要翻译只能在 core 翻,而 core 不知道前端是谁。
- **GUI**:拿到 `"15.1.0, 15.1.0-musl, 16.1.0 (active)"`,想做成一个可点击的版本列表,只能**反向解析自己人拼的字符串**。
- **交互式 TUI**:同上。想让"active"那一项高亮/可切换,得先把括号解出来。
- **NDJSON 消费者**(已经存在的 agent/MCP):同上。

`info_panel` 的 `highlight` 布尔是唯一逃出来的语义,而它表达的是"高亮"(表现)而不是"这是活跃版本"(语义)。

### 1.6 `prevLines`:光标记账穿过了数据层,而且是假的

```cpp
// core/xim/commands.cpp:530-552
DownloadProgressRenderer dlRenderer = [&stream](…, int prevLines) -> int {
    …
    payload["prevLines"] = prevLines;            // ← 终端光标状态进了 JSON
    stream.emit(DataEvent{"download_progress", payload.dump()});
    return static_cast<int>(state.size()) + 2;   // ← 猜的
};
```

而真正的渲染器 `ui::render_download_progress()` **返回了真实行数**(`progress.cpp:381`,`count(body,'\n')`),这个返回值在 `cli.cpp:262` 被**丢弃**。

于是光标上移量来自一个硬编码猜测。它在"每行都放得下"的常见情况下碰巧等于真值——`progress.cpp:383` 的注释正是这么论证的——但在窄终端 stacked 模式、`--agent` 纯文本、GUI 里都不成立。2026-07-29 调研 P1-5 声称修掉的"窄窗口安装画面糊",在这条返回值链断掉的地方还留着一半。

对多前端的意义更直接:**这个字段在 GUI 里毫无意义**,它证明当前"数据协议"没有把表现层的东西挡在外面。

### 1.7 交互能力:写好了,零调用者

| 组件 | 位置 | 状态 | 与"不做全屏"的关系 |
|---|---|---|---|
| `select_version()` | `ui/selector.cpp:36`,`ScreenInteractive::TerminalOutput()` + `Loop()` | **零调用者** | ✅ 行内,**正是要的形态** |
| `select_option()` | `ui/selector.cpp:103`,`ScreenInteractive::Fullscreen()` + `Loop()` | **零调用者** | ❌ 全屏,要改成 `TerminalOutput()` 或删 |
| `read_line()` | `ui/selector.cpp` | 零调用者 | ✅ |
| `confirm()` | `ui/selector.cpp` | 唯一在用的交互(`cli.cpp:274`) | ✅ |

2026-07-29 的 P1-18 已经记过一次"交互选择器是死代码",一年后仍然是。

也就是说:**xlings 今天没有交互式 TUI**。它是一个行式 CLI + 装饰性 ftxui 渲染 + 一个阻塞式 y/n。所谓"TUI 模式"目前只存在于 `is_tui_mode()` 这个名字里,而那个名字指的是别的东西。

好消息是:按"不做全屏"的决定,四个组件里**三个已经是对的形态**,要动的只有 `select_option()` 一处。

### 1.8 能力覆盖:GUI 能驱动多少?

`capabilities.cpp` 注册了 **20 个能力**;CLI 有 **15 个顶层子命令**,其中 `self` 自己还有 8 个动作。

能力**没有**覆盖的:`self install/update/doctor/clean/uninstall/init/migrate/config`(8 个)、`script`、`profile`、`index use/add/remove`。

一个纯靠 capability + NDJSON 驱动的 GUI,今天**做不了 `self doctor`**,而那恰恰是最需要图形化呈现的东西(一屏几十条 finding + 逐条 remedy)。

---

## 二、各个前端各自需要什么

按已定的模块划分列。注意 `--ui-mode` 只有 `cli` / `tui` 两个值(已定方向 6):`tui` 的交互是**配置**,`gui` 是**另一个二进制**,`ndjson` 是**另一个入口点**(`interface` 子命令,在 `src/interface.cpp`,不属于任何 UI 模块)。

| | `cli`(管道/CI/agent) | `tui` | `tui` + interactive | `xlings-gui` | `interface`(已有) |
|---|---|---|---|---|---|
| 模块 | `ui/cli` | `ui/tui` | `ui/tui` | `ui/gui` | `src/interface.cpp` |
| UI 依赖 | **无** | ftxui | ftxui | imgui+glfw | 无 |
| 输出时序 | 追加流 | 追加流 + 原地重绘 | 同 + 行内弹出部件 | 帧 | 追加流 |
| 布局宽度 | 无上限 | 终端宽 | 终端宽 | 自适应窗口 | 无 |
| 颜色 | 无 | SGR | SGR | RGBA | 无 |
| 交互 | 无(须 `-y`,否则报错) | 阻塞 y/n | 键盘导航 | 鼠标+键盘 | 通过 stdin |
| 长任务 | 逐行日志 | 原地进度条 | 可取消 | 可取消+可后台 | ProgressEvent |
| i18n | 需要 | 需要 | 需要 | 需要 | **不需要**(机器读) |
| 生命周期 | 一次性 | 一次性 | 一次性 | **常驻** | 一次性 |

三个关键差异,决定了架构:

1. **`ndjson` 不需要翻译,其余四个都需要。** 所以 `tr()` 的**调用**必须在前端 —— 否则 NDJSON 会拿到中文,agent 解析就坏了。(注意:调用在前端,**表仍留在 core**,三个 UI 模块共用一份;见 §3.4。)
2. **`gui` 是常驻进程,其余都是一次性进程。** 常驻意味着:`Config` 单例要能重载、`log::set_file` 要能多次切换、`console::output_mutex` 这种全局要么无害要么被隔离。命令本身是 `int cmd_x(args, EventStream&)` 的同步函数,放在 worker 线程上没问题。
3. **只有 `tui` 需要光标记账。** 所以 `prevLines` 这类字段必须从协议里拿掉,变成 `ui/tui` 的内部状态 —— 它对 `ui/cli` 无意义,对 `ui/gui` 更无意义。

---

## 三、架构提案

### 3.1 一句话:core 发文档,前端做渲染、翻译和交互

```
core (无 UI 依赖)                前端 (每个 ~300 行)
┌──────────────────┐            ┌──────────────────────────────────────┐
│ 命令逻辑          │  View      │ ui/cli  → 纯文本(人 + agent 共用)  │
│                  │ ─────────► │ ui/tui  → ftxui 行内渲染             │
│ 只发语义:         │  Diagnostic│         + 行内交互部件(可配置)     │
│  Document{...}   │ ─────────► │ ui/gui  → imgui(独立 bin)          │
│  Diagnostic{...} │  Progress  │ ndjson  → 泛型序列化(已有,不翻译)  │
│  Request{...}    │ ─────────► │                                      │
└──────────────────┘  Request   └──────────────────────────────────────┘
        ▲                                    │
        └────────── Response ────────────────┘
```

注意没有"全屏"这一档(已定方向 2),也没有单独的 agent 渲染器(已定方向 4)。

三种、且**只有**三种东西过河:

| | 内容 | 已有对应物 |
|---|---|---|
| `View` | 语义化文档(见 3.2) | `DataEvent`(需重构) |
| `Diagnostic` | 姊妹文档 §4 的 `Diagnostic` | `ErrorEvent` + 437 个 `log::` |
| `Request` / `Response` | 需要用户回答的事 | `PromptEvent` + `EventStream::prompt` |

进度是 `View` 的一种(可替换文档),不是第四种东西。

### 3.2 `View`:语义节点,不是渲染结果

关键判据:**一个字段能不能翻译、能不能重排、能不能点击** —— 能,说明它是数据;不能,说明 core 已经把它渲染掉了。

```cpp
export namespace xlings::view {

// 值是有类型的,前端才能决定怎么显示
using Value = std::variant<
    Text,          // { msgid, args }   ← i18n 的载体
    Literal,       // 不翻译:版本号、包名、路径
    Path,          // { absolute, display }  前端决定缩不缩写
    VersionList,   // { versions[], activeIndex }  ← 取代 "a, b (active)"
    Ref,           // { kind: package|subos|version, id }  GUI 可点击
    Bytes, Duration, Bool
>;

struct Field { std::string msgid; Value value; Emphasis emph; };

struct Document {
    std::string     id;        // "package_info" / "subos_list" —— 稳定,可枚举
    Text            title;
    std::vector<Field>              fields;
    std::vector<std::vector<Value>> rows;    // 表格类
    std::vector<Action>             actions; // 下一步(和 Diagnostic 同构)
};

}
```

三条硬约束,由测试守住:

1. **`Document::id` 必须在一张编译期表里**,每个 id 至少有一个渲染器 —— 直接消灭 §1.4 的 9 个孤儿和 2 个死渲染器。
2. **`Value` 里不允许出现已拼接的列表或已缩写的路径**(grep 测试:`view/` 之外不得调用 `join_deps` / `display_path` 去构造 `Value`)。
3. **不允许任何表现层字段**(`prevLines`、`highlight`、`nameWidth` 一律不进 `Document`)。

`highlight` 换成 `Emphasis{Active, Degraded, Selected}`——说的是**为什么**要强调,前端自己决定用颜色、`▸`、粗体还是别的。

### 3.3 三个前端模块:`ui/cli` / `ui/tui` / `ui/gui`

**已定的方向**:`ui/` 下三个**独立 mcpp 模块**,先都同进程,以后要拆进程也容易。

这个切法比按"模式"切更好,因为它和**依赖图**正好重合:

| 模块 | 依赖 | 负责的模式 | 性质 |
|---|---|---|---|
| `ui/cli` | **无 UI 依赖**(不含 ftxui) | `cli`(管道 / CI / `--agent`) | **地板**,永远编译进去 |
| `ui/tui` | `ftxui` | `tui`(交互与否由配置决定) | 默认(tty 时) |
| `ui/gui` | `mcpplibs:imgui` + glfw/GL | `gui` | 只链进**独立的 `xlings-gui` 二进制**(§3.6) |
| `theme` | **零依赖**(叶子) | 三者共用 + 迁移期的 `core/log` | 配色槽 + 主题文件(§3.8) |

**`--agent` 不占一列** —— 按已定方向 4,它不是一种渲染,而是 `ui/cli` + `interactive = false`。这条决定本身就消灭了 §1.4 那个 19-vs-18 的分裂:今天 `cli.cpp` 和 `agent/text_renderer.cpp` 是**两个**渲染器实现同一批 kind,合并成 `ui/cli` 一个之后,它们不可能再各自漏掉不同的 kind。这是"靠构造消除",不是"靠测试发现"。

三条由此白拿的性质:

1. **`plain` 模式没有任何图形依赖。** CI、管道、agent 走的那条路,不可能被 ftxui 的 bug 影响到。今天不是这样——`--agent` 仍然经过 ftxui 渲染器再脱色。
2. **最小构建成立。** `mcpp build --no-default-features` 得到一个只有 `cli` 前端的小 xlings,适合容器/嵌入场景。
3. **GUI 完全不碰 CLI 的交叉编译面。** `ui/gui` 只链进 `xlings-gui` 那个二进制;`[feature-deps.gui]` 下的 imgui/glfw 只在 `--features gui` 时才被解析下载(mcpp 2026.8.6.2+ 的语义:**声明在哪里就决定了它是否可选**)。`xlings` 本身的依赖图一个字都不变。

`ndjson` 不属于任何 UI 模块 —— 它在 `src/interface.cpp`,是泛型序列化,本来就不需要渲染器,也是唯一**不翻译**的前端。

**这个切法唯一的真风险**:三个模块会重演今天 `cli.cpp`(19 kind)vs `agent/text_renderer.cpp`(18 kind)的分裂,只不过从两份变成三份。所以 §3.2 的 `Document::id` 枚举表**不是可选项**,它是这个切法能成立的前提:

```
测试矩阵 = Document::id 全集 × {cli, tui, gui}
缺一格 → 编译期或单测失败,不是 log::debug 里的一行
```

#### 模块布局(mcpp workspace)

mcpp 的 `[workspace]` + `path` 依赖 + `[features]` / `[feature-deps]` 正好够用,不需要任何新机制:

```toml
# 根 mcpp.toml —— root-package workspace(根本身仍是 bin)
[workspace]
members = ["ui/cli", "ui/tui", "ui/gui"]

[workspace.dependencies]
ftxui = "6.1.9"

[package]
name = "xlings"

# ── 两个 bin target ──────────────────────────────────
[targets.xlings]
kind = "bin"
main = "src/main.cpp"

[targets.xlings-gui]
kind = "bin"
main = "apps/gui/main.cpp"
required_features = ["gui"]   # 特性没开 → 静默跳过(mcpp 定义的语义)

[features]
default = ["tui"]             # gui 不在 default:musl 静态发布构建不该编它
tui = []
gui = []

[dependencies]
xlings-ui-cli = { path = "ui/cli" }      # 地板,无条件

[feature-deps.tui]
xlings-ui-tui = { path = "ui/tui" }

[feature-deps.gui]
xlings-ui-gui = { path = "ui/gui" }
mcpplibs.imgui = "…"          # 索引里已有:C++23 module 包,`import imgui.core;`
```

```toml
# ui/tui/mcpp.toml
[package]
namespace = "xlings"
name      = "ui-tui"

[targets.ui-tui]
kind = "lib"

[dependencies]
ftxui.workspace = true                   # 版本从 workspace 继承
xlings-ui-cli = { path = "../cli" }      # 复用纯文本渲染做降级兜底
```

```toml
# ui/gui/mcpp.toml
[package]
namespace = "xlings"
name      = "ui-gui"

[targets.ui-gui]
kind = "lib"

[dependencies]
mcpplibs.imgui = "…"
xlings-ui-cli = { path = "../cli" }
```

模块名:`xlings.ui.cli` / `xlings.ui.tui` / `xlings.ui.gui`,各自导出一个 `Frontend` 实现。`apps/gui/main.cpp` 只是入口:开窗、起 worker、把 `xlings.ui.gui` 的 `Frontend` 挂上 `EventStream`。

> **为什么 `gui` 不在 `default` 里** —— 它进的不是 `xlings` 这个二进制,而是 `xlings-gui` 那个。静态 musl 的 `dlopen` 是 stub(§3.6.1 实测),所以 `xlings` 这个引导二进制不可能带 GUI;`required_features` 让 `xlings-gui` 在没开特性时是**静默跳过**而不是构建失败。
> 这不影响 Linux 用户拿到 GUI —— `xlings-gui` 是独立的动态链接程序,依赖 xlings 生态已经闭环的图形栈(§3.6.2),分发走索引包(§3.6.4)。

现有 `src/ui/` 的 7 个 partition 按依赖重新归位:

| 现在 | 去处 | 理由 |
|---|---|---|
| `:layout` | **`core`** | 宽度契约不依赖 ftxui(`display_width` 除外),而 core 侧的诊断急需它 —— 姊妹文档 D4 的根因就是它够不着 core |
| `:theme` | `ui/tui` | ftxui 适配层 |
| `:info_panel` `:table` `:banner` `:progress` | `ui/tui` | 全是 ftxui 渲染 |
| `:selector` | `ui/tui` | 局部交互(见 3.3.1) |
| `agent/text_renderer` | **`ui/cli`** | 它已经是纯文本渲染器,而且 2026-07-29 调研测出它比 TUI **更正确**(P1-16) |

`:layout` 下沉到 core 需要把 `display_width` 从 `ftxui::string_width` 换成自带的宽度表 —— core 里已经有 `utf8.cppm`,这是一次性成本,换来的是全部 437 个 `log::` 调用点第一次进入宽度契约。

#### 3.3.1 `tui`:行内渲染 + 行内交互,交互与否是**配置**

**已定方向 2 + 6**:不做全屏 TUI;`tui` 的配色、是否交互等做成主题/配置驱动,**不是新的模式**。

所以模式只有两个(`cli` / `tui`),而 `tui` 的行为由配置调:

```
tui + interactive=false   逐条输出 → 渲染 Document → 追加到滚动缓冲。今天的样子。
tui + interactive=true    同上,但遇到 Request 时就地弹出一个几行高的部件,
                          选完收起,继续往下走。终端历史保持完整。
```

**"交互"从模式降级成配置**,这一步比看上去重要:它意味着交互能力不改变输出形态。同一条命令在 `interactive` 开和关下,**非交互部分的输出逐字节相同** —— 差别只在遇到 `Request` 那一刻。这让 e2e 只需要测一份输出,而不是两份。

关键实现细节:必须用 `ScreenInteractive::TerminalOutput()`(行内,不进 alt-screen),而**不是** `Fullscreen()`。仓库里两种都有现成的:

- `select_version()`(`ui/selector.cpp:36`)用的正是 `TerminalOutput()` —— **它就是我们要的形态**,写好了,零调用者。
- `select_option()`(`ui/selector.cpp:103`)用的是 `Fullscreen()` —— 按已定方向 2,它要么改成 `TerminalOutput()`,要么删掉。

所以"交互式 TUI"的工作量主要不是写部件,是**接线 + 把 `Request` 类型化**(§3.5)。

**"不做全屏"不只是省事,它删掉了一整类问题。** 一个从不接管屏幕的 TUI 意味着:

| 不需要处理的 | 为什么 |
|---|---|
| alt-screen 进/出 | 从不进 |
| 终端状态恢复(SIGINT / 崩溃 / `exec` 走掉) | 没有要恢复的状态;`main.cpp:28` 那个 `atexit` 光标恢复兜底变成唯一一处 |
| 窗口 resize 重排 | 输出已经追加到滚动缓冲,不重排历史 |
| "报告放不下一屏怎么办" | 不分页 —— 这正是 `ui/layout` 注释里 `fit_full_height` 的判断:「A report is not a viewport」 |
| 全屏下 stdout 被占用,日志往哪去 | 日志就在原处,和平时一样 |

代价只有一个:`self doctor` 这类几十行的报告没有可滚动的浏览器。但那本来就该用 `| less` 解决 —— 而且**只有不接管屏幕,`| less` 才是可能的**。

#### 3.3.2 `UiMode` 与能力

`palette.cppm:57-71` 已经把决策拆成三个正交问题(colours / cursor rewrite / live progress),论证写得很对。把它提升成显式模型 —— 注意枚举**只有两个值**:

```cpp
enum class UiMode { Cli, Tui };          // ndjson 是 `interface` 子命令,不是渲染模式

struct UiCapabilities {
    bool color;
    bool cursorRewrite;   // 能原地重绘
    bool interactive;     // ← 配置驱动,不是模式
    bool localized;       // 需要翻译;ndjson 路径 = false
    std::optional<int> width;   // nullopt = 无上限
};
```

三样东西从"模式"降级成"能力/配置",各有理由:

| 原来想做成模式 | 现在是 | 理由 |
|---|---|---|
| `tui-i` | `tui` + `interactive=true` | 交互不改变非交互部分的输出(§3.3.1) |
| `gui` | 独立二进制 `xlings-gui` | 它不是同一个程序的另一种画法(§3.6) |
| `ndjson` | `interface` 子命令(已有) | 它是另一个**入口点**,输出的不是给人看的文档 |
| 全屏 | 不存在 | 已定方向 2 |

**自动降级必须说出来**(无声降级是这个项目反复踩的坑):

- 请求 `tui` 但 stdout 非 tty → 降到 `cli`,发一条 `Diagnostic{Note}`。
- 请求 `tui` 但二进制没编 `tui` 特性 → 降到 `cli`,`Note`。
- `interactive=true` 但 stdout 非 tty 或带了 `--agent` → 交互能力关闭;遇到 `Request` 按 §3.5 报错,不猜。

**`is_tui_mode()` 必须改名**(`platform::is_headless_output()`)—— 它现在的含义是"NDJSON,别往终端写",和 `--ui-mode tui` 字面冲突。

### 3.4 i18n:链路已经通了,缺的是中间两行

**先纠正我上一版的说法。** 我写的"i18n 是死代码"不准确 —— 真实情况更值得注意:**配置链路是完整的,而且有 e2e 覆盖**。

| 环节 | 状态 | 证据 |
|---|---|---|
| CLI 选项 | ✅ 已有 | `cli.cpp:1193` `Option("lang").takes_value().help("Set language (en/zh)")` |
| 锁下写入 `~/.xlings.json` | ✅ 已有 | `cli.cpp:509-513`,走 `update_home_config` |
| 全局读取 | ✅ 已有 | `config.cpp:489-490` |
| 项目覆盖全局 | ✅ 已有 | `config.cpp:592` |
| `xlings config` 回显 | ✅ 已有 | `cli.cpp:598` |
| schema 文档 | ✅ 已有 | `docs/spec/xlings-json-schema.md:24`「界面语言」 |
| **e2e 断言** | ✅ **已有** | `tests/e2e/home_config_lock_test.sh:147-152`:跑 `config --lang en` 并断言值落盘 |
| 中英对照表 | ✅ 已有 | `i18n.cppm`,30 条 |
| `auto` 回退 | ✅ 已有 | `i18n.cpp:16` 调 `platform::get_system_language()` |
| **`set_language(Config::lang())`** | ❌ **无人调用** | — |
| **调用点用 `tr()`** | ❌ **0 处** | — |

也就是说:`lang` 和 `mirror` 走的是**同一条已经修好的路**,只有最后两步没接。这不是"要设计一个 i18n 方案",是**接两行线 + 把文案迁成 msgid**。

`--ui-mode` / `--theme` / `--interactive` 都应当**完全复用这条路**,一个新机制都不引入 —— 完整的配置面见 §4。

#### ⚠️ `auto` 现在**永远只能得到 `en`** —— 实测

**已定的方向**是"默认跟随系统语言"。但现有的探测实现做不到这件事,而且失败得完全无声。

`platform::get_system_language()`(`platform.cpp:88`)用 `std::locale("")`,失败时 `catch (const std::runtime_error&) { return "en"; }`。实测(本机,`LANG=en_US.UTF-8`):

```
                     setlocale(LC_ALL,"")     std::locale("")
glibc  动态           (null)                  threw: name not valid
musl   静态(发布形态) en_US.UTF-8             threw: name not valid
LANG=C 两者           C                       "C"  → 归一成 "en"
```

- **musl 上是结构性的**:musl 的 `newlocale()` 只支持 `C`/`POSIX`,libstdc++ 的 `_S_create_c_locale` 因此对任何真实 locale 名抛异常。而**发布的 Linux 二进制正是静态 musl**(`file` 确认:`statically linked`)。
- **glibc 上也不可靠**:取决于目标机器有没有 `locale-gen` 过那个 locale;没有就抛,一样退化成 `en`。

于是 `auto` 的两种结局——"系统真是英文"和"探测彻底失败"——**输出一模一样**。这正是姊妹文档反复讲的那类 bug,出现在了要实现"跟随系统语言"的那个函数里。

**修法**:不碰 libc locale,直接读环境变量。

```cpp
// POSIX: LC_ALL > LC_MESSAGES > LANG,取第一个非空,截到 '_' / '.' / '@' 之前
// Windows: GetUserDefaultLocaleName() → "zh-CN" → "zh"
// 都拿不到 → "en",并且这是一个**可观测的**结论(debug 日志说明来源)
```

配套单测:设置 `LANG=zh_CN.UTF-8` 断言得到 `zh`,设 `LC_ALL=C` 断言得到 `en` —— 这个测试今天会**直接失败**,正好是回归护栏该有的样子。

#### 翻译发生在哪一层

```
core      发 Text{ msgid, args }        —— 永不翻译,永不调用 tr()
ui/cli    tr(msgid, args, lang)
ui/tui    tr(msgid, args, lang)
ui/gui    tr(msgid, args, lang)         —— 常驻进程,可不重启换语言
interface 原样透传 msgid                 —— 唯一不翻译的前端
```

**表放在哪:留在 `core`。** 三个前端模块共用一张表,搬进任何一个 UI 模块都会造成依赖倒置;留在 core、并加一条 "core 里除 `i18n.cpp` 外不得出现 `i18n::tr`" 的 grep 测试,就同时满足"一份表"和"core 不做本地化"。这比把表搬走更简单,也更好守。

两处要改的设计:

1. **键从枚举换成字符串。** `Msg` 枚举 + 数组下标 + `static_assert(表长 == 枚举长)` 意味着每加一条诊断都必须同时写中文,否则编译失败 —— 这大概是这个模块从没被用起来的直接原因之一。字符串键让缺翻译退化成英文 fallback,新增诊断是一行的事。
2. **msgid 与 `Diagnostic::code` 共用命名空间**,一张表管两件事(`xvm.not_in_subos` 既是错误码也是文案键)。

### 3.5 交互:从"阻塞 prompt"升级为 Request/Response

现有 `EventStream::prompt()`(`event_stream.cpp:45`)其实已经是正确形状:emit + condvar 等待 + `respond()` 唤醒,支持 `CancellationToken` 和超时。GUI 完全能用——命令跑在 worker 线程,GUI 线程 `respond()`。

四处要改:

1. **`interactive == false` 时,`Request` 必须失败,不能猜。** 这是已定方向 4(`--agent` = "尽量没有阻塞")的直接推论,也是修掉 §5 那个真 bug 的地方:今天 `--agent` 无条件回 `defaultValue`,于是 `remove` 的默认 `"n"` 让 `xlings remove foo --agent` **什么都不做且退出码 0**。

   "不阻塞"的正确实现是**报错说清楚**,不是替用户按键:

   ```
   error: this operation needs confirmation, and --agent cannot ask
     what it would do   remove llvm@22.1.8 from subos 'default'
     to proceed         add -y
   ```
   退出码非 0。agent 拿到一条能直接照做的指令,而不是一个假的成功。

2. **默认 30s 超时对 GUI 太短。** 人在 GUI 前思考 30 秒很正常,超时返回 `""` 会被当成"选了默认值"。超时由 `UiCapabilities` 决定:`interactive=false` → 不等待、直接走第 1 条;`tui` 交互 → 无限 + `esc` 取消;GUI → 无限 + 可取消。**没有任何模式应该有"静默超时后当成默认值"这个行为**。
3. **CLI 走的不是这条路。** `cli.cpp:274` 在事件回调里**同步**调 `ui::confirm()`,也就是在发起线程上直接读 stdin,condvar 那条路 CLI 根本没用到。两条交互路径,只有一条被测试。统一到 `respond()`。
4. **`Request` 要有类型**,不只是 `options[]`:`Confirm` / `PickOne{items, default}` / `FreeText{validator}` / `PickMany`。GUI 才能画对控件;`select_version()` 那段死代码正好是 `PickOne` 的现成实现。
5. **`CompletedEvent` 要真的发出来。** 常驻 GUI 必须知道任务结束(现在只有函数返回值,GUI 的 worker 线程要自己包装)。

### 3.6 GUI:独立二进制 `xlings-gui`

#### 3.6.1 为什么必须独立 —— 一条实测约束

`mcpp.toml` 声明 `[target.x86_64-linux-musl] linkage = "static"`,发布产物经 `file` 确认 `statically linked`。而静态 musl 的 `dlopen` 是个 stub:

```
                       dlopen("libm.so.6")
musl 静态               NULL — "Dynamic loading not supported"
glibc 动态(对照)       0x7e5ded125820 — ok
```

GLFW 靠 `dlopen` 找 X11 / Wayland / libGL;GL 驱动本身也只能是 `dlopen` 来的。

**这条约束的作用范围要说准**:它约束的是 **`xlings` 这个引导二进制**,不是 Linux 平台。`xlings` 必须是"到处能跑的静态单文件"——那是它自举整个系统的前提;同一个文件不可能同时是"能开窗口的动态 GL 客户端"。

**所以 GUI 是独立二进制。** 结论由这条约束逼出,不是风格选择。而一旦独立,约束就不再适用于它。

#### 3.6.2 Linux 当然有 GUI —— 图形栈已经闭环

初稿在这里下错了结论。`xlings-gui` 独立之后就是一个**普通的动态链接程序**,和索引里任何 GUI 应用没有区别,而 xlings 生态的图形栈是齐的:

```
libX11 libxcb libXau libXdmcp libXext libXfixes libXi libXinerama
libXrandr libXrender libXcursor libXtst libXxf86vm libxshmfence xorgproto
wayland wayland-protocols libxkbcommon
libglvnd mesa vulkan-loader vulkan-headers libdrm
freetype fontconfig expat
```

**现成模板**:`pkgs/g/godot.lua:138-150` —— 一个真正的 GUI 应用,动态链接 `xim:glibc@>=2.39`,把整套 GL/X11/wayland 栈连同 GPU 驱动桥接一起声明成 runtime deps,注释里连"`libfreetype.so.6` 是启动时 `dlopen` 的、不在 `DT_NEEDED` 里、所以必须显式声明"这种细节都踩过了。`pkgs/g/griddycode.lua` 是第二例。

`xlings-gui` 的配方就是同一份:

```lua
deps = { runtime = { "xim:glibc@>=2.39", "xim:libglvnd@…", "xim:mesa@…",
                     "xim:libX11@…", "xim:wayland@…", "xim:freetype@…", … } }
```

也就是说 **Linux 不但拿得到 GUI,而且是这三个平台里路径最成熟的一个** —— macOS / Windows 反而要各自处理系统窗口系统,没有 xlings 托管的栈可依赖。

#### 3.6.3 构建形态:workspace 里的第二个二进制

mcpp 文档里正好有这个例子(`05-mcpp-toml.md:117-120`):

```toml
[targets.xlings-gui]
kind = "bin"
main = "apps/gui/main.cpp"
required_features = ["gui"]     # 特性没开就静默跳过,不是错误
```

分两步,因为第二步的代价不该现在付:

**Phase A —— 同包第二 target(现在)。** 零结构调整。开发时 `mcpp build --features gui` 就多出一个 `xlings-gui`;musl release 不开这个特性,`required_features` 让缺席是**静默跳过**(mcpp 定义的语义,不是我们的兜底)。

**Phase B —— 拆成 workspace member(要发布 Linux GUI 时)。** 同一个包的两个 target 共享一次构建,也就共享 `--target` 三元组和 `linkage`。要让 `xlings` 是 musl 静态、`xlings-gui` 是 glibc 动态,**必须是两个包**:

```
mcpp.toml         [workspace] + [package] xlings (bin)
core/             lib   ← 从 src/ 抽出来,两个二进制共用
ui/cli/           lib   ← 无 UI 依赖
ui/tui/           lib   ← ftxui
apps/gui/         bin   ← xlings-gui,imgui;自己的三元组和链接方式
```

```bash
mcpp build --target x86_64-linux-musl      # 根包:xlings,静态
mcpp build -p gui                          # 成员:xlings-gui,glibc 动态
```

(`06-workspace.md:130-136`:命令行 > 成员 `mcpp.toml` > workspace 根。)

Phase B 的真实代价是**把 `core/` 从根包抽成成员 lib**(成员不能反向依赖根包)。这一步迟早要做 —— §3.3 的 `:layout` 下沉指向同一个方向 —— 但不该和第一个 GUI 原型绑在一起。

#### 3.6.4 分发:`xlings-gui` 就是一个 xlings 包

**已定方向 5 的推论:`--ui-mode gui` 不存在。** 想要 GUI 就运行 `xlings-gui`,没有第二条入口,也就没有"去哪找它、版本对不对、找不到怎么降级"这一整套问题。这是独立二进制真正的好处 —— 它删掉的不是代码,是**一整类需要维护的规则**。

```
xlings install xlings-gui     # 和装任何 GUI 应用一样,拉齐图形栈依赖
xlings-gui                    # 跑它
```

`xlings-gui` 因此像任何程序一样被 xvm 注册(有 payload、有 shim、有版本、能 `xlings remove`),不需要别名机制,也不需要在 `SHIM_NAMES` 里特判。

**带参数的别名机制留给它真正该服务的场景。** 它确实好用且在生产使用(`mcpp-short-cmd.lua:116` 的 `alias = "mcpp " .. sub` 是一整族短命令,`musl-gcc.lua:343` 的 `"musl-gcc -static"`,`musl-cross-make.lua:60` 甚至 `alias = "xlings script " .. file` 直接指向 xlings 自己)—— 那是"同一个二进制换个名字换套默认参数"。`xlings-gui` 不是那个形状,它是另一个二进制。

顺带:走包分发意味着 **Linux/macOS/Windows 三平台的 GUI 都由索引配方描述**,发布矩阵不变,`mirror-binaries` 不变。这也正是 xlings 该 dogfood 自己的场景。

#### 3.6.5 "同进程"仍然成立

已定方向 1 说"先都同进程"。GUI 成了独立二进制,看起来像拆了进程 —— 但**要拆的那个东西没有拆**:

```
xlings-gui 进程内:  GUI 线程 ←→ EventStream ←→ worker 线程直接调 cmd_install(...)
                    没有 NDJSON,没有 IPC,没有第二份协议
```

它是**第二个入口点,不是第二套架构**。§1.8 那 15 个没有 capability 的操作(`self doctor` 等)照样能直接调 `int cmd_x(args, EventStream&)`,补 capability 仍是并行项而不是阻塞项。

真正的进程拆分(GUI ↔ `xlings interface` 走 NDJSON)留在更远处,§3.6.6 的依赖白名单是保住那条后路的护栏。

#### 3.6.6 护栏:UI 模块的依赖白名单

同进程有一个**必须现在就上护栏**的代价,否则"以后拆进程"会变成"以后重写":

> 同进程的 GUI 可以偷偷 `import xlings.core.config` 直接读 `Config::`,可以调 `log::`,可以绕过 `Document` 自己去查数据。每一次这样做,都在给未来的拆分加一根绳子 —— 而且会让 §1.4 那 9 个孤儿以新形式重新长出来。

护栏:**`ui/*` 三个模块的 `[dependencies]` 里不许出现 core 的数据模块。** 它们只能依赖 `xlings.view`(类型)+ `xlings.core.i18n`(查表)。mcpp 的 workspace 让这一条**声明式可检查** —— 不需要 grep 测试,`ui/gui/mcpp.toml` 里没写就是没有,CI 加一条"UI 成员依赖白名单"检查即可。

注意这条护栏管的是 **`ui/*` 三个渲染模块**,不是 `apps/gui` 那个二进制入口 —— 后者当然要调命令函数,那是它的工作。

同进程 vs 拆进程的其余权衡留给以后:

| | 同进程(现在) | 拆进程(以后) |
|---|---|---|
| 协议开销 | 直接传 `Document` 对象,零序列化 | NDJSON v1.0 |
| 契约压力 | 靠上面那条依赖白名单 | 天然:协议漏一屏就白屏 |
| 崩溃隔离 | GUI 崩 = 包管理器崩 | 隔离 |
| 能力缺口(§1.8) | **不阻塞** —— 直接调命令函数 | 必须先补 15 个 capability |

最后一行是现在最大的实际好处:`self doctor` 等 15 个没有 capability 的操作,GUI 可以直接调 `int cmd_x(args, EventStream&)`,不必等能力层补齐。补 capability 仍然该做,但它从**阻塞项**变成**并行项**。

**依赖可得性已确认**:`mcpplibs:imgui` 是现成的 C++23 module 包(`import imgui.core;`,含 GLFW/OpenGL3 后端),`compat:glfw` / `compat:sdl2` 也在索引里 —— 不需要 vendoring。

### 3.7 `Sink`:log 不再直接写 FILE*

`log::` 现在直接 `fwrite(stdout/stderr)` + `console::output_mutex()`。GUI 里没有 stdout。

```cpp
struct Sink {
    virtual void write(const Diagnostic&) = 0;
    virtual void write(const Document&)   = 0;
};
```

`log::set_sink()` 替代今天的 `is_tui_mode()` 静音开关 —— 静音变成"装一个 NullSink",GUI 变成"装一个 RingBufferSink",而不是在 `log.cppm` 里散布五处 `if (!platform::is_tui_mode())`。

文件日志(`log::set_file`)保持独立,它和终端输出本来就是两回事,这一点现在的设计是对的。

---

### 3.8 主题:独立模块,先只做配色

**已定方向**:主题先只做配色;独立成一个模块方便以后扩展;`default` 是编译进代码的配置,自带的几个可选配色文件放 `config/themes/`;`.xlings.json` 里做**路径引用**。

#### 3.8.1 现状实测:语义层已经有了,但 119:7 没在用

今天的配色是两层:

| 层 | 位置 | 词汇 |
|---|---|---|
| `core/palette.cppm` | core(因为 `log::` 在 core 里写 SGR) | **按颜色命名**:`cyan/green/amber/red/magenta/dim/text/surface/border`,各有 dark/light 两套 |
| `ui/theme.cppm` | ui(ftxui 适配) | **按角色命名**:`title/success/warning/error/hint/highlight/label/body` |

角色层是对的东西。问题是没人用它:

```
按角色调用(theme::title / hint / highlight …)          7 处
按颜色调用(theme::cyan / dim_color / text_color …)    119 处
core 里直接用 palette 颜色(log:: 的四个级别前缀)        4 处
```

**94% 的配色决策是在点名"什么颜色",不是"什么角色"。** 所以一个重映射角色的主题文件,今天改不动任何东西 —— 这才是"做配色主题"的真实成本所在,不是文件格式。

这又是这两份文档里反复出现的同一个形状:抽象在,调用点绕过它。

#### 3.8.2 关键发现:palette 的**基数是对的,词汇是错的**

把那 119 处按用途归类,和 palette 的 9 个槽**一一对应**:

| 今天(颜色名) | 用量 | 实际角色 | 主题里的名字 |
|---|---|---|---|
| `dim_color` | 31 | 次要信息、路径、说明 | `muted` |
| `text_color` | 20 | 正文 | `text` |
| `green` | 16 | 成功、已安装 | `success` |
| `magenta` | 12 | 命令名、标识符 | `alt` |
| `cyan` | 11 | 强调、当前项、进度 | `accent` |
| `border_color` | 11 | 分隔线、面板边 | `border` |
| `red` | 10 | 错误 | `error` |
| `amber` | 8 | 警告 | `warn` |
| `surface` | — | 背景 | `surface` |

所以这不是"设计一套新的槽位",是**一次改名**:`cyan→accent`、`magenta→alt`、`green→success`、`amber→warn`、`red→error`、`dim→muted`,其余同名。

三个好处:

1. **机械可做、可验证** —— 改完之后加一条 grep 测试:`ui/` 里不得再出现按颜色命名的取色函数。这条测试今天会抓到 119 处,改完是 0。
2. **内置默认主题就是今天的取值** —— 零视觉变化,因此这一步是纯重构,可以独立发布、独立回滚。
3. **主题文件从此有意义** —— 换 `accent` 会同时改到进度条、当前项标记、面板标题,因为它们本来就是同一个角色。

`ui/theme.cppm` 现有的 8 个 Decorator(`title/success/warning/error/hint/…`)保留,它们是**角色的组合**(角色 + 粗体/下划线),建在 9 个槽之上。

#### 3.8.3 模块边界

```
xlings.theme          ← 新增,叶子模块,零依赖
  ├─ 9 个语义槽 + dark/light 两套取值
  ├─ 内置主题(编译进二进制,default / mono / high-contrast …)
  ├─ 主题文件解析(JSON)
  └─ 解析顺序:用户主题 → 官方主题 → 内置 default
```

零依赖是有意的:`ui/tui` 要用它,`ui/gui` 以后要用它,而**迁移期 `core/log.cppm` 也还要用它**(在 §3.7 的 `Sink` 落地、core 不再直接写 SGR 之前)。叶子模块谁都能依赖,不会造成方向问题。

`core/palette.cppm` 相应收缩成它本来该是的东西:**背景探测 + 输出能力判定**(`stdout_is_terminal` / `colors_enabled` / `cursor_rewrite_allowed` / OSC-11 探测),不再拥有颜色值。这两件事本来就是不同的问题 —— 一个是"终端能不能上色",一个是"上什么色"。

`ui/theme.cppm` 保留为 **ftxui 适配层**(`Rgb → ftxui::Color`、Decorator),名字可能要改成 `:style` 以免和新的 `xlings.theme` 混淆。

#### 3.8.4 机制:`.xlings.json` 里是一个**路径引用**

**已定方向**:不要 `official/` 这一层目录。核心是 `.xlings.json` 里做**路径引用**,自带的那几个只是放在 `config/themes/` 下而已。

```
内置 default        →  编译进二进制(theme_resources.cppm,比照 profile_resources.cppm)
config/themes/      →  自带的可选主题文件
  ├─ mono.json          无色差:给色觉障碍、黑白终端、以及"别花哨"的人
  └─ high-contrast.json 极端环境:强光下的笔记本、投影、低质量终端
用户主题             →  **放哪都行**,在 .xlings.json 里写路径
```

**就这三个,不再多。** `default`(好看)/ `mono`(可访问)/ `high-contrast`(极端环境)覆盖了三类真实需求;再加就成了维护负担 —— 每个自带主题都要在 9 个槽 × dark/light 两套下被人工看过一遍,否则它就是一个带着 xlings 招牌的坏配色。

```json
// ~/.xlings/.xlings.json
{ "mirror": "CN", "lang": "auto", "theme": "config/themes/mono.json" }

// 项目 .xlings.json —— 相对路径按项目根解析
{ "theme": "./tools/brand-theme.json" }
```

路径解析**复用现成规则**,不新发明:`config.cpp:929-931` 已经在为本地索引源做同一件事 ——

```cpp
auto base = projectScope ? project_dir() : paths().homeDir;
```

绝对路径原样用,相对路径按"全局配置→home / 项目配置→项目根"解析。于是**一个项目可以自带配色**,和它自带索引源是同一个机制。

CLI 上给一层糖,存进文件的仍是路径:

```bash
xlings config --theme mono                    # 糖 → "config/themes/mono.json"
xlings config --theme ./brand.json            # 直接给路径
xlings config --theme default                 # 回到内置
xlings config --theme list                    # 内置 + config/themes/ + 当前引用
```

**所有权因此不靠目录约定,靠"用户指向哪里"**:`config/themes/` 整个归 xlings,升级时按版本标记覆盖(和 `config/shell/` 同款);用户要改就把 `theme` 指向自己的文件。仍有一个小坑要挡:直接原地改 `config/themes/mono.json` 是很自然的动作,升级会覆盖它 —— 文件顶部写一行 `"_comment": "shipped by xlings; copy me, do not edit in place"`,并且 `--theme list` 顺带提示复制的写法。因为 `default` 是编译进去的、这些文件都是**可选**的,被覆盖的后果远小于"用户唯一的主题没了"。

#### 3.8.5 格式:部分覆盖,没有 `extends`

**已定方向**:`default` 是代码里的配置,主题文件**没有覆盖到的就用默认的**。

所以主题是一层**部分覆盖**,只写想改的槽:

```json
{
  "_comment": "shipped by xlings; copy me, do not edit in place",
  "name": "mono",
  "dark":  { "accent": "#C9D1D9", "alt": "#C9D1D9", "success": "#C9D1D9" },
  "light": { "accent": "#24292F", "alt": "#24292F", "success": "#24292F" }
}
```

这条比我上一版好,而且直接**删掉了一个字段**:我原本设计了 `extends` 来表达"我只想改 accent"。有了"缺省即回落到内置 default",`extends` 就是多余的 —— 它只会带来"继承链要不要做几层"这种没有收益的问题。

三条约束:

1. **槽位封闭在 9 个角色**(`accent / alt / success / warn / error / text / muted / border / surface`)。未知键报一条 `Note`(拼错了要能发现),不是静默忽略。**不开放按 `Document::id` 覆盖** —— 那会让主题和 id 表耦合,以后加一个屏幕就要同步所有主题文件。
2. **`dark` / `light` 各自独立回落。** 只写 `dark` 的主题在浅色终端下完全等于 default,这是对的:没写就是没意见。
3. **文件读不出来不能静默**。路径不存在、JSON 坏了、槽位名拼错 —— 都要一条 `Diagnostic`,并说清楚"现在用的是内置 default"。静默回落到默认,正是这个项目反复出问题的那个形状:配错了和没配过,输出一模一样。

#### 3.8.6 首次交互提示:只提示一次

**已定方向**:`interactive=true` 首次弹选择器时给一行提示,**仅第一次**。

```
▸ install 22.1.8 here
  install 20.1.7 here
  cancel
↑↓ select · enter confirm · esc skip
  (first time only) esc 跳过本次;xlings config --interactive false 永久关闭
```

"只提示一次"需要**跨进程**的状态,而现成的 `xself::print_migration_hint_once`(`repair.cpp:139-148`)是 `static bool shown`,**只在一个进程内有效** —— 直接拿来用会变成"每次运行都提示第一次"。

持久化位置跟着现有惯例走:`~/.xlings/.xlings.json` 已经在存 `activeSubos` / `mirror` / `version` 这类状态(`Config::recorded_client_version()` 读的就是其中的 `version`),加一个平铺的记录即可:

```json
{ "hintsSeen": ["tui.interactive.first-run"] }
```

用数组而不是布尔,是因为这类"一次性提示"以后不会只有一个;一个数组让新增提示不必每次改 schema。写入要走 `update_home_config`(锁下读改写),和别的家目录写入同一条路径。

## 四、`--ui-mode` 与配置面

**设计原则:和 `mirror` 走完全同一条路,不引入任何新机制。**

```bash
# 持久化 —— 与 `xlings config --mirror CN` 同款(cli.cpp:1194 旁边加几行 Option)
xlings config --ui-mode     cli|tui|auto
xlings config --lang        en|zh|auto
xlings config --theme       mono|./brand.json|default # 配色:名字是糖,存的是路径(§3.8.4)
xlings config --background  dark|light|auto              # 终端背景(原 XLINGS_THEME)
xlings config --interactive true|false                   # tui 是否行内交互
xlings config                            # 回显,和 mirror 一样列在面板里

# 单次覆盖
xlings --ui-mode cli install gcc         # CI / 管道:无色、无重绘、无上限宽度
xlings --ui-mode tui install gcc         # 默认(tty 时)

# 不在 --ui-mode 里的两个入口
xlings interface install gcc             # NDJSON:另一个入口点,不是渲染模式
xlings-gui                               # GUI:另一个二进制(§3.6)
```

配置落在 `~/.xlings.json` / 项目 `.xlings.json`。**全局性的键平铺,前端自己的设置进子块**:

```json
{
  "mirror": "CN",
  "lang":   "auto",
  "uiMode": "auto",
  "theme":  "config/themes/mono.json",
  "tui": {
    "interactive": true
  }
}
```

分界的理由:`mirror` / `lang` / `uiMode` / `theme` 回答"用哪个源、哪种语言、哪个前端、哪套配色",是**全局选择**,和已有的 `mirror`/`lang` 同级 —— `theme` 平铺是因为 `gui` 以后也要用同一套配色,它不属于 `tui`。`tui.interactive` 才是那个前端自己的设置,换到 `cli` 就没有意义;以后 `gui` 有自己的块(窗口大小、DPI、字体),互不干扰。

**配色本身不写在这里** —— `theme` 存的是一个**路径引用**,主题是一份独立文件(§3.8.4)。理由和 shell profile 一样:一份能抄、能分享、能 diff 的文件,比塞进主配置的一个对象好用;而且路径引用让**项目可以自带配色**,和它自带索引源是同一个机制。

> 我上一版提议把 `lang` 也收进 `"ui": { … }` 子对象。**撤回** —— `lang` 已经在顶层、已经进 schema 文档、已经有 e2e 断言它落在顶层(`home_config_lock_test.sh:152` 读的是 `d['lang']`)。为了"更整齐"去搬它,收益是零,成本是一个兼容层加一条会坏的测试。新键跟着老键的形状走。

**注意 `theme` 和现有的 `XLINGS_THEME` 是两件事,别合并**:

| | 回答什么 | 现状 | 归属 |
|---|---|---|---|
| `XLINGS_THEME=dark\|light\|auto` | **终端背景是深是浅** | 已有:OSC-11 探测 + `COLORFGBG` 回退 | 留在 `palette`(输出能力判定) |
| `theme = default\|mono\|…` | **用哪套配色** | 新增 | `xlings.theme` 模块(§3.8) |

一套主题**同时**给出 dark 和 light 两组取值,由背景探测挑其中一组。把两者合成一个键会让"我在浅色终端下想用 mono"变得无法表达。为对称起见,背景探测也补一个 `config --background dark|light|auto`,`XLINGS_THEME` 保留为兼容别名。

**优先级**(与 `mirror` 一致,e2e 守住):

```
1. 单次 flag(--ui-mode / --lang / --theme / --interactive)
2. 项目 .xlings.json
3. 全局 ~/.xlings.json
4. auto:
     lang     → LC_ALL > LC_MESSAGES > LANG(POSIX)/ GetUserDefaultLocaleName(Windows)
     uiMode   → stdout 非 tty → cli;--agent → cli;否则 → tui
     theme      → 内置 default(编译进二进制;主题文件只覆盖它写了的槽)
     background → OSC-11 探测 → COLORFGBG → dark
     interactive→ **true**(非 tty / --agent 时自动关)
```

**测试注入不需要新东西**:e2e 建好隔离 home 之后跑一次

```sh
RUN config --lang en --ui-mode cli --theme default --interactive false
```

和 `tests/fresh-install/smoke.sh:75` 跑 `config --mirror GLOBAL` 是同一个动作。任何机器上结果都一样,`auto` 的机器差异被挡在测试之外。

**兼容性**:`--agent` 保留,语义是 `--ui-mode cli` + `interactive=false` + **打印 msgid**(已定方向 4);`xlings interface` 不变。两者都已写进文档和 agent skill,不能删。

## 五、UX 设计:同一件事在各个前端

以姊妹文档的例 1(`xlings use llvm`,llvm 装在别的 subos)为例。**同一个 `Diagnostic`**:

**`ui/cli`**(无 ftxui;管道 / CI / `--agent` 走这条)
```
error: llvm is not installed in this subos (default)
  installed elsewhere: 22.1.8, 20.1.7
  install it here: xlings install llvm@22.1.8
  see every subos: xlings use llvm --all
```

**`ui/cli` + `--agent`** —— 同一个渲染器,多打一个 msgid(已定方向 4):
```
error: llvm is not installed in this subos (default)  [xvm.not_in_subos]
  installed elsewhere: 22.1.8, 20.1.7  [installed_elsewhere]
  install it here: xlings install llvm@22.1.8  [action.install_here]
  see every subos: xlings use llvm --all  [action.see_all_subos]
```
msgid 放在行尾方括号里:agent 拿到稳定可 grep 的键(比匹配英文散文可靠得多),人眼可以直接忽略。**没有第二个渲染器**,所以不可能出现"人看到的和 agent 看到的不是同一件事"。

**`ui/tui`,`interactive = false`**(tty 默认)
```
[error] llvm is not installed in this subos (default)
        installed elsewhere   22.1.8, 20.1.7
        install it here       xlings install llvm@22.1.8
        see every subos       xlings use llvm --all
```

**`ui/tui`,`interactive = true`** —— 诊断的 `actions` 直接变成可选项。**行内弹出**(`ScreenInteractive::TerminalOutput()`),选完收起,上面的终端历史原样保留:
```
[error] llvm is not installed in this subos (default)
        installed elsewhere   22.1.8, 20.1.7

        ▸ install 22.1.8 here
          install 20.1.7 here
          show every subos
          cancel
        ↑↓ select · enter confirm · esc cancel
```
选完这一块收起,替换成一行结果 —— 终端里留下的是 `[xlings] llvm -> 22.1.8`,而不是部件残骸。**注意上面四行和 `interactive=false` 时逐字节相同** —— 交互是配置,不改变输出形态(§3.3.1)。

**`xlings-gui`** —— 同样的 `actions` 变成按钮;`VersionList` 变成下拉;`Ref{package, llvm}` 变成可点链接。**三个前端读的是同一个 `actions` 数组** —— 如果 GUI 需要一个"第四个按钮"而 `Diagnostic` 里没有,那说明 `Diagnostic` 定义漏了东西,而不是 GUI 该自己造一个。

**`xlings interface`** —— 不翻译,`msgid` 原样:
```json
{"kind":"error","code":"xvm.not_in_subos","msgid":"xvm.not_in_subos",
 "args":{"target":"llvm","subos":"default"},
 "facts":[{"msgid":"installed_elsewhere","versions":["22.1.8","20.1.7"]}],
 "actions":[{"msgid":"action.install_here","command":"xlings install llvm@22.1.8"}]}
```

**这是整个设计的检验点**:如果 `Diagnostic` / `Document` 定义得对,以上都是同一份数据的几次渲染;如果定义得不对,就得在 core 里写几个分支——那就是今天的样子。

另外三条跨前端的 UX 规矩:

1. **降级要说出来。** "我请求了 tui 但你给了我 cli" 必须是一条 `Note`,不能静默。
2. **actions 在任何前端下都可执行。** `cli`/`tui` 里是可复制的命令行,交互/GUI 里是可点击项 —— 所以 `Action` 必须同时带 `msgid`(说人话)和 `command`(能跑)。
3. **非交互下遇到 `Request` 必须失败而不是猜**(§3.5)。

## 六、落地路线

六步,每步独立可发布。**前两步不引入任何新前端**,但今天就能修掉 §1.4 的孤儿和 §1.6 的假返回值。

| 步 | 内容 | 产出 |
|---|---|---|
| **0** | 立规矩:`Document::id` 枚举表 + "每个 id × 每个前端" 矩阵测试;`unhandled DataEvent` 从 `debug` 提到 `warn` | 9 个孤儿立刻可见;`subos new --from` 的静默被抓 |
| **1** | `View::Document` 落地(第一批 id 见 §8);`prevLines`/`nameWidth` 移出协议,渲染器返回值接回去;`:layout` 下沉到 core | 全部 437 个 `log::` 第一次进入宽度契约;顺手修掉光标记账 |
| **2** | workspace 拆出 `ui/cli` + `ui/tui`;**`agent/text_renderer` 并入 `ui/cli`**;`UiMode{Cli,Tui}` + `UiCapabilities` 收敛八个旋钮;`is_tui_mode` 改名;`config --ui-mode/--interactive` + 对应 flag | 19-vs-18 的分裂**按构造消失**;交互变成配置 |
| **2.5** | `xlings.theme` 叶子模块;**119 处按颜色取色改成按角色取色**(纯重构,内置 default = 今天的取值,零视觉变化);`config/themes/` provisioning;`config --theme <路径\|名字>` | 主题文件第一次真的能改变什么;grep 测试从 119 → 0 |
| **3** | i18n 接线:`set_language(Config::lang())` + 文案迁 msgid + **重写 `get_system_language()`**(读 env,不用 `std::locale`)+ 单测;`--agent` 打印 msgid;**删掉 `config/i18n/{en,zh}.json`** | `lang` 兑现 schema 承诺;`auto` 第一次真能跟随系统;i18n 只剩一套机制 |
| **4** | `Request` 类型化(含**非交互必须报错、不许猜**)+ `CompletedEvent` 真发;`select_version()` 接线;`select_option()` 改 `TerminalOutput()`;`tui.interactive` 生效 | 行内交互(死代码变功能);`remove --agent` 的假成功被修掉 |
| **5** | `[targets.xlings-gui]` + `ui/gui`;索引配方(仿 `godot.lua` 声明图形栈 deps) | GUI,三平台 |

**顺序的四条理由**:

1. **第 1 步是所有后续的地基。** 跳过它直接拆模块,就是把"26 个渲染结果 kind"这份债从两份复制成三份 —— 那正是今天 `--agent` 渲染器在做的事(18 个 kind 的第二份实现)。
2. **第 0 步先于第 1 步**,因为矩阵测试是第 1 步的**进度条**:表里每个 id 在每个前端都有实现才算迁完。没有它,第 1 步会在中途变成"两套并存"。
3. **第 2 步顺手清掉一份债**:已定方向 4 意味着 `agent/text_renderer` 并入 `ui/cli`,两个渲染器变一个,19-vs-18 的分裂不再需要靠测试发现。
4. **第 3 步的 `get_system_language()` 重写不能省。** 不重写的话 `auto` 永远返回 `en`,而"系统真是英文"和"探测失败"输出完全一样 —— 等于给这个功能内置一个永远发现不了的 bug。

第 5 步现在**不阻塞在任何东西上**:GUI 是独立二进制 + 独立配方,既不动发布矩阵,也不用等 capability 补齐(§3.6.5)。

## 七、风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| **第 1 步动 36 个 addField + 26 个 kind** | 面大,容易半途而废留下两套并存 | 用第 0 步的枚举表当进度条,矩阵测试卡住 |
| **三个 UI 模块重演 19-vs-18 分裂** | 从两份 if-else 变成三个包,分裂只会更难发现 | ① 已定方向 4 让 `--agent` 不再是独立渲染器,今天那份分裂**按构造消失**;② 矩阵测试 = `Document::id` 全集 × `{cli, tui, gui}`,缺一格失败 |
| **同进程 GUI 偷偷绕过协议** | 直接 `import xlings.core.config` / 调 `log::`,把"以后拆进程"变成"以后重写" | `ui/*` 的 `[dependencies]` 白名单:只许 `xlings.view` + `xlings.core.i18n`;CI 检查 mcpp.toml,声明式可查 |
| **`Document` 过度设计** | `Value` 变体太多会让三个前端都得实现一堆分支 | 起步只做 6 个变体;新增变体必须同时给出"哪个前端因为缺它而做不了什么" |
| **`:layout` 下沉要替掉 `ftxui::string_width`** | 显示宽度表要自己维护 | core 已有 `utf8.cppm`;一次性成本,换全部诊断进入宽度契约 |
| **NDJSON 协议已发布 v1.0** | 改 `DataEvent` 会破坏现有 agent/MCP 消费者 | `Document` 走**新** kind 前缀 `view.*`,老 kind 并存一个发布周期后再删;协议版本升 1.1 |
| **msgid 化后 NDJSON 不再是英文散文** | 现有消费者匹配的是英文文本 | NDJSON 同时带 `msgid` 和渲染后的 `en`,一个周期后去掉 `en` |
| **`xlings-gui` 的图形栈依赖很长** | 一条 runtime deps 链拉起整个 X11/GL 栈 | 这条链已经被 `godot.lua` / `griddycode.lua` 走通并踩过坑(启动期 `dlopen` 的 `libfreetype` 不在 `DT_NEEDED` 里之类);照抄,别重新发明 |
| **主题文件改不动东西** | 119:7 —— 94% 的取色是按颜色名而不是按角色(§3.8.1),不先做改名,主题就是个摆设 | 第 2.5 步的改名是纯重构(内置默认 = 今天取值,零视觉变化),配 grep 测试:`ui/` 里不得出现按颜色命名的取色函数 |
| **主题槽位失控** | 开放到任意键 / 按 `Document::id` 覆盖会让主题和 id 表耦合 | 槽位**封闭**在 9 个角色;未知键报 `Note` 而不是静默忽略;没有继承链 —— 缺省即回落到内置 `default` |
| **升级覆盖掉原地改的自带主题** | `config/themes/` 归 xlings,按版本标记覆盖(同 `config/shell/`) | 所有权靠"`theme` 指向哪里"而不是目录约定:用户主题放自己的路径。自带文件顶部写明"copy me, do not edit in place";因为 `default` 编译在二进制里、这些文件是可选的,被覆盖的后果远小于"用户唯一的主题没了" |
| **主题配错了静默回落** | 路径不存在 / JSON 坏 / 槽位拼错 → 悄悄用默认色,和"没配过"输出一样 | 三种情况都发 `Diagnostic` 并说明"现在用的是内置 default"(§3.8.5) |

---

## 八、已定方向与仍未决的问题

### 已定(2026-08-22)

1. **三个独立 mcpp 模块 `ui/cli` / `ui/tui` / `ui/gui`,先都同进程。** 拆进程留作后路,§3.6.6 的依赖白名单是保住这条后路的护栏。
2. **不做全屏 TUI。** 用 `ScreenInteractive::TerminalOutput()`;`select_option()` 现有的 `Fullscreen()` 要改或删。收益不只是省事 —— 它删掉了 alt-screen 进出、终端状态恢复、resize 重排、分页 四类问题,并且让 `| less` 成为可能(§3.3.1)。
3. **i18n 默认 `auto`(跟随系统),配置可覆盖,测试一律配 `en`。** 走 `xlings config --lang` 这条已有链路。**但 `get_system_language()` 必须重写**,否则 `auto` 永远返回 `en`(§3.4 实测)。
4. **`--agent` 是"尽量不阻塞"的标识,不是一种渲染;并且打印 msgid。** `ui/cli` 一个渲染器服务人和 agent —— 今天 19-vs-18 的分裂按构造消失。非交互下遇到 `Request` **报错并说明加哪个 flag**,不许猜(§3.5)。
5. **GUI 是独立二进制 `xlings-gui`,`--ui-mode gui` 不存在。** 它删掉的不是代码,是"去哪找它、版本对不对、找不到怎么降级"这一整类规则。分发就是一个 xlings 包,配方仿 `godot.lua`。
6. **`--ui-mode` 只分 `cli` / `tui`;主题与交互是配置。** `ndjson` 是 `interface` 子命令(另一个入口点),`gui` 是另一个二进制 —— 都不是渲染模式。
7. **主题先只做配色,独立成叶子模块 `xlings.theme`。** `default` 编译进二进制;主题文件是**部分覆盖**(没写的槽回落到 default,所以不需要 `extends`);`.xlings.json` 里存的是**路径引用**,自带的 `mono`/`high-contrast` 只是放在 `config/themes/` 下的可选文件。槽位封闭在 9 个角色,不按 `Document::id` 开放。
8. **`tui.interactive` 默认 `true`**(非 tty / `--agent` 自动关);首次弹部件时给一行提示,**仅第一次**,状态持久化在 `~/.xlings/.xlings.json` 的 `hintsSeen`(§3.8.6)。
9. **`Document::id` 表放 `core/view.cppm` 的编译期表** —— "加屏幕要动一处表"正是要建立的约束。
10. **自带主题就三个**:编译进去的 `default`(好看)+ `config/themes/mono.json`(可访问)+ `config/themes/high-contrast.json`(极端环境)。
11. **删掉 `config/i18n/{en,zh}.json`** —— 0.0.4 Lua 时代的遗留,内容还在描述 `xim`/`xvm`/`d2x`/`xself` 这些 0.4.8 就删掉的命令。留着会让人以为 i18n 有两套机制,而真正的表在 `src/core/i18n.cppm`。
12. **取舍按 架构 / 扩展性 / 多平台通用 / 稳定性。**

### 按方向 12 重定的:`Document` 第一批 id

我上一版建议"从 `package_info`(19 个 label,最富)开始"。**按这四条标准,这是错的** —— `package_info` 是最富的,但也是最善变的,而且它本身只是 `info_panel` 的一个使用者,拿它当架构探针会把一次性的字段争论和地基设计搅在一起。

按标准重排:

| id | 架构代表性 | 扩展性 | 多平台 | 稳定性 |
|---|---|---|---|---|
| **`info_panel`** | 通用容器,定死 `Field`/`Value`/`Emphasis` | **一次迁 5 个发出点** | 路径缩写在三平台不同 | 5 个使用者互相印证 |
| **`install_plan` + `install_summary`** | rows + actions 两种形态 | 所有 plan/summary 类的模板 | — | **e2e 覆盖最厚**,迁移可证伪 |
| **`download_progress`** | 唯一的流式形态 | 定死"进度是 Document 的一种" | Windows 控制台差异最大 | 迫使 `prevLines` 出协议(§1.6) |
| **`subos_created` + `subos_forked`** | 最小对照组 | — | — | **矩阵测试的第一个红格**,成本近零 |

先做这四组,`package_info` 放第二批 —— 那时 `Value` 的形状已经被四组不同形态压过一遍,19 个 label 只是填表。

### 仍未决

暂无 —— 上一轮的两条已在下面定案。开工前只剩两件**执行细节**,不需要拍板:

- **`config/i18n/` 的删除会带出一处测试清理。** `tests/e2e/github_owner_migration_audit.sh:52` 把 `config/i18n` 列在一条 `rg -q` 的路径清单里(断言 `xlings.d2learn.org` 仍然存在)。实测:该字符串在 `README.md`(4 处)、`README.zh.md`(4)、`.agents/skills/xlings-quickstart`(5)里都还在,所以**断言不会失败**;但 `rg` 对不存在的路径会往 stderr 打一行错误(实测 `exit=0` + `rg: nonexistent_dir: No such file or directory`)。删文件时顺手把这个路径从清单里去掉,否则 CI 会永久多一行噪音 —— 而"一条永远打印、永远无害的错误"正是让真错误被忽略的开端。
- **`theme_resources.cppm` 的形状照抄 `profile_resources.cppm`**,连同它的版本标记机制;区别只在写出去的目标是 `config/themes/` 而不是 `config/shell/`。
