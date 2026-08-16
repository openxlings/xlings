# #547 与四个 PR（#549/#550/#551 已合，#552 开着）的复盘

日期：2026-08-17 · 基线：`bf9751d` + `3c6c51b`（#552 分支） · 状态：待 review

> **修订记录**
> - v1：按「覆盖不足」立论，方案 8 项，含 `subos repair --all`。
> - **v2（当前）**：review 追问「没用的 subos 为什么也要修」→ 重新测量 →
>   发现补写已有六个入口、第六个在写假话 → §2.2b / §2.3b 新增，
>   §2.6 决策点 2 重写，方案从 8 项减到 6 项。**追问推翻了第一版的前提。**
> - v2 同时纳入 #552（§6）。
> - **v3**：review 追问「第 6 处是不是创建级别的」→ 拆成 A/B 两个站点分别定性
>   （§决策点 3b：B 无可辩解，A 看起来像创建但不是，因为 `install` 根本没有
>   `--runtime` 可指定）→ 由此给出**六个写入方的统一形状**（§决策点 3c：
>   一个 `runtime_for()` + `Intent{Create,Describe}`，差别只在最后一行兜底）→
>   顺带发现第 7 个缺陷（`new --from --runtime X` 静默丢弃 X）。方案 6 项 → 7 项。

---

## 0. 四行结论

1. **#547 的现象描述有一半是错的，而真相在另一个地方。**
   `self doctor --fix` 确实会回填 `subos_info`（只回填活跃 subos），
   `xim install` 也会（`installer.cpp:1375/1382`）—— **覆盖基本不是问题。**
   问题是**内容**：六个写入方里五个走 `preserved_runtime`，
   **第六个直接写 `DEFAULT_RUNTIME` 常量**。你真机上 5 个有块的 subos 里有 2 个
   声明 `glibc@2.44` 而 sysroot 实际服务 `2.39` —— 其中一个就是 issue 里那个
   `mcpp-test`。**它不是「没有块」，它是「有一个假的块」，而今天没有任何检查看得见。**
2. **#550 把自己诊断的那个缺陷又造了一遍。** 它的核心论点是「参数没有调用者就不是功能」，
   而它给 `collect_matches_` 新加的 `forSearch` 参数，唯一的调用者没有传它。
3. **#551 有三个问题，其中一个是数据丢失。** 回滚分支 `remove_all(trash)` 删掉的正是它已经
   挪出去的文件 —— 注释说「挪了一半比原封不动更糟」，然后代码把「挪了一半」变成了「删了一半」。
   另外 `.trash-*` 会泄漏进版本命名空间（7 处代码把 `xpkgs/<pkg>/*` 的每个子目录当版本读），
   且这套逻辑只有 1 个调用点、0 个测试，而它唯一有意义的平台的 CI 在跑到任何 E2E 之前就红了。
4. **#552 方向对，但 PR 里装了三样不相干的东西**，其中 `mcpp.lock` 是一次**真实的依赖变更**
   （移除 9 个 pin、`ftxui` 从 `compat` 搬到 `mcpplibs`），藏在一个改错误文案的 PR 里。
   代码本身还有一处 CRLF 不一致，以及正文承诺的「advice 收窄」代码没做。

---

## 1. 方法：每条结论是怎么来的

本文所有断言标注来源，只有三种：

- **【实测】** 在这台机器上跑出来的，命令附在正文里，可复现。
- **【读码】** 从 `bf9751d` 的源码直接确定的，附文件:行号。
- **【推断】** 由前两者推出、但没有直接观测的。**推断不作为验收依据。**

用到的二进制：`target/x86_64-linux-gnu/4cb652ca216e49d5/bin/xlings`
（2026.8.14.1，`subos_info` 相关代码自那以后未变 —— #549/#550/#551 都没碰
`src/core/subos/` 与 `src/core/xself/init.cpp`）。

---

## 2. #547：现象、真因、以及为什么现在写不出「未知」

### 2.1 实测：`doctor --fix` 会回填，但只回填活跃的那一个

隔离 home，两个 subos 都只有 `{"workspace":{}}`：

```bash
H=/tmp/h547
mkdir -p $H/subos/{default,mcpp-test}/{bin,lib,usr} $H/data/xpkgs $H/bin
printf '{"activeSubos":"default","subos":{"default":{"dir":""}}}' > $H/.xlings.json
printf '{"workspace":{}}' > $H/subos/default/.xlings.json
printf '{"workspace":{}}' > $H/subos/mcpp-test/.xlings.json

# 直接调二进制，不要走 shim ——
# 见 reference_shim_rewrites_xlings_home：shim 会把 XLINGS_HOME 改回真 home
XLINGS_HOME=$H ./target/x86_64-linux-gnu/<fp>/bin/xlings self doctor --fix
```

【实测】结果：

```
⚠ subos runtime   subos 'default' declares runtime glibc@2.44, but its active
                  XVM runtime is <none>; declared runtime payload glibc@2.44 is missing
  → run           xlings install glibc@2.44
· subos manifest  described subos 'default' (runtime glibc@2.44)
▸ healed          1
```

```
subos/default/.xlings.json    → 写入了 subos_info（runtime glibc@2.44）
subos/mcpp-test/.xlings.json  → {"workspace":{}}   原样未动
```

所以 issue 标题里的「never backfilled」不成立，成立的是「**只对活跃 subos 生效**」。

**这一条同时暴露了第二个、更重要的缺陷**：同一次运行里，`--fix` 先写下
`runtime = glibc@2.44`，然后**在下一行警告这个声明是假的**。写入方和检查方在同一次
输出里互相矛盾。

### 2.2 为什么只覆盖活跃 subos —— 而它的「负责人」不存在

【读码】三处：

| 位置 | 内容 |
|---|---|
| `doctor.cpp:679` | `detect_subos_manifest_(st.db, st.ws, p.subosDir, ...)` —— 只传活跃 subos 目录。注释：「Other subos are not inspected from here ... a second shell may be inside one right now.」 |
| `init.cpp:297` | 「Other subos in that home are **`subos doctor --fix`'s job** — init does not enumerate them.」 |
| `subos.cpp:1439` | `usage: xlings subos <new\|use\|list\|ls\|remove\|rm\|info\|i\|stop> [name]` |

**`subos doctor` 不存在。** 注释把责任交给了一个从未实现的命令，于是这块职责落到了地上 ——
这正是本仓库反复出现的「一个问题、多个回答者」的另一面：**一个问题、零个回答者，而文档说有一个。**

`self update` 也不在路上：【读码】`update.cpp` 全文只做三件事 ——
`xlings update` / `xlings install xlings@latest` / `xlings use xlings latest`，
不触碰任何 subos manifest。

### 2.2b 但覆盖不是主要缺陷 —— 懒补写已经在跑了，而且它补错了

> 本节是 review 中被追问「没用的 subos 为什么也要修？用的时候再修不行吗？」之后
> 补做的测量。**结论：这个追问是对的，而且它推翻了 §2.6 的第一版方案。**

【读码】`subos_info` 块的写入方一共 **六处**：

| # | 位置 | 场景 | runtime 从哪来 |
|---|---|---|---|
| 1 | `xself/init.cpp:162` | `self install/init` 修 `default` | `preserved_runtime` ✅ |
| 2 | `subos.cpp:348` | `ensure_subos_info_` | `preserved_runtime` ✅ |
| 3 | `subos.cpp:432` | `subos new` | `effectiveRuntime`（新建，正确）✅ |
| 4 | `subos.cpp:843` | `subos new --from`（fork） | `preserved_runtime` ✅ |
| 5 | `doctor.cpp:1911` | `self doctor --fix` | `preserved_runtime` ✅ |
| 6 | **`installer.cpp:1375` / `1382`** | **`xim install`：某个包声明了 `subos_env`** | **`mf::DEFAULT_RUNTIME` 直接写** ❌ |

第 6 处是关键。它的代码是：

```cpp
fresh[std::string(mf::BLOCK)] = mf::make_block(
    mf::DEFAULT_RUNTIME, std::format("xlings {}", Info::VERSION),
    platform::host_glibc_version());
```

`preserved_runtime` 一次都没被调用。所以：

1. **「用的时候再补」不是一个待做的设计，是一个已经在跑的实现** ——
   往一个无块的老 subos 里装任何带 `subos_env` 声明的包（mesa、nvidia-gl、jdk…），
   块就地补上了。覆盖问题基本上已经被它解决了。
