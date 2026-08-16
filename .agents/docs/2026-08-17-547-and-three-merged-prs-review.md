# #547 与三个已合入 PR 的复盘：分析与优化方案

日期：2026-08-17 · 基线：`bf9751d`（#551 合入后的 main） · 状态：待 review

---

## 0. 三行结论

1. **#547 的现象描述有一半是错的，而真相更难受。** `self doctor --fix` 确实会回填
   `subos_info` —— 只回填**当前活跃的那个 subos**。真正的两个缺陷是：(A) 其余 subos
   没有任何入口，而代码注释指向的 `subos doctor --fix` **这个命令不存在**；
   (B) 回填出来的声明**可能是假的** —— 在你自己的真机上，5 个有块的 subos 里有 2 个
   声明 `glibc@2.44` 而 sysroot 实际服务 `2.39`，其中一个就是 issue 里那个 `mcpp-test`。
2. **#550 把自己诊断的那个缺陷又造了一遍。** 它的核心论点是「参数没有调用者就不是功能」，
   而它给 `collect_matches_` 新加的 `forSearch` 参数，唯一的调用者没有传它。
3. **#551 有三个问题，其中一个是数据丢失。** 回滚分支 `remove_all(trash)` 删掉的正是它已经
   挪出去的文件 —— 注释说「挪了一半比原封不动更糟」，然后代码把「挪了一半」变成了「删了一半」。
   另外 `.trash-*` 会泄漏进版本命名空间（7 处代码把 `xpkgs/<pkg>/*` 的每个子目录当版本读），
   且这套逻辑只有 1 个调用点、0 个测试，而它唯一有意义的平台的 CI 在跑到任何 E2E 之前就红了。

---

## 1. 方法：每条结论是怎么来的

本文所有断言标注来源，只有三种：

- **【实测】** 在这台机器上跑出来的，命令附在正文里，可复现。
- **【读码】** 从 `bf9751d` 的源码直接确定的，附文件:行号。
- **【推断】** 由前两者推出、但没有直接观测的。**推断不作为验收依据。**

用到的二进制：`target/x86_64-linux-gnu/4cb652ca216e49d5/bin/xlings`
（2026.8.14.1，`subos_info` 相关代码自那以后未变 —— #549/#550/#551 都没碰
`src/core/subos/` 与 `src/core/xself/init.cpp`）。

---

## 2. #547：现象、真因、以及为什么现在写不出「未知」

### 2.1 实测：`doctor --fix` 会回填，但只回填活跃的那一个

隔离 home，两个 subos 都只有 `{"workspace":{}}`：

```bash
H=/tmp/h547
mkdir -p $H/subos/{default,mcpp-test}/{bin,lib,usr} $H/data/xpkgs $H/bin
printf '{"activeSubos":"default","subos":{"default":{"dir":""}}}' > $H/.xlings.json
printf '{"workspace":{}}' > $H/subos/default/.xlings.json
printf '{"workspace":{}}' > $H/subos/mcpp-test/.xlings.json

# 直接调二进制，不要走 shim ——
# 见 reference_shim_rewrites_xlings_home：shim 会把 XLINGS_HOME 改回真 home
XLINGS_HOME=$H ./target/x86_64-linux-gnu/<fp>/bin/xlings self doctor --fix
```

【实测】结果：

```
⚠ subos runtime   subos 'default' declares runtime glibc@2.44, but its active
                  XVM runtime is <none>; declared runtime payload glibc@2.44 is missing
  → run           xlings install glibc@2.44
· subos manifest  described subos 'default' (runtime glibc@2.44)
▸ healed          1
```

```
subos/default/.xlings.json    → 写入了 subos_info（runtime glibc@2.44）
subos/mcpp-test/.xlings.json  → {"workspace":{}}   原样未动
```

所以 issue 标题里的「never backfilled」不成立，成立的是「**只对活跃 subos 生效**」。

**这一条同时暴露了第二个、更重要的缺陷**：同一次运行里，`--fix` 先写下
`runtime = glibc@2.44`，然后**在下一行警告这个声明是假的**。写入方和检查方在同一次
输出里互相矛盾。

### 2.2 为什么只覆盖活跃 subos —— 而它的「负责人」不存在

【读码】三处：

