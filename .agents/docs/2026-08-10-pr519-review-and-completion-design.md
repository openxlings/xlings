# PR #519 深度评审与收尾设计

> 状态：**Measured & implemented** —— §0 的决策已执行，§2 的每条发现都有实测结论，其中**两条我的判断被测量推翻**（R5 误判、R2(b) 不成立），一条测量出了原判断没预见的更大问题（R4 的 143 条误报）。
>
> 日期：2026-08-10
>
> 评审对象：`openxlings/xlings#519`（分支 `fix/stability-regression-recovery`，远端 HEAD `a6d13e2`，20 commits，44 files，+6393/-542）
>
> 关联：#506（open）、#513 / #514（closed）、#518（架构讨论）、`mcpp-community/mcpp#392`
>
> 前置计划：`.agents/plans/2026-08-09-stability-regression-recovery-implementation.md`（Task 1–16）
>
> 前置设计：`.agents/docs/2026-08-09-stability-regression-recovery-design.md`

---

## 0. 结论先行

PR #519 的 Task 1–10 已经落地，工程质量高：热路径副作用被真正从调用图里移除（而不是加缓存/超时掩盖），
`BindingSelectionResolver` 把 O(n²) 的 incoming-edge 重建改成一次构建并暴露计数器供测试断言结构而非计时，
D5 把「冷声明」和「损坏状态」分开——这几处都恰好命中了本仓库反复出现的缺陷形态。

但**它现在还不能进入发布流程**，有六个决定要先做：

| 编号 | 决定 | 结论 |
|---|---|---|
| **D0** | 合并前先测 R0 | ✅ **实测确认是真回归，已修**。真实 home 上 35 个 SubOS 只有 2 个有 runtime 声明，20 个「无声明 + 有 active glibc」会被升级盖错 |
| **D1** | 先修 R1（`subos use` 静默成功） | ✅ **已修 + 4 条 E2E**。这是唯一会让 Task 16 验收命令自己假通过的缺陷 |
| **D2** | **保持单 PR #519**（用户决策） | ✅ 全部改动作为 additive commit 进入 #519。拆分要换取的「回滚粒度」改由**测量**提供：每条收紧规则在真实 home 上的增量都有数字，没有一条是盲发的 |
| **D3** | T12/T13（mcpp 代码）与 T11（xlings 发布）并行 | 计划把关键路径写成 5 段串行发布，其中至少 2 段的串行依赖不成立。见 §3.1 |
| **D4** | 每个门禁写成可证伪的数字或命令 | 见 §4。「跑基准记录 median/p95」不是判据 |
| **D5** | 版本号 `2026.8.10.1` | ✅ 已 bump（`mcpp.toml` + `Config::VERSION`）。tags 最新是 `v2026.8.9.2`，今天 2026-08-10 |
| **D6** | 两份从未提交的 `.agents/docs` 落盘 | 待办，见 §6 |

---

## 1. 已核实的当前状态

以下每条都实际查过，不是从 PR 正文转述的。

**分支与基线**

- `origin/main` 是 `origin/fix/stability-regression-recovery` 的祖先（`git merge-base --is-ancestor` 返回真），
  merge base 就是 main 的 HEAD `f203b6b`/`2913a09`。
  → **Task 11 Step 1 的「rebase-free integration audit」已经天然满足**，不需要 merge main。
- 版本号：PR 分支与 main 的 `mcpp.toml` 与 `Config::VERSION` 都是 `2026.8.9.2`。发布 bump 尚未发生。

**CI**

- HEAD `a6d13e2` 上：aarch64 cross/QEMU 与 native aarch64 已 SUCCESS，另有一个 `build-and-test` SUCCESS；
  其余 4 个（含 Linux e2e、Windows/macOS build-and-test）仍 IN_PROGRESS。
- `mergeStateStatus = BLOCKED`，`reviewDecision = REVIEW_REQUIRED`，PR 仍是 Draft。

**工作区**

- `.agents/docs/2026-08-09-stability-regression-recovery-design.md` 在工作区是 **untracked 且内容比 PR 里的旧**
  （工作区写「状态：Proposed，待 review」「`mcpplibs/libxpkg`」；PR 里已是「Accepted for implementation」「`openxlings/libxpkg`」）。
  切到 `fix/stability-regression-recovery` 时这个 untracked 文件会挡住 checkout。
- 另外两份 doc 在**任何分支上都不存在**（`git log --all -- <path>` 为空）：
  - `2026-08-09-instant-query-and-userspace-os-architecture-design.md`——即时查询与用户态 OS 契约设计（11 章）
  - `2026-08-09-open-issues-deep-triage.md`——44 条 open issue 的全量触诊与收敛处置

**issue**

- #506 仍 open；#513 / #514 已 closed（但发布链验证未完成）；#518 是刻意分离的架构讨论。

---

## 1.5 实测结果（2026-08-10，真实 71 GB home 的 hardlink 切片）

切片由 `.agents/tools/slice-real-home.sh` 构建；跑完用 `verify-untouched` 证明真实 store 一个字节都没动
（`OK: ~/.xlings/data/xpkgs unchanged`）。

**本轮最大的成果，也是本 PR 的立身之本：**

