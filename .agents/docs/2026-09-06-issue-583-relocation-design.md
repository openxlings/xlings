> ♻️ **2026-09-06 晚:A1 与自动重指的判断被实测推翻,已在 2026.9.5.1 落地。**
> 见 `2026-09-06-relocated-home-doctor-design.md`。推翻它的不是「搬家应该被支持」,
> 而是三件当时没有量过的事:① 2026.9.4.1 在搬过的 home 上一次 `--fix` 删 1173 条链接、
> 注销 367 条注册,而 234 个 payload 目录全在;② 它给的理由 `its payload is gone` 是**假话**;
> ③ doctor 自己的不收敛护栏在这种 home 上触发(350 → 603 issues),对照组不触发。
> 判据也从「这个 home 搬过家吗」改成「payload 在不在当前根下」—— 后者不是搬家专属的状态,
> 绑定挂载、容器内路径、从备份恢复、父目录改名都会产生它。
>
> ⛔ **以下为当时作废的原文(2026-09-06 白天)。**
> 范围在 review 中被否决:`mv` 一个 home 不是受支持的操作,
> 因此 `self relocate`(B 组)与「拒绝注销可达载荷」(A1)都不做。
> 仍然成立的部分已改写进 `2026-09-06-doctor-blind-spots-and-honest-verdict-plan.md`。
> 保留本文件是为了留住「为什么不做」的判断过程。

# #583 后续设计:搬家不是 bug,`--fix` 的反应才是

> 排查见 `.agents/docs/2026-09-06-issue-583-home-relocation-survey.md`(全部数字来自那次实测)。
> 基线 `a5cf36e` / 2026.9.3.2。
> **决策(已确认)**:T1「home by construction 可搬」**拒绝**;
> T2「doctor 自动检测并重写」**改为显式命令 `xlings self relocate`**;
> T3「不要毁掉可恢复的 home」**摘出来当 bug 立刻修**。
> 状态:**待 review,未动任何代码。**

## 0. 一句话

这次不解决「让 home 可以随便搬」,只解决两件事:
**① 搬了之后 xlings 不许把可恢复的现场毁掉;② 想搬的人有一条能跑的命令。**
前者是 bug 修复(A 组,四条,彼此独立),后者是新命令(B 组)。
T1 的那部分明确写进「不做」并说明理由(C 组),免得下次再被当成遗漏。

| 组 | 内容 | 性质 | 能否独立发布 |
|---|---|---|---|
| **A** | prune 谓词 / `--fix` 成功判定 / 只读崩溃 / lib 断链 | bug 修复 | A1+A2、A3 可独立;**A4 依赖 A1** |
| **B** | `xlings self relocate` | 新命令 | 依赖 A1(共用「载荷可达」判定) |
| **C** | 不做 T1 | 决策记录 | — |

---

## 1. 目标与非目标

**目标**

1. 任何「记录路径失配、但载荷在当前根下可达」的情形,`--fix` **不得注销**,且必须说出这个条件。
2. `--fix` 不得把「删掉问题」呈现为「修好了」;这种情形下退出码必须非零。
3. 只读 / 无写权限的 home 上,`self doctor --fix` 报错退出,不 abort。
4. `<subos>/lib`、`<subos>/lib64` 进入断链检测;断链的修复动作由「删」改为「能重指就重指」。
5. 提供 `xlings self relocate`,把一个搬过家的 home 修回可用。

**非目标(明确不做)**

1. **不**把 `${XLINGS_HOME}` 占位符写进 versions DB(理由见 §5)。
2. **不**承诺 home 可以随意搬迁 —— `PT_INTERP` 的物理约束决定了这个承诺只能是半真(§5)。
3. **不**让 `doctor` 自动执行 relocate。doctor 只诊断、只给命令。
4. 不处理「home 跨机器复制且 store 内容不全」——那是安装问题,不是搬家问题。

---

## 2. 依赖顺序(先读这条,否则 A4 会造成负收益)

```
A1(载荷可达判定) ──┬── A2(成功判定/退出码)
                    ├── A4(lib 扫描 + 重指)   ← A4 单独上 = 删掉 431 条链接,比现状更糟
                    └── B (relocate)
A3(崩溃)          ── 独立,可最先合入
```

A4 现在的修复动作是无条件 `fs::remove`(`doctor.cpp:2413`)。
在搬过家的 home 上,先加广度、后加重指 = 把 431 条链接删干净。
**A4 必须与「能重指就重指」同一个 PR。**

---