| 位置 | 内容 |
|---|---|
| `doctor.cpp:679` | `detect_subos_manifest_(st.db, st.ws, p.subosDir, ...)` —— 只传活跃 subos 目录。注释：「Other subos are not inspected from here ... a second shell may be inside one right now.」 |
| `init.cpp:297` | 「Other subos in that home are **`subos doctor --fix`'s job** — init does not enumerate them.」 |
| `subos.cpp:1439` | `usage: xlings subos <new\|use\|list\|ls\|remove\|rm\|info\|i\|stop> [name]` |

**`subos doctor` 不存在。** 注释把责任交给了一个从未实现的命令，于是这块职责落到了地上 ——
这正是本仓库反复出现的「一个问题、多个回答者」的另一面：**一个问题、零个回答者，而文档说有一个。**

`self update` 也不在路上：【读码】`update.cpp` 全文只做三件事 ——
`xlings update` / `xlings install xlings@latest` / `xlings use xlings latest`，
不触碰任何 subos manifest。

### 2.3 实测：你真机上的覆盖率与两条假声明

```bash
# 全量扫描 ~/.xlings/subos/*
```

【实测】39 个 subos 目录：

| 状态 | 数量 |
|---|---|
| 有 `subos_info` | 5 |
| 无 `subos_info` | 32 |
| `.xlings.json` 缺失/不可读 | 2 |

**5 个有块的里，2 个的声明与 sysroot 实际服务的 libc 矛盾：**

```
agent-influence  declares glibc@2.44   sysroot lib/libc.so.6 → xim-x-glibc/2.39
mcpp-test        declares glibc@2.44   sysroot lib/libc.so.6 → xim-x-glibc/2.39
```

`mcpp-test` 就是 issue 环境栏里那个 subos。**它不是「没有块」，它是「有一个假的块」。**
下游读到 `glibc@2.44`、去找一个没装的载荷、降级 —— `features.h: No such file or directory`
的直接上游就在这里。issue 提的方案（补写）如果照抄现在的 `preserved_runtime`，
**会把这个 bug 从 2 个 subos 扩散到 32 个。**

32 个无块 subos 按「能拿到什么证据」分类：

| 证据 | 数量 | 补写后果 |
|---|---|---|
| workspace 有 active glibc **且** sysroot 有 libc 符号链接 | 17 | 正确，`preserved_runtime` 源 (2) 就够 |
| 两者都没有 | 15 | **任何写入都是猜的** —— 这 15 个正是需要「未知」的 |

### 2.4 根因：现在的不变式不允许表达「未知」

【读码】`manifest.cpp:168`：

```cpp
const auto runtime = b.value("runtime", std::string{});
if (!is_binding(runtime)) {
    out.push_back({Defect::RuntimeMalformed, runtime.empty() ? "absent" : runtime});
}
```

I6 要求 `runtime` 必须是良构 binding。于是：

- 写 `runtime` 缺席 → `validate_block` 判定块无效 → D1 报 broken → `--fix` 重写块 →
  `preserved_runtime` 又回落到 `DEFAULT_RUNTIME` → **`--fix` 每次都"修复"，每次都写回假声明，
  永远不收敛。**
- 于是唯一能通过校验的写法就是编一个 —— **这不是实现疏忽，是不变式逼出来的。**

好消息：【读码】**xlings 内部所有 `info.runtime` 的读取方都已经先判 `is_binding`**：

| 读取方 | 行为 |
|---|---|
| `manifest.cpp:69` `check_runtime_activation` | `if (!is_binding(...)) return nullopt` |
| `doctor.cpp:471` | `if (mf::is_binding(info.runtime)) { ... }` |
| `xvm/commands.cpp:217` | `if (!mf::is_binding(info.runtime) \|\| ...)` |

**「runtime 缺席」对每一个在树读取方都已经是安全的。唯一挡路的是 I6 自己。**

### 2.5 顺带纠正 issue 的一处前提

issue 说「这句措辞（指向 `xlings self update` 的补救）是 xlings 自己写进块里的，mcpp 只是转述」。
【读码】`make_block()`（`manifest.cpp:267`）只写六个键：
`schema_version` / `runtime` / `envs` / `created_at` / `created_by` / `host_glibc`。
**没有任何 hint 字段。** 那句补救是 mcpp 侧自己写的。

