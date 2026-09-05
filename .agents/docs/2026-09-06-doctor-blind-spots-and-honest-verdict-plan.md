# doctor 的两个盲区与一句不诚实的结论

> 来源:#583 的排查(`2026-09-06-issue-583-home-relocation-survey.md`)。
> 那个 issue 的**主诉求已 won't-do**(`mv` 一个 home 不是受支持的操作,见
> `2026-09-06-issue-583-relocation-design.md` 的作废说明与 issue 上的回复)。
> 本文档只收**触发路径完全合规**的缺陷 —— 也就是不搬家、不手工删文件、
> 只用正常命令就能撞上的那些。
> 基线 `a5cf36e` / 2026.9.3.2。**已实现,发布版 2026.9.4.1。**
> 实现中被测量推翻的判断见 §7。

## 0. 门槛:什么才算这次要修的 bug

排查里复现出来的东西不全是缺陷。判据只有一条:

> **触发它的操作是不是受支持的。**
> 复现一个「不被允许的操作」造成的破坏,证明不了缺陷,只证明了那个操作不被允许。

按这条重新分级(推翻了排查文档 §9 的拆分):

| 排查里的发现 | 触发路径 | 判定 |
|---|---|---|
| `<subos>/lib` 断链不在扫描内 | ✅ 纯 `xlings install` | **B2,修** |
| 只读 home `--fix` SIGABRT | ✅ 正常命令 + 正常环境 | **B1,修** |
| prune 之后报 `OK` + exit 0 | ✅ 载荷真的没了,正是 prune 的设计场景 | **B3,修** |
| 「不该注销新根下可达的载荷」 | ❌ 只在 `mv` 之后出现 | **不做** |
| 四种绝对路径 | ❌ 同上 | 设计事实,不是缺陷 |

三条彼此独立,顺序只有一处约束(§2 的陷阱一:B2 的两半必须同批)。
按目标要求合并为**单个 PR**,见 §6。

---

## 1. B1 · 只读 home 上 `self doctor --fix` 直接 SIGABRT

### 为什么这是 bug(四条,任一条都够)

1. `doctor --fix` 是正常命令,只读挂载 / 沙盒 / 权限不足是正常环境。
2. `cli.cpp:1804` 的注释明写这个 try 要接住「any uncaught std::exception … Convert to a
   logged error + non-zero return」。**契约没兑现** —— try 从 `1812` 才开始,而
   `self` / `subos` / `profile` 在 **`cli.cpp:1525`** 就分派完了。
3. 同一次 `--fix` 里,隔壁 `doctor.cpp:2343` 的同类写入**本来就是**包了 try/catch、
   优雅打印 `✗ subos manifest could not write …` 的。**代码自己表明了意图**;
   两个写者对同一种失败给出两种命运。
4. SIGABRT + core dump 在任何情况下都不是可接受的失败形态。

### 实测

```
$ chmod -R a-w <home>; XLINGS_HOME=<home> xlings self doctor --fix
… 完整报告正常打印 …
terminate called after throwing an instance of 'std::runtime_error'
  what():  Failed to write file: <home>/.xlings.json
EXIT=134
```

gdb 栈:
`platform.cpp:403 throw` → `config.cpp:1303 record_client_version`(**无 try**)
→ `doctor.cpp:4048` → `xself.cpp:182` → `cli.cpp:1525` → `main.cpp:92`。

触发面实测:只有真正的 `--fix` 会走到 `doctor.cpp:4048` 的打戳;
`self doctor`、`--deep`、`--fix --dry-run` 都在 `doctor.cpp:3877` / `3888` 提前 return,
所以它们在只读 home 上是正常的 exit 1。

### 怎么修(两处,正交)

1. **`config.cpp:1303` 包 try/catch**,照抄 `doctor.cpp:2343` 的形状:
   失败 → 一条 note(`✗ home stamp  could not write …`),**不改退出码** ——
   打戳失败是环境问题,不是 home 的健康问题。