2. **而它补出来的 runtime 是无条件的常量。** 哪怕该 subos 的 workspace 明明白白写着
   `glibc.active = 2.39`，这条路径也会把它声明成 `glibc@2.44`。

**六个回答者里五个走同一个函数，第六个不走。** 这是 `reference_one_question_many_answerers`
最纯粹的形态 —— 而且它是最容易被漏掉的那种：五个都对，所以 code review 时
「有没有统一入口」这个问题的答案看起来是「有」。

### 2.3 实测：你真机上的覆盖率与两条假声明

```bash
# 全量扫描 ~/.xlings/subos/*
```

【实测】39 个 subos 目录：

| 状态 | 数量 |
|---|---|
| 有 `subos_info` | 5 |
| 无 `subos_info` | 32 |
| `.xlings.json` 缺失/不可读 | 2 |

**5 个有块的里，2 个的声明与 sysroot 实际服务的 libc 矛盾：**

```
agent-influence  declares glibc@2.44   sysroot lib/libc.so.6 → xim-x-glibc/2.39
mcpp-test        declares glibc@2.44   sysroot lib/libc.so.6 → xim-x-glibc/2.39
```

`mcpp-test` 就是 issue 环境栏里那个 subos。**它不是「没有块」，它是「有一个假的块」。**
下游读到 `glibc@2.44`、去找一个没装的载荷、降级 —— `features.h: No such file or directory`
的直接上游就在这里。issue 提的方案（补写）如果照抄现在的 `preserved_runtime`，
**会把这个 bug 从 2 个 subos 扩散到 32 个。**

32 个无块 subos 按「能拿到什么证据」分类：

| 证据 | 数量 | 补写后果 |
|---|---|---|
| workspace 有 active glibc **且** sysroot 有 libc 符号链接 | 17 | 正确，`preserved_runtime` 源 (2) 就够 |
| 两者都没有 | 15 | **任何写入都是猜的** —— 这 15 个正是需要「未知」的 |

### 2.3b 那两条假声明从哪来 —— 以及回填会伪造 provenance

【实测】那 2 条假声明的块头：

```
agent-influence   created_at=2026-08-14T01:21:20Z  by=xlings 2026.8.14.1
mcpp-test         created_at=2026-08-15T14:31:16Z  by=xlings 2026.8.14.1
```

两者都在 `DEFAULT_RUNTIME` 移到 2.44 之后创建，声明 2.44 **在创建的那一刻是对的**；
之后装进去的是 glibc@2.39，sysroot 跟着变了，**声明没有跟着变，也没有任何检查发现**。
所以这两条的成因是「创建时声明 vs 事后实际安装」的漂移，
不一定是回填 —— §2.1 那次实测证明回填**也能**产生同样的假声明，但这两个具体的
subos 归因未定。**两条路径都能造出它，这本身就是问题。**

关键在于：**今天没有任何检查能看见这个漂移。** doctor D5 用的是 workspace 记录，
而这两个 subos 的 workspace 里根本没有 `glibc` 项 → `activeVersion` 为空 →
落到「cold intent」分支，`--fix` 刻意不采纳。而 sysroot 里那条
`lib/libc.so.6 → xpkgs/xim-x-glibc/2.39/...` 的符号链接 —— **摆在那里，没有一行代码看它。**

**顺带一条实测出来的、我第一版漏掉的缺陷：回填会伪造 provenance。**

```
default    created_at=2026-08-05T03:35:16Z  by=xlings 2026.8.5.1
gfxbuild   created_at=2026-08-05T03:35:16Z  by=xlings 2026.8.5.1   ← 逐字节相同
```

两个不同的 subos 共享**同一秒**的 `created_at`。它们不是同一秒被创建的，
是同一秒被**描述**的 —— 2026.8.5.1 正是引入 `subos_info` 的那个版本。
`default` 是这个 home 的原始 subos，远早于 2026-08-05。

【读码】`make_block()` 无条件写 `created_at = utc_now_iso()`。
**于是每一次回填都在一个早已存在的 subos 上盖一个假的出生日期。**
这跟我们想修的是同一类缺陷：一条从未被观测的事实，被一个常量顶替了。
`I8 = ProvenanceMissing` 因此永远不会触发 —— 它被一个假值满足了。

### 2.4 根因：现在的不变式不允许表达「未知」

【读码】`manifest.cpp:168`：

```cpp
const auto runtime = b.value("runtime", std::string{});
if (!is_binding(runtime)) {
    out.push_back({Defect::RuntimeMalformed, runtime.empty() ? "absent" : runtime});
}
```

I6 要求 `runtime` 必须是良构 binding。于是：

- 写 `runtime` 缺席 → `validate_block` 判定块无效 → D1 报 broken → `--fix` 重写块 →
  `preserved_runtime` 又回落到 `DEFAULT_RUNTIME` → **`--fix` 每次都"修复"，每次都写回假声明，
  永远不收敛。**
- 于是唯一能通过校验的写法就是编一个 —— **这不是实现疏忽，是不变式逼出来的。**

好消息：【读码】**xlings 内部所有 `info.runtime` 的读取方都已经先判 `is_binding`**：

| 读取方 | 行为 |
|---|---|
| `manifest.cpp:69` `check_runtime_activation` | `if (!is_binding(...)) return nullopt` |
| `doctor.cpp:471` | `if (mf::is_binding(info.runtime)) { ... }` |
| `xvm/commands.cpp:217` | `if (!mf::is_binding(info.runtime) \|\| ...)` |

**「runtime 缺席」对每一个在树读取方都已经是安全的。唯一挡路的是 I6 自己。**

### 2.5 顺带纠正 issue 的一处前提

issue 说「这句措辞（指向 `xlings self update` 的补救）是 xlings 自己写进块里的，mcpp 只是转述」。
【读码】`make_block()`（`manifest.cpp:267`）只写六个键：
`schema_version` / `runtime` / `envs` / `created_at` / `created_by` / `host_glibc`。
**没有任何 hint 字段。** 那句补救是 mcpp 侧自己写的。

这不改变结论，但改变修法：xlings 不能靠改一个字符串修好它，得**给 mcpp 一个真话可说** ——
即下面 §2.6 里那个可被区分的「未知」状态，和一条真的能修的命令。

### 2.6 方案

#### 决策点 1：「未知」怎么表达

| 方案 | 写法 | 代价 | 判断 |
|---|---|---|---|
| **A（推荐）** | 块存在、`runtime` 键**缺席** | 放宽 I6：`runtime` 缺席合法，存在则必须良构 | 无新字段；「旧格式」= 无块，「已知无 libc」= 有块无 runtime，issue 要的区分天然成立；在树读取方全部已兼容（§2.4） |
| B | `runtime: null` | 同上 + 所有 `value("runtime", "")` 对 null 的行为要逐个确认 | nlohmann 的 `value()` 遇到 null 类型会抛，风险大于收益 |
| C | 新增 `runtime_source: declared\|observed\|inferred\|unknown` | 新字段 + 老 client 读不到 + 需要 schema bump | 违反 `reference_read_write_invariant_asymmetry`：新字段上做门禁会打断所有老 home |

**取 A。** 并且 A 有个 B/C 没有的性质：**它不需要 schema bump。** 放宽校验是向后兼容的
—— 老 client 写的块在新 client 眼里仍然合法。

#### 决策点 2：谁来补，补到哪些 subos

> **本节在 review 后重写。第一版提了 `subos use` hook + `subos repair --all` +
> doctor 全量报告三件事。§2.2b 的测量把前提推翻了 —— 懒补写已经存在，
> 缺的从来不是覆盖。下面是收窄后的方案。**

**原则：一个 subos 的描述，在有人真的要用它的时候补；没人用的，什么都不做。**

##### 为什么「什么都不做」是对的，不只是省事

给那 15 个无证据的 subos 补写，写进去的是：
`schema_version` + `envs:{}` + `created_at` + `created_by` + `host_glibc`，**没有 runtime**。
信息量为零 —— 而**代价不为零**：§2.3b 已经实测到，`created_at` 会记成回填的时刻。
**给一个空 subos 补块 = 凭空造一条假的出生记录，换来零信息。**

##### 但「用的时候」必须定义清楚 —— hook 在 `subos use` 上有洞

【读码】`profile_resources.cppm:87`：

```sh
XLINGS_BIN="$XLINGS_HOME/subos/${XLINGS_ACTIVE_SUBOS:-current}/bin"
```