这不改变结论，但改变修法：xlings 不能靠改一个字符串修好它，得**给 mcpp 一个真话可说** ——
即下面 §2.6 里那个可被区分的「未知」状态，和一条真的能修的命令。

### 2.6 方案

#### 决策点 1：「未知」怎么表达

| 方案 | 写法 | 代价 | 判断 |
|---|---|---|---|
| **A（推荐）** | 块存在、`runtime` 键**缺席** | 放宽 I6：`runtime` 缺席合法，存在则必须良构 | 无新字段；「旧格式」= 无块，「已知无 libc」= 有块无 runtime，issue 要的区分天然成立；在树读取方全部已兼容（§2.4） |
| B | `runtime: null` | 同上 + 所有 `value("runtime", "")` 对 null 的行为要逐个确认 | nlohmann 的 `value()` 遇到 null 类型会抛，风险大于收益 |
| C | 新增 `runtime_source: declared\|observed\|inferred\|unknown` | 新字段 + 老 client 读不到 + 需要 schema bump | 违反 `reference_read_write_invariant_asymmetry`：新字段上做门禁会打断所有老 home |

**取 A。** 并且 A 有个 B/C 没有的性质：**它不需要 schema bump。** 放宽校验是向后兼容的
—— 老 client 写的块在新 client 眼里仍然合法。

#### 决策点 2：谁来补，补到哪些 subos

不是三选一，是三件事各管一段：

| 入口 | 覆盖 | 为什么是它 |
|---|---|---|
| **`subos use <name>`（迁移主路径）** | 被进入的那一个 | 进入 subos 正是需要它的描述的时刻；此刻这个 shell 就要进去，不存在「另一个 shell 正在里面」的额外风险。**不需要新命令，老 home 用着用着就修好了。** |
| **`self doctor` 报告（只报不修）** | 全部 | `st.otherSubos` 已经在手 —— 【读码】`doctor.cpp:1829` 的 `inspect_subos_references(st.db, st.otherSubos)` 就在用它。加一条 `OtherSubos` 级别的 Notice：「N 个 subos 未描述自身，运行 `xlings subos repair --all`」 |
| **`xlings subos repair [<name>\|--all]`（新子命令）** | 指定/全部 | 显式、可脚本化、可被 mcpp 的报错文案指向。**这是 §2.5 里 mcpp 需要的那句真话。** |

**不做**：让 `self doctor --fix` 自动枚举并修改所有 subos。理由不是并发，是**语义**：
`--fix` 修的是「这个 home 当前的运行状态」，而给 15 个空 subos 补写描述是一次迁移，
不是一次修复。混进去会让 `healed N` 的 N 失去意义。

#### 决策点 3：runtime 从哪里推

把 `preserved_runtime` 从 3 源改成 4 源，**仍然是同一个函数**（不新增回答者）：

```
1. 块里已有的良构 binding                      ── subos 自己说过
2. workspace 的 active runtime                 ── use 维护的记录（现状）
3. sysroot 的 lib*/libc.so.6 符号链接指向的载荷  ── 【新】实际在服务的东西
4. ── 到此为止。推不出来 → runtime 缺席（不再回落到 DEFAULT_RUNTIME）
```

第 3 源是**观测**而不是记录，这是它的价值也是它的风险：

- 价值：`reference_absent_record_needs_observation` 说的正是这件事 —— 一个从未被记录的字段，
  用常量兜底就是在宣称一件假事。第 3 源把「常量兜底」换成「看一眼实际是什么」。
- 风险：它成了第三个回答者。**所以 2 与 3 冲突时不做静默取舍** —— 取 2（记录优先），
  同时**报一条新 finding `SubosRuntimeDrift`**：声明/记录/实际三者不一致本身就是缺陷。
  你真机上那 2 个假声明，正是这条 finding 该抓到的。

`DEFAULT_RUNTIME` 的作用域收缩为**只用于 `subos new`**（它本来就是这么写的
——`manifest.cppm:51` 的注释「Scope: NEW subos only」—— 但 `preserved_runtime` 的
fallback 参数把它漏进了每一条重建路径）。

#### 落地清单

