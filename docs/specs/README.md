# Specifications

Formats, protocols and their rules, for someone implementing against them: index
authors, agent and tool authors. Each specification says at its top whether it is
in force, since which version, and where it is implemented; additions are marked
with the client version that reads them. The chapters explain how to use these;
the specifications say exactly what is accepted.

Specifications are in English only.

| Specification | What it governs |
|---|---|
| [xpkg manifest v1](xpkg-manifest-v1.md) | a package recipe: fields, the platform–version matrix, revisions, hooks, dependencies, version constraints |
| [xpkg resource resolution](xpkg-resource-resolution.md) | how a recipe's resources resolve to a URL, a mirror and a sha256 |
| [`.xlings.json`](xlings-json.md) | the configuration file of a home, a project and a SubOS workspace |
| [NDJSON interface v1](interface-ndjson-v1.md) | `xlings interface`: transport, events, capabilities, errors |
| [Diagnostics](diagnostics.md) | how a problem is reported: one marker, its evidence and its way out; severities and stable codes |
| [Themes](themes.md) | colour themes: nine role slots, the file format, failure behaviour |
| [Index distribution](index-distribution.md) | how an index reaches a machine: artefacts, pointers, regions, bounds, self-hosting |
| [Index version contract](index-version-contract.md) | how a snapshot declares the clients it needs, and how clients are routed |
| [Upgrade and compatibility](upgrade-and-compatibility.md) | failure domains, the upgrade a new client runs by itself, what an older client does with newer state, releases |

The command reference is generated from the command tree:
[xlings](../generated/command-reference.md),
[luban](../generated/luban-command-reference.md).