| 命令 | 2026.8.9.2 | 本 PR | |
|---|---|---|---|
| `self doctor`（默认） | **195.16s** | **0.63s** | **310×** |
| `self doctor --deep` | （旧版默认就等于 deep） | 196.95s | 昂贵审计现在是显式的 |

**每条收紧规则在真实 home 上的增量：**

| 规则 | 改前 | 改后 | 判断 |
|---|---|---|---|
| `loader/libc split`（首次实现） | baseline 报 2 broken payload | **147** | ❌ 145 条里 143 条是误报 |
| 同上（修掉 interp 视图误判后） | | **4** | ✅ 多出的 2 条是真的（godot） |
| `subos runtime` D5 | 老 home 静默 | Error + `use` 被拒 | ❌ 真回归，已修 |

多出的那 2 条长这样，是旧规则**结构性看不到**的：

```
xim-x-godot/4.6.3/godot: its interpreter and its xim-x-glibc libraries come from different payloads
    interpreter -> .../data/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
    RUNPATH     -> /usr/lib/x86_64-linux-gnu
    core file   -> /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
```

我们的 loader 配宿主的 libc —— 正是 crash-before-main 那一类。旧规则先按 provider 过滤，
`/usr/lib/...` 没有 provider，于是永远不参与比较。

---

## 2. 代码评审发现

按严重度排列。每条给出：现象 → 证据 → 为什么重要 → 实测结论。

> **两条被测量推翻。** R5 我把 `payload_of` 的返回读反了（它返回原始拼写，和 `store_root_of` 本来就一致）；
> R2(b) 的前提不成立（`by_identity` 只在 store 名完全一致时命中，所以身份不会被改写）。
> 两条都保留在下面并标注，因为「我以为的问题」和「实际的问题」不是同一个，这个差本身值得留档。

### R0（高）✅ 已确认并修复 —— 升级过的老 home 被 D5 判成 Error，且无法用 `use` 走出来

**三个改动叠在一起才产生这个形态，单看每一个都合理。**

**第一层——老 home 升级时会被写进一个它从没声明过的 runtime。**

`src/core/xself/init.cppm:193`（`self install` 路径，即每次升级都会走）：

```cpp
if (mf::validate_block(json).empty()) return;          // block 合法 → 不动
json[std::string(mf::BLOCK)] =
    mf::make_block(mf::preserved_runtime(json, mf::DEFAULT_RUNTIME), …);
```

`preserved_runtime(doc, fallback)` 只在 doc **已经有** `runtime` 且是合法 binding 时保留它，否则返回
fallback = `DEFAULT_RUNTIME = "glibc@2.44"`（`manifest.cppm:56`）。
一个 2026.8.9.1 之前创建的 SubOS 根本没有 `subos_info` 块 → `validate_block` 非空 → 块被写入 →
**runtime 被记成 `glibc@2.44`**，无论这台机器实际在用哪个 glibc。
`doctor.cppm:1846` 的 `--fix` 修复路径同理。

所以「C1 只影响新建 SubOS」这个说法只对**已经有合法 block** 的 home 成立；对更老的 home 不成立。

**第二层——#519 把 D5 的判据从「装没装」换成「是不是 active」，并从 Warning 提成 Error。**

main（`doctor.cppm:654`）：

```cpp
// D5 — the declared runtime is not installed here.
if (mf::is_binding(info.runtime) && !installed(info.runtime)) {
    .level = FindingLevel::Warning,
```

`installed()` 问的是「全局 DB 里有没有这个版本」。声明 2.44、active 2.39、而 2.44 的 payload 也在 →
main 上**一条 finding 都不出**。

#519 改成 `check_runtime_activation(info, activeVersion, payloadExists)`：
声明 2.44 而 active 是 `xim:2.39` → `version_is_active("2.44","xim:2.39")` 为假 → 产生 mismatch；
`unmaterializedDeclaration = activeVersion.empty() && payloadMissing` → active 非空 → **false** →
`FindingLevel::Error`。`count_` 把它计入 `c.broken`，`doctor.cppm:197` 注明 Error「counts toward the exit code」。

**第三层——`xlings use` 现在会拒绝走出这个状态。**

`src/core/xvm/commands.cppm` 新增的 `runtime_activation_refused_` 在 `cmd_use` 最前面执行：
声明 2.44、请求 2.39 → 拒绝，exit 1，文案是「runtime migration is required; create a new SubOS」。

**合起来是什么**

一台从旧版本升级上来、并且当前 active glibc 不是 2.44 的机器：

- `xlings self doctor` 从「安静」变成 **Error + 非零退出码**；
- `xlings use glibc 2.39`（回到它本来在用的那个）被**拒绝**；
- 提示的唯一出路是「新建一个 SubOS」。

而「全局并存多份 glibc payload、2.39 与 2.44 同时在」正是本设计文档自己声明要支持的形态。

**为什么重要**

这不是一个会崩的 bug，是一个**会让既有 home 在升级后变成「按新规则永久不合规」**的状态迁移问题。
它同时命中两个已知形态：reporter/repairer 各说各话（doctor 说要迁移，`--fix` 只会把 block 再写一遍），
以及「一个问题多个回答者」（manifest 声明、XVM active、全局 payload 三方谁说了算）。

