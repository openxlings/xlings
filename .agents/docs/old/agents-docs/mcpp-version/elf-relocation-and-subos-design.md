# ELF 可重定位与多 subos 设计说明

> 关联文档：[pkg-taxonomy.md](pkg-taxonomy.md)、[rpath-and-os-vision.md](rpath-and-os-vision.md)、[subos-sysroot-design.md](subos-sysroot-design.md)

本文档总结预构建包在 xlings 新架构下的 ELF 解释器/RPATH 问题、包索引与预构建的职责划分、多 subos 共享 xpkgs 时的库路径方案、同一 subos 内多版本依赖冲突，以及 LD_LIBRARY_PATH 的副作用与替代思路。面向包维护者与架构决策参考。

### 问题与方案总结（一览）

| 问题 | 结论/方案 |
|------|-----------|
| 预构建 ELF 解释器写死构建机路径，子进程（如 cc1plus）无法执行 | 由 **xpkg 编写者**在 install() 中调用 **libxpkg** 提供的 `patch_elf_loader_rpath` 等接口，统一改为**系统** loader（及可选 RPATH）。 |
| 包索引与预构建二进制职责 | 索引只做元数据与 install/config 脚本；可重定位由构建约定 + 索引侧 install 共同保证。 |
| 多 subos 共享 xpkgs，各 subos 用各自 lib | **不要**把 interpreter 改成 subos loader；install() 只 patch 到 system loader，config() 设 **LD_LIBRARY_PATH = 当前 subos 的 lib**。 |
| 同一 subos 内 A 依赖 b@0.0.1、C 依赖 b@0.0.2 | **会冲突**：当前 subos/lib 按文件名聚合，后装覆盖先装。可借鉴 Nix per-package 闭包或 npm 式 per-program 路径隔离（见下）。 |
| LD_LIBRARY_PATH 污染子进程 | 会波及整棵进程树；可接受、或尽量 RPATH、或 per-subos 副本 + RPATH、或文档化副作用。 |
| 为何不能用 RPATH 替代 LD_LIBRARY_PATH 实现“按 subos 切换” | RPATH 写死在 ELF 中，无法表达“当前 subos”；必须用运行时 LD_LIBRARY_PATH。包内依赖仍可用 RPATH。 |
| elfpatch 接口与双模式 | **接口**：单 ELF 或目录扫描；rpath 显式传入或 closure_lib_paths()。**双模式**：elfpatch.auto(false) 默认手动，auto(true) 时 xim 自动 patch。任务与验证见第四节下「elfpatch 接口设计与双模式」。 |

---

## 一、问题背景：预构建包 ELF 解释器写死路径

预构建包（如 XLINGS_RES 的 gcc）在构建环境中将 ELF 的 **PT_INTERP** 写死为构建机路径（如 `/home/xlings/.xlings_data/subos/linux/lib/ld-linux-x86-64.so.2`）。安装到其他用户或路径后，该解释器不存在，导致：

- 顶层程序（gcc/g++）通过 xvm shim 执行时，xlings 可用系统 loader 做运行时 fallback；
- 但子进程（cc1plus、as、ld 等）由内核直接加载，不经过 shim，会报错：`cannot execute '.../cc1plus': No such file or directory`。

因此必须在**安装或配置阶段**修正包内所有 ELF 可执行文件的解释器（及可选 RPATH），使同一份预构建在任意 `XLINGS_DATA` 下可用。

---

## 二、推荐方案概览

### 2.1 安装时统一 patch 到系统 loader

- **时机**：在 xpkg 的 `install()` 完成解压/复制后，对 `pkginfo.install_dir()` 下所有 ELF 可执行文件执行一次 `patchelf --set-interpreter <系统 loader>`。
- **系统 loader**：如 `/lib64/ld-linux-x86-64.so.2`、`/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2`，或通过 `readelf -l /bin/sh` 从系统二进制解析。
- **依赖**：主机需安装 `patchelf`；缺失时可 log.warn 并跳过，或约定文档说明。

### 2.2 loader / RPATH 由 xpkg 编写者做，libxpkg 提供便捷接口（当前方案）

**当前采用**：loader / RPATH 的设置在 **xpkg 的 install() 里由包编写者完成**；**libxpkg** 只提供通用、易用的接口，不要求 xim 在流程末尾统一调用。

- **职责**：xpkg 编写者在解压/复制后，按需调用 libxpkg 的接口对 `pkginfo.install_dir()` 下 ELF 做 interpreter（及可选 RPATH）；libxpkg 封装 patchelf 细节、路径约定和跨平台 no-op。
- **好处**：包可按需、按范围（如只 patch bin/）处理；不依赖 xim 增加 post-install 钩子；接口清晰，便于在 add-xpackage 文档中写示例。
- **libxpkg 建议提供的接口**（详见下节「elfpatch 接口设计与双模式」）：
  - **elfpatch.patch_elf_loader_rpath(target, opts)**：`target` 可为**单个 ELF 路径**或**目录**（目录时自动扫描 ELF）；`opts.loader`、`opts.rpath`（显式列表或字符串）、`opts.include_shared_libs` 等；编写者可显式通过 pkginfo 获取依赖 install_dir 后手动拼 rpath 列表传入。
  - **elfpatch.closure_lib_paths()**：按当前包 deps 做确定性解析返回闭包 lib 路径列表，供 `opts.rpath` 使用。
  - **elfpatch.auto(enable)**：默认 **false**（手动模式）；设为 **true** 时由 xim 在 install 末尾自动检测依赖并设置当前包的 loader 与 rpath。

示例（xpkg 在 install 末尾调用）：

```lua
import("xim.libxpkg.elfpatch")
-- 只改 interpreter
elfpatch.patch_elf_loader_rpath(pkginfo.install_dir(), { loader = "system" })
-- 或 interpreter + 依赖闭包 RPATH（若提供 closure_lib_paths）
elfpatch.patch_elf_loader_rpath(pkginfo.install_dir(), {
    loader = "system",
    rpath  = elfpatch.closure_lib_paths(),  -- 可选，需确定性实现
    include_shared_libs = true,
})
```

- **与「xim 统一做」的关系**：若后续希望「所有包自动带 interpreter/RPATH、包内不写调用」，可在 xim 的 install 流程末尾增加统一调用；届时 libxpkg 接口仍可复用，xpkg 内调用变为可选覆盖。

---

## 三、包索引与预构建二进制的职责划分

| 层次 | 职责 | 位置 |
|------|------|------|
| **包索引** | 元数据、版本表、install/config/uninstall 脚本；不负责“如何编译出二进制” | xim-pkgindex `pkgs/*.lua` |
| **预构建二进制** | 由 CI 或上游产出并托管；索引只引用（URL 或 XLINGS_RES） | xlings-res / 上游 Release |

索引与二进制解耦；可重定位性由**构建约定**（CI 侧尽量用系统 loader 或构建后 patchelf）与**索引侧 install 钩子**（对已知写死路径的包做 patchelf）共同保证。

---

## 四、多 subos 共享 xpkgs：loader 与库路径分离

xpkgs 为**多 subos 共享**。若在 config() 里把 ELF 的 interpreter 改成**当前 subos** 的 loader，换 subos 再 config 会覆盖，导致其他 subos 无法运行。因此：

