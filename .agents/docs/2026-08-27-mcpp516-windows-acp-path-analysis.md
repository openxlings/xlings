# mcpp#516 分析:Windows 上 UTF-8 路径归档的"解压失败"

日期:2026-08-27 · 基线:xlings `300aef5` / mcpp `f2dbeec`(2026.8.21.3,缺陷同样存在于已发布的 `v2026.8.26.2`) · 状态:待 review

> Issue: https://github.com/mcpp-community/mcpp/issues/516
> 复现:mcpplibs/mcpp-index PR #260,`mcpp test -p httplib-brotli|httplib-tls|httplib-zstd`

---

## 0. 四行结论

1. **不是解压问题,解压是成功的。** 报错发生在解压**之后**,发生在 **mcpp 自己的进程里**,
   不在 xlings 里。`error: internal: unhandled exception:` 这一串字面量全仓只有一处:
   `mcpp/src/main.cpp:36` 的顶层 catch。xlings 的 `src/` 里 "unhandled" 零命中。
2. **精确抛出点:`mcpp/src/modgraph/scanner.cppm:238`** ——
   `is_excluded_walk_dir()` 里的 `dir.filename().string()`。
   MSVC STL 的 `path::string()` 走 `WideCharToMultiByte(ACP)`,遇到当前代码页拼不出的字符
   就返回 `ERROR_NO_UNICODE_TRANSLATION` 并抛 `std::system_error`,
   `what()` 恰好就是 issue 里那一句 "No mapping for the Unicode character exists in the target multi-byte code page."
3. **这不是新缺陷,是 mcpp#230 的同一处漏网。** #230(0.0.96 / #231)修的就是这个转换,
   但只加固了 `path_matches_glob`、`rewrite_rel_copy`、`local_include_dirs_for` 三处,
   **漏掉了同一个 walk 循环里比它们早一行执行的 `is_excluded_walk_dir`**。
   #516 看到的这条整洁的报错,正是 #230 顺手加的顶层 catch 在正常工作 —— 它把
   `__fastfail`/裸 127 变成了可读的一行,但底下的洞还在。
4. **xlings 侧不背这口锅,但它自己有一个同类的、尚未被触发的隐患。**
   `xim/extract.cpp:232/248` 把 UTF-8 的归档条目名往返穿过 `std::filesystem::path`
   的 ACP 窄↔宽转换。今天在 ACP=1252 上是**碰巧**字节保真才没出事;
   在 DBCS 代码页(936/932/949/950)上这个往返随时可能失败,
   而当 `XLINGS_HOME` 自身含非 ACP 字符时它会**当场抛**同一个异常。

---

## 1. 方法:每条结论怎么来的

按 systematic-debugging 的四阶段走。**没有在 Windows 上实测**(手上没有 Windows 主机),
所以第 6 节单列了"我验证不了的部分"和一个 20 行的决定性确认实验。

已完成的取证:

| 手段 | 得到的结论 |
|---|---|
| 全仓 grep 报错字面量 | 抛出点在 mcpp 进程,不在 xlings 子进程 |
| 读 `mcpp::pm::Fetcher::install` | 下载/解压确实是 xlings 子进程干的(NDJSON `install_packages`) |
| 读 MSVC STL 转换语义 | `what()` 文本与 `_No_unicode_translation` 一一对应 |
| 拉取 PR #260 的 `compat.httplib.lua` | `include_dirs = { "*" }` → 从解压根**无界递归遍历** |
| 读 `expand_dir_glob` 全流程 | `is_excluded_walk_dir` 是这条 walk 上**第一个**窄化点 |
| 读 mcpp#230 的四条评论 | 同一个转换、同一个代码页、同一类文件名,已有先例 |
| `git log -L 236,245` | 该行自 #225 引入起从未被加固 |
| 在 mcpp-index 上做统计 | 128 个 recipe 里 **101 个**含以 `*` 开头的 glob |

---

## 2. 根因链(逐跳)

### 2.1 xlings 把文件正确地解压出来了

`compat.httplib` 由 mcpp 通过 NDJSON 调用 xlings 安装
(`prepare.cppm:2940` 或 `2948` → `Fetcher::install` → `call("install_packages", …)`),
解压走 xlings 的 `xim::extract_archive`(libarchive,进程内)。

