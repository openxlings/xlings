> 编写日期: 2026-05-17 | 版本: 2026.9.28.2

# NDJSON 接口协议规范 v1.5

## 1. 概述

`xlings interface` 提供面向程序的结构化 API，使外部客户端（IDE 插件、CI 脚本、AI agent 等）可通过标准 IO 与 xlings 交互。协议版本为 **1.5**，基于 NDJSON（Newline-Delimited JSON）。

| 协议版本 | xlings | 变化 |
|----------|--------|------|
| 1.0 | 0.4.36 起 | 初始版本 |
| 1.1 | 2026.9.27.1 起 | 增补：`install_targets` 事件；`install_plan` 条目的第三个元素 `revision`（§6.3.1） |
| 1.2 | 2026.9.28.1 起 | 增补：`update_packages` 的 `progress` 事件（`index_sync`、`index_rebuild`，§6.1.1）与 `download_progress` 数据事件（§6.3.2）；任何能力都不向 stdout 写 NDJSON 以外的内容（§5） |
| 1.3 | 2026.9.28.2 起 | 增补：`download_progress` 的 `stream` 字段与发送频率的上限（§6.3.2）；`prevLines` 废弃，恒为 0 |
| 1.4 | 2026.9.29.1 起 | 增补：`install_packages` 的 `reconfig` 字段（§7.3）与 `configure` 进度事件（§6.1.2）；已在本 scope 按当前 revision 配置过的包不再重跑 config |
| 1.5 | 2026.9.30.1 起 | 增补：`install_packages` 的 `hook` 进度事件（§6.1.2）；安装 hook 启动的命令的输出写入 hook 日志，不再以 `[stray stdout]` 转发到 stderr（§5）；下载因本地写入失败（磁盘满、无权限）时错误码为 `E_DISK_FULL`（§6.1.3） |

次版本号的变化只做增补，1.0 客户端无需修改即可读取 1.1 的输出。客户端应通过**探测能力**
判断服务端是否提供某项功能（例如 `install_targets` 事件是否出现），而不是比较版本号。

## 2. 传输层

| 方向 | 通道 | 格式 |
|------|------|------|
| 请求 / 控制 | stdin | 每行一个 JSON 对象 |
| 响应 / 事件 | stdout | 每行一个 JSON 对象 |

- 每行以 `\n` 结尾，不含内嵌换行。
- stderr 保留用于调试日志，客户端不应解析。
- 编码固定为 UTF-8。

## 3. 会话启动

客户端通过命令行启动会话：

```
xlings interface [--version] [--list] [<capability> --args '<json>']
```

### 3.1 查询协议版本

```bash
xlings interface --version
```

服务端输出一行后退出：

```json
{"protocol_version":"1.5"}
```

### 3.2 查询可用能力

```bash
xlings interface --list
```

服务端输出能力清单后退出：

```json
{"protocol_version":"1.1","capabilities":[{"name":"install_packages","description":"...","destructive":true,"inputSchema":{...},"outputSchema":{...}}, ...]}
```

### 3.3 执行能力

```bash
xlings interface install_packages --args '{"targets":["gcc@14"],"yes":true}'
```

服务端进入事件流模式：持续向 stdout 输出事件行，直到发出 `result` 行后退出。

`--args-file <path>` 可替代 `--args`，从文件读取 JSON 参数（用于 Windows 引号转义问题）。

#### 3.3.1 参数校验（2026.9.20.1+）

在能力被调度之前，服务端会做一次校验：

- **对所有能力**：`--args` 不是合法 JSON，或不是 JSON 对象；
- **对声明了 `required` 的能力**：`inputSchema.required` 中任何字段**缺失**或取值为 `null`。

第一条不限于声明了 `required` 的能力：没有必填字段的能力**仍然会读可选字段**，
而一份没解析成功的 params 会静默退化成默认值 —— `update_packages` 会去更新整个索引
而不是被请求的那个包，`list_packages` 会列出全部而不是按 `filter` 过滤。
请求没有发生，而回答看起来像一个合法的回答。