- **不要**在 config() 里把 interpreter patch 成 subos loader。
- **推荐**：
  - **install()**：仅做一次，用 `loader = "system"` 把包内 ELF 的 interpreter 统一改为**系统** loader。
  - **config()**（每个 subos 各执行一次）：在 xvm 注册程序时，设置 **LD_LIBRARY_PATH = 当前 subos 的 lib**（如 `system.subos_sysrootdir() .. "/lib"`）。
  - **运行时**：同一份二进制（interpreter = 系统 loader），由 shim 按当前 subos 注入 LD_LIBRARY_PATH，各 subos 实际加载各自的 lib，多 subos 同时生效，无需 per-subos 副本。

**同一 subos 内多版本**（如 gcc@15.1.0 / gcc@13.3.0）：每版本独立目录，install 时各自 patch；config 按 (subos, 版本) 设 LD_LIBRARY_PATH，逻辑不变，不增加 loader/rpath 复杂度。

---

## 四（续）、同一 subos 内多版本依赖是否会冲突

**会冲突。** 当前实现下，同一 subos 内若 **A 依赖 b@0.0.1、C 依赖 b@0.0.2**，会出现依赖版本冲突。

**原因**：

- 安装 A 时会把 A 的依赖列表里的 b@0.0.1 的 `.so` 以**文件名**（如 `libb.so.1`）**链接/复制到 subos/lib**（`aggregate_dep_libs_to`）。
- 安装 C 时同样会把 b@0.0.2 的 `libb.so.1` 写入 **同一个** subos/lib，并用 `force = true` **覆盖**已有同名文件。
- 因此 subos/lib 里**只会保留一份**同名库（后安装的版本）。A 运行时通过 LD_LIBRARY_PATH=subos/lib 加载到的是 B 0.0.2，而不是 A 声明的 B 0.0.1，若 ABI 不兼容就会出错。

**结论**：当前模型是「一个 subos 一个统一的 lib 目录」，按**文件名**聚合依赖，**不支持**同一 subos 内对同一库的多版本并存；后安装的包会覆盖先安装的包对同一依赖的版本。

**可选方向**（需改设计或实现）：

- **按程序单独设 LD_LIBRARY_PATH**：每个程序在 xvm 注册时只把**该程序声明的依赖**的 lib 路径加入 LD_LIBRARY_PATH（例如 A 只带 b@0.0.1 的 lib，C 只带 b@0.0.2 的 lib），不再把所有依赖统一聚合到 subos/lib。这样 A 和 C 各自加载正确版本，但实现上需要 per-program 的依赖解析与路径注入。
- **per-program 路径直接指向 xpkgs**：不按版本在 subos/lib 下分子目录；每个程序的 LD_LIBRARY_PATH 直接列出 data/xpkgs/<pkg>/<ver>/lib，subos 只做版本视图与管理，保留统一的 subos/lib/（可选做扁平默认链接）。
- **文档化现状**：在文档中明确「同一 subos 内同一库仅能存在一个版本，后安装者覆盖；若需多版本共存请使用多个 subos 或等待后续 per-program 依赖解析」。

### 其他工具如何应对“同一环境内多版本依赖”

| 工具 | 策略 | 说明 |
|------|------|------|
| **Conda** | 一个环境一个版本 + SAT 求解器 | 一个 env 里**每个包只保留一个版本**。求解器为整个环境找一组满足所有约束的版本；若 A 要 B 0.0.1、C 要 B 0.0.2 且无公共兼容版本，则**求解失败**，建议拆成多个环境。不在一 env 内共存多版本。 |
| **Nix** | 无共享 lib 目录，按 closure 引用 | 每个包有自己的 **closure**（依赖的 store 路径）。A 的二进制 RPATH/soname 指向 B 0.0.1 的 store 路径，C 指向 B 0.0.2。store 里可同时存在多版本，**同一 profile 内** A 和 C 各用各的，无冲突。 |
| **npm** | 尽量扁平 + 冲突时嵌套 | 尽量把依赖装在顶层 `node_modules`；若 A 要 lodash@3、B 要 lodash@4，则把冲突版本装到**依赖方子目录**（如 `A/node_modules/lodash@3`），多版本通过**路径隔离**共存，解析时按 Node 的模块查找顺序找。 |
| **Spack** | 环境显式支持多 spec | 一个 environment 可显式包含**同一包的多份 spec**（不同版本/变体），用于 build 与 run 依赖不同、或不同包需要不同版本；通过 manifest + lock 记录完整依赖图。 |
| **xlings（当前）** | 单一 subos/lib，按文件名聚合 | 所有依赖的 .so 按**文件名**聚合到 subos/lib，后安装覆盖先安装，**不支持**同库多版本共存。 |

### 可参考方案详解：Nix per-package 闭包 与 npm 按依赖方隔离路径

若要支持「A 依赖 b@0.0.1、C 依赖 b@0.0.2」同 subos 共存，可借鉴以下两种思路。下面先分别说明**原理**，再给出在 xlings 中的**做法与代价**。

---

#### Nix 式 per-package 闭包 — 原理详解

**核心思想**：**不存在“环境级的共享 lib 目录”**；每个包（程序）的依赖在**构建/安装时**就确定为一组**绝对路径**，并写进该包自己的二进制（或 profile 元数据），运行时**只从这些路径**找库，因此不同包可以指向同一库的不同版本而互不干扰。

**1. Store 与路径唯一性**

- Nix 把所有构建产物放在 **store** 里，每个 derivation（包版本）对应一条**内容寻址**的路径，例如 `/nix/store/<hash>-b-0.0.1/lib/libb.so.1`、`/nix/store/<hash>-b-0.0.2/lib/libb.so.1`。同一库的不同版本一定在不同路径下，**不会出现“同名文件在一个目录里被覆盖”**。

**2. 闭包（closure）**

- 一个包的**闭包**定义为：该包及其**全部传递依赖**所对应的 store 路径的集合。例如 A 依赖 b@0.0.1，b@0.0.1 依赖 z@1，则 A 的闭包 = {A 的 store 路径, b@0.0.1 的 store 路径, z@1 的 store 路径}。闭包在**构建/安装时**就完全确定，与“当前环境”无关。

**3. 依赖如何写进二进制**

- 构建 A 时，链接器把 A 的**直接依赖**（如 b@0.0.1）的 store 路径写进 A 的 ELF：**RPATH/RUNPATH** 或 **DT_NEEDED 的 soname 为绝对 store 路径**。因此 A 的二进制里“写死”的是 `/nix/store/xxx-b-0.0.1/lib`，而不是一个公共目录。同样，C 的二进制里写的是 b@0.0.2 的 store 路径。  
- 运行时，动态链接器按 ELF 里的 RPATH/NEEDED 找库，**不会**去查一个统一的“环境 lib 目录”，因此 A 永远加载 b@0.0.1，C 永远加载 b@0.0.2。

**4. 为什么能多版本共存**

- 因为**没有共享的 lib 目录**：每个包只“看见”自己闭包里的路径。b@0.0.1 和 b@0.0.2 在 store 里是两条路径，A 只引用前者，C 只引用后者，**不存在“同一个目录下两个版本抢同一个文件名”的问题**。

**5. 映射到 xlings 的要点**