**实测（2026-08-10）**

真实 home 上 `subos/*/.xlings.json` 的完整普查：

| | 数量 |
|---|---|
| SubOS 总数 | **35** |
| 有 `subos_info.runtime` 声明 | **2** |
| ABSENT | **33** |
| 其中「ABSENT 且有 active glibc」= 升级后会被声明错 | **20** |

构造后的对照：

| | `self doctor` | `use glibc 2.39`（它本来就在跑的） |
|---|---|---|
| 2026.8.9.2 | 无 runtime finding | 走到 binding 计划才失败（fixture 噪声） |
| PR 519（修复前） | **✗ subos runtime Error**，remedy=「新建 SubOS」 | **`runtime activation refused`**，最顶层就拒 |

并且 `self doctor --fix` / `self install` 在无 `subos_info` 块的 home 上确实写入 `glibc@2.44`
—— 两个版本都会写，所以「盖错」是既有行为，**「盖错之后变成 Error 且走不出来」才是 #519 引入的**。

**修复（三处，全部有 E2E）**

1. `manifest::preserved_runtime` 增加第二来源 `observed_runtime(doc, family)`：没有记录过 binding 时，
   读同一个文件里 workspace 的 active 版本。**声明变成真的，而不是缺省的。**
   实测：`doctor --fix` 后老 home 声明 `glibc@2.39`（原为 `glibc@2.44`）。
   —— 「不写 runtime 字段」不可行：`validate_block` 要求 `is_binding(runtime)`，
   不写会让 block 永远无效，doctor 永远报 manifest 损坏。
2. 已经被 2026.8.9.1/.2 盖坏的 home：`doctor --fix` 采纳实际在跑的 runtime，并明说改了什么。
   只在 Error 形态下采纳 —— 「声明了但还没有 active」是冷意图（`subos new --runtime X` 尚未装 X），
   覆盖它就是撤销别人的决定。**修复读的是 detection 用的同一个函数**，不给 reporter/repairer 留分叉空间。
3. doctor 和 `use` 的提示都改成命名两条**真实存在**的出路（adopt / migrate）。
   原文案只说「新建 SubOS」，而正向迁移一直是通的 —— 激活**被声明的**版本正是这个 guard 允许的。
   那是个假死局。

E2E `subos_runtime_declaration_upgrade_test.sh` 三态：S1 legacy / S2 已盖坏 / S3 冷意图。

---

### R1（高）✅ 已修复 —— `subos use` 带修饰符但缺名字时静默成功

**现象**

`resolve_use_name_` 在 mode 分发**之前**执行，空名一律走「只读列候选、退出 0」：

```
src/core/subos.cppm:1853
    auto resolved = resolve_use_name_(name, stream);
    if (resolved.selected.empty()) return resolved.exitCode;   // 空名 → exitCode 0
    name = std::move(resolved.selected);

    if (mode == "global") { … }
    if (mode == "shell")  { … }
    return use_spawn_shell(name, …, cmd);
```

于是：

| 命令 | 旧行为 | 新行为 |
|---|---|---|
| `xlings subos use --cmd 'make'` | exit 1，`missing <name>` | **exit 0，列候选，`make` 从未执行** |
| `xlings subos use --sandbox --gpu --cmd '…'` | exit 1 | **exit 0，命令从未执行** |
| `xlings subos use --shell` | exit 1 | **exit 0，把人类可读表格打到 stdout** |
| `xlings subos use --global` | exit 1 | exit 0，没有持久化 |

`--shell` 那条尤其危险：`src/core/subos.cppm:1169` 自己写着「`--shell` 路径的 stdout 会被调用方 eval」，
而无名分支通过 `dispatch_data_event` → `ui::print_subos_list` 输出的是带 ANSI 的表格。

**为什么重要**

这正是本仓库反复出现的 silent-success 形态：**「没发生」和「成功了」产生同一个退出码**。
更要命的是 Task 16 Step 2 的验收命令本身就是 `xlings subos use gfxverify --sandbox --cmd '…'`——
如果名字写错或 SubOS 尚未创建，这条验收会**退出 0 并打印一张候选表**，而记录者只会看到「exit 0」。
即：**这个缺陷会把最终验收变成不可证伪的**。

**测试为什么没抓到**

`tests/e2e/subos_use_candidates_test.sh` 只测了裸 `subos use`（第 68 行 `RUN subos use`）。
所有带修饰符的用例都提供了名字。无名 + `--cmd` / `--shell` / `--global` / `--sandbox` 的组合零覆盖。

**建议**

只有在「没有任何执行型修饰符」时才把空名当作发现操作；否则保留 usage error（exit 1）：

```cpp
const bool discoveryOnly = cmd.empty() && mode == "spawn" && !sandbox && !gpu;
if (name.empty() && !discoveryOnly) {
    usageError("missing <name> for: xlings subos use "
               "(run `xlings subos use` with no arguments to list candidates)");
    return 1;
}
```

