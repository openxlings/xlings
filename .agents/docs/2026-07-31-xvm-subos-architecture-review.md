# xvm × subos 评估：两个真缺陷，和一批我误判成缺陷的形状

**日期**: 2026-07-31（**重写**；初版同日）
**类型**: 评估 + 实施记录
**基线**: `78e7775` = `2026.7.31.1`
**范围**: `src/core/xvm/**`、`src/core/subos.cppm`、`src/core/config.cppm`、`src/core/xim/installer.cppm`

> **为什么重写**
> 初版把六个方向（D1–D6）并列成"架构问题"。实现并**实测**之后，其中只有两条对应
> 真实缺陷；其余是我从一个真缺陷出发、把周围"形状不好看"的东西归成同一族的结果。
> 本版按 **缺陷 / 该做 / 降级 / 撤回** 重新组织，并完整保留撤回理由 —— 那些理由比
> 结论更有用。

---

## 0. 一句话

> **真缺陷两个，都已修且有差分测试。另有一条小的"该做"。其余方向要么被这两个修复
> 顺带消掉，要么是可维护性下注，要么已撤回。**

| | 内容 | 判据 | 状态 |
|---|---|---|---|
| **缺陷 1** | 共享的版本库里存了 per-subos 的值 → 切 subos 后编译对着旧 sysroot | 真缺陷，可复现 | ✅ [#460](https://github.com/openxlings/xlings/pull/460) + [pkgindex#463](https://github.com/openxlings/xim-pkgindex/pull/463) |
| **缺陷 2** | "当前是哪个 subos"有两个解析器且分叉 → 装进 A、卸看 B | 真缺陷，有差分测试 | ✅ [#460](https://github.com/openxlings/xlings/pull/460) |
| **该做** | `effective_kind` 不是唯一的 tag 读法 | 潜在缺陷（不一致时才发作） | ✅ [#462](https://github.com/openxlings/xlings/pull/462) |
| 拆 sandbox | 修零个缺陷，可维护性下注 | 已实现，成本沉没 | ⚠️ [#461](https://github.com/openxlings/xlings/pull/461) |
| 其余 | 见 §5 | — | 降级 / 撤回 |

---

## 1. 缺陷 1：共享库里存了 per-subos 的值

### 1.1 分层（实测，非推断）

```
~/.xlings/.xlings.json             versions.*.{path,alias,envs,binding*}   → 全 home 共享
~/.xlings/data/xpkgs/…             payload                                 → 全 home 共享
~/.xlings/subos/<n>/.xlings.json   workspace: {active, installed[]}         → per-subos
~/.xlings/subos/<n>/{bin,lib,usr}  shim / 库 / 头文件                       → per-subos
```

**版本库和 payload 在同一侧：共享。** 而 `gcc.lua` 把安装时活动 subos 的绝对路径写进了
`alias`：

```
versions."g++"."15.1.0".alias = ["g++ --sysroot=/home/u/.xlings/subos/default"]
```

对装它的那个 subos 正确，对其余全部错误。`VData::fileSrc/fileDst` 的注释从 `files`
kind 引入起就写着这条规则 —— 只是从没被应用到 `alias` 和 `envs`。

### 1.2 实测影响面：184 → 1

全索引 **184 个 `xvm.add` 调用**。21 个 recipe 用到 `system.subos_sysrootdir()`，逐个查过：

| 用途 | recipe | 进 `xvm.add` 字段吗 |
|---|---|:---:|
| 往 sysroot 物化头文件/证书 | cairo、freetype、fontconfig、expat、fribidi、glib、harfbuzz、libffi、libpng、libxml2、pcre2、pixman、pango、zlib、openssl、python、linux-headers、glibc、ca-certificates、gcc-specs-config | ❌ |
| **写进 `alias`** | **`gcc.lua`** | ✅ **唯一** |

`envs` 全部查过：`brew`（→ payload）、`musl-gcc`（→ payload）、`virtualbox`（→ payload）、
`git`（→ 宿主机 `/etc/ssl/…`）。`bindir` / `includedir` 全部指向 payload。

**这个数字应该在写第一行代码之前就测出来。** 早测出来，就不会有 §5.2 那一串撤回。

### 1.3 判据不是"alias 不许有绝对路径"

`musl-gcc.lua` 也往 alias 里塞绝对路径：

```lua
" -Wl,--dynamic-linker=%s -Wl,-rpath,%s"     -- 指向 pkginfo.install_dir()
```

**但它指向 payload，对每个 subos 都正确。** 所以判据是：

> **记录里不得出现具体的 subos 路径。指向 payload 的绝对路径没有问题。**

### 1.4 修法：核心只认自己的 marker

```
recipe 写   alias = "g++ --sysroot=${XLINGS_DYNAMIC_SUBOS_DIR}"
核心做      执行时把 marker 换成本进程解析到的 subos
```

- 与 `expand_path` 的 `${XLINGS_HOME}` 同机制、低一层；
- **核心不认识 `--sysroot`**，也不认识 `-isysroot` / `--gcc-toolchain=`；
- **注册时不重写 recipe 写下的值** —— 核心只做一件事：展开自己的 marker；
- alias 和 envs 同一条规则（缺陷是**值**的属性，不是参数语法的属性）。

核心逻辑量：展开函数 ~10 行 + 两个调用点 3 行。

老记录靠既有的 `normalize_subos_paths`（`2026.7.30.1` 起就在跑）兜住；
`self doctor --fix` 可按需迁移成 marker。**两者都是兼容路径，不是机制。**

### 1.5 边界契约：事实归 xlings，语法归 recipe

判据只有一条：

> ### 加一个新包 / 新工具链，需不需要改 xlings？

| 场景 | 要改 xlings 吗 |
|---|:---:|
| gcc 要 `--sysroot=<subos>` | ❌ |
| Apple clang 要 `-isysroot <subos>` | ❌ |
| 某工具要 `--gcc-toolchain=<subos>/usr` | ❌ |
| 某工具改用环境变量 | ❌ |
| 某工具要 **payload 路径**而非 subos | ⚠️ 要 —— 需要一个新 marker |

前四行是**新拼写**，第五行是**新事实**。

| | 归谁 |
|---|---|
| 运行时**事实**（subos 在哪、home 在哪、payload 在哪…） | **xlings**：一个封闭的、有稳定性承诺的字典 |
| 把事实拼成这个工具认的**语法** | **recipe** |

当前字典只有两项：`${XLINGS_HOME}`、`${XLINGS_DYNAMIC_SUBOS_DIR}`。

**仍缺一步**：把字典写进 recipe 文档并给出稳定性承诺 —— 否则它是"能用"，不是"契约"。

### 1.6 这套的边界

只覆盖**能被告知**的工具（吃参数或吃环境变量）。对完全无法告知的工具，唯一答案是
视图层（mount namespace / bind mount，沙箱那套已有）：那一层工具不需要任何配合，
但需要 bwrap/proot。**两者是分层，不是竞争。**

---

## 2. 缺陷 2：「当前是哪个 subos」有两个解析器

### 2.1 分叉点

| | 是否 honor `forceGlobalScope_`（`install -g`） | 何时求值 |
|---|:---:|---|
| `update_effective_paths_` → `paths().subosDir` | ❌ | 构造期一次 |
| `xvm_artifact_subos_dir()` | ✅ | 每次调用 |

install 用后者，`use` 与卸载路径用前者。于是在项目目录里 `xlings install -g`：
**装进 global subos，卸载去 project subos 找。**

### 2.2 写测试时又撞出同一形状的两处

1. **`workspace()` / `workspace_installed()` 不 honor `-g`，而 `_mut` 版本 honor** ——
   写进一个 map、从另一个 map 查。`install -g` 注册了，`remove -g` 回答
   "not installed in current subos"，而 shim 明明在磁盘上。
2. **`remove` 压根没有 `-g`** —— 项目里 `install -g` 装的包**结构上无法卸载**。
   scope 只在可逆操作的一半上可表达，那这个操作就不可逆。

**两处都不是读代码读出来的，是写差分测试逼出来的。**

### 2.3 修法

合并为 `Config::resolve_subos_scope_()`（`paths_` 由它派生，`-g` 时重算）；读写两版
访问器函数体相同，并把"读写必须解析到同一个 map"写成注释里的不变量；`remove` 补 `-g`。

差分测试 **E2E-50**：`-g` 装的 `-g` 能卸；不带 `-g` 时两半都作用于项目 subos。
在 `2026.7.30.2` 构建上失败。

---

## 3. 该做（小）：`effective_kind` 成为唯一的 tag 读法

`types.cppm` 提供 `effective_kind(info, data)`，让所有人对"这条记录是什么 kind"口径一致
（per-version `kind` 优先，回落到 target 级 `type`）。然后：

| 读法 | 使用者 |
|---|---|
| `effective_kind()` | bindings、inspect、registration |
| **直接读 `type`** | **`xvm/commands.cppm:487`（决定建不建 shim）**、`xself/doctor.cppm` ×4 |

其中 `doctor.cppm:498` 更糟：**按 target 级回落值跳过整个 target，再逐版本遍历** ——
把其中确实是 program 的版本一起丢掉。

修法两个 helper（`effective_kind_of` / `has_program_kind`），5 个单测双向钉住。
**这是全文唯一一条"该做"而非"必要"** —— 它修的是潜在缺陷，两者不一致时才发作。

---

## 4. 已实现

| PR | 内容 | 差分测试 |
|---|---|---|
| [#460](https://github.com/openxlings/xlings/pull/460) `2026.7.31.2` | 缺陷 2（单一 scope）+ 缺陷 1（marker 渲染） | 11 个 `SubosPlaceholderTest.*`；**E2E-50** 新增；E2E-46 重构 |
| [#462](https://github.com/openxlings/xlings/pull/462) | §3 单一 tag 权威 | 5 个 `EffectiveKindAuthority.*` |
| [#461](https://github.com/openxlings/xlings/pull/461) | 拆 sandbox（`subos.cppm` 2285 → 1252） | 无新增 —— 机械拆分若需新测试证明自己，它就不是机械拆分 |
| [pkgindex#463](https://github.com/openxlings/xim-pkgindex/pull/463) | `gcc.lua` 写 marker | 2 条静态测试，同时钉住 HEADER / LINK 两条轴 |

**合入顺序**：#460 发布并成为 `latest` → 再合 pkgindex#463（老客户端不展开 marker）。

---

## 5. 降级与撤回

### 5.1 降级：是形状，不是缺陷

| 原方向 | 降级理由 |
|---|---|
| **统一物化 planner（原 D3）** | 主要论据是"三处对 scope 不一致"—— **那个缺陷是缺陷 2 修掉的**。只要 scope 只有一个解析器，三处就必然一致，代码写几遍不影响正确性。剩下的"加一种 kind 要改三处"是可维护性 |
| **拆 sandbox（原 D5）** | **修零个缺陷。** 依据是"46% 的行数不相干"和一次已被修复的历史事故。是可维护性下注 —— 已实现且 CI 绿，成本已沉没，但不该称"必要" |
| **和类型取代宽 struct（原 D4-b）** | 要 schema 迁移 + 版本地板，收益是"非法组合不可表达"。没有对应的真实缺陷；`sysroot` 清掉、`libdir`（唯一写入者是反序列化器）也能清掉之后，struct 没那么糟 |
| **绑定表示单一化（原 D6）** | `LegacyGraph` 与 `ProviderGroup` 并存的成本是 15 个错误类型里的至少 6 个 —— **是成本，不是缺陷**。需要版本地板 |
| **scope 显式传参（原 D1-b）** | 缺陷 2 修完后只剩可测性收益 |

### 5.2 撤回：当时看着对，后来被证伪

| 撤回项 | 当时的理由 | 为什么错 |
|---|---|---|
| **`VData::sysroot` 布尔字段**（`2026.7.31.1` 已发布） | "存意图不存答案" | 思路对、粒度错：把 GCC/Clang 的 flag 拼写塞进了通用版本管理器；对 `-isysroot` 无效，对 **envs 完全无效**。已在 #460 删除 |
| **recipe 写 `subos/current`** | 老客户端兜底 | 只跟全局选择，跟不上 `XLINGS_ACTIVE_SUBOS` 和项目 subos —— 修好三种模式里的一种，而剩下两种恰是做隔离用的那两种 |
| **注册时改写 recipe 写下的值** | 让老 recipe 不用改 | 实测受影响 recipe = **1 个**。为一个假想的规模付了"推断 + 重写 + 迁移 + 兼容"四层成本 |
| **拆 `use` 动词为 `enter`/`switch`** | "一个动词做三件事" | **CLI 早就区分开了**（不同动词、`exit to leave` 提示、`[global]` 标签、不同配色 —— `ui/info_panel.cppm:378-410`）。我从内部不一致推出用户体验结论，没查输出 |
| **把"`subos/current` 跟不上环境变量"当缺陷** | 看着像 symlink 的局限 | 一个符号链接**本来就**表示不了 N 个并发 shell 各自的选择。让 `use` 也维护它会让并发 shell 互相覆盖 —— 把一个诚实的局限换成一个不确定的错误 |

### 5.3 这些撤回的共同形状

**看到一个真缺陷，就顺着它把周围所有"形状不好看"的东西归成同一族，然后为假想的规模
付成本。**

四次都可以被同一类动作证伪，而且都很便宜：

| 该做的动作 | 会立刻推翻 |
|---|---|
| 先测实际影响面 | 184 → **1** ⇒ 注册时重写、`subos/current`、compat 转换全都不必要 |
| 先看一眼真实输出 | CLI 早已区分 ⇒ 拆动词不必要 |
| 先问"这个论据是不是已被别的修复消掉了" | D3 之于缺陷 2 |
| 先问"这条记录不通用的话，doctor 为什么要为它设检查" | 判据本身就在告诉你它不通用 |

---

## 6. 待办

| | |
|---|---|
| 把 marker 字典写进 recipe 文档并给稳定性承诺 | 让它从"能用"变成"契约" |
| 清掉 `libdir`（唯一写入者是反序列化器） | 无人写、无人读 |
| 给残留的兼容路径打 `COMPAT(… → drop in 0.6.0)` 标记 | `normalize_subos_paths`、doctor 的 `SubosPathBaked` 迁移 —— 现在它们看起来像永久代码 |
| 卸载路径两套机制的分工（原 D3 的前置问题） | 见 §7 |

---

## 7. 唯一还没查清的东西

卸载侧实际是**两套并存机制**：

| 机制 | 粒度 |
|---|---|
| `cleanup_removed_xvm_program_artifacts` | 按 removal 结果批量 |
| `detach_current_subos_` | 单个 entry target，带 `detachedByBatch` 门 |

而 `detach` 取头文件按 **release** 粒度、取库/文件按 **entry** 粒度，`use` 那侧统一按
release。看着是不对称，但批量机制可能补上了。

**先回答，再决定动不动代码**：

1. `detachedByBatch` 为真/为假时，分别是谁在清理？
2. 移除一个 release 的**成员**（而非 root）时，另一个成员的库/文件资产会被清掉吗？
3. 若确有不对称：是缺陷，还是 refcount 语义（payload 共享）刻意为之？

卸载路径正是这个代码库反复产出"静默成功"缺陷的地方。没查清就重构，只会再造一个同族的。

---

## 8. 与既有 issue 的关系

| | 定位 |
|---|---|
| [#458](https://github.com/openxlings/xlings/issues/458) payload 内烧死路径不可见 | 同一条分界线的**另一侧**：payload 也必须与 subos 无关。marker 管不到它 —— **没有任何诊断能看见 payload 内容** |
| [#408](https://github.com/openxlings/xlings/issues/408) sysroot/bin/lib 多版本共存模型 | 独立，属 0.5 线 |
| [pkgindex#452](https://github.com/openxlings/xim-pkgindex/issues/452) Windows gcc uninstall | 既有缺陷，与本文无关，但挡着 pkgindex 的 windows-test |
