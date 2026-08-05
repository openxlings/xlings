# 依赖解析的唯一权威:从"四个回答者"到"一次解析,处处引用"

**日期**: 2026-08-05
**类型**: 架构设计(architecture)
**触发**: 索引里出现第二个 glibc(2.44)后,`xlings install` 装出的二进制在 `main` 之前段错误
**状态**: 待评审
**所有数字与日志均为 2026-08-05 在隔离 `XLINGS_HOME` 中实测,未触碰 host 的 `~/.xlings`**

---

## 0. 一句话

同一个问题 —— "这个包的依赖 D 是哪个版本?" —— 在 xlings 与 libxpkg 里有**四个独立的回答者**。
它们平时给出同样的答案,所以谁都没错过;一旦索引里出现同一个包的第二个版本,答案开始分叉,
而分叉的产物是一个**在任何用户代码之前就崩溃、且没有任何诊断**的二进制。

本文不是给分叉打补丁,是**取消分叉的可能性**:让解析结果成为唯一权威,总体化、可追溯、
并由一条不变量在安装时把违背当场挡下。

---

## 1. 先把模型说准

之前的表述"一个 home 里同时存在两个 glibc 会出问题"是错的。三层必须分开:

| 层 | 能有几个 | 谁决定 |
|---|---|---|
| **xpkg store** `data/xpkgs/` | **多个** | 安装历史 |
| **subos sysroot** `<subos>/lib/libc.so.6` | **恰好一个** | `xlings use` / 安装时的激活 |
| **单个消费者的 RPATH/INTERP** | **每个应用各自一个** | elfpatch 在安装时写入 |

实测(隔离 home):

```
① store:            2.39  2.44
② subos sysroot:    glibc/2.44
③ gcc  INTERP=glibc/2.44  RUNPATH=glibc/2.44
   mesa            (lib)  RUNPATH=glibc/2.44
```

**多版本共存是设计本身允许的**,而且是推荐形态:消费者直接引用 payload 路径,
所以同一个 subos 里 A 应用钉 2.39、B 应用钉 2.44 是合法的。

问题从来不是"存在两个",而是 —— **同一个消费者的两半来自不同的两个**。

---

## 2. 实测的失败

隔离 home,store 里有 2.39 与 2.44,subos 的 active 是 2.39,
装一个 `deps = { "xim:glibc@>=2.39" }` 的包:

```
INTERP  = xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
RUNPATH = xpkgs/xim-x-glibc/2.44/lib64
```

运行:

```
symbol lookup error: .../glibc/2.44/lib64/libc.so.6:
  undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
```

`GLIBC_PRIVATE` 是 `ld.so` 与 `libc.so.6` 之间的**私有 ABI**,不对外承诺任何稳定性。
2.44 的 libc 向 2.39 的加载器要一个它不提供的符号。

**这条错误信息是本设计的物理依据**:`ld.so` 与 `libc.so.6` 不是两个可以分别选择的依赖,
它们是**一个不可分割的对象的两半**。任何允许它们被分别解析的机制,都是在等一次崩溃。

另一次实测(装配顺序不同)方向相反:`INTERP=2.44 / RUNPATH=2.39`,同样崩溃。
**两个方向都能复现**,说明这不是某一侧的偶然,是结构性的。

---

## 3. 根因:一个问题,四个回答者

"包 P 的依赖 D 解析到哪个版本 / 哪个目录",以下四处各自回答。

**授权它们各自回答的,是一行注释** —— `libxpkg/src/xpkg-executor.cppm:36`:

```cpp
// Pre-resolved exports of each runtime dep. Key is the dep spec as
// it appears in runtime_deps_list (e.g. "xim:glibc@2.39"). Only
// deps that actually declare exports show up; missing entries mean
// "this dep declared nothing — fall back to convention".
std::unordered_map<std::string, DepExport> deps_exports;
```

`DepExport` 只有三个字段:

```cpp
struct DepExport {
    std::string loader;                // 绝对路径,或空
    std::vector<std::string> libdirs;  // 绝对路径,或空
    std::string abi;
};
```

**"缺失即意味着去猜"就是四个回答者的授权书。** 本文六条改动的本质,
是把这个契约从"缺失即去猜"改成 **"缺失不可能发生"**。

