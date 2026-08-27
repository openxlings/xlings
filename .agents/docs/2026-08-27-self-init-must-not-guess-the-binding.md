# `self init` 不该在能查的时候去猜绑定

> 2026-08-27 · 承接 `2026-08-27-runtime-binding-default-from-index.md`
> 触发:那篇的修复**在最要紧的一条路上没有生效**——全新 home 的第一次构建
> 仍然走编译期常量

---

## 0. 一句话

`xlings self init` 创建 `subos/default`,**在这一刻决定 runtime 绑定**;而它依赖的
索引要到之后才可用。所以「向索引解析」这条路在**新用户的第一次**上必然落空。

把「确保索引可用」放进这个决定之前——责任回到知道这件事的人手里。

---

## 1. 实测:修复没有覆盖到主路径

全新 `MCPP_HOME`,已发布的 `xlings 2026.8.27.3`,CN 镜像(配置已回读确认):

```
runtime        = glibc@2.44
runtime_source = fallback        ← 不是 index
```

构建成功、产物能跑,**但只是因为常量恰好等于索引当时提供的版本**。

`runtime_source` 这个字段是上一轮为这种情形加的,它第二次派上用场:
**绑定值正确 ≠ 机制在工作。**

---

## 2. 为什么必然落空:顺序

mcpp 的引导序列(`mcpp/src/platform/xlings/xlings.cppm` `ensure_init()`),
它自己的注释写得很清楚:

```
print_status("Initialize", "mcpp sandbox layout (one-time)")   ← xlings self init,绑定在此定死
// The first real `xlings install` (the NEXT bootstrap step) triggers
// xlings to fetch its package index
print_status("Fetching", "package index (one-time)")           ← 索引这时才到
```

而 xlings 侧,写下这个块的是 `src/core/xself/init.cpp` 的
`ensure_subos_manifest_()`,在 `creating` 分支里调
`mf::runtime_for(..., Intent::Create, {}, def.binding)`。
`def` 由 `xim::index_version_of(DEFAULT_RUNTIME_QUERY)` 求得——**那时索引不在盘上**,
返回 `nullopt`,于是落到 `DEFAULT_RUNTIME_FALLBACK`。

---

## 3. 为什么不能「答不出就不写」

这是最先想到的方案,而且和这个文件已有的词汇一致(`Describe` 意图下写**缺席**)。
但**缺席在下游不是中立的**:

```
// mcpp/src/platform/runtime_binding.cppm
"Where the C runtime comes from a payload, there is now no declared runtime
 to bind to — mcpp declines to guess a version, so the link falls back to the
 host and the hermeticity check will say so."
```

缺席 ⇒ **回落到宿主链接** ⇒ 丢掉封闭性。响是响,但结果是错的。

所以在 `Create` 这条路上,**必须给出一个值**。既然必须给,那就应该去**查**,
而不是猜。

---

## 4. 责任在 xlings,不在调用方

我一度把它判成「mcpp 的顺序问题」,那是把**最小的改动**当成了**问题的归属**。
决定这件事所需要的三样东西,全都在 xlings 手里:

| | 归谁 |
|---|---|
| 索引在不在、能不能刷新 | xlings(`cmd_update`,`PackageCatalog::is_loaded()`) |
| 绑定这个决定本身 | xlings(`runtime_for` 第 5 步) |
| 缺席的后果是「回落宿主」 | xlings(所以只有它知道这里必须给值) |

三样都在手里却不确保第一样就做第二个决定,等于把一条**没写在任何地方的顺序要求**
输出给每一个调用方:

> 「调 `self init` 之前请先取索引,否则绑定是猜的。」

mcpp 没满足它。任何基于 xlings 的工具都不会满足它。**一个用户在新机器上手敲
`xlings self init` 同样不满足。** 让每个调用方记住一条隐式顺序,是接口的问题。

在 mcpp 里调换顺序**能解决今天这个症状**,但陷阱还架在那里等下一个调用方。

---

## 5. 方案

`ensure_subos_manifest_()` 在 `creating` 且即将写入绑定之前,先确保索引可用:

```
creating?
  ├─ 确保索引(见 §5.1 的分级)
  ├─ 索引答得出  → binding = 索引的 latest,runtime_source = index
  └─ 索引答不出  → binding = DEFAULT_RUNTIME_FALLBACK,runtime_source = fallback
Describe?
  └─ 一个字都不改:答不出就是缺席,这条路已经是对的
```

### 5.0 落地形态:分级是**现成**的,不必新写

`CatalogAccess::InstallReady`(`src/core/xim/commands.cpp` 的 `get_catalog`)
已经就是这三级:

```
rebuild() 成功且子索引同步过        → 直接用,不联网
rebuild() 失败 或 子索引从未同步    → sync_all_repos() 一次,再 rebuild
仍然失败                            → 记日志,返回未加载的 catalog
```

所以实现只有两处:

1. `index_version_of(package, access = LocalOnly)` —— 加一个访问模式参数
2. `xself/init.cpp` 的 `creating` 分支传 `InstallReady`

其余调用点(`subos new`、fork、rebuild)保持 `LocalOnly` 不变 —— 它们跑得频繁,
且那时索引通常已经在了。**只有「答案即将被永久写下」的那一处值得一次网络往返。**

### 5.1 「确保索引」要分级,不能一律联网

`self init` 今天是**快且离线**的,变成网络阻塞是实打实的退步——容器构建、
无网机器、CI 的冷启动都会受影响。分三级:

| 情形 | 做什么 |
|---|---|
| 本地索引已存在(不论新旧) | **直接用**,不联网。`latest` 极少变,陈旧的答案也远好过常量 |
| 本地没有索引,但允许联网 | 取一次。这台机器接下来必然要用索引,提前一步墙钟成本为零 |
| 本地没有索引,且离线/取失败 | 落到 `DEFAULT_RUNTIME_FALLBACK`,记 `runtime_source: fallback` ——**与今天完全一致,不更差** |

关键性质:**离线路径的行为不变**。这个改动只会把「本可以查到却猜了」的情形
变成查到,不会把任何今天能成的事变成不能成。

### 5.2 常量退回它该在的位置

`DEFAULT_RUNTIME_FALLBACK` 从「新用户实际使用的值」变回「真的问不到时的兜底」。
`#570` 已经给它定了正确的规则——**指向一个索引不可能停止提供的版本**,
而不是「和 `latest` 保持一致」。这条规则在本方案下更重要:兜底路径会更罕见,
也就更不容易被发现写错。

---

## 6. 各角度的取舍

**架构** — 决定与它依赖的资源被放回同一层。调用方不再承载正确性。

**稳定性** — 消除一条隐式顺序要求。新调用方(以及手敲命令的用户)默认正确。

**优雅/简洁** — 不新增概念:`runtime_source` 已存在,`Create/Describe` 的分野
已存在,分级降级是既有的 `is_loaded()` 语义。净增的是**一次条件性的索引确保**。

**用户体验** — 新用户的第一次构建从「碰巧对」变成「确实对」。离线用户不受影响。
`Fetching package index` 这行会更早出现,位置更合理(它确实是在为紧接着的
决定服务)。

**兼容性** — 只影响**新建** subos。已存在的 subos 由 `runtime_for` 第 1 步保住
已记录的绑定,一个都不动。manifest 不加字段,`SCHEMA_VERSION` 不动。

**跨平台** — 决定点与索引 API 都是平台无关的;Windows 侧 `self init` 的调用形状
不同(mcpp 走 `env::set` 而非 `cd &&`),但本改动在 xlings 内部,两侧同样受益。

**一致性** — 与 `Describe` 路径的教条对齐:**能核实就核实,核实不了就说不知道;
只有在「不知道」会造成更坏后果的地方(Create)才给默认值,并记下这是默认值。**

**无感升级** — 用户不需要做任何事。装到新版之后,新建的 subos 自动跟着索引走;
老 subos 保持原样。

---

## 7. 判据(每条都要反向对照)

**W0 — `self init` 本身在索引未同步的 home 上也解析(落地为 S6)**
`$S4_HOME` 天然就是这个形状:它的 `self init` 跑在 `update` **之前**,索引仓库
已配置但从未同步。断言 `subos/default` 的 `runtime == glibc@9.9.9`
且 `runtime_source == index`。
**反证已实测**:把 `InstallReady` 换回 `LocalOnly`,**S4/S5 照样通过,S6 失败**,
拿到的是常量 `glibc@2.44`。这证明 S6 测的正是 S4/S5 结构上够不到的那段
——它们都断言在 `update` 之后由 `subos new` 建的 subos 上。

**W1 — 有索引时,新建 subos 走索引**
`self init` 在一个索引可用的 home 上 → `runtime_source == index`,且绑定等于
`xlings info xim:<pkg>` 报的 selected version。
**反证**:把 `DEFAULT_RUNTIME_FALLBACK` 改成一个明显错误的值,W1 必须仍然通过。

**W2 — 没索引且离线时,行为与今天一致**
断网 + 空 home → `self init` **成功**(不因取索引失败而失败),
绑定 == `DEFAULT_RUNTIME_FALLBACK`,`runtime_source == fallback`。
**反证**:两个方向都要测,否则「永远回落」与「永远解析」读数相同。

**W3 — `self init` 不因索引而变慢(已有索引时)**
本地索引存在的情况下,`self init` **不发起网络请求**。
判据取「有没有网络调用」,不取墙钟(墙钟会被缓存和机器状态污染)。

**W4 — Describe 一个字没改**
`doctor --fix` 在只有旧记录的 subos 上仍写它实际运行的那个,且**不等于**
当前默认值(该断言必须从源码推导默认值,不能写死字面量——这条已经踩过)。

**W5 — 端到端,新用户**
全新 `MCPP_HOME` + CN,`mcpp new && mcpp build`:构建成功,产物能跑,
且 `subos_info.runtime_source == index`(**不是 fallback**)。
**反证**:改动前的客户端在同一条命令下必须是 `fallback`——已实测,见 §1。

⚠️ 配置类命令跑完要**回读配置文件**确认落盘。本轮踩过:
`mcpp config --mirror CN` 不是命令(应为 `mcpp self config`),
它**打印用法却退 0**,于是几小时的下载全走了 GLOBAL。

---

## 8. 这个方案不解决什么

**老客户端与冻结的 pin。** 已经装在别人机器上的旧 xlings、workflow 里写死的
`BOOTSTRAP_XLINGS_VERSION`、mcpp 里已 vendored 的旧副本——它们的常量不会因为
本改动而变。

按「只管新用户」的范围,这条不影响结论。若将来要覆盖它们,那是另一个层次的
改动(索引键携带打包修订、而载荷目录与绑定用 ABI 身份),条件与理由记在
`2026-08-27-runtime-binding-default-from-index.md` §4.B。

---

## 9. 未决

1. 「本地索引存在但很旧」是否需要一个上限(比如超过 N 天仍刷新一次)。
   倾向不加:`latest` 变动罕见,而 `self init` 变成周期性联网的代价更大。
2. `self init` 是否应在无法确保索引时**告知**(而不只是记 `runtime_source`)。
   倾向加一行提示,与 `subos new` 的回落告警保持一致。
