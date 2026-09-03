# 诊断与守卫的「真值」—— 实施计划

> 设计/调研:`.agents/docs/2026-09-03-diagnostic-truth-survey-and-plan.md`
> 分支 `fix/musl-loader-family-misread`,基线 `ba51901`(2026.9.3.1)。
> 目标版本 **2026.9.3.2**。单 PR 落地。

> **For agentic workers:** 每个 P 有自己的差分验收:**先让差分在旧二进制(2026.9.3.1)上失败**,
> 再让它在新二进制上通过。步骤是 `- [ ]` checkbox。

## Global Constraints

- 构建/测试只用 `mcpp build` / `mcpp test`,**不要**裸 xmake,**不要** `mcpp clean`。
- 单测文件放 `tests/unit/`,自动发现;新增 e2e **必须**注册进 `tests/e2e/run_all.sh`。
- 本地 e2e 一律 `XLINGS_TEST_MIRROR=CN`;沙箱真机验证前 `xlings config --mirror CN`。
- commit 一律 `git commit -F -` + 带引号 heredoc。
- 断言只断言不变量,不断言索引版本号 / 快照条数。
- 真实 home 上的度量只读(`self doctor --deep --all`),`--fix` 只在 slice 上跑。
- 不跨仓库:无新 libxpkg 字段、无索引格式变化、无 recipe API 变化。跨仓协作只有发布链:
  release → `tools/mirror-latest.sh`(本地 gtc 补 GitCode)→ xim-pkgindex `pkgs/x/xlings.lua` bump。

---

## 多角度约束(每条落到一个可验的判据)

| 角度 | 判据 |
|---|---|
| 架构 | 报告与拒绝共用同一谓词(`elf_same_source::check`);修谓词,不在两端各打补丁 |
| 稳定性 | 守卫从「拒一个 plan」回到「拒一个节点」;拒绝写 marker,其余节点继续 |
| 优雅简洁 | remedy 是 `{command, note}`,渲染层负责分行;不再在字符串里夹散文 |
| 用户体验 | 每条打印出来的命令都能原样运行;152 条噪声 → 0,真 finding 浮出来 |
| 兼容性 | 老 home 无迁移动作;老 finding 消失只是不再报,不改任何文件 |
| 跨平台 | 谓词是纯函数,Windows/macOS 上 scan 本就不产生 INTERP 结果;e2e 用 patchelf,非 Linux SKIP 且说明 |
| 一致性 | 一处 `owning coordinate` 逻辑(`xvm::owner`)服务 doctor、dispatch、use、update 四处 remedy |
| 无感升级 | `self update` 不再对 `xim:` 键假报;entry binary 比较前 strip namespace |

---

## 任务依赖图

```
P1 same-source 顺序语义 ─┐
P2 守卫契约(逐节点)   ─┼─→ P7 e2e(同一夹具:两包一 plan)─┐
P4 InterpRuntimeDrift   ─┘                                   │
P3 remedy 契约(owner 解析 + {command,note})─→ S13 扩门 ──────┼─→ P8 文档/版本 → PR/CI → 自审 → release → gtc → 沙箱验证
P5 #579 self update 谓词 ────────────────────────────────────┤
P6 #578 单版本 removal ──────────────────────────────────────┘
```

**可并行起步:** P1 / P3 / P5 / P6(互不触碰同一函数)。
**串行:** P2 在 P1 之后(同一处 installer 代码);P4 在 P1 之后(复用 scan 的逐 ELF 信息);P7 依赖 P1+P2。

---

## P1 · 同源检查 = 加载器会做的选择

`src/core/elf_same_source.cppm::check`。

- [x] `CoreSource` 加 `name`(目录条目名 = soname)
- [x] 沿 RUNPATH **顺序**遍历;每个 core soname 只由**首个**含它的目录回答;后续目录对该 soname 不再计入
- [x] 注入探针(合成目录)路径:一个合成目录视为回答了全部 core soname
- [x] 家族守卫、反向规则(`HostLoaderPayloadCore`)、`describe()` 不动
- [x] 单测:`AnyDifferentCoreRuntimeEntryViolates` 拆成
      `ACompleteFirstDirectoryAnswersEveryCoreSoname`(pass)与
      `AnIncompleteFirstDirectoryLeavesLaterEntriesInPlay`(violated);
      新增真实 home 形状 `OwnGlibcAheadOfADriftedFarmIsNotASplit`(farm 用符号链接)

**差分验收:** 真实 home 只读 `self doctor --deep --all`:`loader/libc split` **152 → 0**;
`FirstCoreRuntimeEntryDeterminesResult` 与 E2E-62 仍过。

## P2 · 守卫拒一个节点,不拒一个 plan(依赖 P1)

`src/core/xim/installer.cpp:3017` 附近。

- [x] `return std::unexpected` → `write_payload_failure_marker` + `onStatus(Failed)` + `continue`
- [x] 文案保留「resolution defect … report it with the two paths above」
- [x] `install_summary` 正常发出;退出码由 `failedCount` 决定(既有逻辑)

**差分验收:** P7 的两包一 plan:被拒包 rc≠0 且有 marker,正常包装上并注册。旧二进制上正常包装不上。

## P3 · remedy 契约:可运行,或空

