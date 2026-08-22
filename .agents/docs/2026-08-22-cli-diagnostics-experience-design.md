# xlings 提示体验优化方案(诊断信息:分级 / 结构 / 文案 / 渲染)

> 日期: 2026-08-22
> 类型: 设计 (design)
> 范围: `src/core/log.*`、全部 `log::{error,warn,info,println}` 调用点、`EventStream`/`ErrorEvent` 渲染、`src/ui/layout.cppm`、`src/core/i18n.*`,以及 `use / install / remove / search / subos use / shim` 的实际输出
> 方法: 源码统计(HEAD `2f12f45`)+ 对已发布二进制 `2026.8.17.2` 的实测(`XLINGS_TERM_WIDTH=100`,终端 / 管道 / `--agent` / `-q` 四路对照)
> 前置文档: [`2026-07-29-tui-output-ux-survey.md`](2026-07-29-tui-output-ux-survey.md) —— 那一轮解决的是**渲染宽度**,本文解决的是**消息本身**

---

## TL;DR

一句话:**xlings 的日志系统只回答"这条有多严重",不回答"这条和上一条是什么关系"**。

调用点没有别的手段表达"这是上一条的细节/下一步",于是复制一条同级别日志。一个问题因此变成三条红色加粗 `[error]`——这正是用户报的症状:

```
[error] 'llvm' is not installed in current subos
[error]   globally available: 20.1.7 22.1.8
[error]   hint: xlings install llvm@<version> (or `xlings use llvm --all` to see global view)
```

三行里只有第一行是错误。第二行是事实,第三行是建议。它们被染成同一种红色,信息密度和可信度一起下降。

七个可测的结构性事实:

| # | 事实 | 测量 |
|---|---|---|
| 1 | error/warn 文案里有 **24% 其实是上一条的续行**,却各自带完整严重级标记 | 313 条中 74 条以空格开头 |
| 2 | `[warn]` **97% 的时候在说"顺便告诉你"**,不是"出问题了" | `.cpp` 里 123 个 warn 调用点,120 个所在路径继续执行或 return 0 |
| 3 | "这个包没装在当前 subos" 有 **9 个回答者、6 种措辞、3 种前缀、2 种严重级、2 种退出码** | 见 §3.3 |
| 4 | 宽度契约在 `ui` 层,而 **83% 的失败输出从 `core` 的 `log::` 出去**,core 不依赖 ui,够不着 | 318 个 log::error/warn vs 65 个 ErrorEvent;实测单行 **877 字符** |
| 5 | **约束来源不可见**:项目 `.xlings.json` 钉住的版本没装时,报错不提这个文件 | 见 §2.2 |
| 6 | `lang` 的配置链路**完整且有 e2e 覆盖**(CLI 选项/锁下写入/项目覆盖/回显/schema/对照表全都在),只有"接到输出上"的两行没写 | `grep -c 'i18n::tr'` = 0;`home_config_lock_test.sh:147` 已在测 `config --lang en` |
| 7 | `did you mean` 的实现存在(`edit_distance_`),但**只服务 5 类用户输入中的 1 类**,而且没有距离阈值 | `subos.cpp:116`;实测 `defualt` 会建议 `dev-hello` |

方案不是逐条改文案,而是补上缺的那一层:**在 `core` 里引入一个结构化诊断对象(summary / facts / actions / source),让渲染和分级都变成渲染器的职责**。仓库里已经有一个正确的小型原型——`xvm::XvmUserError` + `render()`(`src/core/xvm/errors.cppm`),它只覆盖 xvm 注册/移除/绑定三类错误。本方案就是把它提升为全局契约。

---

## 一、方法与证据

所有输出均为实测原样粘贴,环境 `XLINGS_TERM_WIDTH=100`,客户端 `2026.8.17.2`。

代码统计(HEAD `2f12f45`):

```
log::error(   调用点  195
log::warn(    调用点  124
log::info(    调用点   42
log::println( 调用点   76      ← 无标签的裸输出
stream.emit(ErrorEvent  65
i18n::tr / trf 调用点     0
```

(控制流分析只覆盖 `.cpp`:191 个 error + 123 个 warn。)

文案字面量统计(313 条 error/warn):

