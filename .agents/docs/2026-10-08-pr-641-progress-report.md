# PR #641 总体进度报告

评估日期：2026-10-08。评估对象：[xlings PR #641](https://github.com/openxlings/xlings/pull/641)，以及 SubOS 总体架构设计 Part 1、Part 2。§1–10 保存最初 rebase 后的审查基线，§11 起记录后续实施与对应提交的验证。本文结合设计、代码、测试声明、远端 CI 和本地验证；实施记录中的“本地实测”属于原作者的记录，不等同于本轮复验。

## 当前交付状态（Linux 后续验收修复待最新 CI）

后续实现均追加 commit、普通 push，保留审查历史。main 仍为 `c55d89a`，
候选版本为 `2026.10.8.2`，尚未合并、发布。 最新固定head的结果见§26；以下保留历史轮次证据。

`8e8638fc` 的 macOS（147 pass、0 fail、2 skip）、Windows（105/0/44）、
Linux root 完整流水线通过。ARM64 首次 qemu version 检查退出 139，同一 head
重跑交叉构建和原生兼容均通过；原因未确定，补充失败 trace 与 archive 保存，
不放宽原始退出码和候选版本匹配。Linux 主单元为 202 pass、1 fail、5 skip，
ASAN 仍运行；静态 domain、性能和发行版下游因单元失败尚未执行。

唯一 Linux 失败是新 payloadless config remove 回归。artifact 的 uninstall log
确认 hook 已运行一次，但 worker publication 无条件移动从未存在的旧 payload，
随后 fallback 合成不存在的 XVM remove 又遮蔽该错误。最小修复使 missing old +
empty shadow 不制造空 payload，非空首次发布仍用 no-replace；unknown/nonregular
目标拒绝，已有目录保留事务。fallback 仅对 validated selection 合成 remove。
真实 hook 失败继续非零；dev 与 legacy 两路都校验显式/bare hook once、错误版本
拒绝且旧日志不变、真实 hook 错误不吞掉。正在有限本地复验，最新 head 全 CI 待完成。

最终只读审查的 anchored FD `/etc`/sysusers 写入修复已在 Linux/macOS 实际通过，
外部 user data、symlink/hardlink 与 rename 后目录边界均有回归。静态 release 清理
后的 xdev 与四个已构建验收程序保留修复已落地，完整静态实际 pass 门禁尚待执行。
不能用其他 head 或平台的 green 替代这些验收。

`1019d353` 的 macOS、Windows、ARM64 和 Linux root 完整 CI 已实际通过；
Linux 单元为 198 pass、0 fail、5 skip，静态 release 和 candidate cold-home 通过。
静态 domain 硬验收启动前因 release 清理 musl target 同时删除 xdev 而失败。

`476fc244` 的 macOS、Windows、ARM64 和 Linux root 已实际通过；Linux 主车道为
196 pass、3 fail、4 skip，ASAN 仍运行。worker 的 dev/locked、loader alias、完整
exec/net 观测及日志 symlink 拒绝已实际通过。余项分别为首次 writable payload 的
父 mountpoint、HomeLayer fixture 在声明 layer 前执行 legacy metadata，以及 dynamic
开发 client 的 root runtime 闭包拒绝。对应修复与静态实际 pass-only 门禁已落地，
待本批构建和新 head CI。

`f1fd0b19` 的 macOS、Windows、ARM64 和 Linux root 已实际通过；Linux 主车道为
192 pass、6 fail、4 skip，ASAN 仍在运行。此次 worker 已实际启动并返回合法响应，
失败发生在宿主发布日志时没有创建 `logs/recipes`；另一项失败是清 capabilities 的
UID-0 域控制器无法再映射 parent UID0。两项对应修复已落地，待本批构建与新 head CI。
不能将其他平台 green 或后续未执行的发行版场景当作该主车道通过。

`e8fbdd01` 的 macOS、Windows、ARM64（含原生兼容）及 Linux root 流水线已实际通过。
Linux 主车道仍为 191 pass、5 fail、4 skip；ASAN 为 105 程序 pass、1 fail，唯一失败是
受 instrumentation 影响的 generation 切换 11.069 ms 超过 10 ms，无 sanitizer 报错。
完整失败现场证明，worker
在加载 ELF 入口时即因解释器 literal 路径不可见而退出，审计握手错误为次生报错；
前一轮将该错误直接归于两个 listener 的诊断不充分。单 listener 的实际 kernel 回归
已通过，但 namespace 组合验收须等加载路径修复后的 CI。私有域 probe 已正确找到
system bwrap，剩余假阴性来自 uid=0、清 capabilities 的 context 中省略 `--unshare-user`。
两项修复与实际回归已完成本地构建，待新 head CI，不能沿用其他 head 的 green。

`4daee595` 的 ARM64 与 Linux root 流水线通过；macOS、Windows 的构建及单元测试
通过，但两个旧 shell/PowerShell fixture 与新入口所有权边界不兼容，已按实际失败
日志修正 fixture，未降低产品边界。Linux 主测试实际 191 pass、5 fail、4 skip；失败集中于 Lua worker 挂载/审计与私有域
backend 判定；对应修复已追加提交并等待下一 head CI。root 闭包隔离回归和 ASAN
已实际通过。

最终 review 另修复私有域 exec/use 对 `./`、`~/` 挂载源的二次解析；命令分隔符后
argv 保持原样。开发与静态构建通过，真实隔离回归等待 CI。
镜像 self update 不再吞掉错误，验证真实旧 payload→候选 archive 下载/安装/激活、
generation 改变、stage0 SHA/inode 不变及回滚。静态性能测试严格执行 shim <10 ms、
热 exec 额外 ≤5 ms、冷 exec 相对等价直接 provider 额外 ≤10 ms、rootfs 闭包入口
额外 ≤100 ms；CI 要求全部实际 pass。报告要求所有 Linux 必需能力都有通过证据；
Windows 引号契约由 Windows 单独硬验收，WSL1 保留设计明确的尽力验证例外。

下面 §1–10 为初次审查快照，包含当时已知、后来已修复的问题；不能当作当前缺项表。

## 1. 初次审查结论（历史快照）

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


## 11. 续行实施状态（2026-10-08）

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

## 12. 下一实施批次的证据（2026-10-08）

后续遵循用户的新约束：只追加 commit、普通 push，不 force push 或改写已推送提交。
`3ea30c71` 修正隔离测试使用的 xdev 路径，`dd118625` 将 app 自测的实际结果接入报告，
并为 macOS generation 读取失败增加 errno/path 诊断；断言仍要求取得完整 generation。

上游 [libxpkg #46](https://github.com/openxlings/libxpkg/pull/46) 已通过 CI 并合入，
发布 0.0.61；[mcpp-index #516](https://github.com/mcpplibs/mcpp-index/pull/516) 已通过
检查并合入。CN source archive 通过 GET 校验，SHA256 为
`ee1e1a6dd2367a50fd9cd13f179367aa8638b9fa83410bdda60cd4bdb54b9019`。
客户端已经恢复正式依赖并成功下载/构建，未依赖 local path override。

新增实现包含持久 Lua worker（顶层加载、metadata、builder、hook 均可进入执行边界）、
独立控制协议、每目标 payload shadow、受限写入、输出日志和 exec/net 审计通知。
网络 proxy 使用独立 netns 与 supervisor 转接，只有声明的 SOCKS5h 出口；目标域名交给
代理解析。原生强边界仍以 Linux 支持为准，不把 macOS/Windows 的不支持解释成同等隔离。

本地有限回归：protocol 2、worker flow 2、cache 2、network unit 3 个用例通过；worker 的
3 个真实隔离用例及 network 的 2 个用例因本机 bwrap 不可用跳过。后续的 net 审计接入、
cache 所有权证明、resolution evidence 与系统层复用仍需要新批次回归。xdev 资源锁、
HTTP fixture、CLI 与选择器共 17 个 app 用例已通过；动态 CI、趋势及性能矩阵尚未验收。

[索引 #940 的两架构构建](https://github.com/openxlings/xim-pkgindex/actions/runs/37707873566)
均通过，包括 loader 的版本、私有库搜索、locale/gconv、logical-root cache/preload 和
managed loader 不读外来根 cache 的实际运行。两份资源已发布到 GitHub，并在资源出现后立即由本地 gtc 补齐 GitCode；CN 完整 GET 的
SHA256 与构建产物一致。recipe 2.44.3 revision 2 已按架构选择 loader、ABI、URL 和 digest，
#940 最新 head 的两架构及三平台检查通过，已于 2026-10-08T01:22:00Z 合入（`ebf1fbb3417ec513b923279044433887b3af8850`）；旧客户端 2026.10.4.1 在独立 HOME、CN 镜像下实际安装成功，记录 configured revision 2；
对安装后的真实 payload 再跑 logical-root cache/preload 探针通过。候选客户端生态安装仍待验证。

客户端新增库 cache 生成遵守用户数据规则：只写专属 staging，既有 cache 必须有匹配
digest 的 xlings ownership record；未知/外部修改的数据保留并报错。写前记录旧、新 digest，
中断后的任一有效状态可再次识别。root refresh、rollback 与 stage-0 已接入，待本批 CI。

macOS 日志将并发读取失败定位为 `readlink` 返回 EINVAL；`2b78ce6e` 追加有界读取重试，
维持完整 generation 的断言，macOS CI 已通过（run 37709550248）。Windows 的 JSON/string_view 测试比较歧义
由 `88eafe96` 追加提交修正；Linux locked audit 清理断言失败，supervisor teardown 修复待本批回归。xdev 动态矩阵已接入 distro 三车道，
本地计划确认三个脚本各执行一次；未把计划输出当作场景执行证据。

完整交付仍未完成：真实 Linux 隔离、系统层完整闭包/prefix domain、
升级与性能证据、最终 CI、自审、客户端 release 及 CN 真实升级均须继续推进。


## 13. 事务、流与闭包基础批次（2026-10-08）

本批已完整构建通过（GCC 16.1，build 8）。一次完整客户端回归为 98 个测试程序：
94 pass、4 fail；运行记录中的 49 个具体用例 skip 保留为缺证据。三处失败已定位为
Meta 字段顺序、模块导出签名不一致、loader fixture 缺失；第四处 self init 超时来自 fixture
仍自动获取官方索引，已改成显式本地默认索引和测试索引，现有二进制初始化实测 0.047 秒。
四项针对性复验均已通过；借用正式 group fixture 补齐 kind，旧 shared-entry fixture 改用
正式 xim:xlings 身份，产品权限不放宽。逐程序最新证据为 98 pass，保存于
`target/xdev/pr641-hardening/boundaries-latest-binaries.ndjson`，不是第二次全量运行。
xdev 本批完整 4 程序 / 18 用例 pass、0 skip，删除/header lint 和 diff 检查通过。
借用使用正式 provider/version 登记和唯一 checked runtime evidence reader，不复解析依赖名称；
`use`、config 和 remove 对全部资产先预检，以旧 scope 登记作为 ownership，未知用户文件不覆盖。
资产、候选 DB/workspace 与 root refresh 失败时回滚；状态发布使用 atomic writer，并检查写后读回。
系统共享 store 的 GC 保留无法观察外部引用的 payload，用户 store symlink 不能取得删除授权。

新增 NDJSON 1.6 的 `subos_exec` 流式 stdout/stderr、有界分块与取消；非 UTF-8 分块显式标记
base64。xdev 的趋势端到端用例已通过，报告保持 member-qualified ID，导出每平台成功耗时
中位数用于下一次实际 CI 计划；main 历史和 90 天报告 artifact 的接线仍待 CI。

root_mount 的平台接口已构建，通过捕获的 namespace/root FD 发布只读文件或目录，失败回滚；
新增真实 namespace 回归尚待执行。Root GCC shim 的逻辑 loader 选择仅在对应 generation
的真实投影和正式 gcc provider 证明下启用，默认宿主执行不改变；真实生态 consumer 验证待完成。rootfs_instance 已增加唯一 SONAME、/opt 私有库、
root cache、逻辑 PT_INTERP 与 managed loader 对照场景，并移除 desktop 人为 rpath；待 Linux CI 实际执行。

D2 当前只完成 pure closure 与 evidence 基础；private skeleton、逐 payload 的实际绑定和 broker
后的实时挂载更新尚待接入。D3 已加入 strict prefix-domain 选择与创建前预检，跨前缀 namespace
producer 未接通时明确拒绝，不能把它计作已完成。旧镜像测试删除宿主 `/xlings` 的操作已移除，
替换为 owner-private domain 的正式制作场景；producer 接通后必须实际通过，不能用 skip 代替。

已推送 `88eafe96` 的 macOS、Windows、aarch64、Linux root 和 ASAN 已通过；Linux 主套件
仍因 locked audit cleanup 失败，修复处于本批尚未推送的代码中。最终固定 head CI、升级/性能、
Root/Luban 场景、自审、合入和客户端发布链均未完成。

## 14. HTTP fixture、平台修复与旧 home 升级（2026-10-08）

基础批次已普通推送为 `6a1df938`、`d968ee47`、`0faff256`。其固定 head CI 失败：
Linux/ARM musl 缺少 `linux/openat2.h`；Windows 模块混入 Winsock 1/2；旧 mcpp
2026.9.28.3 的生成模块缓存依赖缺失影响 Linux root、ASAN 和 macOS。macOS 另有两项
路径断言把 `/var` 原始路径与 `/private/var` canonical 路径混用。
追加 `562cfbd9` 使用稳定 syscall ABI、统一 Windows 精简头，mcpp 固定到已发布的
2026.10.5.3，CI 索引快照更新为 `ebf1fbb`，路径断言使用实际 canonical 路径。
本地产品构建及 home context/layers 的 20 个用例通过；新 head 的 CI 尚待结果。

testkit 默认 home 已接真实进程内 HTTP fixture：本地真实 xpkg 索引、数值 loopback、
SHA256、实际解包，以及缺包不回落公共索引。真实下载暴露安装器漏识别 `.tar`，修复后
端到端安装通过。HTTP transport 的 3 个用例通过，覆盖真实字节、来源/本地错误、
拒绝外部地址/重定向/畸形响应和取消。带 `network` 的测试继续明确使用真实镜像。
xdev 合并结果为 case 保留 program 身份，防止不同测试程序的同名 case 混入趋势。
xdev 本批完整 6 程序 / 22 用例通过、0 skip，证据为 `xdev-http-complete.ndjson`。

隔离 home 的已发布 2026.10.4.1 已真实安装新版 glibc recipe revision 2；更换该 home
的实际 dispatcher 为候选 2026.10.8.2 后，CN 配置、self init、再次 install glibc 均通过。
configured revision、完整 workspace 与 payload 文件大小/mtime 保持一致，证据为
`target/xdev/pr641-hardening/nminus1-candidate-upgrade.ndjson`。这是现有 home 升级验证，
正式发布后的 self update/fresh install 验证仍待完成。

D2 新增 owner-private RootView 与 cache exact bindings，本地全构建通过；SCM 三 FD 与
broker 完成后的真实挂载更新仍在接线。D3 `new --domain /xlings` 与 export 已接 namespace
producer，本地 prefix 6 用例通过；真实 namespace 用例因本机 bwrap 能力跳过，不能算验收。
已有只读 systemSource 桥、runtime scope 路由及旧镜像 reader 仍在实现。最终 CI、
Root/Luban/性能真实场景、自审、合入与发布链尚未完成。后续继续仅追加提交与普通 push。

## 15. 根视图源码检查点与性能预算（2026-10-08）

`33bd4499` 的 ARM/Linux musl CI 暴露旧 SDK 没有 seccomp notification UAPI。
追加 `777dccf5` 将网络通知和 exec 审计收口到同一稳定 ABI 定义；本地原生平台构建
1.94 s、ARM musl 16.1 交叉构建 8.12 s 均通过。该 head 的全平台 CI 尚待结果。

D2 已接 trusted init 的三 FD（userns/mntns/root）传递、监督 ready 前握手、owner-private
RootView、逐 payload/generation/metadata RO mount，以及 broker 成功后同步 refresh。
refresh 完成前不回复成功；失败回复 125 并结束 session。初次检查点编译发现 array FD
误用 vector-only close API，修复后依赖源码和两项 RootView/闭包 unit 通过。
两项真实 namespace/broker 回归已编译，但本机 bwrap 不可用而 skip；仍需 CI 验证行为。

D3 source reader/facade 源码检查点已编译：typed physical/recorded/logical mapping、RO mount
与 marker inode 证明、限定 schema 路径的 metadata facade、exact payload slot 和外置 ownership
清单、已知 legacy layout 2 reader。producer/runtime/export 的完整桥接仍在继续，尚未验收。

新 generation 性能断言实际使用 300 个不同 payload、至少 600 个链接，测量 plan+commit
及完整 checked switch。三次样本中位数为生成 26,876 us、切换 4,939 us，分别满足
1 s / 10 ms 预算，`test_root_generation_perf-checkpoint-2.ndjson` 为通过证据。
stage-0 在 boot audit 记录 `stage0_elapsed_us`；真实 boot 场景要求四次启动（含 fallback
和 `--now`）均有记录且 ≤200 ms。该启动预算尚未执行，不能用本地 unit 代替。

## 16. D2/D3 集成与实际能力门禁（2026-10-08）

本批已完成 RootView exact closure、broker 同步更新、inode 替换检测、source facade、
私有 prefix producer 和 export 的源码接线。完整 GCC 16.1 产品构建通过；随后一次
完整客户端运行 `d2-d3-full-regression.ndjson` 为 **107 程序 pass、0 fail、54 具体用例 skip**。
RootView/闭包 3 项、source reader 3 项、prefix domain 7 项和 materialize 8 项 unit 通过；
namespace/broker/source export 仍因本机能力跳过，不计为实际隔离证明。
此次 300 payload 性能中位数为生成 36.592 ms、checked switch 5.394 ms，预算通过。

审查确认 locked 的 FD 捕获需要额外规范化：bubblewrap 完成挂载后，`--disable-userns`
创建第二层 user namespace。因此由外层 supervisor 从可信 mount FD 派生真实 owner，
检查捕获 userns 的祖先链，再在 ready 前替换 owner FD；update 保持精确 owner 核验。
相应普通/locked 的真实回归已加入，仍需 Linux CI 成功证据。
依据：[bubblewrap v0.11.2 官方源码](https://raw.githubusercontent.com/containers/bubblewrap/v0.11.2/bubblewrap.c)。

`777dccf5` 的 ARM cross/QEMU、native ARM、Linux root 和 ASAN 已通过。
Windows 失败为 fixture 错把 `/etc` 形式的 root-relative path 当作普通相对路径，已改
`has_root_path()`，本地相关断言通过。macOS 单元和 xdev 全通过，后续 e2e 暴露旧
header 来源消失会阻止其他 scope 刷新。现已区分 Recorded ownership 与 Present 新资产，
从目标树逐条证明并清理过期链接，保留未知 sibling，修复路径尾斜杠的归属比较；原
e2e 的 S1–S4 已通过，S5 提示契约的修复待本批复验。

Linux 主车道实际执行隔离后失败，不能用本地 107 pass 覆盖：worker 传入 upstream
bubblewrap 不存在的 `--preserve-fds`，网络 exec 未识别 `--observe`，root mount 更新
返回 EINVAL。locked cleanup 用例原先 start/exec 的策略不同，125 是隔离 digest 拒绝，
没有触发审计故障；测试改为先持久声明 locked，并检查 `E_AUDIT_WRITE` 后验证清理。
上述失败分别修复、复验后继续普通 push，最终结果以新固定 head 为准。

新增 `DOM-SOURCE` 隔离验收项。Linux 在静态 release candidate 生成后实际执行
checked 系统来源借用和镜像导出，必须产生 pass，skip 使该步骤失败。三平台先一次
`mcpp test --no-run` 构建，再让 xdev 用已建 artifact 执行，避免每个测试程序反复
启动完整构建图。完整 CI、实际启动/生态、最终自审、合入与发布仍待完成。


本批产品构建 `domain-wave-final-build.log` 29.57 s 通过；macOS 同类旧 header 场景的
本地实际 S1–S5 全部通过，`sysroot-pinning-final.log`。owned entry 的 install/use 四种
实际替换场景通过，`entry-owned-replacement-final.log`。命令参考已重新生成，help parity
检查通过；此后新增 config proxy 说明需再次生成。

最终自审发现并继续修复两项：同 destination 重装叠加 mount，后续 remove 会露出旧
payload；native mixed-home export 没有规范借用来源、按 target 误保留未导出的版本。
前者改为有精确旧 tree 回滚的 replacement 事务，后者统一按 checked closure 规范
image-owned metadata，并保留 ELF/alias 所需的精确逻辑 slot。新增实际回归仍待编译和 CI。
私有域生命周期已接确认后删除、双向安全复制和同域 fork；跨域 fork 仍需闭包重建。


最终集成构建 `final-wave-build-2.log` 5.78 s 通过（首次缺少 HomeView import，已修）。
本批针对性回归 **13 个程序 pass、0 fail、17 具体 case skip**，覆盖 replacement 事务、
RootView、domain/source、entry、materialize、observe/network；skip 保留为缺隔离证据。
xdev 全部 **6 程序 / 22 case pass、0 skip**。自动生成命令参考/help parity、27 条
文档命令路径与 NDJSON、平台 header lint、SubOS 删除 lint、CI YAML 和 diff whitespace
检查通过。旧完整回归 107 程序的证据保持独立；新固定 head 的 CI 和实际生态仍待完成。


追加提交 `0c9f395f` 已普通 push，新 head 的五个主 CI workflow 已启动。PR 描述同步到
当前行为和验收状态。本地静态 dist（GCC 16.1 musl）构建 1m05s 通过；使用该静态
候选执行 `RootExport.OwnsBorrowedClosureAndOmitsOtherScopeVersions` 实际通过，0 skip，
证明 native mixed-home 导出的 owned metadata、精确逻辑 slot、仅作用域版本及来源不变。
证据为 `final-static-build.log` 和 `mixed-source-static-local.log`。

复核 Part 2 §4 / §16：跨前缀实例迁移未被要求，不是本方案交付阻塞；原约束是仅相同
前缀共享 payload。新前缀用 package-coordinate producer 重新制作，直接搬移旧绝对路径
payload 拒绝并保留用户数据。同域 fork、确认后删除、双向复制仍需本次实际 CI 验收。


接口复核确认 stdout 全程由 interface 的 StdoutCapture 和子进程 stream framing 保护，
没有原始输出污染 NDJSON；但 SubosEvents 读外层日志、switch_subos 越过私有域全局激活
限制以及删除成功 DataEvent 丢失确实存在。已使用严格 descriptor 选择实际 home、在共享
use_global 路径拒绝域激活并补回父 stream 的 subos_removed；真实 domain fixture 加入
NDJSON 单结果、错误日志来源拒绝、两次复用同一 session 的 exec 断言。
产品构建 29.94 s 与 interface protocol 回归通过；domain producer 重新编译通过但本机
bwrap 仍 skip。原运行中的 ARM workflow 已通过；新接口波次需要追加提交验收。

session join 只通过 Unix socket / SCM 传递命令，由原 session_init 在原 namespace 内
fork/exec；新 producer 不是跨 sibling userns setns。因此没有为假设的 namespace 权限
问题改代码，保留实际 session 复用测试验证。


`0c9f395f` 的 macOS unit/xdev 失败为 RootStoreClosure 两个新断言使用原始 `/var` 路径，
而产品 closure/RootView 已 canonical 为 `/private/var`；产品准备本身成功。测试统一
canonical home/system，相关两个程序本地回归通过（真实 namespace case 仍 skip）。
接口波次已追加为本地 `c89bc054`，其 26 个 interface protocol 用例通过；后续 macOS
路径测试修复继续单独追加，等待本批 Linux/Windows 现场后一起普通 push。

`0c9f395f` 的现场 CI 结果已收齐：ARM 通过；macOS、Windows、Linux root、Linux
unit 有失败，ASAN 尚在运行。此 head 不能合入。追加修复按失败证据收口：

| 现场 | 原因与修复 | 尚需的证据 |
|---|---|---|
| Windows | 两个测试比较原始分隔符 / 使用无 drive 的 `/xlings`；改为规范化路径和平台绝对 recorded home。junction 的可读目标拼写不相等时，继续用文件对象 identity 证明归属，拒绝未知条目 | Windows 新 head 实际运行 |
| Linux worker | 私有 `/home` 遮蔽开发版 ELF loader/RUNPATH；私有 `/tmp` 遮蔽外部索引 symlink 目标。补精确 RO runtime 与 canonical 目标挂载 | 原 Lua / system-layer 隔离断言实际通过 |
| Linux root closure | fixture 在 RO `/` 下挂不存在的 `/root-view-result`，bwrap 尚未启动就失败；结果目录迁入已有私有 `/run` | broker 成功、inode 替换后 remove、refresh 失败 125 的实际用例 |
| Linux root 性能 | 300 payload build 约 76 ms；完整校验后切换 13 ms 超过 10 ms。减少重复 syscall、路径比较与 manifest 读取，保留全量库存与目标校验，预算不变 | 新 head root CI 的完整 checked switch 测量 |

无感升级审查另发现私有 `bin/xlings` 固定于创建时的 source 路径/字节，owner 升级
不能让它更新。创建及 runtime 共用 managed mirror 刷新：v2 证明绑定 owner/private home、
domain marker 和 entry SHA；事务锁串行化 pending/ready；独占 snapshot 同时通过 digest
和 marked-client 检验，再由现有 `entry_binary::replace_with` 发布并修复 stale shim。
恢复只接受日志授权的旧/新字节；未知、损坏或无归属 staging 保留并拒绝。新增六个
filesystem 回归覆盖 source 路径变化、同路径升级、WAL 恢复、legacy 证明和拒绝无写。
两轮只读复核未发现其他明确 P1/P2；构建与新 head 验收仍是独立门禁。

本批产品最终构建通过；108 个测试程序一次构建通过。dispatcher 六个实际回归
全部 pass。300 payload 生成 median 23.034 ms，完整 checked switch median 4.626 ms，
两项预算均通过；链接被改成 regular file / 陌生目录也仍拒绝。受影响的注册 case
49 pass、8 skip、0 fail，另 legacy shim 用例通过。skip 全为本机无 bwrap 的真实隔离
用例，必须由固定新 head CI 补齐。两项安全 lint 与 whitespace 检查通过。


最终 review 的追加验收修正（尚待下一固定 head CI）：

- `a55deb67`：domain exec/use 用外层已解析的 Mount 保留宿主来源；`--` 后命令 argv 不变。
  产品 dev 构建及 domain fixture 编译通过；namespace 用例本地 skip，未记通过。
- `64f3e718`：两个客户端 fixture 使用隔离 xim/provider 及实际候选 bytes，符合共享 entry
  的所有权；remove 三场景本地通过。macOS 默认 Bash 的 UTF-8 fixture 改为 indexed arrays。
- `da383ed4`：ROOT-SELF-UPDATE 走正式 hooks、真实 HTTP candidate archive；严格验证旧/new
  payload 版本、generation 切换、stage0 SHA/inode 保留及 rollback，任何失败硬返回非零。
- `9a24c7f9`：static candidate 的四类 SubOS 性能预算，以及完整 generation 校验预算
  被 CI 显式执行且必须 pass。相同 blocking runner、交错配对、微秒计时和完整环境隔离；
  本地编译通过，真实 bwrap 不可用，四项 skip 明示，不算性能验收。
- `a0d0d725`：测试程序已构建成功时，失败直接报告，不再调用所有 build.ninja 重编译。
  `4daee595` 的 Linux 单元测试在 05:21 返回，重复编译把 job 拖到 05:29 且淹没现场。

`4daee595` 的实际 root closure 重绑/rollback namespace 回归通过。剩余 worker full
观测失败已定位为同一线程分别创建 exec/network 两个 NEW_LISTENER，内核拒绝第二个；
私有域 backend 错判为 resolve_owner_home 对非 shim 也回退到调用者 entry。下一波分别
复用单一通知 listener 并先分类真实 shim；不削弱审计、网络隔离或扩大宿主挂载。

发布前真实 CN 旧版 home 已准备：正式 `2026.10.8.1` quick install，明确设置镜像 CN，
安装 GCC 16.1.0 / mcpp 2026.10.5.3 / glibc 2.44.3；原生 C 程序编译运行和 doctor 通过。
独立 mcpp registry 同样设置 CN，`mcpp new cn-ecosystem`→build（38.48 s）→run 成功，
使用实际 `import std` 模块 C++23。旧 payload 4448 条字节/链接/权限及四项用户数据
sentinel 已留快照，待正式候选发布后执行真实 self update 对比；尚未把候选包冒充发布。


最后三项根因修复与本地证据：

- `588586d9`：worker 仅绑定 exact canonical recipe/source；现有 RO home 的 symlink
  自然解析到该目标，不再穿过 RO symlink 再绑定一遍。未知宿主 sibling 仍不可读。
- `c58ac9a8`：Ports 先由 ShimClassifier 验证候选为 actual dispatcher，再查询 owner；
  普通 system bwrap 仍真实 probe，不因当前 `/xlings/bin/xlings` 调用者归属被排除。
- `b70e8cc7`：worker 和 session-init 共用一个 exec/network notification listener，
  一次可信 pre-filter helper/SCM 交接；typed notice 保留所有 syscall 和 ABI checks。
  exec 审计只读 filename，审计完成后才 CONTINUE；失败拒绝、终止，native 返回 125。
  full + proxy/NAT 回归要求同一真实日志 row 的 exec path、net-attempt 和 mode 证据。

两轮专项只读复核未发现本批明确 P1/P2。产品 dev 构建 31.49 s、dist static 45.84 s
通过；108 程序构建 118.705 s 通过。kernel seccomp 四个真实用例通过，单 FD 同时
阻塞 exec 与 datagram，net 拒绝及 exec allow/deny 实测通过。另针对性 XTEST 5 pass、
16 skip、0 fail；跳过仅因本机无法创建 bwrap namespace，不能据此宣称 namespace
组合验收完成。schema/YAML parse、两项 safety lint、whitespace 检查通过。

`e8fbdd01` 后续修复与有限复验：backend 探针显式 `--unshare-user`，缓存区分
user/mount namespace、凭据及 seccomp 状态；动态 worker 保留 exact canonical 与
ELF 声明的解释器/RPATH 别名，只在被私有视图遮蔽时补 RO 绑定。新动态入口回归
使用目录与文件双重 loader symlink，要求 dev/locked 启动成功、loader 不可写、
相邻宿主文件不可读；宿主启动基线已实际通过，真实隔离部分本机明确 skip。

本批 dev 30.81 s、dist static 24.01 s 构建通过；109 个测试程序一次编译通过
（126.570 s）。五个相关程序全部 exit 0：注册用例 6 pass、7 namespace skip、
0 fail，另四个实际 kernel seccomp 用例通过。安全 lint 与 whitespace 检查通过。
镜像 self-update fixture 动态选取官方 Linux 的真实前序版本，兼容发布后 latest
已与候选相等的情况；两种索引现场的前序选择与 shell 语法检查通过。仍需完整
固定新 head 的隔离、性能、镜像升级和各平台 CI，尚未合入、发布。

ASAN 的 300-payload 工作量保留全部 projection、generation 与完整切换断言；只在
最后将 instrumented timing budget 标为明确 skip，不把该测量当作运行时性能证据。
单独 inventory 的未知目录/替换文件拒绝用例继续受 ASAN 检验。静态性能车道依旧
强制两项 generation case 实际 pass，build ≤1 s、checked switch ≤10 ms 阈值未变。
该调整后的普通构建复验两项实际 pass：生成 median 22.203 ms、完整 checked switch
median 3.561 ms，无 skip；ASAN 的新 head 结果待 CI。

发布路径亦遵循“不改写提交记录”：`bump_index.sh` 在已有 bot branch 上合并最新
main、追加 bump 并普通 push；不存在则从 main 创建，merge 冲突或远端更新拒绝时
停止，保留旧历史。token 由 credential helper 提供，不进入 remote URL。临时 bare
仓库实测连续两次发布保留 previous/main ancestry、重复版本不动 tip、一个 open PR
持续更新；缺分支、检查失败、冲突拒绝及无 token 输出均通过，尚未触发真实发布。


`f1fd0b19` 的完整现场进一步修复：recipe 发布在宿主安全创建 `logs/recipes`，
拒绝重定向符号链接，并独立报告发布失败；没有给 worker 扩大写挂载。实际回归要求
fresh home 输出落盘、stdout 不混入，恶意 log symlink 保留外部文件和时间。

Linux kernel 的 parent-UID0 mapping 检查确认嵌套失败原因。域管理控制器统一映射
为非零 UID/GID 1000，仍映射同一宿主 owner；包括构建、带系统来源的只读 Must worker
和 runtime 管理。实际 rootfs provider 仍 UID0，所有子执行清 capabilities。
DOM-SOURCE 保留只读/导出断言并检查实际 worker UID1000/CapEff0；domain exec 检查
实际 root UID0/CapEff0。仅改 runtime 会留下 constructor 的同类故障，故两者统一处理。
本批尚待构建与固定 head 的实际 namespace 验收，尚未合入、发布。

本批复验完成：dev 构建 29.82 s、dist static 23.01 s 通过；109 个测试程序一次
编译通过（108.423 s）。四个相关程序均 exit 0，注册用例 5 pass、10 skip、0 fail。
所有本机缺 bwrap 或静态入口的实际场景仍明确 skip；日志发布、嵌套 source worker
与实际 root 的身份断言等待下一固定 head CI，未把 skip 当作通过。两项安全 lint、
whitespace 通过；mcpp.lock 仅重排且语义相同，已恢复。全部源码更改以追加提交、
普通 push 保留记录。


`476fc244` 现场 196 pass、3 fail、4 skip 的剩余修复：writable worker 仅为经过
declared store、coordinate、canonical 校验的单 package 父目录准备挂载点；private
shadow mount 保持不变，相邻 payload 写入仍拒绝。HomeLayer fixture 的 self init
本来在 layer 声明前加载 index，legacy undeclared loader 合法执行顶层 Lua；现在
从首次 metadata 读起声明同一实际系统层，并在 init 前后及 install 后验证未越界，
不删除已出现的文件，也不修改 undeclared recipe 兼容路径。

动态开发入口需要根内未声明的 GCC runtime，产品拒绝正确；DomainProducer 全生命周期
改由实际静态 candidate 验收。Linux 静态步骤分别运行 source/export 与 domain producer，
三项完整 case 各恰好一条且必须 pass，任何 skip/missing/fail 都失败；所有 UID0/CapEff0、
mount、session、NDJSON、用户数据和只读来源断言保留。本批尚待构建与固定新 head CI。

本批 dev 30.16 s、dist static 22.73 s 构建通过，109 测试程序一次编译通过
（106.021 s）。三个相关程序 exit0，注册用例 5 pass、8 capability skip、0 fail。
两项安全 lint、CI YAML parse 和 whitespace 通过；lock 仅重排且语义相同，已恢复。
首次 payload 隔离、带来源 metadata 和静态 domain 全生命周期仍须新 head 实际 pass。
未将本机 capability skip 计作完成证据，全部更改继续追加 commit / 普通 push。


最终审查补充：payloadless config 的 has_hook 仅 Load/读取函数表，移除多余
install_dir，不为 metadata 查询创建 package 父目录或 shadow。新增 dev scope 下
显式版本与裸坐标两种卸载回归，要求各 uninstall 在独立 hook log 中实际一次。

机器 /etc 的目录写入从 filesystem root 逐级 openat(O_DIRECTORY|O_NOFOLLOW) 锚定，
目录创建、factory 链接和独立账号文件通过该 FD 操作；passwd/group 同一锁定 regular
FD 检查 nlink=1 后读写。合法 generation→payload factory 叶链接保留，目的名称改用
lexically_relative，source 解析不再制造 ../ 目的路径。既有机器账号文件不读取 factory
默认值；未知账号 symlink/hardlink 明确拒绝并保留。fill/refresh 返回 expected，失败
沿现有 generation 回切及 stage0 recovery 路径传播。外部 sentinel、嵌套 symlink、
root 父目录、var、账号共享 inode 和路径置换均有真实 filesystem 回归，待本批复验。

已核实 146 required（0 planned / deferred）以及 actual passing-evidence 门禁；门禁
存在不等于尚未执行的需求已通过。默认观测仅记录命令路径和环境名、不保存 argv
值；草案 R12 的 raw opt-out 尚未提供。PR ≤15 min 是当前未达到的墙钟目标，不能
由静态产品性能预算 pass 推导满足。无 policy/无借用系统层的 recipe 保留 legacy
执行，macOS/Windows 的 dev 是 advisory；Must 无能力拒绝。旧 entry 不认识新的
min_client，doctor 核实实际入口；不声称旧版能追溯执行新 schema。

本批有限复验完成：dev 33.92 s、dist static 45.90 s 构建通过；109 程序一次
编译通过（267.721 s）。四个相关程序均 exit0，注册用例 18 pass、7 capability
skip、0 fail；其中 rootfs 13 项全部实际通过，含四组新增真实文件系统边界回归。
模拟 release 删除 triple target 后，实际 retained xdev ci plan 与四个 candidate
验收程序的 listing 均正常启动。两项 safety lint、Linux CI YAML/脚本语法、
whitespace 通过；lock 重排语义一致，已恢复。真实 namespace 卸载、静态 domain、
性能、boot/self update 与 ASAN 仍须下一固定 head CI，不以本地 skip 计完成。

## 21. Payloadless publication 根因修复与有限复验

CI `8e8638fc` 唯一失败的实际 uninstall log 有一次 hook marker，错误链已定位到
missing old payload 的 publication，而不是 hook 未执行。最终补丁保留现有目录
事务、no-replace 首发、unknown/nonregular 拒绝；只对 validated selection 合成
默认 XVM remove，hook 真失败非零。共享 dev/legacy 回归覆盖显式/bare once、
9.9.9拒绝且旧日志不变、唯一原始 hook failure 不吞。

开发构建30.31 s、dist/static38.61 s通过，109测试程序构建303.617 s通过。
新增无policy真实CLI专项1 pass；本机namespace能力缺失，dev worker验收待新head CI。
ARM失败诊断只增加原始status/stdout/stderr、shim/provider traces及always保存已有
archive；精确候选版本匹配和原失败退出码保留，不用诊断重跑放宽验收。

最终 xdev 有限复验：e2e/test_lua_worker exit0，注册用例3 pass、7本地 sandbox
capability skip、0fail；unit/test_xim_install exit0，传统gtest63 pass。安全删除lint
13标记、platform header lint、diff-check、ARM YAML/bash/Python语法均通过。

## 22. 静态 source fixture 的执行入口修复

固定`68399f72`的macOS（147/0/2）、Windows（105/0/44）、ARM cross/native、
Linux root完整通过；Linux主单元、静态release与candidate cold-home阶段通过。
ASAN实际107程序pass、0fail，1771.41s，其中build1711.75s、run57.59s；没有
sanitizer错误，instrumented timing明确不作预算证据。

首次static-domain gate长时间停在source用例。只读源码和实际GTest ELF marker
显示具体递归路径：测试直接调用producer::run，后者把当前GTest image当CLI刷新，
version_of(--version)重新运行整套GTest并再次刷新。尚待该CI最终日志补齐现场，
不将推断标为进程树实证。fixture现显式prepare/refresh静态candidate，使用与
production相同的checked source Facade和command，testkit执行每个操作上限120s，
完整记录退出码/signal/output。三个硬验收和所有只读/元数据/导出断言保留。

有限复验：相关2程序编译8.695s通过；实际静态RootExport1pass，source namespace
因本机缺bwrap1skip，不能替代CI实际隔离pass。客户端产品源码未改变。

## 23. 系统模板已挂载不等于目标 home 已配置

固定`2d1ef97a`：macOS147pass/0fail/2skip，Windows105/0/44，ARM cross/native和
Linux root完整通过。Linux主单元204/0/5，static release和cold-home通过；静态
source用例执行2.932秒后因const JSON缺字段断言SIGABRT，随后性能和下游未执行。
ASAN尚待该轮结束；其他平台green不替代source gate。

日志确认producer初始化与创建的前置断言没有报错，随后测试读取借用登记字段
中止；没有栈或home JSON现场，不能断言具体缺失key已被直接采集。源码审查发现
`resolve_base_package_`仅看payload目录是否存在。checked source facade已把只读
slot挂在logical prefix，因此原路径跳过installer，没有运行config和生成目标home
借用登记；与最早const读取`versions/source-member`缺字段的现象吻合。

修复对已存在base先调用`borrowed_mount`验证authority：验证失败拒绝，确为只读借用
则在同进程调用既有`cmd_install`完成checked borrow/config和持久登记。作用域与原
missing base自动安装一致，在新SubOS创建之前，不提前切到尚不存在的scope。
来源payload保持只读，重复配置仍由既有configured verdict决定。测试保留所有原
断言，同时先检查JSON pointer存在并输出完整受控fixture JSON，使失败能正常保存
现场，避免abort。下一固定head仍须三个原静态隔离场景实际pass。

本批有限复验：dev31.95s、static/dist23.83s构建通过；相关2程序构建36.408s通过。
静态source/export程序实际1pass、1本机bwrap能力skip、0fail，未将skip作为隔离
通过。既有legacy fork shell四场景全部通过（local独立性、missing base自动安装、
equals参数、missing source拒绝）。删除/平台header lint和diff-check通过；lock
重排语义一致已恢复。真实借用config和全部静态门禁待下一固定headCI。

## 24. 系统来源实际通过；空模板的 runtime grant 目标修正

固定`77431da7`：macOS147pass/0fail/2skip、Windows105/0/44、ARM cross/native、
Linux root完整通过；Linux主单元204/0/5、static release和cold-home通过。
ASAN实际107程序pass、0fail，1572.62s（build1521.52s/run50.05s），无sanitizer错误。
静态source实际两个pass/0skip：
`DomainSourceProducer.ReusesOnlyCheckedReadonlyBytesAndExportsAnOwnedImage` 3876ms，
`RootExport.OwnsBorrowedClosureAndOmitsOtherScopeVersions` 56ms。§23产品修复已有
真实namespace、只读来源、完整导出与未知字段保留的执行证据。

随后`DomainProducer.InstallsAtLogicalPrefixInsideANamespaceAndPreservesHostData`
在显式shell file grant阶段退出125，bwrap报`Can't create file at /bin/sh`。
原grant未在重放中被改名；fixture的空template没有/usr/bin/sh，/bin等merged-usr
链接指向不可创建runtime mountpoint的只读投影。后续namespace FD错误是启动失败
结果，不是另一处broker/FD根因。性能和发行版下游因此未执行。

最小调整只改fixture：精确核验bin/lib/lib64原始links后，仅移除本次Home自有的
已知link并创建机器目录，按实际shell/loader/library literal目标创建regular占位。
未知条目不替换，/usr和generation/payload不写；仍按原单文件RO grants实际执行，
不挂任何whole host目录。UID0/Cap0、exec/use两组 ./与~/mount source及`--`后argv
原样断言保留。产品源码、scope/closure和权限不变。相关程序编译6.771s通过；本机
无bwrap的实际namespace明确skip，不计验收pass，等待下一固定head硬门禁。

用户最新要求：技术验收和综合自审完成后先汇报，用户review决定是否合入；本轮
不合入、不发布。发布/CN latest实测保留为获准后的步骤，不冒充已完成。

## 25. 私有域复制/删除的 role 路由与独立 CI 验收

`4793f887` 的 Windows、macOS、ARM64 和 Linux root 全流水线通过。
Linux 静态 DomainSourceProducer 与 RootExport 两例实际通过，DomainProducer 已通过
运行时单文件授权、UID/capabilities、相对挂载、session 复用等断言，随后在 `cp` 和
未确认 `remove` 的 outer role guard 失败；性能及发行版下游尚未执行，ASAN 仍运行。
这不能报告为当前 head 达到合入标准。

`cp` 已有 physical producer 的 beneath-copy 适配，`remove` 已有控制目录所有权预检、
用户确认、namespace 内实际删除及仅清理已证明控制数据的适配。错误在于调用这两条
路径前仍使用一律拒绝 private domain 的 outer role guard。现在显式对实际 physical
HOME 调用同一个 strict kind/role/roles::check，并将 Remove 检查放入公共 `remove()`，
让 CLI 和 interface 都保护 running root、boot entry；其他未经适配入口仍拒绝 domain。
用户确认退出 2、未知控制文件拒绝、共享 payload 与日志保留不变。

fixture 的复制结果、escape symlink 和 fork 结果三处改读 `rootfs/tmp`；`root` 是
immutable generation 指针，不能用于机器用户数据。新无 namespace 依赖的回归用例
通过实际 CLI/interface 验证 private scope copy、未确认删除、producer boot-entry
保护、损坏 role 状态和未知 outer 控制文件拒绝。已有损坏 boot 状态回归新增 CLI 与
interface 删除断言，均保留 instance。

本地开发构建 31.42 s、静态构建 25.77 s；两组 SubosState 共 9 例实际 pass、0 fail、
0 skip（private adapter 539 ms）；legacy fork 四个场景实际 pass。完整 DomainProducer
在本地因无可用 bwrap 明确 skip，必须等新 head 的静态 CI 实际 pass。
删除 lint、平台头文件 lint、diff check 通过。

CI 先保存通过 cold-home 的候选 archive，再分别执行 domain 与六项性能预算。
保留三个 domain 与六项性能实际 pass 的原断言，任何失败/跳过/缺失仍在最终 hard
verdict 将 build job 置为失败。下游基于已验证候选是否存在继续执行，避免早期失败
遮蔽 E2E、isolation、Arch 和 distro 的独立结果。修改的 YAML/bash 解析通过，真实
hard-verdict shell 对 125 个 outcome 组合验证仅全部 success 返回 0。

仍保持 draft，后续全部追加提交、普通 push。按用户最新要求，全部必需 CI 通过并
完成最终审查后先报告交给用户 review，未经用户确定不合入、不发布。

## 26. DomainProducer 已通过；独立验收暴露的 Linux 问题收口

固定 `2b802853` 的三个静态 domain/source/export 用例实际通过，包含用户贴出的
DomainProducer 未确认 remove 退出 2，以及 copy/fork/已确认删除/宿主用户数据保留。
macOS 147/0/2、Windows 105/0/44、ARM cross/native、Linux root 全流水线通过。
ASAN 107 程序 pass、0 fail，1824.23s（build1763.48s/run58.69s）。
Linux E2E 为 131 pass、9 fail；热/冷 exec 额外约206ms失败，isolation/system layer、
Arch 和三个 distro 分片失败。执行报告146 required全部有声明，但仅123有通过证据、
23未验证；因此这一 head 不具备合入条件。continue-on-error 的 step conclusion
曾被误读为性能通过，已更正；实际 outcome、日志和案例才是验收依据。

本批产品修复：

- session-init 在 fork 前接入 SIGCHLD self-pipe，main/joined child 恢复信号，
  supervisor 与 init 先排空信号再 reap，使退出唤醒不会丢失。200ms只作TTL/超时tick。
- syscall trace 确认每个极小IPC包也分配并清零1MB。Linux先peek实际packet长，
  按长度和上限分配，接收仍保留原FD/CLOEXEC规则；数据或FD截断关闭已接收FD并拒绝。
- ldconfig 使用私有tmpfs /run，先精确RO闭包alias，再挂独占RW cache staging，
  非递归将 /run 只读。机器自己的 /run和payload不改变，running root直跑规则不变。
- system/multi/root HOME 不交还 SUDO_UID；只有 user/custom/portable HOME可以。
  install --system 在创建目录前要求root，移除会截断未知文件的固定write probe。
- 缺 patchelf 的首次安装在同一个 InstallPlan 上加 Build-kind 顺序约束，工具自己的
  闭包先于工具，其他节点后于工具；不改payload runtime_deps，不递归install或二次resolve。
  自动注入工具不占用户 install_targets 的槽位，显式工具请求也遵守顺序。
- 发布前将 installed[] 排序，与已有writer规范一致，避免多版本成功保存被误判为失败。
  缺失旧注册不能证明任何旧asset，仍不能授权覆盖；incoming缺注册继续拒绝。
- install/use 共用精确file destination选择：新选中的普通文件取消旧冲突active binding，
  保留installed记录和目录claims。共享目录继续由materializer无损unwrap，不写payload。
- 只有提交后的已删除asset路径才清理空父目录；native rmdir拒绝普通文件，
  symlink/nonempty/固定sysroot形状不移除。doctor对已登记同payload同文件名的
  dangling legacy路径构造准确旧link证明，仍由materializer复查和事务替换。

本地复验：先前9个失败E2E全部实际pass，包括共享destination、双方向目录unwrap、
少asset版本切换、另一个SubOS损坏active的恢复，以及删除确认/未知数据/GC拒绝。
精确更新两个内容断言，仅排除3个已知bookkeeping文件，未宽泛排除未知文件。
SubOS名称预检保留并恢复native明确诊断；AC12新增默认home用户数据不变断言。
subos_events改用本已有fixture index，避免生命周期断言依赖网络同步。
Ubuntu20隔离容器的系统entry+新alice HOME，CN首次安装xz：实际patchelf→glibc→xz
配置，退出0；xz及liblzma均打印5.8.3。Arch正式makepkg/pacman仍需新CI实际通过。

针对性unit实际：session wakeup+packet/FD边界3pass，tool-order3pass，
materializer10pass，HomeContext12pass，cache5pass/1本机bwrap skip。
补充隔离root容器：实际static ldconfig生成、/run禁止其他输出、来源字节与机器用户
文件保留、用户替换cache拒绝，该case实际pass（130ms）；这不替代CI非root隔离验收。
本批最终dev与dist构建通过；delete/header lint、bash parse和diff-check通过。

本地非instrumented容器优化后cold额外中位5.035ms通过10ms预算，hot额外5.961ms
仍超过5ms预算；不放宽门槛、不把该失败报告为通过。ptrace采样只作定位，不作预算证据。
新固定head仍要求三个domain、六个静态性能用例、全部平台与146需求真实执行验收。

按用户再次明确的要求：全部追加提交、普通push；达到技术标准并综合自审后先汇报
给用户review决定是否合入，本轮不合入、不发布。
