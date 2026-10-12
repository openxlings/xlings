# xlings 稳健性与可用性综合优化方案

> 起点:四条痛点(提示太吵、`local:` 总和发布包冲突、删不掉/doctor 要先进 subos、
> 一个坏包拖垮别的)+ 一句「综合分析架构 / 跨平台 / doctor 可用性」。
> 基线 `f6ad75f` / 2026.9.5.1。状态:**草案 v2,§7 六个待定点已按真机度量定案,待 review,未动代码。**
> 所有事实都带 file:line,真机数字来自 `~/.xlings`(111 个 subos,版本戳 `v0.4.40`)。

## 0. 一句话

四条痛点不是四个 bug,是**三个结构性原因的四张脸**:

| 结构性原因 | 痛点里的表现 |
|---|---|
| **A. 提示按「写者的状态」触发,不按「读者会遇到的问题」触发** | 版本戳 `v0.4.40` 永远不更新 → 四处提示永远在;`local` 降级警告每条命令都重打 |
| **B. `local` 是一份平行索引,不是「开发覆盖层」** | 真机上 `xim-pkgindex-local` 有 **~200 个 recipe**,是 9 月 11 日一次性整批拷进来的发布索引快照;它永远比发布索引旧一个版本,所以每个裸名都撞警告 |
| **C. `remove` / `doctor` 的世界是「当前 subos」,用户的世界是整个 home** | 删不掉、要先 `subos use`(真机 38 条 remedy 是这句)、doctor 永远清不干净(§1.3 的因果链)、`--fix` 永远跑不完(§1.6 的 D2:65 个没人用的旧 mcpp 要重新下载) |

鲁棒性(痛点 4)和跨平台不是第四个原因,而是这三个原因在「失败」和「另一个 OS」上的放大:
状态有多个真值源时,一个坏条目就能让读者停在半路;e2e 135 : 10 : 7(Linux : Windows : macOS)
意味着 C 这类「谁在什么范围内写状态」的问题在另外两个平台基本没有测过。

memory 里已有的形状:[[one question, many answerers]]、[[multi-subos state split]]、
[[silent-success pattern]]、[[gate the message on behaviour]]。这次的新形状只有一个:
**「清不干净的诊断」和「关不掉的提示」是同一条链**(§1.3)。

---

## 1. 痛点逐条:事实

### 1.1 `run xlings self doctor --fix` 提示太多

**四个发射点,三种机制,零处跨进程记忆:**

| 位置 | 触发命令 | 门控 | 抑制 |
|---|---|---|---|
| `xself/repair.cpp:139-147` `print_migration_hint_once` | `use`、`install`、`list`(两处)结束时 | `.xlings.json["version"]` ≠ 运行版本 且 是 TTY | `static bool` — **只在进程内一次**,每条命令都是新进程 |
| `xself/doctor.cpp:4441` | `self doctor`(含 `--fix`)的 info panel | 同上 | 无 |
| `xself/update.cpp:137-140` | `self update` 成功末尾 | **无条件** | 无 |
| (字符串构造) `repair.cpp:34-49` `migration_hint` | — | `strip_v(recorded) != strip_v(running)` | — |

**戳是谁写的:** 只有两处 —— `self install`(`xself/install.cpp:807-820`,写的是**被安装包的版本**)和
`doctor --fix`(`doctor.cpp:973-985`),后者的条件是:

```cpp
if (outstanding == 0 && after.foreignPayloads == 0) record_client_version(Info::VERSION)
```

`self update` **故意不写**(`update.cpp:82-136` 的注释:它是旧二进制,即将退出),`self init` 也不写。
所以一台只靠 `self update` 升级的机器,戳永远停在第一次 `self install` 的版本 —— 真机就是 `v0.4.40`。

**为什么 `--fix` 也关不掉它:** 见 §1.3 的因果链。

**可用的静音手段:** `-q/--quiet` 把 `log::info`/`warn` 全关(`cli.cpp:576-577`),没有「只关提示」的档位;
没有任何 `XLINGS_*` 环境变量管日志等级。`diag.cppm` 的 `Note/Warn/Error` 三级只是渲染形状,
最终还是走 `log::` 的 Debug/Info/Warn/Error 单一闸门;这四个发射点**一个都没走 `diag::`**。

### 1.2 `local:` 总和发布包冲突

**警告从哪来:** `xim/catalog.cpp:564-569` `announce_demotion_`,只在**裸名**(无命名空间)在多个索引仓
都能解析时触发;去重 memo `demotionsAnnounced_` 挂在进程内的 `static PackageCatalog`(`xim/commands.cpp:149`),
同样不跨进程。**不**在 shim 执行时触发(`src/core/xvm/` 不碰 catalog),但 `install`/`remove`/`update <t>`/
`info`/`list`/`why`/`self doctor` 都会。