## 3. A 组 —— 四个 bug

### A1 · prune 的谓词问错了问题

**现状**

- `doctor.cpp:1333` 「L4」:`fs::is_directory(expand_path(vdata.path, homeStr))` 为假 → `BrokenPayload`。
- `doctor.cpp:2965` `prune_dead_registrations_` 的安全论证是
  「payload is gone … There is nothing to lose that has not already been lost」——
  这个论证有一个**未声明的前提**:「gone」是从*记录里那条字符串*推出来的。

**改成什么**

新增一个判定(不新增任何存储字段):

```cpp
// xvm/owner.cppm 旁边,或 doctor 内部 detail_
// 记录路径失配,但同一个 (store, version) 在当前根下存在 → 载荷可达
std::optional<std::filesystem::path>
payload_reachable_under_current_root(std::string_view recordedPath);
```

实现:`owner.cpp:11` 的 `coordinate_from_payload_path` 已经能从**任意前缀**的路径里
右往左解析出 `<store>/<version>`(注释里写明「the two components after the LAST `xpkgs`」)。
拼上 `Config::paths().dataDir / "xpkgs"` 即得候选新路径,`is_directory` 一次即可。

判定命中时,`BrokenPayload` **改判为一个新的 finding kind**:

| | 值 |
|---|---|
| kind | `FindingKind::StalePayloadRoot`(新增) |
| level | `Error` |
| detail | `demo@1.0.0 的记录指向 <旧根>/data/xpkgs/…,当前 home 根是 <新根>;载荷在 <新路径> 存在` |
| remedy | `xlings self relocate` |
| remedyNote | `这个 home 似乎从 <旧根前缀> 搬到了 <新根>` |

并且 **`prune_dead_registrations_` 跳过它**(在 `victims` 收集处 `continue`,和「另一个 subos 还在用」那条并列)。

**为什么这样切**

- 它把「记录过期」和「载荷没了」变成两个**不同的 finding kind**,而不是同一个 kind 的两种命运。
  同名同级的两种结局是 [[reporter/repairer predicate drift]] 的温床。
- 判定与报告调用**同一个函数**,和这个文件里 `note_if_baked` 的做法一致(detection calls the
  function the repair calls,`doctor.cpp:1355` 附近的注释),避免检测/修复漂移。

**陷阱**

- **两边都要 canonicalize**([[canonicalize both sides]])。旧根可能含符号链接、`/tmp` 与
  `/private/tmp`(macOS)、8.3 短路径(Windows,[[cross-platform test traps]])。
  只规范化一边会让 143 个载荷看起来是坏的 —— 这个坑这个仓库踩过。
- 判定只看目录存在,**不看内容是否完整**。载荷可达 ≠ 载荷健康;后者仍由既有的
  ladder / elfcheck 负责。这条要写进注释,否则下一个读者会以为它保证了更多。

**验收**:§8 的 R1 / R2。

---

### A2 · `--fix` 的成功判定与退出码

**现状**(实测,survey §1):搬家的 home 上 `--fix` 打印
`status OK — workspace, shims, and payloads are all consistent`,`healed 1`,`pruned 1`,**exit 0**。
`doctor.cpp:4051` 的返回是 `after.issues() == 0 && outstanding == 0 && !repair.regressed ? 0 : 1`,
而 prune 把 finding 删掉之后 `after.issues()` 自然归零。

**改成什么**

1. `StalePayloadRoot` 存在时:`--fix` **不 prune、不 heal**,照常打印其余修复,
   末尾输出一条**结论行**而不是 `status OK`:

   ```
   ✗ status   这个 home 的记录指向另一个根 —— 6 个包的载荷在当前根下存在但注册已过期
   → run      xlings self relocate
   ```
   退出码 **1**。
2. `healed` 的口径:`repair.healed` 现在是 `before - after.issues() - repair.pruned`
   (`doctor.cpp:3987`),即 prune 已经被扣掉了 —— 这部分是对的,不动。
   要改的是**结论行**:只要本次有 `StalePayloadRoot`,就不允许出现 `OK`。
3. `pruned > 0` 时,即使全部合法,结论行也要把它单独说出来
   (「注销了 N 个无法恢复的注册」),不要混进 `healed`。

**为什么**:这是 [[silent-success pattern]] 的标准形状 ——
「没发生」和「成功了」输出相同。判据不该是「剩余 finding 数」,而是
「本次是否发生了**信息丢失**」。

