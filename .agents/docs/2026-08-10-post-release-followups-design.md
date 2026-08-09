# 2026.8.10.1 之后的两条后续：`--fix` 的成本，与跨仓并行收口

> 状态：A 已实现（2026.8.10.2）；B2 待提交；B3 由他人接手
>
> 实现记录见 §F —— 其中两条是我自己在实现过程中制造并抓到的错误
>
> 日期：2026-08-10
>
> 前序：`.agents/docs/2026-08-10-pr519-review-and-completion-design.md`（评审与实测）、
> `.agents/plans/2026-08-10-pr519-completion-implementation.md`（收尾计划）
>
> 已发布：`v2026.8.10.1`（main `65002df`），pointer A 已合并，公共索引 `latest -> 2026.8.10.1`

---

## 0. 结论先行

两条互不依赖的后续，可以完全并行：

| | 内容 | 关键结论 |
|---|---|---|
| **A** | `self doctor --fix` 的执行成本 | `--fix` 会跑 **7 次** payload 审计，而**没有任何 repair 消费它们的结果**。在真实 71 GB home 上这是约 **23 分钟的纯浪费** |
| **B** | 跨仓收口（#506、pointer B） | 旧计划把 mcpp 排在发布之后是多余的；但它**漏了一条真实的阻塞依赖**：`xim-pkgindex` CI 把 xlings pin 在 `v2026.8.8.2`，早于 #506 的修复，**先 bump 才能删 tolerance**。<br>**mcpp #392 已由他人接手**（基于最新 xlings 更新发版），B3 只作为背景保留 |

两条都有一个共同的判断标准：**「没检查」和「检查过没问题」不能长得一样**。
A 的设计里它体现为「跳过审计必须说出来」，B 的设计里体现为「tolerance 删掉之后 gate 必须真的能失败」。

---

## A. `self doctor --fix` 的成本

### A1. 根因（读源码得出，不是推测）

`--fix` 路径里 `detect_` 被调用 **7 次**：一次初始（`doctor.cppm:3044`），加上 6 次 `refresh()`
（`3088 / 3092 / 3105 / 3127 / 3134 / 3138`，每次都执行 `3074` 的 `scan = detect_(state, probe, audit)`）。

`refresh()` 的存在是对的，注释也说清了理由——payload 修复跑在**子进程**里并写状态文件，
不重读就会把「重新注册成功」读成失败。问题不在 refresh，在于 `audit` 这个参数带进去的东西。

`audit.deep` 在 `detect_` 里只 gate 两处：

| 位置 | 内容 | `--fix` 需要吗 | 成本 |
|---|---|---|---|
| `doctor.cppm:1201` | `owning_coordinate_(...)` → BrokenPayload 的 `remedy` | **需要**（`repair_payloads_` 靠它判断可重装性） | 便宜（本地 catalog 解析） |
| `doctor.cppm:1475` | `payloadAuditRoots` → ELF 扫描 + NSS 探测 | **不需要** | **昂贵** |

而 `deepAudit = deep || fix`（`2905`），一个 bool 同时开了这两件事。

### A2. 「不需要」是查出来的，不是假设

grep 全文，`FindingKind::LoaderLibcSplit` 与 `FindingKind::NssResolution` 只出现在四类位置：
枚举定义（170/193）、`detect_` 里产生它们（1526/1575/1624）、`count_`（2505/2511）、`render_`（2775/2784）。

**没有任何 repair 函数选择这两种 finding。** 逐个确认过每个 repair 的选择集：

```
repair_payloads_            BrokenPayload
repair_local_               LegacyAliasShim MissingShim OrphanShim
                            SubosEnvOrphan SubosManifest SubosRuntimeMissing SysrootDangling
repair_inactive_            InactiveInstalled
prune_dead_registrations_   BrokenPayload
repair_state_               （不按 finding 选择）
repair_other_subos_         （不按 finding 选择）
```

也就是说：ELF 扫描的产出只进入计数和渲染，`--fix` 跑它 7 次，7 次都没人用。

### A3. 只有一个 repair 能改变 payload

如果 payload 内容不会变，重复审计就一定得到同样的答案。逐个查了每个 repair 是否写 payload store：

- `repair_local_` 的 grep 命中是**一句注释**（讲 `remove()` 与 `remove_all()` 对符号链接的区别），不是写入；
- `repair_inactive_` 跑的是 `xlings use`，写 workspace 和 shim，不写 payload；
- `repair_state_` / `repair_other_subos_` / `prune_dead_registrations_` 命中数为 0；
- **只有 `repair_payloads_`** 通过子进程重装，能真正改变 payload。