```
以两个及以上空格开头(=续行)   74   (24%)
自带 [xxx] 前缀                62   (20%)   —— 8 种不同前缀
首字母大写                      6   (2%)
以句号结尾                      5   (2%)
"hint:" 引导                   25
其他引导词(path/available/tip/then/run/payload/move/drop) 12
```

---

## 二、用户报的两个例子

### 2.1 `xlings use llvm`

```
$ xlings use llvm ; echo $?
[error] 'llvm' is not installed in current subos
[error]   globally available: 20.1.7 22.1.8
[error]   hint: xlings install llvm@<version> (or `xlings use llvm --all` to see global view)
1
```

代码: `src/core/xvm/commands.cpp:682-694`(`collect_version_candidates_`)。

**用户的判断("这应该是警告")对了一半。** 命令确实失败了——什么都没切换,退出码 1,脚本必须能看到失败;把整块降级成 `[warn]` 会破坏"严重级 ↔ 退出码"这个当前基本成立的对应关系(实测 `.cpp` 里 191 个 error 调用点只有 1 个后接 `return 0`)。

真正错的是**结构**:三行里只有第一行是"错误",后两行是它的证据和出路,却被复制了三次红色加粗标记。用户感知到的"不友好"来自这个,而不是来自级别本身。

同一命令的另一条失败路径还带前缀,措辞也不同(`src/core/xvm/commands.cpp:374`):

```
[error] [xlings:use] 'llvm' is not installed in this subos (default)
[error]   nothing was changed
[error]   hint: install it here first with `xlings install llvm@20.1.7`
```

一个命令、一类失败、两种措辞("current subos" / "this subos (scope)")、两种前缀(无 / `[xlings:use]`)、一个有 "nothing was changed" 一个没有。

### 2.2 项目 `.xlings.json` 钉住未安装的版本

复现(scratchpad 里新建项目,不动真实 home):

```
$ cat .xlings.json
{ "workspace": { "mcpp": "2026.99.9.9" } }

$ mcpp --version ; echo $?
[error] xlings: version '2026.99.9.9' not found for 'mcpp'
[error]   available: 0.0.100 0.0.101 0.0.102 0.0.103 0.0.106 0.0.109 0.0.24 0.0.25 0.0.26 0.0.27 …(实测 877 字符,94 个版本)…
1
```

代码: `src/core/xvm/shim.cpp:456-463`。

这条比例 1 更严重,三处独立缺陷叠加:

1. **不说约束从哪来。** 用户没有在任何地方输入过 `2026.99.9.9`,它来自 `<project>/.xlings.json` 的 `workspace.mcpp`。消息完全不提这个文件。用户唯一能做的是猜。
2. **没有出路。** 这条路径**一条 hint 都没有**——不说 `xlings install mcpp@2026.99.9.9`,也不说"改这个文件"。
3. **877 字符一行,且顺序错误。** `get_all_versions()`(`src/core/xvm/db.cpp:340`)直接遍历 `std::map`,返回字典序:`0.0.100, 0.0.101, …, 0.0.24, …, 2026.8.21.1, 2026.8.3.2`。仓库里有 `version_order::sort_desc`,这里没用。既不排序、不截断、不换行,还混进了 `local:` 前缀的本地构建。

同一份未排序未截断的列表在 `xlings use mcpp` 里再出现一次(`commands.cpp:689`)。

---

## 三、缺陷清单

按影响排序。每条给出:证据 → 根因 → 影响。

### D1 — 续行复用严重级标记(最直接的"不友好"来源)

**证据**:313 条文案里 74 条以空格开头,是上一条的续行;每条各自触发一次 `emit_line_`,因此各自带 `[error]`/`[warn]`。

**根因**:`log::` 的四个级别全部回答"多严重",没有任何一个回答"这是上一条的延续"。调用点唯一能用的手段就是再打一条同级日志 + 手工缩进。

**讽刺的是机制已经存在**:`emit_line_`(`src/core/log.cpp:167-174`)会把消息里的 `\n` 后续行按标签宽度对齐缩进——注释里明确写了这是为多行消息设计的。全树 `log::error/warn/info` 里**含 `\n` 的调用点是 0 个**。唯一按这个思路写的是 `xvm::render()`,而它自己又硬编码了 10 空格缩进,和 `emit_line_` 的 8 列缩进叠加成 18 列。

