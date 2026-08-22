# 实施计划:诊断体验 + 多前端地基(单 PR)

> 日期: 2026-08-22
> 类型: 实施计划 (plan)
> 设计依据: [`2026-08-22-cli-diagnostics-experience-design.md`](2026-08-22-cli-diagnostics-experience-design.md) · [`2026-08-22-multi-frontend-architecture-design.md`](2026-08-22-multi-frontend-architecture-design.md)
> 分支: `feat/diagnostics-frontend-arch`
> 目标版本: `2026.8.22.1`

## 范围裁剪(按已定优先级)

| | 本 PR 做到 |
|---|---|
| **cli / tui / 用户提示** | **做完**:诊断模型、结构化输出、模式与配置、主题、行内交互 |
| **gui** | **基础框架 / 一般可用**:独立 target + 能开窗 + 能跑一条命令并渲染 Document/Diagnostic。细节设计后议 |
| `Document` 全量迁移 | **不做**。只迁第一批 id(见 T6),其余留在旧 `DataEvent` 路径,双轨并存 |
| 拆 `core/` 成 workspace 成员 | **不做**(Phase B),GUI 用同包第二 target |

裁剪原则:**能被"接线"完成的先做,需要"重写"的留后**。仓库里大量抽象已经存在却零调用,这一轮的价值主要在把它们接上,而不是造新的。

---

## 任务依赖图

```
        ┌──────────────────────────────────────────────┐
        │ T0  core/diag.cppm — Diagnostic 模型 + emit  │  ← 地基,无依赖
        └───┬──────────────────────────────────────────┘
            │
   ┌────────┼───────────┬──────────────┬──────────────┐
   ▼        ▼           ▼              ▼              ▼
  T2       T3          T7             T9            T15
折行单条  xvm::render  统一"未装于     did-you-mean  Request 类型化
(dep T1)  迁 Diag      本 subos"      (4 类输入)    + 非交互报错
                          │                              │
                          ▼                              ▼
                         T8  约束来源(项目钉版本)      T16 select_version 接线
                                                          + 首次提示

  ── 以下与 T0 并行,互不依赖 ──────────────────────────────
  T1  log:: Sink 雏形 + 单条多行
  T4  双前缀修正(4 处 [xlings] / [index])
  T5  unhandled DataEvent debug→warn + 9 个孤儿
  T6  候选列表:sort_desc + 截断 + Document 第一批 id
  T10 UiMode{Cli,Tui} + UiCapabilities + is_tui_mode 改名
  T12 i18n 接线 + get_system_language 重写 + 删 config/i18n

  T11 config --ui-mode/--theme/--interactive/--background   (dep T10)
  T13 xlings.theme 模块 + 9 角色 + 119 处改名                (dep T10)
  T14 config/themes provisioning + mono/high-contrast        (dep T13)
  T17 gui 骨架 [targets.xlings-gui]                          (dep T0,T10)

  T18 测试(单测 + e2e)          (dep 全部)
  T19 文档/规范                   (dep 全部)
```

**关键路径**:`T0 → T7 → T8`(诊断模型 → 统一措辞 → 约束来源)。其余可并行。

**先做 T5 的理由**:它把 9 个孤儿从 `log::debug` 提到 `warn`,是后续所有迁移的**进度条** —— 没有它,迁到一半"哪些屏幕还没接"是不可见的。

---

## 任务清单

### T0 · `core/diag.cppm` — 诊断模型
- `Level{Note,Warn,Error}`、`Diagnostic{level,code,summary,source,facts[],actions[],nothingChanged}`
- `emit(const Diagnostic&)` → 组成**一条**带 `\n` 的消息,交给 `log::`(`emit_line_` 已按标签宽度缩进续行)
- 不变量(单测守):`actions` 非空、`summary` 无 `[` 前缀、无尾句号
- 替换 `xvm::XvmUserError`(T3)

### T1 · `log::` Sink 雏形
- `log::set_sink()`,默认 TerminalSink;`is_tui_mode()` 的静音改成 NullSink
- 保留 `emit_line_` 的续行缩进(已正确)

