# xlings interface — 公共 API 设计 v1

**Status**: Draft v1 (2026-04-25)
**Scope**: 把 `xlings interface` 子命令定型为 xlings 包管理器的稳定公开 API，供任何外部应用（xstore、Web、CI、Agent、第三方 GUI 等）通过 stdio + NDJSON 协议接入。
**Non-goal**: 不取代 `xlings <subcommand>`（终端 TUI 路径），interface 仅服务程序化调用方。

---

## 1. 设计定位

xlings interface 等价于一个**进程间 RPC 库的 ABI**：

- 调用方进程 spawn `xlings interface <capability> --args <json>`
- 通过 stdin/stdout 双向流式通信
- 协议契约由本文定义；任何客户端实现都不得依赖未声明的字段

### 1.1 我们设计的是 *什么*

**一组关于"包管理"领域的最小正交基础原语 (primitives)**，每个 capability 对应一个 _动词 × 名词_ 组合（install × packages、create × subos、list × packages 等）。

### 1.2 我们设计的 *不是* 什么

❌ **不是为 xstore 定制的端点集**。我们不会出现 `search_packages_for_explore_page` 这种 capability。
❌ **不是 facade 层**。我们不会把"装包 + 拉索引 + 重建 shim"打成一个高层端点；这种组合属于客户端职责。

### 1.3 设计驱动力

| 优先级 | 驱动力 |
|--------|-------|
| 1 | **xlings 自身能力的标准化映射** —— 每个 capability 都是 xlings 领域模型里一个独立、稳定、自洽的语义节点 |
| 2 | **第一类客户端必须能完整组合达成自身需求** —— xstore（当前 + 完全迁移后 + 未来可预见需求）是 v1 的兑现底线；任何 xstore 用例如果 v1 原语组合后达不成，要么补 missing primitive，要么协议有缺陷 |
| 3 | **第三方客户端的组合自由** —— bash + jq、Python、Web Agent、CI runner 等通过相同协议接入，协议设计不能给他们造成天花板 |

**关键约束**：当 "对 xstore 友好" 与 "原语正交清晰" 冲突时，**总是选后者**。xstore 多写几行组合代码 ≪ 协议被特化污染的代价。

---

## 2. Primary 设计原则

| ID | 原则 | 说明 |
|----|------|------|
| P1 | **Protocol-first** | 协议 spec 是 source of truth，先于实现；本文档签入仓库后任何破坏性修改需走 versioning |
| P2 | **Schema-driven** | 每个 capability 三件套：inputSchema / outputSchema / eventSchema；`--list` 输出完整 schema，客户端可自动生成类型 |
| P3 | **协议与渲染彻底分离** | 协议只承载业务语义；TUI 渲染原语（styled_list / info_panel）不进 wire format |
| P4 | **Streaming by default** | 任何耗时 > 200ms 的 capability 必须在执行期间持续 emit 事件，不存在"中间静默"的实现 |
| P5 | **Versioned & additive** | 协议有 `protocol_version` 字段；additive 改动 minor bump，breaking 改动 major bump，至少维护一个 major 周期的兼容 |
| P6 | **Composable lifecycle** | 每次 invocation 是一个 task：query / cancel / pause / resume 全部走协议，客户端不依赖 PID/信号 |
| P7 | **Errors are first-class** | 错误是结构化 ErrorEvent + 枚举 code；客户端不 grep stderr |
| P8 | **Primitives, not endpoints** | 每个 capability 是 _动词 × 名词_ 的最小正交单元；高层场景由客户端组合多个原语完成 |
| P9 | **No client specialization** | 协议设计不允许出现"为 X 客户端定制"的 capability、字段或 dataKind；标准化 xlings 自身能力即可 |

---

## 3. 传输层

### 3.1 调用形式

```
xlings interface <capability> [--args <json>] [--protocol <N>] [--task-id <id>]
xlings interface --list           # 输出所有 capability 的完整 spec
xlings interface --capability <name>   # 单个 capability spec
xlings interface --version        # 协议版本，如 "1.0"
```

| 流向 | 内容 |
|------|------|
| stdin | 控制命令（NDJSON 行，§3.4），可选 |
| stdout | 事件流（NDJSON 行，§3.3） |
| stderr | diagnostic 日志，**不在协议范围**，客户端可忽略 |
| exit code | `0`=success / `1`=capability error / `130`=cancelled / `2`=protocol error |