**影响**:红色噪音 ×3;用户学会跳过 `[error]` 行,真正的错误因此更难被读到。

### D2 — `[warn]` 语义过载

**证据**:`.cpp` 里 123 个 warn 调用点中,只有 3 个所在路径返回非 0;120 个继续执行或返回 0。

典型:一次**成功**的 `xlings use` 会打

```
[warn] 12 name(s) not in llvm@20.1.7 still run from 22.1.8 — -v to list
```

(`src/core/xvm/commands.cpp:607`)。命令成功了,退出码 0,什么都没坏——这是一条**报告**,却用琥珀色 `[warn]` 呈现。同理 `xim/commands.cpp:917`(remove 只删活跃版本的说明)。

**根因**:级别表里缺一档"值得看,但不是问题"。`info` 太安静(和索引拉取刷屏混在一起),`warn` 太吵。

**影响**:用户被训练成忽略 `[warn]`,于是真正的降级警告(`subos.cpp:871` 的 `--runtime ignored`、`entry_binary.cpp:92` 的 entry binary 降级)也被忽略。

### D3 — 一个状态,九个回答者

"这个包没装在当前 subos" 的全部回答者:

| 位置 | 措辞 | 前缀 | 级别 | 退出码 |
|---|---|---|---|---|
| `xvm/commands.cpp:374` | `'{}' is not installed in this subos ({})` | `[xlings:use]` | error | 1 |
| `xvm/commands.cpp:682` | `'{}' is not installed in current subos` | 无 | error | 1 |
| `xvm/shim.cpp:434` | `xlings: '{}' is not installed` | `xlings:` | error | 1 |
| `xvm/shim.cpp:437` | `xlings: '{}' is not installed in current subos` | `xlings:` | error | 1 |
| `xvm/shim.cpp:168` | `shim '{}' is not installed in its owning home{}` | 无 | warn | 继续 |
| `xim/commands.cpp:763` | `xlings: '{}' is not installed in current subos '{}'` | `xlings:` | warn | **0** |
| `xim/commands.cpp:871` | 同上 | `xlings:` | warn | **0** |
| `xim/commands.cpp:981` | `{}@{} is not installed` | 无 | warn | 继续 |
| `xim/commands.cpp:1690` | `{} is not installed — run: xlings install {}` | 无 | warn | 继续 |

6 种措辞、3 种前缀、2 种级别、2 种退出码。用户无法从措辞判断自己处在哪种情况,也无法 grep。

这是 xlings 反复出现的"一个问题多个回答者"形态(参见 `2026-08-17-547-and-three-merged-prs-review.md` 把 subos runtime 的六个写入者收敛成一个),这次落在文案层。

### D4 — 宽度契约够不着 `core`

**证据**:`ui::layout` 定义了完整的宽度/纯文本契约(2026-07-29 那轮的成果),但 `cli.cpp:731` 的注释自己承认:

> `log::` itself cannot do this: core does not depend on ui.

于是只有 65 个 `ErrorEvent` 走了折行;319 个 `log::error/warn` 直接出去,不折行、不截断。实测最长一行 **877 字符**。

而且**折行本身又踩了 D1**:`cli.cpp:735-742` 把折出来的每一行都用一次 `log::error` 发出去,于是一条消息折成三行 = 三个 `[error]`。实测:

```
[error] package 'zzznope' not found in the synced index (xim@artifact:a4416a6, scode@78fe9f0,
[error] dsh@f5b8a17, +3 more), synced 0 seconds ago
[error] the index is current, so this name is either wrong or not published yet
[error]   searched repos: [xim, scode, dsh]; run `xlings update` if the package was just published
```

第二行是第一行的后半句,却成了一条独立的"错误"。

### D5 — 约束来源不可见

除了 §2.2 的项目钉版本,同类问题还有:

- `subos` 声明的 runtime 与请求冲突时(`xvm/commands.cpp:254`)会说 "this SubOS declares X",但不说这条声明写在哪个文件里。
- 依赖范围(`xim:glibc@>=2.39`)解析失败时,不说是谁声明的这个范围。

**根因**:消息由"发现问题的那一层"拼装,而"约束是谁下的"这个信息在更上层,没有被传下去。

**影响**:用户拿到一个自己从没输入过的版本号/名字,唯一的排查手段是全盘 grep。