2. **顶层 try 提到 `cli::run()` 开头**,覆盖 `cli.cpp:1497–1527` 的提前分派。
   只做 1 是修一个症状(下一个无保护写者照样 abort);只做 2 是把 abort 变成
   `internal error: Failed to write file: …`,信息量不如「could not write X」+ 跑完。

### 陷阱

- 提前 try 时,`1497–1527` 之间的早退分支(参数校验 `return 2`、unknown command
  `return 1`)必须**原样返回**,不能被 catch 改写成别的码。
- `record_client_version` 吞掉异常之后,「打戳成功」和「打戳失败」不能再无法区分
  —— 必须有那条 note,否则修完就变成一个新的 [[silent-success pattern]]。

### 验收

只读 home 上 `--fix`:退出码 ∈ {0,1,2},stderr **无** `terminate called`。
断言不变量,不断言具体码([[assert only invariants]])。

---

## 2. B2 · 库农场是 doctor 的盲区,而且断链修复只会删

### 为什么这是 bug

**纯 `xlings install` 就能产生,而 doctor 报 0。** 实测在一台**从未搬过家**的真实 home:

```
subos/*/lib* 下链接总数: 3042    当前断链: 6
xlings self doctor → dangling 相关 finding: 0 条
```

### 证据:freetype 这一例把两层原因都摊开了

```
载荷实际内容   ~/.xlings/data/xpkgs/xim-x-freetype/2.13.2/lib/libfreetype.so.6   ← 在
断链指向       ~/.xlings/data/xpkgs/xim-x-freetype/2.13.2/lib/x86_64-linux-musl/…  ← 载荷里没有这个目录
当前 DB 记录   path = …/2.13.2/lib   sourceName = libfreetype.so.6                 ← 是对的
7 个 subos     ftclean / ftverify / hbverify / postmerge = OK
               current / default / gfxbuild            = DANGLING
```

整个 store 里只有一个 freetype 载荷,没有 musl 变体。所以这不是「残留的旧包」,是:

**第一层(本次要修)· 可见性**
`doctor.cpp:2069` 只走 `{"usr", "etc", "share"}`;库农场在 `<subos>/lib`
(`installer.cpp:1764`;`Config::libDir` = `subosDir/"lib"`,`config.cpp:484`)。
`doctor.cpp` 全文不出现 `libDir`。**这些链接从来没有进入过 detect。**

**第二层(本次不修,单开 issue)· 陈旧性**
sysroot 链接是**每个 subos 一份**,而安装只重写**当前活跃 subos**
(`installer.cpp:2058` 的 `!resolved->active` 门,写入目标是 `artifactSubosDir`)。
freetype 的注册在某次更新里把 `path` 从 `lib/x86_64-linux-musl` 改成了 `lib`,
当时活跃的那几个 subos 被刷新了,其余三个留在旧值上,永远没人回来看。
这是 [[multi-subos state split]] 的标准形状,**要单独一个 issue**,
而且它只有在 B2 修完之后才看得见。

### 怎么修

1. 扫描集合加 `lib`、`lib64`。
2. **修复动作由「删」改为「先问 DB,能重指就重指,不能才删」**:

```
对每条断链 L:
  向版本库要权威来源:xvm::library_placement(db, target, version)   (bindings.cpp:482)
      → 返回 {source = path/sourceName, name = destinationName}
  source 存在      → 重指(xvm::place_asset,commands.cpp:269:staging+rename,
                          对「已经指向同一文件」只花一次 stat)
  source 不存在 / 问不出 → 删(现行为),保留 prune_empty_asset_dirs
```

**不要从链接目标反推来源**。DB 才是权威 —— freetype 这一例里,链接目标是错的、
DB 是对的,反推会把错误固化下来。这条规则**自动治好那 3 条 freetype 链接**,
因为 DB 里的 source 早就正确了。

3. 报告与计数把两种动作分开:`link repointed` / `dangling link removed`。