**`XLINGS_ACTIVE_SUBOS` 是一个继承的环境变量，profile 直接读它，不跑任何 xlings 代码。**
于是「进入一个 subos」有四种方式，只有第一种会执行 xlings：

| 方式 | 会跑 xlings 吗 |
|---|---|
| `xlings subos use <name>`（spawn / emit-shell / global） | ✅ 会 |
| 那个 spawn 出来的 shell 的**任何子进程** | ❌ 继承 env |
| `--global` 之后开的**任何新 shell** | ❌ profile 读 `subos/current` 符号链接 |
| tmux / 新终端 / CI 里 `export XLINGS_ACTIVE_SUBOS=...` | ❌ |

`--global` 的用户**一辈子只跑一次 `subos use`**。这正是 issue 报告者的形状：
`default` 是 current，从来不会再被 `use` 一次。**所以 hook 在 `subos use` 上，
对最常见的那个 subos 永远不触发。**

##### 结论：hook 在「已经在写这个文件、已经拿着锁」的地方

| 入口 | 覆盖 | 状态 |
|---|---|---|
| `xim install`（`installer.cpp:1375/1382`） | 活跃 subos | **已存在** —— 只需把 `DEFAULT_RUNTIME` 换成 `preserved_runtime` |
| `self doctor --fix`（`doctor.cpp:1911`） | 活跃 subos | **已存在且正确** —— 只需第 3 源 |
| `self install/init`（`init.cpp:162`） | `default` | **已存在且正确** |

**三个入口已经全在了。一行新命令都不需要加。**

**删掉的（第一版有、现在不做）**：

- ~~`xlings subos repair [--all]`~~ —— YAGNI。真正的读取方（mcpp）读的是**活跃** subos，
  而活跃 subos 已有三个补写入口。等出现「需要修一个我不在里面的 subos」的具体场景再说。
- ~~doctor 对其它 32 个 subos 出 Notice~~ —— 那是 32 行噪音。
  `doctor.cpp` 自己在 `UnverifiedPayload` 那里写过理由：「a home with 29 of them
  exiting non-zero would train everyone to ignore the command」。同样适用。
- ~~`subos use` hook~~ —— 上面证明它对最常见的情况不触发；加了会让人以为覆盖问题解决了。

**mcpp 的补救文案**因此指向 `xlings self doctor --fix`（活跃 subos，已经能修，
0.64s）——【实测】§2.1 证明它今天就有效，只是修出来的内容可能是假的，
而那正是决策点 3 要解决的。

#### 决策点 3：runtime 从哪里推

> **优先级表的最终版在决策点 3c**（它多了 `--runtime` 那一行和 Create/Describe 两列）。
> 本节留的是**为什么要加第 3 源**，那部分不受 3c 影响。

核心：把「常量兜底」换成一个**观测**源，并且推不出来时不写：

```
1. 块里已有的良构 binding                      ── subos 自己说过
2. workspace 的 active runtime                 ── use 维护的记录（现状）
3. sysroot 的 lib*/libc.so.6 符号链接指向的载荷  ── 【新】实际在服务的东西
4. ── 到此为止。推不出来 → runtime 缺席（不再回落到 DEFAULT_RUNTIME）
```

第 3 源是**观测**而不是记录，这是它的价值也是它的风险：

- 价值：`reference_absent_record_needs_observation` 说的正是这件事 —— 一个从未被记录的字段，
  用常量兜底就是在宣称一件假事。第 3 源把「常量兜底」换成「看一眼实际是什么」。
- 风险：它成了第三个回答者。**所以 2 与 3 冲突时不做静默取舍** —— 取 2（记录优先），
  同时**报一条新 finding `SubosRuntimeDrift`**：声明/记录/实际三者不一致本身就是缺陷。
  你真机上那 2 个假声明，正是这条 finding 该抓到的。

`DEFAULT_RUNTIME` 的作用域收缩为**只用于 `subos new`**（它本来就是这么写的
——`manifest.cppm:51` 的注释「Scope: NEW subos only」—— 但 `preserved_runtime` 的
fallback 参数把它漏进了每一条重建路径）。

#### 决策点 3b：第 6 处到底是不是「创建级别」的？——两个站点，答案不同

> review 追问：「第 6 处好像只能是默认的，是创建级别的？如果没有指定的话？」
> 这个怀疑值得，因为 `installer.cpp` 那里其实是**两个站点**，性质不一样。

【读码】两处的触发条件：

| 站点 | 触发条件 | 手上有什么 |
|---|---|---|
| **A** `installer.cpp:1375` | `.xlings.json` **整个文件不存在** | 只有目录 |
| **B** `installer.cpp:1382` | 文件存在（**带 workspace**），但没有 `subos_info` 块 | workspace 记录 |

**B 是无可辩解的。** 文件在、workspace 在，而 workspace 里可能明明白白写着
`glibc.active = "2.39"` —— 真机上 32 个无块 subos 里有 **17 个**正是这个形状。
把它们声明成 `glibc@2.44` 不是「默认」，是**无视手上已有的答案**。

**A 看起来像创建，但它不是。** 三条理由，按强度排列：

1. **这里不存在「没有指定」这回事 —— 因为没人被问过。**
   【读码】`--runtime` 这个标志**只存在于 `subos new` 的解析器**里
   （`subos.cpp:1461`，全仓唯一一处）。`xlings install` 没有、也不该有它。
   所以 A 站点写 `DEFAULT_RUNTIME`，不是「用户没指定所以用默认」，
   而是**把「从来没问过」记录成了「用户接受了默认」**。
   这和 §2.3b 那个假 `created_at` 是同一类伪造。
2. **真正的创建走的是另一条路。** `subos new` 在 `subos.cpp:432` 先写配置文件，
   带着 `effectiveRuntime`。等到 `xim install` 跑起来时，块早就在了 ——
   A 站点**永远不会看到一个刚被 `subos new` 建出来的 subos**。
   它只会看到**配置文件丢了的既有 subos**。
3. **A 站点也不是没有证据可看。** 配置文件丢了不代表 sysroot 空了 ——
   `lib/libc.so.6` 那条符号链接和 `.xlings.json` 是两件独立的东西。
   （真机上恰好那 2 个无配置的 subos（`agent-os-2` / `test-6`）也确实没有 `lib/`，
   都是 sandbox 脚手架；但这是这两个样本的事实，不是这条路径的性质。）

**那 A 站点在真的什么证据都没有时写什么？—— 写「未知」，而且这比常量更好，
因为它会自我修正：**

```
xlings install glibc@2.44        # 写块的那一刻，runtime 还没装上 → 未知
                                 # 装完之后 workspace 记下了 glibc@2.44
xlings self doctor               # 下一次 Describe 读到它 → 声明变成 glibc@2.44
```

**证据出现，答案就变准。** 而一个常量只可能碰巧对一次，并且永远无法被纠正 ——
因为它长得就像一条有效声明。这正是 `reference_absent_record_needs_observation`
说的那件事。

#### 决策点 3c：统一 1–6 —— 一个函数，两种意图

> review 要求：「统一 1–6 的行为」。下面是具体形状。

问题不是「有六个写入方」，而是**六个写入方各自决定 runtime 从哪来**。
统一的办法不是砍到一个写入方（它们各自有存在理由），而是**让六个都问同一个函数**，
并且把它们的差别收敛成**一个参数**：

```cpp
// manifest.cppm
enum class Intent {
    Create,    // 有人正在新建一个 subos，并且可能说了 --runtime
    Describe,  // subos 已经存在、已经在跑某个东西；不许发明
};

struct BlockSpec {
    std::string runtime;    // 空 = 未知 → 不写该键
    std::string by;         // "xlings <version>"
    std::string hostGlibc;
    Intent      intent;     // Create → created_at；Describe → described_at
};

// 「这个 subos 的 runtime 是什么」——全仓唯一的答案。
// `requested` 只在 Create 下被读取；Describe 传空。
std::string runtime_for(const fs::path& subosDir,
                        const nlohmann::json& doc,
                        Intent intent,
                        std::string_view requested = {});

nlohmann::json make_block(const BlockSpec& spec);   // 替换现有三参数版本
```

**优先级表（唯一一张）**：