- xlings 的 xpkgs 已具备“多版本分目录”（如 `xpkgs/b/0.0.1/`、`xpkgs/b/0.0.2/`），相当于 store 的简化版。  
- 若采用 Nix 式思路：**不要**把依赖聚合到 subos/lib，而是在 install 或 config 时对**每个程序**计算其依赖闭包（deps + 传递），把闭包中的 lib 路径（如 `xpkgs/b/0.0.1/lib64`）写进该程序二进制或该程序单独使用的 RPATH；或退而求其次，把这条路径列表作为**仅该程序**的 LD_LIBRARY_PATH 写入 xvm（见下）。  
- 本质：**依赖信息与“包/程序”绑定，而不是与“环境/目录”绑定**；查找库时按“该程序的闭包”而不是“环境统一目录”。

---

#### npm 式按依赖方隔离路径 — 原理详解

**核心思想**：**按“谁依赖”来划分“谁用哪一份”**；同一库的多个版本可以同时存在于磁盘上的**不同路径**，每个依赖方只在自己的**解析作用域**里看到自己声明的那一份版本，通过**解析顺序/路径优先级**而不是“一个全局目录”决定用哪个版本。

**1. 模块解析规则（Node.js）**

- 当 A 请求 `require('b')` 时，Node 从 **A 所在目录** 开始，先看 `A/node_modules/b` 是否存在，再向上层目录找 `node_modules/b`，直到根。因此**先找到的 b 即被采用**。不同包可以位于不同层级，各自目录下的 `node_modules/b` 可以是不同版本。

**2. 目录布局（扁平 + 冲突时嵌套）**

- npm 安装时尽量把依赖**提升**到顶层 `node_modules`（扁平化），以共享同一版本。  
- 当**无法满足所有依赖的版本约束**时（例如顶层已放了 lodash@4 以满足 B，而 A 需要 lodash@3），npm 会把**冲突的那一版**装在**依赖方下面**：`A/node_modules/lodash` = 3.x。这样：
  - 顶层 `node_modules/lodash` = 4（给 B 用）；
  - A 请求 `require('lodash')` 时，从 A 所在目录向上找，先遇到 `A/node_modules/lodash`（3），故 A 用 3；B 用顶层的 4。  
- **多版本共存**靠的是**路径隔离**：不同版本在不同路径下，解析时“谁先被找到谁生效”，而“谁先被找到”由**请求方所在目录**决定，因此等价于“按依赖方隔离”。

**3. 为什么能多版本共存**

- 因为**没有把“同一库名”压到同一个目录**：lodash@3 和 lodash@4 分别位于 `A/node_modules/lodash` 与 `node_modules/lodash`（或 B/node_modules/lodash），文件名可以相同，但路径不同；解析时根据**调用方位置**走不同路径，因此不会互相覆盖，也不会误用版本。

**4. 映射到 xlings：per-program 的 LD_LIBRARY_PATH**

- 在 xlings 里没有 Node 的“从当前模块目录向上找 node_modules”的解析器，但可以用**环境变量**达到类似效果：**每个程序（相当于一个“依赖方”）拥有自己的一条 LD_LIBRARY_PATH**，这条路径里**只包含该程序声明的依赖**（及传递闭包）的 lib 目录，且**不包含**“其他程序专用的”版本路径。  
- 例如 A 依赖 b@0.0.1，则 A 的 LD_LIBRARY_PATH = `xpkgs/b/0.0.1/lib64`（若有传递依赖则拼接更多路径）；C 依赖 b@0.0.2，则 C 的 LD_LIBRARY_PATH = `xpkgs/b/0.0.2/lib64`。  
- 运行时：启动 A 时 shim 只注入 A 的 LD_LIBRARY_PATH，动态链接器在 A 的路径里找到的只能是 b@0.0.1；启动 C 时只注入 C 的路径，C 只能找到 b@0.0.2。**“按依赖方隔离”** 体现为 **“按程序（依赖方）给出不同的库搜索路径”**，而不是一个统一的 subos/lib。

**5. 与 Nix 式的区别**

- **Nix**：依赖路径写进**二进制**（RPATH/soname），不依赖进程环境；子进程若来自同一包树，也沿用同一闭包。  
- **npm 式（xlings 用 LD_LIBRARY_PATH 实现）**：依赖路径写进**该程序的运行环境**（LD_LIBRARY_PATH），实现简单，但会被子进程继承；若子进程是系统工具或其他包，可能误用该路径。  
- 两者都能实现“A 用 b@0.0.1、C 用 b@0.0.2”；Nix 式更“干净”（无全局 env 污染），npm 式更易在现有 xvm 上落地（per-program env 已有）。

---

#### 小结：做法与代价（与上对应）

**Nix 式 per-package 闭包**

- **做法**：不聚合到 subos/lib；为每个程序计算依赖闭包，把闭包中的 lib 路径写入该程序二进制的 RPATH，或（折中）作为该程序专用的 LD_LIBRARY_PATH。
- **优点**：无共享 lib 目录，无覆盖；多版本自然共存；若用 RPATH 则子进程不依赖 env。
- **代价**：需 per-program 依赖解析与闭包计算；若写 RPATH 需对包内所有 ELF 做 patchelf，且传递依赖要完整。

**npm 式按依赖方隔离路径**

- **做法**：每个程序在 xvm 注册时，根据该程序 deps（及传递闭包）拼出**仅该程序**的 LD_LIBRARY_PATH，不再统一聚合到 subos/lib。
- **优点**：A 与 C 各用各的 B 版本；复用现有 deps 与 xvm per-program envs，改动集中在 config 时生成 LD_LIBRARY_PATH。
- **代价**：LD_LIBRARY_PATH 会继承给子进程；传递依赖需一并加入路径。

两种思路可结合：包自身依赖用 RPATH（$ORIGIN/../lib64），仅“subos 或传递依赖”用 per-program 的 LD_LIBRARY_PATH。

---

#### RPATH 设置应由 xpkg 还是 xlings/xim 处理（data/xpkgs + RPATH + subos 视图）

在「data/xpkgs 存真实包 + 依赖用 RPATH 写进二进制 + subos 仅作版本视图」的设计下：

- **闭包（依赖列表与路径）由谁算**：若 xpkg 需要设 RPATH，闭包路径可由 **libxpkg 提供便捷接口**（如 `elfpatch.closure_lib_paths()`）按当前包 deps 做**确定性解析**后返回，避免包编写者自己解析带来的不确定性；或由 xim 在将来「统一做」时在核心计算。
- **loader / RPATH 的写入由谁做**：
  - **当前方案：xpkg 编写者做**：在各自 `install()` 里按需调用 **libxpkg** 的 `elfpatch.patch_elf_loader_rpath(root_dir, opts)`；libxpkg 提供若干方便接口（见 2.2 节）。  
    - **优点**：包可精细控制范围与参数；不依赖 xim 增加 post-install 钩子；接口清晰、易文档化。  
    - **实现要点**：libxpkg 提供 `patch_elf_loader_rpath`，可选提供 `closure_lib_paths()`（用 match_package_version + 安装路径约定的确定性实现），供 xpkg 一行传入 `rpath`。
  - **可选/后续：xim 统一做**：在 install 流程末尾由 xim 对本次 `install_dir` 统一调用 elfpatch，所有包自动带 interpreter/RPATH，xpkg 内调用变为可选覆盖。  
    - **优点**：包维护者无需每人写一遍，不易遗漏。  
    - **实现要点**：核心在 install 结束时拿到闭包路径列表并调用 elfpatch。

