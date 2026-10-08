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
| xdev CI 计划与选择 | 核心实现，本地回归通过 | 共享 C++ 选择及 9 个 app 用例；三平台 CI 加入 app 自测；动态矩阵及资源锁继续 |
| proxy / net 事件 | 待实现 | 依赖边界稳定后实施，仍是完整交付缺项 |
| hook sandbox / 系统层解析 / store 闭包 | 待实现 | libxpkg、xim-pkgindex 的独立工作树已建 |
| glibc cache / Luban 生态 | 待实现 | 需资源、recipe revision 与实际根内验证 |
| fixture HTTP / 资源锁 / 性能与升级矩阵 | 待实现 | 不以 skip 或声明覆盖替代真实证据 |
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