| # | 改动 | 文件 |
|---|---|---|
| 1 | I6 放宽：`runtime` 缺席合法 | `subos/manifest.cpp:168` |
| 2 | `make_block` 接受空 runtime 时不写该键 | `subos/manifest.cpp:267` |
| 3 | `preserved_runtime` 加第 3 源（sysroot 观测），去掉常量兜底；新签名返回 `optional` 或空串 | `subos/manifest.{cpp,cppm}` |
| 4 | 新 finding `SubosRuntimeUnknown`(Notice) / `SubosRuntimeDrift`(Warning) | `xself/doctor.cppm` + `doctor.cpp` |
| 5 | `subos use` 进入前调用 `ensure_subos_info_` | `subos.cpp` |
| 6 | `xlings subos repair [<name>\|--all]` | `subos.cpp:1439` 的 dispatch |
| 7 | doctor 对其它 subos 的未描述状态出 Notice + 指向 6 | `doctor.cpp:1829` 附近 |
| 8 | mcpp 侧文案改指 `xlings subos repair`（**另一个 repo，另一个 PR**） | mcpp#427 |

#### 验收（每条都要能证伪）

1. 隔离 home，两个 subos 都 `{"workspace":{}}`，**且都无 libc 符号链接** →
   `subos repair --all` 后两个都有块，**且都没有 `runtime` 键**；`self doctor` 出
   2 条 `SubosRuntimeUnknown` Notice，退出码 0。
2. 同上但给 `subos/b/lib/libc.so.6` 造一个指向 `xpkgs/xim-x-glibc/2.39/...` 的链接 →
   b 的块 `runtime == "glibc@2.39"`，a 仍然无 runtime 键。
3. **收敛性**：对 (1) 的结果连跑三次 `self doctor --fix`，`.xlings.json` 字节不变
   （`created_at` 除外则必须不变 —— 若变化说明块在被反复重写）。
4. **回归 §2.3 的那两条假声明**：构造 `runtime=glibc@2.44` + sysroot 指向 2.39 →
   必须出 `SubosRuntimeDrift`，且 `--fix` **不得**静默改写。
5. 老 client 兼容：用 2026.8.14.1 的二进制读一个新写的、无 runtime 键的块 →
   不得崩溃、不得把它当损坏（`is_binding` 已保证，但要真的跑一次）。

---

## 3. #549（docs）：把它读成什么，和它没能关掉什么

【读码】纯文档，`.agents/docs/2026-08-14-...-plan.md` +46 行。

### 它做对的

拿到的是**非构造样本**（index CI 的 `windows-test` 真装了 msvc + windows-sdk，
23 payload / ~220 MB，下载线程与 TUI 线程并发），三个判据全 0，
并且**明确写了不能读成「修复生效了」**（样本量的是 v2026.8.10.1，即 O1–O4 之前）。
结论收窄成一句更强的话：管道那条路从来就没坏过。

这是本仓库该有的写法 —— **`reference_cross_check_harness_traps` 的正例**：
测量集合是从主张推导出来的，没测到的部分被明确标成没测到。

### 残留的两个洞

**洞 1：D4 现在处于「不可证伪」状态。** 判别实验要求一台带控制台的 Windows，
而 CI 的 job 没有附着控制台。这条会一直开着，直到有人手动跑一次 —— 而
「等某人手动跑一次」在这个仓库的历史里等于「永远开着」。

> **优化**：把它变成一个能在 CI 里跑的门。windows runner 上可以
> `AllocConsole()` + `GetConsoleScreenBufferInfo` / `ReadConsoleOutput` 读回控制台缓冲区。
> 做一个最小 harness：一个线程按行 `log::info`，一个线程刷 TUI 帧，跑 2 秒，
> 从控制台缓冲区读回来扫同样三个判据。**这把「结不了案」变成「一个 60 行的测试」。**
> 若判定成本太高，退而求其次：加一个 `XLINGS_OUTPUT_SELFTEST=1` 让进程自检并
> 退出码化，至少让手动那次跑得动、结论可记录。

**洞 2：文档自己记下的那条「顺带发现」没有落地。**
index CI 用 `v2026.8.10.1` 验证 recipe，`ci-xpkg-test.yml` 更旧（`v2026.8.8.2`）。
文档写了「记下来，因为这正是『门禁测的不是发出去的东西』那个形状」，然后就没有然后了。
**这正是 `reference_ci_green_ran_nothing` 的近亲**：门是绿的，测的是四个版本以前的东西。