**结论**：**目前** loader / RPATH 的设置**由 xpkg 编写者负责**，**libxpkg 提供便捷接口**（`patch_elf_loader_rpath`、可选的 `closure_lib_paths()` 等）；xim 不强制在流程末尾统一处理。若后续希望「默认自动 patch、包内可选覆盖」，再在 xim 增加统一调用即可，libxpkg 接口可复用。

---

#### 伪代码示例：xpkg 声明与调用 libxpkg vs xim 流程（当前方案：由 xpkg 编写者做）

**1. xpkg 侧（xim-pkgindex）：声明依赖，在 install() 末尾调用 libxpkg 做 loader/RPATH**

xpkg 在包表中声明依赖与程序列表；`install()` 内解压/复制后，**由包编写者**调用 libxpkg 的便捷接口对 ELF 做 interpreter（及可选 RPATH）。

```lua
-- pkgs/g/gcc.lua（节选，仅示意）
package = {
    name = "gcc",
    programs = { "gcc", "g++", "cpp", ... },
    xpm = {
        linux = {
            deps = { "glibc@2.39", "binutils@2.42", "linux-headers@5.11.1" },
            ["15.1.0"] = "XLINGS_RES",
            ...
        },
    },
}

function install()
    local srcdir = pkginfo.install_file():gsub(".tar.gz$", "")
    os.tryrm(pkginfo.install_dir())
    os.cp(srcdir, pkginfo.install_dir(), { symlink = false })

    -- 由 xpkg 编写者调用 libxpkg 便捷接口（当前方案）
    if is_host("linux") then
        import("xim.libxpkg.elfpatch")
        elfpatch.patch_elf_loader_rpath(pkginfo.install_dir(), {
            loader = "system",
            rpath  = elfpatch.closure_lib_paths(),  -- 可选：确定性闭包路径，由 libxpkg 提供
            include_shared_libs = true,
        })
    end
end

function config()
    -- 注册 programs、写 xvm workspace 等；LD_LIBRARY_PATH 可由 xim 按闭包生成
end
```

**2. xim 侧（xlings）：递归安装依赖、执行 xpkg install/config**

- 安装前：根据 `target` 解析出要装的 (pkg, ver)，递归安装 `deps`。
- 安装：执行 xpkg 的 `install()`（其内已含对 libxpkg 的调用），再执行 `config()`。
- **当前**不在 xim 流程末尾增加「统一 patch ELF」步骤；loader/RPATH 由 xpkg 在 install() 内完成。

```lua
-- 伪代码：CmdProcessor 中（现有逻辑为主）
local deps_list = self._pm_executor:deps()
for _, dep_spec in ipairs(deps_list) do
    new(dep_spec, { install = true, yes = true }):run()
end

if not self._pm_executor:install(xpkg) then return end
-- install() 内已由 xpkg 调用 elfpatch，此处无需再 patch

-- config：写 xvm、per-program LD_LIBRARY_PATH 等
-- ...
```

**3. libxpkg 提供的便捷接口（xlings 内实现，供 xpkg 调用）**

```lua
-- core/xim/libxpkg/elfpatch.lua（伪代码）
function patch_elf_loader_rpath(root_dir, opts)
    if not is_host("linux") then return end
    local loader = opts.loader == "system" and _detect_system_loader() or opts.loader
    local rpath  = opts.rpath
    if rpath and type(rpath) == "table" then rpath = table.concat(rpath, ":") end
    for _, elf_path in ipairs(_collect_elf_under(root_dir, opts.include_shared_libs)) do
        patchelf_set_interpreter(elf_path, loader)
        if rpath and rpath ~= "" then patchelf_set_rpath(elf_path, rpath) end
    end
end

-- 可选：按当前包 deps 做确定性解析，返回闭包 lib 路径列表（见下节「闭包路径解析」）
function closure_lib_paths()
    local deps_list = runtime.get_pkginfo().deps_list or _get_current_pkg_deps()
    local paths = {}
    for _, dep_spec in ipairs(deps_list) do
        local dep_dir = _closure_resolve_dep_install_dir(dep_spec)
        if dep_dir then
            for _, sub in ipairs({"lib64", "lib"}) do
                local d = path.join(dep_dir, sub)
                if os.isdir(d) then paths[#paths+1] = d; break end
            end
        end
    end
    return paths
end
```

**小结**：**当前方案**下，xpkg 声明 `deps` 与 `programs`，在 `install()` 里解压/复制后**由包编写者**调用 libxpkg 的 `patch_elf_loader_rpath`（及可选的 `closure_lib_paths()`）；xim 只负责递归安装依赖与执行 install/config，不在此处统一 patch ELF。

---

#### 闭包路径解析：用「与 aggregate_dep_libs_to 一致」是否有不确定性、能否覆盖要求

**结论**：若直接用现有 `_resolve_dep_dir_via_xvm` / `_resolve_dep_dir_via_scan`（与 aggregate_dep_libs_to 一致）来算「当前包依赖的闭包路径」，**具备不确定性**，**不能可靠覆盖「RPATH 必须指向本次安装实际用到的依赖」这一要求**。

**不确定性来源**

| 方式 | 行为 | 问题 |
|------|------|------|
| **xvm** | 用 `xvm.info(name, version)` 取当前 **xvm 激活/workspace** 下该包的 SPath/Version | 激活版本未必是「递归安装当前包时刚装上的那一版」。例如 gcc 依赖 glibc@2.39 且已装好，但 workspace 里 glibc 激活的是 2.40 → 会得到 2.40 的路径，RPATH 写错。 |
| **scan** | 在 xpkgs 下按 name 找目录，无 version 时取 **排序后最后一个** 版本 | 多版本并存时（如 glibc 2.39 与 2.40）取到哪一版依赖「目录排序」和「是否传了 version」，与「本次安装时实际为 gcc 选的 glibc 版本」可能不一致。 |

因此：闭包路径必须与「**本次安装流程里为当前包解析并安装的依赖**」一一对应，而不能依赖「当前环境里 xvm 认为的版本」或「扫描到的任意一个版本」。

**确定性实现（能覆盖要求）**

- 与**递归安装依赖**使用**同一套解析**：对每个 `dep_spec`（如 `"glibc@2.39"`）用 **`index_manager:match_package_version(dep_spec)`** 得到与 `new(dep_spec, { install = true }):run()` 时相同的解析结果（如 `"glibc 2.39"`）。
- 与**安装路径约定**一致：用与 PkgManagerExecutor 相同的规则算 `install_dir`，即 `path.join(runtime.get_xim_install_basedir(), effective_pkgname, version)`，其中 `effective_pkgname` 为带 namespace 时的 `namespace .. "-x-" .. name` 或 `name`，与安装时一致。
- 这样得到的闭包路径与「本次安装树中实际用到的依赖目录」一一对应，RPATH 可覆盖要求。

**推荐实现方式（二选一或组合）**

