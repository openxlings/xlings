# SubOS #641 交付状态

更新：2026-10-08。依赖与验收见 [续行计划](../../plans/2026-10-08-subos-delivery-plan.md)。

| 任务 | 状态 | 证据 / 后续 |
|---|---|---|
| 获取 #641、rebase 最新 main | 完成 | 本地 `review/pr-641-rebased`，base `c55d89a`，无冲突 |
| 原方案审查 | 完成 | [进度报告](../../docs/2026-10-08-pr-641-progress-report.md)，保存原 CI 与本地现场 |
| 输出与 generation 所有权 | 实现，本地回归通过 | 原候选已复现；独占 staging、拒绝既有输出、完整 generation 清单；删除 lint 通过 |
| 严格状态与 boot 未知键 | 实现，本地回归通过 | stage-0 明确恢复路径；status/config/cp/list 拒绝坏状态 |
| 审计故障与 argv 隐私 | 实现，待真实隔离 CI | checked journal、先记录后放行通知、locked 退出 125 |
| root refresh 失败传播 | 实现，本地回归通过 | use/install/remove/init 不再只警告；失败保留 payload，重试生成投影 |
| min_client | 实现，本地回归通过 | 新文件最低 2026.10.8.2；数字版本比较；doctor 检查实际 entry |
| atomic file writer / policy dangling symlink | 实现，本地回归通过 | 独占 staging、错误传播、三平台 no-replace API；Windows 替换待 CI |
| 声明覆盖与执行证据分离 | 实现，本地回归通过 | `--fail-unverified`；同一车道的通过结果才能验证其声明 |
| xdev CI 计划与选择 | 核心实现，本地回归通过 | 共享 C++ 选择及 9 个 app 用例；三平台 CI 加入 app 自测；distro 接入动态三分片，执行结果待 CI |
| proxy / net 事件 | 主体实现，隔离验证待 CI | SOCKS5h 单出口、独立 netns、native syscall 通知；本地 3 个 unit 通过，2 个隔离 e2e 跳过 |
| hook sandbox | 主体实现，隔离验证待 CI | 正式 libxpkg 0.0.61；persistent worker、独立协议、payload shadow、输出日志、先审计后放行；本地 2 个 worker flow 通过，3 个隔离用例跳过 |
| 系统层解析 / store 闭包 | 实施中 | 严格层读取与 resolution evidence；随后收口逐 payload 挂载及 prefix domain |
| glibc cache / Luban 生态 | 两架构资源已发布，客户端验证实施中 | 索引 PR #940 原生 x86_64/aarch64 loader/cache/preload、三平台检查通过；GitHub/GitCode 资源完整 GET+SHA 验证；客户端接入待本批 CI |
| fixture HTTP / 资源锁 | 主体实现，本地通过 | 17 个 xdev app 用例通过；loopback HTTP、跨进程锁、选择后并行执行；三平台 evidence 纳入报告 |
| 性能与升级矩阵 | 趋势工具已本地通过，产品矩阵待完成 | xdev member/platform 历史及下一轮实际 shard 权重通过；不以 skip 替代产品性能证据 |
| 完整 CI、自审、合入、release、GitCode、索引 | 待完成 | 每个环节绑定同一 head；资源出现立即本地补 CN |
| 真实镜像设置 | 完成 | 实际 entry 执行 `xlings config --mirror CN` 返回 `mirror = CN` |

构建记录：第一次产品编译因版本常量误用失败，修正为 `Info::VERSION` 后
`mcpp build` 通过；xdev 构建、版本规范与文档示例检查通过。后续的 atomic writer
变更需要重新构建验证。统一回归首次 84 个测试程序中 82 通过、2 失败；修复非普通文件读取和
scope/generation fixture 后，受影响用例单独通过，最新逐程序证据为 84 pass。
42 个具体测试仍因能力、平台、网络或未通过 xdev 运行而跳过；不计为已验证。
现场：`target/xdev/pr641-hardening/`。当前安全与选择器批次进入 #641；完整发布仍未完成。
xdev 的 6 个选择逻辑、3 个 CLI 用例及报告执行证据用例通过。
libxpkg worker / metadata API 的 LLVM 构建和 GCC 4 个测试程序通过，客户端接入继续。
正式依赖已发布并进入 mcpp-index；客户端 `mcpp build` 成功下载/构建 `xpkg@0.0.61`。
最新限量回归的 protocol 2、worker flow 2、cache 2、network unit 3 个用例通过；
worker/network 5 个真实隔离用例因本机 bwrap 不可用跳过。后续 cache 所有权、net
事件及 resolution evidence 改动仍需回归。macOS generation 并发读日志定位为 readlink EINVAL，`2b78ce6e` 追加有界重试，未放宽
验收条件，macOS run 37709550248 已通过。后续普通 push：`3ea30c71`、`dd118625`、`2b78ce6e`、`88eafe96`。

历史保留：2026-10-08 用户要求后续仅追加 commit、普通 push，不 force push 或改写已推送提交。

事务/流/闭包基础批次完整构建通过（GCC 16.1，build 8），完整客户端回归 98 程序：94 pass / 4 fail / 49 具体用例 skip；四项定位修复的针对性复验通过，逐程序最新证据98 pass；xdev 4程序/18用例pass、0skip。
D2 的 exact mounts 与 broker 更新、D3 真实 namespace producer 仍需完成；strict domain
预检会在生产器未接通时创建前拒绝，镜像测试不再删除宿主 `/xlings`。NDJSON 1.6
stream/cancel 与 root compiler unit 已通过；真实 namespace/GCC consumer 仍待 CI。