### 陷阱

1. ⚠️ **顺序:加广度必须与重指同一个 PR。** 现行为是无条件 `fs::remove`
   (`doctor.cpp:2413`)。先加广度、后加重指,等于在任何有陈旧链接的 home 上批量删除
   —— 我这台会删 6 条,#583 报告人那台是 51 条。这是本文档唯一的硬顺序约束。
2. 重指后**二次校验链接可解析**。否则「重指了但仍然断」会被计成成功。
3. **Windows 上库农场是硬链接/复制,不是符号链接**(`xvm/commands.cpp:create_link_`)。
   该平台上这条扫描只在符号链接分支生效,e2e 要**显式跳过**而不是假装通过
   ([[cross-platform test traps]])。
4. 不要 follow 目录符号链接(现有扫描已关闭 `follow_directory_symlink`),重指同理:
   只处理文件链接,目录链接原样报告。
5. 「能重指」的判据要**两边 canonicalize**([[canonicalize both sides]])。

### 验收(真机可判别)

在这台真实 home 上:

| | 修复前 | 实测(切片,2026.9.4.1) |
|---|---|---|
| `doctor` 报的断链数 | **0** | **2** |
| `--fix` 之后实际断链数 | 4 | **0** |
| `xim-x-freetype` 载荷是否被改动 | — | **否**(只动链接) |
| 真实 home 是否被触碰 | — | **否**(store 0 个文件变动,6 条断链仍在) |

> 切片里是 4 条(`current/` 是指向 `default/` 的目录链接,不被下降进入),
> doctor 报 2 条(两个真实文件)。**这几条最后是被删除而不是重指的 ——
> 见 §7 第 2 条,那是正确行为,是我的验收标准写错了。**

---

## 3. B3 · prune 之后打印 `status OK` 并 exit 0

### 为什么这是 bug —— 触发路径完全合规

prune 的设计场景就是「载荷真的没了」,`doctor.cpp:2953` 的注释自陈:
*the payload is gone … There is nothing to lose that has not already been lost*。
这是**正常会发生**的状态(索引仓库没了、外平台注册的 alias、一次失败的安装)。

**实测(纯合规 fixture,没有任何搬家动作)** —— 一条载荷确实不存在的注册:

```
· dropped              ghost@1.0.0 — its payload is gone and nothing can restore it
· stale shims removed  ghost
▸ status               OK — workspace, shims, and payloads are all consistent
▸ healed               2
▸ pruned               1
EXIT=0
```

注销注册是**信息丢失**(用户失去了「这个包曾经装过」这条事实),
而它和「什么都没坏」打印同一句结论、同一个退出码。这就是 [[silent-success pattern]]。

**附带一个更小的**:同一次运行里先 `shims created ghost`、再
`stale shims removed ghost`,然后 `healed 2` —— healed 把一来一回的空转算成了治愈。

### 怎么修

1. `pruned > 0` 时**结论行不得为 `OK`**。换成一句指名损失的结论:
   `注销了 N 个无法恢复的注册`,并保留每条 `dropped` 明细。
2. **退出码保持不变(仍然是 0)。**
3. `healed` 不计入本次运行自己创建又自己删除的东西。

#### 为什么退出码不动(这条在 review 中被我自己推翻了一次)

初稿写的是「`pruned > 0` → 非 0」。推翻它的理由:

**退出码回答的问题是「这个 home 还需不需要人来处理」**,而不是「这次运行发生了什么」。
一次合法的 prune 之后,home 是健康的 —— 给非 0 等于把「丢了东西」和「坏了」
混成一个信号,这**正是本条 bug 的镜像**:本条的病因就是两件不同的事共用一个输出,
用退出码去修它会在另一个方向上重犯。

`--allow-prune` 那个折中方案一并否掉:退出码不动就不需要它(YAGNI),
而且它会把「用户是否接受清理」这个语义塞进一个本来只表达健康度的信道。

