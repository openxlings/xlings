# PR #641 自我 review（Part 1 / 2 / 3 综合，2026-10-09）

- 范围：PR #641 全部 165 个提交（423 个文件，+49,571 / −5,257），重点是 Part 3（C43–C53）以及它与 Part 1、Part 2 的接缝。
- 依据：三份设计文档、`tests/requirements.toml`（163 required / 3 deferred / 0 planned）、三平台 CI、本地全量测试。
- 结论放在最前面；每一条发现都写清已修复还是保留，以及证据。

## 0. 结论

Part 3 的十一项（C43–C53）全部落地，没有用"文档声称"代替实现的地方。三处没有做到设计原意，分别是：

| 项目 | 现状 | 理由 |
|---|---|---|
| vz 承载的 VMM | 承载侧完整（契约、生命周期、授权），由假助手验证；签名的 `xlings-vm` 助手本身没有交付 | 需要 Virtualization.framework 的 entitlement 签名，且托管 macOS 无法嵌套虚拟化。拒绝信息如实说明"尚未发布" |
| Windows 上加入运行中的会话（join） | Windows 有 Job Object 进程范围（整树、超时 124、退出码），没有 supervisor | 需要基于 CreateProcess 的 supervisor，这是下一步，没有对外声称已完成 |
| openkal 接入产品代码 | 两个问题都有实测结论；没有把 `kal_*` 接入 xlings | 目前没有任何一项可移植功能会因此变好；第一项有意义的接入随 Luban 内核一起做（§19 C53） |

## 1. 自我 review 中发现并已修复的缺陷

| # | 发现 | 怎么发现的 | 修复 |
|---|---|---|---|
| R1 | C45 的"payload 已不存在"检查让 `subos export` 自己提交的代被拒（导出镜像的链接指向逻辑路径） | 静态车道 RootExport | `b7040ad`：提交只校验树；回滚才校验 payload |
| R2 | 3000 payload 的切换在 dev 构建下 11.1 ms | Linux-root 车道 | `94041bd`：解析改为不用流，payload 只 lstat 一次，dev 构建 3.0 ms |
| R3 | 承载的终端命令依赖 guest 里的 `/usr/bin/env`，而镜像没有用户态 | 假 wsl.exe 的生命周期测试 | `__carrier-env` |
| R4 | interface 的 `list_subos` 有一份自己的副本，缺少 `kind` | C48 测试 | CLI 与 interface 共用一个发射函数 |
| R5 | `luban-init` 作为根包目标时链接了整个前端（6812 个符号） | `nm` | 改为独立包，链接后 0 个前端符号 |
| R6 | 模块中的 `inline constexpr` 变量只在使用它的翻译单元中发射，只链接 modules 的二进制无法链接 | 单独链接 `luban-init` | 去掉 `inline` |
| R7 | macOS 会话从 `/proc/self/exe` 取自身路径 | macOS 车道 125 | 改用 `platform::get_executable_path` |
| R8 | `subos start` 在 macOS 上仍然拒绝（"needs Linux"） | macOS 车道 | 有会话宿主的平台（Linux、macOS）都允许 |
| R9 | 记录"SubOS 在哪里运行"的文件读不出来时被当作本地处理 | 按 AGENTS.md 的"读不到不等于空"规则逐条检查 | 改为拒绝（新增测试） |
| R10 | vz 拒绝时给出的路线指向一个索引里不存在的包 | 自查用户可见文案 | 文案说明"尚未发布" |
| R11 | `lint_platform_headers.sh` 从来没有接入 CI | C43 | 所有 `tools/lint_*.sh` 都进 CI |
| R12 | 测试从调用方继承了 `XLINGS_SUBOS_MODE`（在 SubOS 里运行测试会失败） | 本地运行 | 测试显式设置该变量 |
| R13 | macOS 的 bsdtar 列表格式与 GNU 不同 | macOS 车道 | 测试同时识别两种格式 |
| R14 | openkal 探针放在 `tests/` 下，被 `mcpp test` 当作测试 | macOS 车道 | 移到 `tools/` |

## 2. 分角度检查

**架构。** 依赖方向由 lint 强制：`luban → modules`，`modules` 不依赖 `luban` 和前端。后端分派已移出 core。工具解析只有一张表，提权只有一个入口。矩阵由各实现的声明生成。
遗留：`src/core/subos/sandbox.cpp` 仍有三处按后端判断的连接代码（bwrap 的 seccomp 过滤器、broker socket 的挂载、root view），都属于连接而非策略，下一步可以移进各实现的 `launch`。

**稳定性。** H1/H2/M1/M2 已修复并各有测试。持久化的刷盘顺序由 trace 断言。保留与释放有 CLI 级测试。凡是读不到的状态，一律当作"仍被引用"或"拒绝"，没有一处当作空。

**简洁。** 没有新增顶层名词；承载没有新协议，复用 NDJSON interface；Intent 没有引入第二套策略语义（872 个用例的 golden 证明逐字节一致）。

**用户体验。** 用法没有变化。拒绝时都给出确切的路线，agent 模式不会停下来提问。`subos list` / `status` / `doctor --isolation --json` 都显示承载、ABI 和视图。

**兼容性与无感升级。** `instance.json` 只追加字段，旧客户端忽略。旧的代没有 inventory 时回退到完整遍历校验。`links.tsv` 格式不变。保留账本只在第一次需要时创建。
策略文件的最低客户端版本提到 `2026.10.9.1`：`2026.10.8.2` 从未发布，没有任何已发布客户端写过这个值。
`list_subos` 在某个 SubOS 的种类读不出来时，现在返回 1 而不是 0。这是按"读不到不等于空"做的有意改动。

**跨平台。** Linux 零行为变化，由 golden 证明。macOS 第一次有了会话宿主（分帧的流式 socket、`/tmp` 短链接），三平台 CI 实测。Windows 有 Job Object 范围和 E2E-08（有 WSL2 时测生命周期，没有时测拒绝及其路线）。

**一致性。** 需求 ID 中 163 项 required、3 项 deferred（都写了原因）、0 项 planned。macOS/Windows 会话由各自的工作流把关，Linux 报告中明确记为"由其他工作流把关"。

## 3. 交接时遗留问题的现状

| 交接遗留 | 现状 |
|---|---|
| F12 证据接线 | `24cb3ae9` 的 CI 已全绿 |
| libxpkg 0.0.62 依赖链 | #47 已 review 并合入（`912720f`），客户端固定到 main 上的该提交。三平台源码索引的 0.0.62 条目仍待发布（客户端走 git rev，不依赖这一步） |
| `--observe-raw`、PR 关键路径 ≤15 分钟 | 不在 Part 3 范围内，仍未做 |
| macOS / Windows 的隔离只是 advisory | 由承载提供强隔离：wsl2 已可用；vz 等待助手发布 |
| 私有域删除时内层成功、外层失败的恢复 | 未改动，仍是已知限制 |