### D6 — 候选列表:不排序、不截断、不分组

`get_all_versions()` 返回 `std::map` 的字典序。三处直接拼接输出:`shim.cpp:459`、`xvm/commands.cpp:689`、`xvm/commands.cpp:348`。

后果:94 个版本一行;`0.0.100` 排在 `0.0.24` 前面;`local:` 构建和索引版本混在一起。

仓库里 `version_order::sort_desc` 已被 `xim/commands.cpp:918` 正确使用——同一个仓库里,同一件事有对有错。

### D7 — 前缀无规矩,且存在双前缀

8 种自定义前缀:`[xlings:self]`(19)、`[index]`(15)、`[xim]`(9)、`[xvm]`(8)、`[mirror]`(6)、`[xlings:use]`(5)、`[xlings]`(4)、`[xlings:profile]`(1),外加裸 `xlings:` 形式。251 条没有前缀。

其中 `[xlings]` 是**双前缀**:`log::info("[xlings] entry binary …")`(`entry_binary.cpp:98`)渲染成

```
[xlings] [xlings] entry binary 2026.8.17.1 -> 2026.8.17.2 (xim:xlings)
```

实测索引拉取同理:`[xlings] [index] fetching package index (mirror=CN)...`。

### D8 — `--agent` / `-q` 对 83% 的失败输出无效

`--agent` 承诺 "Plain-text output for LLM agents (no TUI/ANSI)",它替换的是 EventStream 的消费者(`Error:` / `Hint:` 前缀)。但 `log::` 直接调用绕过它。实测三种模式输出**逐字节相同**:

```
$ xlings use llvm            → [error] 'llvm' is not installed in current subos …
$ xlings use llvm --agent    → 完全相同
$ xlings use llvm -q         → 完全相同
```

`-q` 把级别设到 `Error`,而 error 本来就不受级别控制(`log.cppm:94`,`error()` 里没有级别判断),所以 `-q` 对失败输出是空操作。

### D9 — i18n:整条链路都通了,缺中间两行

> 修订(2026-08-22):初稿写的是"i18n 是死代码",不准确。实情更值得注意 —— 这条链路**修好了、还有 e2e 覆盖**,只是没接到输出上。

| 环节 | 状态 | 证据 |
|---|---|---|
| `xlings config --lang` CLI 选项 | ✅ | `cli.cpp:1193` |
| 锁下写入 `~/.xlings.json` | ✅ | `cli.cpp:509-513`,走 `update_home_config` |
| 全局读 / 项目覆盖 | ✅ | `config.cpp:489-490` / `:592` |
| `xlings config` 回显 | ✅ | `cli.cpp:598` |
| schema 文档 | ✅ | `docs/spec/xlings-json-schema.md:24`「界面语言」 |
| **e2e 断言** | ✅ | `tests/e2e/home_config_lock_test.sh:147-152` 跑 `config --lang en` 并断言落盘 |
| 中英对照表(30 条)、`tr/trf`、`auto` 回退 | ✅ | `i18n.cppm` / `i18n.cpp:16` |
| **`set_language(Config::lang())`** | ❌ 无人调用 | — |
| **调用点用 `tr()`** | ❌ 0 处 | — |

也就是说:`lang` 和 `mirror` 走的是**同一条已经修好的路**,只有最后两步没接。这不是"要设计一个 i18n 方案",是接两行线 + 把文案迁成 msgid。

另有一个实现层面的硬阻碍:`platform::get_system_language()` 用 `std::locale("")`,实测在 musl 静态(**发布形态**)和本机 glibc 上**都抛异常**,被 `catch` 成 `return "en"`。于是 `auto` 永远只能得到 `en`,而且"系统真是英文"和"探测彻底失败"输出完全一样。详见多前端文档 §3.4。

**另有第三套遗留**:`config/i18n/{en,zh}.json` 是 0.0.4 Lua 时代的文件,内容还在描述 `xim`/`xvm`/`d2x`/`xself` 这些 0.4.8 就删掉的命令,全树唯一引用是一个域名审计测试在 grep 它。**已定:删掉** —— 留着会让人以为 i18n 有两套机制。

### D10 — `did you mean` 只覆盖 1/5 的用户输入,且无距离阈值

`edit_distance_` + `suggestions_`(`src/core/subos.cpp:116-158`)实现完整,只用于 subos 名。