**代价要说清楚**:只看退出码的脚本仍然看不见 prune。这是可接受的 ——
prune 的可见性由结论行负责,而结论行是人看的。要机器看的话,
正确的做法是将来给 `--json`/interface 输出一个 `pruned` 字段,不是重载退出码。

### 陷阱

- 退出码不动,所以**没有**调用方兼容性问题 —— 这是选它的附带好处之一。
- 不要把 `pruned` 混进 `healed` 的反向修法(`doctor.cpp:3987` 已经把 pruned 扣掉了,
  那部分是对的,别动)。
- 结论行只有两种形态是不够的:`OK` / `注销了 N 个` / `还有问题` 三态必须互不重叠,
  否则修完只是把谎言换了个地方讲。

---

## 4. 不在本次范围

| | 为什么 |
|---|---|
| 任何搬迁支持(`self relocate`、`${XLINGS_HOME}` 占位符) | 操作不受支持,issue 上已 won't-do |
| 「拒绝注销新根下可达的载荷」 | 只在不合规操作后出现,不是缺陷 |
| B2 第二层:多 subos 的陈旧 sysroot 链接 | **单开 issue**;B2 修完才看得见,修法涉及安装期跨 subos 刷新策略,不是一个扫描能解决的 |

---

## 5. 多角度审视

| 角度 | 这次的取舍 |
|---|---|
| **架构** | 三条都**不引入新概念**:没有新命令、没有新存储字段、没有新 finding 之外的抽象。B2 唯一的新依赖是 doctor → `xvm::library_placement`,方向与既有依赖一致(doctor 已经 import xvm)。B1 的 try 提升不是新机制,是把 `cli.cpp:1804` 已经写在注释里的契约落到实处。 |
| **稳定性** | B1 直接消灭一整类 abort(不止这一个写者)。B2 是本次唯一有破坏风险的改动 —— 风险全部集中在「加广度但没有重指」这一种切法上,靠同批约束消掉,再靠重指后的二次校验兜底。B3 不改任何行为,只改措辞与计数,风险为零。 |
| **优雅 / 简洁** | B2 的关键选择是**问 DB,不从链接目标反推** —— 少一套推断逻辑,而且在 freetype 这类「链接错、DB 对」的情形下是唯一正确的方向。B1 选择提升 try 而不是给每个写者加 try,是同一种取舍:让未来的写者不必记得这件事。 |
| **用户体验** | 结论行三态互不重叠(`OK` / `注销了 N 个` / `还有问题`);`link repointed` 与 `dangling link removed` 分开计数,让「修好了」和「删掉了」不再是同一行;B1 把 core dump 换成一行能读懂的 `could not write …`。 |
| **兼容性** | **退出码不变、状态格式不变、索引与配方不变。** 没有迁移步骤,没有 `--fix` 之外的新命令。老 home 下一次跑 doctor 就直接受益。 |
| **跨平台** | B2 的库农场在 Windows 是硬链接/复制而非符号链接(`xvm/commands.cpp:create_link_`),扫描只在符号链接分支生效,e2e **显式跳过并说明**,不静默通过([[cross-platform test traps]])。B1 / B3 三平台行为一致。 |
| **一致性** | B1 让两个写同一份状态的写者对同一种失败给出同一种命运。B2 让 `lib` 与 `usr`/`etc`/`share` 用同一套断链规则 —— 并顺带把 `usr/include` 那一侧的**无条件删**也改成了「能重指就重指」。 |
| **无感升级** | 用户不需要做任何事。三条都在既有命令的既有路径上生效,没有新开关。 |

### 跨仓库

代码改动**只在 `openxlings/xlings`**;不触及 `xim-pkgindex`、配方或索引格式。
跨仓库协作只发生在发布链:版本发布后 `xim-pkgindex` 的版本条目 bump,
以及 GitCode 侧资源(release CI 之外由本地 `gtc` 补齐)。
详见 [[ecosystem release chain]] 与 [[release verification traps]]。

---

