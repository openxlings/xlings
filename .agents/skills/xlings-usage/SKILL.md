---
name: xlings-usage
description: xlings 包管理器完整使用指南 — 安装、多版本管理、SubOS 隔离环境、项目模式、Agent 集成、包索引生态。Use when tasks involve xlings install/use/search/remove flows, subos lifecycle (new/use/fork/stop/remove), project-mode .xlings.json setup, agent sandbox workflows, or custom index/resource-server configuration.
---

# xlings Usage

## Overview

xlings 是通用包管理基础设施,支持:
- 多版本共存 + 即时切换
- 三级 SubOS 隔离(shell / FS / image)
- 去中心化包索引(官方 + 第三方 + 自建)
- Agent 集成(JSON interface + sandbox)

## Installation

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash

# Windows PowerShell
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

Verify: `xlings --version`

## Package Management

```bash
xlings install gcc@16          # install specific version
xlings install node cmake      # install multiple
xlings install gcc --reconfig  # re-run config even where it already ran (else a no-op when configured)
xlings remove gcc              # remove
xlings search python           # search packages
xlings update                  # update package index
xlings list                    # list installed packages
```

## Multi-Version

```bash
xlings install gcc@16 gcc@11   # both installed
xlings use gcc@16              # switch active
xlings use gcc@11              # switch back
gcc --version                  # reflects active version
```

Mechanism: version-view + reference-counting. N environments share one copy of xpkg payloads.

### `xlings use <name>` without a version (scripts and agents)

It never blocks and never pretends:

| installed versions | behaviour | exit |
|---|---|:---:|
| 1 | switches to it | 0 |
| >1 | changes nothing, lists them, names the exact command | **2** |
| 0 in this subos | error with what to install | 1 |

Add `--pick` (`-i`) for the arrow-key picker — opt-in, and it fails loudly
rather than silently doing nothing when there is no terminal. `--all` widens
the candidates to every subos.

Switching a release only moves the programs that release has. Any program the
new release has no version of keeps resolving to the old one and is **named in
the output**; `--strict` refuses such a switch instead.

## SubOS — Environment Isolation

### What isolates, and how much

| Level | Command | Use case |
|-------|---------|---|
| Shell | `xlings subos use <name>` | Version isolation only |
| Sandbox | `xlings subos use <name> --sandbox` / `subos exec <name> --sandbox -- <cmd>` | Linux: bwrap namespaces; macOS/Windows: home redirection only |
| Policy | `xlings subos config <name> --sandbox=dev\|private\|locked` | Declared on the instance; holds however it is entered |

| Preset | Network | Identity | Fetching packages from inside |
|---|---|---|---|
| `dev` | host | host | auto |
| `private` | nat (pasta) | neutral (user / instance hostname / UTC) | ask (owner approves outside) |
| `locked` | none | neutral | deny |

Backends: bwrap (default), `--sandbox landlock` (a kernel write fence, no
namespaces, the host's sockets reachable -- only when asked for, never for
untrusted code), `--sandbox proot` (a view, not a boundary). What a backend cannot give degrades under `dev` and refuses
(exit 125, with the reason and the fix) under `private` / `locked`.
`xlings subos status <name>` shows what is requested and what is in effect;
`xlings subos doctor` checks every instance; `xlings self doctor --isolation
--fix` repairs Ubuntu 24.04's user-namespace restriction (one sudo).

### Lifecycle

```bash
# Create
xlings subos new dev-env
xlings subos new dev-env --storage tmpfs          # ephemeral data
xlings subos new dev-env --from subos:py-ds@1.0.0 # fork from xpkg base

# Declare what it may do (owner, outside the sandbox)
xlings subos config dev-env --sandbox=private     # a preset
xlings subos config dev-env --allow gpu --mount ~/proj:/work
xlings subos config dev-env --sandbox xim:policy-ci@1   # a policy package (locked by sha256)

# Enter
xlings subos use dev-env                          # interactive shell
xlings subos use dev-env --sandbox                # sandbox mode
xlings subos exec dev-env --sandbox -- make -j8   # one command, its own exit code
xlings subos exec dev-env --sandbox=locked -- ./untrusted   # a call may only tighten

# Sessions: one instance, many commands
xlings subos start dev-env --ttl 30m              # later exec/use join it
xlings subos exec dev-env -- make test
xlings subos ps
xlings subos stop dev-env

# Inspect
xlings subos list
xlings subos info dev-env
xlings subos status dev-env                       # requested vs in effect
xlings subos log dev-env --kind perm              # audit (outside the sandbox)
xlings subos report dev-env                       # programs run, files changed

# Remove — deletes the SubOS AND its home (user data). It asks first;
# with nobody at a terminal it deletes nothing and exits 2 unless -y is given.
xlings subos remove dev-env            # asks: shows the path and what home/ holds
xlings subos remove dev-env -y         # only when the USER asked for this deletion
```