`--args ""`（显式空串）等同于不传 `--args`，即 `{}`；未声明的多余字段仍然被接受
（这是 `required` + 顶层类型校验，不是完整的 JSON Schema 校验）。

任一条命中即先发一行 `error`（`code` 为 `E_INVALID_INPUT`，`message` 列出缺失的字段名，
`hint` 给出该能力 `required` 的全集），再发 `result`（`exitCode=1`）并退出。**能力本身不会被执行。**

```jsonc
// xlings interface plan_install --args '{"packages":["cpp"]}'
{"kind":"error","code":"E_INVALID_INPUT","message":"missing required field(s): targets","recoverable":false,"hint":"this capability requires: targets — run `xlings interface --list` for the full schema"}
{"kind":"result","exitCode":1}
```

在 2026.9.20.1 之前，`required` 只是被**公布**、从未被**执行**：字段名写错的请求会得到
`{"exitCode":0,"kind":"result"}`，与「这个目标无需安装任何东西」这个合法且常见的回答
完全同形，客户端无法区分（openxlings/xlings#464）。

校验范围**只有** `required` 与顶层类型，不是完整的 JSON Schema 校验：
`required` 是已经公布出去的那条承诺，这次只是让它成真；
把今天宽松接受的其他形状变成硬错误是另一个决定，有它自己的影响面。
未声明 `required` 的能力不受影响。

## 4. stdin 控制请求格式

会话执行期间，客户端可通过 stdin 发送控制指令：

```json
{"action":"cancel"}
{"action":"pause"}
{"action":"resume"}
{"action":"prompt-reply","id":"<prompt-id>","value":"<answer>"}
```

| action | 说明 |
|--------|------|
| `cancel` | 取消当前执行，能力将抛出 CancelledException |
| `pause` | 暂停执行 |
| `resume` | 恢复执行 |
| `prompt-reply` | 回复服务端发出的交互式提示 |

未识别的 action 会触发一条 `kind: log`（warn 级别）事件。

## 5. stdout 事件格式

每行为一个 JSON 对象，必含 `"kind"` 字段标识类型。stdout 上不会出现这一行 JSON
之外的任何字节——包括某能力在执行期间于进程内运行的、xlings 自身不控制的代码
（例如索引缓存失效后，通过库的 Lua 沙箱执行下载来的索引构建脚本）。这类代码
即使直接写系统级 stdout，也会被截获：可识别为进度的输出转换成一条 `progress`
事件，其余转发到 stderr，两种情况都不出现在 stdout 上——此前 `update_packages`／
`install_packages` 触发的索引重建会把这类脚本的原始终端文本（含 `\r`、ANSI
清行序列）直接混进 NDJSON 流，客户端解析到该行即失败。

## 6. 事件类型

### 6.1 progress

报告任务进度。

