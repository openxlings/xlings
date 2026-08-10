# #524 / #525 根因分析与修复设计

> 状态：**Root cause measured, fix designed, not implemented** —— 两条 issue 的根因都有可运行的复现或实测证据，
> 不是从日志推断的。#524 的修复方案已经在离线 harness 上验证过，#525 的修复方案还只有机制证据、没有端到端实测。
>
> 日期：2026-08-10
>
> 对象：`openxlings/xlings#524`（冷装 gcc 失败）、`openxlings/xlings#525`（GLX 拿不到 FBConfig）
>
> 复现工具：`.agents/tools/repro-dep-install-dir.sh`（离线，无需 xlings / 网络 / 安装，不碰任何 home）
>
> 关联：libxpkg `0.0.54 → 0.0.55 → 0.0.56`、xlings `2026.8.10.1`（#519 / #514）、
> `mcpp-community/mcpp#400`、`mcpp-community/mcpp#401`

---

## 0. 结论先行

| | #524 冷装 gcc 失败 | #525 GLX 无 FBConfig |
|---|---|---|
| **根因** | libxpkg 0.0.55 把 `dep_install_dir` 的**匹配条件从「答案是否唯一」换成了「问题是否够具体」**，同时让 explicit-roots 分支**无条件 `return`**、切断了 scan 兜底。index 里没有一条 recipe 写成它要求的形状。 | glvnd 的 **GLX 侧根本没有 `egl_vendor.d` 的对应物**（实测：`libGLX.so.0` 里只有 `libGLX_%s.so.0` 和 `__GLX_VENDOR_LIBRARY_NAME`，没有任何目录变量）。vendor 可达性只能由**执行 dlopen 的那个对象自己的 RPATH** 提供，而我们的 `libGLX.so.0` 的 RPATH 至今只有 `$ORIGIN`。 |
| **怎么引入的** | 收紧规则时**没有对照真实调用点**。7 个调用点里 6 个是「bare + 无版本」，而新规则要求「namespaced + 精确版本」。 | 机制被验证过，但**验证探针用了 `-Wl,--disable-new-dtags`**，把可达性挂在**消费者的链接参数**上。第一个真实消费者（mcpp，默认 `--enable-new-dtags`）一接触就塌。`glxprobe.c:15-28` 把这条前置条件写得清清楚楚，然后当成前提而不是缺陷。 |
| **影响面** | **6/7 调用点断**，比 issue 报的多两条硬失败（meson 冷装同样失败）和两条**静默降级**（godot 悄悄回落宿主 GL、graphics 横幅显示 unknown）。 | 任何用默认链接参数的 GL 消费者。生态里目前**全部**如此。 |
| **0.0.56 修了吗** | **没有**。它只把同一个 nil 变响了：6/7 依然断。 | — |
| **修复** | libxpkg：bare 查询按**答案唯一性**判定（唯一性守卫本来就在），版本半边按**区间**匹配。实测 6→2。剩下 2 条是真正不同的问题，单独决策。 | 让 `xim:libglvnd` 的 `libGLX.so.0` 自带指向 **GLX vendor 目录**的 RUNPATH。这是 issue 提的方向 B 的正确形态：不是环境变量（glvnd 没有），而是一个目录 + 我们自己载荷上的 RPATH。 |

**两条 issue 是同一个形态的两次发作**，见 §3。

---

## 1. #524 —— 冷装 `xim:gcc@16.1.0` 失败

### 1.1 回归窗口：正好一个 commit

```
origin/main  65002df  2026.8.10.1   ← 失败
             f203b6b  2026.8.9.2    ← 成功
```

`v2026.8.9.2..65002df` 之间只有这一个 commit。它同时做了两件事，**两件都是必要条件**：

1. `src/core/xim/installer.cppm:895` 新增 `configure_dependency_store_roots_()`，
   在 `:2485` 和 `:3294` 两条安装路径上**无条件**填充 `ctx.dependency_store_roots`
   （至少含 selectedStore + global xpkgs，所以**永远非空**）。
2. `mcpp.lock`：`mcpplibs.xpkg` `0.0.54 → 0.0.55`。

单独任一方都不会出事：0.0.55 在没有 roots 字段时会走 scan 兜底；0.0.54 在有 roots 时忽略它。
**是这两个一起发布，才让新分支第一次被执行到。**

### 1.2 根因：两道闸门，外加一次早退