`scan_payload` walk 的是 `Config::paths().dataDir / "xpkgs"`；shim 在 `subos/*/bin`，不在其中。

### A4. 数字

| | 来源 |
|---|---|
| 一次 deep 审计 | **196.3s**（实测：真实 71 GB home 上 `--deep` 196.95s − quick 0.63s） |
| `--fix` 的审计次数 | **7**（读源码） |
| 纯浪费 | 7 × 196.3 ≈ **1374s ≈ 23 分钟** |

> 这是「实测的单次成本 × 读源码得到的次数」，不是端到端计时。
> 端到端计时做不了：`--fix` 会对真实切片发起网络重装，上一轮已因此中止并跑
> `verify-untouched` 确认真实 store 未损。

**这不是本轮引入的回归**——2026.8.9.2 的默认 doctor 同样跑全量审计（195.16s），
所以旧版 `--fix` 一样慢。但现在 quick 路径已经 0.63s，`--fix` 成了唯一还慢的入口。

### A5. 设计

**A5-1. 把一个 bool 拆成两个语义。**

```cpp
struct AuditSelection {
    bool remedies { false };   // 需要 localCatalog：BrokenPayload 的 remedy
    bool payloads { false };   // ELF 扫描 + NSS 探测
    std::optional<xim::PackageMatch> scope;
};
```

| 命令 | remedies | payloads |
|---|---|---|
| `self doctor` | false | false |
| `self doctor --deep` | true | true |
| `self doctor --fix` | **true** | **false** |
| `self doctor --fix --deep` | true | true |

`--fix` 保住它真正需要的（remedy / 可重装性判断），丢掉它从不消费的（ELF 扫描）。
`--scope` 依旧要求 `payloads`（即 `--deep`），因为它窄化的正是 payload 审计。

**A5-2. `--fix --deep` 时，只在 payload 可能变过之后重新审计。**

```cpp
// refresh(payloadsMayHaveChanged):
//   false -> 沿用上一次的 payload findings，只重算其余部分
//   true  -> 重跑 payload 审计
```

按 A3 的结论，只有 phase 2（`repair_payloads_`）之后需要 `true`。
`--fix --deep` 从 7 次审计降到 **2 次**。

实现上不要在 `refresh` 里做 finding 拼接（那会长出「两个答案」）——
让 `detect_` 接受一个可选的「上一轮 payload findings」并在 `payloads==false` 时原样带过，
产生与重算完全相同的 `Scan` 形状。

**A5-3. 跳过必须说出来。**

`--fix` 不带 `--deep` 时，报告结尾固定输出一行：

```
▸ note   payload/runtime audit not run — `xlings self doctor --deep` to include it
```

理由是本仓库反复出现的那个形态：**「没检查」和「检查过没问题」产生同一个退出码**。
`doctor`（quick）与 `--deep` 今天已经存在这个退出码差异且被接受；把 `--fix` 从
「隐式 deep」改成「默认 quick」反而让三者**更一致**，但前提是它必须说清楚自己没做什么。

### A6. 验收

| | 判据 | 怎么测 |
|---|---|---|
| **A-G1** | 真实 home 上 `self doctor --fix` 的 wall time 相对 `--deep` 至少低一个数量级 | 切片上计时；`--fix` 用 `--real` 覆盖它可能重装的包，跑完 `verify-untouched` |
| **A-G2** | `--fix` 的 `LoaderLibcSplit` / `NssResolution` finding 数为 0，`--fix --deep` 与 `--deep` 一致 | 同一 fixture 三种调用对照 |
| **A-G3** | `--fix --deep` 的 payload 审计执行次数 ≤ 2 | 计数器（照 `InventoryTrace::legacyIncomingIndexBuilds` 的做法，**断言结构不断言计时**） |
| **A-G4** | `--fix` 不带 `--deep` 时输出里必须出现「audit not run」 | E2E 字符串断言 |
| **A-G5** | `--fix` 的 BrokenPayload remedy 与今天逐字一致 | 差分对照，证明拆分没有削弱修复能力 |

A-G3 是关键的一条：**计时会因机器而异，次数不会。**

---

## B. 跨仓并行收口

### B1. 旧计划错在哪、又漏了什么

**错的**：把 mcpp #392 排在「xlings 发布之后」。#392 读的是编译器所属 registry home 的 SubOS
manifest，`subos_info.runtime` 自 2026.8.5.1 起就在公开发布里；它的单测和 alternating-build E2E
不需要 2026.8.10.1 的任何新行为。只有**真实 shared-registry #514 集成测试**和**发布**需要。

