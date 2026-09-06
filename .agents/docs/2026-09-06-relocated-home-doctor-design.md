# 被移动的 home:doctor 在销毁之前必须先问当前根

设计方案 · 2026-09-06 · 起因 [#583](https://github.com/openxlings/xlings/issues/583)
基线 `main` @ 8bfa8fa(2026.9.4.1)· 全部数字为本机实测,脚本与日志见文末

> ✅ **已实现并发布于 2026.9.5.1。** §3.8 的两个阶段合并为一个 PR:
> 门控(不再破坏)与重指(记录 / 索引缓存 / subos 清单 / 全部 subos 的链接)一起落地。
>
> 实现与自审查中新增了本文当时**没写到**的三条:
> - **B10** 索引缓存存的是绝对配方路径。不重指等于把修复阶梯关掉,剪枝接手 ——
>   量出来是「记录修好了、仍然掉 45 条注册,而同一个 home 不搬的话修好 40 条」。
> - **B11** `Config::versions()` **合并** project 与全局两个作用域,project 的记录
>   在文本上与「搬过家」同形。§3.1 的判定必须再加一条:旧根**已不存在**,或者
>   **解析到本 home**。否则会把 project 的载荷路径重指进全局 store —— 这是数据损坏,
>   不是误报。
> - **B12** §3.2 只压掉了 BrokenPayload。1173 条悬空链接与 302 条 drift 同样是同一条
>   事实的复述,也要归入那一条 finding —— 并且**压下去的必须数出来**写进那行。
>
> 落地结果与验收见 `2026-09-06-release-2026.9.5.1-notes.md`。

---

## 0. 一句话

`self doctor --fix` 在一个被 `mv` 过的 home 上,**42 秒删掉 1173 条 sysroot 链接、注销 367 条注册,而 234 个 payload 目录一个不少地躺在新根下**;它给出的理由是 *"its payload is gone and nothing can restore it"* —— 两个断言都不成立(payload 在;`xlings install <pkg>@<ver>` 1.25 秒就能恢复,不下载)。

这不是「home 不可搬迁」的后果 —— 那条 won't-do 成立且不变。这是**一个破坏性步骤只在旧根上问了「payload 还在吗」,从来没在当前根上问过**。

---

## 1. 实测结果

二进制:`~/.xlings/bin/xlings` = 2026.9.4.1。真实 home 152 G / 97 subos / 2908 条版本记录,用 `.agents/tools/slice-real-home.sh` 切片(硬链接农场),每轮结束 `verify-untouched` 均为 OK。

### 1.1 合成 home 三格矩阵(机制)

一个 `lib` 条目 + 一个 `program` 条目,各带 payload 与农场链接。

| 格 | home 状态 | `--fix` 退出码 | 结果 |
|---|---|---|---|
| A 对照 | 未移动 | **1** | 什么都没删。报 `shim table 1 missing` |
| B | `mv` 之后 | **0** | 删 `lib/libfoo.so`、`usr/include/foo.h`;注销 `progbar@2.0`;`status  1 registration(s) dropped … the rest of the home is consistent` |
| C | `mv` + 老路径 symlink | **1** | 与 A 完全一致,零破坏 |

> B 格里两个 payload 在删除的那一刻都在新根下,可执行、可读。
> **一个刚被剥掉 sysroot 和注册的 home,退出码 0;一个健康 home 因为缺一个 shim,退出码 1。**

### 1.2 真实 home 切片(规模)

| 指标 | 未移动 | 移动后 | 移动 + `--fix` 之后 |
|---|---|---|---|
| broken payloads | 45 | **411** | 44 |
| binding state | 2 | 304 | **57** |
| warnings(悬空链接) | 24 | **1173** | 0 |
| owned by another subos | 2 | 335 | 335 |
| 报告里出现 "moved"/"relocat" 的次数 | — | **0** | — |

一次 `--fix`(42 秒)的实际动作:

- **1173 条 `dangling link removed`**(default subos 内符号链接 2779 → 1643)
- **367 条注册被 drop**(版本条目 2908 → 2541,targets 1966 → 1840)
- 411 条 broken payload 里,只有 **2** 条能从索引解析出重装坐标(`linux-headers`、`zlib`);其余 **409 条直接进剪枝** —— 修复阶梯救不了搬迁,它只是把剪枝合理化了
- `data/xpkgs` 下 **234 个包目录一个没少**

修复之后再把老路径 symlink 补上(让一切重新解析),剩下的是:broken payloads 0、warnings 0、**binding state 57,其中 14 条 `xvm-binding-target-missing`**(基线 0 条)。这 14 条是剪枝的残留 —— 释放版本的成员被注销,留下的 binding root 指向不再注册的 target —— **路径恢复之后它们不会消失**。报告者说的那 6 条「看起来与移动无关」的 finding 就是这个。

### 1.3 老路径 symlink 之后再跑 `--fix`(报告者当下的状态)

移动 + 老路径 symlink,`--fix`(2026.9.4.1):

| 指标 | 前 | 后 |
|---|---|---|
| 耗时 | — | **10 分 19 秒**(无 symlink 时是 42 秒) |
| default 内符号链接 | 2779 | 3032,其中 **866 条仍然指向老路径** |
| 版本条目 | 2908 | 2986(重装带来 +78),另 pruned 6 |
| 结束状态 | — | `✗ not converging — --fix started with 350 issue(s) and ended with 603 — a repair undid another. This is a bug in doctor, not in the home; please report it` |

**doctor 自己的不收敛护栏在这个 home 上触发了。** 修复之后再跑一次 `self doctor --all`,603 条 binding state 里 **601 条是 `xvm-sysroot-drift`** —— 修复前是 302 条,也就是说 `--fix` 把同一类 finding **翻了一倍**(重装放下了指向当前根的新链接,而记录与其余链接仍名旧根,两边都被同一个文本判据判为「不是 xlings 放的链接」)。

判据在 `xvm/inspect.cpp:412`:

```cpp
const bool ours = entry.linkTarget.starts_with(payloadRoot);   // 纯文本前缀
```

链接指向的文件就在这个 home 的 store 里、解析完全正确,只因为**文本**不以当前根开头就被判成「something replaced it after the fact」,给出的补救是 `xlings use <target> <version>`。这与 `reference_canonicalize_both_sides` 记下的是同一个形态:单边路径比较冒充了「属不属于这个 home」。

**对照组已完成,归因成立。** 未移动切片跑同一条 `--fix`(同样重装了基线的 45 个 broken payload,同样下载了 .NET SDK):`binding state 2`(基线)、healed 40、pruned 6、**没有 `not converging`**。

| | 未移动 | 移动 + symlink |
|---|---|---|
| 结束时 binding state | **2** | **603** |
| `not converging` | 没有 | **触发**(350 → 603) |
| pruned | 6 | 6 |

也就是说:**`--fix` 在被移动的 home 上把 issue 数从 350 推到 603,并撞上 doctor 自己的不收敛护栏;同一命令在未移动的同一个 home 上收敛正常。** 另外两条同向证据:修完之后仍有 866 条链接指向老路径;`xvm-sysroot-drift` 从 302 涨到 601。

### 1.4 恢复代价(推翻「必须重建 1 小时」)

在被剪枝过的 home 上,payload 仍在:

```
$ xlings install xz@5.8.3          # 1.25s, exit 0
xim:xz@5.8.3 is already installed  # 没有下载
→ xz / xzcat / xzdec 全部重新注册,bindingGroup / bindingMembers 完整,path 指向新根
→ subos/default/lib/liblzma.so{,.5,.5.8.3} 三条农场链接被重新放置
```

没有老路径 symlink 也一样(`zstd@1.5.7`,7.8 s,含索引扫描,无下载)。
**结论:被 `--fix` 破坏的东西,`xlings install <name>@<version>` 每包 1–8 秒即可完整复原,不下载、不重建。报告者的一小时不必付。**

### 1.5 其它已测事实

- `--fix --dry-run` **不改动任何东西**(链接数前后一致),但**严重低报**:合成 B 格宣称 `1 action(s) planned`,真实运行做了 3 件破坏性动作;切片上宣称 `411 action(s) planned`,真实运行在此之上还删了 1173 条链接。原因:`repair_local_` 根本不在 dry 分支里(`doctor.cpp:4026-4030`)。
- dry-run 的措辞用过去时描述没发生的事:`no package provides this entry — the registration was dropped`。
- 移动 + 老路径 symlink 且**不跑 `--fix`**:broken payloads 与 warnings 都回到基线,但 binding state 是 **304**,其中 **302 条 `xvm-sysroot-drift`** —— 链接解析完全正确,只是文本前缀不等于当前根。
- `xvm-binding-target-missing` 的来源确认:基线 0,移动 + symlink 未修 0,移动 + `--fix` 后 **14**。**它是剪枝的残留,在路径恢复之后依然存在** —— 正是报告者说的那 6 条「与移动无关」的 finding,实际上是 doctor 自己修出来的。
- 只读 home 的 SIGABRT 复验:exit 1,指名不可写文件,无 abort。2026.9.4.1 的修复站得住。

---

## 2. 分类

### 2.1 真 bug(xlings 的问题)

| # | 缺陷 | 证据 | 位置 |
|---|---|---|---|
| **B1** | **破坏性修复从不在当前根上验证「payload 是否还在」。** 农场链接删除与注册剪枝共用这个盲点 | §1.1 B 格、§1.2:1173 + 367,234 个 payload 目录全在 | `sysroot_link_source_` `doctor.cpp:2258`(末尾 `exists()` 只问旧路径)→ 删除分支 `doctor.cpp:2494`;`prune_dead_registrations_` `doctor.cpp:3049` |
| **B2** | **给出的理由是假的。** `its payload is gone and nothing can restore it` —— payload 在,且 1.25 秒可恢复 | §1.3 | `doctor.cpp:3100` |
| **B3** | **损失口径只认注册剪枝。** 删了 sysroot 文件仍打印 `OK — … all consistent` 并 exit 0 | §1.1 B 格 / 早前合成实验 | `doctor.cpp:3683-3708`,三档判决只挂 `repair.pruned > 0` |
| **B4** | **`--dry-run` 不是 `--fix` 的忠实预览**,且措辞用过去时描述未发生的动作 | §1.4 | `doctor.cpp:4026-4030` 不调用 `repair_local_` |
| **B5** | **农场链接被删之后不可观测。** `FindingKind` 只有 `SysrootDangling`,没有「声明了却缺失」这一类,所以 B1 造成的损失 doctor 自己看不见 | `doctor.cppm:96-230` 全枚举 | — |
| **B6** | **没有任何东西说出「这个 home 被移动了」。** 移动后的报告里 "moved"/"relocat" 出现 0 次,尽管每条 finding 都印着旧路径 | §1.2 | — |
| **B7** | **一个可解析的 home 被判 302 条 `xvm-sysroot-drift`,`--fix` 之后变成 601 条。** 判据 `linkTarget.starts_with(payloadRoot)` 是纯文本前缀,不是链接指向的文件;而且它自我放大 —— 重装放下的新链接让同一判据两边都不满意 | §1.3 / §1.5 | `xvm/inspect.cpp:412` |
| **B8** | `library_placement` 直接读 `data.path`,没过 `expand_path`,与 `doctor.cpp:1334` 不一致。今天 0/2908 条记录使用占位符所以休眠,但它挡住任何「记录相对化」的路,也会在某个 recipe 写出占位符的那天错放链接 | 代码 | `bindings.cpp:499` |
| **B9**(小) | 只读 home 上同一条件重复报 3 次(`subos manifest` ×3、`home stamp` ×2) | §1.4 | — |

### 2.2 用法问题(不是 bug,维持现状)

| # | 事项 | 判定 |
|---|---|---|
| **U1** | 用 `mv` 搬 home | **不支持,won't-do 不变。** `PT_INTERP` 由内核按字面加载,`$ORIGIN` 在那里不展开;linker script、`.xlings-resolution.json` 同理。「构造上可搬迁」做不到,只能做成半真,那更糟 |
| **U2** | 老路径挂 symlink 让一切重新解析 | 用户侧 workaround。实测有效(C 格零破坏),但它不撤销已经造成的破坏,也不该被写进文档当作支持路径 |
| **U3** | `XLINGS_HOME` 指向全局 home、进程在不可写沙箱里 | 配置问题。它触发的崩溃是真 bug,已在 2026.9.4.1 修掉并复验 |
| **U4** | 期待 `doctor --fix` 修好一个被移动的 home | 超出设计范围。**但「不修」不等于「可以毁」** —— 这条界线正是 B1 |
| **U5** | `$MCPP_HOME` registry 是可重建缓存,移动了就重新 provision | 口径正确,报告者已接受。§1.3 只是让这条路不再是唯一路 |

---

## 3. 设计

### 3.0 边界(先说不做什么)

- **不做**「让 home 可搬迁」。ELF `PT_INTERP` / `RPATH`、linker script、payload 内部的绝对路径一律不动。
- **不做**「让搬迁后的 home 看起来是干净的」。修完之后仍然必须有一条 finding 说明:用旧根编译出来的东西不会因为这次修复而能跑。
- 本方案只解决一件事:**doctor 在销毁之前必须先在当前根上问一次。**

### 3.1 一个事实,一个产出者(`HomeRelocation`)

在 `DoctorState` 构建期、任何 payload 检查之前,推导一次:

```cpp
struct HomeRelocation {
    std::string oldRoot;      // 记录里的根
    std::string newRoot;      // Config::paths().homeDir
    std::size_t entries;      // 有多少条记录仍指向 oldRoot
    std::size_t recoverable;  // 其中多少条的 payload 在 newRoot 下真实存在
};
std::optional<HomeRelocation> relocation_of(const VersionDB&, const fs::path& homeDir);
```

放在 `DoctorState`(`doctor.cppm:327`)上作为一个字段 —— 它已经持有 `db` 和 `homeStr`,不需要新的输入:

```cpp
struct DoctorState {
    xvm::VersionDB db; …; std::string homeStr;
    std::optional<HomeRelocation> relocation;   // 构建期算一次
};
```

推导方式:遍历版本库,取每条 path 中 `/data/xpkgs/` 之前的前缀,统计众数;众数 ≠ `homeDir` 且**该前缀下的相对路径在 `homeDir` 下存在** → 判定搬迁。

- 信号是免费且普遍的:本机 2908 条带 path 的记录中,**2901 条前缀完全相同**(`/home/speak/.xlings`),占位符 0 条。
- 剩下 7 条不含 `/data/xpkgs/`(`~/.cargo/bin`、`/usr/bin`)—— 自管理工具,**必须排除在搬迁判定之外**,它们本来就不属于这个 store(与 `xvm/owner.cppm:113` 的口径一致)。这也是「取众数」而不是「取最长公共前缀」的原因:后者会被这 7 条拉成 `/`。
- 众数而不是逐条猜:一次推导、全局消费,避免出现第二个回答同一问题的人(`reference_one_question_many_answerers`)。
- 必须同时要求 `recoverable > 0`。只有旧前缀、新根下什么都没有 —— 那是真的把 payload 删了,不是搬迁,剪枝照旧。

### 3.2 一个谓词,两道门

```cpp
// 记录里的 P 在当前根下的对应物,存在则返回它
std::optional<fs::path> under_current_root(const HomeRelocation&, const fs::path& recorded);
```

**门 1 — 农场链接(`doctor.cpp:2464-2500`)**
`sysroot_link_source_` 在旧路径 `exists()` 失败后,**再用 `under_current_root` 问一次**;命中则 `place_asset` 重新指向新根(现有代码路径,已验证有效),而不是落进删除分支。

> 这不是新规则,是这段代码自己写着的规则:
> *"Deletion is what you do when nothing can say where the link belongs, not when you have not asked."*
> 被移动的 home 说得出来,只是没人在新根下问过。

**门 2 — 注册剪枝(`doctor.cpp:3049`)**
victim 的 payload 若 `under_current_root` 命中,**拒绝 drop**,并给出一条能跑的说明:

```
✗ kept   xz@5.8.3 — its payload is present under this home's current root;
         run `xlings install xz@5.8.3` to re-register it (no download)
```

`xlings install <pkg>@<ver>` 是实测过的恢复动作(§1.3),不是推测出来的建议。

**同时**:`BrokenPayload` 的判定本身(`doctor.cpp:1330`)在搬迁命中的条目上不再报「path missing」,改为归入 `HomeRelocated`。否则 411 条错误会盖住那一条真正说明情况的 finding。

### 3.3 说出条件(B6)

```
✗ home relocated   records were written for <old>, this home is at <new>
                   367 registration(s) and 1173 sysroot link(s) still name the old path;
                   every payload they name is present under the current root
   → run           xlings self doctor --fix
   • note          binaries carrying the old path in PT_INTERP/RPATH are NOT repaired by this;
                   if compilers or the runtime fail, re-provision the home
```

`note` 那一行是这条方案对 U1 的诚实交代 —— 修完不等于搬迁成功。

### 3.4 损失口径(B3)

`RepairReport` 增加 `removedAssets`,三档判决同时读它:

```cpp
if (repair.pruned > 0 || repair.removedAssets > 0) { /* lossy 档,说清删了什么 */ }
```

退出码维持现有规则(「这个 home 还需要处理吗」),不因为有损而变 —— 与 #585 已立的口径一致。真正的修复是门 1/门 2 让**不该发生的删除不再发生**,而不是靠退出码事后表态。

### 3.5 dry-run 保真(B4)

`repair_local_` 增加 `bool dryRun`,dry 分支(`doctor.cpp:4026`)调用它,把重新指向 / 删除逐条列入 `out.planned`;措辞改为将来时(`the registration would be dropped`)。
**验收:同一个 home 上,`--dry-run` 列出的动作条数 == `--fix` 实际执行的条数。**

### 3.6 `library_placement` 过 `expand_path`(B8)

一行修改 + 一个用 `${XLINGS_HOME}` 路径的单测。它今天不会发作,但它是 B1 门 1 的下游,也是任何「记录相对化」方案的先决条件。

### 3.7 留作后续,不在本次范围

- **B5 缺失农场链接的可观测性**:农场是可以从版本库推导出来的(每个 active `lib` 条目 → `library_placement`),所以「声明了却不存在」是可计算的。做法与 shim 表同构(一张派生表、一个写者),同时覆盖 #586 的 per-SubOS 落链问题。**独立开 issue。**
- **B7 `xvm-sysroot-drift` 的判据**:应当比较链接**解析后的目标**是否落在本 home 的 store 内,而不是比较链接文本的前缀。独立开 issue(与 `reference_canonicalize_both_sides` 同类)。
- **B9** 重复 finding 去重。
- 版本库改存 `${XLINGS_HOME}`:能从根上消灭这一类,但需要审计 **70 处**直接读 `path` 的调用(B8 就是其中一处),且对老 home 无效。**本次不做**,B8 是它的前置。

### 3.8 需要你拍板的一处

门 1/门 2 之后,`--fix` 还要不要**主动把版本库里的 2908 条路径前缀改写到新根**?

- **不改写(阶段一)**:doctor 不再破坏,说出条件,把恢复交给 `xlings install`。代价:用户每次跑 doctor 都看到几百条 finding,除非逐包重装或者重建。
- **改写(阶段二)**:一次前缀替换 + 农场重新指向,home 的簿记完全恢复(等价于对每个包跑一次 install,只是不走安装路径)。代价:它离「让 home 看起来可搬迁」只差一句话 —— 必须由 3.3 的 `note` 挡住,否则就是被否掉过的「半真」。

**决定(2026-09-06):两个阶段合并,单个 PR。** 分开发布会留下一个"报得出、修不了"的中间态 ——
用户看到一条 `home relocated`,而唯一的出路仍然是逐包 `xlings install` 或者重建。
风险控制放在谓词与写入两侧:①探测必须能指着一个真实存在的 payload 才成立(否则真空 home
的死记录永远删不掉);②改写只做前缀替换、必须落在路径边界;③改写在状态锁内、走既有的
`Config::save_versions()`;④真删掉的 payload 仍然照常剪枝(验收 T3)。

---

## 4. 验收(每条都是差分,必须在 2026.9.4.1 上失败)

| # | 场景 | 2026.9.4.1 | 期望 |
|---|---|---|---|
| T1 | 合成 home,`mv` 后 `--fix` | 删 2 条链接 + 注销 1 条,exit 0 | 0 删除、0 注销,报 `home relocated`,链接被重新指向新根 |
| T2 | 同上,真实切片 | 1173 删除 / 367 注销 | 0 / 0;`self doctor` 之后 broken payloads 回到基线 45 |
| T3 | **真删 payload**(`rm -rf` 掉 store 目录)后 `--fix` | 剪枝 | **仍然剪枝** —— 门不能把合法剪枝一起关掉 |
| T4 | `--dry-run` vs `--fix` 动作条数 | 1 vs 3;411 vs 411+1173 | 相等 |
| T5 | 只读 home | exit 1 指名路径 | 不回归 |
| T6 | `library_placement` 拿到 `${XLINGS_HOME}/…` | 返回未展开的字面路径 | 展开 |
| T7 | 一个 `--fix` 删除了 sysroot 资产的运行 | `status OK — … all consistent` | 判定为有损,说清删了什么 |

T3 是这套门控最容易被做错的地方:**门的条件是「payload 在当前根下存在」,不是「这个 home 被移动过」**。搬迁只是让这个条件成立的常见原因。

---

## 5. 被实测推翻的候选设计

| 候选 | 为什么不成立 |
|---|---|
| 「加一条诊断就够了」(报告者与我上一轮的结论) | 诊断不阻止删除。实测:1173 + 367 |
| 「修复阶梯会先把包重装回来,剪枝只发生在真没救的条目上」 | 411 条 broken payload 里只有 2 条能解析出重装坐标,409 条直接进剪枝 |
| 「`--dry-run` 可以用来预判风险」 | 它不调用 `repair_local_`,1173 条删除一条都不列 |
| 「那 6 条 `xvm-binding-target-missing` 与移动无关」 | 基线 0 / 移动未修 0 / 移动且 `--fix` 后 14。它们是剪枝的残留,在路径恢复后依然存在 |
| 「恢复必须重建,约一小时」 | `xlings install xz@5.8.3` 1.25 秒、无下载,注册与农场链接全部复原 |
| 「老路径 symlink 之后 home 就干净了」 | broken/warnings 回到基线,但 302 条 `xvm-sysroot-drift` 是文本前缀判据的产物(B7) |

---

## 6. 证据

- 脚本:`$SCRATCHPAD/{reloc_matrix,reloc_repro,reloc_dryrun,reloc_e3}.sh`
- 日志:`$SCRATCHPAD/{relocmatrix/*.log, sliceB.dry.log, sliceB.fix.log, sliceD.moved.log, sliceC.unmoved.log, sliceB.after.symlink.log, sliceF.all.log, e3.install.log, e3b.install.log, ro.log}`
- 切片工具:`.agents/tools/slice-real-home.sh`,每轮 `verify-untouched` = OK(真实 `~/.xlings/data/xpkgs` 未被改动)
- 代码:`doctor.cpp:1330 / 2258 / 2464-2500 / 3049 / 3100 / 3683-3708 / 4026-4030`、`doctor.cppm:96-230`、`bindings.cpp:482-504`、`config.cpp:501`、`installer.cpp:2750 / 3092`