- [x] `xvm::owner`:新增 `recorded_owner(db, target, version)` —— 只返回**有记录证据**的候选
      (provider 记录 / payload 路径),不返回「target 自身」这种猜测
- [x] `xvm/errors.cpp` `not_in_subos`:`NotInSubos` 加 `installCoordinate`;有则
      `xlings install [-g] <coord>`;无则 `xlings search <name>`(一定能跑)
- [x] `xvm/shim.cpp:730` `pinned_version_missing`:同上
- [x] `xim/commands.cpp:2026` `update`:用 `match->canonicalName`
- [x] `doctor::Finding`:`remedy` 拆为 `remedy`(命令)+ `remedyNote`(散文);渲染层 note 另起一行;
      迁移 doctor.cpp:462/548/593/617 四处
- [x] S13 扩门(实际做法):e2e 对每条 `→ run` 取 `xlings <sub>` 的首词,断言 `<sub>` 是真实子命令;
      命令位不得含 `<one of` / `(adopt` / `(then` / `<version>`;`install … --force` 与 `<ns>-x-<pkg>@`
      仍被拒。没有加新命令。
- [x] 单测:`recorded_owner` 三例(provider / path / 无证据→nullopt)

**差分验收:** 真实 home dispatch 一个 group 注册的程序名(如 `slang`)在无主张的 subos:
输出不含 `xlings install slang`;含 `xlings search slang` 或一条可解析坐标。

## P4 · 把 152 条背后的真信息说出来(依赖 P1)

- [x] `elf_same_source::scan_payload_report(dir)` → `{ findings, interpPayloads: map<payload,count> }`;
      `scan_payload` 保持签名(返回 `.findings`);`PayloadScanCache` 缓存 report
- [x] doctor 新 `FindingKind::InterpRuntimeDrift`(**Notice**):按 subos 聚合 ——
      「subos `<n>` 声明 glibc@2.44;N 个可执行文件(M 个包)的 PT_INTERP 指向 glibc 2.39,
      它们靠自身 RUNPATH 运行;2.39 若从最后一个 subos 移除,这些二进制将无法启动」
- [x] `--fix` 不动它;remedy 为空,note 给包清单(最多 8 个 + `+N more`)
- [x] 只对声明了 runtime 的 subos 报;只统计**注册在该 subos** 的包(workspace installed)

**差分验收:** 真实 home 出现 1 条 Notice,数字 ≈ 18 个包;exit code 不因它非零。

## P5 · #579 `self update` 谓词

- [x] `update_landed_on_index_build`:只有 `local:` 前缀判为「不是索引构建」;其它命名空间都是索引
- [x] `entry_binary::replace_with`:比较与打印前 strip namespace(本地 lambda,不引入 xvm.db 依赖)
- [x] `cmd_update`:`use` 之后 `Config::reload_state()` 再读 active
- [x] 单测更新 `SelfUpdateLanding.*`:`xim:2026.9.2.1` → true;`local:0.4.51` → false

**差分验收:** 单测;真实 home 无法安全复现(需要一次真实升级),在 release 后的 `self update`
验证中观察不再出现 `nothing was upgraded` / `DOWNGRADED`。

## P6 · #578 单版本 versionless removal

- [x] `xvm/removal.cpp:246` 分支:`versions.size() == 1` → 该唯一键即 exactVersion
- [x] 单测 `test_xvm_bindings.cpp`:一个存储版本、无 selection、`xvm.remove("fd")` → 成功删除该版本

**差分验收:** 单测在旧实现上得到 `VersionNotFound`。#578 的第 1 点(经目录解析到索引 latest)
在 e2e 复现失败则记录、不在本 PR 处理。

## P7 · e2e(P1+P2 同一夹具)

`tests/e2e/install_guard_loader_order_test.sh`(已注册为 **E2E-97**;在 2026.9.3.1 二进制上于
A1 失败,在 2026.9.3.2 上 PASS):

- 两个假 glibc 载荷 `xim-x-glibc/1.0`(interp)与 `2.0`;farm `subos/default/lib` 符号链接指向 2.0
- 包 A:RUNPATH `[1.0/lib64, farm]` → **必须安装成功**(旧二进制拒绝)
- 包 B:RUNPATH `[farm]` → 真 split,**拒绝**;与包 C(正常)同一 plan:
  C 装上并注册,B 有 failure marker,退出码 ≠ 0
- 非 Linux / 无 patchelf / 无 gcc:SKIP 且打印原因

## P8 · 文档 / 版本 / 发布

- [x] `mcpp.toml` → `2026.9.3.2`
- [x] 调研文档状态改「已实现(P0–P7)」,§8b 追加实施中被推翻的判断
- [x] release notes `.agents/docs/2026-09-03-release-2026.9.3.2-notes.md`
- [ ] 单 PR(**#581**);CI 全绿;自我 review 一次(已做:一处 note 行花括号、一处前缀提升)
- [ ] release;资源一出即本地 `tools/mirror-latest.sh` 补 GitCode
- [ ] xim-pkgindex `pkgs/x/xlings.lua` bump(bump-index 只开 PR 不合并)
- [ ] 沙箱真机验证(先 `xlings config --mirror CN`):`self update`;`self doctor --deep --all` 计数;
      `xlings subos <n> --sandbox --cmd "gcc --version && cmake --version && node --version"`
