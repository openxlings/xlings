# 稳健性与可用性优化 · 实施计划(2026.9.12.1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 一个 PR 落地 `2026-09-12-robustness-and-usability-optimization-plan.md` 的 P0–P5:提示只在影响用户时说且只说一次;`local` 变成带来源、可列可清的覆盖层;`remove` 无论如何都能删干净;doctor 默认报告说真话、`--fix` 不用进 subos 也能收敛;一个坏条目不拖垮别的命令;新契约在三平台都有 e2e。

**Architecture:** 不动 DB / shim 表 / 索引格式。新增两个小模块(`xlings.core.notice`:一次性提示;`xlings.core.xim.overlay`:本地 recipe 的 provenance 与 GC),其余是对 `remove`、`doctor`、`catalog`、`installer` 的定点修改。跨 subos 的修复用**子进程 + `--subos <name>`** 实现(`Config::set_active_subos_override` 已存在,但 doctor 的 500 行命令体里有多处进程级缓存,进程边界最安全)。

**Tech Stack:** C++23 modules(gcc 16.1.0 via `mcpp build` / `mcpp test`,见 memory `reference_build_xlings_via_mcpp`)、gtest 单测、bash e2e(`tests/e2e/*_test.sh`,`project_test_lib.sh` 提供 `find_xlings_bin` / `require_fixture_index`)、GitHub Actions 三平台。

**Spec:** `.agents/docs/2026-09-12-robustness-and-usability-optimization-plan.md`(§3 方案、§5 不变量 I1–I9、§7 定案)。

## Global Constraints

- 版本号 **`2026.9.12.1`**,两处同时改:`mcpp.toml` `version =` 与 `src/core/config.cppm` `Info::VERSION`。改完 `mcpp build`,用 `<bin> --version` 确认后再跑任何 e2e(memory `reference_version_bump_two_places`)。
- 构建/测试只用 `mcpp build` / `mcpp test`;**不要** `mcpp clean`;二进制取 `target/*/*/bin/xlings` 里 `--version` 报 2026.9.12.1 的那个,e2e 用**绝对路径** `XLINGS_BIN=`。
- 断言不变量,不断言版本号/文案措辞(memory `feedback_assert_only_invariants`、`feedback_test_pins_spelling_not_property`)。
- 新 e2e 必须登记进 `tests/e2e/run_all.sh` 的 `TESTS` 数组(否则 run_all 的 orphan 检查 FATAL)。
- 所有真机验证只读;要动 home 就用 `.agents/tools/slice-real-home.sh` 或 e2e 的隔离 home,`--fix` 不对切片跑(memory `reference_repro_from_real_home_slice`)。
- 打印的命令必须能原样运行(memory `reference_printed_commands_must_be_runnable`)。
- 提交信息用 `git commit -F -` + 带引号 heredoc(memory `feedback_commit_messages_via_heredoc`)。
- 每个 task 结束:`mcpp build && mcpp test` 绿,相关 e2e 绿,然后提交。

---

## 文件地图

