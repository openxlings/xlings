# 多 subos 下的 `self doctor --fix` —— 修复边界设计

**日期**: 2026-07-28
**类型**: 设计 (design)
**关联**: `2026-07-28-self-repair-design.md`、`2026-07-28-release-2026.7.28.1-notes.md`
**涉及仓库**: `openxlings/xlings`

---

## 0. 摘要

`self doctor --fix` 的修复阶梯是按**单空间**写的，但它检测的对象（versions DB、
payload）是**全 home 共享**的。这个错配在单 subos 的 home 上不可见，在多 subos 的
home 上产生四个具体后果，其中两个是谎报。

本设计不扩大 `--fix` 的能力范围，而是**给它划一条边界**：修当前空间拥有的东西，
对不拥有的东西说清楚该去哪修，并且不再把「没删成」报告成「删了」。

---

## 1. 状态分层：谁是共享的，谁是每空间独立的

| 层 | 位置 | 作用域 |
|---|---|---|
| versions DB | `~/.xlings.json:versions` | **全 home 共享** |
| payload | `~/.xlings/data/xpkgs/<pkg>/<ver>/` | **全 home 共享**（引用计数） |
| `workspace.active` | `~/.xlings/subos/<name>/.xlings.json` | 每空间 |
| `workspace.installed[]` | 同上 | 每空间 |
| shim / sysroot | `subos/<name>/{bin,lib,usr/include}` | 每空间 |
| 迁移标记 `version` | `~/.xlings.json:version` | **全 home 共享** |

`doctor` 的检测跨越了这条线：check 1/2/2.5/2.6（shim 层）和 check 4 的 workspace
部分只看**当前空间**（`Config::paths().binDir` / `effective_workspace()`），而
check 3（broken payload）遍历的是**全局 versions DB**（`doctor.cppm:363`）。修复
动作则一律发生在当前空间——`xlings install` 的 `config()` 一定注册进当前 subos
（`installer.cppm:2369` "mapping to current subos"）。

`grep -c subos src/core/xself/doctor.cppm` 命中的全是注释：doctor 没有任何跨空间
概念。`tests/e2e/self_doctor_test.sh` 也只用 `subos/default` 一个空间。

---

## 2. 四个具体后果

### D1. R3 谎报「REMOVED」（谎报，最严重）

`repair.cppm:191` 的 R3 是 `remove` 然后 `install`。多空间下 `remove` 有两条路径
会**返回 0 但什么都没删**：

- 该版本被其它 subos 引用 → `installer.cppm:2469-2481` 走 detach 早返回，
  DB 记录和 payload 都保留；
- 该包不在当前 subos 的 `active`/`installed[]` 里 → `commands.cppm:606-631`
  幂等 no-op，退出 0。

R3 拿到 0 之后认为"删成功了"，接着 `install` 因为记录还在而再次被注册层拒绝，于是
输出：

```
✗ repair failed  gcc@15.1.0 — REMOVED but could not reinstall — run `xlings install gcc@15.1.0`
```

「REMOVED」是假的。这是本仓库反复出现的 silent-success 形状的镜像版本：**没发生的事
被报告成发生了**，而且报的是最吓人的那种（用户以为包被删了）。

### D2. `--fix` 的作用域泄漏

check 3 扫全局 DB，所以只属于 subos B 的条目也会在 A 里被修。修复动作是
`xlings install pkg@ver -y`，它的 `config()` 会把包注册进 **A** 的
workspace/installed[]、在 A 的 `bin/` 建 shim、往 A 的 sysroot 铺头文件。

结果：在 A 里跑一次 `--fix`，B 的包被搬进了 A。用户没要求过这件事。

### D3. 其它空间的问题不可见，也没人告诉你去哪修

反过来，B 空间自己的 shim 缺失 / 孤儿 shim / 组不一致，在 A 里跑 doctor 完全看不到。
用户只能自己想到「要逐个空间跑一遍」。

### D4. 迁移标记按 home 盖章，按空间修复

`Config::record_client_version` 写 `~/.xlings.json:version`（`config.cppm:1167`），
是**全 home** 的。在 A 里 `--fix` 成功就盖章，B 从此不再出现
`run xlings self doctor --fix` 的提示——尽管 B 一次都没被修过。

这正是 2026.7.28.1 发布说明里被单独强调过的那类失效：**迁移承诺在它最常见的输入上
静默失效**，只不过上次是"标记永远落不下"，这次是"标记落早了"。

### D5. 引用计数按 scope 二选一，不取并集

`is_version_referenced_anywhere_`（`installer.cppm:1204`）通过
`workspace_config_paths_for_scope_`（`:1166-1201`）枚举工作区文件：Project scope
只枚举 `<proj>/.xlings/subos/*`，否则只枚举 `~/.xlings/subos/*`。两边**不取并集**。