**陷阱**:退出码从 0 变 1 会影响调用方。搜过了,仓库内把 `self doctor --fix` 的
退出码当断言用的地方在 `.agents/tools/doctor-acceptance.sh`(打印数字,不断言)与 e2e;
**合入时需要重新跑一遍 e2e 全量**([[assert only invariants]]:新增断言只断言不变量,
不要断言具体条数)。

---

### A3 · 只读 home 上 `--fix` SIGABRT

**现状**(实测 exit 134,gdb 栈见 survey §4):

```
platform::write_file_atomic   modules/platform/src/platform.cpp:403   throw
Config::record_client_version src/core/config.cpp:1303                无 try
xself::cmd_doctor             src/core/xself/doctor.cpp:4048
cli::run                      src/cli.cpp:1525   ← self 在这里提前分派
main                          src/main.cpp:92
```

**两处都要改,因为它们修的是两件事:**

1. **`Config::record_client_version`(`config.cpp:1303`)包 `try/catch`。**
   同一次 `--fix` 里 `doctor.cpp:2343` 写 subos manifest 的那处**是**包了的,失败时优雅打印
   `✗ subos manifest could not write …`。照抄那个形状:失败 → 一条 note,不改退出码
   (打戳失败不是 home 的问题)。
2. **顶层 `try` 提到 `cli::run` 的函数开头。**
   `cli.cpp:1804` 的注释承诺它接住「any uncaught std::exception … Convert to a logged error」,
   但 `try` 从 1812 才开始,而 `self` / `subos` / `profile` 在 **1525** 就分派完了。
   注释描述的语义和代码覆盖的范围不一致 —— 这是一整类漏洞,不止这一个写者。

**为什么两条都要**:只做 1 是修一个症状(下一个无保护写者照样 abort);
只做 2 是把 abort 变成 `internal error: Failed to write file: …`,信息量仍然不如
「could not write X」+ 继续跑完。两条正交。

**陷阱**:提前 `try` 时注意 1497–1527 之间的早退分支(`return 2` 的参数校验、
unknown command 的 `return 1`)必须保持原样返回,不要被 catch 吞成别的码。

**验收**:§8 的 R5。

---

### A4 · 断链扫描看不见库农场;而且修复动作是「删」

**现状**

- 扫描 `doctor.cpp:2069`:`for (const auto& sub : {"usr", "etc", "share"})` —— 没有 `lib`/`lib64`。
  `doctor.cpp` 全文不出现 `libDir`。
- 库农场写在 `<subos>/lib`(`installer.cpp:1764`;`Config::libDir` = `subosDir/"lib"`,`config.cpp:484`)。
- 实测:一台**从未搬过家**的真实 home,`subos/*/lib*` 下 3042 条链接、**当前就有 6 条断链**,
  `xlings self doctor` 报 **0**。
- 修复动作 `doctor.cpp:2413`:无条件 `fs::remove`。

**改成什么**

1. 扫描集合加 `lib`、`lib64`。
2. **修复规则改为:能重指就重指,不能才删。**

   ```
   对每条断链 L(目标 T 不存在):
     若 coordinate_from_payload_path(T) 命中,且新根下同名文件存在
         → 重指(复用 xvm::place_asset,它已有 staging+rename 的覆盖语义,
                 并且对「已经指向同一文件」的情况是一次 stat)
     否则 → 删(现有行为),并保留现有的 prune_empty_asset_dirs
   ```
3. 报告里把两种动作分开计数:`link repointed` / `dangling link removed`。

**为什么把重指也放进 A 组而不是只放进 relocate**:
因为「删」在**任何**情况下都是错的默认值 —— 目标不存在不等于目标不可恢复。
这条规则同时改善了 `usr/include` 那一侧(现在也是无条件删)。

**陷阱**

- ⚠️ **顺序**:见 §2。加广度必须与重指同批。
- 重指后要**再验一次**链接可解析,否则「重指了但仍然断」会被计成成功([[silent-success pattern]])。
- 不要 `follow_directory_symlink`(现有扫描已关闭),重指同理:只处理文件链接,
  目录链接原样报告。
- Windows 上库农场是硬链接/复制而非符号链接(`xvm/commands.cpp:create_link_`),
  该平台上这条扫描应当只在符号链接分支生效 —— 需要在 e2e 里显式跳过而不是假装通过。

---

## 4. B 组 —— `xlings self relocate`

### 4.1 命令形态