gcc 的 config hook 写的是（`xim-pkgindex/pkgs/g/gcc.lua:544`）：

```lua
local glibc_dir = pkginfo.dep_install_dir("glibc")   -- bare name, 无版本
```

而它声明的是 `deps = { "xim:glibc@>=2.39", ... }` —— **namespaced + 区间**。

0.0.55 的 `pkginfo.lua` 在这条路径上加了三样东西：

```lua
-- :234 resolved_dep —— 闸门 1
function M.resolved_dep(dep_name, dep_version)
    local ns, bare = _parse_namespace(dep_name)
    if not ns then
        if not _is_exact_store_version(dep_version) then return nil end   -- :239 ← dep_version 是 nil，直接 nil
        ...
```

```lua
-- :178 _resolve_dep_via_explicit_roots —— 闸门 2
    if not _is_exact_store_version(dep_version) then return nil end       -- :179 ← 同样 nil
    local ns, bare = _parse_namespace(dep_name)
    if not ns then return nil end                                        -- ← bare 名字，再 nil
```

```lua
-- :296 dep_install_dir —— 早退
    local roots = _RUNTIME and _RUNTIME.dependency_store_roots
    if type(roots) == "table" then
        return _resolve_dep_via_explicit_roots(dep_name, dep_version)     -- ← 无条件 return，scan 永远够不到
    end
    local result = _resolve_dep_via_scan(...)                             -- ← 0.0.54 靠这行活着
```

于是 `dep_install_dir("glibc")` 返回 **nil，且一句话都不说**。gcc 的 hook 只能报「glibc payload not found」——
它说的是它知道的唯一一件事。

**闸门 1 是核心缺陷。** 注意它的循环体里**本来就有唯一性守卫**：

```lua
if record_bare == bare and rec.version == dep_version then
    if candidate then return nil end    -- 两条记录同名 → 失败关闭
    candidate = rec
end
```

歧义已经由这一行判定了。`_is_exact_store_version(dep_version)` 那道前置**没有参与任何消歧**——
它唯一的作用是把「不带版本的查询」整个拒掉，而那正是 index 里**每一条 recipe 实际写的形状**。

> 一句话：**0.0.55 把判定条件从「答案是否唯一」换成了「问题是否够具体」。**
> 这两者只在 recipe 恰好写得很啰嗦时重合，而没有一条 recipe 那么写。

### 1.3 复现：离线、可运行、不需要装任何东西

`.agents/tools/repro-dep-install-dir.sh` 把真实的 `pkginfo.lua`（直接从 libxpkg checkout 按 rev 取）
挂在 stub 过的 xmake stdlib + 假 xpkgs store 上，按**已发布 recipe 的原样调用形状**重放：

```
$ .agents/tools/repro-dep-install-dir.sh            # 默认 0.0.54 / 0.0.55 / 0.0.56
$ .agents/tools/repro-dep-install-dir.sh HEAD fix/xxx   # 任意 rev
```

| 调用点（recipe 原样） | 0.0.54 | 0.0.55 | 0.0.56 | **fixA** |
|---|---|---|---|---|
| `gcc.lua:544` `dep_install_dir("glibc")` | ok | **nil** | **nil** | ok |
| `llvm.lua:210` `dep_install_dir("glibc")` | ok | **nil** | **nil** | ok |
| `graphics.lua:176` `dep_install_dir("nvidia-gl-host-link")` | ok | **nil** | **nil** | ok |
| `meson.lua:116` `dep_install_dir("python")` | ok | **nil** | **nil** | ok |
| `godot.lua:351/422` `dep_install_dir("mesa")` ¹ | ok | **nil** | **nil** | **nil** |
| `mcpp-vscode-clangd.lua:245` `dep_install_dir("llvm-tools", ver)` ² | ok | **nil** | **nil** | **nil** |
| （对照）`dep_install_dir("xim:glibc")` | ok | ok | ok | ok |
| | 0/7 断 | **6/7 断** | **6/7 断** | **2/7 断** |

¹ mesa 是**传递**依赖（godot 声明的是 `xim:graphics@>=0.1`），`resolved_deps` 只记录 `node.runtime_deps`
（`installer.cppm:2472`），所以本来就没有它的记录。
² `llvm-tools` 是 hook 里 `pkgmanager.install()` 装的，不是声明依赖，同样没有记录。

