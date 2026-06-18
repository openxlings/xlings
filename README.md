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

---

## Why xlings?

One tool to install any version of anything, run it rootless, and isolate it OS-like — across Linux, macOS, and Windows.

| | apt / brew | nix | docker | **xlings** |
|---|:---:|:---:|:---:|:---:|
| Multi-version coexistence | ❌ | ✅ | ✅ | ✅ |
| Rootless | ❌ | ⚠️ | ⚠️ | ✅ (except image mode) |
| No daemon | ✅ | ✅ | ❌ | ✅ |
| Cross-platform | ❌ | ⚠️ | ✅ | ✅ Linux / macOS / Windows |
| Isolation granularity | ❌ | FS | FS+ | 🔒 shell / FS / image (3 levels) |
| Storage reuse | — | ✅ store | ❌ image bloat | ✅ version-view + refcount |
| Decentralized index | ❌ | ❌ | ❌ | ✅ official + 3rd + self-hosted |
| Agent / JSON interface | ❌ | ❌ | ⚠️ API | ✅ `xlings interface` (NDJSON) |
| OS-level pkg mgr | apt is | NixOS | ❌ | ✅ (Luban Linux, upcoming) |

---

## Quick Start

**Install — Linux / macOS**

```bash
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
```

**Install — Windows (PowerShell)**

```powershell
irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
```

```bash
xlings install gcc@16 node@24 cmake   # install (version optional)
xlings use gcc@16                      # switch active version
xlings search python                   # search packages
xlings list                            # list installed
```

### Let an AI agent install & explain it

Paste this to any AI agent (Claude, Codex, OpenCode, …):

```
Read the README of https://github.com/openxlings/xlings and install xlings on my machine.
- Linux/macOS: curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash
- Windows: irm https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.ps1 | iex
Then run `xlings agent usage` and follow it to show me how to use xlings.
```

xlings ships a built-in agent guide — once installed, any agent can self-learn the full workflow:

```bash
xlings agent           # overview + skill list
xlings agent usage     # complete usage guide written for LLM agents
```

---

## Usage scenarios

### 🛠 Multi-version toolchains, one command everywhere

Install any version of anything and keep several side by side — switch instantly, no conflicts, no sudo. The same commands work on Linux, macOS, and Windows.

```bash
xlings install gcc@16 gcc@11 node@24 cmake
xlings use gcc@11        # switch back anytime — both stay installed
```

→ [Multi-version guide](docs/quick-start/multi-version.md)

### 📦 Reproducible project environments, consistent across OSes

Commit a `.xlings.json` that declares per-platform versions. Each project gets its **own isolated SubOS**, so teammates and CI — on any distro or OS — land in the exact same environment, without touching the host or other projects.

```json
{
  "workspace": {
    "xmake": "3.0.7",
    "gcc":  { "linux":  "16.1.0" },
    "llvm": { "macosx": "20.1.7", "windows": "19.1.0" }
  }
}
```

```bash
cd my-project/           # entering the dir activates the project SubOS
xlings install           # installs the declared versions, project-local
```

→ [Project env guide](docs/quick-start/project-env.md)

### 🤖 Agents & untrusted code in an isolated SubOS

Run claude / codex / opencode — or any untrusted code — **inside** a rootless SubOS: full permissions within, host untouched. Fork from a base env in seconds and run several isolated instances on one machine.

```bash
xlings subos new agent-ws --from subos:dev-env@latest
xlings subos use agent-ws --sandbox                       # enter the isolated world
xlings subos use agent-ws --sandbox --cmd "python run.py" # or one-shot exec
```

→ [SubOS & Agent guide](docs/quick-start/subos-and-agent.md)

---

## Core capabilities

1. 📦 **Universal package infrastructure** — binary / script / config / subos / tutorial, all as xpkg
2. 🔀 **Multi-version coexistence** — N versions side-by-side; version-view + ref-counting (N envs ≈ 1× storage)
3. 🏗️ **3-level SubOS isolation** — shell (env switch) / FS (bwrap/proot, rootless) / image (ext4, root)
4. 🌐 **Decentralized package index** — official + 3rd-party + self-hosted; resource servers for binary mirrors
5. 🤖 **JSON event interface** — `xlings interface` (NDJSON) for AI agents, CI, and 3rd-party tooling

---

## Documentation

Guides, design notes, and specs live in [`docs/`](docs/).

| Area | Docs |
|------|------|
| **Get started** | [Multi-version](docs/quick-start/multi-version.md) · [Project env](docs/quick-start/project-env.md) · [SubOS & Agent](docs/quick-start/subos-and-agent.md) · [Custom index](docs/quick-start/custom-index.md) |
| **Architecture** | [System overview](docs/architecture/overview.md) |
| **Design** | [SubOS-as-XPKG](docs/design/subos-as-xpkg.md) · [xvm versioning](docs/design/xvm-version-management.md) · [SubOS isolation](docs/design/subos-isolation.md) · [Index ecosystem](docs/design/package-index-ecosystem.md) · [Interface protocol](docs/design/interface-protocol.md) |
| **Spec** | [xpkg manifest v1](docs/spec/xpkg-manifest-v1.md) · [.xlings.json schema](docs/spec/xlings-json-schema.md) · [Interface NDJSON v1](docs/spec/interface-ndjson-v1.md) |

---

## Build from source

```bash
xlings install           # reads .xlings.json → installs mcpp build toolchain
mcpp build
mcpp test
```

The same `.xlings.json` drives CI and the release pipeline.

---

## Ecosystem

| Project | Role | Link |
|---------|------|------|
| **MCPP** | Modern C++ build toolchain ecosystem — distributed through xlings | [github.com/mcpp-community/mcpp](https://github.com/mcpp-community/mcpp) |
| **Luban Linux** | Upcoming Linux distribution using xlings as system-level package manager | *(link when published)* |
| **xim-pkgindex** | Official package index — 60+ packages and growing | [openxlings/xim-pkgindex](https://github.com/openxlings/xim-pkgindex) |

---

## Community

- **Forum**: [forum.d2learn.org/category/9/xlings](https://forum.d2learn.org/category/9/xlings)
- **QQ Groups**: 167535744 / 1006282943
- **Issues**: [github.com/openxlings/xlings/issues](https://github.com/openxlings/xlings/issues)

### Contributing

- [Issue handling & bug fixing](https://xlings.d2learn.org/en/documents/community/contribute/issues.html)
- [Adding new packages](https://xlings.d2learn.org/en/documents/community/contribute/add-xpkg.html)
- [Documentation](https://xlings.d2learn.org/en/documents/community/contribute/documentation.html)

---

**Contributors**

<a href="https://github.com/openxlings/xlings/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=openxlings/xlings" />
</a>

[![Star History Chart](https://api.star-history.com/svg?repos=openxlings/xlings,openxlings/xim-pkgindex&type=Date)](https://star-history.com/#openxlings/xlings&openxlings/xim-pkgindex&Date)
