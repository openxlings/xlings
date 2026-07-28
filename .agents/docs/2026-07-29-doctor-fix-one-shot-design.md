# `xlings self doctor --fix` 一次修完 — 设计方案

2026-07-29 · 承接 `2026-07-28-self-repair-design.md`、`2026-07-28-multi-subos-repair-design.md`

目标只有一句：**在真实的 `~/.xlings` 副本上，`xlings self doctor --fix` 跑一次，
之后 `xlings self doctor` 退出码为 0，且第二次 `--fix` 什么都不做。**

前两轮"无感升级 / doctor --fix"优化没有实际效果，原因不是设计不对，而是
**没有在真实累积状态上验证过**：单元测试与 upgrade-sim 造出来的 home 太干净，
真实 home 里同时存在 Windows 记录、命名空间版本、跨 subos 引用、遗留 pairwise
绑定四种形态，它们互相咬死。所以本方案把"真机切片验证"写进验收，而不是写进
"后续可以做"。

---

## 1. 现场取证（2026-07-29，真实 `~/.xlings`，v2026.7.27.2）

`.xlings.json`：379 个 target，9 个 subos 快照，91G payload。
下面每一条都是从状态文件和 payload 里读出来的，不是推测。

### 1.1 `llvm@20.1.7` 是一条 **Linux home 里的 Windows 注册**

```json
"llvm":  { "versions": { "20.1.7": { "path": "/home/speak/.xlings\\data\\xpkgs\\xim-x-llvm\\20.1.7" } } }
"ar":    { "versions": { "20.1.7": { "alias": ["llvm-ar.exe"],
                                     "path": "/home/speak/.xlings\\data\\xpkgs\\xim-x-llvm\\20.1.7/bin" } } }
```

9 条 entry（`llvm ar cl lib link nm ranlib rc strip`）路径全是反斜杠，别名全带
`.exe`。payload 目录真实存在，里面是 29 个 `.exe` / `.dll`：

```
clang++.exe  clang-cl.exe  lld-link.exe  llvm-ar.exe  libomp.dll …
```

这 9 条是 **owner-less legacy**（没有 `bindingGroup`，只有 pairwise `bindings`）。

**两扇门同时锁死**，这就是用户看到的死循环：

| 方向 | 发生了什么 | 代码位置 |
|---|---|---|
| `install llvm@20.1.7` | Linux 配方走 `alias_apps`（`llvm-ar` 无后缀），`os.isfile` 全部失败 → 6 条 `skip xvm add alias`。batch 里没有 `ar@20.1.7`；而 persisted legacy component 有 → `IncompleteLegacyComponent`，**nothing was changed** | `registration.cppm:761-779` |
| `remove llvm@20.1.7` | 配方 `uninstall()` 对 `collect_bin_apps()` 结果逐个 `xvm.remove` → `clang++.exe`（DB 里根本没有这个 target）→ `recipe removal target is outside the owned selection` | `removal.cppm:208-218` |

装不进去、卸不掉、doctor 只会再叫你去装 —— 三方互指。

### 1.2 命名空间写在 **version key** 里，所有渲染和探针都读反了

```json
"VBoxHeadless": { "versions": { "config:7.2.8": {…}, "local:7.2.8": {…} } }
"mcpp":         { "versions": { "0.0.27": {…}, … } }        ← 另有 local-x-mcpp payload
"freetype":     { "versions": { "2.13.2": {…}, "fromsource:2.13.2": {…} } }
"claude":       { "versions": { …, "local:2.1.142": {…} } }
```

- doctor 一律 `std::format("{}@{}", name, version)`（`doctor.cppm:429` 等 6 处），
  于是打印出 `mcpp@local:0.0.27`、`freetype@fromsource:2.13.2` —— **语法是错的**，
  命名空间必须在最前面：`local:mcpp@0.0.27`。
- `probe_reinstallable` 执行 `xlings info VBoxHeadless@config:7.2.8`
  （`repair.cppm:128`）。`catalog::parse_target_`（`catalog.cppm:94-104`）把
  `config:7.2.8` 当版本号，永远解析不出来 → 探针恒 false → 全部落进
  `✗ repair skipped … no package in the index provides this entry`。
  **所有命名空间条目从来没有被真正尝试修复过。**

### 1.3 finding 认的是 xvm **target**，修复要的是可安装的 **package**

`xlings install nm@20.1.7` —— `nm` 是 llvm 注册出来的程序名，索引里没有这个包。
同类：`ar/cl/lib/link/ranlib/rc/strip`、`xim-musl-gnu-gcc`（包名是 `musl-gcc`）、
`xim-aarch64-musl-gnu-gcc`（包名是 `aarch64-linux-musl-gcc`）、`VBoxHeadless`
（包名是 `virtualbox`）。

`owning_package()` 已经存在（`doctor.cppm:791-819`），但有两个致命限制：