**「有 roots / 无 roots」两列都跑了**：无 roots 时三个版本全绿 —— 这直接证明触发条件是
xlings 2026.8.10.1 开始填这个字段，而不是 recipe 或 home 的问题。

### 1.4 libxpkg 0.0.56 不是这条 issue 的修复

0.0.56（`fix(pkginfo): an unanswerable dependency query must not look like an absent one`）
已经**正确诊断**了这个问题——commit message 里点名了 `gcc.lua` 和 `llvm.lua` 的 bare 调用。
但它的改动**只加诊断**：

```lua
local hit = _resolve_dep_via_explicit_roots(dep_name, dep_version)
if hit then return hit end
...
log.error("dep_install_dir(%s): a bare name cannot be resolved against ...")
return nil                                    -- ← 依然 nil
```

实测：**6/7 依然断**，只是现在会打印一条错误。

它的立场是「nil 在这里是**正确答案**，错的是**沉默**」。**这个立场对了一半。**
对的一半：explicit roots 确实只能回答精确的 namespaced 坐标，让它猜 `compat-x-zlib` 还是 `other-x-zlib` 是错的。
错的一半：`resolved_deps` **不是** explicit roots。它是一张**封闭的小表**，就是这个包自己声明的那几条依赖，
「同一个 bare 名字有没有第二条记录」是**可判定的**，不需要猜。0.0.55 把 roots 分支的正确约束
（必须精确、必须带命名空间）**误加到了 `resolved_deps` 分支上**，而后者恰恰是为了免去调用方重复说一遍而存在的。

### 1.5 issue 里的一条假线索

> 「安装的是 **2.44**，而 hook 的诊断建议的是 **2.39** —— 二者不一致本身就是线索。」

**不是线索。** `xlings install xim:glibc@2.39` 是 `gcc.lua:545` 里的**硬编码字面量**：

```lua
log.error("  install it first: xlings install xim:glibc@2.39")
```

它跟解析结果没有任何关系，2.44 被解析出来是完全正确的（`>=2.39` 允许）。
顺手改掉这个字面量（改成 hook 真正查的那个坐标），否则下一个人还会顺着它走一遍。

日志里**没有**出现 0.0.55 的另外两条消息（`resolver record points to missing payload`、
`no explicit store roots, fell back to a scan`），这本身就是判别证据：它区分了
「记录有但 payload 不在盘上」和「压根没匹配上记录」，落在后者。

### 1.6 为什么 CI 没拦住

- **热 home 天然免疫**：gcc 已装 → 整个 install 被跳过 → config hook 不跑。xlings 自己的 CI 走的是这条路。
- 唯一会冷装 gcc 的是下游（mcpp）的 sandbox 缓存 miss —— 也就是 issue 里那次 A/B。
  换句话说，**这个缺陷的第一个探测器在另一个仓库里**。
- libxpkg 侧 0.0.55 带了 593 行新 `test_executor.cpp`，但测的是 executor 的 C++ 面；
  `pkginfo.lua` 的 Lua 行为**没有任何测试**，而这次改的就是它。

### 1.7 修复方案

**Fix A（libxpkg，根因）——按答案唯一性判定，而不是按问题具体度**

`M.resolved_dep` 的 bare 分支：

1. 删掉 `_is_exact_store_version(dep_version)` 前置（它不消歧，只拒绝合法查询）。
2. 版本半边按**区间**匹配，不按字面量相等 —— 这是 recipe 实际写的语法，
   解析器早就支持（同一形态的旧 bug：xlings#481，`>=2.38` 当字面量比导致匹配不到节点）。
3. 唯一性守卫保留，并且**说出撞了谁**。

实测（`fixA` 列）：6/7 → 2/7。歧义与区间行为单独验过：

| 场景 | 结果 |
|---|---|
| 两个 provider 都叫 bare `zlib` | **nil + 命名双方**（失败关闭）✅ |
| 换成 `compat:zlib` | 正确解析 ✅ |
| `dep_install_dir("glibc", ">=2.39")` → 记录是 2.44 | 命中 ✅ |
| `dep_install_dir("glibc", "2.39")` → 记录是 2.44 | nil ✅ |

**Fix B（xim-pkgindex）——recipe 按声明的坐标提问**