**漏的（更重要）**：`xim-pkgindex` 的 CI 把 xlings pin 死了。

```
ci-test.yml       :53  export XLINGS_VERSION=v2026.8.8.2
ci-test.yml       :153 $env:XLINGS_VERSION = "v2026.8.8.2"
ci-test.yml       :214 export XLINGS_VERSION=v2026.8.8.2
ci-test.yml       :279 export XLINGS_VERSION=v2026.8.8.2
ci-xpkg-test.yml  :58  export XLINGS_VERSION=v2026.8.8.2
ci-xpkg-test.yml  :122 export XLINGS_VERSION=v2026.8.8.2
```

**6 处，全部早于 #506 的修复（2026.8.10.1）。** 今天直接删掉 Windows tolerance，
gate 会用一个没有 provider-aware removal 的 xlings 去跑，必然红——而且红的原因会指向
recipe，不指向 pin。**bump 与删 tolerance 必须在同一个 PR 里。**

### B2. #506 收口：删 tolerance 会同时打开两个断言

`windows-test.ps1` 的 tolerance 是一个 `continue`，位置在 **post-uninstall 检查之前**：

```powershell
if ($delegates) {
    Log-Info "uninstall not asserted: this recipe delegates its Windows install"
    continue        # <- 同时跳过了下面的 shim 泄漏检查
}
```

所以删掉它等于同时恢复两件事：

1. **uninstall 必须成功** —— 这是 2026.8.10.1 的
   `executing_provider_owns_no_version` 应该修好的：`gcc` 在 Windows 上零注册，
   `gcc` 这个 target 下的版本属于 `xim:mingw-w64`，provider 不匹配 → 判定「本 provider 未注册任何版本」
   → 允许 uninstall hook 执行并移除 payload。
2. **shim 泄漏检查** —— 这条**没有被 2026.8.10.1 处理过**，是新暴露面，
   但它是不是缺陷取决于委托卸载到底清不清 shim。查过 `gcc.lua` 之后：

   ```lua
   -- 第 31-35 行，注释是原文：
   -- They are intentionally NOT in the top-level `programs` so that the windows
   -- declared-program audit ... doesn't demand mingw-w64 to provide them.
   programs = { "gcc", "g++", "c++" },     -- 顶层 programs 就是 mingw-w64 提供的那一组

   function uninstall()
       if os.host() == "windows" then
           pkgmanager.uninstall("mingw-w64@" .. version_map_gcc2mingw[pkginfo.version()])
   ```

   所以 `gcc` 的 `programs` **本来就是**它委托给 mingw-w64 的那组 shim，卸载 gcc 应该
   通过委托把它们一起带走。泄漏检查因此是**正确的断言**，不是误报——
   它要问的正是「委托卸载真的清干净了吗」，而这恰恰是 tolerance 一直在掩盖的第二半。

> **这里我先写错过一次，留档。** 初稿凭 `reference_shared_shim_ownership`
> 推断「mingw-w64 拥有 `gcc` shim，所以卸载 gcc 后它正确存活，泄漏检查会误报」。
> 读了 `gcc.lua` 才发现前提不成立：那组 program 是 gcc 自己声明的，委托卸载负责清理。
> 与上一轮 R5/R2(b) 同一个毛病——**凭已有认知推断，没读那个文件**。

### B3. mcpp #392：两个独立缺陷，其中一个旧计划的前提已过时

**B3-1. glibc payload 选择仍然按目录顺序。** `src/xlings.cppm:729`：

```cpp
for (auto& v : std::filesystem::directory_iterator(root, ec)) {
    if (v.is_directory(ec)) return v.path();
}
```

上面的注释自己写着：一个断言「取最高版本」的测试被移除了，因为它「按目录顺序通过或失败，
两种结果都不提供信息」。也就是说这个选择**至今是任意的**——正是 #392 的形状。
修法：从编译器 owner / default SubOS 的 manifest 读 exact binding，
经现有 `probe_payload_paths(compilerBin, binding)` 解析，
删掉这条兜底；`kFixupRev` 递增，让旧指纹无法压制修正。

**B3-2. `LD_LIBRARY_PATH` 上的 payload glibc —— 旧计划说「删掉这个 append」，但它已经被收窄过了。**
`src/build/plan.cppm:711`：

```cpp
if (tc.payloadPaths && !plan.depRuntimeLibraryDirs.empty()) {
    append_unique_path(plan.runtimeLibraryDirs, tc.payloadPaths->glibcLib);
}
```

