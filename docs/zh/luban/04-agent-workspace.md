# Agent 工作区

[English](../../luban/04-agent-workspace.md) | **简体中文**

一个给 AI agent 的、长期存在的私有空间：从创建那一刻起就是私有的，而不是等你记得去锁定之后。

## 创建

```bash
luban new agent --edition agent-workspace --proxy socks5h://127.0.0.1:7897
luban run agent -- claude
luban status agent
```

## 私有的含义

| | |
|---|---|
| 网络 | 只有那个代理（socks5h：域名由代理解析）；代理不可用时不会改走直连 |
| 身份 | 自己的主机名和 machine-id，只生成一次并保持；时区为代理出口所在时区（经代理查询，查不到时为 UTC）——从不用宿主的 |
| 宿主 | 看不到你的 home、设备和 socket；你给它的凭据留在里面 |
| 嵌套 | 禁止嵌套 user namespace |
| 装包 | 经 broker，按它的策略 |

edition 的策略（`agent-private`）在任何东西进入环境之前就由 `luban new` 选定并锁定。没有代理时它拒绝进入，并说明怎样提供一个。

## 共享内核藏不住的

内核版本、CPU 型号、绑定进来的宿主路径——`luban status` 会列出来。需要自己的内核时，在本地虚拟机里运行同一个环境，
网络仍然只有那个代理：

```bash
luban enter agent --mode machine --temp
```

你的账号（API key、git 身份）仍然表明你是谁。

## 不那么私有的 agent

agent 可以使用这台机器的网络、只需要与你的文件隔开时：

```bash
luban config agent --policy xim:agent-confined
```

## 保持最新

```bash
luban upgrade agent          # the newest agent, and the edition's other updates
```

agent 在 edition 里不锁版本：创建时安装当时最新的并记录下来，`upgrade` 再更新它。升级从不放宽隔离。

## 从 agent 或程序驱动 Luban

人敲的命令就是程序用的动词，以 openas（`luban/specs/openas`）描述和报告：每个动词的效果类别——只读、可撤销、
不可撤销、对外——它的参数与选项，以及回答它的问题的选项；一次运行是若干 NDJSON 行（`phase`、`progress`、`log`、
`notice`、`error`、`data`），最后是 `result`。

```bash
luban interface --describe                                   # every verb: effect, arguments, options, what answers it
luban interface status --args '{}'                           # one verb, reported as NDJSON lines
luban interface run --args '{"name": "agent", "command": ["uname", "-a"]}'
XLINGS_ACTOR=agent:my-agent luban interface new --args '{"name": "box", "edition": "tiny"}'
```

没有任何东西会等待回答：需要回答的运行以退出码 2 结束，并给出对应的选项。`XLINGS_ACTOR` 说明是谁在运行
（`agent:<名字>`，`;via=person:<名字>` 表示代谁）；它启动的一切都继承它，操作日志（`xlings self log`）记录它，
以及每个进程是由哪一步启动的。
