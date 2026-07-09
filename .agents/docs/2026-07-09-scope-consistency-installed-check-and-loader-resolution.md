# additive 复用下 loader-provider 的跨 store 解析(elfpatch interpreter/rpath 的真实 xlings 设计缺口)

**日期**: 2026-07-09
**范围**: `xim` 安装流水线(`src/core/xim/installer.cppm`、`resolver.cppm`、`catalog.cppm`、`config.cppm`)+ elfpatch(`openxlings/libxpkg/.../xim/libxpkg/elfpatch.lua`)
**性质**: xlings 自身的架构评估 + 设计方案(非打补丁,非为下游 mcpp 服务)
**行号基准**: `openxlings/xlings` HEAD `685dc3e`、`openxlings/libxpkg`(当前 checkout)

## 文档族与职责边界(三份配套,全部根治设计、无 workaround)

| # | 仓库 / 文档 | 角色 | 与 OpenCV/cmake 故障的关系 |
|---|---|---|---|
| ① | mcpp-index `2026-07-09-mcpp-builddep-loader-store-split-rootcause.md` | 根因总报告 | 先读,了解全貌 |
| ② | mcpp `2026-07-09-project-index-scope-global-infra-fix.md` | 该故障的**唯一必需修复(根治)** | 修 ② 即闭环 |
| ③ | **本篇** openxlings/xlings `2026-07-09-scope-consistency-installed-check-and-loader-resolution.md` | xlings 侧**独立**健壮性缺口(根治) | **不由该故障触发、与 ② 互不依赖** |

**职责边界**:OpenCV/cmake 故障请看 ②(mcpp 侧,唯一必需)。**本篇 ③ 是一个独立的 xlings 缺口**——合法"项目消费者 + 全局 loader-provider(additive 复用)"下 elfpatch 的 provider 解析口径问题,即使 ② 修好也仍值得根治。**本篇只给根治设计(D1),明确不采用 workaround(D2 仅列作对照)。**

---

## 0. 先厘清:本文**不是**修 OpenCV/cmake 那个故障

那个故障(项目沙箱里 `cmake` interpreter 悬空)的**唯一正解在 mcpp**:mcpp 不该把官方全局索引 `xim` 强注册为**项目作用域** index repo(`mcpp/src/config.cppm:684-697`),导致 cmake/glibc/gcc/make 这些**全局工具**被错误项目化、装进项目 store。修好后它们回到 registry(全局),cmake 的 interpreter 天然指 registry 的 glibc,**xlings 一行都不用改**。详见 mcpp-index 仓 `.agents/docs/2026-07-09-mcpp-builddep-loader-store-split-rootcause.md`。

**本文剥离出其中真正属于 xlings 的、与 mcpp 无关的设计缺口**,即用户指出的合法场景:

> **一个合法的项目作用域消费者(如项目本地索引发布的预编译动态可执行体)runtime 依赖一个只在全局(registry)物化的 loader-provider(如 `xim:glibc`)。此时 elfpatch 应把该消费者的 interpreter 指向 provider 真实所在(全局)store 的 loader,而不是消费者自身作用域(项目)store。**

这在 xlings 的 **additive(project 叠加 global)** 模型下是**正常且应支持**的:消费者在 project、provider 在 global,两者本就可以分属不同 store。

---

## 1. additive 复用的既定语义(模型正确)

- catalog 同时扫 project+global 两套 repo(`catalog.cppm` `collect(projectRepos_); collect(globalRepos_)`),版本/工作区项目模式 merge global(`config.cppm:790-792` `versions()` 合并、`merge_workspace_into_` Anonymous)。
- 因此:**一个已在全局装好的 provider(glibc),项目作用域的消费者应"复用"它、而非在项目 store 重装。** 这正是 `installer.cppm:1437-1439` 的"已装"闸门读 merged 版本库的**正当用途**:provider 在任一可达 store 已装 → 跳过重复物化。
- openxlings 甚至已有对应机制的雏形:`.xim-installed` stamp(`installer.cppm:1580-1601`,`apply_install_stamp_if_empty`),注释明说"wrapper packages(linux-headers、fromsource:* 别名)**合法地把 install_dir 留空,因为真实 payload 在另一个单独安装的 dep 里**"。→ **xlings 已经承认"payload 在别的 store"这一 additive 事实。**

**结论:模型没问题。缺口在于——xlings 记了"payload 在别处",却没有让 elfpatch 知道"别处具体是哪个 store"。**

## 2. 缺口:loader-provider 的绝对路径解析走的是"注册/扫描",而非"实际物化 store"

elfpatch 需要把消费者的 PT_INTERP 设为 `<provider 的 install_dir>/<provider.exports.runtime.loader>`(loader 是相对路径,如 `lib64/ld-linux-x86-64.so.2`,`type.cppm:43`;由 resolver 填进 `node.exports.loader`,`resolver.cppm:182/348`)。关键在"**provider 的 install_dir 从哪来**":