上面有一大段注释解释了 SIGSEGV 机理（宿主 `/bin/sh` 拿到 payload 的 `libc.so.6`，
而 PT_INTERP 无法被环境变量覆盖，libc 与 ld.so 通过 GLIBC_PRIVATE 版本锁死），
并说明这个 guard 就是「不需要就不发」。

所以 **Task 13 Step 3 的「Delete the glibc append」已经部分完成**，照旧计划写会重复劳动。
剩下的是它没做的那半：按**目录内容**判断（而不是按名字/路径）过滤掉任何解析后含 libc/loader
的目录，这样被改名或换 namespace 的 payload 也能覆盖。

### B4. pointer B

`xim-pkgindex/pkgs/m/mcpp.lua` 的 `latest` 仍是 `2026.8.8.4`。mcpp 发布后再 bump，
并遵守「三个平台块一致更新」（`reference_partial_version_bump`：部分 bump 在其他平台上读作 not found）。

---

## C. 依赖图

```text
A  doctor --fix 成本拆分            ← 与 B 完全独立，可立即开工
   A1 AuditSelection 拆成 remedies/payloads
   A2 refresh 只在 payload 可能变过时重审计
   A3 "audit not run" 提示
   A4 A-G1..G5 验收

（gcc.lua programs 核查 ✅ 已完成，见 B2：programs 就是委托出去的那组，
  泄漏检查是正确断言，不需要预先改 recipe）

B2 xim-pkgindex 单 PR：
     6 处 XLINGS_VERSION → v2026.8.10.1
   + 删除 Windows #506 tolerance
        │
        ▼
   #506 关闭（原始复现绿了才关）

B3 mcpp #392（可与 A、B1、B2 并行开工，不依赖任何发布）
   B3-1 exact compiler-owner fixup + kFixupRev++
   B3-2 runtime env 的内容判定过滤
        │
        ▼
B3-3 真实 shared-registry #514 集成测试   ← 需要已发布的 2026.8.10.1（已就绪）
        │
        ▼
B4 mcpp 发布 → pointer B
```

**A、B2、B3-1、B3-2 四条今天就能同时开工。** 唯一的串行段是 B2→#506 关闭，以及 B4 依赖 mcpp 发布。

---

## D. 风险

| 风险 | 形态 | 处置 |
|---|---|---|
| A 的拆分削弱了 `--fix` 的修复能力 | remedy 变空、可重装性判断失效 | A-G5 逐条差分对照今天的 remedy 输出 |
| A 的 refresh 缓存产生「两个答案」 | 缓存的 payload findings 与重算的不一致 | 不做拼接：`payloads==false` 时原样带过上一轮，形状完全相同 |
| B2 的 shim 泄漏检查红 | 委托卸载没有清掉 mingw-w64 的 `gcc`/`g++`/`c++` shim | 这是**真缺陷**，不是误报（已确认 `programs` 就是委托出去的那组）。修在 xlings 或 mingw-w64.lua，不要放回 tolerance |
| B2 bump 到 2026.8.10.1 引入其他 recipe 回归 | 索引 CI 大面积红 | bump 与删 tolerance 同 PR，但**先只 bump 跑一轮**确认基线绿，再加删除 |
| B3-1 改动影响已构建产物 | 旧指纹压制修正 | `kFixupRev++`（旧计划已包含，仍然必要） |

---

## E. 尚未核实的

诚实起见，以下是本文**没有**验证的：

- **删掉 tolerance 之后 Windows 是否真的绿。** 需要一次真实 Windows 运行，本地无法判断。
  B2 的 PR 本身就是这个验证。
- ~~`gcc.lua` 的 `programs` 声明~~ —— 已查：`programs = { "gcc", "g++", "c++" }`，
  正是委托给 mingw-w64 的那组，泄漏检查是正确断言。初稿在这一点上推断错了，已在 B2 留档。
- **A5-2 的缓存改造之后 `--fix --deep` 的实际耗时。** 设计预期是 2 次审计，
  但 `--fix` 端到端还包含真实重装，端到端时间不由审计次数单独决定。


---

## F. 实现记录（A，2026.8.10.2）

设计基本按 §A5 落地，但过程里有三件事值得留档：两件是我自己制造的错误，一件是设计里没预料到的。

### F1. 代码

- `AuditSelection` 由一个 `deep` 拆成 `remedies` / `payloads`（`doctor.cppm`）。
  `wantRemedies = deep || fix`，`wantPayloads = deep`。
