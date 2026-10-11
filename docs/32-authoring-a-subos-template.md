# Authoring a SubOS Template

**English** | [简体中文](zh/32-authoring-a-subos-template.md)

A SubOS can be distributed as a package (`type = "subos"`): installing it is
installing a template, and `subos new --from` makes an instance from it in
almost no time — the payloads it declares are shared, not copied.

## How it works

```mermaid
sequenceDiagram
    participant A as author
    participant R as index
    participant U as user or agent
    participant S as instance
    A->>R: publishes a type="subos" package (.xlings.json + files)
    U->>U: xlings install subos:py-ds@1.0.0
    U->>S: xlings subos new exp --from subos:py-ds@1.0.0
    Note over S: the declaration and files are copied; payloads are shared
    U->>S: xlings subos exec exp -- python run.py
```

## A template

```lua
package = {
    spec = "1",
    name = "py-ds",
    namespace = "subos",
    type = "subos",
    archs = {"x86_64", "aarch64"},
    xpm = {
        linux = {
            deps = { "python@3.12", "numpy@2.1" },
            ["latest"] = { ref = "1.0.0" },
            ["1.0.0"] = {},
        },
    },
}
```

With no hooks, the built-in handler makes the skeleton and records the
template's declaration; the dependencies become the instance's workspace. A
template may carry files (a payload with `.xlings.json` and anything else at its
root) and may define `install()`, `config()`, `uninstall()` to do more.

For a SubOS that is a root, the template is a manifest (`subos_kind: "rootfs"`,
`from`, `abi`, `packages`, `policy`, `boot`); the official ones are the Luban
editions — [Luban: editions](luban/02-editions.md) describes the manifest.

## Using it

```bash
xlings install subos:py-ds@1.0.0                   # the template itself
xlings subos new exp --from subos:py-ds@1.0.0      # an instance of it
xlings subos new exp2 --from exp                   # from an instance
xlings subos exec exp -- python -V
xlings subos remove exp
```

## Choices made

| | |
|---|---|
| format | an ordinary xpkg with `type = "subos"`: the same install pipeline |
| path | `xpkgs/<ns>-x-<name>/<ver>/`, as every package |
| a new instance | a copy of the declaration and files (copy-on-write where the filesystem has it); payloads shared |
| storage | chosen when the instance is made, never by the template |
| upgrades | explicit: versions of a template coexist; an instance moves with `subos upgrade` |
