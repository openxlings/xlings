# 索引快照的版本契约与自动路由 —— issue #476 设计方案

**日期**: 2026-08-04（重写；初版同月 08-03）
**类型**: 设计 (design)
**基线**: `main` @ `1e968b6` = `2026.8.3.1`
**相关 issue**: [#476](https://github.com/openxlings/xlings/issues/476)
**前序**: `.agents/docs/2026-06-22-index-as-resource-impl-plan.md`（Y-asset 分发）、
`2026-07-22-issue377-custom-index-artifact-design.md`（自定义 artifact 源）、
`2026-08-03-multiplatform-contracts-architecture-plan.md`（A1 沉默成功主线）
**涉及仓库**: `openxlings/xlings`、`openxlings/xim-pkgindex`、`xlings-res/xim-index`

> **为什么重写**：初版把这件事设计成"为 mcpp 做不透明透传，xlings 永不解释"。
> 那是把一个**基础设施能力**降级成了给某个消费者开的旁路。
> xlings 自己的索引同样会抬高客户端门槛（新的 xvm node kind、新的 `spec` 版本），
> 同样会让老客户端硬失败 —— 这个问题 xlings 自己就有，而且只有 xlings 能解。
> 本版按"xlings 优先"重新设计：**索引声明它需要的客户端版本，客户端自动路由到
> 自己能用的最新快照**；不透明透传降级为该机制的一个自然副产品。

---

## 0. 摘要

### 0.1 要解决的真问题（xlings 自己的，不是 mcpp 的）

索引一旦开始使用新客户端才有的能力，老客户端就**硬失败**。这在本仓库有记录在案的先例：

```
error: unsupported registration node kind 'files'
```

今天的缓解手段是**每个配方自己做能力探测**（`reference_recipe_capability_probe`）——
聪明，但把负担摊到了每一个配方作者身上，且只能覆盖"配方作者想到了"的情况。
索引级的版本契约是这件事的**基础设施解**。

### 0.2 目标形态

```
每个 xlings 客户端：
   拉一次指针（今天已经在做）
   → 得到 [最新快照] + [历史快照]，每条都声明 requires.xlings
   → 过滤出"我这个版本满足"的
   → 取其中最新的一条，落地
   → 若落地的不是最新快照，明确告诉用户：为什么、以及怎么升级
```

版本错配从**硬失败**变成**路由决策**。老客户端继续用它能描述的包，用户升级后自动前进，
两侧不需要协调发布节奏。

### 0.3 五个核心决定

| # | 决定 | 关键理由 |
|---|---|---|
| 1 | 版本比较用 `version_order::compare`，**不是** `semver::satisfies_expr` | **实测**：`semver` 解析不了 4 段版本，`satisfies_expr("2026.8.3.2", ">=2026.8.3.1")` 返回 **0**。建在它上面 = 每个客户端都路由到空集 |
| 2 | 契约是 `{min, max}` 两个裸版本号，**不引入范围表达式语法** | 两个边界就是范围；引入第二套语法会与包解析的 semver 语义漂移，且把 §1 的 4 段 bug 一起带进来 |
| 3 | `requires` 是 **consumer → 约束** 的映射，xlings 只解释 `xlings` 键 | 一套机制同时服务 xlings 自己和第三方；不透明透传变成自然结果而非特设旁路 |
| 4 | 指针**纯增字段**，不重构成 `latest` + `history` 兄弟节点 | issue 的形状会让**今天所有已发布客户端**判定指针无效（§3.1 有代码证据） |
| 5 | 路由**永不适用于客户端自身的升级路径** | 否则老客户端被路由到老索引 → 看不到新 xlings → 永远升不上去。**死锁**（§5） |

---

## 1. 决定性的实测：现有 semver 不能表达 xlings 自己的版本

写这份方案时先测了，因为整个契约的可行性压在这上面：

```
semver::parse("2026.8.3.1")                     → FAIL          (ok=0)
semver::satisfies_expr("2026.8.3.1", ">=2026.8.3.2") → 0        [期望 0 ✓ 但属巧合]
semver::satisfies_expr("2026.8.3.2", ">=2026.8.3.1") → 0        [期望 1 ✗]
version_order::compare("2026.8.3.1", "2026.8.3.2") < 0 → 1      [✓]
version_order::compare("2026.8.3.1", "0.4.69")     > 0 → 1      [✓]
```

`semver::Version` 是 **3 段**（`major/minor/patch`，`semver.cppm:10`），
而 xlings 自己的版本是 **4 段** `YYYY.M.D.N`。

**最危险的地方不是它错，是它错的方式**：`satisfies_expr` 解析失败时返回 `false` 而不是报错。
如果契约建在它上面，每个客户端对每条快照都判"不满足" → 路由到空集 → 报
"没有兼容快照"。看起来像**发布方没发历史**，实际是解析器不认版本号。
这正是 `2026.8.3.1` 那一批修复的 A1 形状：**声明了契约，而没有任何东西验证它真的成立**。

`version_order::compare`（`src/core/version_order.cppm`，本仓库 `2026.8.3.1` 引入）
按分量做数值比较，段数任意，正是为这类版本号写的。

**不扩展 semver 到 N 段**：它服务的是**包**版本约束（`gcc@15`、`^1.2`），
改它的解析会波及所有包解析路径。索引契约是另一个问题域，用另一个（已存在且正确的）比较器。

---

## 2. 契约 schema

### 2.1 索引树声明自己需要什么客户端

索引仓库根目录新增 `index-compat.json`（可选；缺失 = 无约束 = 今天的行为）：

```json
{
  "format_version": 1,
  "requires": {
    "xlings": { "min": "2026.8.3.1" }
  }
}
```

- `min` **含**，`max` **不含**（`[min, max)`）。两个都可选。
- `requires` 是 **consumer 名 → 约束** 的映射。xlings 只解释键 `"xlings"`，
  其余原样搬运并通过 `xlings index list --json` 暴露给对应消费者（§7）。
- 语义故意贫乏：没有 `^`、没有 `~`、没有 `||`。**能被误读的语法就是会被误读的语法**，
  而这条契约的失败模式是"整个索引对某类客户端不可用"。

### 2.2 指针 schema：只加字段，不重构

```json
{
  "format_version": 2,
  "indexes": {
    "xim": {
      "format_version": 1,
      "index_version": "e2aad0b",
      "index_name": "xim",
      "generated_at": "2026-08-03T15:01:07Z",
      "source_commit": "e2aad0b…",
      "artifact": { "name": "xim-index-e2aad0b.tar.gz", "sha256": "…", "size": 371803 },

      "requires": { "xlings": { "min": "2026.8.3.1" } },

      "history": [
        { "index_version": "e2aad0b", "generated_at": "2026-08-03T15:01:07Z",
          "requires": { "xlings": { "min": "2026.8.3.1" } },
          "artifact": { "name": "xim-index-e2aad0b.tar.gz", "sha256": "…", "size": 371803 } },
        { "index_version": "20e53c6", "generated_at": "2026-08-03T09:12:44Z",
          "requires": {},
          "artifact": { "name": "xim-index-20e53c6.tar.gz", "sha256": "…", "size": 371707 } }
      ],
      "history_truncated": false
    }
  },
  "client_latest": { "xlings": "2026.8.3.1" }
}
```

#### 为什么不能用 issue 提议的形状

issue 提议把 `indexes.<name>` 改成 `{latest: {...}, history: [...]}`。
但现有解析是硬校验（`indexfetch.cppm:250`）：

```cpp
if (!j.is_object() || !j.contains("artifact") || !j["artifact"].is_object())
    return std::nullopt;
```

新形状下 `indexes.<name>` 不再直接含 `artifact` → **今天每一个已发布的 xlings 都会把
指针判为无效**。一个为"别让老客户端掉队"提的特性，落地方式会让所有老客户端立刻掉队。

纯增字段则：老客户端读到的还是原来那个 manifest，新字段被 `value()` 默认值吞掉。

#### 其余形状说明

- **`history[0]` 就是当前快照**，与顶层重复约 250 字节。故意的：客户端只过滤**一个**列表，
  不必"先看 latest 再看 history"，少一条分支就少一处会写错的地方。
- **`client_latest`** 是 §5 死锁解法的一半：无论路由到哪个快照，客户端都能知道
  "有更新的我"。
- **`history_truncated`** 让客户端能区分"没有兼容快照"和"兼容的被截断了"——
  后者该提示用户而不是断言不存在。

### 2.3 `history` 收录策略（发布侧）

不是"留最近 N 个"。真实数据：`xlings-res/xim-index` 的滚动 `latest` release 里有
**313 个 artifact**（从不修剪），但**契约变动极少**。所以：

```
history = 倒序，取以下并集，上限 32：
    A) 最近 8 个快照（不论 requires）        → 支持"回滚一个版本"这种调试需求
    B) 每个不同 requires 值的最新那个快照     → 支持"找到我这个版本还能用的最新快照"
超出 32 丢最老的，并置 history_truncated = true
```

B 是这个特性存在的理由，A 是日常可用性。并集在真实历史上约 10 余条，序列化约 3 KB。
指针从 ~400 B 涨到 ~3.5 KB，仍是一次 raw fetch，仍在 gitcode raw 的舒适区。

---

## 3. 客户端：自动路由

### 3.1 选择算法

```
snapshots = manifest.history 若非空，否则 [manifest 自身]
candidates = [s for s in snapshots if satisfies(Info::VERSION, s.requires["xlings"])]

candidates 非空          → 取 index_version 最新的那条（history 已倒序 → 取首个）
candidates 为空          → E_INDEX_NO_COMPATIBLE_SNAPSHOT，不动本地索引树
                           消息里给出：我的版本、各快照要求的最低版本、升级命令
```

```cpp
// 契约求值：两个边界，一个比较器，没有语法
bool satisfies(std::string_view self, const Requirement& req) {
    if (!req.min.empty() && version_order::compare(self, req.min) <  0) return false;
    if (!req.max.empty() && version_order::compare(self, req.max) >= 0) return false;
    return true;   // 空约束 = 无条件满足
}
```

### 3.2 可观测性：路由到非最新时必须说出来

沉默地降级和沉默地失败一样糟。`xlings update` 在路由到非最新快照时打印：

```
[index] xim: using 20e53c6 (2026-08-03) instead of e2aad0b
        e2aad0b requires xlings >= 2026.8.3.1, this is 2026.8.2.1
        upgrade: xlings self update
```

这不是装饰。没有它，用户会以为自己在最新索引上，然后困惑于"为什么新包搜不到"——
而这正是 `2026.8.3.1` 那批修复反复在打的形状。

`xlings self doctor` 同样报告一行：当前索引快照、是否为最新、若不是则原因。

### 3.3 手动钉子：调试与可复现，不是主路径

自动路由是主路径。钉子是**覆盖**：

```jsonc
{ "index_repos": [
    { "name": "xim", "url": "…", "artifact": "…", "version": "20e53c6" }
] }
```

```console
$ xlings index list [<name>] [--json]     # 枚举，无副作用
$ xlings index use <name> 20e53c6         # 写钉子并同步
$ xlings index use <name> latest          # 清钉子，回到自动路由
```

- 钉子**绕过兼容过滤**（你要求了具体版本就给你），但**不绕过 sha256 校验**。
- 钉到一个 `history` 里没有的版本 → **硬失败并列出可选集**，绝不回退。
  回退等于把客户端悄悄送回它正要躲开的快照。
- 钉到一个自己不满足的快照 → 允许，但打印警告（可能正是调试意图）。

**不提供 `xlings update --index-version`（issue 的写法）**：一次性钉会被下一次
`xlings update` 静默还原，"什么都没发生"和"成功了"再次不可区分。状态放配置里才可见。

**为什么 `xlings index` 而不是塞进 `update`**：`update` 有副作用、查询没有；
`update` 的位置参数已是 `[package] [version]`；`2026.8.3.1` 刚建立的 `CommandSpec`
单一事实源要求每个命令的参数形状可枚举，`index` 是干净的一行 spec，塞进 `update` 是特例。

### 3.4 可选集只有 `latest ∪ history`

**不支持钉到任意版本号**。任意版本意味着客户端自己拼 URL 且**手上没有 sha256**，
直接废掉"指针钉哈希"这条现有安全性质（记录：*陈旧镜像会被自动拒绝*）。
每条 history 自带 sha256，选旧快照沿用同一套校验。

---

## 4. 谁来抬 floor，以及怎么保证它没写错

一个只靠人记得去改的契约等于没有契约。

### 4.1 什么时候该抬

索引开始使用**只有新客户端才有**的能力时：新的 xvm 注册 node kind、新的 `spec` 版本、
新的 libxpkg 字段语义。

### 4.2 让它可检查（xim-pkgindex 侧 CI）

新增一个 CI 检查：扫描全部配方里出现的"已知需要客户端支持的构造"，
与 `index-compat.json` 声明的 `min` 比对，不覆盖就红。

```
recipe 使用 kind='files'      → 需要 >= 0.4.70
recipe 声明 spec = "2"        → 需要 >= 0.4.52
…（一张构造 → 最低客户端版本的表，随能力新增而追加）
```

这是把"作者承诺"换成"可验证的事实"——与本仓库 `2026.8.3.1` 那批修复同一条原则。

### 4.3 与"配方能力探测"的关系：互补，不替代

`reference_recipe_capability_probe` 记录的做法（配方在 Lua 里探测某函数是否存在）
**继续有效且更细粒度**：它让**单个配方**在老客户端上优雅降级，而不是让整个索引对老客户端不可用。

分工：

| | 粒度 | 代价 | 适用 |
|---|---|---|---|
| 配方能力探测 | 单个配方 | 配方作者要写 | 少数配方尝鲜新能力 |
| 索引版本契约 | 整个快照 | 发布方声明一次 | 索引整体开始依赖新能力 |

**先探测，实在做不到才抬 floor。** 抬 floor 是把老客户端冻在旧快照上——是安全网，不是首选。

---

## 5. 死锁：老客户端被路由到老索引，就再也看不到新 xlings

**这是本方案里最重要的一节，也是"从 xlings 侧考虑"才会暴露的问题。**

老快照的 `pkgs/x/xlings.lua` 里 `latest.ref` 是那个时代的版本。所以：

```
客户端 2026.8.2.1
  → 路由到 20e53c6（因为 e2aad0b 要求 >= 2026.8.3.1）
  → 该快照里 xlings latest = 2026.8.2.1
  → xlings self update：已是最新，退出 0，什么也没做
  → 永远升不上去，因此永远回不到新索引
```

死锁**由构造保证会发生**，不是概率事件。任何只做"路由"而不处理这条的设计都是坏的。

### 解法：路由不适用于客户端自身的升级路径

两半：

1. **指针顶层 `client_latest.xlings`** —— 与索引快照无关，任何客户端都读得到。
   `xlings update` 据此打印可升级提示（§3.2 那段消息的最后一行就来自它）。

2. **`xlings self update` 绕过路由**：始终取**最新**快照，且只从中读 `pkgs/x/xlings.lua`
   这一个配方。理由：该配方形状极稳定（`XLINGS_RES` + 每架构 sha256），
   而"解析一个新索引里的一个稳定配方"远比"用整个新索引"风险低。
   若该读取失败，**回退到 forge release 解析**——即 `quick_install.sh` 今天走的那条路，
   完全不经索引。

第 2 点顺带修掉一个既有弱点：今天 `self update` 依赖索引整体健康；之后它有一条不依赖索引的兜底。

---

## 6. 边界与交互

| 情形 | 行为 |
|---|---|
| 指针无 `history`（v1） | 只有一条候选（当前快照）。**仍然做兼容检查**：不满足就报错，不静默使用 |
| 索引树无 `index-compat.json` | 无约束，等价今天 |
| `XLINGS_INDEX_SOURCE=git` | git 源没有历史，但**树里有 `index-compat.json`**。不满足则**拒绝并说明**，好过稍后神秘失败 |
| 发布包内置索引 | 构造上兼容；仍应带该文件，让检查路径统一 |
| 子索引（awesome/scode/d2x） | 各自独立 key、独立路由。混合状态可接受（独立命名空间），但 `update` 需逐个报告落地版本 |
| 指针 CDN 延迟 | 不变。被路由/钉住的客户端天然免疫（`reference_index_publish_lag`） |
| 首次安装（无本地索引） | 同路径；无兼容快照时给出明确的"你需要哪个版本" |

---

## 7. 第三方消费者：同一机制的自然结果

`requires` 是 consumer → 约束的映射，xlings 只解释 `xlings` 键：

```json
"requires": {
  "xlings": { "min": "2026.8.3.1" },
  "mcpp":   { "min": "2026.8.3.3" }
}
```

- xlings 自动路由只看 `xlings`。
- 其余键**原样透传**，经 `xlings index list --json` / NDJSON capability
  `list_index_versions` 暴露给对应消费者，由它自己判断并调用 `xlings index use`。
- xlings **不校验**其他键的语义，只校验整体是 JSON 对象且序列化 ≤ 4 KiB
  （指针在每次 `update` 的热路径上）。

于是 #476 里 mcpp 的需求**不需要任何专门设计**——它是这套机制的一个使用者，
而不是这套机制的目的。

---

## 8. 安全

- **每条 history 自带 `artifact.sha256`**，选旧快照沿用同一套钉哈希校验，不新增未验证下载路径。
- **降级攻击**：能篡改指针者可诱导客户端选旧的、有已知问题的索引。但同一攻击者今天就能
  **直接投喂一个旧指针**达到同样效果——本特性不放大它。
- **拒绝服务**：篡改者可把所有快照的 `min` 抬到不可达 → 客户端无兼容快照。
  失败是**响亮**的（报错 + 说明），不是静默降级，所以可诊断。
- 真正的缓解是 manifest 里预留的 `signature`（X-full）。本方案不实现它，
  但**约束**：history 的每一条与 `client_latest` 都必须落在未来签名的覆盖范围内。

---

## 9. 测试契约（每条都必须先红）

沿用 `2026.8.3.1` 的规矩：**一条测试如果在被测对象完全不工作时仍然通过，它验证的是副本**。

| # | 契约 | 反证方式（必须让它失败） |
|---|---|---|
| 1 | 4 段版本的比较正确：`2026.8.3.2` 满足 `min=2026.8.3.1` | 改用 `semver::satisfies_expr` → 必须红（这是 §1 的实测，写成回归） |
| 2 | 客户端低于最新快照 floor → 落地**次新的兼容快照**，且打印原因 | 改成总取 `history[0]` → 必须红 |
| 3 | 无任何兼容快照 → 硬失败，**且不动本地索引树** | 改成回退最新 → 必须红 |
| 4 | v1 指针（无 history）+ 不兼容 → 报错而非静默使用 | 改成忽略 requires → 必须红 |
| 5 | 钉到 history 之外的版本 → 硬失败并列出可选集 | 改成回退 latest → 必须红 |
| 6 | 钉住的 artifact sha256 不符 → 拒绝并保留原树 | 去掉校验 → 必须红 |
| 7 | 非 `xlings` 的 `requires` 键逐字节透传（含未知键/嵌套/unicode） | 加任何"规范化" → 必须红 |
| 8 | **死锁不成立**：客户端被路由到老快照后，`self update` 仍能装到 `client_latest` | 让 self update 走路由 → 必须红 |
| 9 | v1 指针 + 无约束 → 行为与今天**逐字节一致**（差分测试，对比旧二进制） | 任何回归 → 必须红 |
| 10 | history 组装：20 快照 / 3 种 requires → 恰为 A∪B 且倒序 | 改成"取最近 N 个" → 必须红 |

第 9 条是防回归关键：这条路径今天服务所有用户，**新特性的默认路径必须一字不差**。
第 8 条是本方案最容易被漏掉的契约，而漏掉它等于把老用户永久冻住。

---

## 10. 分期

| 期 | 内容 | 独立价值 | 依赖 |
|---|---|---|---|
| **P1** | 客户端：解析 `requires`/`history`/`client_latest`、`satisfies` 求值、自动路由 + 硬失败、路由原因输出 | 任何已发布 v2 指针的索引立刻可用 | 无 |
| **P2** | `self update` 绕过路由（§5） | **解死锁——必须与 P1 同批发布** | P1 |
| **P3** | 发布工具：读 `index-compat.json`、`push_index_pointers.sh` 组装 history + `client_latest` | 官方索引开始发布契约与历史 | — |
| **P4** | `xlings index list/use` + NDJSON capability + 钉子 | 调试、可复现、第三方消费者 | P1 |
| **P5** | xim-pkgindex CI：构造 → 最低客户端版本 的可检查表（§4.2） | 把 floor 从承诺变成事实 | P3 |

**P1 与 P2 不可拆分发布。** 只发 P1 会让老客户端被路由到老索引却无法升级——
比今天的硬失败更糟，因为它是静默的。

---

## 11. 明确不做

- **不扩展 `semver` 到 N 段**（§1）：会波及所有包解析。
- **不引入范围表达式语法**：两个边界够用；第二套语法会与包语义漂移。
- **不支持钉到 history 之外的任意版本**：没有 sha256 就没有安全保证。
- **不做 `update --index-version` 一次性钉**：会被下次 update 静默还原。
- **不改 artifact 保留策略**：本来就永久保留，本方案只加寻址能力与契约。
- **不替第三方消费者做兼容判断**：xlings 只解释 `requires.xlings`。
- **不移除配方能力探测**：它更细粒度，且是首选（§4.3）。

---

## 12. 一句话

> 让索引声明它需要什么客户端，让客户端自己路由到能用的最新快照——
> 前提是这个比较器**认得 xlings 自己的版本号**（现有 semver 不认，§1 实测），
> 而且**升级通道不能被自己路由掉**（否则老客户端被永久冻住，§5）。