- 安装器**没有**把 provider 的权威 install_dir 传给 elfpatch:`executor.apply_elfpatch_auto()` **无参**(`installer.cppm:1612`)。
- 于是 elfpatch 在 `elfpatch.lua` 内部**自行解析**:`closure_lib_paths`(`:580-631`)对每个 runtime dep 先取 `deps_exports[dep].libdirs`(声明式),否则**回退 `pkginfo.dep_install_dir(dep_name, dep_version)`**(`:620`,"convention via pkginfo.dep_install_dir lookup",`:606`)。
- 而 **`deps_exports` 目前恒空**:持久化声明式 exports 的 **#351 已被 revert**(`685dc3e` Revert "persist declared xpkg exports…",理由:其唯一消费者是 mcpp、分层不当)。→ 实际走 `pkginfo.dep_install_dir` 这条**约定解析**。
- `pkginfo.dep_install_dir` 解析到 provider 的**注册/扫描位置**(xvm SPath / store 扫描),**取决于 provider 节点的名义作用域**,而非"payload 实际物化在哪个 store"。

**故障不变式**:当 provider 因 additive 复用而"名义作用域/注册位置"与"实际物化 store"不一致时——例如 provider 名义 project、实际只在 global 物化(被 `:1437` 复用跳过);或反之——elfpatch 会把消费者 interpreter 指向一个**未物化的 store**,导致 PT_INTERP 悬空、消费者不可执行。

> 与 build-dep 的对照(佐证这是"解析口径"问题):build-dep 的 PATH 注入用 `locate_dep_install_dir_(plan, dep)`(`installer.cppm:1488`),它取 provider **节点在 plan 里的 `storeRoot`**(`:1062-1063`)。这是"名义作用域"口径——同样在"名义≠实际物化"时会偏。**两条解析路径(elfpatch 走 pkginfo 扫描、build-dep 走 plan.storeRoot)口径还不统一。**

## 3. 更深的架构问题

- **P1 —— 没有"provider 实际物化 store"的单一权威来源。** resolver 有 plan(每节点 scope/storeRoot),installer 有"是否复用/跳过"的事实,但这些**没有汇成一个"每依赖的 effective 物化 install_dir"传给 elfpatch**;elfpatch 只能事后用 `pkginfo.dep_install_dir` 扫描**推断**。
- **P2 —— "已装跳过"(`:1437`)与"loader 解析"(elfpatch)对同一 provider 的 store 认知可不一致。** 前者可因"在 global 已装"而跳过 project 物化;后者却可能解析到 project(名义)。二者必须共用同一"effective store"。
- **P3 —— #351 revert 留下的真空。** revert 掉的是"**持久化到磁盘、且唯一消费者是 mcpp**"的 exports 文件(分层理由正确);但 elfpatch 本就需要"provider 的 install_dir/libdirs/loader"这份信息——现在退回到脆弱的约定扫描。**正解不是复活持久化文件,而是由 xlings 自己在安装期把 plan already 算出的 provider install_dir 以内存方式喂给 elfpatch**(第一消费者=xlings 自身,契合 revert 留言"若需 installed-state 元数据,应以 xlings 自己为第一消费者")。
- **P4 —— 解析口径分裂。** elfpatch 走 `pkginfo.dep_install_dir`(xvm/scan)、build-dep PATH 走 `plan.storeRoot`。同一"依赖在哪"的问题两套答案,易漂移。

> 评估:additive 模型 OK;缺口是"provider 实际物化 store"缺一个**解析期确定、安装/elfpatch 共用**的权威来源,导致 elfpatch 用扫描去猜,在"名义≠实际"时猜错。

---

## 4. 设计方案(重写)