### 3.2 协议版本

- 协议主版本：`1`（本设计）
- 客户端启动时可传 `--protocol <N>`；xlings 不支持该版本时立即 emit `error{code:E_PROTOCOL}` 后退出
- `xlings interface --version` 输出当前默认协议版本（如 `"1.0"`）

### 3.3 事件类型（stdout NDJSON 行的 `kind`）

每行是一个 JSON 对象，`kind` 字段必填。

| kind | 何时 emit | 字段 |
|------|----------|------|
| `progress` | 长操作阶段切换 / 百分比刷新 | `phase: string`, `percent: 0..1`, `message?: string` |
| `log` | 调试 / 信息日志 | `level: "debug"\|"info"\|"warn"\|"error"`, `message: string` |
| `data` | 业务数据点 | `dataKind: string`, `payload: object`（结构由 dataKind 决定，见 §6） |
| `prompt` | xlings 需要询问客户端 | `id: string`, `question: string`, `options?: string[]`, `defaultValue?: string` |
| `error` | 出错（可恢复或不可恢复） | `code: ErrorCode`, `message: string`, `recoverable: bool`, `hint?: string` |
| `heartbeat` | 5s 内无其他事件时 | `ts: ISO8601 string` |
| `result` | **终止事件**，每次调用必为最后一行 | `exitCode: int`, `summary?: string`, `data?: object` |

设计约定：
- `result` 是终止事件，客户端读到后可立即 EOF
- 其他事件可任意混排
- `prompt` 必须有 `defaultValue`；客户端在 5s 内不通过 stdin reply 时，xlings 用 default 继续
- xlings 在 5s 内没有任何输出（无 progress / data / log）时必须 emit `heartbeat`

### 3.4 控制通道（stdin）

stdin 接受 NDJSON 控制行，xlings 主进程异步读取：

```json
{"action":"cancel"}
{"action":"pause"}
{"action":"resume"}
{"action":"prompt-reply","id":"<prompt-id>","value":"<answer>"}
```

| action | 行为 |
|--------|------|
| cancel | 设置 CancellationToken；capability 优雅清理；emit `error{code:E_CANCELLED}` 后 emit `result{exitCode:130}` |
| pause | 仅 `capabilities.pause=true` 的 capability 接受；暂停下载/解压等可中断操作 |
| resume | 配对 pause |
| prompt-reply | id 必须匹配最近一次未应答的 prompt；不匹配 → emit `error{code:E_PROTOCOL}` |

未知 action → `error{code:E_PROTOCOL}`，**不退出**进程。

---

## 4. Capability spec 格式

`xlings interface --list` 输出：

```json
{
  "protocol_version": "1.0",
  "capabilities": [
    {
      "name": "install_packages",
      "description": "Install one or more packages",
      "category": "write",
      "destructive": true,
      "estimatedDuration": "long",
      "capabilities": {
        "cancellation": true,
        "pause": true,
        "prompts": true
      },
      "inputSchema":  { /* JSON Schema for --args */ },
      "outputSchema": { /* shape of result.data on success */ },
      "eventSchema": {
        "data": {
          "install_planned":    { /* JSON Schema */ },
          "download_progress":  { /* JSON Schema */ },
          "install_progress":   { /* JSON Schema */ },
          "install_completed":  { /* JSON Schema */ }
        },
        "prompt": {
          "overwrite_existing": { /* JSON Schema */ }
        }
      }
    }
  ]
}
```

字段含义：

| 字段 | 含义 |
|------|------|
| name | capability 标识（snake_case），与 `xlings interface <name>` 对应 |
| description | 一句话描述 |
| category | `read` / `write` / `local`（见 §5） |
| destructive | 是否会修改持久状态（已有字段，保留） |
| estimatedDuration | `instant`（< 200ms） / `short`（< 5s） / `medium`（< 1min） / `long` |
| capabilities.cancellation | 是否支持 cancel 控制命令 |
| capabilities.pause | 是否支持 pause/resume |
| capabilities.prompts | 是否会 emit prompt 事件 |
| inputSchema | JSON Schema for `--args` JSON |
| outputSchema | result.data 的 JSON Schema |
| eventSchema.data | 该 capability 可能 emit 的所有 dataKind 的 schema |
| eventSchema.prompt | 同上，prompt id 的 schema |

