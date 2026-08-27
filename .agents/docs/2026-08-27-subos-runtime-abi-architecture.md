# subos runtime:从「记下一个版本号」到「声明一份 ABI 契约」

> 2026-08-27 · 承接 `2026-08-27-self-init-must-not-guess-the-binding.md`
> 触发:glibc 2.44.2 撤了三次都撤不干净,而每一次给出的「真因」
> 都只是下一层的表象。

---

## 0. 一句话

subos 的 `runtime` 是一份**声明**,而系统里没有任何东西负责让实际状态收敛到它 ——
声明由 subos 层写,实际由 xim 层定,两层只通过一个「各自算出来的目录名」间接相遇。

把它变成**有执行者的不变量**,并且把今天挤在一个字符串里的三件事拆开。

---

## 1. 一个字符串扛了三件事

```
runtime = "glibc@2.44"
```

它同时在说:

| 它表示的 | 例子 | 变化节奏 |
|---|---|---|
| **ABI** —— 消费者链接的目标 | `linux-x86_64-glibc` | 几乎不变 |
| **provider** —— 谁提供这份 ABI | `glibc` | subos 创建时定,之后不变 |
| **payload 修订** —— 盘上哪一份 | `2.44` → 目录名 | **每次重新打包都变** |

三者节奏差了几个数量级,却编码在一个字符串里。直接后果:

> **重新打包一次 = 换一个绑定身份。**

而那是假的 —— 2.44 与 2.44.2 是同一份上游 glibc、同一个 ABI,修订是**我们的打包行为**。
2.44.2 撤了三次,每次都是在用索引给这个编码方式打补丁。

---

## 2. 实测:不变量没有执行者

一个不变量需要三个角色。对照现状:

| 角色 | 谁做 | 状态 |
|---|---|---|
| **establisher** 创建时确立 | `subos.cpp:666` "A declared runtime is installed, not merely recorded" | ⚠️ **只覆盖三条创建路径中的一条**(见下) |
| **enforcer** 之后不许偏离 | —— | ❌ **不存在** |
| **checker** 发现偏离 | mcpp `select_glibc_payload_lib` | ⚠️ 在**另一个仓库** |

**establisher 的闸开在错误的位置**:

```cpp
if (!runtime.empty()) {   // 看的是 --runtime 原始入参,不是 effectiveRuntime
```

| 创建路径 | 绑定来源 | 装不装 |
|---|---|---|
| `subos new --runtime X` | 显式 | **装** |
| `subos new`(不带 flag) | 默认值 | 只记 |
| `self init` → `subos/default` | 默认值 | 只记 ← **每个新用户走这条** |

紧邻那段的注释自己点名了:*"the default subos that `self install` creates goes through that path."*

**enforcer 缺席的判据**(可机械复核):

```
runtime_for（读绑定的函数）出现在 4 个文件:
  src/core/subos/manifest.{cpp,cppm}  src/core/subos.cpp
  src/core/xself/init.cpp             src/core/xself/doctor.cpp
src/core/xim/ 下:零出现。
```

**而权威是反的**。`pin_to_active_if_satisfies_`(`xim/commands.cpp:372`)读
`xvm::get_active_version` —— 让**实际状态**去约束后续解析。于是 manifest 那一行
降级成注释,真正执行它的只有下游的 mcpp。

**分歧从哪来**:`catalog.cpp` 的 `select_version_` 对同一张表有两条分支 ——

```cpp
// 无 hint（绑定走这条）:  return latestIt->second.ref;        // latest 的 ref
// 有 hint（36 处 >=2.39）: return select_best(available, hint); // 最大满足,且显式排除 latest
```

`latest` 不是表中最大项时,两者**必然**给出不同答案。

⭐ 「记了不装」不是根因,是这个假设失效时**唯一会炸的地方** ——
因为没装,所以谁先到谁说了算,而先到的那个问的是另一个问题。

---

## 3. 设计的真正约束:runtime 不一定是 glibc

`RUNTIME_PACKAGES` 已经列了五个:`glibc` / `musl` / `wasi-libc` / `macos_sdk` / `ucrt`。
索引里今天只有前两个。**而后两个根本不是包:**