于是一个 global scope 的包如果同时映射进了 project subos，在全局空间
`xlings remove` 看不到项目侧的引用 → payload 被真删 → 项目空间留下
`xvm-active-version-missing`，而项目里什么都没做过。

---

## 3. 设计原则

1. **共享的东西共享地修，独占的东西就地修。** payload 和 DB 记录是共享的，谁修都
   一样；workspace / shim / sysroot 是每空间的，只能在那个空间修。
2. **不能修的，要说清楚在哪修。** 一条 finding 如果属于别的空间，报告里必须带上空间
   名和切过去的命令，而不是沉默或越界修。
3. **不许谎报。** 一级修复动作声称成功，必须由独立复检确认，而不是信子进程的退出码。
   这条在 `2026-07-28-self-repair-design.md` §6.3 已经立过，R3 的 remove 是它的漏网。
4. **单空间 home 的行为一个字节都不许变。** 绝大多数用户只有一个 subos，本次改动对
   他们必须是完全不可见的。这条由「无人认领 → 当前空间可修」的归属判据保证（见 §4.2）。

---

## 4. 方案

### 4.1 新增：跨空间快照与只读检查（纯函数）

`profile.cppm` 增加快照加载（文件系统侧）：

```cpp
struct SubosSnapshot {
    std::string        name;
    xvm::SubosWorkspace workspace;   // active + installed[]
};
std::vector<SubosSnapshot> load_subos_snapshots(const fs::path& xlingsHome);
```

`find_subos_referencing` 改为基于它实现，消除第二份枚举逻辑。

`xvm/inspect.cppm` 增加**纯函数**检查（不碰文件系统，可单测）：

```cpp
struct SubosRef { std::string subos; Workspace active; WorkspaceInstalled installed; };

// 其它空间引用了本 home DB 里不存在的版本
std::vector<BindingFinding> inspect_subos_references(
    const VersionDB& db, const std::vector<SubosRef>& others);

// (target, version) 的归属：当前空间是否拥有 / 哪些别的空间拥有
struct OwnershipVerdict {
    bool                     ownedHere { false };
    std::vector<std::string> otherSubos;      // 排序后的空间名
};
OwnershipVerdict subos_ownership(const Workspace& here, const WorkspaceInstalled& hereInstalled,
                                 const std::vector<SubosRef>& others,
                                 const std::string& target, const std::string& version);
```

两个新 finding code：

| code | 含义 | severity | `--fix` |
|---|---|---|---|
| `xvm-subos-active-missing` | 空间 X 的 active 指向未注册的版本 | Broken | ❌ 报告，给切换命令 |
| `xvm-subos-installed-dangling` | 空间 X 的 `installed[]` 含未注册的版本 | Notice | ❌ 报告 |

`installed[]` 悬挂设为 Notice：它不影响当前使用（active 才被 dispatch 读），但会
让 `remove` 的引用计数把一个已经不存在的版本算成"还有人用"。报告即可，不进退出码。

### 4.2 doctor：按归属决定修不修（D2/D3）

check 3 每条 broken payload 先问归属：

```
ownedHere                    → 生成 RepairTask（今天的行为）
!ownedHere && otherSubos 非空 → 报告 "✗ broken payload [subos: B]"，
                               hint: `xlings subos use B` 然后 `xlings self doctor --fix`
!ownedHere && otherSubos 为空 → 生成 RepairTask（无人认领的孤儿，谁修都行）
```

第三条是**兼容性关键**：0.4.69 的 home 是 legacy 字符串 schema，`installed[]` 是空的，
所以升级用户的绝大多数条目都"无人认领"。判据写成"没人认领就修"，2026.7.28.1 的迁移
能力一字不变；写成"必须当前空间认领才修"会把它整个废掉。

### 4.3 R3：删完要复检（D1）

`repair_one` 增加一个注入的复检回调：

```cpp
// remove 之后确认记录真的没了。注入是因为 repair.cppm 不碰状态文件，
// 而这个函数存在的全部理由就是 remove 会在只 detach 的情况下报成功。
using RemovalVerifier = std::function<bool(const std::string&, const std::string&)>;
```

`run(remove) == 0` 之后若 verifier 说记录还在：

```
{false, "none", "re-register failed; `remove` only detached this subos —
                 <target>@<ver> is still referenced by another subos.
                 Remove it there first: xlings subos use <X> && xlings remove <t>@<v>"}
```

关键是 **rung 保持 "none"**，不再是 "reinstall"，因为没有任何东西被删除。doctor 侧
verifier 的实现：`Config::reload_state()` 后查 DB 里 `(target, version)` 是否还在。

### 4.4 迁移标记：所有空间都干净才盖章（D4）