---

## 5. API 目录 v1

> 设计方法：先列出 xlings 领域里所有名词（Package / Version / Subos / Shim / Repo / Env / SystemStatus / xlings binary），再为每个名词列出可对它做的最小动作。**v1 选取"客户端不组合就达不成"的最小集合**；任何能由其它原语组合得到的端点不入 v1。
>
> **xstore 覆盖列**说明：
> - ✅ = xstore 已通过 interface 用到
> - ⚠️ = xstore 用了，但绕过 interface（直接调子命令或本地实现），需要 capability 化
> - ⏳ = xstore 未用，未来可能组合用到
> - — = xstore 不需要（其他客户端可能需要）

### 5.1 Read（只读，幂等）

| capability | 用途 | 期间 emit | result.data | xstore |
|-----------|------|-----------|-------------|--------|
| `list_packages` | 列出包；`{installed?, keyword?, namespace?}` 全 optional，全 null 时返回索引内全部 | `data:package_listed` 流式（每发现一个 emit 一条） | `{count}` | ✅（覆盖 xstore 的 search + list_installed 两个用例，由 keyword 区分） |
| `package_info` | 查看单包详情，含已安装版本列表 | — | `{name, namespace, description, homepage, license, dependencies[], maintainer, versions:[{version, path, active, source, installed}]}` | ✅（同时覆盖 xstore 当前的 list_versions —— versions 字段内嵌） |
| `list_subos` | 列出所有 subos 及 active | — | `{active, list:[{name, created, package_count}]}` | ⚠️ 当前走子命令 |
| `list_subos_shims` | 列出指定 subos 的 shim（独立 capability：shim 列表可能很大，与 subos 元数据分开） | — | `{subos, shims:[{name, target, version}]}` | ⚠️ 当前走子命令 |
| `list_repos` | 列出已注册的 sub-index 仓库 | — | `{repos:[{name, url, last_updated}]}` | ⏳ |
| `system_status` | xlings 自身全局状态 | — | `{xlings_version, xlings_home, disk_used, package_count, subos_count, ...}` | ✅ |

### 5.2 Write（变更持久状态）

| capability | 用途 | 期间 emit | result.data | xstore |
|-----------|------|-----------|-------------|--------|
| `install_packages` | 安装一个或多个包 | `progress`, `data:install_planned/download_progress/install_progress/install_completed`, `prompt:overwrite_existing` | `{installed[], skipped[], failed[]}` | ✅ |
| `remove_package` | 卸载单个包/版本 | `progress`（删大目录） | `{target, removedFiles, freedBytes}` | ✅ |
| `update_packages` | 拉取所有 sub-index 仓库最新 | `progress`（per-repo），`data:repo_updating/repo_updated` | `{repos:[{name, commits, packages_added, packages_changed}]}` | ✅（当前静默，需补 emit） |
| `use_version` | 切换某包的活动版本 | `data:shim_rebuilt` | `{target, oldVersion, newVersion}` | ✅ |
| `create_subos` | 新建一个 subos；带 `copyFrom` 时从已有 subos 克隆（即覆盖原 clone_subos 用例） | `progress` | `{name, path, package_count}` | ⚠️ 当前走子命令 |
| `switch_subos` | 切换活动 subos | `data:shim_rebuilt` | `{oldActive, newActive}` | ⚠️ 同上 |
| `remove_subos` | 删除某 subos | `progress` | `{name, freedBytes}` | ⏳ |
| `add_repo` | 注册一个 sub-index 仓库 | — | `{repo}` | ⏳ |
| `remove_repo` | 移除一个 sub-index 仓库 | — | `{repo}` | ⏳ |
| `self_update` | xlings 自身升级到指定 release | `progress`, `data:download_progress` | `{oldVersion, newVersion}` | ⏳ |

### 5.3 Local（不动 xlings 状态，仅查询/计算本地信息）

| capability | 用途 | result.data | xstore |
|-----------|------|-------------|--------|
| `env` | 返回 xlings 当前隔离的环境变量集合（含指定 subos 的 PATH） | `{XLINGS_HOME, XLINGS_SUBOS, PATH, ...}` | ⚠️ xstore 当前自己组装，可改为调 env |

