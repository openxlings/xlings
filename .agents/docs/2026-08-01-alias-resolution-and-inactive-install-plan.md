# alias 解析口径 与「已安装但无 active 版本」优化方案

**日期**: 2026-08-01
**类型**: 计划（plan）→ **已实现，随 `2026.8.1.1` 发布**
**基线**: `2026.7.31.3`（已发布）
**代码**: `src/core/xvm/shim.cppm`、`src/core/xself/doctor.cppm`、
`src/core/xvm/registration.cppm`、`src/core/xim/commands.cppm`
**相关**: `.agents/docs/2026-07-29-orphan-shim-inactive-group-root-design.md`、
`.agents/docs/2026-07-29-doctor-fix-one-shot-design.md`、
[#452](https://github.com/openxlings/xlings/issues/452)

两项，互不依赖。都来自同一次真实现场：一台用了几个月的 home 上
`xlings self doctor --fix` 刷出 34 条 `alias unresolved`，而与此同时
`codex` 报 `/usr/bin/env: 'node': No such file or directory` —— doctor 一条都
没提。

| # | 问题 | 类型 | 规模 |
|---|---|---|---|
| **P1** | doctor 的 alias 解析口径比运行时窄两级，34 条全是误报 | **误报**（掩盖真断裂） | 小 |
| **P2** | 包已安装、payload 完好、却无 active 版本、无 shim，三个出口全静默 | **真缺陷**（静默成功） | 中 |

两者是一对：P1 制造噪音，P2 制造沉默，合起来的效果是
**doctor 报了 34 条不该报的，同时漏了唯一该报的那条。**

---

## 现场（真实 home，`2026.7.31.3`）

```
$ xlings self doctor --fix
  ⚠ alias unresolved  madd@0.0.1 alias 'mcpp' not resolvable in
                      @xlings/data/xpkgs/xim-x-mcpp-short-cmd/0.0.1
                      (may be a system command)
  … 共 34 条 …
  warnings            34

$ codex
/usr/bin/env: 'node': No such file or directory
```

实测两条事实，后面所有推理都建立在它们上：

```
$ madd --help          → exit 0，正常打印 mcpp add 用法   ← 34 条警告全是误报
$ which node npx npm   → node (none) / npx (none) / npm 有 ← doctor 一个字没说
```

---

## P1. doctor 的 alias 解析口径比运行时窄两级

### 成因

运行时执行 alias 的实际路径（`shim.cppm:425-501`）先拼 PATH 再交给 shell：

```cpp
// shim.cppm:427-447
if (!expanded_path.empty() && exists(expanded_path)) new_path = expanded_path;   // ① payload
if (exists(bin_path))            new_path += … + bin_path;                        // ② payload/bin
if (!cfg_bin.empty())            new_path += … + cfg_bin;                         // ③ subos binDir
if (!existing_path.empty())      new_path += … + existing_path;                   // ④ 继承的 PATH
platform::set_env_variable("PATH", new_path);
…
return platform::exec(cmd);
```

doctor 的判定（`doctor.cppm:614-619`）只走了 `resolve_executable`：

```cpp
const auto aliasProg = alias_program_(vdata.alias[0], st.homeStr);
if (fs::path(aliasProg).is_absolute()) continue;
if (!xvm::resolve_executable(aliasProg, vdata.path, st.homeStr).empty()) continue;
```

而 `resolve_executable`（`shim.cppm:243-269`）只查 `path/prog` 与
`path/bin/prog` —— **只覆盖 ①②，完全没有 ③④。**

`xim-x-mcpp-short-cmd/0.0.1/` 里没有任何 payload：

```
.xim-installed  .xpkg-install.json  .xpkg.lua      ← 只有元数据
```

这个包的全部职责就是注册 30 个短命令，全部转发给**另一个包**里的 `mcpp`。
运行时在 ③ subos binDir 里一击命中，doctor 因为不看 ③ 而必然报错。
`musl-gcc-static → musl-gcc`、`gcc-specs-config → xlings`、
`xpkg-helper → xlings` 同理。

`doctor.cppm:608-613` 的 TODO 已经承认判据不足，并据此把等级压到 warning。
压等级缓解的是后果，没有修口径 —— 代价是**真断裂和误报混在同一个等级里，
34 条噪音之下第 35 条没人看得见**。

### 目标：让 doctor 问的问题和运行时做的事逐级对齐

新增一个返回「在哪一级命中」的解析函数，放进 `xvm/shim.cppm` ——
和运行时同源，两边不会漂移（沿用 `doctor.cppm:534-537` 已确立的
"Detection calls the function the repair calls" 原则）：

```cpp
// xvm/shim.cppm
enum class AliasOrigin { Payload, SubosBin, SystemPath, Nowhere };
struct AliasResolution { AliasOrigin origin; std::filesystem::path path; };

AliasResolution resolve_alias_program(const std::string& prog,
                                      const std::string& payloadPath,
                                      const std::string& xlingsHome);
```

查找顺序严格复刻 ①②③④，Windows 下 ③ 要带 `shim_ext_`（`.exe`）重试。

doctor 按命中级别三分：

| 命中位置 | doctor 判定 | 理由 |
|---|---|---|
| ① payload / ② payload/bin | **静默** | 今天的 `continue`，不变 |
| ③ subos binDir | **静默** | 兄弟 shim，xlings 自己管得到 —— `madd → mcpp` 就在这一级 |
| ④ 系统 PATH | **notice** | 能用，但依赖宿主机；是可移植性信号，不是缺陷 |
| 哪都没有 | **error** | alias 真的断了，今天被压成 warning 埋掉了 |

预期效果：现场 34 条 → **0**。

### 两个必须写进实现的边界

**1 — ④ 的判定依赖 doctor 自己的 PATH。**
doctor 进程的 PATH 不等于用户登录 shell 的 PATH。在 CI / 剥离环境里，一个
alias 到 `bash` 的包可能落到「哪都没有」而误报 error。所以 error 分支必须
带一句环境说明，且 detail 里打印实际查过的四个位置。

**2 — 「哪都没有」要先问是不是 P2。**
如果 alias 目标本身是一个已知的 xvm program target、只是**没有 active 版本**，
那它不是「alias 断了」，而是 P2 那条更精确的发现。此时不报
`alias unresolved`，改报 P2 的 `no active version`，remedy 指向真正的修法。
—— 这是 P1 和 P2 唯一的耦合点，也是把两件事放进一份文档的原因。

### 需要一并调整的既有契约

`tests/e2e/self_doctor_test.sh` **S10** 现在删掉 payload 里的 alias 目标后断言
warning + `exit 0` + `--fix 不得触碰`：

```bash
# self_doctor_test.sh:376-381
echo "$out" | grep -q "alias unresolved" || fail …
[[ $rc -eq 0 ]] || fail "S10: alias warning alone should exit 0; got $rc"
```

新口径下 `alias-real` 在四级里都找不到 → **error + exit 1**。S10 必须改成断言
error，并新增两条 fixture 覆盖 ③ 和 ④：

- **S10-a** alias 目标只存在于 subos binDir → 静默、exit 0
- **S10-b** alias 目标只存在于系统 PATH → notice、exit 0
- **S10-c** 四级皆无 → error、exit 1（原 S10 改写）

`.agents/tools/doctor-acceptance.sh:75-76` 数的是
`alias unresolved.*claude` 的条数，新口径下 before/after 都会归零，断言要相应
放宽或改数 notice。

---

## P2. 已安装、payload 完好、无 active 版本 —— 三个出口全静默

### 现场

```
subos/default/.xlings.json:
  node  {"installed": ["23.6.0", "24.15.0"]}                        ← 无 active
  npx   {"installed": ["node-23.6.0", "node-24.15.0"]}              ← 无 active
  npm   {"active": "11.2.0", "installed": ["11.2.0", "node-…"]}     ← 有（来自 xim-x-npm）
```

payload 完好（`xim-x-node/24.15.0/bin/node`，122 MB，在），但没有 active 就
不生成 shim，`which node` 为空。用户最终看到的是**第三方工具**抛出的
`/usr/bin/env: 'node': No such file or directory` —— 错误信息里出现的是
`node` 和 `env`，从头到尾没有 xlings 三个字。

**node 与 npx 同时缺、npm 独存**，这是判定成因的决定性证据（见下）。

### 成因 A（install 侧，根因）

`registration.cppm:921-925`：

```cpp
const bool anyMemberActive = std::ranges::any_of(
    group.members, [&](const auto& member) {
        return candidateWorkspace.contains(member.first);   // ← 只问"这个名字被占了吗"
    });
const bool activateGroup = batch.useAfterInstall || !anyMemberActive;
```

node 组的成员是 `{node, npm, npx}`。`npm` 被**另一个包** `xim-x-npm@11.2.0`
占着 → `anyMemberActive == true` → `activateGroup == false` →
**整组都不激活**，包括没有任何人竞争的 `node` 和 `npx`。

`registration.cppm:918-920` 写明的意图是：

> Leaving a name alone when something already owns it is also the right answer
> across providers: installing gcc must not silently take `cc` away from an
> active llvm.

意图是**不要抢别人的名字**，实现却升级成了**一个被占的名字否决整组**，
连没被占的名字一起不激活。结果是刚装好的 node 整个不可达。
node/npx 缺、npm 在，正是"整组否决、npm 因另有其主而幸存"的指纹。

### 成因 B（remove 侧）

`removal.cppm:346-349` 删掉 active 指针后，`385+` 的 group-coherent 复活只肯
整组搬迁，否则维持不激活，理由写在 `361-366`：

> Otherwise the group stays inactive and the user re-selects explicitly —
> **an inactive toolchain is a visible problem**, an incoherent one is not.

两条路径殊途同归，且都押在同一个假设上。

### 这个假设被本次现场证伪了

「inactive 是可见问题」在今天的三个出口上**一个都不成立**：

| 出口 | 代码 | 对 node 说了什么 |
|---|---|---|
| `xlings list` | `commands.cppm:852-866` 只读 `workspace_installed()`，**从不查 active** | `◆ xim:node@24.15.0` —— 和正常包一模一样 |
| `xlings self doctor` | Check 1 遍历 `st.ws`（active 表），无 active 的名字压根不在表里 → 跳过 | 一个字没说 |
| shim | 没有 active 就不写 shim，binDir 里没有文件 | Check 2 遍历 binDir 也看不到 |

Check 3 遍历 DB，node@24.15.0 是 program、path 有效、`node` 可执行文件解析成功
→ `continue`，同样静默。**三个检查全过，问题就在中间那条缝里。**

而 doctor 已经把判定所需的数据加载进内存了 —— `DoctorState::wsInstalled`
（`doctor.cppm:150`）就是 installed 表，只是从来没和 `st.ws` 交叉比对过。

### 目标 A：新增 Check —— `InactiveInstalled`

```cpp
// FindingKind 新增
// 该 subos 装了这个包、payload 在、却没有任何 active 版本，因此没有 shim。
// list 会把它显示成正常安装，doctor 的其余检查全部够不到它：Check 1 遍历
// active 表，这个名字不在表里；Check 2 遍历 binDir，没有文件可看。
InactiveInstalled,
```

判定：对 `st.wsInstalled` 中每个 installed 非空的 name，
若该 name 有 program kind、且 `st.ws` 中无 active（或为空）→ 报 Error。

必须排除的三类（否则会把正常状态报成缺陷）：

1. **binding root / release anchor** —— `is_binding_root()` 为真，或
   `payload_has_any_executable_()` 为假。库包本来就不该有 active program，
   这正是 #452 的教训。
2. **该 name 的所有已安装版本都不是 program kind**（复用 Check 3 的
   `effective_kind` 逐版本判定）。
3. **归属其它 subos 的条目** —— 复用 `xvm::subos_ownership()`，避免在
   subos A 里把 subos B 的选择报成缺陷（`doctor.cppm:465-481` 已有先例）。

detail 里要同时打印 installed 列表和"无 shim"这个后果，因为后者才是用户
真正撞上的现象：

```
✗ no active version  node — installed 23.6.0, 24.15.0; none active, so no
                     shim exists in this subos (`node` is not on PATH)
                     fix: xlings use node@24.15.0
```

### 目标 B：`--fix` 自动激活最高已安装版本

`--fix` 直接把 active 指向最高的已安装版本（沿用 `removal.cppm:390-393` 的
`version_key_greater` 排序，而不是安装先后）。

这与 `removal.cppm:361-366` 确立的「不替用户猜版本」原则**存在张力，必须正面
处理**：那条原则的前提是"inactive 可见"，前提已被证伪；在用户看不见的前提下
拒绝修复，等于把包永久留在不可达状态。`--fix` 是用户显式请求修复的入口，
在这里做出选择是它的职责。

但自动激活**必须是组一致的**，否则会重新引入 binding-group 模型专门要防的
混合工具链。约束三条：

1. **不抢已被占用的名字。** 任何已有 active 的 name，`--fix` 一律不碰。
   node 场景下 `npm` 留在 `xim-x-npm@11.2.0`，只激活 `node` 和 `npx`。
2. **同 provider 的竞争 → 整组不动，只报告。** 若组内有 name 被
   **同一 provider 的另一个版本**占着，那是 release 升级决策，只有用户能做
   （对应 `test_xvm_bindings.cpp:3140-3152` 钉住的 gcc 15→16 场景）。
3. **虚拟 root 不单独激活。** 若组内可激活的成员全是无可执行文件的
   binding root，则整组不动 —— 否则 #452 的 orphan shim 原样回来
   （`test_xvm_bindings.cpp:3166` 钉住这条）。

### 目标 C（根因）：install 侧改成"被占的名字不否决兄弟"

`registration.cppm:921-925` 改为三段式判定：

```
1. 找出组内所有已被占用的成员名，按"当前占用者的 provider"分类
   （占用者 provider = db[name].versions[activeVersion].bindingGroup.provider）
2. 若存在【同 provider】的竞争 → 整组不激活（今天的行为，保留）
3. 否则 → 激活所有【未被占用】的成员；被占用的留给原主
   3a. 若可激活的成员全是无可执行文件的 binding root → 整组不激活
```

**遗留条目的 provider 未知**（如 `node-23.6.0` 只有 `path`，无
`bindingGroup`）→ 保守按【同 provider】处理，即维持今天的否决行为，
避免老 home 出现行为回归。

### 四个已知场景的逐条对照

这套规则必须在改之前先过一遍已有的钉子，否则就是拿一个缺陷换另一个：

| 场景 | 竞争的名字 / 占用者 provider | 新规则结果 | 依据 |
|---|---|---|---|
| **装 node，npm 已被 xim-x-npm 占** | `npm` / `xim:npm`（异） | 激活 node、npx；npm 留给原主 | 修好本文现场 ✓ |
| **gcc 16 覆盖已激活的 gcc 15** | `gcc`,`g++` / `xim:gcc`（同） | 整组不激活 | `test_xvm_bindings.cpp:3148-3152` 不破 ✓ |
| **musl-gcc flavor，gcc 已被 glibc gcc 占** | `gcc` / `xim:gcc`（异）；未占用的只剩虚拟 root `xim-musl-gnu-gcc` | 3a 命中 → 整组不激活 | `test_xvm_bindings.cpp:3166` 不破、#452 不复发 ✓ |
| **装 gcc，cc/c++ 已被 llvm 占** | `cc`,`c++` / `xim:llvm`（异） | 激活 gcc、g++、cpp、gcov；cc/c++ 留给 llvm | **行为变化**：今天是整组不激活。这正是 `918-920` 注释所声明的意图 ⚠ |

第四行是唯一的行为变化，需要单独一条单测钉住，并在 release notes 里写明。

### 目标 D：`list` 标出未激活

`commands.cppm:886-891` 渲染时查一次 `Config::effective_workspace()`，
无 active 的条目加标记：

```
◆ xim:node@24.15.0  (inactive — no shim; `xlings use node@24.15.0`)
```

这条独立于 A/B/C，可以最先落地：**它是三个静默出口里唯一一个用户会主动去看
的**，改动也最小。

### 验收 → 新增 E2E `inactive_installed_test.sh`

隔离 home，遵循
`.agents/docs` 既有的 isolated-home 约定（不预置 `data/`，否则 index 变成
符号链接、差分不可证伪）：

| # | 构造 | 断言 |
|---|---|---|
| **S1** | 装一个 group 包，其中一个成员名先被另一 provider 占住 | 未被占的成员**有** active、**有** shim，能执行 |
| **S2** | 手工把某 target 的 active 抹掉、保留 installed | `doctor` 报 `no active version`、**exit 1** |
| **S3** | 承 S2 → `doctor --fix` | active 指向最高已安装版本，shim 出现，命令可执行 |
| **S4** | 承 S2 → `list` | 该条目带 `inactive` 标记 |
| **S5** | 虚拟 root（无可执行文件）无 active | **不**报 `no active version`（沿用 release anchor notice） |
| **S6** | 同 provider 竞争（gcc 15→16 形状） | 整组不激活；`--fix` **不**替用户选版本，只报告 |

S5/S6 是防回归的重点：它们钉的是"**不该**报、"**不该**自动修"，而这两类
恰恰是 A/B/C 最容易过度触发的方向。

---

## 落地顺序

四步，每步独立可发：

1. **D（list 标记）** —— 最小、无风险、直接减轻现场痛感。
2. **A（doctor 新增 Check）+ S2/S5** —— 让问题第一次可见。
3. **P1（alias 四级口径）+ S10 系列改写** —— 噪音归零，且 A 已经就位，
   「哪都没有」才能正确降级到 P2 的精确发现（P1 的边界 2 依赖 A 先落地，
   **顺序不能反**）。
4. **C（install 根因）+ B（--fix）+ S1/S3/S6** —— 改动最大、对照表最长，
   单独一个 PR，release notes 写明第四行的行为变化。

## 实现时改掉的六处（计划 → 实际）

计划是照着现场写的，实现时被测试和真实数据推翻了六处。记在这里，因为每一处
都是"看起来对的判据其实分不开两种东西"。

**1 — `is_binding_root` 不能用来排除虚拟锚点。**
计划里的排除条件写的是"binding root 或无可执行文件"。实测 `node@24.15.0`
**就是**自己那个 release 的 root（npm/npx 都 bind 到它），所以这个判据会把
本次要修的那个包第一个排除掉 —— 加上去之后 doctor 对 node 一言不发。
真正的判据只有 payload：**只问可执行文件，和 Check 3 同序**。

**2 — 虚拟锚点也有 `path`，`kind` 也可能是 `program`。**
install 侧本来打算用 `path.empty()` 认虚拟 root。实测 `xim-musl-gnu-gcc`
记的是 musl-gcc 自己的目录；#452 fixture 里那个明确"不写 bindir"的
`xim-anchor-root`，libxpkg 仍然给它填上了包的 install_dir，`kind` 也照样
默认成 `program`，`sourceName` 照样从 target 推出来 —— **和 `node` 的记录逐字段
相同**。E2E-45 直接把这条打了回来（`S1 precondition: the root was activated`）。
最终解法：registration 层按设计不碰文件系统，所以由**装完 payload 的那一层
回答**，新增 `RegistrationNode::runnable`，默认 `true`，只在跨 provider 那一
条路径上读。

**3 — `contested.empty()` 必须单独短路。**
锚点守卫写成合取项之后，一个"只有一个虚拟 root、且无人竞争"的批次也不再激活，
`VirtualGroupRetainsEmptyPayloadAndNoSelfEdge` 当场变红。守卫只该守跨 provider
这一扇新开的门，无人竞争的 release 走的还是老路。

**4 — INV-2 也得同样地认 provider（计划里没有）。**
install 侧放行之后，`self doctor` 的 Check 4 立刻把新状态判成
`xvm-active-group-incoherent` —— 即 install 造出一个 doctor 判定为坏的状态。
两边对"什么算一个 release"必须用同一条规则：`inspect.cppm` 的 INV-2 现在同样
跳过被**其它 provider** 持有的名字。同 provider 的分裂仍然报。

**5 — `--fix` 走 `use`，会带走被占的名字；办法是先说，不是不做。**
> ⚠️ **本条判断有误，已被
> `.agents/docs/2026-08-01-doctor-fix-nonconvergence-postmortem.md` 取代。**
> 披露解决了"用户不知情"，没解决"搬走名字之后原 release 失去连贯性，
> 会被同一次 `--fix` 里的 `plan_incoherent_deactivation` 立刻拆掉"。
> 结果是 `--fix` 不收敛，在真实 home 上抹掉了 `gcc`/`ld` 的 active。

计划的约束 1 是"`--fix` 不抢已被占用的名字"。实现时发现 `use` 天然是
release 级操作：`xlings use node@24.15.0` 一定会把 `npm` 一起搬走，
所以"`--fix` 做得比它自己打印的 remedy 更窄"会变成第三种行为。
最终：`--fix` 就执行打印的那条 remedy，但 finding 里**提前列出会被搬动的名字**
（`—  activating it also moves npm (11.2.0 → node-24.15.0)`）。
静默地改变用户的选择才是这一族缺陷的本体，改变本身不是。

**6 — finding 按 release 聚合（计划里是按 target）。**
真实 home 上按 target 报是 **50 行**（llvm 37、binutils 11、node 2），每行带一条
一模一样的 remedy —— 正是 broken payload 的 `groupKey` 当初要解决的掩埋问题，
而且 `node` 就埋在中间。改成按 release 聚合后是 **3 行**。

## 不做什么

- **不**让 doctor 去理解 alias 目标的 shebang。`codex` 是
  `#!/usr/bin/env node` 的 JS 脚本，本次现场的直接触发点是它 —— 但
  「解释器缺失」是包依赖问题，属于 recipe 的 `deps`，不是 doctor 的职责。
  P2 修好之后 `node` 自然在 PATH 上，这条现场就消失了。
- **不**动 `resolve_executable` 现有的两级语义。运行时的 ①② 是对的，
  P1 是在它之上**补** ③④，不是改它 —— `shim.cppm:504` 的非 alias 路径仍然
  只该看 payload。
- **不**顺手清理用户 `.bashrc` 里那条指向已删除 dev build 的 node PATH。
  那是本次现场"为什么是突然"的答案（`build/xlings/data/xim/runtimedir/…`
  已随 build 树重建而消失，它此前一直在替 xlings 兜底），属于用户环境，
  记录在此备查，不进代码。