### T2 · ErrorEvent 折行改单条
- `cli.cpp:731-743`:`wrap_to_width` 结果 `join("\n")` 后**一次** `log::error`

### T3 · `xvm::render()` → `Diagnostic`
- 删掉硬编码的 10 空格缩进(与 `emit_line_` 的 8 列叠加成 18)

### T4 · 双前缀
- `entry_binary.cpp:83/92/98`、`indexfetch.cpp`、`mirror/registry.cpp`:去掉消息里自带的 `[xlings]`

### T5 · 孤儿 DataEvent
- `cli.cpp:263` `log::debug` → `log::warn`
- 补 4 个 CLI 路径孤儿:`subos_forked`(**成功后零输出**)、`remove_blocked`、`update_plan`、`update_summary`
- 删 2 个死渲染器(`search_results`/`table` 无 emit 点)或补 emit

### T6 · 候选列表 + `Document` 第一批
- `shim.cpp:459`、`xvm/commands.cpp:689/348`:`version_order::sort_desc` + 取前 N + `+M more`
- 第一批 id:`info_panel`(5 个发出点)、`install_plan`/`install_summary`、`download_progress`(顺带把 `prevLines` 移出协议)、`subos_created`/`subos_forked`

### T7 · 统一"未装于本 subos"
- 9 个回答者 → 1 个 helper,措辞/前缀/级别/退出码各一种

### T8 · 约束来源
- `Diagnostic::source`:项目 `.xlings.json` 钉版本、subos runtime 声明

### T9 · did-you-mean
- `subos.cpp:116` 的 `edit_distance_` 提到 `core/textmatch.cppm`
- 接:子命令、包名、版本号、索引源名;**补距离阈值** `≤ max(2, len/3)`

### T10 · UiMode
- `enum UiMode{Cli,Tui}` + `UiCapabilities{color,cursorRewrite,interactive,localized,width}`
- `platform::is_tui_mode()` → `is_headless_output()`
- 收敛八个旋钮

### T11 · config 面
- `config --ui-mode|--theme|--interactive|--background`,写 `~/.xlings.json` 平铺键
- 优先级:flag > 项目 > 全局 > auto

### T12 · i18n
- `i18n::set_language(Config::lang())` 接线;`tr()` 用于第一批诊断
- **重写 `platform::get_system_language()`**:读 `LC_ALL > LC_MESSAGES > LANG`(POSIX)/ `GetUserDefaultLocaleName`(Windows),不用 `std::locale`
- 删 `config/i18n/{en,zh}.json` + 清理 `github_owner_migration_audit.sh:52` 的路径清单

### T13 · `xlings.theme`
- 叶子模块;9 个角色槽 `accent/alt/success/warn/error/text/muted/border/surface`
- **119 处按颜色取色 → 按角色取色**(内置 default = 今天取值,零视觉变化)
- grep 测试:`ui/` 不得再出现按颜色命名的取色函数

### T14 · 主题文件
- `theme_resources.cppm`(照抄 `profile_resources.cppm` 的版本标记机制)
- 写 `config/themes/{mono,high-contrast}.json`;`theme` 存**路径引用**,解析复用 `config.cpp:929-931` 的 base 规则
- 部分覆盖(缺省回落内置 default),无 `extends`
- 三种失败(路径不存在/JSON 坏/槽位拼错)都发 `Diagnostic`,不静默

### T15 · Request 类型化
- `Confirm/PickOne/FreeText`;**非交互遇到 Request → `Diagnostic{Error}` + 非 0**,不猜
- 修掉 `remove --agent` 打印 `cancelled` 且退出 0 的假成功

### T16 · 行内交互
- `select_version()` 接线;`select_option()` 的 `Fullscreen()` → `TerminalOutput()`
- 首次提示一次,`hintsSeen` 持久化在 `~/.xlings/.xlings.json`

### T17 · GUI 骨架
- `[targets.xlings-gui]` + `required_features=["gui"]` + `[feature-deps.gui] mcpplibs.imgui`
- `ui/gui`:能开窗、渲染 Document/Diagnostic、跑一条命令
- 默认不开;`mcpp build --features gui` 才产出