| 文件 | 责任 | 变更 |
|---|---|---|
| `mcpp.toml`, `src/core/config.cppm` | 版本号;`[profile.asan]` | T0, T9 |
| `src/core/notice.cppm` / `notice.cpp` (新) | `notice_once(id, Diagnostic)`:落盘 memo 的一次性 Note | T1 |
| `src/core/config.cppm` / `config.cpp` | `recorded_verified_version()` / `record_verified_version()`(键 `verifiedBy`) | T2 |
| `src/core/xself/repair.cppm` / `repair.cpp` | 删 `print_migration_hint_once`;`migration_hint` 改签名 | T2 |
| `src/core/xself/update.cpp` | 删无条件两行 | T2 |
| `src/core/xvm/commands.cpp`, `src/core/xim/commands.cpp` | 删 4 处 hint 调用 | T2 |
| `src/cli.cpp` | 升级一次性提示;`remove` 新 flag;`config --list/--remove/--clear-xpkg` | T2, T4, T5 |
| `src/core/xim/catalog.cpp` | `announce_demotion_` → 纯重复不说、否则 Note 一次 | T3 |
| `src/core/xim/overlay.cppm` / `overlay.cpp` (新) | provenance、identical 判定、GC、状态 | T4 |
| `src/core/xim/commands.cpp` | `cmd_add_xpkg` 接 overlay;新 `cmd_list_xpkg`/`cmd_remove_xpkg`/`cmd_clear_xpkg`;`cmd_update` 后 GC;`cmd_remove` 重写解析与范围 | T4, T5 |
| `src/core/xim/installer.cppm` / `installer.cpp` | `uninstall` 容错(缺 recipe、hook 失败);`UninstallOutcome` 新字段;sysroot 刷新到 pinning subos | T5, T7 |
| `src/core/xself/doctor.cppm` / `doctor.cpp`, `src/core/xself.cpp` | D1 probe 常驻;D2 unclaimed→prune;`--subos`;`--show-ok`;`--fix` 跨 subos 子进程;stamp 条件 | T6 |
| `src/core/profile.cppm` / `profile.cpp` | `load_subos_snapshots` 报告不可读 subos | T8 |
| `src/cli/spec.cpp`, `docs/generated/command-reference.md` | 新 flag 的规范与生成文档 | T4, T5, T6, T10 |
| `tests/unit/test_notice.cpp` (新), `tests/unit/test_xim_overlay.cpp` (新), `tests/unit/test_self_repair.cpp`, `tests/unit/test_xim_catalog.cpp` | 单测 | T1–T4 |
| `tests/e2e/notice_once_test.sh`, `local_overlay_test.sh`, `remove_force_contract_test.sh`, `doctor_cross_subos_fix_test.sh`, `doctor_remedy_mode_parity_test.sh`, `broken_home_isolation_test.sh`, `sysroot_refresh_pinning_subos_test.sh` (均新), `entry_binary_and_isolation_test.sh` (改), `run_all.sh` | e2e | T2–T8 |
| `.github/workflows/xlings-ci-macos.yml`, `xlings-ci-linux.yml`(asan job) | 平台覆盖 | T9 |
| `docs/quick-start/multi-version.md`, `.agents/docs/*` | 用户文档、发布记录 | T10 |

---

### Task 0: 版本号与计划入库

**Files:** Modify `mcpp.toml:32`, `src/core/config.cppm:13`. Add `.agents/docs/2026-09-12-*.md`(两份)。

- [ ] `sed -i 's/^version = "2026.9.5.1"/version = "2026.9.12.1"/' mcpp.toml && sed -i 's/VERSION = "2026.9.5.1"/VERSION = "2026.9.12.1"/' src/core/config.cppm`
- [ ] `mcpp build`;`B=$(find target -path '*/bin/xlings' -type f -printf '%T@ %p\n'|sort -rn|head -1|cut -d' ' -f2); $B --version` → `xlings 2026.9.12.1`
- [ ] Commit: `chore: 2026.9.12.1 — robustness/usability round, plan docs`

---

### Task 1: `notice_once` —— 一个 Note 在一个 home 里只说一次

**Files:** Create `src/core/notice.cppm`, `src/core/notice.cpp`; Modify `src/core.cppm`(export 列表,照 `xlings.core.diag` 的写法);Test `tests/unit/test_notice.cpp`。

**Interfaces (Produces):**
```cpp
export module xlings.core.notice;
import std; import xlings.core.diag;
export namespace xlings::notice {
// Injected memo so the decision table is testable without a home.
struct Memo {
    std::function<bool(std::string_view id)> seen;
    std::function<void(std::string_view id)> mark;   // may throw; swallowed
};
// Returns true when it printed. Key = id + "\x1f" + fingerprint.
bool notice_once(const Memo& memo, std::string_view id, std::string_view fingerprint,
                 const diag::Diagnostic& d);
// Bound to Config::hint_seen / Config::mark_hint_seen.
bool notice_once(std::string_view id, std::string_view fingerprint, const diag::Diagnostic& d);
Memo config_memo();
}
```
Rules: `d.level` forced to `Note`; if `seen(key)` → return false; else `diag::emit(d)`, then `mark(key)` in try/catch (a read-only home must not turn a note into a crash); return true. `xlings.core.notice` imports `xlings.core.config` for the bound overload (config does not import diag, no cycle).

