# subos 选择泄漏 与 异平台载荷 —— 修复/优化方案

**日期**: 2026-07-30
**类型**: 计划 (plan) + 设计 (design)
**起因**: 用户真实环境现场取证（`g++` 一直对着 `subos/dev-hello` 编译；`install llvm@20.1.7` 报成功但注册了 29 个 Windows `.exe/.dll`）
**基线**: xlings `518e525` = `2026.7.29.2`；xim-pkgindex `origin/main` `10cf9dbf`
**涉及仓库**: `openxlings/xlings`、`openxlings/xim-pkgindex`（**不涉及 `mcpplibs/libxpkg`** —— 见 §3.2 方案对比）
**相关 issue**: [#408](https://github.com/openxlings/xlings/issues/408)（sysroot/bin/lib 多版本共存模型，设计讨论/0.5）、[#419](https://github.com/openxlings/xlings/issues/419)、[#423](https://github.com/openxlings/xlings/issues/423)、[#447](https://github.com/openxlings/xlings/issues/447)（已由 `a71f2a2` 修复）

---

## 0. 摘要与分批策略

现场是两个**互不相关**的缺陷族，共同点只有一个：**"什么都没发生"和"成功了"的输出完全一致**（memory `project_silent_success_pattern`）。

| 族 | 一句话 | 全新装能否复现 |
|---|---|---|
| **A. subos 选择泄漏** | 安装时把活动 subos 的**绝对路径**烧进 xvm alias（`g++ --sysroot=/…/subos/dev-hello`），`subos use` 从不重写它 | **能**，且必然 |
| **B. 异平台载荷谎报成功** | `already-installed` 快路径只检查"目录非空"，5 月遗留的 Windows 载荷完美通过；config 钩子照跑，把 `.exe/.dll` 注册成 program | 缺陷在最新版，但触发需 store 里先有异平台载荷 |

两族**技术上完全独立**，可并行、可分别发布。A 是唯一"任何用户全新装都会踩"的，优先。

| 批次 | 主题 | 仓库 | 独立价值 | 阻塞关系 |
|---|---|---|---|---|
| **A-1** | alias 执行期归一化 subos 路径 | xlings | ★ 让 `subos use` 对已装工具链真正生效，且**无需重装即修好所有存量 home** | 无 |
| **A-2** | `envs` 同源归一化 | xlings | 关掉同一风险的另一半出口 | A-1（复用同一函数） |
| **A-3** | doctor 报告 + `--fix` 一次性重写 DB | xlings | 让这个状态从"对所有诊断工具隐形"变为可见可清 | A-1（复用同一函数与规则） |
| **A-4** | 悬空 sysroot 链接的检测与清理 | xlings | 修 #419/#423 残留的现场（3 个指向已删 `/tmp` 的链接） | 无（与 A-3 同一 PR 更省事） |
| **A-5** | 隔离护栏：拒绝物化指向 `dataDir` 之外的 sysroot 链接 | xlings | 从源头阻止隔离 home 往真实 home 里写链接 | 无 |
| **B-1** | 载荷平台戳 + `already-installed` 平台校验 | xlings | ★ 异平台/半解压载荷不再被当成"已装好" | 无 |
| **B-2** | `llvm.lua` 的 `.dll/.exe` 判定改为 payload-gated | xim-pkgindex | Linux 上不再把 `libomp.dll` 注册成 program | 无 |
| **B-3** | alias 全军覆没 → 安装失败 | xim-pkgindex | 补上 #447 门禁的补集 | 无 |
| **B-4** | already-installed 但未激活时显式提示 | xlings | silent-success 家族的一条 | 无 |

**发布建议**：A-1..A-5 一个 PR 串（可分 2 个 PR：A-1/A-2 与 A-3/A-4/A-5），随下一个补丁版本发；B-1/B-4 一个 PR；B-2/B-3 pkgindex 一个 PR（注意索引发布滞后，见 §6）。

---

## 1. 现场证据

> 全部为只读取证，未改动用户真实 home。

### 1.1 A 族

`~/.xlings/.xlings.json`（**全 home 共享**的 xvm 库）里 14 条烧死路径：

```json
"g++": { "versions": { "16.1.0": {
    "alias": ["g++ --sysroot=/home/speak/.xlings/subos/dev-hello"] }}}
```

`gcc` / `cc` / `c++` / `x86_64-linux-gnu-{gcc,g++,c++}` 的 15.1.0 与 16.1.0 全是这一条。而同一文件里：

```
activeSubos = "default"        subos/current -> default
```

来源 `xim-pkgindex/pkgs/g/gcc.lua:200`：

```lua
local sysroot_dir = system.subos_sysrootdir()
local alias_args = ""
if sysroot_dir and sysroot_dir ~= "" then
    alias_args = string.format(' --sysroot=%s', sysroot_dir)
else
    log.warn("subos dir is empty, skip alias sysroot injection")
end
```

`system.subos_sysrootdir()` 的值由 `installer.cppm:805-810` 注入，最终来自 `config.cppm:354-361`：

```cpp
auto activeSubos = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
if (activeSubos.empty()) { activeSubos = globalActiveSubos_; }
return paths_.homeDir / "subos" / activeSubos;   // 具体名字，不是 subos/current
```

**注意 `XLINGS_ACTIVE_SUBOS` 优先于 `activeSubos` 字段** —— 不需要 `subos use --global`，在一个 `subos use dev-hello` spawn 出的子 shell 里 `install gcc` 就足以烧死。

执行侧没有任何逃生口：`shim.cppm:443` `std::string alias_cmd = vdata->alias[0];`，随后原样拼参数 `platform::exec`。`db.cppm:482` 的 `expand_path` 只展开 `${XLINGS_HOME}`，且**只作用于 `vdata.path`，不作用于 `alias`**。`subos.cppm` 全文对 `alias` 只有两处提及，都是 `xvm-alias` 这个内建 shim 名 —— **没有任何 alias 重写、没有 config 钩子重跑**。

诊断侧也看不见：`doctor.cppm:70` 明确写 `alias warning → not auto-fixed (could be intentional external)`，而 `g++` 这条 alias 的**程序名本身可解析**（坏的只有 `--sysroot=` 的目标），所以连 `AliasUnresolved` 警告都不会产生。

**为什么反复卸载重装无效**：`linux-headers` 的 config 落在活动 subos = `default`（输出里也如实写了 `subos: default`），而编译器读的是 `dev-hello`：

```
subos/default/usr/include/linux   -> ~/.xlings/data/xpkgs/xim-x-linux-headers/5.11.1/include/linux   ✓
subos/dev-hello/usr/include/linux -> /tmp/tmp.7eGKbPHC46/mcpp-home/registry/data/.../include/linux   ✗ 悬空
```

两条路径从来没有交集。`dev-hello/usr/include/{linux,asm,asm-generic}` 三个链接（7-29 16:45）全部指向已删除的 `/tmp/tmp.7eGKbPHC46/...` —— 某次**隔离 home 的 mcpp 运行**：registry/载荷被隔离到 `/tmp`，但 subos 路径仍由 `XLINGS_ACTIVE_SUBOS=dev-hello` + **真实 homeDir** 拼出，于是链接物化进了真实 home。这是 A-5 的直接依据。

### 1.2 B 族

```
$ file ~/.xlings/data/xpkgs/xim-x-llvm/20.1.7/bin/clang.exe
PE32+ executable (console) x86-64, for MS Windows
```

`20.1.7/bin` mtime 5-19、`lib` 5-17（几个月前 Windows 目标测试留在真实 store 里的载荷）；`.xpkg.lua` mtime 7-30 12:33（说明 config 钩子跑了，载荷没重解压）。旁证：`22.1.8/bin` 有 `clang.cfg`/`clang++.cfg`，`20.1.7/bin` 一个都没有 —— `install()`（唯一会解压 Linux tarball 并写 `.cfg` 的地方）**从未在 Linux 上执行过**。

`installer.cppm:2213`：

```cpp
bool payloadInstalled = node.alreadyInstalled;
```

其 trust-but-verify 兜底只做 `is_directory(expanded) && !is_empty(expanded)` —— **零平台校验**。

`llvm.lua` 两个过滤器都是 host-gated：

```lua
-- :104  只有 host==windows 才跳过 .dll
if os.host() == "windows" and name:sub(-4) == ".dll" then return false end
-- :118  只有 host==windows 才剥掉 .exe
if os.host() == "windows" and filename:sub(-4):lower() == ".exe" then
    return filename:sub(1, -5), filename
end
```

于是 Linux 上真的注册了 29 个条目，含 `libomp.dll` / `libiomp5md.dll` **作为 program**。`a71f2a2`（#447）加的"注册零个程序即失败"门禁拦不住 —— 它确实注册了 29 个，只不过全是 `.exe`。随后 Linux 的 alias 表（`cc→clang`）在 20.1.7 下找不到 `clang`，六条警告全 fail-open：

```lua
for _, app in ipairs(aliases) do
    if os.isfile(path.join(bindir, app.alias)) then
        xvm.add(app.name, {...})
    else
        log.warn("skip xvm add alias (not found): " .. app.name .. " -> " .. app.alias)
    end
end
```

补充：`commands.cppm:365-395` 的 `activate_requested_targets` 只在 `active.empty() || useAfterInstall` 时切换，`clang++` 的活动版本是 22.1.8，所以 20.1.7 **既没生效也没提示没生效**（B-4）。

---

## 2. 缺陷编号与归因

| # | 缺陷 | 位置 | 任务 |
|---|---|---|---|
| ① | alias 里烧死 subos 绝对路径，切 subos 无效 | `gcc.lua:200` + `config.cppm:354` + `shim.cppm:443` | A-1 |
| ② | `envs` 同一风险（gcc 目前注释掉了，llvm 等可能用） | `shim.cppm:295-304` | A-2 |
| ③ | 该状态对 doctor 完全隐形 | `doctor.cppm:497-554` | A-3 |
| ④ | 悬空 sysroot 链接无人检测、卸载不清 | 与 #419/#423 重叠 | A-4 |
| ⑤ | 隔离 home 把链接物化进真实 home | `sysroot.declare_headers` 路径 | A-5 |
| ⑥ | `already-installed` 不校验载荷平台 | `installer.cppm:2213` | B-1 |
| ⑦ | `.dll/.exe` 判定 host-gated 而非 payload-gated | `llvm.lua:104,118` | B-2 |
| ⑧ | alias 全军覆没仍报成功 | `llvm.lua:~420` | B-3 |
| ⑨ | already-installed 未激活无提示 | `commands.cppm:365-395` | B-4 |

---

## 3. Track A 设计

### 3.1 目标不变量

> **`subos use` 之后，该 home 里所有工具链看到的 sysroot 必须是新 subos 的 sysroot。**
> 这一点必须在**不重装任何包**的前提下对**存量 home** 成立。

### 3.2 三方案对比（决策：方案 2）

| | 方案 1：recipe 写占位符 | 方案 2：**shim 执行期归一化** | 方案 3：`subos use` 时重写 DB |
|---|---|---|---|
| 做法 | `gcc.lua` 写 `--sysroot=${XLINGS_SUBOS}`，shim 展开 | alias/env 里凡指向 `<home>/subos/<X>` 的绝对路径，执行时改指当前活动 subos | `use` 时扫全库改写 alias |
| 跨仓 | xlings + pkgindex（+ 能力探测） | **仅 xlings** | 仅 xlings |
| 版本门 | 需要 recipe capability probe（memory `reference_recipe_capability_probe`），否则新 recipe 打旧客户端 | **不需要** | 不需要 |
| 修存量 home | **修不了**（旧 alias 已烧死，须重装） | **全修**（不重装） | 需要用户主动跑一次 `use` |
| 三种选择模式（project / env / global）是否都对 | 是（若展开点唯一） | **是**（复用唯一解析点 `Config::xvm_artifact_subos_dir()`） | **不对**：env-spawn 与 project 模式**从不写 DB** |
| 其他风险 | — | 需要精确的"哪些路径算我们的"规则 | 重复 #443 的整文档重写窗口 |

**方案 2 胜出**：单仓、无版本门、修存量、三模式都对。方案 1 保留为"未来让 recipe 少烧路径"的清洁化，非本次必需。

> 与 #408 的关系：#408 的根因表述是 *"Selection 被复制进别人的产物"*（RPATH / `clang.cfg` / `specs`）。**xvm `alias` 字符串本身是这一族里第四个、也是最便宜的一个** —— 它完全在我们进程内，执行时可重解析。本方案就是 #408 的**单边廉价子集**，不预判 0.5 的整体重设计。

### 3.3 归一化规则（精确）

对一个字符串（alias 命令行 或 env 值）：

1. 找出每一处 `/subos/` 子串（Windows 上同时找 `\subos\`）。
2. 向左扩到"路径 token"起点：遇到 `空格 \t = : ; , " '` 或串首即停。得到 **prefix**。
3. **仅当** prefix 等于 `homeDir`，**或** prefix 以 `.xlings` 结尾时才改写；否则原样透传。
   （后者覆盖 project 模式的 `<projectDir>/.xlings/subos/<name>`，以及 `$HOME` 经软链到达的等价路径。）
4. 向右取到下一个路径分隔符或 token 边界，得到 **subos 名字段**；空则透传。
5. 用 `Config::xvm_artifact_subos_dir()` 整体替换 `prefix + "/subos/" + name`，**后缀（如 `/usr/include`）、周围的 flag、引号一律不动**。

**性质**：幂等（已是活动 subos 时替换结果字节相同）；对非我方路径（`/opt/subos/foo`）零影响；对 `--sysroot=` 之外的任何 flag 同样有效（未来 `-I`、`PATH=a:b` 都覆盖）。

**关键选择**：替换目标是 `Config::xvm_artifact_subos_dir()` 而**不是** `subos/current` 符号链接。理由：`xvm_artifact_subos_dir()` 是唯一同时正确处理 project / env / global 三种模式的解析点（`config.cppm:1084-1096`），而 `subos/current` 只反映 global 模式。

---

## 4. Track B 设计

### 4.1 B-1 载荷平台戳

安装钩子成功后，在载荷根目录写 `.xpkg-install.json`：

```json
{ "os": "linux", "arch": "x86_64", "xlings_version": "2026.7.29.2",
  "index_ref": "10cf9dbf", "installed_at": "2026-07-30T12:33:11Z" }
```

`already-installed` 快路径改为：**戳存在且 `os` 与本机一致** 才算已装；不一致 → 重跑 install 钩子。

**存量 home 没有戳**，所以需要一次性启发式，且必须**宁放过不误杀**（误判会触发一次不必要的重装）：探测载荷 `bin/` 下最多 8 个常规文件的魔数 —— ELF `7F 45 4C 46`、PE `4D 5A`、Mach-O `CF FA ED FE`/`FE ED FA CF`。

- 至少一个被识别、且**全部**与本机格式不符 → 判为异平台，重跑 install；
- 无法识别（脚本、纯数据）或 `bin/` 不存在 → **判为已装**（不动）；
- 判定后无论结论如何都补写戳，实现自愈（下次不再启发式）。

### 4.2 B-2 payload-gated 过滤

`llvm.lua` 的两个 `os.host() == "windows"` 条件改为看**文件名本身**：`.dll` 永远不是 program（任何 host）；`.exe` 后缀永远剥掉（任何 host）。载荷内容决定语义，不是 host。

### 4.3 B-3 alias 门禁

alias 循环计命中数；`#aliases > 0 且命中 == 0` → `log.error` + `return false`。这是 #447 门禁（"声明了 program 却零注册即失败"）的补集。

---

## 5. 任务拆分（TDD）

> 约定：xlings 侧 `mcpp build` / `mcpp test`（**不用裸 xmake**，memory `reference_build_xlings_via_mcpp`）；单测 gtest + C++23 modules；e2e bash。pkgindex 侧 pytest。

### A-1 alias 执行期归一化 ★

**A-1.1 先写测试**（`tests/unit/test_xvm_shim.cpp` 末尾新增一节）

```cpp
// ── subos path normalization (2026-07-30) ────────────────────────────
//
// An install-time absolute subos path baked into an alias survives every
// `subos use`, because the versions DB is shared by the whole home and
// nothing rewrites it. These pin the rewrite that happens at exec time.

namespace {
std::string norm(const std::string& text,
                 const std::string& home,
                 const std::string& active) {
    return xlings::xvm::normalize_subos_paths(text, home, active);
}
}  // namespace

TEST(SubosPathNormalizeTest, RewritesBakedSysrootToActiveSubos) {
    EXPECT_EQ(norm("g++ --sysroot=/home/u/.xlings/subos/dev-hello",
                   "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              "g++ --sysroot=/home/u/.xlings/subos/default");
}

TEST(SubosPathNormalizeTest, KeepsSuffixAfterSubosName) {
    EXPECT_EQ(norm("cc -I/home/u/.xlings/subos/dev/usr/include -O2",
                   "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              "cc -I/home/u/.xlings/subos/default/usr/include -O2");
}

TEST(SubosPathNormalizeTest, LeavesForeignSubosPathsAlone) {
    // A user's own /opt/subos/... is not ours. Byte-identical passthrough.
    const std::string cmd = "tool --root=/opt/subos/foo/bar";
    EXPECT_EQ(norm(cmd, "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              cmd);
}

TEST(SubosPathNormalizeTest, AcceptsProjectSubosByDotXlingsSuffix) {
    // Project mode bakes <projectDir>/.xlings/subos/<name>, which is not
    // under homeDir at all -- the `.xlings` suffix rule is what catches it.
    EXPECT_EQ(norm("g++ --sysroot=/w/proj/.xlings/subos/anon",
                   "/home/u/.xlings", "/w/proj/.xlings/subos/dev"),
              "g++ --sysroot=/w/proj/.xlings/subos/dev");
}

TEST(SubosPathNormalizeTest, IsIdempotent) {
    const std::string cmd = "g++ --sysroot=/home/u/.xlings/subos/default";
    EXPECT_EQ(norm(cmd, "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              cmd);
    EXPECT_EQ(norm(norm(cmd, "/home/u/.xlings", "/home/u/.xlings/subos/default"),
                   "/home/u/.xlings", "/home/u/.xlings/subos/default"),
              cmd);
}

TEST(SubosPathNormalizeTest, RewritesEveryOccurrence) {
    EXPECT_EQ(norm("g++ --sysroot=/h/.xlings/subos/a -B/h/.xlings/subos/a/usr/lib",
                   "/h/.xlings", "/h/.xlings/subos/b"),
              "g++ --sysroot=/h/.xlings/subos/b -B/h/.xlings/subos/b/usr/lib");
}

TEST(SubosPathNormalizeTest, EmptyActiveDirIsNoOp) {
    // Never rewrite to nothing: an unresolvable active subos must leave the
    // alias as it was, so the failure names the real path.
    const std::string cmd = "g++ --sysroot=/h/.xlings/subos/a";
    EXPECT_EQ(norm(cmd, "/h/.xlings", ""), cmd);
}

TEST(SubosPathNormalizeTest, HandlesWindowsSeparators) {
    EXPECT_EQ(norm("g++ --sysroot=C:\\Users\\u\\.xlings\\subos\\dev",
                   "C:\\Users\\u\\.xlings", "C:\\Users\\u\\.xlings\\subos\\default"),
              "g++ --sysroot=C:\\Users\\u\\.xlings\\subos\\default");
}
```

`mcpp test` → 编译失败（函数不存在）。

**A-1.2 实现**（`src/core/xvm/db.cppm`，紧接 `expand_path` 之后 —— shim / doctor / registration 都已 `import xlings.core.xvm.db`，且此函数不依赖 `Config`，保持纯函数可测）

```cpp
// ── subos-relative alias/env normalization (2026-07-30) ──────────────
//
// `gcc.lua` bakes `--sysroot=<subos_sysrootdir()>` -- the ABSOLUTE path of
// whichever subos was active at install time -- into the alias. The versions
// DB is shared by the entire home and `subos use` rewrites nothing, so that
// path outlives every switch: the user switches to `default` and their g++
// keeps compiling against `dev-hello`. Re-point such paths at the subos THIS
// process resolves to, at the moment the alias is executed. Doing it here
// rather than at install time is what makes existing homes correct without a
// reinstall.
//
// Only provably-ours paths are touched: the segment before /subos/ must be the
// home itself, or end in `.xlings` (which is how a PROJECT subos --
// <projectDir>/.xlings/subos/<name>, not under homeDir at all -- is caught).
// A user's own /opt/subos/foo, the flags around the path, and all quoting come
// through byte-identical.
std::string normalize_subos_paths(const std::string& text,
                                  const std::string& xlings_home,
                                  const std::string& active_subos_dir) {
    if (active_subos_dir.empty() || text.empty()) return text;

    static constexpr std::string_view kPosix = "/subos/";
    static constexpr std::string_view kWin   = "\\subos\\";

    auto is_sep = [](char c) { return c == '/' || c == '\\'; };
    // Where a path token can start: whitespace, plus the punctuation that
    // glues a path onto a flag (`--sysroot=`, `-I` is not a boundary but the
    // preceding space is, `PATH=a:b`, quotes).
    auto is_boundary = [](char c) {
        return c == ' ' || c == '\t' || c == '=' || c == ':' || c == ';'
            || c == ',' || c == '"' || c == '\'';
    };
    auto path_equal = [](std::string_view a, std::string_view b) {
#if defined(_WIN32)
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca == '\\') ca = '/';
            if (cb == '\\') cb = '/';
            ca = static_cast<char>(std::tolower(static_cast<unsigned char>(ca)));
            cb = static_cast<char>(std::tolower(static_cast<unsigned char>(cb)));
            if (ca != cb) return false;
        }
        return true;
#else
        return a == b;
#endif
    };

    std::string out;
    out.reserve(text.size());
    std::size_t cursor = 0;

    while (cursor < text.size()) {
        auto p1 = text.find(kPosix, cursor);
        auto p2 = text.find(kWin, cursor);
        auto hit = std::min(p1, p2);   // npos is max, so min() picks the real one
        if (hit == std::string::npos) break;

        // Widen left to the start of the path token.
        std::size_t start = hit;
        while (start > cursor && !is_boundary(text[start - 1])) --start;
        // ':' is a boundary (PATH=a:b), which would cut the drive letter off
        // `--sysroot=C:\Users\...`: prefix becomes `\Users\u\.xlings` and the
        // replacement splices a second drive spec onto the surviving `C:`.
        // Step back over a drive letter that is itself token-initial.
        if (start >= cursor + 2 && text[start - 1] == ':'
            && std::isalpha(static_cast<unsigned char>(text[start - 2]))
            && (start < cursor + 3 || is_boundary(text[start - 3]))) {
            start -= 2;
        }
        std::string_view prefix(text.data() + start, hit - start);

        // Right: the subos NAME segment only.
        std::size_t nameStart = hit + kPosix.size();
        std::size_t nameEnd = nameStart;
        while (nameEnd < text.size()
               && !is_sep(text[nameEnd]) && !is_boundary(text[nameEnd])) {
            ++nameEnd;
        }

        const bool ours =
            path_equal(prefix, xlings_home)
            || (prefix.size() >= 7
                && path_equal(prefix.substr(prefix.size() - 7), ".xlings"));

        if (!ours || nameEnd == nameStart) {
            out.append(text, cursor, nameEnd - cursor);   // passthrough
        } else {
            out.append(text, cursor, start - cursor);
            out.append(active_subos_dir);
        }
        cursor = nameEnd;
    }
    out.append(text, cursor, std::string::npos);
    return out;
}
```

`mcpp test` → 8 个断言全绿。

**A-1.3 接入 shim**（`src/core/xvm/shim.cppm:443`）

```cpp
        // The alias may carry an install-time subos path; re-point it at the
        // subos this process resolves to (project / env / global all handled
        // by xvm_artifact_subos_dir).
        std::string alias_cmd = normalize_subos_paths(
            vdata->alias[0], xlings_home,
            Config::xvm_artifact_subos_dir().string());
```

**A-1.4 e2e 差分测试**（`tests/e2e/subos_alias_sysroot_follows_active_test.sh`）

沿用 `self_doctor_multi_subos_test.sh` 的 fixture-index 模式（`RUN_IN <subos>` + `XLINGS_ACTIVE_SUBOS`）。fixture recipe：

```lua
package = {
    spec = "1", name = "sr-probe",
    description = "alias sysroot normalization fixture",
    authors = {"xlings-ci"}, licenses = {"MIT"}, type = "package",
    archs = {"x86_64"}, status = "stable", categories = {"test-fixture"},
    xpm = { linux = { ["1.0.0"] = {} }, macosx = { ["1.0.0"] = {} },
            windows = { ["1.0.0"] = {} } },
}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.system")
import("xim.libxpkg.xvm")
function install()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    -- the alias TARGET: prints whatever sysroot it was handed
    io.writefile(path.join(bindir, "sr-real"),
                 "#!/bin/sh\necho \"probe-args: $*\"\n")
    return true
end
function config()
    local bindir = path.join(pkginfo.install_dir(), "bin")
    -- exactly what gcc.lua does: bake the install-time absolute subos path
    xvm.add("sr-probe", {
        bindir = bindir,
        alias  = "sr-real --sysroot=" .. system.subos_sysrootdir(),
    })
    return true
end
function uninstall() xvm.remove("sr-probe") return true end
```

断言：

```bash
chmod +x "$HOME_DIR/data/xpkgs/xim-x-sr-probe/1.0.0/bin/sr-real"

# 1) installed while `dev` is active -> the DB really does bake dev
RUN_IN dev install sr-probe >/dev/null 2>&1 || fail "install failed"
grep -q '/subos/dev' "$HOME_DIR/.xlings.json" \
  || fail "precondition lost: the alias no longer bakes the install-time subos"

# 2) THE ASSERTION: running from `default` must target default's sysroot
out="$(RUN_IN default "$HOME_DIR/subos/default/bin/sr-probe" 2>&1 || true)"
case "$out" in
  *"/subos/default"*) : ;;
  *) fail "shim still targets the install-time subos: $out" ;;
esac
case "$out" in
  *"/subos/dev"*) fail "install-time subos leaked into exec: $out" ;;
esac
```

> 差分性要求：**先确认第 1 步的前置条件仍然成立**。否则 recipe 哪天不再烧路径，这个测试就变成不可伪证的空过（memory `reference_isolated_home_test_traps`）。
> `XLINGS_BIN` 用绝对路径，不要 `find … | head -1`（memory `reference_e2e_xlings_bin_selection`）。

### A-2 `envs` 同源归一化 —— 依赖 A-1

`setup_envs` 增第 4 参 `const std::string& active_subos_dir`，两个调用点（`shim.cppm:441`、`:491`）传 `Config::xvm_artifact_subos_dir().string()`：

```cpp
    for (auto& [key, value] : vdata.envs) {
        auto expanded = normalize_subos_paths(
            expand_path(value, xlings_home), xlings_home, active_subos_dir);
```

测试：`SubosPathNormalizeTest` 已覆盖纯函数；再加一条 env 路径的单测（`envs = {{"SYSROOT", "/h/.xlings/subos/a"}}` → 读出 `/h/.xlings/subos/b`）。
（`gcc.lua` 当前的 `envs` 是注释掉的，但 llvm/其他 recipe 随时会用 —— 这是关掉出口，不是修现象。）

### A-3 doctor 可见 + `--fix` 一次性重写 —— 依赖 A-1

1. `FindingKind` 新增 `SubosPathBaked`（`doctor.cppm:92` 附近）；level = **Warning**（不计入退出码：执行期已被 A-1 修正，它是"该清理的历史包袱"，不是"坏了"）。
2. detect：遍历 DB 每个 `alias[0]` 与每个 `envs` 值，`normalize_subos_paths(...) != 原值` 即命中，报 `target = "<program>@<version>"` 并给出 old→new。
3. `--fix`：把归一化结果写回 DB。注意 `doctor.cppm:70` 现有策略 `alias warning → not auto-fixed` 说的是 `AliasUnresolved`（可能是有意的外部命令）；`SubosPathBaked` 不同 —— 它的改写目标由我方唯一解析点算出，无歧义，**可以自动修**。这条策略差异必须写进 `doctor.cppm` 头部注释，否则下一个人会以为是漏改。
4. 写回走既有原子写；**不要**对已 flock 的文件 rename（memory `reference_atomic_write_vs_flock`）。

测试：`tests/unit/test_xvm_doctor.cpp` 加两例（命中 + `--fix` 后 detect 为空，即幂等）。

### A-4 悬空 sysroot 链接 —— 与 #419/#423 重叠

1. detect：遍历 `<subos>/usr/include`、`<subos>/usr/lib` 下的符号链接，`[ -L ] && ! [ -e ]` 即悬空。
2. `--fix`：删除悬空链接（安全、本地，与 orphan shim 同级）。
3. **测试必须用 `[ -L ]` + `[ -e ]` 两条**：悬空链接 `[ -e ]` 为假，只用 `-e` 会把"链接还在"读成"已经清干净了" —— #423 的原测试就是这么假过的。

### A-5 隔离护栏

在物化 sysroot 链接的地方，若 link target 不在 `Config::paths().dataDir` 之下（且不在 project 的 data 根之下），拒绝并具名报错。这会直接阻止 `/tmp/tmp.XXXX/mcpp-home/registry/...` 落进真实 home。

测试：单测构造 target 在 dataDir 外 → 期望失败且**磁盘零改动**。

### B-1 载荷平台戳

**测试先行**（`tests/unit/test_xim_install.cpp`）：
1. 造 `bin/clang.exe`（写入 `MZ` 魔数）的假载荷 + `alreadyInstalled=true` → 期望**判为异平台**（重跑 install）；
2. 造 `bin/clang`（`\x7fELF`）→ 期望判为已装；
3. `bin/` 只有 `#!/bin/sh` 脚本 → **inconclusive → 判为已装**（宁放过不误杀）；
4. 戳存在且 `os` 匹配 → 直接已装，不做魔数探测；
5. 无戳且判为已装 → **补写了戳**（自愈）。

实现：`installer.cppm:2213` 改为 `bool payloadInstalled = node.alreadyInstalled && payload_matches_host_(expanded);`，成功安装后写戳。

### B-2 / B-3 pkgindex

`pkgs/l/llvm.lua`：去掉两处 `os.host() == "windows" and`；alias 循环加命中计数与失败返回。

pytest（`tests/l/test_llvm.py`，沿用 `parse_xpkg(PKG_FILE)` + `meta.raw_content` + `@pytest.mark.static`）：

```python
@pytest.mark.static
def test_dll_filter_is_not_host_gated():
    src = parse_xpkg(PKG_FILE).raw_content
    assert 'os.host() == "windows" and name:sub(-4) == ".dll"' not in src, \
        "a .dll is never a program on any host -- payload decides, not os.host()"

@pytest.mark.static
def test_alias_loop_fails_when_nothing_registered():
    src = parse_xpkg(PKG_FILE).raw_content
    assert "alias_hits" in src and "log.error" in src, \
        "an alias table where every entry missed must fail the install (#447's complement)"
```

### B-4 未激活提示

`commands.cppm:394` 的 `"{}@{} is already installed"` 之后，若该 target 有活动版本且不是本次请求的版本，追加一行具名提示 + `--use` 出口。e2e 断言输出里出现该提示。

---

## 6. 跨仓库依赖与发布顺序

```
openxlings/xlings          A-1 → A-2 → A-3        (A-1 是三者的共同依赖)
                           A-4, A-5, B-1, B-4     (彼此独立)
openxlings/xim-pkgindex    B-2, B-3               (独立，可与上面并行)
mcpplibs/libxpkg           不需要改动             ← 方案 2 相对方案 1 的决定性优势
```

顺序要点：

1. **A 与 B 无顺序依赖**，可并行合入。
2. pkgindex 的 B-2/B-3 合并后，CI 仍会拉到**旧索引产物**一段时间（memory `reference_index_publish_lag`）—— 此期间 llvm 相关失败可能是陈旧索引而非新 bug，不要据此回滚。
3. 若希望 xlings CI 立刻验证新 `llvm.lua`，需同步 bump **6 个 workflow 的 `XIM_PKGINDEX_REF`**（memory `project_ci_index_ref_pin`）。
4. fresh-install CI 的 llvm 单元格目前是**已知红**（memory `project_fresh_install_ci`）；B-2 可能改变其表现，判读时以"是否仍是同一原因"为准。
5. PR 合并用 Sunrisepeak 账号 squash + `--admin`（memory `feedback_merge_as_sunrisepeak_bypass_squash`）。

**建议开的 issue**（当前最大号 #453，下一个 ≈ #454）：①②③ 合一（A 族，引用 #408 作为上位设计）、④ 挂到 #419/#423、⑤ 单独一条（隔离泄漏）、⑥⑦⑧ 合一（B 族，引用 #447）、⑨ 单独一条。

---

## 7. 验收标准

| # | 判据 | 方法 |
|---|---|---|
| V1 | 全新 home：`subos create dev` → `use dev` → `install gcc` → `use --global default` → `g++` 的 sysroot 指向 **default** | A-1.4 的 e2e |
| V2 | 存量 home（用户现场）**不重装**即修好 | 从真实 home 切片复现（`.agents/tools/slice-real-home.sh`，memory `reference_repro_from_real_home_slice`），跑 `verify-untouched` |
| V3 | `xlings self doctor` 报出 14 条烧死 alias；`--fix` 后再 detect 为空且幂等 | A-3 单测 + 切片手测 |
| V4 | `dev-hello` 的 3 个悬空链接被 doctor 报出并可清 | A-4，断言用 `[ -L ]` + `[ -e ]` |
| V5 | 隔离 home 的运行**不再**往真实 home 写 sysroot 链接 | A-5 单测 + 一次隔离 mcpp 运行后 `verify-untouched` |
| V6 | Linux 上 `install llvm@20.1.7`（store 里留着 Windows 载荷）**重跑 install 钩子**，注册的是 `clang` 而非 `clang.exe`；`libomp.dll` 不再是 program | B-1 单测 + 切片手测 |
| V7 | alias 表全 miss 时安装**失败**（非成功 + 6 条 warn） | B-3 pytest + 一次真实 llvm 安装 |
| V8 | 六个 workflow 全绿 | CI |

---

## 7.1 实施记录（与计划的偏差）

实施于 2026-07-30，随 `2026.7.30.1` 发布。三处与本文原计划不同，记录理由：

| 项 | 计划 | 实际 | 理由 |
|---|---|---|---|
| **A-5 隔离护栏** | 拒绝物化 `dataDir` 之外的 sysroot 链接 | **降级为 warning，仍然物化** | 硬拒绝打掉了 3 个既有单测（`XvmHeaderSymlinkTest`），而它们的 `/tmp` 源路径是合法用法的缩影：recipe 完全可以合法暴露 store 之外的头文件（包装系统头文件是最明显的一种）。拒绝会破坏今天能用的包，与"生态全部可用"直接冲突。**告警在事发当场给出**，而残留由 A-4 清理 —— 这一组合覆盖了实测损害，且不改变任何现有行为 |
| **B-1 载荷戳的读写** | 用 `nlohmann::json` | 手写/手读三个字段 | 在 `installer.cppm` 顶部实例化 JSON 解析器触发 GCC 16 modules 故障：`failed to load pendings for 'std::map'`，且报错指名的是**另一个**模块（`xlings.core.xvm.commands`）。同 memory `reference_gcc16_modules_map_ide` 的形状。戳只有三个字符串字段、无用户输入，手写是完备的 |
| **A-1 归一化规则** | 只处理 `/subos/` 前缀判定 | 追加两条边界修正 | ① Windows 盘符：`:` 是 token 边界，会把 `C:` 切掉并拼出第二个盘符 → 回退两个字符；② 紧贴路径的 flag：`-B/h/.xlings/subos/a` 会把 `-B` 一起吞掉 → 向右推进到路径真正开始处。两条都有单测 |

### 真实环境验证暴露的两个追加问题（B-1 的两次返工）

拿实测 home 的切片跑真实 `install llvm@20.1.7`，第一版 B-1 **判对了却修不好**，而且比原来更糟：

1. **判定层次错了。** 平台校验放在 installer 里，可那时 plan 已经按"已安装"建好 —— **没有下载任何产物**。install 钩子于是空手运行，而 `llvm.lua` 的 `os.tryrm(install_dir)` 把原本还在的载荷删了。
   → 判定**上移到 catalog 的 `exists && !is_empty` 探针**（`catalog.cppm:368`）—— 正是它让 `installed` 为真、让 plan 为空。异平台载荷现在从计划阶段就不算已装，于是像任何缺失包一样下载重装。分类器因此拆成独立模块 `xim/payload.cppm`（catalog 不能 import installer）。
2. **戳写早了。** 戳是在 install 块之后**无条件**写的，包括什么都没装的情况。于是上一条那个被清空的目录带上了**本平台的戳**，之后每次运行都信它 —— 自愈变成了自我欺骗。
   → 只在**确实走过安装路径**（`!payloadInstalled`）且**文件内容不矛盾**时才写；内容判定用 `classify_payload_content`（不读戳），否则一次运行可以自证自话。
3. **戳本身骗过了空目录探针。** 只含一个戳的目录 `!is_empty` 为真 = "已安装"。
   → `payload_has_content()` 忽略我方 bookkeeping 文件；`.xim-installed` **故意仍然计入** —— 对 wrapper 包它的含义正是"已装，这里本就没东西"。

**验证结果**（切片，真实 store + 真实网络）：`install llvm@20.1.7` 真的重新下载并安装了 Linux 载荷（`bin/clang -> clang-20`、`clang.cfg` 出现），`.exe/.dll` 注册数 **29 → 0**，`clang`/`clang++`/`cc`/`c++`/`llvm-ar` 都在 20.1.7 上注册，`xlings use clang 20.1.7` 后 `clang --version` 输出 **clang version 20.1.7**。
首次 install 会在 config 阶段停在既有的 `xvm-owned-group-incomplete` 门禁（旧的 29 条 `.exe` 注册属于同一个 owned group），提示"先 uninstall 再 install"——**这是既有守卫，不是本次引入**；`remove` → `install` 走通全流程。

**交付清单**（xlings 侧 11 个文件）：`db.cppm`(+归一化纯函数) / `shim.cppm`(alias+envs 接入) / `doctor.cppm`(2 个新 finding + 2 条修复) / `installer.cppm`(载荷平台判定+戳) / `xim/commands.cppm`(未激活提示) / `xvm/commands.cppm`(越界告警) / 2 个新 e2e + `run_all.sh` 注册 / 2 个单测文件 +18 个断言 / 版本号。

**测试**：单测 25 文件全绿（新增 `SubosPathNormalizeTest` 10 例、`PayloadPlatformTest` 8 例）；新增 2 个 e2e 均已确认是**真差分**（对 `2026.7.29.2` 的二进制运行会失败，且失败在正确的断言上）；另跑通 10 个回归敏感的既有 e2e。pkgindex 侧 613 个 static 测试全绿，2 个新断言同样确认差分。

---

## 8. 不做什么（边界）

- **不做 #408 的整体重设计**。sysroot / bin / lib / 头文件的多版本共存与切换模型仍归 0.5 线。本方案只做其中"alias 里的 selection 副本"这一条，因为它 100% 在我方进程内、执行期可重解析、且能修存量。
- **不改 `gcc.lua` 的 `--sysroot` 注入方式**（方案 1）。它需要 recipe capability probe 与跨仓协调，而收益（少烧一次路径）在方案 2 落地后接近于零。留作后续清洁化。
- **不在 `subos use` 时批量重写 DB**（方案 3）。env-spawn 与 project 模式从不写 DB，做了也不对，还多一个整文档重写窗口。
- **不动 `subos/current` 的现有用途**（`XLINGS_BIN`、shim 发现）。本方案不引入第二套 subos 解析路径 —— 恰恰相反，它把 alias 收拢到已有的唯一解析点 `Config::xvm_artifact_subos_dir()`。
- **不自动删除异平台载荷目录**。B-1 只是重跑 install 钩子（钩子自己会 `os.tryrm(install_dir())`）；直接 `rm -rf` store 目录属于卸载语义，不该由"已装判定"顺手做。

---

## 9. 评估报告：subos 路径的记录形式（2026-07-31）

**起因**：`2026.7.30.1` 之后我一直主张"recipe 不用改，执行期已纠正"，并把改
recipe 的成本估成"跨仓 + capability probe"。被追问后重查，**这个成本估算是错的**，
下面是重估的完整过程和结论。

### 9.1 先纠正一处事实错误

我先前说"改 recipe 需要 capability probe，否则老客户端拿到不认识的
`${XLINGS_SUBOS}` 占位符"。这只对 **`${XLINGS_SUBOS}` 这一种方案**成立 —— 那是
§3.2 里被否掉的方案 1，我把它的成本当成了"改 recipe"的成本。

实际上仓库里**早就有**一个不需要任何协议的动态路径：

```
~/.xlings/subos/current -> default          # symlink
```

`xself/init.cppm:270` 建立它，`subos.cppm:865`（`subos use --global`）维护它，
`Config::list_subos_names()` 显式把 `current` 排除在 subos 列表之外。它是**已经
存在、已经维护、老客户端天然认识**的东西 —— 因为它就是一个路径。

### 9.2 四个角度的评估

#### 架构

| | 烧绝对 subos 路径（现状） | 写 `subos/current` |
|---|---|---|
| 记录的含义 | "安装那一刻活动的是谁" —— **一个与包无关的事实** | "跟着用户走" |
| 谁负责纠正 | 只有新客户端的执行期归一化 | 路径自己 + 执行期归一化 |
| 老客户端 | **冻结在安装时**的 subos | 跟随全局切换 |
| 层次 | 状态里混进了一次性上下文 | 状态里只有意图 |

关键判断：**`sysroot` 是"当前环境"的属性，不是"这个包版本"的属性**，把它固化进
按 (target, version) 存储的记录本身就是层次错配。`current` 不解决全部（见稳定性），
但它把记录从"一次性事实"降级成"一个可解引用的引用"，方向是对的。

#### 稳定性

`current` **只跟踪全局持久选择**，跟不上另外两种选择模式：

| 选择模式 | `current` 能跟上吗 | 执行期归一化能吗 |
|---|:---:|:---:|
| 持久 `activeSubos`（`subos use --global`） | ✅ | ✅ |
| `XLINGS_ACTIVE_SUBOS` 环境变量（每 shell） | ❌ | ✅ |
| 项目 subos（`<project>/.xlings/subos/<n>`） | ❌ | ✅ |

所以 **`current` 不是执行期归一化的替代品，是它的下位兜底**：新客户端两者都有，
老客户端至少从"冻结"升级成"跟随全局"。**不存在回退风险** —— 今天老客户端拿到的
是一个更差的固定值。

另一处稳定性问题是这次顺带查出来的：`update_current_symlink_` 用
`fs::create_directory_symlink`，而 `self init` 用 `platform::create_directory_link`。
**两者在 Windows 上不等价**（符号链接要开发者模式/提权，junction 不要），也就是
init 铺下一个 junction、之后每次切换都更新失败，而 `subos list` 照常报告切换成功
—— 又一例"没发生和成功了输出一致"。已统一为 `platform::create_directory_link`。

#### 简洁实现

不需要新 API、不需要 probe、不需要 libxpkg 改动。recipe 侧就是一次字符串替换：

```lua
local function portable_sysroot()
    local dir = system.subos_sysrootdir()
    return (dir:gsub("([/\\])subos([/\\])[^/\\]+", "%1subos%2current", 1))
end
```

xlings 侧的配套改动只有两处，都复用了已有函数：

1. **doctor 的检测基准从"活动 subos"换成"`current`"**。判据不变（"归一化会不会
   改变这个字符串"），只是归一化的目标换了：写死具体 subos 的会变，已经写
   `current` 的不会变。**两行**。
2. **`--fix` 也改写成 `current`**，而不是活动 subos。

#### 跨平台

- symlink/junction 差异见上，已统一。
- recipe 的替换同时处理 `/` 和 `\`。
- `current` 在 Windows 上是目录 junction，编译器沿着它解析 `--sysroot` 没有问题。
- 执行期归一化本来就同时认两种分隔符（`SubosPathNormalizeTest.HandlesWindowsSeparators`）。

### 9.3 评估中发现的真正缺陷：`--fix` 修不掉自己报的问题

改完检测基准后 E2E-46 的 A5 立刻失败，暴露出一个**现状就存在**的缺陷：

> `--fix` 把记录改写成**当前活动 subos 的绝对路径** —— 也就是**把钉子从一个
> subos 挪到另一个**。在另一个 subos 里跑 `doctor`，同一条告警原样再报一次。

也就是说这条 finding 的 remedy 在多 subos 的 home 上**永远无法清零**，而它的
文案是"`--fix` rewrites the record"。这不是我引入的，是原本就在那儿、只有把检测
基准挪到 `current` 才会暴露出来的。

修法与上面的方案收敛到同一点：**`--fix` 改写成 `<home>/subos/current`**。于是

- 检测与修复用同一个基准，`--fix` 幂等且真的能清零；
- 修好的记录对**所有** subos 同时正确，不再需要逐个 subos 跑一遍 `--fix`；
- 执行期行为完全不变（`current` 照样被归一化成真正活动的目录）。

### 9.4 结论与修正后的决策

| 项 | 先前决策 | 修正后 |
|---|---|---|
| 改 recipe 的成本 | 跨仓 + capability probe | **跨仓，不需要 probe** |
| 收益 | 只有"doctor 少一条常驻告警" | 加上"老客户端不再冻结"+"`--fix` 能清零" |
| 结论 | 不做 | **xlings 侧先做**（检测基准 + `--fix` 目标 + Windows 链接一致性），recipe 侧随后独立 PR |

xlings 侧已在 `2026.7.31.1` 分支实现，测试：
`SubosPathNormalizeTest.TheCurrentSpellingStillNormalizesAtExecTime` /
`.NormalizingTowardsCurrentLeavesTheCurrentSpelling` /
`.NormalizingTowardsCurrentStillMovesAPinnedSubos`，以及 E2E-46 **A7**
（recipe 写 `current` → doctor 不报 + shim 仍然到达活动 subos）。

**顺序不敏感**：xlings 侧先落地是安全的（老 recipe 照旧被检测和修复），recipe 侧
先落地也不会坏（只是那条告警继续在旧 xlings 上出现）。这正是它不需要 probe 的
另一种说法。

**仍然不做的**：`${XLINGS_SUBOS}` 占位符方案（§3.2 方案 1）。它需要新协议、需要
probe，而 `current` 已经把同一个问题解决到"新客户端完全正确、老客户端明显更好"。

### 9.5 用"共享 vs per-subos"这条原则复审整个方案

原则（用户提出）：**payload 里的一切应尽量通用（与 subos 无关）；subos / sysroot / xvm
这一层才允许与 subos 相关。**

先把"哪一层是共享的"查实（读真实 home，不是推断）：

| 层 | 物理位置 | 存什么 | **是否 per-subos** |
|---|---|---|:---:|
| payload | `data/xpkgs/<ns>-x-<pkg>/<ver>/` | 二进制 + 自带配置（gcc 的 `specs`） | **共享** |
| **版本库 (versions DB)** | `~/.xlings/.xlings.json` → `versions.*` | `path` / **`alias`** / `envs` / `bindings` | **共享** |
| workspace | `subos/<n>/.xlings.json` | 只有 `{name: {active, installed[]}}` | per-subos |
| sysroot / shim | `subos/<n>/{usr,lib,bin}` | 物化出来的视图 | per-subos |

实测值：

```
~/.xlings/.xlings.json      versions."g++"."15.1.0".alias
    = ["g++ --sysroot=/home/speak/.xlings/subos/default"]     ← per-subos 的值

~/.xlings/subos/default/.xlings.json
    = {"workspace": {...}}          ← 只有"哪个版本是活的"，没有 alias/envs/path
```

**关键结论：版本库和 payload 在原则的同一侧 —— 都是共享的。** 所以
`--sysroot=<某个具体 subos>` 写进 `alias`，与把 subos 路径写进 `specs`
**是同一个违反，只是高了一层**。"xvm 层可以与 subos 相关"这句话对
**workspace/shim/sysroot** 成立，对**版本库不成立**。

#### 逐项复审

| 项 | 值在哪一层 | 符合原则 |
|---|---|:---:|
| LINK（loader/rpath）→ payload `specs` payload-direct | 共享 | ✅ 新 recipe 符合；旧安装的数据违反 → [#458](https://github.com/openxlings/xlings/issues/458) |
| 载荷平台身份 `.xpkg-install.json` | 共享 | ✅ 记的是 os/arch，与 subos 无关 |
| `payloadForeign` / `installed` 拆分 | 共享 | ✅ |
| **HEADER（`--sysroot`）→ 版本库 `alias`** | **共享** | ❌ **仍然违反** |
| workspace 的 `active/installed[]` | per-subos | ✅ |
| 悬空链接扫描 / shim / sysroot 物化 | per-subos | ✅ |
| `use` 确定性、group 遗留成员报告 | per-subos（workspace） | ✅ |

#### 那一处违反的性质

已做的两步都是**绕过**，不是**消除**：

- **执行期归一化**（`2026.7.30.1`）：让*行为*正确 —— 但记录里仍然是一个 per-subos 的值，
  躺在共享库里。所以才需要 doctor 有一条检查、需要 `--fix`、需要老客户端讨论。
- **改写成 `subos/current`**（本轮）：把"钉死一个 subos"降级成"一个可解引用的引用"，
  **减轻**了违反（记录不再断言"是 default"），但**没有消除** —— 共享库里仍然存着一个
  只在某个 subos 语境下才有意义的字符串。

**判据很简单：如果这条记录是完全通用的，那么 doctor 就不需要为它设一条检查。**
它需要，就说明它不通用。

#### 真正符合原则的形态（第三个选项，之前没考虑过）

不存路径，存**意图**——让本来就知道活动 subos 的那一层去合成：

```lua
xvm.add(prog, { bindir = ..., alias = prog, sysroot = true })   -- 声明"我要 subos sysroot"
```

shim dispatch 时由 xlings 拼出 `--sysroot=<活动 subos>`。于是：

- 共享库里**一个 subos 相关的字符串都没有** → 原则完全成立；
- `normalize_subos_paths` 对这条不再需要，`SubosPathBaked` 这条 finding **整条消失**，
  连同它的 `--fix`、它的收敛问题、它的多 subos 讨论；
- 三种选择模式天然正确（合成时才决定）。

代价是**老客户端不认识 `sysroot` 字段 → 直接丢掉 `--sysroot` → 头文件搜索坏掉**，
这比路径不对更糟。所以它需要一个兼容故事 —— 但**不需要 probe**，双写即可：

```lua
xvm.add(prog, {
    alias   = prog .. " --sysroot=" .. portable_sysroot(),  -- 老客户端沿着 current 走
    sysroot = true,                                          -- 新客户端优先用这个，忽略上面的文本
})
```

新客户端读 `sysroot=true` 就自己合成，压根不看 alias 里那段；老客户端不认识该字段，
沿用 `current`，行为比今天好。**两边都不坏，且不需要探测对方版本** —— 因为回退路径
本身已经够好。

#### 修正后的路线

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 执行期归一化（让行为先正确） | 已发 `2026.7.30.1` |
| 2 | 记录降级为 `subos/current` + `--fix` 可收敛 + doctor 基准对齐 | PR #457，未合 |
| 3 | recipe 写 `current`（HEADER 轴） | 待提，xim-pkgindex |
| 4 | payload 内烧死路径的可见性 | [#458](https://github.com/openxlings/xlings/issues/458) |
| **5** | **`sysroot = true` 声明式注册 + 双写，删除 `SubosPathBaked` 整条链路** | **本节新增，未排期** |

阶段 5 才是"符合原则"，1–3 是让它在到达那里之前不再咬人。把它写在这里，是为了
**不要把阶段 2 当成终点** —— 它是一个刻意接受的、有理由的中间态。