盘上落下的目录名是**真正的日文** `日本語Dir`,不是 mojibake。**这一点有硬证据,不是推测**:
mcpp 抛出的是 `ERROR_NO_UNICODE_TRANSLATION`,该错误的前提是**宽字符名里存在 ACP 拼不出的字符**。
如果 xlings 写下的是被 CP1252 打散后的 mojibake(`æ—¥æœ¬èª…Dir`),那些字符**逐个都在 CP1252 里**,
mcpp 的宽→窄转换就不会失败。**mcpp 抛了,恰恰证明 xlings 解压是对的。**

机制上也对得上:`extract.cpp:106-120` 的 `ensure_archive_locale_()` 在 Windows 上
`setlocale(LC_CTYPE, ".UTF-8")`,libarchive 的 `archive_write_disk_windows.c` 因此把
`archive_entry_set_pathname()` 收到的窄字节按 UTF-8 转宽,再 `CreateFileW`。
**这一行 setlocale 是承重的**(见 §5.3)。

### 2.2 mcpp 拿到解压树后,对整棵树做无界递归遍历

`compat.httplib.lua` 的描述符是 Form B(`mcpp = { ... }` TableBody):

```lua
mcpp = {
    include_dirs = { "*" },        -- ← 这里
    sources = { "mcpp_generated/compat_httplib_anchor.cpp" },
    ...
}
```

`include_dirs` 经 `manifest/xpkg.cppm:1242` 落到 `buildConfig.includeDirs`,
再由 `scanner.cppm:788` 的 `local_include_dirs_for()` 交给 `expand_dir_glob(root, "*")`。

`expand_dir_glob`(`scanner.cppm:441`)的收敛逻辑:

```
glob = "*"                          → 含通配符,不走字面量快路径
glob_literal_prefix("*")            → 通配符在第 0 位 → 字面前缀为空
start = prefix.empty() ? root : …   → start = verRoot
recursive_directory_iterator(start) → 遍历整棵 cpp-httplib 源码树
```

于是 `test/www/日本語Dir/` 必然被访问到。`scanner.cppm:796` 的注释把 `"*"`
称作 "the `*` extracted-tarball-root glob convention" —— 这是**约定**,不是 httplib 的怪癖。

### 2.3 遍历循环里的第一个窄化点就抛了

```cpp
// scanner.cppm:482-486(expand_dir_glob),388-391 处同构
if (!e.is_directory(eec) || eec) continue;      // 无转换
if (is_excluded_walk_dir(e.path(), root)) {     // ← 进这里

// scanner.cppm:236-239
bool is_excluded_walk_dir(const std::filesystem::path& dir,
                          const std::filesystem::path& root) {
    auto name = dir.filename().string();                                 // ← 抛
    if (name == ".mcpp" || name == ".git" || name == "target") return true;
```

MSVC STL 的 `path::string()` → `__std_fs_convert_wide_to_narrow(ACP, …)`,
内部对非 UTF-8 代码页传入 `lpUsedDefaultChar`;一旦用了替换字符就返回
`__std_win_error::_No_unicode_translation`(= Win32 1113),调用方
`_Throw_system_error_from_std_win_error` 抛 `std::system_error`。
`std::system_error(ec)` 不带 what_arg 时 `what()` 就是 `ec.message()`,
即 **"No mapping for the Unicode character exists in the target multi-byte code page."** —— 与 issue 逐字一致。

该函数**每个目录条目调用一次**,且位于循环体第一行,
所以它比后面所有已加固的站点都先执行。加固后面三处,对目录名根本无效。

### 2.4 异常一路无人接手,直到 main

`expand_dir_glob` / `local_include_dirs_for` / scanner / prepare / `cli::run`
全链路没有 catch,最终由 `mcpp/src/main.cpp:28-37` 的顶层 catch 接住:

```cpp
} catch (const std::exception& e) {
    std::println(std::cerr, "error: internal: unhandled exception: {}", e.what());
    rc = 70;   // EX_SOFTWARE
```

---

## 3. 为什么是这个包、为什么 Linux/macOS 全绿

- **Linux/macOS**:`path::string()` 只是把 native 的 `std::string` 拷一份,无编码转换,
  永远不抛。整个失效模式在非 Windows 上**不存在**。