`gcc/llvm/meson/graphics` 全部改成 namespaced（`"xim:glibc"`、`"xim:python"`、`"xim:nvidia-gl-host-link"`）。
这**单独就能修好这四条**（对照行已证），但它是**卫生**不是根因修复：Fix A 不做，
下一条写 bare 的 recipe 还会再踩，而且 godot / clangd 那两条它修不了。**两个都要做。**

**Fix C（剩下 2 条，需要你拍板）**

这两条是**真正不同的问题**，不该被 Fix A 顺手糊过去：

- `godot → mesa`：问的是**传递**依赖的 payload。选项：
  (a) godot 直接声明 `xim:mesa@>=25`（它确实用 mesa 的 libdir，声明是诚实的）；
  (b) xlings 把传递依赖也记进 `resolved_deps`。
  **倾向 (a)** —— (b) 会把 `resolved_deps` 从「我声明了什么」变成「解析器装了什么」，是更大的语义变更。
  **注意这条现在是静默的**：`have_stack=false` → godot 悄悄回落宿主 GL 目录，
  正好是 recipe 注释里说它要防的 mcpp#352（`GLIBC_2.43 not found`）。
- `clangd → llvm-tools`：hook 里 `pkgmanager.install()` 装的，本来就没有记录。
  应当走 `tool_payload_dir`（它保留了独立的 scan 路径），而不是 `dep_install_dir`。

**Fix D（护栏）——这一类不能再靠下游发现**

1. **libxpkg**：把本文的 harness 收成 `pkginfo.lua` 的真单测，覆盖上表 7 种调用形状 × 有/无 roots。
   这是这次唯一能在改动当天就拦住的东西。
2. **xim-pkgindex**：结构化 lint —— 每个 `pkginfo.dep_install_dir(<字面量>)` 的第一参数
   必须带命名空间，且必须出现在该 recipe 的 `deps` 里。要**解析** Lua，不要正则
   （regex 改 recipe 「能解析、但意思变了」是本仓库的老账）。
3. **xlings**：`fresh-install` CI 加一条真·冷装 `xim:gcc@16.1.0`。

**发布顺序**：libxpkg（Fix A + 单测）→ bump xlings `mcpp.lock` → xlings patch 版本 →
xim-pkgindex（Fix B/C）→ 通知 mcpp 把 pin 提回。

---

## 2. #525 —— graphics 栈装好后 GLX 仍无 FBConfig

### 2.1 实测：GLX 侧没有 `egl_vendor.d` 的对应物，也不可能有

在本机宿主 libglvnd 上直接读符号表：

```
$ strings /lib/x86_64-linux-gnu/libGLX.so.0 | grep -iE 'vendor|libGLX_'
libGLX_%s.so.0
__GLX_VENDOR_LIBRARY_NAME
__GLX_FORCE_VENDOR_LIBRARY_%d

$ strings /lib/x86_64-linux-gnu/libEGL.so.1 | grep -iE 'vendor|library_path'
__EGL_VENDOR_LIBRARY_DIRS
__EGL_VENDOR_LIBRARY_FILENAMES
/etc/glvnd/egl_vendor.d:/usr/share/glvnd/egl_vendor.d
library_path
```

对比是彻底的：

| | EGL | GLX |
|---|---|---|
| 发现机制 | JSON 目录（环境变量可改） | **无** |
| 定位方式 | JSON 里的 `library_path`，**可以是绝对路径** | `dlopen("libGLX_<name>.so.0")`，**只有 SONAME** |
| 环境变量能给什么 | **目录** | **只有名字** |

所以 issue 里那句「设 `__GLX_VENDOR_LIBRARY_NAME=nvidia` 也没用——名字对了，库仍然找不到」
是**结构性的**，不是配置漏了：glvnd 从来没给过 GLX 一个说「去哪里找」的接口。

**这也顺带解释了 EGL 为什么一直是好的**，而它的好是**误导性**的：
`mesa.lua:161-166` 和 nvidia 的 vendor JSON 都把 SONAME **改写成了绝对路径**。
绝对路径对「谁加载了 libEGL、它的 RUNPATH 是什么」完全免疫。
**EGL 通过是因为它走了一条 GLX 没有的路，不是因为发现层配好了。**
`libs/graphics.lua:90-94` 的 `DISCOVERY` 表里三个变量（`LIBGL_DRIVERS_PATH`、
`__EGL_VENDOR_LIBRARY_DIRS`、`XDG_DATA_DIRS`）—— 没有一个是给 GLX 的，因为没有可给的。

### 2.2 那条 dlopen 只有四条搜索路径，我们只能用一条