### 5.4 v1 不开放（v1.x / v2 再议）

- `validate_config`、`which` —— 实用但非必需；客户端可用 list_subos_shims + 字段检查替代
- `run` / `exec` —— 在 subos 环境跑命令；PTY、stdin 转发、信号语义太重，需单独 RFC
- `lock` / `freeze` —— 依赖锁定，数据模型未稳定
- 双向 streaming（客户端 push 数据给 capability）—— v1 仅控制通道方向

### 5.5 v1 capability 数量统计

**18 个端点 → 17 个原语 → v1 实施 17 个 + 4 个推迟**。Read 6 + Write 10 + Local 1 = **17 个 v1 原语**。

之前 draft 中 `search_packages`/`list_versions`/`clone_subos` 三个被合并到现有原语：
- `search_packages` ⊆ `list_packages({keyword: ...})`
- `list_versions(target)` ⊆ `package_info(target).versions`
- `clone_subos(from, to)` ⊆ `create_subos({name: to, copyFrom: from})`

合并的依据：动词相同、名词相同、只是参数不同 —— 应该是同一个原语的不同入参。

---

## 6. 标准 dataKind 清单

业务事件 schema（去 TUI 化）：

### 6.1 Package events
- `package_listed` — `{name, namespace, version, description, installed, active, source, tags?}`
- `package_info` — 完整包详情
- `package_removed` — `{target, version, freedBytes}`

### 6.2 Version events
- `version_active_changed` — `{target, oldVersion, newVersion}`

### 6.3 Repo events
- `repo_updating` — `{name, url}`
- `repo_updated` — `{name, commits_pulled, packages_added, packages_changed, duration_ms}`

### 6.4 Download events
- `download_started` — `{name, totalBytes?}`
- `download_progress` — `{files: [{name, totalBytes, downloadedBytes, started, finished, success}], elapsedSec, sizesReady}`
- `download_completed` — `{name, totalBytes, durationMs, success}`

### 6.5 Install events
- `install_planned` — `{packages: [{name, version, deps[]}], totalBytes?}`
- `install_progress` — `{currentPackage, phase: "download" | "extract" | "shim", percent}`
- `install_completed` — `{installed[], skipped[], failed[]}`

### 6.6 Subos events
- `subos_listed` — `{active, list: [...]}`
- `subos_created` — `{name}`
- `subos_switched` — `{oldActive, newActive}`
- `subos_removed` — `{name, freedBytes}`

### 6.7 Shim events
- `shim_rebuilt` — `{subos, count, durationMs}`

### 6.8 System events
- `system_status` — `{xlings_version, xlings_home, disk_used, package_count, subos_count}`

> **Deprecated（v1 仍 emit 双发，v2 移除）**：
> - `styled_list`、`info_panel` —— ftxui 渲染原语，不是业务数据。现有客户端在过渡期可继续用，但新代码必须用上面结构化 dataKind。

---

## 7. 错误码枚举

`ErrorEvent.code` 取自下表（client 必须能识别这些值，未知 code 视为 `E_INTERNAL`）：

| code | 含义 | recoverable 默认 |
|------|------|-----------------|
| `E_NETWORK` | 网络超时 / DNS / connection refused | true |
| `E_AUTH` | 认证失败（gitee 等需要登录） | true |
| `E_NOT_FOUND` | 包/版本/subos/repo 不存在 | true |
| `E_PERMISSION_DENIED` | 文件系统权限 / sudo | true |
| `E_DISK_FULL` | 磁盘不够 | true |
| `E_DEPENDENCY_CONFLICT` | 依赖冲突 | true |
| `E_INTEGRITY` | checksum / signature 校验失败 | false |
| `E_CANCELLED` | 用户通过 stdin 触发 cancel | true |
| `E_TIMEOUT` | 操作超时（与 E_NETWORK 区分） | true |
| `E_INVALID_INPUT` | --args JSON 格式错 / 必填字段缺失 | false |
| `E_PROTOCOL` | 协议错误（如客户端发了 unknown action） | false |
| `E_INTERNAL` | 兜底；伴随 stderr 上 stack trace 协助 bug 上报 | false |

