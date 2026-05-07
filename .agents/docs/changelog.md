# Change Log | [xlings论坛](https://forum.d2learn.org/category/9/xlings)

## 2026

### 2026-05 (v0.4.19)

- **`xlings list / use / remove` 全部 subos-aware (PR B,接 0.4.18 的 C2 schema)**
  - 0.4.18 落了 `installed[]` 数据,但 user-visible 的几个命令还是从全局 `versions` DB 读 —— 在新建的空 subos 里跑这些命令体感像啥都没改。这次把 shim + use + list + remove 全部改成读 `Config::workspace_installed()`。
  - **shim 错误提示**(`src/core/xvm/shim.cppm`)三态区分:
    - 当前 subos `installed[]` 里有 program 但没 active → `no active version of '...' in current subos` + `available: <subos-scoped 列表>` + `hint: xlings use ...`
    - 当前 subos 没有但全局 DB 有 → `'X' is not installed in current subos` + `hint: xlings install X`(payload 在别的 subos 装过,这个 subos 需要显式 opt-in)
    - 全局也没有 → `'X' is not installed` + `hint: xlings install X`
  - **`xlings use <pkg>`(无 version)** 默认只列当前 subos `installed[]` 里的版本,标题改成 `<pkg> versions (current subos)`;加 `--all` / `-a` flag 切回全局视图(`<pkg> versions (all subos)`)。subos `installed[]` 为空时报错并提示 `xlings install`,不再丢一个空 panel 让用户猜。
  - **`xlings use <pkg> <ver>` auto-add 语义**:用户在 fresh subos 里 `use` 一个全局 versions DB 已知但当前 subos 没装的版本 → **自动把版本加进 `installed[]` 然后切 active**,不强迫用户先 `install`。payload 全局共享,这个操作零成本。
  - **`xlings list`** 默认只列当前 subos `installed[]` 的包,标题 `Installed packages (current subos):`;加 `--all` / `-a` 看全局 (`Installed packages (all subos):`)。空时给 hint。
  - **`xlings remove <pkg>` 拒绝 fresh subos 的"假 remove"**(`src/core/xim/commands.cppm`):pre-fix 的现象是 `xlings remove gcc` 在没装 gcc 的 subos 里也会"成功",因为 catalog resolve 到 latest declared version、active workspace 是空所以 detach no-op、跨 subos refcount 看到别的 subos 装了 gcc 就走"detach only"分支并报告 `✓ removed (subos: tmp)`。新逻辑在 cmd_remove 入口处先检查 `Config::workspace()` + `Config::workspace_installed()`,**不在当前 subos 直接报错** + 列出哪些 subos 装了这个包 + 给出切 subos 的命令。
  - **`xlings update`** 不需要改 —— 它已经走 `effective_workspace` 拿 `currentActive`,空就报 not installed。已经 subos-aware 了。
  - 影响:`shim.cppm` / `xvm/commands.cppm` / `xim/commands.cppm` / `cli.cppm`。新加 `cmd_list / cmd_list_versions(..., bool all = false)`。
  - 后续:还可以再加 `xlings list <pkg>` 也支持(目前 list 是顶层包列表,不是某 pkg 的版本列表)、`xlings status` 显示 subos 的 installed[] 总览 —— 但这次先把用户报告的 case 全部修掉,别的小 polish 后续 patch 处理。

### 2026-05 (v0.4.18)