**已实现**：`discoveryOnly = mode=="spawn" && cmd.empty() && !sandbox && !gpu && !no_keep && !keep_forever && ttl_sec==0`，
否则 usage error + exit 1。E2E 补了 4 条无名用例（`--cmd` / `--sandbox --cmd` / `--shell` / `--global`），
断言 exit 1、标记字符串未出现、`--shell` 的 stdout 不含候选表、`--global` 没有持久化任何东西。

---

### R2（中）✅ (a) 已修复 / ❌ (b) 判断有误 —— `list <filter>` 可能是 `list` 的真子集

有两个**前置过滤**跑在身份被 catalog 校正之前，方向相反：

**(a) workspace 侧：shallow 候选集比完整候选集窄**

```
src/core/xim/inventory.cppm  build_owner_coordinates()
    const auto shallow = shallow_owner_candidates_for(db, pair);
    auto mayMatch = any_of(shallow, candidate_may_match…);
    if (!mayMatch) { …by_short_name 兜底… }
    if (!mayMatch) continue;                 // ← 整个 pair 被跳过
    auto candidates = owner_candidates_for(db, pair, cache, resolver, trace);
```

`shallow_owner_candidates_for` 在 legacy 图上只沿 `info->second.bindings` 的**出边**做传递遍历；
而 `owner_candidates_for` 走的是 `BindingSelectionResolver`，它的 `members` 里还包含只能通过**入边**到达的成员
（`legacy_root_in_selection` 就是在检查入边）。所以完整解析得出的 owner 名字可能匹配 filter，
而 shallow 集合里没有任何候选匹配 → 这一行被静默丢弃。

**(b) payload 侧：过滤跑在身份校正之前**

```
src/core/xim/inventory.cppm  assemble_inventory()
    const auto [namespaceName, name] = identity_from_store_name(storeName);
    if (!identity_matches(namespaceName, name, filter, match)) continue;   // 校正前
    …
    // 之后才：
    metadata.load_stamped_details(record.namespaceName, record.name)  // 可能改写 canonicalName
```

`identity_from_store_name` 按 `-x-` 切分。一个**不含 `-x-` 的旧 store 目录**（例如 `gcc/`）得到 ns=""、
canonical=`gcc`；而 `load_stamped_details` 会把它校正成 `xim:gcc`。于是：

- `xlings info gcc` → `collect_package_inventory(…, CoordinateMatch::exact)`，filter 是 `xim:gcc`，
  前置过滤按 `gcc` 比较 → 不匹配 → **旧 home 上一个确实装着的包被报成「未安装」**。
- 反向：store 名匹配但校正后不匹配的行会被保留。

**为什么重要**

两条都是「性能修复破坏身份语义」，而交接文档自己在第 124 行写了这条教训。
两条都不会报错，只会少一行/多一行。

**结论**

**(b) 不成立，撤回。** `by_identity` 的契约是「store 名必须完全一致才算命中」——
一个不含 `-x-` 的旧 store 目录解析到 `xim:gcc` 时，因为 store 名不等，`resolved.reset()`，返回 nullptr。
所以 stamped 行的 `canonicalName` 只会被**相等的值**覆盖，过滤前后是同一个字符串。我读漏了这个 reset。

**(a) 成立，已修。** `shallow_owner_candidates_for` 现在同时沿入边走
（`build_incoming_edges` 一次构建，且**只有带 filter 的查询才付这个成本**）。
配一条差分不变量单测 `XimInventoryFilter.AFilteredQueryEqualsFilteringTheFullQuery`：
对同一 fixture，`list <F>` 的行集合必须等于 `list` 按 F 过滤。
fixture 是 `owner@1 → member@1` 且 member 没有任何出边 —— 只能反向走到 owner。
配套的 `TheSkipDecisionFollowsEdgesBackwards` 用 `EXPECT_FALSE(blind.contains("owner"))`
钉住「旧实现确实看不到它」，所以这条测试是真 RED 转 GREEN，不是恒真断言。

---

### R3（中）✅ 已修复（比原建议更细）—— `remove` 的退出码与它自己的注释矛盾

`cmd_remove` 的「本 subos 没有 / 其他 subos 也没有 / 磁盘上没有 payload」分支：

```
src/core/xim/commands.cppm
    if (!payloadOnDisk) {
        auto selected = catalog.resolve_target(target, detect_platform());
        if (!selected) { stream.emit(ErrorEvent{…}); return 1; }      // ← 新
        …
        } else {
            // Genuinely absent. Here the "remove what isn't there is
            // success" convention is right … without breaking scripts
            // that re-run `remove` defensively.
            log::warn(…); return 0;
        }
```

同一个分支里，注释仍在说「防御性重跑不能被破坏」，而它上面三行的新代码正是在破坏它——
只要坐标在索引里解析不出来（recipe 已从索引下线、`--add-xpkg` 的本地 recipe 已删除、名字打错），
`remove` 现在返回 1。

`catalog.is_loaded()` 的早期守卫仍在，所以「完全没有索引」的机器不受影响；受影响的是
「有索引，但这个名字不在索引里」。

**实测把「二选一」证伪了。** 改成一律 exit 0 之后，`config_install_no_implicit_dir_test.sh` 失败：
Task 9 Step 2 明确要求「从未装过的 config 坐标必须失败且计数不变」，用的是
`remove config-no-payload@9.9.9`。两个契约都对，因为**它们回答的不是同一个问题**：