- [ ] 写 `tests/unit/test_notice.cpp`:三个 case —— `PrintsOnceThenRemembers`(fake memo:第一次 true 且 mark 被调一次,第二次 false 且不 emit)、`DifferentFingerprintPrintsAgain`、`MarkThrowingDoesNotPropagate`。
- [ ] `mcpp test --filter Notice*` 红 → 实现 → 绿。
- [ ] Commit: `feat(notice): a note is said once per home, keyed by what triggered it`

---

### Task 2: 迁移提示按观察触发,升级只提醒一次

**Files:** Modify `src/core/config.cppm:579-594`, `src/core/config.cpp:1272-1310`, `src/core/xself/repair.cppm:182`, `repair.cpp:34-49,139-147`, `update.cpp:137-140`, `xvm/commands.cpp:973`, `xim/commands.cpp:875,1504,1593`, `xself/doctor.cpp:4441-4443,4914`, `src/cli.cpp:315-335` 附近。Test `tests/unit/test_self_repair.cpp`(migration_hint 用例)、`tests/e2e/notice_once_test.sh` (新)。

**Interfaces:**
- Config 新增(与 `recorded_client_version` 同形,键 `"verifiedBy"`):`static std::string recorded_verified_version();` `static std::expected<void,std::string> record_verified_version(const std::string&);`
- `migration_hint(recorded, running)` 保留(纯函数,doctor 报告用),文案改为事实句:`"set up by {}; last verified by {}"`,**不再**包含 `run xlings self doctor --fix`(它只在 doctor 报告里出现,那句是重复)。

- [ ] 删 `print_migration_hint_once`(声明+定义+4 个调用)和 `update.cpp:137-140` 三行(连同其注释块的最后一段)。
- [ ] `doctor.cpp:4914` 条件 `outstanding == 0 && after.foreignPayloads == 0` → `outstanding == 0`,改为 `record_verified_version(Info::VERSION)`;`self install`(`xself/install.cpp:807-820`)不动。
- [ ] `doctor.cpp:4441`:`migration_hint(Config::recorded_client_version(), verified.empty() ? Info::VERSION : verified)`,仅当 `recorded_verified_version() != Info::VERSION` 时加字段 `"home"`(不是 "migration")。
- [ ] `cli.cpp`:新增 `show_upgrade_notice_once_()`,在 `show_interactive_hint_once_` 旁;条件:TTY(`platform::supports_rewrite_output()`)且顶层命令 ∉ {`self`, `interface`, `--version`, `-h`};`fp = Info::VERSION`;`notice::notice_once("client.upgraded", fp, {.code="self.upgraded", .summary=std::format("xlings is now {}; this home was last verified by {}", VERSION, verified_or_setup), .actions={{"check the home", "xlings self doctor"}}})`。只在 `recorded_verified_version()`(回退 `recorded_client_version()`)与 `Info::VERSION` 不同时调用。
- [ ] e2e `notice_once_test.sh`(I1、I2):隔离 home,`self init`,把 `.xlings.json["version"]` 改成 `"v0.4.40"`;`RUN list` 两次:第一次 stderr 含 `xlings is now` 至多 1 次,第二次 0 次;`RUN self doctor --fix -y`(fixture home 无 finding)退出 0 后 `python3 -c` 读 `verifiedBy == 2026.9.12.1`;之后 `RUN list` 不含 `xlings is now` 也不含 `self doctor --fix`;`strings "$XLINGS_BIN" | grep -c "packages installed by the previous client"` 为 0。登记 `"E2E-100|notice_once_test.sh||"`。
- [ ] 单测:`test_self_repair.cpp` 里 migration_hint 的用例改断言新文案不含 `--fix`。
- [ ] Commit: `fix(hints): the migration nudge is a doctor fact, and an upgrade is announced once`

---

### Task 3: 降级警告:纯重复不说,其余说一次

**Files:** Modify `src/core/xim/catalog.cpp:543-570`, `catalog.cppm`(import notice);Test `tests/unit/test_xim_catalog.cpp:821-907`、`tests/e2e/entry_binary_and_isolation_test.sh:100-128`。

