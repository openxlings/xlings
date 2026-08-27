# 默认 runtime 绑定应当来自索引,而不是常量

> 2026-08-27 · 承接 `2026-08-26-private-glibc-preload-sysconfdir.md`
> 与 `2026-08-09-ecosystem-closure-design.md` §C1(AD-11 / DEFAULT_RUNTIME)
> 触发:`xim-pkgindex#692` 合入后,**全新 home 的第一次 `mcpp build` 直接失败**

---

## 0. 一句话

`DEFAULT_RUNTIME = "glibc@2.44"` 和索引里 glibc 的 `latest` 是**同一个决定写在两个仓库**。
`#692` 把 `latest` 提到 `2.44.2`,这个常量没跟着动,于是每一个**新建** subos
声明了一个盘上不存在的绑定。

把常量退化成**包名**,版本在**创建时向索引解析**并照旧持久化到 manifest;
索引不可读时回落到一个钉死的版本,**但把"这次是回落"记下来**。

---

## 1. 已经落地的部分(本设计的前提)

`xim-pkgindex#692`(已合入 main,索引已发布):

| | |
|---|---|
| `glibc.lua` | `latest → 2.44.2`;条目 append-only,`2.44`/`2.39` 原样保留 |
| 产物 | `glibc-2.44.2-linux-x86_64.tar.gz`,GitHub + GitCode 双区,下载回比对逐字节一致 |
| 补丁 | `elf/rtld.c` 的 preload 路径跟随 `--sysconfdir`;`XLINGS_LD_PRELOAD_FILE` 可运行期覆盖;变量入 `unsecvars.h` |
| 判据 | `verify-preload-closure.sh`(四态退出码,两侧对照);`TestRevisionOrdering` |

沙箱实测(真实 `/etc/ld.so.preload`,非 overlay):

```
[已发布的 2.44]  libc.so.6: error while loading shared libraries: libz.so.1
[2.44.2 走 CN]   GNU C Library (GNU libc) stable release version 2.44.
```

---

## 2. 问题:一个决定,两个仓库

### 2.1 实测

全新 `MCPP_HOME`,第一次 `mcpp build`:

```
Downloading xim:glibc@2.44.2                       ← 索引侧正确
error: default toolchain post-install fixup:
       selected RuntimeBinding glibc@2.44 requires payload
       '<home>/registry/data/xpkgs/xim-x-glibc/2.44',
       but it is not installed; mcpp will not fall back to another directory entry
```

取证:

```
gcc payload 的 PT_INTERP   → .../xim-x-glibc/2.44.2/lib64/ld-linux-x86-64.so.2   ✓ elfpatch 对了
subos/default/.xlings.json → "runtime": "glibc@2.44"                            ✗ 常量
xim-x-glibc/ 目录          → 2.44.2                                              (只有它)
```

载荷目录按版本命名,所以盘上没有任何东西答应 `glibc@2.44` 这个绑定。

### 2.2 影响面的形状

**只影响新建 subos,一个老 subos 都不受影响**——因为绑定是创建时属性、持久化在各自
manifest 里,`preserved_runtime` 会保住已记录的有效值。这正是最容易漏掉的形状:
开发机上早就建好的 subos 全绿,而任何新用户、新环境、CI 的干净 runner 全红。

### 2.3 为什么这次会漏,而以前不会

`DEFAULT_RUNTIME` 从 2.39 挪到 2.44 是**上游版本**变化——罕见、显眼、有人盯。
`2.44 → 2.44.2` 是**打包修订**:同一个上游发布,只因为产物要重打而变,
而且"索引发了个新修订"这件事在 xlings 这边没有任何东西能感知。
**修订号的引入,才让这两处第一次具备了漂移的条件。**

---

## 3. 方案 A:常量退化成包名,版本向索引解析

### 3.1 为什么这不违反当初写常量的理由

`manifest.cppm` 里那条理由是:

> A constant rather than a lookup: ... inventing a **"pick the newest libc present"**
> rule would make two homes with the same command produce different subos.

它反对的是**扫描机器上装了什么**。那确实不确定:两台机器装的东西不同,答案就不同。

**读索引的 `latest` 不是那件事。** 同一个索引快照 → 同一个答案,而这正是
`latest` 的定义,每一个 `xlings install` 已经这么做了。

而且策略本身已经搬走了。`glibc.lua` 现在把它写成明规则:

> `latest` ... **TRACKS the highest glibc of any distribution we support**,
> as a standing policy rather than a per-version judgement call

常量里那段 `our_glibc >= host_glibc` 的论证,是这条策略的**陈旧副本**。
删掉副本、留下正本,是这个改动真正在做的事。

### 3.2 形状