```json
{"kind":"progress","phase":"downloading","percent":42,"message":"gcc-14.2.0.tar.xz"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| phase | string | 当前阶段标识 |
| percent | number | 0-100 整数，-1 表示不确定 |
| message | string | 人类可读描述 |

#### 6.1.1 `update_packages` 的 progress 事件

`update_packages` 在刷新索引（未指定 `target`，或指定 `target` 时刷新之前）的
过程中，按下列 phase 报告进度；三者都可能一次都不出现——本地/离线源没有网络
字节可报，缓存未失效时不会重建：

| phase | 何时出现 | i/n 的含义 |
|-------|---------|-----------|
| `index_sync` | 每同步一个配置的索引仓库 | 该仓库在其所属分组（顶层 index_repos，或一批已发现的子索引）中的序号/该分组总数 |
| `index_rebuild` | 索引缓存失效、重新解析 pkgs/ 时 | 已处理/待处理的包文件总数 |

`percent` 为 `i/n`（分组总数为 0 时为 0）；`message` 同时给出仓库名或包文件名，
便于在没有专门 UI 的客户端里也能打印出有意义的一行。索引工件下载的字节级进度
走 `download_progress`（§6.3.2），不走这里。

`index_rebuild` 的步骤来自索引自带的 `pkgindex-build.lua`。自 2026.9.29.1 起这些输出经
libxpkg 的 `BuildOutput` 直接交给 xlings，不再经过进程的 stdout；事件的 phase 与 message
写法不变。

#### 6.1.2 `install_packages` 的 progress 事件（1.4 起）

安装过程中，每个**确实做了事**的节点完成时报告一次：

| phase | 何时出现 | message |
|-------|---------|---------|
| `configure` | 一个节点装好（payload 本次写入）或在本 scope 配置好（payload 已在 store，本次映射进来） | `installed <ns:name@version>` 或 `configured <ns:name@version>` |

`percent` 为已完成数 / 本次预计要做事的节点数。已在本 scope 按当前 revision 配置过的节点
不做任何事，也不报告（见 §7.3）。CLI 前端把同一件事打印成一行 `  [i/n] installed …`。

1.5 起，一个 install / config hook 运行较久时还会出现：

| phase | 何时出现 | message |
|-------|---------|---------|
| `hook` | hook 已运行 15 秒，之后每 60 秒一次，直到它结束 | `<ns:name@version> <hook> hook running <耗时>[: <日志最后一行>]` |

`percent` 与 `configure` 事件相同（已完成数 / 预计节点数）。hook 启动的命令的输出写入
`<XLINGS_HOME>/logs/hooks/<ns>-<name>@<version>.<hook>.log`（每次运行重写）；此前它们由 §5
的截获转发到 stderr（每行带 `[stray stdout]` 前缀），stderr 上的输出则原样到达。hook 失败时，
`error` 事件的 message 带有输出的最后 20 行和这个日志的路径。

#### 6.1.3 下载失败的错误码（1.5 起）

一个包的下载失败时，`error` 事件恰好出现一次，message 是失败原因（此前还会多出一个 message
为空的事件，和一个 `download artifact missing`）。原因在本机——写入失败、磁盘上的文件比收到的
短、或按服务器给出的大小判断磁盘放不下——时 `code` 为 `E_DISK_FULL`，`hint` 给出下载目录；此时
不会再尝试其他镜像（它们写的是同一块磁盘）。

### 6.2 log

日志消息。

```json
{"kind":"log","level":"info","message":"extracting archive..."}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| level | string | `debug` / `info` / `warn` / `error` |
| message | string | 日志内容 |

### 6.3 data

结构化数据载荷，用于返回查询结果等。

```json
{"kind":"data","dataKind":"env","payload":{"xlingsHome":"/home/user/.xlings",...}}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| dataKind | string | 数据类别标识 |
| payload | object/string | 结构化数据；若原始 JSON 解析失败则为字符串 |

#### 6.3.1 安装相关的 data 事件

`install_packages`（以及 CLI 的 `xlings install`）按以下顺序输出三类 data 事件。

**`install_plan`**：待安装的节点（依赖已展开，已安装且为当前载荷的节点不列出）。全部目标均已
安装时不输出。`plan_install`（dry-run）输出此事件后结束。

```json
{"kind":"data","dataKind":"install_plan","payload":{"packages":[["xim:glibc@2.44.3","reinstall: recipe revision 1, installed revision 0",1],["xim:node@22.4.0","",0]]}}
```

`packages` 的每一项为数组 `[坐标, 说明, 修订号]`：

| 位置 | 类型 | 说明 |
|------|------|------|
| 0 | string | `<namespace>:<name>@<version>` |
| 1 | string | 说明；磁盘上已有该版本但修订号不同时为重装原因，否则为空串 |
| 2 | integer | 配方对该版本声明的打包修订号（1.1 起；见 xpkg 规范 5.1.1） |

**`install_summary`**：本次运行的计数。全部目标均已安装时不输出。

```json
{"kind":"data","dataKind":"install_summary","payload":{"success":1,"failed":0}}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| success | integer | 本次运行实际安装的节点数；载荷已存在且为当前载荷的节点不计入 |
| failed | integer | 失败的节点数 |

