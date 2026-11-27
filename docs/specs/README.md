# Specifications

Formats, protocols and their rules, for someone implementing against them: index
authors, agent and tool authors. Each specification says at its top whether it is
in force, since which version, and where it is implemented; additions are marked
with the client version that reads them. The chapters explain how to use these;
the specifications say exactly what is accepted.

Specifications are in English only.

| Specification | What it governs |
|---|---|
| [The xpkg format](xpkg.md) | a package recipe: `spec`, the package table, the platform–version matrix and architectures, landing, hooks and their environment, declarations, the index's declarations, the checker. A byte copy of libxpkg's `docs/spec/xpkg.md` of the version xlings builds with |
| [Payload rules](payload-rules.md) | what an installed payload may contain (rules A, B, D, E, M, P), where each is enforced, and the index's check `xlings __payload-check` |
| [xpkg resource resolution](xpkg-resource-resolution.md) | how a recipe's resources resolve to a URL, a mirror and a sha256 |
| [`.xlings.json`](xlings-json.md) | the configuration file of a home, a project and a SubOS workspace |
| [Bundles](bundle.md) | a directory of archives and index snapshots for a machine without a network: `bundle.toml`, `download --to`, `install --from` |
| [NDJSON interface v1](interface-ndjson-v1.md) | `xlings interface`: transport, events, capabilities, errors |
| [Diagnostics](diagnostics.md) | how a problem is reported: one marker, its evidence and its way out; severities and stable codes |
| [Themes](themes.md) | colour themes: nine role slots, the file format, failure behaviour |
| [Index distribution](index-distribution.md) | how an index reaches a machine: artefacts, pointers, regions, bounds, self-hosting |
| [Index version contract](index-version-contract.md) | how a snapshot declares the clients it needs, and how clients are routed |
| [Operation journal](operation-journal.md) | what a command did: one file per operation, its lines, the trace its processes share, what is never written |
| [Upgrade and compatibility](upgrade-and-compatibility.md) | failure domains, the upgrade a new client runs by itself, what an older client does with newer state, releases |
| [Derived state](derived-state.md) | state derived from a fact carries it and is checked where it is used: removal and repair, host facts, the recipe a payload keeps, the shell profile |
| [Store roots](store-roots.md) | what the store's collection counts as a reference, and the roots another program registers |

The command reference is generated from the command tree:
[xlings](../generated/command-reference.md),
[luban](../generated/luban-command-reference.md). The hook interface's
reference is generated from libxpkg's interface manifest:
[xpkg hook API](../generated/xpkg-hook-api.md).

"xpkg manifest v1" ([xpkg-manifest-v1.md](xpkg-manifest-v1.md)) is replaced by
[the xpkg format](xpkg.md); the old address points there.
