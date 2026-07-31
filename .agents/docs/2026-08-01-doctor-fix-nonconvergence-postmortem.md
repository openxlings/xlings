# `self doctor --fix` 不收敛 —— 2026.8.1.1 回归的复盘与修复方案

**日期**: 2026-08-01
**类型**: 回归复盘（postmortem）+ 方案（plan）
**引入版本**: `2026.8.1.1`（PR [#465](https://github.com/openxlings/xlings/pull/465)，commit `10788ce`）
**代码**: `src/core/xself/doctor.cppm`、`src/core/xvm/inspect.cppm`、
`src/core/xim/payload.cppm`
**前序**: `.agents/docs/2026-08-01-alias-resolution-and-inactive-install-plan.md`、
`.agents/docs/2026-07-29-orphan-shim-inactive-group-root-design.md`（#452）

---

## 现场

`2026.8.1.1` 发布后，在一台装了多版本工具链的真实 home 上：

```
$ xlings self doctor          → 3 条 no active version
$ xlings self doctor --fix    → 上百行 `deactivated`，结束时 4 条 no active version
$ gcc --version
[error] xlings: no active version of 'gcc' in current subos
$ ld --version
[error] xlings: no active version of 'ld' in current subos
```

**`--fix` 让 home 比之前更坏**：`gcc` / `g++` / `ld` / `xim-gnu-gcc` / `binutils`
的 active 全被抹掉，shim 还在但无版本可派发。

这是本次工作要消灭的那一族缺陷的镜像：不是"没做事却报成功"，而是
**"做了事，把事做反了，还报了个更长的清单"**。

---

## 引入点：精确到一行

`repair_inactive_`（`doctor.cppm:1467`，#465 新增）被挂在 phase 2.5：

```
phase 1    repair_state_        ← 既有
phase 2    repair_payloads_     ← 既有
phase 2.5  repair_inactive_     ← 新增：跑 `use <root>@<ver>`
           refresh()
phase 3    repair_state_        ← 既有：plan_incoherent_deactivation
```

`repair_state_` 里的 `plan_incoherent_deactivation`（`inspect.cppm:668`）会把
不连贯 release 的成员 **erase 掉 active**（`doctor.cppm:1296`
`mutableWs.erase(target)`）。

于是形成闭环：

```
repair_inactive_  →  use 激活 release A
                  →  A 里没有的名字 stranded 在 release B 上
repair_state_     →  B 不连贯 → erase 掉 B 全体成员的 active
                  →  这些名字变成"已安装、无 active"
detect_           →  正好命中新增的 InactiveInstalled 检查
下一轮 --fix      →  去激活 B → 把 A 拆散 → ……
```

一个修复要"让它可达"，另一个要"让它连贯"，**在同一次 `--fix` 调用里首尾相接**。

**为什么评审没挡住**：`repair_inactive_` 的注释论证了"`use` 会搬走别的
provider 持有的名字，所以要先披露"，但只考虑了**被搬走的那个名字**，没考虑
**被搬走之后原 release 整体失去连贯性**，也没去看同一个函数下面 200 行就有一个
专门拆除不连贯 release 的修复。E2E-52 的 S1–S6 全是单 release / 双 package 的
形状，没有一个是"多版本共存 + 跨 release 名字重叠"。

#452 的设计文档原话被引用过又被违反了：

> Every earlier fix in this area made one `--fix` converge; none checked that
> the next install stayed clean.

---

## 缺陷一：doctor 的报告口径（显示问题）

三个独立的误报，叠在同一条 finding 上。

### 1a. 把"你没选中的另一个版本"当成缺陷

```
✗ no active version  llvm@20.1.7 …29 program(s) are not on PATH
  → run              xlings use llvm@20.1.7
```

`llvm` 装了 `20.1.7` 和 `22.1.8`，用户选了 `22.1.8`。**这是正常状态**，不是缺陷。
而给出的 remedy 会把用户正在用的 llvm 降级。

`xim-gnu-gcc@15.1.0` 和 `@16.1.0` 同时被报是同一个形状：一个包的两个 release，
本来就只能激活一个，却被报成两条互相矛盾的 finding。

**漏掉的判据**：release 的 **root target 若已有 active（任意版本）**，这个
release 就是"未选中的替代品"，不是不可达的安装。

| 场景 | root | root 的 active | 应否报 |
|---|---|---|---|
| `llvm@20.1.7` | `llvm` | `22.1.8` | ✗ 不报 |
| `xim-gnu-gcc@15.1.0` | `xim-gnu-gcc` | 无 | ✓ 报（但见 1d） |
| `node@24.15.0`（#465 原始案例） | `node` | 无 | ✓ 报 |

这条规则**同时保住原始修复并消掉这批噪音**。

### 1b. 把异平台 payload 的产物算成"不在 PATH 上的程序"

被点名的 29 个是：

```
clang++.exe  clang-cl.exe  clang.exe  libiomp5md.dll  libomp.dll  …
```

实测 `data/xpkgs/xim-x-llvm/20.1.7/bin/` 里**只有 Windows 二进制**，且没有
`.xpkg-install.json` 戳 —— 正是 `payload.cppm:14-20` 记录的那个已知案例：

> The measured case: a May-era Windows llvm@20.1.7 in a Linux store, which
> registered `clang.exe` … `libomp.dll` as programs …

这些在 Linux 上**永远不可能**出现在 PATH 上。`classify_payload_platform()` /
`classify_payload_content()`（`payload.cppm:83`/`104`）已经能判定，installer
已经在用（`installer.cppm:2255`），**只有新增的这个检查没调**。

### 1c. 库被算进"program(s) not on PATH"

`libomp.dll`、`libiomp5md.dll` 出现在程序清单里，是 1b 的下游：异平台 payload
把 DLL 注册成了 `kind = program`。修好 1b 即消失；不必单独处理。

### 1d. 同一 root 的多个未激活 release 各报一条

`xim-gnu-gcc@15.1.0` 与 `@16.1.0` 都没 active 时，两条 finding 给出两条互斥的
remedy。应当**按 root 合并成一条**，把版本列出来交给用户选 —— 也就是
`xlings use xim-gnu-gcc` 那个 picker 的语义。

---

## 缺陷二：`--fix` 的 bug —— 而且**能自动识别**

我在对话里说过要让 `--fix` "退回只报告不动手"。**那个判断是错的**，理由是
"两个修复相互对冲，自动激活不安全"。事实是：

> 判断"这次激活会不会被另一个修复立刻拆掉"所需要的两个函数，
> **仓库里都已经有了**，而且都已导出。

- `plan_use_switch(db, ws, target, version)`（`switch_plan.cppm:109`）
  —— 算出激活后**整个 release 的成员映射**，且已经会报 `stranded`。
- `plan_incoherent_deactivation(db, ws)`（`inspect.cppm:668`）
  —— 就是 phase 3 那个拆除动作本身。

所以正确的做法不是退让，而是**预演**：

```
候选修复 →  把 plan_use_switch 的结果套用到 workspace 的副本上
        →  对副本跑 plan_incoherent_deactivation
        →  非空 ⇒ 这次激活会被 phase 3 立刻拆掉 ⇒ 不做，改报冲突
        →  为空 ⇒ 安全，执行
```

这是**用另一个修复自己的判据**来决定这个修复该不该动手，两边不可能再漂移 ——
和 `SubosPathBaked` 那条"detection calls the function the repair calls"
（`doctor.cppm:534-537`）是同一个原则，我在同一个文件里遵守了一次又忘了第二次。

### 预演规则对四个已知场景的判定

| 场景 | 激活后是否有 release 变不连贯 | 结果 |
|---|---|---|
| `node@24.15.0`（原始案例） | 否（`npm` 移走后旧 `xim:npm` 已不在 workspace，不参与判定） | ✓ 自动修复 |
| `binutils@2.42` | 是 —— `ar`/`nm`/`ranlib`/`strip` 会从 `llvm@22.1.8` 移走，llvm 那个 release 随即不连贯 | ✗ 不动手，报冲突 |
| `llvm@20.1.7` | 不适用（1a 已把它挡在报告之外） | — |
| `xim-gnu-gcc@16.1.0` | 是 —— `cc`/`c++` 会从 `llvm@22.1.8` 移走 | ✗ 不动手，报冲突 |

`binutils` 那一行正是用户真正需要看到的话：**两个包都提供 `ar`，只能选一个**。
比"来回拆家"和"悄悄降级"都强。

### 冲突时该说什么

```
✗ no active version  binutils@2.42 is installed in this subos but no version is
                     active — 11 program(s) are not on PATH
  ▸ conflict         activating it would take ar, nm, ranlib, strip from the
                     active xim:llvm@22.1.8, leaving that release incoherent —
                     `--fix` will not choose between them
  → run              xlings use binutils@2.42   (and accept the llvm split)
```

### 顺序也要改

即便有了预演，`repair_inactive_` 也应当**移到最后一个 `repair_state_` 之后**：
让拆除类修复先收敛到稳定状态，激活再在稳定状态上做一次。现在它夹在
phase 2.5，作用于一个还会被 phase 3 改写的 workspace。

---

## 缺陷三（新增防线）：`--fix` 必须证明自己收敛

上面两条都是具体缺陷。这一条是**为什么这类缺陷能溜过去**的答案：
`--fix` 从来没有检查过"我做完之后是不是更好了"。

加一条终局断言：`--fix` 结束时，重新检测一次，**issue 数不得高于开始时**。
高于就把这件事直接说出来：

```
▸ warning  --fix started with 3 issue(s) and ended with 4 — a repair undid
           another. Nothing further was attempted; please report this.
```

这不是修复手段，是**止损与自曝**。它会把今天这个 bug 在第一次运行时就变成一行
可见的告警，而不是上百行 `deactivated` 里的一个静默恶化。

---

## 方案汇总

| # | 改动 | 文件 | 规模 |
|---|---|---|---|
| **F1** | root target 已有 active ⇒ 不报（未选中的替代品） | `doctor.cppm` Check 2.7 | 小 |
| **F2** | 异平台 payload ⇒ 不报（调 `classify_payload_platform`） | `doctor.cppm` Check 2.7 | 小 |
| **F3** | 同 root 的多个未激活 release 合并成一条 | `doctor.cppm` Check 2.7 | 小 |
| **F4** | `repair_inactive_` 预演 + `plan_incoherent_deactivation` 判定，冲突则只报告 | `doctor.cppm` | 中 |
| **F5** | `repair_inactive_` 移到最后一个 `repair_state_` 之后 | `doctor.cppm` | 小 |
| **F6** | `--fix` 终局收敛断言 | `doctor.cppm` `cmd_doctor` | 小 |
| **F7** | 报告方与执行方共用同一个 provider 判据 | `inspect.cppm` | 小 |

F1–F3 是**显示口径**，F4–F5 是 **`--fix` 的 bug**，F6 是**防线**。三组互相独立，
可分别落地；但 F6 应当**最先落**，这样 F1–F5 的验收有一个客观的收敛判据。

### F7 —— 实现时才挖出来的真正主因

写 E2E-53 的 fixture 时才发现：**报告方和执行方对"不连贯"的定义不一致，而这个
不一致就是级联的发动机。**

`2026.8.1.1` 给 INV-2（`analyze_bindings`，doctor **报告**用）加了 provider 判据
——"被另一个 provider 持有的名字不算这个 release 掉队"。但
`plan_incoherent_deactivation`（`inspect.cppm:668`，**`--fix` 真正执行的拆除**）
**没有加**。

于是同一个 home 上：

```
xlings self doctor        →  这里没问题（INV-2 认 provider）
xlings self doctor --fix  →  把 gcc / ld 拆了（拆除方不认 provider）
```

node 那个 home 也一样：node 的 release 含 `npm`，而 `npm` 被 `xim:npm@11.2.0`
持有 → 拆除方判定 node 的 release 不连贯 → 把 `node`、`npx` 一起拆掉。**这正是
gcc/ld 消失的机制。**

修法不是给拆除方也写一份判据（那只会等下次再漂移一次），而是**把判据提成一个
函数 `detail_::held_by_another_provider_()`，两边都调它**。

F7 落地后，F4 的预演在跨 provider 场景下不再触发 —— 因为已经没有架可打了。
F4 仍然必要：它守的是**同 provider 的残留态**（一个 release 被拆了一半、root 没
active 但某个成员还 active），也就是用户机器当时的状态。

---

## 验收 → E2E-53 `doctor_fix_convergence_test.sh`

必须用**多版本共存 + 跨 release 名字重叠**的 fixture，这正是 E2E-52 缺的形状：

| # | 构造 | 断言 |
|---|---|---|
| **S1** | 一个包两个 release（`tool@1.0` / `tool@2.0`），激活 2.0 | `tool@1.0` **不被报告**（F1） |
| **S2** | 两个包争同一个名字（`shared`），各自另有独占名字 | 报冲突、**不自动激活**（F4） |
| **S3** | 承 S2 → `--fix` 跑两次 | 第二次 issue 数 **不高于**第一次（F6），且 workspace 逐字节不变 |
| **S4** | 异平台 payload（写入 PE 魔数的假 `.exe`） | **不被报告**（F2） |
| **S5** | 同 root 两个 release 都无 active | 报**一条**，列出可选版本（F3） |
| **S6** | 单一 release、无竞争（#465 的 node 形状） | `--fix` **仍然自动激活**，不得因为这次修复而退化 |

S6 是防回归的关键：F4 的预演不能把 #465 修好的那条路一并关掉。

---

## 需要一并做的事

- **修好的版本发布前，`--fix` 的这个 finding 是危险的。** 已在真实 home 上造成
  `gcc`/`ld` 不可用。发布说明里要写明恢复命令：
  `xlings use gcc <ver>` / `xlings use binutils 2.42` —— `use` 本身不 erase，
  只有 `--fix` 会。
- `.agents/docs/2026-08-01-alias-resolution-and-inactive-install-plan.md`
  的"实现时改掉的六处"第 5 条（`--fix` 走 `use`，靠披露而非回避）**判断有误**：
  披露解决了"用户不知情"，没解决"另一个修复会把它拆掉"。该条需要标注被本文档
  取代。

## 不做什么

- **不**把 `--fix` 对这类 finding 退回成只报告。那是用能力换安全，而判断安全所
  需的两个函数仓库里都已存在 —— 退让掩盖的是没去用它们。
- **不**去动 `plan_incoherent_deactivation` 的拆除语义。它是对的：不连贯的
  release 必须拆。要改的是**在制造不连贯之前就不要动手**。
- **不**顺手清理那个 Windows llvm@20.1.7 payload。它是 F2 唯一的真实素材，
  而且删不删是用户的事；doctor 认出它并闭嘴就够了。