```
xlings self relocate [--from <old-root>] [--dry-run] [-y] [--no-elf]

  --from <old-root>   旧根;不给则从记录里推断(见 4.2)
  --dry-run           只报告每个阶段会改多少,不写任何东西
  -y, --yes           跳过确认(ELF 阶段不可逆,默认要确认)
  --no-elf            跳过阶段 3,只修记录与链接
```

落点:`src/cli/spec.cpp`(`self` 的 children,紧邻 `migrate`)+
`src/core/xself.cpp` 的 action 分派 + 新文件 `src/core/xself/relocate.{cppm,cpp}`。
帮助文本就在 spec.cpp 内联(仓库没有单独的 i18n 表)。

### 4.2 旧根从哪来 —— 不需要新字段

`.xlings.json` **没有** home 字段(键只有 activeSubos / hintsSeen / index_repos /
knownProjects / lang / mirror / repo / subos / theme / tui / version / versions / xim),
报告人提议的「recorded root」不存在。**但也不需要**:

```
候选旧根 = { p 的最长前缀 | p ∈ 所有 versions[].path,
             且 p 含 "/data/xpkgs/",取 "/data/xpkgs/" 之前的部分 }
```

- 唯一 → 就是它,打印 `X → Y` 并要求确认。
- 多个(历史上搬过两次)→ 全部列出,要求 `--from` 指定。
- 零个(所有记录都已指向当前根)→ 打印「没有需要搬迁的记录」,exit 0,**幂等**。

`--from` 覆盖推断,用于跨机器/换用户名的场景(旧根在本机根本不存在,推断仍然有效,
因为它只读记录不读磁盘)。

### 4.3 四个阶段,按「越不可逆越靠后」排列

| # | 阶段 | 改什么 | 可逆性 | 默认 |
|---|---|---|---|---|
| 1 | **记录** | `versions[].path` / `alias` / `envs` 里的旧根前缀 → 新根 | 高(状态锁下一次原子写) | 开 |
| 2 | **链接** | `<subos>/{lib,lib64,usr,etc,share}` 下目标含旧根的符号链接 → 重指 | 高(重指可再重指) | 开 |
| 3 | **ELF** | `PT_INTERP` + `RPATH/RUNPATH` 里的旧根前缀 | **不可逆**(原地改文件) | 开,需确认 |
| 4 | **文本** | `.xlings-resolution.json`;glibc 的 `lib/libc.so` linker script;`bin/ldd` 的 `TEXTDOMAINDIR` | 中 | 开 |

每个阶段独立报数,`--dry-run` 只报数。中途失败:**已完成的阶段保留**(阶段间无回滚),
因为 1→4 是单调「越来越正确」的,停在任何一步都不比停在上一步更坏。这一点要在输出里说清楚。

阶段 1 在 `xvm::acquire_state_lock(homeDir)` 下做。锁文件是 `.xlings.lock`
(`lock.cpp:16`),与被写的 `.xlings.json` **不是同一个 inode**,所以 rename 式原子写安全 ——
[[atomic write vs flock]] 的坑在这里天然避开了,但**不要**顺手把锁改到 `.xlings.json` 上。

阶段 4 里 `.xlings-resolution.json` 是最低优先级:它唯一的消费者是
`xim/commands.cpp:246` 的**展示**命令,过期只会打印错的路径。可以做,不值得为它加复杂度。

### 4.4 ELF 阶段的三个硬约束

1. **绝不 patch glibc 载荷里的 `ld.so` / `libc.so.6`**([[never patchelf the glibc payload]])。
   重写 ld.so 会得到 **exit 139、零输出**。阶段 3 必须有一条显式排除:
   属于 runtime 载荷(`xim-x-glibc/*`、`xim-x-musl/*`)的 loader 与 libc 本体跳过,
   **只改指向它们的 PT_INTERP**,不改它们自己。