| 顺序 | 源 | 性质 | Create | Describe |
|---|---|---|---|---|
| 1 | 块里已有的良构 binding | 声明 | ✅ | ✅ |
| 2 | `--runtime`（`requested`） | 人的意图 | ✅ | 参数不存在 |
| 3 | workspace 的 active runtime | 记录 | ✅ | ✅ |
| 4 | sysroot `lib*/libc.so.6` 指向的载荷 | **观测** | ✅ | ✅ |
| 5 | `DEFAULT_RUNTIME` | 常量 | ✅ 兜底 | ❌ **→ 空串 → 不写 runtime 键** |

**唯一的差别就在最后一行。** `DEFAULT_RUNTIME` 的作用域因此**字面上**等于它自己
注释里写的那句 —— `manifest.cppm:51`：「Scope: NEW subos only」。

**六个站点的归属**：

| # | 站点 | Intent | 现在传的 | 改成 |
|---|---|---|---|---|
| 1 | `init.cpp:162` | Describe | `preserved_runtime(json, DEFAULT)` | `runtime_for(dir, json, Describe)` |
| 2 | `subos.cpp:348` `ensure_subos_info_` | **Create** | `preserved_runtime(json, runtime)` | 加 Intent 形参；由调用方 3 传入 |
| 3 | `subos.cpp:432` `subos new` | **Create** | `make_block(effectiveRuntime, …)` | `runtime_for(dir, {}, Create, effectiveRuntime)` |
| 4 | `subos.cpp:843` `new --from` | **Create** | `preserved_runtime(cfg, runtime?:DEFAULT)` | `runtime_for(dir, cfg, Create, runtime)` |
| 5 | `doctor.cpp:1911` | Describe | `preserved_runtime(doc, DEFAULT)` | `runtime_for(p.subosDir, doc, Describe)` |
| **6a** | `installer.cpp:1375` | **Describe** | **`DEFAULT` 直接写** | `runtime_for(dir, {}, Describe)` |
| **6b** | `installer.cpp:1382` | **Describe** | **`DEFAULT` 直接写** | `runtime_for(dir, *doc, Describe)` |

**2 个 Create、5 个 Describe。今天有 2 个 Describe 站点在按 Create 的规则写。**

##### 统一时白捡的第 7 个缺陷：`new --from … --runtime X` 会静默忽略 X

【读码】`subos.cpp:843`：

```cpp
manifest::preserved_runtime(subosCfg,
    runtime.empty() ? manifest::DEFAULT_RUNTIME : runtime)
```

`preserved_runtime` 的第 1 源是「块里已有的 binding」。fork 的时候
`copy_tree_` 刚把 base 的 manifest 拷过来，**base 的块必然在**，
于是它永远赢过 `--runtime` —— **用户显式指定的值被静默丢弃。**

行为本身可以辩护（fork 复制的是 base 的载荷，声明成别的就是在骗人），
**但「静默」不行**。统一之后 `runtime_for` 能看见 requested 与结果不一致，
应当报出来：

```
[warn] --runtime glibc@2.44 ignored: fork of 'base' carries glibc@2.39,
       which is what its copied payloads were built against
       use `xlings subos new <name>` without --from to pick a runtime
```

#### 决策点 4：provenance 不许伪造

【读码】`make_block` 无条件写 `created_at = utc_now_iso()` / `created_by = <当前版本>`。
回填时这两个值都是假的（§2.3b 实测）。

改法：`make_block` 增加一个「这是回填还是创建」的入参 —— 回填时**不写 `created_at`**，
改写 `described_at` / `described_by`。I8 (`ProvenanceMissing`) 相应地接受「二者其一」。
这样 `subos info` 能如实说出「这个 subos 是 2026-08-05 被**描述**的，创建时间未知」。

#### 落地清单（收窄后：7 项，全部在既有入口上，无新命令）

| # | 改动 | 文件 | 性质 |
|---|---|---|---|
| 1 | 新增 `Intent` + `BlockSpec` + `runtime_for()`（决策点 3c 那张优先级表） | `subos/manifest.{cpp,cppm}` | **核心：唯一答案** |
| 2 | `runtime_for` 的第 4 源：读 `<subos>/lib*/libc.so.6` 符号链接 → `<pkg>@<ver>` | `subos/manifest.cpp` | **核心：观测** |
| 3 | **七个站点全部改调 `runtime_for`**（决策点 3c 的归属表），`preserved_runtime` 退役 | `init.cpp` `subos.cpp`×3 `doctor.cpp` **`installer.cpp`×2** | **核心：归队** |
| 4 | I6 放宽：`runtime` 缺席合法（存在则必须良构） | `subos/manifest.cpp:168` | 表达「未知」的前提 |
| 5 | `make_block(BlockSpec)`：空 runtime 不写该键；`created_at` / `described_at` 二选一 | `subos/manifest.cpp:267` | 决策点 4 |
| 6 | 新 finding `SubosRuntimeUnknown`(Notice) / `SubosRuntimeDrift`(Warning)；`new --from` 忽略 `--runtime` 时出 warn | `xself/doctor.{cppm,cpp}` + `subos.cpp:843` | 让不一致可见 |
| 7 | mcpp 侧文案改指 `xlings self doctor --fix`（**另一个 repo，另一个 PR**） | mcpp#427 | |

**不做**（第一版有，被 review 砍掉）：`subos repair --all`、`subos use` hook、
doctor 扫描其它 subos。理由见决策点 2。

**为什么 1–3 必须在同一个 PR 里**：`runtime_for` 加进来而站点没改完，
就是第七个回答者。**统一的价值全在「一个都不剩」上** ——
`preserved_runtime` 必须在同一个 commit 里被删掉，让「还有站点没归队」变成编译错误
而不是需要有人去查的事情。

#### 验收（每条都要能证伪）

0. **归队完整性**（编译期）：`preserved_runtime` 在树里搜不到任何调用者，
   且符号本身已删除。**这条不是测试，是「1–3 必须同 PR」的机械保证。**
1. **站点 6b**：隔离 home，subos 有 `.xlings.json`（含 `workspace.glibc.active = "2.39"`）
   但无块 → 走一次带 `subos_env` 声明的安装 → 块的 `runtime == "glibc@2.39"`。
   **这条今天必红**（会写成 `glibc@2.44`），是整个改动的主门禁。
1b. **站点 6a**：同上但 `.xlings.json` **整个不存在**、且 `lib/libc.so.6` 指向
   `xpkgs/xim-x-glibc/2.39/...` → 块的 `runtime == "glibc@2.39"`；
   把那条符号链接也删掉 → **无 `runtime` 键**（而不是 `glibc@2.44`）。
   这两条一起证明 §决策点 3b 的结论：A 站点不是创建级别的。
1c. **Create 仍然走默认**：`subos new fresh`（无 `--runtime`、空目录）→
   `runtime == DEFAULT_RUNTIME`。**这条保证收窄没有误伤新建路径。**
1d. **`new --from` 不再静默**：`subos new f --from base --runtime glibc@2.44`
   而 base 声明 2.39 → 结果仍是 2.39，**且 stderr 出现说明 X 被忽略的 warn**。
2. **未知可表达**：subos 无块、无 workspace 记录、无 libc 符号链接 →
   `self doctor --fix` 后块存在且**没有 `runtime` 键**；`self doctor` 出 1 条
   `SubosRuntimeUnknown` Notice，**退出码 0**。
3. **收敛性**：对 (2) 的结果连跑三次 `self doctor --fix`，`.xlings.json` **逐字节不变**。
   （今天这条会红：`created_at` 每次重写。）
4. **sysroot 源**：给 `subos/b/lib/libc.so.6` 造一个指向 `xpkgs/xim-x-glibc/2.39/...`
   的链接、workspace 为空 → `runtime == "glibc@2.39"`，不是 `2.44`。
5. **漂移可见**：构造 `runtime=glibc@2.44` + sysroot 指向 2.39 → 必须出
   `SubosRuntimeDrift`，且 `--fix` **不得**静默改写（改的是声明的含义，得由人决定）。
   **这条能在你真机上直接验**：`agent-influence` 和 `mcpp-test` 就是现成样本。
6. **provenance**：回填出来的块**没有 `created_at`**，有 `described_at`。
7. **老 client 兼容**：用 2026.8.14.1 的二进制读一个无 `runtime` 键的块 →
   不得崩溃、不得判为损坏（`is_binding` 已保证，但要真的跑一次 —— 见
   `reference_recipe_capability_probe`：对着真的旧二进制验）。

---

## 3. #549（docs）：把它读成什么，和它没能关掉什么

【读码】纯文档，`.agents/docs/2026-08-14-...-plan.md` +46 行。

### 它做对的