1. 它**只在 `if (fix)` 分支里**（`doctor.cppm:732`）。plain `doctor` 的
   `→ run` 行完全没走它，所以裸跑 doctor 打印的命令 **100% 是错的**。
2. 三个候选（recorded provider / target 自身 / binding root）对上面这批全部
   落空，因为它们都不是包名。

**缺的是第四个候选，而且它最可靠**：payload 路径本身就编码了包名。
`package_store_name(ns, name) == ns + "-x-" + name`（`catalog.cppm:56-59`），
于是：

| payload 路径 | ⇒ 可安装坐标 |
|---|---|
| `data/xpkgs/xim-x-llvm/20.1.7` | `llvm@20.1.7` |
| `data/xpkgs/config-x-virtualbox/7.2.8` | `config:virtualbox@7.2.8` |
| `data/xpkgs/local-x-mcpp/0.0.27/bin` | `local:mcpp@0.0.27` |
| `data/xpkgs/fromsource-x-freetype/2.13.2` | `fromsource:freetype@2.13.2` |
| `data/xpkgs/xim-x-musl-gcc/15.1.0` | `musl-gcc@15.1.0` |
| `data/xpkgs/xim-x-aarch64-linux-musl-gcc/15.1.0` | `aarch64-linux-musl-gcc@15.1.0` |

一条规则覆盖了报告里几乎全部"命令是错的"的行。

### 1.4 报告是**修复前**的状态，尾巴上再贴修复结果

`add_field` 全部发生在检测循环里（`doctor.cppm:160-609`），repair pass 在
`doctor.cppm:660-963` 才跑。于是同一次运行里：

```
✗ binding state  'zlib' is active at 1.3.1 but 'libz.so' … is not active   ← 检测时
· deactivated  zlib (was part of xim:zlib@1.3.1)                            ← 修复后
binding state   8                                                           ← 仍按修复前计数
```

用户读到的是"报了 8 个问题，又说修好了" —— 这就是"重复提示"的主因，
`2026-07-28-self-repair-design.md` §6.2 写了"re-detect once, at the end"，
**只对 repair 结果做了，没有对整份报告做**。

### 1.5 同一个 release / 同一份 payload 被拆成 N 行

```
✗ broken payload  VBoxHeadless@config:7.2.8 path …/config-x-virtualbox/7.2.8 missing
✗ broken payload  VBoxManage@config:7.2.8   path …/config-x-virtualbox/7.2.8 missing
✗ broken payload  vbox@config:7.2.8         path …/config-x-virtualbox/7.2.8 missing
```

一个丢失的 payload 目录 ⇒ 3 行；llvm ⇒ 9 行。加上 30 行
`ⓘ release anchor`（意思是"这里没问题"），报告里 60% 是噪音。

### 1.6 `claude` 别名告警是真 bug：引号吃掉了绝对路径判断

```json
"alias": ["\"/home/speak/.xlings/data/xpkgs/xim-x-claude/2.1.142/…/claude.exe\""]
```

`doctor.cppm:512` 的 `fs::path(alias_prog).is_absolute()` 拿到的字符串首字符是
`"`，判定为**相对路径**，于是走 `resolve_executable` 失败 → 告警。
9 条 warning 里有 5 条是这个原因，纯误报。

### 1.7 其他 subos 的 18 条 finding：步骤已经写死了，却要用户手动做

`inspect_subos_references`（`inspect.cppm:361-411`）给出的 hint 已经是确定动作：

- `xvm-subos-installed-dangling` → 从那个 subos 的 `installed[]` 里删掉一项
- `xvm-subos-active-missing` → 那个 subos 的 `active` 指向未注册版本

两者都是**纯元数据编辑**，不需要网络、不需要 payload、不会把包拉进当前 subos。
今天却一律 report-only（`doctor.cppm:602-609`）。

### 1.8 本 subos 的 INV-1 也没人修

```
✗ binding state  the active version is not registered [xim-aarch64-musl-gnu-gcc@15.1.0-aarch64-musl]
```

`--fix` 里有 dangling-edge pruning、incoherent deactivation、metadata reset，
唯独没有 INV-1（active 指向未注册版本）的处理，而它和 1.7 的
`subos-active-missing` 是同一种病，只是发生在当前 subos。

---

## 2. 缺陷清单 → 修复项

