# 三个核心原因,七条设计 —— #604 / #582 / #606 / #602 / #464 / #376 / #608 / #522

> 基线 `5c52a16` / 2026.9.16.1。状态:**v3 —— 已实现并验证,见 §10 落地记录。**
> v1 按 issue 分族;v2 按 `.agents/docs/2026-08-06-subos-architecture-proposal.md` §1.3 的
> **R1–R7** 重新推导。**v1 的三条方案被这套规则自己判为 workaround,已替换**(§8)。
> 所有 `file:line` 与真机数字都是 2026-09-20 实测。

---

## 0. 一句话

八条 issue 不是八个 bug,也不是三族症状,而是**三个核心原因**:

| | 核心原因 | 违反 | 实例 |
|---|---|---|---|
| **C1** | 「**哪个 subos 是全局的**」有两个回答者,其中一个答错 | R3 | #604 #582 |
| **C2** | 「**读不到**」被当成「**是空的**」;派生表于是推导出「全部删除」 | R2 | #604 #582(放大器) |
| **C3** | 「**这次操作做成了什么**」由事后重新推导,而不是由一条记录回答 | R5 + R1 | #606 #602 #464 #376 |
| **C4** | 「**哪个 loader 为这个文件负责**」由当前 subos 回答,而不是由文件回答 | R3 + R6 | #608 #522 |

C1 与 C2 **互不包含**:各自能独立造成同一个后果,修掉任一个另一个仍在(§3.2)。
C3 有四张脸,它们共用同一处代码。C4 跨两个仓库,但 xlings 侧的那一半是**删除**一个回答者,不是新增一个调度器。

本仓已记录的形状:[[one question, many answerers]]、[[silent-success pattern]]、
[[absent record needs an observation]]、[[gate the message on behaviour]]、
[[read write invariant asymmetry]]。本轮新增一条:
**「同一个模块已经为它两个输入中的一个写对了规则,却没有应用到另一个」**(§3.2)。

---

## 1. 判据:先把规则摆出来

`.agents/docs/2026-08-06-subos-architecture-proposal.md` §1.3 的七条,本文全程用作判据:

- **R1 权威记录必须是全量的** —— 每一项都记,不只是「声明了东西的那些」。
- **R2 约定只在写端应用** —— 读端永远不猜。
- **R3 删除而非调和** —— 如果改动是**增加**一条路径而不是**移除**一条,它是 workaround。
- **R4 对产物断言,不对意图断言** —— 在安装时失败,而不是在运行时。
- **R5 决策必须持久化** —— 需要复现才能查看的决策不叫可追溯。
- **R6 内部消费者绑定 payload,不绑定视图。**
- **R7 闭包完整** —— 关于「需要什么 / 引用了什么」的测量必须覆盖传递闭包。

同文档 §1.3 给 R3 配的**可执行判据**,本文每条设计都要过一遍:

> 一个修复如果只是让两个独立答案**更可能一致**,它是 workaround。只有**删掉第二个回答者**才是解决。

同文档 **提议 A2**:契约文档里禁止「缺省即约定」式措辞——
「没有 X 就回退到 Y」要么改成「X 必须存在」,要么改成「没有 X 是错误」。
**本轮给 A2 补一条代码侧的可执行判据(建议纳入规范):**

> 读端出现 `return {}` / `value(key, default)` / `if (!exists) → 空值` 来处理「没读到」,
> 就是 R2 违例。「没读到」必须作为**一个与空值不同的值**向上传播。
> 判据来源:[[absent record needs an observation]] —— 20/35 个真实 SubOS 曾被一个
> 「从未记录过的字段的常量回退」盖上了它们从未运行过的 runtime 戳。

---

## 2. 证据(压缩;完整复现步骤见 §9)

三件都是本机 2026-09-20 用**已发布的 2026.9.16.1 二进制**实测,不是推断。

**E1 — 项目作用域里的一次 `use` 会删掉全局 shim。** 手搭隔离 home:

| 操作 | 全局 `subos/default/bin` |
|---|---|
| 非项目目录 `use demo 1.0` | `demo other` ✓ |
| **项目目录** `use demo 1.0` | `demo` —— `other` 被静默删除 |
| 把全局工作区拷到 `subos/_/.xlings.json` 再试 | `demo other` —— **删除停止** |
| 删掉那个文件再试 | `demo` —— **删除回来** |

随后在全局作用域跑 doctor,**逐字复现 #604**:`✗ shim table 1 missing (other)` / `exit = 1`。

**E2 — 真机规模。** 本机 `~/.xlings`:`✗ shim table 172 missing`。抽样交叉核对:
`gcc` active 在 `16.1.0` 但 bin 里没有它;`c++` / `ar` / `as` / `addr2line` 同理。
即 AGENTS.md 指定的开发工具链命令**处于激活状态却不在 PATH 上**。
(诚实边界:证明的是机制与单条因果;172 条未逐条归因,归因命令见 §7 的 T0。)

**E3 — CI 自带精确二分。** `gh run list --workflow xlings-ci-fresh-install.yml`,看 **job 级**:

| 运行 | 时间(UTC) | head | `* fresh (core)` |
|---|---|---|---|
| 33622280698 | 2026-09-02 10:59 | `7c35579` | ✅ 绿(此前 35 次全绿,回溯至 08-16) |
| **33732705448** | **2026-09-03 08:19** | **`ba51901`** | ❌ 四格全红 |
| …至 35437990256 | 2026-09-19 | | ❌ 四格全红 |

`ba51901` = **#580「a shim asserts routing, not state — one derived table, one writer」(2026.9.3.1)**。
这与 C1+C2 完全吻合:误读比 #580 老得多,但 #580 之前 shim 目录**只增不删**,
读空的工作区**没有后果**;#580 让这张表变成会算 `toRemove` 的派生表,老误读第一次有了破坏力。

> 对 #604 的两处更正:红的起点是 **2026-09-03**(可归到单个 commit),不是「至少 09-12」;
> 它猜测的「llvm 红格交接到 core 没人记录」属实,llvm 四格现已全绿,
> `reference_fresh_install_ci` 那条记忆已过期。

