# install / use 语义优化方案

**日期**: 2026-07-31
**类型**: 计划（plan）→ **已实现，随 `2026.7.31.3` 发布**
**基线**: `2026.7.31.2`（已发布）
**相关**: [#459](https://github.com/openxlings/xlings/issues/459)、`.agents/docs/2026-07-31-xvm-subos-architecture-review.md`

四项，互不依赖，可任意顺序落地。共同点是同一族：**命令报告的东西和它实际做的
不一致** —— 有的报成功而产物半残，有的没做错事却报 error。

| # | 问题 | 类型 | 规模 |
|---|---|---|---|
| **P1** | 依赖展开绕开了已有的版本语义；且已满足仍装最新版 | **真缺陷** ×2 | 小-中 |
| **P2** | `use` 在未安装该包的 subos 里静默给出半可用工具链 | **真缺陷**（静默成功） | 小 |
| **P3a** | `--pick` 该删 | 多余表面 | 小 |
| **P3** | `use <pkg>` 列版本却打印 `[error]` | 语义/表达 | 小 |
| **P4** | 测试里的"假 PASS" | 方法问题 | — |
| **P5** | 提议新建已存在的能力（两次） | 方法问题 | — |

**落地状态（2026.7.31.3）**：P1 / P2 / P3 / P3a 全部实现，P4 写进
`.agents/skills/xlings-contributing`（Step 4 新增"不会因为错误理由通过的断言"一节）。
测试：`semver` 新增 8 组 `satisfies_expr` 单测、`test_xim_catalog` 新增 6 组
`pin_target_to_active` 单测、新增 **E2E-51** `install_use_semantics_test.sh`（S1–S6），
**E2E-48** 的 N2/N3/N4 按新契约改写。全部在 `2026.7.31.2` 的**已发布二进制**上跑过
差分，逐条失败。

---

## P1. 依赖解析没有接上已有的版本语义，且已满足也照装最新版

### 现场

```
Packages to install (2):
    ◆ xim:mcpp@2026.7.30.3
    ◆ xim:mcpp-short-cmd@0.0.1
▸ Proceed? n

$ mcpp --version
mcpp 2026.7.30.2          ← 本地已有，且 mcpp-short-cmd 的 deps 没有指定版本
```

### 查证：版本语义**已经完整实现**，只是没接到依赖那条路

`src/core/semver.cppm`（344 行，28 个单测）已有完整的 range 支持：

```
"15"              → Gte{15.0.0} Lt{16.0.0}     ← 裸前缀自动展开成区间
"15.1"            → Gte{15.1.0} Lt{15.2.0}
">=1.0.0 <2.0.0"  → 区间
"^1.2.3"  "~1.2.3"  "1.2.*"  "1.*"             ← 全都有
select_best(available, range_expr)             ← 取满足约束的最高版本
```

**而且索引侧已经在用它**：`catalog.cppm:179` 的 `select_version_()` 对 `versionHint`
调用 `semver::select_best`，所以 `xlings install python@3` 能正常解析。

**实现时的复核修正**：`resolver.cppm:83` / `:134` 那段确实是"把 hint 原样当版本号"，
但它属于 `resolve()` 的 **IndexManager 重载**，而生产路径走的是 **PackageCatalog
重载**（`commands.cppm:316` 是唯一调用者）—— 那个重载只有 4 个单测在用，且它们全都
`GTEST_SKIP`。**它已随本次改动整体删除**：两条版本解析路径正是让上面这份诊断落在
死代码上的原因，留着它就是留着下一次误诊。

真正在生产路径上的两个口子是：

| 口子 | 位置 | 后果 |
|---|---|---|
| 依赖展开**根本不看工作区** | `resolver.cppm` `expand()` → `catalog.resolve_target(dep)` | 无约束 dep 一律解析成索引最新版 |
| 顶层的"已装就用"用的是**字符串前缀** | `commands.cppm` `pin_to_active_if_satisfies_`：`active.rfind(verHint,0)==0` | `@1.1` 会被 active `1.10` 命中；`^`/`>=` 一律不认 |

所以 range 语义在**索引选版**那一侧一直是通的（`install python@3` 本来就能解析），
断在**"已经装了的算不算数"**这一侧 —— 而这正是决定"要不要装"的那个判断。

顺带一个佐证：`semver::select_highest` **零调用者** —— 这个能力写好了，接了一半。

**这也解释了为什么索引里 `deps` 几乎全是 `@2.39` 这种锁死写法** —— 不是作者想锁，
是写 `@3` 会坏。修完之后 recipe 才有理由改用宽松约束。

### 两个缺陷

**P1-a 依赖展开绕开了版本语义。** 修法不是"实现 range"（已经有了），而是把
"已激活的版本满不满足这条约束"做成**一个**判据，两条路径都走它：

- `semver::satisfies_expr(version, expr)` —— 单版本满足性，空约束恒真；
  非 semver 的版本串（四段的 `2026.7.31.2`、带风味后缀的 `15.1.0-musl`）
  退化为**按分量边界**的前缀匹配，所以 `1.1` 不会命中 `1.10`；
- `xim::pin_target_to_active(target, activeOf)` —— 满足就把 target 改写成
  `name@<active>`，于是它解析成"已安装"并从计划里消失；不满足就原样返回；
- `resolve(catalog, targets, platform, activeOf)` 多接一个回调，`expand()` 和
  topo 走的是同一次改写（两处不一致会让边被静默丢掉）。

**P1-b 无约束时直接取 latest，不看本地已装什么**（`resolver.cppm:134` 的 `else` 分支）：

**"没有指定版本"被实现成了"要最新版本"，而它的意思是"任意满足的版本都行"。**

### 目标语义

| deps 写法 | 现在 | 应当 |
|---|---|---|
| `xim:pkg@2.39` | 装 2.39 | 不变（精确锁定就是锁定） |
| **`xim:pkg@3`** | ❌ `unknown version: 3` | 已装满足 `3` 的 → **用它**；否则装索引里满足 `3` 的最高版 |
| **`xim:pkg@^1.2`** | ❌ 同上 | 同上（`semver` 已支持这些运算符） |
| **`xim:pkg`** | 装 latest | 已装**任意**版本 → **用它**；否则 latest |

"用它"的判据**只有一条：本 subos 当前激活的版本**。

原方案写的第二档"`installed[]` 里满足约束的最高版本"**实现时去掉了**：payload 装在
哪个 subos 之外是共享的，一个"装过但在本 subos 没激活"的版本如果因此被判定为满足、
从而跳过安装，得到的正是 §P2 那个半可用结果 —— 换了身衣服而已。只认活动版本既安全，
也正好是优先级的第一档。

满足性判断用 `semver`（已有），**不新建任何版本语义**。

### 为什么 P1-b 是缺陷而不是偏好

1. **装个小工具顺手升级了工具链** —— 用户装 `mcpp-short-cmd`，mcpp 被换掉，他没要求过；
2. **同一条命令两天给出不同结果**，仅因为索引动了 —— 与 `install` 的幂等承诺相悖；
3. **把可选升级变成强制** —— `xlings install mcpp@latest` 才是升级的表达式。

### 边界

- **不新增版本语法** —— `semver.cppm` 已覆盖前缀、`^`、`~`、`*`、区间；
- **不自动降级** —— 已装版本高于约束上界时保持已装，不回退；
- **不改精确锁定的语义**。

### 验收 → E2E-51 `install_use_semantics_test.sh`

| | 断言 | 用例 |
|---|---|---|
| 活动 1.0.0，装 `deps={"xim:sem-lib"}` 的包 | 计划里不出现、2.0.0 的 payload 不落盘、活动版本不动 | S1 |
| 活动 1.0.0，`deps={"xim:sem-lib@1"}` | 同上，且不得出现 `unknown version` | S2 |
| 活动 1.0.0，`deps={"xim:sem-lib@2.0.0"}` | **仍装 2.0.0** —— 精确锁定不被"已装"覆盖 | S3 |
| 全新 subos 无活动版本，`@1` | 装索引里满足 `1` 的**最高**版 1.5.0（不是同样在盘上的 2.0.0） | S4 |

差分：S1 在已发布的 `2026.7.31.2` 二进制上失败（计划里出现 `sem-lib@2.0.0`）。

---

## P2. `use` 在未安装该包的 subos 里静默给出半可用工具链

### 现场（隔离 home 实测，`2026.7.31.2`）

```
subos probe（全新）
xlings use gcc 16.1.0        →  [xlings] gcc -> 16.1.0        ← 报告成功
g++ -print-sysroot           →  …/subos/probe                 ← 正确
g++ t.cpp                    →  fatal error: features.h: No such file or directory
```

`usr/include` 是空的。gcc 能跑、sysroot 对，**只是编译不了**，全程零提示。

### 成因

`use` 的 auto-add 语义（0.4.19+）：payload 在全局版本库里注册过，就允许在任何 subos
激活，并静默补进 `installed[]`。这对**无依赖的单体包**是对的（payload 共享，激活是
免费的），对**有依赖的包**则不是 —— glibc 是 gcc 的**依赖**，不是它 release 的成员，
而版本库根本不记依赖关系：

```
gcc 条目键: alias, bindingGroup, destinationName, kind, path, sourceName
bindingMembers: gcc / g++ / cpp / …    ← 没有 glibc
```

所以 **`use` 连"缺了什么"都答不上来**。

### 目标：不记依赖，改为在"本 subos 未安装"时提示

不引入依赖记录（那是更大的改动，且本问题不需要）。判据用现成的数据：

> **该包在本 subos 的 `installed[]` 里吗？**

| 情况 | 行为 |
|---|---|
| 在 → 正常切换 | 不变 |
| **不在** | **不切换**，只提示去安装 |

```
$ XLINGS_ACTIVE_SUBOS=probe xlings use gcc 16.1.0
gcc 未在本 subos (probe) 安装过
  安装: xlings install gcc
```

**不做任何动作**（原方案写的是"仍然切换 + 警告"，已改）。理由：**auto-add 给出的
是一个不完整的结果** —— 它激活了 release 本身，却激活不了它的依赖，而 `use` 没有
数据能知道缺什么。既然做不对，就不要做；把它交给知道怎么做对的那条路（`install`
会解析并安装依赖，实测在同一个全新 subos 里给出 140 个头文件、编译通过）。

这也让 `use` 的语义变干净：**`use` 只在已安装的版本之间切换，安装是 `install` 的事。**

### 实现时挖出来的连带缺陷：读写两侧的不变式不对称

`subos_workspace_to_json` 一直有一条写时不变式 —— 注释里写着 "an active version
is implicitly installed"，序列化时把 `active` 并进 `installed[]`。**但解析器没有这条
规则**：

```
文件里:  "gcc": "15.1.0"          （pre-0.4.19 的字符串形式）
读进来:  active=15.1.0, installed=[]   ← "这个 subos 什么都没装"
```

auto-add 在的时候这个不对称是无害的（谁都没读 `installed[]` 做决定）；**闸门一加，
它立刻变成"拒绝掉文件自己说正在用的那个版本"** —— 对每一个从旧客户端升上来的 home
都成立，还有所有手写的 platform-conditional 条目（`{linux = ..., default = ...}`）。

修法是把同一条不变式补到读侧，让 round trip 成为不动点。**这正是 §P5 那条规则的
另一半**：改一个判据之前，先看它读的数据是怎么来的 —— 差点就把一条兼容性断路当成
新功能发出去了。

（实现细节：这个 helper 必须写成函数体内的 lambda。GCC 16 在"模块类型上的 std 容器
第一次实例化发生在**命名空间作用域**的 helper 里"时会 ICE，而崩掉的 cc1plus 留下一个
截断的 BMI，报错报在**别的**翻译单元上：`Bad file data`。）

对应单测 6 组（`XvmSubosWorkspaceJsonTest.*`），含一条"只有 `installed[]` 没有
`active` 的条目不许凭空长出 active"的反向断言，和一条 round-trip 不动点。

### 已验证的对照

`xlings install gcc -y` 在同一个全新 subos 里 → `usr/include` 140 项 → 编译通过。
**install 路径没有这个问题**，因为它解析并安装依赖。

### 验收 → E2E-51 S5 / S6

- S5：全新 subos `use sem-lib 1.0.0` → 非零退出；提示里同时出现 subos 名和
  `install`；**没有**生成 shim，subos 的 workspace 里**没有**这个包的记录；
- S6：同一 subos `install` 之后再 `use` → 退出 0，切换生效；且 `default` subos
  自始至终没被影响。
- 差分：S5 在 `2026.7.31.2` 上失败（它切换了）。

实现落在 `cmd_use` 里 `resolved` 算完、`plan_use_switch` 之前 —— 即**所有文件系统
动作之前**，判据是现成的 `filter_to_subos_installed_`。auto-add 没有整个删掉：
release 的**成员**仍然照旧补进 `installed[]`（新版本多出一个程序时，它是随用户要过的
release 一起到的，不是替它到的）。

---

## P3. `use <pkg>` 列出版本却打印 `[error]`

### 现场

```
$ xlings use gcc
  ◆ gcc versions (current subos)
  ──────────────────────────────────────────
  ▸ 15.1.0               @xlings/data/xpkgs/xim-x-gcc/15.1.0/bin
    15.1.0-aarch64-musl  …
    …
[error] 'gcc' has 5 installed versions (currently active: 15.1.0); name the one you want
[error]   xlings use gcc <version> (versions: 15.1.0 15.1.0-aarch64-musl 15.1.0-musl 16.1.0 16.1.0-musl) | xlings use gcc --pick to choose interactively
```

两个问题，都是我在 `2026.7.31.2` 引入的：

1. **语义**：用户没做错任何事。"没指定版本"不是错误，是**信息不足**。打 `[error]`
   把一次正常的查询渲染成了故障。
2. **冗余**：版本列表在上面的面板里已经完整列过一遍，`[error]` 行又列了一遍；
   `--pick` 提示在非终端环境毫无意义。

### 目标

**退出码也一起改成 0** —— 因为它本来就不是错误。原方案写"退出码不变(2)"是错的：
我把"什么都没做就必须非零"这条契约**套用过头了**。那条契约管的是**一个动作失败或
空转**；而 `use <pkg>` 不带版本时是一次**查询**，它完整地给出了答案 —— 没有失败可言。

真正被那条契约禁止的，是**旧行为**：只有一个候选版本、明明可以切换，却打印列表并
返回 0。那一条已经修好了（一个候选 → 直接切换）。

```
$ xlings use gcc
  ◆ gcc versions (current subos)
  ──────────────────────────────────────────
  ▸ 15.1.0               @xlings/…/15.1.0/bin
    16.1.0               @xlings/…/16.1.0/bin
    …
  ▸ tip  xlings use gcc <version>
$ echo $?
0
```

具体：

| | 现在 | 改为 |
|---|---|---|
| 渲染 | `ErrorEvent` → `[error]` ×2 行 | 一行 `tip`，与面板同一宽度契约 |
| 版本列表 | 面板 + 提示行各一遍 | **只在面板里** |
| `--pick` | 有这个 flag | **整个功能删掉**（见下） |
| 退出码 | 2 | **0** |
| 机器通道 | `ErrorEvent{InvalidInput}` | 不发 —— 这不是错误 |

### 顺带：删掉 `--pick`

`--pick`（交互式上下键选择）是我在 `2026.7.31.2` 加的，用来给"被拿掉的 picker"留一个
显式出口。既然默认路径已经确定（一个候选就切、多个就列），这个 flag 只是多一条要维护
、要测试、要文档化的路径，而它解决的问题已经不存在。删掉。

### 需要一并调整的既有契约

**E2E-48 的 N2/N3/N4 断言退出码 2，要改。** 那三条锁的是"歧义时不阻塞、不静默切换"，
这两点仍然成立 —— 变的只是"不是错误"。已改为断言：**不阻塞、退出 0、状态未变、
候选已列出、输出里没有 `[error]`、版本列表只出现一次**。N4 从"`--pick` 无终端时报错"
改成"`--pick` 必须被当作未知 flag 拒绝" —— 一个被静默忽略的 flag 比一个不存在的更糟，
脚本会以为自己要到了什么。

实现上，">1 候选" 这一支现在**就是** `cmd_list_versions`（同一个面板、同一条 tip、
退出 0）——`use` 不再自己拼一份措辞，"列版本"只有一个实现。

**代价要说清楚**：脚本无法再用退出码区分"切换了"和"列出了"（两者都是 0）。
这是可以接受的，因为**表达切换意图的方式是给出版本号** —— `xlings use gcc 16.1.0`
成功即 0、失败非零，语义无歧义。不给版本号本来就不是在表达切换。

- 0 个候选仍然是真错误（退出 1，`[error]` 保留）；
- **"什么都没做就必须非零"这条仍然适用于真正的动作**（`install` / `remove` / 带版本的
  `use`），只是不适用于查询。

---

## P4. 测试里的"假 PASS"

不是产品缺陷，是我的方法问题，写在这里因为它会重犯。

```bash
# 断言：-print-sysroot 应指向 probe
[[ "$out" == *"/subos/probe"* ]] && ok
# 实际 out：env: '…/subos/probe/bin/g++': No such file or directory
#                      ^^^^^^^^^^^^ 错误信息里也含这个串 → 假 PASS
```

**子串断言在错误信息里同样成立。** 这与本轮追的"没发生和成功了输出一致"是同一形状，
只不过发生在测试自己身上。

规则：

1. **断言成功时，先断言命令成功**（退出码/无 stderr），再断言内容；
2. **子串断言要选不会出现在失败输出里的串** —— `-print-sysroot` 的正确输出是
   *整行等于*那个路径，用 `==` 而不是 `*…*`；
3. **每条新断言都要在"应当失败"的构建上跑一次** —— 本轮其他测试都做了，这一条漏了。

---

## P5. 方法：提议新增能力之前，先 grep 这个能力的名字

本轮我**两次**提议新建一个已经存在的东西：

| 我提议的 | 实际已有 | 应该做的检查 |
|---|---|---|
| "recipe 需要一个可移植的 subos 写法" | `subos/current`（`self init` 建、`subos use --global` 维护） | `grep -rn '"current"' src/` |
| "索引侧不支持模糊约束，要实现 range" | `semver.cppm` 完整 range + 28 单测 + `catalog.cppm` 已在用 | `grep -rn "select_best\|parse_range" src/` |

两次的形状一样：**从一个真现象出发，直接跳到"需要造一个新机制"，没有先问"这个机制
是不是已经有了、只是没接上"。**

代价不只是白写代码 —— 它会把方案的形状带偏：P1 因此被写成"要实现 range 求解"，
边界里还专门写了"不实现区间运算符"，而 `^`/`~`/`>=` 早就在那儿。

规则：**提议新增任何能力之前，用它的领域词汇 grep 一遍**（`range`、`satisfies`、
`current`、`marker`…）。这比读架构便宜得多，而且这轮两次都能立刻命中。

与 §P4 是同一类：**先量、先查，再动结论。**

---

## 落地顺序

```
P1 (deps 满足)   ─┐  互不依赖
P2 (use 提示)    ─┤  可并行
P3 (use 呈现)    ─┘
P4              —— 写进测试约定，无代码
```

P3 最小（纯呈现 + 删 `--pick`），P2 次之（一个判据 + 一段提示 + 不动作），
P1 也不大（一个判据 + 一次改写 + 一个回调），**不需要新建版本语义**。

### 实际改了什么（`2026.7.31.3`）

| 文件 | 改动 |
|---|---|
| `src/core/semver.cppm` | `+satisfies_expr()` —— 单版本满足性，空约束恒真，非 semver 版本串按分量边界退化 |
| `src/core/xim/resolver.cppm` | `+pin_target_to_active()`；`resolve()` 接受 `activeOf` 回调，`expand()` 和 topo 同步改写；**删掉 IndexManager 重载**（第二条版本解析路径） |
| `src/core/xim/commands.cppm` | `pin_to_active_if_satisfies_` 改为调用共用判据；把回调传进 `resolve()` |
| `src/core/xvm/commands.cppm` | `cmd_use` 新增"本 subos 未安装则拒绝"闸门；`cmd_use_by_name` 的多候选支改为委派 `cmd_list_versions`；删 `pick` 形参 |
| `src/core/xvm/db.cppm` | **读侧补上写侧一直有的不变式**：`subos_workspace_from_json` 把 `active` 并入 `installed[]`。见下 |
| `src/cli.cppm` | 删 `--pick` / `-i` 及其 help 行、`select_version` prompt 分派 |
| `src/ui/selector.cppm` | 删 `select_version()`（已无发射方） |
| `tools/linux_release.sh` | d2x 校验找的是 `xpkgs/d2x`，而 payload 在 `xim-x-d2x` —— 这条**从命名空间化布局起就一直在失败**，只是每个 CI 调用方都设了 `SKIP_NETWORK_VERIFY=1`，唯一能发现它的检查从来没跑过 |

---

## 不做什么

- **不在版本库里记依赖关系** —— P2 用现成的 `installed[]` 就够；记依赖是更大的改动，
  应当由真正需要它的需求驱动（例如"`use` 自动激活依赖"），而不是为一条警告；
- **不新增版本语法** —— `semver.cppm` 已经有前缀 / `^` / `~` / `*` / 区间，还有 28 个单测；
- **不在 `use` 里自动安装** —— 提示即可，安装是 `install` 的事；
- **不保留 `--pick`** —— 它解决的问题已经不存在。