| # | 缺陷 | 用户可见症状 | 修复项 |
|---|---|---|---|
| D1 | 报告是修复前状态 | 报了又说修好了，计数对不上 | **F1** detect → repair → re-detect → render |
| D2 | 坐标渲染把 ns 放在版本里 | `mcpp@local:0.0.27` | **F2** `display_coordinate()` 统一渲染 |
| D3 | remedy 从 target 生成 | `xlings install nm@20.1.7` | **F3** payload 路径反解包名 + remedy 只从可安装坐标生成 |
| D4 | owner 解析只在 `--fix` 里 | 裸跑 doctor 的命令全错 | **F3**（同一函数提到检测阶段） |
| D5 | 探针不认命名空间 | 所有 ns 条目 "repair skipped" | **F3**（探针用 `ns:pkg@ver`） |
| D6 | 一 payload N 行 | 报告噪音 | **F4** 按 (payload, version) 折叠 + notice 归并计数 |
| D7 | 阶梯没有"修不好就清掉"的出口 | 死循环 | **F5** R4 prune |
| D8 | removal 对未注册 target 报错 | `remove` 也走不通 | **F6** 未注册即 no-op |
| D9 | registration 拒绝缺员的 legacy 组件 | `install` 走不通 | **F7** 同 payload 的 owner-less 缺员允许丢弃 |
| D10 | 引号绝对路径别名 | 5 条误报 warning | **F8** 去引号后再判断 |
| D11 | 其他 subos 只报不修 | 18 条要手动做 | **F9** 直接改它们的状态文件 |
| D12 | 本 subos INV-1 不修 | 1 条常驻 | **F10** 反激活 |

---

## 3. 设计

### F1 报告 = 修复之后的状态

`cmd_doctor` 拆成三段，`add_field` 只在最后一段调用：

```cpp
struct Findings { … };                       // 纯数据，不含渲染
Findings detect(const DoctorState&);         // 只读，无副作用
RepairOutcome repair(const Findings&, ...);  // 只改状态，不渲染
void render(const Findings& remaining, const RepairOutcome&, EventStream&);
```

`--fix` 的流程固定为：

```
detect()  →  repair()  →  Config::reload_state()  →  detect() 再来一遍  →  render(第二次结果)
```

渲染的**永远是第二次 detect 的结果**。修好的问题不再出现在列表里，只出现在
`healed N` 的汇总；没修好的问题原样列出，并附上"为什么没修好"。

这条同时消掉 §1.4 的自相矛盾和大部分"重复提示"，也让计数天然一致：
`broken payloads` 就是第二次 detect 数出来的数字。

不加 `--fix` 时只跑第一次 detect，行为不变。

**幂等性由构造保证**：第二次 `--fix` 的第一次 detect 就是空的，repair 无事可做。

### F2 坐标渲染：命名空间永远在最前面

新增单一入口（放 `xvm/db.cppm`，紧挨 `parse_ns_version`）：

```cpp
// versions DB 的 key 形如 "ns:ver"；对外展示与命令行接受的形式是 "ns:target@ver"。
// 两者顺序相反，doctor 过去直接 "{}@{}" 拼接，产出的是不能粘贴执行的字符串。
std::string display_coordinate(std::string_view target, std::string_view versionKey);
//   ("mcpp", "local:0.0.27")   -> "local:mcpp@0.0.27"
//   ("llvm", "20.1.7")         -> "llvm@20.1.7"
```

**替换 doctor 里全部 6 处 `"{}@{}"`**，以及 `inspect.cppm` 里所有把
`target`/`version` 拼进 `summary`/`hint` 的地方（`inspect.cppm:110-118`、
`180-190`、`262-274`、`375-406`）。

配套单测：`display_coordinate` 的往返性质 —— 对任意 `(t, ns:v)`，
`catalog::parse_target_(display_coordinate(t, v))` 解析出的 namespace/name/version
必须等于 `(ns, t, v)`。

### F3 remedy 只能从"可安装坐标"生成

新增 `xvm/owner.cppm`（纯函数，可单测）：

```cpp
struct InstallCoordinate {                  // 一定可以拼成命令行
    std::string ns;        // "" | "local" | "config" | "fromsource" | "xim"
    std::string package;   // 包名，不是 xvm target
    std::string version;   // 裸版本号，不带 ns
    std::string command() const;            // "xlings install ns:pkg@ver"
};

// 从 payload 路径反解。data/xpkgs/<ns>-x-<pkg>/<ver>[/...] 是安装器写死的布局
// （installer.cppm:2463 + catalog.cppm:56），所以这是**记录自带的**包身份，
// 不是猜测。反斜杠先归一化，Windows 记录也能解。
std::optional<InstallCoordinate>
coordinate_from_payload_path(std::string_view path, std::string_view xpkgsRoot);
```

`owning_package()` 从 `if (fix)` 里提出来，候选顺序改为：

| 序 | 候选 | 说明 |
|---|---|---|
| 1 | `bindingGroup.provider@providerVersion` | 当前格式记录，权威 |
| 2 | **payload 路径反解**（新增） | 覆盖 §1.3 全部案例，含 Windows 记录 |
| 3 | target 自身 | 0.4.69 anchor 条目本身就是包名 |
| 4 | 可达的 binding root | 成员名对索引无意义时 |

每个候选仍然要过 `probe_reinstallable`，但探针改成
`xlings info <ns:pkg@ver>`（用 `InstallCoordinate::command` 同源拼装），
于是 §1.2 的命名空间条目第一次真正被探测。

