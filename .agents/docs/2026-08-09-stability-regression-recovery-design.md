# xlings 稳定性回归恢复与运行时可用性修复设计

> 状态：Accepted for implementation；根因复核与实现进行中
>
> 日期：2026-08-09
>
> 性质：Bugfix / regression recovery，不是功能扩张
>
> 范围：`openxlings/xlings`、`openxlings/libxpkg`、`openxlings/xim-pkgindex`；`mcpp-community/mcpp` 只修改现有 runtime 选择缺陷并承担真实验收，不引入 xlings 专用架构
>
> 架构边界：长期的 State Store、PackageInstance、RuntimeDescriptor、typed host bridge 等设计转入 [xlings #518](https://github.com/openxlings/xlings/issues/518)；本方案不等待也不实现那轮架构重构

## 0. 结论

本轮只解决四组已经发生、可复现、可回归验证的问题：

1. `xlings list`、`xlings info gcc`、`xlings self doctor` 等本地命令从即时响应退化到数秒或无界等待；
2. xlings runtime 与生态图形栈在真实桌面程序上存在 loader/libc、宿主 shell、GPU/窗口系统闭包的稳定性缺口；
3. #513、#514、#506 等安装、解析、移除链路中的真实正确性缺陷；
4. `xlings subos use` 无参数直接报错、候选发现与实际进入使用不同答案源、名称不能安全模糊匹配。

总策略是：

> 恢复旧版最好延迟，同时保留新版已经增加的正确性覆盖；修当前事实链，不引入 SQLite，不重写状态系统，不为 mcpp 增加 xlings 专用分支。

本方案完成后应满足：

- 除显式下载、更新、安装、深度审计外，本地查询立即给出结果；
- `info <pkg>` 的成本与其他包、SubOS 和 payload 文件数量无关；
- 默认 doctor 不遍历全 payload，也不逐 ELF 启动外部工具；
- runtime 的 loader/libc 核心混用在安装时被双向拦截；
- mcpp #392 的原始场景在多个 glibc payload 并存时稳定通过；
- hook 失败不再产生空 `E_INTERNAL`；
- shared registry dependency 优先由精确解析记录定位；若它属于宿主的独立依赖域，则只在宿主显式提供的有序 store roots 中做 exact namespace/version 查找；
- 零 XVM target 或委托安装的包可以对称移除；
- `xlings subos use` 无参数可发现候选，模糊匹配不会误进环境。

## 1. 范围与非目标

### 1.1 本轮必须完成

- 已知慢命令的根因修复；
- 对所有“本地只读命令”做同类路径审计，防止只修三个命令名；
- 保留 `2026.8.3.1` 以后新增的 exact inventory、namespace、project store、zero-target、degraded 状态等正确性；
- 默认 doctor 恢复到 `2026.8.5.1` 之前的有界交互体验；
- 保留 loader/libc 全仓检查能力，但改成显式 deep audit；
- 修复 loader/libc 守卫漏掉的反向组合；
- 固化 glibc 2.44、ncurses、Mesa、Wayland、NVIDIA/WSL bridge 已落地能力的真实验收；
- 修复 #513、#514、#506；
- 修复 SubOS 候选、默认 SubOS 兼容和 `subos use` 交互。

### 1.2 明确不做

- 不引入 SQLite；
- 不自研另一个数据库或二进制状态格式；
- 不在本轮建立完整 PackageInstance/EffectManifest 账本；
- 不在本轮发布通用 RuntimeDescriptor/HostBridge API；
- 不把宿主驱动或任意 `/usr/lib` 打进 xlings；
- 不自动原地升级已有 SubOS 的 glibc；
- 不为 mcpp 硬编码 shared registry 路径到 xlings/libxpkg；
- 不借性能修复删除新版已验证的输出和兼容性语义；
- 不把硬件未验证写成通过。

## 2. 当前证据与回归边界

### 2.1 性能回归是确定的

同一个长期使用的真实 `XLINGS_HOME`：

| 命令 | 2026.8.2.1 | 2026.8.5.1 | 2026.8.9.2 | 回归边界 |
|---|---:|---:|---:|---|
| `xlings list` | 0.21 s | 7.37 s | 7.43 s | `0c362cf` / 2026.8.3.1 |
| `xlings info gcc` | 0.08 s | 17.26 s | 17.31 s | `0c362cf` / 2026.8.3.1 |
| `xlings self doctor` | 0.84 s | 0.86 s | 30 s 内无输出 | `c512679` / 2026.8.5.3 |

该 home 有约 124 个包目录、323 个版本、38 个 SubOS、434,210 个普通 payload 文件和约 141 GiB 数据。

规模只是放大器。根因分别是：

- `collect_package_inventory()` 先构造所有包、所有 SubOS 的 inventory，再删除非目标包；
- `list` 对所有已装记录做 recipe/cached metadata 与文件状态的同步重组；
- doctor 默认对全部 payload 递归扫描，逐 ELF 调用外部 `patchelf`；
- doctor 的 coordinate probe 又启动 `xlings info` 子进程，叠加慢路径。

这不是网络等待、锁持有、binding 逻辑死循环或渲染慢，而是同步执行了没有交互上界的本地重算。

### 2.2 “回到旧版”不是修复

`2026.8.3.1` 同时增加了正确性覆盖：

- exact inventory 不再由 index 的 latest 结果替代安装事实；
- namespace identity；
- current SubOS 与 `--all`；
- missing payload/degraded 显示；
- project-scoped store；
- zero-target/data-only package stamp；
- active/inactive 和多版本显示；
- CLI/TUI/Agent/interface 输出一致性。

因此验收线不是“延迟回到 2026.8.2.1，但语义也回去”，而是：

> 延迟至少恢复到旧版最好水平；语义覆盖不得低于当前已经落地的 contract tests。

### 2.3 runtime/graphics 当前不是空白

截至当前快照，以下能力已经落地：

| 能力 | 当前状态 |
|---|---|
| 新 SubOS 默认 runtime binding | `glibc@2.44`，2026.8.9.1 |
| 宿主 glibc 记录 | `subos_info.host_glibc` |
| loader 与 libc 同 payload 检查 | 安装路径已有，但谓词少一半 |
| closure 检查 | index CI hard gate；客户端 fresh payload warn-only |
| private glibc 环境净化 | SubOS/mcpp 已有局部守卫 |
| ncurses/libtinfo | 已进入 xim-pkgindex |
| Mesa | 25.0.7.2，含 llvmpipe/radeonsi/iris/nouveau/zink/d3d12/RADV |
| Wayland | runtime 支持和 probe 已存在 |
| NVIDIA | `nvidia-gl-host-link`，不复制整套宿主驱动 |
| WSL2 | `wsl-gl-host-link` + `/dev/dxg` 支持 |
| graphics meta package | `xlings install graphics` |

剩余问题主要是：守卫缺口、选择结果不唯一、旧 SubOS 迁移口径、真实硬件矩阵和 release gate，而不是再设计一套图形架构。

### 2.4 一个必须立即修的守卫缺口

当前 `elfcheck::check()` 只检查：

```text
xlings loader + 另一个 xlings glibc
```

如果 `PT_INTERP` 是宿主 loader，它会直接跳过。已经真实发生并导致 JDK 发布回滚的组合恰好是：

```text
host loader + xlings libc in RPATH
```

宿主 `ld.so` 会从 RPATH 拿到私有 `libc.so.6`，进程在 `main` 前崩溃。两种方向都属于同一个 bug：一个进程混用了两套 core runtime。

## 3. 本地只读命令的统一契约

### 3.1 命令分类

命令按是否允许慢操作分类，而不是逐个打补丁：

| 类别 | 示例 | 默认允许网络 | 默认允许 payload 递归 | 默认允许 child process |
|---|---|---:|---:|---:|
| 纯元数据 | `--version/help/config/index list/subos list` | 否 | 否 | 否 |
| 包查询 | `search/list/info/why/xvm list` | 否 | 否 | 否 |
| 快速健康检查 | `self doctor` | 否 | 否 | 仅严格有界的单次平台 probe；目标为 0 |
| 显式深审计 | `self doctor --deep` | 否 | 是 | 是，但 bounded |
| 变更/获取 | `install/update/self update/index update` | 是 | 按事务需要 | 按事务需要 |

如果后续新增命令，必须先归入其中一类，不能靠“这个命令大概会快”维持体验。

### 3.2 结构约束

所有纯查询必须满足：

- 不访问网络；
- 不执行 recipe hook；
- 不递归遍历 payload；
- 不启动新的 `xlings`；
- 不在查询入口做全量迁移；
- 不等待安装、下载或 deep audit 的长锁；
- 输入规模只与本问题需要的 metadata records 有关。

性能预算是第二道门，结构约束是第一道门。只测时间会被 CI 机器速度掩盖；禁止的工作量一旦进入路径，即使小 fixture 仍可能绿。

## 4. 工作流 A：彻底修复命令卡顿

### A1. `info <pkg>` 改成真正 targeted inventory

当前反模式：

```text
all packages × all SubOS × all payload roots
  -> assemble inventory
  -> erase everything except requested package
```

修复后的数据流：

```text
resolve one PackageMatch
  -> read target recipe once for detail fields
  -> look up only this identity in current/all SubOS workspace maps
  -> look up only this identity's XVM owner records
  -> inspect only this identity's exact payload version directories
  -> render
```

实现约束：

- `collect_package_inventory` 接收已解析的 canonical identity/store root，不能重新做全 catalog search；
- workspace JSON 可以读取，但只按目标 key 提取；
- VersionDB 已加载到内存后只查相关 target/provider；
- payload-only package 只检查目标的 store directory 和 stamp，不扫其他包；
- project/global store 由已解析 `PackageMatch.storeRoot` 决定；
- namespace 不明确时 fail/显示 ambiguity，不合并两个 identity；
- 保留多版本、多个 SubOS、active、missing payload、index unavailable 状态。

复杂度目标：

```text
O(number of SubOS metadata files + target versions)
```

不得与其他 package 数量和 payload 文件数量相关。

### A2. `list` 采用 workspace-first + 查询前置过滤

`list` 本来就需要枚举已装包，但只应枚举“安装记录”，不能枚举全部 index recipe 或 payload 文件。

修复路径：

1. current list 从当前 workspace `installed/active` 建行；
2. `--all` 再合并各 SubOS workspace；
3. zero-target/data-only package 从 store 顶层的 installer stamp 做浅层发现；
4. missing payload 只对已经生成的 `(identity, version)` 做存在性检查；
5. filter 在 metadata lookup 和 row assembly 之前生效，不能先重建所有行再删除；
6. description、type、program summary 只从现有本地 catalog/cache 读取；cache 缭字段时显示空/unknown，不在 list 中执行 Lua recipe 重建 cache；
7. 本轮不以新增 cache schema 为前置；若后续确需 lightweight summary，另做可丢弃的 cache 小版本升级，并只在显式 index update/build 路径重建。

`list` 允许的目录访问上限：

- SubOS metadata 文件；
- store 的 package/version 顶层；
- 每个候选的 stamp/目录存在性。

禁止进入任一 payload version 的内容树。

### A3. 默认 doctor 回归 quick mode

`c512679` 增加的 loader/libc 全 store 扫描从默认 `detect_()` 移到显式 deep path。

默认 doctor 保留：

- 配置与必要目录；
- workspace/XVM binding 元数据一致性；
- dangling shim/sysroot link 的 bounded metadata/path 检查；
- 当前 SubOS manifest、env、runtime binding；
- 未完成或损坏的轻量状态；
- 上次 deep audit 的摘要（若存在）。

默认 doctor 删除：

- 全 xpkgs 递归 ELF scan；
- 每 ELF 两次 `patchelf`；
- 为 coordinate probe 启动 `xlings info`；
- 对历史 payload 的运行期功能探针。

原有能力通过以下显式入口保留：

```text
xlings self doctor --deep
xlings self doctor --deep --scope <package[@version]>
```

`--deep` 不是新增诊断能力，而是把已经存在的昂贵检查从错误的默认入口移到显式入口。

deep mode 必须：

- 100 ms 内显示扫描范围；
- 显示 package/version/file 进度；
- worker 数量有界；
- 可取消；
- scope 可限定；
- 失败保留已完成 findings；
- 不改变安装状态；`--fix` 仍是独立写事务。

### A4. coordinate probe 改成内部调用

doctor 当前没有 catalog/resolver 依赖；为避免 module cycle，只在 catalog 叶子模块增加一个小型、只读、no-sync 的 coordinate 可解析性查询。doctor 调用它，不通过 shell 重新进入 `xlings info`，也不导入会反向依赖 xself 的 `xim.commands`。

这项修改是局部去递归：

- 不改变 resolver 规则；
- 不增加状态格式；
- 不引入新公共 API；
- 测试用同一组 coordinate fixture 对比旧 CLI 结果与内部结果。

### A5. 覆盖所有查询入口

修复三个已知命令后，对 CLI spec 中全部只读命令做自动审计：

- 空 home；
- fresh installed home；
- legacy home；
- 100+ package / 30+ SubOS metadata fixture；
- payload 目录含 0、10k、100k 个深层文件；
- corrupt/stale index cache；
- project-scoped index；
- namespace collision。

结构断言：

```text
payload_files_visited == 0
child_xlings_spawned   == 0
network_requests       == 0
```

这些计数优先作为 test-only instrumentation，不要求扩张公开 CLI。

### A6. 性能验收

目标以真实长期 home 为准：

| 命令 | 目标 | 最低不可退化线 |
|---|---:|---:|
| `xlings info gcc` | warm ≤ 100 ms | 不慢于 2026.8.2.1 的同机结果 + 20% |
| `xlings list` | warm ≤ 250 ms | 不慢于 2026.8.2.1 的同机结果 + 20% |
| `xlings self doctor` | warm ≤ 1 s | 必须有界完成并在 100 ms 内首屏 |

CI 时间门禁采用较宽的平台阈值避免噪声，但结构断言不能放宽。

### A7. 语义不回退矩阵

下列现有测试必须继续通过，并补 heavy-home 变体：

- `list_exact_inventory_test.sh`；
- `info_output_contract_test.sh`；
- namespace 同名；
- project-scoped payload；
- zero-target/data-only package；
- missing payload/degraded；
- inactive 多版本；
- `list --all` SubOS attribution；
- CLI、`--agent`、interface 结果同构。

## 5. 工作流 B：runtime 与生态图形栈稳定性

### B1. 不大改架构的边界

本轮沿用现有机制：

- SubOS 的 `subos_info.runtime` 是 runtime 选择；
- resolver 的 exact result 是依赖选择；
- payload RUNPATH/PT_INTERP 是产物事实；
- package env 和现有 host-link 是图形发现机制；
- `xlings-gl-doctor` 和现有 verify scripts 是诊断入口。

不引入 RuntimeDescriptor、portal framework 或新数据库。本轮只要求这些现有答案互相一致，并对真实失败组合设硬门禁。

核心 runtime 不变量是：

- 同一个全局 store 中允许并预期存在多份 glibc payload；它们是可共享、可被不同 SubOS 引用的不可变库存，不因当前未生效而删除或报错；
- 每个 SubOS 在一次解析和执行中只能有一个生效 runtime，结果必须是 `runtime family + exact version + exact payload path`；
- `subos_info.runtime` 是当前 SubOS 的权威 active binding；payload 目录只承载库存，不能反向决定哪个版本生效；
- legacy SubOS 可以从编译器已烘焙的 runtime 或唯一兼容候选恢复答案，但恢复后的单次执行结果仍必须唯一；
- 只有“缺少权威 binding 且兼容证据无法得到唯一结果”才拒绝执行，不能因为 store 中同时有 2.39 和 2.44 就拒绝。

### B2. loader/libc 守卫必须双向

合法组合：

| PT_INTERP | core libc search path | 结果 |
|---|---|---|
| host loader | host core | 允许 |
| xlings loader A | xlings libc A | 允许 |
| xlings loader A | xlings libc B | 拒绝 |
| host loader | 任一 xlings libc/loader dir | 拒绝 |

修改现有 `elf_same_source` 谓词，不增加第二套实现：

- 宿主 interpreter 不再直接 skip；
- RPATH/RUNPATH 中若目录提供 `libc.so.6` 或 `ld-linux*`，必须与 interpreter 属于同一 core；
- xlings loader 情况继续要求 provider/version 同源；
- shared library/static binary 不选择 interpreter，继续跳过；
- `$ORIGIN` 需按被测 ELF 位置归一后再判断；
- finding 必须同时打印 interpreter 和危险 core path。

同一谓词用于：

- 安装时 hard fail；
- xim-pkgindex CI；
- `doctor --deep` 历史审计；
- unit differential fixture。

### B3. runtime 选择不得再按目录顺序

mcpp 当前 build probe 已优先读取目标 SubOS runtime binding，并保留 compiler baked value 兼容老 SubOS；但 post-install fixup 仍有 `directory_iterator` 首个 glibc payload 路径。还需区分两个合法 authority：共享 compiler 自身的 cfg/PT_INTERP 属于 compiler owner/default SubOS；某个 project 的 target flags 属于该 project 选择的 SubOS。不能让后构建的 sibling SubOS 重写共享 compiler。

本轮要求：

1. target 有 `subos_info.runtime`：target build/link 只使用对应 exact payload；不存在则明确失败；
2. shared compiler fixup：只读取 compiler owner/default SubOS 的 exact runtime，并持久化到 cfg/fixup fingerprint；不能读取当前 project SubOS；
3. 老 SubOS：优先读取 compiler 已烘焙的 runtime；
4. 只有一个兼容 glibc payload：可作为 legacy SubOS 的无歧义兼容结果；
5. 多个 payload 本身完全合法；仅当缺少相应 artifact 的权威记录、没有 compiler baked runtime，且多个兼容候选无法唯一确定时，才拒绝选择并列出候选；
6. 禁止按目录顺序、字符串顺序、“newest”注释或改名目录选择；
7. 两个 sibling SubOS 交替 clean build 时，共享 compiler cfg 不抖动，各自 target 仍消费各自 resolved binding。

这是 mcpp 消费现有 xlings manifest 的修复，不要求 xlings 为 mcpp 新增专用路径。

### B4. host process 环境必须净化

保持现有原则并补齐入口：

- 用户产物可通过自身 RUNPATH 使用 xlings runtime；
- host shell、`xdg-open`、`notify-send`、`gio`、构建工具等宿主进程不得继承 private glibc 目录；
- `LD_LIBRARY_PATH`、`LD_PRELOAD`、`DYLD_LIBRARY_PATH`、`DYLD_INSERT_LIBRARIES` 中包含 loader/libc 的项必须移除；
- 非 core 的用户显式路径保留；
- child-only environment，不修改父进程全局环境；
- shell path 与 direct exec path 使用同一净化谓词。

mcpp 必须从 target 的 process-global `LD_LIBRARY_PATH` 中彻底删除任何实际包含 libc/loader 的目录；非 libc 的 runtime dependency libdirs 保留。仅净化 mcpp 自己直接启动的 child 不够，因为 target 还会再启动 `/bin/sh`、`xdg-open` 等 host-loader grandchildren。

回归场景必须包含 `__pointer_chk_guard@GLIBC_PRIVATE` 原始签名，而不是只测一个伪路径字符串。

### B5. 旧 SubOS 不静默换 runtime

现有 SubOS 的 runtime 是创建期属性。本轮保持：

- 新建 SubOS 默认记录 2.44；
- 显式 `--runtime` 时安装该 runtime；
- 老 SubOS 不原地自动从 2.39 改到 2.44；
- doctor 快速显示“declared runtime missing/低于创建时 host floor”；
- 推荐迁移命令是新建 SubOS并重装，而不是改 JSON 后继续使用旧产物；
- 所有提示必须说明哪些产物需要 clean rebuild。
- `subos_info.runtime` 与该 SubOS workspace/XVM active runtime 不一致时，doctor 报出两边 exact version；普通 `xlings use glibc@different` 拒绝制造分裂状态，迁移必须显式且原子。

避免两种无感破坏：已有 2.39 产物被 2.44 loader 接管，以及 runtime 字段改了但 compiler cfg/产物仍指向旧版本。

### B6. 图形栈本轮只收口现有能力

不重新拆 Mesa，不升级 Mesa 大版本，不发明新图形抽象。收口项目：

- Mesa 25.0.7.2 payload 内容与 recipe 描述一致；
- `iris_dri.so`、`d3d12_dri.so`、radeonsi、nouveau、llvmpipe 存在性；
- build-only llvm-dev/SPIRV/DirectX-Headers 不泄漏到 runtime DT_NEEDED；
- Wayland probe；
- NVIDIA host-link 驱动版本漂移与回退；
- WSL2 `/dev/dxg`、D3D12/WSLg；
- ncurses/libtinfo 进入 runtime closure；
- graphics meta package fresh install；
- GLOBAL/CN 资源 sha256 一致。
- `.agents/tools/graphics/verify-stack.sh --subos <name>` 的 install/query/build 必须全部显式作用于同一个 named SubOS，不能创建 `gfxverify` 后又安装到 ambient default；
- NVIDIA interposer 区分 built 与 closure-complete，并把 unresolved SONAME 写入 doctor/验收输出；warning-only install 不得被汇报为 sandbox-complete。

### B7. mcpp #392 原始场景验收

使用隔离 `HOME/XLINGS_HOME/MCPP_HOME`，至少包含：

1. 同时存在 glibc 2.39 和 2.44 payload；
2. active SubOS runtime 为 2.44；
3. clean build 后产物 PT_INTERP/RUNPATH 只指向 2.44；
4. 将 2.39 改成无效历史候选的 fixture 不改变选择；
5. 主程序稳定启动；
6. 主程序调用宿主 shell 成功；
7. `xdg-open/notify-send/gio` 至少用不会执行真实桌面副作用的 probe 验证 loader 可启动；
8. `LD_LIBRARY_PATH` 不含 private glibc；
9. llvmpipe 无 GPU路径可渲染；
10. 对应硬件路径由 native runner/真实设备给出 renderer/provenance。

关键执行证据必须通过真实 SubOS 入口获得，例如：

```text
xlings subos use gfxverify --sandbox --cmd '<probe>'
xlings subos use gfxverify --sandbox --gpu --cmd '<hardware-probe>'
```

仅设置 `XLINGS_ACTIVE_SUBOS` 或手写 payload loader 可以作为单元/定位手段，不能代替最终 sandbox 验收。

通过标准是重复 clean build/run 后仍一致，不能用手工删除 2.39 作为前置。

### B8. 真实平台矩阵

| 环境 | 门禁 | 当前不得冒充的结论 |
|---|---|---|
| Linux 无 GPU/container | llvmpipe 像素 + provenance | 不能代表硬件 GPU |
| Linux NVIDIA | proprietary renderer + driver drift | 不能代表 nouveau |
| Linux AMD | radeonsi native runner/实机 | 仅 payload 存在不算渲染通过 |
| Linux Intel | iris native runner/实机 | 仅 `iris_dri.so` 存在不算通过 |
| Wayland | headless compositor probe | X11 通过不能代表 Wayland |
| WSL2 | `/dev/dxg` + d3d12/WSLg 实机 | 普通 Windows runner 不能代表 WSLg |
| macOS | native loader/framework/signing smoke | 不套用 glibc/Mesa 结论 |
| Windows | native DLL search/app-local smoke | 不套用 ELF/RPATH 结论 |

缺真实硬件时状态是 `not exercised`，不是 pass，也不是无条件阻塞 Linux 通用修复。

## 6. 工作流 C：三个真实正确性 bug

### C1. #513：hook 失败不能是空错误

问题链：

- Lua `install()` 返回 `false`；
- libxpkg `HookResult.success=false`，但 `error` 为空；
- xlings 把空字符串继续传到 status/error；
- terminal/interface/mcpp 最终只看到空 `E_INTERNAL`；
- recipe stdout、stderr、`log.error` 又不在同一出口。

最小修复分两层：

**libxpkg：**

- `false` 且无 message 时生成稳定的 `E_HOOK_RETURNED_FALSE` 文本；
- Lua exception 保留原始错误；
- hook-local bounded stdout/stderr/log tails；
- 输出上限，超出时注明截断；
- `HookResult.error` 在 `success=false` 时必须非空，作为单元不变量。

**xlings：**

- installer 不再用空 message 构造失败 status；
- terminal 显示 bounded tail；
- NDJSON stdout 只输出协议帧，hook output 转成 `LogEvent`；
- 完整日志路径可选，但不存在日志文件时不能给假路径；
- 同一个失败在 CLI、CI、interface、mcpp 中必须有相同 code/message。

验收 fixture 覆盖：`return false`、Lua exception、stdout、stderr、`log.error`、超长输出、非 UTF-8、NDJSON。

### C2. #514：shared registry dependency 必须来自显式解析上下文

不把 `$MCPP_HOME/registry/data/xpkgs` 写进 xlings 或 libxpkg 的 fallback 列表。

现有 `ExecutionContext.resolved_deps` 已能记录 xlings 自身 runtime dependency graph 中的：

```text
canonical name + exact version + install_dir + libdirs + source
```

但 mcpp 的 `mcpp.deps` 是宿主在调用 xlings 前完成的独立依赖图，不一定出现在 xlings 的 `node.runtime_deps` 中，因此不能把这张 map 宣称为所有宿主依赖域的 total map。

修复：

- `resolved_deps` 中存在 exact record 时始终优先；record 指向缺失 payload 时明确失败，不扫描另一个版本；
- `ExecutionContext` 增加宿主显式提供的、有序去重的 `dependency_store_roots`；xlings 只从当前/project/global data roots 构造 `<data>/xpkgs`，不读取 `$MCPP_HOME`；
- record 缺失时，libxpkg 只在这些显式 roots 中按 exact namespace/version 查找，绝不按目录顺序或 bare-name 猜测；
- explicit roots 仍找不到时，现代 context 明确失败；只有没有新字段的老 host 才进入带 warning 的 legacy heuristic；
- xlings runtime deps 继续要求 resolver record 完整；不能因为 mcpp 的独立 dependency domain 合法缺 record 而放宽 xlings 自己的 graph；
- mcpp 不实现第二套路径推断；它只通过现有 `XLINGS_HOME=<registry>` 与 `XLINGS_PROJECT_DIR=<member>/.mcpp` 拓扑接受集成验证。

conformance fixture：current store、shared store、project store、多版本、namespace collision、同 bare-name 外 namespace、record 指向损坏 payload、explicit roots 之外的诱饵目录。

### C3. #506：package 没注册 target 仍必须可移除

本轮不建立完整 PackageInstance 数据库，使用已有 provider ownership 和 payload stamp 做精确修复。

当前错误谓词问：

```text
这个 target 有没有任何版本？
```

正确问题是：

```text
执行当前 uninstall 的 provider 是否注册过属于自己的版本？
```

修复：

- 在 exact removal selection 前检查 executing provider 的 owned binding groups；
- target 上只有其他 provider 的版本，等价于本 provider 零版本；
- 仍然执行本 package 的 `uninstall()`；
- 只释放本 provider 拥有的 binding/effect；
- dependency payload 是否删除仍由现有 reverse-dependency/ref 规则决定；
- dependency-free config/script package 也必须运行 uninstall；
- 不伪造 anchor target。

验收：

- Windows gcc -> mingw-w64 delegation；
- Linux config/script 零 target；
- foreign provider 同名 target；
- uninstall hook marker；
- 其他 provider shim/workspace 不变；
- 删除 xim-pkgindex 中对应 tolerance，以 tolerance 消失作为通过证据。

## 7. 工作流 D：`xlings subos use`

### D1. 无参数是发现操作，不是输入错误

```text
$ xlings subos use

Available SubOS environments:
  default  *
  dev
  legacy

Use: xlings subos use <name>
```

契约：

- 输出与 `xlings subos list` 同一候选集合；
- 无任何状态修改；
- 不启动 shell；
- 不访问网络；
- 返回 0；
- 空集合提示 `xlings subos new <name>`。

需要同时修改：

- CLI spec：`name` 从 required 变 optional；
- SubOS run parser：空 name 路由到 candidate display，不再第二次报错。

### D2. 一个候选源

当前 `subos list` 可以合成 `default`，但 `subos use` 只信 home registry，所以同一个 home 会得到两个答案。

引入局部的 `SubosCandidateView`：

- canonical source 是 home `.xlings.json` 的 `subos` registry；
- 兼容旧 home 时，只允许 bounded 的 synthesized `default`：`subos/default/.xlings.json` 存在且 registry 缺它；
- list、use validation、fuzzy resolve、interface capability 都消费同一 view；
- 只读命令不偷偷写 registry；
- 下一次明确写操作或 `self migrate` 补登记；
- 不扫描任意目录并把残留目录自动认成 SubOS。

### D3. 安全模糊匹配

建议顺序：

1. case-sensitive exact；
2. case-insensitive exact（若多个则 ambiguous）；
3. unique case-insensitive prefix；
4. substring/edit-distance 只生成建议，不自动进入。

结果：

| 结果 | 行为 | exit |
|---|---|---:|
| exact | 进入 | shell/command exit |
| unique prefix | 显示 resolved name 后进入 | shell/command exit |
| multiple | 列候选，不执行 | 2 |
| none | NotFound + 最相近建议，不执行 | 1 |
| no name | 列全部，不执行 | 0 |

所有 `--global/--shell/--sandbox/--cmd/--gpu` 模式先完成同一 name resolution，再分发；模糊匹配不得因模式不同产生不同结果。

### D4. 自动化接口

CLI 人类输出之外，DataEvent 保持结构化：

```text
subos_candidates {
  reason: missing_name | ambiguous | not_found,
  query,
  candidates: [{name, active, dir}],
  auto_selected
}
```

无参数和歧义都不能让 Agent 猜测后进入环境。

## 8. 交付拆分与依赖图

用户要求“尽量单 PR”。一个 GitHub PR 不能跨 repository，因此边界是：每个代码仓尽量一个集成 PR，PR 内保留按 bug 拆分的 additive commits 和独立 test gates，最终按仓库规则 squash 合入。只有 package index 的两次 release pointer 更新存在天然时序依赖，允许拆成两个极小跟进 PR。

```text
libxpkg PR (#513 capture + #514 explicit store roots)
  -> libxpkg 0.0.55 release
  -> xlings integrated PR
       query/info/list + quick/deep doctor
       bidirectional loader/libc + SubOS runtime equality
       #513/#514 host integration
       #506 provider-aware removal
       subos use discovery/fuzzy
  -> xlings 2026.8.9.3 release
  -> xim-pkgindex pointer PR A + GLOBAL/CN verification
  -> mcpp PR (#392 exact owner fixup + target env + real #514 integration)
  -> mcpp 2026.8.9.1 release
  -> xim-pkgindex ecosystem PR B
       mcpp pointer + remove #506 Windows tolerance
       named-SubOS graphics verifier + NVIDIA closure reporting
       native lifecycle matrix
  -> public cold-home / old-home / sandbox ecosystem audit
```

上述版本号是当前远端 head 不再前进时的预期值；每次 cut 前重新读取 release/tag 与 index，普通日期版本的 `N` 从 1 开始，绝不把 `.0` 当常规发布。

### Repo PR-L：openxlings/libxpkg

- #513：保证失败 error 非空，捕获 bounded Lua `print/io.write/io.stderr/log` transcript，禁止 process-global fd redirect；
- #514：`ExecutionContext.dependency_store_roots`，exact record first，explicit roots second，legacy-only heuristic last；
- unit contract 与并发/截断/namespace fixtures；
- CI 终态后 squash merge，发布 `0.0.55`（若版本仍可用）。

### Repo PR-X：openxlings/xlings 单一稳定性集成 PR

- A：catalog no-sync policy、targeted info、filter-first list、quick/deep doctor、无 recursive child CLI；
- B：双向 loader/libc guard、manifest/workspace runtime equality；
- C：消费 libxpkg 新 contract，修 #506 provider-owned/zero-target removal；
- D：`subos use` no-arg/shared candidates/safe unique prefix/interface events；
- 同一分支以小 commits 和 focused gates推进，最后 full unit/e2e + Linux/macOS/Windows/aarch64 CI；
- squash merge，发布 `2026.8.9.3`（若仍是当天下一普通版本），立即完成 GitHub assets、GitCode top-up 与 index pointer A。

### Repo PR-M：mcpp-community/mcpp

- 删除 post-install directory-first glibc；shared compiler fixup 使用 owner/default SubOS exact runtime；project target 使用其自身 SubOS exact runtime；
- target `LD_LIBRARY_PATH` 保留非 core runtime dirs，删除所有实际含 libc/loader 的目录；
- 两份 glibc、两个 sibling SubOS、一个 shared compiler 的交替 clean build/run；
- #514 只增加真实 shared-registry integration，不在 mcpp 重复实现 store 推断；
- CI 终态后 squash merge，发布 `2026.8.9.1`（若仍是当天首个普通版本）。

### Repo PR-I：openxlings/xim-pkgindex 生态收口

- pointer A 在 xlings release 后立即更新 xlings 四平台 hash/latest，避免发布不可安装；
- pointer B/生态 PR 在 mcpp release 后更新 mcpp，删除 #506 Windows tolerance；
- 修 named-SubOS graphics verifier 和 NVIDIA unresolved closure reporting；
- `xlings subos use <name> --sandbox --cmd` 的 llvmpipe/Wayland/host-child gate；有设备时再加 `--gpu` native cell；
- build/publish index artifacts，校验 GLOBAL/CN GET 与 sha256，跑 native platform lifecycle。

每个 PR body 写清上游 release 和下游 consumer 的 exact ref；不得先 bump 未发布依赖，也不得把 source checkout 通过写成 public ecosystem 通过。

## 9. 验证矩阵

### 9.1 隔离原则

所有 stateful 测试使用临时：

```text
HOME
XLINGS_HOME
MCPP_HOME
project root
SubOS root
```

测试前后 snapshot 宿主 xlings 状态；不得对真实 home 执行 install/use/remove/migrate。

### 9.2 三层 gate

1. **结构 gate**：禁止 payload walk/child xlings/network；
2. **语义 gate**：现有 contract fixtures 全保留；
3. **真实 gate**：长期 home、fresh home、old home、native platform、真实硬件。

### 9.3 三平台

- Linux：完整 e2e + long-home + ELF/runtime/graphics；
- macOS：native CLI/query/doctor、Mach-O 安装错误输出、shared registry #514；
- Windows：native CLI/query/doctor、gcc delegation #506、SubOS fuzzy/use；
- aarch64：至少 query/subos/doctor 和 release binary smoke；
- GPU/WSL2：只能由具备对应设备的 runner/实机给出结论。

### 9.4 发布门禁

发布前必须有：

- 当前 source binary；
- release/static candidate；
- fresh install 获得的公开 latest；
- 当前长期 home 的只读 benchmark；
- old-home read-only compatibility；
- xim-pkgindex latest 和资源镜像一致性；
- mcpp #392 exact scenario；
- GitHub Actions Linux/macOS/Windows/aarch64 终态。

workflow running/cancelled/superseded 不算通过。

## 10. 回滚与兼容

本方案避免状态格式大迁移，所以回滚面有限：

- query reader 改动失败时可回滚二进制，不需要回滚 home；
- cache format bump 可丢弃重建，cache 不是权威；
- deep doctor 不改变状态；
- runtime default 只影响新 SubOS；
- loader/libc hard fail 会拒绝新坏 payload，不修改旧 payload；
- #506 只放宽“本 provider 零注册”的合法路径，foreign provider 仍受保护；
- fuzzy match 在执行前可确定，无半进入状态。

## 11. 完成定义

四条工作流都必须完成，不能用“架构 issue 已建立”替代 bugfix：

- A：三个已知慢命令恢复，并完成全只读命令结构审计；
- B：双向 core-runtime 守卫 + #392 真实闭环 + 图形支持矩阵诚实落盘；
- C：#513/#514/#506 的原始 repro 全绿，workaround/tolerance 删除；
- D：`subos use` 无参数/精确/唯一前缀/歧义/无匹配/legacy default 全部有测试。

没有 native 或硬件证据的格子必须留作未验证，不能把 source review 作为完成。

## 12. 本轮 review 点

### R1. 查询修复范围

是否确认：修复覆盖全部本地只读命令；三个已知命令是 release blockers，不是唯一审计对象。

### R2. doctor 入口

是否确认：默认恢复 quick doctor；现有全 payload ELF 审计保留为显式 `self doctor --deep`。

### R3. runtime hard fail

是否确认：`host loader + xlings libc` 与 `xlings loader A + xlings libc B` 都是确定错误，安装必须 hard fail。

### R4. #514 兼容边界

已按当前事实修正：`resolved_deps` 只对 xlings 自己的 runtime dependency graph 保持 total；mcpp 的 `mcpp.deps` 属于独立依赖域。exact record 存在时必须权威且损坏即 fail；record 缺失时只查 host 显式提供的 ordered store roots；现代 context 在 explicit roots 仍无 exact namespace/version 后 fail；只有没有新字段的老 context 才允许带 warning 的 heuristic fallback。绝不扫描 `$MCPP_HOME`。

### R5. `subos use` 模糊匹配

是否确认：只自动接受 exact 和 unique prefix；substring/edit-distance 只建议，避免误入环境。

### R6. D6

当前不引入 SQLite。若未来数据规模需要更强 control plane，另开 ADR；本轮不以数据库为任何 bugfix 前置。

## 13. 相关问题与材料

- [xlings #506](https://github.com/openxlings/xlings/issues/506)
- [xlings #513](https://github.com/openxlings/xlings/issues/513)
- [xlings #514](https://github.com/openxlings/xlings/issues/514)
- [xlings #518 — userspace OS architecture discussion](https://github.com/openxlings/xlings/issues/518)
- [mcpp #392](https://github.com/mcpp-community/mcpp/issues/392)
- `2026-08-09-ecosystem-closure-design.md`
- `2026-08-08-mesa-rebuild-iris-d3d12-wayland-design.md`