> *"there is no ucrt payload to bind to — it is an OS component"*
> —— mcpp `runtime_binding.cppm`

所以「声明的 runtime 必须被装上」对它们是**假命题**。今天这个区分靠**散落的名字判断**:
`runtime_provider()` 只是取 `@` 前面那段,由每个调用方自己决定拿它怎么办。

⚠️ **这正是「加一个新 runtime 要改 N 处」的形状。** 任何只盯着 glibc 的修法都会
在第三个 runtime 上重来一遍。

---

## 4. 模型

### 4.1 三个正交概念

**ABI 已经存在,而且被声明了两遍。**

```lua
-- xim-pkgindex pkgs/g/glibc.lua:62-66
exports = { runtime = {
    loader = "lib64/ld-linux-x86-64.so.2",
    abi    = "linux-x86_64-glibc",
} },
```

```cpp
// xlings src/core/subos/manifest.cpp:9-19
std::string family_of(std::string_view runtime, std::string_view arch) {
    if (name == "glibc")     return std::format("linux-{}-glibc", arch);
    if (name == "musl")      return std::format("linux-{}-musl", arch);
    ...
```

⚠️⚠️ **同一个事实,包声明一份、引擎从名字再推一份,而两边谁也不知道对方存在。**
一个新 runtime 要同时改包和 `family_of`,漏掉后者就得到 `"unknown"`。

⇒ **ABI 以包声明的那份为准**,`family_of` 退役成「旧 manifest 的回退推导」。
⭐ 这一条是让非 glibc runtime **不需要改引擎代码**的关键。

### 4.2 vendored 与 hosted 是两种不变量

| | 例子 | 载荷 | 不变量 |
|---|---|---|---|
| **vendored** | glibc, musl, wasi-libc | 在 store 里 | 必须**装上并钉住** |
| **hosted** | ucrt, macos_sdk | 无 | 必须**在位且兼容**,绝不安装、绝不钉 |

hosted runtime 连 recipe 都没有,所以这个区分不能由包声明 ——
它必须住在 xlings,但是**一张表**,不是散落的 name check。
`RUNTIME_PACKAGES` 已经是那张表,加一列即可。

### 4.3 不变量的完整陈述

> **一个 subos 声明恰好一个 runtime;装进这个 subos 的一切都链接向它。**

这句话仓库里已经写了一半 —— `manifest.cppm` 的「subos 层的『恰好一个』」一节,
连同 `duplicate_bindings` 谓词和那次实测(mesa@25.0.7 与 25.0.7.1 同时绑在
`default`,EGL 把设备枚举了两遍,doctor 一声不吭)。

⭐ **runtime 只是这条不变量里「违反即致命」的那个特例。** 别的包重复只是浪费,
runtime 重复/缺失会让 INTERP 和 RUNPATH 来自两份不同的 libc。

---

## 5. 设计

### D1 —— 解析优先级:declared > active > index

`pin_to_active` 的思路是对的,只是钉错了源:
**active 是安装顺序的意外,declared 是一个决定。**

作用域限定在 runtime 包(绑定本来就只声明这一类)。三级:

```
1. subos 声明了 runtime,且该版本可解析  → 钉到它
2. 否则,该包在本 subos 已 active        → 钉到它（今天的行为）
3. 否则                                  → 索引解析
```

**这是根治**:在一个声明了 runtime 的 subos 里,那个包的版本**不再由
`select_best` 回答**,§2 末尾那个「同一张表两个问题」当场消失。

⭐ **副产物:`self init` 不必变重。** 不用急着下 40MB —— 第一个需要 libc 的安装
会自动装成绑定的那一个。5 秒的 `self init` 与正确性同时保住。

**降级规则(必须实现,否则老 subos 全砖):**

| 情形 | 行为 |
|---|---|
| declared 的版本不在索引里 | 退回第 2/3 级 + **告知**,绝不硬失败 |
| declared 与 active 冲突 | declared 赢,**并出声** —— 那意味着有人往这个 subos 里装过别的 libc |
| hosted runtime | 不参与钉,走 4.2 的另一条不变量 |

### D2 —— ABI 是被声明的值,不是从名字推的