拿到的是**非构造样本**（index CI 的 `windows-test` 真装了 msvc + windows-sdk，
23 payload / ~220 MB，下载线程与 TUI 线程并发），三个判据全 0，
并且**明确写了不能读成「修复生效了」**（样本量的是 v2026.8.10.1，即 O1–O4 之前）。
结论收窄成一句更强的话：管道那条路从来就没坏过。

这是本仓库该有的写法 —— **`reference_cross_check_harness_traps` 的正例**：
测量集合是从主张推导出来的，没测到的部分被明确标成没测到。

### 残留的两个洞

**洞 1：D4 现在处于「不可证伪」状态。** 判别实验要求一台带控制台的 Windows，
而 CI 的 job 没有附着控制台。这条会一直开着，直到有人手动跑一次 —— 而
「等某人手动跑一次」在这个仓库的历史里等于「永远开着」。

> **优化**：把它变成一个能在 CI 里跑的门。windows runner 上可以
> `AllocConsole()` + `GetConsoleScreenBufferInfo` / `ReadConsoleOutput` 读回控制台缓冲区。
> 做一个最小 harness：一个线程按行 `log::info`，一个线程刷 TUI 帧，跑 2 秒，
> 从控制台缓冲区读回来扫同样三个判据。**这把「结不了案」变成「一个 60 行的测试」。**
> 若判定成本太高，退而求其次：加一个 `XLINGS_OUTPUT_SELFTEST=1` 让进程自检并
> 退出码化，至少让手动那次跑得动、结论可记录。

**洞 2：文档自己记下的那条「顺带发现」没有落地。**
index CI 用 `v2026.8.10.1` 验证 recipe，`ci-xpkg-test.yml` 更旧（`v2026.8.8.2`）。
文档写了「记下来，因为这正是『门禁测的不是发出去的东西』那个形状」，然后就没有然后了。
**这正是 `reference_ci_green_ran_nothing` 的近亲**：门是绿的，测的是四个版本以前的东西。

> **优化**：给这两个 workflow 的 pin 加一条来源约束 —— 要么跟 `releases/latest`，
> 要么在 pin 旁边写明**为什么必须钉在这一版**并加一条 CI 检查，在 pin 落后 latest
> 超过 N 个版本时报警。**不写理由的 pin 会一直漂。**

---

## 4. #550（`info` 对索引里有的包说"不存在"）

### 它做对的

- 改在 `resolve_target` 这个唯一收口上 —— 【读码】17 个调用点
  （`commands.cpp` ×8、`resolver.cpp` ×4、`installer.cpp`、`sandbox.cpp`、
  `doctor.cpp`、`catalog.cpp`），所以 `install` / `remove` / `info` / 依赖解析
  **一次全部受益**。这是正确的层。
- **中途自己抓到的那次错误值得单独表扬**：第一版把「版本不存在」也报成「平台不支持」，
  被 E2E-76 抓到。修法是要求「请求的那个版本在该平台真能解析出来」——
  `select_version_(*pkg, plat, parsed.version).empty() → continue`。
  这是把「一句话里同时说两件互相矛盾的事」变成了不可能。
- E2E fixture 是**双向**的：`config-no-payload@9.9.9` 必须仍说 not found 且**不得**列平台，
  `config-other-platform` 必须说 no build 且必须点名 windows。
  单向 fixture 会被错误实现满足 —— 这一点在 fixture 注释里写明了。

### 缺陷 1：它把自己诊断的那个 bug 又造了一遍

PR 的论点原文：

> 「参数没有调用者就不是功能，而唯一为它而写的地方正是唯一没用它的地方。」

【读码】它给 `collect_matches_` 新加了 `bool forSearch = false`：

```cpp
// catalog.cppm:227
std::vector<PackageMatch> collect_matches_(const std::string& target,
                                           const std::string& platform,
                                           bool forSearch = false) const;
```

`collect_matches_` 的调用者只有一个：

```
catalog.cpp:615:  auto matches = collect_matches_(target, platform);   // 没传
```

而 `platforms_offering_` 是绕过 `collect_matches_` 直接调 `build_matches_` 的
（`catalog.cpp:583`）。**所以新加的这个参数，今天没有任何调用者。**

> **优化**：把 `collect_matches_` 的 `forSearch` 删掉。它是顺手加的对称性，
> 不是需求。**留着它，下一个人会以为 `collect_matches_(x, y, true)` 是被支持的路径。**
> （若确实想让 `search` 走同一条路，那就让 `search` 真的调它 —— 但那是另一件事，
> 不该以一个没人传的默认参数的形式先躺在这里。）

### 缺陷 2：`platforms_offering_` 把包又解析了一遍

【读码】`build_matches_` 内部已经 `load_package(*matched)` 过一次并丢弃
（`catalog.cpp:408`），`platforms_offering_` 拿到 match 后又 `load_package(m.rawName)`
（`catalog.cpp:585`）。只发生在错误路径上，代价可接受，但：

> **优化**：`PackageMatch` 已经带 `pkgFile`；要么让 `build_matches_` 顺手把
> 平台集合算出来，要么加个 `load_package` 的小缓存。**低优先级，记录在案。**

### 缺陷 3：一个门禁可能被这次改动悄悄削弱

【读码】`tests/e2e/install_subindex_first_run_test.sh:119`：

```bash
if grep -qE "package 'testd2x:d2testpkg[^']*' not found" <<<"$CLEAN"; then
  fail "#366 regression: sub-index package 'not found' on first run"
fi
```

这是**靠匹配错误文案**来守 #366 的。今天它仍然有效（首次运行时包根本不在索引里，
`platforms_offering_` 返回空，文案仍是 not found）—— 但它现在依赖一个「不会走到新分支」的
假设，而那个假设没写在任何地方。

> **优化**：把这条守卫从「文案不出现」改成「**安装真的成功**」。
> `gate_the_message_on_behaviour` 说的正是这个：守在文案上的门，在行为变了之后还会继续绿。

### 验收

- `xlings search msvc` 与 `xlings info msvc` 在 Linux 上不再给出互相矛盾的答案（已有 E2E 覆盖）。
- 删掉 `collect_matches_` 的 `forSearch` 后全量编译 + E2E 通过。
- `install_subindex_first_run_test.sh` 改成断言安装成功后仍然通过。

---

## 5. #551（卸载删不掉被占用的 payload）

### 它做对的

三种拒绝方式的分类是准的（只读位 / 文件被打开 / 目录被打开），处置顺序也是对的
（清只读 → 挪走 → 接受目录骨架）。「Windows 允许重命名打开着的文件，删则不允许」
是这套方案的支点，而**判断它属于 xlings 而不是 mcpp 是对的** —— 17 个
`resolve_target` 调用者说明 xim 有很多消费者，每个自己写一遍这套逻辑会以很多种方式写错。

而且【实测】你贴的那条 CI 日志末尾就有它的证据：

```
Cleaning up orphan processes
Terminate orphan process: pid (8696) (vctip)
```

`vctip.exe` 在 job 结束时还活着 —— 注释里写的那个进程，是真的。

### 缺陷 1（严重 / 数据丢失）：回滚分支删掉了它已经挪走的文件

【读码】`installer.cpp:2995-2998`：

```cpp
if (moved != files.size()) {       // half-moved is worse than untouched
    fs::remove_all(trash, ignore);
    return false;
}
```

`trash` 里装的**正是已经成功挪出去的那些文件**。`remove_all(trash)` 把它们**删了**，
不是挪回去。结果：

- payload 目录里少了一个子集的文件；
- 那个子集**已经不存在于任何地方**；
- 函数返回 false，上层只 `log::warn`，包仍然注册着。

注释说「缺了任意子集文件的 payload 比原封不动更糟」，然后代码亲手制造了这个状态。
**这是 `project_silent_success_pattern` 的镜像：一次失败的清理产生了比失败更坏的结果，
而输出看起来只是一条 warn。**

> **修法**：回滚必须是 `rename` 回去，不是 `remove_all`。
> ```cpp
> if (moved != files.size()) {
>     for (std::size_t i = 0; i < moved_index_list.size(); ++i)
>         fs::rename(trash / staged_name[i], files[moved_index_list[i]], ignore);
>     fs::remove_all(trash, ignore);   // 此时应为空；非空要报出来
>     return false;
> }
> ```
> 并且：挪回去也可能失败。**挪不回去必须是一条 error 而不是 warn** ——
> 那是唯一一种「载荷真的坏了」的情况，用户必须知道要重装。