**E4 — `ldd` 对宿主二进制说谎,而且是静默的。** 本机 Ubuntu(**无 DT_RELR**,与 #608 的 Fedora 不同):

| 目标 | 宿主 ldd `not found` 行 | subos ldd | subos rc |
|---|---|---|---|
| `/usr/bin/echo` | 0 | 1 | **0** |
| `/usr/bin/ls` | 0 | 2 | **0** |
| `/usr/bin/git` | 0 | 3 | **0** |
| `/usr/bin/python3` | 0 | 4 | **0** |

**退出码是 0**。#608 报的 `DT_RELR` 硬失败(rc=1)只是同一缺陷在某台机器上的一种报错方式;
更常见的脸是安静地把宿主库全报成 `not found`,`set -e` 与 `pipefail` 都抓不到。

**E5 — `PT_INTERP` 在两个世界里都给出逐字一致的答案**(D6 的实证基础,§4.6)。

**E6 — 2.39 的 payload 在本机就是坏的。** `bin/ldd` 第 29 行
`./ld-linux.so.2 .64/... .x32/..."`(`RTLDLIST="` 头被吃掉),`bash -n` 在第 38 行报语法错误;
`TEXTDOMAINDIR` 仍是构建机路径 `/home/xlings/.xlings_data/...`。2.44 / 2.44.2 完好。

---

## 3. C1 与 C2:两个原因,一个后果

### 3.1 C1 —— 「哪个 subos 是全局的」有两个回答者

两个问题在代码里长得一样,其实不是一个:

| 问题 | 回答者 | 项目作用域下的值 |
|---|---|---|
| 这条命令**作用在**哪个 subos | `resolve_subos_scope_()` → `paths_.activeSubos`(`config.cpp:469-488`) | `_` 或 `<projectSubos>` |
| 哪个 subos**是全局的** | `global_subos_dir_()`(`config.cpp:367-373`) | 永远是 home 的 activeSubos |

`load_global_workspace_()`(`config.cpp:823-828`)用了**第一个**去回答**第二个**:

```cpp
auto subosConfigPath = paths_.homeDir / "subos" / paths_.activeSubos / ".xlings.json";
```

构造函数里顺序是对的(`config.cpp:594-609`:先算路径 → 读全局工作区 → 读项目配置 → 再算路径)。
**`reload_state_()` 把顺序反了**(`config.cpp:832-847`):先 `load_global_workspace_()`,
此时 `paths_.activeSubos` 已经是项目的。而 `reload_state()` 是每条写命令拿到锁后必做的第一件事
(`xim/commands.cpp:362`、`xvm/commands.cpp:554`、`xself/update.cpp:106`、doctor 的 20 处)。

**两个变种,后者更糟:**

| 项目形态 | 读到的文件 | `globalWorkspace_` |
|---|---|---|
| 匿名(无 `subos` 字段) | `~/.xlings/subos/_/.xlings.json`(通常不存在) | **空** |
| 具名 `subos: "myenv"` | `~/.xlings/subos/myenv/.xlings.json` | **另一个 subos 的工作区** |

这个 bug 在仓库里**已经被写下来过一次**,却被绕过而不是被删除。`xim/commands.cpp:1697-1702` 的注释:

> "set_active_subos_override's own reload reads the OLD `paths_.activeSubos`
>  (it updates the workspace before it recomputes which subos that even means)"

当时的处置是**再 reload 一次**。按 R3 判据,那是 workaround:它让两个答案更可能一致,没有删掉第二个回答者。

### 3.2 C2 —— 「读不到」被当成「是空的」

`load_workspace_from_file_`(`config.cpp:272-282`):

```cpp
if (!fs::exists(path)) return {};        // ← 文件不在 → 空工作区
... parse ...
return {};                                // ← 解析失败 → 空工作区
```

于是 `globalWorkspace_` 拿到一个**与「这个 subos 真的什么都没激活」无法区分**的值。
接着 `sync_shim_tables()`(`xself/init.cpp:568-620`)照单全收:

```cpp
sync_one(Config::global_subos_dir(),        // 目录:走对的回答者
         Config::global_workspace(), ...);  // 工作区:空的
```

`plan_table`(`xvm/shim_table.cpp:119-147`)于是把除保留名与项目贡献以外的**全部**条目算进 `toRemove`。

**关键对称性 —— 同一个模块已经为它另一个输入写对了这条规则:**

```cpp
// xvm/shim_table.cpp:76,compute_desired 内
for (const auto& project : projects) {
    if (!project.readable) continue;     // ← 项目状态读不到 → 不贡献,也不据此删除
```

`ProjectContribution::readable`(`shim_table.cppm:59-70`)正是「没读到 ≠ 空」的正确实现,
由 `project_contributions()`(`xself/init.cpp:489-547`)在文件缺失或解析失败时置 false。
**两个输入,一个有 `readable`,一个没有。** 有的那个从未出过事。

### 3.3 为什么两条都要修(互不包含)

| 场景 | C1 修好后 | C2 修好后 |
|---|---|---|
| 匿名项目(`subos/_/` 不存在) | ✅ 读对文件 | ✅ 拒绝重建 |
| **具名项目**(`subos/myenv/` 存在且可读) | ✅ 读对文件 | ❌ **不触发**——文件是可观测的,只是不该读它 |
| 全局 subos 的 `.xlings.json` 损坏 / 写入竞态 | ❌ **不触发**——路径对了,内容读不出来 | ✅ 拒绝重建 |

这与本仓 [[0802 cross-package switch]] 记下的形状一致:**两个 guard,谁也不吞掉谁。**

---

## 4. 设计:七条,每条过一遍 R3 判据

### D1 —— 「哪个 subos 是全局的」只留一个回答者(R3)

新增 `Config::global_subos_name_()`,并让所有读者走它:

```cpp
// config.cpp,private
[[nodiscard]] std::string Config::global_subos_name_() const {
    // 覆盖 > 环境 > home 的 activeSubos 字段。
    // 刻意不看 paths_.activeSubos:那个字段回答的是"这条命令作用在哪",
    // 在项目作用域里是项目的 subos,而项目的 subos 永远不是全局作用域。
    if (!activeSubosOverride_.empty()) return activeSubosOverride_;
    if (auto env = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS"); !env.empty()) return env;
    return globalActiveSubos_;
}
```

**被删除的回答者(R3 的实质):**
1. `load_global_workspace_()` 自己那份路径计算 → 改调 `global_subos_dir_() / ".xlings.json"`。
2. `ScopedSubosOverride` 里那次**补偿性的第二次 `reload_state()`**(`xim/commands.cpp:1705`)。
   v1 说「保留,无害,只更新注释」——**按 R3 那仍是第二个回答者,必须删**。
   删除的安全判据:`global_subos_name_()` 把 `activeSubosOverride_` 排在第一位,
   所以 `set_active_subos_override` 内部那次 `reload_state_()` 已经读对了。
3. `global_subos_dir_()` 顺带开始认 `activeSubosOverride_`。今天是 no-op —— 四个 override 调用点
   (`xim/commands.cpp:1696`、`subos.cpp:773`、`subos.cpp:2017`、`doctor.cpp:5072`)
   **都已同时设置 `XLINGS_ACTIVE_SUBOS`** —— 但它消掉了不对称本身。

**R7 闭包检查(v1 漏了,本轮补上)。** `grep -rn "\.activeSubos" src/` 全量过一遍,
还有**第三份手抄副本**:

* `xim/installer.cpp:1221-1232` 的 `current_workspace_config_path_()` 是
  `Config::save_workspace()`(`config.cpp:1515-1565`)那个四路分支的**逐字手抄**。
  今天答对了(它先判 `has_project_config()`,所以 `paths_.activeSubos` 只在非项目作用域被用到),
  但**它是第二份实现**,两边任何一边改动就分歧。
  → 把它提成 `Config::workspace_config_path()` 导出一次,两处都调。
* `xim/installer.cpp:1233-1245` 的 `load_workspace_file_` 是 `load_workspace_from_file_` 的
  **第二份副本**,带同一个 `return {}` —— 归 D2 处理。
* 其余 `paths().activeSubos` 的使用(`subos.cpp:128/1411/1501/1615`、
  `xim/commands.cpp:1268/1669`、`capabilities.cpp:240/558`、`doctor.cpp` 若干)
  回答的都是「这条命令作用在哪」,**是对的**,不动。

**判据:** 改完后,`src/` 里 `homeDir / "subos" / <某个名字>` 这种拼接只剩
`resolve_subos_scope_()` 与 `global_subos_name_()` 两处,且它们回答两个不同的问题。

### D2 —— 「没读到」是一个与「空」不同的值(R2)

```cpp
// 三种情况必须可区分,不能都塌成 {}
std::optional<xvm::SubosWorkspace> Config::load_workspace_from_file_(const fs::path&);
//   nullopt  → 没观测到(父目录不存在 / 文件不存在 / 解析失败)
//   有值但空 → 观测到了,这个 subos 确实什么都没激活(新建 subos 的合法状态)
```

* `globalWorkspace_` 旁边加 `globalWorkspaceObserved_`,由 `load_global_workspace_()` 写。
* **消费侧只加一条规则**:`sync_shim_tables()` 的 `sync_one` 在工作区未被观测时
  **不重建那张表**,并 `log::warn` 说明原因。不是阈值、不是比例——**二元的**,
  因为「观测到了」本身就是二元的。(阈值式的「删太多就不删」会是新的启发式,
  按 R3 判据同样是 workaround。)
* 删除 `installer.cpp:1233` 的副本,改调 Config 的那一个。

**这条与 D1 的关系已在 §3.3 证明互不包含。**
**判据(R2):** 读端不再有「没有就当成 X」的分支;`grep -n "return {};" ` 在这两个 loader 里归零。

### D3 —— 安装的结论是一条记录,不是一次重新推导(R5 + R1)

**这是 v1 最需要改的一条。** v1 对 #606 的方案是在 `commands.cpp:699` 的 `else if` 上
**加一个** `&& xvm::has_version(...)`。按 R3 判据:那只是让两个独立答案更可能一致,
**是 workaround**,而且它让「装成了吗」的回答者从 5 个变成 6 个。

今天「**这次安装做成了什么**」的回答者:

| # | 回答者 | 位置 |
|---|---|---|
| 1 | `result`(`expected<>`)——只覆盖 plan 级失败 | `commands.cpp:845-852` |
| 2 | `successCount` / `failedCount` 计数器 | `commands.cpp:808-818` |
| 3 | `InstallPhase::Failed` 事件流 | 同上 |
| 4 | `plan.pending_count()` / `requestedAlreadyInstalled[]` | `commands.cpp:719-725` |
| 5 | `xvm::has_version(db, ...)` 事后状态探测 | `commands.cpp:693` |
| 6 | **什么都不问**——`activate_requested_targets` 的 `else if` 分支 | `commands.cpp:699` |

**设计:** `Installer::execute` 返回一条**全量**(R1)的 per-node 结论记录并**持久于本次调用**(R5):

```cpp
struct NodeOutcome {
    std::string planKey, name, canonicalName, version;
    enum class Status { Installed, AlreadyPresent, Failed, Skipped } status;
    std::string errorCode;   // wire 串,空 = 无错误
    std::string message, hint;
};
// InstallReport { std::vector<NodeOutcome> nodes; }
```

**全部下游只读这一条记录**,回答者 6 → 1:
* **#606**:「installed, but 还解析到 X」只对 `status ∈ {Installed, AlreadyPresent}` 的 match 打印。
  不再事后探测 `has_version`(那是第 5 个回答者)。
* **#376**:`ErrorEvent` 由 `outcome.errorCode/hint` 构造,**一个发射点**。
  两个解压失败点(`installer.cpp:2851-2868`、`installer.cpp:3153-3170`)把
  `ExtractError::kind` 写进 outcome 而不是丢掉:
  `InvalidInputArchive → E_INVALID_INPUT`(hint 与既有的 `evict_invalid_archive_cache_`
  行为一致:缓存已清,重试会重新下载)、`LocalWriteFailure → E_DISK_FULL`、`Internal → E_INTERNAL`。
  **不新增 `ErrorCode::ExtractionFailed`** —— 现有三个够用,新增 wire code 是对所有客户端的兼容性事件。
* **exit code**:`any(status == Failed)`,不再维护独立计数器。
* `install_summary`、以及 `installer.cppm` 里已有的「promised programs and delivered none」检查,
  都改读同一条记录。

> **更正 #376 的前提**(v1 已记,此处保留):#374 之后解压失败**确实**会发 `ErrorEvent`
> (`commands.cpp:808-818`),mcpp 不会再看到 "xlings emitted no structured error"。
> 剩下的缺口只是 `kind` 被丢掉,范围比 issue 描述的小得多。

**判据(R3):** 改完后 `cmd_install` 里不存在第二处「从状态反推这次装没装成」。

### D4 —— 「会不会切换」由拥有最终状态的那个进程回答(R3)

`self update` 是 fork/exec:`update.cpp:55` 跑 `xlings install xlings@latest -y`,
`update.cpp:75` 再跑 `xlings use xlings latest`。子进程说的是真话,两行后父进程把它变成假话。

**规则:「我没有切换」是关于操作最终状态的陈述,只有拥有最终状态的那个进程可以做出它。**

* `xlings install` 增加 `--use`(透传 `useAfterInstall`)。今天该开关只有三条内部路径能置位
  (`cli.cpp:573/596`、`capabilities.cpp:100`、`subos.cpp:2036`),**CLI 上没有入口**,本身是缺口。
* `update.cpp:55` → `xlings install xlings@latest -y --use`。于是切换发生在子进程内,
  D3 的 outcome 记录里 status 为 Installed 且已激活,#606 的分支**不可达**。
* **删掉** `update.cpp:75` 的 `platform::exec("xlings use xlings latest")` —— 第二个执行者。
  `update_landed_on_index_build`(`update.cpp:105-124`)是**只读验证**,保留。

  安全判据(实现前必须逐条确认,任一不成立就退回"保留第二个 exec"):
  1. `install --use` 走 `activate_requested_targets` → `xvm::cmd_use(name, version)`,
     与 `use xlings latest` 是**同一个** `cmd_use`,因此入口二进制自替换
     (`xvm/commands.cpp:846` 之后那段)同样发生;
  2. 两条路径最终写进 workspace 的都是**具体版本串**而非 `latest` 引用
     —— `update_landed_on_index_build` 判的是 provider 前缀,依赖这一点;
  3. 真机 2026.9.16.1 → 2026.9.20.1 升级验证通过。

这同时消掉 issue 提到的第二点(提示说 `2026.9.12.1`、而刚跑的入口二进制是 `2026.9.14.1`):
提示不再出现,两个答案就不会并排出现。入口二进制与 workspace binding 的差异本来就由
`self doctor` 负责,不该由安装提示兼职。

### D5 —— 声明出去的契约由一个执行者执行(R2)

`inputSchema` 里的 `"required"` 是**已经发布给客户端**的契约(`interface.cpp:74-80` 原样吐给 `--list`),
但派发点 `interface.cpp:142` 只有 `cap->execute(cap_args, ...)`,没有任何一侧校验;
各能力自己用 `json.contains(...)` 手写,写法不一致。

真机盘点:**20 个能力,12 个声明了 `required`** ——
`search_packages(keyword)`、`install_packages(targets)`、`plan_install(targets)`、
`remove_package(target)`、`package_info(target)`、`list_installed_versions(target)`、
`use_version(target,version)`、`create_subos/switch_subos/remove_subos(name)`、
`add_repo(name,url)`、`remove_repo(name)`。

**设计:** 在 `interface.cpp:142` 之前按 `cap->spec().inputSchema`
(`runtime/capability.cppm:13-20` 已把它挂在 spec 上)集中校验:
* 解析失败或非 object → `E_INVALID_INPUT`,exit 1;
* `required` 任一字段缺失或为 `null` → `E_INVALID_INPUT`,exit 1,
  message 列出缺的字段,hint 给出该能力 `required` 全集。

**只校验 `required` 与顶层类型,不做全量 JSON Schema 校验**:后者要引依赖,
且会把今天宽松接受的形状变成硬错误,风险与收益不成比例。

**判据(R3 + R2):** 被删掉的回答者是 12 份手写检查(可分批迁出),
留下的唯一回答者是**已经发布出去的那份 schema**;新增能力**不需要**再写校验代码。

### D6 —— 「哪个 loader 为这个文件负责」由文件回答(R3)

xlings 源码里**没有任何 `ldd` 代码**:`ldd` 是 glibc 包注册的普通 `program`
(真机 `~/.xlings/.xlings.json`:`"ldd": {"type":"program", ...}`),
走通用 shim 派发(`xvm/shim.cpp:550`),最终 exec glibc payload 里的 `bin/ldd` 脚本;
而 subos 的 bin 在 PATH 首位,于是它遮蔽了宿主 `/usr/bin/ldd`。

**核心原因不是「遮蔽」,是「替换」**:两个 ldd 都用**自己的 `RTLDLIST`** 决定用哪个 loader,
**谁都不看文件自己的 `PT_INTERP`**。实测(§9 E5)连宿主 ldd 也一样:
它解析一个 xlings 二进制时,第一行是
`…/xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2 => /lib64/ld-linux-x86-64.so.2` ——
用宿主 loader 去跑一个声明了别的 loader 的文件。

而 `PT_INTERP` 是**安装时冻结的决定**(`2026-08-06` §1.5 的第三层原文:
「每个消费者的 RPATH/INTERP:安装时冻结的决定」),它就是这个问题唯一的权威回答。

**设计:** `ldd` 的 shim 在 exec 打包脚本**之前**:

```
取第一个非 flag 参数
  ├─ 没有参数 / 只有 flag(--version、--help)      → 交给打包的 ldd(原样)
  ├─ elfread::read() 返回 nullopt(不是可解析 ELF)  → 交给打包的 ldd(它会说 not a dynamic executable)
  ├─ interpreter 为空(静态 / 共享库)               → 交给打包的 ldd(文件对此没有答案)
  └─ interpreter 非空                               → exec 它,带 LD_TRACE_LOADED_OBJECTS=1
```

**注意这里没有「宿主 / 我们的」分支。** v1 的方案是「PT_INTERP 在 XLINGS_HOME 外 →
委派给宿主 ldd」——那是**新增一个调度器**,按 R3 判据是 workaround,而且它把
「宿主 ldd 存在」变成了新的依赖。本版**删掉**「用当前 subos 的 RTLDLIST 替换文件的 PT_INTERP」
这个动作本身,回答者 2 → 1。

**实证(E5,本机逐字 diff):**

| 目标 | 本设计的输出 vs |  |
|---|---|---|
| `/usr/bin/git`(宿主,PT_INTERP=`/lib64/ld-linux-x86-64.so.2`) | `/usr/bin/ldd` | **IDENTICAL** |
| `…/xim-x-zstd/1.5.7/bin/zstd`(我们的,PT_INTERP=`…/xim-x-glibc/2.44/lib64/…`) | 今天的 subos ldd | **IDENTICAL** |
| `…/xim-x-mcpp/…/bin/mcpp`(静态,无 PT_INTERP) | 走打包脚本 → `not a dynamic executable` | 与宿主 ldd 一致 |

零件全部现成:`elfread::read()`(`core/elfread.cppm:76`,**ELF32/ELF64 都支持**,
`elfread.cpp:185-198` —— 32 位正是 #522 里 Steam 那条路径的必要条件);
`exec_host_program_()`(`shim.cpp:528`);`report_passthrough_()`(`shim.cpp:516`)
的既有纪律——**只在 stderr 是 tty 时说话**(`shim.cpp:518`),管道里与裸 PATH 解析逐字一致。

**顺带的收益(v1 的方案拿不到):** 坏掉的 2.39 脚本对**所有**动态可执行文件都不再在路径上,
不只是宿主的。这对已经装在用户机器上的坏 payload 立即生效,不需要等索引仓发布。

**不做**:改 PATH 顺序、或不注册 `ldd`(#608 建议 3)。顺序是整个 subos 模型的基础,
subos 内部也确实需要能解析自身二进制的 ldd。把**语义**修对比把**顺序**改掉代价小得多。

### D7 —— 产物在受理时被断言(R4)

2.39 的 payload 是坏的(E6),而断言式的 `elfpatch.relocate_build_paths` **已经在 libxpkg 里**
(可从 `strings ~/.xlings/bin/xlings` 读到其设计注释:按 token 边界回溯、枚举整个 payload、
事后断言「不得残留 marker」+「每个被改写的 shell 脚本必须能被它自己 shebang 的解释器 `-n` 通过」;
注释点名的失败案例**逐字就是 2.39 的症状**)。

**问题是 2.39 的 recipe 没调用它。** 于是:

* **索引仓侧**(`openxlings/xim-pkgindex-fromsource`,另开 PR):glibc 2.39 recipe 改用
  `elfpatch.relocate_build_paths{ marker = "fromsource-x-glibc/" .. pkginfo.version() }`,删掉手写那套。
* **xlings 侧(R4)**:安装完成后,对该包 `programs` 里注册的每个程序,
  若解析目标是文本脚本且 shebang 指向 sh/bash/dash/ksh/zsh/ash,跑一次 `<interp> -n`;
  不过 → `Status::Failed` + `E_INVALID_INPUT`(经 D3 的 outcome 通路),**而不是装完说成功**。

**它为什么不是第二个回答者(R3 自检):**
`relocate_build_paths` 是**生产者断言**——保护调用它的 recipe 作者;
xlings 这条是**受理断言**——保护用户不受「从未采纳那个能力的 recipe」的影响。
2.39 恰好证明了后者存在。作用域不同,不是重复。

**边界(诚实):** 这条守卫**发现不了** `TEXTDOMAINDIR` 那类「语法合法但路径是构建机的」残留。
那个只有 `relocate_build_paths` 的 marker 断言能发现,而它必须由 recipe 调用。
不在 xlings 侧重造 —— 否则就真的是第二个回答者了。
范围被 `programs` 严格限死(不扫全 payload),成本恒定。

---

## 5. 覆盖矩阵

| issue | 由哪条设计解决 | 核心原因 |
|---|---|---|
| #604 fresh-install core 连红 | D1 + D2(+ §7 T6 的具体化断言) | C1 + C2 |
| #582 install 剪掉 mcpp 的 shim | D1 + D2 | C1 + C2 |
| #606 失败的安装打印 `installed` | D3 | C3 |
| #602 `self update` 切换前说没切换 | D4(+ D3 使分支不可达) | C3 |
| #464 `required` 无人执行 | D5 | C3 |
| #376 `ExtractError::kind` 被丢掉 | D3 | C3 |
| #608 ldd 拒绝宿主 DT_RELR 二进制 | D6 | C4 |
| #522 2.39 的 ldd 脚本被改写坏 | D6(立即缓解)+ D7 + 索引仓 PR(根治) | C4 |

---

## 6. 全部七条设计的 R3 自检

| 设计 | 删掉的回答者 | 新增的路径 | 过 R3? |
|---|---|---|---|
| D1 | `load_global_workspace_` 的路径计算;`ScopedSubosOverride` 的补偿 reload;`current_workspace_config_path_` 手抄副本 | 无(一个 private helper 取代三处计算) | ✅ |
| D2 | `installer.cpp` 里 loader 的第二份副本;两处 `→ 空值` 的猜测 | 一个 `observed` 位(**表达的是已有信息,不是新策略**) | ✅ |
| D3 | 回答者 6 → 1 | 一条记录类型 | ✅ |
| D4 | `self update` 的第二个 activation 执行者 | 一个 CLI flag(补上已存在能力的入口) | ✅ |
| D5 | 12 份手写 `required` 检查 | 一处集中校验 | ✅ |
| D6 | 「用当前 subos 的 RTLDLIST 替换文件的 PT_INTERP」 | 无分支(不是 host/ours 调度器) | ✅ |
| D7 | 无(新增受理断言) | 一条 R4 断言 | ⚠️ **R4 豁免**:R4 明确要求「在安装时失败」;§4.7 给了它不与生产者断言重复的判据 |

---

## 7. 实施

> 下一版 **2026.9.20.1**(`N` 从 1 起,`.0` 保留给正式版 —— AGENTS.md)。
> 版本号改**两处**:`mcpp.toml:32` 与 `src/core/config.cppm:13`([[version bump two places]]);
> `mcpp test` 不重链 `bin/xlings`,`mcpp build` 后必须验 `--version`。
> 构建前 `xlings use gcc@16.1.0`;**不要** `mcpp clean`([[mcpp clean breaks the build]])。
> 本地 e2e 必须 `XLINGS_TEST_MIRROR=CN`。

### PR 1 — D1 + D2(C1 + C2)

* **T0(先做,只读)** 真机基线:记录 `self doctor` 的 `shim table` 行(现 172 missing),
  以及 `comm -13 <(ls subos/default/bin) <(全局工作区里 kind=program 的 active 名字)`。
  修完再跑一次,差额即实际挽回量。**不要先跑 `--fix`**,那会毁掉基线。
* **T1** D1:`global_subos_name_()`;`global_subos_dir_()` 与 `load_global_workspace_()` 都走它;
  删 `ScopedSubosOverride` 的补偿 reload。
* **T2** D1 的 R7 收尾:`Config::workspace_config_path()` 导出一次,
  `installer.cpp:1221` 改调它。
* **T3** D2:loader 返回 `optional`;`globalWorkspaceObserved_`;
  `sync_one` 未观测则拒绝重建 + warn;删 `installer.cpp:1233` 的副本。
* **T4** 单元测试(`tests/unit/`):
  - 匿名项目作用域 `reload_state()` 后 `global_workspace()` == 全局 subos 文件内容;
  - 具名项目(home 里恰好有同名 subos)下 `global_workspace()` **≠** 那个 subos 的工作区;
  - `XLINGS_ACTIVE_SUBOS` / `activeSubosOverride_` 仍被尊重;
  - **未观测的工作区 → `sync_one` 的 `toRemove` 为空**(D2 的不变量,与 D1 独立可测)。
* **T5** e2e `tests/e2e/project_install_preserves_global_shims_test.sh`(新建,
  **必须注册进 `tests/e2e/run_all.sh`** —— [[e2e set -e silent death]]):
  全局装 A、B → 建只声明 A 的项目 → 项目内 `xlings install -y` → **断言 B 仍在** → `doctor` 退出 0。
  **不要** `rm -rf "$GLOBAL_BIN"`。
* **T6** 改 `tests/e2e/project_shim_mirror_test.sh`:把 setup 的 `rm -rf "$GLOBAL_BIN"`
  (第 20-22 行)换成「记录现有条目,结束时断言仍在」。保留它原有的新增方向断言。
  —— **守这块地的测试在 setup 里把证据删了,这是 v1 找到的新形状,值得写进 memory。**
* **T7** `tests/fresh-install/smoke.sh`:在 `suite_core` 的 `self doctor` **之前**、
  项目安装**之后**加一行「全局装的 mcpp shim 仍在」的断言,
  让 fresh-install 下次以**具体原因**变红而不是以一条泛化的退出码。

### PR 2 — D3 + D4 + D5(C3)

* **T8** D3:`NodeOutcome` / `InstallReport`;两个解压点写 `kind`;下游全部改读它;
  exit code 由记录得出。
* **T9** D4:`install --use`;`update.cpp:55` 用它;删 `update.cpp:75` 的第二个 exec
  (先逐条确认 §4.4 的三条安全判据)。
* **T10** D5:`interface.cpp:142` 前集中校验 `required`。
* **测试:**
  - `tests/unit/test_interface_protocol.cpp`:**缺少已声明 required 字段的 params 不得 exit 0**
    (#464 自己点名要这条);外加 `plan_install` + `{"packages":["cpp"]}` → exit 1 + `E_INVALID_INPUT`。
  - e2e:必然下载失败的本地 recipe(`url = "http://127.0.0.1:1/x.tar.gz"`),
    在另一版本 active 时安装 → 断言输出**不含** `installed, but`、退出 1。
  - e2e:截断的 `.tar.gz` → wire 上是 `E_INVALID_INPUT` 而非 `E_INTERNAL`。
  - `self update` 真机验证 2026.9.16.1 → 2026.9.20.1。
    注意 [[validate release via self install]]:别用空 `XLINGS_HOME` 造半个 home 来"验证"。

### PR 3 — D6 + D7(C4)

* **T11** D6:`shim.cpp` 里 `ldd` 的 PT_INTERP 规则。写成具名 helper,
  注释说明**为什么只有 ldd 是特例**(它是唯一「答案属于参数、不属于 subos」的注册程序)。
* **T12** e2e `tests/e2e/ldd_answers_in_the_files_world_test.sh`(注册进 `run_all.sh`,Linux only):
  - `ldd <宿主动态二进制>` 的 `=>` 行集合与 `/usr/bin/ldd` **一致**,且**不含** `not found`;
  - `ldd <xlings 拥有的动态二进制>` 仍解析到 store 内路径(不回归);
  - `ldd <静态二进制>` / `ldd --version` / `ldd`(无参)行为不变。
  按 [[assert only invariants]]:断言「集合一致」「没有 not found」这些**性质**,
  **不要** pin 具体库名或路径([[test pins spelling not property]])。
* **T13** D7:受理断言,走 D3 的 outcome 通路。
* **T14(跨仓,另开 PR)** `xim-pkgindex-fromsource` 的 glibc 2.39 recipe 改用
  `relocate_build_paths`。注意 [[index edit structural check]](解析并逐平台枚举,别只看 regex)
  与 [[index publish lag]](合并后 CI 仍会抓旧索引产物一段时间)。

### 顺序与风险

PR 1 独立、单点、可单独回滚,**先发**;它是唯一一条正在造成真机数据损失的。
PR 2 触及最关键命令(`self update`),D4 的三条安全判据不满足就退回保留第二个 exec。
PR 3 的 D6 风险最低(实测逐字一致),D7 可延后。

---

## 8. 自我 review:v2 推翻了 v1 的什么

**被本仓规则推翻的(这是本次 review 的主要产出):**

| v1 的方案 | 判据 | v2 |
|---|---|---|
| #606:在 `else if` 上**加** `&& has_version(...)` | R3:只是让两个独立答案更可能一致,回答者 5 → 6 | **D3**:一条全量 outcome 记录,回答者 6 → 1 |
| #608:PT_INTERP 在 XLINGS_HOME 外 → **委派给宿主 ldd** | R3:新增调度器,且引入「宿主要有 ldd」这个新依赖 | **D6**:删掉「用 subos 的 RTLDLIST 替换文件的 PT_INTERP」,无分支 |
| C1:`ScopedSubosOverride` 的补偿 reload「保留,无害,只更新注释」 | R3:无害的冗余路径仍是第二个回答者 | **D1**:删掉,并给出删除的安全判据 |
| 只修「读错了路径」 | R2 / 提议 A2:`return {}` 把「没读到」塌成「是空的」 | **D2**:独立的第二条原因,与 D1 互不包含(§3.3) |
| 闭包只看了 `config.cpp` | R7 | **D1 的 R7 收尾**:找到第三份手抄(`installer.cpp:1221`)与第二份 loader 副本(`:1233`) |
| #602:保留 `use xlings latest` 第二个 exec | R3 | **D4**:删掉,保留只读验证,并写明退回条件 |

**被度量推翻的(v1 已记,保留):**

1. **「#604 和 #582 是两条 issue」** —— 同一条因果链,一个在 CI、一个在用户机器上。
2. **「#604 是 doctor 的 reporter/repairer 谓词漂移」**(issue 自列的两种读法之一)—— 错。
   doctor 与 writer **共用** `plan_shim_table`(`doctor.cpp:931` vs `init.cpp:595`),没有漂移;
   漂移在上游:喂给它的 `global_workspace()` 是空的。
3. **「#376 的前提成立」** —— 已过期。#374 之后解压失败确实会发 `ErrorEvent`;缺口只剩 `kind`。
4. **「#608 是 Fedora 44 / DT_RELR 的问题」** —— 错。本机 Ubuntu 无 DT_RELR,
   同一缺陷表现为「每个宿主库 not found + rc=0」,**更安静因而更危险**(E4)。
5. **「2.39 的坏 ldd 说明改写逻辑还没修」** —— 不准确。断言式的 `relocate_build_paths`
   已在 libxpkg 里;问题是 **2.39 的 recipe 没调用它**。根治归属在索引仓。
6. **「fresh-install 的红始于 2026-09-12」**(#604 的观察窗口)—— job 级历史显示始于
   **2026-09-03 的 `ba51901`(#580)**,之前 35 次全绿。CI 自带的二分比 issue 那张表精确一个 commit。
7. **「`xlings install -g` 也会剪 shim」** —— 错。`-g` 走 `set_force_global_scope`(`cli.cpp:1713`),
   它立刻 `update_effective_paths_()` 把 `activeSubos` 翻回全局名,之后的 `reload_state()` 就读对了。
   这条不对称正好解释了 fresh-install 里哪些包活下来、哪些没有。

---

## 9. 附:复现步骤

**E1(C1/C2,无需网络)** 手搭 home:`bin/xlings` 拷发布二进制;
`.xlings.json` 写 `activeSubos: default` + `versions{demo,other}`;
`subos/default/.xlings.json` 写 `{"workspace":{"demo":"1.0","other":"1.0"}}`;
`subos/default/bin/{demo,other}` 软链到 `../../../bin/xlings`;
项目目录写 `.xlings.json{"workspace":{"demo":"1.0"}}` 与 `.xlings/.xlings.json`(versions + workspace)。
然后对比「非项目目录 `use`」与「项目目录 `use`」后的 `subos/default/bin`。
具名变种:项目 `.xlings.json` 加 `"subos":"myenv"`,home 里也建一个 `subos/myenv`。

**E4/E5(C4)** `~/.xlings/subos/current/bin/ldd <宿主二进制>` vs `/usr/bin/ldd`;
再用 `PT_INTERP`(从程序头读)+ `LD_TRACE_LOADED_OBJECTS=1` 直接跑,三者 `sed 's/ (0x[0-9a-f]*)//' | sort` 后 diff。

**E6** `sed -n '29p' ~/.xlings/data/xpkgs/xim-x-glibc/2.39/bin/ldd` 与 `bash -n` 同一文件。


---

## 10. 落地记录(2026.9.20.1)

七条设计全部落地在**一个 PR** 里,加上跨仓的规范更新。每条都有**差分验证**:
同一个测试对 2026.9.16.1 发布二进制必须失败、对本次构建必须通过。

### 代码

| 设计 | 落点 |
|---|---|
| D1 | `Config::global_subos_name_()`;`global_subos_dir_()` / `load_global_workspace_()` / 新的 `Config::workspace_config_path()` 都走它。删掉 `ScopedSubosOverride` 进出两处补偿性 `reload_state()`,删掉 `installer.cpp` 里 `current_workspace_config_path_()` 这份手抄副本 |
| D2 | `Config::read_workspace_file_()` 返回 `optional`;`globalWorkspaceObserved_` + `Config::global_workspace_observed()`;`sync_shim_tables` 未观测则拒绝重建并说明原因。`installer.cpp` 的 `load_workspace_file_`(死代码)删除 |
| D3 | `InstallStatus` 增加 `planKey`/`errorCode`/`hint`,`Installer::execute` 统一填 `planKey`;`cmd_install` 用一张 `outcomes` 表取代两个计数器,notice / ErrorEvent / summary / exit code 全部读它;`extract_wire_error_` 把 `ExtractError::kind` 映射到 wire code + hint |
| D4 | `self update` 改用 `xlings install xlings@latest -y --use` |
| D5 | `interface.cpp` 派发前 `missing_required_fields_()` 集中校验 |
| D6 | `shim.cpp` 的 `ldd_target_interpreter_()` + `PT_INTERP` 委派 |
| D7 | **撤回,未落地** —— 实测发现它不成立,见下面第 6 条 |

### 测试

| 测试 | 对 2026.9.16.1 | 对本次构建 |
|---|---|---|
| `tests/e2e/project_scope_preserves_global_shims_test.sh`(E2E-114,5 行) | **FAIL** | PASS |
| `tests/e2e/ldd_answers_in_the_files_world_test.sh`(E2E-115) | **FAIL** | PASS |
| `tests/e2e/install_outcome_is_a_record_test.sh`(E2E-116,含正向对照) | **FAIL** | PASS |
| `tests/unit/test_interface_protocol.cpp` 新增 3 个 | **FAIL** | PASS |
| `tests/e2e/project_shim_mirror_test.sh`(改:不再 `rm -rf` 全局 bin) | PASS | PASS |
| `tests/fresh-install/smoke.sh`(加:项目安装后 mcpp shim 仍在) | — | — |

### 规范 / 文档

- `docs/spec/interface-ndjson-v1.md` §3.3.1:`required` 自 2026.9.20.1 起由服务端执行;§8 补解压错误码分级。
- `docs/design/xvm-version-management.md`:「作用在哪」vs「哪个是全局的」两个问题;「读不到」三态规则。
- `.agents/docs/2026-08-06-subos-architecture-proposal.md`:A2 的代码侧可执行判据 + A1 的三条新实例 + §9 落地批次。
- **跨仓** `xim-pkgindex/docs/V2/xpackage-spec.md`:R2 增补「"没观测到"是一个值,且不是空值」及其判据;
  「写契约」一节增补「两个读起来一样的问题就是同一个陷阱」。

### 实现期间新发现的缺陷:两条 agent 调用能删掉一个 home 里的全部 SubOS

不是读代码读出来的,是量 D5 的影响面量出来的。2026.9.16.1 上,12 个声明了 `required`
的能力里 **6 个对 `{}` 退出 0**;其中 3 个是 destructive,而其中一对组合起来是**数据丢失**:

```console
$ xlings interface create_subos --args '{}'   # {"exitCode":0}
$ xlings interface remove_subos --args '{}'   # {"exitCode":0}

subos before: current default keepme
subos after:  (空的 —— 连 subos 根目录本身都没了)
```

`create_subos` 注册了一个 name 为 `""`、dir 为 `<home>/subos/` 的条目 —— 那是 subos **根目录**,
因为 subos.cpp 里每条路径都是 `<home>/subos/<name>` 拼出来的。
`remove_subos` 随后找到这条注册、通过存在性检查、把根目录删了:
`default`、`current` 和这个 home 里其他所有 subos,一次调用,两次都报成功。

**只有发布出去的 agent 接口能走到。** CLI 走不到 —— 它的解析器要求这个参数
(`missing <name> for: xlings subos remove|rm`)。有人在前面看着的那条路,恰好是安全的那条。

两道守卫,互不重复:

| 层 | 陈述的是什么 | 挡得住 `{}` | 挡得住 `{"name":""}` |
|---|---|---|---|
| interface 派发点 | **请求**是否符合公布的 schema | ✅ | ❌(字段在,是合法请求) |
| `subos::create` / `subos::remove` | **操作**自己的前置条件,谁来问都一样 | ✅ | ✅ |

E2E-117 分别钉住两层,并且断言的是**效果**而不是退出码 —— 目录已经没了之后再退出非零毫无意义。
另外带一条对照:真实名字仍然能建能删(否则「全部拒绝」会通过上面每一行)。

### 实现期间被度量推翻的判断(v2 的错)

1. **「CLI 上没有 `--use`,需要新增」** —— 错。`cli.cpp` 的 install 子命令**早就有**
   `-u/--use`,我上一轮的 grep 模式漏了它。D4 因此只剩改 `self update` 一行。
2. **「2.39 的 recipe 没调用 `relocate_build_paths`,需要改索引仓」** —— 错。
   `xim-pkgindex/pkgs/g/glibc.lua` 的 `__relocate()` **已经**正确调用它(还带 `type()` 探测
   和老客户端回退)。**实测**:用本次构建在隔离 home 里全新安装 `glibc@2.39`,
   产出的 `bin/ldd` 的 `RTLDLIST` 完整、`TEXTDOMAINDIR` 已重定位、`bash -n` 通过、安装退出 0。
   → **#608 的「fresh install 里也有」不成立**:报告人说的是新建 **SubOS**,
   而 payload 在 `data/xpkgs` 里是全 home 共享的,建 subos 从不重新重定位它。
   真正的问题是**旧客户端留下的历史 payload**,补救是重装该包。**索引仓无需改动。**
3. **「D2 只要区分『文件在不在』」** —— 不够。第一版把「文件不存在」一律当成「没观测到」,
   于是**全新的 home 根本不写表** —— 正是本 PR 要修的 bug 的镜像。
   被 `project_shim_mirror_test.sh` 抓到。正确的是**三态**:
   目录不在 → 没观测到;目录在、文件不在 → **观测到且为空**(新建 subos 的合法状态);
   文件在但读不出 → 没观测到。已补 E2E-114 第 5 行锁住。
4. **「D7 直接跑 `<interp> -n` 就行」** —— 有假阳性。`#!/bin/zsh` 而宿主没装 zsh 时
   `zsh -n` 退出 127,那是「命令找不到」不是「脚本坏了」,会把好包拒掉。
   已加解释器可用性守卫:查不到就跳过 —— **没有观测就不下判决**。
6. **D7「注册的 shell 脚本必须能 parse」是一条不成立的断言** —— 发布前拿真机所有
   注册脚本量了一遍:**49 个里 3 个被拒,只有 1 个真的坏**。

   | 脚本 | 判定 |
   |---|---|
   | `ldd@glibc-2.39` | 真的坏(正是这条守卫的目标) |
   | `pip@3.13.12` | **假阳性** |
   | `pip3@3.13.12` | **假阳性** |

   pip 是刻意写成的 sh/python **polyglot**:

   ```sh
   #!/bin/sh
   '''exec' "$(dirname -- "$(realpath -- "$0")")/python3.13" "$0" "$@"
   ' '''
   import sys
   ```

   `sh` 把第 2 行读成 `exec` 并把文件交给 python;python 把同一行读成 docstring 开头。
   它一直能正常工作。`sh -n` 拒绝它,是因为 `-n` 解析**整个文件**,而 shell 根本走不到第 2 行之后。

   也就是说这条检查**无法区分**「被路径改写破坏的产物」与「polyglot / 任何提前 exec 走的脚本」。
   那不是断言,是一条有已知假阳性类的启发式 —— 而那一类里就有 `pip`,
   发出去会让**每台机器上的 `xlings install python` 失败**。已撤回。

   它本来要买的东西其实已经有人买了:`relocate_build_paths` 对**它自己改写过的**
   产物做同一条断言(它知道自己动了什么,这正是同一条检查在那里成立、在这里不成立的原因);
   2.39 的全新安装已被实测是好的;历史坏产物重装即修;
   而用户真正撞到的症状(宿主二进制的 `ldd`)由 D6 挡掉,坏脚本根本不在路径上。

7. **D6 第一版把 ldd 的 flag 一起传给了 loader** —— 自我 review 时发现,
   `ldd -r f` 变成 `ld.so -r f`,loader 把 `-r` 当成要追踪的程序:
   `-r: cannot open shared object file`。**同一个工具因为一个 flag 给出两种答案**,
   比它替换掉的那个统一的错误答案更难发现。已改为按 glibc 自己 ldd 的做法把
   `-d/-r/-u/-v` 翻译成 `LD_WARN`/`LD_BIND_NOW`/`LD_DEBUG`/`LD_VERBOSE`,只把文件交给 loader;
   不认识的选项原样交回打包脚本。五种形式实测与 `/usr/bin/ldd` 逐字一致。

8. **`#if defined(...)` 平台宏** —— 按 review 意见全部改为 `if constexpr (platform::OS_NAME == ...)`,
   包括本 PR 触及文件里原有的那些(`shim_table.cpp` 的 `kShimExt`、`shim.cpp` 的三处
   可执行后缀、`test_shim_table.cpp` 的 `named()`)。两个分支都会被编译器检查,
   另一个平台的 CI 不再是唯一会发现问题的地方。