1. **按 dep_spec 即时解析**：在算闭包时对每个 `dep_spec` 调用 `match_package_version(dep_spec)`，再 load 该包得到 `(name, namespace, version)`，用 `path.join(basedir, effective_pkgname, version)` 得到 `dep_install_dir`。不依赖 xvm/scan，与安装流程同源、可复现。
2. **递归安装时落盘记录**：在递归安装每个依赖后，把 `(dep_spec → 实际 install_dir)` 写入当前包可读的上下文（如 runtime 或临时结构），算闭包时直接查表。保证「闭包路径 = 本次安装真实用到的路径」。

伪代码（确定性解析，与安装约定一致）：

```lua
-- 确定性：与 match_package_version + 安装路径约定一致，不依赖 xvm/scan
function _closure_resolve_dep_install_dir(dep_spec)
    local dep_pkgname = index_manager:match_package_version(dep_spec)  -- 与递归 install 时相同
    if not dep_pkgname then return nil end
    local dep_pkg = index_manager:load_package(dep_pkgname)
    if not dep_pkg then return nil end
    local effective = dep_pkg.namespace and (dep_pkg.namespace .. "-x-" .. dep_pkg.name) or dep_pkg.name
    local install_dir = path.join(runtime.get_xim_install_basedir(), effective, dep_pkg.version)
    return os.isdir(install_dir) and install_dir or nil
end
```

**小结**：用 deps_list + 现有 _resolve_dep_dir_* 算闭包**具备不确定性**，不能可靠覆盖「RPATH 指向本次安装实际依赖」的要求；应改为用 **match_package_version + 安装路径约定**（或递归安装时记录）做**确定性**闭包路径解析。

---

#### elfpatch 接口设计与双模式（xim 自动 / xpkg 手动）

**1. 接口约定**

- **patch_elf_loader_rpath(target, opts)**  
  - **target**：可为 **单个 ELF 文件路径** 或 **目录路径**。  
    - 单文件：仅对该 ELF 设置 interpreter / RPATH。  
    - 目录：对该目录下 ELF 进行**自动扫描**（可配置是否递归、是否包含 .so），对扫描到的每个 ELF 设置 interpreter / RPATH。  
  - **opts**：`loader`（"system"|"subos"|"/path"）、`rpath`（字符串或路径列表）、`include_shared_libs`（目录模式下是否包含 .so）、`recurse`（目录是否递归）等；非 Linux 做 no-op。

- **rpath 的两种来源（编写者任选）**  
  - **显式指定**：编写者通过 pkginfo 或现有接口获取依赖的 install_dir，自行拼出准确的 **rpath 列表**（如各依赖的 lib/lib64 绝对路径），传给 `opts.rpath`。  
  - **辅助接口**：`elfpatch.closure_lib_paths()` 用确定性解析返回当前包 deps 的闭包 lib 路径列表，编写者设置 `opts.rpath = elfpatch.closure_lib_paths()` 即可。

**2. 双模式：auto(true) vs 手动（默认 auto(false)）**

- **elfpatch.auto(enable)**  
  - 设置是否启用 **xim 自动为当前包设置 loader 和 rpath**。  
  - **默认 false**：不自动；由 xpkg 编写者在 install() 中按需调用 `patch_elf_loader_rpath`，rpath 可显式传入或使用 `closure_lib_paths()`。  
  - **auto(true)**：启用自动模式；xim 在**当前包 install() 执行完成后**，自动检测当前包依赖（确定性解析），对 `pkginfo.install_dir()` 下 ELF 统一设置 loader + 闭包 RPATH；xpkg 内可不写 patch 调用（写了则先执行包内调用，再执行自动逻辑时可按策略覆盖或跳过，由实现约定）。

- **方案对比**

| 模式 | 设置 | 行为 |
|------|------|------|
| **手动（默认）** | `elfpatch.auto(false)` 或不调用 auto | xim 不自动 patch；编写者在 install() 中调用 `patch_elf_loader_rpath(单文件或目录, opts)`，rpath 显式列出或用 `closure_lib_paths()`。 |
| **xim 自动** | `elfpatch.auto(true)`（在 install 前或包内设置） | xim 在 install 流程末尾检测当前包 deps，对 install_dir 下 ELF 自动设置 system loader + 确定性闭包 RPATH。 |

**3. 使用示例（编写者）**

```lua
-- 单 ELF
elfpatch.patch_elf_loader_rpath(path.join(pkginfo.install_dir(), "bin", "gcc"), { loader = "system", rpath = my_rpath_list })

-- 目录扫描（当前包 install_dir 下所有 ELF）
elfpatch.patch_elf_loader_rpath(pkginfo.install_dir(), {
    loader = "system",
    rpath  = elfpatch.closure_lib_paths(),
    include_shared_libs = true,
})

-- 显式 rpath：通过 pkginfo 等拿到依赖 install_dir 后手动拼列表
local deps_list = pkginfo.deps_list or {}
local rpath_list = {}
for _, dep_spec in ipairs(deps_list) do
    local dep_dir = _my_resolve_dep_install_dir(dep_spec)  -- 或使用 libxpkg 提供的确定性解析接口
    if dep_dir then
        for _, sub in ipairs({"lib64", "lib"}) do
            local d = path.join(dep_dir, sub)
            if os.isdir(d) then table.insert(rpath_list, d); break end
        end
    end
end
elfpatch.patch_elf_loader_rpath(pkginfo.install_dir(), { loader = "system", rpath = rpath_list })
```

**4. 任务拆解**

| 序号 | 任务 | 说明 |
|------|------|------|
| T1 | **libxpkg.elfpatch：patch_elf_loader_rpath(target, opts)** | 实现 target 为**单文件**时仅 patch 该 ELF；target 为**目录**时扫描目录下 ELF（可选递归、可选含 .so），对每个 ELF 设置 loader 和可选的 rpath。依赖 patchelf；非 Linux no-op。 |
| T2 | **libxpkg.elfpatch：closure_lib_paths()** | 按当前包 deps 做**确定性解析**（match_package_version + 安装路径约定）返回闭包 lib 路径列表；供编写者 opts.rpath 使用。需能拿到当前包 deps_list（如 runtime/pkginfo 注入）。 |
| T3 | **libxpkg.elfpatch：auto(enable) 与查询** | 提供 `elfpatch.auto(enable)` 设置是否自动；`elfpatch.is_auto()` 或等价查询供 xim 在 install 末尾判断是否执行自动 patch。默认 false。 |
| T4 | **xim：install 末尾根据 auto 执行自动 patch** | 当 `elfpatch.is_auto() == true` 时，在 xpkg install() 返回后、config() 前，对当前 pkginfo.install_dir 用确定性闭包调用 `patch_elf_loader_rpath(install_dir, { loader = "system", rpath = closure_lib_paths() })`；否则不调用。 |
| T5 | **文档与示例** | 在 add-xpackage（或等价）中补充：单 ELF / 目录扫描、显式 rpath vs closure_lib_paths()、auto(true/false) 的用法与适用场景。 |

**5. 验证方法**