### 缺陷 2：`.trash-*` 泄漏，而且泄漏进了版本命名空间

【读码】成功路径（`installer.cpp:2999-3002`）：

```cpp
ec.clear();
fs::remove_all(root, ec);
fs::remove_all(trash, ignore);   // ← 被占用的那个文件仍然被占用，这里必然失败
if (!ec) return true;            // ← 仍然返回 true
```

**挪走一个打开着的文件并不会关闭它。** 所以在这个函数唯一被需要的场景里，
`remove_all(trash)` 一定失败，`.trash-<version>` 一定留下。
注释说「The leftover directories carry no meaning and are removed on a later run」——
【读码】`remove_payload_dir` 只有 1 个调用者，参数永远是 `<store>/<pkg>/<version>`，
**没有任何代码会再碰 `<store>/<pkg>/.trash-<version>`。**

而 `trash` 建在 `root.parent_path()`，也就是**版本目录的同级**。
【读码】把 `xpkgs/<pkg>/*` 的每个子目录当成一个版本来读的地方有 7 处：

```
src/core/xim/commands.cpp:131          src/core/xself/doctor.cpp:629
src/core/xim/commands.cpp:801          src/core/xself/doctor.cpp:1415
src/core/xim/inventory.cpp:464         src/core/xself/doctor.cpp:1491
src/core/profile.cpp:338
```

【实测·读码】逐个核对过：**七处没有一处过滤点号开头的目录**，都是
`if (!verDir.is_directory()) continue;` 之后直接把目录名当版本号用。落到实际影响：

| 站点 | 会发生什么 |
|---|---|
| `doctor.cpp:1415` | 产出坐标 `xim:node@.trash-22.17.1` 的 `UnverifiedPayload` finding |
| `doctor.cpp:1491` | 深度审计把 trash 里的文件也 readelf 一遍 |
| `commands.cpp:801` | `payload_has_content(trash)` 为真 → **唯一版本已卸载的包仍读作「载荷在盘上」** |
| `inventory.cpp:464`、`profile.cpp:338` | 以 `<pkg>/.trash-<ver>` 的形式进入清单 |
| `commands.cpp:131`、`doctor.cpp:629` | 各自要求 `.xlings-resolution.json` / `bin/`，trash 里没有 → 无影响 |

**卸载失败的副作用是凭空多出一个版本。**

另外【读码】trash 里的文件名是 `<序号>-<basename>`，目录层级被拍平了 ——
所以即便想恢复也没有映射可用。**这条本身就说明当前的回滚分支不可能是「回滚」。**

> **修法（两条都要）**：
> 1. trash 放到**版本命名空间之外** —— `<home>/data/trash/<pkg>/<version>-<n>/`，
>    或至少 `<store>/.trash/`（前缀点号不够，7 处枚举没有一处过滤点号 —— 已核对）。
> 2. 给它一个**真的会来的清理时机**：`self doctor` 每次跑都尝试清一遍
>    （便宜、幂等、且 doctor 本来就在遍历 store），清不掉就报 Notice 说明谁占着。
>    「a later run」必须指向一段真实存在的代码。

### 缺陷 3：一个调用点、零测试、而唯一相关平台的 CI 没跑到

【读码】其余仍在裸用 `remove_all` 且面对同一问题的地方：

| 位置 | 场景 | Windows 上会怎样 |
|---|---|---|
| `subos.cpp:531`、`subos.cpp:1231` | `xlings subos remove` | 另一个 shell 把 subos 当工作目录 → 删不掉 |
| `xself/uninstall.cpp:302,317` | `xlings self uninstall` | 同上，且 `xlings.exe` 自己可能就在里面 |
| `xself/install.cpp:695` | 安装期清理 | 同上 |

（`installer.cpp:1026` 不在此列 —— 【读码】它只在 `installDir` 为空时触发。）

测试：**#551 没有任何测试。** `remove_payload_dir` 在匿名 namespace 里，
从单元测试够不到。而 Windows E2E：【读码】`.github/workflows/xlings-ci-windows.yml`
只跑 E2E-01…07，**仓库里 20 个涉及 `remove` 的 E2E 一个都不在 Windows 上跑。**

> **修法**：
> 1. 把 `remove_payload_dir` 提到一个可测的位置（`xim::payload` 或一个小的
>    `fs_remove.cppm`），让其余 5 个站点都用它。**「谁都没法比这更好地删掉别人占着的文件」
>    这个论点，对 subos remove 和 self uninstall 一样成立。**
> 2. **回滚分支在 Linux 上是可测的**：把某个子目录 `chmod a-w`，它里面的文件就 rename 不出去
>    → 强制走 `moved != files.size()` → 断言「所有文件仍在原处」。
>    这条测试今天会红，正是缺陷 1。
> 3. trash 泄漏在 Linux 上也可测：造一个 rename 成功但 `remove_all` 失败的形状
>    （目录被设为不可写），断言 store 目录下不出现新的「版本」。

### 验收

- 上述三条测试加进 `tests/unit` / `tests/e2e` 并在 Linux 上通过。
- `xlings list` 在一次失败卸载之后，输出与卸载前逐字节相同（除被卸载的那一项）。
- 5 个 `remove_all` 站点收敛到一个函数。

---

## 6. #552（开着）：「不在索引里」与「还没同步到」的第三次拆分

> 本节在 review 中被要求一并纳入。**本文档本身就在这个 PR 里** —— 见 §6.4。

### 6.1 它做对的，而且它属于一个清晰的序列

这是同一个 conflation 的第三刀：

| PR | 拆开的两件事 |
|---|---|
| #550 | 「不存在」 vs 「存在、但这个平台没有构建」 |
| **#552** | 「不存在」 vs 「**存在、但同步过来的索引还没有它**」 |
| （剩下的） | 「不存在」 vs 「存在、但你没权限 / 镜像没有」 |

诊断是准的，而且**信息本来就在盘上**（同步下来的索引是一个 git 工作树）。
**直接读 `.git` 而不是 fork `git`** 是对的选择，理由和 2026.8.14.1 那次
「读 ELF header 而不是 fork patchelf」一模一样：错误路径不该在失败的那一刻
新增一个对 PATH 上有没有 git 的依赖。读不到就退回原文案，静默降级。
loose ref / `packed-refs` / detached HEAD 三种形态都覆盖了。

### 6.2 缺陷 1：PR 正文承诺的收窄，代码没有做

正文说：

> the existing `xlings update` advice is attached to the case it actually
> applies to instead of to every miss

【读码】`not_found_()` 的实现是：只要**任何一个 repo 能读出 revision**，
就拼上那句 `if it was just published, ... run xlings update`。
正常 home 里 `xim-pkgindex` 永远是 git 工作树 → **这句话仍然贴在每一次 miss 上**，
包括打错字。**revision 加上了，advice 一点也没收窄。**

> **优化**：真正能把两者分开的，是**这份索引有多旧**。revision 回答的是
> 「哪个快照」（CI 需要，用来和 merge commit 对时间线），而人需要的是
> 「我这份是什么时候同步的」。两个都便宜：
> ```
> package 'mcpp@2026.8.16.3' not found in the synced index
>   (xim@288efe5, synced 4 hours ago)
>   → if it was just published, run `xlings update`     ← 仅当 synced 超过阈值
> ```
> 同步时间可以从 `.git/FETCH_HEAD` 或索引目录的 mtime 读到，同样不需要 fork git。
> **门槛之下不打印那句 advice**，正文承诺的收窄才算兑现。

### 6.3 缺陷 2：CRLF 在两个读取器之间处理不一致

【读码】同一个函数里两处读文件：

```cpp
auto read_first_line = [](const fs::path& f) -> std::string {
    ...
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();          // ← 剥掉 \r
    return line;
};
...
std::ifstream packed(gitDir / "packed-refs");
while (std::getline(packed, line)) {
    ...
    if (line.compare(sp + 1, refName.size(), refName) == 0
        && line.size() == sp + 1 + refName.size()) {   // ← 没剥 \r，长度精确匹配
```

`packed-refs` 那条路径**没有**剥 `\r`，而它用的是**精确长度**比对。
在任何让该文件带上 CRLF 的场景下（Windows 上 `core.autocrlf` 被改过、
索引被非 git 工具写过），匹配必然失败 → `sha` 为空 → 退回原文案。
**失败方式是「消息变回旧样子」，没有任何提示** —— 这正是本文档 §8 在讲的那个形状。

