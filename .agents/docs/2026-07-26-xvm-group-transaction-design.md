# XVM Group Transaction — 架构级设计方案

**日期**: 2026-07-26
**类型**: 设计 (design)
**关联**: `2026-07-26-xvm-pr384-review.md`(评审与场景对照)、
`2026-07-26-xvm-provider-binding-group-design.md`(PR #384 自带设计，本文继承其数据模型判断)、
`2026-07-26-xvm-group-transaction-plan.md`(实施计划与验收标准)、
[openxlings/xlings#384](https://github.com/openxlings/xlings/pull/384)

---

## 0. TL;DR

PR #384 已经把**身份问题**解决对了：`(provider, providerVersion, group)` 是正确的所有权单位。
本设计不推翻它，而是补上它没解决、且是真正故障来源的三件事：

> **① 状态分层** —— Catalog / Selection / Materialization 三层，层间是单向函数关系。
> **② 提交点收敛到一次 rename** —— 文件系统不再参与事务，改为幂等收敛（reconcile）。
> **③ 恢复即收敛** —— `doctor --fix` 不是特例修复逻辑，就是重跑 reconcile。

核心洞察：

**不要试图让文件系统操作变成事务。让物化成为持久化状态的纯函数，然后"重建"就取代了"回滚"。**

```
Catalog  (全局, 只增)      ── 装了什么      ← install / remove 写
   │
   ├─► Selection (每 subos) ── 选了什么      ← use / install / remove 写   ★ 唯一提交点
   │      │
   │      ▼
   └─► Materialization      ── 磁盘上长什么样  ← 由上两层完全决定，可随时重建
       (bin/ shim, usr/include, usr/lib)
```

`Materialization = f(Selection, Catalog)` 一旦成立：崩溃、断电、部分失败都不需要回滚，
只需要在下一条命令（或 doctor）里重新求值一次 `f`。

---

## 1. 问题定位：PR #384 解决了什么、没解决什么

| 故障类别 | PR #384 | 本设计 |
|---|---|---|
| 身份缺失（gcc@15 与 g++@15 是否同源） | ✅ 已解决 | 继承 |
| 悬空边 / 幻影版本 | ✅ 已解决（精确删除 + 双向边清理） | 继承 |
| 删除权限过宽 | ✅ 已解决（provider-scoped exact removal） | 继承 |
| 元数据校验 | ✅ 已解决（fail-closed + JSON Pointer） | 继承并**去掉冗余不变量** |
| **物化不在事务内** | ❌ 明确列为 out of scope | ★ 本设计核心 |
| **多入口一致性（`use` 侧）** | ❌ `cmd_use` 未接入新 resolver | ★ 本设计核心 |
| **崩溃/并发安全** | ❌ 无锁、写文件不原子 | ★ 本设计核心 |
| **坏状态可恢复** | ❌ fail-closed 无出口 | ★ 本设计核心 |

一句话：**PR #384 修好了"记录"，本设计修"记录与现实之间的落差"。**

### 1.1 现状的三个硬事实（代码证据）

```
src/platform.cppm:262   write_string_to_file = fopen("w") + fwrite + fclose
                        → 无 temp+rename、无 fsync
                        → ~/.xlings.json 写到一半断电 = 整个版本库丢失
```

```
git grep -n "lock" -- src/core/xvm/     → 空
                        → install / remove / use 之间零互斥
                        → 并发丢更新在 PR #384 之后从"状态过时"升级为"工具链不可用"
```

```
src/core/config.cppm:1021 save_versions()   → ~/.xlings.json      (读-改-写整个文件)
src/core/config.cppm:1049 save_workspace()  → subos/.xlings.json
                        → 两次独立写，无 journal，无共同提交点
```

**这三条比 binding group 本身更致命，且都不在 PR #384 的范围内。**

### 1.2 一个已存在但未接线的资产

`src/core/profile.cppm` 已实现完整的 generation 机制：
`commit(envDir, packages, reason)` / `list_generations()` / `rollback(envDir, gen)`，
落盘为 `<subos>/generations/NNN.json` + `.profile.json`。
`config.cppm:1061` 与 `subos.cppm:407` 已经在建这个目录。

**但 xvm 完全没有使用它** —— `git grep generations -- src/core/xvm/` 为空。

本设计**复用这套 generation 编号与目录**作为 Selection 的历史，而不是另造一套（§4.4）。

---

## 2. 数据模型：group 提升为顶层表

### 2.1 目标形态

```jsonc
{
  "versions": {
    "gcc":       { "type": "program", "versions": {
        "15.1.0": { "path": "...", "kind": "program", "group": "pkgindex:gcc@15.1.0/gcc" } } },
    "g++":       { "type": "program", "versions": {
        "15.1.0": { "path": "...", "kind": "program", "group": "pkgindex:gcc@15.1.0/gcc" } } },
    "libstdc++": { "type": "lib",     "versions": {
        "gcc-15.1.0": { "path": "...", "kind": "lib",  "group": "pkgindex:gcc@15.1.0/gcc" } } }
  },

  "groups": {
    "pkgindex:gcc@15.1.0/gcc": {
      "provider":        "pkgindex:gcc",
      "providerVersion": "15.1.0",
      "name":            "gcc",
      "members": { "gcc": "15.1.0", "g++": "15.1.0", "libstdc++": "gcc-15.1.0" },
      "headers": [ { "sourceDir": "xpkgs/gcc/15.1.0/include", "destinationPrefix": "" } ],
      "libs":    [ { "sourceDir": "xpkgs/gcc/15.1.0/lib64",   "destinationPrefix": "" } ]
    }
  }
}
```

group key = `"<provider>@<providerVersion>/<name>"`，即 PR #384 的 `BindingGroupRef` 三元组
的规范字符串形式。**身份没变，只是从"复制到每个成员 + 指定一个 root 存 manifest"改成
"存一份，成员持外键"。**

### 2.2 直接收益（可量化）

| PR #384 中的构造 | 目标形态 |
|---|---|
| `BindingErrorKind::RootReferenceMismatch` | **消失**（无 root 概念） |
| `BindingErrorKind::RootMissingFromManifest` | **消失** |
| `BindingErrorKind::MemberReferenceMismatch` | **消失**（外键单向，无反向引用可失配） |
| `BindingErrorKind::StartMemberMissing` | **消失**（`members.contains(target)` 即答案） |
| `BindingErrorKind::GroupIdentityMismatch` | **消失** |
| `bindings.cppm:183-195 non_root_metadata_error_()` | **删除** |
| commit `c51ee19 allow complete binding root migration` | **删除**（root 不存在，无需迁移） |
| `resolve_provider_group_()` 105 行校验遍历 | 退化为一次 map 查找 + 成员存在性检查 |
| `VInfo::bindings` 星形边双写 | 仅作兼容读，写侧删除 |

`bindings.cppm` 预计从 539 行降到 ~180 行，`BindingErrorKind` 从 15 个降到 ~7 个。

### 2.3 为什么这是正确的归一化

当前形态把 group 的属性（成员表、头文件表）存在**恰好是成员之一**的那条记录上。这制造了三类
本不该存在的问题：

1. **自指不变量** —— root 必须指向自己，且必须出现在自己的 manifest 里（两条独立校验）
2. **成员反向引用** —— 每个成员都复制一份完整 `BindingGroupRef`，5 个字段全都可能失配
3. **删除 root 的特殊性** —— 删掉 root 就删掉了整组的 manifest，需要 root 迁移逻辑

顶层表让 group 成为一等实体：可独立创建、查询、删除、引用计数。这是 rpm（`Packages` 表 +
`Basenames` 索引）、dpkg（`status` 中 package 与 `Source:` 字段分离）、Nix（derivation 与
output path 分离）的共同做法。

### 2.4 兼容与迁移

三种输入形态并存，**读三写一**：

| 形态 | 来源 | 读 | 写 |
|---|---|---|---|
| `groups` 顶层表 | 本设计 | ✅ | ✅ |
| root 承载 `bindingMembers`/`bindingGroup` | PR #384 | ✅ 归一化为 group | ❌ |
| `VInfo::bindings` 成对边 | ≤ 0.4.69 | ✅ 经校验后归一化为 group | ❌ |

归一化在 `versions_from_json` 之后一次性完成，进入内存后**只有一种表示**。
下游（resolver / registration / removal / use）只认 `groups`。

> **重要**（依 plan 决议 Q5 —— 单一 `release/0.4.70` 集成分支）：
> P0–P4 全部在集成分支上完成，main 从不承载中间态。因此 PR #384 的 root-manifest 形态
> **既未发布给用户，也从未进入 main 的任何提交**。
>
> 上表中间那一行只服务于已在用 `fix/xvm-binding-groups` 分支的开发者，
> **在合入 main 前的 R.1 阶段直接删除，无需任何弃用周期**。
>
> 对外发布的 0.4.70 只需兼容一种历史格式：**≤ 0.4.69 的 legacy 成对边**。

---

## 3. 状态分层

### 3.1 三层定义

| 层 | 存储 | 语义 | 谁写 | 幂等 |
|---|---|---|---|---|
| **Catalog** | `~/.xlings.json` → `versions` + `groups` | 这台机器上**存在**哪些 payload | install / remove | 否（只增/只减） |
| **Selection** | subos 运行时状态文件 → `active` + `installed` + `generation` + `reconciled` | 这个 subos **选中**了什么 | use / install / remove | 否 |
| **Materialization** | `<subos>/bin/*`、`<subos>/usr/include/*`、`<subos>/usr/lib/*` | 磁盘上的实际视图 | reconciler | ✅ **是** |

**Selection 的落盘位置**（依 plan 决议 Q2）：

| 模式 | 路径 | 是否 user-authored |
|---|---|---|
| global | `<home>/subos/<name>/.xlings.json` | 否 |
| project (Named / Anonymous) | `Config::project_state_path()` → `<project>/.xlings/` | 否 |
| project manifest（意图声明） | `<project>/.xlings.json` | ✅ **是 —— 永不写入运行时字段** |

统一规则：**`generation` / `reconciled` / ledger 属于 subos 运行时状态，永不写入任何
user-authored 文件。** 这与 `types.cppm:119-134` 已确立的"project 文件声明意图、
不承载运行时状态"原则一致 —— 否则每次 `xlings use` 都会在用户的 git 仓库里制造
噪音与合并冲突。

### 3.2 不变量

```
INV-1  Selection.active[t] = v  ⟹  Catalog.versions[t].versions 包含 v
INV-2  Selection.active 中同属一个 group 的成员，要么全部 active 于同一 group，要么全部不 active
INV-3  Materialization ≡ reconcile(Selection, Catalog)      —— 可在任意时刻重新求值验证
INV-4  Catalog 中每个 group 的每个 member 都必须存在于 Catalog.versions
```

**INV-2 是整个项目的核心不变量。** PR #384 在注册与删除侧维护了它，但 `use` 侧和删除回退
（评审 §3.3）尚未维护。

**INV-3 是本设计的新增不变量，也是可恢复性的全部来源。**

### 3.3 Materialization Ledger

reconciler 需要知道"`usr/include/c++/15.1.0` 这个文件是谁放的"，否则无法安全清理。

PR #384 的 `types.cppm:100-117` 已经定义了 `MaterializedOwner` / `MaterializedEntry` /
`MaterializedLedger`，**但没有任何生产代码使用它**。本设计启用它：

```jsonc
// <subos>/.xlings-materialized.json
{
  "generation": 42,
  "entries": {
    "usr/include/c++/15.1.0": { "kind": "header", "group": "pkgindex:gcc@15.1.0/gcc" },
    "usr/lib/libstdc++.so.6": { "kind": "lib",    "group": "pkgindex:gcc@15.1.0/gcc" },
    "bin/g++":                { "kind": "shim",   "group": "pkgindex:gcc@15.1.0/gcc",
                                "member": "g++",  "version": "15.1.0" }
  }
}
```

ledger 是**派生数据**（可由扫描 + Selection 重建），存盘只是为了避免每次全量扫描 sysroot。
损坏时的处理是重建，不是报错 —— 与 `bindingIntegrityIssues` 的处理方式形成对照（§6.2）。

---

## 4. 提交协议：commit-then-reconcile

### 4.1 核心流程

```
xlings use gcc 15.1.0
  │
  ├─ 1. acquire per-home lock                       ← §5
  ├─ 2. 锁内重新加载 Catalog + Selection            ← 防丢更新
  ├─ 3. resolve group(gcc@15.1.0) → 成员全集         ← 纯函数，O(1) 查表
  ├─ 4. 计算 nextSelection（内存）                   ← 纯函数
  ├─ 5. preflight: 每个成员的 payload / header / lib 源存在且可读
  │        └─ 任一失败 → 报错退出，磁盘零改动
  │
  ├─ 6. ★ 提交点：write_file_atomic(<subos>/.xlings.json, nextSelection)
  │        generation = N+1, "reconciled": false
  │        └─ 失败 → 报错退出，磁盘零改动（旧文件完好）
  │
  ├─ 7. reconcile(Selection, Catalog)                ← 幂等收敛
  │        ├─ diff ledger 与目标物化集合
  │        ├─ 删除不再需要的条目
  │        ├─ 创建缺失的条目
  │        └─ 更新 ledger
  │
  ├─ 8. 标记 "reconciled": true（非关键写，失败无害）
  └─ 9. release lock
```

### 4.2 为什么提交点可以只是一次 rename

传统做法（PR 设计文档 §6.2 所描述的）是：暂存所有文件 → 备份 → 替换 → 提交 JSON → 删备份。
问题是它需要在 N 个独立路径上维持事务语义，而 POSIX 不提供跨路径的原子替换。

本设计的做法是**把"要做什么"提交下去，而不是把"做完的结果"提交下去**：

- 第 6 步之后，系统的**意图**已经持久化且原子
- 第 7 步只是让现实追上意图，它**幂等且可重入**
- 第 7 步中途崩溃 → `reconciled: false` 留在盘上 → 下一条任何 xvm 命令看到它就先重跑第 7 步

这是 Nix profile activation、Kubernetes controller、Terraform apply 的共同模式：
**声明式状态 + 收敛循环**，而不是命令式事务 + 回滚。

### 4.3 reconcile 的幂等性要求

```cpp
// 伪代码
void reconcile(const Selection& sel, const Catalog& cat, Ledger& ledger) {
    auto desired = compute_desired_materialization(sel, cat);   // 纯函数
    auto current = ledger.entries;

    for (auto& [path, entry] : current)
        if (!desired.contains(path)) remove_entry(path, entry);

    for (auto& [path, entry] : desired)
        if (!current.contains(path) || current[path] != entry) install_entry(path, entry);

    ledger.entries = desired;
    ledger.generation = sel.generation;
}
```

要求：

- `compute_desired_materialization` 必须是纯函数（无 IO、无时间、无随机）
- `install_entry` / `remove_entry` 必须容忍"目标已处于期望状态"
- reconcile 全过程不读 `desired` 之外的任何状态 → 可在单元测试中用内存 FS 全覆盖

**禁止静默 clobber**（依 plan 决议 Q3）：现有 `commands.cppm` 的
`install_headers` / `install_libdir` 对已存在目标做**无条件 `fs::remove_all(target)`** ——
这正是一个 group 的头文件被另一个 group 静默覆盖的机制。`install_entry` 必须改为：

```
目标已存在
  ├── ledger 记录属主 == 本次 desired 的属主  → 幂等跳过（或按内容差异更新）
  ├── ledger 记录属主 == 其它 group          → 先走 remove_entry 显式移除，再创建
  └── 不在 ledger 中（unmanaged）            → 报告为 unmanaged，不删（设计 D5）
```

**链接机制不变**（`create_link_`：POSIX symlink / Windows 目录 junction、文件
hardlink→copy 回退）。但由于 Windows 的 copy 回退在文件系统上不留回指信息，
**ledger 必须是权威存储而非可从 FS 反推的缓存** —— FS 扫描只用于发现 unmanaged 条目。
ledger 条目粒度 = `includedir` 的**顶层条目**，与 `install_headers` 现有循环逐项对齐。

### 4.4 与 `profile.cppm` generation 的关系

复用 `profile::commit()` 的编号与 `<subos>/generations/NNN.json` 目录，把 Selection 的每次
提交记为一个 generation。收益：

- `xlings profile rollback <n>` 立刻对 xvm 生效（当前它对 xvm 完全无效）
- 用户有了"撤销上一次 use"的能力，这是坏状态的**第二条**出路
- 不必新造历史机制

需要做的改造：`profile::Generation::packages` 当前是 `map<name, version>`，需扩展为携带
group key，否则 rollback 回去的 Selection 无法保证 INV-2。

---

## 5. 并发：per-home advisory lock

```
<home>/.xlings.lock        ← flock(LOCK_EX) / LockFileEx
```

- 作用域：**per-home**，不是 per-subos —— 因为 Catalog 是全局的
- 持有者：`install` / `remove` / `use` / `doctor --fix` 全部命令
- 不持有者：`list` / `show` / shim dispatch（只读，接受读到略旧的状态）
- 超时：默认 30s，超时后报"另一个 xlings 进程正在操作，PID <n>"
- stale 检测：锁文件写入 PID + 启动时间；持有者进程不存在则强夺

**为什么不能等 Task 4**：PR #384 之后，两个并发 `install` 的丢更新后果从"某个包的版本记录
过时"变成"group manifest 指向已被另一进程删掉的成员 → fail-closed → 整条工具链不可用"。
**严重度被放大了，锁必须与强校验同批。**

---

## 6. 恢复：doctor 即 reconciler

### 6.1 三级恢复

```console
$ xlings self doctor
# L1 只读检查
  ✓ catalog integrity            (INV-1, INV-4)
  ✗ selection coherence          (INV-2)  gcc@15.1.0 active 但 g++ active 于 musl@1.2.5
  ✗ materialization drift        (INV-3)  usr/include/c++/16.1.0 无主

$ xlings self doctor --fix
# L2 收敛（不丢数据）
  ✓ rematerialize                 → 重跑 reconcile，消除 INV-3 偏差
  ✓ coherence repair              → 把不一致的 group 整组停用（保守），提示用户重新 use

$ xlings self doctor --fix --reset-metadata
# L3 破坏性（需显式 flag）
  ✓ drop unparseable group metadata → 降级为 legacy singleton，payload 保留
  ✓ rebuild ledger from filesystem scan
```

**L2 覆盖绝大多数场景，且不需要任何特殊逻辑 —— 它就是第 4.1 步的第 7 步。**
这是状态分层最大的工程红利：**恢复代码 = 正常路径代码。**

### 6.2 `bindingIntegrityIssues` 改为非持久

评审 §3.4：当前 `vdata_to_json` 把解析期发现的问题写回磁盘，把派生数据固化成了状态。

改为：

- issue 只存在于内存（`VData` 字段保留，序列化时跳过）
- `doctor` 负责报告
- `doctor --fix --reset-metadata` 负责清除

对照：`MaterializedLedger` 损坏 → 重建；`bindingIntegrityIssues` → 报告 + 可清除。
**两者都不应该是"永久污点"。**

---

## 7. 错误呈现契约

fail-closed 只有配上可执行的错误信息才成立。定义统一的用户级错误结构：

```cpp
struct XvmUserError {
    std::string_view code;      // "group-member-missing" —— 稳定、可搜索、可文档化
    std::string      what;      // 一句话说明发生了什么
    std::string      provider;  // pkgindex:gcc@15.1.0
    std::string      target;    // libstdc++
    std::string      version;   // gcc-15.1.0
    std::string      path;      // JSON Pointer(如适用)
    std::string      hint;      // 一条可执行的出口
};
```

渲染契约：

```
✗ <what>
  code:     <code>
  provider: <provider>
  at:       <target>@<version> [<path>]
  hint:     <hint>
  nothing was changed          ← 仅当确实零改动时输出，且必须是真的
```

强制要求：

1. **`install` / `use` / `remove` 的失败路径不得只走 `log::warn` + `return false`**
   （当前 `installer.cppm:1330-1365` 就是如此）
2. 每个 `RegistrationErrorKind` / `RemovalErrorKind` / `BindingErrorKind` 必须映射到一个
   `code` 和一条 `hint`，**无 hint 不得合入**
3. `nothing was changed` 是承诺，不是修辞 —— 有对应测试

---

## 8. 明确的非目标

- **不做跨路径原子文件系统** —— 用 commit-then-reconcile 规避，不用 generation 化 sysroot
- **不重写 shim dispatch** —— `2026-06-04-shim-owner-anchoring-design.md` 的 owner anchoring
  保持不变；shim 只是 Materialization 的一种条目
- **不改 xpkg / libxpkg 的 recipe DSL** —— `xvm.add` / `binding` / `headers` 语法不变，
  只在 installer 侧改归一化
- **不做跨 subos 的全局引用计数 GC** —— 该机制**已经存在且粒度正确**：
  `installer.cppm:1079-1115` 的 `is_version_referenced_anywhere_()` 扫描所有 subos 的
  `.xlings.json`，同时检查 `active` 与 `installed[]`。其 `(target, version)` 粒度与
  payload 的实际存放粒度（`xpkgs/<name>/<ver>/`）一致，**不应改成 group 粒度**。
  分层上：引用计数属 Catalog（payload 该不该删），reconcile 属 Materialization
  （本 subos 该长什么样），两者不耦合。P3 只需补一条"subos A 的 reconcile 不修改
  subos B 任何路径"的隔离测试
- **不引入 SQLite 或其他嵌入式数据库** —— 生态规模（数十到数百条记录）远未到需要它的量级，
  JSON + 原子写足够

---

## 9. 与 PR #384 的关系

### 9.1 保留

- `BindingGroupRef` 三元组身份定义（`types.cppm:7-15`）
- provider = `PlanNode.canonicalName`、providerVersion = `PlanNode.version` 的取值来源
- 两阶段注册（全量校验 → 一次应用）与 shadow-copy 语义
- 精确删除、双向边清理、`resolve_exact_version_key` 的歧义拒绝
- JSON Pointer 错误路径
- 137 个失败路径测试（绝大多数在归一化后仍然适用，需改造断言而非删除）

### 9.2 替换

| PR #384 | 替换为 |
|---|---|
| root 承载 manifest + 成员反向引用 | `groups` 顶层表 + 成员外键（§2） |
| `VInfo::bindings` 星形边双写 | 仅兼容读 |
| `bindingIntegrityIssues` 持久化 | 内存态 + doctor 报告（§6.2） |
| 逐 target `installed[].rbegin()` 回退 | 整组一致回退或整组停用（INV-2） |
| FS 副作用 `log::warn ... continue` | reconcile（§4） |

### 9.3 补上

- `write_file_atomic`（§1.1）
- per-home lock（§5）
- Materialization ledger + reconciler（§3.3、§4）
- `cmd_use` 接入 group resolver（这是 PR #384 显式声明的 Draft 边界）
- doctor 三级恢复（§6）
- 用户级错误契约（§7）

---

## 10. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 归一化改动面大，可能引入新回归 | 分两步：先加 `groups` 表并双写、验证等价后再删旧写路径（计划 P2） |
| reconcile 误删用户手放在 sysroot 的文件 | ledger 只管理自己创建的条目；未知条目**报告但不删**，`--prune` 才删 |
| 官方索引中存在会被新校验拒绝的 recipe | 发布前跑全索引 dry-run 审计（计划 P4.1），这是硬门禁 |
| 开发者已在 PR #384 分支上产生了 root-manifest 格式数据 | 集成分支期间保留兼容读，**合入 main 前的 R.1 阶段删除**；该格式不进入 main 也不进入任何发布（§2.4） |
| 集成分支长期存在导致与 main 漂移 | 每周 `git merge origin/main` 进集成分支（不 rebase）；漂移冲突就地解决（plan §0.1） |
| 锁引入死锁或误报"另一进程在跑" | stale PID 检测 + 30s 超时 + `XLINGS_NO_LOCK=1` 逃生阀（仅诊断用） |
| 一次性交付过大 | 计划按 P0–P4 分 5 批，每批独立可发布（见 plan 文档） |

---

## 11. 设计决策记录

| # | 决策 | 备选 | 理由 |
|---|---|---|---|
| D1 | group 顶层表 | 保留 root manifest | 消除 5 类错误 + root 迁移逻辑；错误分类表本身即证据（§2.2） |
| D2 | commit-then-reconcile | stage/backup/replace/cleanup | POSIX 无跨路径原子替换；收敛模式让恢复代码 = 正常代码（§4.2） |
| D3 | 复用 `profile.cppm` generation | 新造 xvm 历史机制 | 该机制已存在且已建目录，只是没接线；顺带让 rollback 对 xvm 生效（§4.4） |
| D4 | per-home 锁而非 per-subos | per-subos 锁 | Catalog 是全局的，per-subos 锁保护不住它 |
| D5 | ledger 未知条目报告不删 | 全权管理 sysroot | 用户可能手动放文件；静默删除是不可接受的破坏性行为 |
| D6 | 不引入 SQLite | 嵌入式 DB | 数据量差 3–4 个数量级；JSON + 原子写足够，且保持可手工检查 |
| D7 | `bindingIntegrityIssues` 不落盘 | 保持 PR #384 行为 | 派生数据持久化会把瞬时问题固化成永久状态（§6.2） |
