# The Agent Workspace

**English** | [简体中文](../zh/luban/04-agent-workspace.md)

A long-lived, private place for an AI agent: private from the moment it exists,
not after you remember to lock it down.

## Making one

```bash
luban new agent agent-workspace --proxy socks5h://127.0.0.1:7897
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
luban try agent --proxy socks5h://127.0.0.1:7897
```

Your accounts (an API key, a git identity) still say who you are.

## A less private agent

For an agent that may use this machine's network and only needs to be kept from
your files:

```bash
luban config agent policy xim:agent-confined
```

## Keeping it current

```bash
luban upgrade agent          # the newest agent, and the edition's other updates
```

The agent is not pinned in the edition: the newest at creation is installed and
recorded, and `upgrade` moves it. An upgrade never loosens isolation.