同一函数里两种换行处理，是最容易在 review 中滑过去的不一致。

另一处更小的：loose ref 文件内容没有做十六进制校验。符号引用链
（`ref: refs/heads/x` 指向另一个符号引用）会被当成 sha，截成 `ref: re` 打出来。
罕见，但 `sha.size() >= 7` 这个门槛挡不住它。

### 6.4 缺陷 3：这个 PR 里装了三样不相干的东西

【实测】`gh pr view 552 --json files`：

| 文件 | 增删 | 与 PR 标题的关系 |
|---|---|---|
| `src/core/xim/catalog.{cpp,cppm}` | +92 −3 | ✅ 就是它 |
| `.agents/docs/2026-08-17-...-review.md` | +571 −0 | ❌ **就是本文档** |
| `mcpp.lock` | +10 −56 | ❌ **依赖解析变了** |

`mcpp.lock` 那条不是格式化 —— 它**移除了 9 个 pin**
（`compat.{bzip2,lua,lz4,mbedtls,xz,zlib,zstd}`），并且把 `ftxui` 从
`compat` 命名空间挪到了 `mcpplibs`，hash 也换了：

```
-[package."ftxui"]  namespace = "compat"     hash = "fnv1a:6ecc43e2f9201d7d"
+[package."ftxui"]  namespace = "mcpplibs"   hash = "fnv1a:b3ae3d5c6e5a7a91"
```

同时删掉了文件头那段解释「这个文件记录什么、为什么不 pin 未来构建」的注释。

**这是一次真实的依赖变更，藏在一个改错误文案的 PR 里。**
`project_ci_index_ref_pin` 记的正是这条路上的雷：动 mcpp 要同步 6 个 workflow 的
`XIM_PKGINDEX_REF`。一个命名空间搬家 + 7 个 pin 消失，在「不 pin 未来构建」的
lock 语义下**可能无害，也可能是下一次 CI 玄学的源头**，而没有人会在
review 一条错误文案时去查它。

> **优化**：拆成三个 PR。`mcpp.lock` 单独一个并说明为什么变
> （是 mcpp 版本变了？索引变了？还是本地跑了一次 `mcpp build` 顺手带上的？）；
> 文档单独一个；catalog 那 92 行留在这里。

### 6.5 缺陷 4：又一次「新逻辑、零测试」

`index_revision_` 是一个**纯函数**：输入一个目录，输出一个短 sha。
造 fixture 极其便宜 —— 临时目录里写 `.git/HEAD` + loose ref、写 `packed-refs`、
写 detached HEAD、写一份 CRLF 的 `packed-refs`（§6.3 那条今天会红）、
写一个非 git 目录。**五个用例，一个下午都不用。**

PR 正文说的验证是「把 reader 抽出来单独编译，对着真的同步索引跑了一遍」——
那是**一次性的、不可重放的**手工验证。#550 有 E2E，#551 有 0 个，#552 有 0 个。

> **这是连续第三个 PR 把新逻辑塞在 `.cpp` 的匿名/文件作用域里，因此够不到单测。**
> `remove_payload_dir`（#551）、`index_revision_`（#552）都是这个形状。
> 不是「忘了写测试」，是**放的位置让测试写不了**。建议立一条约定：
> 新增的、有分支的纯函数放进可 import 的模块里，哪怕只为可测。

### 6.6 一处值得肯定的、作者自己可能没看见的事

新消息带一个内嵌 `\n`。【读码】`log.cpp:136` 的 `emit_line_` 在 2026.8.14.1 的
O1/O2 里已经做对了这件事：按 tag 宽度缩进后续行，整条在互斥锁下一次写出并 flush。
**#552 是这个能力的第一个使用者，而且它是安全的。**

不过消息里那两个显式空格会和自动缩进**叠加**（`[error] ` 8 列 + 2 列 = 10 列）。
作者在正文里说本地构建被 stale `gcm.cache` 挡住了 —— 也就是说
**这条消息渲染出来的样子，作者自己没看到过**。建议合入前贴一张真实输出。

### 6.7 #552 的落地建议

| 项 | 动作 |
|---|---|
| `mcpp.lock` | **拆出去**，单独 PR，说明变更来源 |
| 本文档 | **拆出去**，docs PR |
| §6.3 CRLF | 修，并加 fixture |
| §6.2 advice 收窄 | 加同步时间 + 阈值，兑现正文的承诺 |
| §6.5 可测性 | `index_revision_` 挪到可 import 的位置 + 5 个用例 |
| 输出样子 | 贴一张真实渲染 |

---

## 7. 横切：Windows CI 现在是红的，而且它红的方式最坏

【实测】`gh run list --workflow=xlings-ci-windows.yml`：

| run | 时间 | 结论 | 内容 |
|---|---|---|---|
| 31948575455 | 08-16 12:59 | ✅ success | #551 的 PR 分支 |
| **31949882496** | **08-16 13:27** | **❌ failure** | **#551 合入后的 main（同样的代码）** |

失败点【实测】：

```
tests/unit/test_runtime.cpp(771): error: Expected equality of these values:
  tm.info(tid).status      Which is: 4-byte object <01-00 00-00>   ← running
  TaskStatus::completed    Which is: 4-byte object <03-00 00-00>
[  FAILED  ] TaskManager.PromptHandling (1440 ms)
```

【读码】`TaskStatus` 枚举：`pending=0, running=1, waiting_prompt=2, completed=3`。
测试在 `respond()` 之后以 `100 × 10ms = 1s` 的上限轮询 completed，
在 runner 上没等到。**同一份代码在 PR 分支上通过 —— 这是 flake，不是回归。**

但它的代价不是「一次 flake」：

> unit test 步骤失败 → 后续 **15 个步骤全部 skipped** →
> Build / SubOS sandbox contract / Fresh XLINGS_HOME / E2E-01…07 **一个都没跑**。

也就是说：**#551 这个纯 Windows 的修复，合入 main 之后，Windows 上的端到端信号是零。**
这与 `reference_ci_green_ran_nothing` 是同一个形状的另一面 ——
那边是「绿的但什么都没跑」，这边是「红的因此什么都没跑」，两者都让人读不出真实状态。

> **优化（按性价比排序）**：
> 1. `TaskManager.PromptHandling` 的 1s 硬上限换成**带超时的条件变量等待**，或至少
>    把上限提到 10s 并在超时时打印任务的实际状态。
>    `reference_lock_timeout_couples_to_tests` 的教训在这里反过来用：
>    **超时值是测试的参数，不该是产品行为的复刻。**
> 2. Windows workflow 把 unit test 与 E2E 拆成**两个不互相阻塞的 job**（共享 build 产物）。
>    一个计时敏感的单元测试不该有权决定「今天要不要跑 E2E」。
> 3. 把 `remove_*` 系列 E2E 里**至少一条**排进 Windows leg —— #551 的整个论证前提
>    就是「Windows 上会失败」，而现在没有任何自动化能观察到这一点。

---

## 8. 优先级与落地顺序

| 序 | 项 | 为什么是这个顺序 | 规模 |
|---|---|---|---|
| **P0** | #551 缺陷 1（回滚删文件） | 唯一一条会**丢数据**的 | ~20 行 + 1 测试 |
| **P0** | Windows CI 拆 job / 放宽 flake 上限 | 在它修好之前，**下面每一条在 Windows 上都验证不了** | workflow + 1 处测试 |
| **P0** | #552 把 `mcpp.lock` 拆出去 | 一次依赖变更藏在文案 PR 里，合了就查不出来了 | 拆分而已 |
| **P1** | #547 §2.6 的第 1–3 项（`runtime_for` + sysroot 观测 + 七个站点归队） | **就是那条假声明的成因**；三条必须同 PR，否则等于加第七个回答者 | ~150 行 |
| **P1** | #551 缺陷 2（trash 泄漏进版本命名空间） | 卸载失败会污染 `list`/doctor/引用计数 | ~30 行 + 1 测试 |
| **P2** | #547 §2.6 的第 4–6 项（I6 放宽 + provenance + 三条 finding/warn） | 让 15 个无证据 subos 有个诚实的表达 | ~120 行 |
| **P2** | #552 §6.2/6.3/6.5（收窄 advice、CRLF、可测性） | PR 还开着，趁现在改 | ~60 行 + 5 用例 |
| **P2** | #551 缺陷 3（5 个站点收敛 + Windows E2E） | 正确但不紧急 | 中等 |
| **P3** | #550 删掉没人用的 `forSearch` | 卫生 | 3 行 |
| **P3** | #549 的两个洞（控制台自检、CI pin 漂移） | 都是「让将来能结案」，不是「现在坏了」 | 各自独立 PR |