> **优化**：给这两个 workflow 的 pin 加一条来源约束 —— 要么跟 `releases/latest`，
> 要么在 pin 旁边写明**为什么必须钉在这一版**并加一条 CI 检查，在 pin 落后 latest
> 超过 N 个版本时报警。**不写理由的 pin 会一直漂。**

---

## 4. #550（`info` 对索引里有的包说"不存在"）

### 它做对的

- 改在 `resolve_target` 这个唯一收口上 —— 【读码】17 个调用点
  （`commands.cpp` ×8、`resolver.cpp` ×4、`installer.cpp`、`sandbox.cpp`、
  `doctor.cpp`、`catalog.cpp`），所以 `install` / `remove` / `info` / 依赖解析
  **一次全部受益**。这是正确的层。
- **中途自己抓到的那次错误值得单独表扬**：第一版把「版本不存在」也报成「平台不支持」，
  被 E2E-76 抓到。修法是要求「请求的那个版本在该平台真能解析出来」——
  `select_version_(*pkg, plat, parsed.version).empty() → continue`。
  这是把「一句话里同时说两件互相矛盾的事」变成了不可能。
- E2E fixture 是**双向**的：`config-no-payload@9.9.9` 必须仍说 not found 且**不得**列平台，
  `config-other-platform` 必须说 no build 且必须点名 windows。
  单向 fixture 会被错误实现满足 —— 这一点在 fixture 注释里写明了。

### 缺陷 1：它把自己诊断的那个 bug 又造了一遍

PR 的论点原文：

> 「参数没有调用者就不是功能，而唯一为它而写的地方正是唯一没用它的地方。」

【读码】它给 `collect_matches_` 新加了 `bool forSearch = false`：

```cpp
// catalog.cppm:227
std::vector<PackageMatch> collect_matches_(const std::string& target,
                                           const std::string& platform,
                                           bool forSearch = false) const;
```

`collect_matches_` 的调用者只有一个：

```
catalog.cpp:615:  auto matches = collect_matches_(target, platform);   // 没传
```

而 `platforms_offering_` 是绕过 `collect_matches_` 直接调 `build_matches_` 的
（`catalog.cpp:583`）。**所以新加的这个参数，今天没有任何调用者。**

> **优化**：把 `collect_matches_` 的 `forSearch` 删掉。它是顺手加的对称性，
> 不是需求。**留着它，下一个人会以为 `collect_matches_(x, y, true)` 是被支持的路径。**
> （若确实想让 `search` 走同一条路，那就让 `search` 真的调它 —— 但那是另一件事，
> 不该以一个没人传的默认参数的形式先躺在这里。）

### 缺陷 2：`platforms_offering_` 把包又解析了一遍

【读码】`build_matches_` 内部已经 `load_package(*matched)` 过一次并丢弃
（`catalog.cpp:408`），`platforms_offering_` 拿到 match 后又 `load_package(m.rawName)`
（`catalog.cpp:585`）。只发生在错误路径上，代价可接受，但：

> **优化**：`PackageMatch` 已经带 `pkgFile`；要么让 `build_matches_` 顺手把
> 平台集合算出来，要么加个 `load_package` 的小缓存。**低优先级，记录在案。**

### 缺陷 3：一个门禁可能被这次改动悄悄削弱

【读码】`tests/e2e/install_subindex_first_run_test.sh:119`：

```bash
if grep -qE "package 'testd2x:d2testpkg[^']*' not found" <<<"$CLEAN"; then
  fail "#366 regression: sub-index package 'not found' on first run"
fi
```

这是**靠匹配错误文案**来守 #366 的。今天它仍然有效（首次运行时包根本不在索引里，
`platforms_offering_` 返回空，文案仍是 not found）—— 但它现在依赖一个「不会走到新分支」的
假设，而那个假设没写在任何地方。

> **优化**：把这条守卫从「文案不出现」改成「**安装真的成功**」。
> `gate_the_message_on_behaviour` 说的正是这个：守在文案上的门，在行为变了之后还会继续绿。

