# 依赖名按声明方解析 + SubOS 用户数据保护:实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** 实现 `.agents/docs/2026-09-25-dep-name-resolution-optimization-plan.md` 的全部已定项(W0–W5、S1–S9,D1–D9、D2'),
xlings 侧一个 PR、一个版本(2026.9.26.1),xim-pkgindex 侧一个修复 PR + 一个发布 bump PR。

**Architecture:** 依赖解析新增一个带声明方的入口,结果以边记在 PlanNode 上,安装器/反查只读边;
显式条目 = 已声明子索引时按同一仓库解释(一个目录、子索引层级);subos 整体删除只能由持有
`UserConfirmed` 的调用者执行,自动流程拿不到它。

**Tech Stack:** C++23 modules(mcpp,gcc@16.1.0 / llvm@20.1.7 / musl / MSVC),gtest,bash e2e,Lua(pkgindex CI)。

**Spec:** `.agents/docs/2026-09-25-dep-name-resolution-optimization-plan.md`

## Global Constraints

- 版本号 `2026.9.26.1`(N 从 1 开始),改 `mcpp.toml` 和 `src/core/config.cppm` 两处,`mcpp build` 后用 `--version` 验证。
- NDJSON 协议 v1.0 只做**新增**:`remove_subos.yes`、`create_subos.yes`;删掉 `noDeps` 字段描述并在传 `true` 时拒绝。不改 `ErrorEvent` 结构(依赖链写进 message,沿用 `resolver.cpp:140` 的 `a -> b` 格式)。
- 不改 libxpkg。
- 加载配置时不改写用户配置文件;只读命令不写入。
- 需要确认但没人回答:exit 2,不改任何东西(沿用 `confirmed_or_refused_` 的约定)。
- 跨平台:目录遍历用 `!= std::default_sentinel`;挂载检查只在 Linux 生效;新代码先过 `mcpp build --toolchain llvm@20.1.7`。
- e2e 一律隔离 home、`XLINGS_TEST_MIRROR=CN`,新 e2e 注册进 `tests/e2e/run_all.sh`。
- 提交信息用 `git commit -F -` + 单引号 heredoc。

## Review Focus

1. **两个同级仓库同名、声明方仓库也有同名** —— 必须选声明方的,不能报歧义(W1 规则 2)。测试在 B1。
2. **声明方仓库有这个名字但没有要求的版本 / 没有本平台构建** —— 报错点名该仓库,不能换到别的仓库(W1 规则 4)。测试在 B1、B2。
3. **非终端下 `subos remove X` 不带 `-y`** —— 什么都不删,exit 2,消息里有路径和数据量,提示 `-y`。测试在 B10 + e2e。
4. **NDJSON `remove_subos` 不带 `yes`** —— 返回给 agent 一段说明(会删什么、多大、怎样确认),不删。测试在 B10 + unit interface。
5. **显式 scode 条目 + 已声明子索引** —— 只同步一个目录,`index use scode <ver>` 的固定作用在那个目录上。测试在 B6 + e2e。

---

## 任务依赖

```
仓库 A  xim-pkgindex
  A1 W0 xmake 依赖加前缀 + 删过期豁免          (独立,最先合并 → 老客户端受益)
  A2 W5 check-dep-namespace 豁免过期 + 子索引冲突  (独立)
  A3 发布后 bump pkgs/x/xlings.lua             (依赖 B-release)

仓库 B  xlings(单 PR,按任务分提交)
  B1 catalog.resolve_dependency ──► B2 resolver 边/去重/链 ──► B3 installer 读边
                                         └──────────────► B4 dependents 读记录
  B5 noDeps 拒绝                                         (独立)
  B6 仓库身份(同一目录/层级) ──► B7 写入口 + --rm-index-repo ──► B8 doctor 索引缓存
  B9 确认 + UserConfirmed + delete_subos + B14 删除记录 ──► B10 subos remove/create
                                                   ├──► B11 doctor subos 配置/未登记目录
                                                   └──► B13 self install/uninstall/clean
  B12 GC 拒绝(用 B14 记录)
  B15 e2e + lint(随各组补齐) ──► B16 文档/规范 ──► B17 版本号 ──► PR/CI ──► 自审 ──► 发布
发布后:gtc 补 GitCode 资源 ─► A3 ─► 真机验证(subos --sandbox,mirror CN)
```