**渲染规则**：
- 解析出坐标 → 打印 `→ run  <coord.command()>`
- 解析不出坐标 → **不打印任何命令**，改为
  `→ 无索引提供此条目；--fix 会直接清除该注册`（对应 F5）

绝不再出现"照着做也没用"的命令。

### F4 折叠与降噪

- **broken payload 按 `(expandedPayloadPath, versionKey)` 分组**，
  一组一行，其余成员放进同一行的尾巴：

  ```
  ✗ broken payload  config:virtualbox@7.2.8  路径缺失 @xlings/data/xpkgs/config-x-virtualbox/7.2.8
                    受影响程序 3 个: VBoxHeadless, VBoxManage, vbox
    → run  xlings install config:virtualbox@7.2.8
  ```

  组标题用 §F3 解析出的**包坐标**；解析不出时退回组内字典序第一个 target 的
  `display_coordinate`。

- `ⓘ release anchor` / `BindingSeverity::Notice` 不再逐条打印，
  合并为汇总行 `ⓘ 正常但值得知道  release anchor 30 · sysroot 未跟踪 2`，
  明细放到 `--all`（新增 flag）。

- `⚠ alias unresolved` 同一 target 的多个版本合并为一行（`claude` 5 条 → 1 条）。

> **flag 名不能叫 `--verbose`。** 那是个**全局** flag：`cli.cppm` 用它抬日志级别，
> 并且在派发前把它从 argv 里**删掉**，所以 `self doctor --verbose` 里的解析永远
> 收不到。第一版就是这么写的，help 里写了、实测输出一字不差 —— 一个被文档化
> 的空操作。改名 `--all`：9 行 → 40 行，实测。

### F5 阶梯补第四级：R4 prune

现有阶梯 R2 re-register → R3 remove+install → stop。新增：

```
R4 prune   —— 删除 versions DB 里这条注册（以及指向它的 workspace/installed 引用）
```

**只有同时满足下列全部条件才允许**：

1. detect 已证明 payload 目录**不存在**，或存在但既无可执行文件也不是 release anchor；
2. R2、R3 都失败（或 R3 因不可重装而未尝试）；
3. §F3 解析不出可安装坐标，**或**坐标探针失败（索引里没有）；
4. 该 (target, version) 不被任何 subos 的 `active` 引用为"正在使用且可用"。

删的是**已经死掉的记录**：payload 已经没了，任何索引都拿不回来，留着只会
每次 doctor 变红、并且挡住迁移标记。这不是丢数据，是丢一条指向虚空的指针。

- `--dry-run` 逐条列出将被 prune 的记录（`→ would drop`），且**不做任何改动**。
- 报告里 prune 单独成类：`· 已清除  local:mcpp@0.0.27（payload 缺失且索引不提供）`，
  绝不混进 `healed`。
- prune 走 `acquire_state_lock` + `reload_state` + `save_versions`，与现有
  dangling-edge pruning 同一模式（`doctor.cppm:665-691`）。

### F6 removal：未注册的目标是 no-op，不是错误

`removal.cppm:208-218`，`op == "remove"` 且 `context.hasSelection` 时：

```cpp
auto memberIt = context.members.find(operation.name);
if (memberIt == context.members.end()) {
    // 该 target 在 DB 里根本不存在 —— 配方列的名字在本平台没有注册过
    // （llvm 的 uninstall() 会对 collect_bin_apps() 的结果逐个 remove，
    //  Windows payload 在 Linux 上就是 clang++.exe 这种名字）。
    // 删一个不存在的东西已经完成了。只有"存在但不属于本次 selection"才是冲突。
    if (!db.contains(operation.name)) continue;          // ← 新增
    return std::unexpected(RemovalError{ … SelectionInvalid … });
}
```

判据是 **DB 里有没有这个 target**，不是"在不在 selection 里"。存在但越界仍然
硬失败 —— 那是真正的越权删除。

单测：三个用例（不存在→跳过、存在且在 selection→删、存在但不在 selection→报错）。

### F7 registration：owner-less legacy 组件允许缺员

`registration.cppm:770-779` 今天要求 legacy component 的每个成员都在 batch 里，
否则 `IncompleteLegacyComponent`。这在"同一个包在另一个平台注册了更多名字"时
永久锁死（§1.1）。

放宽条件——缺席成员满足**全部**下列条件时，从组件里丢弃而不是拒绝：

1. 该成员 **owner-less**（无 `bindingGroup`）—— 没有任何 provider 在保护它；
2. 它的 `path` 归一化后与本 batch 的 payload 根**同源**（同一个
   `data/xpkgs/<store>/<ver>` 前缀）—— 证明它就是本包注册的；
3. 它当前不是任何 subos 的 active。

丢弃的成员写进返回的 `RegisteredMember` 列表（新增 `droppedLegacy` 标记），
由调用方打印一行 `[xvm] 丢弃本平台不再注册的遗留条目: ar@20.1.7`。

不满足 2 或 3 的仍然拒绝：那种情况下 batch 确实在改别人的东西。