> **Agents:** a SubOS home holds the user's work. Over the interface,
> `remove_subos` without `"yes": true` returns what it would delete and changes
> nothing — tell the user, and pass `"yes": true` only after they confirm.
> Never add `-y` / `yes` just to make a cleanup step succeed. Every deletion is
> recorded in `$XLINGS_HOME/logs/destructive.ndjson`.

### Project-local SubOS

When a project has `.xlings.json` with workspace declarations, entering the project directory **automatically activates a project-scoped SubOS**:

```bash
cd my-project/     # seamlessly enters project SubOS
xlings install     # installs deps into project isolation
```

## Agent Workflows

### Agent runs inside SubOS

Treat `--sandbox` as a security boundary only on Linux with bwrap (proot is
a view, landlock fences writes only). macOS and Windows redirect
HOME/USERPROFILE and must not run untrusted code.

Declare the audience: `XLINGS_AGENT_MODE=1` (or `--agent`). In agent mode no
command waits for input; an interactive `subos use` is refused (exit 2) and
names `subos exec`.

Inside a sandbox the xlings home is read-only and xlings still works: reads
run locally, `install` / `remove` go to the owner's broker and are decided by
the instance's policy, owner-only commands return 13 with the command to run
outside, and `fetch=ask` requests wait (exit 75) for
`xlings subos requests / approve / deny` outside.

```bash
# Create isolated env for agent
xlings subos new agent-ws --from subos:dev-env@latest
xlings subos config agent-ws --sandbox=private

# Enter — agent runs INSIDE this world
xlings subos use agent-ws --sandbox
# → start codex / claude / opencode here

# Multiple instances on one host
xlings subos new agent-ws-1 --from subos:dev-env@latest
xlings subos new agent-ws-2 --from subos:dev-env@latest
```

### Programmatic interface

```bash
xlings interface
# NDJSON protocol v1.1 over stdio
# → {"protocol":"1.1","capabilities":[...]}
```

### One-shot command execution

```bash
xlings subos exec agent-ws --sandbox -- python analyze.py
# exit codes: the command's own; 125 setup; 126/127 cannot run / not found;
# 124 timeout (--timeout); 128+n signal; 13 E_PERMISSION; 75 queued for approval
```

## Package Index Ecosystem

### Add custom index

In `~/.xlings/.xlings.json` or project `.xlings.json`:

```json
{
  "index_repos": [
    { "name": "xim", "url": "https://github.com/openxlings/xim-pkgindex.git" },
    { "name": "my-team", "url": "git@gitlab.internal:devtools/pkgs.git" }
  ]
}
```

### Resource servers (binary mirrors)

```json
{
  "XLINGS_RES": {
    "GLOBAL": "https://github.com/xlings-res",
    "CN": "https://gitcode.com/xlings-res"
  }
}
```

## type="subos" Packages

Install a subos base package and fork from it:

```bash
xlings install subos:py-ds@1.0.0             # install base (lands in xpkgs/)
xlings subos new exp --from subos:py-ds@1.0.0 # fork (0s, shared storage)
```

## Key Flags Reference

| Flag | Context | Effect |
|------|---------|--------|
| `--cmd "<cmd>"` | `subos use` | Non-interactive single command exec (prefer `subos exec -- argv`) |
| `--sandbox[=preset\|backend]` | `subos use/exec/start/config` | Isolation: dev / private / locked, or bwrap / landlock / proot |
| `--net`, `--fetch`, `--allow`, `--mount`, `--observe` | `subos config` (declares) / a call (tightens only) | Policy overrides |
| `--storage <mode>` | `subos new` | shared / tmpfs / image |
| `--from <spec>` | `subos new` | Fork from local subos or pkg-spec |
| `--keep` | `subos use` | The session outlives the shell until `subos stop` (Linux) |
| `--no-keep` | `subos use` | End the session with the shell |
| `--ttl <sec>` | `subos use/start` | Session idle timeout |
| `-y` | `install` | Skip confirmation prompts |
| `-g` | `install` | Install to global scope (not project) |
| `--reconfig` | `install` | Run config() again for the whole closure; without it a package already configured in this scope at its recipe revision is left alone |

## Toolchain Switching (dev)

```bash
xlings use gcc@16.1.0   # switch active gcc for dev builds
```
