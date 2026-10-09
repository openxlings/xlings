<div align=center>
  <img width="120" src="https://xlings.d2learn.org/imgs/xlings-logo.png">

  <h1>xlings</h1>

  <em>Universal package infrastructure with OS-like SubOS isolation.<br/>
  Multi-version · Rootless · Decentralized Index · Agent-ready.</em>

  <b> [Website] | [Docs] | [Package Index] | [Forum] </b>

  [中文](README.zh.md) | English
</div>

[Website]: https://openxlings.github.io/
[Docs]: docs/
[Package Index]: https://openxlings.github.io/xim-pkgindex
[Forum]: https://forum.d2learn.org/category/9/xlings

<p align=center>
  <em>Used by: <a href="https://github.com/mcpp-community/mcpp">MCPP</a> · upcoming <b>Luban</b> Linux</em>
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

**Several toolchains, no conflicts** — [guide](docs/quick-start/multi-version.md)

```bash
xlings install gcc@16 gcc@11 node@24
xlings use gcc@11        # switch back any time; both stay installed
```

**One environment for the whole team** — [guide](docs/quick-start/project-env.md)

```json
{ "workspace": { "xmake": "3.0.7", "gcc": { "linux": "16.1.0" }, "llvm": { "macosx": "20.1.7" } } }
```

```bash
cd my-project/ && xlings install     # the project's own SubOS, the declared versions
```

**Agents and untrusted code in a sandbox** — [guide](docs/quick-start/subos-and-agent.md)

```bash
xlings subos new agent-ws --sandbox=private      # isolation is declared once, here
xlings subos exec agent-ws -- python run.py      # every entry is that sandbox; its own exit code
xlings subos use agent-ws                        # or a shell inside -- no flag needed
```

| Platform | Release | `--sandbox` isolates |
|---|---|---|
| Linux x86_64 / aarch64 | ✅ | filesystem, processes, network, identity (bwrap; on aarch64, once a bwrap is available for it) |
| macOS 14+ arm64 | ✅ | **only `$HOME`** |
| Windows x86_64 | ✅ | **only `%USERPROFILE%`** |

> On macOS and Windows `--sandbox` is not a security boundary: use an OS
> sandbox or a VM for code you do not trust. `xlings subos status <name>`
> shows what a SubOS asks for and what this machine gives it.

**Upgrade and repair** — [guide](docs/quick-start/self-management.md)

```bash
xlings self update
xlings self doctor --fix
```

## Documentation

| Topic | Docs |
|---|---|
| **Guides** | [Multi-version](docs/quick-start/multi-version.md) · [Project env](docs/quick-start/project-env.md) · [SubOS & Agent](docs/quick-start/subos-and-agent.md) · [Custom index](docs/quick-start/custom-index.md) · [Self-management](docs/quick-start/self-management.md) · [Build from source](docs/build-from-source.md) |
| **Design** | [Architecture](docs/architecture/overview.md) · [SubOS isolation](docs/design/subos-isolation.md) · [SubOS-as-XPKG](docs/design/subos-as-xpkg.md) · [xvm versioning](docs/design/xvm-version-management.md) · [Index ecosystem](docs/design/package-index-ecosystem.md) · [Interface protocol](docs/design/interface-protocol.md) |
| **Spec** | [xpkg manifest v1](docs/spec/xpkg-manifest-v1.md) · [.xlings.json schema](docs/spec/xlings-json-schema.md) · [Interface NDJSON v1](docs/spec/interface-ndjson-v1.md) · [Commands](docs/generated/command-reference.md) |
| **Compare** | [xlings vs apt / nix / docker](docs/comparison.md) |

## Ecosystem and community

- [MCPP](https://github.com/mcpp-community/mcpp) — modern C++ toolchain, distributed through xlings
- [xim-pkgindex](https://github.com/openxlings/xim-pkgindex) — the official package index
- **Luban Linux** — upcoming distribution using xlings as its system package manager
- [Forum](https://forum.d2learn.org/category/9/xlings) · QQ 167535744 / 1006282943 · [Issues](https://github.com/openxlings/xlings/issues)
- Contributing: [issues](https://xlings.d2learn.org/en/documents/community/contribute/issues.html) · [packages](https://xlings.d2learn.org/en/documents/community/contribute/add-xpkg.html) · [docs](https://xlings.d2learn.org/en/documents/community/contribute/documentation.html)

<a href="https://github.com/openxlings/xlings/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=openxlings/xlings" />
</a>

[![Star History Chart](https://api.star-history.com/svg?repos=openxlings/xlings,openxlings/xim-pkgindex&type=Date)](https://star-history.com/#openxlings/xlings&openxlings/xim-pkgindex&Date)