> 这一条是 `#422` "owner-less 直接 adopt" 的自然延伸：既然 owner-less 条目
> 可以被就地接管，那么**同一份 payload 下、本平台不再产生的 owner-less 条目
> 就应该被就地清理**，而不是让它把接管本身挡回去。

### F8 别名去引号

`doctor.cppm:507-514`，取出 `alias_prog` 后先剥掉成对的 `"` / `'`，再判断
`is_absolute()`。同时对 `${XLINGS_HOME}` 做一次 `expand_path`
（`doctor.cppm:493` 的 TODO 顺手结掉）。

### F9 其他 subos：直接修状态文件

新增 `profile::save_subos_workspace(dir, SubosWorkspace)`（与
`load_subos_snapshots` 对称，`profile.cppm:202`），以及
`xvm::subos_workspace_to_json`。写入走 home 级 `acquire_state_lock`
（锁是 per-home 的，`lock.cppm:132`，覆盖所有 subos 状态文件）。

`--fix` 对其他 subos 做且仅做两件事：

| finding | 动作 | 为什么安全 |
|---|---|---|
| `xvm-subos-installed-dangling` | 从该 subos `installed[<target>]` 删掉该版本 | 没有任何东西通过 installed[] 分发；留着只会把 payload 钉在一个不存在的版本上 |
| `xvm-subos-active-missing` | 从该 subos `active` 删掉该 target（**反激活**） | 指针指向未注册版本，shim 分发必然失败。反激活是可见问题，用一条 `xlings use` 就能恢复；替它猜一个版本才是危险的 |

**明确不做**：不往别的 subos 安装任何东西，不把包拉进当前 subos。
这条边界是 `2026-07-28-multi-subos-repair-design.md` §5 定的，本方案不动它 ——
只是把"纯删除的元数据修复"从"报告"移到"执行"，因为它不需要任何决策。

报告里这类改动单列：
`· 其他 subos 已修  dev: 反激活 node（24.4.1 未注册）`。

`deferredToOtherSubos`（另一个 subos 拥有的 broken payload）**保持不修**，
仍然只报告 —— 修它需要在那个 subos 里跑 install，会把包拉进来。
迁移标记的门也保持包含它（`doctor.cppm:1030`）。

### F10 本 subos 的 INV-1 反激活

`--fix` 增加一轮：当前 workspace 里 active 指向未注册版本的 target，
按 F9 同样的理由反激活。放在 dangling-edge pruning 之后、incoherent
deactivation 之前（未注册的 active 会让后者误判）。

---

## 4. 模块与改动面

| 文件 | 改动 | 规模 |
|---|---|---|
| `src/core/xvm/db.cppm` | + `display_coordinate` | ~15 行 |
| `src/core/xvm/owner.cppm` | **新增**：`InstallCoordinate`、`coordinate_from_payload_path`、owner 候选链 | ~140 行 |
| `src/core/xvm/removal.cppm` | F6 | ~6 行 |
| `src/core/xvm/registration.cppm` | F7 | ~45 行 |
| `src/core/xvm/inspect.cppm` | F2 渲染统一；+ `plan_subos_metadata_repair` / `apply_…`（纯函数） | ~90 行 |
| `src/core/profile.cppm` | + `save_subos_workspace` | ~30 行 |
| `src/core/xself/repair.cppm` | + R4 prune、探针改用 `InstallCoordinate` | ~60 行 |
| `src/core/xself/doctor.cppm` | **拆成 detect / repair / render 三段**；F1、F4、F8、F10 | 重构主体，净增约 200 行 |

`doctor.cppm` 今天 1049 行、`cmd_doctor` 单函数 964 行 —— 这次拆分本身就是必要的
维护性修复，F1 也没法在不拆的情况下实现（检测必须能跑两遍）。

---

## 5. 验收：真机切片验证

**不接受"退出码 0"作为通过标准**，也不接受在 upgrade-sim 上通过 —— 前两轮
就是这么"通过"的。

### 5.1 为什么必须切片而不是整份复制

`~/.xlings` 91G，磁盘 `/` **已 100% 占用，仅剩 9.0G**。整份 `cp -a` 不可能。
好消息是关键状态全是小文件：

| 内容 | 大小 | 复制方式 |
|---|---|---|
| `.xlings.json`（379 targets） | 249K | 真实复制 |
| `config/`、`data/xim-pkgindex`、`data/xim-index-repos`、`data/local-indexrepo` | ~3M | 真实复制 |
| `subos/default/` | **9.7M** | 真实复制（sysroot 拆除必须忠实） |
| `subos/<其余 8 个>/.xlings.json` | KB 级 | 只复制状态文件 —— 它们正是钉住 payload 的东西 |
| `data/xpkgs/`（69G） | — | `cp -al` 硬链接farm + 修复目标包真实复制 |
| `bin/xlings` | — | 放入新构建的二进制 |
| `subos/current -> default` | — | **最后**创建（见下） |

