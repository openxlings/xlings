# PR #519 收尾实施计划（2026.8.10.1）

> 设计与实测：`.agents/docs/2026-08-10-pr519-review-and-completion-design.md`
>
> 前序计划：`.agents/plans/2026-08-09-stability-regression-recovery-implementation.md`（Task 1–16）
>
> 决策：**单 PR #519**，版本 `2026.8.10.1`，additive commit，禁 amend/rebase/force-push。

---

## 1. 八个角度各自要什么

同一批改动，从八个角度看要求并不相同。列出来是因为**其中三对互相冲突**，
而冲突点正是本轮最容易做错的地方。

| 角度 | 这轮的具体要求 | 谁负责证明 |
|---|---|---|
| **架构** | 一个问题只有一个回答者：runtime 权威 = manifest 声明；声明的来源 = 实际在跑的 runtime，不是常量 | R0 的 `preserved_runtime` 三级来源 |
| **稳定性** | 只读命令不得有副作用；昂贵审计必须显式 | quick doctor 195.16s→0.63s；`InventoryTrace` 结构断言 |
| **优雅简洁** | 修复读的必须是检测读的同一个函数 | `--fix` 的采纳走 `observed_runtime`，与 D5 同源 |
| **用户体验** | 每条拒绝都要命名一条**真实存在**的出路 | R0 的 adopt / migrate 双路径；「新建 SubOS」是假死局 |
| **兼容性** | 老 home 升级后不得变成「按新规则永久不合规」 | S1/S2/S3 三态 E2E |
| **跨平台** | 路径身份比较不得依赖分隔符；Windows/macOS/aarch64 结论只来自 native runner | `PayloadOf.AcceptsWindowsSeparators` 的不变量断言 |
| **一致性** | `list <F>` 必须等于 `list` 按 F 过滤 | `AFilteredQueryEqualsFilteringTheFullQuery` |
| **无感升级** | 升级不需要用户做任何事；需要时 `--fix` 一条命令解决 | R0 修复 1（预防）+ 修复 2（治已病） |

### 三对冲突，以及本轮怎么裁的

1. **稳定性 vs 一致性** —— 过滤前置判定越省，越可能漏行。
   *裁法*：只有带 filter 的查询才付反向索引的成本，且用差分不变量把「省」和「对」绑在一起。
   省错了测试会红，而不是少一行。

2. **兼容性 vs 架构** —— 让老 home 保持无声明最兼容，但 `validate_block` 要求声明合法，
   无声明 = 永久报 manifest 损坏。
   *裁法*：不是「不声明」，而是**声明它实际在跑的东西**。架构不让步，兼容性也拿到。

3. **用户体验 vs 无感升级** —— `--fix` 自动采纳 active runtime 很省事，但会撤销
   「我声明了 2.44，正准备迁移」这种真实意图。
   *裁法*：只在 Error 形态（有 active 且不一致）采纳；冷意图（无 active）只警告不动。
   discriminator 是 `activeVersion.empty()`，不是猜。

---

## 2. 任务依赖 DAG

```text
[已完成 · 本地全绿]
  A1 R0 runtime 声明来源 + --fix 采纳 + 双出路
  A2 R1 subos use 修饰符守卫
  A3 R2(a) 反向边 + 差分不变量
  A4 R3 裸名/显式版本二分
  A5 R4 interp 穿符号链接
  A6 R6/R7
  A7 版本 2026.8.10.1
          │
          ▼
  B1 docs 落盘(D6) ──┐
                     ├─> B3 commit + push #519 ─> B4 全平台 CI terminal green
  B2 PR 正文更新 ────┘                                    │
                                                          ▼
                                        C1 squash merge (Sunrisepeak, --admin)
                                                          │
                                                          ▼
                                        C2 release 2026.8.10.1 全平台 assets
                                                          │
                                        ┌─────────────────┼─────────────────┐
                                        ▼                 ▼                 ▼
                              C3 本地 gtc 补 GitCode   C4 GET+sha256    C5 pkgindex pointer A
                                        └─────────────────┼─────────────────┘
                                                          ▼
                                        D1 生态真实验证 subos --sandbox --cmd
                                                          │
        ┌─────────────────────────────────────────────────┤
        ▼                                                 ▼
  E1 mcpp #392 exact owner (可与 B/C 并行开发)      E2 #506 收口
  E2' mcpp #514 真实集成 (需要 C2 已发布的 xlings)         │
        └────────> E3 mcpp release ──> E4 pointer B ──────┘
```

**关键：E1 不依赖 C2。** mcpp #392 读的是编译器所属 registry home 的 SubOS manifest，
`subos_info.runtime` 自 2026.8.5.1 起就在公开发布里；它的单测与 alternating-build E2E
不需要本 PR 的任何新行为。只有 E2'（真实 shared-registry 集成）和 E3（发布）需要 C2。
把 E1 排在 B/C 之后是把 5 段串行发布写死，实际只需要 3 段。