2. **写 `DT_RPATH`,不是 `DT_RUNPATH`**([[DT_RPATH vs DT_RUNPATH transitivity]])。
   patchelf 默认写 RUNPATH,而 RUNPATH 不传递给依赖的依赖 —— 可执行文件上的标签决定了
   它深层 dlopen 的东西能不能被找到。这个仓库已经因此付过代价(#525)。
3. **`PT_INTERP` 必须是绝对路径**。内核按字面量加载,不展开 `$ORIGIN`。
   所以阶段 3 是「重写成新的绝对路径」,不是「改成相对」。
   这也是 §5 拒绝 T1 的物理原因。

扫描用 `xlings.core.elfread`(进程内读 interp + rpath,`elfread.cppm`:
一次 13,729 个 ELF 的量级它已经承担过),**只对命中旧根前缀的文件**调 patchelf。
patchelf 定位复用 `elfcheck::locate_patchelf`(`closure_check.cppm:245`);
找不到 patchelf → 阶段 3 报「无法执行,原因:缺 patchelf」并给安装命令,**不静默跳过**。

### 4.5 幂等

再跑一次必须是 `没有需要搬迁的记录` + exit 0。
阶段 2/3 天然幂等(前缀不再命中);阶段 1 靠 4.2 的「零候选」短路。

---

## 5. C 组 —— 明确不做,以及理由

| 不做 | 理由 |
|---|---|
| 把 `${XLINGS_HOME}` 写进 versions DB | `VData::path` 有约 10 处**不过 `expand_path` 的裸读者**(`bindings.cpp:501/536`、`installer.cpp:108`、`owner.cpp:169`、`db.cpp:154/163/209`)。只改写者会让 sysroot 链接指向字面量 `${XLINGS_HOME}/…`。收益(新 home 天然可搬)已被 `relocate` 覆盖,YAGNI。 |
| 承诺 home 可自由搬迁 | `PT_INTERP` 不展开 `$ORIGIN` —— 做不到纯粹。半可搬比不可搬更危险:用户会以为能搬。 |
| doctor 自动 relocate | doctor 是**每次运行**都走的诊断路径,加「可能搬家了」的启发式会污染所有诊断,而且猜错的代价是自动重写别人的 home。 |

**如果将来要做 T1**,前提是先把上面那 10 处裸读者收敛成一个入口
(建议方向:在 `db.cpp:816` 反序列化时就展开成绝对路径,磁盘上存占位符、内存里永远绝对
—— 这样 10 个读者一个都不用动)。注意方向与 [[read/write invariant asymmetry]] 相反:
那个坑是「写归一、读不归一」,这里是「写归一、读也归一」,不会伤到老 home。

---

## 6. 接口清单(落到文件)

| 文件 | 改动 |
|---|---|
| `src/core/xvm/owner.cppm/.cpp` | 新增 `payload_reachable_under_current_root()`(或放 doctor detail_,见开放问题 Q3) |
| `src/core/xself/doctor.cppm` | 新增 `FindingKind::StalePayloadRoot` |
| `src/core/xself/repair.cppm` | `RepairKind`(声明在此,`:19`)增 `LinkRepointed` |
| `src/core/xself/doctor.cpp` | `1333` 判定分流;`2069` 扫描集合;`2413` 重指规则;`2965` prune 跳过;`3987/4051` 结论与退出码;`4048` 打戳容错 |
| `src/core/config.cpp` | `1303` `record_client_version` 包 try/catch |
| `src/cli.cpp` | 顶层 `try` 提到 `run()` 开头(覆盖 1525 的提前分派) |
| `src/core/xself/relocate.cppm/.cpp` | **新文件**,四阶段 |
| `src/cli/spec.cpp` | `self relocate` 条目 + flags |
| `src/core/xself.cpp` | action 分派 |
| `tests/e2e/*.sh` + `run_all.sh` | 新 e2e,**必须注册**([[e2e set -e silent death]]) |
| `.agents/tools/relocate-acceptance.sh` | **新** harness,打印数字不打印结论 |

---

## 7. 输出契约:三态必须可区分

任何一次 `doctor` / `relocate`,读者必须能分辨:

| 状态 | doctor 说什么 | 退出码 |
|---|---|---|
| 没搬过家 | 现有输出不变 | 现有 |
| 搬了,载荷可达 | `StalePayloadRoot` + `xlings self relocate` + **不 prune** | **1** |
| 搬了,载荷也真没了 | 现有 `BrokenPayload` → ladder → prune(合法) | 现有 |

`relocate` 同理:`没有需要搬迁的记录` / `搬迁了 N 项` / `阶段 3 无法执行(缺 patchelf)`
三者不得共用同一句结论。这是 [[silent-success pattern]] 的直接防御。

---

## 8. 测试

### 8.1 单测(`mcpp test`)
- `payload_reachable_under_current_root`:命中 / 不命中 / 路径含符号链接(两边 canonicalize)/ Windows 反斜杠记录。
- 旧根推断:唯一 / 多个 / 零个。

### 8.2 e2e(注册进 `tests/e2e/run_all.sh`)

| ID | 场景 | 断言(只断不变量) |
|---|---|---|
| R1 | 构造 home → `mv` → `doctor` | 出现 `StalePayloadRoot`;**不**出现 `OK` |
| R2 | 同上 → `doctor --fix` | `versions` 非空;`workspace` 非空;退出码非 0 |
| R3 | 同上 → `self relocate -y` → `doctor` | 退出码 0;shim 可执行 |
| R4 | R3 之后再 `relocate` | 输出「没有需要搬迁的记录」;退出码 0 |
| R5 | 只读 home → `doctor --fix` | 退出码 **不是 134**;stderr 无 `terminate called` |
| R6 | 库农场断链(不搬家)→ `doctor --fix` | 断链被**重指**;链接可解析 |

R5 是本次唯一一条「断言不崩溃」的用例 —— 写法上要断言退出码 ∈ {0,1,2} 且无 `terminate`,
不要断言某个具体值([[assert only invariants]])。

### 8.3 验收 harness

`.agents/tools/relocate-acceptance.sh`,照 `.agents/tools/doctor-acceptance.sh` 的约定:
**打印可判别的数字,不打印结论**(「a pass cannot be asserted without the numbers that
justify it」)。至少输出:搬家前/后/relocate 后的
`versions 条数 / workspace 条数 / 各 subos 断链数 / PT_INTERP 命中旧根的 ELF 数`。

### 8.4 真机验收

`.agents/tools/slice-real-home.sh --dst <scratch>` 切一份真实 home(硬链接农场,几秒),
`mv` 之,跑 A 组 + relocate,最后 **`verify-untouched` 必须跑**(不要用推理代替它,
[[repro from a real-home slice]])。⚠️ ELF 阶段会**原地改文件**,而切片是硬链接农场 ——
**阶段 3 的真机验收必须对 `--real` 复制出来的 store 做**,否则会改到真实 home 的 inode。

---

## 9. 风险与已知陷阱

| 风险 | 来源 | 设计如何规避 |
|---|---|---|
| A4 单独合入 → 删掉 431 条链接 | 现修复动作是 remove | §2 顺序约束,A4 与重指同 PR |
| ELF 阶段改坏 ld.so → exit 139 无输出 | [[never patchelf the glibc payload]] | §4.4-1 显式排除 |
| patchelf 写成 RUNPATH → 深层 dlopen 失败 | [[DT_RPATH vs DT_RUNPATH transitivity]] | §4.4-2 强制 RPATH |
| 单边 canonicalize → 大量假阳性 | [[canonicalize both sides]] | A1 陷阱条 |
| 切片上跑 ELF 阶段污染真实 home | 硬链接农场不防原地重写 | §8.4 `--real` + `verify-untouched` |
| 退出码 0→1 影响既有调用方 | A2 | 合入前全量 e2e |
| 新 e2e 静默不跑 | [[e2e set -e silent death]] | 必须注册 `run_all.sh` |
| 「重指了但仍然断」被计成成功 | [[silent-success pattern]] | 重指后二次校验 |

---

## 10. PR 拆分

| PR | 内容 | 依赖 |
|---|---|---|
| **P1** | A3(只读不崩 + 顶层 catch 覆盖 self/subos/profile) | 无。风险最低,建议先合。 |
| **P2** | A1 + A2(判定 + 结论/退出码) | 无 |
| **P3** | A4(lib 扫描 + 重指) | P2 |
| **P4** | B(`self relocate`) | P2 |
| **P5** | 文档:README / docs 里写明「home 不可自由搬迁,搬了跑 relocate」 | P4 |

---

## 11. 开放问题(需要拍板)

**Q1 · ELF 阶段默认开还是默认关?**
我倾向**默认开 + 先扫描报数 + 确认**(`-y` 跳过确认,`--no-elf` 关闭)。
理由:不改 ELF 的 relocate 修不好「跑起来」这件事,用户会以为 relocate 失败了。
反方:它是唯一不可逆的阶段。

**Q2 · doctor 检测到搬家时退出码给 1,是否可接受?**
按 §7 我给的是 1。如果有把 `doctor --fix` 的 0 当成「没问题」的外部脚本,这会是行为变更。

**Q3 · `payload_reachable_under_current_root` 放哪?**
放 `xvm::owner`(和 `coordinate_from_payload_path` 同模块,relocate 也要用)
还是 doctor 的 `detail_`(用完即弃)?我倾向前者,因为 B 组要复用。

**Q4 · A 组四条是否按 §10 拆四个 PR?**
还是 A1+A2+A4 合成一个「doctor 判定与修复语义」的 PR?
我倾向拆,理由是 A4 风险最高、最需要单独的真机验收。
