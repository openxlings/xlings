# sysroot / bin / lib / 文件 的多版本共存与切换 —— 架构设计

**日期**: 2026-07-27
**类型**: 设计 (design)
**状态**: 待评审。**不阻塞 0.4.70** —— 下述问题均非 0.4.70 引入
**关联**: [#408](https://github.com/openxlings/xlings/issues/408)、`2026-07-26-xvm-group-transaction-design.md`
**涉及仓库**: `openxlings/xlings`、`mcpplibs/libxpkg`（xpkg spec）、`openxlings/xim-pkgindex`

---

## 0. 摘要

0.4.70 让 `xlings use` **整组切换程序**。但同一个模型下的**头文件和库并没有真正被管理**：

- **库**：Catalog / Selection 里是一等公民，但 `VData::libdir` **没有任何写入者**，`switch_plan` 永远不产生 lib 变更 → **`use` 对库是 no-op**
- **头文件**：模型里根本没有这个概念 → 索引自己长出了 **7 种写入方式**

本文提出：**把资产收敛成两类（`program` / `files`），把物化收敛成一条路径，把 `config()` 收窄成纯声明。**

**结论先说**：整套架构成立，但**不建议一次做完**。第 9 节给出四阶段方案，每一阶段都由证据而非计划推动。**第一阶段就能修掉全部已知的用户可见问题**，而后三阶段都应该等待证据。

---

## 1. 现状

### 1.1 sysroot 有 7 个写入者，语义互相矛盾

| 机制 | 位置 | 语义 | 谁在用 |
|---|---|---|---|
| `xvm::install_headers` | `src/core/xvm/commands.cppm` | symlink，**覆盖** | header：**0 个 recipe** |
| `Library` effect | `src/core/xim/installer.cppm` | symlink，覆盖，**无 active 门禁** | 所有 `xvm.add(type="lib")` |
| `sysroot.install_headers` | `xim-pkgindex/libs/sysroot.lua` | symlink，**skip-if-exists** | glibc / linux-headers / openssl / python |
| `os.cp` 裸拷贝 | 各 recipe | **复制**，覆盖 | zlib / libxml2 / libffi / harfbuzz / libpng / cairo / glib … |
| `gcc-specs-config` | recipe | **改写 gcc payload 内的 `specs`** | gcc |
| `elfpatch` | libxpkg | 改写 payload 内 ELF 的 RPATH / interpreter | 10+ recipe |
| llvm `clang.cfg` | recipe | 生成 cfg，**固化 dep 的 payload 路径** | llvm |

两点必须点名：

- **`os.cp` 是复制不是链接** —— 副本连指针都不是，对它谈"版本切换"没有意义；卸载靠 recipe 自己 `os.tryrm("zlib.h")` 一个硬编码文件名列表
- **`gcc-specs-config` 改写别的包的 payload** —— `(name, version) → payload` 不再是函数，**Catalog 不可变性被破坏**（真实机器上可见 `.specs-rewritten-16.1.0-payload.stamp`）

### 1.2 三类资产的完整度

| 资产 | Catalog | Selection | 物化 | 切换 |
|---|---|---|---|---|
| **program** | ✅ | ✅ | shim（运行时 dispatch） | ✅ **由构造正确** |
| **lib** | ✅ | ✅ | 安装时写一次 symlink | ❌ `VData::libdir` **无写入者**；`install_libs()` **零调用点**（死代码） |
| **header** | ❌ | ❌ | 7 种野路子 | ❌ 模型里没有它 |

### 1.3 可观察的后果（openssl）

```
install openssl@3.1.5   库 → 3.1.5   头 → 3.1.5
install openssl@3.2.0   库 → 3.2.0（Library effect 覆盖，无 active 门禁）
                        头 → 3.1.5（Lua skip-if-exists，跳过）
                        ↑ 装完就错配，与 use 无关

use openssl 3.1.5  →  workspace 3.1.5 | 头 3.1.5 | 库 3.2.0   ✗
use openssl 3.2.0  →  workspace 3.2.0 | 头 3.1.5 | 库 3.2.0   ✗
```

**两个方向都错，`use` 两次都报成功。** 症状是编译期与链接期不是同一个版本 —— 不报错，运行时才炸。

影响面：**同一个包装了 ≥2 个版本、且提供头文件或库**。官方索引里为 openssl / glibc / python / linux-headers。只装一个版本、或纯 program 包（索引里绝大多数）不受影响。

### 1.4 幻影节点：doctor 的锚点误报（真实数据）

`cairo.lua` 写 `xvm.add(package.name)`，`type` 未设 → C++ 侧默认成 `"program"`。而 cairo payload **连 bin 目录都没有**。

在一台真实机器（372 条目、246 target）上，`self doctor` **实测**报出 **9 条**
`✗ broken payload`，其中 **2 条是锚点节点**（`binutils@2.42`、`cairo@1.18.0`）——
它们没有可执行文件是设计使然，而给出的修复建议 `xlings install <pkg>@<ver>` 修不了。

> **一处更正**：本文早先版本写的是"31 条假警报"。那是一个脚本的口径（program 类型、
> payload 根目录下没有同名可执行文件），**不是 doctor 的输出**。doctor 走
> `resolve_executable`，会一并搜 `bin/` 子目录，所以那 31 条里绝大多数能解析、从未被报出。
> 实测口径是 **9 报 2 误**。

**根因**：recipe 想说"这只是一个挂发布的名字"，**模型里没有这个概念**，于是它撒谎说这是个程序。C++ 里其实有 `group` kind（`supported_kind_` 接受），但 Lua 的默认值让它不可达。

### 1.5 根因三条

1. **不变量写在了错误的抽象层。** `INV-3: Materialization = f(Selection, Catalog)` 对 program 是**恒等式**（shim 在 exec 时解析，无状态需同步）；对 header/lib 只是一个**缓存一致性义务**（编译器直接 `open()`，`ld.so` 直接 `open()`，xlings 不拥有解析点）。写成恒等式，"谁来兑现"就从未被提出 —— ledger 被排到 P3.1 而非前置条件，是这个建模错误的直接推论。

2. **物化策略没有唯一所有者。** 七个实现、两个仓库、三种语言、两种互相矛盾的冲突策略。而第三种（Lua skip-if-exists）存在的原因，**正是第二种（xvm overwrite）表达不了它需要的策略**。

3. **Selection 被复制进别人的产物。** RPATH / `clang.cfg` / `specs` 里存着"当前哪个 glibc"的副本。状态模型里**没有 "Selection 依赖 Selection" 这条边**，所以切 glibc 时既不能重算也不能报警。

---

## 2. 目标架构

### 2.1 分界线：谁在解析

| | `program` | `files` |
|---|---|---|
| 谁做解析 | **xlings** —— shim 在 `exec` 时读 Selection | **别人** —— 编译器 / `ld.so` 按路径查找 |
| **物化是否依赖 Selection** | **否** —— shim 是 xlings 二进制的 hardlink，与版本无关，建一次永不换 | **是** —— 每版本内容不同，切换必须换文件 |
| 切换成本 | 改一个 JSON 字符串 | 重指 N 个链接 |

这条线由 **OS 接口逼出来**，不是分类学偏好：`exec` 走一个 xlings 拥有的文件，`#include` 和 `DT_NEEDED` 不走。

**库是 `files` 的一种**，`dst = usr/lib/<soname>`。代码里硬编码的 `sysroot_lib` 因此消失。

### 2.2 什么该是 target

判据只有一条：

> **能被单独 `xlings use` 的东西，就能被单独切走。**

但 0.4.70 之后，`plan_use_switch` 先 `resolve_binding_selection`，**从任何成员进入都解析出整组** —— 所以多一个 target **不会**造成 INV-2 违规。

因此 `files` 节点可以走 `xvm.add`，保持 API 兼容：

```lua
xvm.add("openssl",              { bindir = bindir, binding = tag })
xvm.add("libssl.so.3",          { type = "lib",   bindir = libdir, binding = tag })
xvm.add("openssl_xvm_files",    { type = "files", src = "include/openssl",
                                  dst = "usr/include/openssl", binding = tag })
```

带 `type = "files"` 的节点比今天的幻影**严格更好**：doctor 知道不该找可执行文件 → 锚点误报从根上消失。

约束：
- 名字用保留形态（`<pkg>_xvm_files`），防止与真实包碰撞
- `xlings list` 中隐藏 —— 它不是用户要 use 的入口

### 2.3 三个写入目标桶

| 桶 | 内容 | 归属 |
|---|---|---|
| **① subos 内** | headers / libs / `etc/ssl` → `subos/<n>/...` | ✅ **xlings 管** |
| **② payload 内** | gcc 改写 `specs`、llvm 写 `clang.cfg`、elfpatch 改 RPATH | ⚠️ **决策点 D2** |
| **③ subos 外 / host** | desktop entry(4) / 字体(1) / fontconfig(1) / pmwrapper | ❌ **不管，recipe 自理** |

③ 的实测代价接近零 —— 只有 6 个 recipe，**而且它们已经在自己管、且管得对**（`code.lua` 用版本戳文件名 `vscode.<version>.xvm.desktop` 避免碰撞，uninstall 自己 `os.tryrm`）。`pmwrapper` 那类**本来就不该 xlings 管** —— apt/pacman 有自己的数据库，再记一份必然对不上。

文档需明确写死：**subos 外的文件不参与版本切换、不被 `doctor` 检查、不随卸载自动清理。**

### 2.4 路径都是相对的

```jsonc
{ "src": "include/openssl",     // 相对 payload 根
  "dst": "usr/include/openssl", // 相对 subos 根（白名单内）
  "how": "link" }               // link | copy（copy 记 sha256）
```

**两端都相对**，所以这条记录既与 subos 无关，也与 home 无关 —— 整个 `~/.xlings` 搬走、payload 被多个 subos / 多个 home 共享，都仍然有效。

这一条**必须由 API 保证而非靠自觉**：声明接口不接受绝对路径，让错误**不可表示**。（payload 跨 subos 共享有引用计数，见 `tests/e2e/subos_payload_refcount_test.sh`；写绝对 dst 会造成跨 subos 污染。）

**dst 白名单**：

```
✅ usr/**      headers / libs
✅ etc/**      ca-certificates 那类
❌ bin/**      shim 的地盘，xlings 独占
❌ ../** /**   任何逃逸
```

### 2.5 物化收敛成一条路径

**决策：install 不物化，只有 use 物化。**

```
materialize(Selection, Catalog) → filesystem      ← 唯一入口
```

| | 现在 | 之后 |
|---|---|---|
| 物化代码路径 | **三条**：install effects / `cmd_use` / 删除后的 fallback 重物化 | **一条** |
| `active` 门禁 | `InstallHeaders` 有，`Library` 没有（0.4.70 的漏洞） | **概念消失** —— 非激活版本根本不物化 |
| 装完能用 | effects 直接建 | `useAfterInstall`（**0.4.70 已有此机制**） |
| 删除后回退 | 0.4.70 专门写了一段"把幸存发布的头文件放回去" | 重算 Selection → 调同一个函数 |

**`INV-3` 第一次从口号变成一个真实存在的函数。** 而 0.4.70 给 `InstallHeaders` 加的 `active` 门禁、以及那段 fallback 重物化，都会变成**不必要的补丁** —— 再次印证它们打在了错误的层上。

### 2.6 切换 = 全删全装 + 逐条原子替换

语义上"先删当前版本、再装目标版本"，**不做差集** —— 差集是 bug 的温床，而每个版本都有自己的声明，文件数量不同也无所谓。

实现上用原子替换消除窗口：

```c
symlink(new_src, "dst.tmp");
rename("dst.tmp", dst);        // POSIX rename 原子覆盖已存在的 symlink
```

- 语义是"全删全装"（简单、显然正确）
- 每个条目**没有窗口** —— 0.4.70 加 `fs::equivalent` 跳过就是为了避免"先删后建"让并发编译看到头文件消失
- 只有"旧版有、新版没有"的条目才是真正的 `unlink`

Windows：文件可 `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`，**目录 junction 没有原子替换** → 那里接受窗口（决策点 D5）。

### 2.7 `merge` 不是运行时模式

我们一度把两件事混成一个词：

| | 是什么 | 需要吗 |
|---|---|---|
| **声明期展开** | "把 `include/` 下的东西各自放进 `usr/include/`" → 展开成 130 条 | 方便，可选 |
| **运行时模式** | 物化时判断"这条是 merge 还是 mount" | ❌ **不需要** |

因为**存的永远是展开后的显式条目**。展开在声明时做完，切换时只有一种操作：把 src 放到 dst。

副作用：**包内冲突也在展开时当场可见**，不需要运行时仲裁。

### 2.8 记录粒度：实际物化的那一层

以 glibc 为例（真实数据：`include/` 顶层 130 项，递归 477 个文件）：

| 记什么 | 条数 | 是不是"槽位" |
|---|---|---|
| 130 个顶层链接 | 130 | ✅ **真正被占用的槽** |
| 递归 477 个文件 | 477 | ❌ 400+ 在 glibc 自己的子树里，不占共享命名空间 |

**记录"实际创建了哪些 dst 条目"，不多不少。**

zlib 那种把 `zlib.h`/`zconf.h` **直接**放进共享目录的，它们本身就是顶层槽 → 各一条。glibc 的 `sys/` 是一个指向目录的槽，里面的内容不是槽。**冲突检测天然发生在正确的粒度上。**

### 2.9 hook 生命周期

```lua
function install()      -- payload 落盘（不变）
function config()       -- 【收窄】只做声明：xvm.add(...)
                        --        不查环境、不写文件、不改 payload
function use()          -- 【新】切换后跑。此刻 Selection 确定
function uninstall()    -- 【大概率可整个省掉】provider-scoped 卸载已在 0.4.70
```

`config()` 今天混了三件事（以 gcc 为例）：

```lua
local glibc_lib = __find_glibc_runtime()      -- ② 查环境
__rewrite_specs_linux(rpath, dynamic_linker)  -- ② 生成产物（写进 payload）
xvm.add("xim-gnu-gcc"); for ... xvm.add(...)  -- ① 声明注册 × 22
sysroot.install_headers(...)                  -- ③ 物化（openssl）
```

拆分去向：① 留在 `config()`；② 移到 `use()`；③ 由 xlings 做。

**不能直接"每次切换跑 config()"** —— 那会把 ① 一起带进来：`use` 会改 Catalog、会撞重复注册、会让 plan-then-execute 失效，而且 config() 的语义是"刚装好配置一下"（可能有一次性工作），与"选择变了刷新派生状态"不同。

`use()` 的设计要点：

1. **跑在提交点之后**

```
plan_use_switch()   纯决策，可预检、可拒绝            ← 0.4.70 已有
materialize()       确定性、全删全装、原子替换         ← 新
run use hooks       任意 Lua                         ← 新，在提交之后
```

hook 失败 = "切换成功但某个配置没重新生成"，**降级而非撕裂**，再跑一次 `use` 即修复。

2. **跑谁的** —— 被切换的 group + 任何 `use_deps` 命中变化的 active group：

```lua
use_deps = { "glibc", "linux-headers" }
```

这就是**那条缺失的 Selection→Selection 边**，而且形式比模板好：它是**数据**（名字列表），不是嵌在字符串里的语法。顺带让 `xlings use glibc 2.40` 能告诉你"这会影响 clang、gcc"。

3. **只能写 subos 内**，同一个白名单
4. **产物要能追踪** —— hook 只能写它声明过的 `files` 条目（标 `generated = true`）
5. **必须幂等**

### 2.10 `args`：只保留静态变量

`shim.cppm:451` 今天已有注入机制，但塞在 `alias` 字段里、按**第一个空格**切分（路径含空格就废）。

| 变量 | 依赖 Selection | 归谁 |
|---|---|---|
| `${subos}` | ❌ | **`args` 保留** —— gcc 的 `--sysroot=` 够用 |
| `${self}` | ❌ | **`args` 保留** |
| `${dep:x}` | ✅ | **交给 `use()`** |

这样 `args` 只剩两个静态替换，**不需要 DSL 的复杂部分**（无条件、无缺失处理、无嵌套），而依赖 Selection 的复杂逻辑全在 Lua 里，表达力充分。且 `${subos}`/`${self}` 是纯函数，声明仍可安全共享。

`args` 改为**数组**，消除现在按空格切分的脆弱性。

### 2.11 幻影节点的两条职责及其拆解

幻影不是随手加的，它承担两个职责（`registration.cppm:377` 强制根必须是 batch 内的真实节点；根还是 `bindingMembers`/`bindingHeaders` 的存放处）：

| 职责 | 解法 |
|---|---|
| **清单存放处** | → 展开结果搬到声明之外（见 D3） |
| **group 的名字** | → label 显式声明，默认取包名，**不再要求有对应节点** |

两条都解掉后，根节点没工作了 → 幻影消失 → 那 2 条误报随之消失。

**顺带修掉 #406 撞到的坑**：`xvm.setup` 调两次报重复注册，是因为每次都 `M.add(name)` 造根节点。根不再需要之后，同 label 的两次声明就是合并。

### 2.12 术语（避免继续混用）

| 词 | 含义 | 代码里 |
|---|---|---|
| **batch** | 一次 `config()` 的全部注册 | `RegistrationBatch` |
| **group** | `(provider, providerVersion, label)`，**切换的最小单位** | `BindingGroupRef` |
| **label** | group 的名字，今天默认取 rootTarget | `BindingGroupRef.group` |
| **root** | 存清单的节点 —— **提议取消** | `rootTarget/rootVersion` |

前文用过的 "release" 一词与 group 同义，后续统一用 **group**。

---

## 3. 稳定性

### 3.1 不变量与保证方式

| 不变量 | 现在怎么保证 | 之后 |
|---|---|---|
| INV-1 激活的版本必须已注册 | 校验 | 不变 |
| INV-2 group 成员全激活或全不激活 | `plan_use_switch` 预检拒绝（**可表示，靠拒绝**） | 同左；若采纳 D6（Selection 改 release-keyed）则**不可表示** |
| INV-3 物化 = f(Selection, Catalog) | **没有任何东西在保证** | **收敛成一个函数**，是唯一物化入口 |

### 3.2 失败模式

| 场景 | 行为 |
|---|---|
| `plan` 阶段失败（成员缺失、group 不可解析） | 磁盘零改动 + 具名原因 + hint（0.4.70 已有） |
| `materialize` 中途崩溃 | 部分条目已换。**下一次 `use` / `doctor --fix` 全删全装即修复**（幂等） |
| `use()` hook 失败 | 物化已提交，只是派生产物陈旧。**降级而非撕裂**，重跑 `use` 修复 |
| 声明与 payload 不符（recipe 改过） | 展开时发现，报错并指出条目 |
| dst 冲突（跨包 / 包内） | **声明/展开时**发现（决策点 D4 定策略） |

### 3.3 并发

0.4.70 的 home 级状态锁已覆盖 `install`/`remove`/`use`，#405 又扩到 subos/config/self install。物化在锁内进行，**本设计不引入新的并发面**。

### 3.4 本设计**削减**的复杂度

| 项 | 现在 | 之后 |
|---|---|---|
| sysroot 写入者 | 7 | **1** |
| 物化代码路径 | 3 | **1** |
| `active` 门禁 | 需要（且 `Library` 漏了） | **概念消失** |
| 幻影锚点节点 | 2 条误报为 broken payload | **正确归类** |
| 冲突策略 | 2 种互相矛盾 | **1 种** |
| 属性 `mode` / `switch` | 讨论中一度需要 | **均已删除** |
| gcc 手写 binding tag | **22 处**，漏一处即静默半切换 | 隐式 |

---

## 4. 性能

### 4.1 实测数据（真实安装）

```
~/.xlings/.xlings.json      107.6 KB / 246 targets / 372 (target,version) / 平均 296 B/条
payload 顶层扇出（极度倾斜）:
    glibc/include            顶层 130   递归  477
    musl-gcc/…/include       顶层 108   递归 1944
    binutils / linux-headers 顶层  11
    zlib / libffi / expat    顶层   2
    openssl / gcc            顶层   1     ← 大多数
```

### 4.2 各操作成本

| 操作 | 成本 |
|---|---|
| 切换一个包（openssl，顶层 1 项） | **1 次 symlink + rename** |
| 切换 glibc（顶层 130 项） | ~130 × (symlink+rename) ≈ **亚毫秒** |
| 展开一个声明（`readdir` payload 目录） | glibc 130 个 dirent ≈ **微秒级** |
| 全量物化（50 包的 subos） | 几千次系统调用 ≈ **个位数 ms** |

**都不是瓶颈。**

### 4.3 为什么不做 dispatch 时注入 flag

| | 切换成本 | **每次编译成本** |
|---|---|---|
| 物化到 sysroot | 一次几 ms | **0** |
| dispatch 注入 `-isystem` | 0 | 50 包 → 100 条路径；GCC 头文件查找是**每条 `#include` × 路径数**；一个 TU 50 个 include × 100 条 = 上千次 stat，**每次编译都付** |

**成本方向是错的。** Nix 能用注入是因为**每次构建只看见声明的依赖**（`buildInputs = [openssl zlib]` 就 2 个），而 xlings 没有"每次构建的依赖范围"这个概念 —— 一次编译看见的是"装了什么"。

### 4.4 已知热路径问题（既有，与本设计无关）

**`shim.cppm:347` 的 `Config::versions()` 没有缓存** —— 每次 dispatch 解析整个版本库（真实安装 107 KB）。一次大型构建有上万次编译器调用。

尝试量化时 `list --all` 会去刷索引而卡住，**只有代码事实，没有数字**。建议单独立项测量 + 加缓存。**这也是不把模板解析放进 dispatch 的另一个理由。**

### 4.5 递归展开为什么不可行（历史教训）

`libs/sysroot.lua` 的注释记录了原因：glibc 的 20 个子目录与 Ubuntu host `/usr/include` 重叠（光 `sys/` 就 87 个 host 条目）时，递归走法涨到 **500+ 次操作，打爆 proot 的 talloc pool，同 session 里的 npm install 直接崩溃**。

**顶层非递归是为此付出的代价，不是偷懒。** §2.8 的"只记实际物化的那一层"与之一致。

---

## 5. 决策点

| # | 决策 | 选项 | 倾向 |
|---|---|---|---|
| **D1** | **install 是否物化** | (a) 物化（现状，需 `active` 门禁） / (b) **不物化，只有 use 物化** | **(b)** —— 已在讨论中确认 |
| **D2** | **payload 内改写**（specs / clang.cfg） | (a) 承认，作为一类声明 / (b) **移进 subos，`use()` 生成** / (c) 移进 generation | **(b)**；elfpatch 的 RPATH **保留**（指向 payload = Catalog 引用，不可变，正确） |
| **D3** | 展开结果存哪 | (a) **物化时 `readdir` 现算** / (b) 存 `xpkgs/<n>/<v>/.xpkg/files`（与已有 `.xpkg.lua` 同级） | 阶段一 **(a)**；(b) 作为阶段二，见 §9 |
| **D4** | 跨包 dst 冲突策略 | (a) **报错 + 显式优先级** / (b) 深度合并 | **(a)** —— (b) 是唯一强制 generation 的需求 |
| **D5** | Windows 原子性 | (a) **接受窗口**（就地替换） / (b) 配置指针 / (c) 逐项 junction | **(a)** + 文档写明平台差异 |
| **D6** | Selection 是否改 release-keyed | (a) 保持 target-keyed / (b) 改 release-keyed，**INV-2 变得不可表示** | (b) 更好，但**是状态格式变更**，必须 0.5 线 |
| **D7** | 是否放弃"回滚到 0.4.69" | 取消根节点会打断 legacy 成对边双写（`registration.cppm:831`，星型拓扑，无根即无边） | 到 0.5 线时**显式宣告放弃**，不要默默失效 |
| **D8** | 深度合并要不要支持 | 决定 generation 是必需还是可选 | **需要真实冲突案例才能定** |

---

## 6. 无感升级

### 6.1 用户侧契约

| # | 契约 |
|---|---|
| U1 | `xlings self update` 后无需任何手工步骤 |
| U2 | 旧 `~/.xlings.json` 直接可读，**无格式迁移**（除非采纳 D6，那属于 0.5 线并需要独立的迁移设计） |
| U3 | 已有 shim 继续工作，不需要重建 |
| U4 | 未迁移的 recipe 行为**完全不变** |
| U5 | 已装的包在升级后仍然激活、版本不变 |

### 6.2 xpkg spec 版本门禁

**现有先例可直接沿用。** `installer.cppm:1715`：

```cpp
// Fail-closed arch gate: ... Gated on spec >= "2": in V1 the `archs` field
// was never enforced and is frequently under-declared ... V2 authors opt
// into correct per-arch declarations and want this enforced.
if (pkg->spec == "2" && !pkg->archs.empty()) { ... }
```

**spec 就是一个逐包的"选择加入"开关**，不是全局切换。真实索引里 `spec = "1"` 112 个、`spec = "2"` 6 个 —— 两代并存，运转正常。

提议 **`spec = "3"`**，语义：

| | spec ≤ 2（现状） | **spec = 3** |
|---|---|---|
| `config()` | 可查环境、可写文件、可改 payload | **必须纯声明**（接口不提供环境查询） |
| `type = "files"` | 不可用 | 可用 |
| `use()` hook | 忽略 | 生效 |
| 物化 | 老路径（effects + recipe 自己写） | **xlings 统一物化** |
| dst | 无约束 | **相对 subos 根 + 白名单** |

**两代并存，不设截止日期。** 未迁移的包一行不改，行为一字不变。

### 6.3 兼容读窗口

| 数据 | 处理 |
|---|---|
| 旧 DB 里的 `type = "lib"` 节点 | 继续按 lib 物化（老路径） |
| 旧 DB 里的幻影 program 节点 | 识别为 group 锚点，**doctor 不再报 broken payload**（这一条可以立刻做，不依赖任何其它改动） |
| legacy 成对边 | 继续双写，直到 D7 决定放弃 |

---

## 7. xim-pkgindex 迁移

### 7.1 规模

**118 个 recipe，其中 28 个碰 sysroot 或改 payload。** 其余 90 个纯 program 包**一行都不用改**。

### 7.2 分类与迁移方式

| 类 | 数量 | 现状 | 迁移 | 难度 |
|---|---|---|---|---|
| **A. 纯 program** | ~90 | `xvm.add` | **不动** | — |
| **B. 独占目录的头文件** | openssl, libxml2… | `sysroot.install_headers` / `os.cp` | 换成 `type="files"` 一条声明 | **低** |
| **C. 散落文件** | zlib, libffi, expat（顶层 2 项） | `os.cp` 两个文件 | 声明展开源目录 | **低** |
| **D. 大批量 merge** | glibc(130), musl-gcc(108), linux-headers(11), binutils(11) | Lua skip-if-exists | 声明 + 展开；**冲突策略要先定（D4）** | **中** |
| **E. 改 payload** | gcc（specs）、llvm（clang.cfg） | `config()` 里生成 | 移到 `use()`，输出改到 subos | **高** |
| **F. elfpatch RPATH** | 10+ | 改 payload 内 ELF | **不动** —— 指向 payload 是 Catalog 引用，正确 | — |
| **G. subos 外** | code, 字体, fontconfig, pmwrapper | recipe 自己写 | **不动**，文档写明不管 | — |

### 7.3 建议顺序

1. **B、C 先行**（各 1–2 个包试点）—— 验证声明格式与物化路径，风险最低
2. **D** —— 需要 D4 冲突策略先定。glibc 是最重要也最危险的一个，**skip-if-exists 是刻意的**（host bind-mount 优先），换成覆盖语义会破坏它
3. **E 最后** —— 需要 `use()` hook 就绪，且**应当先有真实工具链 E2E**（见 §9 风险 3）

### 7.4 半盲期

过渡期索引里会同时存在守规矩与不守规矩的 recipe，**冲突检测只覆盖已迁移的包**。这是弃用窗口的固有代价，**接受并写进文档**。

---

## 8. 横向对比（为什么这个形状）

| 系统 | 共存靠 | 切换靠 | 需要台账 |
|---|---|---|---|
| Debian / RPM | **soname 写进文件名**（运行期免费）；**构建期直接放弃**（`-dev` 包 `Conflicts:`） | `update-alternatives` = symlink farm + 优先级 | ✅ `/var/lib/dpkg/alternatives/` |
| Homebrew | `Cellar/<pkg>/<ver>/` | `brew link` | ✅；冲突大的包设 **keg-only**（不进共享空间） |
| **Nix / Guix** | content-addressed store | profile = 派生 symlink farm，**换一个 symlink**，世代 + 回滚 | ❌ |
| Nix（构建期） | 同上 | **不用共享目录** —— `cc-wrapper` 注入，范围限于**声明的依赖** | ❌ |
| Spack / Lmod | 每 hash 一个 prefix | 改环境变量 | ❌ |
| macOS Framework | `Versions/A`、`Versions/B` | `Versions/Current` symlink | ❌ |
| Yocto | — | **每个 recipe 一棵 sysroot**，只含声明的依赖 | ❌ |

**规律 1：谁增量改共享命名空间，谁就需要台账。** xlings 现在是增量改**且没有台账** —— 这就是缺口。

**规律 2：所有解决了多版本共存的系统，都是"store 负责共存，view 负责选择"。**

**规律 3：所有系统都把构建期和运行期分开。**

**xlings 抄不了的一条**：Nix 要求一切由自己构建，xlings 消费上游预编译二进制。所以**共享 sysroot 删不掉**，只能把它从"增量可变的共享目录"变成"由声明驱动、单一所有者的派生视图"。

---

## 9. 可行性评估

### 9.1 优点

1. **每一条都由真实代码或真实数据推出**，没有从抽象推演的部分
2. **删的比加的多**（见 §3.4）
3. **复用既有机制**：spec 门禁、`useAfterInstall`、provider-scoped 卸载、`.xpkg.lua` 约定、`xvm.add` 的 type 分发
4. **迁移逐包进行，无截止日期**，90/118 个 recipe 一行不改

### 9.2 风险（诚实列出）

| # | 风险 | 说明 |
|---|---|---|
| **R1** | **整体体量是一个多版本工程，不是一个特性** | 两类模型 + 统一物化 + `use()` hook + `use_deps` + spec 3 + 28 个 recipe 迁移 + 跨 3 仓库协调 |
| **R2** | **`use()` 把任意代码带回切换路径** | 已用"提交点之后"缓解，但它确实侵蚀了 0.4.70 建立的 plan/execute 分离 |
| **R3** | **没有任何真实工具链验证** | 计划里的 **P4.2（真实 GCC 15/16 E2E）从未做过**，`xvm_toolchain_group_test.sh` 不存在，一直由 fixture 顶替。**E 类迁移在此之前不应开始** |
| **R4** | **`use_deps` 是第二套依赖图** | 与安装期 `deps` 并存。两个依赖概念是坏味道，需要想清楚能否统一 |
| **R5** | 跨仓库协调 | 0.4.70 的经验：libxpkg / mcpp / 索引的版本钉不齐，代价是整整一天的 CI 停摆 |
| **R6** | **D8（深度合并）未定** | 它单独决定 generation 是必需还是可选，也就决定这套东西是一个版本还是一条产品线 |

### 9.3 反方案：什么都不做（结构上）

必须诚实评估这个选项，因为它可能是对的：

- **没有任何官方 recipe 声明过 header op**，4 个铺 sysroot 的走的都是 Lua 路径
- 受影响的只有"同一个包装 ≥2 个版本且提供头/库"—— 官方索引里 4 个包
- doctor 的锚点误报**可以单独修**（让它识别锚点节点），不需要任何架构改动 —— 事实上已经修了

**如果多版本头文件/库不是真实用户需求，那么"只修具体 bug"是正确选择。** 本设计的价值取决于这个判断。

### 9.4 **建议：四阶段，每阶段由证据而非计划推动**

这是本文最重要的一节。**不建议一次做完。**

#### 阶段 0 —— 立刻可做，不依赖任何决策

- [ ] `doctor` 识别 group 锚点节点，不再报 `broken payload`（**修掉锚点误报**）
- [ ] `Library` effect 补 `active` 门禁 —— *注：若阶段一采纳 D1(b)，此项作废，所以可以直接等*
- [ ] `VData::libdir` 写入或删掉依赖它的死代码（`install_libs()` 零调用点）
- [ ] 写 E2E 证明 **provider-scoped 卸载能替代手写 `xvm.remove`** —— 这是删掉所有 recipe `uninstall()` 样板的前提，且**完全无条件**
- [ ] `~/.xlings.json` 全量重写模型加**阈值告警**（>2 MB 或 >5000 条），而不是现在做分片

#### 阶段一 —— 最小可行（修掉全部已知用户可见问题）

- `spec = "3"` 门禁 + `type = "files"` 声明
- **install 不物化，`use` 物化**（D1b）→ 三条物化路径收敛成一条
- 展开在物化时 `readdir` 现算（**D3a，不引入 manifest**）
- 冲突：报错 + 显式优先级（D4a）
- 迁移 B、C 两类试点

**收益**：7 个写入者 → 1；头文件真正可切换；卸载精确；冲突可检测；锚点误报消失；`active` 门禁概念消失。

**不需要**：manifest、`use()` hook、`args` 模板、`use_deps`、generation。

#### 阶段二 —— manifest（**条件触发**）

**触发条件**：recipe 在安装与卸载之间发生变更，导致"重算展开"与"当初实际物化"不一致，且这种漂移在实践中造成了问题。

在此之前，`readdir` 现算是等价的、且少一份需要维护的状态。

#### 阶段三 —— `use()` hook + `use_deps`（**条件触发**）

**触发条件**：gcc / llvm 的 cfg 陈旧被证明是真实痛点。

注意范围其实很小：**只有 gcc 和 llvm 两个 recipe** 会把 dep 的 payload 路径烧进产物。在证据出现之前，把它们的现状写进已知限制，比引入一套新 hook + 第二套依赖图更划算。

#### 阶段四 —— generation（**可能永远不做**）

**唯一触发条件**：D8 判定需要深度合并。其余收益（免 ledger、漂移检测、回滚）阶段一/二都能做到。

### 9.5 给评审的一句话

> **架构是对的，但应该按证据分四段走，而不是当成一个工程一次做完。**
> **阶段零 + 阶段一就能修掉今天全部已知的用户可见问题**，而后三个阶段各自解决一个**尚未被证明存在**的问题。
> 最大的风险不是设计错，是**在 R3（没有任何真实工具链验证）没解决之前就动 E 类（gcc / llvm）**。

---

## 9.6 执行拓扑（跨仓库依赖顺序）

> **定序原则：xlings 先发布，再迁 xpkg 包。** 索引 recipe 依赖新的声明能力，
> 而新能力要经由 libxpkg 发版、进入 mcpp-index、被 xlings 消费之后才存在。
> 反过来先迁索引，会让已发布的 xlings 读不懂新 recipe。

```
T1  xlings 侧实现 + 六平台 CI                    ← 不依赖任何外部仓库
     │  库随 release 切换 / 非激活不覆盖 / lib 目录统一 / doctor 锚点
     ↓
T2  合入 → 发布 xlings 0.4.70                    ← 用户拿到修复
     │
     ├─────────────────────────────┐
     ↓                             ↓
T3  libxpkg 0.0.47                T5a 老用户无感升级演练
     │  merge → tag → mcpp-index        (xlings self update，隔离 HOME)
     ↓
T4  xim-pkgindex 包迁移 + 规范文档
     │  B 类先行；glibc 待定
     ↓
T5b 全生态功能验证（隔离环境）
```

**为什么 T3 排在 T2 之后而不是并行**：libxpkg 的新字段只有被 xlings 消费才有意义，
而消费代码要进 xlings 的下一个版本。先发 0.4.70（不含消费代码）让已验证的修复落地，
libxpkg 与索引迁移作为下一个版本的内容推进 —— 与 §9.4 的阶段划分一致。

**每一层的失败都不回滚上一层**：T4 卡住不影响已发布的 T2；T3 卡住不影响 T4 之外的任何事。

## 9.7 版本命名规范

发布对外使用**复合标识**：

```
(0.4.70) 2026.07.27.0
 ^^^^^^   ^^^^^^^^^^^^
 语义版本   发布日期 + 当日序号
```

| 用途 | 用哪个 | 为什么 |
|---|---|---|
| `mcpp.toml` / `Info::VERSION` / git tag / 索引条目键 | **语义版本 `0.4.70`** | **机器要比较它** |
| release 标题、CHANGELOG、发布说明、文档 | **`(0.4.70) 2026.07.27.0`** | 人读，带时间线 |
| 同日多次发布 | 末位递增 `.1` `.2` | — |

**为什么机器版本不改成日期式**：`~/.xlings.json` 里存着 `"version": "v0.4.68"`，
`xlings self update` 靠版本比较决定是否升级，mcpp-index / xim-pkgindex 的条目也以版本号为键。
把机器可读版本换成 `2026.07.27.0`，**已发布的老客户端无法与 semver 比较** —— 它们要么不升，
要么误升。这与"老用户无感升级"直接冲突。

日期式若要成为机器版本，需要单独设计一个升级期的双版本比较兼容窗口，属独立改动。

## 9.8 libxpkg 发布链（T3 展开）

`xvm.files` / `args` 需要经完整发布链才能被索引使用：

```
1. openxlings/libxpkg   PR 合并 → tag 0.0.47
2. GitHub release       产物（tarball）+ sha256
3. gitcode 镜像         CN 侧产物（mcpp-index 条目是 GLOBAL + CN 双 URL）
4. mcpplibs/mcpp-index  pkgs/x/xpkg.lua 增加 0.0.47 条目（两个 URL + sha256）
5. xlings               mcpp.toml 的 xpkg 升到 0.0.47
6. xlings               实现消费 type="files" 的物化（新代码 + 一轮 CI）
7. xim-pkgindex         recipe 改用 xvm.files
```

**第 3 步（CN 镜像）容易漏。** mcpp-index 每个版本条目都带 `GLOBAL`（GitHub）和
`CN`（gitcode）两个 URL；只发 GitHub 会让 CN 用户解析失败。
校验 gitcode 产物要用 GET 而非 HEAD（HEAD 返回 401，GET 302 → CDN 200）。

**xlings 0.4.70 不在这条链上** —— 它只用 0.0.46 已有的字段，可以先发。

## 10. 核心 review 点

请重点确认这几条：

| # | 问题 | 我的倾向 |
|---|---|---|
| 1 | **多版本头文件/库是不是真实需求？** 若否，§9.3"只修具体 bug"就是答案 | 需要你判断 —— 这决定整份设计的价值 |
| 2 | **D1**：install 不物化，只有 use 物化 | 是。三条路径收敛成一条，`active` 门禁概念消失 |
| 3 | **D4**：跨包 dst 冲突 → 报错 + 优先级，**不做深度合并** | 是。深度合并是唯一强制 generation 的需求 |
| 4 | **spec = "3"** 逐包选择加入，两代并存不设截止 | 是。`spec = "2"` 已是成功先例 |
| 5 | **分四阶段、后三段由证据触发** | 是。这是本文核心建议 |
| 6 | **R3**：E 类（gcc/llvm）迁移前必须先做真实工具链 E2E（P4.2） | 是。这是硬前置 |
| 7 | **D6 / D7** 属 0.5 线（状态格式变更 + 放弃 0.4.69 回滚） | 是，且 D7 要**显式宣告**而非默默失效 |

---

## 附：本文引用的代码位置

| 事实 | 位置 |
|---|---|
| `VData::libdir` 无写入者 | `src/core/xvm/registration.cppm`（`RegistrationNode` 无该字段） |
| `install_libs()` 零调用点 | `src/core/xvm/commands.cppm` |
| `Library` effect 无 active 门禁 | `src/core/xim/installer.cppm:1568` |
| kind 默认成 program | `src/core/xim/installer.cppm:296` |
| group label 默认取 rootTarget | `src/core/xvm/registration.cppm:380` |
| 根必须是 batch 内节点 | `src/core/xvm/registration.cppm:376`（`RootNotInBatch`） |
| legacy 成对边双写（星型） | `src/core/xvm/registration.cppm:831` |
| provider-scoped 卸载回退 | `src/core/xim/installer.cppm:516`（`foundProviderRelease`） |
| shim 按第一个空格切 alias | `src/core/xvm/shim.cppm:451` |
| shim 每次 dispatch 读版本库（无缓存） | `src/core/xvm/shim.cppm:347` |
| spec 门禁先例 | `src/core/xim/installer.cppm:1715` |
| `config()` 无条件执行（payload 已存在也跑） | `src/core/xim/installer.cppm:2191` |
| 递归展开打爆 proot 的记录 | `xim-pkgindex/libs/sysroot.lua` 注释 |
