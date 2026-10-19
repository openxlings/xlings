# Agent 与机器接口

[English](../50-agents-and-machine-interface.md) | **简体中文**

两条相互独立的轴：**谁在读**输出（人还是 agent），以及**怎么呈现**（文本、`--json`、NDJSON 接口）。
受众是声明出来的，从不猜测。

## Agent 模式

```bash
export XLINGS_AGENT_MODE=1     # or --agent on any command; 0 turns it off
```

agent 模式下命令**从不等待输入**。它回答不了的问题变成结构化错误，列出候选项和能回答它的确切参数，命令以退出码 2 结束。
输出是纯文本（无颜色、无动画）。

```bash
xlings subos remove work --agent     # exit 2: a deletion needs -y, said in the error
xlings subos remove work --agent -y  # the user asked for it: deleted
```

每个退出码只有一种含义；SubOS 的见 [10](10-subos.md)，诊断信息格式见 [specs/diagnostics](../specs/diagnostics.md)。

## `--json`

报告状态的命令接受 `--json`，输出一个 JSON 文档：

```bash
xlings subos status work --json
xlings subos log work --json
xlings index list --json
xlings self doctor --isolation --json
```

## NDJSON 接口

`xlings interface` 是一个基于 stdin 和 stdout 的稳定协议：每行一个 JSON 对象，每行写出即刷新。
版本 **1.13**；完整约定见 [specs/interface-ndjson-v1](../specs/interface-ndjson-v1.md)。

```bash
xlings interface --version     # {"protocol_version":"1.13"}
xlings interface --list        # every capability, with its JSON Schema
xlings interface install_packages --args '{"targets":["gcc@16.1.0"],"yes":true}'
xlings interface install_packages --args-file ./args.json
```

### Network diagnosis

`network_endpoints` 在本地发现已配置的远程索引资源。
`probe_url` 使用下载器的传输层检查一个资源，并返回脱敏后的结构化结果。
两者均不初始化 home 或安装工具。`read_only_queries` 能力也保证状态查询与
接口握手不改变选定的 home。客户端负责包含端点发现与 DNS 的总截止时间；
`socket_timeout_ms` 仅限制 socket 操作。完整字段与边界见
[协议](../specs/interface-ndjson-v1.md#network-diagnosis-113)。

### 事件（stdout）

| kind | 字段 | |
|---|---|---|
| `progress` | `phase`、`percent`、`message` | 进度 |
| `log` | `level`、`message` | 一行日志 |
| `data` | `dataKind`、`payload` | 结构化数据 |
| `prompt` | `id`、`question`、`options`、`defaultValue` | 给客户端的问题 |
| `error` | `code`、`message`、`recoverable`、`hint` | 错误 |
| `heartbeat` | `ts` | 空闲 5 秒后 |
| `result` | `exitCode`、`data` | 最后一行 |

```json
{"kind":"progress","phase":"download","percent":42,"message":"fetching gcc@16.1.0"}
{"kind":"error","code":"NotFound","message":"unknown capability: foo","recoverable":false,"hint":"run `xlings interface --list`"}
{"kind":"result","exitCode":0,"data":{"installed":true}}
```

### 请求（stdin）

| action | 字段 | |
|---|---|---|
| `cancel` | — | 停止 |
| `pause` / `resume` | — | 暂停与继续 |
| `prompt-reply` | `id`、`value` | 回答一个 prompt |

```json
{"action":"prompt-reply","id":"confirm-1","value":"yes"}
```

### 在 SubOS 里运行命令

```bash
xlings interface subos_exec --args '{"name":"agent-ws","sandbox":true,"argv":["make","test"],"timeout":"10m"}'
xlings interface subos_events --args '{"name":"agent-ws","kind":"perm"}'
```

`subos_exec` 在命令运行中分别流式输出 stdout 与 stderr 的片段（无效 UTF-8 会被标记并以 base64 编码）；`cancel` 停止它。

## 核心与呈现分离

核心代码返回结构化结果，只通过一个端口提问；它从不按受众分支，也不拼装面向用户的文本。人看到的、`--json` 打印的、
接口流出的，是同一个结果的三种呈现——所以它们不会相互偏离。
