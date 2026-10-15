# The Agent Workspace

**English** | [简体中文](../zh/luban/04-agent-workspace.md)

A long-lived, private place for an AI agent: private from the moment it exists,
not after you remember to lock it down.

## Making one

```bash
luban new agent --edition agent-workspace --proxy socks5h://127.0.0.1:7897
luban run agent -- claude
luban status agent
```

## What private means

| | |
|---|---|
| network | only the proxy (socks5h: names resolved by the proxy); no fallback to a direct connection when the proxy is down |
| identity | a host name and a machine-id of its own, made once and kept; the time zone of the proxy's exit (asked through the proxy; UTC when it cannot be known) — never the host's |
| the host | nothing of your home, devices or sockets; credentials you give it stay inside |
| nesting | no nested user namespaces |
| installing | through the broker, by its policy |

The edition's policy (`agent-private`) is chosen and locked by `luban new` before
anything enters the environment. Without a proxy it refuses to enter and says how
to give one.

## What a shared kernel cannot hide

The kernel version, the CPU model, the host paths of what is bound in — `luban
status` lists them. For a kernel of its own, the same environment in a local VM,
its only network still the proxy:

```bash
luban enter agent --mode machine --temp
```

Your accounts (an API key, a git identity) still say who you are.

## A less private agent

For an agent that may use this machine's network and only needs to be kept from
your files:

```bash
luban config agent --policy xim:agent-confined
```

## Keeping it current

```bash
luban upgrade agent          # the newest agent, and the edition's other updates
```

The agent is not pinned in the edition: the newest at creation is installed and
recorded, and `upgrade` moves it. An upgrade never loosens isolation.

## Driving Luban from an agent or a program

The same verbs a person types are a program's, described and reported in
openas (`luban/specs/openas`): each verb's effect -- read, reversible,
irreversible, external -- its arguments and options, and the options that
answer its questions; a run is NDJSON lines (`phase`, `progress`, `log`,
`notice`, `error`, `data`) ending with `result`.

```bash
luban interface --describe                                   # every verb: effect, arguments, options, what answers it
luban interface status --args '{}'                           # one verb, reported as NDJSON lines
luban interface run --args '{"name": "agent", "command": ["uname", "-a"]}'
XLINGS_ACTOR=agent:my-agent luban interface new --args '{"name": "box", "edition": "tiny"}'
```

Nothing waits for an answer: a run that needs one ends with exit code 2 and
names the option. `XLINGS_ACTOR` says who is running it (`agent:<name>`,
`;via=person:<name>` for whom); everything it starts inherits it, and the
operation journal (`xlings self log`) records it with the step that started
each process.

`--as` says it on the command line, and an agent run in a system is held to
it: the record is this home's, which nothing in the system can write.

```bash
luban run agent --as agent:claude -- claude
```