**建议拆 PR**：

- P0 三条各自独立（`mcpp.lock` 那条本来就是「拆出去」）。
- **#547 拆成两个**：先落 P1 的两条（纯粹修正一个错值，无 schema 语义变化，好回滚），
  再落 P2 的表达层（改不变式、改 provenance，语义面更大）。
- **不要把 #547 和 #551 放进同一个 PR** —— 一个是迁移语义，一个是文件系统语义。

---

## 9. 一句话的横切观察

这五件事是同一个形状的五个面：

> **一段代码宣称了一件它没有观测过的事，而宣称和事实长得一模一样。**

| | 宣称 | 实际 |
|---|---|---|
| #547 `installer.cpp:1375/1382` | 「这个 subos 跑 glibc@2.44」 | 从没看过它跑什么 |
| #547 同上，另一面 | 「用户没指定所以用默认」 | `install` 根本没有 `--runtime` 可指定 |
| #547 `new --from --runtime X` | （什么都不说） | X 被丢弃了 |
| #547 `created_at` | 「这个 subos 生于 2026-08-05」 | 那是回填的时刻 |
| #550 `collect_matches_(…, forSearch)` | 「这条路径被支持」 | 没有调用者 |
| #551 `remove_all(trash)` | 「已回滚」 | 已删除 |
| #552 packed-refs CRLF | 「读不到 revision」 | 读得到，只是多了一个 `\r` |
| #549 D4 | 「待实测」 | 没有任何载体能测 |

`project_silent_success_pattern` 记的是「没发生」与「成功了」输出相同。
这六条是它的上游：**「没观测」与「已确认」在代码里长得相同。**
修法始终是同一个 —— **让缺席可被表达，并且让它看起来就是缺席。**

### 附：这一轮真正教会我的一件事

第一版方案（`subos repair --all` + `subos use` hook + doctor 全量扫描）
是在**没有量过谁在写这个块**的情况下写出来的。它假设缺陷是「覆盖不够」，
于是设计了三个新的覆盖入口。

被追问「没用的 subos 为什么也要修」之后再去量，才发现：
**补写已经有六个入口了，第六个在写假话。** 正确的动作不是加第七个，
是让第六个归队 —— 从 8 项减到 6 项，而且删掉的全是新增的面。

> 「多加一个入口」几乎总是比「找出已有的那几个入口为什么不一致」容易想到。
> 这是 `reference_one_question_many_answerers` 的实践版：
> **在提出新答案之前，先数一遍现在有几个答案。**

---

## 10. 实现之后：这份方案里被推翻的五条

> 2026-08-17 追加。落地 PR #553 / 2026.8.17.1。
> 按本仓库的惯例：**方案里错的地方要留在方案里**，删掉它等于把「当时是怎么想的」
> 一起删掉，而下一个人会重新想一遍。

### 10.1 「回滚必须把文件挪回去」—— 错，而且错在更深的地方

§5 缺陷 1 说：回滚分支应当 `rename` 回去而不是 `remove_all`。照做，然后
**测试红了**：`APartialMoveRestoresEveryFileItDisplaced` 发现文件根本没回来。

原因不是回滚写错了，是**「回滚到未触碰」从来就不可达**。快路径 `remove_all`
不是原子的 —— 在一棵有文件被占用的树上，它会先删掉所有能删的再报错。
**等我们知道有东西被占用时，别的文件已经没了。**

所以 §5 提的修法（rename 回去）只是把一个不可能实现的承诺换了个写法。
真正可达的不变式是：**残留不能读起来像「已安装」** —— 因为
`payload_has_content` 对它为真，下一次 `xlings install` 会接收残骸。
落地改成 `Partial` 把目录 stamp 成 incomplete，`install_state` 最先检查它。

**这条是测试写出来的，不是想出来的。**

### 10.2 「冻结目录就能在 Linux 上走到失败分支」—— 错

第二版测试用 `chmod a-w` 让 rename 失败。也红了：`clear_readonly_` 会把目录
chmod 回可写 —— 那正是它存在的理由（Windows 的只读位）。

推论：**在 POSIX 上，我们自己的树里没有任何东西能抵抗我们。**
打开的 fd 不阻止 unlink，权限我们随时能改回来。`Partial` 分支在
Linux/macOS 上**结构性不可达**。

于是把落点 `settle_removal` 提成一个真函数并用真实残留驱动它，
而**不去断言那条分支** —— 断言一条测试从未进入的路径，是一个什么都不意味着的绿。

### 10.3 「`self init` 永远是 Describe」—— 错，一半

§决策点 3c 的归属表把 `init.cpp:162` 标成 Describe，理由是「`self init` 跑在
install 和 update 上，不是创建」。

**E2E-67/S1 抓到了：** `ensure_home_layout` 在**全新 home** 上就是在创建
`default` —— 而 Describe 禁止回落到默认值，于是新 home 的 `default`
一个 runtime 都没记。

正确的分叉是 `subos.cpp` 本来就在用的那个：**配置文件存不存在**（在任何写入之前采样）。
一个函数可以同时是两种意图，取决于它遇到的世界。

> 顺带一条方法论：这条是被一个**为相反失败而写的**测试抓到的
> （E2E-67 原本防的是「默认值一升，已有 subos 被重新声明」）。
> 一条规则的两半都上门禁，才抓得到从另一头掉下去的那次。

### 10.4 「旧 client 读新块没问题」—— 错

§决策点 1 说方案 A「老 client 写的块在新 client 眼里仍然合法」，这是对的；
但发布说明里我顺手写成了双向兼容，那是错的。

旧 client 的 `validate_block` 会把「`runtime` 缺席」判成 `RuntimeMalformed`，
它的 `--fix` 会用**它那个年代的** `DEFAULT_RUNTIME` 重写。
不崩溃、不丢 `envs`，但一个诚实的「未知」会被降级回一个猜测。

**这是降级路径的代价。它没有测试覆盖，只有发布说明 §7 那段文字** —— 写下来，
是因为「没测到」和「测过了」在文档里长得一样，而这正是本文 §9 那张表在说的事。

### 10.5 「#552 只需要修 CRLF」—— 不够

§6.3 说 `index_revision_` 的 packed-refs 分支漏剥 `\r`。真的，但那是表面。

实现时才发现 `get_repo_head_hash` **早就存在**，而且**认识两种索引形态** ——
git 工作树，以及没有 `.git` 的 artifact 安装（`.xlings-index-version`）。
新写的读取器对后者一律返回空，**会让整个特性对那些 home 静默失效**，
而 artifact 恰恰是 v0.4.52 之后的主流形态。

所以 #552 的问题不是「有个 CRLF bug」，是**它是第二个回答者** ——
本文 §9 那张表里的一行，我在写 §6 时只看见了它的症状。

---

## 11. 落地清单对照

| 方案项 | 状态 |
|---|---|
| 1 `Intent` + `BlockSpec` + `runtime_for()` | ✅ |
| 2 sysroot 观测源 | ✅（含悬空链接、musl、非 store 链接的单测） |
| 3 七站点归队 + `preserved_runtime` 删除 | ✅ 同 commit |
| 4 I6 放宽 | ✅ |
| 5 `make_block(BlockSpec)` + provenance 二选一 | ✅ 外加 `describe_block` 的创建记录承接（方案里没有，10.1 同源的反向错误） |
| 6 `SubosRuntimeUnknown` / `SubosRuntimeDrift` / `--from` warn | ✅ |
| 7 mcpp 侧文案 | ⏳ **另一个 repo，另一个 PR**（mcpp#427） |
| §5 #551 回滚 | ✅ 但按 10.1 重新定义 |
| §5 trash 出版本命名空间 + 真的清理 | ✅ |
| §5 五站点收敛 | ⏳ **未做** —— `subos remove` / `self uninstall` 仍在裸用 `remove_all`。函数已经可复用，收敛是独立 PR |
| §4 删 `forSearch` | ✅ |
| §6 #552 CRLF / advice 收窄 / 可测性 | ✅（并按 10.5 合并为一个读取器） |
| §7 Windows CI 拆 job | ✅ |
| §3 #549 控制台自检载体 | ⏳ **未做** —— D4 仍然开着 |