**优先级规则:** `catalog.cpp:166-168` —— `local` = 1,其它全部 = 0,只有两档;规则只看命名空间是否显式,
不看有没有写版本。

**真机上 `local` 是什么:**

```
data/xim-pkgindex-local/pkgs/   ~200 个 recipe,全部 mtime 2026-09-11 20:36–20:38(一次整批 --add-xpkg)
data/xpkgs/local-x-*             50 个载荷 store(xim-x-* 201 个)
警告里的 local:mcpp@2026.9.11.3  = 本地 recipe 副本声明的版本;xim 索引今天已是 2026.9.12.2
subos/default workspace           ~10 个包的 active 指向 local:*(cuda 12.9.86、node、…),mcpp 不在其中
```

即:这不是「开发时装了个 `local:mcpp`」,是**整份发布索引的一份冻结快照**在和活的索引赛跑,
而且没有任何命令能列出它、看到它的来源、或整份清掉:

- `config --add-xpkg` 有(`cli.cpp:881-885`,`xim/commands.cpp:1886-1987`),**没有 `--list-xpkg` / `--remove-xpkg`**。
- `remove local:x` 一次删一个已安装版本,删不掉索引里的 recipe。
- doctor 的 R4 prune 只清「载荷没了且索引不提供」的死注册,不碰活的 `local` recipe。
- 没有隐藏/禁用/固定某个命名空间的开关。

**开发隔离为什么难:** `main.cpp:54-65` —— 直接跑 `xlings` 二进制尊重 `XLINGS_HOME`,但**经 shim** 进来的
(包括 `mcpp`)会被改写回 owner home。`XLINGS_SHIM_ANCHOR=0` 能关(`xvm/shim.cpp:142-146`),
未在任何文档出现。

### 1.3 删不掉、doctor 要先进 subos

**`remove` 今天的 `--force` 只跳过两件事**(`xim/commands.cpp:1182`、`:1318`):多版本警告、反向依赖守卫。
它**不**跨越:

| 场景 | 行为 | 位置 |
|---|---|---|
| recipe 不在索引(最常见于 `local:`) | `resolve_target` 失败 → `uninstall failed`,**`--force` 无效** | `commands.cpp:1236-1291`,`installer.cpp:3256-3258` |
| uninstall hook 抛错 | 整个 remove 失败,**状态一字未动** | `installer.cpp:3404-3411` |
| 无版本、只有一条记录 | 解析到索引的 latest 而不是已装版本 → 永远不解析(#578) | `commands.cpp:1156-1233` |
| 注册在别的 subos | 退出 0,让用户 `subos use <other> && remove` | `commands.cpp:962-1039` |
| 委托安装的包(gcc→mingw) | `exact removal version is not registered`(#506) | — |

顺序也是错的方向:**hook 先于状态写入**(`installer.cpp:3373-3462`)。hook 一坏,包就成了不可删除的僵尸 ——
而 hook 坏恰恰是「包有问题」时最常见的形状。

**doctor 的范围:** `doctor.cpp:88-93` 明写「DB 层以上一切按当前 subos」;`--all` 是 **verbose 的别名**
(`xself.cpp:155`),不是「所有 subos」;没有 `--subos <name>`。它能改写其它 subos 的 workspace 文件
(`repair_other_subos_`,`doctor.cpp:3050-3113`),但载荷级修复、shim 表同步、manifest 修复都只做当前 subos。
`init.cpp:430-432` 有一行注释把这活推给一个**不存在的** `subos doctor --fix`。

**「本地从来没真正处理好过」的因果链**(这是本次最重要的一条发现):

```
真机 111 个 subos
 → 当前 subos 之外的包对 doctor 都是 ForeignPayload
 → ForeignPayload 是 report-only(doctor.cppm:113-120,修就等于把包装进错误的 subos)
 → after.foreignPayloads 永远 ≠ 0
 → doctor.cpp:973 永远不写版本戳
 → §1.1 的四处提示永远在,并且提示的内容(run doctor --fix)正是刚跑过、没用的那条命令
```

`--fix` 在这台机器上**结构性地不可能收敛**,不是哪个修复器写错了。

### 1.4 一个坏包拖垮别的

按失败类型,当前的爆炸半径:

| 失败 | 传播 | 位置 |
|---|---|---|
| 某个索引仓加载失败 | **按仓跳过**,记 `loadWarnings_` | `catalog.cpp:515-516` ✅ |
| 某个 recipe 加载失败(作为依赖) | `resolver.cpp:186` 只 warn,但 `plan.has_errors()` 让 `execute` **整单拒绝** | `installer.cpp:2250` |
| install hook 中途抛错 | 载荷留在盘上,重试说 already installed(memory);现有 `IncompletePayload` 失败戳能让 `--fix` 重跑 | `repair_incomplete_` |
| 一个 subos 的 `.xlings.json` 损坏 | `nlohmann::json::parse(..., false)` 不抛;后续是否按条目跳过**未从代码确定** | `subos.cpp:63`,`profile.cpp:157-187` |
| 由 mcpp 装的包 | `xlings install <任何包>` 把它们的 shim 清掉,含 mcpp 自己(#582) | — |
| 只读 home 上 `--fix` | 曾 SIGABRT,2026.9.4.1 已修 | 0906 doc B1 |

**没有的东西:** sanitizer 构建(#433,一个 UAF 穿过三个发布);「坏 home」fixture 集(`tests/` 里 grep
`broken|corrupt|fault` 只命中 fixture 索引里的无关文件);per-subos 的读取隔离测试。

### 1.5 跨平台

| | Linux | Windows | macOS |
|---|---|---|---|
| 单元测试 | 52 个文件 | 同(`continue-on-error`,verdict 末尾再断) | 同 |
| e2e | **135**(run_all 清单) | **~10** `.ps1` | **~7** `.sh` |
| fresh-install / 冷机 | ubuntu + centos7 | quick_install.ps1 | quick_install.sh |

平台分支集中在 `xself/install.cpp`(10)、`subos/sandbox.cpp`(9)、`xvm/shim.cpp`(6)、
`xself/uninstall.cpp`/`doctor.cpp`/`xim/installer.cpp`(各 5)—— 分布是合理的,问题不在分支数,
在**§1.3 那类「范围」问题在两个平台上几乎没有 e2e**:remove、doctor、跨 subos 在 Windows/macOS 只有
`subos_cmd_contract` 和 `bootstrap_home` 两条摸得到。memory 里 [[cross-platform test traps]] 的六个
POSIX 假设都是靠 CI 变红才发现的,没有一条是本地先测出来的。

---

### 1.6 真机上跑一遍 doctor:三个数字和两个缺陷

在 `~/.xlings`(111 subos)上只读地跑了三种模式:

| 命令 | 耗时 | broken payload | 报「no remedy,`--fix` 会丢弃注册」 | 报「→ run xlings install …」 |
|---|---|---|---|---|
| `self doctor` | 5.9 s | 107 | **100** | 0 |
| `self doctor --deep` | 7.8 s | 107 | 3 | **99** |
| `self doctor --fix --dry-run` | 36.5 s | — | — | 97 xim + 2 local「would run」 |

**D1 · 默认报告对 100 条 finding 说了假话。** 同一批条目(fd@10.4.2、go@1.26.2、65 个旧 mcpp…)
索引里明明有(`pkgs/f/fd.lua:30` 就是 `10.4.2`),默认模式却说「没有任何索引提供」。原因:
`doctor.cpp:4564` 只在 `deepAudit || fix` 时构造 catalog,而 `CoordinateProbe`(`:4617`)在没有 catalog 时
对一切返回 false,渲染层(`:3986`)把 false 原样翻译成「no package in any index provides」。
`--deep` 只多花 **2 s** 就得到正确答案 —— 这是 [[reporter/repairer predicate drift]] 的标准形状,
而且这次是**默认入口**在说假话。这解释了「本地从来没处理好过」的一半:用户看到的报告和 `--fix` 要做的事不是一回事。

**D2 · 修复梯子对「没人引用的坏载荷」选了最贵的一档。** 107 条 broken payload 里 65 条是 `mcpp@0.0.24…2026.8.30.2`
的旧版本,没有一条带 `[active]` 或 `[subos: …]` 标记,即没有任何 subos 引用它们。梯子
(`repair_payloads_`,`doctor.cpp:3125+`)的规则是「索引能提供就重装」,所以 `--fix` 在这台机器上 =
**下载 65 个 mcpp 旧版 + 34 个别的**,几百 MB、几十分钟,然后大概率中途失败。正确的档位是 prune:
记录没人用、载荷已坏,丢掉注册,需要时 `xlings install mcpp@0.0.100` 一秒回来。
这解释了另一半:`--fix` 不是不收敛,是**永远跑不完**。

**另外两个数字:** 38 条 finding 的 remedy 是 `xlings subos use <X> && xlings self doctor --fix`,
分布在 25 个 subos 上 —— 就是痛点里「还要进相关 subos」的全部来源;全 home 16,588 个 sysroot 链接
`find -xtype l` 用了 0.03 s,所以「全 home 扫描太慢」这个担心不成立(§8)。

---

## 2. 根因归并

| # | 根因 | 证据 | 影响的痛点 |
|---|---|---|---|
| R1 | **提示的触发源是一个没人负责更新的戳** | 戳只有两个写者、一个条件不可能满足 | 1 |
| R2 | **提示没有跨进程记忆** | 两个 `static bool`/`static set`,每条命令都是新进程 | 1、3 |
| R3 | **`local` 命名空间没有来源、没有列表、没有清除** | `--add-xpkg` 单向;200 个孤儿 recipe | 3 |
| R4 | **可写状态的范围 ≠ 用户操作的范围** | remove/doctor 按当前 subos;111 个 subos | 4、doctor |
| R5 | **删除把「执行 recipe 的告别仪式」放在「撤销注册」前面** | hook 失败 = 僵尸包 | 4、鲁棒 |
| R6 | **没有故障注入面** | 无坏 home fixture、无 sanitizer、Windows/macOS 无 remove/doctor e2e | 鲁棒、跨平台 |

---

## 3. 方案(按收益 / 风险排序)

### P0 · 提示治理:只在影响用户时说,说一次,说得短

**规则(三条,写进 `diag`):**

1. **默认档只打两类东西**:本次命令的结果,和**阻碍本次命令**的问题。
   「可能有问题,建议跑 X」这类**建议**属于 Notice,默认不打,`--verbose` 打,`self doctor` 报告里打。
2. **同一个 Notice 在一个 home 里只打一次**,直到它的触发条件变了。
   memo 落盘:`<home>/.xlings.json["notices"]`(或独立 `state/notices.json`),键 = notice-id + 触发指纹
   (版本戳的指纹是 `recorded→running`;降级警告的指纹是 `target+chosen+losers`)。
3. **打出来的每条 Notice 都要能被同一条命令关掉**。「run doctor --fix」这条如果 `--fix` 跑完还在,
   是 bug(§1.3 的链就是这条规则的反例)。

**具体改动:**

- `print_migration_hint_once` 的 `static bool` → 落盘 memo;`update.cpp:137` 的无条件两行**删掉**
  (新二进制第一次运行时自己会判断);`doctor.cpp:4441` 保留(报告里本来就该有)。
- 版本戳拆成两个字段,消除「戳」的语义混乱:`setupVersion`(只由 `self install` 写,信息用)、
  `verifiedBy`(由 `doctor --fix` 写)。提示只看 `verifiedBy`,而且**只在廉价探针为正时**打:
  探针 = `DuplicateVersionKey` / legacy binding 形状是否存在(已有检测器,只跑 DB 层,毫秒级)。
  探针为负 → 直接写 `verifiedBy`,不提示。这样一台干净的老 home 升级后一句话都不会说。
- `doctor.cpp:973` 的写戳条件改成 **「没有 Error 级 finding 残留」**,不是「零 finding」;
  `ForeignPayload` 本来就是 Notice。
- 降级警告(`announce_demotion_`)降为 Notice + 落盘 memo;**若 `local` 候选是发布 recipe 的陈旧副本
  (见 P1 的 provenance),不算冲突,不打。**

**验收不变量:** 任何命令连续跑两遍,第二遍的 stderr 里不出现第一遍已出现过的 Notice;
`doctor --fix` 退出 0 之后,下一条任何命令不打 migration hint。

### P1 · `local` 从「平行索引」变成「带来源的覆盖层」

- `--add-xpkg` 落盘 provenance:`xim-pkgindex-local/.overlay.json` 记 `{name, source(path|url), added_at,
  upstream_sha256(如果同名 recipe 当时在主索引里)}`。
- 新增三个动词(沿用 `config --*-xpkg` 拼写,和现有对称):`--list-xpkg`、`--remove-xpkg <name>`、
  `--clear-xpkg`。`remove-xpkg` 只删 recipe;已安装的 `local:` 版本由 `remove local:<name> --all` 管(P2)。
- **字节相同的副本自动 GC**(§7-Q2 定案):`--add-xpkg` 时若文件与某个已同步索引里的同名 recipe 字节相同,
  不入库,打一行 Notice「identical to xim:<name>; nothing to add」;`xlings update` 之后对 overlay 里每个文件
  再比一次,相同即删。真机 159 个里 **157 个会这样消失**,剩下 emsdk(改过)和 mcpp(落后一版)。
- **不相同的副本**:保留,`--list-xpkg` 标 `modified` / `behind xim by N`;本地版本 < 主索引且未 `--pin` 的
  从裸名解析里剔除(文件不动),`--clear-xpkg --stale` 手动清。降级警告只对**这一类**才可能出现。
- 开发隔离写成文档 + 一个工具:`.agents/tools/dev-home.sh`,封装 `env -i XLINGS_HOME=<slice>
  XLINGS_SHIM_ANCHOR=0 <abs-path-to-build>/xlings`,并在 README 的开发章节写清:**dev build 不进真 home**。
  `mcpp build` 那一侧如果会往真 home 注册 `local:xlings`,需要在 mcpp 仓改,这里只标记依赖。

**待你决定(§7-Q2):** 陈旧副本是「剔除但保留文件」还是「直接 GC」。

### P2 · `remove`:先撤注册,再告别;`--force` 意味着「无论如何都要没」

- **解析对象换成已安装记录**(#578):`remove <name>` 先查 DB 里这个名字有哪些版本、在哪些 subos,
  再决定;索引只用来找 uninstall hook,**找不到 recipe 不是失败**,是「跳过 hook」+ 一行 Notice。
- **顺序反转**:① 从所有目标 subos 的 workspace 撤注册 → ② `sync_shim_tables` → ③ 跑 hook
  → ④ 删载荷。hook 失败时状态已经一致(包不再可达),hook 的 stderr 原样打出、退出码 1 但**不回滚**;
  `--force` 下降为 Notice、退出 0(§7-Q3 定案)。前提:hook 里的 `xvm.remove(...)` 在注册已撤销后必须是
  幂等 no-op(memory:[[xpkg hook runtime traps]] 说它们本来就静默),实现时要用 e2e 锁住这一点。
- 新 flag:
  - `--all`:该名字**所有版本**;
  - `--all-subos` / `--subos <name>`:范围;没给时默认当前 subos,但**如果当前 subos 没有而别的有**,
    不再退出 0 让用户去切,而是列出并问(`-y` 时取 `--all-subos`);
  - `--force`:语义扩为「依赖者 / 缺 recipe / hook 失败 / 载荷缺失 全部不阻止」。
  - `--purge`(可选):连 `local` overlay 里的 recipe 一起删。
- #506(委托安装)在「解析对象换成已安装记录」之后自然消失:mingw-w64 的记录在 DB 里,gcc 的不在,
  `remove gcc` 会说「gcc 没有注册版本;它委托给了 mingw-w64@x,要一起删吗」。

**验收不变量:** 对任何 DB 里存在的 `<ns>:<name>@<ver>`,`remove <ns>:<name>@<ver> --force -y` 之后,
DB、所有 subos workspace、所有 shim 表、载荷 store 里都不再出现它,退出码 ∈ {0,1};
recipe 缺失、hook 抛错、载荷已删三种前置状态下结论相同。

### P3 · doctor:范围是 home,不是 subos;`--fix` 必须收敛

- **默认报告必须说真话(D1)**:`CoordinateProbe` 在默认模式也构造 catalog(实测 +2 s);做不到时
  remedy 留空并写「未探测」,不写「没有索引提供」。[[absent record needs an observation]]。
- **梯子按引用状态选档(D2)**:被某个 subos 引用(active 或 installed)的坏载荷 → 重装;
  没有任何 subos 引用的 → prune,报告里写「registration dropped; `xlings install <coord>` brings it back」。
  真机上 `--fix` 从 99 次下载变成 ~34 次下载 + 65 次 prune。
- **读取默认全 home**(§7-Q5 定案):DB 层本来就是全 home 的(5.9 s 已含 111 个 subos 的快照);
  subos 层的 workspace + shim 表 + sysroot 链接三项加进默认扫描(16,588 个链接 stat 0.03 s);
  报告按 subos 分节,当前标 `*`,`--subos <name>` 收窄;`--deep` 才逐 subos 读载荷 ELF。
- **`--fix` 跨 subos**:用已有的 `Config::set_active_subos_override`(`config.cpp:462-465`)逐个进入,
  做当前 subos 级的修复;`ForeignPayload` 类别随之消失(每个包在它自己的 subos 里都是 native)。
  `--fix --subos <name>` 收窄。
- **`--all` 改名**为 `--show-ok`(保留 `--all` 一个版本作为别名并警告),把「所有 subos」这个词还给它本来的意思。
- **收敛契约写进测试**:`doctor_fix_convergence_test.sh` 现在只测单 subos;加「三个 subos、其中一个含坏载荷、
  一个含缺 recipe 的 `local:`」的 fixture,断言 `--fix` 两遍后 Error=0 且第二遍 planned=0。
- #586(sysroot 只刷新活跃 subos)两侧都关(§7-Q4 定案):doctor 跨 subos 之后 `SysrootDangling` 修复覆盖
  所有 subos;安装侧改记录路径时,用已有的 `profile::find_subos_pinning_version` 找到**引用该版本的每个 subos**
  并刷新它们的链接 —— 集合有界(通常 1–5 个),不是 111 个。

### P4 · 鲁棒性:失败域 = 一个包

- **读路径**:`list`/`info`/`doctor`/shim 解析对每个条目 try/catch,坏条目变成一条 finding,不中断。
  具体三处:`profile::load_subos_snapshots` 对单个 subos 解析失败 → 记 `SubosUnreadable` finding 继续;
  `catalog.load_package` 失败在 `list` 里显示为 `(recipe unavailable)` 而非报错;`plan.has_errors()`
  保持整单拒绝(依赖缺失时半装是更坏的结果),但错误信息要点名是**哪个** recipe、来自**哪个**索引仓。
- **写路径**:install hook 失败已有失败戳;remove 按 P2 反转顺序;`sync_shim_tables` 只删自己表里的
  条目(#582 的 mcpp shim 被清,是「谁的 shim」没有记录 —— 表需要 owner 字段,不认识的不碰)。
- **故障注入 fixture**:`tests/fixtures/broken-home/` 一份 home,含五种坏:坏 subos json、缺载荷的注册、
  缺 recipe 的 `local:`、hook 会抛错的 recipe、指向不存在二进制的 xvm 记录。一条 e2e 断言
  `list / install <好包> / use <好包> / <好包的 shim> / self doctor` 五条命令都退出非崩溃码且**只动它们的目标**
  (before/after 快照 diff)。
- **sanitizer CI**(#433):ASan+UBSan 单元测试一个 job,`continue-on-error` 一个月后转硬。

### P5 · 跨平台:把「范围」类 e2e 带到另外两个平台

- 定义 **platform core contract**:bootstrap → install → use → remove(含 `--force`、跨 subos)
  → `self doctor --fix` 收敛 → shim 路由,6 条,三平台必跑。Windows 已经在用 git-bash 跑 `prepare_fixture_index.sh`,
  所以优先把 `.sh` 直接跑起来,`.ps1` 只留 PowerShell 特有的(profile、ACP)。
- P4 的 broken-home fixture 也进三平台。
- memory 里 [[cross-platform test traps]] 的六条写成 `tests/README.md` 的检查表,PR 模板引用。

---

## 4. 架构层:把「写状态的人」收成一个

三个根因(R1、R2、R4)都是**同一份状态有多个写者、每个写者只写自己看得见的范围**。
过去一年的修法一直是「把 N 个写者收成 1 个」(shim 表、subos runtime、entry binary,memory 里各有记录),
这次要收的是 **home 级元状态**:

```
home_state (新模块,src/core/home_state.cppm)
  ├─ stamps      setupVersion / verifiedBy            写者:self install / doctor --fix
  ├─ notices     {id, fingerprint, shown_at}          写者:diag::notice_once()
  ├─ overlay     local recipe provenance              写者:config --add/remove/clear-xpkg
  └─ subos       enumerate() / with_scope(name, fn)   读者:doctor / remove / install
```

- `diag::notice_once(id, fingerprint, text)` 是**唯一**打 Notice 的入口;`log::info` 直接打提示的四处全部改走它。
- `with_scope(name, fn)` 封装 `set_active_subos_override` + 还原,doctor/remove 的跨 subos 都用它,
  不再各自 `subos use`。
- 这不是大重构:四个文件各 100–200 行,现有 `Config::recorded_client_version` / `record_client_version`
  搬家即可。**不做**的:不动 DB 格式、不动 shim 表格式、不动索引格式。

---

## 5. 不变量(测试锁这些,不锁具体文案 —— [[assert only invariants]])

| # | 不变量 | 锁定它的测试 |
|---|---|---|
| I1 | 同一 home 上任何命令跑两遍,第二遍不重复第一遍的 Notice | 新 `notice_once_test.sh` |
| I2 | `doctor --fix` 退出 0 后,任何命令不再打 migration hint | 扩 `doctor_fix_convergence_test.sh` |
| I3 | `doctor --fix` 两遍:第二遍 Error=0、planned=0,**多 subos 下成立** | 同上 + broken-home fixture |
| I4 | `remove X --force -y` 之后 X 在 DB/workspace/shim 表/store 全部不可见,与 recipe/hook/载荷状态无关 | 新 `remove_force_contract_test.sh` |
| I5 | 一个坏条目不改变其它包的 `list/install/use/shim` 结果(快照 diff 为空) | 新 `broken_home_isolation_test.sh` |
| I6 | 主索引已有同名且不旧于本地副本时,裸名解析不打任何警告 | 扩 `index_same_name_namespace_test.sh` |
| I7 | I3、I4、I5 在 Windows 与 macOS 同样成立 | platform core contract |
| I8 | 对每条 finding,`self doctor` 与 `self doctor --deep` 给出的 remedy 相同(默认模式不说「没有索引提供」除非 `--deep` 也这么说) | 新 `doctor_remedy_mode_parity_test.sh`,用真机快照切片 |
| I9 | 没有任何 subos 引用的坏载荷,`--fix` 不发起下载(计 `xlings install` 子进程数 = 0) | broken-home fixture |

---

## 6. 分期与 PR 切分

| 期 | 内容 | 关闭的 issue | 风险 | 实施 |
|---|---|---|---|---|
| **1** | P0 全部 + `home_state` 骨架(stamps + notices) | — | 低;纯输出与元数据 | PR: this branch (fix/robustness-usability-2026.9.12), pkgindex PR #826 |
| **2** | P1(overlay provenance + 三个动词 + 陈旧退场) | #564 的一半 | 低;新增命令 | PR: this branch (fix/robustness-usability-2026.9.12), pkgindex PR #826 |
| **3** | P2(remove 反转顺序 + `--all`/`--subos`/`--force` 扩义) | #578、#506 | **中**;动删除顺序,需 I4 先落 | PR: this branch (fix/robustness-usability-2026.9.12), pkgindex PR #826 |
| **4** | P3(D1 默认模式跑 probe、D2 未引用即 prune、doctor 全 home、`--fix` 跨 subos、`--all` 改名) | #586(两侧) | 中;D1/D2 可先单独出一个小 PR,收益最大 | PR: this branch (fix/robustness-usability-2026.9.12), pkgindex PR #826 |
| **5** | P4 + P5(fixture、sanitizer、三平台 contract) | #433、#582、#427 的一部分 | 低,但 CI 时间 +10–15 min | PR: this branch (fix/robustness-usability-2026.9.12), pkgindex PR #826 |

期 1、2 可并行;期 3 依赖 I4 的测试先写;期 4 依赖 `home_state.subos.with_scope`。

---

## 7. 六个待定点:定案与依据

| # | 问题 | 定案 | 依据(真机度量) |
|---|---|---|---|
| Q1 | 提示默认档 | **分两类。** 建议类(「run doctor --fix」)默认**不打**,只出现在 `self doctor` 报告和 `--verbose`;唯一例外是**升级事件**:新二进制第一次运行时打一行「xlings 已升到 X;`self doctor` 可检查 home」,落盘 memo,只此一次。结果类(「选了 xim 没选 local」)**打一次**/指纹,落盘 memo。 | 四个发射点里三个是建议、一个是结果;建议在 doctor 报告里本来就有一份,再打是重复。结果类改变了本次命令做了什么,值得说一次;Q2 落地后它在真机上只剩 mcpp/emsdk 两个指纹。 |
| Q2 | 陈旧 local 副本 | **字节相同的直接 GC**(add 时不入库、update 后清);**不相同的保留**,解析里剔除、列表里标注、手动 `--clear-xpkg --stale`。 | 159 个里 157 个与已同步索引**字节相同**,删掉零信息损失;2 个不同的(emsdk 改过、mcpp 落后一版)是用户真正的意图,机器不该猜。 |
| Q3 | `remove --force` 容忍 hook 失败 | **容忍。** 无 `--force`:状态已撤、hook stderr 原样打出、退出 1、不回滚;`--force`:退出 0 + Notice。 | 261 个 recipe 里 252 有 uninstall hook,内容 308 处 `xvm.remove`(新顺序下 xlings 自己已做完)、45 处 `os.tryrm`、24 处 `system.exec`、7 处 `sudo`、3 处 profile 编辑 —— 会在 home 之外留东西的不到 30 个,而「hook 一坏包就删不掉」是每个包都可能撞的。 |
| Q4 | 安装侧写全部 subos 的 sysroot | **写「引用该版本的每个 subos」,不写全部。** `find_subos_pinning_version` 已有;doctor 兜底剩余。 | 改一条共享 DB 记录的人要刷新这条记录的每个消费者,集合有界(1–5);写 111 个既慢又会把包塞进没装它的 subos。 |
| Q5 | doctor 默认范围 | **全 home,分节显示;`--subos` 收窄。** 同时修 D1(默认模式也跑 probe)和 D2(未引用的坏载荷 prune)。 | 默认模式 5.9 s 已含全部 subos 快照;probe 只值 2 s;16,588 个链接 stat 0.03 s。真正贵的是 `--fix --dry-run` 的 36 s,来自搬家扫描与深审计,和范围无关。「当前 subos + 一行汇总」会把 38 条「去别的 subos 修」继续留给用户。 |
| Q6 | `local:xlings` / `local:mcpp` 来自 mcpp 构建流程? | **不是。** mcpp 源码仓(`mcpp-community/mcpp`)里没有 `--add-xpkg` 也没有 `local:`,它只 `xlings install mcpp`。来源是 **xim-pkgindex 的 recipe 测试循环**:`xpkg-creater` skill 第 1 步 `xim --add-xpkg <recipe>`,没有第 8 步「移除」;09-11 那一批 159 个不在 fish history 里,是 agent 会话跑的。**mcpp 仓不用改**;pkgindex 的 skill 文档加一行「测完 `xlings config --remove-xpkg <name>`」,Q2 落地后即使忘了也会在下次 `update` 自清。 | grep 三个 mcpp 工作树、fish history 时间窗、`testing-and-acceptance.md:15`。 |

## 8. 本次调查里我原本想错的四处

1. 我一开始以为 `local:mcpp@2026.9.11.3` 是一个**装进去的**开发版本。真机 `local-x-mcpp` store 最新只到
   0.0.56,没有任何 subos 的 active 指向 `local:mcpp` —— 它只是本地 recipe 副本声明的版本号。
   痛点 3 的根源是**索引层**而不是安装层,这改变了 P1 的整个形状。安装层的 `local:*` 记录另有约 10 个
   (cuda、node 等),那是 P2 的 `remove local:<name> --all` 要管的,两层要分开修。
2. 我以为提示关不掉是 memo 没落盘(R2)。落盘 memo 是必要的,但不充分:真正让它永久存在的是 §1.3 的链
   —— 修复器**结构性不可能**满足写戳的条件。只修 R2 会把「每条命令都提示」变成「每个版本提示一次」,
   而戳还是永远不更新。

3. 我在 v1 里写「全 home 扫描 0.63 s × 111 ≈ 70 s 太慢」,拿这个当 Q5 的主要顾虑。量出来默认模式 5.9 s
   **已经**加载了全部 111 个 subos 的快照,链接 stat 0.03 s;贵的是 `--fix --dry-run` 的 36 s,和范围无关。
   按文件名和 subos 数估算不是度量([[blast-radius undersampling]])。
4. 我以为 `local:` 的堆积和 mcpp 的构建流程有关(Q6)。三个 mcpp 工作树里没有一处 `--add-xpkg`;
   来源是 pkgindex 的 recipe 测试 skill,它有「加」没有「删」。修的地方在 xlings(P1)和 pkgindex 的一行文档,
   不在 mcpp。
5. 跨 subos 的 doctor e2e 一开始靠一个「打戳」helper 直接在 fixture 里写版本记录,那个 helper 读的是
   **旧的** `version` 字段。测试在旧写法下也能通过,但它锁的是「这个 helper 恰好写成了什么样子」,不是
   「doctor 认得这条记录处于什么状态」这个不变量——helper 和被测代码用两套字段名互相凑巧对上了。
   换成真实的写路径(而不是手写 fixture JSON)之后,这条测试才第一次真正对被测行为断言。
6. doctor 默认报告为了省 §1.3 量出来的 2 s,把「这条记录有没有可跑的修复动作」这个 probe 挂在了
   `--deep` 后面——于是默认输出对每一条它自己判定为「有问题」的记录,只要没跑 probe 就一律打印
   「no remedy」,在一个真实 home 上变成 100 行清一色的假阴性:不是真的修不了,是根本没问过。
   代价省错了地方:省的是 2 s 的探测开销,付出的是「默认体检报告不可信」。

## 9. 附:本次的度量方法(可复跑)

```bash
# 提示发射点
grep -rn "self doctor --fix\|registered in its format" src/
# 本地索引规模与来源时间
ls ~/.xlings/data/xim-pkgindex-local/pkgs/*/ | wc -l
stat -c %y ~/.xlings/data/xim-pkgindex-local/pkgs/m/mcpp.lua
# 版本戳与 subos 数
python3 -c "import json;print(json.load(open('$HOME/.xlings/.xlings.json'))['version'])"
ls ~/.xlings/subos | wc -l
# doctor 三种模式的耗时与 remedy 真假(只读)
time xlings self doctor        > /tmp/d.out 2>&1; grep -c "no package in any index provides" /tmp/d.out
time xlings self doctor --deep > /tmp/e.out 2>&1; grep -cE "→ run +xlings install" /tmp/e.out
time xlings self doctor --fix --dry-run > /tmp/f.out 2>&1; grep -c "would run" /tmp/f.out
# 未被任何 subos 引用的坏载荷(没有 [active] / [subos: …] 标记)
grep "✗ broken payload" /tmp/d.out | grep -vE "\[active\]|\[subos:" | wc -l
# 本地 recipe 与已同步索引逐文件 sha256 比对(159 → 157 相同)
#   见本文 §1.6;脚本为 python3 一段,比较 data/xim-pkgindex-local/pkgs 与 data/xim-pkgindex/pkgs
# uninstall hook 的副作用面
grep -rl "function uninstall" <pkgindex>/pkgs | wc -l
grep -rhA12 "function uninstall" <pkgindex>/pkgs | grep -oE "profile|os\.tryrm|xvm\.remove|system\.exec|sudo" | sort | uniq -c
# e2e 平台覆盖
grep -c '|.*_test.sh|' tests/e2e/run_all.sh
grep -cE '\.ps1' .github/workflows/xlings-ci-windows.yml
grep -cE 'tests/e2e/.*\.sh' .github/workflows/xlings-ci-macos.yml
```