- [ ] `announce_demotion_`:先算 `allDuplicates = ranges::all_of(chosen.demotedVersions, ==chosen.version)` —— 需要把 `NamespaceRankResult_::demoted` 从字符串改为 `struct {std::string coordinate; std::string version;}`(`catalog.cpp:170-192`、`PackageMatch::demoted` 同步;打印处用 `.coordinate`)。`allDuplicates` → return(不打、不记)。否则 `notice::notice_once("catalog.demoted", target+"\x1f"+chosen.canonicalName+"\x1f"+losers, {.code="xim.namespace_priority", .summary=std::format("'{}' also provided by {}; selected {} by namespace priority (local ranks last)", ...), .actions={{"pick the other", std::format("xlings install {}", chosen.demoted.front().coordinate)}}})`。进程内 `demotionsAnnounced_` 保留。
- [ ] 单测新增 `DuplicateLocalCopyIsSilent`(同版本 → `demoted` 非空但 render 不调用;用 `Memo` 注入计数)与 `BehindLocalCopyNotesOnce`。
- [ ] e2e `entry_binary_and_isolation_test.sh` S1/S2:两次运行,第一次含 `namespace priority`,第二次不含;新增 S3:local 与 xim 同版本 → 两次都不含。
- [ ] Commit: `fix(catalog): a local copy identical to the index is not a conflict; a real one is said once`

---

### Task 4: `local` 覆盖层:provenance、三个动词、identical GC

**Files:** Create `src/core/xim/overlay.cppm`, `overlay.cpp`; Modify `src/core/xim/commands.cppm/.cpp`(`cmd_add_xpkg:1886-1987`、`cmd_update:~1995`)、`src/cli.cpp:881-885,1780-1785`、`src/cli/spec.cpp:38`、`src/core.cppm`;Test `tests/unit/test_xim_overlay.cpp`(新)、`tests/e2e/local_overlay_test.sh`(新)。

**Interfaces (Produces):**
```cpp
export module xlings.core.xim.overlay;
export namespace xlings::xim::overlay {
struct Entry { std::string name, source, addedAt, sha256, version; };
struct Status { enum Kind { Unique, Identical, Modified, Behind } kind; std::string upstreamRepo, upstreamVersion; };
std::filesystem::path dir();                         // Config::global_data_dir()/"xim-pkgindex-local"
std::filesystem::path recipe_path(const std::filesystem::path& dir, std::string_view name); // pkgs/<l>/<name>.lua
std::map<std::string, Entry> load(const std::filesystem::path& dir);      // .overlay.json, missing → {}
void save(const std::filesystem::path& dir, const std::map<std::string, Entry>&);
std::string file_sha256(const std::filesystem::path&);
// Candidate upstream recipe files with the same name, from every non-local global repo dir.
std::vector<std::pair<std::string /*repo*/, std::filesystem::path>> upstream_candidates(std::string_view name);
Status status_of(const Entry&, const std::filesystem::path& localFile);   // compares sha256 / declared latest version
std::vector<std::string> gc_identical(const std::filesystem::path& dir);  // removes Identical files + entries, returns names
}
```
Version comparison uses `version_order::compare`(存在于 `xlings.core.version_order`);"declared latest" 从 `xpkg::load_package(file)->xpm` 取 `latest` ref,没有则空(Status 退化为 Unique/Identical/Modified)。