| 用户怎么问 | 意思 | 无法解析时 |
|---|---|---|
| `remove pkg` | 「确保 pkg 没了」——脚本会防御性重跑 | warn + **exit 0**；recipe 下线的包就是「没了」 |
| `remove pkg@9.9.9` | 「删掉**这个**版本」 | **exit 1**；坐标本身是错的，报成功等于确认删掉了一个不可能存在的东西 |

判据就是 `target.contains('@')`。两半都钉了 E2E：原有的显式坐标用例，
外加新增的 `remove no-such-package-anywhere` 必须 exit 0。

---

### R4（中→高）✅ 测量发现 143 条误报并已修复 —— `elfcheck::check` 一次放宽了三处

`src/core/elf_same_source.cppm` 同时做了三件事：

1. 删掉了 `provider_of(payload) != f.provider → continue` 的**同 provider 过滤**——现在任何 payload core 都参与比较；
2. 删掉了 `if (f.interpPayload.empty()) return f;` 的早退，新增 `HostLoaderPayloadCore` 规则——
   host INTERP 的二进制只要 RUNPATH 指向含 libc/loader 的 payload 目录就是 Error；
3. 「任一 same-source 条目即通过」变成「每个被证明的 core 条目都必须一致」。

三条各自都讲得通（`core_runtime_sources_` 的注释解释得很清楚：directory_iterator 顺序不确定，
选一个条目会让结论依赖文件系统历史）。问题在于**它们改变了哪些既有 home 会被判为 broken，而证据里没有这个数字**。

`FindingLevel::Error` 按 `doctor.cppm:197` 的注释「counts toward the exit code」计入退出码，
且 `--fix` 现在**蕴含 `--deep`**（`const bool deepAudit = deep || fix;`），
所以 `self doctor --fix` 在真实 home 上可能新增一批 `LoaderLibcSplit` Error 并改变退出码。

**实测：baseline 2 → PR 147。** 其中 143 条是同一个误报类：

```
interpreter -> .../xim-pkgindex-fromsource/.xlings/subos/default/lib/ld-linux-x86-64.so.2
core file   -> ~/.xlings/data/xpkgs/xim-x-glibc/2.39/lib/ld-linux-x86-64.so.2
```

那个 interpreter **就是我们自己的 loader**，只是经由 SubOS 视图到达。
而 `payload_of` 的文档注释自己就写了「a subos directory」返回空 —— 于是
`f.interpPayload.empty()` 被当成「host loader」，触发反向规则。
RUNPATH 一侧早就用 `weakly_canonical` 穿符号链接了；**只穿一侧，两侧就不可能一致**。

修复：`interpPayload` 为空时，先把 interp 自身 `weakly_canonical` 一次再判。
实测 143 → **0**，剩下 4（baseline 2 + 2 条真的 godot loader/libc split）。
两条新单测：`InterpThroughASubosViewIsNotTheHostLoader`（真实符号链接农场）与
`ARealHostInterpWithAPayloadCoreStillViolates`（反向规则对真正的宿主 interp 仍然生效）。

**这就是 D0/D4「先测量再合并」的全部价值**：不测的话，`--fix` 会在真实 home 上
声称 147 个 payload 损坏并逐个 `--force` 重装。

---

### R5 ❌ 判断有误，已撤回 —— Windows 路径拼写

```cpp
inline std::string payload_of(std::string_view p) {
    const auto normalized = store_parse_path_(p);      // '\' → '/'
    const std::string_view parsed(normalized);
    …
    return std::string(parsed.substr(pos - …));        // 返回归一后的拼写
}

inline std::string store_root_of(std::string_view p) {
    const auto normalized = store_parse_path_(p);
    auto pos = std::string_view(normalized).rfind(marker);
    …
    return std::string(p.substr(0, pos + marker.size() - 1));   // 返回原始拼写
}
```

下标算术是安全的（逐字符替换，长度不变）。但**同一个 store 在两个 helper 里得到两种拼写**：
`payload_of` 给 `C:/x/xpkgs/a/1`，`store_root_of` 给 `C:\x\xpkgs`。
任何把 `payload_of` 的结果与基于 `store_root_of` 拼出的路径做字符串比较的地方，在 Windows 上会静默不等。
`payload_identity_` 只对其中一侧做了 canonical 化。

**我读反了。** `payload_of` 的返回是 `p.substr(...)` —— **原始拼写**，不是归一拼写；
归一只用来找切分点。`store_root_of` 做的是同一件事。两者本来就一致。
按我的「修复」改成归一后，既有测试 `PayloadOf.AcceptsWindowsSeparators` 立刻失败——
那条测试正是这个不变量的守卫，它做对了。改动已回滚。

保留下来的只有一条加强：把不变量本身写进那条测试
（`payload_of(p).starts_with(store_root_of(p))`，两种拼写都验），
这样将来真的分叉时，失败发生在这里，而不是在某个比较它们的地方。

---

### R6（低）✅ 已修复 —— `get_catalog(InstallReady)` 的自愈分支在一条真实路径上永不触发