## 6. 交付:单个 PR

按目标要求合并为**一个 PR**,内部按三条独立提交,便于 review 与回滚:

| 提交 | 内容 | 备注 |
|---|---|---|
| 1 | **B1** 只读不崩:`config.cpp` 打戳容错 + `cli.cpp` 顶层 try 覆盖 self/subos/profile | 最独立 |
| 2 | **B3** 结论行三态 + healed 口径 | 不改行为 |
| 3 | **B2** 扫描加 `lib`/`lib64` **+** 重指(两半必须在同一个提交里) | 唯一有破坏风险的一条 |
| 4 | e2e(D1–D5)+ 验收 harness 扩展 + `run_all.sh` 注册 | |
| 5 | 版本号 + 文档 | 见下 |

**版本号**:`2026.9.3.2` → **`2026.9.4.1`**。
两处都要改([[version bump: two places]]):`mcpp.toml` 与 `src/core/config.cppm`,
且 `mcpp test` 不会重链 `bin/xlings` —— `mcpp build` 之后必须验 `--version`。

### 测试

**e2e**(注册进 `tests/e2e/run_all.sh`,[[e2e set -e silent death]]):

| ID | 场景 | 断言 |
|---|---|---|
| D1 | 只读 home → `doctor --fix` | 无 `terminate called`;退出码 ∈ {0,1,2} |
| D2 | 载荷缺失的注册 → `--fix` | 输出**不含** `status OK`;含指名损失的结论行;`dropped` 明细在 |
| D3 | `<subos>/lib` 造一条断链,DB 有正确 source → `--fix` | 链接被**重指**且可解析;载荷未被改动 |
| D4 | `<subos>/lib` 造一条断链,DB 问不出 source → `--fix` | 链接被删除;空目录被清理 |
| D5 | Windows | D3/D4 **显式跳过**并说明原因,不静默通过 |

**真机验收**:`.agents/tools/slice-real-home.sh --dst <scratch>` 切片后跑全部三条,
最后 `verify-untouched` 必须跑([[repro from a real-home slice]])。
本次三条**都不改载荷内容**,所以切片的硬链接农场是安全的 —— 但仍然跑,不用推理代替它。

**生态验证**:`xlings config --mirror CN` 之后,
用 `xlings subos <name> --sandbox --cmd "…"` 在真实 subos 里跑一遍,
确认三条改动没有影响正常的安装 / 构建路径。

**验收 harness**:扩 `.agents/tools/doctor-acceptance.sh`,加三个可判别的数字:
`断链数(报告的) / 断链数(实际的) / pruned 数`。照它现有的约定 ——
打印数字,不打印结论。

---

## 7. 实现推翻了什么(写在最后,因为这几条只有做完才知道)

### 1. 多出来一条:计数的标签在说谎(B4,已一并修)

追 `healed` 的时候量出来的:一个只有 subos manifest 问题的 home 会打印
**`broken payloads 1`,而列表里一条 payload 都没有**。
原因是 `count_` 把 `SubosManifest` / `SubosEnvOrphan` / `SubosEnvUnresolved` /
`SubosRuntimeMissing(Error)` 全部加进 `c.broken`,而那个计数器被渲染成
「broken payloads」。

这正是同一个文件里 `AliasUnresolved` 的注释早就描述过的形状 ——
*a count that does not match the list … sends people looking for a line that is
not there* —— 只是发生在隔壁一个字段上。

修法:`Counts` 增加 `subos` 字段,`issues()` 照旧把它算进总数(**所以退出码不动**),
渲染成 `subos issues`。实测:修前 `broken payloads 2`,修后
`broken payloads 1` + `subos issues 1`,与列表一致。

### 2. B2 的真机验收标准是我写错的,不是代码错

计划里写「6 条链接应可解析(重指)」。实测:切片上这几条 freetype 链接
**被删除**了,不是重指。查下来是**正确行为** ——
`libfreetype.so.6` 根本不在 `default` 的 workspace 里(非激活版本),
而非激活版本的库本来就不该出现在 sysroot,重指过去等于装了一个 `use` 从没要求过的库。