- [ ] 单测 `test_xim_overlay.cpp`:临时目录里造 `pkgs/a/alpha.lua` 与一个假 upstream 目录;`IdenticalBySha`、`ModifiedWhenBytesDiffer`、`BehindWhenUpstreamNewer`、`GcRemovesOnlyIdentical`、`LoadSaveRoundTrip`。`upstream_candidates` 的 repo 列表用注入参数版本 `upstream_candidates(name, std::span<const RepoDir>)` 便于测试。
- [ ] `cmd_add_xpkg`:校验通过后、搬到 letter 目录前:`sha = file_sha256(luaFile)`;若任一 upstream 同名文件 sha 相同 → 删掉刚拷的文件,`diag::emit(Note, "xim.overlay_identical", summary "{name} is identical to {repo}:{name}; nothing to add", actions {{"install it", "xlings install {name}"}})`,返回 0。否则写 provenance(`source` = 用户给的路径或 URL,`addedAt` ISO 时间,`version` = declared latest)。
- [ ] 新命令:`cmd_list_xpkg(stream)`(表:name / version / status / source,`DataEvent{"table"...}` 照 `cmd_list` 的表事件形状,或直接 `log::println` 对齐列),`cmd_remove_xpkg(name)`(删文件+条目,`catalog.rebuild()`,找不到 → error 退出 1),`cmd_clear_xpkg(what)`(`all` | `stale` = Identical+Behind;打印删了哪些)。
- [ ] `cli.cpp` `config`:`--list-xpkg`(flag)、`--remove-xpkg <NAME>`、`--clear-xpkg <WHAT>`;`spec.cpp` 同步三条;重新生成 `docs/generated/command-reference.md`(`python3 tests/scripts/test_generated_command_reference.py --xlings <bin> --write`)。
- [ ] `cmd_update`:`catalog.rebuild(true)` 成功后 `auto gone = overlay::gc_identical(overlay::dir()); if (!gone.empty()) log::println("{} local recipe(s) identical to the synced index were removed: {}", ...)`,再 `rebuild(true)` 一次。
- [ ] e2e `local_overlay_test.sh`:用 fixture index 做 home;(a) `config --add-xpkg <fixture 里某 recipe 原件>` → 输出含 `identical`,`pkgs/` 下没有该文件;(b) 拷贝并改一行注释再 add → `--list-xpkg` 显示 `modified`;(c) 把 upstream 的 latest 改高 → `stale`;`--clear-xpkg stale` 删掉;(d) `--remove-xpkg` 删 unique;(e) I6:同版本副本存在时 `info <bare>` stderr 不含 `namespace priority`。登记 `E2E-101`。
- [ ] Commit: `feat(xim): the local index is an overlay with provenance — list, remove, clear, and GC what the index already has`

---

### Task 5: `remove`:先撤注册,`--force` 意味着无论如何都要没,范围可跨 subos

**Files:** Modify `src/core/xim/installer.cppm`(`UninstallOutcome`)、`installer.cpp:3226-3430`、`src/core/xim/commands.cppm:142`、`commands.cpp:921-1450`、`src/cli.cpp:1636-1656`、`spec.cpp:24-26`;Test `tests/e2e/remove_force_contract_test.sh`(新)。

**Interfaces:**
```cpp
struct UninstallOutcome { bool detachedOnly; std::string target, version;
    std::string hookFailure;      // non-empty: the recipe's uninstall() failed; state was withdrawn anyway
    bool recipeUnavailable{false}; // no index provides the recipe; hook skipped
};
int cmd_remove(const std::string& target, bool yes, EventStream&, bool force,
               bool all, std::optional<std::string> subosScope /* "" = current, "*" = all */);
```

- [ ] `Installer::uninstall`(a)`catalog_->resolve_target` 失败时不返回:从 `Config::versions_mut()` 取 `get_vinfo(targetName)`;版本 = `requestedVersion` 或(仅一个版本时)那个;`installDir` = `xvm::expand_path(vdata.path, home)` 向上找到 `xpkgs/<store>/<ver>` 两级(复用 `xvm::coordinate_from_payload_path` 的路径解析或 `profile.cpp:230-250` 的 `xpkgs/` 截取逻辑);`recipeUnavailable = true`,`useDefaultRemoval = true`,跳过 executor。找不到 DB 记录才 `unexpected`。(b)hook `!result.success` → `outcome.hookFailure = format_hook_failure(...)`,`useDefaultRemoval = true`,继续。(c)`xvm_ops` 为空且 `useDefaultRemoval` → push `remove detachTarget@detachVersion`(现有逻辑)。
- [ ] `cmd_remove` 解析改 **DB 优先**:无 `@` 时:`active` → 用;否则 DB 里恰一个版本 → 用;多个且 `!all` → 列出 + `xlings remove X --all` 提示,退出 2。`--all`:对 DB 里每个版本依次 `installer.uninstall(name@ver)`(从高到低)。`resolve_target` 只用于显示名与 payload 检查;失败不阻断。
- [ ] 范围:`subosScope=="*"` 或(未指定且当前 subos 没有而别的有且 `yes`)→ `for name in profile::find_subos_referencing(home, bare)`:`auto prev = Config::set_active_subos_override(name); ... uninstall ...; Config::set_active_subos_override(prev)`。`--subos NAME` 单个。membership guard 的 diag 加 action `{"remove it everywhere", "xlings remove {} --all-subos"}`。
- [ ] 结果处理:`hookFailure` 非空:`force` → `diag Note "xim.uninstall_hook_failed"`(facts: hook stderr 首行),返回 0;否则 `diag Warn` 同 code,facts 含 "state withdrawn: yes",actions `{"finish anyway", "xlings remove {} --force -y"}`,返回 1。`recipeUnavailable` → Note `xim.uninstall_recipe_unavailable`。
- [ ] `cli.cpp`:`--all`、`--all-subos`、`--subos <NAME>`;`--force` help 改 "Remove even if packages depend on it, the recipe is gone, or its uninstall hook fails";`spec.cpp` 同步。
- [ ] e2e `remove_force_contract_test.sh`(I4,fixture index 需两个新 recipe:`hookfail.lua` 的 `uninstall()` 里 `error("boom")`;`plain.lua` 正常;建到 `tests/fixtures/xim-pkgindex/pkgs/`(检查 `prepare_fixture_index.sh` 怎么消费 fixture)):
  S1 hook 失败:`remove hookfail -y` 退出 1,但 DB/workspace/shim/store 里都没有它;S2 `remove hookfail --force -y` 在装好的副本上退出 0;S3 recipe 删掉后 `remove plain --force -y` 退出 0 且干净;S4 payload 先 `rm -rf` 再 remove 退出 0 且 DB 干净;S5 #578:DB 有一条记录、workspace 无 active,`remove plain -y` 删掉那条;S6 两个 subos 都装了 plain,`remove plain --all-subos -y` 从 default 跑完两个都没有、store 也没有;S7 两个版本 `remove plain --all -y`。每个 S 后用一个 `assert_gone name ver` 函数检查四处。登记 `E2E-102`。