### 5.2 两条必须遵守的规则（都踩过坑）

1. **`subos/current` 一定最后建**。`glob("subos/*/.xlings.json")` 即使跳过
   `default`，也会经 `current` 符号链接再次命中 `default`，把待测的绑定悄悄改掉，
   产出一个"看起来真实的假结果"。
2. **硬链接 farm 的风险点是"就地改写"，不是删除。** `rm` 只是 unlink，不动
   真实 payload；但 `io.writefile()` 会 truncate 同一个 inode ——
   llvm 配方的 `__install_linux_cfg()` 正是往 `install_dir/bin/clang.cfg`
   写文件。所以**凡是本次修复会重装的包，必须真实 `cp -a`**
   （`xim-x-llvm` 627M，空间够）。

   收尾用一次**证据检查**兜底，而不是靠推理：

   ```
   跑之前:  touch /tmp/marker
   跑之后:  find ~/.xlings/data/xpkgs -newer /tmp/marker | tee changed.txt
   要求:    changed.txt 为空
   ```

   非空即判定本次验证作废（并说明哪个包被就地改写了）。

3. 全程 `env -i HOME=$HOME PATH=/usr/bin:/bin XLINGS_HOME=$SLICE`，
   让运行完全离开宿主配置。

### 5.3 脚本

新增 `.agents/tools/slice-real-home.sh`（造切片）与
`tests/e2e/self_doctor_real_slice_test.sh`（跑断言）。
后者从环境变量取切片路径，切片不存在时 **skip 而不是 pass** ——
它依赖真实累积状态，不能进无条件 CI，但必须能被一条命令重跑。

### 5.4 断言表 — **已实测**（2026-07-29，`2026.7.29.0` vs 已发布的 `2026.7.28.4`）

切片：`slice-real-home.sh --dst … --bin …`，69G 硬链接农场 + 179 个 `.xpkg.lua`
真实副本 + `subos/default` 全量 + 239 条 sysroot 符号链接重指向。
每次运行都 `env -i HOME=… PATH=/usr/bin:/bin XLINGS_HOME=<slice>`。

| # | 断言 | 修复前（实测） | 修复后（实测） | 结果 |
|---|---|---|---|---|
| A1 | `self doctor --fix` 退出码 | 1 | **0** | ✅ |
| A2 | 紧接着 `self doctor` 退出码 | 1 | **0** | ✅ |
| A3 | 第二次 `--fix` 的 healed / pruned | — | **无（幂等）** | ✅ |
| A4 | `✗ repair failed` 行数 | 9 | **0** | ✅ |
| A5 | `✗ repair skipped` 行数 | 11 | **0** | ✅ |
| A6 | `broken payloads` | 20 | **0** | ✅ |
| A7 | `binding state` | 134 | **0**（`--fix` 前 6 行，折叠后） | ✅ |
| A8 | `other subos findings` | 18 | **0** | ✅ |
| A9 | `name@ns:ver` 形式的坐标 | 19 | **0** | ✅ |
| A10 | 每条 `→ run` 命令粘贴执行的退出码 | 3 条中 2 条失败 | **1 条，0 失败** | ✅ |
| A11 | `.xlings.json:version` | `v2026.7.27.2` | **2026.7.29.0** | ✅ |
| A12 | `claude` 相关 alias 误报 | 5 | **0** | ✅ |
| A13 | 报告行数 | 239 | **46（修复前）→ 9（修复后）** | ✅ |
| A15 | 真实 `~/.xlings/data/xpkgs` 未被改写 | — | **`find -newer` 为空** | ✅ |

| A14 | 修复后 `xlings use gcc` 双向可切 | — | **15.1.0 ⇄ 16.1.0 均成功，shim 报告切换后的版本，doctor 仍退出 0** | ✅ |

A14 在修复完成的切片上实测：`gcc` 有 4 个 version key
（`15.1.0` / `15.1.0-musl` / `15.1.0-aarch64-musl` / `16.1.0`），来回切换后
`gcc --version` 分别报 15.1.0 与 16.1.0，`self doctor` 退出码保持 0。
两个 musl flavor 也回到了 versions 列表 —— 这是 §5.5 第 2 条（R3 不再把好包
卸掉不装回去）之后的正确结果：它们现在是被重新注册的，而不是被当成
collateral 清掉的。

#### 一个差点让整张表变成"不可证伪"的坑

`grep` 在这份报告上**什么都不输出**，因为渲染出来的面板里有一个 NUL 字节，
GNU grep 于是把整个流当二进制，`-c` 连计数都不打印。后果是每一条断言都读成
空/0 —— **无论真实值是多少都会"通过"**。

发现方式是给每个"应该为 0"的断言配一个**必须非 0 的对照**：
把 A9 的 grep 拿去跑旧二进制的报告，预期 19，结果也是空 —— 说明坏的是 grep，
不是 bug 修好了。加 `grep -a` 之后：

