# xlings 2026.8.2.1 多平台用户体验与稳定性深度审计

> 审计日期：2026-08-03（Asia/Shanghai）
> 审计对象：`openxlings/xlings` `main@3b3c0552b0f2d1709fa553d64bd76004bbe2e6c0`
> 公开版本：`v2026.8.2.1`
> 视角：首次安装用户、日常 CLI 用户、CI/脚本用户、AI Agent、跨平台维护者
> 状态：调研完成；本文只报告问题和建议，没有修改产品代码

## 1. 结论先行

xlings 的基础架构和主路径已经具备相当好的产品骨架：公开安装包可以在 Linux x86_64、Linux aarch64、macOS 14/15 arm64、Windows x86_64 上完成 quick install；五个平台都能安装并运行 ninja，完成 SubOS 创建/查询/全局切换/删除，运行 `self doctor`，并通过同一套 NDJSON capability 接口工作。macOS 14 的最低版本承诺也有真实 runner 证明。静态输出层相较 2026-07-29 的旧审计已有明显进步：帮助、列表和信息面板在窄终端不再越界，重定向的静态输出不再带 NUL，`NO_COLOR` 对静态输出有效。

但从“用户能否相信命令结果”的标准看，当前公开版还不能评为稳定。三个问题直接破坏正确性：

1. `xlings list` 会隐藏已安装、可运行、可切换的非 latest 版本；公开 fresh-install 的四个 core 平台持续因此失败。
2. macOS 和 Windows 的 `subos use --sandbox --cmd` 根本不执行命令，却返回 0；README 推荐给 Agent 的核心路径会产生假成功。
3. Linux aarch64 会把明确只支持 x86_64 的包加入安装计划，随后对 d2x、glibc、OpenSSL、bwrap、proot 请求不存在的 aarch64 资源并报 404；包解析没有在下载前拒绝不兼容架构。

因此，当前最准确的产品判断是：

- **包安装与版本管理内核：可用，但库存查询不可信。**
- **Linux x86_64 SubOS：主路径可用；真实 sandbox 已验证。**
- **macOS/Windows SubOS：shell/global 生命周期可用；sandbox 是 HOME 重定向级别，而且 `--cmd` 当前失效。**
- **Linux aarch64：xlings 本体和架构正确的简单包可用；包生态和 sandbox 还没有形成可依赖的闭环。**
- **CLI 静态排版：显著改善；帮助信息、动态进度和语义排序仍需收口。**
- **Agent/NDJSON：接口设计扎实；但 CLI 假成功、列表漏报和文档旧协议会放大自动化风险。**
- **发布体系：构建覆盖强，但 release 绿色不等于用户路径绿色；当前公开版就是反例。**

### 1.1 追加验证：Issue #471 是真实缺陷

2026-08-03 对未修复二进制进行了隔离复现：将 `XLINGS_HOME` 指向一个
尚未 bootstrap 的目录后首次执行包安装，payload 阶段完成，但命令在写
`subos/default/.xlings.json` 时退出 1。根因是 `Config::save_workspace()`
假设目标 SubOS 目录已存在。修复契约是写入前创建所选状态文件的父目录，
并以本地 recipe 验证首次安装退出 0、精确 installed/active 状态落盘；该
回归属于 cold-home 与 release candidate 门禁的一部分。