- **Windows CI(windows-2025,en-US,ACP=1252)**:1252 是单字节代码页,拼不出任何 CJK 字符。
- **为什么只有 httplib 炸**:`compat.openssl` / `compat.zstd` / `compat.brotli` 的上游 tarball
  里没有非 ASCII 路径。cpp-httplib 是**第一个**带这种路径的依赖
  (`test/www/日本語Dir/`,只有 3 个条目)。它和 brotli/tls/zstd 三个 feature 无关 ——
  三个测试都依赖同一个 `compat.httplib`,所以三个一起挂,这正是 issue 里观察到的形状。

### 爆炸半径不是一个包

在 mcpplibs/mcpp-index 上统计(128 个 recipe):

| 口径 | 数量 |
|---|---|
| 含以 `*` 开头的 glob 字符串(→ 从 verRoot 无界遍历) | **101 / 128(79%)** |
| `include_dirs` 含通配符 | 74 |
| `include_dirs = { "*" }` 原样 | 20 |

也就是说:**任何一个上游 tarball 哪天新增一个非 ACP 文件名,对应的 Windows 构建当天就红**,
而且是以"内部错误"的面貌红,不指向任何可操作的原因。这不是概率很低的事 ——
测试数据里放一个 CJK/西里尔/带重音的文件名,在开源项目里相当常见。

---

## 4. 先例:mcpp#230 修的就是这个,只是漏了一处

#230(0.0.95 起 windows CI exit 127)的根因,按 Sunrisepeak 自己的结论:

> 在 windows runner(ACP=1252)上撞到 vendored xim-pkgindex 里的中文文件名
> `bug-report---问题反馈.md`,`path_matches_glob` 的 `generic_string()` 宽→窄转换抛
> `std::system_error`,未捕获 → `__fastfail`(0xC0000409),git-bash 显示为裸 127。