```cpp
std::expected<void, std::string> result;      // 默认构造 = 成功
bool rebuiltThisCall = false;
if (!initialized) { result = mgr.rebuild(); initialized = true; rebuiltThisCall = true; }
if (access == InstallReady && !installReadyChecked) {
    if (!mgr.is_loaded() && !rebuiltThisCall) result = mgr.rebuild();
    bool subIndexesNeverSynced = !sub_indexes_initialized();
    if (!result || subIndexesNeverSynced) { … 自愈 resync … }
```

当**同进程内先有 LocalOnly 调用**初始化了单例、且 `is_loaded()` 为真时，
`result` 保持默认构造（成功），`!result` 分支永远不会进——
只有 `subIndexesNeverSynced` 能触发自愈。

这条路径今天就是活的：`src/core/subos/sandbox.cppm` 的 `backend_target_unavailable_()` 已改成
`get_catalog(CatalogAccess::LocalOnly)`，它可以先于 `cmd_install` 执行。

影响有限（`is_loaded()` 为真时本来也不需要 resync），但「一个默认构造的 expected 被当成一次真实探测的结果」
是会长出后续缺陷的形状。

**已改成** `std::optional<std::expected<void, std::string>>`：
「没探测过」现在是 `nullopt`，与「探测成功」不再同形，自愈分支改用显式的
`buildFailed = result.has_value() && !result->has_value()`。

---

### R7（低）✅ 已修复（文案）—— `--fix --scope X` 只窄化检测，不窄化修复

`--scope` 只影响 `payloadAuditRoots`；其余所有检测与 `repair_payloads_` / `repair_local_` 仍然全 home 生效。
而 `--fix` 蕴含 `--deep`，所以 `--fix --scope X` 是合法组合，announce 行会打印
`deep audit scope: X`，然后去修与 X 无关的目标。

**已改文案**（而不是改行为）：announce 行现在是
`deep audit scope: X (payload/runtime audit only; other checks and repairs still cover this home)`。
让 `--scope` 也约束修复是另一个语义决定，本轮不做——但说的和做的现在一致了。

### 值得肯定的两处

- `BindingSelectionResolver` 把 incoming-edge 索引从「每次解析重建」改成「一次构建 + 计数器」，
  并让 `InventoryTrace::legacyIncomingIndexBuilds` 可被测试断言。
  **断言结构而不是断言计时**，这正是这类性能修复该有的验证方式。
- D5 把「声明了但未 materialize 且无 active binding」与「active 不一致 / active payload 丢失」分成
  Warning 与 Error 两级。这是本仓库反复踩的「冷状态被当成损坏状态」的正确解法。

---

## 3. 未完成工作的重新排序

### 3.1 计划里站不住的四个假设

**假设 1：T12/T13 必须等 T11 发布的 xlings。**

计划写「Consumes: … publicly released xlings from Task 11」。但 T12（mcpp #392 exact compiler owner fixup）
读的是**编译器所属 registry home 的 SubOS manifest**，`subos_info.runtime` 自 2026.8.5.1 起就在公开发布里了；
T12 的 RED 单测（两个 glibc payload 目录、显式 owner binding、反转目录创建顺序）与 alternating-build E2E
都不需要 #519 的任何新行为。

真正需要已发布 xlings 的只有：T13 Step 4 的**真实 shared-registry #514 集成测试**（它要跑真实安装 hook），
以及 T14 的发布。

→ **T12、T13 Step 1-3 可以与 T11 并行。** 关键路径从 5 段串行收缩到 3 段。

**假设 2：一次 squash merge 是合适的落地方式。**

#519 一个 squash 涵盖 8 个子系统：只读查询、doctor 分层、runtime 权威、#513/#514 host 集成、
#506 removal、subos use。其中**只有后三组改变了拒绝语义**（新增 refusal / 改变退出码 / 改变 owner 选择）。
如果 R4 说的 D5/ELF 收紧在真实 home 上过严，回滚它会把查询性能修复一起带走。

→ 建议拆成两次合并（见 3.2 的 P2 / P4），代价是多一轮全平台 CI，收益是**风险可回滚粒度**与更可读的 review。
这不违反「禁止 amend/rebase/force-push」——拆分是往新分支 cherry-pick，#519 的历史不动。

**假设 3：预期版本 `2026.8.9.3`。**

main 已经是 `2026.8.9.2`（`2913a09`，PR #516 已合），今天是 **2026-08-10**。
按「常规日期版本不用 `.0`」的约束，下一个是 **`2026.8.10.1`**。
计划自己写了「cut 前重新计算」，但文档里的预期值现在会误导照做的人。

**假设 4：Task 16 的 GPU cell 有确定的可用性。**

计划写「2026-08-09 preflight 主机有 RTX 4080 / 550.144.03 / `/dev/nvidia0` … unless that external state disappears」。
把验收挂在某台机器未来某天的外部状态上，如果它静默降级成 SKIP，验收就不可证伪了。

→ 需要在开跑前把每个 cell 标成 **required / may-SKIP**，并且 may-SKIP 的 cell 必须写明「SKIP 的判据是什么」
（不是「没跑」，而是「探测到设备不存在」）。