- [ ] Commit: `fix(remove): withdrawal first, then the recipe's goodbye — --force/--all/--all-subos make "remove" mean removed`

---

### Task 6: doctor:默认报告说真话,未引用即 prune,`--fix` 跨 subos

**Files:** Modify `src/core/xself/doctor.cppm`(`Finding::unclaimed`, `cmd_doctor` 签名加 `std::optional<std::string> subos, bool showOk`)、`doctor.cpp:1340-1375, 3125-3210, 3661-3770, 3986, 4419, 4470-4620, 4780-4930`、`src/core/xself.cpp:129-184`、`spec.cpp:56`;Test `tests/e2e/doctor_remedy_mode_parity_test.sh`、`doctor_cross_subos_fix_test.sh`(新),`doctor_fix_convergence_test.sh`(扩)。

- [ ] **D1**:`doctor.cpp:4564` 的 `if (deepAudit || fix)` → 无条件 `localCatalog.emplace(); if (!rebuild) localCatalog.reset();`(`--scope` 分支保持在里面)。`:1366` `if (audit.deep)` 去掉门 → 总是算 remedy。渲染 `:3986`:`localCatalog` 为空时文案 "index unavailable — could not check whether a package provides this entry"(新增一个 `Scan::probeAvailable` 标志)。
- [ ] **D2**:`Finding` 加 `bool unclaimed{false}`;`reportBrokenPayload` 在 `!owner.ownedHere && owner.otherSubos.empty() && !isActiveHere` 时置 true。`repair_payloads_`:按 coord 分组后,`covered` 全 unclaimed → 不装:dryRun 打 `prune {} (unreferenced; `xlings install {}` brings it back)`;实跑走新抽出的 `prune_one_(st, target, version, out)`(从 `prune_dead_registrations_` 抽 victims→删 DB 记录→`save_versions` 的那段),note `registration dropped — xlings install {coord} brings it back`。
- [ ] `--subos <NAME>`:`xself.cpp` 解析(同 `--scope` 的两种写法),`cmd_doctor` 开头 `if (subos) Config::set_active_subos_override(*subos);`(在 `load_state_` 之前;名字不存在 → InvalidInput 退出 2)。`--all` → `--show-ok`,`--all` 保留为别名并 `log::warn("--all now means --show-ok; ...")`(spec 里只写 `--show-ok`)。
- [ ] **跨 subos `--fix`**:在 Phase 2 之后、stamp 之前:`owners = set<string>` ← 当前 scan 里 `ForeignPayload` 的 `f.subos.front()` 与 `OtherSubos` 类 finding 的 subos;对每个 name(排序):`announce("repairing subos {} — running `{} self doctor --fix --subos {}`")`,`rc = run(std::format("{} self doctor --fix --subos {}{} {}", client, name, dryRun?" --dry-run":"", quiet_suffix()))`,记 `repair.notes`(失败列出 rc)。**递归护栏**:子进程带 `--subos` 时**不**再跨 subos。之后 `refresh()`。`--dry-run` 下只打 planned 行 `would run <cmd>`。
- [ ] `render_` `:4419`:`"{} in other subos — --fix repairs them there"`。stamp 条件已在 T2 改。
- [ ] e2e `doctor_remedy_mode_parity_test.sh`(I8):fixture home 装一个包,`rm -rf` 其 payload 的 `bin`;`self doctor` 与 `self doctor --deep` 各抓 `→ run` 行,`diff` 为空;且默认模式不含 "no package in any index provides"。`E2E-103`。
- [ ] e2e `doctor_cross_subos_fix_test.sh`(I3 多 subos):subos `other` 装 plain,破坏 payload;从 `default`:`self doctor` 报 1 条 foreign;`self doctor --fix -y` 退出 0 且**不需要** `subos use`;再跑一次 `--fix --dry-run` 无 `would run`。加 I9:一条 unclaimed 记录(手工写进 DB、无 workspace 引用、payload 删掉)→ `--fix --dry-run` 输出含 `prune` 且不含 `would run .*install`。`E2E-104`。
- [ ] Commit: `fix(doctor): the plain report tells the truth, an unreferenced payload is pruned not downloaded, and --fix walks every subos`