| 验证项 | 操作 | 预期 |
|--------|------|------|
| **单 ELF** | 对单个可执行文件调用 patch_elf_loader_rpath(elf_path, opts)，用 readelf -l 查看 | PT_INTERP 为系统 loader；若传 rpath 则 RUNPATH 为给定路径。 |
| **目录扫描** | 对 install_dir 调用 patch_elf_loader_rpath(install_dir, opts)，遍历 bin/ 与 lib/ 下 ELF | 所有目标 ELF 的 interpreter 与 rpath 均被设置一致。 |
| **显式 rpath** | xpkg 中手动拼 rpath 列表（依赖的 lib 路径）并传入，安装后运行依赖该库的程序 | 程序能正确加载依赖库（无 LD_LIBRARY_PATH 时也能运行）。 |
| **closure_lib_paths** | xpkg 使用 opts.rpath = elfpatch.closure_lib_paths()，安装后检查 ELF 的 RUNPATH | RUNPATH 包含当前包 deps 的 lib 路径（与递归安装的依赖一致）。 |
| **auto(false) 默认** | 不调用 auto(true)，且 xpkg 内不写 patch 调用，安装包 | xim 不执行自动 patch；若包内也未调用，则 ELF 保持未 patch（或仅 CI/构建侧已 patch）。 |
| **auto(true)** | 在合适处设置 elfpatch.auto(true)，xpkg 内不写 patch，安装包 | xim 在 install 后自动对 install_dir 下 ELF 设置 loader + 闭包 RPATH；readelf 验证与手动调用结果一致。 |

---

#### 方案设计：Nix 式 per-program 解析 + 保留聚合 lib

若希望**既做到 Nix 式的“每个程序只看到自己闭包、多版本无冲突”**，**又保留“所有库聚合在一个目录树下”的便利**（便于浏览、备份、或与现有“subos/lib 即环境 lib”的直觉一致），可采用**按版本分目录聚合 + per-program 指向版本路径**的混合设计。

**1. 数据与视图分离**

- **data/xpkgs**：存放**实际的**库和应用（二进制、.so 等），如 `data/xpkgs/b/0.0.1/lib/libb.so.1`、`data/xpkgs/a/1.0/bin/a`。安装时只写入 xpkgs，不复制到 subos。
- **subos**：只做**版本视图**（哪个程序用哪一版依赖）和**管理**（xvm workspace、激活版本等）。subos 下是**统一的** `lib/`（即 `subos/lib/` 一个目录），不按 (pkg, ver) 再分子目录。

**2. 版本视图的实现：per-program 路径直接指向 xpkgs**

- 每个程序在 xvm 注册时，根据其 **deps（及传递闭包）** 算出一条 **LD_LIBRARY_PATH**，其中**直接列出 data/xpkgs 下的路径**，例如：
  - A 依赖 b@0.0.1 → A 的 LD_LIBRARY_PATH 含 `data/xpkgs/b/0.0.1/lib`（或 lib64；若 b 依赖 z@1，则再含 `data/xpkgs/z/1.0/lib`）；
  - C 依赖 b@0.0.2 → C 的 LD_LIBRARY_PATH 含 `data/xpkgs/b/0.0.2/lib`。
- 启动时只注入该程序的路径；**解析时直接到 xpkgs 取库**，无需经过 subos/lib 下的 (pkg, ver) 子目录。多版本自然共存，无覆盖。

**3. 统一的 subos/lib/ 的用途**