subos manifest 里 `runtime_abi` 与 `runtime` 并列记录,值取自包的
`exports.runtime.abi`。

- 消费者据 **ABI** 判兼容,不必认识 `"glibc"` 这个词
- `family_of` 只在 `runtime_abi` 缺席(旧 manifest)时回退
- 新增一个 runtime = 在包里声明 abi + 在 4.2 那张表加一行,**引擎的判断逻辑不动**

### D3 —— 满足性谓词收归 xlings 一处

```
runtime_status(subos) -> Satisfied | Missing | Mismatched | Hosted
```

doctor、`subos info`、以及把 subos 交给消费者之前都用它。
mcpp 保留它的精确查找作**最后一道**(#392 的理由没变),但它不该是**发现者** ——
发现者跑到下游仓库,本身就是上游缺谓词的症状。

### D4 —— 换绑是操作,不是副作用

```
xlings subos runtime set <binding>      # 显式重绑 + 重链
```

**索引更新永远不改变已有 subos 的绑定。** 推论要写明白并接受:

> 打过补丁的 runtime **只进新 subos**;已有 subos 要人明确迁移。

这是对「安全修复能不能自动到达用户」的**自觉回答**:不能,而且是刻意的 ——
在一个 subos 底下换掉 libc,正是 #392 要防的事。今天这件事是**沉默地做不到**,
D4 把它变成**显式地做得到**。

### D5 —— 创建期安装降级为优化

有了 D1,正确性不再依赖走了哪条创建路径。
`--runtime` 显式时仍然 eager(用户明说了,给他备好),默认路径保持懒 ——
`subos.cpp:666` 那段注释描述的危险由 D1 兜住,而不是靠这段代码在场。

---

## 6. 迁移与兼容

- **已有 subos 一个不动**:D4 保证索引变化不改绑定;D1 第 1 级读的就是它们已记的值
- `runtime_abi` 缺席 = 旧 manifest,由 `family_of` 回退推导 ⇒ `SCHEMA_VERSION` 不动
- ⚠️ **新键要先测**:同类实测记录在案 —— 依赖清单加新层名时,一个键被**静默忽略**、
  另一个让**整份 manifest 拒绝加载(exit 2)**。判据不是「新 xlings 支持了」,
  是**索引 `latest` 指向的那个版本**怎么反应。五分钟能测,测法见
  `new-capability-key-floor-measured`
- xim-pkgindex 侧:一条测试断言 runtime 包不出现「同 ABI 两个键」,
  或者显式接受并由 D4 承担后果

---

## 7. 判据(每条都要跑反向对照)

**A1 —— 窗口判据(核心)**
全新 home,`self init` 之后、**任何 runtime 被激活之前**,让索引提供一个比绑定
更大的 runtime 版本,断言装下来的是**绑定那一个**。
**反向对照**:摘掉 D1,同一条必须红 —— 否则它证明不了任何东西。

**A2 —— 规则不认识 "glibc"**
用 `musl` 建一个 subos,A1 原样成立。
⭐ 这条是 D2 的判据:**只对 glibc 成立的修法在这里会红。**

**A3 —— hosted runtime 不被当成缺件**
ucrt / macos_sdk 绑定下,不得尝试安装,且 `runtime_status` 不得报 `Missing`。
**反向对照**:把它错标成 vendored,这条必须红。

**A4 —— 索引动不了已有 subos**
把索引 `latest` 移到一个新修订,断言已有 subos 的 `runtime`、`runtime_abi`
与**实际激活的版本**三者都不变。

**A5 —— 降级不硬失败**
断网、或 declared 的版本已从索引撤下 → 命令**成功**,退回第 2/3 级并告知。
⚠️ 两个方向都要测,否则「永远降级」与「永远钉住」读数相同。

**A6 —— 分母**
A1 里要断言「恰好一个 runtime 载荷目录」,而不是「绑定那个存在」——
后者在装了两份的情况下也通过。

---

## 8. 这个方案不解决什么

**老客户端拿不到打过补丁的 runtime。** append-only 的 sha256 承诺封死了
「换掉已发布版本的字节」,而任何新版本号都会成为 `select_best` 的最大者。
⇒ **没有任何办法把补丁送到老客户端手里。**

这不是本方案的缺陷,是精确身份 + append-only 的必然代价。D4 的价值在于
把它从「沉默地做不到」变成「显式的迁移操作」。

---

## 8.5 实现回填（2026.8.27.5,本文落地后补）

**落地形态与本文的三处出入,都是实测推翻推理:**

1. **D1 不需要改 resolver,一行都不用。** `pin_target_to_active` 的**机制**本来就是
   对的,错的是喂给它的函数回答了另一个问题。改的是那个函数(`subos_version_of_`),
   外加把 `ActiveVersionFn` / `pin_target_to_active` 改名成不再说谎的
   `SubosVersionFn` / `pin_target_to_subos`。
   ⭐ **「同一个机制,换一个更好的问题」比新增机制便宜一个量级。**

2. **D2 不需要往已发布的包里加新键。** `exports.runtime.abi` **早就存在**,
   glibc.lua 与 musl.lua 都已声明。§6 里「新键可能让整份 manifest 拒绝加载」
   那条最大风险**根本不存在**。新增的只有 subos manifest 里的 `runtime_abi`,
   而 `validate_block` 是**点名检查**不是穷举拒绝,`parse` 走 `b.value(...)`
   ⇒ 旧 xlings 读到会忽略,`SCHEMA_VERSION` 不动。

3. **D3 没有新建 `runtime_status` 四态谓词。** 仓库里已有
   `check_runtime_activation`,再造一个就是本文第 4.1 节批评的那种「同一事实两处
   推导」。改成**扩它**:加一条 hosted 早退。⭐ 写文档时提的 API 形状,不该压过
   落地时发现的既有 API。

**D5 一行代码都没改。** 有了 D1,创建期那段 eager install 不再承担正确性 ——
改的是它的**注释**:它曾经声称自己防住了那个失败,而它只防住了三条创建路径里的一条。

### 判据实测

| | 内容 | 结果 |
|---|---|---|
| A1 | 声明压过索引最大者 | 7.7.7 胜出,7.7.8 未被选 |
| A2 | 同一条规则用 musl | 6.6.6 胜出 |
| A2b | `runtime_abi` 来自包声明 | `linux-x86_64-musl` |
| A3 | hosted 不报缺件 | doctor 不提 ucrt 载荷 |
| A4 | 索引移到 7.7.8 后已有 subos 不动 | 仍 7.7.7 |
| A5 | 声明不可满足时降级不硬失败 | 落到 7.7.8 |
| A6 | 分母:计划里恰好一个 runtime 版本 | 恰好一个 |

**两个反向对照都跑了,而且它们抓到的是不同的东西:**

```
摘掉 declared 那一级        → A1 红:解析选中 xim:glibc@7.7.8(索引最大者)
把那一级改成只认 "glibc"    → A1 绿而 A2 红 ← A2 独有的牙齿
```

⭐ 第二个对照是必须做的:**只做第一个的话,「修好了」与「只修好了 glibc」读数相同**,
而本文第 3 节的全部论证就是这两者不是一回事。

⚠️ **A4 第一次是红的,而红的是 fixture 不是代码**:我那次改写把 7.7.7 从表里删掉了,
于是走的是 A5 的降级路径却挂着 A4 的名字。**两个场景在 fixture 里被我混成了一个。**
判据要能区分「索引推荐了更新的」与「声明的那个被撤下了」——它们降级方式不同。

## 9. 未决

1. **vendored/hosted 那张表放哪** —— 倾向 `RUNTIME_PACKAGES` 加一列,
   与 `family_of` 的退役同一个 PR,免得又变成两处推导。
2. **`runtime_abi` 的键名**要与 `exports.runtime.abi` 对齐还是另起 —— 倾向同名,
   两处指同一个事实时用同一个词。
3. **musl subos today 真跑得起来吗** —— 索引里有包 ≠ 能建 subos。
   A2 之前要先验一次,否则那条判据会以「基础设施没有」的形式假绿。
4. **D1 是否该扩到 runtime 之外** —— `duplicate_bindings` 说明同样的问题对
   mesa 也发生过(只是不致命)。倾向先只做 runtime,把通用化留到有第二个实例。