**`install_targets`**（1.1 起）：每个请求目标解析到的版本与载荷位置。由顶层安装调用在
**每一条路径**上输出一次，包括全部目标均已安装、解析失败和安装失败；配方在安装过程中
通过 `pkgmanager.install()` 发起的嵌套安装不输出此事件。`update_packages` 升级到新版本时
内部执行的安装同样输出此事件；`plan_install`（dry-run）不输出。

```json
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"glibc@2.44","namespace":"xim","name":"glibc","version":"2.44.3","revision":1,"status":"installed","payload_dir":"/home/u/.xlings/data/xpkgs/xim-x-glibc/2.44.3"}]}}
```

`targets` 与请求中的 `targets` 一一对应、顺序相同。`status` 描述本次运行是否满足了该请求：
运行在生成安装计划之前终止时（例如另一个目标无法解析），每一项均为 `failed`，已解析的项仍
填写 `namespace`、`name`、`version`。每一项包含：

| 字段 | 类型 | 说明 |
|------|------|------|
| request | string | 调用方请求的原始字符串 |
| namespace | string | 解析到的包命名空间；未能解析时为空串 |
| name | string | 解析到的包名；未能解析时为空串 |
| version | string | 实际解析到的版本；未能解析时为空串 |
| revision | integer | 配方对该版本声明的打包修订号 |
| status | string | `installed`（本次运行安装）/ `already_present`（载荷已存在且为当前载荷）/ `failed` |
| payload_dir | string | 载荷目录的绝对路径；`status` 为 `failed` 时为空串 |

三种情形的示例：

```jsonc
// 全新安装
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"xim:node@22","namespace":"xim","name":"node","version":"22.4.0","revision":0,"status":"installed","payload_dir":"/home/u/.xlings/data/xpkgs/xim-x-node/22.4.0"}]}}
// 全部已安装（此时不输出 install_plan 与 install_summary）
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"xim:node@22","namespace":"xim","name":"node","version":"22.4.0","revision":0,"status":"already_present","payload_dir":"/home/u/.xlings/data/xpkgs/xim-x-node/22.4.0"}]}}
// 失败（此处为目标不存在；解析成功而安装失败时 namespace/name/version 仍会填写）
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"xim:nosuchpkg","namespace":"","name":"","version":"","revision":0,"status":"failed","payload_dir":""}]}}
```

#### 6.3.2 download_progress

字节级下载进度，`install_packages` 与 `update_packages` 共用同一形状——后者
用它报告索引工件（当索引仓库以工件方式获取、且字节数已知时；git/本地源没有
这一层进度）的下载，客户端因此无需为两者各写一套渲染逻辑。

```json
{"kind":"data","dataKind":"download_progress","payload":{"stream":"index:xim","files":[{"name":"xim","totalBytes":204800,"downloadedBytes":102400,"started":true,"finished":false,"success":false}],"nameWidth":3,"elapsedSec":1.2,"sizesReady":true,"prevLines":0}}
```

事件只携带数据，不携带渲染状态（1.3 起）。如何绘制由客户端决定，并由客户端按 `stream`
保存：上一帧的行数、上一帧的时间、哪些任务已经报告过开始与结束。

发送频率有上限：同一个 `stream` 每 100 ms 至多一个事件，状态未变化时不重复发送，而最后
一个事件（其中每个任务都已 `finished`）总会发送。以工件方式获取的索引在获取完成后发送
一个 `finished` 为真的事件，无论下载过程中是否知道总字节数。

