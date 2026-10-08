# PR #641 总体进度报告

评估日期：2026-10-08。评估对象：[xlings PR #641](https://github.com/openxlings/xlings/pull/641)，以及 SubOS 总体架构设计 Part 1、Part 2。本文结合设计、最终代码、测试声明、远端 CI 和 rebase 后本地验证；实施记录中的“本地实测”属于原作者的记录，不等同于本轮复验。

## 1. 结论

**两阶段主要代码已落地，当前处于集成与验收收口阶段，还不满足合并、发布条件。**

Part 1 已形成声明策略 → spec 编译 → provider → supervisor/session → broker → 审计的完整主链。Part 2 已加入根投影、generation、部署 R、启动项、导出以及 Luban 分级模板，范围已经明显超过 PR 标题和描述。

现在的主要工作是修复已有实现的边界、同步设计和实际能力，并取得真实环境测试的通过证据。尤其需要处理 `subos pack` 删除现有目录、Linux 删除 lint、macOS generation 原子可见性失败、locked 审计失败语义及 rootfs 能力报告不一致。

不能用“123 required 都有测试声明”推导出“全部功能验收完成”。本轮普通 Linux 测试通过，但真实沙箱用例跳过；当前远端 Linux 主车道在构建前失败，新增加的发行版验收车道尚未运行。

## 2. 本地分支与 rebase

| 项目 | 结果 |
|---|---|
| 原远端分支 | `feat/subos-architecture-640` |
| PR 原 head | `4df5c19ca525082eeeacf4d34ae468c95c52e619` |
| 获取方式 | `git fetch origin main refs/pull/641/head:refs/remotes/origin/pr-641` |
| 本地工作分支 | `review/pr-641-rebased` |
| 独立 worktree | `/home/speak/workspace/github/openxlings/xlings-pr641` |
| rebase 基线 | `origin/main`：`c55d89aa43ed4e048964af3791365d7f6532f13d`，#647 |
| rebase 后 head | `a8c2aba1834737cad4b088ef2e6500bd05115248` |
| 原 head 备份 | `backup/pr-641-before-rebase` |
| rebase | 成功，无需人工解决冲突；main 是新 head 的祖先 |
| 提交数量 | PR API 返回 75 个提交；rebase 后相对 main 为 74 个非 merge 提交 |
| 当前规模 | 相对 main：206 个文件，+21,320 / -3,115 行，包含重命名和模块迁移 |

原工作目录处于 `perf/shim-dispatch`，有 `.xlings.json` 删除及两个未跟踪文件；本次使用独立 worktree，保留这些变动。未推送、未修改远端 PR、未提交产品代码修复。

main 的 macOS LLVM 23.1.3 调整（#645）和 recipe 按客户端进程架构解析的修复（#647）已进入本地分支。`range-diff` 中移除的是已在 main 的 #643 shell completion 提交；另一个差异是 #647 对测试 import 上下文的更新。版本继承 main 的 `2026.10.8.1`，不是对本 PR 的独立发版。

## 3. Part 1：安全隔离与管理架构

“已实现”在下表指代码和对应测试已存在；真实环境验收情况另见第 6 节。

| 设计范围 | 当前实现 | 进度判断 |
|---|---|---|
| C0，C1–C4：模块拆分、确认、事件、HomeView/Ports、名称模型 | SubOS 独立包；公共 guard/observe 最终合入 `modules/runtime`；CLI 适配拆到 configure/run/audit 等单元 | 主体完成，最终目录与初稿不同 |
| C5–C6：HomeContext、mode/layout、写者规则 | mode/layout 声明、高 layout 限制写入、home 配置损坏拒绝覆盖、未知键保留 | 主体完成；Part 2 新读写者仍有缺口 |
| C7–C8、C10–C11：spec/provider 与 S0 修复 | home 只读、实例可写、其他实例与审计隐藏、环境白名单、pid/ipc/uts、终端防注入 | 主体完成；本机无法复验真实 bwrap |
| C9、C12：会话和外部执行 | supervisor/session-init、Unix socket 与 fd 传递、exec/start/stop/cp/temp、退出码、keep/ttl | Linux 主链已实现；跨平台 detached session 不支持 |
| C13、T4：agent 契约 | `XLINGS_AGENT_MODE`/`--agent`，PTY 无输入扫描，候选与非阻塞错误 | 已实现，相关本地契约通过 |
| C14、C16：策略 | dev/private/locked、外置 policy、未知安全字段拒绝、decide、规则、策略包锁定和升级 | 已实现；策略包联网用例本轮跳过 |
| C15：broker 权限 | 客户端和服务端判定、auto/ask/deny、审批、owner-only、broker 默认拒绝未知请求 | 主体完成；hook 沙箱明确未实现 |
| C17、C21、C25：能力、诊断与性能 | caps 缓存、must/should、降级提示、doctor/isolation fix、shim 与热会话性能测试 | 主体完成；完整环境矩阵与原预算未全部验收 |
| C18–C20：Landlock、挂载、授权、身份 | Landlock 显式选择；具名授权；private/locked 中性身份与 nested userns 限制 | 已实现；Landlock 为写限制，不提供文件隐藏 |
| C22：观测 | supervisor 写 events.ndjson，exec 通知、会话/权限事件、结束时统计 rw 映射文件变化、log/report/trace | 部分完成；审计失败语义与逐连接 net 事件未闭合 |
| C23：网络 | none、pasta NAT、publish、host-loopback | none/NAT 已实现；proxy 延后 |
| C24、C26–C27：layer、系统部署、rootfs | 已由 Part 2 接续，不能继续按 Part 1“全部延后”描述 | 见下一节 |

隔离的实际强度仍需按档位和平台说明：dev 默认保留宿主网络，private/locked 的网络隔离才隐藏宿主抽象 socket；macOS/Windows 仍是 home-redirect，未实现 Linux 等强度的文件、进程、网络边界。

## 4. Part 2：宿主即 SubOS、根呈现与 Luban

Part 2 §19 已有 C28–C42 的实施记录，代码也有对应入口；但 checkpoint 有实施记录并不表示其完成标准“CI 全绿”已经满足。

| checkpoint | 当前落地 | 尚需确认 / 差异 |
|---|---|---|
| C28 S，C29 AUR | 系统二进制为新用户 home 建立 entry；AUR 仅装系统本体，去掉 root uninstall hook；增加两用户包测试 | isolation-fix、arch-package 当前远端均跳过 |
| C30/C30a：前缀域、bootstrap | single/multi 路径模型；缺 patchelf 先加入安装计划，仍不可用则失败；简单 alias 不经 shell | `--domain` 构建器/用户私有系统域未做，multi 直接在 `/xlings` 构建；是范围简化 |
| C31：kind/role | view/rootfs、host/boot_entry，允许操作统一表 | 损坏 instance.json 被解释为 view，读者规则需收口 |
| C32–C33：根投影与 generation | merged-usr、直接链接、generation 树、rename 指针、rollback/prune、factory etc/sysusers | macOS 原子可见性测试失败；投影更新失败目前只警告 |
| C34：库搜索 | `/lib64` 接到根的 `/usr/lib`，支持 host-built ELF | 根自己的 ld.so.cache 推迟，需 glibc revision；gcc 编译的程序仍需显式 `/usr/lib` rpath |
| C35：rootfs 实例 | bwrap 根呈现、uid 0、嵌套 default、broker 安装后刷新 `/usr` | store 整体只读可见，未按闭包逐 payload 绑定；gate 仍报告 RootfsRuntime 不支持 |
| C36 M/layer | `install --system`，用户 PATH 顺序为用户层 → 系统层 → 宿主；rootfs 支持 fetch=layer，普通 view 拒绝 | 属于 PATH 跨层使用，不是把系统层条目合入用户 VersionDB；需求文本已缩窄 |
| C37：导出、pack、diff | rootfs 目录、tar、disk，tar 作为 docker/WSL 导入输入 | OCI/WSL 专有开关合并为 tar；pack 现有目录删除问题已复现 |
| C38 R | root 中切换 xlings 保留 stage-0 entry，运行版本随 `/usr` generation 移动 | 实际 root 中 self update/rollback 需要 distro 验收 |
| C39、C42：启动 | stage-0、boot.json、once/tries/fallback/mark-good，busybox init 的 `--now` | qemu 启动和不重启内核切换车道尚无当前 head 的通过记录 |
| C40 Luban | tiny/core/desktop、from 链、基础工具、内核与离屏渲染场景 | 跨仓库模板已合并；当前 xlings head 的联验未完成 |
| C41：发行版 CI | distro 的 bwrap/docker/qemu 场景、system_layer、AUR、WSL 报告 | YAML 和脚本已接线，但当前 distro 等跳过，WSL 实际也 skip |

Part 2 里程碑 A 要求 tiny 从空根构建、进入、内部装包并通过闭包检查。原作者记录称这些本地实测已完成，还包括 core 144 个形态 X ELF 的闭包和 desktop 离屏像素验证。本轮未重新运行这些场景；当前 head 的 distro CI 未启动，因此里程碑 A 应标为“有作者本地证据，CI 验收待完成”。

跨仓库前置：[xim-pkgindex #929](https://github.com/openxlings/xim-pkgindex/pull/929) 已在 `2026-10-06T06:33:24Z` 合并，head 为 `0ac7cb2b292218362a17c5c15c58e685ecce7397`。PR 包含 Luban 模板、bash/coreutils、内核、busybox applet、perl shebang、binutils wrapper 修复；其主要检查通过。资源发布和所有平台可下载性仍应由发行版场景验证，不能由索引 PR 合并状态代替。

## 5. 测试与开发基础设施完成度

T1/T2/T4/T5 的主要链路已经存在：C++ XTEST 元数据、隔离 home、进程 runner、能力声明、统一报告、脚本适配、需求覆盖门禁。T3 有实质推进：Linux release 产物复用于 e2e，asan 离开普通 PR 关键路径，多个车道集中报告。

但 Part 1 §24 的完整 CI 方案未全部实现：

- `xdev ci plan` 只出现在 usage；`main()` 没有执行分支，调用会返回 2。
- 未见设计中的动态分片、按 area 选择、资源调度、趋势、quarantine/flaky、代码覆盖率门槛完整实现。`resources` 元数据存在，不能视为调度器已完成。
- Linux root workflow 仍独立构建，dev/unit 与 release 也有不同构建；“每平台每 commit 只构建一次”不能按字面宣称完成。
- testkit 的 isolated home 仍写镜像配置，未实现设计中的进程内 fixture HTTP server；distro 明确声明 network，并安装真实包。无网络 PR 的目标尚未完成。
- Part 2 新增了 shell 场景，通过 `# xtest:` 接入报告；这是对“新增测试全部 C++”的实际偏离，实施记录已说明，但总体设计应同步。
- T6 的 `xdev lint/release` 子命令未实现；现有 lint 通过 `xdev test --suite lint` 适配。T6 原本允许放在 PR 后。
- Part 2 的 generation/stage-0 性能预算未见对应完整断言；现有 perf 主要验证 shim 开销和热加入比冷启动快。

## 6. 验证证据

### 6.1 rebase 后本地

工具：mcpp `2026.10.5.3`，gcc `16.1.0`；产物为 Linux GNU 开发版，非静态 release 包。

| 验证 | 本轮结果 |
|---|---|
| `mcpp build` | 通过，约 32.65 s |
| `mcpp build -p xdev` | 串行构建通过，约 4.49 s |
| 首轮 `XLINGS_TEST_MIRROR=CN mcpp test` | 75 个测试二进制通过，2 失败 |
| 首轮失败归因 | 既有 ProgressOutput 用例未隔离 TERM=dumb；新增 Deployment 用例中测试镜像变量覆盖 GLOBAL 断言 |
| 清理环境后 `xdev test` | **77 个二进制通过，0 失败；XTEST 69 pass / 0 fail / 36 skip** |
| XTEST 跳过原因 | sandbox 33、macOS 1、Windows 1、network 1 |
| `xdev test --no-mcpp --suite lint` | 2 pass / 1 fail；SubOS 删除 lint 报 7 条违规，含重复归类 |
| `xdev test --no-mcpp --suite contract-scripts` | 9 pass / 1 fail；文档示例检查把行尾 `wsl --import` 注释识别为 xlings 参数 |
| 其余契约 | CLI spec parity、generated command reference、i18n、版本、头文件等检查通过 |
| 本地 `report --requirements --fail-uncovered` | 123 required：97 covered / 26 uncovered，0 planned，3 deferred，0 unknown；退出 1 |
| 全仓静态声明核查 | 123 required 均有 C++、场景脚本或 WSL workflow 声明；未发现未知 ID |
| `git diff --check origin/main` | 失败，设计文档和 subos.cpp 有少量空白格式问题 |

本机 `xdev doctor` 显示 userns/bwrap/sandbox 不可用，pasta 未安装，也没有免交互 sudo。因此 77 个二进制通过只说明可执行的本地检查通过，**不证明 bwrap/NAT/rootfs/启动隔离已复验**。本次没有改变主机 userns/AppArmor 设置，也未运行修改 `/xlings` 的系统场景。

本地覆盖的 97 项是 xdev 读取到的测试元数据覆盖，包含已 skip 的用例声明；26 项未覆盖主要是部署、rootfs、镜像、启动及 Luban 场景，本轮没有执行其脚本。`apps/xdev/src/main.cpp:413` 起的覆盖计算遍历 meta，不与 pass 状态关联。门禁有助于防止漏写测试，但不等于行为验证通过。

### 6.2 远端：原 PR head 的最近检查

以下状态对应原 head `4df5c19`，不能转移为 rebase 后 `a8c2aba` 的 CI 结论。PR 仍为 OPEN、Draft，API 中无正式 review。

| 车道 | 状态 | 含义 |
|---|---|---|
| [Linux build-and-test](https://github.com/openxlings/xlings/actions/runs/37423886275/job/112139141966) | FAILURE | 删除 lint 先失败，后续 build/unit/release 尚未执行 |
| [macOS build-and-test](https://github.com/openxlings/xlings/actions/runs/37423886270/job/112139098051) | FAILURE | 76 个测试二进制 pass，1 fail；generation 并发读者测试记录 11 次“没有有效代” |
| [Windows](https://github.com/openxlings/xlings/actions/runs/37423886304) | SUCCESS | 当前远端 Windows 车道通过 |
| [aarch64](https://github.com/openxlings/xlings/actions/runs/37423886323) | SUCCESS | cross-build/qemu 与 native compatibility 均通过 |
| [Linux root](https://github.com/openxlings/xlings/actions/runs/37423886395) | SUCCESS | 现有 root workflow 通过，不是新增 distro 验收 |
| [unit-asan](https://github.com/openxlings/xlings/actions/runs/37423886275/job/112139142373) | SUCCESS | 本 PR 带 ci:asan；该 job 未声明真实隔离能力 |
| e2e / isolation-fix / arch-package / distro | SKIPPED | 依赖失败的 Linux build-and-test |
| [wsl-import](https://github.com/openxlings/xlings/actions/runs/37423886275/job/112139184538) | SUCCESS，实际用例 SKIP | 下载 artifact 后确认：`WSL1 could not be set up on this runner` |
| [Linux report](https://github.com/openxlings/xlings/actions/runs/37423886275/job/112139242765) | FAILURE | 汇总失败；不是发行版需求覆盖通过证据 |

总计：7 SUCCESS、3 FAILURE、4 SKIPPED。WSL 的 green 是尽力车道设计所允许的结果，不能计为导入成功。

## 7. 需要优先收口的问题

### P1：pack 删除了调用者预先存在的目录（已复现）

`src/core/subos/root_cmd.cpp:676` 的 pack staging 使用 `<out>/<pkg>-<version>`，无存在性/所有权检查就 `remove_all`。在临时目录预建 `out/review-1/user-owned.txt`，执行：

```sh
xlings subos pack probe --as demo:review@1 --out <temporary>/out
```

命令退出 0，预先存在的文件被删除；没有确认过程。export 的固定 `.name.stage` 也有同类实现，需要一并审查。本轮复现全部在临时 home 和临时输出目录进行。

应使用唯一且由本次操作创建的 staging 目录，清理只作用于已证明所有权的路径；现有输出冲突应明确拒绝或采用授权后的替换语义。**不要通过补 lint 豁免掩盖此问题。** generation/prune 是派生数据，可以在证明其边界后标记，但不能与任意输出目录等同处理。

### P1：generation 的验收与失败处理未闭合

远端 macOS 的 `SubosRootfs.AGenerationSwitchIsOneRenameAndARollbackMovesThePointer` 失败，本轮 Linux 同一测试通过。当前证据还不足以判定 macOS 文件系统行为、测试读取方式或实现哪一侧是根因，需要保留该平台上的测试结论，不能直接降为“偶发”。

另一个代码问题是 `src/core/xself/init.cpp:691` 处 root refresh 失败只 `log::warn`。这条路径不能把投影失败传回安装/切换结果，可能使 workspace 与 `/usr` generation 不一致。需要错误传播及失败后的状态一致性验证。

### P1：locked 审计失败没有执行设计规定的终止语义（代码确认）

Part 1 §22 明确要求：审计写入失败时 locked 结束会话，其他档位警告。实际 `modules/runtime/src/observe.cpp:70` 的 append 返回 void、忽略打不开文件及写失败；`modules/subos/src/session.cpp:94` 的 audit 无法获知结果，未实现 locked 分支。

需要可检测的 journal 写入结果及会话层的处理，并补审计不可写/磁盘满的真实失败用例。本轮由于 bwrap 不可用，未把静态判断冒充运行复现。

### P2：新增 kind/boot 状态读写未全部遵循 C6 规则

`src/core/subos/root.cpp:21` 将损坏 JSON 变为 `{}`，`kind_of` 再默认 view；本轮把临时实例 instance.json 写坏，`subos status --json` 仍退出 0、显示 kind=view。`declare_kind` 复用该读者，存在覆盖损坏文件的实现路径。

`modules/subos/src/boot.cpp:46` 的 save 从 Config 重建 JSON，不保留未知字段。应区分缺失与不可读/损坏，复用严格更新读取并保留扩展键。

### P2：rootfs 功能、能力报告和覆盖清单矛盾

`modules/subos/src/gates.cpp:52` 仍将 Linux RootfsRuntime 固定报告为 `supported=false / not in this release`；`tests/requirements.toml` 中 ISO-ROOTFS 仍 deferred，而 INST-ROOTFS 已 required；PR 描述及旧计划仍称 C27 rootfs、fetch=layer 和系统层解析延后。

应以 Part 2 最终实现更新这些回答者，说明 layer 的 rootfs 限制、PATH 跨层语义、未做闭包绑定，而不是只修改摘要文字。

### P2：新增场景与发布链尚缺当前版本验收

Linux lint 阻断了新增 distro、AUR 和 isolation-fix；本地文档契约还新增一个失败。先修这两个门禁，再运行静态 release 包对应的 rootfs_instance/image/boot、system_layer、AUR、isolation doctor fix。`--tar` 的 WSL2 验收仍需合适 runner，现有 WSL1 尽力报告可保留。

## 8. 明确保留的范围限制

以下不应隐藏在“全部完成”里；其中若要随本 PR 发布，需维护者接受并在用户文档说明：

- net=proxy；broker 安装的 hook 沙箱；interface subos_exec 实时流式输出：Part 1 实施记录明确延后。
- root 的 ld.so.cache/glibc sysconfdir revision：Part 2 明确延后；`/lib64 = /usr/lib` 提供当前替代搜索路径。
- rootfs store 的闭包级隐藏：未做；整体 store 只读可见。ROOT-STORE-CLOSURE 没有成为现行 required ID。
- macOS/Windows 原生强隔离及 rootfs runtime：未做；通过 home-redirect/导出 Linux tar 路线提供当前能力。
- private/locked 下的网络/权限限制应按实际 provider 判定；dev 及 proot/home-redirect 的降级不能用“默认强隔离”概括。
- 按连接的 net 观测、min_client 老 entry 诊断、完整性能/环境/故障矩阵及 xdev 动态 CI/趋势工具：与总体设计相比尚未完整落地，也未全部纳入 required 清单。

## 9. 建议的后续顺序

1. 先修 pack/export staging 的所有权边界，逐项审查删除 lint；同时修文档示例的行尾注释识别问题。
2. 收口 generation 的 macOS 失败、投影失败传播、kind/boot 严格读写、locked 审计失败语义。
3. 更新 RootfsRuntime probe、ISO-ROOTFS 状态、PR 标题/描述、Part 1 实施计划；给 T3/T6、hook/proxy/cache/closure 等范围明确标记。
4. 对 rebase 后 head 重新取得 Linux/macOS/Windows CI，重点跑新增 distro、AUR、system layer 和 isolation-fix；检查每项为 pass 而不是只看 job green。
5. 根据这些结果决定是否进入 ready-for-review；发布再按实际发布日期编号，走 release、CN mirror、索引更新和真实冷 home 升级验证。

## 10. 本轮证据位置

- 统一 C++ 测试和需求报告：`target/xdev/pr641-review/report.md`、`report.json`。
- lint：`target/xdev/pr641-review-lint/report.md`。
- 契约脚本：`target/xdev/pr641-review-contracts/report.md`。
- 构建、首轮测试、远端 macOS日志、原 PR JSON、range-diff：`target/xdev/pr641-review/evidence/`。
- 复现现场：`/tmp/pr641-review-9ofjuh0h`（只用于本轮隔离复现）。

本轮执行了分支获取、rebase、分析、构建与验证，新增本文；没有把检查失败自动修成通过，也没有 push 或修改远端 PR。


## 9. 续行实施状态（2026-10-08）

本报告前述结果是初次评估的快照。续行依赖计划见
[完整交付计划](../plans/2026-10-08-subos-delivery-plan.md)，任务状态见
[交付状态](../tasks/subos_delivery_641/README.md)。

已经实现：独占 staging 和不覆盖发布；generation 完整清单校验；严格 kind/role/boot/anchor/init
读取与未知键保留；stage-0 错误恢复；root refresh 失败传播；locked 审计写失败拒绝/终止与
先审计后确认 exec；默认 argv 值脱敏；policy min_client 与实际 entry doctor；dangling policy
拒绝；随机独占 atomic writer 与 Windows 替换。修复了文档行尾注释解析与 TERM/mirror 测试污染。

产品 build 通过。一次完整本地运行产生 84 个测试程序记录，82 通过、2 失败；针对性修复后
两类失败用例单独通过，逐程序最新证据为 84 pass。具体用例仍有 42 个 skip（包括真实沙箱、
平台、network 与尚未通过 xdev 调用的报告用例），没有将这些声明转成实际验证。
原始运行、重跑和合并后的 `latest-binaries.ndjson` 均保存在 `target/xdev/pr641-hardening/`。

xdev 的报告已区分声明覆盖与执行证据，并用同车道关联防止另一平台的通过覆盖本车道的 skip。
新的 CI 选择实现正在进行。libxpkg 的独立工作树已实现持久外置 hook 执行接口，尚未集成客户端；
元数据加载及 index build 的 Lua 顶层执行也必须纳入边界，不以只拦 system.exec 宣称隔离完成。
xim-pkgindex 使用独立工作树，原工作树用户改动保留。真实 home 的镜像已设置为 CN。

这些工作尚未改变最终验收结论：完整 CI、proxy/连接事件、系统层/闭包、hook worker、库 cache、
fixture/资源调度、升级/性能证据与发布链仍需完成。候选版本为 2026.10.8.2，正式发布时按当天
已有 release 重新确定；没有发布 #641 的资源或更新 latest。
