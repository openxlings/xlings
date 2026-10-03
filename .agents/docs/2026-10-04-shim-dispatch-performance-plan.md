# Shim 分发性能优化方案:让 `mcpp --version` 回到毫秒级

**日期**:2026-10-04
**类型**:性能优化总体方案(架构 / 稳定性 / 兼容性 / 跨平台 / 简洁性权衡)
**状态**:R0–R4 全部实施(单 PR)。任务拆分与依赖见 §12,实施记录见 §13。mcpp 仓库无需改动(内置 dist profile 已满足 R0)
**起因**:
- 用户实测:`mcpp --version`(经 shim)452 ms,直连 payload 的 mcpp 0.45 ms,差 **~1000 倍**
- 外部分析报告:`mcpp-shim-perf-report.md`(mcpp-community,2026-10-03),给出规模曲线与 `-O0` 证据
- 前置事实:#615(shim 身份与 handoff)、2026-09-03(routing vs state)、2026-9.29.1(install configures once per scope)

**共用的一条原则**(与本仓库既有设计一脉相承):
**派生数据永远不能成为权威;缓存未命中必须落回"完整但正确"的路径,而不是落回"猜"。**
shim 热路径的所有优化都按这个方向设计——错了就慢,不能错得不一样。

---

## 0. 一句话

shim 每次分发都要为回答一个 **O(1) 的问题**("mcpp 的 active 版本在哪")
付 **两遍 O(全量) 的 JSON DOM 解析**,而且整个发布二进制是 **`-O0 -g`** 构建的。
三件事叠起来,把一次 0.5 ms 的工具调用放大到 450 ms。

---

## 1. 实测与根因(两份来源交叉验证)

### 1.1 耗时分布(本机,`~/.xlings/.xlings.json` = 3.65 MB,versions 3970 条 / 2.4 MB)

| 调用方式 | 耗时(warm) | 说明 |
|---|---|---|
| 直连 payload 的 mcpp | **0.45 ms** | 12 MB 静态二进制,直接 exec |
| `mcpp --version`(shim) | **452 ms** | user 态 432 ms;syscall 合计仅 3 ms |
| `xlings --version`(CLI 路径) | 276 ms | 只解析一遍 home 配置 |
| `XLINGS_SHIM_ANCHOR=0 mcpp --version` | 285 ms | 跳过 owner 锚定,少解析一遍 |
| 同一二进制 + **空 home** | **1.5 ms** | 二进制本身启动的底噪 |

syscall 侧只有 3 ms,时间几乎全是用户态 CPU;strace 里 ~2150 次 32–64 KB 匿名
mmap 是 DOM 解析时分配器扩容的特征。

### 1.2 规模曲线(外报告 §2.2,-O0 下)

| versions 条数 | .xlings.json | shim 耗时 |
|---|---|---|
| 0 | 2 KB | 4.4 ms |
| 500 | 256 KB | 43 ms |
| 1000 | 510 KB | 88 ms |
| 2000 | 1 MB | 169 ms |
| 3200 + 200 knownProjects | 1.9 MB | 315 ms |

**≈ 每 1000 条记录 85 ms,线性增长。** mcpp 按日期发版、旧版本一直累积,
所以这是"装得越多越慢"的可持续恶化,不是一次性问题。

### 1.3 根因,按贡献排序