### R1 — 解析器的 plan 节点(权威)
`xlings/src/core/xim/installer.cppm:2264`

```cpp
for (auto& depNode : plan.nodes) {
    if (!name_match || !dep_version_matches_(depNode.version, dep_ver)) continue;
    if (depNode.exports.loader.empty() && depNode.exports.libdirs.empty()) {
        break;                                   // ← 权威结果在这里被丢弃
    }
    auto depInstallDir = depRoot / effective_store_name_(depNode) / depNode.version;
    ...
    ctx.deps_exports[dep_spec] = std::move(e);   // ← 只带 exports 声明过的东西
}
```

**两个缺陷,都在这十几行里**:

- 依赖**没声明任何 exports** → `break`,`depInstallDir` 算出来了却扔掉
- 依赖声明了 `loader` 但**没声明 `libdirs`** → `e.libdirs` 为空

glibc 恰好是第二种。它的 recipe 明写:

```lua
exports = { runtime = { loader = "lib64/ld-linux-x86-64.so.2", abi = "...",
                        -- libdirs not declared → falls back to {lib64, lib} convention
} }
```

于是 **loader 走权威通道,libdir 走"约定"通道** —— 一个对象的两半,两条路。

### R2 — 目录扫描
`libxpkg/src/lua-stdlib/xim/libxpkg/pkginfo.lua` `_resolve_dep_via_scan`

0.0.49 之前:把版本**表达式**当目录名 join(`xpkgs/xim-x-glibc/>=2.39`),必然落空。
0.0.49 之后:精确名优先,否则取**已安装中满足表达式的最高版**。

### R3 — xvm 的 active 版本
同文件 `_resolve_dep_via_xvm` —— R2 落空时的兜底,答"这个 subos 当前激活的是谁"。

### R4 — `{lib64, lib}` 约定推导
`libxpkg/src/lua-stdlib/xim/libxpkg/elfpatch.lua` `closure_lib_paths`

```lua
local declared = deps_exports[dep_spec]
if declared and declared.libdirs and #declared.libdirs > 0 then
    ...                                  -- 用 R1
else
    dep_dir = pkginfo.dep_install_dir(dep_name, dep_version)   -- 用 R2/R3
    for _, sub in ipairs({"lib64", "lib"}) do ... end
end
```

**第五重不一致**:`dep_install_dir` 被不同调用方传入不同的版本参数。同一次安装的日志里:

```
scan dep=glibc ns=nil bare=glibc ver=2.39      ← 有的传解析后的
scan dep=glibc ns=nil bare=glibc ver=>=2.39    ← 有的传原始表达式
```

### 分叉表

| 场景 | R1(INTERP) | R2/R3(RPATH) | 结果 |
|---|---|---|---|
| 单版本 | 2.39 | 2.39 | 巧合一致 |
| 全新装,取最高 | 2.44 | 2.44 | 巧合一致 |
| `pin_target_to_active` 偏向已激活 | **2.39** | **2.44** | **崩** |
| 一次事务里装了两个版本 | **2.44** | **2.39** | **崩** |

**平时一致是巧合,不是保证。** 这是本设计要消除的性质。

---

## 4. 为什么 0.0.49 是 workaround

它只把 R2 从"必然落空"改成"取最高版"。当 R1 也取最高版时两者一致 —— 但
`pin_target_to_active` 的存在意味着 R1 **有意**偏向已激活的较低版本。
上一节第三行就是实测出来的反例。

**判据**:一个修复如果只是让两个独立答案"更容易相同",它就是 workaround。
只有取消"两个独立答案"这件事本身,才是解决。

---

## 5. 设计原则

1. **一次解析,处处引用**。解析只发生在一个地方(resolver / plan),其余全部**引用**,不得重新推导。
2. **权威记录必须总体化**。不能只记"声明过 exports 的依赖" —— 恰恰是没声明的那些走了野路。
3. **不可分割的对象整体解析**。`ld.so` + `libc.so.6` 是一个对象;
   `deps_exports` 必须让消费者无法只拿到其中一半。