并行:仓库 A 不需要编译,和仓库 B 并行。仓库 B 受磁盘限制(剩 ~27G,一份构建 ~8G)只在一个工作树里串行构建。

---

### B1 catalog:按声明方解析依赖

**Files:** `src/core/xim/catalog.cppm`、`src/core/xim/catalog.cpp`;测试 `tests/unit/test_xim_catalog.cpp`

**Produces:**
```cpp
struct DeclaringRepo { std::string repoName; PackageScope scope { PackageScope::Global }; };
std::expected<PackageMatch, std::string>
PackageCatalog::resolve_dependency(const std::string& spec,
                                   const DeclaringRepo& declarer,
                                   const std::string& platform) const;
std::string format_ambiguous_dependency(std::string_view spec, std::string_view declarer,
                                        std::span<const PackageMatch> matches);
```
规则:显式前缀 → `resolve_target`;声明方仓库里 `find_candidates(name)` 非空 → 只在该仓库解析,
版本/平台不满足就报错(点名 `<repo>` 和包);为空 → `resolve_target`,歧义时改用
`format_ambiguous_dependency`(不给 `install <候选>`,给"recipe 应写前缀"和 `config --rm-index-repo`)。

- [ ] 测试:fixture 两个同级 repo R1、R2 都有 `libx`,R1 有 `app` 依赖 `libx`:`resolve_dependency("libx",{R1})` → `R1:libx`;`resolve_target("libx")` 仍歧义;R1 无 `libx` 时退回全局(歧义消息不含 `xlings install`);R1 有 `libx` 但无 `@9` → 报错且不返回 R2 的。
- [ ] 实现并通过;`mcpp test`。

### B2 resolver:声明方、边、去重、依赖链

**Files:** `src/core/xim/libxpkg/types/type.cppm`(`DepEdge`、`PlanNode::depEdges`)、`src/core/xim/resolver.cpp`;测试 `tests/unit/test_xim_catalog.cpp`

**Produces:** `struct DepEdge { std::string spec; DepKind kind; std::string nodeKey; };` `PlanNode::depEdges`。
- expand 对依赖用 `resolve_dependency(dep, {node.repoName, node.scope})`;pin 失败时在同一声明方下退回不 pin。
- 失败消息 `"<path a -> b> -> <spec>: <error>"`,同一消息只推一次。
- topoVisit 只读 `depEdges`,删掉重新解析。
- [ ] 测试:R1:app → libx 的 plan 含 `R1:libx`,`depEdges[0].nodeKey == "R1:libx@1.0"`;依赖歧义时 `plan.errors.size()==1` 且含 `->`。

### B3 installer:按边取依赖节点

**Files:** `src/core/xim/installer.cpp`(`deps_exports`/`resolved_deps` 循环、`locate_dep_install_dir_`)、`installer.cppm`;测试 `tests/unit/test_xim_install.cpp`
- 按 `depEdges` 的 `nodeKey` 找 plan 节点;没有边(旧构造的 plan)时退回原按名字匹配。
- [ ] 测试:plan 含 `a:libx@1` 与 `b:libx@1`,节点的边指向 `b:libx@1` → `locate_dep_install_dir_` 返回 `b-x-libx/1`。

### B4 remove 前依赖方反查读解析记录

**Files:** `src/core/xim/commands.cpp`(`direct_dependents_of`)
- 有 `.xlings-resolution.json` 时按记录里的 `name`(canonical)比较;没有时退回按裸名比较。
- [ ] 测试(unit):记录写 `xim:ncurses` 的消费者,不被报成 `scode:ncurses` 的依赖方。

### B5 noDeps