用户会打错的名字有五类,覆盖情况:

| 输入 | 有建议? | 实测 |
|---|---|---|
| subos 名 | ✅ | `subos use defualt` → `did you mean: default, dev-hello, gfxbuild` |
| 子命令 | ❌ | `xlings instal gcc` → `[error] unknown command: instal`(无建议) |
| 包名 | ❌ | `xlings install zzznope` → 刷新索引 10.8s 后报"没找到" |
| 版本号 | ❌ | §2.2 |
| 索引源名 | ❌ | `no index source named '{}'`(`index_cmd.cpp:99/168/217`,三处相同措辞) |

而且唯一有建议的那一类**没有距离阈值**:`suggestions_` 无条件取排序后的前 3 个。实测编辑距离 `defualt→default` = 2、`defualt→dev-hello` = 6、`defualt→gfxbuild` = 6,后两个照样被列为"did you mean"。建议不可信,用户就不会再读建议。

### D11 — 打错包名要等 10.8 秒,且进度转义泄漏到管道

```
$ time xlings install zzznope3
real 0m10.809s
```

索引刷新本身是设计决定([`2026-07-14-issue366-install-index-refresh-design.md`](2026-07-14-issue366-install-index-refresh-design.md)),不推翻。问题是它期间的输出:

```
$ xlings install zzznope 2>/dev/null | cat -v
^M[1/7] awesome::xim.lua^[[K^M[2/7] awesome::template.lua^[[K…
```

`\r` + `\e[K` 写进管道。`ui::layout::render_to_string` 明确把 `erase_eol` 关联到 `stdout_is_terminal()`,但**这个写入者不在 `src/` 里**(全树没有任何 `\r` 字面量输出;它来自索引侧/被 vendored 的构建脚本)。也就是说存在**第五条输出通道,`src/` 里的任何契约都够不着它**。

同类:`ui/progress.cpp:367,370` 的 `\033[<N>A\r` / `\033[J` 也没有 tty 判定。

### D12 — 答案被埋在面板下面

`xlings subos use defualt` 先打印完整 subos 列表面板,再在最后打错误。`xlings use <name>`(多版本)先打面板,不打下一步。

面板和诊断是两个不同的东西,现在共用一条输出顺序,没人决定谁在前。

---

## 四、方案:补上缺失的那一层

三层,自下而上。**L1 是全部价值所在**,L2/L3 是把它落到实处。

### L1 — `core` 里的结构化诊断对象

新增 `src/core/diag.cppm`(放 `core`,因为 90% 的用户可见文本从 core 出去):

```cpp
export namespace xlings::diag {

enum class Level { Note, Warn, Error };   // ← Note 是新的一档

struct Fact   { std::string label; std::string value; };
struct Action { std::string label; std::string command; };  // command 可直接复制执行

struct Diagnostic {
    Level              level;
    std::string        code;      // 稳定、可搜索、可文档化:如 "xvm-not-in-subos"
    std::string        summary;   // 一句话,必须能独立成立
    std::string        source;    // 约束从哪来:文件路径 / subos 名 / 索引快照   ← D5
    std::vector<Fact>   facts;    // 证据(已装版本、已搜索的仓库…)
    std::vector<Action> actions;  // 出路,至少一条
    bool               nothingChanged { false };  // 是承诺,不是装饰
};

void emit(const Diagnostic&);     // 唯一出口

}  // namespace xlings::diag
```

约束(写进头注释,由测试守住):

1. **一个 Diagnostic 渲染成一条日志**——一个严重级标记,续行靠缩进。直接消灭 D1。
2. **`summary` 不含前缀**,前缀由渲染器按命令上下文统一加(或者干脆不加)。消灭 D7。
3. **`actions` 非空**,否则测试失败——这正是 `xvm::errors.cppm` 已经写下的规矩("No hint means the error is not ready to be shown to a user"),把它推广到全局。
4. **`code` 稳定且可搜索**,进 `docs/spec/diagnostics.md`,允许用户和 agent 按码检索。
5. **`level` 与退出码强绑定**:`Error` ⇔ 非 0,`Note`/`Warn` ⇔ 0。用 e2e 守住(现状已 96% 满足,只需固化)。

`xvm::XvmUserError` 是它的直接前身,迁移时删掉,`describe()` 改为返回 `Diagnostic`。