| 断言 | 对照（必须非 0） | 断言值（必须 0） |
|---|---|---|
| A4/A5 on `fix.txt` | `· dropped` = 19、`other subos repaired` = 18 | `repair failed` = 0、`repair skipped` = 0 |
| A6 on `after.txt` | `nothing to do` = 1 | `broken payload` = 0 |
| A9 | 旧二进制 = 19 | 新二进制 = 0 |

规矩沿用 [[reference-isolated-home-test-traps]]：**先证明这条检查能报出非 0，
再相信它报出的 0。**

（面板里那个 NUL 字节本身是个小瑕疵 —— e2e 里 `command substitution: ignored
null byte in input` 的警告也是它。不影响正确性，另开。）

#### A10 的两次修正 —— 都是真问题

1. 第一次跑 A10 时三条命令全失败，原因是 TUI 面板行尾带 `\r`，被当成一个空
   参数传进去（`package '' not found`）。**harness 的 bug**，不是产品的。
   这正是"必须机器判定"的价值：目视永远看不出这个。
2. 修掉 harness 之后仍有 2/3 失败：`xlings install xim:musl-gcc@15.1.0`
   会被 dangling edge 卡住（`xvm-binding-validation-failed`），而 `--fix`
   会先剪边再装，所以只有 `--fix` 能成功。这是**产品 bug**：报告印出了
   一条当时不可能成功的命令 —— 正是用户第 5 条反馈的循环。
   修法见 F3 末段：只要扫描里存在 dangling edge，payload 类修复建议一律
   改为 `xlings self doctor --fix`。

### 5.5 实施中发现的、设计没有预料到的五件事

都是跑出来的，不是读出来的：

1. **source 侧 dangling edge 从来没被剪过，也从来没被报过。**
   `plan_dangling_edge_pruning` 对"own version 未注册"的边直接 `continue`，
   理由是"解析总是从真实的 (target, version) 出发，够不到它"。这个理由是错的：
   install 一旦注册了那个 version，边立刻变得可达，然后 batch 自校验失败
   （`legacy binding source version is missing`）。差分证明：同一份切片、同一条
   `xlings install xim:musl-gcc@15.1.0`，**带边失败、去边成功**。
   真实 home 上有 14 条这样的边。原来的单测断言了错误的前提，已重写。

2. **R3 会把好包卸掉不装回去。** 旧代码在 `remove` 成功后先问
   `removalDone`，答"记录还在"就直接 return —— 执行了 remove+install 的破坏
   一半，跳过了恢复一半。实测把一个正常的 `musl-gcc` 卸掉了。
   现在：remove 成功后**永远**尝试 install，`removalDone` 只决定**怎么报告**
   （避免谎称 REMOVED，这是 multi-subos e2e 守的那条）。

3. **修复顺序必须是"元数据 → payload → 元数据 → prune"。**
   一次 llvm 重注册会产生 29 个孤儿 shim 和 29 条 release 不一致 —— 那些正是
   第一阶段修的东西，只是当时还不存在。单趟修复会把它们原样报成"没修好"。

4. **prune 必须避开别的 subos 还在用的条目。** 设计里写了这个条件，实现时漏了，
   被 multi-subos e2e 抓到：它 prune 掉了 `other` 还 active 的版本，把
   "payload 坏了"换成了"指针指向不存在"，而且退出 0。

5. **修复失败必须自己走进退出码。** R3 把包从当前 subos 摘掉又装不回去之后，
   该条目在本 subos 变成"别人的问题"，重新检测看不见它，于是 doctor 退出 0 ——
   典型的 silent success。现在按 (target, version) 记录失败条目，只要它在最终
   扫描里还以任何形式存在，就计入退出码。

### 5.6 折叠规则（实现时补的，设计只写了 payload 一种）

同一个问题不能占 N 行。三处：

- **broken payload 按 payload 分组** —— 设计里就有。
- **binding state 按 (code, 条目) 分组** —— 一个遗留 anchor 每绑定一个程序就有
  一条 dangling edge，两个 gcc flavor 各五条，于是 14 行描述 5 个条目。
  字段名仍然全部列在那一行上。
- **alias unresolved 按 target 分组**，且 `warnings` 计数按 target 计 ——
  计数和列表必须对得上，否则就是上个版本刚修掉的
  "broken payloads 1 但列表里没有对应行"那个形状。

`ⓘ migration` 从独立 panel 改为主 panel 的最后一个字段：独立 panel 会渲染出
一个空标题栏，读起来像画坏了的框而不是脚注。

### 5.7 原始断言表（保留，作为设计时的预期）

