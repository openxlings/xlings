# 命令确定性与 0730 收尾 —— 优化方案

**日期**: 2026-07-30
**类型**: 计划 (plan)
**基线**: xlings `2026.7.30.1`（已发布）+ xim-pkgindex `19d776b5` / xlings 跟踪 bump 已合入
**前序**: `.agents/docs/2026-07-30-subos-selection-leak-and-foreign-payload-plan.md`
**涉及仓库**: `openxlings/xlings`（全部）；`openxlings/xim-pkgindex`（仅 P3-4 清洁化，可选）
**相关 issue**: [#408](https://github.com/openxlings/xlings/issues/408)、[#419](https://github.com/openxlings/xlings/issues/419)、[#423](https://github.com/openxlings/xlings/issues/423)、[#447](https://github.com/openxlings/xlings/issues/447)

---

## 0. 摘要与分批

`2026.7.30.1` 发出去几小时内，真实使用暴露了**一个我引入的回归**和**一族早就存在、但只有 agent/脚本会踩的设计问题**。它们和上一版修的两族缺陷是同一个家族：**一条命令"什么都没做"和"做成了"在输出与退出码上不可区分**。

| 批次 | 主题 | 独立价值 | 阻塞关系 |
|---|---|---|---|
| **P0-1** | `remove` 被载荷平台判定误伤（**回归**） | 用户当下无法卸载异平台包 —— 恢复路径被自己堵死 | 无（补丁已就绪） |
| **P1-1** | `use <name>` 无版本时**没有确定性语义**（TTY 阻塞 / 非 TTY 静默 no-op） | agent 与脚本可用性 | 无 |
| **P1-2** | group 切换把成员**静默失活**，用户掉回系统工具链 | 用户可见的能力倒退，无任何提示 | 无 |
| **P1-3** | 全部交互点统一契约 + 非交互回归测试 | 防止 P1-1/P1-2 再长回来 | P1-1 |
| **P2-1** | CN 客户端**没有跨区回退**，镜像缺件 = 硬 404 | 每次发布的手动 CN 步骤从"加速"变成"必须"，漏做即断供 | 无 |
| **P2-2** | 极慢源不会被换掉（watchdog 门槛之上） | 25 分钟装一个 36MB 包 | 无 |
| **P3-1** | `xlings-ci-fresh-install` 4 个 `core` 单元红 | 已红两个版本，掩盖真实回归 | 无 |
| **P3-2** | A-5 隔离护栏无测试、只告警 | 上一版留下的欠账 | 无 |
| **P3-3** | doctor 只扫当前 subos 的 sysroot | 多 subos 用户要手动逐个切 | 无 |
| **P3-4** | `gcc.lua` 仍烧绝对路径（记录层面不干净） | 纯清洁化，收益低 | P1 全部之后 |

**发布建议**：P0-1 单独作为 `2026.7.30.2` **立刻发**；P1 作为一个 PR 串（`2026.7.31.1`）；P2/P3 排后。

---

## 1. P0-1 —— `remove` 被平台判定误伤（我引入的回归）

### 现场

用户升级到 `2026.7.30.1` 后：

```
$ xlings use llvm            → 列出 20.1.7 / 22.1.8，选 20.1.7
$ xlings install llvm@20     → 真的开始下载（7.0 MB/s）   ← 平台判定按预期工作
$ xlings remove llvm@20
[warn] xim:llvm@20.1.7 is not installed        ← 载荷明明在磁盘上
```

`~/.xlings/data/xpkgs/xim-x-llvm/20.1.7/bin/` 里躺着 29 个 Windows `.exe`，目录非空。

### 原因

上一版把平台判定**折进了** `PackageMatch::installed`（`catalog.cppm`）：

```cpp
if (match.installed
    && classify_payload_platform(installDir) == PayloadPlatform::Foreign) {
    match.installed = false;      // ← 这一句
}
```

而 `remove` 的门禁读的是同一个字段（`commands.cppm:701`）：

```cpp
if (!match->installed) {
    log::warn("{}@{} is not installed", displayName, displayVersion);
    return 0;
}
```

于是**上一版文档里写的恢复步骤 `remove` → `install` 被自己堵死了**。同一个字段被两个语义不同的问题共用：
- 安装侧问的是"**要不要重新下载**"；
- 卸载/列表侧问的是"**磁盘上有没有东西**"。

### 修法（补丁已在工作区就绪）

拆成两个字段，`installed` 恢复为纯事实：

```cpp
struct PackageMatch {
    ...
    // 载荷目录存在且有内容。不回答"是谁的平台构建的" —— 见 payloadForeign。
    bool installed { false };
    // 载荷可证明属于另一个平台。与 installed 分开，因为两者回答不同的问题：
    // install 必须把它当成"未安装"（否则计划为空 → 不下载 → 重装无产物可用），
    // 而 remove / list 必须仍然看得见磁盘上的东西。
    bool payloadForeign { false };
};
```

消费点只改安装计划这一侧：

```cpp
// resolver.cppm
node.alreadyInstalled = match.installed && !match.payloadForeign;
// commands.cppm
requestedAlreadyInstalled[plan_key(match)] = match.installed && !match.payloadForeign;
```

`remove` / `list` 保持读 `installed`，行为回到 `2026.7.29.2`。

### 测试（必须是差分）

`tests/e2e/foreign_payload_reinstall_test.sh` 追加 **F4**：

```bash
# ── F4: a foreign payload must still be removable ────────────────────
#
# The platform check decides "should this be downloaded again". It must not
# decide "does this exist" -- 2026.7.30.1 folded the two into one field and
# `remove` started refusing the very package its own error message told the
# user to remove.
log "F4: remove still works on a foreign payload"
rm -f "$PKG_DIR/.xpkg-install.json"
rm -f "$PAYLOAD/fp-tool"
printf 'MZ\220\0\0\0\0\0\0\0\0\0\0\0\0\0' > "$PAYLOAD/fp-tool.exe"

out="$(RUN remove fp-probe -y 2>&1 || true)"
if grep -q "is not installed" <<<"$out"; then
  fail "F4: remove refused a payload that is on disk; got:\n$out"
fi
[[ ! -d "$PKG_DIR" ]] || fail "F4: payload survived remove"
```

---

## 2. P1 —— 命令确定性契约

### 2.1 P1-1 `xlings use <name>` 没有确定性语义

`xvm/commands.cppm:583` 起：

```cpp
const bool interactive = versions.size() > 1
    && platform::stdin_is_terminal()
    && platform::supports_rewrite_output()
    && !platform::is_tui_mode()
    && !palette::plain_forced();

if (interactive) { /* 上下键 picker，阻塞直到有人按键 */ }
... // 非交互：打印一个 info_panel
return 0;      // ← 什么都没做，退出码 0
```

两条路都不可用：

| 环境 | 现状 | 问题 |
|---|---|---|
| 有 TTY 的 agent / CI / 被 pty 包装的工具 | **阻塞在上下键选择上** | 无限等待，没有超时 |
| 无 TTY | 打印列表，**退出码 0，状态没变** | agent 认为"切换成功"，实际什么都没发生 |

> 注意：现有的 TTY 门**不足以**解决 agent 问题 —— 大量 agent/终端工具会分配 pty，`stdin_is_terminal()` 为真。**"有没有人在键盘前"不可探测，所以不能拿它当契约。**

**目标语义（推荐）—— 确定即执行，歧义即拒绝：**

| 候选版本数 | 行为 | 退出码 |
|---|---|---|
| 1 | **直接切换**（无歧义，没有可问的） | 0 |
| >1，且其中恰有一个是当前 subos 已激活 | 保持不变并说明"已经是它了" | 0 |
| >1 | **不切换**，列出候选 + 给出精确命令，`ErrorEvent{InvalidInput}` | **2** |
| 0 | 报错（现状已正确） | 1 |

**上下键 picker 改为 opt-in**：`xlings use <name> --pick`（或 `-i`）。默认路径永不阻塞。

```cpp
// 歧义时：拒绝而不是发问。谁在键盘前是不可探测的，能探测的只有
// "这条命令有没有唯一正确结果"。没有就报错,并把候选写成机器可读的一行。
if (versions.size() > 1 && !pick) {
    stream.emit(ErrorEvent{
        .code = ErrorCode::InvalidInput,
        .message = std::format("'{}' has {} installed versions; name one",
                               target, versions.size()),
        .recoverable = true,
        .hint = std::format("xlings use {} <version>   (versions: {})",
                            target, join(versions, " ")),
    });
    return 2;
}
```

### 2.2 P1-2 group 切换静默失活成员

用户实测：`xlings use llvm` → 20.1.7 之后，`clang++ --version` 变回系统的 `Ubuntu clang 18.1.3`。

工作区（`subos/default/.xlings.json`）说明了一切：

```json
"llvm":        { "active": "20.1.7", "installed": ["20.1.7","22.1.8"] },
"clang":       {                     "installed": ["22.1.8"] },   ← active 没了
"clang++":     {                     "installed": ["22.1.8"] },   ← active 没了
"clang++.exe": { "active": "20.1.7", "installed": ["20.1.7"] }
```

20.1.7 这个 release 的成员是一堆 `.exe` 名字（上一版 pkgindex 修复之前注册的垃圾），所以切过去以后：
`clang` / `clang++` **在本 subos 没有任何活动版本** → shim 被删除 → PATH 落到 `/usr/bin/clang++`。

而命令的全部输出是：

```
[xlings] llvm -> 20.1.7
```

**这是"能力倒退且零提示"**，与 `.exe` 注册是不是垃圾无关 —— 任何两个成员集不同的 release 之间切换都会这样。

**修法**：切换后比对"切换前有 active 的程序名"与"切换后有 active 的程序名"，差集必须显式报告：

```
[xlings] llvm -> 20.1.7
  ⚠ 3 program(s) have no version in this release and are now inactive:
      clang (was 22.1.8)   clang++ (was 22.1.8)   llvm-ar (was 22.1.8)
    restore one with: xlings use clang++ 22.1.8
```

`--strict` 下直接拒绝切换（零改动 + 具名原因），供 CI 使用。

### 2.3 P1-3 交互点统一契约

现存的四个提问点：`install` 模糊匹配多选、`install` Proceed 确认、`remove` 确认、`use` 版本选择。统一为三条硬规则：

1. **任何 prompt 都必须有 flag 出口**（`-y` / `--pick` / 显式参数）。
2. **非交互路径必须有确定结果**：要么执行，要么退出码 ≠ 0 并说明原因。**禁止"打印一段东西然后 return 0"**。
3. **不得用"是不是 TTY"决定语义**，只用它决定**呈现**（panel vs picker）。

回归测试 `tests/e2e/non_interactive_contract_test.sh`：把每个可能提问的命令在 `stdin=/dev/null`、`XLINGS_NON_INTERACTIVE=1`、以及**伪 TTY**（`script -qc`）三种环境下各跑一次，断言：**不挂起**（`timeout 30`）、退出码符合上表、状态改变与退出码一致。

---

## 3. P2 —— 分发韧性

### 3.1 P2-1 CN 客户端没有跨区回退（本次发布真实断供 3 小时）

`2026.7.30.1` 发布后，CN 用户 `xlings install xlings@latest`：

```
[error] download failed for xim:xlings@2026.7.30.1: HTTP 404
[error] download artifact missing for xlings
```

链路查证：

| 事实 | 证据 |
|---|---|
| GitHub 侧齐全 | `xlings-res/xlings` 8 个资产俱在 |
| GitCode 侧**只有 4 个 `.sha256`，4 个大包全缺** | 4×`.tar.gz/.zip` GET → **404**；4×`.sha256` → 200 |
| CI 明确放弃并写了原因 | `[mirror] gtc … upload STALLED (killed at 90s) — cross-border runner->OBS wall；Finish … by running tools/mirror-latest.sh from a CN environment` |
| 手动补齐后立即恢复 | 从 CN 跑 `tools/mirror-latest.sh xlings` → 4 个包全部 200 |

**但真正的缺陷不是"忘了跑手动步骤"**，而是 `tools/mirror-latest.sh` 头注释里的这句在默认配置下**不成立**：

> *"correctness does not depend on it — CN clients already fall back to the GitHub asset URLs"*

`config.cppm` 的默认表是**按区分桶**的：

```cpp
{ "GLOBAL", { "https://github.com/xlings-res" } },
{ "CN",     { "https://gitcode.com/xlings-res" } },
```

`resource_servers()` → `candidate_resource_servers_for_(mirror_)` **只返回当前区的那一桶**，而 `build_xlings_res_fallback_urls_` 是"候选集减去已选" → CN 用户的 fallback 列表**恒为空**。`mirror::expand` 只做 github → CN 方向的展开，反向没有。**于是 GitCode 缺件 = 硬 404，没有任何回退。**

**修法（二选一，推荐 A）：**

- **A. 跨区兜底**：`build_xlings_res_fallback_urls_` 在同区候选用尽后，追加**其他区**的服务器（顺序靠后，仅作最后手段）。CN 用户优先走 GitCode，缺件时自动落到 GitHub —— 手动镜像步骤回到它原本声称的"加速而非必须"。
- B. 发布流水线把手动步骤变成硬门禁（release 结束前校验 CN 侧 8 个资产全 200，否则标红）。**这只是把断供变成可见，不解决用户当下装不上。**

A 和 B 不冲突，建议 A 先做、B 作为发布检查表补充。

### 3.2 P2-2 极慢源不会被换掉

> **更正**：我先前说"缺一个 stall 检测"是错的。`src/libs/tinyhttps.cppm:27-34` 已有停滞看门狗（默认 **<10 KB/s 持续 15s → 放弃该候选换下一个**，`XLINGS_DOWNLOAD_LOW_SPEED` 可覆盖），`downloader.cppm` 还会 `penalize_host` 惩罚它。

实测那次"25 分钟装一个 36MB 包"的真实成因是：源稳定在 **~105 KB/s**，**高于 10 KB/s 的地板**，所以看门狗永远不触发；而 `retryCount=3` × `maxTimeSec=600` 让单个候选最多可以耗掉 30 分钟。

**修法**：候选选择只在**开始时**排一次序（`mirror::adaptive::reorder`），中途不再评估。补一条**相对判据**：当已测得的其他候选延迟明显更优、且当前候选实测吞吐低于某阈值（如 200 KB/s）持续 N 秒时，切换候选而不是把 3 次重试耗在同一个慢源上。同时把"正在用哪个源"打进进度行，慢的时候用户至少知道慢在哪。

---

## 4. P3 —— 收尾欠账

| # | 项 | 现状与动作 |
|---|---|---|
| P3-1 | `xlings-ci-fresh-install` 4 个 `core` 单元红（`FAIL xlings list omits mcpp`） | **已确认与本次无关**：`518e525`（上一个 release）上是同样 4 个单元、同样步骤失败。先立 issue 定性：是 `list` 的 per-subos 交集（`workspace_installed`）在全新安装路径下没被写入，还是 fresh 脚本的断言过时 |
| P3-2 | A-5 隔离护栏只告警、无测试 | 补一个单测：源在 `dataDir`/`homeDir` 之外时**必须**产生告警且链接仍然建立（锁住"告警不是拒绝"这个刻意选择），避免下一个人误以为是漏改 |
| P3-3 | doctor 只扫当前 subos 的 sysroot | 悬空链接扫描扩展到 `Config::list_subos_names()` 的全部 subos：**报告**全部，**只修复**当前的（沿用多 subos 既有边界），并给出 `XLINGS_ACTIVE_SUBOS=<n> xlings self doctor --fix` |
| P3-4 | `gcc.lua` 仍烧绝对路径 | 执行期已纠正，纯清洁化；做的话走 recipe capability probe，见前序文档 §3.2 方案 1 |
| — | #408 整体模型 | 仍在 0.5 线，本方案不动 |

---

## 5. 任务拆分与依赖

```
P0-1 remove 回归 ──► 发 2026.7.30.2（单独 PR，最快路径）
P1-1 use 确定性 ──┬─► P1-3 非交互契约测试
P1-2 group 失活 ──┘
P2-1 跨区兜底   （独立）
P2-2 慢源再评估 （独立）
P3-1 fresh-install 定性 → 可能派生独立修复
P3-2 / P3-3     （独立，小）
P3-4            （P1 之后，可选，跨仓）
```

---

## 6. 验收标准

| # | 判据 | 方法 |
|---|---|---|
| V1 | 异平台载荷可以 `remove`，也仍然会被 `install` 重装 | F4 + 既有 F2/F3 |
| V2 | `xlings use <name>` 在**伪 TTY** 下 30s 内返回，且退出码区分"切了/没切" | `script -qc` + `timeout 30` |
| V3 | 单候选时 `use <name>` 真的切换 | e2e |
| V4 | group 切换导致的失活成员被逐个点名，`--strict` 下拒绝切换 | e2e，断言输出含 `clang++ (was 22.1.8)` |
| V5 | 断掉 GitCode 后 CN 配置仍能装上（自动落 GitHub） | 用假的 CN 资源服务器指向 404 主机 |
| V6 | 四个可提问命令在三种环境下都不挂起 | `non_interactive_contract_test.sh` |
| V7 | 六个 workflow 全绿；`fresh-install` 的红有 issue 编号 | CI |

---

## 7. 不做什么

- **不把交互式 picker 删掉**，只把它从"唯一完成路径"降级为 `--pick` 的显式选择。人在终端前时它仍然好用。
- **不改 group/binding 模型**（成员集合怎么定义、切换怎么原子化）—— 那是 #384 家族与 0.5 线的事。P1-2 只做"如实报告 + `--strict` 拒绝"。
- **不把 CN 镜像自动化**（CN 定时任务/自建 runner）。P2-1 让手动步骤重新变成"加速"，自动化是频率问题，不是正确性问题。
- **不动 `xlings list` 的 per-subos 语义**：P3-1 先定性再决定。

---

## 7.1 实施记录（2026-07-30，`2026.7.30.2`）

一个 PR 交付 P0-1 + P1 全部 + P2-1，以及规范文档。与上面写的方案有三处偏差，
都是实现/实测过程中改的，记在这里以方案为准。

### 偏差 1：`use <name>` 的"已经是它了"一档被删掉

原表里有一行"`>1` 且其中恰有一个已激活 → 保持不变并说明，退出码 0"。**没有实现，
故意的**：用户没有指定版本，就不存在"唯一正确结果"，退出 0 等于告诉调用方
"我照你说的做了"。现在这一档并入拒绝档，只是消息里多一句
`currently active: <ver>`。

最终语义（`cmd_use_by_name`）：

| 候选数 | 行为 | 退出码 |
|---|---|:---:|
| 1 | 直接切换 | 0 |
| >1 | 不改动，列候选 + 精确命令 + `ErrorEvent{InvalidInput}` | **2** |
| 0 | 报错（原样） | 1 |

`--pick` 在没有终端时**也返回 2**并说明原因，而不是退回到"打印面板 + 0"——
那正是原缺陷换了个旗标。picker 被取消（用户按 ESC）同样是 2：什么都没变。

另外把 `cmd_list_versions` 拆了出来：`list_installed_versions` 这个 capability
以前和 `use <name>` 共用一个函数，也就是说**一个只是想"看看有哪些版本"的调用方
可能拿回一个被切换过的工具链**。现在它只列，永不切。

### 偏差 2：P1-2 的缺陷描述是错的 —— 不是"静默失活"，是"静默混装"

用 fixture 实测（1.0.0 注册 `gs-a`+`gs-b`，2.0.0 只注册 `gs-a`）：

```
### use gs-probe 2.0.0
[xlings] gs-probe -> 2.0.0
### workspace
gs-a: active 2.0.0     gs-b: active 1.0.0     gs-probe: active 2.0.0
```

`gs-b` **没有**被失活，它仍然可用、仍然指向被离开的那个 release。所以用户看到的
不是"命令不见了"，而是"`llvm` 说 20.1.7，`clang` 还在答 22.1.8"，而全部输出只有
一行。方案里写的 "3 program(s) … are now inactive" 措辞随之改成
"not part of this release and still resolve to the release you switched away
from"，并给出它现在解析到的版本。

判据放在 `plan_use_switch`（纯函数，可单测）：只报告**当前活动版本恰好等于出场
release 里那个版本**的成员 —— 用户自己已经挪走的不算。出场 release 解析不出来时
不报告也不失败：那条路径正是用来修复它的。

> 用户原始现场里 `clang` 的 workspace 条目**确实没有 `active`**，这与本次实测出的
> 形状不同，说明那台机器上还有另一条路径动过它（大概率是安装/移除侧）。本次没有
> 复现出那条路径，**不宣称已修**；已修的是这里实测到的这一族。

### 偏差 3：P2-1 采用方案 A，并且把它做成了下载侧独有的列表

`Config::resource_servers()` 语义不变（"这个区应该被指到哪"，选择用），新增
`resource_servers_with_cross_region()`：同区候选在前，其余区按 key 排序追加去重。
只有 `build_xlings_res_fallback_urls_` 用它 —— 回退顺序保持偏好不变，CN 用户仍然
先走 GitCode，只有全挂了才落 GitHub。

### 交付清单

| 项 | 代码 | 测试 |
|---|---|---|
| P0-1 | `catalog.cppm` `payloadForeign` 与 `installed` 分离；`resolver.cppm` / `xim/commands.cppm` 只在安装计划侧消费 | E2E-47 **F4**（旧构建复现 `is not installed`） |
| P1-1 | `xvm/commands.cppm` `cmd_use_by_name` / `collect_version_candidates_` / `emit_version_panel_`；`cli.cppm` `--pick` | **E2E-48** N1–N7（含伪 TTY + `timeout 30`） |
| P1-2 | `switch_plan.cppm` `StrandedMember`；`cmd_use` 报告 + `--strict` | 5 个单测 `XvmSwitchPlan.*Stranded*`；**E2E-49** G1–G3 |
| P2-1 | `config.cppm` `all_resource_servers_for_`；`installer.cppm` 回退列表换源 | **scenario `xlings_res_cross_region`**（CN 指向死主机，旧构建装不上） |
| 规范 | `AGENTS.md`、`xlings-contributing`、`xlings-usage`、`docs/quick-start/multi-version.md` | — |

全部 e2e 都在旧构建上跑过一遍确认会失败 —— 差分成立，不是"顺便通过"。

### 发布与真实验证（`2026.7.30.2`）

- xlings#455 合入 `bd0ac7f`（7 项 CI 全绿）；`release.yml` 全绿；本地 `gtc`
  跑 `tools/mirror-latest.sh xlings` 校验 **16 个 URL 全 200**（GitHub 8 + GitCode 8，
  用 GET 不用 HEAD）；xim-pkgindex#458 sha256 与已发布 sidecar 逐一比对后合入。
- **在隔离 home 里用已发布的 linux-x86_64 产物实测**（不是本地构建）：

  | 检查 | 结果 |
  |---|---|
  | 产物 sha256 = 已发布 sidecar | ✅ |
  | 冷 home 解析索引、`self install`、`--version` | ✅ 2026.7.30.2 |
  | `use mcpp`（两个版本，有歧义） | ✅ 退出码 **2**，两个候选都列出，状态未变 |
  | 同上，**伪 TTY** 内 | ✅ 30s 内退出码 2 —— 正是以前 agent 卡死的那条路径 |
  | `use mcpp --pick`（无终端） | ✅ 退出码 2 并说明原因 |
  | 指名切换 + shim 实跑 | ✅ `mcpp 2026.7.30.2` |
  | `remove mcpp@…` | ✅ |
  | 只剩一个版本后 `use mcpp` | ✅ 退出码 0，真的切了 |
  | `subos new` / `self doctor` | ✅ |

- `xlings-ci-fresh-install`（跟随 release 自动触发）：**gcc / llvm 8 个单元全绿**
  （上一轮 llvm 还是红的），4 个 `core` 单元仍红，是同一条既有断言
  `FAIL xlings list omits mcpp`，在 `518e525` 和 `2026.7.30.1` 上同样红 ——
  **与本次无关**，已立 [#456](https://github.com/openxlings/xlings/issues/456)。

### 本次没做

P2-2（慢源再评估）、P3-1..P3-4 全部留到下一轮：它们互相独立，且都不在"用户当下
装不上/切不动"的路径上。P3-1 需要先定性再动 `list` 的 per-subos 语义。

---

## 7.2 后续批次（`2026.7.31.1`，P2-2 / P3-2 / P3-3）

### P2-2 —— 真正的缺陷不是"慢"，是**深度优先**

再查一次 `src/libs/tinyhttps.cppm` 的候选循环，发现它是
`for url { for attempt { } }`：**一个坏源会先把自己的重试预算耗光，第二个候选才
会被碰到**。按出厂参数就是 `retryCount 3 + 1` 次 × `maxTimeSec 600` =
**第一个候选独占 40 分钟**，而健康镜像一次都没试过。看门狗盖不住它 —— 它只在
低于 ~10 KB/s 时触发，而实测那个源稳定在 ~105 KB/s：慢到装不完，快到不像停滞。

改成**广度优先**：每个候选先各试一次，再进入下一轮。

- 每个候选的尝试次数**不变**（`retryCount + 1`）；
- "校验失败就出局""stalled 就出局"两条放弃规则**不变**；
- 只有顺序变了。

顺带补了两个诊断（这次 CN 断供最难查的就是"它到底在从哪拉"）：候选列表打到
debug，实际服务的候选与首选不同时打一行 `[mirror] <pkg> served by <url>`。
为此给 `DownloadFileResult` 加了 `sourceUrl` —— 已有的 `finalUrl` 是重定向后的
CDN 地址，回答不了"我的哪个镜像给的"。

5 个单测（`DownloadOrder.*`）用 `transferOverride` 这个测试缝锁住顺序、预算、
两条出局规则和 `sourceUrl`。

### P3-2 —— 把"告警不是拒绝"这个刻意选择锁进测试

`install_headers` 对 home 外的源只告警、仍然建链，因为 recipe 合法地可能暴露
home 外的头文件（包系统头的 wrapper 就是），拒绝会打断今天能用的包。这看起来
太像"改了一半"，所以补一个单测同时钉住两半：**必须告警**，且**必须仍然建链**。
少了第二个断言，一个开始拒绝的版本也能通过。

### P3-3 —— doctor 扫全部 subos 的悬空链接

这类链接**不是**在它所在的那个 subos 里产生的：实测那次是一个隔离运行把头文件
物化进了真实 home 的 `dev-hello`，而用户人在 `default`。所以"只看你站着的地方"
恰好保证漏掉这个扫描存在的理由。

现在报告全部 subos，前缀 `[<name>]`；修复仍只动当前 subos（另一个 shell 可能正
激活着它），并对非当前 subos 的发现单独渲染
`→ run  XLINGS_ACTIVE_SUBOS=<name> xlings self doctor --fix` ——
底部那句通用 hint 对它是错的。

E2E-42 加 S8/S8b/S8c；已确认在 `2026.7.30.2` 构建上失败。

### P3-4 仍然不做（这次是有理由的，不是排期）

`gcc.lua` 烧绝对路径：执行期已纠正，`doctor --fix` 也会重写记录，所以它是**记录
层面的干净**，不是用户可见缺陷。做它要跨仓 + 一个 capability probe，否则老客户端
拿到 `${XLINGS_SUBOS}` 这种它不认识的占位符 —— 那比烧一个绝对路径**更糟**。
唯一真正的收益是"装完 gcc 后 doctor 不再常驻一条 `subos path` 告警"。
建议单独一轮做，先在 xlings 侧确认 probe 形状，再动 recipe。

---

## 8. 更正记录

写这份方案时查证推翻了我先前给出的两条结论，记在这里以免后来者照抄：

1. ~~"下载缺一个 stall 检测"~~ → **停滞看门狗已存在**（10 KB/s / 15s，可用 `XLINGS_DOWNLOAD_LOW_SPEED` 覆盖）。真实问题是"慢但不停滞"的源不会被换掉（§3.2）。
2. ~~"索引发布滞后是缓存机制导致的"~~ → 本次那个 404 **与索引缓存无关**：索引已经正确解析到 `2026.7.30.1`，失败发生在**下载阶段**，因为 GitCode 大包缺件且**没有跨区回退**（§3.1）。索引侧的产物滞后是另一件事，本次没有触发。