`hint` 字段给客户端展示给用户的修复建议（如 `E_NETWORK` 的 hint 可能是 "set HTTPS_PROXY"）。

---

## 8. 兼容性承诺（v1.x）

在 v1 主版本内：
- 不删除任何 capability
- 不删除任何 dataKind / errorCode
- 不修改字段语义
- 可新增 capability、dataKind、错误码（minor bump）
- 可在 inputSchema 加可选字段（minor bump）
- 不可让已有 schema 字段变必填（major bump）

v2 出来时，xlings 必须同时支持 `--protocol 1` 至少一个 minor 周期。

---

## 9. 客户端组合配方（Composition Recipes）

> 这一节是给客户端实现者看的"如何用 v1 原语达成业务用例"指南，**不是** v1 协议的额外端点。所有用例都通过单 capability 调用或多 capability 组合达成。

### 9.1 用例 → 原语组合速查

| 用例 | 组合方式 |
|------|---------|
| 列出所有已安装包 | `list_packages({installed: true})` |
| 搜索 npm 包索引里包含 "react" 的包 | `list_packages({keyword: "react", namespace: "xim"})` |
| 显示某包详情 + 已装版本 | `package_info({target})`（versions 已内嵌） |
| 列出某包所有已装版本 | `package_info({target})` 后取 `.versions[]` |
| 装包并显示进度 | `install_packages({targets, yes: true})`，订阅 `data:download_progress`/`install_progress` |
| 取消进行中的安装 | 向同 invocation 的 stdin 写 `{"action":"cancel"}\n` |
| 暂停 / 恢复下载 | stdin 写 `{"action":"pause"}` / `{"action":"resume"}` |
| 卸载并显示腾出的空间 | `remove_package({target})`，读 `result.data.freedBytes` |
| 全量刷新索引并显示每个仓库进度 | `update_packages({})`，订阅 `data:repo_updating`/`repo_updated` |
| 切换包活动版本，并刷新当前 subos 的 shim 表 | `use_version({target, version})` 后调 `list_subos_shims({subos: <active>})` |
| 创建一个跟当前 subos 一样的 sandbox 用于试验 | `create_subos({name: "sandbox", copyFrom: "<active>"})` |
| 切到另一个 subos 后刷新 shim 列表 | `switch_subos({name})` 后调 `list_subos_shims({subos: name})` |
| 查询当前 subos 的环境变量准备 spawn 子进程 | `env({})` 取得 PATH / XLINGS_HOME |
| 升级 xlings 自身 | `self_update({})`，订阅 `data:download_progress` |

### 9.2 Anti-pattern：v1 故意不支持的"便捷端点"

| 看似有用的端点 | 为什么 v1 不提供 |
|---------------|----------------|
| `install_and_use(target, version)` | 客户端自己 `install_packages` 后 `use_version` 即可，且语义上是两件事（装可成功，切版本可单独失败） |
| `search_and_filter_installed(keyword)` | `list_packages({keyword})` 返回时已含 `installed` 字段，客户端本地 filter |
| `package_with_changelog(target)` | changelog 不是 xlings 领域语义；要嘛 `package_info` 字段扩展，要嘛客户端去 GitHub 取 |
| `bulk_remove(targets[])` | `remove_package` 一次一个；批量是客户端循环 + 进度聚合的事 |

> 方向只有一个：客户端代码可能多几行，但协议保持小、正交、清晰，所有客户端都受益。

### 9.3 xstore 现状映射

xstore 当前 16 个 IPC 通道（`backends/electron/xlings.ts`）→ v1 原语映射：

