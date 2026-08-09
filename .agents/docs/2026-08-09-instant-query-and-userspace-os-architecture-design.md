# xlings 即时查询与用户态 OS 契约优化设计

> 状态：Proposed，待 review
>
> 日期：2026-08-09
>
> 范围：`openxlings/xlings`、`mcpplibs/libxpkg`、`openxlings/xim-pkgindex`，以及作为契约消费者的 `mcpp-community/mcpp`
>
> 核心问题：默认只读命令出现秒级乃至无界等待；安装事实、依赖解析、运行时边界缺少统一权威模型
>
> 非目标：为 mcpp 增加专用兼容分支，或把 xlings 扩张成内核、驱动、桌面环境管理器
>
> Review 分流（2026-08-09）：长期架构讨论已转入 [xlings #518](https://github.com/openxlings/xlings/issues/518)；当前可交付修复以 [`2026-08-09-stability-regression-recovery-design.md`](./2026-08-09-stability-regression-recovery-design.md) 为准。本文件保留为调查与架构背景，其中 SQLite 推荐已被本轮决定否决，现阶段继续使用版本化 JSON。

## 1. 结论先行

### 1.1 `list / info / self doctor` 是明确的回归，不是可接受的“大目录正常变慢”

本机同一份真实、长期演进的 `XLINGS_HOME` 上，旧版和新版二进制对照结果如下：

| 命令 | 2026.8.2.1 | 2026.8.5.1 | 2026.8.9.2 | 判断 |
|---|---:|---:|---:|---|
| `xlings list` | 0.21 s | 7.37 s | 7.43 s | 2026.8.3.1 引入的 CPU 型回归 |
| `xlings info gcc` | 0.08 s | 17.26 s | 17.31 s | 2026.8.3.1 引入的 CPU 型回归 |
| `xlings self doctor` | 0.84 s | 0.86 s | 30 s 内无输出 | 2026.8.5.3 引入的无界默认深扫 |

`2026.8.6.2 self doctor` 在 12 秒预算内同样超时，说明 doctor 回归并非 2026.8.9.2 才出现。
旧版 doctor 会因发现健康问题而返回非零；表中比较的是“是否在有界时间内完成并给出结果”，不是把 finding 数量当作成功率。

这份 home 当前约有：

- 124 个包目录；
- 323 个版本目录；
- 38 个 SubOS；
- 434,210 个 payload 普通文件；
- 约 141 GiB 数据；
- 主状态文件约 590 KiB，部分 workspace 状态约 86 KiB。

这个规模放大了问题，但不是根因。包管理器必须把“用户长期使用后积累很多版本、SubOS 和文件”视为正常状态，而不是异常输入。查询耗时不应与 payload 文件总数相关，`info 单包` 更不应与全部包、全部 SubOS 相关。

严重级别建议：

- `list/info`：P0 性能与用户体验回归；
- 默认 `self doctor`：P0 可用性回归；
- #513 空错误：P0 可诊断性缺陷；
- #514、#506：P1 数据模型和生命周期一致性缺陷；
- #392：P1 用户态 OS 运行时契约缺陷，图形应用会把影响扩大到 GPU/窗口系统/宿主服务边界。

### 1.2 不是锁死、binding 递归或传统死循环

当前证据不支持以下假设：

- `.xlings.lock` 被其他进程长期持有：文件存在但无 `lsof/lslocks` 持有者；
- XVM binding 图出现纯逻辑死循环：`list/info` 的 CPU 时间几乎等于墙钟时间；
- 网络下载卡住：这些命令的慢路径发生在本地目录、Lua 元数据和子进程探测；
- 单纯输出渲染慢：主要时间发生在构造结果之前。

真正原因是“有限但没有合理上界的同步重算”：

1. `list` 每次重新组装 inventory，并为已安装记录解析 catalog 元数据；
2. `info gcc` 名义上只查询一个包，实际先构造所有 SubOS、所有 payload 的完整 inventory，再过滤到 gcc；
3. 默认 `doctor` 遍历所有包、所有版本、所有文件，并对 ELF 调用外部 `patchelf`；
4. doctor 内部还通过启动新的 `xlings info ...` 进程探测坐标，叠加了已经回归的 info 路径。

所以用户看到的“卡住”是设计上无界的查询路径，而不是必须等到真正无限循环才算 bug。

### 1.3 四个 issue 指向三个共同的架构缺口

| 共同缺口 | 直接表现 | 对应问题 |
|---|---|---|
| 查询面与审计面未分离 | `list/info` 全量重建；`doctor` 默认扫 43 万文件并大量 fork | 当前卡顿 |
| 包实例、解析结果、安装效果没有统一账本 | 共享缓存依赖找不到；零 XVM target 的包无法移除；移除从 target 反推包 | #514、#506 |
| 执行域和宿主桥接不是一等契约 | 私有 glibc、宿主 libtinfo/Mesa、`LD_LIBRARY_PATH` 和宿主 shell 混用 | mcpp #392 |

#513 则说明错误和事件协议仍停留在“函数返回 bool + 旁路日志”的阶段：一旦旁路输出被 UI/NDJSON 隔离，失败就退化成空的 `E_INTERNAL`。

### 1.4 推荐路线不是大爆炸重写

推荐“契约先行的绞杀式迁移”：

1. 立即热修当前回归，让只读命令恢复即时响应；
2. 引入可事务化的安装账本和每 payload receipt，但先以 shadow 模式写入；
3. 把 XVM、workspace、shim、sysroot 视为账本的物化视图；
4. 要求安装 hook 使用 resolver 传入的精确 `ResolutionContext`，不再在安装中猜目录；
5. 引入跨消费者稳定的 `RuntimeDescriptor` 和 typed host bridge；
6. 经过双读比对和真实旧 home 迁移验证后，再切换权威读路径。

这样能同时避免两种风险：

- 只打局部补丁，几个月后换一个命令再次全量扫描；
- 为了“正确架构”一次重写状态系统，让已有 home 承担高风险无感迁移。

### 1.5 多视角评估

| 视角 | 当前评价 | 主要依据 | 目标门槛 |
|---|---|---|---|
| 架构 | 高风险 | package、target、payload、workspace、cache 各自保存部分真值，查询时再拼装 | package instance / resolution / effect / runtime 四个权威契约 |
| 稳定性 | 高风险 | 默认 doctor 无界深扫；跨 DB/文件系统缺少统一 transaction/recovery | committed generation、journal、幂等恢复、quick/deep 分离 |
| 兼容性 | 中高风险 | 老布局可继续工作，但规模放大回归；迁移与审计混在默认路径 | bounded migration、双读比对、N-2 read-only/rollback 验证 |
| 优雅简洁 | 中高风险 | 多处通过目录扫描和 CLI 子进程重复回答同一问题 | resolver 一次决策、内部 API、物化视图、typed bridge |
| 用户体验 | 严重 | 本地只读命令 7–17 秒，doctor 30 秒无首屏；失败可为空错误 | 100 ms 首屏、明确进度/取消、非空结构化错误 |
| 性能 | 严重 | info 对单包做全 inventory；doctor 对文件数和进程数线性放大 | query 与 payload 文件数解耦，算法 counter 进入 CI |
| 多平台 | 中高风险 | CLI 有平台抽象，但 runtime/graphics 仍以 Linux glibc 路径经验为主；native 数值未验证 | tagged runtime backend + Linux/macOS/Windows native gate |
| 可诊断性 | 严重 | #513 空 `E_INTERNAL`；“卡住”没有阶段、计数和范围 | 统一事件流、correlation id、trace counter、完整日志定位 |

## 2. 调查范围与证据边界

### 2.1 当前分析快照

| 仓库/组件 | 分析快照 |
|---|---|
| xlings 工作树 | `2913a09`，已发布行为以 `2026.8.9.2` 二进制复现 |
| 回归前基线 | `2026.8.2.1` |
| list/info 回归提交 | `0c362cf`，`feat(core): multiplatform UX contracts`，发布于 `2026.8.3.1` |
| doctor 回归提交 | `c512679`，`feat(xim): one resolution, referenced everywhere`，发布于 `2026.8.5.3` |
| libxpkg | tag `0.0.54` |
| mcpp | `80291ca` / main |
| xim-pkgindex | `origin/main=e8029381` |

以上性能与系统调用证据来自 Linux/WSL2 本机。macOS、原生 Windows 和其他架构上的具体数值尚未实测，本文不会把 Linux 数值冒充三平台结论。由于 Windows 进程创建和杀毒实时扫描、macOS dyld/签名校验都有不同成本，当前实现有充分理由在这些平台更差，但必须由 native runner 给出最终证据。

### 2.2 doctor 的系统调用证据

对当前 `self doctor` 做 12 秒统计：

| 指标 | 观测值 |
|---|---:|
| `execve` | 579 |
| clone | 289 |
| wait | 288 |
| open | 约 60,900 |
| read | 约 62,522 |
| 系统调用总数 | 约 433,769 |

6 秒的进程跟踪中可以看到：

- 191 次 `/bin/sh`；
- 189 次 payload 中的 `patchelf`；
- doctor 内递归启动 `xlings info local:codex@...`；
- 对大量 ELF 逐个执行 `--print-interpreter`，动态 ELF 再执行 `--print-rpath`；
- 扫描范围包含 GCC 的对象文件等并不属于默认健康检查关键路径的文件。

这不是“doctor 做得比较认真”，而是把离线全仓审计器放进了用户期望即时返回的默认诊断命令。

## 3. 当前代码中的直接根因

### 3.1 `info` 的注释和实现互相矛盾

[`collect_package_inventory`](../../src/core/xim/inventory.cppm) 的注释写的是：

> One package's rows, without materialising the rest of the inventory.

实现却是：

1. 调用 `collect_inventory(catalog, allSubos=true)`；
2. 读取全部 SubOS snapshot；
3. 组装全部 workspace 和 payload 记录；
4. 为 inventory 记录加载 catalog/package 元数据；
5. 最后 `erase_if` 删除非目标包。

[`cmd_info`](../../src/core/xim/commands.cppm) 的注释也明确说“构造完整 inventory 会让 info 与 index 大小成正比”，但随后仍调用上述完整 inventory 实现。这是可定位的实现 bug，不是抽象的性能建议。

### 3.2 `list` 在每次查询时重建“安装事实 + 展示元数据”

`cmd_list` 每次调用 `collect_inventory`。当前 inventory 同时承担：

- 当前 SubOS opt-in 状态；
- 全局 payload 是否存在；
- active 版本；
- catalog 描述和 programs 等展示信息；
- 老布局兼容；
- payload-only 包发现。

这些事实来自不同生命周期，却在查询时才合并。只要任何一个来源需要目录扫描或 Lua recipe 解析，整个查询就被拖入昂贵路径。

问题不在某个循环能否再优化 20%，而在“安装时已经知道的事实为什么要在每次 list 时重新推导”。

### 3.3 `doctor` 把安装时验证器复用成默认全仓验证器

[`doctor.cppm`](../../src/core/xself/doctor.cppm) 对 `data/xpkgs/<pkg>/<version>` 做全量遍历，并对每个版本调用 [`elf_same_source.cppm`](../../src/core/elf_same_source.cppm) 的 `scan_payload`。

`scan_payload`：

1. 递归遍历 payload 的每个普通文件；
2. 打开每个文件读取 ELF magic；
3. 对 ELF 启动 shell + `patchelf --print-interpreter`；
4. 对动态 ELF再启动 shell + `patchelf --print-rpath`；
5. 解析结果判断 loader/libc 是否同源。

同一实现用于“刚安装 payload 的校验”是合理的，因为范围是本次变更且用户已经在执行安装；用于默认 doctor 的全历史 store 则不合理。

### 3.4 doctor 又通过子进程调用慢路径

`cmd_doctor` 的 coordinate probe 用外部 `xlings info` 判断候选是否可解析。虽然有进程内 map 缓存，但：

- 每个唯一坐标仍至少启动一次完整 xlings；
- 该 info 本身会构造完整 inventory；
- 日志注释仍假设 `xlings info` 约 50 ms，而真实当前 home 已是 17 秒。

这是隐式递归放大，不是逻辑无限递归：doctor → info → 全量 inventory。

## 4. 四个 issue 的综合判断

### 4.1 xlings #514：共享缓存不是根因，“解析结果没有跨执行宿主闭合”才是

问题：

> `pkginfo.install_dir()` 对只存在于 mcpp shared registry cache 的依赖返回 nil。

直接原因是 libxpkg 的 fallback 扫描只覆盖：

- `_RUNTIME.xpkg_dir`；
- 当前安装目录推导出的 xpkgs root；
- project data xpkgs；
- 最后尝试 XVM。

它不知道 `MCPP_HOME/registry/data/xpkgs`，因此 sibling workspace member 已缓存的依赖不在扫描域中。

但是“给 fallback 加第四个 mcpp 路径”不是正确修复：

1. xlings/libxpkg 不应知道每个上层消费者的私有目录布局；
2. 共享缓存里“存在一个目录”不等于当前事务解析并拥有该依赖；
3. 多版本、namespace、平台、架构、损坏/半安装状态会让扫描产生第二个 resolver；
4. mcpp 一旦换缓存布局，问题重现；
5. 任意宿主都可以再发明第五个 store。

libxpkg 0.0.54 已有正确方向：

- `ResolvedDep` 记录 canonical name、精确版本、`install_dir`、`libdirs` 和来源；
- `ExecutionContext.resolved_deps` 设计为所有 runtime dep 的 total map；
- `pkginfo.dep_install_dir` 优先读取它，并明确把后续扫描称作“对一个已有唯一答案的问题给出第二个答案”。

所以 #514 的系统性结论是：

> 安装 hook 的宿主必须把自己 resolver 的精确结果完整注入 `ResolutionContext`；如果宿主无法提供，安装阶段应明确失败或进入显式 legacy 模式，而不是安静地扫描更多目录。

mcpp 可以继续拥有自己的 shared registry；xlings 的通用责任是定义稳定、与具体 store 布局无关的上下文协议和 conformance test，而不是为 mcpp 写专用路径。

### 4.2 xlings #513：错误不是日志文本，而是协议数据

问题：

> install hook 的 stdout、`log.error`、stderr 被吞掉，`install()` 返回 false 时只得到空 `E_INTERNAL`。

当前链路中的信息丢失点：

1. Lua hook 只返回 bool 时没有错误 message；
2. `HookResult` 虽有 `output/error`，但正常 false 分支不会自动填入 error；
3. stdout/stderr/log 走不同旁路，未归入 hook result；
4. installer 把空 `hookResult.error` 继续传给状态事件；
5. terminal UI、CI 和 `xlings interface` 对旁路输出的处理不同；
6. NDJSON 不能允许 recipe 原始 stdout 直接混入协议 stdout。

因此修复不能只是“不要吞 stdout”。正确模型应是结构化事件和结构化失败：

~~~text
HookResult {
  success,
  error: {
    code,
    message,          # failure 时必须非空
    phase,
    package_instance,
    correlation_id
  },
  stdout_tail,
  stderr_tail,
  events[],
  full_log_path
}
~~~

约束：

- `success=false` 且 recipe 未给 message 时，由执行器生成 `E_HOOK_RETURNED_FALSE` 和明确默认文本；
- terminal 可以实时显示事件；
- CI 至少显示有界 tail，并给完整日志路径；
- NDJSON stdout 只输出协议帧，recipe stdout/stderr 转为 `LogEvent`；
- 单条/总输出有大小上限，避免恶意或失控 hook 填满内存；
- 每个阶段支持 timeout、取消和 correlation id；
- 不允许空 `E_INTERNAL` 到达用户。

### 4.3 xlings #506：包实例不是 XVM target，target 也不是安装锚点

问题：

> Windows 的 gcc recipe 委托 mingw-w64 安装，gcc 自己没有注册 XVM target，移除时报告 exact removal version is not registered。

当前补丁方向把 `VersionNotFound + target 完全没有版本` 视为“零注册包”，但 issue 评论指出一个更精确的反例：

- target `gcc` 可以存在版本；
- 这些版本全部由另一个 provider（mingw-w64）拥有；
- 正在移除的 provider（gcc recipe）自己拥有零版本；
- “target 是否有版本”因此回答错了问题。

根本问题是从 XVM target 反推包实例：

- package/config/script/delegator 可以合法地注册零 target；
- 一个 package 可以注册多个 target；
- 多个 provider 可以在不同版本上贡献同名 target；
- package 还可能只贡献 env、headers、files、sysroot、dependency ref 或 uninstall hook；
- 安装成功的判据与移除可定位的判据不对称。

正确结论：

> 每个成功安装的 package release 都必须有独立 `PackageInstanceId` 和 `EffectManifest`；XVM target 只是其中一种效果，不是 package 的身份证。

不建议为零 target 包伪造 anchor target。伪 anchor 会污染 `list/use/remove` 语义，且仍无法描述委托边、环境、文件和引用计数。

### 4.4 mcpp #392：这是 xlings 用户态 OS 边界的通用问题

问题现场：

- WSL2 宿主 glibc 2.43；
- SubOS/private runtime glibc 2.39；
- clang cfg 把 private loader、RPATH 和 include 固化进产物；
- 系统 `libtinfo` 需要 `GLIBC_2.42`，系统 Mesa 需要 `GLIBC_2.43`；
- private 2.39 loader 无法加载这些宿主库；
- 切换 private 2.44 后主程序可启动；
- 但 `LD_LIBRARY_PATH` 继续泄漏给宿主 `/bin/bash`，导致 `GLIBC_PRIVATE __pointer_chk_guard` 错误；
- 扫描多个 glibc payload 的 fixup 还可能选中旧版本或改名后的历史目录。

这不应被归类为“mcpp 桌面程序比较特殊”。任何由 xlings 构建和运行、同时需要以下能力的程序都会遇到同类问题：

- 图形栈：Mesa、Vulkan ICD、OpenGL、Wayland/X11；
- 终端/输入：ncurses、libtinfo；
- 音频、打印、通知、浏览器打开；
- GPU vendor driver；
- 宿主 shell 或桌面服务；
- 插件和 `dlopen`。

作为通用用户态 OS，xlings 的问题不是“没有自动升级到足够新的 glibc”，而是：

1. 没有把执行域作为显式选择；
2. 没有向消费者提供权威 runtime descriptor；
3. 没有约束“一进程一套 loader/libc 核心”；
4. 没有把宿主能力通过 typed bridge 暴露；
5. 允许 `LD_LIBRARY_PATH` 这种进程树全局变量替代逐进程 launch policy；
6. 消费者只好扫描目录、读编译器遗留 cfg 或猜“最新版本”。

mcpp 当前 main 已开始做正确的局部收敛：

- 优先读 SubOS runtime binding；
- 读取 compiler baked runtime 只作为老 SubOS 兼容；
- 多个 glibc 都在且无权威记录时拒绝随意选择；
- child process 环境会清理 inherited private glibc path。

但 `find_sandbox_glibc_lib` 仍按目录迭代返回第一个带 loader 的 payload，注释称“newest”却没有版本排序。这再次说明：消费者不应负责选 runtime；xlings 应交付已选择、可验证的 runtime descriptor。

结论是：

> #392 暴露了 xlings 的通用架构责任，但修复方式不是“xlings 特判 mcpp”，而是 xlings 定义并兑现执行域、runtime closure 和 host bridge 契约；mcpp 只是第一个完整消费该契约的桌面应用构建器。

## 5. 设计原则与硬不变量

### 5.1 查询不变量

- Q1. 默认只读查询不访问网络。
- Q2. 默认只读查询不递归扫描 payload。
- Q3. `info/list` 不启动子进程，不执行 recipe hook。
- Q4. 单包查询复杂度为 O(1) 或 O(log N)，不得先构造全局 inventory。
- Q5. payload 文件数量不能影响 `info/list` 延迟。
- Q6. 超过 100 ms 的阶段必须可观测；超过交互预算的工作必须显式化。

### 5.2 状态不变量

- S1. 成功安装的每个 package release 都有 `PackageInstanceId`。
- S2. package instance 即使注册零 XVM target 也可查询、引用和移除。
- S3. XVM/workspace/shim/sysroot 是物化视图，不是安装事实权威。
- S4. 所有写入通过有日志、可恢复、幂等的 transaction。
- S5. 查询读取稳定 generation snapshot，不等待长写锁。
- S6. 重型迁移不得隐藏在第一个查询中。

### 5.3 解析不变量

- R1. 同一事务中，一个 dependency spec 只有一个精确解析结果。
- R2. hook 只消费 resolver 结果，不重新扫描/比较版本。
- R3. “目录存在”不等于“事务解析并拥有”。
- R4. shared/external store 通过 store id 和 lease/ref 描述，不通过宿主路径特判。
- R5. legacy 扫描只能是显式兼容模式，并输出来源和降级告警。

### 5.4 生命周期不变量

- L1. 安装提交什么效果，移除就反向处理什么效果。
- L2. recipe 声称成功不等于 transaction 成功；效果落盘、验证、记账均需成功。
- L3. 移除从 package instance 开始，不从 target 猜 package。
- L4. 委托是一条有所有者的 dependency/effect edge，不是隐式副作用。
- L5. crash 后只能得到 committed 或 recoverable，不得得到“payload 有了但账本不知道”且无诊断。

### 5.5 运行时不变量

- U1. 一个进程的 loader、libc 核心和基础 ABI 必须来自同一 runtime profile。
- U2. private glibc 路径不得无差别继承给宿主进程。
- U3. host capability 必须显式声明并通过 bridge/portal/broker 进入。
- U4. 消费者不得从目录顺序推导 runtime 版本。
- U5. Linux、macOS、Windows 使用同一上层语义、不同 native backend，不能强套 Linux glibc 模型。

## 6. 目标架构

~~~text
               xim-pkgindex / custom indexes
                         |
                compiled CatalogSnapshot
                         |
                  Resolver + Planner
                         |
                ResolutionContext (exact)
                         |
          +--------------+----------------+
          |                               |
   Hook Executor                    Runtime Resolver
   structured events               RuntimeDescriptor
          |                               |
          +--------------+----------------+
                         |
               Install Transaction Journal
                         |
             immutable/staged payload store
                         |
                State Store + Receipts
        (instances, refs, effects, resolutions)
                         |
       +-----------------+------------------+
       |                 |                  |
   Query API        materialized XVM    host bridges
  list/info/status  shim/workspace view  GPU/portal/shell
       |
  CLI / TUI / NDJSON / agents / mcpp and other consumers

Deep audit reads receipts/certificates and dirty payloads explicitly.
Routine queries never walk payloads.
~~~

### 6.1 四层职责

1. **声明层（xim-pkgindex）**
   - package identity、版本、平台/架构；
   - runtime/build deps；
   - exports/capabilities；
   - 声明式 effects 和 delegation；
   - runtime/host capability 需求。

2. **决策层（xlings resolver/planner）**
   - namespace、版本约束、平台/架构选择；
   - 精确 package instance；
   - store、runtime profile、host bridge；
   - 安装/移除 transaction plan。

3. **执行层（libxpkg + platform backends）**
   - 只执行 plan；
   - hook 读取 ResolutionContext；
   - 所有日志/错误作为事件；
   - 不重新做包解析，不猜“最新目录”。

4. **状态与展示层（State Store + projections）**
   - package instances、refs、effects、transaction；
   - XVM/workspace/shim/sysroot 为投影；
   - list/info 直接查询快照；
   - doctor 分 quick metadata check 与 explicit deep audit。

## 7. 控制面状态模型

### 7.1 PackageInstanceId

建议最小标识：

~~~text
PackageInstanceId {
  canonical_name,       # namespace:name
  version,
  platform,
  arch,
  store_id,
  content_digest
}
~~~

`content_digest` 区分“同名同版本但 payload 内容不同”的供应链异常；正常 resolver 仍以 canonical name/version/platform/arch 选择，提交时绑定 digest。

路径不应进入身份。绝对路径可以移动、home 可以迁移、mcpp 可以使用外部 store。路径由 `store_id + relative_payload_path` 解析。

### 7.2 InstallRecord

~~~text
InstallRecord {
  schema,
  instance_id,
  state,                  # staged / committed / quarantined / removing
  origin_index,
  recipe_digest,
  installed_at,
  payload_relative_path,
  payload_digest,
  resolution_id,
  runtime_profile_id,
  effect_manifest_id,
  verification_certificate,
  transaction_id
}
~~~

### 7.3 ResolutionContext

~~~text
ResolutionContext {
  id,
  subject_instance,
  runtime_deps: {
    original_spec -> {
      instance_id,
      store_id,
      install_uri,
      libdirs,
      exports,
      source,
      lease_id
    }
  },
  build_deps: {...},
  runtime_descriptor_id
}
~~~

规则：

- install/config/uninstall hook 内要求此上下文；
- `pkginfo.dep_install_dir` 只查 exact record；
- 外部 store 用 `install_uri`/store adapter 解析；
- 在 install hook 中记录缺失时返回 `E_RESOLUTION_CONTEXT_INCOMPLETE`；
- 仅工具脚本、离线查询等无安装事务场景允许 `--legacy-scan`；
- fallback 必须返回 `source=legacy-scan` 并可被测试检测，不能静默。

### 7.4 EffectManifest

~~~text
EffectManifest {
  package_instance,
  xvm_targets[],
  programs[],
  libraries[],
  headers[],
  files[],
  env_mutations[],
  subos_refs[],
  dependency_refs[],
  delegated_instances[],
  host_capability_bindings[],
  uninstall_hook
}
~~~

这里记录“实际提交的效果”，而不仅是 recipe 声明。声明用于 plan，manifest 用于精确回滚。

#506 的 Windows gcc 应表现为：

- gcc package instance 存在；
- 自有 `xvm_targets=[]`；
- `delegated_instances=[mingw-w64@13.0.0]` 或拥有一条 dependency ref；
- uninstall hook 可运行；
- 移除 gcc 时释放自己拥有的 edge；
- mingw payload 是否删除由全局 refcount 决定；
- 其他 provider 的 gcc target 不会被误删。

### 7.5 存储实现建议

推荐：

- 嵌入式 SQLite 作为 control-plane store；
- 每个 payload 旁保存可读的 immutable `install-receipt.json`；
- transaction journal 记录跨 DB/文件系统阶段；
- payload 仍保持普通目录和内容寻址/版本布局，不塞进数据库；
- XVM JSON、workspace 文件在迁移期继续生成，作为兼容投影。

选择 SQLite 的原因：

- package/subos/ref/effect/dependency 本身是关系数据；
- 需要事务、索引、schema migration、并发 reader；
- 自己在多个 JSON 文件上重新实现 WAL、锁、恢复，复杂度和 bug 面更大；
- 可静态链接，符合单二进制交付；
- SQL 只封装在 `state::Store`，业务层不散落 SQL。

receipt 的作用不是形成第二个日常权威，而是：

- payload 可自描述；
- DB 损坏时可以显式重建；
- 外部 store 可以携带安装证明；
- 迁移和审计有稳定输入。

需要单独 ADR 确认 SQLite 的静态体积、三平台构建和 license；如果否决 SQLite，替代实现也必须满足同样的 transaction/snapshot/index 契约，不能退回“多个 JSON + 查询时扫描”。

### 7.6 transaction 状态机

~~~text
planned
  -> payload_staged
  -> hook_executed
  -> verified
  -> payload_committed
  -> state_committed
  -> projections_materialized
  -> complete
~~~

每一步幂等，并记录恢复动作：

- `payload_staged` 之前失败：删除 staging；
- payload 已 rename、DB 未提交：按 journal 补提交或 quarantine；
- DB 已提交、projection 未完成：从 manifest 重放 projection；
- remove 中断：根据 tombstone 继续，不重新猜效果；
- 下次启动只检查少量未完成 transaction，不做全 store 扫描。

## 8. 即时查询面

### 8.1 `xlings info <pkg>`

新路径：

1. 从 CatalogSnapshot 以 canonical key 查询声明元数据；
2. 从 State Store 索引查询该 package 的 instances；
3. 查询当前 SubOS ref/active projection；
4. 格式化输出。

禁止：

- 构造其他 package 的 inventory；
- 加载/执行所有 Lua recipe；
- 遍历其他 SubOS 文件；
- 遍历 payload；
- 网络刷新；
- 启动 `xlings` 子进程。

P0 不等数据库落地，先把 `collect_package_inventory` 改成真正的 targeted reader：

- 只读目标 package 的 store dir；
- 只在 workspace map 中按 key 查找；
- SubOS snapshot 只提取该 key；
- metadata 只加载目标 recipe；
- 复杂度与其他 package/payload 文件数无关。

### 8.2 `xlings list`

长期路径从 State Store 查询：

- 当前 SubOS：join `subos_refs + package_instances + catalog_summary`；
- `--all`：查询全局 package instances；
- active 状态来自 projection generation；
- description/program summary 来自 compiled CatalogSnapshot。

list 不应通过“payload 非空”重新发现安装。payload consistency 属于 deep audit。

### 8.3 CatalogSnapshot

xim-pkgindex 在 publish/build-index 阶段产出 normalized metadata：

- canonical identity；
- versions/platforms/archs；
- description/categories/program promises；
- runtime/build deps；
- exports/runtime capabilities；
- schema digest。

xlings 更新 index 时验证并导入 snapshot。日常 query 不执行 recipe。local/custom index 可以在显式 `index update/build` 时编译；snapshot 过期时显示 generation/stale 标记，而不是查询时悄悄重建。

### 8.4 性能预算

参考规模：1,000 package instances、100 SubOS、1,000,000 payload 文件。

| 命令 | warm p95 | cold p95 | 首次可见输出 | 子进程 | payload walk |
|---|---:|---:|---:|---:|---:|
| `version/help` | ≤ 30 ms | ≤ 100 ms | ≤ 30 ms | 0 | 0 |
| `info <pkg>` | ≤ 100 ms | ≤ 300 ms | ≤ 100 ms | 0 | 0 |
| `list` | ≤ 200 ms | ≤ 500 ms | ≤ 100 ms | 0 | 0 |
| `self doctor` quick | ≤ 500 ms | ≤ 1 s | ≤ 100 ms | 0，必要的单个原子 probe 除外 | 0 |

预算先作为回归 gate，而不是“优化目标以后再看”。如果 native Windows/macOS 证明冷启动预算需要按平台调整，可以设置平台阈值，但不允许移除“零 payload walk/零无界 child process”的结构约束。

### 8.5 可观测性

新增 `--trace-perf` 或结构化 trace：

~~~text
state_open_ms
catalog_lookup_ms
state_query_ms
format_ms
subos_records_read
package_records_read
payload_files_scanned
child_processes_spawned
network_requests
state_generation
legacy_fallbacks
~~~

CI 对 `info/list` 断言：

- `payload_files_scanned == 0`；
- `child_processes_spawned == 0`；
- `network_requests == 0`；
- 单包 info 的 `package_records_read` 不随其他包数量增长。

## 9. doctor 重设计

### 9.1 默认 quick doctor

默认 `xlings self doctor` 只做控制面健康检查：

- DB/schema/generation 可读；
- 未完成 transaction 数量；
- projection generation 是否落后；
- workspace ref 指向的 instance 是否存在；
- XVM binding group/owner 元数据一致性；
- receipt/DB 的抽样或提交时 certificate 状态；
- 配置、权限、必要目录；
- runtime descriptor 是否完整；
- 上次 deep audit 的时间和结果。

它不递归遍历 payload，不对每个 ELF 启动工具，不调用外部 `xlings info`。

coordinate probe 改为调用内部 resolver/query API。一个进程内模块调用不应通过 shell 重新进入 CLI。

### 9.2 显式 deep doctor

~~~text
xlings self doctor --deep
xlings self doctor --deep --scope gcc@16.1.0
xlings self doctor --deep --changed-since <generation>
xlings self doctor --deep --workers 4
~~~

特性：

- 明确提示扫描范围、预计文件数；
- 100 ms 内输出进度；
- 支持取消、超时和 resume；
- 默认只扫 dirty/未认证 payload；
- 对已认证且 digest 未变的 payload 复用 install-time certificate；
- bounded worker pool；
- 结果写 audit cache，不改变安装状态；
- `--fix` 为独立 transaction，不能默认开启；
- 可输出机器可读 finding。

### 9.3 ELF/Mach-O/PE 检查

优先实现进程内只读 parser：

- Linux：读取 ELF program headers/dynamic section 获取 `PT_INTERP`、RPATH/RUNPATH、NEEDED；
- macOS：读取 Mach-O load commands、rpath、dylib、签名/架构元数据；
- Windows：读取 PE import table、manifest、machine type、runtime metadata。

外部 `patchelf/otool/dumpbin` 可保留为 debug cross-check，不作为每文件默认执行器。

安装时校验天然只检查本次 transaction 的 staged payload；deep doctor 主要验证外部修改、老版本迁移和历史遗留。

## 10. 错误、日志与接口协议

### 10.1 统一事件流

所有前端消费同一语义事件：

~~~text
StatusEvent
ProgressEvent
LogEvent { level, stream, message, package, phase, correlation_id }
DataEvent
ErrorEvent { code, message, cause_chain, hints, log_path, context }
CompletionEvent
~~~

呈现方式不同，但信息不能不同：

- CLI/TUI：适合人的样式和实时进度；
- CI：稳定纯文本 + tail + log artifact；
- NDJSON：一行一个 protocol event；
- Agent：稳定 code、context 和 remediation。

### 10.2 hook 失败契约

- Lua `error("...")` → `E_HOOK_EXCEPTION`；
- `return false, "..."` → `E_HOOK_REJECTED`；
- 仅 `return false` → `E_HOOK_RETURNED_FALSE` + 自动默认 message；
- timeout → `E_HOOK_TIMEOUT`；
- canceled → `E_CANCELED`；
- output 截断要显式标记；
- ErrorEvent 的 message 非空是 schema validator 的硬约束；
- `E_INTERNAL` 只用于真正未分类 invariant/exception，并总是带 correlation id 和 log path。

### 10.3 安全边界

- NDJSON stdout 绝不透传 recipe 原始字节；
- hook stdout/stderr 编码失败时 base64 或 escaped event；
- 日志默认去除 token、credential、敏感 env；
- 事件大小和总量有上限；
- hook 执行目录、环境和 PATH 明确记录；
- host process 与 payload process 使用不同 launch policy。

## 11. 用户态 OS 运行时与图形栈

### 11.1 xlings 应该负责什么

xlings 作为用户态 OS 应负责：

- 用户态 package graph 和版本；
- runtime profile 的选择与闭包；
- loader/libc/sysroot 一致性；
- package exports 与 host capability 声明；
- SubOS 的 runtime identity；
- 每进程 launch environment；
- payload/store/ref/effect 生命周期；
- 对外稳定的 runtime/query/interface contract。

xlings 不应负责：

- 内核和内核 ABI；
- 宿主 GPU kernel driver；
- WindowServer/Wayland compositor/Windows desktop；
- 系统级设备权限；
- 把所有宿主文件系统伪装成 xlings 包；
- 为每个上层构建器维护私有路径规则。

边界之外的能力通过 typed host bridge，而不是把宿主 `/usr/lib` 加进 private loader 的搜索路径。

### 11.2 RuntimeDescriptor

公共部分：

~~~text
RuntimeDescriptor {
  id,
  execution_domain,       # host | xlings-hermetic | portable-static
  os,
  arch,
  abi,
  sysroot,
  loader,
  core_library_dirs,
  sdk,
  min_os,
  runtime_packages[],
  environment_policy,
  host_bridges[],
  capabilities[],
  provenance,
  generation
}
~~~

Linux backend：

- glibc/musl + exact version；
- exact `PT_INTERP`；
- sysroot、lib/lib64；
- dynamic loader cache policy；
- Wayland/X11/audio/DRI/Vulkan 等 host bridges；
- native Linux、WSL2、container capability facts。

macOS backend：

- architecture/universal slices；
- min macOS deployment target；
- SDK identity；
- system frameworks 与 packaged dylib 的边界；
- `@rpath/@loader_path` policy；
- hardened runtime、library validation、Team ID/signing requirement；
- 不尝试用“私有 libc”复制 Linux 模型。

Windows backend：

- machine/subsystem；
- UCRT/MSVC runtime 或 MinGW runtime identity；
- app-local DLL set；
- package dependency graph/manifest；
- `AddDllDirectory/LoadLibraryEx` launch policy；
- 禁止依赖不受控 PATH/current-directory 搜索顺序。

### 11.3 三种执行域

1. **host**
   - 使用宿主 loader/core runtime；
   - xlings 只提供不冲突的工具/数据；
   - 适合强依赖系统 framework/driver 的程序。

2. **xlings-hermetic**
   - loader、libc/core deps 全部来自 xlings profile；
   - index package 必须满足闭包；
   - 宿主能力只通过 bridge；
   - 是 xlings 用户态 OS 的默认目标形态。

3. **portable-static**
   - 适合能够静态闭包的 CLI/服务；
   - 仍可能需要 DNS、证书、GPU 等 capability；
   - 不能把“静态”误解为无需宿主契约。

profile 在 resolve/build/run 三阶段必须是同一个 id。构建器可以决定用户 target 采用哪个 profile，但不能自行选择 profile 内的 glibc 目录。

### 11.4 一进程一套核心运行时

对 Linux：

- private loader + private glibc 可以加载同 profile 的库；
- 宿主 loader + 宿主 glibc 可以加载宿主闭包；
- private loader 加载宿主 `libtinfo/Mesa` 是未验证的 ABI 混合；
- host `/bin/bash` 继承 private `LD_LIBRARY_PATH` 同样是未验证混合；
- glibc 版本“更高”只解决 symbol floor，不解决 `GLIBC_PRIVATE` 和 loader/libc 同源。

GNU glibc 的 hardening 文档也明确不建议依赖 `LD_LIBRARY_PATH` 等 `LD_*` 环境变量改变默认动态链接行为。这里应优先使用产物自身 RUNPATH、明确 loader、runtime profile 和逐进程启动环境。

### 11.5 HostBridge/Portal

~~~text
HostBridge {
  kind,                  # wayland, x11, dri, vulkan-icd, audio, open-uri...
  provider,
  protocol_version,
  mounts[],
  sockets[],
  devices[],
  env[],
  launch_policy,
  compatibility_probe
}
~~~

关键是共享“协议和设备”，不是共享任意宿主动态库。

例如：

- Wayland：socket + protocol；
- X11：socket/display + auth；
- GPU：`/dev/dri` + 与 kernel driver 匹配的 userspace bridge；
- WSLg：检测 vGPU/WSLg capability，使用明确的 D3D12/Wayland bridge；
- open URI/file chooser/notification：broker/portal；
- host shell：由 host broker 启动，并清理 `LD_LIBRARY_PATH/DYLD_*` 等 private runtime 变量。

Flatpak 的 runtime + portal 模型证明了“隔离 runtime，通过 portal 使用宿主服务”的边界是可行的；xlings 不需要复制 Flatpak，但应采用同类的 typed boundary。

### 11.6 图形栈策略

图形能力按平台和硬件形成 capability bundle：

| 环境 | 推荐策略 |
|---|---|
| Linux AMD/Intel | xlings Mesa userspace + host DRM device bridge；验证 kernel/user ABI |
| Linux NVIDIA | vendor driver bridge，按 driver ABI 匹配，不盲目复制宿主 lib dirs |
| WSL2 | WSLg/vGPU capability bridge；运行 probe 报告 renderer 与 fallback |
| macOS | system Metal/OpenGL framework + signed app/runtime policy |
| Windows | DirectX/system graphics API + app-local cross-platform runtime DLL |

提供显式命令：

~~~text
xlings runtime show
xlings runtime explain <binary>
xlings graphics diagnose
xlings host-bridge list
~~~

输出实际 loader、runtime package、renderer、桥接来源和软件 fallback，避免用户只能从 `GLIBC_*` 或“窗口没出现”反推原因。

## 12. xim-pkgindex 的角色

### 12.1 从 recipe 集合升级为可验证契约源

新增或规范化声明：

~~~text
package.provides
package.effects
package.delegates
exports.runtime
requires.runtime_capabilities
requires.host_capabilities
supported_execution_domains
~~~

recipe 仍可包含命令式安装逻辑，但 identity/dependency/export/effect promise 必须可静态编译进 CatalogSnapshot。

### 12.2 closure certificate

当前 `dep-closure-check.sh` 已在验证：

- payload 的外部 soname 是否由直接 runtime dep 提供；
- 使用 xlings loader 的 sealed payload 是否仍依赖 host-only soname；
- host-loader payload 与 xlings-loader payload 的政策区别。

建议把结果升级为 install receipt 的 certificate：

~~~text
ClosureCertificate {
  payload_digest,
  runtime_profile_id,
  objects_scanned,
  interpreters,
  needed_sonames,
  providers,
  host_capabilities,
  tool_version,
  result
}
~~~

这样 default doctor 只检查 certificate 与 payload digest/generation；只有 dirty/legacy payload 才进入 deep audit。

### 12.3 针对 #506 的 index conformance

必须增加三类 fixture：

1. 零 XVM target 的 config/script package；
2. delegation package 自己零 target、被委托 package 有 target；
3. 同名 target 已被另一个 provider 拥有。

统一验收：

- install 成功；
- list/info 显示 package instance；
- remove 总会运行本 package 的 uninstall lifecycle；
- 只释放本 package 拥有的 effects/refs；
- 其他 provider target 不变；
- dependency ref 为零时才删除共享 payload。

### 12.4 针对 #514 的 host conformance

libxpkg 提供 fixture host：

- dependency 在当前 store；
- dependency 在 external/shared store；
- 多版本同时存在；
- namespace 同名；
- cache 目录存在但不在 resolution；
- resolution record 指向缺失/损坏 payload。

任何宿主包括 xlings、mcpp 或未来 Agent runner 都必须通过同一 suite。期望值来自 ResolutionContext，而不是扫描顺序。

## 13. 兼容与迁移

### 13.1 绝不在第一个查询中做重型“无感迁移”

“无感”应定义为：

- 不打断正常操作；
- 可回滚；
- 有进度和错误；
- 不改变语义；
- 不把几分钟工作伪装成命令卡死。

它不等于“无论多少文件都在启动时静默扫描”。

约束：

- 查询启动时 schema 检测 ≤ 10 ms 量级；
- 可自动完成的 metadata migration 必须受 250 ms 硬预算约束；
- 超出预算时继续走已优化 legacy read path，并提示在下一次写操作迁移；
- 全 payload 验证永远不是 schema migration；
- 冲突返回 `E_STATE_MIGRATION_CONFLICT` 和报告，不循环重试。

### 13.2 分阶段迁移

**Release A：P0 + shadow store**

- 修复当前 targeted query 和 quick doctor；
- 引入 State Store API、receipt、transaction journal；
- install/update/remove 开始双写；
- 旧状态仍是对外权威；
- 查询 shadow compare，不影响用户结果。

**Release B：双读比对**

- 新安装默认从 State Store 读；
- 老 home 仍可走 legacy reader；
- CI 和 opt-in telemetry/trace 比较两套结果；
- mismatch 生成可读报告，不静默选择。

**Release C：State Store 权威**

- 查询只读 DB snapshot；
- legacy JSON/XVM 继续作为 projection 输出；
- 老 home 在第一次写操作或显式 `self migrate` 导入；
- 支持 read-only rollback 到前一版客户端。

**Release D：收缩 legacy**

- 至少跨两个正常 release window；
- 公开 migration/rollback 成功率和遗留分类；
- 再决定是否移除旧 reader。

### 13.3 legacy importer

输入：

- global/project/SubOS workspace；
- VersionDB 与 binding-group metadata；
- payload stamp；
- `.xlings-resolution.json`；
- xpkg snapshot；
- payload directory identity。

规则：

- 以 canonical namespace/name/version/platform/arch 归一化；
- binding provider 与 package identity 分开；
- 绝不从 target 名反推唯一 package；
- symlink walk 记录 canonical inode/file id 并设深度上限；
- 同名冲突 quarantine，不“最后一个赢”；
- importer 只读 metadata 和目录层级，不打开 payload 全部文件；
- 导入前 snapshot，导入后 diff report；
- 可重复执行且结果稳定。

### 13.4 锁和并发

- SQLite WAL/local snapshot 允许查询与安装并行；
- query 不等待 payload 下载和深度验证；
- writer lock 记录 PID、command、start time、transaction id、generation；
- 卡住时输出持有者和可恢复动作；
- 不用零字节 lock file 作为唯一诊断；
- network filesystem home 若不支持 WAL，显式切换兼容 journal mode 并告警；
- projection 写入使用 temp + fsync + atomic rename。

## 14. 实施顺序

### P0：恢复即时响应和可诊断性

1. `collect_package_inventory` 改为真正单包 targeted path；
2. `list` 避免无关 SubOS/payload/catalog 工作，至少恢复旧版量级；
3. 默认 doctor 移除全 store ELF 扫描；
4. 原深扫迁到 `--deep`，立即显示进度；
5. doctor coordinate probe 改内部 API；
6. 增加 `--trace-perf` 关键 counter；
7. 添加 heavy-home performance fixtures；
8. #513 保证 false hook 得到非空错误，保留 stdout/stderr tail；
9. 发布前在当前长期 home 和全新 home 都跑真实命令。

P0 的完成条件不是“测试通过”，而是：

- 当前 124/323/38/43 万文件 home 上 `info/list` 恢复到交互预算；
- default doctor ≤ 1 秒级并有即时输出；
- 三个命令零 payload recursive walk；
- info/list 零 child process；
- #513 repro 不再出现空 `E_INTERNAL`。

### P1：安装账本与 query control plane

1. `state::Store` API；
2. SQLite/替代 backend ADR；
3. PackageInstance/refs/effects schema；
4. install receipt 和 transaction journal；
5. CatalogSnapshot；
6. list/info shadow read；
7. crash recovery tests；
8. legacy importer。

### P2：生命周期与 resolver 契约

1. EffectManifest；
2. XVM/workspace/shim/sysroot projection；
3. removal 从 package instance 开始；
4. delegation/refcount；
5. ResolutionContext v1；
6. libxpkg install mode 缺 record 时 fail closed；
7. #506/#514 conformance suite；
8. xlings 与 mcpp 都作为 host 跑 suite。

### P3：RuntimeDescriptor 与 host bridge

1. Linux descriptor 和 `xlings interface` schema；
2. SubOS runtime identity 迁移；
3. per-process sanitized launcher；
4. graphics/portal/host-shell bridge；
5. xim-pkgindex capability/closure schema；
6. mcpp 删除目录选择逻辑，消费 descriptor；
7. macOS/Windows native backend；
8. `runtime explain / graphics diagnose`。

### P4：切权威与清理

1. 双读差异归零；
2. old-home native migration matrix；
3. State Store 权威；
4. legacy projection deprecation；
5. deep audit incremental cache；
6. 文档与稳定协议版本政策。

## 15. 验证矩阵

### 15.1 性能

| 维度 | 样本 |
|---|---|
| package instance | 0 / 100 / 1,000 / 10,000 |
| versions | 1 / 3 / 20 per package |
| SubOS | 1 / 10 / 100 |
| payload files | 0 / 100k / 1M |
| catalog indexes | official / local overlay / missing / stale |
| state | fresh / long-lived / legacy / partial migration / corrupt |

断言不仅看时间，还看算法 counter。仅时间测试可能在快机器上掩盖一次全量扫描。

### 15.2 issue 回归

- #513：stdout、stderr、`log.error`、Lua exception、false/no-message、NDJSON；
- #514：external shared cache、sibling workspace、多版本、namespace、missing record；
- #506：zero-target、delegating、foreign-provider target、dependency-free config；
- #392：private 2.39 + host newer lib、private 2.44 + host shell、多个 glibc payload、改名历史目录、WSLg/Mesa。

### 15.3 crash/一致性

在 transaction 每个状态点 kill：

- staging 后；
- hook 后；
- payload rename 后；
- DB commit 前后；
- projection 写一半；
- remove hook 后；
- payload 删除前。

重启后必须：

- 自动识别唯一未完成 transaction；
- 给出恢复动作；
- query 仍读上一个 committed generation；
- 不做全 store 扫描；
- 重放幂等。

### 15.4 平台

| 平台 | 必须验证 |
|---|---|
| Linux glibc | ELF loader/libc closure、Wayland/X11、DRI、host broker |
| Linux musl | runtime profile 不混入 glibc、静态/动态路径 |
| WSL2 | WSLg/vGPU、host interop、`LD_LIBRARY_PATH` 清理 |
| macOS x86_64/arm64 | Mach-O/rpath、universal、dyld env、library validation/signing |
| Windows x86_64/arm64 | DLL search、UCRT/MSVC/MinGW、app-local、delegation removal |

必须用 native runner；不能从 Linux 源码审查推断 macOS/Windows 已通过。

### 15.5 用户旅程

- fresh install；
- 从 2026.8.2.1、2026.8.3.1、2026.8.5.3、2026.8.9.2 长期 home 升级；
- 只运行 list/info 的用户；
- 多 SubOS；
- project-local index；
- 离线；
- Agent/NDJSON；
- interrupted install/remove；
- release artifact + published latest + real pkgindex。

## 16. 方案对比

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. 只修两个慢循环 | 最快恢复；变更小 | #506/#514/#392 的真值碎片继续存在；其他命令会再次扫描 | 仅作为 P0，不是终局 |
| B. 一次性重写状态/运行时 | 模型整齐 | 迁移风险最高；延误 P0；跨三平台难以一次验证 | 不推荐 |
| C. P0 + 契约先行渐进迁移 | 立即止血；可双读验证；兼容旧 home；逐层收敛 | 迁移期有双写/projection 成本 | 推荐 |

## 17. 风险与控制

| 风险 | 控制 |
|---|---|
| SQLite 增加依赖和体积 | 独立 ADR、静态三平台构建 gate、封装 backend |
| DB 与文件系统无法原子提交 | transaction journal + staged rename + 幂等 recovery |
| dual-write 产生差异 | generation、shadow compare、差异报告、旧状态保留 |
| compiled catalog 与动态 recipe 不一致 | snapshot digest、publish-time validator、显式 stale |
| EffectManifest 漏掉 hook 旁路写入 | sandbox/operation interception；未声明外写入在 CI fail |
| runtime profile 过度 Linux 化 | tagged platform backend，公共语义不公共字段硬套 |
| host bridge 变成“挂整个宿主” | capability allowlist、protocol/device 优先、审计输出 |
| quick doctor 因“检查少了”失去信任 | 展示 last deep audit/certificate；明确 quick/deep 语义 |
| 性能再次回退 | 结构 counter + native benchmark 作为 PR gate |

## 18. 非目标

- 不在本方案中替 mcpp 选择具体 GUI toolkit；
- 不要求所有用户程序必须 hermetic；
- 不把宿主 GPU driver 打包进通用 payload；
- 不承诺跨任意旧 glibc/新驱动组合自动兼容；
- 不用 fake XVM target 修补 package identity；
- 不通过扫描更多目录解决 ResolutionContext 缺失；
- 不把 full payload audit 放回默认 doctor；
- 不在日常 list/info 中自动更新 index。

## 19. 建议 review 的六个决策点

### D1. 是否确认查询契约

建议确认：

> 除显式下载/refresh/deep audit 外，xlings 查询必须即时、本地、无 payload walk、无 child process。

这是后续所有实现取舍的最高约束。

### D2. 是否接受“P0 先恢复，架构渐进迁移”

建议接受方案 C。P0 不等待 State Store；State Store 也不能以“以后会重构”为理由跳过。

### D3. 是否确认 package instance 是安装锚点

建议确认：

> XVM target 是 effect，不是 package identity；零 target package 是正常形态。

这决定 #506 是继续修 predicate，还是从根上结束 target 反推。

### D4. 是否确认 ResolutionContext fail-closed

建议确认：

> install transaction 中缺 resolver record 应明确失败；目录扫描只保留为显式 legacy/offline 工具能力。

这决定 #514 是补 mcpp path，还是建立真正通用的 host contract。

### D5. 是否确认运行时策略

建议沿用并强化当前目标：

> xlings-hermetic 追求 X-complete；宿主库/服务只通过 typed host bridge，禁止半 host 半 private core runtime。

同时保留显式 host profile，不强迫所有应用 hermetic。

### D6. control plane 存储方向（已 review）

当前不引入 SQLite，也不引入自定义二进制数据库。若后续推进 State Store，先以存储无关接口、版本化 JSON snapshot、atomic replace、bounded transaction journal 和 immutable payload receipt 落地；当前 bugfix 不以 State Store 为前置。未来是否采用 SQLite 必须另开 ADR，以真实规模和三平台原型重新证明。

## 20. 参考

### Issues

- [openxlings/xlings #514 — shared registry cache dependency install_dir](https://github.com/openxlings/xlings/issues/514)
- [openxlings/xlings #513 — hook output swallowed and empty E_INTERNAL](https://github.com/openxlings/xlings/issues/513)
- [openxlings/xlings #506 — delegating package removal](https://github.com/openxlings/xlings/issues/506)
- [mcpp-community/mcpp #392 — GUI/runtime/graphics stack failure](https://github.com/mcpp-community/mcpp/issues/392)

### 仓库内相关设计

- [Dependency resolution single source](./2026-08-05-dependency-resolution-single-source.md)
- [Userspace distro hermetic strategy](./2026-08-05-userspace-distro-hermetic-strategy.md)
- [Ecosystem closure design](./2026-08-09-ecosystem-closure-design.md)
- [Declared vs effective open defects](./2026-08-08-declared-vs-effective-open-defects-design.md)

### 平台权威资料

- [GNU C Library — Dynamic Linker](https://sourceware.org/glibc/manual/latest/html_node/Dynamic-Linker.html)
- [GNU C Library — Dynamic Linker Hardening](https://sourceware.org/glibc/manual/2.43/html_node/Dynamic-Linker-Hardening.html)
- [Microsoft — Dynamic-link library search order](https://learn.microsoft.com/windows/win32/dlls/dynamic-link-library-search-order)
- [Microsoft — Run Linux GUI apps with WSL](https://learn.microsoft.com/windows/wsl/tutorials/gui-apps)
- [Apple — Allow DYLD environment variables entitlement](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.cs.allow-dyld-environment-variables)
- [Apple — Disable Library Validation entitlement](https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.cs.disable-library-validation)
- [Flatpak — Basic concepts: runtimes, sandboxes and portals](https://docs.flatpak.org/en/latest/basic-concepts.html)