- **subos/lib/** 保持为**统一的一个目录**（不设 subos/lib/<pkg>/<ver>/）。可用于：
  - **兼容或默认视图**：若需“不指定版本也能找到库”的脚本或临时二进制，可将**默认版本**的 .so 链到 `subos/lib/`（如 `subos/lib/libb.so.1` → `data/xpkgs/b/<默认版本>/lib/libb.so.1`），与当前“按文件名压平到 subos/lib”的旧行为一致；
  - 或在不做多版本隔离时，继续把当前 subos 用到的依赖链到统一 subos/lib/。
- 多版本隔离与正确解析**不依赖** subos/lib 的目录结构，而是依赖 **per-program 的 LD_LIBRARY_PATH 直接指向 xpkgs**。

**4. 效果小结**

| 维度 | 说明 |
|------|------|
| **xpkgs** | 实际库和应用只存于 data/xpkgs；安装即写入 xpkgs，不复制到 subos。 |
| **subos** | 版本视图 + 管理；subos 下为**统一的** lib/（一个目录），不设 subos/lib/<pkg>/<ver>/。 |
| **版本隔离** | per-program 闭包：每个程序的 LD_LIBRARY_PATH **直接**列出 xpkgs 路径（如 data/xpkgs/b/0.0.1/lib），实现 Nix 式无冲突。 |
| **实现要点** | ① install 时只写 xpkgs；② config 时为每个程序计算闭包，把闭包对应的 **xpkgs/<pkg>/<ver>/lib** 拼成 LD_LIBRARY_PATH 写入 xvm；③ 可选：统一 subos/lib/ 下做扁平默认链接（subos/lib/<soname> → xpkgs/<pkg>/<默认版本>/lib/<soname>）用于兼容。 |

**5. 与 xlings 现有架构一致**

- 已有**统一的** data/xpkgs，subos 通过链接或引用使用；本方案不引入 subos/lib/<pkg>/<ver>/，仅用**统一的 subos/lib/** 配合 per-program 指向 xpkgs 的路径，实现“xpkgs 放实际库和应用、subos 下是版本视图”。

**6. 与“在 subos/lib 下再按 (pkg,ver) 建树”的对比**

- **若在 subos 下建 subos/lib/<pkg>/<ver>/**：聚合视图更直观，但目录结构复杂，且与“subos 下统一 lib/”的约定不符。
- **本方案**：统一 subos/lib/；版本视图完全由 **per-program 指向 xpkgs 的路径** 表达，实现简单、与“xpkgs 存实际、subos 做视图”一致。

---

#### 角色划分：应用/库直接映射 xpkgs，subos 仅做聚合与版本隔离与管理

**设计概括**

- **应用与库的安装**：二进制和库文件**只存在于 data/xpkgs** 下（如 `data/xpkgs/a/1.0/`、`data/xpkgs/b/0.0.1/lib/`）；“安装”即下载/解压到 xpkgs 并做必要 patch，**不**在 subos 下复制一份。程序运行时通过 xvm 解析到的路径**直接指向 xpkgs**（或经 subos 的链接间接指向）。
- **subos 的职责**：**不**作为应用/库的存储位置，只承担：
  - **聚合**：subos 下是**统一的** `subos/lib/`（一个目录），不设 subos/lib/<pkg>/<ver>/；可选在此做扁平默认链接（如 subos/lib/libb.so.1 → xpkgs/b/<默认版本>/lib/libb.so.1）用于兼容或脚本。
  - **版本隔离**：通过 per-program 闭包决定每个程序看到哪些 (pkg, ver)；各程序的 LD_LIBRARY_PATH **直接**指向 data/xpkgs/<pkg>/<ver>/lib，实现 Nix 式无冲突。
  - **管理**：xvm workspace、当前激活的程序版本等，都挂在 subos 下。

这样 **data/xpkgs 是唯一真实数据源**（实际库和应用），**subos 只是版本视图与管理层**：轻量、可多 subos 共享同一 xpkgs、删除 subos 不影响 xpkgs。

**“闭包 + 统一 subos/lib/”是否合适**

- **可以，且与上述角色划分一致**。具体来说：
  - **闭包**：每个程序在注册时计算其依赖闭包，运行时只从闭包内的路径加载库；路径**直接写 xpkgs**（如 data/xpkgs/b/0.0.1/lib），不经过 subos/lib 下的 (pkg,ver) 子目录。
  - **统一 subos/lib/**：subos 下只保留一个 **lib/** 目录，用于兼容或默认视图（可选）；多版本解析不依赖 subos/lib 的目录结构，只依赖 per-program 指向 xpkgs 的路径。同一 xpkgs 可被多个 subos 引用，每个 subos 的“视图”由该 subos 内注册的程序及其闭包决定。
- **优点**：数据只存一份（xpkgs），subos 可增删可复制；闭包保证程序用对版本；统一 lib/ 符合“subos 下是版本视图”的约定，不引入 subos/lib/<pkg>/<ver>/。

**可优化点**

- **与 xvm 一致**：subos 内注册的程序列表与各程序的 LD_LIBRARY_PATH（闭包）由同一套依赖推导，避免两处逻辑不一致。
- **统一 subos/lib/ 的填充**：若保留“默认版本”的扁平链接到 subos/lib/，可按需在 config 或首次运行时建链，避免为未使用的包建链接。

---

#### 包索引分发的包 vs 基于 subos 视图构建的用户应用：依赖冲突可能性与应对

**包索引分发的包**：通过 per-program 闭包（LD_LIBRARY_PATH 直接指向 xpkgs 路径）解析依赖，每个程序只看到自己声明的 (pkg, ver)，**不会有依赖冲突**。

**基于 subos 视图构建的用户应用**：用户在自己的项目里编译、链接出的可执行文件（例如用 subos 里的 gcc、-L subos/lib 或 -L xpkgs/... 链接了 libb），在**运行时**若不经 xvm 启动，则不会自动带上某条 per-program 的 LD_LIBRARY_PATH，而是依赖**当前环境的 LD_LIBRARY_PATH**（如 shell 里 export 的、或 subos 的“默认视图” subos/lib）。因此**有冲突可能**：

- **构建与运行视图不一致**：用户构建时用到的 libb 来自 xpkgs/b/0.0.1，但运行时环境里 LD_LIBRARY_PATH 指向 subos/lib，而 subos/lib 里默认链的是 b@0.0.2 → 运行时加载到 0.0.2，可能 ABI 不兼容或行为异常。
- **同一 subos 内多个用户应用需要不同版本**：应用甲需要 libb@0.0.1、应用乙需要 libb@0.0.2；若两者都依赖“当前 subos 的 LD_LIBRARY_PATH”（即统一 subos/lib/），则 subos/lib 只能提供一种默认版本，另一应用会拿错版本。

**应对思路**

| 思路 | 说明 |
|------|------|
| **用户应用也纳入 per-program 闭包** | 用户将自建可执行文件“注册”到 xvm（例如通过 xpkg 或 xvm add 指定该二进制及其依赖列表），xvm 为其计算闭包并写入 LD_LIBRARY_PATH；运行时通过 xvm/shim 启动，与索引包同样享受按程序隔离，无冲突。需提供“声明依赖”的入口（如配置文件或命令行指定 deps）。 |
| **构建时把闭包写进 RPATH** | 用户构建时用 `-Wl,-rpath,data/xpkgs/b/0.0.1/lib`（及传递依赖）等，把所需版本路径写进二进制；运行时不再依赖 LD_LIBRARY_PATH，与 Nix 式一致。要求用户在构建阶段明确依赖版本并传入正确路径；适合可控的构建流程。 |
| **构建与运行共用同一“默认视图”** | 在 subos 内约定：构建时使用的 LIBRARY_PATH/LD_LIBRARY_PATH 与运行时的 subos/lib（默认视图）一致，例如都由“该 subos 的默认 (pkg, ver)”决定。用户在同一 subos 下构建并运行、且不混用多版本时，可避免冲突；一旦切换默认版本或跨 subos，仍可能错配。 |
| **文档与约定** | 在文档中说明：未通过 xvm 注册的用户应用看到的是 subos 的默认视图（统一 subos/lib/）；若依赖特定版本或存在多版本需求，建议 (a) 注册到 xvm 并声明依赖，或 (b) 构建时写死 RPATH 到对应 xpkgs 路径。 |

**Nix 与 Conda 如何解决用户构建应用**

- **Nix**
  - **通过 derivation 构建**：用户把应用写成 Nix 表达式（`stdenv.mkDerivation` 等），`nix-build` 在隔离环境中用声明好的依赖构建；产物进 store，**RPATH 由 Nix 自动设为依赖的 store 路径**（或由 ld-wrapper 等注入），运行时不再依赖 LD_LIBRARY_PATH，闭包由二进制自带，无冲突。
  - **在 nix-shell 里 ad-hoc 编译**：若用户只在 shell 里手写 `gcc -o myapp ...` 且不把 myapp 纳入 derivation，则产出的二进制**不在 store 里、也没有 Nix 注入的 RPATH**。运行时要么 (1) 仍在同一 `nix-shell` 下执行（依赖该 shell 的 LD_LIBRARY_PATH / 环境），要么 (2) 用 `patchelf --set-interpreter` / `--set-rpath` 把 loader 和库路径写进二进制，再配合 LD_LIBRARY_PATH 或 RPATH 使用。即：**要么“构建与运行同一环境”，要么手动 patchelf 把闭包固化进二进制**。
- **Conda**
  - **一个 env 一套版本**：每个 conda environment 内每个包只有一个版本；用户 `conda activate myenv` 后在该 env 下构建和运行，**构建与运行共用同一套 lib**（由 activate 脚本设置 PATH、LD_LIBRARY_PATH 等）。因此用户应用不会出现“构建用 A 版本、运行拿到 B 版本”的错配。
  - **多版本需求**：若应用甲需要 libb@0.0.1、应用乙需要 libb@0.0.2，做法是**建两个 env**（如 `myenv_a`、`myenv_b`），各自安装对应版本，在不同 env 下分别构建和运行。用环境隔离替代同一环境内的多版本并存。

**对比小结**：Nix 靠“ derivation 产出带 RPATH 的 store 产物”或“同一 shell 环境 / 手动 patchelf”；Conda 靠“一个 env 一套版本 + 多版本用多 env”。xlings 的“用户应用注册到 xvm 并声明依赖”更接近 Nix 的 per-program 闭包思想；若只提供“默认视图”则类似 Conda 的单 env 单版本，多版本需多 subos 或显式闭包。

**建议**：提供“用户应用注册到 xvm 并声明依赖”的能力（或等价机制），使基于 subos 构建的用户应用也能获得与索引包相同的 per-program 闭包，从根上避免依赖冲突；同时文档化默认视图的适用范围与上述替代方案。

---

## 五、为什么必须用 LD_LIBRARY_PATH，RPATH 能否替代

在「同一份二进制、多 subos 共享、各 subos 用各自 lib」的前提下：

- **RPATH/RUNPATH** 是写在 ELF 里的**固定**路径，一份二进制只能带一套；「当前 subos」仅在运行时可知，无法用一条 RPATH 同时适配多 subos。若为每个 subos 写不同 RPATH，则需 per-subos 副本，违背共享 xpkgs。
- **LD_LIBRARY_PATH** 在进程启动时由 shim 按当前 subos 设置，同一二进制在不同 subos 下拿到不同库路径，故**必须**用该运行时机制实现“按 subos 切换库路径”。

**RPATH 仍可用于**：包自身依赖（如 gcc 的 libstdc++），设 `RUNPATH=$ORIGIN/../lib64`；与 LD_LIBRARY_PATH 可同时使用。

---

## 六、LD_LIBRARY_PATH 的副作用与应对

**问题**：LD_LIBRARY_PATH 是进程级环境变量，会**被所有子进程继承**。设置后，主程序及其 fork/exec 的子进程（如 gcc 调用的 ld、as、系统工具）都会优先从当前 subos 的 lib 解析依赖，导致：

- “一设置就不能放心使用当前 subos 外的工具”：子进程若为系统或其他 subos 的二进制，可能错绑到当前 subos 的库，产生 ABI/符号问题。
- 整棵进程树被“绑”到当前 subos，无法仅对主程序生效。

**应对思路**：

| 做法 | 说明 | 代价 |
|------|------|------|
| **接受副作用** | 子进程多为同环境工具链且 glibc ABI 兼容时风险可控。 | 混用系统/其他 subos 子工具时有错绑风险。 |
| **尽量 RPATH，少用 LD_LIBRARY_PATH** | 包内依赖用 RPATH；LD_LIBRARY_PATH 只给确实需要 subos lib 的程序，并文档说明会污染子进程。 | 需 subos lib 的程序仍须设 LD_LIBRARY_PATH。 |
| **Per-subos 副本 + RPATH** | 每 subos 一份二进制，RPATH 指向该 subos 的 lib，不设 LD_LIBRARY_PATH；子进程不再继承 subos 路径。 | 磁盘与维护成本增加。 |
| **Wrapper 在 exec 前 unset** | 主程序或 wrapper 在 exec 子进程前清掉 LD_LIBRARY_PATH。 | 需改上游或维护大量 wrapper，通常不现实。 |

**建议**：采用“system loader + LD_LIBRARY_PATH 指向 subos/lib”时，在文档中明确：LD_LIBRARY_PATH 会波及整棵进程树；若需强隔离或混用多环境子工具，可考虑 per-subos 副本 + RPATH。

---

## 七、其他工具对比（简要）

- **Nix**：RPATH 指向 store，同一 derivation 路径一致；不做“同一二进制多套 lib 按上下文切换”。
- **Environment Modules / Lmod**：用 LD_LIBRARY_PATH（及 PATH）在 `module load` 时切换环境，与“按 subos 设 env”思路一致。
- **Spack**：RPATH + 可选 env；多环境时也依赖 env 或不同二进制。
- **Docker**：单 rootfs，无跨环境共享同一二进制。
- **Gentoo Prefix**：多通过 LD_LIBRARY_PATH 或 wrapper 设当前 prefix 的库路径。

共性：“同一二进制、多套库路径按运行上下文切换”依赖**运行时**机制（LD_LIBRARY_PATH 或 wrapper）；RPATH 用于路径固定或相对二进制可表达的依赖。

---

## 八、跨平台

- **Linux**：`patchelf --set-interpreter`（及可选 `--set-rpath`）。
- **macOS**：无 PT_INTERP；用 `install_name_tool` 修正 dylib/RPATH。
- **Windows**：DLL 通过 PATH 解析，一般无需改 PE。

策略统一为“安装时按当前环境修正 loader/库路径”，实现按平台分支。

---

## 九、建议方案（分阶段）

在以上问题与业界做法基础上，建议按阶段实施以下方案。

### 短期（当前可做）

1. **预构建 ELF 解释器与 elfpatch 接口**  
   - **libxpkg.elfpatch**：`patch_elf_loader_rpath(target, opts)` 支持 **target = 单 ELF 或目录**（目录自动扫描）；**rpath** 可**显式传入**（编写者通过 pkginfo 拿依赖 install_dir 后拼列表）或使用 `closure_lib_paths()`。  
   - **双模式**：`elfpatch.auto(enable)`，**默认 false**（手动）：由 xpkg 编写者在 install() 中调用上述接口；**auto(true)**：xim 在 install 末尾自动检测依赖并为当前包设置 loader + rpath。  
   - 依赖主机 patchelf；非 Linux no-op。任务拆解与验证见「elfpatch 接口设计与双模式」。

2. **多 subos 与库路径**  
   - 保持「install() 只 patch 到 system loader；config() 为每个程序设 LD_LIBRARY_PATH = 当前 subos 的 lib」；多 subos 共享 xpkgs、多版本（同一包多版本）逻辑不变。  
   - 在文档中明确：LD_LIBRARY_PATH 会波及整棵进程树；同一 subos 内**同一库仅能存在一个版本**（后安装覆盖），若需多版本依赖共存请使用多个 subos 或等待 per-program 依赖解析。

3. **包自身依赖**  
   - 对包内 lib（如 gcc 的 libstdc++.so）在 install 时用 patchelf 设置 `RUNPATH=$ORIGIN/../lib64`，减少对 LD_LIBRARY_PATH 的依赖，缩小污染面。

### 中期（支持同一 subos 内多版本依赖共存）

4. **per-program 的 LD_LIBRARY_PATH（npm 式）**  
   - 在 config() 生成 xvm 注册时，**不再**把当前包依赖聚合到 subos/lib，而是根据**该程序声明的 deps**（及可选传递闭包）解析出各依赖的 lib 路径，拼成一条 **仅针对该程序** 的 LD_LIBRARY_PATH 写入 xvm。  
   - 这样 A 依赖 b@0.0.1、C 依赖 b@0.0.2 可在同一 subos 内共存；实现上需：依赖解析（deps + 可选传递）、路径拼接、以及是否保留“部分聚合到 subos/lib”的兼容（如仅 glibc/系统库聚合，其余 per-program）。

5. **可选：Nix 式闭包 RPATH**  
   - 若希望进一步避免 LD_LIBRARY_PATH 污染子进程，可为每个程序在 install/config 时计算其依赖闭包，并把该程序二进制（及子二进制）的 RPATH 设为这些依赖的 lib 路径；不再为该项目设置 LD_LIBRARY_PATH。实现成本较高（需对包内所有 ELF 写 RPATH，且传递依赖要完整），可作为后续演进。

### 长期与文档

6. **文档与约定**  
   - 在 xlings 与 xim-pkgindex 文档中固定：预构建可重定位流程、多 subos 下 loader 与 LD_LIBRARY_PATH 的职责、同一 subos 内多版本依赖的现状与可选方向（per-program 路径 / Nix 式闭包）、LD_LIBRARY_PATH 副作用及适用场景。

---

## 十、小结

1. **预构建 ELF 解释器**：在 install() 中用 patchelf 统一改为系统 loader；推荐在 libxpkg 中提供通用函数，各 xpkg 按需调用。
2. **包索引与预构建**：职责解耦；可重定位由构建约定与索引侧 install 共同保证。
3. **多 subos**：install() 只 patch 到 system loader；config() 为每个程序设 LD_LIBRARY_PATH = 当前 subos 的 lib；多 subos × 多版本不增加 loader/rpath 逻辑复杂度。
4. **同一 subos 内多版本依赖**：当前会冲突（subos/lib 按文件名聚合、后装覆盖）；可借鉴 Nix per-package 闭包或 npm 式 per-program LD_LIBRARY_PATH，见第四节可参考方案与第九节建议方案。
5. **LD_LIBRARY_PATH**：实现“按 subos 用各自 lib”必须使用；会污染整棵进程树，需在文档中说明；包内依赖尽量用 RPATH 缩小污染面。