---

### Task 7: 安装侧把 sysroot 刷新到每个 pinning 该版本的 subos(#586)

**Files:** Modify `src/core/xim/installer.cpp:1930-2075`;Test `tests/e2e/sysroot_refresh_pinning_subos_test.sh`(新)。

- [ ] 把 effect 循环体抽成 `auto place_effects_into = [&](const fs::path& subosDir, const xvm::Workspace& ws, const xvm::WorkspaceInstalled& wsi) { ... }`,当前调用不变。
- [ ] 循环后:`for (name : profile::find_subos_pinning_version(home, target, version))` 排除当前;`auto prev = Config::set_active_subos_override(name); place_effects_into(Config::paths().subosDir, Config::workspace(), Config::workspace_installed()); Config::set_active_subos_override(prev);`。只对该 subos 里 **active** 的版本放置(`resolved->active` 的门在 lambda 内按传入的 ws 判断,自然成立)。
- [ ] e2e:两个 subos 都 `install lib@1 -u`(fixture 里带 lib 节点的 recipe,复用 `xvm_library_switch_test.sh` 用的包);在 default 里 `remove lib -y && install lib@1 -u`(触发注册重写);断言 `subos/other/lib/*.so*` 全部 `-e`(`find -xtype l` 为空)。`E2E-105`。
- [ ] Commit: `fix(installer): a registration change refreshes the sysroot of every subos that pins it (#586)`

---

### Task 8: 失败域 = 一个包(坏 home fixture)

**Files:** Modify `src/core/profile.cppm/.cpp:157-187`(`load_subos_snapshots(home, std::vector<std::string>* unreadable = nullptr)`)、`src/core/xself/doctor.cpp`(`SubosUnreadable` Warning finding + `FindingKind`)、`src/core/xim/commands.cpp` `cmd_list --all` 一行 note;Test `tests/e2e/broken_home_isolation_test.sh`(新)。

- [ ] `load_subos_snapshots`:`parse` 失败 / `catch` / 缺 `workspace` → `if (unreadable) unreadable->push_back(name)`,继续。doctor `load_state_` 收集进 `DoctorState::unreadableSubos`,`detect_` 每个出一条 `SubosUnreadable`(Warning,detail 指向文件,remedy 空,remedyNote "fix the JSON by hand or `xlings subos remove <name>`")。
- [ ] e2e(I5):fixture home 装 `plain`、`hookfail`;注入:`subos/broken/.xlings.json` 写 `{garbage`;DB 里加 `ghost@1.0` 指向不存在的 payload;`config --add-xpkg` 一个 recipe 后删掉其文件;记录 `snap_before = (sha256 of plain payload tree + shim table listing)`;跑 `list`、`install plain -y`、`use plain 1.0`、`$HOME_DIR/subos/default/bin/plain --version`、`self doctor`,每条退出码 ∈ {0,1,2}(不是 134/139)且 stderr 无 `terminate called`;`self doctor` 输出含 `broken` subos 名;`snap_after == snap_before`。`E2E-106`。
- [ ] Commit: `fix(robustness): an unreadable subos is a finding, not an abort; a broken-home fixture proves the blast radius is one package`

