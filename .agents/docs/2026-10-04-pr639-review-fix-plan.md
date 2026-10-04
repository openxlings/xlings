# PR #639 第二轮 review 修复方案

- 对象:PR #639 `perf/shim-dispatch`(head `0157beb`),版本 2026.10.4.1(尚未发布)
- 来源:第二轮 review(PR #639 评论)
- 状态:**已实施**(处置与验证记录见 `2026-10-04-shim-dispatch-performance-plan.md` §15)
- 落地方式:在 `perf/shim-dispatch` 上追加提交，每个修复一个 commit，不 bump 版本;
  合入前把本文的处置结果追加到 `2026-10-04-shim-dispatch-performance-plan.md` §15

---

## 0. 一句话

双写窗口里，"DB 文件可信"的依据必须是**内容相等**，不能只看**谁最后写入**;
局部捕获能提前停下，也必须有**证明**支撑，不能只靠**推测**。
两个问题都出在同一种结构上：用一个弱信号去代替一个本来可以直接验证的事实。

## 1. 问题清单与优先级

| # | 级别 | 问题 | 证据 | 修复规模 |
|---|---|---|---|---|
| F1 | P0 | RMW 无条件重戳，过期 DB 重新变得可信，接着 `save_versions` 把旧客户端的记录从两份副本里都抹掉 | 本地复现 | ~60 行(含去重) |
| F2 | P1 | `AbortAtVersions` 会丢掉排在 `versions` 之后的顶层键(`mirror`/`lang`/`knownProjects`…) | 本地复现，与 2026.9.30.1 对比 | ~80 行 |
| F3 | P1 | 惰性 `ensure_index_config_` 让全局 `xim.index-base` 覆盖项目值 | 读代码确认 | ~20 行 |
| F4 | P2 | `save_workspace()` 全局分支不会自己触发懒加载 | 潜在问题 | 1 行 |
| F5 | P2 | Windows release 按 `FullName` 选 exe，dev 和 dist 产物并存 | 产物体积反常 | ~5 行 |
| F6 | P3 | 双写 e2e 的模拟不真实，S4 是空断言;缺 F1/F2 的回归测试 | 读测试 | 测试 |
| F7 | P3 | 过时注释、每次 shim 都打的 warn、`dbIndex` 空条目 | 读代码 | 零散 |

顺序:F1 → F2 → F3 → F4 → F6(测试随各修复一起提交)→ F5 → F7。

---

## 2. F1 [P0]:重戳以内容相等为前提

### 2.1 根因

`rmw_home_config_locked_`(config.cpp)和 `update_home_config`(home_config.cpp)在写完 config 后，
只要 `stamp != 新 stat` 就执行重戳。它们默认"本次 RMW 没改 versions,所以 DB 仍是 config 的忠实副本"。
这个前提只在 DB **写入之前就新鲜**时成立。双写窗口防的正是"旧客户端改了 config 后 DB 已过期"这种情况,
此时重戳会把过期副本重新扶正。

复现链:新客户端 install → 旧客户端只改 `config.versions`(加入 oldpkg)→ 新客户端执行 `config --mirror`
(过期 DB 被重戳)→ 新客户端 `remove` 触发 `save_versions`(从过期 DB 加载后整份写回)→ oldpkg 在两份副本中都消失,
doctor 也报不出分歧。

### 2.2 设计

stamp 的语义收紧为一句话:**"config 的 stat 为 X 时,DB.versions == config.versions"。**
只有能证明这句话成立的写入者才可以打 stamp:

- `save_versions`:同一次 dump 写出两份，天然成立(保持现状)。
- RMW 两个出口:手上就有刚写出的完整 `json`,直接比较 `wrapper["versions"] == json["versions"]`
  (nlohmann 深比较)。相等就重戳，不等或任一方缺失就**不动**,stamp 保持过期，读者继续回退到 config。

为什么不用"写入前 stat == 旧 stamp"这种 stat 判据：持锁区间之外的写入者(手改配置、更老的无锁客户端)
可能在"取 stat"和"读内容"之间插入一次写入，这时 stat 判据会误判。内容判据比较的正是刚写出去的字节，
不存在这个窗口。成本是对 DB 做一次深比较。重戳路径本来就已经解析了 DB(现有代码),RMW 也已经解析了整份 config,
新增开销可以忽略。

### 2.3 改动

1. `home_config.cppm` 导出两个函数，替换现在 4 处复制粘贴的格式化和重戳代码:
   ```cpp
   // "<size>:<file_clock ticks>",save_versions / load / restamp 共用这一个回答者。
   std::string versions_db_stamp(std::uintmax_t size, std::filesystem::file_time_type mtime);

   // 刚写完 config(json == 写出的内容,调用方持有状态锁)。
   // 只有 DB 包装体的 versions 与 written["versions"] 内容相等时才重戳;
   // 其余情况(不等、缺失、DB 损坏)一律不动。尽力而为，绝不抛异常。
   void restamp_versions_db_if_equal(const std::filesystem::path& home,
                                     const nlohmann::json& written);
   ```
2. `rmw_home_config_locked_` 和 `update_home_config` 尾部各自换成一行调用。
3. `save_versions` 和 `load_versions_json` 改用 `versions_db_stamp`。

### 2.4 测试

- e2e `E2E-130 dual_write_restamp_test.sh`(由 review 复现脚本改写为断言):
  新客户端 install → 旧客户端只改 config → 新客户端执行 `config --mirror`
  → 断言 DB 的 stamp **仍然过期**→ 新客户端 `remove` 另一个包 → 断言 `config.versions` 中 **oldpkg 仍在**,
  并且 DB 重新生成后也包含 oldpkg。
- 单测:`RestampSkipsWhenVersionsDiffer`、`RestampAppliesWhenVersionsEqual`、`RestampSkipsWhenDbCorrupt`。

---

## 3. F2 [P1]:提前停止解析必须有证明

### 3.1 根因

`SelectiveCaptureSax` 的 abort 条件是"已读部分有序，并且见过 `activeSubos`"。
已读部分有序，推不出 `versions` 之后没有字典序更小的键。手改配置时追加在末尾、
Python `json.dump` 不带 `sort_keys`,都会产生这种文件。
复现:`{"activeSubos", "versions", "mirror":"CN", "lang":"zh"}` 这样的文件，在 PR 构建下
`interface env` 读出的是 `mirror="" lang=""`,而 2026.9.30.1 读出的是 `CN/zh`。

### 3.2 设计

只有当 config 的字节**可证明**由 xlings 的有序 dump 产生时，才允许 abort。
证据现成就有：只有 xlings 自己的写入者会给 DB 打 stamp(F1 之后语义已经收紧),
而 xlings 的每次写入都是 nlohmann `std::map` dump,键一定有序。所以条件是:

```
abort 允许 ⇔ sortedSoFar && sawActiveSubos            (现有，保留作为纵深防御)
             && config 当前 stat == DB 包装体的 stamp   (新增：证明)
```

stamp 不匹配(手改、旧客户端写入、没有 DB 文件)时，退化为 SkipVersions 语义:
`versions` 只做 lex 不建 DOM,其后的键照常捕获。慢一些，但不会漏键。

读 stamp 的成本：包装体由 `dump(2)` 写出，键顺序是 `format` → `stamp` → `versions`。
只读 DB 文件开头的 4 KiB,用同一个 SAX 在遇到 `versions` 键时 abort(截断的输入在 abort 之前不会被读到),
取出 `format` 和 `stamp`。代价是一次 stat 加一次 4 KiB 读取，不会去解析 2.4 MB 的 DB。

### 3.3 改动

1. `home_config.cpp`:新增 `read_versions_db_stamp_prefix(home) -> std::optional<std::string>`(4 KiB 前缀 + SAX abort)。
2. `home_config_capture(configPath, AbortAtVersions)`:先比较 config 的 stat 与 DB stamp,
   把结果作为 `SelectiveCaptureSax` 的 `abortAtVersions` 实参传入。memo 条目增加 DB 的 stat,用于校验
   (DB 被重写、stamp 变化时 memo 失效)。
3. `HomeConfigCapture::truncated` 的语义不变，只有真正 abort 时才为 true。

### 3.4 测试

- 单测 `AppendedKeyAfterVersionsIsCaptured`:无 DB 文件，键追加在 `versions` 之后 → 能捕获 `mirror`/`lang`,`truncated == false`。
- 单测 `AbortOnlyWithMatchingStamp`:stamp 匹配时 `truncated == true`;改一次 config(stat 变化)后 `truncated == false`,并且尾部键可见。
- 单测 `StampPrefixReaderIgnoresTruncatedTail`:4 KiB 截断不影响 stamp 读取。
- 性能复测(同一个 3.65 MB home):迁移后的 home 在 F1+F2 之后，稳态应仍在 5–16 ms
  (每次 install/use/RMW 后 stamp 都会被维持),验收时把数字记录到实施记录里。

### 3.5 不采纳

- "用 `dbIndex` 是否存在作为证据":新客户端写出的文件之后仍可能被手改并追加键,`dbIndex` 证明不了尾部没有新键。
- "一律 SkipVersions":每次 shim 都要 lex 整个 3.6 MB,PR 的主要收益会被吃掉一半以上。
  等 2027.4 窗口结束、`versions` 从 config 中移除后，这个问题自然消失，届时连 abort 机制一起删除。

---

## 4. F3 [P1]:`index-base` 优先级回到"项目优先于全局"

### 4.1 根因

main 的顺序是：构造函数先读全局 `xim.index-base`,再由 `load_project_config_` 写入项目值覆盖。
PR 把全局读取挪到了懒加载的 `ensure_index_config_`,它在项目加载**之后**才运行，并且直接写同一个成员
`indexBases_`,于是全局值反过来覆盖了项目值。`reload_state_` 会清掉 `indexConfigLoaded_`,同样的覆盖会再发生一次。

### 4.2 改动

用两个成员替换 `indexBases_`,和 `projectIndexRepos_`/`globalIndexRepos_` 的写法保持一致:

```cpp
std::vector<ArtifactBase> globalIndexBases_;   // ensure_index_config_ 写
std::vector<ArtifactBase> projectIndexBases_;  // load_project_config_from_dir_ 写

std::vector<ArtifactBase> Config::index_bases() {
    auto& self = instance_();
    if (self.hasProjectConfig_ && !self.projectIndexBases_.empty())
        return self.projectIndexBases_;
    self.ensure_index_config_();
    return self.globalIndexBases_;
}
```

额外收益：项目值命中时，不再为 `xim` 触发一次 SkipVersions 全量 lex。

### 4.3 测试

单测(或 e2e 片段):home config 和项目 manifest 都设置 `xim.index-base` → `Config::index_bases()` 返回项目值;
`reload_state()` 之后仍然返回项目值。

---

## 5. F4 [P2]:`save_workspace` 自己负责懒加载

`Config::save_workspace()` 的 `else`(全局)分支开头加一行 `self.ensure_global_workspace_();`,
与 `save_versions` 里的 `ensure_global_versions_()` 对称。

现有 19 个调用点都会先经过某个会触发 ensure 的访问器，所以这只是防御性修改，行为不变。
它消除的是"未加载 = 空 → 整个 subos 的 workspace 被清空"这条潜在路径。

---

## 6. F5 [P2]:Windows 打包选中正确的 exe

`tools/windows_release.ps1`:

```powershell
$BIN_FILE = Get-ChildItem "$PROJECT_DIR\target" -Recurse -Filter "xlings.exe" |
  Where-Object { $_.FullName -match "[\\/]+bin[\\/]+xlings\.exe$" } |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1
Info "Packaging $($BIN_FILE.FullName) ($($BIN_FILE.Length) bytes)"
```

这与 macOS 脚本已经记录过的"按最新 mtime,不按哈希目录名"一致。
验收:Windows CI 日志里选中的路径属于 dist 的 fingerprint 目录;同时对比 dev 与 dist 两个 exe 的大小，记录到实施记录里。

---

## 7. F6 [P3]:让双写 e2e 测到真实形态

- `dual_write_window_test.sh` S2/S4:**不再修改 `dbIndex`**。真实的 ≤2026.9.30.1 客户端会原样保留旧的 `dbIndex`。
  S3 继续断言分发能看到 oldpkg。
- S4 改为**保留 shim 文件**,断言分发结果与 config 一致(py-demo 已移除 → 报 not installed,或回落到 PATH),
  而不是让 bash 报 ENOENT。
- 新增 F1 对应的 E2E-130(见 §2.4),登记到 `run_all.sh`。

## 8. F7 [P3]:零散修正

- `doctor.cppm` / `doctor.cpp` 中 `VersionsDbDivergence` 的注释改为 stamp 之后的真实语义：手改 config 会使 stamp 失配，读者会回退到 config;
  分歧只说明两份副本内容不同，不再意味着"手改不可见"。
- `load_versions_json` 在 DB 损坏时的 `log::warn` 降为 `log::debug`。shim 进程内没有跨进程 memo,
  DB 损坏期间每次调用被包装的工具都会往 stderr 打一行;这类报告交给 doctor。
- `program_index_to_json` 跳过 `versions` 为空的条目，让快路径与慢路径的 `has_versions` 语义一致，零成本。
- 实施记录 §14 中"RMW 不改 versions,所以重戳不会掩盖真实分歧"一句改为 F1 的结论，并标注由第二轮 review 修正。

---

## 9. 验收

- [ ] `mcpp test` 全绿(新增单测：F1 3 例、F2 3 例、F3 1 例)
- [ ] e2e(`XLINGS_TEST_MIRROR=CN`):E2E-129(改写后)、E2E-130(新增)、project 系列、doctor 系列通过
- [ ] review 复现脚本的结果反转:[3] 中 stamp 保持过期,[4] 中 `config has oldpkg? true`
- [ ] 键追加形态：PR 构建的 `interface env` 读出 `CN/zh`,与 2026.9.30.1 一致
- [ ] 性能：迁移后 home 的稳态 shim 仍为个位到十几毫秒，数字写入实施记录
- [ ] 全平台 CI 通过;Windows 日志确认打包的是 dist 产物

## 10. 风险

| 风险 | 缓解 |
|---|---|
| F1 深比较 2.4 MB JSON 的开销 | 只发生在 RMW(低频，本来就解析整份 config);实测记录 |
| F2 新增的前缀读取让 shim 热路径多一次 stat 和 4 KiB 读取 | 同在页缓存中，量级是微秒;性能验收覆盖 |
| F2 后旧客户端写过的 home 一直走 lex 慢路径 | 与 PR 已接受的"legacy home ~41 ms"一致，任意一次新客户端 install/use 就会恢复 |
| 手改配置恰好与 stamp 的 size 和 mtime 都相同 | 与 stamp 设计本身的残余窗口相同(等长改写且落在同一个时间戳 tick 内),不新增风险 |