**Files:** `src/capabilities.cpp`;测试 `tests/unit/test_interface_protocol.cpp`
- schema 去掉 `noDeps`;`noDeps:true` → ErrorEvent `E_INVALID_INPUT`,hint 说明不支持、依赖总会安装,exit 2。

### B6 仓库身份:声明过的子索引只算一个仓库

**Files:** `src/core/xim/repo.cppm/.cpp`、`src/core/xim/catalog.cpp`(`repo_specs_`)、`src/core/xim/index_cmd.cpp`
**Produces:**
```cpp
enum class IndexEntryKind { Independent, DeclaredSubIndex, NameCollision };
IndexEntryKind classify_index_entry(const IndexRepo& repo);        // 读 declared_sub_index_url
std::filesystem::path effective_repo_dir(const IndexRepo& repo, bool projectScope);
```
- sync:全局条目里 `DeclaredSubIndex` 的不单独同步;子索引同步时把同名显式条目的 `version`/`artifactBases`/`source` 叠加上去。
- catalog:`DeclaredSubIndex` 的显式条目不作为一级仓库加入。
- `collect_index_sources` 用 `effective_repo_dir`。
- [ ] 测试:classify 三种情况;`effective_repo_dir` 指向 `xim-index-repos/<dir>`。

### B7 写入口 + `config --rm-index-repo`

**Files:** `src/cli.cpp`、`src/capabilities.cpp`、`src/cli/spec.cpp`(+ i18n)
- `--index-repo` / `add_repo`:`NameCollision` 拒绝;`DeclaredSubIndex` 接受并提示"仍是子索引"。
- `config --rm-index-repo <name>`:按名删除条目;不存在则报错 exit 1。

### B8 doctor:重复的索引缓存目录

**Files:** `src/core/xself/doctor.cpp/.cppm`
- 发现 `DeclaredSubIndex` 条目对应的 `data/<name>` 目录存在 → Notice "reclaimable index cache";`--fix` 删除(派生数据,写删除记录)。`NameCollision` → Warning,无 `--fix`。

### B9 确认、UserConfirmed、delete_subos、删除记录(含 S8)

**Files:** 新建 `src/core/confirm.cppm/.cpp`(把 `commands.cpp:85 confirmed_or_refused_` 移过来)、新建 `src/core/destructive_log.cppm/.cpp`、`src/core/subos.cpp`
**Produces:**
```cpp
namespace xlings::confirm {
class UserConfirmed { /* 私有构造 */ public: std::string_view how() const; };  // "terminal" | "-y" | "yes:true"
std::optional<UserConfirmed> ask(EventStream&, std::string id, std::string question,
                                 std::string_view what, bool yes, std::string_view yesSpelling, int* rc);
}
namespace xlings::destructive_log {
void record(std::string_view op, const std::filesystem::path& path,
            std::uintmax_t bytes, std::uintmax_t files, std::string_view confirmedBy);
}
namespace xlings::subos {
struct TreeCensus { std::uintmax_t homeBytes, homeFiles, otherUserFiles; };
TreeCensus census(const std::filesystem::path& dir);
std::expected<void, std::string> delete_subos(const std::filesystem::path& dir,
                                              const confirm::UserConfirmed&);
}
```
- `delete_subos`:目标必须是 `<home>/subos/` 直接子目录或登记的自定义目录,非根、非 `current`、非符号链接;Linux 下 `/proc/self/mountinfo` 中目录下有挂载点则拒绝;写删除记录。
- `census`:`home/` 字节与文件数;home 之外非链接、非 shim 的普通文件数(D9)。

### B10 subos remove / create

**Files:** `src/core/subos.cpp/.cppm`、`src/capabilities.cpp`
- `remove(name, yes, stream)`:确认内容 = 路径 + census;没人回答且无 yes → ErrorEvent(message 写明会删什么和数据量,hint:"this needs the user's confirmation — if the user asked for it, call again with `yes: true`" / CLI 为 `-y`),exit 2。
- CLI `subos remove` 读 `-y`/`--yes`。
- `create`:目录已存在且未登记 → 同样确认"收养";输出 `adopted`;回滚只删本次新建的条目。
- [ ] 修改所有删除 subos 的现有测试加 `-y` / `yes:true`。