4. **违背要在安装时响,不要在运行时崩**。
5. **可追溯优先于可猜测**。"为什么选了这个版本"必须是可查的数据,不是要靠读代码复现的推理。
6. **净删除**。方案的产出应当是**少一条代码路径**,不是多一条。

---

## 6. 方案

### 6.1 `resolved_deps`:总体化的权威记录

xlings 在构建 `deps_exports` 的同一次遍历里,为**每一个** runtime dep 记录一条,
与它是否声明 exports 无关:

```cpp
struct ResolvedDep {
    std::string spec;         // recipe 里的原文,如 "xim:glibc@>=2.38"
    std::string name;         // 规范名 "xim:glibc"
    std::string version;      // 解析出的具体版本 "2.44"
    std::string install_dir;  // 绝对路径,权威
    std::string source;       // 为什么是它:"plan" | "pinned-active" | "only-installed"
};
ctx.resolved_deps;            // spec -> ResolvedDep
```

`install_dir` 现在就在那行 `auto depInstallDir = ...` 里算着,
**只是被 `break` 丢掉了**。把 `break` 移到记录之后即可 —— 这不是新增能力,是不再丢弃已有结果。

### 6.2 libxpkg:`dep_install_dir` 变成查表

```lua
function M.dep_install_dir(dep_name, dep_version)
    -- ① 权威记录。安装上下文里必然存在,且已经是解析后的具体目录。
    local rec = _resolved_dep(dep_name, dep_version)
    if rec then return rec.install_dir end
    -- ② 仅在没有安装上下文时(工具脚本、离线调用)才扫描。
    return _resolve_dep_via_scan(dep_name, dep_version)
        or _resolve_dep_via_xvm(dep_name, dep_version)
end
```

**R2/R3 不再参与安装路径**。它们退化为"没有权威记录时的尽力而为",
且这种情况下必须 `log.warn` —— 让"走了野路"这件事本身可见。

### 6.3 `libdirs` 的默认值由 xlings 给,不由消费者推导

依赖没声明 `libdirs` 时,**xlings** 按 `{lib64, lib}` 约定填好绝对路径写进 `ResolvedDep`,
而不是让每个消费者自己去 `os.isdir` 试。

这样 `closure_lib_paths` 里的 `else` 分支**整个删掉** —— 原则 6 的净删除。

### 6.4 不可分割对象:loader 与 libdir 同源断言

这条要被实现成代码,所以写成无歧义的规格。

#### 断言的对象

一个真实的、坏掉的 `gcc`(隔离 home 里构造出来的),它的两个 ELF 字段:

```
INTERP                                          ← 只有一个,绝对路径
  <store>/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2

RUNPATH                                         ← 一串,按序搜索
  <store>/local-x-gcc/16.1.0/lib64
  <store>/xim-x-glibc/2.44/lib64                ★
  <store>/xim-x-binutils/2.42/lib
  <subos>/pin239/lib
```

- **INTERP**:内核 `execve` 时读它,决定用哪个动态加载器启动这个程序
- **RUNPATH**:加载器按序在这些目录里找 `DT_NEEDED`(`libc.so.6`、`libm.so.6` …)

#### "payload 目录"与"provider"的定义

store 布局固定为 `data/xpkgs/<store-name>/<version>/`,所以从任意一条绝对路径截出前两段,
就得到它属于哪个 payload;`<store-name>` 就是 provider:

```
<store>/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
        └ provider ┘└ver┘
        └──── payload 目录 ────┘
```

#### 断言

```
p_interp := payload_of(INTERP)                       → <store>/xim-x-glibc/2.39
provider := provider_of(p_interp)                    → xim-x-glibc
候选     := { payload_of(e) | e ∈ RUNPATH, provider_of(e) == provider }
                                                     → { <store>/xim-x-glibc/2.44 }
要求     := 候选为空,或 p_interp ∈ 候选
```

上例中候选 = {2.44},不含 2.39 → **FAIL**。

一句话:**这个程序被规定用 2.39 的加载器启动,却被规定去 2.44 的目录里找 libc。**

#### 为什么这一条就够

`ld.so` 与 `libc.so.6` 是同一次编译产出的两半,之间有一组不对外承诺的私有符号。
实测崩溃正是这个:

```
undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
```

2.44 的 libc 向 2.39 的加载器要一个后者不提供的符号。
**两半同源,这类问题在物理上不存在;不同源,就是在等崩。**
所以这条断言不是启发式,是"这对对象不可分割"这个事实的直接表达。

#### 边界(必须按此实现)

| 情况 | 处理 | 理由 |
|---|---|---|
| 共享库(`.so`,无 INTERP) | SKIP | 库不决定加载器,由主程序决定 |
| 静态二进制(无 INTERP) | SKIP | 没有加载器可言 |
| RUNPATH 里没有该 provider | PASS | 只有出现了才要求一致 |
| RUNPATH 里该 provider 出现多次 | 有一条同源即 PASS | 加载器按序取首个命中,同源在列即可 |
| `$ORIGIN` 等相对项 | 跳过 | 不含 `/xpkgs/<provider>/` |
| 非 store 路径(如 `<subos>/lib`) | 跳过 | 不是 payload |

#### 触发点

- **安装时**(elfpatch 写入前):不满足则**中止安装**,打印两个具体路径和各自来源
- **doctor**(§6.6):遍历存量,同一份实现

#### 已实测,不是设想

`.agents/tools/check-loader-libc-same-source.sh` 已按上述规格实现:

```
构造的分叉包           FAIL   INTERP → glibc/2.39 , RPATH → glibc/2.44
正常包                 OK     两边都是 glibc/2.44
三个已装生态全量扫描    196 个二进制,OK=196  FAIL=0  误报 0
```

零误报是它能升级为 doctor 规则的前提 —— 会误报的检查用不了几次就被当成噪音跳过,
那和没有这条检查是一样的。

#### 与现状的对比

| | 现在 | 有断言之后 |
|---|---|---|
| 现象 | `undefined symbol: __pointer_chk_guard` | 安装中止,打印两个路径 |
| 定位成本 | 要 `LD_DEBUG=libs` 逐层追 | 直接可读 |
| 发生时机 | 用户运行时,可能几周后 | 安装当场 |

成本:一次字符串比较。

### 6.5 可追溯:解析结果落盘

每次安装把 `resolved_deps` 写进包的安装记录(`<install_dir>/.xlings-resolution.json`):

```json
{ "package": "xim:gcc@16.1.0", "installed_at": "...",
  "deps": [ { "spec": "xim:glibc@>=2.39", "version": "2.39",
              "install_dir": ".../xpkgs/xim-x-glibc/2.39",
              "source": "pinned-active" } ] }
```

配套一条命令:

```
xlings why <pkg> <dep>
  xim:gcc@16.1.0 → xim:glibc@>=2.39
    解析为 2.39   (source: pinned-active —— 该 subos 已激活 2.39)
    install_dir  .../xpkgs/xim-x-glibc/2.39
    INTERP       .../2.39/lib64/ld-linux-x86-64.so.2   ✓ 同源
    RPATH        .../2.39/lib64                         ✓ 同源
```

**可复现性**:同一个 plan + 同一个 store 状态 → 同一份 `resolved_deps`。
落盘之后,"当时为什么选了它"不再需要复现环境去推。

### 6.6 doctor:把不变量变成常态检查

`xlings doctor` 增加一条:遍历已安装包,对每个 ELF 检查
INTERP 与 RPATH 中同一 provider 的条目是否同源;不同源则报告并给出 `--fix`(重装该包)。

这条同时覆盖**历史遗留**的错配 —— 本文第 2 节那个 gcc 就是被这样发现的,
但发现方式是"跑起来崩了",不是"doctor 说了"。

---

### 6.7 六条之间的关系

```
①②③  取消分叉的可能性        ← 正确性(架构)
④     不信任自己,当场拦下     ← 稳定性(不变量)
⑤     决策变成可查的数据       ← 可追溯 / 可复现
⑥     覆盖已经装出去的存量     ← 一致性(存量与新装同一标准)
```

①②③ 只保护**将来**的安装 —— 已经装在用户机器上的错配不会自愈,
而这次事故里唯一发现它的方式是运行时崩溃。⑥ 是覆盖存量的那一层。