0.0.96(#231)的修复是三件事:
1. glob walk 里剪掉 `.mcpp`;
2. **"never-throw on unnarrowable names"**;
3. `main()` 顶层 catch(exit 70,不再是裸 127)。

第 2 条落在了三个站点,今天在代码里还留着当时的注释:

| 站点 | 位置 | 是否加固 |
|---|---|---|
| `path_matches_glob` | `modgraph/glob.cppm:47-57` | ✅ try/catch → return false |
| `rewrite_rel_copy` | `modgraph/scanner.cppm:513-518` | ✅ 直接返回原字节 |
| `local_include_dirs_for` | `modgraph/scanner.cppm:780-785` | ✅ 不做 generic_string 往返 |
| **`is_excluded_walk_dir`** | **`modgraph/scanner.cppm:238`** | ❌ **裸 `.string()`** |

`git log -L 236,245:src/modgraph/scanner.cppm` 显示这个函数由 #225 引入、
`.filename().string()` 自始至今未变。#231 当时改的就是它下面一行的排除列表,**擦肩而过**。

这正是本仓 `reference_one_question_many_answerers` / `reference_reporter_repairer_predicate_drift`
记的那个形状:**同一个问题有多个回答者,只加固了其中几个**。
所以 §7 的方案里,P1 比 P0 更重要 —— 再打一个点补丁,下次还会有第五个站点。

---

## 5. xlings 侧的真实情况

### 5.1 被冤枉的部分

"解压逻辑把 UTF-8 归档路径通过 ANSI 代码页转换" —— 这个判断**在结果上不成立**:
文件是以正确的 UTF-16 名字落盘的(§2.1 的反证)。
如果 xlings 真的按 ANSI 转换了,mcpp 反而不会抛这个异常。

issue 里"建议修复方向"的 4 条对 **mcpp** 是对的,对 xlings 是无的放矢。

### 5.2 确实存在的隐患(与本 issue 无关,但同类)

> 已单独立 issue:**openxlings/xlings#571**。不挂在 mcpp#516 下面 ——
> 挂上去会让 #516 看起来"两边都有责任",而 xlings 在那条路径上做对了。

`src/core/xim/extract.cpp` 有三处同类问题,今天**碰巧**没被触发:

**X1 — 重定基路径穿过 ACP 往返(`extract.cpp:232`、`248`)**

```cpp
auto rebased = (canonicalDest / *safeRel).lexically_normal().string();
::archive_entry_set_pathname(entry, rebased.c_str());
```

`*safeRel` 是 UTF-8 字节。在 Windows 上 `path(std::string)` 走
`MultiByteToWideChar(ACP, MB_ERR_INVALID_CHARS, …)`,`.string()` 再走回来:

| ACP | 行为 |
|---|---|
| 1252(en-US runner) | 每个 UTF-8 字节在 1252 里都有定义 → 往返**字节保真** → 侥幸正确 |
| 1252 且字节落在 0x81/0x8D/0x8F/0x90/0x9D | `MB_ERR_INVALID_CHARS` 拒绝 → **抛** |
| 936/932/949/950(DBCS) | UTF-8 字节序列是否构成合法 DBCS 序列**纯看运气**;不合法即**抛** |

抛出后 `extract_archive_detailed` 不接,由 `cli.cpp:1827` 的顶层 catch 变成
`internal error: <what>` + exit 1 —— 不崩,但错误里不含归档 URL、目标路径、失败条目,
和 issue 里抱怨 mcpp 的那句"至少应当报告失败的归档条目"是同一个毛病。

**X2 — 目标目录自身含非 ACP 字符时必抛**

`canonicalDest` 来自 `XLINGS_HOME`。用户名是 CJK 而系统 ACP 拼不出它(例如英文版
Windows + 中文账户名)时,`.string()` 在**任何**归档上都会抛。
这条与归档内容无关,是**今天就能碰到**的路径。

**X3 — 宽名回退路径静默吞名(`extract.cpp:35-42`)**

```cpp
std::string path_from_wide_(const wchar_t* raw) {
    try { return std::filesystem::path(std::wstring(raw)).generic_string(); }
    catch (...) { return {}; }          // ← 宽→ACP 失败 → 空名
}
```

返回空串后 `check_safe_pathname_` 报 `"empty pathname"` —— 这句话是假的:
名字不是空的,是拼不出来。**"没发生"和"成功了"产生同样的输出**的又一例
(见 `project_silent_success_pattern`)。

**X4 — 零测试覆盖**

`tests/unit/test_runtime.cpp` 的 `ExtractFixture` 只造 `src/hello.txt`、
`src/sub/nested.txt`,全 ASCII。xlings 有 `xlings-ci-windows.yml`,
加一个非 ASCII 条目的 case 成本极低,而且是唯一能证伪 X1/X2 的地方。

### 5.3 一条承重的、没有标注的 setlocale

`extract.cpp:106-120` 的 `setlocale(LC_CTYPE, ".UTF-8")` 是 Windows 上非 ASCII
条目名能正确落盘的**唯一原因**。它今天看起来像一句无关紧要的 locale 初始化,
没有任何测试钉住它。哪天有人以"进程级 setlocale 有副作用"为由把它挪走或改成 `""`,
所有非 ASCII 条目会静默变成 mojibake,**且没有任何断言会红**。

### 5.4 一个应当明确拒绝的"xlings 侧修法"

有一种很自然的想法:在 Windows 上把非 ACP 条目名改写成安全形式(转义/哈希)再落盘,
这样下游谁都不会炸。**不要这么做**:它破坏内容完整性,让解压结果与上游 tarball 不一致,
上游构建脚本按原名找文件会失败,而且这个改写在 Linux/macOS 上不发生 ——
制造出一个跨平台不一致的产物。**忠实解压是对的,消费者必须自己处理。**

---

## 6. 我验证不了的部分,和决定性确认实验

**没验证的**:未在 Windows 上实测。§2.3 的抛出点是静态推导,
由五条独立证据支撑(报错字面量归属、MSVC STL 语义、`include_dirs = {"*"}` 的遍历不可避免性、
`is_excluded_walk_dir` 在循环里的位置、#230 的同源先例),但**推导不是测量**。

一个可能的偏差:issue 报告"错误发生在 feature 依赖下载之前"。
若 `compat.brotli` 等在同一 CI job 的先前测试里已装好,就不会打印 `Downloading` 行,
时序与"扫描阶段抛出"一致;但如果它确实是在 `loadVersionDep` 内部抛的,
那抛出点会是 `installedLayoutMatchesIndex` 里的 `expand_glob`
(`prepare.cppm:2761/2772/2780`)—— **同一个 `is_excluded_walk_dir`,结论不变**。

**决定性确认实验(不需要 httplib,不需要重建 mcpp,20 行)**:
在任一 Windows(ACP≠65001)机器或 `windows-2025` runner 上:

```powershell
mcpp new acp-probe && cd acp-probe
mkdir "src\日本語Dir"
'int probe(){return 0;}' | Out-File -Encoding utf8 "src\日本語Dir\probe.cpp"
# 在 mcpp.toml 的 [build] 里加: include_dirs = ["*"]
mcpp build
```

预期(缺陷成立):`error: internal: unhandled exception: No mapping for the Unicode
character exists in the target multi-byte code page.`,exit 70。
若打不出这一句,§2.3 的定位就是错的,应回到 Phase 1 重新取证。

打完 P0 补丁后同一条命令必须变绿 —— 这同时也是回归测试的内容(§8)。

---

## 7. 解决方案

### P0 — 修掉抛出点(mcpp,1 个函数)

`mcpp/src/modgraph/scanner.cppm:236-245`:

```cpp
bool is_excluded_walk_dir(const std::filesystem::path& dir,
                          const std::filesystem::path& root) {
    // 按 path 比较,不要窄化。`filename().string()` 在 MSVC 上走宽→ANSI 转换,
    // 对当前代码页拼不出的名字抛 std::system_error(mcpp#230 / #516)——
    // 而这个函数是 walk 循环体的第一行,每个目录条目都会经过它。
    // 三个字面量都是 ASCII,转成 native(Windows 上是宽)无损;
    // path::operator== 比较的是 native 串,行为与原来的窄串比较逐字一致。
    static const std::filesystem::path kMcpp{".mcpp"};
    static const std::filesystem::path kGit{".git"};
    static const std::filesystem::path kTarget{"target"};
    const auto name = dir.filename();
    if (name == kMcpp || name == kGit || name == kTarget) return true;
    ...
}
```

静态常量而非每次构造临时 path,保住 #225 的 per-entry 开销预算。

### P1 — 把"一个问题多个回答者"收敛掉(mcpp,真正的修复)

P0 只是第四个点补丁。真正要做的是**让这类转换只有一个写法**:

1. 在 `mcpp.modgraph.glob`(已有的"唯一 glob 匹配器"模块)里加一个唯一入口:

```cpp
// 走查得到的路径 → 窄串。失败返回 nullopt,永不抛。
// 任何来自 directory_iterator 的路径都必须经过这里。
std::optional<std::string> try_narrow(const std::filesystem::path& p);
```

2. 审计并改写**所有"来自文件系统走查的路径"的窄化点**。
   下表是复查后的结果 —— 初稿我按文件名扫了一遍就列表,
   那是 `reference_blast_radius_undersampling` 记的那个错(**按文件名统计不是统计**),
   逐个追输入来源之后砍掉了两个误报:

| 站点 | 输入来源 | 复查结论 |
|---|---|---|
| `modgraph/scanner.cppm:238` | 任意包/项目树的目录名 | **P0,确认触发** |
| `pack/digest.cppm:48` | `pack/prebuilt.cppm:144` 的递归走查 | **真**:打包路径不经过 glob 过滤 |
| `scaffold/template.cppm:265` | 模板目录名 | **真**,但只在第三方模板目录含非 ACP 名时 |
| ~~`build/resources.cppm:66`~~ | RcTool 的 `path.filename()` | **误报**:那是 `windres`/`rc.exe`/`llvm-rc`,payload 相对定位,全 ASCII |
| ~~`modgraph/p1689.cppm:339`~~ | `source.filename()` | **误报**:CJK 文件名根本到不了这里,见下 |

   `p1689.cppm:339` 的误报值得单独说,因为它暴露了一个**今天就存在的行为**:
   `path_matches_glob` 的 try/catch(#231 加的)对拼不出的名字返回 `false`,
   于是**一个 CJK 命名的源文件永远不会成为 unit** —— mcpp 今天不是崩,是**静默不编译它**。
   p1689 拿到的 unit 列表里因此不可能有这种名字。
   真正能让 p1689 抛的是**同一行的 `source.string()`(第 341 行,整条绝对路径)**:
   当**项目根目录自身**含非 ACP 字符时(英文版 Windows + 中文项目路径),它会抛;
   但那种情况下 `path_matches_glob` 会先把所有源文件判为不匹配,
   构建先以"没有源文件"失败 —— 又一个 `project_silent_success_pattern`。

3. 写进 `AGENTS.md` 一条不变式:
   **"任何来自 `directory_iterator` 的 `path` 都不得直接 `.string()`/`.generic_string()`。"**
   加一条 CI 的 grep 门(本仓 `project_silent_success_pattern` 里同类门已有先例)。

### P2 — 战略选项:让进程 ACP 变成 UTF-8(独立 issue,不与 #516 绑定)

给 mcpp 的 Windows 可执行文件嵌入带 `<activeCodePage>UTF-8</activeCodePage>` 的清单,
MSVC STL 的 `__std_fs_code_page()` 就返回 CP_UTF8,`path::string()` 从此**返回 UTF-8 且永不抛**,
上面所有站点一次性全部正确。要求 Windows 10 1903+(CI runner 满足)。

**修正:初稿我把它写成"风险大、不能动",这个判断是错的。** 复查如下:

- **它不会让今天能用的场景变差。** 今天能跑通的路径全是 ASCII,
  而 ASCII 在 UTF-8 和 CP1252/936 下**字节完全相同** —— build.ninja、CDB、命令行一个字节不变。
- 真正的未知不是"会不会坏",而是**"够不够"**:mcpp 写出 build.ninja 之后,
  是**另一个进程**(xlings store 里的 `ninja.exe`,`config.cppm:192`)去读它。
  ninja 用窄 API,按**它自己的** ACP 解释字节。所以给 mcpp 嵌清单只解决 mcpp 一半,
  非 ASCII 路径能不能端到端跑通,取决于 ninja(以及编译器驱动)那一侧。
- 因此 P2 的定位是:**安全,但可能不彻底**。它能消灭"mcpp 崩溃"这一类,
  但不保证"CJK 路径的项目能构建"。后者要单独测量,不能顺带宣称。

结论不变(独立 issue),但理由从"危险"改成"收益边界未测"。
若要立,验收标准必须写成"mcpp 不再抛",而不是"支持 CJK 路径"。

### PX — xlings 侧(与 #516 无关,独立修)

1. **Windows 上不做窄往返**:`extract.cpp:224-250` 改成用宽路径 +
   `archive_entry_copy_pathname_w()` / `archive_entry_copy_hardlink_w()`。
   UTF-8 → path 用 `std::u8string` 显式构造(`path(std::u8string)` 是标准里唯一
   明确按 UTF-8 解释的构造),`.wstring()` 交给 libarchive,全程不碰 ACP:

```cpp
inline std::filesystem::path path_from_utf8_(std::string_view s) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
```

2. **`path_from_wide_` 不许静默吞名**:转换失败时返回一个可区分的错误,
   让 `check_safe_pathname_` 报"条目名无法在当前代码页表示",而不是假的 "empty pathname"。
3. **失败信息带上下文**:`ExtractError` 里补归档路径 + 失败条目名
   (issue 提的这条对 xlings 同样成立)。
4. **把 `setlocale(".UTF-8")` 钉住**:加一个断言它有效的测试,否则它随时会被无声移除。
5. **补测试**:`ExtractFixture` 加一个 `src/日本語Dir/日本語File.txt` 条目的 case,
   跑在 `xlings-ci-windows.yml` 上。

---

## 8. 回归测试怎么写才有效(这一节比补丁重要)

**在 Linux/macOS 上写的任何测试都无法证伪这个缺陷** —— 那两个平台上
`path::string()` 不做转换,任何文件名都"能表示"。这正是本仓
`reference_cross_platform_test_traps` 记的那类陷阱。

有效的回归测试必须同时满足:

1. **跑在 Windows CI 上**,且 runner 的 ACP ≠ 65001
   (`windows-2025` 默认 1252,满足;若哪天 runner 镜像改成 UTF-8 ACP,**测试会静默失效**);
2. **仓库里落一个真实的非 ACP 目录名**(例如 `tests/fixtures/glob-acp/日本語Dir/probe.txt`),
   不是运行时创建 —— 运行时用 `create_directory(L"日本語Dir")` 也可以,更省事,
   且不受 git/checkout 配置影响,**推荐后者**;
3. **断言的是"不抛"**,不是"匹配到了几个" ——
   加固后 `日本語Dir` 会被 `path_matches_glob` 判为不匹配(它的 try/catch 返回 false),
   这是**可接受的既有语义**:一个 ACP 拼不出的名字也没法写进 glob 或编译命令。
   把断言写成"必须匹配到"会让补丁看起来没修好;
4. **同时钉住 ACP 前提**:测试开头打印 `GetACP()`,并在 ACP==65001 时 `GTEST_SKIP()`
   并附一句说明。否则镜像一变,这个测试就变成永远绿的装饰品。

建议的最小形状(mcpp 侧):

```cpp
TEST(Scanner, GlobWalkSurvivesUnnarrowableDirName) {
#ifdef _WIN32
    if (::GetACP() == 65001) GTEST_SKIP() << "ACP is UTF-8; this case cannot trigger";
    // 造 <tmp>/日本語Dir/probe.txt,用宽字面量,绕开源文件编码
    ...
    EXPECT_NO_THROW({ (void)mcpp::modgraph::expand_dir_glob(tmp, "*"); });
    EXPECT_NO_THROW({ (void)mcpp::modgraph::expand_glob(tmp, "mcpp.toml"); });
#else
    GTEST_SKIP() << "narrow conversion is a no-op off Windows";
#endif
}
```

再加一条**端到端**的:mcpp-index 里保留 httplib 的三个 example 在 Windows 矩阵上跑
—— 这是唯一能覆盖"真实上游 tarball + 真实 include_dirs 约定"的组合。

---

## 9. 待 review 的决策点

1. **P0 补丁提到哪个仓?** 我的判断:**mcpp**,不是 xlings。
   xlings 侧的 PX 是独立 issue,不应挂在 #516 下面 —— 挂上去会让 #516
   看起来"两边都有责任",而实际上 xlings 在这条路径上做对了。
2. **#516 该怎么回复?** 建议直接给出:抛出点文件行号 + #230 的关联 + §6 的确认实验,
   并说明"解压是成功的"这一反证 —— 报告人给的排查方向是合理的,但结论指错了方向,
   把这一步讲清楚比直接给补丁更有价值。
3. **`try_narrow()` 返回 nullopt 时,调用方该怎么办?**
   这不是 #516 的成因,而是"收敛成唯一入口"这件事**必然要一次性定下来**的口径,
   否则每个调用方还是各答各的 —— 那就没收敛。
   现状(#231 留下的)是**静默丢弃**:`path_matches_glob` 返回 false,
   于是一个 CJK 命名的源文件既不报错也不编译,**它就是不存在**。
   两个选项:
   - **A(保持静默)**:改动最小,与今天行为一致。
     代价:用户加了个 `模块.cppm`,构建成功、符号找不到,没有任何提示。
   - **B(丢弃但记一条 warning,指名文件)**:`try_narrow` 失败时由调用方
     `ui::warn` 一次,内容形如 `skipped: <目录的可拼写前缀>\<无法在代码页 1252 表示的名字>`。
     代价:要给这条 warning 找一个不刷屏的去重点。
   我倾向 **B**,理由是本仓反复吃过的那个亏(`project_silent_success_pattern`):
   "没发生"和"成功了"输出一样。但这是口径不是对错,你定。
4. **P2 立不立独立 issue?** 建议立、不排期。
   注意我上面修正了对它的判断:它**不危险**(ASCII 路径下字节不变),
   只是**可能不彻底**(ninja 是独立进程、按自己的 ACP 读 build.ninja)。
   所以如果立,验收写成"mcpp 不再抛这个异常",不要写成"支持 CJK 路径"。
   它和 #516 的关系是:**它是这一整类问题的根治候选**,但 #516 不靠它也能修好。
5. **PX-4(钉住 setlocale)你是否认同它是"承重"?** 如果你认为
   libarchive 在 Windows 上还有别的路径能正确处理 UTF-8(不依赖 C locale),
   那这条可以降级;我没有实测,只读了调用链。
