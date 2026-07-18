# Issue #374 — `install_packages` 多 index_repos 静默 exit 1 分析与修复设计

**日期**: 2026-07-19
**状态**: Bug confirmed（隔离环境实测复现，见 §3），fix proposed（未实现）
**Issue**: [openxlings/xlings#374](https://github.com/openxlings/xlings/issues/374)，下游追踪 mcpp-community/mcpp#238
**影响版本**: 0.4.62 实测复现（当前源码 0.4.66 同）
**关联代码**:
- `src/core/xim/catalog.cppm:351-375`（`rebuild()` fail-fast）
- `src/core/xim/repo.cppm:542-566,625-626`（`sync_all_repos` 项目 repo fail-fast）
- `src/core/xim/commands.cppm:114-123,180-197,255-260,399-409`（`cmd_install` 用 `log::error` 而非 stream）
- `src/core/log.cppm:151-164`（`log::error` → stderr，TUI 模式下**完全静默**）
- `src/interface.cppm:181-185`（interface 入口 `set_tui_mode(true)`）
- `src/runtime/event.cppm:35-63`（`ErrorEvent` / `ErrorCode` 结构化错误——已有但 xim 层未用）
- `src/core/subos.cppm:96,371,...`（**同仓库内已正确使用** `stream.emit(ErrorEvent{...})` 的范式）

---

## 0. 结论先行（Verdict）

**是真实问题，且已实测复现。** 与 2026-05-22 的 `exitCode=0` 静默吞失败（已在 `commands.cppm:456` 修复）是**不同的第二个 bug**：这次是**解析/建库阶段** exit **1**，NDJSON 流里**零诊断**。

根因是两条**架构级**缺陷叠加，issue 标题里的"≥2 index_repos"只是**触发条件**，不是根因：

1. **可观测性缺口（"silent" 的来源）**：xim 命令层所有失败都走全局 `log::error(...)`，它写 stderr/日志文件，而 interface 入口把进程设成 `tui_mode=true` → `log::error` 被**整条吞掉**（连 stderr 都不到）。而 NDJSON 协议**本就有** `ErrorEvent` 结构化错误事件，interface 框架层自己在用，`subos.cppm` 命令层也在用——**唯独 xim 命令层不用**。于是任何非零退出都产出 `{"exitCode":1,"kind":"result"}` + 空事件流。**这条影响所有 exit-1 路径，不止多 repo。**

2. **catalog 跨 repo "全有或全无"（"≥2 repos" 触发的来源）**：`PackageCatalog::rebuild()` 对第一个建库失败的 repo 直接 `return std::unexpected` → **一个坏 repo 拖垮整个 catalog**，健康 repo 也一起废。单 repo 时没有"另一个"能坏；一旦多命名空间继承 + 默认命名空间重定向生出第 2 个 repo（且它无 `pkgs/`/不可 sync/重定向到空目标），整库 fail。讽刺的是**紧挨着的 sub-index 早就是 best-effort**（`sync_all_repos` 里 warn+continue），只有 main/project repo 是致命的。

次要facet：跨 repo 同名裸名解析走 `resolve_target` 的 ambiguous 硬失败，也被同样吞掉（见 §4.3）。

---

## 1. 现象（Issue 复述）

mcpp 通过 `xlings interface install_packages` 装包，当项目 `.xlings.json` 的 `index_repos` 有 **≥2 项**时：

```
{"exitCode":1,"kind":"result"}
```

——exit 1，但 NDJSON 流里没有任何 error / data / log 事件，消费方无法知道**哪个 repo / 哪个包**失败。单 repo 配置一切正常。

Issue 给出的触发组合：
- 根级 `[indices]` 继承（多命名空间仓库）
- 默认命名空间重定向

二者叠加产出多条 `index_repos`（如 `mcpplibs` + `local-dev`）。真实场景：多命名空间 workspace 里的 opencv/ffmpeg 模块包。

Issue 期望：要么跨所有 repo 解析（first-match-wins），要么发出结构化错误事件，指明搜了哪些 repo、哪个包解析失败。

---

## 2. 根因链（代码级）

### 2.1 `catalog.rebuild()` 对单个坏 repo fail-fast

`src/core/xim/catalog.cppm:351-375`：

```cpp
std::expected<void, std::string> rebuild(bool forceRebuild = false) {
    projectRepos_.clear();
    globalRepos_.clear();
    auto specs = repo_specs_();                       // 项目 repos + 全局 repos + sub-index + local
    for (auto& spec : specs) {
        auto state = make_state_(spec);
        auto hash = get_repo_head_hash(spec.dir);
        auto result = state.index.load_or_rebuild(hash, forceRebuild);
        if (!result) {
            return std::unexpected(result.error());   // ← 第一个坏 repo 就整库放弃
        }
        ...
    }
    loaded_ = true;
    return {};
}
```

`load_or_rebuild` → `build()` 在 repo 目录无 `pkgs/` 时返回 `unexpected`：

```cpp
// src/core/xim/index.cppm:146-148
if (!fs::exists(repoDir_ / "pkgs")) {
    return std::unexpected(
        std::format("pkgs/ directory not found in {}", repoDir_.string()));
}
```

于是：第 2 个项目 repo（重定向生成的 `local-dev` 之类，无 `pkgs/` / sync 失败 / 重定向到空目标）→ `rebuild()` 在它身上 `unexpected` → `loaded_` 永远 false。

### 2.2 `sync_all_repos` 对项目 repo 也 fail-fast

`src/core/xim/repo.cppm:542-566, 625-626`：

```cpp
auto syncRepos = [&](const std::vector<IndexRepo>& repos, bool projectScope) {
    for (auto& repo : repos) {
        ...
        if (Config::is_local_repo_source(repo, projectScope)) {
            if (!detail_::ensure_local_repo_link_(repoDir, sourceDir)) {
                return false;                         // ← 一个坏 repo → 整个 sync 放弃
            }
            continue;
        }
        if (!sync_repo(repoDir, url, force)) {
            return false;                             // ← 同上
        }
    }
    return true;
};
...
if (Config::has_project_config() && !Config::project_index_repos().empty()) {
    if (!syncRepos(Config::project_index_repos(), true)) return false;  // ← 项目 repo sync fail-fast
    // ↓↓↓ 紧接着的 sub-index 却是 best-effort（warn + continue）↓↓↓
    for (auto& [name, repo] : projMerged) {
        if (sync_repo(...)) { ... }
        else { log::warn("failed to sync project sub-index repo: ..."); }   // ← 一致性对比
    }
}
```

**同一函数内，项目 repo 是致命的，sub-index 是 best-effort。** 这个不一致就是根因 2 的直接体现。

`ensure_local_repo_link_` 在源目录无 `pkgs/` 时 `return false`（repo.cppm:25-28），本地重定向目标为空即触发。

### 2.3 catalog 建不起来 → `cmd_install` 用 `log::error` 退出

`src/core/xim/commands.cppm:114-123`：

```cpp
auto& catalog = get_catalog();          // 内部首个 rebuild 失败 → loaded_=false（get_catalog 无 stream）
if (!catalog.is_loaded()) {
    log::info("package index not available, updating...");
    sync_all_repos(true);               // 又 fail-fast，false
    auto rebuildResult = catalog.rebuild();
    if (!rebuildResult || !catalog.is_loaded()) {
        log::error("package index not available");   // ← 关键：走 log::error
        return 1;
    }
}
```

### 2.4 `log::error` 在 interface 模式下被完全吞掉

`src/interface.cppm:184-185`：

```cpp
stream.set_enabled(tui_listener, false);
platform::set_tui_mode(true);           // ← interface 入口把进程设成 TUI 模式
```

`src/core/log.cppm:151-164`：

```cpp
export template<typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    auto msg = std::format(fmt, std::forward<Args>(args)...);
    if (!platform::is_tui_mode()) {       // ← TUI 模式 → 整块跳过（连 stderr 都不写）
        std::print(stderr, "[error] ");
        ...
    }
    write_to_file_("[error] ", msg);      // 只有配了日志文件才留痕
}
```

**链闭合**：catalog fail-fast（2.1/2.2）→ `cmd_install` 走 `log::error` 退出（2.3）→ interface 的 TUI 模式吞掉 `log::error`（2.4）→ 消费方只看到 `{"exitCode":1,"kind":"result"}`，零诊断。

---

## 3. 实测复现（2026-07-19 测得）

完全本地化（无网络），用现成 release 二进制（0.4.62）+ fixture 索引，用只读 `plan_install`（不下载）隔离"建库失败"这一根因：

```
全局 XLINGS_HOME：index_repos = [{xim → tests/fixtures/xim-pkgindex 绝对路径}]  # 本地，无网络
项目 A（单 repo）：index_repos = [{projgood → 一个有 pkgs/ 的合法目录}]
项目 B（双 repo）：index_repos = [{projgood → 合法}, {projbad → 存在但无 pkgs/}]
目标：plan_install ninja（只在全局 xim 索引里）
```

| | 项目 A（单 repo，合法） | 项目 B（双 repo，第 2 个无 pkgs/） |
|---|---|---|
| exit code | `0` | `1` |
| NDJSON stdout | `install_plan` 事件 + `result` | **只有** `{"exitCode":1,"kind":"result"}` |
| stderr | 空 | **空**（错误被 TUI 模式吞掉，连 `2>/dev/null` 都不需要） |

项目 A 输出：
```
{"dataKind":"install_plan","kind":"data","payload":{"packages":[["xim:ninja@1.12.1",""]]}}
{"exitCode":0,"kind":"result"}
```
项目 B 输出：
```
{"exitCode":1,"kind":"result"}
```

与 issue 症状逐字吻合。复现脚本存于 `scratchpad/repro374/`（可移植为 e2e 用例，见 §6）。

> 注：比 issue 报告的还严重一点——错误不仅"不在 NDJSON 里"，而是**任何地方都没有**（stderr 也空），因为 interface 的 `tui_mode=true` 直接关掉了 `log::error` 的终端输出。

---

## 4. 修复设计（按优先级）

三层，对应两条根因 + 一个次要 facet。P0 单独就能让 issue 的"零诊断"消失；P1 才从结构上根治"≥2 repo 就崩"。

### 4.1 P0 — 让命令层失败在 wire 上可见（治"silent"，收益最大）

问题本质是**两条并行诊断通道**：全局 `log::*`（stderr/文件，TUI 下静默）与 `EventStream`（NDJSON wire）。xim 命令层对某些事 `stream.emit(DataEvent)`、对失败却 `log::error` → 用错了通道。`subos.cppm` 早已示范正确做法。三个互补动作：

**(a) 系统性：把全局 logger 桥接到 EventStream（一处修，覆盖所有 `log::error`）**

`log` 是底层模块（只 `import xlings.platform`），不能反向依赖 runtime。用**类型擦除的 sink**保持分层干净：

```cpp
// core/log.cppm 新增
export void set_sink(std::function<void(Level, std::string_view)> sink);  // 由上层注入
// error()/warn() 里，除现有分支外，若 sink 已装则 sink(Level::Error, msg);
```

interface 会话（`interface.cppm` 建 session 时）注入一个 sink，把 `Level::Error/Warn` 转成 `LogEvent`（或 `ErrorEvent`）发到 stream。**这样所有现存及未来的 `log::error` 在 interface 模式下自动落到 NDJSON**，不用逐点改、也不会再漏。CLI 模式不装 sink，行为不变。

**(b) 定点升级：`cmd_install` 关键失败点发 rich `ErrorEvent`（对齐 subos.cppm 范式）**

对 issue 最相关的三处，emit 带 `ErrorCode` + `hint` 的结构化错误（比裸 LogEvent 更好，指明搜了哪些 repo）：

```cpp
// commands.cppm: catalog 建不起来（2.3 处）
stream.emit(ErrorEvent{
    .code = ErrorCode::NotFound,
    .message = "package index not available: " + <累积的 per-repo 失败原因>,
    .recoverable = true,
    .hint = "run `xlings update`, or check index_repos in .xlings.json",
});
// commands.cppm:180-197 not-found / 182-185 ambiguous 两处同理
//   not-found → NotFound + "searched repos: [xim, projgood, projbad]"
//   ambiguous → InvalidInput + 候选列表（format_ambiguous_candidates 已现成）
```

**(c) 兜底不变量：非零退出 ⇒ wire 上至少一个 error 事件**

在 `interface::run`（`interface.cppm:282-289` 出结果前）或 `exit_result` 包装处加断言式兜底：capability 返回非零、且本次执行期间未发过任何 `ErrorEvent`，则合成一个通用 `ErrorEvent{Internal, "install_packages failed (exit N)"}`。粗，但**保证协议自洽**、防未来回归——"非零退出必有错误事件"成为可测不变量。

### 4.2 P1 — catalog/sync 跨 repo：降级而非崩塌（治"≥2 repo"）

把"全有或全无"改成"best-effort + 上报被跳过的 repo"，对齐紧挨着的 sub-index 已有行为。

**(a) `rebuild()` fail-soft**：不再首个失败即 `return unexpected`；把每个坏 repo 记进 `std::vector<RepoLoadError>{name,scope,dir,error}`，继续建其余；`loaded_ = (至少 global 主索引已载入)`。新增 `catalog.load_warnings()` 供命令层读取——**catalog 保持 stream 无关**（正确分层），由 `cmd_install` 在 `get_catalog()` 后把 warnings 通过 P0 通道 emit 出去。

```cpp
for (auto& spec : specs) {
    auto result = state.index.load_or_rebuild(hash, forceRebuild);
    if (!result) {
        loadWarnings_.push_back({spec.name, spec.scope, spec.dir, result.error()});
        continue;                                   // ← 跳过坏 repo，不拖垮全库
    }
    (spec.scope == Project ? projectRepos_ : globalRepos_).push_back(std::move(state));
}
loaded_ = !globalRepos_.empty() || !projectRepos_.empty();
return loaded_ ? std::expected<void,std::string>{}
              : std::unexpected("no index repo could be loaded");
```

**(b) `sync_all_repos` 项目 repo fail-soft**：把 `syncRepos(project_index_repos, true)` 里的 `return false` 改成 warn + continue（和其下 sub-index 循环完全一致），单个项目 repo sync 失败不再中断整个 sync。

**结果**：§3 的项目 B 变成——`projbad` 被跳过并在 wire 上 warn（"repo 'projbad' skipped: pkgs/ not found"），`ninja` 仍从健康索引解析成功，exit 0。这正是 issue 期望的"健康 repo 照常出包 + 结构化诊断指明哪个 repo 出事"。被跳过的 repo **绝不静默**——靠 P0 保证被 emit。

### 4.3 P2 —（可选）跨 repo 同名裸名的优先级策略

`resolve_target`（catalog.cppm:397-409）对裸名多命中走 ambiguous 硬失败。issue 的"first match wins"可作为**配置开启**的增强：按 `index_repos` 声明顺序取第一个。默认仍保留 ambiguous 结构化错误（静默二选一会掩盖真实冲突）——但有了 P0，即便保持现状，ambiguous 也终于在 wire 上可见了。**P2 非必需**，P0+P1 已解 issue。

---

## 5. 优先级与兼容性

| 优先级 | 改动 | 解决 | 破坏性 |
|---|---|---|---|
| **P0** | logger→stream 桥 + 定点 ErrorEvent + 非零兜底 | issue 的"零诊断"（所有 exit-1 路径） | 无（CLI 不装 sink；纯增量） |
| **P1** | rebuild/sync fail-soft + 上报跳过 | "≥2 repo 就崩"的结构根因 | 行为更健壮：坏 repo 从"整库 fail"→"跳过+warn"；单 repo 配置零影响 |
| P2 | 声明顺序 first-match（配置开关） | 跨 repo 同名裸名 | 默认关，opt-in |

**建议落地顺序**：先 P0（小、单独就让 issue 症状消失、可测不变量防回归），P1 紧随（真正让多命名空间 workspace 稳），P2 视需要。

**关键兼容性**：P1 把"坏 repo 致命"改成"best-effort"，与同函数内 sub-index 的既有语义一致，不是新范式。唯一需盯的：坏 repo 被跳过后若目标恰只在坏 repo 里，则应是**结构化 NotFound**（P0 保证），而非静默——这正是 §6 要断言的。

---

## 6. 验证 / 测试

新增 e2e（模仿 `tests/e2e/install_silent_failure_test.sh`），三条断言：

1. **不变量**：任意导致非零 exit 的 interface 调用，NDJSON 流里**必有 ≥1 个 `kind":"error"` 或 `level":"error"` 事件**（P0 兜底）。
2. **§3 复现回归**：双 repo（第 2 个无 `pkgs/`）+ 目标在健康索引 → **exit 0** + 一条"repo skipped"的 warn/error 事件（P1）。
3. **真缺失**：双 repo + 目标哪个 repo 都没有 → **exit 非零** + 结构化 `NotFound` 事件（含搜索过的 repo 列表）。

复现脚手架（已验证可用）：本地 fixture 索引 + 绝对路径 index_repos + `XLINGS_INDEX_SOURCE=git`（免 artifact/网络）+ 只读 `plan_install`。

## 7. 涉及面

`log::error` 静默 + 命令层不发 `ErrorEvent` 影响**所有** xim capability 的失败可观测性，不止 install：`cmd_search/cmd_remove/cmd_update/cmd_info/cmd_list` 全部同款（都用 `log::error(...); return 1;`）。P0 的 logger→stream 桥一次性覆盖它们。`cmd_update` 内部调 `cmd_install`，额外继承本 bug。

## 8. 与 2026-05-22 静默失败的区别

| | 2026-05-22（已修） | 本 issue #374 |
|---|---|---|
| 阶段 | 下载/安装（per-package） | 解析/建库（catalog） |
| exit | **0**（假成功） | **1**（真失败但无诊断） |
| 根因 | `failedCount` 不进退出码 | catalog fail-fast + `log::error` 被 TUI 吞 |
| 现状 | 已修（`commands.cppm:456`） | 本文档提案 |

两者共同暴露一件事：**interface 契约声称"事件描述过程、result 携带码"，但命令层的失败诊断没进事件通道。** P0 是这条契约的系统性补齐。
