# 子索引首次同步修复 + 安装期按需索引刷新 + 子索引构建进度标准 — 跨仓库方案

**日期**: 2026-07-14
**类型**: 方案 / 架构 (design)
**状态**: Design draft, 待 review
**范围**: 修复 [#366](https://github.com/openxlings/xlings/issues/366)(首次运行子索引从不同步);为 `xlings install` 增加"包/版本不在当前索引 → 自动触发一次索引刷新并重试"的按需机制;把子索引构建脚本(`pkgindex-build.lua`)的**单行进度输出**固化为跨仓库开发规范。覆盖 `openxlings/xlings` + 全部 `xim-pkgindex-*` 子索引仓。
**关联代码/脚本**:
- `src/core/xim/commands.cppm:32-55`(`get_catalog`)、`:83-96`(`cmd_install` 入口 `is_loaded` 闸)、`:123-160`(resolve 失败路径)、`:928-940`(`cmd_update`)
- `src/core/xim/catalog.cppm:351-375`(`rebuild`)、`:173-239`(`repo_specs_`,`exists(subDir/"pkgs")` 卫语句)、`:397-409`(`resolve_target`,唯一 "not found" 节点)
- `src/core/xim/repo.cppm:473-645`(`sync_all_repos`)、`:451-453`(`discovered_global_sub_repos`)
- `xim-pkgindex-{scode,awesome,d2x}/pkgindex-build.lua`(子索引构建 + 进度输出)
- `src/core/config.cppm:16`(`VERSION`,当前 dev `0.4.62`;最新 tag `v0.4.64`)

**关联文档**:
- `.agents/docs/2026-06-25-index-ecosystem-unification-plan.md`(主+默认子索引 artifact 统一;§9 验收 #1 "全新安装全部 artifact-managed" 正是 #366 打不住的路径)
- `.agents/docs/2026-06-30-index-artifact-git-regression-analysis.md`(相邻的 sync/迁移闸回归,0.4.62 修复;本方案沿用其"迁移闸/回退非破坏"思路)
- `docs/design/index-distribution.md`(index-as-resource / Y-asset 获取、CN 设计、发布流)
- `docs/design/package-index-ecosystem.md`(三层源模型、命名空间解析、`sync_all_repos` 顺序)
- `.agents/docs/2026-06-24-pkgindex-publish-decoupling-ci.md`(子索引发布解耦、指针 merge、客户端低成本刷新)

---

## 0. TL;DR

`xlings install gcc`(或任何依赖 `scode:`/`awesome:`/`d2x:` 命名空间的包)在**全新机器**上报
`[error] package 'scode:linux-headers@5.11.1' not found`。根因:**子索引在首次运行时从不同步**——
`get_catalog()` 只在 `mgr.rebuild()` **失败**的兜底分支里调 `sync_all_repos()`,而主索引存在时
rebuild 永远成功(子索引缺失只是"少几个仓",不构成失败),兜底永不触发,子索引 `pkgs/` 从未被拉取。
`xlings update` 能修是因为它**无条件** `sync_all_repos(true)` 一次(§4 workaround)。

本方案三件事(对应用户三条诉求):

1. **修 #366(P0 bugfix,client)**:`get_catalog()` 首次构建时,若"默认子索引尚未落地"(标记 JSON 缺失),
   **无条件同步一次**再 rebuild——一处修全命令(install/use/update/…)。
2. **安装期按需刷新(P1 UX,client)**:`cmd_install` 解析到"包/版本不在当前索引"时,
   **先触发一次索引刷新再重试解析一次**(而非直接报错退出)。索引天生有 TTL/滞后,
   这让"刚发布的新包/新版本"无需用户手动 `xlings update` 即可安装。
3. **子索引构建进度规范(跨仓)**:把 scode/awesome/d2x 的 `pkgindex-build.lua` 现有"**每文件一行**"
   (N 个包 = N 行)**收敛为一条 `\r` 原地自刷新的进度条**——`只要一行,不用多行`,单行实时显示
   `[n/N] <ns>::<file>` 当前正在处理的包文件;**只有报错时才换行**打红色错误并中断(`除非报错`)。
   固化为开发规范,同步三仓、写进新增子索引模板。

三者互补:**#1 修系统性首次缺口(所有命令),#2 让存量/陈旧索引在 install 时自愈,#3 让子索引构建输出清爽单行且报错可诊断。**

### 0.1 已拍板决策(建议,待 review 确认)

| 决策点 | 选择 | 备选(未采纳) |
|---|---|---|
| #366 修复位置 | `get_catalog()` 首次构建加"子索引标记缺失 → 同步一次"(修全命令) | 只在 `cmd_install` 修(漏 use/其它命令) |
| 首次同步的触发条件 | **仅当** `xim-indexrepos.json` 标记缺失(便宜判定,常规运行零开销) | 每次首访无条件 sync(拖慢正常启动) |
| 安装期按需刷新触发 | resolve "not found"(非 ambiguous)→ 刷新 + 重试**一次**,带**进程内冷却**去抖 | 每次 miss 都刷(弱网/真缺包时反复卡顿) |
| 按需刷新的刷新粒度 | 复用 `sync_all_repos(true)` + `rebuild(true)`(与 `xlings update` 同款) | 只刷单个命名空间(实现复杂,收益小) |
| 进度输出形态 | **单行 `\r` 原地自刷新**进度条(`cprintf`+`\r`+清行,收尾换行)【用户拍板】 | 每文件一行(N 包 = N 行,现状) |
| 进度错误分支 | `try/catch` 包裹 append(沙箱无 pcall),失败**换行**打 `${red}` 行 + `raise` 中断构建(除非报错) | 静默跳过(丢失可诊断性) |
| 版本切分 | `0.4.65` = P0(#366)+ 进度规范;`0.4.66` = P1(按需刷新) | 全塞 0.4.65(bugfix 与 feature 混发,回滚粒度差) |

---

## 0.5 实施进度与验证(2026-07-14,实现期 live)

**全部三部分已实现并本机验证通过**(评审后按 goal「开始实现 + 整个生态打通」执行)。

| 项 | 状态 | 落点 | 验证 |
|---|---|---|---|
| **C1** get_catalog 首次同步(#366) | ✅ 已实现 | `commands.cppm:32-63` + `repo.cppm sub_indexes_initialized()` | e2e `install_subindex_first_run_test.sh` 绿;实测 C1 **单独**即解析(无 C2 兜底) |
| **C2** install 按需刷新 + 去抖 | ✅ 已实现 | `commands.cppm` `cmd_install`(resolve 失败分支)+ `index_refresh_cooldown_elapsed()` | e2e `install_refresh_on_missing_test.sh` 绿;实测「刚发布 newpkg」刷新后命中 |
| **C3** 子索引 `\r` 单行进度 + 错误分支 | ✅ 已实现(3 仓) | `xim-pkgindex-{scode,awesome,d2x}/pkgindex-build.lua` | `luac -p` 通过;xmake lua 实跑:成功单行自刷新 + 宽度左截断、坏文件红行 + `raise` 非零退出(255) |
| VERSION bump | ✅ | `config.cppm:16` → `0.4.65` | `xlings --version` = 0.4.65 |
| 无回归 | ✅ | — | 既有 `sub_index_install_test.sh` 绿(命名空间 + bare name) |
| 构建 | ✅ | `mcpp build` → `Finished release in 42s` | 全新指纹全量重编,零 error |

**验证环境要点(重要,需你知悉)**:
- **本分支 `fix/issue356-transactional-cache` 的 `mcpp.toml` 依赖 pin 落后于自身源码**,导致**改前就无法构建**(与 #366 无关):
  源码用 `tinyhttps::DownloadToFileResult::expectedBytes`(需 `tinyhttps 0.2.9`)、`import mcpplibs.xpkg.compat`(需 `xpkg 0.0.44`),
  而 `mcpp.toml` 仍 pin `tinyhttps 0.2.8` / `xpkg 0.0.42`。`main` 已是 `0.2.9`/`0.0.44`。为出可验证构建,我把两 pin **对齐到 main**
  (`mcpp.toml`)。**这属于分支存量断裂修复,非 #366 改动**——建议本分支 rebase/merge main 后由该分支自行处置(见 §9.4)。
- **xim 脚本沙箱无 `pcall`/`dofile`/`error`**(实测),C3 因此用 xmake `try/catch` + `raise`;`os.getwinsize` 无 tty 时返回 `32767` 哨兵,须 `<1000` 过滤。

---

## 1. 现状(精确核实)— 已建成 vs 缺口

### 1.1 已经建成的(不要重复造)

| 能力 | 状态 | 证据(file:line) |
|---|---|---|
| `sync_all_repos` 全量同步主+子索引(artifact 优先,git 兜底) | ✅ | `repo.cppm:473-645`;写 `xim-indexrepos.json`(`:613`) |
| `xlings update` 无条件 `sync_all_repos(true)` + `rebuild(true)` | ✅(即 #366 的 workaround) | `commands.cppm:928-940` |
| `cmd_install` 有 `!is_loaded()` 闸会 sync | ⚠️ 存在但**首次永不触发**(见 1.2) | `commands.cppm:88-96` |
| 命名空间解析 `scode:linux-headers@5.11.1` | ✅ | `catalog.cppm:84-104`(parse)、`:316-348`(collect) |
| 唯一 "not found" 判定节点 | ✅ | `catalog.cppm:400`,`"package '{}' not found"` |
| 子索引构建:`pkgindex-build.lua` append `template.lua` | ✅ | `xim-pkgindex-{scode,awesome,d2x}/pkgindex-build.lua` |
| 进度输出 `[n/N] <ns>::<file>` | ⚠️ 现为**每文件一行**(N 包=N 行,三仓一致),本轮改 `\r` 单行 | 同上 `install()` 循环内 `cprint` |
| 子索引 artifact 发布(手动 + 发版兜底) | ✅ | `xim-pkgindex/.github/workflows/publish-sub-indexes.yml`、`release.yml` |

### 1.2 缺口

**缺陷 A —— 子索引首次运行从不同步(#366 核心)**
`get_catalog()`(`commands.cppm:32-55`)只在兜底触发 sync:
```cpp
auto result = mgr.rebuild();          // :36
if (!result) {                        // :37 —— 仅失败才进
    log::warn("catalog build failed ({}); resyncing indexes...", result.error());
    if (sync_all_repos(true)) {       // :44 —— 唯一的首次 sync 机会
        result = mgr.rebuild(true);   // :45
    }
    ...
}
```
但 `rebuild()`(`catalog.cppm:351-375`)对子索引是**加法**:`repo_specs_()`(`:173-239`)只为
**磁盘已存在**的子仓建 spec,且逐个 `if (exists(subDir/"pkgs"))` 卫语句(`:199/:214`)。全新机器上
主索引在、子索引 `pkgs/` 不在 → 子仓被跳过 → **rebuild 成功、`loaded_=true`**、兜底永不触发 →
`scode` 根本不在 catalog → `collect_matches_` 空 → `resolve_target` 报 `not found`。

**缺陷 B —— `cmd_install` 的 `is_loaded` 闸对首次是空转**
`commands.cppm:88-96` 的 `if (!catalog.is_loaded())` 里其实**也**调了 `sync_all_repos(true)`——
但 `is_loaded()` 返回 `loaded_`(缺陷 A 里已是 `true`),闸永不进入。**第二处漏网。**

**缺陷 C —— resolve 失败即退,无按需刷新(UX;#366 的显性症状 + 用户诉求 3)**
`cmd_install` 显式命名空间解析失败时直接 `log::error + return 1`(`commands.cppm:132-143`),
**不重试、不刷新**。这既是 #366 的报错出口,也是"包/版本刚发布但本地索引陈旧"时的体验断点。

**缺口 D —— 进度输出多行 + 缺错误分支**
现有循环**每文件一行** `cprint`(N 包 = N 行刷屏),且只有 `${green}`(写入)/`${yellow}`(skip)两支,
`io.writefile`/模板解析异常无任何提示。与用户诉求"**只要一行不用多行**、打印具体包文件(**除非报错**)"
不符——应收敛为一条 `\r` 自刷新单行,且报错时换行打红色行并中断。

---

## 2. 目标架构

```
                         xlings install <target>
                                  │
                    ┌─────────────▼──────────────┐
                    │ get_catalog() [首次]         │   ← P0(#366)
                    │  rebuild()                   │
                    │  若 xim-indexrepos.json 缺失 │   ← 新增:默认子索引从未落地
                    │    → sync_all_repos(true)    │      则无条件同步一次
                    │    → rebuild(true)           │
                    └─────────────┬────────────────┘
                                  │  (子索引现已在 catalog)
                    ┌─────────────▼──────────────┐
                    │ resolve_target(target)      │
                    │   命中 → 安装                │
                    │   not found(非 ambiguous)   │   ← P1(按需刷新)
                    │     且本轮未刷过 & 过冷却    │
                    │       → sync_all_repos(true) │      "包/版本不在当前索引 → 先刷新"
                    │       → rebuild(true)        │
                    │       → resolve_target 重试1 │
                    │   仍 not found → 原错误/fuzzy │
                    └──────────────────────────────┘

  发布侧(子索引内容,进度规范在此产生输出):
    xim-pkgindex-{scode,awesome,d2x}  push pkgs/**
       → publish(主仓 publish-sub-indexes.yml / 发版兜底)
       → 运行 pkgindex-build.lua.install():
            for file in pkgs/**.lua:
              pcall(append template)
              成功 → cprintf("\r[${green}n/N${clear}] <ns>::"..file.."\x1b[K")   ← 单行原地自刷新(只要一行)
              失败 → print("") ; cprint("[${red}n/N${clear}] <ns>::"..file.." ERROR:"..err) ; raise  ← 除非报错才换行
            收尾 print("") 保留最终进度行
       → 打包 artifact + 移指针(内容哈希 immutable,详见 index-distribution.md)
```

**与既有架构的一致性**:P0/P1 完全复用 `sync_all_repos(bool)` + `catalog.rebuild(true)`——
即 `cmd_update` 已用、已经过实战的那对原语;失败仍走 git 兜底(`repo.cppm:511-513`),零新风险面。
不引入新的传输/签名机制(index-as-resource / minisign 归属既有方案,本轮不动)。

---

## 3. 多仓库角色与协作

| 仓库 | 角色 | 本方案改动 | 谁来改 | 版本/PR |
|---|---|---|---|---|
| **`openxlings/xlings`** | 客户端 C++ | P0 `get_catalog` 首次同步(缺陷 A/B);P1 `cmd_install` 按需刷新(缺陷 C);e2e | 本仓 PR | `0.4.65`(P0)、`0.4.66`(P1) |
| **`openxlings/xim-pkgindex-scode`** | 默认子索引内容 | 进度输出改 `\r` 单行自刷新 + 错误分支(缺口 D) | 跨仓 PR | 无版本号(内容仓,artifact 内容哈希) |
| **`openxlings/xim-pkgindex-awesome`** | 默认子索引内容 | 同上 | 跨仓 PR | 同上 |
| **`d2learn/xim-pkgindex-d2x`** | 默认子索引内容(跨 org) | 同上 | 跨仓 PR(需 d2learn 协调) | 同上 |
| **`openxlings/xim-pkgindex`** | 主索引 + 子索引声明 `xim-indexrepos.lua` | 无代码;新增子索引接入规范文档链接 | — | — |
| **`xim-pkgindex-fromsource` / `-ros2` / `-dragonos`** | 非默认/社区子索引 | **不改**(自包含 pkg / binary 发布,不走 template-append 模型);仅在采用 `pkgindex-build.lua` 时须遵循规范 | — | — |

**跨仓一致性约束(沿用 unification-plan §3,不满足则子索引静默退回 git)**:同一默认子索引的
① `xim-indexrepos.lua` URL、② 客户端 `xim-indexrepos.json` URL、③ 发布用 repo URL、④ 组合指针 key 名
必须对齐;key 名(`scode`/`awesome`/`d2x`)是 `repo.name`→指针 key 的唯一纽带。本方案不改这些
URL/key,仅确保 P0 首次 sync 后 `xim-indexrepos.json` 被正确写出(`repo.cppm:613`)。

---

## 4. 缺口与改造(逐项)

### C1 — `get_catalog()` 首次无条件同步默认子索引 【client / P0 / 修 #366】

**问题**:缺陷 A/B。**改动**(`commands.cppm:32-55`):在首次 `rebuild()` 成功后,判定"默认子索引是否
从未落地"——以 `xim-indexrepos.json`(`repo.cppm:sub_repos_json_path()`)**不存在**为便宜信号
(全新机器上必然缺失,一旦 sync 过即存在,常规运行零开销):

```cpp
if (!initialized) {
    auto result = mgr.rebuild();
    // #366: 全新机器主索引 rebuild 会成功,但默认子索引从未同步。
    // 标记 JSON 缺失 == 默认子索引从未落地 → 无条件同步一次再 rebuild。
    bool subIndexNeverSynced = !std::filesystem::exists(sub_repos_json_path());
    if (!result || subIndexNeverSynced) {
        if (!result) log::warn("catalog build failed ({}); resyncing indexes...", result.error());
        else         log::info("initializing sub-indexes (first run)...");
        if (sync_all_repos(true)) {
            result = mgr.rebuild(true);
        }
        if (!result) { log::error("failed to build catalog: {}", result.error());
                       log::info("try running: xlings update"); }
    }
    initialized = true;
}
```

> `sub_repos_json_path()` 若未 export,则在 `repo.cppm` 暴露一个 `bool sub_indexes_initialized()`
> 谓词供 `commands.cppm` 调用(不泄漏路径细节)。二选一,实现期定。

**收益**:一处修全命令;`cmd_install` 的 `is_loaded` 闸(缺陷 B)保持不变作为二次保险。
**风险**:近零——首次多一次 sync(用户本就要联网装包);失败仍 git 兜底。常规运行因 JSON 已存在而跳过。

### C2 — `cmd_install` "not found → 刷新 → 重试一次" 【client / P1 / 用户诉求 3】

**问题**:缺陷 C。**改动**(`commands.cppm:132-143`,resolve 失败、非 ambiguous 分支,`return 1` 之前):

```cpp
// 包/版本不在当前索引 → 可能是本地索引陈旧(新包/新版本刚发布)。
// 触发一次索引刷新并重试解析一次,再决定报错。带进程内去抖,避免真缺包时反复刷。
if (!match && !refreshed_for_missing && !match.error().contains("ambiguous")
    && index_refresh_cooldown_elapsed()) {
    log::info("'{}' not in current index; refreshing index...", target);
    refreshed_for_missing = true;
    if (sync_all_repos(true)) {
        catalog.rebuild(true);
        match = catalog.resolve_target(pinned, platform);
        if (!match && pinned != target) match = catalog.resolve_target(target, platform);
    }
}
if (!match) { /* 既有 ambiguous / explicit-ns / fuzzy 路径不变 */ }
```

- `refreshed_for_missing`:**每次 install 调用最多刷一次**(即使多 target),避免 N 个缺包 = N 次全量刷。
- `index_refresh_cooldown_elapsed()`:进程内/短 TTL 冷却(如 60s,读 artifact 指针 sha 命中即零下载,
  见 decoupling-ci.md §3),避免脚本循环里对真缺包反复触发全量刷。
- 对**显式命名空间**同样生效——这正是 #366 `scode:linux-headers` 的出口;刷新后若 scode 索引出现新版本即自愈。

**收益**:`xlings install <刚发布的包>` 无需用户先手动 `xlings update`;与 P0 叠加后,
即使 P0 已同步过、但**索引内容陈旧**(包已发布、本地未刷),install 仍能按需自愈。
**风险**:低——真缺包场景多一次刷新(有冷却去抖);逻辑局限在 install 失败分支,不改解析核心。

> **备选(实现期评估)**:把重试下沉到 `catalog.cppm:resolve_target`(唯一 not-found 节点)可让
> **所有**命令共享;但 `PackageCatalog` 未 import `sync_all_repos`,需注入 sync 回调,改动面更大。
> 本轮取**上层 `cmd_install`**(已 import 两原语,最小改动、最贴现有架构),`cmd_use` 等按需后续跟进。

### C3 — 子索引构建进度:多行 → `\r` 单行自刷新 + 错误分支 【跨仓 / 用户诉求 2】

**现状**:scode/awesome/d2x 的 `pkgindex-build.lua` 是**每文件一行** `cprint`(N 包 = N 行刷屏),无错误分支。
**改动**:收敛为**一条 `\r` 原地自刷新**的进度条(`只要一行不用多行`),用 `cprintf`(color、**不换行**)+ 行首
回车 + `\x1b[K` 清到行尾;**只有报错才换行**打红行并中断;循环收尾补一次换行保留最终进度。目标模板(以 scode 为例):

```lua
-- console width (fallback 80) so the single self-refreshing line never wraps
local function term_width()
    local w = tonumber(os.getenv("COLUMNS"))
    if w and w > 0 and w < 1000 then return w end
    -- os.getwinsize returns a 32767 sentinel when there is no tty; guard it.
    if type(os.getwinsize) == "function" then
        local sz = os.getwinsize()
        if type(sz) == "table" and sz.width and sz.width > 0 and sz.width < 1000 then
            return sz.width
        end
    end
    return 80
end

function install()
    os.cd(pkgsdir)
    os.execv("git", {"clean", "-fdx"})
    os.execv("git", {"checkout", "."})

    local files = os.files(path.join(pkgsdir, "**.lua"))
    local template_content = io.readfile(template)
    local all_index_cnt = #files
    local built_index_cnt = 0
    local width = term_width()
    for _, file in ipairs(files) do
        built_index_cnt = built_index_cnt + 1
        if not file:endswith("pkgindex-update.lua") then
            local name = path.relative(file, pkgsdir)         -- 相对名(g/gcc.lua),尽量小
            -- xmake sandbox has NO pcall/dofile/error; use try/catch + raise.
            local ok, err = true, nil
            try {
                function() io.writefile(file, io.readfile(file) .. template_content) end,
                catch { function(errors) ok = false; err = errors end }
            }
            if ok then
                -- 尽量小 + 不超控制台宽度:按 width 预算左截断,\r 原地刷新,\x1b[K 清残留
                local prefix = string.format("[%d/%d] scode::", built_index_cnt, all_index_cnt)
                local budget = width - #prefix - 1
                if budget < 8 then budget = 8 end
                if #name > budget then name = ".." .. name:sub(#name - budget + 3) end
                cprintf("\r[${green}%d/%d${clear}] scode::%s\x1b[K", built_index_cnt, all_index_cnt, name)
                io.flush()
            else
                print("")   -- 除非报错:换行,打印红色错误并中断(非零退出,CI/发布可见)
                cprint("[${red}%d/%d${clear}] scode::%s ERROR: %s", built_index_cnt, all_index_cnt, name, tostring(err))
                raise("pkgindex-build failed at " .. name)
            end
        end
    end
    print("")   -- 收尾换行,保留最终进度行,后续日志不覆盖
    return true
end
```
> **xmake 沙箱约束(实测,关键)**:`pcall`/`dofile`/`error` 在 xim 脚本沙箱里**不存在**——必须用 xmake 的
> `try { fn, catch { handler } }` 捕获、`raise(...)` 抛出。`cprintf` = 带色**不换行**打印;`\x1b[K`(ESC[K)
> 清行尾避免长文件名残影;`io.flush()` 实时刷新(沙箱内可用)。
> **控制台宽度**(用户诉求):`term_width()` 取 `COLUMNS`→`os.getwinsize()`(**须 `<1000` 过滤 32767 无-tty 哨兵**)→80 兜底;
> 显示**相对名**并按 `width` 预算**左截断**(`..tail`),`\r` 单行永不换行/回绕。skip 文件不占行、仅计数。
> **实机验证**:xmake lua 跑真实 `install()`——成功单行自刷新(COLUMNS=50 触发左截断)、坏文件换行打红行 + `raise` 非零退出(255)。

**规范(见 §5 开发规范)**:凡采用 template-append 构建模型的子索引仓,`pkgindex-build.lua` 的 `install()`
必须使用统一 **`\r` 单行自刷新** `[n/N] <ns>::<file>` 输出,`${green}` 成功刷新 / `${red}` 错误换行并中断。
**非该模型的仓(fromsource 自包含 + binary、ros2/dragonos)不适用,不强改。**

---

## 5. 开发规范(cross-repo dev standards)

### 5.1 子索引构建输出规范
1. **单行自刷新(只要一行)+ 宽度自适应**:整个构建**只占一行**,用 `cprintf("\r...\x1b[K")` + `io.flush()` 原地刷新
   `[built/all] <namespace>::<relpath>`,显示当前包文件;**不得每文件换行刷屏**。行宽须**不超控制台宽度**
   (`term_width()`=`COLUMNS`→`os.getwinsize()`→80),超出时对文件名**左截断**(`..tail`);**尽量小**——用相对名而非绝对路径。循环结束补一次 `print("")`。
2. **颜色语义**:`${green}` = 成功(自刷新行);`${red}` = 失败。skip 文件(`pkgindex-update.lua`)不单独占行,仅计入 `n/N`。
3. **错误即换行 + 中断(除非报错)**:任一文件 append/parse 失败 → 先 `print("")` 换行,再打 `${red}` 错误行 + `raise(...)`,构建**非零退出**(CI/发布必须能看见失败)。
4. **命名空间前缀**必须与仓库 `namespace`(`scode`/`awesome`/`d2x`/…)一致,便于多仓并发发布时区分日志来源。
5. **幂等**:`install()` 开头 `git clean -fdx && git checkout .` 复位 `pkgs/`,保证重复构建结果一致。

### 5.2 客户端"索引可用性"约定
1. **首次落地由 `get_catalog` 负责**(C1):任何进入 catalog 的命令,首次运行都保证默认子索引已尝试同步。
2. **按需刷新只在 install 失败分支**(C2):不在解析热路径无脑刷;带"本轮一次 + 冷却"双重去抖。
3. **刷新粒度 = 全量 `sync_all_repos(true)`**:与 `xlings update` 同款,复用其 artifact-优先/git-兜底/指针 sha 命中即零下载的既有韧性(不新造单命名空间刷新路径)。
4. **git 兜底不可回退破坏 marker**:沿用 0.4.62 修复(`2026-06-30` 文档),按需刷新失败不得抹掉 artifact 标记。

### 5.3 版本与发布约定
1. **client bugfix/feature** 走 `src/core/config.cppm` `VERSION` bump + `release.yml`;bugfix(#366)独立小版本便于回滚。
2. **子索引内容仓**无 xlings 版本号,产物是**内容哈希 immutable artifact**;PR 合并触发主仓 `publish-sub-indexes.yml` 或发版兜底刷新(见 §7 CI)。
3. 跨 org 仓(d2x @ `d2learn`)改动需 d2learn 协调 Actions/secrets;未就绪时靠 xlings 发版兜底刷新(降级可接受)。

---

## 6. 分阶段落地

| 阶段 | 内容 | 仓库 | 版本/PR | 收益 |
|---|---|---|---|---|
| **P0(bugfix,立即)** | C1 `get_catalog` 首次同步 + e2e | `xlings` | `0.4.65`,PR "fix(xim): sync sub-indexes on first run (#366)" | 全新机器 `install scode:*` 直接可用,免手动 update |
| **P0.5(跨仓,可并行)** | C3 三个默认子仓补进度错误分支 | scode/awesome/d2x | 各一 PR "chore(build): robust one-line progress + error branch" | 构建输出可读可诊断;规范固化 |
| **P1(UX feature)** | C2 `cmd_install` 按需刷新 + e2e | `xlings` | `0.4.66`,PR "feat(install): refresh index on-demand when target missing" | 装刚发布的新包/新版本无需先 update |
| **后续(本轮不做)** | resolve_target 层下沉(全命令共享)、单命名空间增量刷新、ETag/304 精准新鲜度 | 全生态 | — | 见 §8 |

**推荐顺序**:P0 与 P0.5 并行(互不依赖)→ 发 `0.4.65` bugfix → P1 → 发 `0.4.66`。
P0 是纯本仓、可独立先发的热修;P0.5 跨仓但零客户端耦合;P1 依赖 P0 已合并(叠加语义)。

---

## 7. CI / 发布闭环

### 7.1 client(xlings)
- **P0/P1 PR** 走既有 9 条 CI(`xlings-ci-linux{,-e2e,-root}.yml`、`-aarch64`、`-archlinux`、`-macos`、`-windows`)。
- **新增 e2e**(§9 测试),挂在 `xlings-ci-linux-e2e.yml`。
- 合并后 bump `config.cppm` `VERSION` → `release.yml` 发 4 平台二进制 + 主/子索引 artifact;
  `xlings` recipe(`xim-pkgindex/pkgs/x/xlings.lua`)bump 到新版(沿用 unification-plan §0.5 的 `version-check.py` 自动升级链)。

### 7.2 子索引仓(scode/awesome/d2x 的 C3 PR)
- scode/awesome **当前无 CI workflow**、d2x 仅 `gitee-sync.yml`;C3 是**纯构建脚本**改动,不引入新 CI。
- 合并后:内容/构建脚本更新经**主仓 `xim-pkgindex/.github/workflows/publish-sub-indexes.yml`**
  (`workflow_dispatch`,可选 `name=scode|awesome|d2x|all`)重打包 artifact + 移指针(`push_index_pointers.sh` **merge** 不覆盖兄弟 key);
  **发版兜底**:`release.yml` 仍在发版时整套刷新默认子索引(至少发版时新鲜)。
- **验证钩子**:进度错误分支(§C3)让 `pkgindex-build.lua` 在坏 pkg 时**非零退出**,使 publish 工作流能 fail-fast(此前静默通过)。

### 7.3 生态打通(端到端)
`子仓 push pkgs/** → 主仓 publish-sub-indexes.yml → xlings-res/xim-index(GH+GitCode)artifact + 指针` →
`客户端 xlings install <新包> → C1 保证子索引已落地 / C2 按需刷新拉到新指针 → 命中安装`。
即:**内容侧发布**与**客户端消费**两端由指针(内容哈希)解耦,#366 修复关掉"首次消费"缺口,C2 关掉"陈旧消费"缺口。

---

## 8. 兼容性 / 回滚 / 风险

- **回退保留**:`XLINGS_INDEX_SOURCE=git` 一键全 git;C1/C2 的 sync 任一步失败自动 git 兜底(`repo.cppm:511-513`),不破坏现有 e2e / 本地 `file://` fixture(本地源永不走 artifact/sync)。
- **C1 风险**:近零。仅"标记 JSON 缺失"时多一次 sync;JSON 存在则完全跳过,常规启动无回归。需 e2e 覆盖"全新机器 install 子索引包"。
- **C2 风险**:低。真缺包多一次刷新(有本轮一次 + 冷却去抖);仅作用于 install 失败分支,不改解析核心。需 e2e 覆盖"目标缺失 → 刷新后命中"与"目标真缺失 → 只刷一次后正常报错"。
- **C3 风险**:极低。仅子索引构建脚本;`raise` 中断是**期望**行为(坏 pkg 应 fail-fast)。不影响客户端。
- **跨 org(d2x)**:C3 PR 需 d2learn review;未合并前 d2x 进度输出保持现状(无错误分支),不阻塞 scode/awesome。
- **与 0.4.62 迁移闸的关系**:C1/C2 不触碰迁移闸/原子交换逻辑(`2026-06-30` 文档范畴),仅在其之上补"首次触发"与"按需重试"。

---

## 9. 文件级改动清单 + 测试

> 下列均为**已实现**(工作树)。C1+C2 co-implemented,client VERSION 现为 `0.4.65`;若要分两发,P1 单独版可后置为 `0.4.66`(见 §6)。

### 9.1 `openxlings/xlings`(P0 C1,已实现)
- `src/core/xim/commands.cppm:32-63` — `get_catalog`:首次 rebuild 后 `bool subIndexesNeverSynced = !sub_indexes_initialized();`,缺失即 `sync_all_repos(true)` + `rebuild(true)`(C1)。
- `src/core/xim/repo.cppm`(`discovered_project_sub_repos` 之后) — 新增导出谓词 `bool sub_indexes_initialized()`(= `exists(sub_repos_json_path(false))`)。
- `src/core/config.cppm:16` — `VERSION` → `0.4.65`。
- **测试(新增,已绿)**:`tests/e2e/install_subindex_first_run_test.sh` —— priming update 后删 marker + 子仓目录(= #366 态),`install testd2x:d2testpkg` 断言**不报 `not found`** 且解析进 plan、marker 重建。挂 `xlings-ci-linux-e2e.yml`。

### 9.2 `openxlings/xlings`(P1 C2,已实现)
- `src/core/xim/commands.cppm` — `cmd_install`:入口前置 `bool refreshedForMissing = false;`;resolve 失败(非 ambiguous)分支在 `return 1` 前 `sync_all_repos(true)` + `rebuild(true)` + 重试解析一次(C2)。
- `src/core/xim/commands.cppm`(`cmd_remove` 前向声明后) — 新增文件内 `bool index_refresh_cooldown_elapsed()`(`steady_clock`,60s 进程级去抖)。
- **测试(新增,已绿)**:`tests/e2e/install_refresh_on_missing_test.sh` —— 全同步后向子仓提交 `newpkg`(本地索引陈旧),`install testd2x:newpkg` 断言 **C2 `refreshing index` 触发**且刷新后命中(C1 不触发,隔离 C2)。

### 9.3 `xim-pkgindex-{scode,awesome,d2x}`(P0.5,已实现,各一 PR)
- `pkgindex-build.lua` `install()` — 每文件多行 `cprint` → **`\r` 单行自刷新** `cprintf` + `\x1b[K` + `term_width()` 左截断(C3,§4 模板);`try/catch` 包裹 append(沙箱无 pcall)+ 换行 + `${red}` 错误行 + `raise`。
- **测试**:`luac -p` 通过;xmake lua 实跑真实 `install()` —— 成功单行自刷新(COLUMNS=50 触发左截断)、坏文件红行 + `raise` 非零退出(255)。(CI 可挂主仓 `ci-xpkg-test.yml` 或本地 `xim install pkgindex-update` 冒烟。)

### 9.4 `openxlings/xlings` — `mcpp.toml` pin 对齐(**非 #366,分支存量断裂**)
- `mcpp.toml`:`tinyhttps 0.2.8 → 0.2.9`、`xpkg 0.0.42 → 0.0.44`(与 `main` 一致)。**原因**:本分支源码已用
  `expectedBytes` / `mcpplibs.xpkg.compat`(0.2.9 / 0.0.44 才有),但 pin 未随之 bump,导致**改前即无法 `mcpp build`**。
  为出可验证构建而对齐。**建议**:本分支 rebase/merge `main` 后此改动自然消解;若 #366 单独出 PR,应基于 `main`(已含正确 pin),不带此项。

---

## 10. 后续阶段(本轮明确不做,留档)

- **`resolve_target` 层下沉**:把 C2 的"not found → 刷新 → 重试"下沉到 `catalog.cppm:400` 唯一节点,
  让 `cmd_use` / resolver / installer 全命令共享(需给 `PackageCatalog` 注入 sync 回调,改动面大)。
- **单命名空间增量刷新**:C2 现为全量 `sync_all_repos`;包数增长后可只刷 miss 的命名空间(需按 key 拆 sync)。
- **ETag/304 精准新鲜度**:替代 7 天 stamp / 冷却近似,`update`/按需刷新走条件请求(unification-plan §8)。
- **端到端签名(minisign)/ 稀疏索引**:归属 index-as-resource 既有 roadmap,与本方案正交。

---

## 11. 验收标准

1. **#366 修复**:全新机器(清空 `xim-index-repos/`),`xlings install scode:linux-headers@5.11.1`(或任一子索引包)**不报 `not found`**,自动同步子索引后安装成功;子索引 `pkgs/` 与 `xim-indexrepos.json` 均落地。
2. **常规无回归**:`xim-indexrepos.json` 已存在时,`get_catalog` **不触发**额外 sync(启动耗时与 0.4.64 持平)。
3. **按需刷新命中**:本地索引缺某新发布包/版本、远端指针已有时,`xlings install <target>` 刷新后命中安装,**无需先手动 `xlings update`**。
4. **按需刷新去抖**:目标**真缺失**时,单次 `install` 最多触发**一次**刷新,最终正常报 `not found`,不死循环、不反复卡顿。
5. **进度规范**:scode/awesome/d2x 构建输出为**一条 `\r` 自刷新单行** `[n/N] <ns>::<file>`(非 N 行刷屏),收尾一次换行;人为构造坏 pkg 时**换行**打 `${red}` 行并**非零退出**。
6. **回退**:`XLINGS_INDEX_SOURCE=git` 全程 git 正常;C1/C2 任一步失败自动 git 兜底,现有 e2e/fixture 全绿。