```
manifest.cppm(保持零依赖:不 import Config/xvm/catalog)
    DEFAULT_RUNTIME_PACKAGE  = "glibc"        ← 名字
    DEFAULT_RUNTIME_FALLBACK = "glibc@2.44.2" ← 仅在索引答不出时使用

    runtime_for(subosDir, doc, intent, requested = {},
                defaultRuntime = DEFAULT_RUNTIME_FALLBACK)
                ^^^^^^^^^^^^^^ 新增参数;第 5 步用它,不再用常量

subos.cpp(可以 import xim,已经在 import xlings.core.xim.commands)
    resolve_default_runtime() -> { binding, source }
        auto& catalog = xim::get_catalog(xim::CatalogAccess::LocalOnly);
        if (!catalog.is_loaded())                    -> { FALLBACK, Fallback }
        auto m = catalog.resolve_target("glibc", platform);
        if (!m || m->version.empty())                -> { FALLBACK, Fallback }
        return { "glibc@" + m->version, Index }
```

`runtime_for` 仍然是**唯一漏斗**——文件自己的教条("all of them ask the same
question, and the ONE thing they legitimately disagree about becomes a
parameter")。这次新增的参数正好是那个"唯一可以合法分歧的东西"。

### 3.2.1 解析器住在 xim 层,不在 subos 层

第一版把 `resolve_default_runtime()` 放在 `subos.cpp`,因为那里已经 import 了
`xlings.core.xim.commands`。**但这样 `xself/init.cpp` 够不到它**,而
`init.cpp` 正是 `xlings self init` 写 `subos/default` 的那条路——也就是
mcpp 沙箱初始化实际走的那条。漏掉它等于这个修复对主路径无效。

`init.cpp` 不能 import `xlings.core.subos`(subos 反过来 import xself,成环)。
所以「问索引要一个包的版本」下沉到 xim:

```
xim.commands:   std::optional<std::string> resolve_latest_version(pkg)
                    // 纯索引查询,LocalOnly,不知道 runtime 是什么

subos / xself:  拿到版本 -> 拼 "glibc@<ver>",或落到 FALLBACK,并记 source
                    // 策略在这里,查询在下面
```

分层也更正:xim 回答「索引说这个包是什么版本」,subos/xself 决定
「拿这个答案做什么」。前者没有 runtime 的概念,后者不需要知道索引长什么样。

### 3.3 只有 Create 传解析值

已核实的五个调用点:

| 位置 | intent | 传什么 |
|---|---|---|
| `xself/init.cpp:183` | Create | 解析值(**这条最要紧**:`self init` 写的 `subos/default` 就是 mcpp 首次构建读的那块) |
| `subos.cpp:479`(new) | Create | 解析值 |
| `subos.cpp:896`(fork) | Create | 解析值 |
| `subos.cpp:386`(rebuild) | 变量 | 解析值(Describe 时第 5 步不执行) |
| `xself/doctor.cpp:2056` | Describe | **不传**,保持返回空 |

Describe 路径一个字都不能改:那里"没有答案"必须继续是空,而不是一个默认值——
否则就是把猜测记成事实,而这正是这个文件此前修过的缺陷。

### 3.4 回落必须留痕

回落本身是必要的(`subos new` 今天不依赖索引,不能因为这个改动变成硬依赖),
但**回落的结果和解析的结果长得一模一样**,这是不能接受的:后来的人无法分辨
一个 subos 钉在旧 glibc 上是"当时索引就这么说"还是"当时索引读不到"。

两件事一起做:

1. **创建时给出可见告警**,点名 `xlings update` 和 `--runtime` 两条出路
2. **写进 manifest**:`subos_info.runtime_source ∈ {explicit, index, fallback}`

字段是**追加**的,老读者用 `b.value(key, default)` 读,不认识就忽略,
所以 `SCHEMA_VERSION` 不动(提升它会让今天的读者对明天的 manifest 报"更新的
schema",而这个字段并不改变任何既有语义)。

---

## 4. 没有采用的方案,以及为什么记下来

### B. 绑定身份 = ABI 身份,而不是打包修订号

`2.44.2` 与 `2.44` 的 **ABI 完全相同**——修订号是我们的打包行为。所以更根本的模型是:
索引键用 `2.44.2`(为了排序与 sha),而**载荷目录与绑定用 `glibc@2.44`**,
修订号对所有消费者不可见,`DEFAULT_RUNTIME` 永远不用动。

不采用的原因:要打破"一个版本一个目录"(`xim-x-<name>/<version>`)这个全生态
不变量,引入 install_as / abi 概念。**它更正确,但它是另一个立项**,
而本文要修的破口今天就在流血。

记下来是因为:如果 A 落地后仍然出现"某处硬编码了 glibc 版本"的第三例,
那说明问题不在具体某个常量,而在这个目录身份模型,那时应当直接做 B。

### C. 让绑定解析在找不到载荷时回落到兼容版本

mcpp 明确拒绝("will not fall back to another directory entry"),而且有理由:
编译侧与产物 interpreter 指向不同 glibc 时,一切看起来都正常,直到加载另一份。
放宽这里等于把一个响亮的失败换成一个安静的错误。

---

## 5. 验收判据

每条都要有**能证伪的反向对照**——否则不知道它在测什么。

### V1 —— 新建 subos 拿到的是索引的 latest,不是常量

**落地为** `tests/e2e/subos_runtime_binding_test.sh` 的 S4:给测试 home 一个
**能回答**的本地索引,其 `latest` 指向 `9.9.9` —— 这个字符串在整棵树的常量、
fixture、载荷里都不存在,所以 manifest 里出现它**只可能**是解析来的。
断言 `runtime == glibc@9.9.9` 且 `runtime_source == index`。

这就是反证内置进判据本身:一个仍然读常量的实现,S4 必然失败,
而且失败信息直接说出后果("会在下次 glibc 重新发布时再次漂移")。

### V2 —— 索引读不到时回落,且**留痕**

**落地为**同一个 e2e 的 S1:该 home 用的是**空索引**(原本就是为了避免联网克隆),
所以它天然是回落路径。断言绑定 == 从源码读出的 `DEFAULT_RUNTIME_FALLBACK`
(不是写死的字面量),且 `runtime_source == "fallback"`。

S1 与 S4 合起来才是判据:只有一侧的话,"永远回落"和"永远解析成同一个值"
读数相同——而那正是这次要消灭的形状,不能拿它来验证它自己。

### V3 —— Describe 路径没有被改到

`doctor --fix` 在一个只有 2.39 记录的 subos 上,`runtime` 仍是 `glibc@2.39`,
且**不等于当前默认值**。

`tests/e2e/subos_runtime_describe_test.sh` 里那条断言必须**从源码推导**当前默认值
而不是写死字面量——这次已经踩过:它原本写 `!= "glibc@2.44"`,默认值一动
就变成恒真。

### V4 —— 端到端:全新 home 的第一次 `mcpp build`

沙箱(`xlings subos use <n> --sandbox bwrap`)+ 独立 `MCPP_HOME`:

```
mcpp config --mirror CN
mcpp new hello && cd hello && mcpp build
```

判据:**构建成功**,且 `$MCPP_HOME/registry/data/xpkgs/xim-x-glibc/` 里的目录名
与 subos 声明的 runtime 版本**相等**。

**反证**:在打补丁前的 xlings 上跑同一条,必须复现
`selected RuntimeBinding glibc@2.44 requires payload ... but it is not installed`。
(已实测复现过,见 §2.1。)

⚠️ `mcpp build 2>&1 | tail` 的退出码是 `tail` 的。判据取**输出文本**或分开取 `$?`。

### V4.5 —— 索引查询必须带命名空间(**实测抓到的缺陷**)

`2026.8.27.1` 发出去之后,在沙箱里用**已发布的客户端**跑,新建 subos 记的是

```
runtime        = glibc@2.44.2      ← 值是对的
runtime_source = fallback          ← 但机制是死的
```

原因:按裸名 `glibc` 查索引,在任何普通 home 上都是**歧义**的——默认子索引里
`scode` 也发 glibc:

```
[error] package 'glibc' is ambiguous, candidates:
  1. scode:glibc@2.44.2
  2. xim:glibc@2.44.2
```

`resolve_target` 返回错误 ⇒ `index_version_of` 变成 nullopt ⇒ 调用方读成
「索引答不出」⇒ 回落常量。**常量重新掌权,而且什么都不说。**

**只有 provenance 字段让它可见。** 那天钉死的值和索引的答案恰好相同,
所以单看绑定完全像是成功的。这正是 §3.4 加那个字段的理由,而它第一次就派上了用场。

修法:查询用**坐标**(`xim:glibc`),绑定用**名字**(`glibc`)——两个常量,
因为它们回答的是两个问题。

判据 **S5**:给 home 两个都答应 `glibc` 的索引仓库(xim 说 9.9.9,scode 说 8.8.8),
断言主索引说了算。**S4 结构上做不到这件事**——它的 home 只有一个仓库,裸名不歧义。
实测:把查询换回裸名,**S4 照样通过,S5 失败**。

S5 还会先断言「歧义是真的」再断言结论,否则它会悄悄退化成 S4 的副本。

### V5 —— preload 闭包没有被这轮改动破坏

```
.agents/tools/graphics/verify-preload-closure.sh <payload>   → 0
```
对未打补丁的载荷 → 2(inconclusive,**不是** 0)。

---

## 6. 落地顺序

1. **先落常量 bump**(`glibc@2.44` → `glibc@2.44.2`),一行。
   它把今天所有新建 subos 的破口立刻堵上,且是这个文件文档化的既有动作
   ("A future default bump updates one line here, knowingly")。
   同批把 `subos_runtime_describe_test.sh` 的恒真断言改成推导式。
2. **再落方案 A**,带 V1–V3 的测试。
3. V4 在 A 落地后于沙箱跑一次,作为跨仓库的合闸判据。

第 1 步不是第 2 步的替代品:没有第 2 步,下一次 glibc 出修订会**一模一样地**再来一次。

---

## 7. 未决

1. `runtime_source` 的取值集合是否还需要第四种(例如 `preserved`,
   表示 rebuild 保住了已记录的值)。倾向不加:那条路径不经过默认值。
2. 索引里出现多个 libc 家族(musl)时,`DEFAULT_RUNTIME_PACKAGE` 单值是否够用。
   `RUNTIME_PACKAGES` 已经是个有序表,届时默认值应当沿用同一个 tie-break,
   而不是新写一份顺序。