### B11 doctor:subos 配置损坏、未登记目录

**Files:** `src/core/xself/doctor.cpp`
- SubosUnreadable:`--fix` 把坏文件改名 `.xlings.json.corrupt-<UTC>`,写最小配置(Describe manifest + 空 workspace);remedyNote 去掉 `subos remove`。
- 新 finding:未登记的 subos 目录(含数据量),remedy `xlings subos new <name>`,无 `--fix`。

### B12 GC

**Files:** `src/core/profile.cpp`
- `collect_subos_references_` 收集 unreadable;有 unreadable 或全局 versions 解析失败 → 拒绝 GC 并列出;路径按分量解析找 `xpkgs`。

### B13 self install / uninstall / clean

**Files:** `src/core/xself/install.cpp`、`uninstall.cpp`、`clean.cpp`
- install 第 1 步只删发布包里有的顶层条目;覆盖分支用 B9 的确认 + 逐个 `delete_subos`。
- uninstall 确认内容列出各 subos 的 home 数据量。
- clean:`homeDir == $HOME` 或 `.xlings` 不是旧缓存布局时跳过并说明。

### B15 e2e + lint

- `tests/e2e/dep_declarer_scope_test.sh`(AC1、AC1b、AC2、AC2b、AC4–AC7)
- `tests/e2e/subos_user_data_test.sh`(AC10–AC18、AC20)
- `tools/lint_subos_remove_all.sh`(AC19)接进 `xlings-ci-linux.yml`
- 注册 run_all.sh;现有测试补 `-y`。

### B16 文档规范

AGENTS.md(subos 用户数据规则 + 依赖按声明方解析)、`docs/spec/interface-ndjson-v1.md`、
`docs/quick-start/subos-and-agent.md`、`.agents/skills/xlings-usage/SKILL.md`、`docs/generated/command-reference.md`、
方案文档里"依赖链写进 message"的更正、发布说明。

### B17 版本号 2026.9.26.1

---

### A1 xim-pkgindex W0

- `pkgs/x/xmake.lua:98` → `"xim:ncurses"`;删 `check-dep-namespace.lua` 的 xmake 豁免;改 87–96 行注释。
- 验证:`lua .github/scripts/check-dep-namespace.lua --check .` 通过。

### A2 xim-pkgindex W5

- 豁免过期:被豁免的名字在 base 分支 `pkgs/` 存在时失败(CI 里 `git cat-file -e origin/main:pkgs/<c>/<name>.lua`)。
- 冲突集合加入 `xim-indexrepos.lua` 声明的子索引的包名(CI 浅克隆)。

### A3 发布后 bump `pkgs/x/xlings.lua`(全部平台 + latest + sha256)

---

## 验证与发布

1. `mcpp build` / `mcpp test` / `mcpp build --toolchain llvm@20.1.7` / `mcpp build --target x86_64-linux-musl`。
2. `XLINGS_TEST_MIRROR=CN bash tests/e2e/run_all.sh`(或按 CI 的方式跑新增与受影响的测试)。
3. 推 PR,等 Linux/macOS/Windows CI 全绿。
4. 自审(整个 diff 过一遍 + code-review)。
5. 以 Sunrisepeak squash 合并;`gh workflow run release.yml --ref main`。
6. release 资源一出现就本地 `bash tools/mirror-latest.sh xlings`(gtc)补 GitCode,GET 验证。
7. A3 bump 合并,等索引发布。
8. 真机验证:`xlings config --mirror CN`,`xlings self update`,然后在 `xlings subos new <v> && xlings subos use <v> --sandbox --cmd "..."` 里验证:xmake 能装(显式 scode 配置下)、`subos remove` 需要确认、doctor/GC 行为、`index use scode`。