| xstore IPC | v1 实现 | 状态 |
|-----------|---------|------|
| `check_xlings` | 客户端本地文件检查（不需要协议） | — |
| `get_xlings_env` | `env({})` | ⚠️ 需切到协议 |
| `search_packages(kw)` | `list_packages({keyword: kw})` | ✅ 协议覆盖；xstore 端调用名改一下 |
| `list_installed` | `list_packages({installed: true})` | ✅ 协议覆盖；xstore 端调用名改一下 |
| `package_info(target)` | `package_info({target})` | ✅ |
| `list_versions(target)` | `package_info({target}).versions` | ✅ 协议覆盖；xstore 端 extractor 改吃 versions 字段 |
| `use_version(target, ver)` | `use_version({target, version: ver})` | ✅ |
| `remove_package(target)` | `remove_package({target})` | ✅ |
| `update_index` | `update_packages({})` | ✅ 但需补 emit（xlings 侧） |
| `system_status` | `system_status({})` | ✅ |
| `install_package_stream(...)` | `install_packages({...})` 流式 | ✅ |
| `cancel_install` | stdin `{"action":"cancel"}` 到当前 invocation | ⚠️ 当前 SIGKILL |
| `pause_install` / `resume_install` | stdin `{"action":"pause"\|"resume"}` | ⚠️ 当前 SIGSTOP/SIGCONT |
| `list_subos` | `list_subos({})` | ⚠️ 当前走 `xlings subos` 子命令 |
| `switch_subos(name)` | `switch_subos({name})` | ⚠️ 同上 |
| `create_subos(name)` | `create_subos({name})` 或 `create_subos({name, copyFrom})` | ⚠️ 同上 |
| `list_subos_shims(subos)` | `list_subos_shims({subos})` | ⚠️ 同上 |
| `get_config` / `save_config` | xstore 本地 config 文件（不走 xlings） | — |
| `pty:*`、`window-*` | Electron 自身能力 | — |

**xstore 完全迁移后的清理项**：
1. 删 6 处绕过 interface 的逻辑（subos 五件套 + cancel/pause/resume 信号）
2. 删 `extractPackages` / `extractVersions` 这种从 styled_list/info_panel 反推的 hack —— 直接吃结构化 dataKind
3. xstore 端的 `list_versions` IPC 砍掉，前端改用 `packageInfo(target).versions`
4. `installPackageStream` 的 cancel/pause/resume 改写成对子进程 stdin 写控制行

---

## 10. 实施路线（可独立执行）

### Phase 1 — 协议骨架（PR-1，1-2 天）
1. 本文档作为 spec 落地 → `docs/plans/2026-04-25-interface-api-v1.md`
2. cli.cppm 把 ProgressEvent / LogEvent / ErrorEvent / PromptEvent 全部 wire 到 NDJSON（当前只通 DataEvent）
3. cli.cppm 异步读 stdin，解析 control channel，driving CancellationToken
4. 引入 `--protocol`、`--version` 元命令、heartbeat 计时器

### Phase 2 — Spec 完整化（PR-2，2-3 天）
5. `capability::CapabilitySpec` 加 `category` / `outputSchema` / `eventSchema` / `capabilities` / `estimatedDuration`
6. 补现有 9 个 capability 的完整 spec
7. `xlings interface --list` 输出新格式
8. `tests/interface_contract_test.cpp`：每个 capability 跑 invocation + JSON Schema 校验

### Phase 3 — 补齐 capability 集合（PR-3，2-3 天）
9. subos 五件套 capability 化（list / create / switch / remove / list_shims）
10. `env` capability
11. update_packages / search_packages / install (extract 阶段) 的事件 emit 补齐

### Phase 4 — 去 TUI 化（PR-4，3-5 天）
12. 新增结构化 dataKind（package_listed 等）
13. 老 styled_list / info_panel 双发，标 deprecated
14. xstore 切到新 dataKind（需 PR 到 openxlings/xlings 的 xstore-ndjson 分支 + xstore 自身 PR）

### Phase 5 — 稳定性 + 文档（PR-5，1-2 天）
15. heartbeat & backpressure
16. 协议合规测试套件进 CI
17. 写 `docs/interface-protocol-tutorial.md`：bash+jq、Python、Node 三个 reference client

---

## 11. 验收标准（v1.0 GA）

- [ ] 本文档无破坏性修改
- [ ] 每个 capability 三件套 schema 完整且通过 JSON Schema 校验
- [ ] `tests/interface_contract_test` 跑过
- [ ] 至少 3 个 reference client 通过协议跑核心场景
- [ ] xstore-ndjson 分支已切到 v1 协议
- [ ] CI 跑协议合规测试

---

## 12. 未来（v1.x / v2）

- `xlings interface --serve`：长进程 daemon 模式，多 invocation 复用一个 xlings 进程，省去频繁 spawn
- `xlings interface --transport=tcp:<port>`：网络版（远程 xlings agent）
- `run` / `exec` capability + PTY 通道
- 依赖锁定、reproducible install
