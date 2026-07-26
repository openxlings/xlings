# XVM Group Transaction — 实施计划与验收标准

**日期**: 2026-07-26
**类型**: 计划 (plan)
**架构**: `2026-07-26-xvm-group-transaction-design.md`
**评审依据**: `2026-07-26-xvm-pr384-review.md`
**前置 PR**: [openxlings/xlings#384](https://github.com/openxlings/xlings/pull/384)（Draft）
**涉及仓库**: `openxlings/xlings`、`openxlings/libxpkg`、`openxlings/xim-pkgindex`

---

## 0. 目标与分批策略

**总目标**：让 `xlings use / install / remove` 在任意入口、任意失败点上，都只产生两种结果 ——
**整组正确**，或**磁盘零改动 + 具名原因 + 可执行出口**。

分 5 批推进，**全部汇入单一 `release/0.4.70` 集成分支**，统一验证后一次性合入 main 发布：

| 批次 | 主题 | 汇入目标 | 独立价值 | 阻塞关系 |
|---|---|---|---|---|
| **P0** | 修 PR #384 的合并阻塞项 | `release/0.4.70` | 消除功能倒退 | 无 |
| **P1** | 崩溃与并发安全底座 | `release/0.4.70` | 修比 binding group 更严重的数据丢失风险 | 与 P0 并行 |
| **P2** | group 归一化 | `release/0.4.70` | 删掉 1/3 不变量表面，为 P3 铺路 | P0, P1 |
| **P3** | 物化收敛（commit-then-reconcile） | `release/0.4.70` | ★ 真正兑现"要么整组正确要么零改动" | P2 |
| **P4** | 生态验证 | `release/0.4.70` | 真实 GCC 15/16 端到端证明 | P3 |
| **R** | **集成验证 → 合入 main → 发布** | `main` | **唯一对外版本 `0.4.70`** | P0–P4 全部 |

> **排序原则 1 —— 强校验与恢复手段不得分批发布。**
> #384 已落地的 fail-closed 与其恢复手段（P0.6 / P1.3）之间不允许存在发布窗口，
> 否则用户拿到的是评审 §5.3 的"死路"场景。
>
> **排序原则 2 —— main 永不承载中间态（决议 Q5，见 §5）。**
> 多 PR 允许且鼓励（保持每个 PR 可审阅），但**全部以 `release/0.4.70` 为 base**，
> main 只在 P0–P4 全部完成且集成验证通过后接收一次合并。
>
> **这一模型的直接收益**：PR #384 的 root-manifest 格式、P0 的
> `includedir` 临时双写、P2 之前的过渡代码，**都不会出现在 main 的任何提交上**，
> 更不会进入任何 tag。设计 §2.4 的三种兼容读因此可以在合入 main 前**降为两种** ——
> 不是"保留一个弃用周期"，而是根本不存在。

---

## 0.1 分支与 PR 策略

### 分支拓扑

```
origin/main ──┬─────────────────────────────────────────────► (R) merge ──► tag v0.4.70
              │                                                    ▲
              └─► release/0.4.70 ──┬── PR: 本套设计文档            │
                  (集成分支)        ├── PR #384 基础层(原样合入)    │
                                    ├── PR: P0.1 header/lib 切换   │
                                    ├── PR: P0.2 header group      │
                                    ├── PR: P0.3 回退保守化        │
                                    ├── PR: P0.4 用户级错误        │
                                    ├── PR: P0.6 doctor L1         │
                                    ├── PR: P1.1 atomic write      │
                                    ├── PR: P1.2 per-home lock     │
                                    ├── PR: P1.3 doctor L2/L3      │
                                    ├── PR: P2.1 groups 表         │
                                    ├── PR: P2.2 resolver 简化     │
                                    ├── PR: P3.1 ledger            │
                                    ├── PR: P3.2 reconciler        │
                                    ├── PR: P3.3 cmd_use           │
                                    └── PR: P4.2 E2E ──────────────┘
```

> **#384 原样合入，P0 作为后续独立 PR** —— 集成分支不面向用户，所以没有"先修好再合"的
> 必要；原样合入能立刻止住 10k 行 PR 的腐化并解锁 P1 并行。评审 §3.1 / §3.2 的阻塞判定
> 针对的是**合入 main**，不是合入集成分支（见评审 §7 的判据修订）。
> 唯一的硬前提是 **#384 自身 CI 六平台全绿**。

### 规则

| 项 | 规则 |
|---|---|
| 集成分支 | `release/0.4.70`，从 `origin/main` 切出，**不 force-push** |
| PR base | 一律 `release/0.4.70`；**任何 PR 不得直接 target main** |
| PR 粒度 | 一个 PR ≤ 一个子任务（P1.1 / P2.2 …） |
| **PR 合入方式** | **squash merge** —— 一个子任务在集成分支上恰好一个提交，历史线性可读 |
| PR CI 门禁 | 六个 workflow（linux / linux-root / macos / windows / aarch64 / e2e）**全绿**才合入 |
| 版本号 | `core/config.cppm` 的版本在 **R.2 一次性**改为 `0.4.70`；各 PR 不动版本号 |
| main 同步 | 每周或 main 有提交时，`git merge origin/main` 进集成分支（**merge 不 rebase**） |
| 合入 main | 单个 PR，**merge commit 不 squash**（保留 P0–P4 每任务一提交的分批历史） |
| 发布 | 合入 main 后手动触发 `release.yml`（`workflow_dispatch`） |

> **为什么 PR 用 squash、合入 main 用 merge**：squash 让集成分支上"一个任务 = 一个提交"，
> 约 10 个提交即覆盖 P0–R；合入 main 时用 merge commit 把这 10 个提交完整带过去，
> 既避免开发过程的噪音提交进入 main，又保留了可追溯的分批结构。

### 长期分支的两个已知风险与对策

| 风险 | 对策 |
|---|---|
| 集成分支与 main 漂移（main 可能合入无关修复） | 强制每周 `merge origin/main`；漂移冲突在集成分支解决，不推回 main |
| 跨仓依赖：P4.1 的 pkgindex 改动在另一个仓 | pkgindex 侧先合并并取得 commit sha → 在集成分支上同步更新 **6 个 workflow 的 `XIM_PKGINDEX_REF`**（见 memory `project_ci_index_ref_pin.md`），R 阶段前必须完成 |

---

## 0.2 用户无感升级契约（一等约束）

0.4.70 是一次涉及**状态格式 + 失败语义 + 物化机制**的三重改动。对外唯一可接受的体验是：
**用户执行 `xlings self update` 之后，什么都不用做，一切照常。**

### 硬性契约（违反任一条即不可发布）

| # | 契约 | 实现依赖 | 验收 |
|---|---|---|---|
| U1 | `xlings self update` 0.4.69→0.4.70 无需任何手工步骤 | — | A16 |
| U2 | 旧 `~/.xlings.json`（legacy 成对边）**首次读取时自动归一化**为 groups 表，用户不感知 | P2.1 | A12 |
| U3 | 已有 shim 继续工作，**不需要重建** | shim 走 workspace 指针，格式无关 | A16 |
| U4 | 已有 subos / project 配置继续工作 | P2.1 归一化覆盖两种 scope | A16 |
| U5 | 升级前处于激活状态的工具链，升级后**仍然激活且版本不变** | P2.1 + P3.3 | A16 |
| U6 | 升级后首次命令若发现物化漂移，**自动 reconcile 而非报错** | P3.2 | A17 |
| U7 | **降级安全**：回滚到 0.4.69 后，binding 关系仍然有效 | 见下 | A18 |

### U6 是最容易被 fail-closed 破坏的一条

升级瞬间，用户的 sysroot 是 0.4.69 留下的、没有 ledger 记录的状态。若 P3.2 的 reconciler
把"不在 ledger 中"一律当作 unmanaged 而拒绝接管，**每个老用户升级后第一条命令都会撞上
"物化漂移"**。

> **决议**：首次升级时（检测到无 ledger 文件）执行一次
> **adoption pass** —— 依据 Selection + Catalog 反推出 desired 集合，
> 对已存在且内容匹配的路径**认领进 ledger**（而非报告为 unmanaged），
> 只有真正无法归属的才标记 unmanaged。
> 这是"报告不删"（设计 D5）的一个明确例外，仅在 ledger 缺失时触发一次。

### U7 降级安全 —— 新发现的问题

设计 R.1 原计划删除 legacy 成对边的**写**路径。但这会造成：

```
用户 0.4.69 ──升级──► 0.4.70（写 groups 表，不写成对边）
                         │
                         └──回滚──► 0.4.69
                                     └─► 读不懂 groups 表，成对边又已消失
                                         → binding 关系全部丢失
                                         → gcc/g++ 静默错乱重现（且比升级前更糟）
```

0.4.69 的 `versions_from_json` 不认识顶层 `groups` 键，会静默忽略；而成对边一旦停写，
它就认为所有包都没有 binding。**这是一次不可逆升级，且失败模式是静默的。**

> **决议：0.4.70 继续双写 legacy 成对边，0.4.71 才停写。**
>
> - 成本极低：星形边写入代码已存在（`registration.cppm:777-786`），保留即可
> - 收益：`xlings self update` 的回滚路径同样无感
> - 因此 **R.1 只删除 root-manifest 兼容读与 `includedir` 临时双写，
>   不删除成对边写路径**（原 R.1 第 2 条相应收窄）

---

## P-1 — 集成分支的 CI 前置条件（计划外，实施时发现）

> 计划初稿假定"PR 汇入集成分支即受 CI 门禁保护"。实测不成立，补记为 P-1。

### P-1.1 六个 workflow 未覆盖 `release/**` ✅ 已修（PR #386）

六个 workflow 的 `push` 与 `pull_request` 都只写了 `branches: [main, master]`，
因此 **base 为 `release/0.4.70` 的 PR 完全不跑 CI**（#385 实测 `no checks reported`）。
这让整个集成分支模型失去意义 —— 工作只会在抵达 main 时才被验证。

已加 `release/**` 到六个 workflow。实测生效：改动推上去后六个 check 立即开始运行。

### P-1.2 CI 每次运行都丢弃 mcpp payload 缓存 ✅ 已修（PR #386）

每个 workflow 先恢复 ~800MB 的 `~/.mcpp` 缓存，紧接着执行
`rm -rf "$HOME/.mcpp/registry"` —— 而 `registry/data/xpkgs` 正是已下载/解压的 payload
所在。等于缓存毫无作用，且每次运行都必须从各上游主机重新拉取**全部传递依赖**。

任何一个上游主机不可达 → 整个 CI 全线失败。已改为只清理需要为 GLOBAL 重新解析的
索引与 mirror 配置，保留 `data/xpkgs`（payload 由 recipe 的 sha256 固定，与 mirror 无关）。

实测生效：macOS `build-and-test` 由失败转通过，日志显示 `Cached compat.libarchive v3.8.7`
（复用，无 Downloading）。

### P-1.3 上游拉取稳定性 ⬜ 未解决（不在本仓）

2026-07-26 当天，多个互不相关的包在冷缓存路径上拉取失败：
`compat.bzip2@1.0.8`、`gtest@1.15.2`、`ftxui@6.1.9`。已排除索引仓 URL 失效
（`git ls-remote` 正常）与源站失效（sourceware / gitcode 本地均 200），且
**main 的内容当天同样失败**，属 runner 侧上游可达性问题。

durable 修法在上游 mcpp registry：给这些包的 GLOBAL 源补可靠镜像 ——
CN 侧已有 `gitcode.com/mcpp-res/*`，GLOBAL 侧缺同等镜像。

> **对本计划的影响**：P-1.3 未解决期间，**R.3 的"三平台 CI 全绿"门禁无法达成**。
> 这不改变门禁本身，只是把它的达成时点绑定到上游恢复。开发与 PR 审阅可继续 ——
> 本机 mcpp 缓存是热的，`mcpp build` / `mcpp test` 正常。

### P-1 的教训，回写到全局约束

计划把 CI 当成既有基础设施，没有先验证它覆盖新的分支拓扑。**引入新的分支模型时，
第一步应当是验证门禁在该拓扑下确实生效，而不是假定。**

---

## 1. 全局约束

沿用 `2026-07-26-xvm-provider-binding-group-plan.md` 的隔离纪律，并加严两条：

- ❗ 任何可变 CLI / E2E 调用必须设置临时 `HOME`、`XLINGS_HOME`、`MCPP_HOME`、XDG 目录与
  中立工作目录。**永不对宿主 xlings home 执行 install / remove / use。**
- ❗ 仅通过 mcpp 构建（`mcpp build` / `mcpp test`），Linux 上用 GCC 16。
  参见 memory `reference_build_xlings_via_mcpp.md`。
- 每个行为切片**先写测试**（`superpowers:test-driven-development`）。
- 保持增量 git 历史，不 amend / rebase / force-push。
- Commit 规范：`<type>(<scope>): <description>`。
- **新增**：所有 PR 的 base 必须是 `release/0.4.70`；**任何 PR 不得直接 target main**（§0.1）。
- **新增**：各 PR 不改 `core/config.cppm` 的版本号，版本在 R.2 一次性设定。
- **新增**：每个 `*ErrorKind` 枚举值必须同时提供 `code` 与 `hint`，无 hint 不得合入（设计 §7）。
- **新增**：每个批次必须包含至少一条**正向兼容性**测试（评审 §4 指出三个缺陷全部落在
  这个盲区里）。

---

## P0 — 修 PR #384 的合并阻塞项

> **交付物**：#384 从 Draft 转为可合并。**不引入新架构**，只消除倒退。

### P0.1 恢复头文件/库切换能力 🔴

**问题**：评审 §3.1 —— `installer.cppm:740` 的 `vdata.includedir = op.includedir;` 被删除，
但 `commands.cppm:206-215` 仍读它；`bindingHeaders` 零消费者。

**文件**：`src/core/xim/installer.cppm`、`src/core/xvm/registration.cppm`

**方案**（P0 阶段取最小改动，P3 会被 reconciler 取代）：
在 `RegistrationNode` 上补 `includedir` / `libdir` 字段，注册时写回 `VData`，
与 `bindingHeaders` **并行双写**。

- [ ] 测试先行：`XvmUseHeaderSwitch, InstalledPackageSwitchesSysrootHeaders`
      —— 内存 DB 安装两个版本 → `cmd_use` → 断言 sysroot include 路径切换
- [ ] `RegistrationNode` 增加 `includedir` / `libdir`
- [ ] `normalize_xpkg_registration_plan` 从 `XvmOp` 填充
- [ ] `apply_registration_batch` 写入 `VData::includedir` / `libdir`
- [ ] 回归测试：安装 → use v1 → use v2 → 断言 sysroot 两次都正确切换

**验收**：
```
✓ 新装包 versions.json 中 includedir 非空
✓ use v1 → use v2 → sysroot include 内容随之改变
✓ 已有 137 个测试全绿
```

### P0.2 header group 归属 🔴

**问题**：评审 §3.2 —— `installer.cppm:202-205` 从不设 `.group`，
`registration.cppm:563-572` 在 `groups.size() != 1` 时硬失败。

**文件**：`src/core/xim/installer.cppm`

- [ ] 测试先行：`XimRegistrationPlan, HeadersWithMultipleUngroupedNodesResolveToPrimary`
      —— 构造 `headers` + 2 个无 binding 的 `add`，断言**成功**且 header 归属主节点 group
- [ ] `normalize_xpkg_registration_plan` 中，header 的 `.group` 默认取
      **批次主节点**（`node.name` 对应的 registration node）所在 group
- [ ] 主节点不存在时（纯 lib 包）才回落到"唯一 group"规则
- [ ] 两者都不适用时保留 `HeaderAmbiguous`，但补 `hint`

**验收**：
```
✓ headers + N 个无 binding add 的 recipe 安装成功（N ≥ 2）
✓ HeaderAmbiguous 仅在真正无法判定时触发，且错误信息含 hint
```

### P0.3 删除回退保守化 🟠

**问题**：评审 §3.3 —— `removal.cppm:312-325` 逐 target 取 `installed[].rbegin()`，
可产生跨 provider 混搭。

**文件**：`src/core/xvm/removal.cppm`

- [ ] 测试先行：`XvmRemovalFallback, IncoherentCandidateDeactivatesWholeGroup`
      —— gcc@15(GCC) + g++@15-musl(musl) 都在 installed，删 gcc@16 后断言
      **不会**出现 gcc=GCC15 / g++=musl15 的组合
- [ ] 测试先行：`XvmRemovalFallback, CoherentSurvivingGroupIsSelectedWhole`
- [ ] 回退算法改为：
      1. 收集所有受影响 target 所属的候选 group
      2. 仅当某个 group 的**全部成员**都在 `installed` 且都存在于 DB 时，才整组激活
      3. 存在多个完整候选时，按 `(provider 相同优先, 版本降序)` 取一个，**确定性**
      4. 无完整候选 → **整组从 active 中移除**，并输出提示
- [ ] 回退结果必须满足 INV-2（设计 §3.2）

**验收**：
```
✓ 任何 remove 之后，Selection.active 满足 INV-2
✓ 回退选择是确定性的（同输入同输出，与 installed[] 插入序无关）
✓ 无完整候选时整组停用，且 stdout 明确告知用户需要重新 use
```

### P0.4 用户级错误呈现 🟡

**问题**：评审 §3.7 / §5.4 —— 失败只走 `log::warn` + `return false`。

**文件**：新增 `src/core/xvm/errors.cppm`；改 `src/core/xim/installer.cppm`

- [ ] 定义 `XvmUserError`（设计 §7）与渲染函数
- [ ] 为 `RegistrationErrorKind`(17) / `RemovalErrorKind`(8) / `BindingErrorKind`(15)
      建立 `kind → {code, hint}` 全映射表
- [ ] 编译期或单测保证映射**穷尽**（switch 无 default + `-Werror=switch`）
- [ ] `installer.cppm:1330-1365` 的两处 `log::warn` 改为渲染 `XvmUserError` 到 stderr
- [ ] `nothing was changed` 仅在确实零改动时输出

**验收**：
```
✓ 每个 ErrorKind 都有 code 与 hint（单测穷尽检查）
✓ install 失败时用户在默认日志级别能看到原因与出口
✓ 断言 "nothing was changed" 的场景确实零改动（对比前后 DB 快照）
```

### P0.5 PR 文案与拆分

- [ ] PR body 的 "apply it atomically" → "single-batch validation with all-or-nothing
      in-memory apply"，并明确 FS 副作用非事务
- [ ] 补充"本 PR 不改变 `cmd_use` 的物化行为"的显式声明
- [ ] 评估拆分为 4 个 PR：`db/types` | `bindings` resolver | `registration`+`removal` |
      installer adapter。若维持单 PR，须在 body 中给出按 commit 的审阅顺序

### P0.6 doctor 认识 binding 元数据（L1 只读）

**问题**：评审 §3.4 —— fail-closed 无出口。P0 先交付**只读诊断**，L2/L3 在 P1.3。

**文件**：`src/core/xself/doctor.cppm`

- [ ] `xlings self doctor` 报告 `bindingIntegrityIssues`（含 JSON Pointer）
- [ ] 报告 INV-1 / INV-2 / INV-4 违反
- [ ] 每条报告附 `hint`

**验收**：
```
✓ 人为写入损坏的 bindingGroup → doctor 能报出具体字段路径
✓ 人为制造 INV-2 违反 → doctor 报出不一致的 group 与成员
```

---

## P1 — 崩溃与并发安全底座

> **交付物**：修掉比 binding group 更严重的数据丢失风险。**与 P0 同批发布。**

### P1.1 原子文件写 🔴

**问题**：设计 §1.1 —— `platform.cppm:262` 是裸 `fopen("w")`，
`~/.xlings.json` 写到一半断电 = 整个版本库丢失。

**文件**：`src/platform.cppm`、`src/core/config.cppm`

- [ ] 测试先行：写入过程注入失败 → 断言原文件内容完好
- [ ] 新增 `platform::write_file_atomic(path, content)`：
      同目录 `.<name>.tmp.<pid>` → `fwrite` → `fflush` → `fsync` → `rename` → 目录 `fsync`
- [ ] Windows：`ReplaceFileW` / `MoveFileExW(MOVEFILE_REPLACE_EXISTING)`
- [ ] `Config::save_versions` / `save_workspace` / `profile::commit` 全部切换
- [ ] 全仓审计：`git grep write_string_to_file` 逐处判定是否需要原子语义

**验收**：
```
✓ 单测：模拟 tmp 写失败 → 目标文件字节不变
✓ 单测：rename 前进程退出 → 目标文件字节不变，遗留 tmp 可被下次清理
✓ 全部 save_* 路径已切换（grep 无遗留）
```

### P1.2 per-home advisory lock 🔴

**文件**：新增 `src/core/xvm/lock.cppm`

- [ ] 测试先行：两个进程并发 install → 断言无丢更新
- [ ] `<home>/.xlings.lock`，`flock(LOCK_EX)` / `LockFileEx`
- [ ] 写入 PID + 进程启动时间；stale 检测后可强夺
- [ ] 30s 超时，超时错误含持有者 PID
- [ ] 接入 `cmd_install` / `cmd_remove` / `cmd_use` / `doctor --fix`
- [ ] **不**接入只读命令与 shim dispatch
- [ ] 锁内**重新加载** Catalog + Selection（设计 §4.1 第 2 步）
- [ ] `XLINGS_NO_LOCK=1` 逃生阀，使用时打印警告

**验收**：
```
✓ E2E：并发 2×install 不同包 → 两个包都在最终 versions.json 中
✓ E2E：并发 install + use → 无 group manifest 指向缺失成员
✓ 持锁进程被 kill -9 → 下一次命令能检测 stale 并继续
✓ 只读 xlings list 在持锁期间不阻塞
```

### P1.3 doctor L2/L3 恢复 🟠

**文件**：`src/core/xself/doctor.cppm`、`src/core/xvm/db.cppm`

- [ ] `bindingIntegrityIssues` 从 `vdata_to_json` 移除（设计 §6.2）
- [ ] `--fix`（L2）：整组停用不一致的 group，修复 INV-2
- [ ] `--fix --reset-metadata`（L3）：丢弃不可解析的 group 元数据，降级为 legacy singleton，
      **payload 保留**
- [ ] 每次 `--fix` 前打印将要做的改动，`--yes` 跳过确认

**验收**：
```
✓ 评审 §5.3 的"死路"场景现在有出路：
    损坏 metadata → use 拒绝 → doctor 报告 → doctor --fix --reset-metadata → use 成功
✓ --fix 不删除任何 payload
✓ 保存后的 versions.json 不含 bindingIntegrityIssues
```

---

## P2 — group 归一化

> **交付物**：`groups` 顶层表成为唯一内存表示，删除 5 类冗余错误。

### P2.1 引入 `groups` 顶层表（双写验证期）

**文件**：`src/core/xvm/types.cppm`、`db.cppm`

- [ ] `GroupRecord` 类型 + `GroupTable = map<string, GroupRecord>`
- [ ] `groups_to_json` / `groups_from_json`
- [ ] 加载后归一化：`groups` 表 > root manifest > legacy 成对边（设计 §2.4）
- [ ] **双写期**：同时写 `groups` 与 root manifest，加断言二者等价
- [ ] 等价性 property test：随机 DB → 两种表示 → resolve 结果必须一致

**验收**：
```
✓ 三种历史格式都能加载为同一份内存 GroupTable
✓ 双写等价性 property test 通过 ≥ 10000 例
```

### P2.2 resolver 简化

**文件**：`src/core/xvm/bindings.cppm`

- [ ] 删除 `RootReferenceMismatch` / `RootMissingFromManifest` /
      `MemberReferenceMismatch` / `StartMemberMissing` / `GroupIdentityMismatch`
- [ ] 删除 `non_root_metadata_error_()`
- [ ] `resolve_provider_group_` 退化为查表 + 成员存在性检查
- [ ] 原有 137 个测试中受影响的**改造断言，不删除用例**

**验收**：
```
✓ bindings.cppm ≤ 220 行（当前 539）
✓ BindingErrorKind ≤ 8 个（当前 15）
✓ 改造后测试全绿，且被删枚举对应的场景改为断言"成功解析"或映射到保留枚举
```

### P2.3 停止写旧表示

- [ ] 关闭 root manifest 写路径，仅保留兼容读
- [ ] 关闭 `VInfo::bindings` 星形边写路径，仅保留兼容读
- [ ] `erase_exact_registration_edges_` 降级为迁移期清理工具

**验收**：
```
✓ 新装包的 versions.json 只含 groups 表 + 成员外键
✓ 0.4.69 产生的 versions.json 仍可正确加载并在下次保存时自动升级
```

---

## P3 — 物化收敛（commit-then-reconcile）

> **交付物**：★ 真正兑现"要么整组正确、要么磁盘零改动"。

### P3.1 Materialization ledger

**文件**：新增 `src/core/xvm/materialize.cppm`

- [ ] 启用 PR #384 已定义但未使用的 `MaterializedLedger`（`types.cppm:100-117`）
- [ ] `<subos>/.xlings-materialized.json` 读写（用 `write_file_atomic`）
- [ ] `compute_desired_materialization(Selection, Catalog) -> Ledger`，**纯函数**
- [ ] ledger 损坏 → 扫描重建，不报错
- [ ] 未知条目（不在 ledger 中的 sysroot 文件）→ **报告不删**（设计 D5）

**验收**：
```
✓ compute_desired_materialization 无 IO/时间/随机（单测在内存 FS 下全覆盖）
✓ 删除 ledger 文件 → 下次命令自动重建且结果一致
✓ 手工放入 usr/include/mine.h → reconcile 不删除它，doctor 报告为 unmanaged
```

### P3.2 reconciler

- [ ] `reconcile(Selection, Catalog, Ledger&)` 实现（设计 §4.3）
- [ ] 幂等性 property test：连续 reconcile N 次，第 2 次起零改动
- [ ] 中断恢复 property test：在任意步骤注入失败 → 重跑 → 收敛到正确状态

**验收**：
```
✓ reconcile ∘ reconcile ≡ reconcile（≥ 10000 随机例）
✓ 任意单点注入失败后重跑，最终 Materialization ≡ f(Selection, Catalog)
```

### P3.3 `cmd_use` 改为 commit-then-reconcile

**文件**：`src/core/xvm/commands.cppm`

> 这是 PR #384 显式声明未完成的部分。

- [ ] 删除 `collect_bindings` 自定义遍历（`commands.cppm:221-237`），改用 group resolver
- [ ] 按设计 §4.1 的 9 步实现
- [ ] `generation` + `reconciled` 标志写入 Selection
- [ ] 任何命令启动时若见 `reconciled: false`，先补跑 reconcile
- [ ] preflight 失败 → 磁盘零改动

**验收**（对应评审 §5.1 的目标输出）：
```
✓ use gcc 15.1.0 → gcc/g++/gcc-ar/libstdc++/headers 全部切换
✓ 任一成员 payload 缺失 → 拒绝 + 具名原因 + 磁盘零改动
✓ 提交点后 kill -9 → 下一条命令自动完成 reconcile
✓ 从 g++ / libstdc++ / 虚拟 root 任一入口 use，结果完全一致
```

### P3.4 接线 `profile.cppm` generation

- [ ] `profile::Generation::packages` 扩展携带 group key（设计 §4.4）
- [ ] `xlings use` 每次提交记一个 generation
- [ ] `xlings profile rollback <n>` 触发 Selection 回滚 + reconcile

**验收**：
```
✓ use v1 → use v2 → profile rollback 1 → 完整回到 v1 的整组状态（含 headers/libs）
✓ rollback 后的 Selection 满足 INV-2
```

---

## P4 — 生态验证与发布

### P4.1 pkgindex 全索引审计（硬门禁）

**仓库**：`openxlings/xim-pkgindex`

- [ ] 实现 `xlings index audit`（或离线脚本）：对每个 recipe 跑 registration dry-run
- [ ] 输出会被新校验拒绝的 recipe 清单
- [ ] 逐条修复：PR #384 设计文档 §3 已列出的
      e2fsprogs root 后注册 / syslinux 自绑定 / musl flavor root 版本错位 /
      openssl 重复注册 / 声明在组外的兄弟可执行文件
- [ ] 加入 pkgindex CI

**验收**：
```
✓ 全索引 dry-run 零拒绝
✓ pkgindex CI 对新 recipe 强制跑 audit
```

### P4.2 真实 GCC 15/16 端到端

**文件**：`tests/e2e/xvm_toolchain_group_test.sh`

隔离 HOME 下的完整证明：

- [ ] install gcc@15.1.0、gcc@16.1.0
- [ ] use 15 → 断言 `gcc/g++/gcc-ar --version` 全为 15 且 sysroot headers 为 15
- [ ] use 16 → 同上为 16
- [ ] 从 `g++` 入口 use 15 → 与从 `gcc` 入口结果逐字节一致
- [ ] remove 16 → 断言 15 完整可用，且 INV-2 成立
- [ ] 真实编译一个用到 `<format>` 的 C++23 程序，验证 ABI 一致

### P4.3 跨平台

- [ ] Linux / macOS / Windows 三平台在 `release/0.4.70` 上 CI 全绿
- [ ] 按 memory `project_ci_index_ref_pin.md`：pkgindex 侧合并后取得 commit sha，
      同步集成分支上 **6 个 workflow 的 `XIM_PKGINDEX_REF`**

---

## R — 集成验证、合入 main、发布

> 本阶段在 `release/0.4.70` 上执行，**是唯一产生对外版本的阶段**。
> P0–P4 全部完成之前不得进入 R。

### R.1 过渡代码清理（合入 main 前的最后机会）

集成分支模型的红利：这些代码从未进入 main，因此可以**直接删除而非弃用**。

- [ ] 删除 P0.1 的 `includedir`/`libdir` 临时双写 —— 其职责已由 P3 的 reconciler 承接
- [ ] 删除 PR #384 root-manifest 形态的兼容读（设计 §2.4 中间那一行）
      —— 该格式从未出现在 main 的任何提交上，无需弃用周期
- [ ] 删除 P2.1 双写验证期的等价性断言
- [ ] `git grep` 确认无 `TODO(P[0-4])` / 临时开关遗留

> ⛔ **不删除 legacy 成对边的写路径** —— 依 §0.2 决议 U7，0.4.70 必须继续双写，
> 否则回滚到 0.4.69 会静默丢失全部 binding 关系。该写路径在 **0.4.71** 才移除，
> 并在 0.4.70 的 CHANGELOG 中预告。

**验收**：
```
✓ 兼容读只剩两种形态：groups 顶层表 + ≤0.4.69 legacy 成对边
✓ 写路径：groups 表 + legacy 成对边（U7 双写），无 root-manifest、无 includedir 双写
✓ 无过渡开关
✓ 清理后全量测试仍绿（清理不得依赖被删代码的测试）
```

### R.2 版本号与发布物料

- [ ] `core/config.cppm` 版本一次性置为 `0.4.70`
- [ ] CHANGELOG：**必须**包含
      - fail-closed 带来的行为变化（哪些过去静默通过的操作现在会失败）
      - `xlings self doctor --fix` / `--reset-metadata` 的出路说明
      - `xlings profile rollback` 现在对 xvm 生效
- [ ] 确认无遗留的 `0.4.71` / `0.4.72` 引用（本轮全部收敛到单一 `0.4.70`）

### R.3 集成验证门禁

在 `release/0.4.70` 上**全部通过**才可发起合入 main 的 PR：

- [ ] `git merge origin/main` 已同步至最新，冲突已解决
- [ ] `mcpp build` + `mcpp test` 三平台全绿
- [ ] **场景级验收矩阵 A1–A14 全部通过**（§2）
- [ ] 每批次的 DoD（§3）逐条复核通过
- [ ] 全索引 dry-run 零拒绝（P4.1）
- [ ] 真实 GCC 15/16 E2E 通过（P4.2）
- [ ] 从 0.4.69 的真实 `~/.xlings.json` 升级演练通过（隔离 HOME）
- [ ] 崩溃注入演练：P1.1 / P3.2 的 kill -9 场景在三平台各跑一次

> ⚠️ 遵循 `superpowers:verification-before-completion`：以上每一项都必须**先运行、
> 后勾选**，不得凭推断。勾选时在 PR 中附命令与输出摘要。

### R.4 合入与发布

- [ ] 发起 `release/0.4.70` → `main` 的 PR，**merge commit 不 squash**
- [ ] 合入后手动触发 `release.yml`（`workflow_dispatch`，version 留空取
      `core/config.cppm`）
- [ ] 按 memory `project_release_cancel_recovery.md` 处理 release 中断恢复
- [ ] 按 memory `project_custom_index_artifact.md` 执行发布后的手动 bump / mirror
- [ ] 发布后保留 `release/0.4.70` 分支一个版本周期，便于回查

---

## 2. 场景级验收矩阵

评审 §5.5 的对照表转为**可执行验收**。每格对应一条 E2E 断言，全部通过才算 Done。

| # | 场景 | 验收断言 | 批次 |
|---|---|---|---|
| A1 | `use` 切换程序 | 组内全部 program 版本一致 | P3.3 |
| A2 | `use` 切换头文件 | sysroot include 内容随组切换 | P0.1 → P3.3 |
| A3 | `use` 切换库 | sysroot lib 内容随组切换 | P0.1 → P3.3 |
| A4 | `use` 失败 | 磁盘零改动 + code + hint | P0.4, P3.3 |
| A5 | 多入口一致 | 从任一成员 use，结果逐字节一致 | P3.3 |
| A6 | `remove` 精确性 | 只删本 release，同名其它 release 完好 | 已由 #384 覆盖 |
| A7 | `remove` 回退一致 | 回退后满足 INV-2，且确定性 | P0.3 |
| A8 | 坏状态可发现 | doctor 报出具体字段与不变量违反 | P0.6 |
| A9 | 坏状态可恢复 | doctor --fix 后 use 成功，payload 未丢 | P1.3 |
| A10 | 崩溃安全 | 任意点 kill -9 后下条命令收敛到一致状态 | P1.1, P3.2 |
| A11 | 并发安全 | 并发 install/use 无丢更新、无幻影成员 | P1.2 |
| A12 | 正向兼容 | 0.4.69 的 versions.json 可加载并自动升级 | P2.1 |
| A13 | 索引兼容 | 全索引 dry-run 零拒绝 | P4.1 |
| A14 | 回滚 | profile rollback 完整还原整组含物化 | P3.4 |
| A15 | 过渡代码清零 | 兼容读只剩 2 种形态，无 root-manifest/includedir 双写 | **R.1** |
| A16 | **无感升级** | 0.4.69 真实 home → self update → 激活工具链版本不变、shim 不重建、无需手工步骤 | P2.1, R.3 |
| A17 | **无 ledger 认领** | 0.4.69 遗留 sysroot 首次 reconcile 走 adoption pass，不报漂移、不删文件 | P3.2 |
| A18 | **降级安全** | 0.4.70 写出的状态回滚到 0.4.69 后，binding 关系仍有效 | R.1(U7 双写) |

**A1–A18 全部在 `release/0.4.70` 上复跑通过（R.3），才可发起合入 main 的 PR。**
单个 PR 只需保证其归属批次的那几条，不要求全绿 —— 全绿是 R 阶段的门禁，不是每个 PR 的门禁。

---

## 3. Definition of Done

### 3.1 单个 PR 的门禁

1. base 是 `release/0.4.70`（**不是 main**）
2. `mcpp build` 通过（Linux GCC 16 隔离沙箱）
3. `mcpp test` 全绿，且**新增测试数 > 0**
4. 本 PR 涉及的 A 编号断言通过
5. 至少一条**正向兼容性**测试（不是失败路径测试）
6. 每个新增 `ErrorKind` 都有 `code` + `hint`（单测穷尽检查）
7. `git diff --check release/0.4.70...HEAD` 通过
8. 未使用宿主 xlings home（在 PR 描述中明示隔离方式）
9. 未改动 `core/config.cppm` 的版本号

### 3.1.1 上游故障期的合入例外（有界，不得扩大）

P-1.3 期间 CI 无法全绿。集成分支不面向用户，因此允许在**同时满足**下列条件时合入：

1. 本机 `mcpp build` 与 `mcpp test` 全绿，且输出记录在 PR 中
2. CI 失败**仅**呈现已知上游拉取签名 —— `error: fetch '<pkg>@<ver>' failed (exit 1)`
   或 `xlings install_packages failed`，且失败发生在**编译任何本仓代码之前**
3. 出现任何其它失败签名（编译错误、测试失败、链接错误）→ **一律不合入**
4. 在 PR 中明确记录当时的 CI 状态与失败签名，不写"CI 通过"

> ⚠️ 这是**推迟**验证，不是豁免验证。R.3 的三平台全绿门禁不受本例外影响：
> 上游恢复后集成分支必须整体重跑并全绿，否则不得向 main 提 PR。
> 例外仅适用于 PR → 集成分支这一段。

### 3.1.2 评审授权

用户已明确：**只验收最终的 release PR**（`release/0.4.70` → `main`）。
中间 PR 由实施方按 §3.1 与 §3.1.1 自行把关合入。

这提高而非降低了 R.3 的标准 —— 它是唯一一次外部验收，因此 §3.2 的每一条都必须
附命令与输出，不得以"已在中间 PR 验证过"替代。

### 3.2 集成分支合入 main 的门禁（R.3）

在 3.1 之上追加：

10. A1–A18 **全部**通过
11. 三平台 CI 全绿（PR 级只要求 Linux）
12. 全索引 dry-run 零拒绝 + 真实 GCC 15/16 E2E 通过
13. 0.4.69 真实数据升级演练通过
14. R.1 过渡代码清理完成
15. CHANGELOG 完整（行为变化 + `doctor --fix` 出路 + rollback 生效说明）

> ⚠️ 遵循 `superpowers:verification-before-completion` —— **任何"完成/通过"的声明前
> 必须先运行验证命令并确认输出**，不得凭推断断言。R.3 的每一项需在 PR 中附命令与输出摘要。

---

## 4. 工作量估计与并行度

| 批次 | 生产代码 | 测试 | 可并行 |
|---|---|---|---|
| P0 | ~600 行改动 | ~800 行 | P0.1/P0.2/P0.3 可并行；P0.4 依赖前三者的枚举定稿 |
| P1 | ~450 行新增 | ~600 行 | P1.1 与 P1.2 完全独立，可并行 |
| P2 | ~400 行改动 / −350 行删除 | 改造现有 137 例 | 串行（P2.1 → P2.2 → P2.3） |
| P3 | ~900 行新增 | ~1200 行 | P3.1 → P3.2 串行；P3.3/P3.4 可并行 |
| P4 | 索引侧为主 | E2E ~300 行 | P4.1 与 P4.2 可并行 |
| R | 净删除（R.1 清理） | 复跑为主 | 串行，不可并行 |

**P2 与 R 的净代码量都是负的** —— 归一化删掉的比加上的多，集成分支模型又让过渡代码可以
直接删除而非弃用。这是设计正确性的一个旁证。

### 关键路径

```
P0 ─┐
    ├─► P2 ─► P3.1 ─► P3.2 ─┬─► P3.3 ─┐
P1 ─┘                        └─► P3.4 ─┼─► R.1 ─► R.2 ─► R.3 ─► R.4
                        P4.1 ──────────┘
                        P4.2 (依赖 P3.3) ┘
```

P0 与 P1 完全并行（互不触碰同一文件）；P4.1 的 pkgindex 审计可从 P2 完成后即开始，
但其 `XIM_PKGINDEX_REF` 的回填必须在 R.3 之前完成（§0.1 风险表）。

---

## 5. 开放问题与决议

> 初稿列了 5 个"未决问题"。核对代码后，其中 **Q1 与 Q4 的提问本身带着错误前提**，
> 查证即自解；Q2/Q5 也可以现在定。仅 Q3 的实现细节需等到 P3.1。
> 决议一并记录在此，作为实施时的依据。

### Q1 — `groups` 表放 `~/.xlings.json` 还是独立文件？

**初稿理由（"独立文件更利于原子写与并发"）不成立。** 原子写是 per-file 的；
`versions` 与 `groups` 必须共同满足 INV-4，且**永远一起变**（install / remove 同时改两者，
无任何只改其一的场景）。拆成两个文件 = 两次原子写 = 主动引入撕裂窗口。

> ✅ **决议：同一个文件，`groups` 与 `versions` 平级。**

`save_versions()`（`config.cppm:1021-1046`）对整个 `~/.xlings.json` 读-改-写，
对**其它键**（mirror 配置等）确有丢更新风险 —— 由 P1.2 的"锁内重载"解决，与拆分无关。

**拍板时点：已定（P2.1 直接执行）。**

### Q2 — project 模式是否也需要 Selection generation？

`types.cppm:119-134` 的注释已确立原则：project `.xlings.json` 是**用户手写、进 git 的
意图声明**，不带 `installed[]`；只有 subos workspace 携带运行时状态。给进 git 的文件加
`generation` / `reconciled` 计数器会制造 git 噪音与合并冲突。

但 project 模式**确实会物化**（`config.cppm:1056-1073` 建了 `bin/lib/usr/generations`）。

> ✅ **决议：意图与状态分离，统一规则 ——**
> **`generation` / `reconciled` / ledger 属于 subos 运行时状态，永不写入任何
> user-authored 文件。**
> 全部落 `Config::project_state_path()`（`<project>/.xlings/`），global 模式落 `<subos>/`。

**拍板时点：已定 —— 且必须现在定，P3.1 即需使用，不能等 P3.4。**

### Q3 — Windows 上 `usr/include` 物化用什么机制？

**机制已经定了**，见 `commands.cppm` 的 `create_link_`：

```
POSIX     → fs::create_symlink
Windows   → 目录: platform::create_directory_link (junction)
             文件: create_hard_link，失败回退 copy_file
```

真问题是 **ledger 能否从文件系统反推属主**：symlink / junction 可 `read_symlink()` 反查，
**Windows 的 copy 回退不能**（复制文件无回指信息）。

相关缺陷：`install_headers` 对已存在目标做**无条件 `remove_all(target)`**，
会静默覆盖另一个 group 的头文件 —— 这正是 A2/A3 场景失效的具体机制。

> ✅ **决议（四条）：**
> 1. 保留现有链接机制，不改
> 2. **ledger 是权威，不是缓存** —— 不依赖从 FS 反推；FS 扫描只用于报告 unmanaged 条目
>    （因此 Windows copy 回退不构成阻碍）
> 3. reconcile 遇到"目标已存在、属于另一 group、且不在本次 desired 中" → **走删除路径，
>    禁止静默 `remove_all`**
> 4. ledger 粒度 = includedir 的**顶层条目**，与 `install_headers` 现有循环对齐

**拍板时点：P3.1 前确认实现细节，方向已定。**

### Q4 — 跨 subos 引用计数 GC

**初稿表述不准确：跨 subos 引用检查已经存在并可用** ——
`installer.cppm:1079-1115` 的 `is_version_referenced_anywhere_()` 扫描所有 subos 的
`.xlings.json`，同时检查 `active` 与 `installed[]`，兼容 bare/namespaced（注释记录了
0.4.19 的演进原因）。其粒度 `(target, version)` **本来就是正确的** —— payload 存于
`xpkgs/<name>/<ver>/`，就是这个粒度，不是 group 粒度。

状态分层也自然回答了这个问题：

| 关注点 | 归属层 | 机制 |
|---|---|---|
| payload 该不该从磁盘删 | **Catalog** | `is_version_referenced_anywhere_()`（已有） |
| 本 subos 该长什么样 | **Materialization** | reconcile（P3 新增） |

> ✅ **决议：确认为非目标，不改粒度。**
> 仅在 P3 补一条测试：**subos A 的 reconcile 不修改 subos B 的任何路径。**

**拍板时点：已定。**

### Q5 — #384 先合并，还是把 P2 并入？

`.github/workflows/release.yml` 是 `workflow_dispatch` 触发 —— **合并不会自动发版**。
所以这是假二选一：真正要决定的是"中间态放在哪里"。

> ✅ **决议（终版）：单一 `release/0.4.70` 集成分支。**
> 多 PR 允许且鼓励，但**全部以 `release/0.4.70` 为 base**；P0–P4 全部完成并通过
> 集成验证（R.3）后，一次性合入 main 并发布唯一版本 `0.4.70`。

```
origin/main ──────────────────────────────────────────► (R.4) merge ──► v0.4.70
                                                             ▲
       release/0.4.70 ── #384(P0) ─ P1 ─ P2 ─ P3 ─ P4 ─ R.1~R.3
                         └────────── 多 PR 汇入，main 不可见 ──────────┘
```

**相比"合入 main 后 hold tag"，集成分支多出的收益**：过渡代码**根本不进 main 的历史**，
因此可以在 R.1 直接删除而非弃用 ——

| 过渡产物 | hold-tag 模型 | 集成分支模型 |
|---|---|---|
| #384 root-manifest 兼容读 | 进了 main，需保留弃用周期 | **R.1 直接删除** |
| P0.1 `includedir` 临时双写 | 进了 main，成为公开行为 | **R.1 直接删除** |
| P2.1 双写验证期断言 | 需要额外 PR 清理 | **R.1 直接删除** |
| 设计 §2.4 兼容读形态数 | 3 种 | **2 种** |

**代价**：长期分支漂移 + 跨仓依赖排序，对策见 §0.1 的风险表。

**拍板时点：已定。#384 需将 base 从 main 改为 `release/0.4.70`。**

### 决议对实施计划的影响

| 决议 | 影响 |
|---|---|
| Q1 | P2.1 不新增文件，`groups` 写入 `~/.xlings.json` |
| Q2 | **P3.1 提前需要** project/global 两种 state path 的统一抽象，不能推到 P3.4 |
| Q3 | P3.1 的 ledger 设计为权威存储；P3.2 新增"禁止跨 group 静默 clobber"断言 |
| Q4 | P3.2 新增跨 subos 隔离测试；不新增引用计数代码 |
| Q5 | 新增 `release/0.4.70` 集成分支与 **R 阶段**；#384 改 base；版本号在 R.2 一次性设定；新增 R.1 过渡代码清理 |