**净效果是代码路径减少一条**(6.3 删掉 `else`),不是增加。
这是判断它是不是 workaround 的最直接标准。

**doctor 的一条纪律**(本仓库有过至少三次教训):
报告的规则和 `--fix` 执行的规则必须是**同一份实现**,不能各写一遍。
两者漂移的表现是"doctor 报了,修完还报",或者反过来。

---

## 7. 改动清单

| 位置 | 改动 | 性质 |
|---|---|---|
| `installer.cppm:2264` | `break` 移到记录之后;补 `libdirs` 约定默认值 | 不再丢弃已有结果 |
| `installer.cppm` ctx | 新增 `resolved_deps` | 新字段 |
| `pkginfo.lua` `dep_install_dir` | 先查 `resolved_deps` | 查表优先 |
| `elfpatch.lua` `closure_lib_paths` | 删除 `else` 重新推导分支 | **净删除** |
| `elfpatch.lua` `_apply` | 同源断言 | 新不变量 |
| `installer.cppm` | 写 `.xlings-resolution.json` | 可追溯 |
| `cli` | `xlings why` | 可查 |
| `doctor.cppm` | 同源检查 + `--fix` | 常态化 |

**libxpkg 0.0.49 的 `_version_satisfies` 保留**,但作用域收窄为"无安装上下文时的兜底",
并在走到它时 warn。它不再是安装路径的一部分。

---

## 8. 验收判据

每条都必须可执行、可复现,且**在修复前是失败的**:

- **A1 分叉不可构造**:在 store 有 2.39/2.44、subos active 为 2.39 的隔离 home 里,
  装 `deps = {"xim:glibc@>=2.39"}` 的包 → INTERP 与 RPATH 同源。
  (修复前:`INTERP=2.39 / RPATH=2.44`,实测已复现)
- **A2 反向也不可构造**:一次事务装两个版本 → 同源。
  (修复前:`INTERP=2.44 / RPATH=2.39`,实测已复现)
- **A3 断言先于崩溃**:人为破坏一个包的 RPATH 后重装 → 安装中止并打印两个路径,
  而不是装完在运行时 `GLIBC_PRIVATE` 崩。
- **A4 可追溯**:`xlings why gcc glibc` 输出版本、来源、两个路径,且与磁盘一致。
- **A5 doctor 能发现历史遗留**:把本文第 2 节那个 home 交给 doctor → 被报告出来。
  (判据脚本已就绪并零误报,见 §6.4)
- **A6 单版本零回归**:只有一个 glibc 的 home,行为与现在逐字节一致。

---

## 9. 兼容与迁移

- **老 recipe 不动**。`resolved_deps` 是 hook 运行时新增的字段,recipe 感知不到。
- **老客户端**:`.xlings-resolution.json` 是新文件,老客户端忽略它。
- **能力探测**:libxpkg 里 `type(_RUNTIME.resolved_deps) == "table"`,
  不是 `if _RUNTIME.resolved_deps then` —— 未知字段在 stub 上恒真,
  这个陷阱本仓库已经踩过两次(`subos.env`、`xim.pkgindex.sysroot`)。
- **发布顺序**:libxpkg(读) → xlings(写 + 断言) → 索引可放心用范围依赖。
  顺序反了会让断言在没有 `resolved_deps` 的客户端上误报,所以**读端先行**。

---

## 10. 这个方案顺带解决的

- **索引 16 处 glibc 钉死可以放宽**。当前不敢放,是因为放了就可能分叉;
  分叉不可构造之后,`>=` 成为安全的默认写法,
  于是"新 glibc 能装载更多既有二进制"这个收益才真正可用。
- **`--runtime` 的语义闭环**。subos 声明的运行时进入解析的 `pinned-active` 来源,
  `xlings why` 能直接答"为什么这个 subos 里的包都用 2.39"。
- **同类缺陷的通用形状**:本仓库反复出现"两个读者读同一份状态"
  (doctor 报告 vs `--fix` 执行、`use` 切换 vs payload 的 INTERP、
  header 声明 vs 安装期物化)。本文的 §6.4 断言与 §6.5 落盘是这一类的通用解法:
  **让两个读者变成一个记录 + 一条断言**。
