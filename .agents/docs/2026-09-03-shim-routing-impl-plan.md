# 路由表与状态分离 —— 实施计划

> 设计:`.agents/docs/2026-09-03-project-shim-routing-vs-state-design.md`
> 分支 `fix/shim-routing-vs-state`,基线 HEAD `7c35579`(发布版 2026.9.2.1)。
> 目标版本 **2026.9.3.1**。单 PR 落地。

> **For agentic workers:** 步骤是 `- [ ]` checkbox。每个 D 有自己的差分验收,
> **先让差分在旧二进制上失败**,再让它在新二进制上通过。

## Global Constraints

- 构建/测试只用 `mcpp build` / `mcpp test`,**不要**裸 xmake,**不要** `mcpp clean`。
- 工具链 `gcc@16.1.0`;链接报 glibc/musl 错时先 `xlings use gcc@16.1.0`。
- **模块实现单元里禁止 range adaptor 管道**(`views::split`、`transform | ranges::to`);
  `std::ranges::any_of` / `find_if` 安全。
- 新增 e2e 必须注册进 `tests/e2e/run_all.sh`。
- commit 一律 `git commit -F -` + 带引号 heredoc(`-m` 里的反引号会被执行)。
- 断言只断言不变量,不断言索引版本号 / pointer revision / 快照条数。
- 真实 home 上的验证走 slice(`.agents/tools/slice-real-home.sh`),跑完
  `verify-untouched`。
- 真机生态验证用 `xlings subos <name> --sandbox --cmd "..."`。

---

## 任务依赖图

```
D0 路由表模块 (shim_table)  ─┬─→ D2 删 mirror ─┐
D1 knownProjects            ─┘                 ├─→ D8 测试 ─→ D9 文档/版本 ─→ PR/CI ─→ release ─→ 真机验证
                             └─→ D3 doctor 合并 ┤
                             └─→ D5 诊断归属   ─┘
D4 透传 (shim dispatch)     ────────────────────┤   (与 D0 无耦合,可并行)
D6 commands/packages 计数    ────────────────────┤   (独立)
D7 legacy alias Windows 判据 ────────────────────┘   (独立,小)
```

**可并行起步:** D0 / D1 / D4 / D6 / D7。
**串行依赖:** D2、D3、D5 必须等 D0(+D1)。

---

## D0 · 路由表模块(唯一写者)

新增 `src/core/xvm/shim_table.{cppm,cpp}`。放在 `xvm/` 而不是 `xself/` 或 `subos/`:
它是版本↔命令名的映射,属于 xvm 的语域;也为 §8.5 的模块抽取铺路。

- [ ] `DesiredSet compute_desired(...)` — 本 subos 的 active program(`kind == "program"`,
      排除 binding-root-only)∪ 已知项目现算的命令名 ∪ BUILTIN
- [ ] `ActualSet scan_actual(binDir, entryBinary)` — 只收**确实是我们的 shim** 的文件,
      判据 `std::filesystem::equivalent(candidate, entryBinary)`(symlink 跟随 +
      hardlink 文件标识,一次调用两平台都对);其余记为 `foreign`
- [ ] `TableDiff plan(desired, actual)` — `{toAdd, toRemove, foreign}`,平台后缀用
      `shim_filename()`(从 doctor 提取共用)
- [ ] `TableReport apply(diff, binDir, entryBinary)` — 增用 `create_shim`
      (已含 `displace_locked_file`),**删也用 `displace_locked_file`**(今天是裸
      `fs::remove`);失败**计数并报告**,不吞
- [ ] 差集应用,**绝不清空重建**(避免 PATH 空窗 + 单文件占用毁掉整次)
- [ ] 调用方持 state lock(与 workspace 写同一把锁)

**差分验收:** 单元测试 —— 造一个含 3 个 desired、2 个 stale、1 个 foreign 的目录,
断言 diff 精确、foreign 不被删。旧二进制没有这个模块,测试不存在即为「先失败」。

## D1 · knownProjects

- [ ] `~/.xlings/.xlings.json` 新增 `knownProjects: { "<abs path>": {"lastSeen": "..."} }`
      —— **只存路径,不缓存命令名**(缓存会过期,见设计 §6.3)
- [ ] `Config::known_projects()` / `Config::register_known_project(dir)`
- [ ] project scope 的 `install` 成功后登记/刷新
- [ ] 重建时对每个键读 `<key>/.xlings/.xlings.json` 现算命令名;不存在或不可解析 →
      丢弃该项目并在报告里说明(跳过而非让整次失败,同 `load_subos_snapshots` 政策)

**差分验收:** e2e —— 项目 A 安装后 `knownProjects` 有它;删掉项目目录后 `doctor --fix`
把它的命令名清出所有 subos 的表。