另外，Task 16 Step 1 写「用公共 `quick_install` 装到新的临时 root」——
`quick_install` 会忽略 `XLINGS_HOME`，直接打到真实 home。验证发布产物应当用 tarball 自带的 `self install`。

### 3.2 修正后的推进顺序

```
P0a R0 测量：真实旧 home 上的 runtime 声明 /     (只测量，不改代码，阻塞 P4)
     active glibc / doctor 退出码 / use 是否被拒
     ├─ 不误伤 → 记录数字，继续
     └─ 误伤   → 改 init.cppm 的老 home 兜底 + 给 use 留迁移出口

P0b R1 修复 + 4 条 E2E                          (xlings, 阻塞一切)
     └─ 这是唯一会让后续验收假通过的缺陷

P1  R2/R3/R5/R6/R7 修复 + 差分不变量测试         (xlings)
P1' R4 增量测量：真实 home 上 doctor --deep 的   (只测量，不改代码)
     Error 增量与退出码差，对照 2026.8.9.2
     ├─ 增量可解释 → 继续
     └─ 增量不可解释 → 先收窄 elfcheck 规则

P2  合并 A：只读 + 诊断 + #513/#514              ┐
     (query/catalog/inventory/doctor/installer    │  可与 P3 并行
      的 hook transcript 与 store roots)          │
                                                  │
P3  mcpp T12/T13 Step 1-3：单测 + E2E RED/GREEN  ┘  不需要已发布 xlings

P4  合并 B：runtime 权威 + #506 removal + subos use
     └─ 需要 P0a 与 P1' 的测量结论

P5  版本 2026.8.10.1 → 全平台 CI → squash → release
     → GitCode GET+sha256 → xim-pkgindex pointer A

P6  mcpp T13 Step 4（真实 shared-registry 集成，需要 P5 的已发布 xlings）
    → T14 mcpp 发布 → pointer B

P7  T15 生态收口：删掉 #506 的 Windows tolerance、graphics verifier 命名域
P8  T16 真实审计 → issue 关闭证据
```

与原计划的差异只有三处：新增 P0/P1'，把 P2 与 P3 并行，把原 T11 拆成 P2/P4/P5。

---

## 4. 门禁的可证伪定义

原计划的门禁多数写成动作（「跑一个基准并记录 median/p95」），不是判据。以下把每个门禁写成
**一个能失败的命令**。数字是建议初值，可以调，但必须先定下来。

| 门禁 | 判据（失败即阻塞） | 怎么测 |
|---|---|---|
| G1 查询结构 | `info <pkg>` 与 `list <filter>` 的 `InventoryTrace` 中：网络请求 0、子 `xlings` 进程 0、`legacyIncomingIndexBuilds ≤ 1`、`payloadVersionDirs` 不随无关包数量增长 | `tests/e2e/query_heavy_home_test.sh` 已有骨架，补 trace 断言 |
| G2 查询延迟 | 真实旧 home：`info gcc` < 0.5s、`list gcc` < 0.5s、`list` < 1.0s（当前实测 0.143 / 0.132 / 0.204，留 3–5x 余量） | 同上，warm median of 5 |
| G3 差分不变量 | `list <F>` 的行集合 == `list` 的行集合按 F 过滤，对含 legacy 入边绑定与无 `-x-` stamped 目录的 fixture 成立 | 新增 unit（R2 建议 1） |
| G4 quick doctor 预算 | 默认 `self doctor`：无 `patchelf`/`getent` 进程、无子 `xlings`、无 payload 递归遍历 | `self_doctor_depth_test.sh` 已覆盖，补进程计数 |
| G5 deep doctor 增量 | 真实 home 上 `self doctor --deep` 相对 `2026.8.9.2` 新增的 `LoaderLibcSplit` Error **逐条可解释**；退出码变化需在 PR 正文声明 | 手工对照，结论写入本文档的证据表 |
| G5' 老 home 兼容 | 一个「无 `subos_info` 块 + active glibc ≠ 2.44」的 home，升级后 `self doctor` 不得从 0 变非零；`xlings use glibc <当前 active>` 不得被拒 | 用 `.agents/tools/slice-real-home.sh` 切一份真实 home 副本跑（R0） |
| G6 subos use 安全性 | 无名 + `--cmd`/`--shell`/`--global`/`--sandbox` 各自 exit 1，且标记字符串不出现、stdout 无 `export` | R1 建议的 4 条 E2E |
| G7 #506 | foreign provider 的 DB / workspace / shim / payload 在删除后逐项 byte 一致；Windows native 上删掉 tolerance 后测试必须真通过 | 现有 `remove_foreign_provider_delegator_test.sh` + P7 的 pkgindex 改动 |
| G8 发布完整性 | 每个平台 asset 在 GitHub 与 GitCode 上 **GET**（非 HEAD）跟随重定向后 sha256 与 sidecar 一致 | `tools/mirror-latest.sh` + 手工 GET |
| G9 冷装 | 用 tarball 自带的 `self install` 装到干净 root（**不是** `quick_install`），版本、`list`、`info`、quick doctor 全部正确 | 见 3.1 假设 4 |
| G10 GPU cell | 每个 cell 显式标 required / may-SKIP；SKIP 必须附「探测到设备不存在」的命令输出，不能是「没跑」 | Task 16 Step 2 |