所以 `sysroot_link_source_` 只在「该 target 在本 subos 有激活版本」时才给来源,
是对的;验收标准应该是**「doctor 看得见(0→2)且 `--fix` 之后断链归零」**,
而不是「全部重指」。

### 3. 「`healed 2` 是把自己撤销的工作算成了治愈」—— 这个说法过强

初稿拿 `healed 2` 当证据。实测拆开是:`before=3`(shim 表 1 + broken payload 1 +
subos manifest 1)、`after=0`、prune 归因 1 → `healed 2`,而那 2 确实是
**真的修好的**(shim 表和 subos manifest)。所以那个具体数字并不是谎报。

`healed` 的归因修正(按**findings** 而不是按 **registrations** 扣减)仍然保留 ——
它在「一条被 prune 的注册贡献了两条 findings」时才会体现差别,是对的,
只是不该拿 `healed 2` 当它的证据。**这一条是我自己的证据链错了,不是代码错了。**

### 4. e2e 的第一版 S1 在旧二进制上也是绿的

`make_home "$S1" missing` 时,载荷缺失 → prune → 只读 home 上 prune 失败 →
`outstanding != 0` → 根本走不到那个打戳,于是**新旧二进制都不崩**。
改成 `present` 之后,对 2026.9.3.2 稳定复现 exit 134。

差分验证已经写进测试文件的注释里:这条测试对旧二进制必须 FAIL,
否则它守住的是零。

---

## 8. 第二轮自查又抓出四条(都在这次自己新写的代码里)

### 1. S1 在拿到自己的 catch 之后,就不再证明任何事了

`record_client_version` 有了局部 try/catch,于是**即使顶层 try 仍然开在分派下面**,
S1 也会通过。也就是说 B1 的两半里,只有第一半有门,第二半(try 提升)没有。

补 S7:用 `subos new` 在只读 home 上 —— 一个和打戳毫无关系、且原本就在 handler
之外的子树。实测差分:

| | `xlings subos new probe`(只读 home) |
|---|---|
| 2026.9.3.2 | **exit 134**,`terminate called` |
| 2026.9.4.1 | **exit 1**,打印 `filesystem error: … Permission denied` 并指出路径 |

### 2. 打戳失败的提示落在报告面板**外面**

我的注释说它和 subos manifest 那个写者「共享同一命运」,而那个是打在面板里的。
说法和行为不一致就是下一个读者的坑。打戳移到 `render_` 之前,失败作为
`✗ home stamp` 进入面板。

顺带补 S6:**不崩溃是必要不充分的** —— 一个空 catch 也能不崩,而且会让
「打戳成功」和「打戳失败」再次无法区分,正好是这次要消灭的形状。所以那条消息
自己有一道门,并且断言这种失败**不改退出码**(记账失败不是 home 不健康)。

### 3. `sysroot_link_source_` 取了第一个匹配

一个目标名同时被两个 active 条目声明,是一个有两个答案的问题;挑一个 = 一次
自信的错误修复,正是这次改动要消灭的形状。改成收集全部匹配,**恰好一个才用**。
先量后改:真实 home 95 个 active lib 条目、**0 个重名**,所以这条拒绝不会拒绝掉
任何真实发生的事。

### 4. 我把 catch 的覆盖面扩大之后,它的建议就错了

`filesystem_error` 一律附「this is likely a bug; please report at …」。
扩大覆盖面之后它开始接住只读 home 的权限错误 —— 对这种情况,正确的下一步是换一个
可写位置,不是提 issue。`permission_denied` / `read_only_file_system` 单独给话术。

**这四条都不是「计划没写到」,而是「写完之后才看得见」。** 记在这里是因为
前三条的共同点是:**新加的保护自己制造了一个新的不可分辨状态**,而这正是
本次要修的那类 bug。