### 验收

- `xlings search msvc` 与 `xlings info msvc` 在 Linux 上不再给出互相矛盾的答案（已有 E2E 覆盖）。
- 删掉 `collect_matches_` 的 `forSearch` 后全量编译 + E2E 通过。
- `install_subindex_first_run_test.sh` 改成断言安装成功后仍然通过。

---

## 5. #551（卸载删不掉被占用的 payload）

### 它做对的

三种拒绝方式的分类是准的（只读位 / 文件被打开 / 目录被打开），处置顺序也是对的
（清只读 → 挪走 → 接受目录骨架）。「Windows 允许重命名打开着的文件，删则不允许」
是这套方案的支点，而**判断它属于 xlings 而不是 mcpp 是对的** —— 17 个
`resolve_target` 调用者说明 xim 有很多消费者，每个自己写一遍这套逻辑会以很多种方式写错。

而且【实测】你贴的那条 CI 日志末尾就有它的证据：

```
Cleaning up orphan processes
Terminate orphan process: pid (8696) (vctip)
```

`vctip.exe` 在 job 结束时还活着 —— 注释里写的那个进程，是真的。

### 缺陷 1（严重 / 数据丢失）：回滚分支删掉了它已经挪走的文件

【读码】`installer.cpp:2995-2998`：

```cpp
if (moved != files.size()) {       // half-moved is worse than untouched
    fs::remove_all(trash, ignore);
    return false;
}
```

`trash` 里装的**正是已经成功挪出去的那些文件**。`remove_all(trash)` 把它们**删了**，
不是挪回去。结果：

- payload 目录里少了一个子集的文件；
- 那个子集**已经不存在于任何地方**；
- 函数返回 false，上层只 `log::warn`，包仍然注册着。

注释说「缺了任意子集文件的 payload 比原封不动更糟」，然后代码亲手制造了这个状态。
**这是 `project_silent_success_pattern` 的镜像：一次失败的清理产生了比失败更坏的结果，
而输出看起来只是一条 warn。**

> **修法**：回滚必须是 `rename` 回去，不是 `remove_all`。
> ```cpp
> if (moved != files.size()) {
>     for (std::size_t i = 0; i < moved_index_list.size(); ++i)
>         fs::rename(trash / staged_name[i], files[moved_index_list[i]], ignore);
>     fs::remove_all(trash, ignore);   // 此时应为空；非空要报出来
>     return false;
> }
> ```
> 并且：挪回去也可能失败。**挪不回去必须是一条 error 而不是 warn** ——
> 那是唯一一种「载荷真的坏了」的情况，用户必须知道要重装。

### 缺陷 2：`.trash-*` 泄漏，而且泄漏进了版本命名空间

【读码】成功路径（`installer.cpp:2999-3002`）：

```cpp
ec.clear();
fs::remove_all(root, ec);
fs::remove_all(trash, ignore);   // ← 被占用的那个文件仍然被占用，这里必然失败
if (!ec) return true;            // ← 仍然返回 true
```

**挪走一个打开着的文件并不会关闭它。** 所以在这个函数唯一被需要的场景里，
`remove_all(trash)` 一定失败，`.trash-<version>` 一定留下。
注释说「The leftover directories carry no meaning and are removed on a later run」——
【读码】`remove_payload_dir` 只有 1 个调用者，参数永远是 `<store>/<pkg>/<version>`，
**没有任何代码会再碰 `<store>/<pkg>/.trash-<version>`。**

而 `trash` 建在 `root.parent_path()`，也就是**版本目录的同级**。
【读码】把 `xpkgs/<pkg>/*` 的每个子目录当成一个版本来读的地方有 7 处：

```
src/core/xim/commands.cpp:131          src/core/xself/doctor.cpp:629
src/core/xim/commands.cpp:801          src/core/xself/doctor.cpp:1415
src/core/xim/inventory.cpp:464         src/core/xself/doctor.cpp:1491
src/core/profile.cpp:338
```

【实测·读码】逐个核对过：**七处没有一处过滤点号开头的目录**，都是
`if (!verDir.is_directory()) continue;` 之后直接把目录名当版本号用。落到实际影响：