原则不变：**任何计时通过都不能豁免结构违规**（G2 绿而 G1 红 = 阻塞）。

---

## 5. 风险与回退

| 风险 | 触发形态 | 处置 |
|---|---|---|
| **R0：老 home 升级后永久不合规** | 无 `subos_info` 块的 home 被 `self install` 写入 `glibc@2.44`；active 是 2.39 → doctor Error + `use` 被拒 | **P0a 先测量**。若误伤：老 home 路径不写 runtime 字段（保持无声明），并给 `use` 一条显式迁移出口 |
| ELF 规则收紧误伤既有 home | `doctor --deep` / `--fix` 在真实 home 上新增大量 Error | P1' 先测量。若不可解释：把 `HostLoaderPayloadCore` 降为 Warning 并单独出一版，不与查询修复同批 |
| `cmd_use` 的 runtime 拒绝挡住合法迁移 | 用户想换 SubOS 的 runtime，doctor 和 use 两条路都指向「新建 SubOS」 | 确认它与 main 里已有的 C2 install-time guard 不重叠、不互相掩盖；确认「迁移」不是唯一出路 |
| 两次合并带来 CI 成本 | 多一轮全平台 | 接受。风险可回滚粒度更值钱 |
| 未提交 doc 丢失 | 切分支时 untracked 文件挡住 checkout / 被清理 | D6：立刻落盘 |

---

## 6. 当前状态与剩余工作

### 已完成（本地全绿）

| | 内容 | 验证 |
|---|---|---|
| R0 | runtime 声明来自实际在跑的 runtime；`--fix` 采纳；两条真实出路 | 7 unit + 3 态 E2E |
| R1 | 缺名字 + 执行型修饰符 = usage error | 4 条 E2E |
| R2(a) | 过滤前置判定同时走入边 | 差分不变量单测（真 RED→GREEN） |
| R3 | 裸名 exit 0 / 显式版本 exit 1 | 两半各一条 E2E |
| R4 | interp 也穿符号链接再分类 | 2 unit + 真实 home 143→0 |
| R6 | `optional<expected>`，「没探测过」≠「探测成功」 | — |
| R7 | announce 行说清 scope 只窄化检测 | — |
| 版本 | `2026.8.10.1`（`mcpp.toml` + `Config::VERSION`） | tags 最新 `v2026.8.9.2` |

本地门禁：unit **36/36**；受影响 E2E **18/18**；`test_generated_command_reference`、
`test_cli_spec_parity`、`test_docs_examples` 全过；真实 store `verify-untouched` 干净。

### 剩余

1. **D6**：`2026-08-09-stability-regression-recovery-design.md` 的工作区副本是**旧版**，删掉
   （PR 里的是新版，且它会挡住 checkout）；另外两份从未提交的 doc 作为独立 docs commit 落盘。
2. 提交 + push 到 #519，等全平台 CI terminal green。
3. release `2026.8.10.1` → 本地 `gtc` 补 GitCode 资源 → GET+sha256 校验 → xim-pkgindex pointer A。
4. 生态真实验证：`xlings subos <name> --sandbox --cmd "…"`。
5. 跨仓：mcpp #392 / #514 集成、pointer B、删掉 #506 的 Windows tolerance。

详细任务拆分与依赖见 `.agents/plans/2026-08-10-pr519-completion-implementation.md`。

---

## 附录 A：本次评审的核实方法

为了让后续 reviewer 知道哪些结论是测过的、哪些是读代码得出的：

- **读过完整 diff**：`git diff origin/main...origin/fix/stability-regression-recovery -- src/`（3397 行）与
  `-- tests/`（3046 行），逐 hunk。
- **读过完整源文件**（不只是 diff）的部分：`get_catalog` 全体、`subos.cppm` 的 `run()` use 分支与
  `use_emit_shell` 上下文、`cmd_remove` 的 subos-membership guard 全段、`doctor.cppm` 的 `count_` 与 probe 用点、
  `init.cppm` 的 manifest 写入路径、`manifest.cppm` 的 `preserved_runtime`。
- **grep 核实**：`probe_coordinate` 删除后无残留调用者（0 命中）；`--shell` stdout 被 eval 的注释在
  `subos.cppm:1169`；`FindingLevel::Error` 计入退出码在 `doctor.cppm:197`；
  `DEFAULT_RUNTIME` 的三个写入点在 `init.cppm:197`、`doctor.cppm:1846`、`subos.cppm:901`。
- **main 对照**：D5 在 main 上是 `!installed(info.runtime)` + Warning（`doctor.cppm:654`），
  用来确认 R0 第二层确实是 #519 引入的行为变化，不是既有行为。
- **git 核实**：分支祖先关系、两个仓的版本号、三份 doc 的提交状态、PR CI rollup、C1 提交（`054be37`）的范围说明。
- **未做**：没有构建、没有跑测试、没有在真实 home 上测量。
  所以 **R0 与 R4 是待测量项**——它们的**机制**已在代码里核实，**影响面**没有。
  R1/R2/R3/R5/R6/R7 是读代码得出的，其中 R1 有 E2E 覆盖缺口作为独立佐证。