### L2 — 一个渲染器,四种模式

渲染器放 `cli` 层(可以依赖 `ui::layout`),`core` 只发数据。这样 D4 自动消失——宽度契约第一次能作用到全部诊断。

| 模式 | 触发 | 形态 |
|---|---|---|
| Rich | 交互终端 | 彩色标记 + 对齐的 facts/actions 两列 |
| Plain | 管道 / `NO_COLOR` | 同结构,无 SGR、无 CR、无行尾空格 |
| Agent | `--agent` | `Error: …` / `Source: …` / `Fact: …` / `Action: …` 逐行,LLM 友好 |
| NDJSON | `xlings interface` | 直接映射到 `ErrorEvent`,`code` 复用为 wire code |

同一份数据,四种渲染。D8 因此变成真的。

**Rich 模式下例 1 的目标形态**:

```
[error] llvm is not installed in this subos (default)
        installed elsewhere   22.1.8, 20.1.7
        install it here       xlings install llvm@22.1.8
        see every subos       xlings use llvm --all
```

四行变一个标记;版本降序;动作在右侧对齐、可直接复制。

**例 2 的目标形态**(D5 的核心价值):

```
[error] mcpp@2026.99.9.9 is pinned here, and no such version is installed
        pinned by             ./.xlings.json  →  workspace.mcpp
        installed here        2026.8.21.1 (active), 2026.8.18.3, 2026.8.18.2, +78 more
        install the pin       xlings install mcpp@2026.99.9.9
        list all versions     xlings list mcpp
```

用户第一次能看到那个陌生版本号是谁写的。

### L3 — 写下来的文案规约 + 机器守卫

新增 `docs/spec/diagnostics.md`,把下面这些从"惯例"变成"契约":

1. **summary 是一个陈述句**:小写开头、不加句号、不超过 ~72 列、先说事实再说影响。
2. **动作用祈使式命令**,不用 "you should" / "please"。
3. **一个状态一种措辞**:`is not installed in this subos ({scope})` 只写一次(D3)。
4. **候选列表统一经过 `render_candidates()`**:`version_order::sort_desc` → 取前 N → `+M more` + 指向完整列表的命令(D6)。
5. **`nothingChanged` 只在真的没写过状态时置位。**
6. **前缀白名单**:模块前缀要么废除,要么收进渲染器,调用点不许自己拼(D7)。

守卫(否则规约会在三个 PR 内失效):

- **单测**:遍历一张编译期诊断表,断言每条都有非空 `actions`、`summary` 无 `[` 开头、无尾句号、长度 ≤ 72。
- **grep 测试**:`src/` 里除 `diag.cpp` 外不得出现 `log::error("  ` / `log::warn("  `(以空格开头的续行)。这一条直接防 D1 回归。
- **e2e**:对 §2 两个场景断言"恰好一个 `[error]` 标记";四种模式各跑一遍断言结构一致。

### L4 — 把已有但没接线的能力接上

不写新算法,只是接线:

| 已有 | 现状 | 接到 |
|---|---|---|
| `subos.cpp:116` `edit_distance_` | 只服务 subos 名 | 子命令、包名、版本号、索引源名(D10);同时补距离阈值 `≤ max(2, len/3)` |
| `version_order::sort_desc` | 只有 1/4 的候选列表用了 | 全部候选列表(D6) |
| `i18n` 对照表 + `xlings config --lang` 整条链路 | 只差 `set_language()` + `tr()` 两处接线 | 前端按 `code` 查表;先覆盖 top-40 诊断(D9) |
| `ui/selector.cppm` `select_version()` | 零调用者(2026-07-29 P1-18 至今未接) | `use <name>` 多候选时交互选择(D12) |

**关于 i18n**:建议把 `Msg` 枚举换成 `Diagnostic::code` 字符串键,英文文案留在代码里作为 fallback,中文放独立表。这样新增诊断不必同时改两处,缺翻译时退化成英文而不是编译失败。

---

## 五、分批落地

四批,每批可独立发布、独立回滚。

### 批 1 —— 机制(不改任何文案)