| 站点 | 会发生什么 |
|---|---|
| `doctor.cpp:1415` | 产出坐标 `xim:node@.trash-22.17.1` 的 `UnverifiedPayload` finding |
| `doctor.cpp:1491` | 深度审计把 trash 里的文件也 readelf 一遍 |
| `commands.cpp:801` | `payload_has_content(trash)` 为真 → **唯一版本已卸载的包仍读作「载荷在盘上」** |
| `inventory.cpp:464`、`profile.cpp:338` | 以 `<pkg>/.trash-<ver>` 的形式进入清单 |
| `commands.cpp:131`、`doctor.cpp:629` | 各自要求 `.xlings-resolution.json` / `bin/`，trash 里没有 → 无影响 |

**卸载失败的副作用是凭空多出一个版本。**

另外【读码】trash 里的文件名是 `<序号>-<basename>`，目录层级被拍平了 ——
所以即便想恢复也没有映射可用。**这条本身就说明当前的回滚分支不可能是「回滚」。**

> **修法（两条都要）**：
> 1. trash 放到**版本命名空间之外** —— `<home>/data/trash/<pkg>/<version>-<n>/`，
>    或至少 `<store>/.trash/`（前缀点号不够，7 处枚举没有一处过滤点号 —— 已核对）。
> 2. 给它一个**真的会来的清理时机**：`self doctor` 每次跑都尝试清一遍
>    （便宜、幂等、且 doctor 本来就在遍历 store），清不掉就报 Notice 说明谁占着。
>    「a later run」必须指向一段真实存在的代码。

### 缺陷 3：一个调用点、零测试、而唯一相关平台的 CI 没跑到

【读码】其余仍在裸用 `remove_all` 且面对同一问题的地方：

| 位置 | 场景 | Windows 上会怎样 |
|---|---|---|
| `subos.cpp:531`、`subos.cpp:1231` | `xlings subos remove` | 另一个 shell 把 subos 当工作目录 → 删不掉 |
| `xself/uninstall.cpp:302,317` | `xlings self uninstall` | 同上，且 `xlings.exe` 自己可能就在里面 |
| `xself/install.cpp:695` | 安装期清理 | 同上 |

（`installer.cpp:1026` 不在此列 —— 【读码】它只在 `installDir` 为空时触发。）

测试：**#551 没有任何测试。** `remove_payload_dir` 在匿名 namespace 里，
从单元测试够不到。而 Windows E2E：【读码】`.github/workflows/xlings-ci-windows.yml`
只跑 E2E-01…07，**仓库里 20 个涉及 `remove` 的 E2E 一个都不在 Windows 上跑。**

> **修法**：
> 1. 把 `remove_payload_dir` 提到一个可测的位置（`xim::payload` 或一个小的
>    `fs_remove.cppm`），让其余 5 个站点都用它。**「谁都没法比这更好地删掉别人占着的文件」
>    这个论点，对 subos remove 和 self uninstall 一样成立。**
> 2. **回滚分支在 Linux 上是可测的**：把某个子目录 `chmod a-w`，它里面的文件就 rename 不出去
>    → 强制走 `moved != files.size()` → 断言「所有文件仍在原处」。
>    这条测试今天会红，正是缺陷 1。
> 3. trash 泄漏在 Linux 上也可测：造一个 rename 成功但 `remove_all` 失败的形状
>    （目录被设为不可写），断言 store 目录下不出现新的「版本」。

### 验收

- 上述三条测试加进 `tests/unit` / `tests/e2e` 并在 Linux 上通过。
- `xlings list` 在一次失败卸载之后，输出与卸载前逐字节相同（除被卸载的那一项）。
- 5 个 `remove_all` 站点收敛到一个函数。

---

## 6. 横切：Windows CI 现在是红的，而且它红的方式最坏

【实测】`gh run list --workflow=xlings-ci-windows.yml`：

| run | 时间 | 结论 | 内容 |
|---|---|---|---|
| 31948575455 | 08-16 12:59 | ✅ success | #551 的 PR 分支 |
| **31949882496** | **08-16 13:27** | **❌ failure** | **#551 合入后的 main（同样的代码）** |

失败点【实测】：