### 核心原则
**"某依赖的 payload 实际物化在哪个 store"必须是解析期确定的**权威值,由 installer 在调用 elfpatch 时**以内存方式**传入;elfpatch 与 build-dep 解析都消费同一个值,不再各自 xvm/scan 推断。**不引入持久化文件(不复活 #351),不往项目 store 复制 provider。**

### D1(推荐,根治 P1–P4)—— 让 installer 给 elfpatch 传"provider 的 effective install_dir"
1. **解析/安装期**:为每个节点确定 `effective_install_dir` —— 按 additive 语义取**payload 实际所在**的 store:
   - 若该 provider 在 project store 已物化(其作用域版本库 `projectVersions_` 记录且 payload 存在)→ project store;
   - 否则若在 global 已物化(`globalVersions_` 记录)→ **global store**(这正是"复用 global"的情形);
   - 否则 = 待安装目标(其自身作用域 store)。
   - **判据以作用域版本库记录为准**,不用"目录非空"(会被写进 install_dir 的 `.xpkg.lua` 骗到:`installer.cppm:65` + `has_directory_entries_`/`!is_empty` `:216`/`catalog.cppm:309`)。
2. **传给 elfpatch**:给 `apply_elfpatch_auto` 增加入参(或经 `_RUNTIME.deps_exports` 注入,内存态):每个 runtime dep → 其 `effective_install_dir`(+ 已知的 `exports.runtime.loader/libdirs`)。elfpatch 的 `closure_lib_paths`(`elfpatch.lua:610-620`)与 interpreter 解析**优先用注入值**,`pkginfo.dep_install_dir` 仅作最后兜底。
3. **interpreter** = provider 的 `effective_install_dir` + `exports.runtime.loader`;**rpath** 同理用各 dep 的 `effective_install_dir`。
   - 于是:glibc 只在 global 物化 → `effective_install_dir(glibc)=global` → 消费者 interpreter 指 **global** loader(存在)→ 可执行;项目特有的 provider → project store。**additive 语义被正确贯彻、无冗余复制。**
4. **统一 build-dep 口径**:`locate_dep_install_dir_`(`:1062-1063`)也改用 `effective_install_dir`(而非仅 `node.storeRoot`),消灭 P4 的两套答案。

好处:一个事实源(`effective_install_dir`,解析期算、内存传递),同时修 P1(权威来源)、P2(跳过与解析一致)、P3(不复活持久化文件、xlings 自用)、P4(口径统一)。

### D2(❌ workaround —— 仅列作对照,不采用)
> 记录于此仅为说明"为何不走治标路线",**本设计不采用**。
- 只改 `elfpatch.lua` 的 `pkginfo.dep_install_dir` 兜底:消费者自身 store 下 provider 未物化(以版本库/`.xim-installed` 判定)时回退 global。
- 为何不采用:仍是 elfpatch 事后推断(P1/P4 未除),与 `:1437` 跳过决策仍可能不同步;是打补丁而非建立单一事实源。

### 采用:D1(唯一根治设计)
把"provider 实际物化 store"提升为**安装期权威、内存传递**的单一事实源(`effective_install_dir`),一次对齐 additive 模型、同时消除 P1–P4。**不采用 D2 的治标路线。**

### ❌ 反模式(不要做)
- **不要把全局 provider 物化进项目 store**(即"让项目 store 有货")——那是坐实"全局工具被项目化"的错误、且冗余复制;OpenCV 那个故障的正解是 mcpp 别项目化全局工具,不是在 xlings 这边把 glibc 往项目里塞。
- **不要复活 #351 的 `.xpkg-exports.json` 持久化文件** —— D1 用内存传递、xlings 自用,规避 revert 的分层问题。

---

## 5. 最小复现(纯 xlings,不经 mcpp)
1. 全局装 provider:`xlings install glibc@2.39`(进 global store + `globalVersions_`)。
2. 一个**项目本地索引**发布消费者 `foo`:预编译**动态可执行体**,声明 runtime loader dep `xim:glibc@2.39`,需 elfpatch。
3. 在含该项目 `.xlings.json`(声明该本地 index_repo)的目录 `xlings install foo` →
   - `foo` 项目作用域、装进项目 store;glibc 被 `:1437` 判"已装(global)"→ 不在项目 store 物化(**正确的复用**);
   - elfpatch 解析 glibc install_dir 时(`pkginfo.dep_install_dir`/扫描)命中"消费者自身/名义作用域"而非 global 实际 store → `foo` 的 PT_INTERP 指向未物化目录 → **`foo` 不可执行**。
> 这是"合法 additive 复用"下的真实失败,与 mcpp 无关。`prefer_project_scope_`(`catalog.cppm:380`,去重同包两作用域)的存在也印证 xlings 预期"同包可跨作用域",却未让 loader 解析随之作用域感知。

## 6. 行动项(唯一根治路线 D1)
- [ ] installer 在安装期算每节点 `effective_install_dir`(以作用域版本库为准,不用"目录非空"),经参数/内存 `_RUNTIME` 传入 `apply_elfpatch_auto`;`elfpatch.lua` interpreter/rpath 优先消费之,`pkginfo.dep_install_dir` 降为兜底。
- [ ] 统一 `locate_dep_install_dir_`(build-dep PATH)与 elfpatch 的 provider 解析口径为 `effective_install_dir`。
- [ ] 补 §5 的纯 xlings e2e(global-provider + project-consumer 动态可执行体);现有 `elfpatch_install_verify_test.sh` 仅单 store。
- [ ] (相互独立,非本篇)OpenCV/cmake 故障由**文档 ②(mcpp 根治)**解决;本篇与之互不依赖。