- **subos workspace schema 升级:`{active, installed[]}` 形式 (Plan C2,PR A) (#273)**
  - 0.4.18 `2026-05-07-xlings-cmd-subos-binding-analysis.md` 里的 C 系方案落地第一步:**subos** 的 `.xlings.json` 中 `workspace` 每个 target 的 value 从原本的 `"<version>"` 字符串升级为对象形式 `{ "active": "X", "installed": ["X","Y","Z"] }`。**project** `.xlings.json` 不动 —— 它是声明意图(`xxx = { linux = "..." }` 平台条件式)而非运行时状态,无需 `installed`。
  - **解析层(`xvm::subos_workspace_from_json`)三种 value 形态都接收**:
    1. 字符串 `"1.2.3"`(0.4.18 之前的旧文件,自动当作 `active`,`installed[]` 留空,下次 save 自动改写)。
    2. 对象 `{active, installed}`(新形式)。
    3. 对象 `{linux: ..., windows: ..., default: ...}`(平台条件式 fallback,subos 文件本来不会出现这个形态,但用户手编时容忍掉)。
  - **形态 (2) vs (3) 的歧义靠保留 key 区分**(C2 design 里的 Plan 1):object 出现 `active` 或 `installed` 这两个 key → 当作新形式;否则按平台条件解析。两套 schema 永远不会撞到同一份文件里。
  - **写入层(`xvm::subos_workspace_to_json`)永远输出新形式**,且强制不变量:`active` 出现在序列化结果里时,一定也出现在 `installed[]` 里(serializer 自动补)。`installed[]` 排序稳定,降低 git diff 噪音。
  - **GC refcount 修正(`installer::is_version_referenced_anywhere_`)**:之前只检查"某 subos 的 active == version"。新形式下,某 subos 可能保留 X 在 `installed[]` 但 active 是别的版本,这种 case 之前会被误判为"无人用",触发 GC 把 payload 删掉。现在跨 subos refcount 同时遍历 active 和 `installed[]`,**两者任一命中就视为被引用**。
  - **install / remove 维护 `installed[]`**:
    - `process_xvm_operations_` add(program 类型) → 把 `ver_key` 加进当前 subos 的 `installed[]`(去重);active pointer 仍按旧规则(无 active 或 `--use` 时切到新版本)。
    - `process_xvm_operations_` remove(无论是 user-initiated remove 还是 install-time upgrade) → 同步从 `installed[]` 抹掉 `ver_key`(再次防御:detach_current_subos_ 通常已经先抹了,但 install upgrade 路径不走 detach)。
    - `detach_current_subos_` → 永远先把 `version` 从 `installed[]` 移除;如果它正好是 active,则做 sysroot 拆除并**自动 fallback 到 `installed[]` 里剩余最高版本**(列表是 sorted ascending,`back()` 即最高);剩余为空才清空 active pointer。**避免"删了 active 之后命令就完全报错"的硬中断**。
  - 影响范围:`config.cppm`、`xvm/db.cppm`、`xvm/types.cppm`(新增 `WorkspaceInstalled` 类型 + `SubosWorkspace` 结构)、`xim/installer.cppm`、`profile.cppm`(GC scan 同时看两边)。**没有 schema 强制升级步骤** —— 旧文件第一次 save 才被改写。
  - **compat 入口**:`xself::compat::v0_4_18::kSchemaForm` 是空 sentinel,文档化此次 schema migration,挂在 `removal_target: 0.6.0`(到时把 `subos_workspace_from_json` 里支持字符串形态的分支也一起删掉)。
  - **db.cppm 顺手清理**:之前 `workspace_from_json` 内嵌一份 `current_platform_key` lambda,硬编码 `linux` / `macosx` / `windows`,这次新加 `subos_workspace_from_json` 又复制了一份。两个都改成直接用 `platform::OS_NAME`(per-OS 模块已经导出的常量,`src/platform/{linux,macos,windows}.cppm`)。重复实现合并到 `resolve_platform_workspace_value_` 一个 helper 里。
  - 测试:新增 `subos_workspace_c2_schema_test.sh`(4 个 scenario,覆盖 lazy migration、active+installed 并存、save invariant、active-less object 的容忍读取);**原 7 个 e2e 测试**里把 workspace value 当字符串读的 python helper 全部升级成兼容 dict / str 两种(`remove_self_guard`、`remove_multi_version`、`xlings_self_replace`、`self_doctor`、`update_package`、`cli_target_compat`、`install_idempotent`)。
  - **下一步(后续 release)**:`xlings list / use / update` 加 subos-aware 过滤(只显示当前 subos 的 `installed[]` 视图),把这次落下来的数据模型用起来。

- **Windows CI / release xrepo 缓存路径修复 (#273)**
  - 现象:Linux / macOS 的 `Cache xrepo packages` 步骤缓存命中正常(每次 ~5MB / ~3MB),但 **Windows 这个缓存从来没有上传过**(`gh api repos/.../actions/caches` 里 `xmake-pkgs-Windows-*` 一条都没有)。导致 Windows 每次 PR / release 都从源码重编 libarchive + zstd + bzip2 + lz4 + zlib via MSVC,**`Configure xmake` 单步耗时 ~8 分钟**。
  - 根因:xmake 的 global dir 在 Windows 上是 `%LOCALAPPDATA%\.xmake`(xmake 源码 `_global_dir()`:`LOCALAPPDATA → APPDATA → USERPROFILE`,然后拼 `.xmake`)。原 workflow 缓存路径 `~\.xmake\packages` 在 GitHub Actions runner 上展开成 `%USERPROFILE%\.xmake\packages` —— 一个 xmake 根本不写入的目录。所以缓存步骤每次都"成功"但实际什么都没缓存。
  - 修复:`xlings-ci-windows.yml` 和 `release.yml` 的 Windows 缓存路径都从 `~\.xmake\packages` 改成 `~\AppData\Local\.xmake\packages`。Linux / macOS 的路径不动(那两个平台 `~/.xmake/packages` 就是 xmake 实际用的位置)。冷启动第一次仍然 ~8 分钟,之后命中缓存应该和 Linux/macOS 相当(~30 秒)。

- **prompt marker subos 名色调:bold green → bold magenta(8-color "经典紫")(#272)**
  - 用户反馈 v0.4.17 的 bold-green 名字跟周围 prompt 颜色站位不协调,绿色和 entering message 的 magenta、switched message 的 cyan 混在一起会让 prompt 看着乱。
  - 三种 shell(bash/zsh、fish、PowerShell)同步切到 SGR `\033[1;35m`(8-color magenta,大多数终端 theme 渲染为低饱和度紫色)。括号 `[xsubos:` 和 `]` 仍是 slate-400 灰。`NO_COLOR` / `TERM=dumb` 仍走纯文本 fallback。
  - `profile_resources::kVersion` 8 → 9。已装 v8 用户(只有 v0.4.17 极短窗口期内升级过的人)下次 xlings 调用时通过 `compat::v0_4_17::auto_upgrade_profiles_if_stale` 自动覆盖。

- **shim 报错 hint 区分 "未安装" vs "未激活"(用户 0.4.17 反馈)**
  - 旧:`gcc` 命令(orphan shim) → workspace 找不到 → 永远提示 `xlings use gcc <version>`,但 gcc 可能根本没装,使用 `xlings use` 没用。
  - 新:shim 拿到空 active 后,先查全局 versions DB:
    - 0 个版本 → `[error] 'gcc' is not installed` + `hint: xlings install gcc`
    - ≥1 个版本 → `[error] no active version of 'gcc' in current subos` + `available: <list>` + `hint: xlings use gcc <version>`
  - 仅改报错文案,不动 install/remove/use 的语义,零回归风险。

- **subos-binding 设计文档(`docs/plans/2026-05-07-xlings-cmd-subos-binding-analysis.md`)**
  - 把"`xlings list / use / update / install / remove` 与 subos 绑定"的真实数据模型讲清楚:`versions DB` 全局、`workspace` per-subos、还缺一个 per-subos `registered` 集合。
  - 列出三种修复方向:**A**(只改 shim UX,本 patch 已落)/ **B**(installer 收紧 versionless `xvm.remove` 处理,0.4.19 候选)/ **C**(per-subos `registered` 集合,真正的 subos 作用域,0.5.0 minor)。
  - 用户报告的 `xlings remove gcc` 后 `gcc` 命令仍报错的根因(versions DB 是全局视图,subos 没有 registered 集合,导致 shim 留存 + workspace auto-switch 走偏)文档化,作为 0.5.0 重构的 baseline。

### 2026-05 (v0.4.17)

- **shell-level subos 切换 + auto-upgrading profile + TUI 渲染统一 (#269)**
  - `xlings subos use foo` 默认从"改全局符号链接"改成 **spawn 一个新交互 shell**(POSIX `execl` / Windows `CreateProcess+WaitForSingleObject`),env 里有 `XLINGS_ACTIVE_SUBOS=foo`,`exit` 直接回到原 shell,**两个 terminal 终于可以用不同 subos 互不干扰**。原"持久 + 全局"行为通过显式 `--global` flag 保留。`--shell <kind>` 是给测试 / 高级用户的 eval-able snippet 输出,不在用户帮助里出现。
  - 嵌套策略:同 subos 二次 use → 一行 `› already in subos NAME` 退 0,**不重复 spawn**。不同 subos use → `▾ nesting subos FROM -> TO  ('exit' returns to FROM)` 提示后再 spawn。spawn 模型下"先退出当前 subos 再进新的"在物理层做不到(子进程无法操控父 shell exec(2)),所以只做嵌套 + 友好提示。
  - shell profile 三种 shell 都换成 `XLINGS_BIN="$XLINGS_HOME/subos/${XLINGS_ACTIVE_SUBOS:-current}/bin"`(env 优先,没设就回退到 `subos/current` 全局符号链接,保留原有兜底);Config 端的 `update_effective_paths_()` 也对齐同样的 project > env > global 优先级链,保证 `xpkg install` / PATH 看到的是同一个 active subos。
  - profile 多了一个 prompt marker:进入 subos shell 后 prompt 自动加 `[xsubos:NAME]` 前缀(cyan + bold name,bash/zsh/fish/pwsh 各自原生上色),色盲 / 不支持 ANSI 的终端通过 `NO_COLOR` 或 `TERM=dumb` 退到纯文本。再次 source profile **不会**叠加多个 marker。
  - profile 内嵌到 C++ 模块:新增 `src/core/xself/profile_resources.cppm` 持有 bash / fish / pwsh 三份 payload + `kVersion` 常量,`init.cppm` 通过 `import` 消费;旧的 `config/shell/xlings-profile.{sh,fish,ps1}` 文件删掉(那三份和 init.cppm 内嵌的 raw literal 之前差 1 字节都没人发现)。**单一来源**。
  - profile 升级机制:`# xlings-profile-version: <N>` 标记 + `write_or_upgrade_profile_`。版本递进 v1 → v5(env override → 加 prompt marker → marker 改名 `[xsubos:...]` → 加 ANSI 颜色)。**新装走 fresh write,旧装走自动升级且保留用户在 marker 之外的编辑**。
  - **`xlings update xlings` 后老用户也能拿到新 profile**:新 binary 启动时(每次 xlings 调用)跑 `xself::compat::v0_4_17::auto_upgrade_profiles_if_stale`,版本不一致就静默重写。一次性的成本,旧 binary 的 `xlings update xlings` 路径只翻 xvm 指针、不调 `ensure_home_layout`;新 binary 自检自愈。
  - TUI 输出统一为单行模板,四种事件 + 四种颜色:`▸ switched to subos NAME [global] (DIR)` cyan / `▸ entering subos NAME (exit to leave)` magenta / `› already in subos NAME` gray / `▾ nesting subos FROM -> TO ('exit' returns to FROM)` amber。`[global]` tag 把"持久"和"per-shell ephemeral"显式区分。
  - **compat 模块整理**:`src/core/xself/compat_0_4_8.cppm` → `compat.cppm`,每个 compat 块归到 `vX_Y_Z` 子命名空间,header 注释含 `removal_target` 字段。当前两组:`v0_4_8::*`(legacy alias 迁移,drop in 0.6.0)+ `v0_4_17::*`(profile auto-upgrade,permanent self-heal)。删除某个 compat 是删一整块 namespace 的一次性操作,所有 caller 立刻编译报错暴露出来。
  - 测试:新增 `subos_shell_level_test.sh`(14 个 scenario,含 prompt 颜色 + 嵌套 + 并行 sub-shell 隔离 + PATH dedup)和 `subos_profile_upgrade_test.sh`(4 个,含 v1 legacy 升级 + 同版本保留用户编辑)。
  - 现有测试一处适配:`xlings subos use NAME` 默认行为变更 → `release_subos_smoke` / `subos_events` / `subos_payload_refcount` 显式补 `--global`。

### 2026-05 (v0.4.16)

- **修复：对未安装的包再次 `xlings remove` 仍打印 "✓ removed" 的循环 (#266)**
  - `src/core/xim/commands.cppm`：`cmd_remove` 在用户不带版本号、active 绑定又为空时,曾把解析降级到 catalog 配方里的"声明最高版本",再把 `!installed` 的判定门控在 `resolvedToDefiniteVersion` 上 —— 注释里那句"留给 installer.uninstall 处理"是错的,uninstall 在空 DB 上会跑完所有 no-op 仍报 success,把用户卡在重复卸载循环里。
  - 修复双管齐下:active 为空时先回查 xvm DB(`xvm::pick_highest_version`,与 multi-version remove 的兜底复用同一函数);移除 `resolvedToDefiniteVersion` 守门,只要 catalog 解出的 match 不在磁盘上就 warn `"<pkg>@<ver> is not installed"` 并 exit 0,不再 emit `remove_summary`。
  - `tests/e2e/remove_multi_version_test.sh` 加 Scenario 4:第三次卸完最后一个版本后再 `remove`,断言 exit 0、无 `removed.*subos` 摘要、有 `not installed` 诊断、DB / workspace / store 仍为空。

### 2026-05 (v0.4.15)

- **下载缓存：sha256 缺失时改用 HEAD 探测，不再每次重下 (#TBD)**
  - `src/core/xim/downloader.cppm`：包索引中只声明 `url` 不声明 `sha256` 的条目（fixture 索引里约 8%；实际 `node.lua` / `nvm.lua` 之类 `_linux_url` helper 拼出来的 URL 占比更高）原来每次 `xlings install` 都会重下整包。新增 HEAD-fallback 缓存路径：`fs::file_size` 与服务端 `Content-Length` 比对 + `<destFile>.meta` sidecar 记录的 `Last-Modified` / `ETag` 与本次 HEAD 响应比对，命中则跳过下载。HEAD 失败（离线/服务端拒 HEAD）回退"文件存在即信"，airline-friendly。
  - sha256 路径在不匹配时主动 `fs::remove` 旧文件，预防将来 tinyhttps 启用 Range/resume 时拼出腐烂文件。
  - `src/libs/tinyhttps.cppm`：`query_remote_meta()` 返回完整 `RemoteFileMeta { contentLength, lastModified, etag, ... }`；老的 `query_content_length()` 改成薄包装。
  - `tests/unit/test_main.cpp::XimDownloaderTest::MetaSidecarRoundTrip` 锁定写入 + 读取 + 缺失 + 畸形行容错。

- **bump：mcpplibs-xpkg 0.0.37 → 0.0.38**
  - 带入 libxpkg 的 `os.dirs` glob 修复（POSIX 下 `ls -d "<pat>"` 双引号会让 shell 跳过 glob 展开，导致 `os.dirs("…/v*")` 静默返回空表）。

### 2026-04 (v0.4.3)

- **下载器：自动识别系统代理 (#222)**
  - `src/libs/tinyhttps.cppm` 增加 env 解析：`HTTPS_PROXY` / `HTTP_PROXY` / `ALL_PROXY`（含小写变体）按 libcurl 优先级生效；`NO_PROXY` 支持 exact / dotted-suffix / bare-suffix / `*` 通配。
  - 命中代理时通过 `log::debug("tinyhttps: using proxy ... for ...")` 输出，全局 `-v` 可见。
  - 复用 `mcpplibs::tinyhttps` 0.2.0 已内建的 HTTP CONNECT 隧道，无需上游改动。
  - 9 个 `TEST(Proxy, …)` 单测锁定行为矩阵。

- **TUI：Linux/macOS/Windows 图标一致 (#221)**
  - 移除 `src/ui/theme.cppm` 的 `#ifdef _WIN32` ASCII fallback：三个平台用同一组 BMP 图标 `○ ↓ ▾ ⊕ ✓ ✗ › ▸ ◆`，把 `⟐` `⚙` 这种缺字形的偏门字符换掉。
  - 新增 `tests/unit/test_main.cpp::ThemeIcons` 4 个单测：逐字节锁定 + 防 ASCII fallback + 强制 3-byte BMP UTF-8 + 渲染流抓 stdout 验字节。
  - 配套 `tests/e2e/tui_utf8_test.sh` 在 Linux/macOS CI 跑端到端编码验证。
  - macOS / 文档加 Windows 字体推荐段。

- **架构：移除 agent 子系统 (#220)**
  - 删除 `src/agent/` 整目录（14 个 .cppm，~3000 行）+ `src/libs/` 中 8 个 agent-only 模块。
  - 删除 `mcpplibs-llmapi` 外部依赖；`mcpplibs-tinyhttps` 保留（xim 下载器使用）。
  - 51 files changed, +119 / -16768 行；xlings 回归到纯包管理器 + xvm 运行时定位。

- **修复：`xlings remove <pkg>` 不再误删整包版本表 (#219)**
  - `src/core/xim/installer.cppm` 修复：当 uninstall hook 发出无版本号的 `xvm.remove(name)` 时，旧路径 `versions_mut().erase(name)` 把整个包条目擦掉（其它已装版本变孤儿）；现在改为用外层 resolved 的 `detachVersion` 兜底，`xvm::remove_version` 精确删一个；删后若有剩余版本且被删的是 active，自动按 semver 降序切到最高。
  - 新增 `xvm::pick_highest_version()`。
  - `tests/e2e/remove_multi_version_test.sh` 端到端回归（hermetic、私有 fixture index、Linux + macOS CI 都跑）。

- **CI：macOS bootstrap xlings v0.3.2 → v0.4.2**
  - `.github/workflows/xlings-ci-macos.yml` 和 `release.yml`：旧 v0.3.2 在 GitHub Actions 非 TTY 下载 LLVM 时 progress 输出被吞，导致 7+ 分钟无日志看似卡死；v0.4.2 进度行正常输出，CI 不再误判。

### 2026-03 (v0.4.0)

- **xvm C++ 集成：消除 Rust xvm 依赖**
  - Lua xvm 模块从 shell-out 调用 Rust `xvm` 二进制改为收集 `_XVM_OPS` 操作表
  - C++ 侧在 hook 执行后通过 `PackageExecutor::xvm_operations()` 读取并统一处理
  - 新增 `xvm.setup()` / `xvm.teardown()` 高层 API（一次调用注册程序/库/头文件）
  - VData 扩展 `includedir` / `libdir` 字段，支持头文件和库的 symlink 追踪
  - 头文件安装改为 symlink 方式（`install_headers()`），版本切换时自动切换
  - 卸载流程增强：自动清理 VersionDB 条目和 workspace 引用
  - 修复 shim.cppm 跨平台 PATH 分隔符问题（使用 `platform::PATH_SEPARATOR`）
  - 修复 commands.cppm 跨平台 symlink 问题（Windows 使用 junction/hardlink 回退）
  - 移除 Rust xvm 源码（core/xvm/Cargo.toml, src/, shim/, xvmlib/）
  - 移除 CI/release 脚本中所有 Rust 构建步骤和 xvm 二进制验证
  - 新增 7 个单元测试（VData 新字段 + 头文件 symlink 操作），总计 81 个测试

- **xim 核心模块 C++ 重写**
  - 将 xim 包管理器核心从 Lua/xmake 子进程架构迁移到原生 C++23 模块实现
  - 消除 `xmake xim -P ...` 子进程调用，install/remove/search/list/info/update 命令全部在 C++ 内完成
  - 新增 7 个 C++23 模块：`xlings.xim.types`、`xlings.xim.repo`、`xlings.xim.index`、`xlings.xim.resolver`、`xlings.xim.downloader`、`xlings.xim.installer`、`xlings.xim.commands`
  - 基于 libxpkg (C++ 库) 实现包索引构建、搜索、版本匹配和包加载
  - 新增 DAG 依赖解析器：DFS 拓扑排序、循环检测、已安装跳过
  - 新增并行下载器：`std::jthread` 并发控制、SHA256 校验、镜像支持
  - 安装编排器通过 libxpkg `PackageExecutor` 运行 Lua hook（install/config/uninstall）
  - CLI 直接调用 `xim::cmd_*` 函数，不再依赖 xmake 运行时
  - 51 个单元测试覆盖所有模块（类型、索引、解析器、下载器、安装器、命令）
  - 解决多个 GCC 15.1.0 C++23 模块 bug（ICE、链接符号缺失、运行时格式错误）

### 2026-02

- **Bug fixes: xim task / xvm path quoting / elfpatch tool detection**
  - Fix `xlings install`/`search` failing with "invalid task: xim" on Windows when `find_xim_project_dir` falls back to source tree root (which lacks `task("xim")` definition). Now falls through to `~/.xlings` installed layout automatically.
  - Fix `xvm add --path` breaking when path contains spaces (e.g. macOS `/Applications/My App/`). The `--path` argument is now properly quoted, consistent with `--alias`, `--type`, etc.
  - Replace `find_tool()` in elfpatch with direct execution probe (`_try_probe_tool`). Tools like `install_name_tool`/`otool` on macOS and `patchelf`/`readelf` on Linux are now detected by actually running them, with `try{}` catching failures gracefully and printing actionable hints.
  - Added `tests/e2e/bugfix_regression_test.sh` covering all three fixes.

- **xlings self install 安装逻辑优化**
  - data/subos 保留策略改为「直接不删除、选择性不覆盖」，不再使用备份/恢复
  - 升级时 data/subos 完全保留不合并，避免 subos 损坏
  - 移除「保留缓存数据」交互提示，data/subos 自动保留
  - 优化用户提示与打印布局

## 2025

### 2025-08

- **发布xvm-0.0.5 + xpkg/xscript复用机制**
  - 增加了库类型的多版本管理机制, 以及`xvm info`详情查询 - [PR](https://github.com/d2learn/xlings/pull/108) - 2025/8/16
  - xpkg/xscript 即是包也是程序(脚本)的复用机制 (示例: [musl-cross-make](https://github.com/d2learn/xim-pkgindex/blob/main/pkgs/m/musl-cross-make.lua)) - [PR](https://github.com/d2learn/xlings/pull/109) - 2025/8/14
- **文档:** 初步完善文档: [快速开始](https://xlings.d2learn.org/documents/quick-start/one-click-install.html)、[常用命令](https://xlings.d2learn.org/documents/commands/install.html)、 [xpkg包](https://xlings.d2learn.org/documents/xpkg/intro.html)、[参与贡献](https://xlings.d2learn.org/documents/community/contribute/issues.html) - [PR](https://github.com/d2learn/xlings-docs/commit/122b060855e4c41cd7f95801f2656bca0a5a6fc1) - 2025/8/9


### 2025-07

- **代码优化:** 修复一些bug并优化相关代码、适配macos - [commits](https://github.com/d2learn/xlings/commits/main/?since=2025-07-01&until=2025-07-31) - 2025/7

### 2025-06

- 跨平台: 初步支持MacOS平台、xim添加冲突解决功能(xpkg的`mutex_group`字段实现) - 2025/6

### 2025-05

- 新功能: 增加包索引网站、支持多语言i18n - 2025/5

### 2025-02

- d2x: 重构公开课/教程项目相关命令, 形成独立的d2x工具 - [PR](https://github.com/d2learn/xlings/pull/79) - 2025/2/19

### 2025-01

- xim: 增加archlinux上aur的支持 - [PR](https://github.com/d2learn/xlings/pull/67) - 2025/1/10
- xvm: 增加版本管理模块 - [文章](https://forum.d2learn.org/topic/62) / [PR](https://github.com/d2learn/xlings/pull/60) - 2025/1/1

## 2024

- xpkg增加自动匹配github上release的url功能 - [文章](http://forum.d2learn.org/post/208) - 2024/12/30
- xlings跨平台短命令 - [视频](https://www.bilibili.com/video/BV1dH6sYKEdB) - 2024/12/29
- xinstall模块: 重构&分离框架代码和包文件 - [包索引仓库](https://github.com/d2learn/xim-pkgindex) / [PR](https://github.com/d2learn/xlings/pull/49) -- 2024/12/16
- xinstall功能更新介绍 - [文章](https://forum.d2learn.org/topic/48) / [视频](https://www.bilibili.com/video/BV1ejzvY4Eg7/?share_source=copy_web&vd_source=2ab9f3bdf795fb473263ee1fc1d268d0)
- 增加DotNet/C#和java/jdk8环境的支持
- 增加windows模块和安装器自动加载功能, 以及WSL和ProjectGraph的安装支持 - [详情](http://forum.d2learn.org/post/96)
- 软件安装模块增加deps依赖配置和"递归"安装实现
- 初步xdeps项目依赖功能实现和配置文件格式初步确定
- install模块添加info功能并支持Rust安装