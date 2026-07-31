# install / use 语义优化方案

**日期**: 2026-07-31
**类型**: 计划（plan）
**基线**: `2026.7.31.2`（已发布）
**相关**: [#459](https://github.com/openxlings/xlings/issues/459)、`.agents/docs/2026-07-31-xvm-subos-architecture-review.md`

四项，互不依赖，可任意顺序落地。共同点是同一族：**命令报告的东西和它实际做的
不一致** —— 有的报成功而产物半残，有的没做错事却报 error。

| # | 问题 | 类型 | 规模 |
|---|---|---|---|
| **P1** | 依赖已满足仍装最新版 | **真缺陷** | 中 |
| **P2** | `use` 在未安装该包的 subos 里静默给出半可用工具链 | **真缺陷**（静默成功） | 小 |
| **P3** | `use <pkg>` 列版本却打印 `[error]` | 语义/表达 | 小 |
| **P4** | 测试里的"假 PASS" | 方法问题 | — |

---

## P1. 依赖已满足仍装最新版

### 现场

```
Packages to install (2):
    ◆ xim:mcpp@2026.7.30.3
    ◆ xim:mcpp-short-cmd@0.0.1
▸ Proceed? n

$ mcpp --version
mcpp 2026.7.30.2          ← 本地已有，且 mcpp-short-cmd 的 deps 没有指定版本
```

### 成因（`src/core/xim/resolver.cppm:134-150`）

```cpp
if (!versionHint.empty()) {
    node.version = versionHint;          // deps 写了 @X → 用 X
} else {
    // 没写版本 → 找 "latest" ref，或取最高版本
    node.version = latestIt->second.ref; // ← 完全不看本地装了什么
}
```

**"没有指定版本"被实现成了"要最新版本"，而它应该是"任意满足的版本都行"。**

索引里 `deps` 的实际写法只有两种（全索引统计）：

| 形态 | 例 | 出现 |
|---|---|---|
| 锁定精确版本 | `xim:glibc@2.39` | 多数 |
| 不写版本 | `xim:mcpp`、`xim:node`、`xim:npm` | 少数 |

**没有版本范围语法。** 所以本方案不需要实现区间求解 —— 只需要把"不写版本"这一档
改对。

### 目标语义

| deps 写法 | 当前 | 应当 |
|---|---|---|
| `xim:pkg@X` | 装 X | 不变（精确锁定就是锁定） |
| **`xim:pkg`** | **装 latest** | **本 subos 已装任意版本 → 用它；否则装 latest** |

"用它"的优先级：**本 subos 的活动版本** > 本 subos `installed[]` 里的最高版本。

### 为什么这是缺陷而不是偏好

1. **它让"装一个小工具"顺手升级了工具链。** 用户装 `mcpp-short-cmd`，得到的是
   mcpp 被换掉 —— 而他没有要求过。
2. **它与 `install` 的幂等承诺相悖**：同一条命令在两天里给出不同结果，仅仅因为
   索引动了。
3. **它把可选的升级变成了强制的**：`xlings install mcpp@latest` 才是升级的表达式。

### 边界（不做的）

- **不实现版本范围**（`>=`/`^`）—— 索引里没有，加了就要维护求解器；
- **不改精确锁定的语义** —— `@2.39` 就是 2.39；
- **不自动降级** —— 已装版本高于 latest 时保持已装，不回退。

### 验收

- 本地有 `mcpp@A`，装一个 `deps = {"xim:mcpp"}` 的包 → 计划里**不出现** mcpp；
- 本地无 mcpp → 计划里出现 `mcpp@latest`；
- `deps = {"xim:glibc@2.39"}` 且本地是 2.38 → 仍然装 2.39（锁定不受影响）；
- 差分测试：在 `2026.7.31.2` 构建上第一条必须失败。

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
| **不在**（auto-add 路径） | 仍然切换，但**明确提示**：本 subos 未安装过它，依赖可能不完整，建议 `xlings install <pkg>` |

```
[xlings] gcc -> 16.1.0
[warn] gcc 未在本 subos (probe) 安装过 —— 这次是从全局版本库直接激活的
       它的依赖不会被一起激活，工具链可能不完整
       补齐: xlings install gcc
```

**保持"仍然切换"**：auto-add 本身是有用的（对无依赖包完全正确），把它改成拒绝会
打断今天能用的用法。这里要修的是**静默**，不是行为。

### 已验证的对照

`xlings install gcc -y` 在同一个全新 subos 里 → `usr/include` 140 项 → 编译通过。
**install 路径没有这个问题**，因为它解析并安装依赖。

### 验收

- 全新 subos `use gcc` → 切换成功 **且** 出现该警告；
- 同一 subos `install gcc` 之后再 `use gcc` → **无**警告；
- 差分：`2026.7.31.2` 构建上无警告。

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

**退出码不变（2），只改呈现。** 这正是本轮定下的契约：*TTY 决定呈现，不决定语义*
—— 而我把呈现做错了。

```
$ xlings use gcc
  ◆ gcc versions (current subos)
  ──────────────────────────────────────────
  ▸ 15.1.0               @xlings/…/15.1.0/bin
    16.1.0               @xlings/…/16.1.0/bin
    …
  ▸ tip  xlings use gcc <version>
$ echo $?
2
```

具体：

| | 现在 | 改为 |
|---|---|---|
| 渲染 | `ErrorEvent` → `[error]` ×2 行 | 一行 `tip`，与面板同一宽度契约 |
| 版本列表 | 面板 + 提示行各一遍 | **只在面板里** |
| `--pick` | 总是提 | **仅当有终端时**提 |
| 退出码 | 2 | **2（不变）** |
| 机器通道 | `ErrorEvent{InvalidInput}` | 由退出码承担；`interface.cppm` 对非零退出且无 ErrorEvent 的情况已有合成路径 |

### 边界

- **不要因为"看起来不像错"就改成退出 0** —— 什么都没做就必须非零，这是 E2E-48
  锁住的契约；
- 0 个候选仍然是真错误（退出 1，`[error]` 保留）。

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

## 落地顺序

```
P1 (deps 满足)   ─┐  互不依赖
P2 (use 提示)    ─┤  可并行
P3 (use 呈现)    ─┘
P4              —— 写进测试约定，无代码
```

P3 最小（纯呈现），P2 次之（一个判据 + 一段提示），P1 最大（要在 resolver 里接入
"本 subos 已装什么"）。

---

## 不做什么

- **不在版本库里记依赖关系** —— P2 用现成的 `installed[]` 就够；记依赖是更大的改动，
  应当由真正需要它的需求驱动（例如"`use` 自动激活依赖"），而不是为一条警告；
- **不实现版本范围求解** —— 索引里不存在这种写法；
- **不把 `use` 的歧义退出码改回 0** —— 什么都没做就得非零；
- **不动 auto-add 语义本身** —— 它对无依赖包是对的，P2 修的是静默。