| # | 根因 | 证据 | 贡献 |
|---|---|---|---|
| 1 | **发布二进制 `-O0 -g`**:`DW_AT_producer` 实测含 `-O0`;`tools/linux_release.sh` 只跑 `mcpp build`(默认 dev profile);`mcpp.toml` 无 release profile | 外报告 §2.4;本机 readelf 确认;127 MB 中 ~100 MB 是 `.debug_*` | ~10× 乘数(vendored json.hpp 同文件 -O0 72 ms vs -O2 7.1 ms) |
| 2 | **home 配置被完整 DOM 解析两遍**:<br>① `resolve_dispatch_home` → `home_knows_program`(src/core/xvm/shim.cpp:115)只想查 `versions[mcpp]` 是否存在,却 `read + json::parse` 全文件;<br>② `Config::Config()`(src/core/config.cpp:618)把同一个文件再解析一遍 | 本机 strace:两次 3.65 MB readv;perf:38% + 56% | ~60% |
| 3 | **解析即全量反序列化 + 深拷贝**:`Config::versions()` 按值返回合并库(config.cpp:1092),shim.cpp:726 `auto db = Config::versions()` 每次深拷 3970 条;`load_global_versions_from_json_` 反序列化所有程序所有版本,而 shim 只需要其中一个子树 | perf:17%(反序列化)+ 1.5%(拷贝) | ~20% |
| 4 | **`versions` 数据模型冗余**:每个别名(7z/7zip/7zz)都内联完整 bindingGroup/bindingMembers/绝对路径,平均 588 B/条;全部塞在 home `.xlings.json` 里,被所有路径连带解析 | 本机对 3.65 MB 文件的字段分布统计 | 放大 #2/#3 的输入 |
| 5 | 固定开销:大型静态二进制启动、ftxui 等全局构造、profile 自愈读 3 个文件、cwd→root 项目探测 | 外报告 §1(299 缺页)、§3.4 | 小(1.5–2 ms),但决定终局 |

**被排除的嫌疑**:127 MB 体积、静态链接、debug_info 本身(空 home 下 1.5 ms 启动);
磁盘 I/O(全程 3 ms);exec 链(两次 execve 合计 ~1 ms)。

---

## 2. 方案总览

五个阶段,前两个无争议先做;R2/R3 是一组权衡(局部加速 vs 数据模型),给出建议后
按实测决定;R4/R5 是终局选项,**默认不做**,除非前面做完后数字仍不达标。

| 阶段 | 内容 | 改动面 | 风险 | 预期(本机 warm) |
|---|---|---|---|---|
| **R0** | 发布构建走优化 profile + strip + 制品检查 | 3 个 release 脚本 + mcpp.toml + release workflow | 低(需全量 e2e) | 452 → **~45 ms** |
| **R1** | 每文件每进程只解析一次;shim 不深拷版本库 | home_config/config/shim 内部 | 低 | → **~25 ms** |
| **R2** | shim 最小初始化(跳过 CLI-only 字段) | config + main | 中(要枚举 dispatch 依赖) | → **~8–12 ms** |
| **R3** | 数据模型瘦身:binding 去重 + `versions` 迁出 home 配置 | 存储格式 + 读写双方 + 兼容窗 | 中高(格式迁移) | → **~3–5 ms** |
| **R4** | 解析结果派生缓存(一个写入者,校验式) | 新增 subos 级缓存文件 | 中(失效面) | → **~1.5–2.5 ms** |
| **R5** | 独立极小 shim 启动器 | 新二进制 + 打包 + #615 机制 | 高(机制重复) | → **~1 ms** |

直连基线 0.45 ms。**R0–R2 后差距 ~20×;R0–R4 后 ~4×;再往下就不值得追了。**

---

## 3. R0:发布构建优化(最大乘数,最小代码)

### 3.1 改动

1. `mcpp.toml` 增加 release profile(mcpp 原生支持 `--profile release|dist`,
   `[profile.asan]` 是现成的写法参照):

   ```toml
   [profile.release]
   opt      = 3
   debug    = false
   cxxflags = ["-ffunction-sections"]
   ldflags  = ["-Wl,--gc-sections"]   # 平台对应:macOS 同名;MSVC 用 /OPT:REF,/Gy
   ```

2. `tools/linux_release.sh` / `macos_release.sh` / `windows_release.ps1`
   的 `mcpp build` 加 `--release`(或 `--profile dist`,按 mcpp 语义选)。
3. Linux/macOS 构建后 `strip`(或 `objcopy --only-keep-debug` 出 `.debug` 包
   挂到 release 资产,主二进制 strip——保留可调试性但不让用户下载 100 MB)。
4. **制品检查进 release workflow**(规则只在 merge 后生效就等于没有——参照
   `no_xlings_version_pin_check.sh` 的做法):
   - Linux/macOS:readelf/otool 断言无 `.debug_info`/`DW_AT_producer` 不含 `-O0`;
   - 断言二进制体积上限(如 < 60 MB,当前 127 MB);
   - Windows:断言 exe 未内嵌调试目录指向 -O0 pdb。

### 3.2 稳定性