| # | 断言 | 修复前（今天实测） | 修复后要求 |
|---|---|---|---|
| A1 | `self doctor --fix` 退出码 | 1 | **0** |
| A2 | 紧接着 `self doctor` 退出码 | 1 | **0** |
| A3 | 第二次 `--fix` 的 `healed` + 清除计数 | — | **0**（幂等） |
| A4 | `✗ repair failed` 行数 | 9 | **0** |
| A5 | `✗ repair skipped` 行数 | 11 | **0**（要么修好，要么按 F5 清除并计入"已清除"） |
| A6 | `broken payloads` 计数 | 20 | **0** |
| A7 | `binding state` 计数 | 8 | **0** |
| A8 | `other subos findings` | 18 | **0** |
| A9 | 输出里出现 `@<ns>:` 形式的坐标 | 6 处 | **0**（grep `-E '[A-Za-z0-9_.+-]+@[a-z]+:'` 无命中） |
| A10 | 每条 `→ run` 的命令粘贴执行后退出码 | 多数非 0 | **全部 0**（脚本逐条执行验证） |
| A11 | `.xlings.json:version` | `v2026.7.27.2` | 运行中的版本（迁移标记落地） |
| A12 | `alias unresolved` 中 `claude` 相关 | 5 | **0** |
| A13 | 报告总行数 | ~120 | **≤ 40**（降噪目标，非硬门槛，超出则复核 F4） |
| A14 | 修复后 `xlings use gcc 15.1.0` / `16.1.0` 双向可切 | 需实测 | 与修复前一致，不得回归 |
| A15 | 真实 `~/.xlings/data/xpkgs` 未被改写 | — | `find -newer` 为空（§5.2） |

**A10 是最关键的一条**：它把"命令是对的"从人工目视变成机器判定，
正是用户第 1、3、5 条反馈的直接度量。

### 5.5 每一步都要能被证伪

按 `reference-repro-from-real-home-slice` 的规矩：报告结果时必须打印
判别性计数（例如 `other-subos refs: before=18 after=0`），
证明两次跑的确处于不同状态 —— 而不是断言"通过了"。

---

## 6. 实施顺序

阶梯式：每一步都能独立验证，且**前一步的门没过就不进下一步**。

| 步 | 内容 | 门 |
|---|---|---|
| S0 | `slice-real-home.sh` + 基线：在切片上跑今天的二进制，把 §5.4 "修复前"一列**实测填满** | 基线数字与用户贴的输出一致 |
| S1 | F6 removal no-op + F7 registration 缺员容忍（单测先行） | 单测过；切片上 `xlings remove llvm@20.1.7` 与 `install llvm@20.1.7` 至少一条能走通 |
| S2 | F2 `display_coordinate` + 全量替换 | 往返单测过；切片输出 A9 = 0 |
| S3 | F3 `xvm/owner.cppm` + owner 解析提到检测阶段 | 单测（6 条路径反解用例）过；切片上裸跑 doctor 的 A10 通过 |
| S4 | F1 doctor 拆 detect/repair/render + 二次 detect | 切片上"报了又说修好了"的自相矛盾消失；A3 幂等 |
| S5 | F5 R4 prune（含 `--dry-run` 预览） | A5 = 0；`--dry-run` 后状态文件字节不变 |
| S6 | F8 别名去引号 + F10 本 subos INV-1 | A12 = 0；A7 = 0 |
| S7 | F9 其他 subos 元数据修复 | A8 = 0 |
| S8 | F4 折叠降噪 + `--verbose` | A13 |
| S9 | 全量 §5.4 复跑 + `find -newer` 证据检查 | A1–A15 全绿 |

---

## 7. 明确不做

- **不做版本升级。** "旧格式"不等于"旧版本"，`--fix` 不会把 gcc 15 换成 16。
- **不往别的 subos 安装东西。** F9 只删不装。
- **`--reset-metadata` 不并入 `--fix`。** 它丢的是 release 成员与头文件信息，
  是唯一"真的丢信息"的修复，必须显式请求。
- **不修 `xim-pkgindex` 里的配方。** llvm 配方在 Linux 上 remove 一堆 `.exe`
  是它自己的毛病，但**客户端不能依赖配方正确才能自愈** ——
  F6 让客户端对任意配方都保持可卸载。配方本身另开 issue。

---

## 8. 风险

| 风险 | 处置 |
|---|---|
| R4 prune 删掉用户还想要的记录 | 四个前置条件全部要求"payload 已不存在且索引拿不回来"；`--dry-run` 全量预览；报告单列不混入 healed |
| F7 放宽后 batch 改到别人的东西 | 三个条件里两个是"同 payload 根"和"非 active"，只放行本包自己的 owner-less 残留 |
| F9 反激活让用户以为包丢了 | 报告明写 `run xlings use <target> <version>` 恢复；`installed[]` 不动，包还在 |
| doctor 拆分引入回归 | S4 单独一步；`tests/unit/test_xvm_doctor.cpp`、`test_self_repair.cpp`、`tests/e2e/self_doctor_test.sh`、`self_doctor_multi_subos_test.sh` 全部先跑通再进 S5 |
| 切片验证污染真实 home | `env -i` + `XLINGS_HOME` 隔离；`find -newer` 事后证据检查；**任何破坏性命令都不对 `~/.xlings` 本体执行** |