## D2 · 删 mirror(依赖 D0)

- [ ] 删 `src/core/common.cpp` + `src/core/common.cppm`
- [ ] 三个调用点改为走 D0:`xim/installer.cpp:2009`、`xvm/commands.cpp:827`、
      `xim/libxpkg/types/script.cpp:74`
- [ ] 删 `src/core.cppm:12` 的 `export import xlings.core.common;`
- [ ] 删 `xim/libxpkg/types/subos.cpp:6` 的**无用 import**(实测从不用 `common::`)

**差分验收:** e2e —— 在隔离 home 里于项目内 `xlings install`,断言全局 subos bin
**零新增文件**。旧二进制上此断言失败。

## D3 · doctor 规则合并(依赖 D0)

- [ ] 删 Check 1(active → shim)与 Check 2 / `FindingKind::OrphanShim`
      (`doctor.cpp:838-882`、`2191-2197`、`2824`、`3041`、`3273`)
- [ ] 新增 `FindingKind::ShimTableDrift`,per subos 一条,detail 列出要加/要删的名字
- [ ] `--fix` 应用差集;**删,并打印清单,不问**
- [ ] `foreign` 单独报告为 notice,不动文件

**差分验收:** 在真实 home 的 slice 上跑 `doctor --fix`,断言那 23 个名字被移除且
报告列出它们;`verify-untouched` 必须 OK。

## D4 · 透传(独立)

- [ ] `shim.cpp` 的 PATH 遍历加**自排除**:`<home>/bin`、`<home>/subos/*/bin`、
      项目 `.xlings/subos/*/bin`,用**路径前缀**判断(Windows 硬链接下 `is_same_file`
      不可靠);`XLINGS_SHIM_DEPTH` 保留兜底
- [ ] 透传**只在 `here.empty()` 那一支**(`not_in_subos`)发生;
      `xvm.no_active_version` 与 `xvm.pinned_version_missing` 必须报错
- [ ] `stderr` 是 TTY 时打印一行 `[xlings] <name>: no version in subos '<n>'; running <path>`;
      非交互静默
- [ ] Windows 的 PATHEXT 复用 `resolve_alias_program`(`shim.cpp:255-259`)

**差分验收:** e2e —— 隔离 home,项目声明 node、全局没有;项目外敲 node 得到宿主机的
(rc=0),`installed[]` 有但没 active 时必须 rc≠0。

## D5 · 诊断归属(依赖 D1)

- [ ] `not_in_subos` 的 remedy 按来源分支:项目提供的说明是**哪个项目**;
      全局装用 `xlings install -g <pkg>`;不再输出 `xlings install slang` 这种不可执行建议

**差分验收:** 断言输出里含项目路径,且不含裸 `xlings install <target>`。

## D6 · commands / packages 计数(独立)

- [ ] `subos.cpp:70-82` 与 `1453-1462` 的字段改名:面板 `tools` → **`commands`**、
      JSON `pkgCount` → **`commands`**
- [ ] 新增 **`packages`** —— 从 workspace 的 release 层面算
      (`profile.cpp` 的 `load_subos_snapshots`),每个 subos 多读一个 JSON
- [ ] `capabilities.cpp` 的对应 payload 同步

**差分验收:** 真实 home 上 `default` 从 `tools: 121` 变成
`commands: <准确值> packages: <release 数>`。

## D7 · legacy alias 的 Windows 判据(独立,小)

- [ ] `compact/xself.cpp:29-38` 的 `is_legacy_alias_symlink_to_bootstrap` 去掉
      `fs::is_symlink` 前置,改用 `std::filesystem::equivalent` —— Windows 上 shim 是
      hardlink,该判据恒 false,**四个调用点在 Windows 上从来没生效过**

**差分验收:** 单元测试 —— 用 hardlink 造一个 alias,断言判据为真(旧实现为假)。

## D8 · 测试

- [ ] 单元:D0 的 diff / foreign;D7 的判据
- [ ] e2e:D2 的零泄漏、D1 的项目回收、D4 的透传三态、D6 的计数
- [ ] **全部新增 e2e 注册进 `tests/e2e/run_all.sh`**
- [ ] 一条测试锁住语义:「`bin/` 里存在文件 ⇏ 该名字有 active version」

## D9 · 文档 / 版本 / 发布

- [ ] `mcpp.toml` 版本 → `2026.9.3.1`
- [ ] 更新 `src/core/xvm/README.md`(路由 vs 状态的口径)
- [ ] 设计文档状态改「已实现」,§3 补实施中被推翻的判断
- [ ] 单 PR;CI 全绿
- [ ] 自我 review 一次
- [ ] release + `gtc` 补 GitCode 资源
- [ ] 真机生态验证:`xlings subos <name> --sandbox --cmd "..."`