`libGLX.so.0` 内部 `dlopen("libGLX_nvidia.so.0")`，glibc 的顺序：

| # | 路径 | 在我们这儿 |
|---|---|---|
| 1 | **调用方对象**的 DT_RPATH（且调用方无 DT_RUNPATH 时**沿加载链向上传递**） | ✅ **唯一可用的** |
| 2 | `LD_LIBRARY_PATH` | ❌ 进程全局，会毒死宿主二进制（issue 里的 `timeout: __pointer_chk_guard` 就是它；xim 自己的安装警告说的也是它） |
| 3 | **调用方对象**的 DT_RUNPATH（**不传递**） | ✅ 可用，但必须写在 `libGLX.so.0` 自己身上 |
| 4 | `ld.so.cache` / 默认目录 | ❌ 我们的 ld.so 的 cache 路径在任何地方都不存在（INTERP 切换后宿主兜底就没了） |

**结论：可达性必须由执行 dlopen 的那个对象自己携带。** 那个对象是 `libGLX.so.0` —— 是我们的文件
（`xim:libglvnd`），所以我们能改。这一点 2026-08-06 已经用 A/B/C 实测过：
调用方带 DT_RPATH 或 DT_RUNPATH 都能解析，两者都行；调用方什么都不带就失败。

### 2.3 被加载的 `libGLX.so.0` 是我们的，但它的 RPATH 只有 `$ORIGIN`

`compat.glx-runtime` 不是宿主直通，而是**从 SubOS view 里挑选并 symlink 出来的桥**
（mcpp 自己的设计文档：「仍需要从 SubOS view 选择并 symlink GL 库到自己的 runtime 目录……
这是过渡桥，而不是最终模型」）。所以程序加载到的 `libGLX.so.0` **确实是我们 libglvnd 的载荷**。

问题不在于加载到了谁，而在于：

- `libGLX.so.0` 在 `<xpkgs>/xim-x-libglvnd/<ver>/lib/`，RPATH 是 `$ORIGIN`；
- `libGLX_nvidia.so.0` 在 `<xpkgs>/xim-x-nvidia-gl-host-link/0.1.1/lib/`，**另一个载荷目录**；
- `<subos>/lib` 把两者并到一起，但**没有任何东西把这个目录写进 libGLX.so.0 的搜索路径**。

`$ORIGIN` 展开的是**符号链接解析之后**的真实目录，所以走 subos farm 的符号链接也救不了。

而消费者（app）的 RUNPATH 里**即使有** `glx_runtime/lib`（里面可能同时有 vendor 的符号链接），
也帮不上忙 —— **DT_RUNPATH 不传递**，服务不了下游对象发起的 dlopen。这正是陷阱所在。

### 2.4 探针为什么一直是绿的 —— 这就是引入点

`xim-pkgindex/.agents/tools/graphics/glxprobe.c:15-28`：

```c
// BUILD REQUIREMENT -- DT_RPATH, not DT_RUNPATH:
//   gcc -o glxprobe glxprobe.c -ldl \
//       -Wl,-rpath,<subos>/lib -Wl,--disable-new-dtags
//
// glvnd's dlopen of the vendor is served by the CALLING object's search
// path, and libGLX.so.0's own RPATH is `$ORIGIN` -- it cannot see the vendor
// package. What makes it resolve is that DT_RPATH is searched transitively up
// the load chain to the executable ... build the same probe with
// --enable-new-dtags (the default on many distros) and glvnd finds no vendor
// at all
```

`verify-stack.sh:166`、`verify-host-link.sh:110,133` 也都带 `-Wl,--disable-new-dtags`。

也就是说：

- 缺陷被**准确地识别了**（注释把失败模式、原因、默认值风险全写对了）；
- 然后被**当成对消费者的前置条件**接受，而不是当成我们载荷上的缺口去补；
- 于是**生态里每一条 GLX 绿灯，都是靠一个真实消费者不会传的链接参数换来的**。

mcpp 用默认 `--enable-new-dtags` 发 `-Wl,-rpath`（`flags.cppm:711-720`），得到 DT_RUNPATH，
不传递 —— 前置条件破了，vendor 找不到，glvnd 吞掉 dlopen 错误，用户看到的就是
`GLX: No GLXFBConfigs returned`。

**mcpp 没做错任何事。** 把可达性挂在消费者链接参数上，这个契约本身就不成立。