| 字段 | 类型 | 说明 |
|------|------|------|
| stream | string | 事件所属的流（1.3 起）：`index:<仓库名>` 为一次索引获取，`install:<n>` 为一次安装的一批下载；客户端按它保存各自的渲染状态。1.2 的服务端不发送该字段，其事件视为同一个流 |
| files | array | 本次事件里的每一个下载任务 |
| files[].name | string | 任务标签——`update_packages` 用索引仓库名，`install_packages` 用文件名 |
| files[].totalBytes | number | 总字节数；`sizesReady` 为 false 时无意义 |
| files[].downloadedBytes | number | 已下载字节数 |
| files[].started / finished / success | boolean | 该任务的生命周期 |
| nameWidth | number | 渲染对齐用的标签列宽；结构化客户端可忽略 |
| elapsedSec | number | 自本次刷新/安装开始的已用秒数 |
| sizesReady | boolean | 总字节数是否已知（服务器在响应头给出前不知道） |
| prevLines | number | 已废弃（1.3 起恒为 0，2.0 移除）。1.2 中它是上一帧渲染的终端行数，即渲染器的状态；1.3 的客户端忽略它 |

### 6.4 prompt

服务端向客户端请求用户输入。

```json
{"kind":"prompt","id":"confirm-remove","question":"Remove gcc@14?","options":["yes","no"],"defaultValue":"no"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| id | string | 提示 ID，回复时引用 |
| question | string | 提示问题 |
| options | array | 可选项列表（可为空数组） |
| defaultValue | string | 默认值 |

客户端应通过 stdin 发送 `{"action":"prompt-reply","id":"<id>","value":"<answer>"}` 回复。

### 6.5 error

错误事件。不一定是终止性的——`recoverable` 指示能力是否仍在运行。

```json
{"kind":"error","code":"E_NOT_FOUND","message":"unknown capability: foo","recoverable":false,"hint":"run `xlings interface --list`"}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| code | string | 错误码（见下表） |
| message | string | 错误描述 |
| recoverable | boolean | true 表示执行仍继续 |
| hint | string? | 可选修复建议 |

错误码枚举：

| code | 含义 |
|------|------|
| `E_INVALID_INPUT` | 参数校验失败 |
| `E_NOT_FOUND` | 目标不存在 |
| `E_NETWORK` | 网络错误 |
| `E_DISK_FULL` | 磁盘空间不足 |
| `E_PERMISSION` | 权限不足 |
| `E_CANCELLED` | 被用户取消 |
| `E_INTERNAL` | 内部错误 |

### 6.6 result

终止行，标志会话结束。每次执行恰好输出一行。