---

### Task 9: 平台覆盖 + sanitizer

**Files:** Modify `.github/workflows/xlings-ci-macos.yml`(在 `subos_cmd_contract_test.sh` 步骤后加一个 "platform core contract" 步骤跑 `remove_force_contract_test.sh`、`doctor_cross_subos_fix_test.sh`、`broken_home_isolation_test.sh`、`notice_once_test.sh`)、`.github/workflows/xlings-ci-linux.yml`(新 job `unit-asan`:`mcpp test --profile asan`,`continue-on-error: true` 并在 summary 打印 verdict)、`mcpp.toml`(`[profile.asan] inherits dev, cxxflags += ["-fsanitize=address,undefined","-fno-omit-frame-pointer"], ldflags += ["-fsanitize=address,undefined"]` —— 先 `mcpp build --help` / mcpp 文档确认 profile 键名;不支持则只加 job 骨架并在 PR 里说明)、`tests/e2e/run_all.sh`(登记 E2E-100..106)。Windows:新契约的 bash e2e 依赖 `env -i` 与 POSIX 路径,**不**加入 Windows;在 PR 描述与 §7 写明留给下一轮的 ps1 port。
- [ ] 本地跑 `bash tests/e2e/run_all.sh` 全绿(或至少新增 7 条 + 改动的 2 条)。
- [ ] Commit: `ci: platform core contract on macOS; asan unit profile`

---

### Task 10: 文档与发布记录

- [ ] `docs/generated/command-reference.md` 重新生成;`docs/quick-start/multi-version.md` 的 `--add-xpkg` 段加 `--list-xpkg` / `--remove-xpkg` / `--clear-xpkg`;`docs/README.md` 若有命令索引同步。
- [ ] `.agents/docs/2026-09-12-robustness-and-usability-optimization-plan.md` §6 表加 "实施:PR #N";§8 追加实施中被推翻的判断。
- [ ] xim-pkgindex 仓:`.agents/skills/xpkg-creater/references/testing-and-acceptance.md` 第 7 步后加 "8) `xlings config --remove-xpkg <pkg>`(或留着,下次 `xlings update` 会自动清掉与索引相同的副本)" —— 单独一个小 PR。
- [ ] Commit: `docs: command reference, overlay how-to, plan status`

---

### Task 11: 自审、CI、PR、发布、真机验证

- [ ] `mcpp build && mcpp test`;`bash tests/e2e/run_all.sh`;`/code-review` 自审一次,修掉发现。
- [ ] `git push -u origin fix/robustness-usability-2026.9.12`;`gh pr create`(标题 `fix(remove,doctor,xim,hints): ... (2026.9.12.1)`,正文按 .agents/docs 惯例:改了什么、不变量、真机数字、留下的);等 6 个 workflow 全绿(fork 不需要 approval,本仓分支)。
- [ ] 合并:按合并约定 `feedback_merge_bypass_squash`。
- [ ] 发布:`gh workflow run release.yml`(按 `.agents/docs/2026-09-06-release-2026.9.5.1-notes.md` 的步骤);release asset 一出即 `gtc` 补 gitcode(memory `project_ecosystem_release_chain`、`reference_gitcode_release_download_verify`),不等 mirror job;bump xim-pkgindex 的 xlings 条目(memory `project_release_cancel_recovery` 的手动 bump 配方)。
- [ ] 真机验证(只读或隔离):`xlings config --mirror CN`;`xlings self update` 到 2026.9.12.1;`xlings self doctor` 的 "no package in any index provides" 计数(期望 3,与 `--deep` 一致);`xlings subos <name> --sandbox --cmd "gcc --version"`、`--cmd "python3 --version"` 等生态命令;`xlings config --list-xpkg` 看到 157 identical;写 `.agents/docs/2026-09-12-release-2026.9.12.1-notes.md`。
