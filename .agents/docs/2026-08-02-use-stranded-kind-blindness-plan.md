# `use` 把「换了个包」当成「切了一半」：跨包切换的误报、噪音与 `--strict` 死锁

**日期**: 2026-08-02
**类型**: 计划（plan）→ **已实现，随 `2026.8.2.1` 发布**
**基线**: `2026.8.1.2`（已发布，源 `b720f34`）
**代码**: `src/core/xvm/switch_plan.cppm`、`src/core/xvm/types.cppm`、
`src/core/xvm/commands.cppm`、`tests/unit/test_xvm_switch.cpp`、
`tests/e2e/group_switch_report_test.sh`
**相关**: [#468](https://github.com/openxlings/xlings/issues/468)、
`.agents/docs/2026-07-30-cli-determinism-and-followup-plan.md` §2.2（stranded 报告的出处）、
`.agents/docs/2026-08-01-alias-resolution-and-inactive-install-plan.md`（provider 归属口径）

---

## 结论先行

1. **`stranded` 的语义前提是"用户离开了一个 release，它的一部分没跟上"。跨包切换
   不满足这个前提**：`xlings use java 25.0.4-zulu` 是把 `java` 这个名字的归属从
   temurin 交给 zulu，temurin 本身一个字节没动、仍然完整、仍然 active。没有任何东西
   "掉队"，所以**默认不该有一行 warn**。
2. doctor 早在 `2026.8.1.1` 就采纳了这个口径（`held_by_another_provider_`：
   *"a program name held by another provider is ownership, not incoherence"*）。
   **`use` 是唯一没跟上的那一处** —— 又一次报告方判据漂移。
3. 用户真正需要知道的不是"谁掉队了"，而是**"你这次不是换版本，是换了个包"**。所以
   把 warn 换成切换行上的一句归属提示，细节留给 `-v`。

---

## 1. 现场（你的真实 home，`2026.8.1.2`）

### 1.1 JDK：一行 warn，指着一个不存在的程序

```
$ xlings use java 25.0.4-zulu
[xlings] java -> 25.0.4-zulu
[warn] 1 program(s) not in java@25.0.4-zulu, still on the old release:
[warn]     jdk-temurin (still 25.0.4+7)          ← kind = "group"，不落 shim/lib/file
[warn]   move: xlings use jdk-temurin <version>  ← 它只有一个版本，且已经 active
[warn]   drop: xlings remove jdk-temurin@25.0.4+7 ← 卸掉用户想留着的 JDK
```

**为什么 `use java` 会有 warn？** 因为三家 JDK 的程序集**完全一样**：

```
jdk-corretto.lua:159  local PROGRAMS = { "java", "javac", "jar", "javadoc", "jshell" }
jdk-temurin.lua :212  local PROGRAMS = { "java", "javac", "jar", "javadoc", "jshell" }
jdk-zulu.lua    :155  local PROGRAMS = { "java", "javac", "jar", "javadoc", "jshell" }
```

五个程序一个不差地跟着切了。**唯一"没跟过来"的就是那个 group 根**——一个不落任何
文件的虚拟节点。所以修好之后，`xlings use java <任意发行版>` 是**零 warn**，不是"少几
行 warn"。（如果哪天某家 JDK 多提供一个 `jwebserver`，那才是值得说一句的差异——见
§3 P2 的 `-v` 清单。）

### 1.2 gcc：18 行 warn，`--strict` 拒绝

从你 home 的 `~/.xlings/.xlings.json` 直接算出来的：

```
xim-gnu-gcc@16.1.0       成员 23 个
xim-musl-gnu-gcc@16.1.0-musl  成员 6 个
gcc 16.1.0 -> 16.1.0-musl 会报: 18 条
['gcc-ar','gcc-nm','gcc-ranlib','gcov','gcov-dump','gcov-tool',
 'libasan.so','libasan.so.8','libatomic.so','libatomic.so.1',
 'libgcc_s.so','libgcc_s.so.1','libstdc++.so','libstdc++.so.6',
 'x86_64-linux-gnu-c++','x86_64-linux-gnu-g++','x86_64-linux-gnu-gcc',
 'xim-gnu-gcc']
```

这 18 个名字，musl 那个包**压根不提供**，用户无处可"move"，"drop" 又等于卸掉 gnu
gcc。**提示 100% 不可执行。**

### 1.3 两条硬证据，决定了修复路线

| 事实 | 出处 | 推论 |
|---|---|---|
| `xim-gnu-gcc` 的 `kind` 是 **`program`**，不是 `group`（尽管 `gcc.lua:215` 现在写着 `type = "group"`——这条 entry 是更早的 recipe 注册的，状态不会回头重打类型） | 你 home 的 `versions` 段 | **只按 kind 过滤，对 gcc 场景一条都挡不住**（18 → 18） |
| `provider` 字段在两边都齐全：`xim:jdk-temurin` / `xim:jdk-zulu`、`xim:gcc` / `xim:musl-gcc` | 同上 | **按 provider 判"是不是换了个包"是可判定的、且已在状态里** |

所以 issue 里写的 P1（"跳过非 program 的成员"）**不是本方案的核心**，只能算第二道闸。

---

## 2. 判据：什么才算"掉队"

`stranded` 这个特性当初的现场是：`xlings use llvm 20.1.7` 打了一行，`clang` 还在答
22.1.8。那里的关键是 **同一个包的两个 release**，用户明确离开了 22.1.8，而 `clang`
没跟上——**用户想要的整体，散了。**

| 场景 | release 有没有散 | 旧包还完整吗 | 默认该说什么 |
|---|---|---|---|
| **同包换版本**（llvm 20↔22、gs-probe 1↔2） | **散了**：留下的名字指向用户刚离开的 release | 不完整了 | **报**（今天的行为，正确） |
| **跨包换归属**（temurin↔zulu、gcc↔musl-gcc） | **没散**：整组一起切了，旧包原封不动、仍 active | 完整 | **不报**，只提示"换包了" |

第二行正是 doctor 的 INV-2 从 `2026.8.1.1` 起的口径：一个名字被**另一个 provider**
持有，是**归属**，不是不一致（`inspect.cppm:313`、`detail_::held_by_another_provider_`）。
`use` 只是没跟上。

---

## 3. 方案

### P1（核心）— `stranded` 只在**同一 provider** 内成立

`switch_plan.cppm:255-272`，在进入枚举之前先问"这次是不是换了个包"：

```cpp
// 换包 ≠ 半切。
//
// `use java 25.0.4-zulu` 把 `java` 这个名字从 temurin 交给 zulu：整个 zulu release
// 一起切过来了，temurin 一个字节没动、仍然完整、仍然 active。没有任何东西掉队,
// 所以没有任何东西可报 —— 而"报"的代价是实打实的：gcc 从 gnu 切到 musl 会刷 18 行
// 不可执行的提示，`--strict` 直接拒绝，两个发行版之间因此永远切不动（根的名字就是
// 包名，两个包按构造不可能同名）。
//
// 这也是 doctor 从 2026.8.1.1 起的口径:一个名字被另一个 provider 持有是归属,不是
// 不一致（inspect.cppm 的 held_by_another_provider_）。use 是最后一处没跟上的。
//
// 元数据缺失（provider 记录之前写下的 entry）按"同一个包"处理:老 home 保留更严格
// 的旧行为,而不是悄悄丢掉一次检查。和 held_by_another_provider_ 的回落方向一致。
std::string provider_of_(const VersionDB& db, const std::string& target,
                         const std::string& version) {
    const auto it = db.find(target);
    if (it == db.end()) return {};
    const auto vit = it->second.versions.find(version);
    if (vit == it->second.versions.end() || !vit->second.bindingGroup) return {};
    return vit->second.bindingGroup->provider;
}

bool same_package_switch_(const VersionDB& db, const std::string& target,
                          const std::string& from, const std::string& to) {
    const auto a = provider_of_(db, target, from);
    const auto b = provider_of_(db, target, to);
    if (a.empty() || b.empty()) return true;   // 回落：按同包处理
    return a == b;
}
```

计划里同时记下这次切换的两个包名，渲染层要用（见 P2）：

```cpp
struct UseSwitchPlan {
    // ...
    std::vector<StrandedMember> stranded;

    // 这次切换的 release 坐标，渲染切换行用（见 P2）。provider 名是用户
    // `xlings install` 时会打的那个名字 —— 不是 binding 根名（`xim-gnu-gcc`），
    // 根名是实现细节，用户没见过。`from*` 在首次激活时为空；两端都为空表示
    // 元数据缺失（老 entry 没有 bindingGroup），此时切换行回到今天的单行。
    std::string fromProvider;
    std::string fromProviderVersion;
    std::string toProvider;
    std::string toProviderVersion;
};
```

枚举循环只在同包时执行；跨包时 `stranded` 保持为空，`fromProvider` / `toProvider`
被填上。

### P2 — 输出：把 warn 换成一句归属提示，细节交给 `-v`

`--verbose` / `-v` 是既有的全局开关（`cli.cppm:738` → `log::set_level(Debug)`），
`log::get_level()` 已导出，不需要给 `use` 加新参数。

**切换行：括号从句始终打印，不只跨包时打印。**

`xlings use java 25.0.4-zulu` 里，`java` 是**成员名**，不是包名——用户从这一行看不出
自己动的是哪个包的哪个版本。所以括号里补的是**这次切换的 release 坐标**，统一格式是
"release A -> release B"，包名相同时自然塌缩成一个：

```
# 跨包
[xlings] java -> 25.0.4-zulu   (xim:jdk-temurin 25.0.4+7 -> xim:jdk-zulu 25.0.4)
# 同包换版本（用户打的是成员名，包名在这里补上）
[xlings] clang -> 22.1.8       (xim:llvm 20.1.7 -> 22.1.8)
[xlings] gcc -> 16.1.0         (xim:gcc 15.1.0 -> 16.1.0)
# 首次激活（没有前一个 release 可比）
[xlings] java -> 25.0.4-zulu   (xim:jdk-zulu 25.0.4)
```

三个细节：

- 用 **provider 名**（`xim:jdk-zulu`，用户 `xlings install` 时打的名字），不是 binding
  根名（`xim-musl-gnu-gcc`，实现细节，用户没见过）。
- 括号里的版本是 **`providerVersion`（包版本）**，和 target 的版本不是一回事：
  `xim:jdk-temurin 25.0.4+7` vs `java 25.0.4+7-temurin`。这正好回答"我到底切的是哪个
  包的哪个版本"。
- `{target} -> {version}` 的前半段**逐字不动**（现有 E2E-49 的
  `grep -q -- "gs-probe -> 2.0.0"` 靠它）。
- 元数据缺失（老 entry 没有 `bindingGroup`）时整个括号从句省略，回到今天的单行。

**默认（同包，且真有掉队）** —— 一行汇总，细节不铺开：

```
[xlings] llvm -> 20.1.7
[warn] 47 name(s) not in llvm@20.1.7 still run from 22.1.8 — -v to list them
```

这句话要**自己解释清楚发生了什么**：同一个包的两个 release 注册的名字集合可以不一样,
`use` 只写它切过去的那个 release 的成员,新 release 没有的名字**原地不动** —— 于是
"一部分切了、一部分没切"。措辞必须同时给出三件事:**多少个**、**为什么没跟过来**
（`not in llvm@20.1.7`）、**它们现在是什么**（`still run from 22.1.8`）。

> **注意：这个场景在真实 home 上几乎不发生 —— 见 §3.6。** 全库 246 个 target 扫下来，
> 同一个 provider 的两个 release 程序集不同的，只有 `llvm 20.1.7 / 22.1.8` 一对，而那
> 一对是**脏数据**（一个 Windows payload 装进了 Linux store），不是正常的版本演化。
> 所以这条 warn 的默认形态更应该收敛而不是铺开。

版本号取**用户离开的那个 release** 的版本（切换前的 `workspace[target]`），不要取
`stranded.front().version`：成员可以带 flavor 后缀，逐个不同,拿第一个当代表会印出一个
误导性的版本号。

**`-v`（两种情况都展开）**：

```
[xlings] java -> 25.0.4-zulu  (package: xim:jdk-temurin -> xim:jdk-zulu)
[warn] 18 name(s) still come from xim:gcc:            ← 跨包：措辞是"仍来自",不是"掉队"
[warn]     libstdc++.so.6 (16.1.0, lib)
[warn]     gcc-ar (16.1.0)
[warn]     …
[warn]   xim:musl-gcc does not provide them; nothing to switch them to.
```

```
[xlings] llvm -> 20.1.7
[warn] 3 program(s) not in llvm@20.1.7, still on the old release:   ← 同包：今天的原文
[warn]     clang (still 22.1.8)
[warn]   move: xlings use clang <version>
[warn]   drop: xlings remove clang@22.1.8
```

**`--strict`**：只对**同包**掉队拒绝；跨包永不拒绝（否则 java / gcc 永远切不动，就是
issue 的原话）。拒绝时**始终列全**，不看 `-v`——那是错误路径，用户必须看到理由。

跨包的清单从哪来：`stranded` 已经为空，所以 `-v` 那一份要单独算。放在 plan 上，和
`stranded` 分开的字段，两者互斥：

```cpp
    // 跨包切换时,旧包仍然持有的名字。永远只用于 `-v` 展示,不进 `stranded`、
    // 不参与 `--strict` 判定 —— 它们没有掉队,它们属于另一个包。
    std::vector<StrandedMember> retainedByOldPackage;
```

### P3 — 第二道闸：`kind = "group"` 的成员永远不该出现在 `stranded` 里

即使同包，也可能撞上（recipe 在两个版本之间改了根名），而且这条在语义上独立成立：
group 不落 shim、不落库、不落文件（`types.cppm:162/171` 对 group 直接返回空名字，
`commands.cppm:516` 的写 shim 循环也跳过它），"还停在旧版本"描述不了任何东西。

`types.cppm`（kind 知识本来就住在这里，和 `effective_kind_of` 并排）：

```cpp
// 一个成员被留下,用户能观察到吗?
//
// 只有 group 不能:它不落 shim、不落库、不落文件。名字还在,名字背后什么都没有。
//
// 注意这和 registration/installer 里的 `namesAnArtifact`（`kind != "group" &&
// kind != "files"`）不是同一个问题:`files` 不带 source/destination *名字*,但它确实
// 往 subos 里放了一个文件,会被留下,该被算进去。
//
// 黑名单而非白名单:kind 缺失(0.4.70 之前的 entry 回落到 VInfo::type)时按"算数"
// 处理。漏报一个真程序,正是这个特性当初要终结的事;多报一个未知 kind 只是噪音。
bool kind_can_strand(std::string_view kind) { return kind != "group"; }
```

`StrandedMember` 带上 kind，渲染层用它区分 `(16.1.0)` 和 `(16.1.0, lib)`：

```cpp
struct StrandedMember {
    std::string target;
    std::string version;   // 它现在仍解析到的版本
    std::string kind;      // "program" | "lib" | "files" —— 永不为 "group"
};
```

> P3 单独看只挡得住 §1.1 的 `jdk-temurin` 一行，挡不住 §1.2 的 18 行（`xim-gnu-gcc`
> 在真实状态里 kind 是 `program`）。**它是补充，不是主修。**

---

---

## 3.5 P4（**另开 issue，不在本 PR**）— 同包掉队不该是"交叉"，应该是"停用"

评审时提出的一条，查下来**它不是偏好，是系统自己的不变量被 `use` 违反了**。

### 证据：doctor 早就是这个口径，而且写在注释里

`inspect.cppm:670-678`，`plan_incoherent_deactivation` 的开头：

> *Deactivating rather than repairing is deliberate. Repair would mean choosing
> which member is "right", and there is no basis for that choice… **An inactive
> toolchain is a visible problem the user resolves with one `xlings use`; an
> incoherent one reports the right version from `gcc --version` and fails much
> later.***

INV-2 的措辞更直接：*"an active release is active as a whole"*。

### 今天两条命令给出相反的答案

假设一次同包切换真的落下了成员（构造场景见 §3.6：真实 home 上唯一那一例是脏数据）：

| 步骤 | 结果 |
|---|---|
| `xlings use <pkg> <新版本>` | 入口切过去了，新 release 没有的成员仍指向旧 release |
| `xlings self doctor` | 报 `xvm-active-group-incoherent`（同 provider，`held_by_another_provider_` 不豁免） |
| `xlings self doctor --fix` | `plan_incoherent_deactivation` 把落下的成员**全部停用**，保留用户刚选的版本 |

**`use` 造出来的状态，doctor 判为坏，`--fix` 立刻拆掉。** 用户要走两步才到终态，中间那
一步还是个假状态。这正是仓库里反复出现的"报告方和执行方两条规则"（见
`2026-08-01-…-plan.md` 的复盘）。

### 建议

`use` 当场做 `doctor --fix` 会做的事：**新 release 没有的名字，直接停用**，并说一句：

```
[xlings] llvm -> 20.1.7   (xim:llvm 22.1.8 -> 20.1.7)
[warn] 47 name(s) not in llvm@20.1.7 were deactivated — -v to list them
[warn]   bring them back: xlings use llvm 22.1.8
```

`--strict` 的含义也随之收紧成"连停用都不接受，干脆不切"，比今天更自洽。

### 为什么单独一个 PR

三个已知代价，每个都要自己的 e2e：

1. **停用之后会变成 `InactiveInstalled`**（2026.8.1.1 加的 Error 级检查）：doctor 会
   常报一条 "llvm@22.1.8 installed but no active version"。它是**真的**，但要确认
   `activation_conflict_` 的预检确实拦住了 `--fix` 把它激活回去——否则就是 2026.8.1.1
   那场乒乓的重演。
2. **shim 的归属**：停用后 shim 文件还在，跑起来打印 "no active version"，这正是 #452
   的形状。要确认它落在既有的 orphan/anchor 规则的哪一侧。
3. **`use` 从"只报告"变成"会动别的 release"**：这是 `2026-07-30` 那次刻意的取舍
   （`switch_plan.cppm:81-84` 的注释："deactivating them would be a guess about
   intent"）。推翻它的理由是充分的（doctor 已经在这么做），但要在文档里改掉那段注释，
   而不是留下两段互相矛盾的说明。

本 PR 先把**跨包误报**清掉——那 18 行和 1 行 warn 里，**一条都不是**真掉队。P4 处理的
是剩下那条真的。

**优先级：低。** 见 §3.6——真实 home 上同包掉队的唯一实例是脏数据。P4 修的是一条正确
但触发面接近零的不变量；跨包误报才是用户每天撞的那个。

---

## 3.6 附带发现：`stranded` 这个特性的立项证据，本身是一次 recipe bug

评审时问到"llvm 20 和 22 为什么注册的 program 不一样"，查下来答案是：**它们不该不
一样，那是一个已修复的 recipe bug 留下的残骸。**

```
$ ls ~/.xlings/data/xpkgs/xim-x-llvm/20.1.7/bin | head
clang-cl.exe  clang++.exe  clang.exe  clang-scan-deps.exe
libiomp5md.dll  libomp.dll  lld-link.exe  llvm-addr2line.exe  …   共 29 个
$ ls ~/.xlings/data/xpkgs/xim-x-llvm/22.1.8/bin | head
clang  clang++  clang-22  clang.cfg  …
```

**一台 Linux home 的 store 里躺着 29 个 Windows 文件。** `llvm.lua` 当时的两个过滤器都
按 `os.host() == "windows"` 判断，而不是按文件本身判断，于是把 `libomp.dll` 注册成
program、把 `clang.exe` 注册成一个名叫 `clang.exe` 的程序——29 条注册，没有一条能跑。

已由 xim-pkgindex `19d776b5`（*"fix(llvm): decide by what a file is, not by which host
is asking"*，#454，2026-07-30）修掉。但**状态不会自愈**：entry 和 payload 都还在。

这件事有两个后果，都值得记下来：

1. **`stranded` 特性的立项现场（`switch_plan.cppm:60-71`、E2E-49 头注释里写的
   "measured on a real home：`xlings use llvm 20.1.7` 打了一行，`clang++` 还在答另一个
   release"）测的就是这份脏状态。** 那句注释顺带断言 *"Two releases of the same
   package routinely register different program sets"* —— 全库 246 个 target 扫下来，
   同 provider 且成员集不同的**只有这一对**。`routinely` 站不住。特性本身没错（上游确实
   可能增删工具），但它的紧迫性被一次脏数据放大了。**实现 P1 时顺手把那句注释改准。**
2. **这台 home 该清一下**：29 个不可运行的注册目前哪个 subos 都没激活（已核，0 个
   active），所以不影响使用，但它们会让任何按 payload 探测的检查报红。
   `xlings remove llvm@20.1.7` 后按需重装即可——**在动之前先看一眼 `self doctor` 现在
   对它说了什么**，那是验证 doctor Check 3 是否真的抓得住"注册了但跑不了"的免费样本。

---

## 4. 不做什么

- **本 PR 不改 `stranded` 的动作语义**：同包掉队仍然只报告、不代为 deactivate ——
  但这一条**已被 P4 推翻，只是排期在后**（§3.5），不是长期结论。
- **不让 `--strict` 放过同包掉队**：那正是它存在的理由。
- **不给 `use` 加新参数**：`-v` 是既有全局开关，够用。
- **不动 doctor**：INV-2 的口径已经是对的，本方案是让 `use` 去对齐它，反向改动只会
  把 2026.8.1.1 修好的东西打回去。
- **不碰 xim-pkgindex**：本方案不依赖 recipe 补 `type = "group"`（真实状态证明那条路
  救不了已经装好的 home）。索引侧的 lint 可以另开 issue，属于锦上添花。

---

## 5. 任务分解

> 构建统一走 `mcpp build` / `mcpp test`（不要直接 xmake）。

### 任务 1：planner —— 跨包切换不产出 `stranded`

**文件**
- 改：`src/core/xvm/types.cppm`（新增 `kind_can_strand`）
- 改：`src/core/xvm/switch_plan.cppm:72-102`（`StrandedMember.kind`、
  `UseSwitchPlan.fromProvider/toProvider/retainedByOldPackage`）、`:255-272`（判据）
- 测：`tests/unit/test_xvm_switch.cpp`

**接口**
- 产出：`bool xlings::xvm::kind_can_strand(std::string_view)`；
  `StrandedMember{target, version, kind}`；
  `UseSwitchPlan{..., fromProvider, toProvider, retainedByOldPackage}`。

- [ ] **步骤 1：先写会失败的测试**

匿名 namespace 里加双发行版 helper（`switch_group_` 的 provider 写死成
`pkgindex:gcc`，两个包必须各自独立）：

```cpp
// 两个包提供同名程序,各有自己的 group 根 —— jdk-temurin / jdk-zulu 的形状。
//
// 三个细节都照抄真实 recipe（`pkgs/j/jdk-zulu.lua` 的 config()）:
//   * 根名就是包名（spec D1 要求 config() 注册 package.name）,所以两边的根名按构造
//     不可能相同;
//   * 程序成员用 flavor 版本（`25.0.4+7-temurin`）,和根的版本（`25.0.4+7`）不是同一
//     个字符串 —— xvm 不允许两个包认领同一个 (name, version);
//   * provider 形如 `xim:<包名>`,和真实状态里的字面量一致。
void distribution_group_(xlings::xvm::VersionDB& db,
                         std::string_view provider,
                         std::string_view root,
                         std::string_view rootVersion,
                         const std::vector<std::string>& programs,
                         std::string_view programVersion,
                         std::string_view rootKind = "group") {
    const xlings::xvm::BindingGroupRef ref{
        .provider = std::string(provider),
        .providerVersion = std::string(rootVersion),
        .group = std::string(root),
        .rootTarget = std::string(root),
        .rootVersion = std::string(rootVersion),
    };
    std::map<std::string, std::string> manifest;
    manifest[std::string(root)] = std::string(rootVersion);
    for (const auto& p : programs) manifest[p] = std::string(programVersion);

    auto& rootInfo = db[std::string(root)];
    rootInfo.type = std::string(rootKind);
    auto& rootData = rootInfo.versions[std::string(rootVersion)];
    rootData.path = std::format("/pkg/{}/{}", root, rootVersion);
    rootData.kind = std::string(rootKind);
    rootData.bindingGroup = ref;
    rootData.bindingMembers = manifest;
    rootData.bindingMembersDeclared = true;

    for (const auto& p : programs) {
        auto& info = db[p];
        if (info.type.empty()) info.type = "program";
        auto& data = info.versions[std::string(programVersion)];
        data.path = std::format("/pkg/{}/{}", root, rootVersion);
        data.kind = "program";
        data.bindingGroup = ref;
    }
}
```

四条测试：

```cpp
// ── 换包 ≠ 半切 ──────────────────────────────────────────────────
//
// `use java <zulu>` 把名字的归属从 temurin 交给 zulu:整组一起切了,temurin 原封不动、
// 仍然完整、仍然 active。没有东西掉队。而"报"的代价是实打实的:根名就是包名,两个包
// 按构造不可能同名,所以不过滤的话 --strict 永远切不动两个发行版。

TEST(XvmSwitchPlan, SwitchingBetweenTwoPackagesStrandsNothing) {
    xlings::xvm::VersionDB db;
    distribution_group_(db, "xim:jdk-temurin", "jdk-temurin", "25.0.4+7",
                        {"java", "javac"}, "25.0.4+7-temurin");
    distribution_group_(db, "xim:jdk-zulu", "jdk-zulu", "25.0.4",
                        {"java", "javac"}, "25.0.4-zulu");
    const xlings::xvm::Workspace ws{
        {"java", "25.0.4+7-temurin"}, {"javac", "25.0.4+7-temurin"},
        {"jdk-temurin", "25.0.4+7"}, {"jdk-zulu", "25.0.4"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "java", "25.0.4-zulu");

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_TRUE(plan->stranded.empty());
    EXPECT_EQ(plan->fromProvider, "xim:jdk-temurin");
    EXPECT_EQ(plan->toProvider, "xim:jdk-zulu");
}

TEST(XvmSwitchPlan, NamesTheOtherPackageStillOwnsAreListedSeparately) {
    // 旧包多一个新包没有的程序:它没有掉队,它属于另一个包 —— 只进 -v 的清单,不进
    // stranded,不参与 --strict。
    xlings::xvm::VersionDB db;
    distribution_group_(db, "xim:jdk-temurin", "jdk-temurin", "25.0.4+7",
                        {"java", "javac", "jwebserver"}, "25.0.4+7-temurin");
    distribution_group_(db, "xim:jdk-zulu", "jdk-zulu", "25.0.4",
                        {"java", "javac"}, "25.0.4-zulu");
    const xlings::xvm::Workspace ws{
        {"java", "25.0.4+7-temurin"}, {"javac", "25.0.4+7-temurin"},
        {"jwebserver", "25.0.4+7-temurin"},
        {"jdk-temurin", "25.0.4+7"}, {"jdk-zulu", "25.0.4"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "java", "25.0.4-zulu");

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_TRUE(plan->stranded.empty()) << "--strict must stay switchable";
    ASSERT_EQ(plan->retainedByOldPackage.size(), 1u);
    EXPECT_EQ(plan->retainedByOldPackage.front().target, "jwebserver");
}

TEST(XvmSwitchPlan, AGroupRootIsNeverStrandedEvenWithinOnePackage) {
    // 同包也能撞上:recipe 在两个版本之间改了根名。group 根不落 shim/库/文件,
    // "还停在旧版本"描述不了任何东西。
    xlings::xvm::VersionDB db;
    distribution_group_(db, "xim:demo", "demo-root-old", "1.0.0",
                        {"demo"}, "1.0.0");
    distribution_group_(db, "xim:demo", "demo-root-new", "2.0.0",
                        {"demo"}, "2.0.0");
    const xlings::xvm::Workspace ws{
        {"demo", "1.0.0"}, {"demo-root-old", "1.0.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "demo", "2.0.0");

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    EXPECT_TRUE(plan->stranded.empty())
        << "demo-root-old names no artifact; it cannot be left behind";
}

TEST(XvmSwitchPlan, ARealProgramLeftBehindWithinOnePackageIsStillReported) {
    // 这条是整个特性存在的理由,不能被上面三条误伤:同包换版本,程序没跟上。
    xlings::xvm::VersionDB db;
    switch_group_(db, "15.1.0", {"gcc", "g++", "gcc-ar"}, "15.1.0");
    switch_group_(db, "16.1.0", {"gcc", "g++"}, "16.1.0");
    const xlings::xvm::Workspace ws{
        {"gcc", "15.1.0"}, {"g++", "15.1.0"}, {"gcc-ar", "15.1.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "gcc", "16.1.0");

    ASSERT_TRUE(plan.has_value()) << plan.error().what;
    ASSERT_EQ(plan->stranded.size(), 1u);
    EXPECT_EQ(plan->stranded.front().target, "gcc-ar");
    EXPECT_EQ(plan->stranded.front().kind, "program");
    EXPECT_TRUE(plan->fromProvider.empty()) << "same package: no package clause";
}
```

- [ ] **步骤 2：跑，确认它红**

```
mcpp test --filter 'XvmSwitchPlan.*'
```

期望：前三条失败或编译不过（缺 `kind` / `fromProvider` / `retainedByOldPackage`
字段），第四条绿（既有行为）。**编译失败也算红,但要确认失败原因是缺字段,不是 helper
写错。**

- [ ] **步骤 3：最小实现**（§3 P1 + P3 的正文）

- [ ] **步骤 4：跑绿**

```
mcpp test --filter 'XvmSwitchPlan.*:XvmGroupHeaders.*:EffectiveKindAuthority.*'
```

既有的 `ReportsProgramsTheIncomingReleaseDoesNotHave` 等必须保持绿（`switch_group_`
两个版本用同一个 provider，是同包路径）。

- [ ] **步骤 5：提交**

```bash
git add src/core/xvm/types.cppm src/core/xvm/switch_plan.cppm tests/unit/test_xvm_switch.cpp
git commit -m "fix(xvm): switching packages is not a half-finished switch"
```

### 任务 2：`cmd_use` 输出 —— 归属提示 + 默认收敛 + `-v` 展开

**文件**：改 `src/core/xvm/commands.cppm:421-431`（`--strict`）、`:558`（切换行）、
`:567-581`（报告）

- [ ] **步骤 1：切换行始终补上 release 坐标**

`plan` 上带回四个字段（P1 任务里一并加）：`fromProvider` / `fromProviderVersion` /
`toProvider` / `toProviderVersion`，`from*` 在首次激活时为空。

```cpp
    // `java` 是成员名,不是包名 —— 光看 `java -> 25.0.4-zulu`,用户不知道自己动的是
    // 哪个包的哪个版本。括号里补的是这次切换的 release 坐标,包名相同时塌缩成一个。
    // 用 provider 名(`xim:jdk-zulu`,用户 install 时打的名字)而不是 binding 根名
    // (`xim-musl-gnu-gcc`,实现细节);版本用 providerVersion(包版本),它和 target 的
    // 版本不是一回事(`25.0.4+7` vs `25.0.4+7-temurin`)。
    if (plan->toProvider.empty()) {
        log::info("{} -> {}", target, resolved);          // 元数据缺失:回到今天的单行
    } else if (plan->fromProvider.empty()) {
        log::info("{} -> {}  ({} {})", target, resolved,
                  plan->toProvider, plan->toProviderVersion);
    } else if (plan->fromProvider == plan->toProvider) {
        log::info("{} -> {}  ({} {} -> {})", target, resolved,
                  plan->toProvider, plan->fromProviderVersion,
                  plan->toProviderVersion);
    } else {
        log::info("{} -> {}  ({} {} -> {} {})", target, resolved,
                  plan->fromProvider, plan->fromProviderVersion,
                  plan->toProvider, plan->toProviderVersion);
    }
```

- [ ] **步骤 2：报告分三段**

```cpp
    const bool verbose = log::get_level() <= log::Level::Debug;
    // 用户离开的那个 release。`workspace` 是 :409 读出来的本地副本,写回走的是
    // `Config::workspace_mut()`,所以到这里它仍然是**切换前**的状态 —— 正是需要的。
    const auto leftIt = workspace.find(target);
    const std::string leftRelease =
        leftIt == workspace.end() ? std::string{} : leftIt->second;

    // ① 同包掉队:默认一行汇总,-v 展开。意图不变,噪音收敛 —— 真实 home 上一次
    //    `use llvm 20.1.7` 会刷 47 行加两行提示,读者第二次就不看了。
    //
    //    `leftRelease` = 用户离开的那个 release 的版本(切换前的 workspace[target]),
    //    不是 stranded.front().version:成员可以带 flavor 后缀、逐个不同,拿第一个当
    //    代表会印出一个误导性的版本号。
    if (!plan->stranded.empty()) {
        if (!verbose) {
            log::warn("{} name(s) not in {}@{} still run from {} — -v to list them",
                      plan->stranded.size(), target, resolved, leftRelease);
        } else {
            log::warn("{} program(s) not in {}@{}, still on the old release:",
                      plan->stranded.size(), target, resolved);
            for (const auto& m : plan->stranded) {
                if (m.kind == "program")
                    log::warn("    {} (still {})", m.target, m.version);
                else
                    log::warn("    {} (still {}, {})", m.target, m.version, m.kind);
            }
            log::warn("  move: xlings use {} <version>",
                      plan->stranded.front().target);
            log::warn("  drop: xlings remove {}@{}",
                      plan->stranded.front().target,
                      plan->stranded.front().version);
        }
    }

    // ② 跨包:旧包仍然持有的名字。默认一个字都不说 —— 它们没有掉队,旧包完整如初,
    //    切换行的 (package: A -> B) 已经把发生了什么说清楚了。
    if (verbose && !plan->retainedByOldPackage.empty()) {
        log::warn("{} name(s) still come from {}:",
                  plan->retainedByOldPackage.size(), plan->fromProvider);
        for (const auto& m : plan->retainedByOldPackage) {
            if (m.kind == "program")
                log::warn("    {} ({})", m.target, m.version);
            else
                log::warn("    {} ({}, {})", m.target, m.version, m.kind);
        }
        log::warn("  {} does not provide them; nothing to switch them to.",
                  plan->toProvider);
    }
```

- [ ] **步骤 3：`--strict` 只拦同包，且拒绝时始终列全**

```cpp
    // 跨包的 retainedByOldPackage 不参与:拦它等于宣布 java 永远不能在两个发行版之间
    // 切,而根名就是包名,用户做什么都改变不了这一点。
    if (strict && !plan->stranded.empty()) {
        log::error("[xlings:use] --strict: not switching {} to {}", target, resolved);
        log::error("  {} program(s) would stay on the old release:",
                   plan->stranded.size());
        for (const auto& m : plan->stranded) {      // 错误路径:不看 -v,永远列全
            log::error("    {} (still {})", m.target, m.version);
        }
        log::error("  hint: drop --strict, or move each one first");
        return 1;
    }
```

- [ ] **步骤 4：跑既有 E2E-49**

```
bash tests/e2e/group_switch_report_test.sh
```

G2 断言 `grep -q "gs-b"` 会因为默认改成汇总行而**失败** —— 这是预期的行为变更。把 G2
拆成两条：默认只断言汇总行存在（`grep -q "still run from 1.0.0"`，并额外断言默认输出里
**没有** `gs-b`，否则"收敛"这件事无法证伪），另加一条 `RUN -v use …` 断言列表里有
`gs-b`。G1（`--strict` 列全）和 G3（无掉队时安静）不动。

- [ ] **步骤 5：提交**

```bash
git add src/core/xvm/commands.cppm tests/e2e/group_switch_report_test.sh
git commit -m "feat(xvm): say 'you switched packages', and keep the detail behind -v"
```

### 任务 3：E2E —— 两个包提供同名程序

**文件**：改 `tests/e2e/group_switch_report_test.sh`（追加第二组 fixture + G4/G5/G6）

- [ ] **步骤 1：加 fixture（非对称，才有意思）**

`gsx-alpha` 提供 `gsx-tool` + `gsx-extra`，`gsx-beta` 只提供 `gsx-tool`：

```bash
add_gsx_pkg() {   # $1 = alpha|beta, $2 = 额外程序（可空）
  local d="$1" extra="$2"
  cat > "$LOCAL_INDEX_DIR/pkgs/g/gsx-$d.lua" <<LUA
package = {
    spec = "1", name = "gsx-$d", description = "two-package fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = {
        linux   = { ["1.0.0"] = {} },
        macosx  = { ["1.0.0"] = {} },
        windows = { ["1.0.0"] = {} },
    },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
local PROGRAMS = { "gsx-tool" ${extra:+, \"$extra\"} }
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    for _, n in ipairs(PROGRAMS) do
        io.writefile(path.join(bindir, n), "#!/bin/sh\necho \"" .. n .. " $d\"\n")
    end
    return true
end
function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    -- 根不落任何可执行文件,所以声明 type = "group"（jdk-* / gcc 的真实写法）。
    xvm.add("gsx-$d", { type = "group" })
    for _, n in ipairs(PROGRAMS) do
        -- flavor 版本:xvm 不允许两个包认领同一个 (name, version)。
        xvm.add(n, { bindir = bindir, version = "1.0.0-$d",
                     binding = "gsx-$d@1.0.0" })
    end
    return true
end
function uninstall() xvm.remove("gsx-$d") return true end
LUA
}
add_gsx_pkg alpha gsx-extra
add_gsx_pkg beta ""
```

> 落地时先跑一次 `RUN list gsx-tool`，确认版本号真的是 `1.0.0-alpha` / `1.0.0-beta`。
> 如果 libxpkg 对 `version` 的处理与预期不符，**以实际输出为准改断言，不要改断言去迁
> 就一个没验证过的假设。**
>
> 另注：第二个包安装时 `gsx-tool` 已被第一个包占着，`registration.cppm:1028` 的
> `activateGroup` 会因为 `contested` 非空而**不自动激活**（2026.8.1.1 起的既有行为）。
> 测试必须显式 `use`，不能假设装完就切过去了。

- [ ] **步骤 2：写 G4/G5/G6，先对未改动的二进制跑，确认 G4 红**

```bash
# ── G4: 换包不刷 warn,也不拦 --strict ────────────────────────────────
log "G4: switching packages is quiet and --strict-clean"
RUN install gsx-alpha@1.0.0 -y >/dev/null 2>&1 || fail "install gsx-alpha failed"
RUN install gsx-beta@1.0.0  -y >/dev/null 2>&1 || fail "install gsx-beta failed"
find "$HOME_DIR/data/xpkgs" -type f -name 'gsx-*' -exec chmod +x {} +
RUN use gsx-tool 1.0.0-alpha >/dev/null 2>&1 || fail "use alpha failed"

rc=0
out="$(RUN use gsx-tool 1.0.0-beta --strict 2>&1)" || rc=$?
[[ "$rc" == "0" ]] \
  || fail "G4: --strict refused a package switch; got:\n$out"
grep -qi "warn" <<<"$out" \
  && fail "G4: a package switch printed a warning; got:\n$out"
grep -q "gsx-alpha" <<<"$out" \
  || fail "G4: the output does not say which package was switched away from; got:\n$out"
grep -q "gsx-tool beta" <<<"$(RUN_SHIM gsx-tool)" || fail "G4: the switch did not take"
# 旧包原封不动:它多出来的程序照常跑。
grep -q "gsx-extra alpha" <<<"$(RUN_SHIM gsx-extra)" \
  || fail "G4: the other package's own program stopped working"

# ── G5: -v 才展开归属清单 ────────────────────────────────────────────
log "G5: -v lists what the old package still owns"
RUN use gsx-tool 1.0.0-alpha >/dev/null 2>&1 || fail "use back to alpha failed"
vout="$(RUN -v use gsx-tool 1.0.0-beta 2>&1)" || fail "verbose use failed"
grep -q "gsx-extra" <<<"$vout" \
  || fail "G5: -v does not name what stayed with the old package; got:\n$vout"

# ── G6: doctor 对两个包共存保持安静,--fix 不把选择改回去 ─────────────
log "G6: doctor is clean after a package switch"
doc="$(RUN self doctor 2>&1 || true)"
grep -qi "gsx-alpha\|gsx-beta" <<<"$doc" \
  && fail "G6: doctor flagged the coexisting packages; got:\n$doc"
fix="$(RUN self doctor --fix 2>&1 || true)"
grep -q "gsx-tool beta" <<<"$(RUN_SHIM gsx-tool)" \
  || fail "G6: --fix moved gsx-tool back to the other package;\n$fix"
```

对**未改动的** `main` 构建跑 G4，必须红（`--strict` 退出非 0，且输出里有 `[warn]`）。
**先看到红,再改** —— 否则无法排除测试写成了永远绿。

- [ ] **步骤 3：用改过的二进制跑全绿**

```
bash tests/e2e/group_switch_report_test.sh
```

- [ ] **步骤 4：提交**

```bash
git add tests/e2e/group_switch_report_test.sh
git commit -m "test(e2e): switching between two packages that provide one program"
```

### 任务 4：版本与文档

- [ ] **步骤 1**：`mcpp.toml:3` 与 `src/core/config.cppm:16` 同步改为 `2026.8.2.1`。
- [ ] **步骤 2**：本文档头部 `类型` 改为「已实现，随 `2026.8.2.1` 发布」。
- [ ] **步骤 3**：`mcpp build && mcpp test && bash tests/e2e/run_all.sh` 全绿后提交。

```bash
git add mcpp.toml src/core/config.cppm .agents/docs/2026-08-02-use-stranded-kind-blindness-plan.md
git commit -m "chore: 2026.8.2.1"
```

---

## 6. 验收清单

| # | 断言 | 怎么验 |
|---|---|---|
| 1 | `xlings use java 25.0.4-zulu` **零 warn** | E2E G4；真实 home 手验 |
| 2 | 切换行说清"换了包"：`(xim:jdk-temurin 25.0.4+7 -> xim:jdk-zulu 25.0.4)` | E2E G4 的 `grep gsx-alpha` |
| 2b | **同包**切换也带 release 坐标：`gs-probe -> 2.0.0  (xim:gs-probe 1.0.0 -> 2.0.0)` | E2E G2 加一条 grep |
| 3 | `--strict` 能在两个包之间切 | E2E G4 的 `rc == 0` |
| 4 | `gcc 16.1.0 -> 16.1.0-musl` 从 18 行降到 0 行 | 真实 home 手验（切完记得切回来） |
| 5 | 同包掉队仍然报，默认一行、`-v` 列全、`--strict` 仍拦 | 单元 `ARealProgramLeftBehindWithinOnePackageIsStillReported`；E2E G1/G2 |
| 6 | `-v` 能列出旧包仍持有的名字 | E2E G5 |
| 7 | doctor 不因两个包共存而报警、`--fix` 不改回选择 | E2E G6 |
| 8 | 老 home（无 provider 元数据）行为不变坏 | `same_package_switch_` 的回落：任一端缺 `bindingGroup` → 按同包处理 → 仍然报 |

**必须先看到红**：验收项 1、3 要先对未改动的二进制跑一遍确认失败，再接受它们变绿。
这是本仓库反复吃亏的那类"从没发生过和成功了输出一模一样"的唯一防线。

---

## 6.5 落地记录（2026-08-02，`2026.8.2.1`）

计划与实现的差异，以及"先看到红"的两次证据：

| 计划 | 实际 |
|---|---|
| 任务 1 的 `same_package_switch_()` 布尔函数 | 实现为 `detail_::provider_of_()` 返回 `(provider, providerVersion)` 一对——切换行本来就要用 providerVersion，两个字段一次取出，省掉第二次查表 |
| 任务 1 的四条单测 | 落地五条：多加一条 `ALibraryLeftBehindWithinOnePackageIsStrandedAndSaysSo`，钉住"lib 仍然算掉队、但不叫 program" |
| E2E 只加 G4/G5/G6 | 另加 G2b（`-v` 才列名字），并把 G3 的失效断言修好——它原本 grep 的是 `still resolve`，而真实措辞从来都是 `still on the old release`，**那条断言从写下起就不可能失败** |

**红-绿证据（两条判据各验一次，缺一不可）**

1. 把 `samePackage` 钉成 `true` 重编：`NamesTheOtherPackageKeepsAreListedApartFromStranded`
   变红，而 JDK 那条**仍然绿**——说明 JDK 场景是被 kind 那道闸救的，package 判据的独占
   证据是 `jwebserver` 那条。写测试时若只留 JDK 一条，就会以为 package 判据被覆盖了。
2. 把 `kind_can_strand` 钉成恒 `true` 重编：`AGroupRootIsNeverStrandedEvenWithinOnePackage`
   变红。两道闸都是承重的。
3. E2E G4 对**未改动的**二进制（`git stash push -- src/` 后重编）确认红：

```
[error] [xlings:use] --strict: not switching gsx-tool to 1.0.0-beta
[error]   2 program(s) would stay on the old release:
[error]     gsx-alpha (still 1.0.0)          ← group 根，issue #468 的原形
[error]     gsx-extra (still 1.0.0-alpha)    ← 另一个包自己的程序
```

**顺带修正的注释**：`switch_plan.cppm` 里 `StrandedMember` 上方那句
*"Two releases of the same package **routinely** register different program
sets"* 已改写——见 §3.6，246 个 target 里同包成员集不同的只有一对，且那一对是脏数据。
留着它会让下一个人从错误的频率推出错误的优先级。

---

## 6.6 附带修复：`tools/linux_release.sh` 把验证装进了要打包的目录

实现过程中跑全量 E2E 撞到的，**与本 issue 无关，但同一个 PR 修掉**——因为它让本次发布
的产物本身不可信。

`linux_release.sh` 的网络验证段直接对 `$OUT_DIR` 执行：

```bash
xlings update            # 装进 $OUT_DIR
xlings install d2x@0.1.3 # 装进 $OUT_DIR
tar -czf "$ARCHIVE" ... "$PKG_NAME"   # 然后把 $OUT_DIR 打包
```

本地打出来的 tarball 因此带上：

```
data/xpkgs/{xim-x-d2x,xim-x-glibc,xim-x-openssl}   152 MB
.xlings.json                                        177 个注册条目
subos/{default,current}/.xlings.json                177 个 active 条目
```

用户解开这个包，第一条 `xlings` 命令面对的是**别人机器上的 home**，payload 路径在他
那里根本不存在。

**为什么一直没人发现**：所有 CI 调用方都设 `SKIP_NETWORK_VERIFY=1`，制造污染的那个分支
在 CI 里从不执行——已发布的 tarball 是干净的，而按脚本自己 Usage 行操作的人拿到的是脏
的。典型的[静默成功]形状：37 MB 和 152 MB 的包都能解开、都能跑。

**怎么暴露的**：E2E-12 用我打的包跑，`d2x` 已经在包里了 → `payloadInstalled = true`
→ elfpatch 整段被跳过（`installer.cppm:2475`）→ "elfpatch did not report 0 failures"。
一次误报把一个真缺陷顶了出来。

**修法两条**：

1. 网络验证改成对 `$OUT_DIR` 的**副本**执行（`build/.release_verify_home_$$`，
   随 trap 清理）。
2. 打包前加一道闸问**产物本身**，而不是相信流程：`data/xpkgs` 必须为空，
   `.xlings.json:versions` 与每个 `subos/*/.xlings.json:workspace` 必须为空。
   先对被污染的产物验证过它是红的（177/177/177 + 3 个 payload 目录），再修的脚本。

---

## 7. 评审决策记录（2026-08-02）

| # | 议题 | 结论 |
|---|---|---|
| 1 | 跨包切换默认给不给汇总行 | **完全静默**。那些名字新包压根不提供，没有任何可执行动作；切换行的括号从句已经说清发生了什么 |
| 2 | 括号里放 provider 名还是 binding 根名 | **provider 名 + providerVersion**，且**同包也打印** —— `xlings use java …` 里 `java` 是成员名，用户从行首看不出自己动的是哪个包 |
| 3 | 同包掉队要不要改成"停用"而不是交叉 | **要，但另开 issue**（§3.5 P4）：doctor 的 INV-2 与 `plan_incoherent_deactivation` 已经是这个口径，`use` 是违反方；三个代价各需自己的 e2e |
| 4 | `use java` 为什么会 warn | 三家 JDK 程序集完全相同，唯一"没跟过来"的是 group 根 → 修好后**零 warn** |
| 5 | 同包默认收敛是可见行为变更 | 接受；E2E-49 的 G2 拆成"默认断汇总行 + 断看不到 `gs-b`"与"`-v` 断列表"两条，不悄悄改断言迁就实现 |