参考：[Issue #471](https://github.com/openxlings/xlings/issues/471)。

## 2. 审计范围与证据等级

### 2.1 基线确认

- `main` 与 `origin/main` 均指向 `3b3c055`。
- `src/core/config.cppm` 与 `mcpp.toml` 均声明 `2026.8.2.1`。
- GitHub 最新 release 为 [`v2026.8.2.1`](https://github.com/openxlings/xlings/releases/tag/v2026.8.2.1)，发布时间为 2026-08-02。
- Release 提供四种资产：Linux x86_64、Linux aarch64、macOS arm64、Windows x86_64，并生成 SHA256 sidecar。
- 对应 [Release workflow 30732933118](https://github.com/openxlings/xlings/actions/runs/30732933118) 为绿色。

### 2.2 实际验证层级

本报告按可信度从高到低使用以下证据：

1. **公开 release + 原生 OS runner**：临时 Draft PR #470 固定安装 `v2026.8.2.1`，不使用工作区构建产物，在五种原生环境执行相同探针。
2. **公开 release + 本地全隔离 HOME**：使用临时 `HOME`、`XLINGS_HOME` 和净化后的 `PATH` 执行真实 `quick_install` 与 `tests/fresh-install/smoke.sh core`。
3. **官方 post-release fresh-install**：查看 release 后的 workflow_run 与定时重跑，而不是只看发布 workflow。
4. **当前 main 源码与测试定义**：用于解释已复现行为的根因和覆盖空洞。
5. **现有用户文档/内置 skill**：用于判断命令行为与用户承诺是否一致。

临时 PR 只用于取证，未准备合并。证据收集后已经关闭 PR，并删除临时远端分支和本地工作树。两次 run 仍可查看：

- [30757027290：五平台基础 UX 探针](https://github.com/openxlings/xlings/actions/runs/30757027290)
- [30757284022：五平台 SubOS sandbox/command 探针](https://github.com/openxlings/xlings/actions/runs/30757284022)

注意：临时 workflow 的 job 绿色只表示“探针运行并上传结果”，每条产品命令的退出码被单独记录，不能把 job 绿色当成产品全绿。

### 2.3 明确未覆盖的范围

- 没有可用的 macOS Intel、Windows ARM64 原生 runner；当前 release 也没有对应资产。
- 没有做长期并发压力、磁盘耗尽、断电恢复、代理/TLS 中间人、弱网长时重试测试。
- Linux image 模式需要 loop mount/root，未在本次临时 PR 中启用。
- 交互式 TTY 的主观主题观感没有跨实体终端人工检查；本次重点是可复制的字节、宽度和命令语义。
- 没有修改或验证上游 `xim-pkgindex` 的修复，只读取公开客户端实际取得的索引快照。

## 3. 多平台结果矩阵

符号：✅ 成功；❌ 产品行为失败；⚠️ 可运行但语义/承诺有限；— 无此平台或未覆盖。

| 用户路径 | Linux x86_64 | Linux aarch64 | macOS 14 arm64 | macOS 15 arm64 | Windows x86_64 |
|---|---:|---:|---:|---:|---:|
| 固定公开版 quick install | ✅ | ✅ | ✅ | ✅ | ✅ |
| `xlings --version` = `2026.8.2.1` | ✅ | ✅ | ✅ | ✅ | ✅ |
| 搜索索引 | ✅ | ✅ | ✅ | ✅ | ✅ |
| 安装并运行 `ninja@1.12.1` | ✅ | ✅ | ✅ | ✅ | ✅ |
| 安装 `d2x` | ✅ | ❌ 三个资源 404 | ✅ | ✅ | ✅ |
| SubOS new/list/info/global switch/remove | ✅ | ✅ | ✅ | ✅ | ✅ |
| `subos --sandbox --cmd` 真正执行命令 | ✅ | ❌ 后端资源 404 | ❌ 未执行，exit 0 | ❌ 未执行，exit 0 | ❌ 未执行，exit 0 |
| `self doctor` | ✅ | ✅ | ✅ | ✅ | ✅ |
| interface version/list/status | ✅ | ✅ | ✅ | ✅ | ✅ |
| 静态 `NO_COLOR` 帮助无 ESC/NUL | ✅ | ✅ | ✅ | ✅ | ✅；CR 为标准 CRLF |
| 安装进度重定向无控制序列 | ❌ | ❌ | ❌ | ❌ | ❌ |

补充官方 fresh-install 结果：

| 环境 | core | gcc | llvm |
|---|---:|---:|---:|
| Ubuntu 24.04 | ❌ `list` 漏 mcpp | ✅ | ✅ |
| CentOS 7 / glibc 2.17 | ❌ 同上 | ✅ | ✅ |
| Windows | ❌ 同上 | ✅ | ✅ |
| macOS 14 | ❌ 同上 | — 索引无 macOS gcc | ✅ |

这说明 xlings 不是“所有东西都坏了”：公开工具链的安装和运行覆盖有真实价值。但同样说明绿色的 gcc/llvm cell 不能抵消 core 生命周期持续红色。

## 4. 已确认的优势

### 4.1 发布包启动面广，Linux 兼容策略有效

- Linux release 是静态 musl 包；CentOS 7 的 gcc/llvm fresh-install 仍能通过，证明旧 glibc 主机不是纸面兼容。
- macOS release 在 macOS 14 与 15 原生 runner 都能安装运行；常规 CI 还检查 `minos 14.0` 和不依赖系统 libc++。
- Windows quick installer、release 自安装与 PowerShell 环境写入在 Windows runner 上工作。
- Linux aarch64 的公开二进制确实能在原生 ARM runner 运行，不只是 QEMU `--version`。

### 4.2 基本包生命周期跨平台高度一致

五个平台都完成：搜索、安装 ninja、运行 `ninja --version`、列表、SubOS 基础生命周期、doctor、Agent plain list 和 NDJSON 查询。路径分隔符和用户目录也能正确本地化。

Linux 安装 x86_64 d2x 时会自动拉取 glibc/OpenSSL 并完成 ELF relocation，体现 xpkg 依赖编排与自带 runtime 的价值。macOS/Windows 的 d2x 单包路径也成功。

### 4.3 静态输出基础设施已解决旧审计中的大部分底层问题

当前 `src/ui/layout.cppm` 建立了统一宽度、display-width、非 TTY、NUL/CR/尾空格和 `NO_COLOR` 契约；`src/core/palette.cppm` 统一 stdout/stderr 颜色开关和明暗主题。实测结果：

- `NO_COLOR=1 TERM=dumb xlings -h` 在四种 POSIX runner 均为 `esc=0, nul=0, cr=0`。
- Windows 为 `esc=0, nul=0`，32 个 CR 对应正常 CRLF，不是游标重绘。
- 本地以 `XLINGS_TERM_WIDTH=32/40/60/80` 测试 help/search/list/info，所有行都没有超过指定宽度。
- bare `xlings` 现在会显示帮助并返回 0，不再静默退出。
- unknown command 与缺失 required arg 会返回非零；cmdline action 的返回码不再被吞掉。

这些是实质改善，应保留统一 layout/palette 层，不应回退为各 renderer 自己处理。

### 4.4 Agent/程序接口的方向正确

- `xlings interface --version` 稳定输出 `{"protocol_version":"1.0"}`。
- `--list` 当前暴露 19 个 capability，并附带 input/output schema 与 destructive 标志。
- 单次 capability 执行输出 NDJSON data/error/result，未知 capability 会给 `E_NOT_FOUND` 并以 1 退出。
- Windows 提供 `--args-file`，规避 cmd.exe/MSVC CRT JSON 引号问题。
- `xlings agent` 与 `xlings agent usage`/`skills usage` 的内置知识不依赖仓库文件，适合首次安装后的 Agent 自举。

## 5. P0：先修正确性与假成功

### P0-1：`list` 隐藏已安装的非 latest 版本

#### 用户可见行为

隔离 fresh home 中：

1. 安装 `mcpp@2026.7.28.2` 和 `mcpp@2026.7.29.1`。
2. 两个 payload 目录与 `.xpkg-install.json` 都存在。
3. workspace 记录两个 installed 版本，并以 `2026.7.29.1` 为 active。
4. `mcpp --version` 与两次 `xlings use` 正常。
5. `xlings list` 和 `xlings list --all` 都完全没有 mcpp。
6. `xlings info mcpp@2026.7.29.1` 又能正确显示 active 与两个 installed 版本。

官方 [`xlings-ci-fresh-install` run 30733511409](https://github.com/openxlings/xlings/actions/runs/30733511409) 与定时重跑 [30739919536](https://github.com/openxlings/xlings/actions/runs/30739919536) 在 Linux、CentOS 7、macOS、Windows 四个 core cell 上重复失败。已存在 issue [#456](https://github.com/openxlings/xlings/issues/456)。本次复现进一步排除了 issue 初始描述中的一个猜测：两个 mcpp 版本实际上都在 payload 和 workspace 中，不是旧版本被替换。

#### 根因

`cmd_list` 不是从 workspace installed 集合枚举库存，而是先调用 `catalog.search()`。`PackageCatalog::build_matches_()` 对每个包只通过 `select_version_()` 选出一个版本；无显式 hint 时优先 `latest` ref。随后：

```cpp
if (!match.installed) continue;
if (!all && !in_current_subos(match.name, match.version)) continue;
```

当前索引中 mcpp `latest -> 2026.8.2.2`，而 fresh test 安装的是两个旧版本，因此唯一的 search match 被判为未安装，整个包被删除。`--all` 只跳过 subos 交集，不会改变 catalog 只生成 latest match 的事实。

受影响的不只是 TUI：`--agent list` 与 interface `list_packages` 复用同一路径，也会漏报。

#### 用户影响

- 用户无法用最自然的库存命令确认安装结果。
- 自动化可能重复安装、错误清理或错误判定迁移状态。
- `self doctor` 可以说状态 OK，而 `list` 同时说包不存在，破坏工具可信度。
- 发布后的核心 smoke 持续红色，其他真实回归会被红色背景噪声掩盖。

#### 建议修复边界

库存应以 xlings 自己持久化的事实为源：当前 subos 的 `workspace.installed[]`、all 模式下各 subos 引用集合及 payload/store 状态。catalog 只负责为每个已安装的精确版本补充描述，不应先选择 latest 再推断库存。

验收门槛：

- 安装两个非 latest 版本后，默认 list、`--all`、`--agent`、interface 均能看到正确包和版本状态。
- latest 未安装、latest 已安装、索引已删除旧版本、payload 缺失、跨 subos 共享五种情况都有测试。
- 修复 [#456](https://github.com/openxlings/xlings/issues/456) 后，两次 fresh-install core 全部恢复绿色。

### P0-2：macOS/Windows `--sandbox --cmd` 不执行命令却返回 0

#### 原生证据

探针：

```text
xlings subos use ux-audit --sandbox --cmd <print mode and home>
```

- Linux x86_64：打印 `mode=sandbox home=/home/runner`，exit 0。
- macOS 14/15：没有 marker；stderr 显示交互 bash 的 “no job control” 和提示符，stdin EOF 后 exit 0。
- Windows：没有 marker；stdout 显示 PowerShell banner/提示符和 “entering subos”，stdin EOF 后 exit 0。

这不是平台隔离等级不同本身造成的，而是参数语义丢失。`subos::run` 正确解析 `--cmd` 并传给 `sandbox::enter`；Linux 构造 bwrap/proot argv 时使用 `cmd`，但 macOS 分支固定 `execl(shell, "-i")`，Windows 分支固定 `cmdline = exe`，两者都没有读取 `cmd`。

#### 用户影响

README 直接把此命令推荐为 Agent/不可信代码的一次性执行方式。CI 或 Agent 会认为任务成功，实际任务一行都没有执行。这比清晰报“不支持”更危险。

#### 建议修复边界

- macOS HOME-redirect 路径使用 `shell -c <cmd>`，并原样传播退出码。
- Windows 复用非 sandbox 的 CreateProcess command helper，按 pwsh/powershell/cmd 正确构造一次性命令。
- 若某平台暂不支持该组合，必须在解析期非零退出，不能打开交互 shell。
- 测试必须断言 marker 出现、`exit 37` 传播为 37、命令没有执行时不能返回 0。
- 帮助与 README 要明确 macOS/Windows 是 HOME/USERPROFILE 重定向，不是 Linux 的完整文件系统视图隔离。

### P0-3：aarch64 不兼容包被解析为可安装，最后以多个 404 失败

#### 原生证据

Linux aarch64：

```text
xlings install d2x -y -g
```

安装计划接受 `glibc@2.39`、`openssl@3.1.5`、`d2x@2026.08.02.2`，三个下载都 HTTP 404。随后 sandbox 自动安装 `bwrap@0.11.2`，404；再回退 `proot@5.4.0`，仍 404，最终无法启动 sandbox。

公开索引快照中 d2x、glibc、OpenSSL 的 `archs` 都只声明 `x86_64`。客户端仍按 `linux-aarch64` 拼接资源 URL，说明平台匹配只筛到了 `linux`，没有在解析/计划阶段执行架构兼容性门禁。

同一 ARM runner 上 ninja 成功，证明不是 aarch64 xlings 二进制、DNS 或 GitHub runner 普遍故障。

#### 用户影响

- 用户先看到完整安装计划和长时间 0% 动画，最后才得到三组 404 与二次注册错误。
- 错误看起来像镜像故障，实际是包根本不支持该架构。
- sandbox 作为 xlings 核心能力，在已发布的 Linux aarch64 平台不可用。
- 安装失败后继续执行 install/config audit，产生“download missing”“declared programs not registered”等派生噪声，掩盖首要原因。

#### 建议修复边界

- resolver 在生成 PlanNode 前同时校验 OS 与 arch；不兼容时给单一、可行动错误，如 `d2x has no linux-aarch64 artifact; supported: linux-x86_64, ...`。
- 依赖图也必须做相同校验，不能只校验根包。
- 只有确实发布 aarch64 payload 后，d2x/bwrap/proot 才应在 ARM 搜索/安装路径显示为可用。
- 下载失败后不得继续执行 install/config hook，也不应再产生注册缺失的级联错误。
- 正式 aarch64 CI 至少增加 native quick install、架构兼容包安装、明确不兼容包 fail-closed、sandbox backend 四项。

## 6. P1：统一 CLI 契约与发布门禁

### P1-1：帮助系统与真实 parser 已明显漂移

当前帮助由手写 `SubHelp`/`print_help` 表维护，而真实命令和 option 又在 cmdline builder、`subos::run`、`xself::run` 中维护。实测漂移包括：

- 顶层帮助遗漏真实可用的 `interface` 和 `profile`。
- 顶层 OPTIONS 遗漏 `--help` 与 `--version`。
- `list -h` 遗漏真实的 `-a, --all`。
- `use -h` 仍说省略版本且多候选时 “exit 2”，真实行为是列出版本并 exit 0。
- `subos -h` 把子命令列在 OPTIONS 下，遗漏 `stop`、别名以及 `new/use` 的绝大多数参数。
- `self -h` 同样把子命令列为 OPTIONS；`self doctor -h` 只能重复父级帮助。
- `subos use -h` 只能重复父级帮助，看不到 `--global/--shell/--sandbox/--cmd/--keep/--ttl/--gpu`。
- `self doctor --bogus` 静默忽略未知参数并 exit 0；大部分 cmdline 子命令会明确报 unknown option。
- 32 列帮助虽不越界，却把关键 usage 截成 `xlings [OPTIONS] [SUBCOMMAND`，成为不可复制的语法。

建议建立单一 CommandSpec：parser、顶层帮助、子命令帮助、Agent skill 摘要和 completion 都从同一描述生成。对仍需手写解析的 `subos/self/profile/agent`，也至少先声明完整 spec，再由 handler 消费。增加全命令 help golden test，并检查文档提到的 option 都出现在 `-h`。

### P1-2：动态安装进度仍污染 pipe、CI 日志和 Agent 输出

静态输出已经遵守非 TTY 契约，但下载进度没有：`render_download_progress()` 无条件在每一帧末尾追加 `ESC[J`；`canRewrite=false` 只阻止 cursor-up，并不阻止 clear-screen 控制序列或 200ms 一帧的重复快照。

临时 runner 把每条安装命令的 stdout 重定向到文件，仍得到：

- ninja：每个平台 8–11 个 `ESC[J`。
- d2x：Linux x86_64 15 个、macOS 6–7 个、Windows 11 个；aarch64 失败路径有 110 个。
- aarch64 sandbox backend 失败路径单段有 204 个 `ESC[J`，约 19 KiB 重复 0% 帧。

quick installer 自身在 POSIX 也无条件输出颜色；Linux/macOS 安装日志分别有 42–50 个 ESC，curl progress 还输出 CR。Windows installer相对干净，只保留标准 CRLF。

建议：

- 非 TTY/`--agent`/`NO_COLOR` 时完全禁止游标控制字符。
- 非 TTY 只在阶段变化或固定低频率输出一行摘要，最终输出一次结果。
- `render_download_progress` 只有 `canRewrite` 为真时才追加 cursor/erase 序列。
- quick_install shell 同样探测 TTY/NO_COLOR，并给 curl 使用 `--silent --show-error` 或可读的低频摘要。
- 对 install 的 stdout/stderr 做字节级回归：ESC=0、NUL=0、无重复整帧。

### P1-3：发布绿色与真实用户绿色之间存在结构性窗口

`release.yml` 成功后才触发 floating-latest fresh-install。因此它可以创建公开 release、发布索引、镜像并 bump index，然后 post-release core 才失败。`v2026.8.2.1` 正是这种状态：Release run 绿色，紧随其后的 core 四平台红色；定时重跑仍红。

此外：

- macOS/Windows 常规 CI 中 quick-install smoke 是 `continue-on-error`。
- aarch64 正式 CI 只交叉构建并用 QEMU 跑 `--version`。
- release 的 mirror 与 index bump 有预期的 best-effort/人工补齐流程，workflow 绿色不能单独代表全球可安装。

建议发布分两阶段：

1. 生成候选资产与候选索引，跑完整 cold-home core/toolchain matrix。
2. 只有候选通过才创建正式 release 和更新 `latest`。

post-release 定时 smoke 仍保留，用于发现镜像/索引随后漂移。若 GitHub Actions 约束使候选流程难以一步完成，至少把 fresh failure 自动写入 release summary/issue，并让发布状态明确为 degraded，而不是无人消费的红色 workflow。

### P1-4：quick install 的目标选择与完整性验证不够明确

#### 新 `XLINGS_HOME` 可能被 PATH 中旧安装抢占

本地隔离复现时，即使设置了新的临时 `HOME` 和 `XLINGS_HOME`，只要 PATH 仍能找到宿主已有 xlings，`self install` 就检测旧安装并尝试原地重装。只有同时净化 PATH 后才进入新临时 home。

源码原因：`detect_existing_home()` 仅当 `$XLINGS_HOME/bin` 与 `$XLINGS_HOME/subos` 已存在时才接受显式环境变量；对一个尚未创建的新目标路径，它会继续执行 `command -v xlings`/`where xlings` 并优先旧安装。

显式 `XLINGS_HOME` 应有最高优先级，即使目录尚不存在；若目标看起来不安全，再清晰拒绝。升级“当前 PATH 中的安装”应是未指定目标时的便利行为，不应覆盖用户明确目标。

#### 发布了 SHA256，却没有在 installer 中使用

release workflow 为四个资产生成 `.sha256`；shell installer 只检查 gzip magic，PowerShell 只检查 ZIP magic。下载内容若被缓存、镜像或链路错误替换为另一个合法压缩包，installer 仍会执行其中二进制。

建议从同一 source 下载 sidecar，验证 hash 后才解压；fallback 时每个 source 的包与 checksum 必须绑定。无法取得 checksum 应默认失败，若为了兼容提供显式跳过开关，应高亮警告。

#### 安装后提示硬编码 bash

Linux/macOS 总是提示 `source ~/.bashrc`，即使用户当前是 zsh/fish。安装逻辑其实能写多种 profile，最终提示应根据 shell 给正确命令，或只说“restart shell”并列出实际修改的文件。

### P1-5：公开的跨平台 sandbox 承诺需要降噪并精确定义

README 用“across Linux, macOS, Windows”和“untrusted code…host untouched”描述 SubOS；内置 usage skill 把 `--sandbox` 统一称为 full filesystem isolation。真实实现是：

- Linux：bwrap/proot 文件系统视图隔离；image/tmpfs 依赖 bwrap。
- macOS：HOME/TMPDIR/XDG 重定向，没有 namespace/ptrace 文件系统视图。
- Windows：USERPROFILE/APPDATA/TEMP/XDG 重定向，没有 Linux 等价的文件系统边界。

设计文档中的跨平台表比 README 更诚实，但也已落后：它说 Windows 不支持 sandbox，当前源码实际实现了 HOME redirect，只是 `--cmd` 失效。

这不是要求三平台强行实现同一种内核机制；重点是用户必须知道安全边界。建议 README、quick start、内置 skill、`subos -h` 都展示平台 capability table，并明确 HOME redirect 不适合执行真正不可信代码。

## 7. P2：可发现性、信息密度与文档一致性

### P2-1：`info` 的版本展示无序、泄漏内部 sentinel，非 TTY 单行过长

`xlings info mcpp@2026.7.29.1` 的 available 字段：

- 版本顺序近似 unordered map 遍历，没有语义排序。
- `2026.*` 与 `0.0.*` 混杂。
- 把内部 `res_versioned` 当成普通版本展示。
- 非 TTY 时所有版本放在一个 2867-byte 行；虽然“不截断”保留了信息，但日志几乎不可读。
- `xlings info mcpp` 选择索引 latest 后显示该版本 `installed no`，容易让用户误以为整个包没安装；精确版本查询才显示旧版本已安装。

建议把 package-level installed 与 selected-version installed 分开；过滤内部键；使用稳定版本排序；默认显示 latest/active/installed 和最近若干版本，完整集合放到 `--all-versions` 或结构化接口。非 TTY 也应按语义换行，而不是把“无限宽”理解为“一行无限长”。

### P2-2：错误渠道与格式不统一

- cmdline parser 的 `Error: unknown option` 出现在 stdout。
- SubOS 手写 parser 的 `[error] ...` 出现在 stderr，并附 hint。
- Agent renderer 又使用 `Error:`/`Hint:`。
- unknown command 输出 `[error]` 后追加整页 help；missing required arg 只输出单行 `Error:`。
- 安装不兼容架构时先产生 HTTP 404，再产生 hook 与注册错误，缺少单一主因。

建议统一规则：机器可判断的失败都以非零退出；诊断写 stderr；人类错误有稳定 prefix/code/hint；结构化接口保持 E_*；派生错误不得淹没首因。

### P2-3：用户文档存在旧命令与旧协议示例

- `docs/quick-start/multi-version.md` 仍出现 `subos create`/`subos enter`，当前 CLI 是 `new`/`use`。
- `docs/quick-start/subos-and-agent.md` 给出的 NDJSON 示例使用 `{"action":"subos.new"...}` 作为请求；当前 interface 是 `xlings interface <capability> --args ...`，stdin 的 `action` 只用于 cancel/pause/resume/prompt-reply。
- 多份文档标注版本 `0.4.36`，而产品已进入日期版本，平台行为已变化。
- 顶层 README、中文 docs、内置 skill、CLI help 各自维护命令摘要，已经出现不同步。

建议增加文档命令 lint/smoke：抽取 fenced command，在公开版或 fixture index 上至少验证 parser；对 interface 示例做 NDJSON schema test。人类文档和内置 skill 应引用同一个生成的 command/capability 清单。

### P2-4：公开支持矩阵缺少架构维度

当前资产实际支持：Linux x86_64/aarch64、macOS arm64、Windows x86_64。quick installers 能识别 macOS x86_64 与 Windows ARM64，却没有对应 release 资产；用户只会在 source probe 阶段看到“不可用/asset missing”，README 没有提前说明。

建议在 Quick Start 前放 OS × arch × 最低版本矩阵；installer 对“OS 支持但 arch 无资产”给直接错误，不要把两个 release source 都描述成网络不可用。

### P2-5：POSIX quick installer 的四段日期版本排序只显式比较前三段

shell installer 使用：

```text
sort -t. -k1,1n -k2,2n -k3,3n
```

它没有把日期版本的第 4 段 `N` 作为数值 key。当前 `.1/.2` 通常会被 sort 的整行 fallback 恰好排对，但 `.9/.10` 会有潜在歧义；PowerShell 的 `[version]` 则明确处理四段。应增加 `-k4,4n` 并测试两个镜像处于同一天不同 revision 的情况。

## 8. 用户旅程评估

### 8.1 首次安装

**好的部分**：双源探测、按延迟选源、下载失败 fallback、macOS quarantine 清理、Windows profile/PATH 写入都工作；公开版本在五个 runner 约 2–7 秒内完成安装。

**阻力**：POSIX 日志无条件 ANSI/curl progress；没有 checksum 验证；新 XLINGS_HOME 会被 PATH 中旧安装抢占；shell 提示硬编码 bash；支持架构未前置说明。

### 8.2 搜索、安装、运行

**好的部分**：搜索快，安装计划清晰，ninja 五平台闭环；d2x 在四个非 ARM 环境闭环；依赖与 runtime relocation 在 Linux x86_64 可工作。

**阻力**：架构不兼容不 fail closed；失败后级联错误太多；非 TTY progress 极度冗长；安装成功后 `list` 不一定承认安装。

### 8.3 多版本切换

**好的部分**：两个 mcpp 版本可并存、bare-name 多候选查询 exit 0、精确 use 可切换、active 信息在精确 `info` 中正确。

**阻力**：help 仍描述旧 exit 2；inventory 与 xvm/workspace 事实矛盾；`info <bare>` 的 latest 视角容易误导已安装状态。

### 8.4 SubOS

**好的部分**：五平台创建、列表、信息、全局切换、恢复 default、删除、doctor 一致；Linux x86_64 sandbox 可自动安装 bwrap 并执行一次性命令。

**阻力**：帮助几乎不暴露真实 option；macOS/Windows sandbox command 假成功；aarch64 后端不存在；公开安全承诺没有按平台限定。

### 8.5 CI 与 Agent

**好的部分**：`--agent` plain list、内置 skills、19-capability NDJSON、稳定 result/exitCode 构成良好自动化底座。

**阻力**：Agent 依赖的 list 也漏包；sandbox command 可假成功；动态 progress 仍输出控制序列；quick-start 的 interface 示例是旧协议；发布状态没有把 post-release 红灯传回用户。

## 9. 建议实施顺序

### 批次 A：恢复“结果可信”（阻断后续普通发布）

1. 修 `list` 的库存数据源，关闭 #456，跑绿四平台 fresh core。
2. 修 macOS/Windows sandbox `--cmd` 执行和退出码；不支持时 fail closed。
3. 在 resolver 加 OS+arch 兼容门禁；停止失败后的 hook/config 级联。
4. 给 Linux aarch64 增加 native 运行门禁，至少覆盖 ninja、一个明确不兼容包、sandbox backend。

### 批次 B：收口自动化输出和发布门禁

1. 非 TTY/Agent progress 完全去控制序列、降频。
2. 在正式发布前跑候选资产 cold-home matrix。
3. quick installer 校验 SHA256，尊重显式 XLINGS_HOME，改善 shell/profile 提示。
4. 把平台 sandbox 安全边界写进 README/help/skill。

### 批次 C：降低学习成本

1. 用单一 CommandSpec 生成 parser/help/completion/skill 摘要。
2. 修完整嵌套帮助与 unknown option。
3. 稳定 `info` 版本排序、过滤 sentinel、语义换行。
4. 清理旧文档命令与旧 interface 示例；增加示例执行测试。

## 10. 推荐的发布验收门槛

下一次宣称“多平台稳定”前，建议至少满足：

- release candidate 在 Linux x86_64、Linux aarch64、macOS floor、Windows x86_64 上从空 HOME quick install。
- `search -> install -> run -> list -> info -> use -> remove` 全链路一致，list 与持久化状态逐项核对。
- `subos new -> --cmd -> --sandbox --cmd -> info -> remove`；每个平台明确其隔离等级，marker 与退出码均断言。
- Agent plain output 与 NDJSON 各跑一次相同只读和失败场景。
- 所有非 TTY 输出字节审计：无 NUL、无游标控制序列、颜色遵守 opt-out、日志不产生高频整帧。
- quick installer 下载后验证 release sidecar SHA256。
- fresh-install core/gcc/llvm 没有红色 cell；不允许把持续已知红当背景噪声。
- GitCode mirror 用 GET 验证；index `latest` 与四个平台 hash 已发布；这些结果进入 release summary。

## 11. 可直接拆分的工作项

| 优先级 | 工作项 | 所属仓库 | 最小验收 |
|---|---|---|---|
| P0 | list 按 installed exact versions 枚举 | xlings | #456 四平台 core 绿 |
| P0 | macOS/Windows sandbox `--cmd` 与退出码 | xlings | marker + exit 37 原生测试 |
| P0 | OS/arch 兼容解析 fail closed | xlings/libxpkg | ARM 不再发出无效下载 |
| P0 | 补齐或明确拒绝 ARM d2x/glibc/OpenSSL/bwrap/proot | xim-pkgindex/resources | 原生 ARM 安装路径明确成功或明确 unsupported |
| P1 | 非 TTY progress 无 ESC/重复帧 | xlings | 五平台字节测试 |
| P1 | 候选 release fresh-install 前置门禁 | xlings CI | 公开 latest 前 core 全绿 |
| P1 | quick installer SHA256 验证 | xlings | 双源包/sidecar 绑定验证 |
| P1 | CommandSpec/完整 help | xlings | 全命令 help golden |
| P1 | 平台隔离安全边界文档 | xlings docs/skill | README/help/skill 同一矩阵 |
| P2 | info 稳定排序与摘要 | xlings | 无 sentinel、稳定快照、合理行宽 |
| P2 | 文档命令/NDJSON 示例 smoke | xlings | 示例可由当前公开版执行 |

## 12. 最终评价

xlings 现在最有价值的部分不是“所有平台已经完全一致”，而是它已经有统一的包模型、版本视图、SubOS 生命周期、公开静态发行物和 Agent interface，这些骨架在真实五平台上大多能跑。当前风险集中在边界层：catalog 把 latest 查询误当库存、平台分支吞掉 `--cmd`、resolver 不拒绝不兼容架构、动态 renderer 没有完全服从非 TTY 契约、发布流程没有让用户 smoke 反向阻断公开 latest。

这些问题都不要求推翻架构。优先把“命令说成功就一定做了、命令说已安装就一定看得到、平台不支持就下载前明确拒绝”三条产品契约做成 CI 门禁，xlings 的一致性和可信度会有一次明显跃升；随后再统一帮助、文档和输出密度，易用性才会和现有技术能力匹配。