- `-O0` 可能一直在掩盖 UB(#433 的 asan profile 已经证明过这类问题存在)。
  **上线前三平台全量 e2e + fresh-install 必须绿**,这是本阶段最大的成本,也是
  它真正的价值:让优化构建成为唯一被测试的构建形态,而不是"测试测 dev、用户拿 dist"。
- `-ffunction-sections + gc-sections` 在 C++23 modules + 静态 musl 下需验证;
  有任何平台不稳就先只做 opt/strip,gc-sections 单独跟进。

### 3.3 兼容性 / 跨平台

- 纯构建配置,无任何磁盘格式与协议变化,新老客户端零影响。
- 三平台脚本同步改,防止"Linux 优化了、Windows 还是 -O0"的漂移。
- 跨平台红利最大的恰恰是另两端:macOS Gatekeeper/XProtect 扫描 127 MB、
  Windows Defender 扫描 shim(硬链接/拷贝),体积降一个数量级后冷启动/扫描同步受益。

### 3.4 预期

JSON 热点 ~10×:本机 452 → **~45 ms**;外报告的基线场景 2.8 → **~1.2 ms**;
二进制 127 → **~20–30 MB**;冷启动 10–12 → ~4–6 ms。

---

## 4. R1:一个进程里,一个文件只解析一次

### 4.1 现状

`main()` 的顺序是:先 `resolve_dispatch_home`(`home_knows_program` 全量解析,
只为一个存在性判断;若 envHome 与 owner 不同且可解析,**还会再解析 envHome 一遍**),
后 `Config::Config()` 再解析同一文件。谁先谁后不重要——**同一个文件在同一次进程里
被解析两次**才是问题,调用顺序只是决定谁付第一遍。

### 4.2 改动

1. `src/core/home_config.cppm` 的 `read_home_config()` 增加进程内 memo:
   `(path, size, mtime_ns) → parsed json`。身份取自 **open 后的 fd fstat**
   (先读内容、fstat 同一 fd,天然排除"stat 后被替换"的竞态;进程生命周期
   ≤ 一次命令调用,不存在跨进程陈旧问题)。
2. `home_knows_program` 改走 `read_home_config`(自动享受 memo),存在性判断
   逻辑不变。
3. `Config::versions()` 按值返回保留给写路径;为读路径新增
   `find_vdata(program, version)`(先 project 后 global,避免深拷整个合并库),
   shim.cpp:726 改用它。`merged_versions` 只在真正需要合并视图的命令里发生。

### 4.3 稳定性 / 兼容性

- memo 是进程局部的,不落盘、不跨进程,和现状相比没有引入新的陈旧语义
  (现状里两次独立读同样可能读到改了一半的文件,fstat 身份反而更一致)。
- 无格式变化。`find_vdata` 是新增查询,`versions()` 语义不动,所有既有调用方零影响。

### 4.4 预期

解析次数 2→1:本机 ~45 → **~25 ms**;顺带消灭 3970 条的深拷贝。

---

## 5. R2:shim 最小初始化

### 5.1 现状

shim 路径为一次 exec 付出了 CLI 才需要的全部初始化:index repos、resource servers、
mirror 解析、ui prefs、knownProjects 记忆、`load_project_config_` 的 cwd→root 探测、
profile 自愈。shim 真正需要的只有:

- `Config::paths()`(home/active subos 定位);
- effective workspace 里 `workspace[program].active`;
- `versions[program]` 子树(含其 bindingGroup/bindingMembers 引用到的名字);
- 出错分支的 `workspace_installed`(tri-state 诊断,shim.cpp:741);
- routing 判定 `invoked_as_routing_entry_` / passthrough(只 stat,不解析)。

### 5.2 改动

1. `Config` 拆两档初始化:`full()`(CLI,现状)与 `dispatch_minimal()`(shim)。
   后者只解析 home 配置的 `activeSubos`/`subos`/`versions` 三类字段与 subos 配置。
2. `versions[program]` 按需提取:nlohmann SAX 过滤器,只对
   `versions.<program>` 子树物化 DOM,其余键跳过(O(file) 词法扫描 +
   O(条目) 构造);bindingMembers 引用的其他名字二遍提取。
3. profile 自愈(`auto_upgrade_profiles_if_stale`)维持对所有调用开放
   (shim 是最高频路径,是自愈最可靠的触发器),但把比较成本压到
   "只读每文件首行版本标记"(一次性 3 个小 open/read,µs 级),并可用
   `config/shell` 目录 mtime 做门闩。

### 5.3 稳定性

- 本阶段的全部风险集中在**枚举依赖**:漏掉一个 dispatch 真正读的字段,
  表现为某些包的 shim 行为变化。缓解:
  - `shim_dispatch` 依赖面以代码引用清单形式固化在测试注释里;
  - e2e 覆盖:普通程序、group 成员(7z/7zz)、binding、项目工作区、
    passthrough、未激活 tri-state 六类场景(现有 subos_xpkg_* 套件已是底子)。
- SAX 提取器是纯函数,对同一文件与全量解析结果做等价性单测(免费且决定性)。

### 5.4 简洁性代价(诚实的权衡)

"两档初始化"是永久性复杂度:今后每个新 Config 字段都要回答"shim 要不要"。
这正是 R3 想消掉的负担——见 §6 的权衡。若 R3 被采纳,SAX 提取器可以降级为
"只在超大 home 上的逃生通道",甚至不实现。

### 5.5 预期

~25 → **~8–12 ms**(剩余大头:一遍全量词法扫描 + versions 之外的 ~0.4 MB)。

---

## 6. R3:数据模型瘦身(与 R2 二选一为主,可叠加)

### 6.1 两个动作

1. **binding 去重**:`7z/7zip/7zz` 三个名字各存一份相同 bindingGroup/bindingMembers
   (本机实测平均 588 B/条,大头是重复)。组信息以 rootTarget 一份为权威,
   别名条目只留 `aliasOf` 指针。`versions` 预期缩 ~3×。
2. **`versions` 迁出 home `.xlings.json`**,落 `data/versions.json`
   (或 xvm DB 既有落点)。home 配置回到 ~0.4 MB 标量字段;shim 解析输入从
   3.65 MB → ~0.4 MB,普通解析即可,R2 的 SAX 复杂度失去必要性。

### 6.2 兼容性(本方案最需要小心的一节)

**老客户端把 `versions` 的缺失读成"什么都没装"。** 这是比 2026.9.29.1 的
`configured{}` 危险得多的迁移:那次是**新增**顶层键,老客户端忽略不认识的键;
这次是**移走**老客户端要读的键。

因此迁移必须是:

- **双写**:写入者同时维护 `data/versions.json` 与 home `.xlings.json` 的
  `versions`(老客户端继续正确);新客户端读新文件、**缺失/不可读时落回旧文件**
  ("没能读到"≠"是空的",与 `global_workspace_observed()` 同一纪律)。
- home 配置带 `versionsRef: "data/versions.json"` 声明指针 + `version` 递增,
  供 `self doctor` 报告双源分歧(以 xvm 指针翻转为权威,和 `xlings update` 同构)。
- 双写窗口显式声明截止版本(参照 #615 的 `COMPAT … drop in 2027.3` 惯例),
  到点删除旧字段,一次 release note 说清。

### 6.3 稳定性

- 格式迁移由写入路径(install/use/remove)在下次写时自然完成,fresh-install 与
  升级 e2e 各加一个"旧格式 home 升级后新老字段一致"的断言。
- `self doctor` 增加"versions 双源校验":两边条数/指纹不一致即报错并给出 remedy。
- GC 与 doctor 本就读 versions,迁移后它们改为读新文件,拒绝语义不变
  (不可读即拒绝,绝不当作空库)。

### 6.4 与 R2 的取舍

| | R2(SAX + 两档初始化) | R3(数据模型瘦身) |
|---|---|---|
| 永久复杂度 | 两档初始化 + SAX 提取器,每个新字段都要表态 | 一次迁移 + 双写窗口,窗口后**代码更简单** |
| 风险面 | 行为回归(漏字段) | 格式迁移(可双写兜底) |
| 收益上限 | ~8–12 ms | ~3–5 ms |
| 兼容成本 | 零 | 需要窗口管理 |

**建议:R0/R1 先行,R2 与 R3 之间选 R3。** 理由是简洁优雅:R2 是把复杂度
永久留在代码里换速度;R3 是把复杂度一次性付给迁移、换回一个长期更小的解析输入
和更干净的数据模型(版本库本来就是 `xvm/db.cppm` 的职责,塞在 home 配置里
才是历史意外)。若 R3 短期不做,R2 是不碰格式的次优解,两者不冲突。

### 6.5 预期

~25(R1 后)→ **~3–5 ms**(全量普通解析 0.4 MB,O2 下 ~0.4 ms 量级)。

---

## 7. R4:解析结果派生缓存(终局选项,默认不做)

### 7.1 设计

每个 subos 一个紧凑缓存(如 `subos/<s>/shim-cache/<program>` 或单行式文件),
内容由**唯一写入者**(install/use/remove,即 `sync_shim_tables` 所在路径)写,
条目 = `exe_path + envs + alias` 的最终解析结果。热路径变为:
stat 缓存 → 读一个小文件 → execvp。

### 7.2 为什么现在不做

- R0–R3 后全路径已到 ~3–5 ms,缓存再省 ~2 ms,但引入一整类新失效面。
- 本仓库为"派生表"付过的学费都写在 AGENTS.md 里:routing vs state、
  "输入为空 → 派生全删"、`knownProjects` 的 size/mtime 缓存是 #633 刚补的。
  一个**结果缓存**的正确性依赖"输入指纹必含全部影响量"(home 配置 + subos 配置 +
  project 配置 + payload 在不在),枚举错一个就是静默分发出错的环境变量/旧版本。
- 如果做,纪律必须是:**缓存只作加速器,校验失败一律落回完整解析**
  (错误方向 = 慢,永远不能 = 错);指纹用 hash 而非裸 mtime+size
  (防原位改写);`self doctor` 增加缓存 vs 全路径的分歧检查。

### 7.3 预期

~3 → **~1.5–2.5 ms**。直连 0.45 ms,差距 ~4×,到了"不值得再追"的区间。

---

## 8. R5:独立极小启动器(记录在案,倾向不做)

单独编一个只做查表 + exec 的 `xlings-shim`(不链 ftxui/libarchive/tinyhttps/lua),
shim 全部指向它,查不到再 exec 完整入口。固定开销可到 ~0.3 ms,并且不再受主
二进制体积影响。代价:第二个人工制品进三平台打包;#615 的 handoff/身份判定要在
两个二进制间保持一致;Windows 硬链接 shim 的重指向语义翻倍。**R0 之后主二进制
只剩 20–30 MB、启动 ~1 ms,这套复杂度大概率换不回值得的东西。** 仅当未来
GUI/更多 target 把主二进制重新养大时再评估。

---

## 9. 不做什么(明确排除)

- **换 JSON 库(simdjson 等)**:R0 后 nlohmann 在 -O2 下 ~10 MB/s 对
  0.4 MB 输入已足够;换库是全仓库读路径的横切变更,收益不再显著。
- **用户侧方案进文档即可**:清理旧版本缩 `versions`;延迟敏感场景
  (IDE/LSP)直接配 payload 绝对路径。这些是缓解,不是修复,不占用实施资源。
- **CI 上加耗时断言**:runner 噪声太大,会变成 flaky 门闩。性能防线放在
  **制品检查**(R0 的 -O0/体积断言)——它确定性、零噪声。

---

## 10. 实施顺序与验收

```
R0 ──→ R1 ──→ [R3 | R2 二选一,建议 R3] ──→ (可选) R4
      每个 R 独立成 PR(squash merge),单独可回滚
```

验收基线命令(每阶段跑同一组,记录进 PR):

```bash
hyperfine --warmup 3 'mcpp --version' \
  "$HOME/.xlings/data/xpkgs/xim-x-mcpp/2026.10.3.1/bin/mcpp --version"
wc -c ~/.xlings/.xlings.json          # R3 前后对比
readelf -S <released-bin> | grep -c debug_info   # R0 后应为 0
```

| 阶段 | 本机 warm(452 ms 起点) | 基线 home(2.8 ms 起点) | e2e 要求 |
|---|---|---|---|
| R0 | ~45 ms | ~1.2 ms | 三平台全量 + fresh-install |
| R0+R1 | ~25 ms | ~1.0 ms | Linux 全量 |
| +R3 | **~3–5 ms** | ~0.8 ms | 升级迁移 + doctor 双源 + 全量 |
| (+R2 替代) | ~8–12 ms | ~0.9 ms | shim 六类场景 + 全量 |
| (+R4) | ~1.5–2.5 ms | ~0.7 ms | 缓存失效矩阵 + doctor 分歧 |

**总目标:shim 调用与直连的差距从 ~1000× 收敛到个位数倍,且不再随
versions 规模线性增长。**

---

## 11. 风险清单

| 风险 | 阶段 | 缓解 |
|---|---|---|
| -O0 长期掩盖的 UB 在优化构建暴露 | R0 | 三平台全量 e2e + asan 套件前置跑一轮;发现即修,不回退 profile |
| gc-sections 与 modules/musl 组合异常 | R0 | 独立小步,可单独撤掉只留 opt/strip |
| shim 依赖字段枚举不全 | R2 | 依赖面清单 + 等价性单测 + 六类场景 e2e |
| 迁移窗口内老客户端读不到 versions | R3 | 双写 + 读回退;`self doctor` 双源校验;窗口截止版本显式声明 |
| 缓存指纹漏算输入 | R4 | 校验失败落回全路径(方向只能是慢);doctor 分歧检查 |
| 三平台脚本漂移(只改了 Linux) | R0 | 一并 PR + 制品断言进各自 release workflow |


---

## 12. 任务拆分与依赖关系(实施视图)

多角度映射:每个任务标注它服务的主要维度。依赖关系只有两条硬边:
**R3(versions 迁出)必须先于 R2 的最终形态**(SAX 定向提取只服务于迁移后的读路径),
**R1(memo)必须先于 R2**(否则 home_knows_program 与 Config 构造重复解析)。
R0 与 R4 与其他任务无耦合,可并行。

```
R0(构建/制品)──────────────────────────┐ 并行
R1(home_config memo + find_vinfo)──→ R2(廉价核心构造 + 定向读)
R3(versions 迁出 + 双写 + doctor)──↗
R4(shim_view 指纹缓存 + 读回写)────────┘ 依赖 R1-R3 提供的正确全路径
```

| 任务 | 内容 | 架构 | 稳定性 | 简洁优雅 | 用户体验 | 兼容性 | 跨平台 | 一致性 | 无感升级 |
|---|---|---|---|---|---|---|---|---|---|
| T1 = R0 | dist profile + 三平台制品检查 | 构建产物即契约 | e2e 全绿为门 | 删掉 profile 漂移的可能 | 冷启动/扫描提速 | 零格式变化 | 三脚本同步 + 各自 workflow | dev/dist 不再"测的和发的不一样" | 用户重装即得 |
| T2 = R1 | 进程内 capture memo;`Config::find_vinfo` 单条目合并 | 一个文件一次进程只解析一次;一个目标的答案不再深拷全库 | fstat 身份防"stat 后被换" | 读路径不再为写路径的形状付钱 | — | 纯内部 | — | find_vinfo 与 merged_versions 同一覆盖语义(等价性单测) | — |
| T3 = R3 | `data/versions.json` 双写 + 读回退 + dbIndex + doctor 双源校验 | versions 归 xvm,home 配置回归标量 | 双写同 dump 不可分叉;读回退"未读到≠空" | 去重**不做**(实测仅 ~2x 且迁移后无运行时收益,格式机制不值得) | home 配置不再被无关路径连带解析 | **双写窗口**:老客户端继续读 `versions` 字段 | 顺序语义与平台无关 | versions 的"住址"只有一个回答者(load_versions_json) | 老 home 下次写入自然迁移;doctor --fix 兜底 |
| T4 = R2 | 构造函数廉价核心(AbortAtVersions 捕获)+ 惰性三组(workspace/versions/index 配置) | shim 与 CLI 分摊解析成本,单一 Config 不设两档回答者 | 早退双重门闩(有序性 + activeSubos 已见);损坏=未观察到 | 惰性而非两档:新字段默认 CLI-only,不加思维负担 | CLI 顺带受益 | 捕获缺键=键真的不存在(有序流保证) | SAX 是 nlohmann 自带接口,三平台一致 | activeSubos/subos 的读者仍是 resolve_subos_scope_ 一个 | — |
| T5 = R4 | per-(subos, ctx) 指纹校验视图缓存,读路径回写 | 派生数据、只加速、永不权威 | 指纹全等才命中;miss 只准变慢;陈旧写入天然失效 | 无写入者钩子:回写即自愈,少一个遗漏面 | 稳态 shim ≈ 直接运行量级 | 缓存文件是新增派生物,删除无碍 | mtime 为文件时钟本地比较,无跨平台语义 | 与 routing table 同一"派生"纪律,失败方向相同 | 旧 home 首次分发即建缓存 |

**跨仓库协作结论**:mcpp 仓库无需改动(内置 `dist` = -O3 + 链接期 strip,`--profile dist` 直用;
调查证据见 mcpp `src/build/prepare/toolchain.cpp:236-242`);xim-pkgindex 无需改动(本 PR 不改
recipe 语义,recipe revision 不动;release 时的 sha256 补录照旧走 mirror-latest.sh,不在 PR 范围)。


---

## 13. 实施记录(2026-10-04)

全部五阶段在同一 PR 落地。代码落点:

| 阶段 | 文件 | 内容 |
|---|---|---|
| R0 | `tools/linux_release.sh` `macos_release.sh` `windows_release.ps1` `.github/workflows/release.yml` `mcpp.toml` | `--profile dist`(-O3+链接期 strip);制品门槛:stripped/无 debug_info(ELF)、无 LC_SYMTAB(Mach-O)、无 .pdb(PE)、体积 ≤80MB;版本号 2026.10.4.1 |
| R1+R2 | `src/core/home_config.*` `src/core/config.*` | `SelectiveCaptureSax`(AbortAtVersions / SkipVersions 两档)+ 进程内 memo(fstat 身份);Config 构造只做廉价捕获,workspace/versions/index 配置三组惰性化;`Config::find_vinfo`(单目标覆盖合并,与 merged_versions 同语义) |
| R3 | `config.cpp`(`save_versions` 双写)`xvm/db.*`(`program_index_to_json`)`xself/doctor.*` | `data/versions.json` 先写、home config 后写(崩溃方向安全);dbIndex 随 dump 写入;`load_versions_json` 唯一回答者(文件→配置字段,损坏≠空);doctor 新增 `VersionsDbDivergence`(检查 + --fix 删除派生文件) |
| R4 | `src/core/xvm/shim_view.*`(新模块)`xvm/shim.cpp` | per-(subos, 项目 ctx) 指纹校验视图缓存 `<subos>/.shim-view/<ctx><program>.json`;**读路径回写**(成功解析后原样 stamp 存储),无写入者钩子;per-program 裁剪(整张 workspace 表 ~350KB → 单条目) |
| 读路径 | `xvm/shim.cpp` | `home_knows_program` 走 dbIndex 快路径(miss 落回真实 DB);dispatch 改 view-or-slice,`Config::versions()` 深拷贝从热路径消失;`profile.cpp` 迁移到 `load_versions_json` |

### 实测(本机:3.65MB home 配置 / 3970 版本条目 / 388KB subos 配置)

| 场景 | 改动前 | 改动后 | 倍数 |
|---|---|---|---|
| shim `mcpp --version`(旧版 home 形态) | 451 ms | **41 ms**(legacy 路径:双解析尚在;首次 install/use 迁移后即为下两行) | ~11× |
| shim `mcpp --version`(迁移后 home,缓存未命中) | 451 ms | 45 ms | ~10× |
| **shim `mcpp --version`(迁移后 home,缓存命中,稳态)** | **451 ms** | **5–16 ms**(同一二进制多次测量区间;其中 ~3.3 ms 是本地动态链接二进制的启动地板,release 静态 musl 口径预计更低) | **~30–90×** |
| 直连 payload(参照) | 0.44 ms | 0.44 ms | — |
| 发布二进制体积 | 127 MB(-O0 -g) | 13.2 MB(O3 + stripped;release 脚本 musl 静态口径预计同量级) | ~10× |

隔离测量环境:`/tmp/shimperf`,真实 home 配置复制 + 正确写入者键序(`dbIndex` 位于字母序位置——
**用外部脚本迁移时必须 `sort_keys`,否则 dbIndex 落在 `versions` 之后,快路径失效**;真实写入者经
nlohmann map dump 天然有序)。

### 单测与 e2e

- 新增 `tests/unit/test_shim_view.cpp`(19 用例):捕获两档语义、有序性降级、损坏=未观察、
  memo 失效、db 文件回退三态、dbIndex 编码、home_knows 索引/词干双命中、
  view 往返/指纹失配矩阵/路径穿越拒绝/垃圾文件 miss。
- 全套 `mcpp test`:除 2 个 **改动前即失败** 的既有用例外全部通过
  (`test_progress_output` 终端颜色环境、`test_interface_protocol` 网络等待超时;
  均已在干净树基线复现)。
- e2e:subos_xpkg_install/use、project_shim_mirror、project_scope_preserves_global_shims、
  bootstrap_home、entry_binary、broken_home_isolation、diagnostics、doctor×2、project×2 等
  (结果见 PR 描述);另加一条手工双写脚本验证:install 产出双源、remove 后一致、
  篡改配置后 doctor 捕获、`--fix` 删除派生文件。

### 与方案的偏差(诚实记录)

1. **R3 去重未做**:实测 mcpp 条目(168 版本)32KB 中可去重字段(bindingGroup/kind/
   sourceName/destinationName 重复)只支撑 ~2× 缩减,且迁移后 shim 根本不读全量 DB,
   运行时收益≈0;引入 defaults/模板机制换 ~2× 文件体积不划算。窗口结束(停止双写)后
   home 配置自然缩 ~90%,再评估。
2. **R2 的"两档初始化"改为"惰性三组"**:两档初始化会把"shim 要不要这个字段"变成永久
   的双答案者;惰性化让 CLI 与 shim 共用同一套读者,只是按需付解析成本。
3. **R4 从"写入者重建"改为"读路径回写"**:sync_shim_tables 只同步作用域 + 全局 subos,
   逐 subos 重建会遗漏多 subos/多项目场景;回写式缓存 miss 后自愈,且失败方向仍是
   "变慢",不是"变错"。指纹 = 调用方新取的 stat 全集,内容与 stamp 同一时刻,陈旧写入
   天然失效。
4. **legacy home(未迁移)仍有 ~41ms**:home_knows 与 ensure_global_versions_ 各做一次
   全量解析(memo 未覆盖 load_versions_json)。属过渡态——任意 install/use 写入即迁移;
   如需可后续给 load_versions_json 加 shared_ptr memo。
5. **e2e 抓到并修掉的指纹缺口(真实回归,记下它的形状)**:第一版指纹集漏了
   **项目运行态文件**(`<proj>/.xlings/.xlings.json`)——Anonymous 项目的 `use` 把
   workspace 写在那里,于是"切到 20.19.0 → 切回 22.17.1"的第二次分发命中了陈旧视图,
   `project_e2e` / `project_data_routing` / `diagnostics_contract`(S2d 诊断一致性)
   三个 e2e 当场抓住。修法:指纹集改由 Config 自己的唯一回答者给出——
   `workspace_config_path()`(本 scope 读写的那个 workspace 文件)+ 全局 subos 文件 +
   `project_state_path()`(项目 versions 同住此文件)+ manifest + home 配置 + versions DB。
   教训与 routing-table 时代一致:**派生缓存的输入清单,必须由那"一个回答者"枚举,
   不能由缓存的使用者手抄一遍路径。**

### 兼容性边界(已钉死的形状)

- **老客户端 + 新 home**:home config 保留完整 `versions`(双写),`dbIndex` 是新增键,
  老客户端忽略;`subos`/`activeSubos` 语义未动。老客户端读 home 配置的路径全部照旧。
- **新客户端 + 老 home**:`data/versions.json` 缺失 → 读配置字段;文件损坏 → 回退配置;
  两者皆不可读 → 未观察(nullopt),拒绝类消费者保持拒绝。
- **手改配置**:dbIndex 命中即可信(与 versions 同一 dump);miss 落回真实 DB;双源分歧
  被 doctor 报告并可 `--fix`。早退捕获双重门闩(键序 + activeSubos 已见)挡住
  versions-first 的病态文件。
- **窗口截止**:双写窗口到 `drop in 2027.4`(与 #615 的 2027.3 惯例同构);到点删除
  home 配置的 `versions` 字段,届时 home 配置缩 ~90%,CLI 的 SkipVersions 二次捕获
  也可以随之删除。