```
tests/unit/test_runtime.cpp(771): error: Expected equality of these values:
  tm.info(tid).status      Which is: 4-byte object <01-00 00-00>   ← running
  TaskStatus::completed    Which is: 4-byte object <03-00 00-00>
[  FAILED  ] TaskManager.PromptHandling (1440 ms)
```

【读码】`TaskStatus` 枚举：`pending=0, running=1, waiting_prompt=2, completed=3`。
测试在 `respond()` 之后以 `100 × 10ms = 1s` 的上限轮询 completed，
在 runner 上没等到。**同一份代码在 PR 分支上通过 —— 这是 flake，不是回归。**

但它的代价不是「一次 flake」：

> unit test 步骤失败 → 后续 **15 个步骤全部 skipped** →
> Build / SubOS sandbox contract / Fresh XLINGS_HOME / E2E-01…07 **一个都没跑**。

也就是说：**#551 这个纯 Windows 的修复，合入 main 之后，Windows 上的端到端信号是零。**
这与 `reference_ci_green_ran_nothing` 是同一个形状的另一面 ——
那边是「绿的但什么都没跑」，这边是「红的因此什么都没跑」，两者都让人读不出真实状态。

> **优化（按性价比排序）**：
> 1. `TaskManager.PromptHandling` 的 1s 硬上限换成**带超时的条件变量等待**，或至少
>    把上限提到 10s 并在超时时打印任务的实际状态。
>    `reference_lock_timeout_couples_to_tests` 的教训在这里反过来用：
>    **超时值是测试的参数，不该是产品行为的复刻。**
> 2. Windows workflow 把 unit test 与 E2E 拆成**两个不互相阻塞的 job**（共享 build 产物）。
>    一个计时敏感的单元测试不该有权决定「今天要不要跑 E2E」。
> 3. 把 `remove_*` 系列 E2E 里**至少一条**排进 Windows leg —— #551 的整个论证前提
>    就是「Windows 上会失败」，而现在没有任何自动化能观察到这一点。

---

## 7. 优先级与落地顺序

| 序 | 项 | 为什么是这个顺序 | 规模 |
|---|---|---|---|
| **P0** | #551 缺陷 1（回滚删文件） | 唯一一条会**丢数据**的 | ~20 行 + 1 测试 |
| **P0** | Windows CI 拆 job / 放宽 flake 上限 | 在它修好之前，**下面每一条在 Windows 上都验证不了** | workflow + 1 处测试 |
| **P1** | #551 缺陷 2（trash 泄漏进版本命名空间） | 卸载失败会污染 `list`/doctor/引用计数 | ~30 行 + 1 测试 |
| **P1** | #547 全套（§2.6 的 8 项） | 影响面最大（真机 32/39），但不阻塞别的 | ~300 行 + 5 条验收 |
| **P2** | #551 缺陷 3（5 个站点收敛 + Windows E2E） | 正确但不紧急 | 中等 |
| **P2** | #550 删掉没人用的 `forSearch` | 卫生 | 3 行 |
| **P3** | #549 的两个洞（控制台自检、CI pin 漂移） | 都是「让将来能结案」，不是「现在坏了」 | 各自独立 PR |

**建议拆 PR**：P0 两条合一个（都是「让下一步能被验证」）；#547 单独一个；
#551 的收敛单独一个；#550 那 3 行搭车。**不要把 #547 和 #551 放进同一个 PR** ——
一个是迁移语义，一个是文件系统语义，混在一起没法单独回滚。

---

## 8. 一句话的横切观察

这四件事（#547 的假声明、#550 的空参数、#551 的假回滚、#549 的不可证伪）
是同一个形状的四个面：

> **一段代码宣称了一件它没有观测过的事，而宣称和事实长得一模一样。**

- `preserved_runtime` 用常量兜底 → 宣称一个从未观测的 runtime；
- `collect_matches_(…, forSearch)` → 宣称一条没人走的路径；
- `remove_all(trash)` 回滚 → 宣称「已回滚」而实际是「已删除」；
- D4 的判别实验没有 CI 载体 → 宣称「待实测」而实际是「永不实测」。

`project_silent_success_pattern` 记的是「没发生」与「成功了」输出相同。
这四条是它的上游：**「没观测」与「已确认」在代码里长得相同。**
每一条的修法都一样 —— **让缺席可被表达，并且让它看起来就是缺席。**