### 2.5 修复方案

**Fix E（根因，xim-pkgindex）——`xim:libglvnd` 自带 GLX vendor 搜索路径**

1. `libs/graphics.lua` 增加 `graphics.GLX_VENDOR_DIR`，一个**只放 `libGLX_*.so.0` 符号链接**的目录。
   **必须放在 `usr/` 下**（例如 `usr/lib/glx-vendor`）：xlings 只允许 `usr`/`etc`/`share` 开头的
   file asset 目标，被拒绝**不报错、只是不发生** —— `dst = "lib/dri"` 那次就是这么变成 llvmpipe 的
   （`libs/graphics.lua:63-74` 已记录这条规则和那次事故）。
2. vendor 包（`nvidia-gl-host-link`、`mesa`、`wsl-gl-host-link`）在 config 时把自己的
   GLX 入口点链进这个目录 —— 与它们已经在做的 `graphics.declare_egl_vendor()` 对称。
3. `libglvnd` 的 config hook 给 `libGLX.so.0` 写上指向该目录的 **DT_RUNPATH**。

  为什么是 RUNPATH 而不是 RPATH：调用方自带时两者都能服务这条 dlopen（2026-08-06 A/B/C 实测），
  而 RUNPATH **不传递** —— 它不会渗进 vendor 库自己的依赖查找，那部分由 interposer 单独控制
  （`nvidia-gl-host-link.lua:418-455` 明确需要 DT_RPATH，是**另一个对象**上的另一条规则）。
  **不要**把 `<subos>/lib` farm 整个塞进去：farm 里有 `libc.so.6` / `ld-linux` 符号链接，
  进 RPATH 是已知地雷；专用目录从结构上避开了它。

  实现入口：`elfpatch.set({rpath=...})` 的 `rpath` 参数**从不被读取**，
  唯一接受 caller rpath 的是 `elfpatch.patch_elf_loader_rpath`。成功判定用 `r.patched > 0`
  （缺 patchelf 时返回 `{0,0,0}`，按真值判会把「没发生」当成功）。

**Fix F（护栏）——探针必须按真实消费者的方式构建**

`glxprobe` / `verify-stack.sh` / `verify-host-link.sh` **去掉 `--disable-new-dtags`**，改用默认。
这一条现在就应该做，且**做完立刻会变红** —— 那就是它该有的样子：
现在的绿灯是假的，Fix E 落地后它才应该真绿。
再加一条**显式**的 `--disable-new-dtags` 变体，用来区分「vendor 不可达」和「其它环节坏了」。

**Fix G（边界，与 mcpp 协同）**

Fix E 落地后 `compat.glx-runtime` 的 symlink bridge 就不再是可达性的必要条件了
（mcpp 自己的路线图里它本来就标着「过渡桥，最终应弃用」）。
建议顺序：xlings 侧先修好并实测，**再**推动 mcpp 弃用桥 —— 反过来会在没有替代品的时候把 GL 彻底断掉。

### 2.6 需要在实测中确认的两点

以下两条来自 2026-08-06 的记录，写进方案前应当在装好栈的机器上复核（本机没装 graphics 栈，无法就地验证）：

1. `xim:libglvnd` 载荷里 `libGLX.so.0` 的 RPATH **当前确实只有 `$ORIGIN`**（`readelf -d`）。
2. 给它加上 vendor 目录的 DT_RUNPATH 后，**默认 dtags** 构建的 imgui/GLFW 程序能拿到 FBConfig。
   判据用 `/proc/self/maps` 里 GL 对象的实际路径，**不要**用 `glxinfo -B` 的 renderer 字符串 ——
   renderer 字符串在「我们的栈生效」和「整条链都来自 `/usr/lib`」两种情况下**完全一样**。

---

## 3. 共同的形态

两条 issue 都不是「写错了一行」，而是同一个判断失误的两次发作：

> **用一个比真实条件更强的前提去验证机制，然后把那个前提当成已经成立。**

- **#524**：新规则要求「namespaced + 精确版本」。它在**作者手写的调用**下成立，
  在 index 里 7 个真实调用点中 6 个不成立。收紧规则时没有去枚举调用点。
- **#525**：机制要求「消费者带 DT_RPATH」。它在**手工加 `--disable-new-dtags` 的探针**下成立，
  在任何用默认参数的真实构建系统下不成立。而这条前提被写进注释、当成了契约。