- 新增 `src/core/diag.cppm` + 渲染器,`log::` 保留不动。
- 修 `cli.cpp:731-743`:把折行结果合成**一条**带 `\n` 的消息再发,而不是逐行 `log::error`。
- 修 `xvm::render()` 的硬编码 10 空格缩进(与 `emit_line_` 的 8 列叠加)。
- 修双前缀(`entry_binary.cpp`、`indexfetch.cpp` 等 4 处 `[xlings]`)。

**可见收益**:D1 的一半、D4 的折行部分、D7 的双前缀。风险最低。

### 批 2 —— 迁移高频路径(用户天天撞的)

`use` / `install` / `remove` / shim dispatch,约 60 个调用点。同时落地:

- D3:统一 "not installed in this subos" 为**一个** helper。
- D5:`Diagnostic::source` 打通——项目 `.xlings.json` 钉版本、subos runtime 声明。
- D6:候选列表统一渲染。
- D2:把 `use` 的 stranded 报告、`remove` 的多版本说明改成 `Note`。

**可见收益**:用户报的两个例子彻底解决。

### 批 3 —— did-you-mean + 模式化渲染

- D10:`edit_distance_` 提到 `core/textmatch.cppm`,接 4 类输入 + 距离阈值。
- D8:Agent / Plain / NDJSON 三种渲染真正生效。
- D11:定位 `\r\e[K` 的写入者(不在 `src/` 内,需先查 index 侧与 vendored 依赖),补 tty 判定;顺手给 `ui/progress.cpp:367,370` 补上。

### 批 4 —— i18n 接线 + 剩余调用点

- D9:`lang` 生效(两行接线 + 文案迁 msgid),top-40 诊断中文化;**同时重写 `platform::get_system_language()`**(改读 `LC_ALL`/`LC_MESSAGES`/`LANG`,不用 `std::locale`),否则 `auto` 永远只会得到 `en`。
- 剩余约 190 个 `log::error/warn` 调用点迁移或标注为"内部诊断,不面向用户"。
- `docs/spec/diagnostics.md` 定稿 + 码表。

---

## 六、验收

| 编号 | 断言 | 手段 |
|---|---|---|
| A1 | §2 两个场景各只产生 **1 个** `[error]` 标记 | e2e |
| A2 | 任何诊断行 ≤ `XLINGS_TERM_WIDTH` | e2e(40/60/100/200 列 × 8 条命令) |
| A3 | 每条诊断至少一个 action | 单测遍历诊断表 |
| A4 | `Error` ⇔ 退出码非 0;`Note`/`Warn` ⇔ 0 | e2e 矩阵 |
| A5 | 四种模式结构等价(同样的 summary/facts/actions) | e2e 对四路输出做结构比对 |
| A6 | 钉版本失败的消息包含 `.xlings.json` 字样 | e2e |
| A7 | `src/` 内无以空格开头的 `log::error/warn` 续行 | grep 测试 |
| A8 | 候选列表降序且截断 | 单测 |
| A9 | 距离超阈值的名字不出现在建议里 | 单测(`defualt` 不得建议 `dev-hello`) |

**回归护栏必须先对旧二进制失败**——2026-07-29 那轮记录过一个教训:检查工具本身静默通过。A1/A2/A6/A7 都要先对 `2026.8.17.2` 跑一遍并确认失败。

---

## 七、明确不做

- **不把 `use` 的失败降级成 `[warn]`。** 命令确实失败了,退出码 1,脚本要能看见。用户感知到的问题是结构不是级别,§4 已经解决。
- **不推翻 `install` 打错名字时刷新索引**(设计决定),只改它的输出与耗时可见性。
- **不重写 `ui/` 的面板渲染**,2026-07-29 那轮已经收敛,本方案只补 `core` 侧的诊断通道。
- **不引入外部 i18n 依赖**,现有对照表机制够用。

---

## 附:一页速查(改前 / 改后)

```
改前                                    改后
────────────────────────────────────────────────────────────────
[error] 'llvm' is not installed …       [error] llvm is not installed in this subos (default)
[error]   globally available: 20.1.7…           installed elsewhere   22.1.8, 20.1.7
[error]   hint: xlings install llvm@…           install it here       xlings install llvm@22.1.8
                                                see every subos       xlings use llvm --all

3 个红色标记                             1 个红色标记
版本字典序                               版本降序
hint 挤成一行 92 字符                    动作两列对齐、可复制
不说来源                                 来源成为一等字段
```