`record_client_version` 的门槛从 `repairFailed == 0` 收紧为
`repairFailed == 0 && deferredToOtherSubos == 0`，其中 `deferredToOtherSubos` 只统计
**别的空间拥有的 broken payload**（即 §4.2 的第二条分支）。

只统计这一类，不统计 `xvm-subos-active-missing`：后者 `--fix` 修不了，计入会让标记
永远落不下，重演 gcc-runtime 那次"提示永不消失"。

盖章条件与顺序无关：在 A 修完 A，再去 B 修完 B 时，A 已经干净 → 标记在 B 落下。

### 4.5 引用计数取并集（D5）

`workspace_config_paths_for_scope_` 改为始终返回 global subos 路径 ∪ project subos
路径（有 project config 时），去重。

方向是**保守**的：并集只会让 payload 被保留得更多，永远不会多删。代价是一个陈旧的
project subos 文件可能长期钉住一份 payload——`xlings self clean` 的 GC 是这条路的
出口，且它本来就用的是另一套枚举（`profile::collect_subos_references_`，已经是全 home）。

---

## 5. 明确不做

- **doctor `--all-subos`**：一个进程内切换 subos 需要重建 Config 单例的路径解析、
  重新加载 workspace、并对每个空间重跑 shim 层检查。收益是省几次命令，代价是给
  doctor 引入一个它今天没有的可变全局状态。§4.2 的归属报告已经消除了"不知道去哪修"
  这个真正的死路。
- **自动跨空间修复**：在 A 里替 B 修，意味着替 B 决定它的 workspace 应该长什么样。
  这是 `xlings use` 的语义，不是 `doctor` 的。
- **`installed[]` 的自动清理**：悬挂项报告为 Notice。清理它等于替用户决定"这个空间
  不再想要这个版本"，而它可能只是等着被重新安装。

---

## 6. 验收

不是"命令退出 0"。逐条对应上面的缺陷：

| # | 断言 | 现在 | 之后 |
|---|---|---|---|
| A1 | 两空间共用 gcc，B 独占 node；在 A 里 `--fix` 后 A 的 workspace 是否含 node | 含（泄漏） | **不含** |
| A2 | 同上场景，A 的报告是否指出 node 属于 B | 否 | **是，带 `subos use B` 命令** |
| A3 | R3 的 remove 只 detach 时的输出 | `REMOVED but could not reinstall` | **不含 REMOVED，指出被哪个空间引用** |
| A4 | 在 A 修完（B 仍有待修）后 `~/.xlings.json:version` | 已盖章 | **未盖章** |
| A5 | B 修完后 | — | **盖章** |
| A6 | 单 subos home（含 legacy 空 `installed[]`）的 `--fix` 行为 | 修复 56 条 | **完全不变** |
| A7 | global 包被 project subos 引用时的 `remove` | payload 被删 | **payload 保留** |
| A8 | 空间 B 的 active 指向未注册版本，在 A 里跑 doctor | 无输出 | **报告 + 切换命令** |

A6 是回归闸门，用 `tests/e2e/self_doctor_test.sh` 原样保证（不修改该文件）。

---

## 7. 实施拆分

| # | 步骤 | 产出 | 门槛 |
|---|---|---|---|
| S1 | `profile::load_subos_snapshots` + `find_subos_referencing` 基于它重写 | `profile.cppm` | 单测：临时目录三个空间，含损坏 JSON |
| S2 | `inspect_subos_references` + `subos_ownership`（纯函数） | `xvm/inspect.cppm` | 单测：A8、归属三分支 |
| S3 | doctor 集成归属判据 + 新 finding 输出 | `xself/doctor.cppm` | e2e A1/A2/A8 |
| S4 | R3 复检回调 | `xself/repair.cppm` + doctor 注入 | 单测（假 runner + 假 verifier）+ e2e A3 |
| S5 | 迁移标记门槛收紧 | `xself/doctor.cppm` | e2e A4/A5 |
| S6 | 引用计数取并集 | `xim/installer.cppm` | e2e A7 |
| S7 | e2e `self_doctor_multi_subos_test.sh` + 接入 `run_all.sh` / CI | tests | 全绿 |
| S8 | 版本号 → PR → CI → release → 生态验证 | — | 四项发布后校验（见 §8） |

## 8. 发布后校验（不可省）

按 `project-release-verification-traps` 的四项，逐项用能证伪的方法：

1. `gh release view v<ver> --json assets` → 期望 **8** 个资产；
2. 自己下载并 `sha256sum`，与 sidecar **和** 索引 recipe 两边比对；
3. 读 `xim-pkgindex` main 上 `pkgs/x/xlings.lua` 的**三个**平台表 + `latest`；
   `bump-index` 只开 PR 不合并，必须检查是否有未合并的 PR；
4. CN 镜像用 **GET**（HEAD 返回 401）并完整下载一个资产做哈希。
