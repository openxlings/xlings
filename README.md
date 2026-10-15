<div align=center>
  <img width="120" src="https://xlings.d2learn.org/imgs/xlings-logo.png">

  <h1>xlings</h1>

  <em>Universal package infrastructure with OS-like SubOS isolation.<br/>
  Multi-version · Rootless · Decentralized Index · Agent-ready.</em>

  <b> [Website] | [Docs] | [Package Index] | [Forum] </b>

  [中文](README.zh.md) | English
</div>

[Website]: https://openxlings.github.io/
[Docs]: docs/README.md
[Package Index]: https://openxlings.github.io/xim-pkgindex
[Forum]: https://forum.d2learn.org/category/9/xlings

<p align=center>
  <em>Used by: <a href="https://github.com/mcpp-community/mcpp">MCPP</a> · <a href="docs/luban/README.md"><b>Luban</b></a></em>
</p>

One tool to install any version of anything, run it without root, and isolate
it like an OS of its own — on Linux, macOS and Windows.

## Quick Start

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

```bash
xlings install gcc@16 node@24 cmake   # install (version optional)
xlings use gcc@16                      # switch the active version
xlings search python                   # search packages
xlings list                            # what is installed
```

Using an AI agent? Tell it to run `xlings agent usage` — a complete guide
written for agents ships with xlings.

## What it does

| Area | What you get |
|---|---|
| **Packages** | binaries, scripts, configs, SubOS bases — all one format (xpkg), from official, third-party or self-hosted indexes |
| **Versions** | any number side by side; N environments share one copy of each payload |
| **SubOS** | named environments: their own tools, home and policy — from a PATH switch to a rootless sandbox with a private network |
| **Agents** | `xlings subos exec`, exit codes that mean something, `XLINGS_AGENT_MODE=1`, and an NDJSON interface (`xlings interface`) |
| **Repair** | `xlings self doctor --fix` checks its own state and repairs it in one run |

## Scenarios

**Several toolchains, no conflicts** — [guide](docs/03-packages-and-versions.md)

```bash
xlings install gcc@16 gcc@11 node@24
xlings use gcc@11        # switch back any time; both stay installed
```

**One environment for the whole team** — [guide](docs/04-projects.md)

```json
{ "workspace": { "xmake": "3.0.7", "gcc": { "linux": "16.1.0" }, "llvm": { "macosx": "20.1.7" } } }
```

```bash
cd my-project/ && xlings install     # the project's own SubOS, the declared versions
```

**Agents and untrusted code in a sandbox** — [guide](docs/10-subos.md)

```bash
xlings subos new agent-ws --sandbox=private      # isolation is declared once, here
xlings subos exec agent-ws -- python run.py      # every entry is that sandbox; its own exit code
xlings subos use agent-ws                        # or a shell inside -- no flag needed
```

| Platform | Release | `--sandbox` isolates |
|---|---|---|
| Linux x86_64 / aarch64 | ✅ | filesystem, processes, network, identity (bwrap) |
| macOS 14+ arm64 | ✅ | **only `$HOME`** |
| Windows x86_64 | ✅ | **only `%USERPROFILE%`** |

> On macOS and Windows `--sandbox` is not a security boundary: use an OS
> sandbox or a VM for code you do not trust. `xlings subos status <name>`
> shows what a SubOS asks for and what this machine gives it.

**Upgrade and repair** — [guide](docs/06-self-management.md)

```bash
xlings self update
xlings self doctor --fix
```

## Luban

A whole Linux userland in one command, rootless — then boot it as a machine.
Luban is the OS model on xlings: environments made from editions, rolled back by
generation, exported as images, shown in the Luban OS window with their desktop.
[Quick start](docs/luban/01-quick-start.md) · [The desktop](docs/luban/08-desktop.md)

```bash
luban new box       # a Linux system, no root needed (Luban Core: shells, editors, git, compilers)
luban enter box     # its shell; exit to leave
```
Smaller or desktop editions (`--edition tiny`, `--edition desktop`), the same
system as a machine with its own kernel (`--mode machine`), bootable images
(`luban export`), the Luban OS window, macOS and Windows: [Quick start](docs/luban/01-quick-start.md).


## Documentation

| Topic | Docs |
|---|---|
| **Start** | [What xlings is](docs/00-what-xlings-is.md) · [Getting started](docs/01-getting-started.md) · [Scenarios](docs/02-scenarios.md) · [Commands by scenario](docs/09-commands-by-scenario.md) |
| **Use** | [Packages and versions](docs/03-packages-and-versions.md) · [Projects](docs/04-projects.md) · [Indexes and mirrors](docs/05-indexes-and-mirrors.md) · [Self-management](docs/06-self-management.md) |
| **SubOS** | [SubOS](docs/10-subos.md) · [Sandbox and policy](docs/11-sandbox-and-policy.md) · [Roots and generations](docs/12-roots-and-generations.md) · [macOS and Windows](docs/13-subos-on-macos-and-windows.md) |
| **Extend** | [Authoring a package](docs/30-authoring-a-package.md) · [Hosting an index](docs/31-hosting-an-index.md) · [SubOS templates](docs/32-authoring-a-subos-template.md) |
| **Contracts** | [Agents and the machine interface](docs/50-agents-and-machine-interface.md) · [Supported platforms](docs/51-supported-platforms.md) · [Specifications](docs/specs/README.md) · [Commands](docs/generated/command-reference.md) |
| **Luban** | [Luban OS](docs/luban/README.md) — environments, editions, images and machines on xlings |
| **xlings itself** | [Build from source](docs/90-build-from-source.md) · [Architecture](docs/91-architecture.md) · [Release](docs/92-release.md) |

All of it: [docs/](docs/README.md).

## Ecosystem and community

- [MCPP](https://github.com/mcpp-community/mcpp) — modern C++ toolchain, distributed through xlings
- [xim-pkgindex](https://github.com/openxlings/xim-pkgindex) — the official package index
- [Luban](docs/luban/README.md) — the OS model on xlings: environments, editions, bootable images
- [Forum](https://forum.d2learn.org/category/9/xlings) · QQ 167535744 / 1006282943 · [Issues](https://github.com/openxlings/xlings/issues)
- Contributing: [issues](https://xlings.d2learn.org/en/documents/community/contribute/issues.html) · [packages](https://xlings.d2learn.org/en/documents/community/contribute/add-xpkg.html) · [docs](https://xlings.d2learn.org/en/documents/community/contribute/documentation.html)

<a href="https://github.com/openxlings/xlings/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=openxlings/xlings" />
</a>

[![Star History Chart](https://api.star-history.com/svg?repos=openxlings/xlings,openxlings/xim-pkgindex&type=Date)](https://star-history.com/#openxlings/xlings&openxlings/xim-pkgindex&Date)
