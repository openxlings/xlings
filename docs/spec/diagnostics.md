# 诊断信息规约 (Diagnostics)

> 状态: 生效 (2026.8.22.1 起)
> 实现: `src/core/diag.cppm` · `src/core/uimode.cppm` · `libs/theme/`
> 设计: [`.agents/docs/2026-08-22-cli-diagnostics-experience-design.md`](../../.agents/docs/2026-08-22-cli-diagnostics-experience-design.md)

## 1. 一个问题,一个严重级标记

用户看到的每一个问题,**只带一个** `[error]` / `[warn]` 标记。证据和出路是它的**续行**,不是独立的消息。

```
[error] llvm is not installed in this subos (default)
          from                  ./.xlings.json  ->  workspace.llvm
          installed elsewhere   22.1.8, 20.1.7
          install it here       xlings install llvm@22.1.8
          see every subos       xlings use llvm --all
```

这条规约存在的原因是它曾被违反:`log::` 的四个级别都在回答"有多严重",没有一个回答"这是上一条的延续",于是调用点只能再打一条同级日志。全树 313 条 error/warn 文案里,74 条(24%)是这样的续行,各自付了一个红色标记。

**做法**:构造一个 `diag::Diagnostic`,调用 `diag::emit()`。不要连打多条 `log::error`。

## 2. `Diagnostic` 的字段

| 字段 | 含义 | 必填 |
|---|---|---|
| `level` | `Note` / `Warn` / `Error` | 是 |
| `code` | 稳定、可搜索、可文档化,如 `xvm.not_in_subos` | 是 |
| `summary` | 一句话,小写开头,不加句号,不换行,不自带 `[前缀]` | 是 |
| `source` | **这个约束是谁下的** —— 文件路径 + 字段 | 有则填 |
| `facts` | 证据,两列对齐渲染 | 否 |
| `actions` | 出路;`label` 是人话,`command` 能直接跑 | `Warn`/`Error` 必填 |
| `nothingChanged` | 一个**承诺**,只有真的没写过状态才置位 | 否 |

`validate()` 会拒绝违反上述的对象,`tests/unit/test_diag.cpp` 逐条断言。

### 2.1 `source` 为什么是一等字段

项目 `.xlings.json` 里写 `"mcpp": "2026.99.9.9"`,每次 `mcpp` 调用都会失败,而用户从没输入过这个版本号。旧消息只说 `version '2026.99.9.9' not found for 'mcpp'`,不提任何文件 —— 唯一的排查手段是全盘 grep。

`Config::version_source(target)` 给出这个答案;新增任何"由配置决定"的失败都应该填它。

### 2.2 `actions` 为什么必填

一个没有出路的错误没有写完。这条规矩原本只写在 `xvm/errors.cppm` 的注释里("No hint means the error is not ready to be shown to a user"),现在是全局不变量。

`Note` 例外:它是提醒,可能确实无事可做。

## 3. 候选列表

任何"你本可以选这些"的列表都必须经过 `diag::candidates()`:

- **按版本序降序**,不是 `std::map` 的字典序;
- **截断到前 N**,并说明**丢了多少**、去哪看全部。

曾经三处直接拼接:实测一行 877 字符、94 个版本、`0.0.100` 排在 `0.0.24` 前面。

## 4. 严重级

| 级别 | 用于 | 退出码 |
|---|---|---|
| `Error` | 命令失败了 | 非 0 |
| `Warn` | 降级了,但命令继续 | 通常 0 |
| `Note` | 关于一件**成功**的事的说明 | 0 |

`Note` 是新增的一档。此前 123 个 `log::warn` 调用点里有 120 个所在路径最终成功 —— 琥珀色 `[warn]` 同时表示"出问题了"和"顺便告诉你",于是用户学会了跳过它。

**一次成功的 `xlings use` 报告哪些名字没跟过来,是 `Note`;一个 subos 忽略了你要求的 runtime,是 `Warn`。**

## 5. 前缀

消息里**不要**自带 `[xlings]` —— `log::info` 已经打了。模块前缀(`[xim]` / `[xvm]` / `[index]`)只在它确实增加信息时使用。

## 6. 交互

### 6.1 非交互下遇到确认

调用方必须声明默认值的**方向**:

| 策略 | 何时用 | 行为 |
|---|---|---|
| `Proceed` | 默认是"继续"(install / update) | 照常执行 —— 这是 E2E-48 断言的既有行为 |
| `Refuse` | 默认是"不做"(remove) | 报 `Diagnostic{Error}`,退出码 2,**不假装成功** |

这条区分来自一个真实缺陷:`--agent` 用 `defaultValue` 回答所有提示,而两个确认的默认值方向相反 —— `xlings remove foo --agent` 打印 `cancelled`、退出 0,包还在。调用它的 agent 会读成"删除成功"。

### 6.2 交互式选择是**opt-in**

`tui.interactive` 默认 **false**。

原因写在 E2E-48(`non_interactive_contract_test.sh` N3)里:**pty 不是"有人在键盘前"的证据**。agent 和自动化工具经常分配 pty,所以"是不是终端"这个判据恰好把阻塞分支交给了最不能回答的一批调用者。2026.7.30 因此删掉了选择器和它的 `--pick` 开关。

`xlings config --interactive true` 是不同性质的信号:一个**存下来的承诺**,由那个会坐在键盘前的人做出。

## 7. 前端与模式

`--ui-mode` 只有两个值:

| 值 | 含义 |
|---|---|
| `cli` | 纯文本。管道、CI、`--agent` |
| `tui` | ftxui **行内**渲染。**不做全屏** |

不在 `--ui-mode` 里的两个入口:`xlings interface`(NDJSON,另一个入口点)、`xlings-gui`(另一个二进制)。

**降级必须说出来。** 请求 `tui` 但 stdout 不是终端 → 降到 `cli` 并发一条 `Note` 说明原因。静默降级会让用户以为配置没生效。

## 8. 配置面

新键与 `mirror` / `lang` **同级平铺**,走同一条链路(CLI 选项 → 锁下写 `~/.xlings.json` → 项目覆盖全局 → `xlings config` 回显):

```json
{
  "mirror": "CN",
  "lang":   "auto",
  "uiMode": "auto",
  "theme":  "config/themes/mono.json",
  "tui":    { "interactive": false }
}
```

`tui.*` 进子块,因为它是那个前端自己的设置;换到 `cli` 就没有意义。

```bash
xlings config --lang        en|zh|auto
xlings config --ui-mode     cli|tui|auto
xlings config --theme       <name>|<path>|default|list
xlings config --interactive true|false
```

### 8.1 `lang`

默认 `auto`,跟随系统语言(POSIX 读 `LC_ALL` > `LC_MESSAGES` > `LANG`;Windows 读 `GetUserDefaultLocaleName`)。

**不要用 `std::locale("")`**。实测:musl 静态(发布形态)的 `newlocale` 只支持 `C`/`POSIX`,libstdc++ 对任何真实 locale 名抛异常;glibc 上则取决于目标机有没有 `locale-gen` 过。两种情况都被 catch 成 `"en"`,于是"系统真是英文"和"探测彻底失败"输出一模一样。

测试一律显式配 `--lang en`。

## 9. 主题

见 [`themes.md`](themes.md)。要点:

- 槽位**封闭**在 9 个角色(`accent / alt / success / warn / error / text / muted / border / surface`);
- 取色一律**按角色**,不按颜色名。`ui/` 里出现 `theme::cyan()` 这类调用即是回归;
- 主题文件是**部分覆盖**,没写的槽回落到编译进二进制的 `default`;
- 路径不存在 / JSON 坏 / 槽位拼错,**三种都要报**,不静默回落。

## 10. 稳定码表

| code | 含义 |
|---|---|
| `xvm.not_in_subos` | 目标没装在当前 subos |
| `xvm.no_active_version` | 装在这个 subos 但没有活跃版本 |
| `xvm.unknown_target` | 这个 home 不认识这个名字 |
| `xvm.pinned_version_missing` | 被钉住的版本没装 |
| `xim.remove_absent` | 要删的东西不在这里 |
| `cli.needs_confirmation` | 需要确认但无人可问 |
| `cli.bad_ui_mode` / `cli.bad_interactive` | 配置值非法 |
| `ui.mode_degraded` | 前端降级了 |
| `ui.interactive_first_run` | 首次交互提示(仅一次) |
| `theme.not_found` / `theme.unreadable` / `theme.bad_json` / `theme.unknown_slot` / `theme.bad_color` | 主题加载问题 |

新增诊断时把 code 加到这张表。