```json
{"kind":"result","exitCode":0}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| exitCode | integer | 0 成功，非 0 失败，130 表示取消 |
| data | object? | 能力返回值中 `exitCode` 以外的字段（可选；返回值只有 `exitCode` 时省略） |

安装结果不在 `result` 行中，而在 `install_targets` 事件中（§6.3.1）。

### 6.7 heartbeat

空闲超过 5 秒时自动发出，表示进程仍存活。

```json
{"kind":"heartbeat","ts":"2026-05-17T08:30:00Z"}
```

## 7. 支持的能力（Capabilities）

| 名称 | 说明 | 破坏性 |
|------|------|--------|
| `search_packages` | 关键字模糊搜索包 | 否 |
| `install_packages` | 安装一个或多个包 | 是 |
| `plan_install` | 安装预演（dry-run） | 否 |
| `remove_package` | 移除一个包 | 是 |
| `update_packages` | 刷新索引或升级指定包 | 是 |
| `list_packages` | 列出已安装的包 | 否 |
| `package_info` | 查询包详细信息 | 否 |
| `list_installed_versions` | 列出已安装版本 | 否 |
| `use_version` | 切换激活版本 | 是 |
| `system_status` | 系统状态 | 否 |
| `list_subos` | 列出所有 sub-OS | 否 |
| `list_subos_shims` | 列出活跃 sub-OS 的 shim | 否 |
| `create_subos` | 创建 sub-OS | 是 |
| `switch_subos` | 切换活跃 sub-OS | 是 |
| `remove_subos` | 删除 sub-OS | 是 |
| `env` | 返回当前环境信息 | 否 |
| `list_repos` | 列出索引仓库 | 否 |
| `add_repo` | 添加/更新索引仓库 | 是 |
| `remove_repo` | 移除索引仓库 | 是 |

各能力的 `inputSchema` / `outputSchema` 通过 `xlings interface --list` 获取。
其中 `required` 自 2026.9.20.1 起由服务端统一执行，见 §3.3.1。

### 7.1 需要用户确认的能力（2026.9.26.1+）

sub-OS 的 home 是用户数据，删掉就无法恢复。因此下面两个能力只有在调用方带上
`"yes": true`（表示**用户已经确认**）时才会动手；不带时什么都不改，返回
`E_INVALID_INPUT`、exitCode=2，`message` 写明会删除 / 接管哪个目录、里面有多少数据，
`hint` 说明这是用户的决定：

| 能力 | 什么时候需要 `yes` |
|------|------------------|
| `remove_subos` | 总是需要：它会删除这个 sub-OS 的 home 和其中所有文件 |
| `create_subos` | 目标目录已经存在且不是空的（未登记的 sub-OS 目录，或通过 `dir` 传入的已有目录）：`yes` 表示用户同意原样接管它 |

agent 的正确做法：把 `message` 里的内容告诉用户，用户同意后再带 `"yes": true` 重新调用。
不要为了让调用成功而自行加上 `yes`。

```jsonc
// xlings interface remove_subos --args '{"name":"dev"}'
{"kind":"error","code":"E_INVALID_INPUT","recoverable":true,
 "message":"removing subos 'dev' deletes /home/u/.xlings/subos/dev (home 2.5 GB in 30256 file(s)); nothing was removed",
 "hint":"this needs the user's confirmation. Tell the user what would be deleted; if they ask for it, call remove_subos again with \"yes\": true"}
{"kind":"result","exitCode":2}
```

每次实际发生的删除都会追加一行到 `<XLINGS_HOME>/logs/destructive.ndjson`，记录路径、
大小、确认方式（`terminal` / `-y` / `yes:true`）、命令行和父进程。

### 7.2 已移除的字段

`install_packages` / `plan_install` 的 `noDeps` 曾写着 "Skip dependency installation"，
但从未生效（依赖总会被安装）。2026.9.26.1 起它从 schema 中删除；传 `"noDeps": true`
会被拒绝（`E_INVALID_INPUT`，exitCode=2），`false` 或不传与以往相同。

### 7.3 `install_packages` 的 `reconfig`（1.4 起）

包的 payload 由整个 home 共享，而"在某个 scope（subos 或项目）里配置过"是那个 scope 的事实。
自 2026.9.29.1 起，每个 scope 在自己的 `.xlings.json` 里记录 `configured`
（`"<ns>:<name>@<version>": <revision>`）。一个节点同时满足以下两条时，安装不会重跑它的 config：

1. 这条记录的 revision 等于配方当前的 revision；
2. 该 payload 在 ledger 里的每个注册项都在本 scope 的 `installed[]` 中。

整个闭包都满足时，`install_packages` 只做激活和一次路由表重建，`install_targets` 照常报告
`already_present`。

| 字段 | 类型 | 说明 |
|------|------|------|
| reconfig | boolean | 为 `true` 时，plan 里的每个节点都重跑 config（即 2026.9.29.1 之前的行为）。默认 `false` |

没有记录一律视为"未配置"：config 会运行并写入记录，旧 home 由此自然迁移。配方改变了 config
的效果时必须提升 `revision`，各 scope 会在下一次触及该包的安装时各自重新配置。

## 8. 错误处理

- **启动阶段错误**（如未知能力名）：服务端先输出一行 `error` 事件，再输出 `result`（exitCode=1），然后退出。
- **执行阶段错误**：通过事件流中的 `error` 事件报告。`recoverable=true` 表示执行未中断；`recoverable=false` 后通常紧跟 `result` 终止行。
- **参数不满足 `inputSchema.required`**：code 为 `E_INVALID_INPUT`，exitCode=1，能力不被执行（§3.3.1）。
- **需要确认但没有确认**（2026.9.26.1+）：code 为 `E_INVALID_INPUT`，exitCode=2，什么都没有改变（§7.1）。
- **依赖解析失败**（2026.9.26.1+）：`message` 以依赖链开头，例如
  `xim:xmake@3.1.1 -> ncurses: ...`，同一个失败只报一次。
- **内部异常**：code 为 `E_INTERNAL`，exitCode=1。
- **解压失败**（2026.9.20.1+）：不再一律是 `E_INTERNAL`。归档损坏 / 含不支持的条目为
  `E_INVALID_INPUT`（`hint` 说明缓存已清除、重试会重新下载）；写入失败为 `E_DISK_FULL`。
  在此之前 `ExtractError` 的种类在传递中被丢弃，客户端无法区分「重试」与「清理磁盘」
  （openxlings/xlings#376）。
- **取消**：exitCode=130。
- **被策略拒绝**：exitCode=2。命令是合法的、也没有出错，但 xlings 拒绝执行它。
  与 1 分开，是因为客户端对这两者该做的事不同：1 是"出问题了"，2 是"你要的这件事
  我不做，除非你明确覆盖"。

### 8.1 `remove_package` 的反向依赖拒绝（2026.8.8.1+）

当活跃 subos 里有已安装的包**直接依赖**移除目标时，`remove_package` 会拒绝执行，
先发一条 `remove_blocked`,再以 exitCode=2 结束：

```json
{"kind":"data","dataKind":"remove_blocked","payload":{
  "subos":"default","name":"glibc","version":"2.39",
  "required_by":[{"name":"xim:binutils","version":"2.42"},
                 {"name":"xim:llvm","version":"22.1.8"}]}}
{"kind":"result","exitCode":2}
```

传 `{"force": true}` 可越过这一检查（`remove_plan` / `remove_summary` 照常）。

只看**直接**依赖:依赖方的 libdirs 只有在被直接声明时才会进入其载荷的 RPATH 闭包，
所以隔了一跳的包并没有把这个载荷放在任何搜索路径上，删掉它不会经由 loader 影响到它。

## 9. 完整会话示例

```
$ xlings interface install_packages --args '{"targets":["node@22"],"yes":true}'
```

stdout 输出（每行一个 JSON）：

```json
{"kind":"progress","phase":"resolving","percent":0,"message":"resolving node@22"}
{"kind":"progress","phase":"downloading","percent":25,"message":"node-v22.4.0-linux-x64.tar.xz"}
{"kind":"progress","phase":"downloading","percent":80,"message":"node-v22.4.0-linux-x64.tar.xz"}
{"kind":"log","level":"info","message":"extracting node-v22.4.0-linux-x64.tar.xz"}
{"kind":"progress","phase":"installing","percent":90,"message":"linking shims"}
{"kind":"data","dataKind":"install_summary","payload":{"success":1,"failed":0}}
{"kind":"data","dataKind":"install_targets","payload":{"targets":[{"request":"node@22","namespace":"xim","name":"node","version":"22.4.0","revision":0,"status":"installed","payload_dir":"/home/u/.xlings/data/xpkgs/xim-x-node/22.4.0"}]}}
{"kind":"result","exitCode":0}
```

客户端收到 `kind: result` 后即可关闭 stdin 并退出。