### T18 · 测试
- 单测:diag 不变量、textmatch 阈值、theme 部分覆盖、`get_system_language` 读 env
- e2e:两个场景各只 1 个 `[error]`;四路模式结构等价;宽度矩阵;`config --lang/--theme` 落盘

### T19 · 文档
- `docs/spec/diagnostics.md`(码表)、`xlings-json-schema.md` 补新键、AGENTS.md 结构、CHANGELOG

---

## 多角度检查表(合并前逐条过)

| 角度 | 检查 |
|---|---|
| **架构** | core 不依赖 ui;`ui/*` 依赖白名单;`Document::id` 表是唯一注册处 |
| **稳定性** | 每个新观察点有自己的测试;护栏先对旧二进制跑失败 |
| **优雅简洁** | 一个 Diagnostic 一个标记;无重复渲染器;删掉的代码 > 新增的接线 |
| **用户体验** | 两个原始场景解决;降级必说;action 可复制可执行 |
| **兼容性** | `--agent` / `xlings interface` / `XLINGS_THEME` 保留为别名;老 `DataEvent` kind 并存一个周期 |
| **跨平台** | Windows 无 `LANG`(用 `GetUserDefaultLocaleName`);路径分隔;`\r` 不进管道;三平台 CI 绿 |
| **一致性** | 措辞一处;前缀白名单;候选列表一个渲染器 |
| **无感升级** | 内置 default 主题 = 今天取值(零视觉变化);新配置键缺省即旧行为;`config/themes` 首次运行自动 provision |

---

## 发布与验证

1. 版本 `2026.8.22.1`(`mcpp.toml` + `Info::VERSION`)
2. CI 全绿(linux / linux-e2e / linux-root / archlinux / macos / windows / aarch64)
3. 自我 review 一轮
4. release + 本地 `gtc` 补 GitCode 资源
5. 真实验证:`xlings subos <name> --sandbox --cmd "..."` 覆盖 use / install / remove / config / 主题 / 交互 / agent 模式

---

## 实施记录(2026.8.22.1 / PR #556)

**落地**:T0 T1(部分) T2 T3 T4 T5 T6(部分) T7 T8 T10 T11 T12 T13 T14 T15 T16 T17 T18 T19

**未做,及原因**:

| | 状态 | 为什么 |
|---|---|---|
| **T1** `log::` Sink | 只做了单条多行;`set_sink()` 未落地 | GUI 走独立二进制,不再急需;core 仍直写 FILE* |
| **T6** `View::Document` | 只立了 `diag` 地基,26 个 kind 仍走旧 `DataEvent` | 双轨并存;迁移是下一轮的主体 |
| **T9** did-you-mean 推广 | 只在 theme 槽位上做了(含距离阈值) | `edit_distance_` 尚未提到 `core/textmatch`;包名/子命令/版本号仍无建议 |
| **T12** msgid 化 | 只接了线(`set_language` + `auto` 修复) | 文案未迁到 `tr()`;`--agent` 未打印 msgid |
| `ui/cli` `ui/tui` 拆成成员 | 未做 | 需要先把 `core/` 抽成成员(Phase B);`theme` 和 `gui` 已经是成员 |
| E2E 覆盖"拒绝确认" | 未做 | 需要可安装的 fixture;单测覆盖了,e2e 文件里写明了为什么不在那儿 |

**两处推翻了原计划**(都是跑既有测试发现的,不是读代码):

1. **T16 的交互默认值**:计划写"默认 true",E2E-48 N3 用真 pty 断言 `use <name>` 不许阻塞 —— 那是 2026.7.30 的既有决定。改成 opt-in。
2. **T15 的拒绝策略**:一刀切拒绝会破坏 `install <pkg>`(不带 `-y`)完成这个被断言的行为。缺陷不是"猜",是"猜否并称之为成功" —— 策略交给调用方声明。

**两个平台陷阱**(本地 `mcpp build --toolchain llvm@20.1.7` gate 抓到):`\x` 转义吃掉后续 hex 位;无 stream 的 `std::println` 在 clang 下把格式串当参数 —— 后者的触发条件是"这个 TU 还 import 了什么",从文件本身无法预测。