- `refresh()` 从无参改为 `refresh(bool payloadsMayHaveChanged)`，**六个调用点逐个标注**。
  只有 `repair_payloads_` 之后是 `true`。参数**没有默认值**——默认值会让新增的调用点
  安静地选到错误的那一侧。
- `payload_findings_of_(scan)` 显式列出属于 payload 审计的 FindingKind。
  写成白名单而不是「detection 没重算的其余部分」：将来新增一种 payload 派生的 finding，
  忘记登记会在这里显现，而不是在远处悄悄少一行。
- 携带用 `const std::vector<Finding>*`，整份带过、不做拼接。拼接就是第二个回答者。
- `--fix` 不带 `--deep` 时输出 `▸ • not audited  payload/runtime audit did not run — add \`--deep\` to include it`。

`--scope` 的报错文案同步改成 `` `--scope` requires `--deep` (it narrows the payload/runtime audit) ``——
旧文案里的「or `--fix`, which implies it」现在是错的。

### F2. 我做错的第一件：对照组是错的版本

第一次验证「`--fix` 是否还产出 remedy」时，我拿 `~/.xlings/bin/xlings` 当 2026.8.10.1 的对照。
它其实是 **2026.8.9.2**——PR #519 之前的版本，探针还是子进程 `xlings info`，根本不走 catalog。
基于这个对照我一度认定自己引入了回归。

第二次我构建了 `origin/main` 做对照，结论反过来了；但那次两边都在 worktree 的 cwd 下跑，
而 worktree 里有 `.xlings.json`，会引入 project scope——**两次都不是干净口径**。

最后用 E2E 的同口径（`env -i` + `cd /tmp`）比较**已发布的 2026.8.10.1** 与本次构建，
输出逐字一致，才确认没有回归。

教训与上一轮同源：**对照组本身要先验证**。`--version` 是一句话的事。

### F3. 我做错的第二件：守卫被 `|| true` 吞掉

给 remedy fixture 写的执行器是：

```bash
REMEDY_RUN() { ( cd /tmp && exec env -i ... "$XLINGS_BIN" "$@" ) || true; }
```

然后用它写守卫：

```bash
REMEDY_RUN info xim:bat >/dev/null 2>&1 || fail "setup: index does not resolve"
```

`|| true` 让退出码恒为 0，**这个守卫从来没有守过任何东西**。它本来就是为了防止
「fixture 不可用导致断言空洞」而写的，结果自己就是空洞的——本仓库反复出现的那个形态，
这次出现在我的测试脚手架里。

### F4. 没预料到的：fixture 索引比想象中难立起来

复制 `tests/fixtures/xim-pkgindex` 再加 `xim-indexrepos.lua`、删缓存、跑 `self init`，
catalog 仍然报 `package index not available`。已有的
`doctor_fix_convergence_test.sh` / `foreign_payload_reinstall_test.sh` 有能用的索引 fixture，
但把它们的完整 setup 搬过来，测的就变成了「它们的 setup」而不是本次改动。

所以反向断言改成**差分**：同一个 home 上 `--fix` 与 `--fix --deep` 关于
broken payload / remedy / prune 的输出必须逐字一致——只有 payload 审计这一项不同，
任何差异都说明拆分伸到了不该伸的地方。再加一条**空洞性守卫**：两次输出如果完全相同，
差分就是拿一个东西和自己比，直接判失败。

正向路径（真实可解析坐标产出 `xlings install ...` 并驱动 ladder）由上述两条既有 E2E 覆盖，
它们在本次改动后仍然通过。

### F5. 既有测试的契约翻转

`self_doctor_depth_test.sh` 断言的是「`--fix` 蕴含 deep」——那正是本次要改的契约。
断言从 `-gt 0` 翻成 `-eq 0`，并补两条让翻转保持诚实：

- `--fix --deep` 在同一个 home 上必须仍然走 payload（否则「不走」可以靠删掉功能达成）；
- `--fix` 必须自己说出跳过了审计。

### F6. 验收对照

| 判据 | 结果 |
|---|---|
| A-G2/G3 审计次数 | quick 0 / `--deep` 1 / `--fix` **0** / `--fix --deep` ≤2 —— 由 patchelf recorder 计数 |
| A-G4 跳过要声明 | quick 与 `--fix` 都输出 not-audited；`--deep`、`--fix --deep` 都不输出 |
| A-G5 remedy 不变 | `--fix` 与 `--fix --deep` 的 repair 视图逐字一致，且两次输出整体不同（非空洞） |
| A-G1 真实 home 计时 | **未做**——`--fix` 会在切片上发起真实网络重装，上一轮已因此中止。次数下降是结构证据，计时留待有隔离环境时补 |