两者的失败方式也一致，都是本仓库反复出现的**沉默**：
`dep_install_dir` 返回 nil 不说话（0.0.56 才补上一句），glvnd 吞掉 dlopen 错误只报 `No GLXFBConfigs`。
**「没找到」和「没发生」产生了相同的输出。**

对应的通用护栏，两条都指向同一件事：**验证必须用消费者真实的形状**——
真实的调用写法、真实的链接参数、真实的冷 home。

---

## 4. 验收判据（可证伪）

| # | 命令 | 通过条件 |
|---|---|---|
| A1 | `lua5.4 repro_dep_install_dir.lua pkginfo-<new>.lua` | 7 个调用点中 ≥5 个 ok；剩下的只能是 godot/clangd 且各带一条**命名了原因**的诊断 |
| A2 | 歧义用例（两个 provider 同 bare 名） | nil **且**日志点名双方 |
| A3 | 冷 home（全新 `XLINGS_HOME`，`--mirror CN`）`xlings install -y xim:gcc@16.1.0` | 退出 0，`gcc/g++/c++` 三个 shim 都注册 |
| A4 | 同一冷 home `xlings install -y meson` | 退出 0 |
| A5 | `readelf -d <libglvnd>/lib/libGLX.so.0` | RUNPATH 含 GLX vendor 目录 |
| A6 | imgui 模板，**默认 dtags** 构建后 `mcpp run` | 出窗口；且 `/proc/self/maps` 里 GL 对象路径在我们的载荷下 |
| A7 | `verify-stack.sh`（去掉 `--disable-new-dtags`） | Fix E 之前**红**，之后**绿** |

A7 是这次最重要的一条：**它现在必须先红**，否则说明改的不是这个缺陷。

---

## 5. 已定的决策（2026-08-10）

| # | 决定 | 结论 |
|---|---|---|
| **D1** | godot → mesa | ✅ **godot 直接声明 `xim:mesa@>=25`**，并按 namespaced 坐标提问。它确实用 mesa 的 libdir，声明是诚实的；不走「把传递依赖记进 `resolved_deps`」那条路——那会把 `resolved_deps` 从「我声明了什么」变成「解析器装了什么」，是更大的语义变更 |
| **D2** | libxpkg 版本 | ✅ **仍叫 0.0.56**。实测 0.0.56 从未发布：mcpp-index 的 `xpkg.lua` 最高 `0.0.55`，git tag 最高 `0.0.55`，`71b9ed7` 只活在未合并的 `fix/dep-install-dir-fallback` 上。所以在该分支上追加 Fix A，一个 PR 发一个 0.0.56 —— 既保留它那批**诊断**（Fix A 之后它们只在真正无解的查询上响），又不制造「同号不同树」的幽灵版本 |
| **D3** | GLX vendor 目录形态 | ✅ **E1** —— 目录放 libglvnd **自己的 payload 内**，`$ORIGIN/glx-vendor`，home 级装齐所有 vendor，per-subos 由 `__GLX_VENDOR_LIBRARY_NAME` 选。理由：vendor 载荷本来就是 per-home 的；「哪个 subos 用哪个 vendor」glvnd 已经有 per-subos 选择器，不需要再用目录表达。E2（subos 绝对路径）会把共享 payload 钉死在一个 subos 上；E3（per-subos stub）不跨包写但多一层，且依赖真 libGLX 保持 DT_RPATH |
| **D4** | 发布方式 | ✅ **#524 与 #525 合并成一轮发布**。#524 是硬失败（任何冷 home 装不上 gcc 和 meson：新机器、CI 冷缓存、`fresh-install`、全部下游 mcpp 构建），必须尽快；#525 同属一个形态，一起发省一轮生态传播 |
| **D5** | 与 mcpp 的桥 | ✅ **串行**：xlings 侧先修好并实测，**再**推动 mcpp 弃用 `compat.glx-runtime` 的 symlink bridge。并行会出现「桥没了、RUNPATH 还没到」的窗口，GL 会完全不可用 |

**E1 的已知代价**（记录在案，不粉饰）：vendor 包要往 libglvnd 的 store 目录里写符号链接，
这是**跨包写**。落地时必须确认 integrity 清理不会把它当孤儿删掉——这条进验收（见 §4 A5）。

实施计划与任务依赖：`.agents/plans/2026-08-10-issue-524-525-implementation.md`
