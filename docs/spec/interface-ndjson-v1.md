# NDJSON 接口协议规范 v1.0

> 更新日期: 2026-09-26 | 版本: 2026.9.26.3

## 1. 概述

`xlings interface` 提供面向程序的结构化 API，使外部客户端（IDE 插件、CI 脚本、AI agent 等）可通过标准 IO 与 xlings 交互。协议版本为 **1.0**，基于 NDJSON（Newline-Delimited JSON）。

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
{"protocol_version":"1.0"}
```

### 3.2 查询可用能力

```bash
xlings interface --list
```

服务端输出能力清单后退出：

```json
{"protocol_version":"1.0","capabilities":[{"name":"install_packages","description":"...","destructive":true,"inputSchema":{...},"outputSchema":{...}}, ...]}
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

每行为一个 JSON 对象，必含 `"kind"` 字段标识类型。

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
{"kind":"result","exitCode":0,"data":{"installed":["gcc@14.2.0"]}}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| exitCode | integer | 0 成功，非 0 失败，130 表示取消 |
| data | object? | 能力返回的结构化结果（可选） |

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
{"kind":"data","dataKind":"install_summary","payload":{"installed":["node@22.4.0"]}}
{"kind":"result","exitCode":0,"data":{"installed":["node@22.4.0"]}}
```

客户端收到 `kind: result` 后即可关闭 stdin 并退出。
