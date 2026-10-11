# Documentation

**English** | [简体中文](zh/README.md)

This tree is the **manual for what xlings implements**: each chapter says what
a capability does, how it is used, and where it stops. Why a design is the way
it is, the alternatives that were turned down, and work that is planned rather
than shipped live in the design records, not here.

## Where a document lives

| tree | reader | contents |
|---|---|---|
| `docs/**` | someone with a task in hand | how to use what xlings implements |
| [`docs/luban/**`](luban/README.md) | someone running Luban environments, images or machines | Luban OS: editions, environments, images, the machine |
| [`docs/specs/**`](specs/README.md) | someone implementing against a format or a protocol: index authors, agent and tool authors | formats, protocols and their rules |
| [`docs/generated/**`](generated/command-reference.md) | anyone looking up a command | the command references, generated from the command trees |
| `.agents/docs/**` | whoever changes xlings, and whoever later asks why | design records, measurements, what was refuted |

A chapter points to a specification for exact rules; it does not point to a
design record — a record describes a moment and promises nothing, so what a
reader needs is written here or in a specification.

## Start here

| To | Read |
|---|---|
| install xlings and a first package | [01](01-getting-started.md) |
| keep several versions of a tool | [03](03-packages-and-versions.md) |
| give a project its own toolchain | [04](04-projects.md) |
| run an agent or untrusted code in a sandbox | [10](10-subos.md), [11](11-sandbox-and-policy.md), [50](50-agents-and-machine-interface.md) |
| make an environment, an image or a machine of its own | [Luban](luban/README.md) |
| publish packages, or an index of your own | [30](30-authoring-a-package.md), [31](31-hosting-an-index.md) |
| repair a home that misbehaves | [06](06-self-management.md) |
| build xlings itself | [90](90-build-from-source.md) |

## Chapters

The first digit is the part; within a part the order is a reading order.

| | |
|---|---|
| `0x` | the fundamentals |
| `1x` | SubOS: environments, sandboxes, roots |
| `3x` | extending xlings: packages, indexes, templates |
| `5x` | the contracts a program may rely on |
| `9x` | xlings itself |

### 0x — Fundamentals

- [00 — What xlings Is](00-what-xlings-is.md) — what it does, and how it differs from apt, nix and docker
- [01 — Getting Started](01-getting-started.md) — install, a first package, where to go next
- [02 — Scenarios](02-scenarios.md) — common tasks and the features each uses
- [03 — Packages and Versions](03-packages-and-versions.md) — install, use, remove; many versions side by side
- [04 — Projects](04-projects.md) — `.xlings.json`: a project's own toolchain
- [05 — Indexes and Mirrors](05-indexes-and-mirrors.md) — where packages come from
- [06 — Self-Management](06-self-management.md) — update, doctor, clean
- [09 — Commands by Scenario](09-commands-by-scenario.md) — the lookup, once the nouns are known

### 1x — SubOS

- [10 — SubOS](10-subos.md) — named environments: create, enter, run, share, remove
- [11 — Sandbox and Policy](11-sandbox-and-policy.md) — isolation, network, identity, grants, audit
- [12 — Roots and Generations](12-roots-and-generations.md) — a SubOS as `/`: generations, rollback, export, boot
- [13 — SubOS on macOS and Windows](13-subos-on-macos-and-windows.md) — what each platform enforces; Linux SubOS on Windows

### 3x — Extending xlings

- [30 — Authoring a Package](30-authoring-a-package.md) — an xpkg recipe
- [31 — Hosting an Index](31-hosting-an-index.md) — a package index of your own
- [32 — Authoring a SubOS Template](32-authoring-a-subos-template.md) — `type = "subos"` packages

### 5x — Contracts for programs

- [50 — Agents and the Machine Interface](50-agents-and-machine-interface.md) — agent mode, `--json`, the NDJSON interface
- [51 — Supported Platforms](51-supported-platforms.md) — platforms, what a machine must provide, versions and compatibility

### 9x — xlings itself

- [90 — Building from Source](90-build-from-source.md)
- [91 — Architecture](91-architecture.md) — modules, data layout, how an install and a SubOS work inside
- [92 — Releasing](92-release.md)

## Look it up

**Commands**

| | chapter | | chapter |
|---|---|---|---|
| `install`, `use`, `remove`, `list`, `search` | [03](03-packages-and-versions.md) | `update`, `index` | [05](05-indexes-and-mirrors.md) |
| `self update`, `self doctor`, `self clean` | [06](06-self-management.md) | `config` | [05](05-indexes-and-mirrors.md), [06](06-self-management.md) |
| `subos new / use / exec / start / cp / remove` | [10](10-subos.md) | `subos config / status / log / report` | [11](11-sandbox-and-policy.md) |
| `subos rollback / export / boot / pack` | [12](12-roots-and-generations.md) | `interface` | [50](50-agents-and-machine-interface.md) |
| every command and option | [generated](generated/command-reference.md) | `luban …` | [luban](luban/README.md), [generated](generated/luban-command-reference.md) |

**Files and keys**

| | where |
|---|---|
| `.xlings.json` (home, project, SubOS) | [04](04-projects.md), [spec](specs/xlings-json.md) |
| an xpkg recipe (`pkgs/<x>/<name>.lua`) | [30](30-authoring-a-package.md), [spec](specs/xpkg-manifest-v1.md) |
| a SubOS policy (`policy.json`) | [11](11-sandbox-and-policy.md) |
| the NDJSON interface | [50](50-agents-and-machine-interface.md), [spec](specs/interface-ndjson-v1.md) |
| diagnostics and exit codes | [specs/diagnostics](specs/diagnostics.md), [10](10-subos.md) |
| themes | [specs/themes](specs/themes.md) |

## Specifications

- [specs/](specs/README.md) — the index of all specifications
