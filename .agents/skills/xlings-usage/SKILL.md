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

## SubOS — Environment Isolation

### Three levels

| Level | Command | Root? | Use case |
|-------|---------|:---:|---|
| Shell | `xlings subos use <name>` | No | Version isolation only |
| FS (sandbox) | `xlings subos use <name> --sandbox` | No | Full filesystem isolation |
| Image | `xlings subos use <name> --sandbox` (storage=image) | Yes | Block-device isolation |

### Lifecycle

```bash
# Create
xlings subos new dev-env
xlings subos new dev-env --storage tmpfs          # ephemeral data
xlings subos new dev-env --from subos:py-ds@1.0.0 # fork from xpkg base

# Enter
xlings subos use dev-env                          # interactive shell
xlings subos use dev-env --sandbox                # sandbox mode
xlings subos use dev-env --cmd "echo hello"       # single command
xlings subos use dev-env --sandbox --cmd "..."    # sandbox + command

# List / info
xlings subos list
xlings subos info dev-env

# Keeper (high-frequency exec)
xlings subos use dev-env --sandbox --keep         # keep mount namespace alive
xlings subos stop dev-env                         # release keeper

# Remove
xlings subos remove dev-env
```

### Project-local SubOS

When a project has `.xlings.json` with workspace declarations, entering the project directory **automatically activates a project-scoped SubOS**:

```bash
cd my-project/     # seamlessly enters project SubOS
xlings install     # installs deps into project isolation
```

## Agent Workflows

### Agent runs inside SubOS

```bash
# Create isolated env for agent
xlings subos new agent-ws --from subos:dev-env@latest

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
# NDJSON protocol v1.0 over stdio
# → {"protocol":"1.0","capabilities":[...]}
```

### One-shot command execution

```bash
xlings subos use agent-ws --sandbox --cmd "python analyze.py"
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
| `--cmd "<cmd>"` | `subos use` | Non-interactive single command exec |
| `--sandbox` | `subos use` | Enable FS-level isolation (bwrap/proot) |
| `--storage <mode>` | `subos new` | shared / tmpfs / image |
| `--from <spec>` | `subos new` | Fork from local subos or pkg-spec |
| `--keep` | `subos use` | Keep mount namespace alive (no timeout) |
| `--no-keep` | `subos use` | Force disable keeper |
| `--ttl <sec>` | `subos use` | Custom keeper idle timeout |
| `-y` | `install` | Skip confirmation prompts |
| `-g` | `install` | Install to global scope (not project) |

## Toolchain Switching (dev)

```bash
xlings use gcc@16.1.0   # switch active gcc for dev builds
```