---

## 3. 逐步执行

### B — 提交与 CI

- [ ] **B1** 三份 doc 落盘
  - 删除工作区那份 `2026-08-09-stability-regression-recovery-design.md`（旧版草稿，且会挡住 checkout）
  - `2026-08-09-instant-query-and-userspace-os-architecture-design.md`、
    `2026-08-09-open-issues-deep-triage.md` 作为独立 docs commit
- [ ] **B2** PR 正文更新：版本、实测数字（310×、143→0）、新增 E2E 清单
- [ ] **B3** additive commit + push（heredoc 提交信息；push 前扫本机身份/路径/凭据）
- [ ] **B4** 等 Linux / Linux E2E / Linux root / macOS / Windows / aarch64 cross+native 全部 terminal green
  - running / cancelled / superseded 都不算过
  - 失败只加 additive fix commit，然后重新从 final head 起算

### C — 发布

- [ ] **C1** squash merge（切到 Sunrisepeak，`--admin`，合完切回）
- [ ] **C2** release workflow → 等全平台 assets + sidecar
  - **绿了但没产物 = 失败**
- [ ] **C3** 本地 `bash tools/mirror-latest.sh xlings`（gtc 补 GitCode 资源）
- [ ] **C4** 逐平台 **GET**（不是 HEAD）跟随重定向，sha256 对 sidecar：
      linux x86_64 / linux aarch64 / macOS arm64 / windows x86_64
- [ ] **C5** `xim-pkgindex/pkgs/x/xlings.lua` 全平台版本 + `latest` pointer；fresh-install 不得 pin

### D — 生态真实验证

用**已发布**的 xlings，不是本地构建。

- [ ] **D1a** 冷 home：用 tarball 自带的 `self install` 装到干净 root
      （**不用 `quick_install`** —— 它忽略 `XLINGS_HOME`，会打到真实 home）
- [ ] **D1b** 真实 sandbox 路径，逐条记录输出：
      ```
      xlings subos use <name> --sandbox --cmd 'xlings --version'
      xlings subos use <name> --sandbox --cmd 'mcpp --version'
      xlings subos use <name> --sandbox --cmd '<llvmpipe 像素 + Wayland 探针>'
      ```
- [ ] **D1c** 有设备时再跑 `--sandbox --gpu --cmd`，记录 renderer / device node /
      loaded-object provenance / 像素输出
- [ ] **D1d** 老 home 只读兼容：`list`、`info gcc`、quick doctor、`--deep --scope`
- [ ] **D1e** R0 的真实回归验证：一个无 `subos_info` 块的 home，升级后
      `self doctor` 不得从 0 变非零，`use glibc <当前 active>` 不得被拒

### E — 跨仓（E1 可立即并行开工）

- [ ] **E1** mcpp #392：exact compiler-owner fixup，删 `find_sandbox_glibc_lib`，`kFixupRev++`
- [ ] **E2** xim-pkgindex：删掉 #506 的 Windows tolerance，让真实卸载 marker + foreign-provider
      保全变成硬门禁
- [ ] **E2'** mcpp #514 真实 shared-registry 集成（需要 C2）
- [ ] **E3** mcpp release + mirror
- [ ] **E4** pointer B + graphics verifier 命名域

---

## 4. 门禁（可证伪）

沿用设计文档 §4 的 G1–G10，本轮新增两条，都已在本地成立：

| | 判据 | 现状 |
|---|---|---|
| **G5'** | 无 `subos_info` 块 + active glibc ≠ 2.44 的 home，升级后 doctor 不得从 0 变非零，`use <当前 active>` 不得被拒 | ✅ E2E S1/S2/S3 |
| **G11** | `doctor --deep` 在真实 home 上相对 `2026.8.9.2` 新增的每条 `LoaderLibcSplit` **逐条可解释** | ✅ 143 误报已消除，剩 2 条 godot，逐条已核 |

不变的原则：**任何计时通过都不能豁免结构违规。**

---

## 5. 过程约束

- 单 PR #519，additive commit；禁 amend / rebase / force-push
- 提交信息一律 `git commit -F -` + 带引号 heredoc（`-m "…"` 里的反引号会真的执行）
- E2E 只用隔离临时 HOME/XLINGS_HOME/SubOS root；跑完 `slice-real-home.sh verify-untouched`
- 「本地过」「远端 HEAD 过」「发布后公共链路过」是三种独立证据，不能互相替代
- 常规日期版本不用 `.0`；fresh-install 不得 pin 已发布的 xlings 版本
- macOS / Windows / aarch64 的结论只来自 native runner
