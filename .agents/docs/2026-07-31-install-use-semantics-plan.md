# install / use 语义优化方案

**日期**: 2026-07-31
**类型**: 计划（plan）
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

问题在于**有两条版本解析路径，只有一条接了它**：

| 路径 | 怎么解析版本 | 支持 range |
|---|---|:---:|
| **catalog**（顶层目标） | `select_version_()` → `semver::select_best` | ✅ |
| **resolver**（依赖展开，`resolver.cppm:83` / `:134`） | `if (!versionHint.empty()) return versionHint;` **原样当版本号用** | ❌ |

```cpp
// resolver.cppm:83   算 key 时
if (!versionHint.empty()) return versionHint;
// resolver.cppm:134  定 node.version 时
if (!versionHint.empty()) node.version = versionHint;
```

于是 `deps = {"xim:python@3"}` 带着字面量 `"3"` 一路走到 libxpkg 的精确
`versions.find("3")` → `unknown version: 3`。

顺带一个佐证：`semver::select_highest` **零调用者** —— 这个能力写好了，接了一半。

**这也解释了为什么索引里 `deps` 几乎全是 `@2.39` 这种锁死写法** —— 不是作者想锁，
是写 `@3` 会坏。修完之后 recipe 才有理由改用宽松约束。

### 两个缺陷

**P1-a 依赖展开绕开了版本语义。** 修法不是"实现 range"（已经有了），而是让
resolver 的两处走 catalog 已有的解析（`select_version_` / `semver::select_best`），
两条路径口径合一。

**P1-b 无约束时直接取 latest，不看本地已装什么**（`resolver.cppm:134` 的 `else` 分支）：

**"没有指定版本"被实现成了"要最新版本"，而它的意思是"任意满足的版本都行"。**

### 目标语义

| deps 写法 | 现在 | 应当 |
|---|---|---|
| `xim:pkg@2.39` | 装 2.39 | 不变（精确锁定就是锁定） |
| **`xim:pkg@3`** | ❌ `unknown version: 3` | 已装满足 `3` 的 → **用它**；否则装索引里满足 `3` 的最高版 |
| **`xim:pkg@^1.2`** | ❌ 同上 | 同上（`semver` 已支持这些运算符） |
| **`xim:pkg`** | 装 latest | 已装**任意**版本 → **用它**；否则 latest |

"用它"的优先级：**本 subos 的活动版本** > `installed[]` 里满足约束的最高版本。

满足性判断用 `semver::satisfies` / `select_best`（已有），**不新建任何版本语义**。

### 为什么 P1-b 是缺陷而不是偏好

1. **装个小工具顺手升级了工具链** —— 用户装 `mcpp-short-cmd`，mcpp 被换掉，他没要求过；
2. **同一条命令两天给出不同结果**，仅因为索引动了 —— 与 `install` 的幂等承诺相悖；
3. **把可选升级变成强制** —— `xlings install mcpp@latest` 才是升级的表达式。

### 边界

- **不新增版本语法** —— `semver.cppm` 已覆盖前缀、`^`、`~`、`*`、区间；
- **不自动降级** —— 已装版本高于约束上界时保持已装，不回退；
- **不改精确锁定的语义**。

### 验收

- 本地有 `mcpp@A`，装 `deps = {"xim:mcpp"}` 的包 → 计划里**不出现** mcpp；
- 本地有 `python@3.11`，`deps = {"xim:python@3"}` → **不出现** python；
- 本地无 python，`deps = {"xim:python@3"}` → 出现索引里满足 `3` 的最高版（不是 `unknown version`）；
- `deps = {"xim:glibc@2.39"}` 且本地 2.38 → 仍装 2.39；
- 差分：以上前三条在 `2026.7.31.2` 构建上必须失败。

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

### 已验证的对照

`xlings install gcc -y` 在同一个全新 subos 里 → `usr/include` 140 项 → 编译通过。
**install 路径没有这个问题**，因为它解析并安装依赖。

### 验收

- 全新 subos `use gcc 16.1.0` → **不切换**，提示去 install，workspace 未变；
- 同一 subos `install gcc` 之后再 `use gcc 16.1.0` → 正常切换，无提示；
- 差分：`2026.7.31.2` 构建上会切换并静默 —— 新断言在其上必须失败。

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
这两点仍然成立 —— 变的只是"不是错误"。改为断言：**不阻塞、退出 0、状态未变、
候选已列出**。

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
P1 也不大（resolver 两处改调已有的解析 + 接入"本 subos 已装什么"），**不需要新建
版本语义**。

---

## 不做什么

- **不在版本库里记依赖关系** —— P2 用现成的 `installed[]` 就够；记依赖是更大的改动，
  应当由真正需要它的需求驱动（例如"`use` 自动激活依赖"），而不是为一条警告；
- **不新增版本语法** —— `semver.cppm` 已经有前缀 / `^` / `~` / `*` / 区间，还有 28 个单测；
- **不在 `use` 里自动安装** —— 提示即可，安装是 `install` 的事；
- **不保留 `--pick`** —— 它解决的问题已经不存在。
