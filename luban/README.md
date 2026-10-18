# luban/

Luban OS: its components and services, and the specifications and reusable
components it is built from. Three kinds of package live here, one directory
each; every package is its own `mcpp.toml`.

| Directory | What | May import | Moves out |
|---|---|---|---|
| `specs/` | Specifications: a contract, its declarations, its conformance check (`README.md`, `SPEC.md`, `src/`, `conformance/`) | the standard library and itself; never another specification | with its second implementer, or a user outside this repository |
| `kits/` | Implementations of specifications and reusable components | the standard library, specifications, other kits, their C libraries; never `xlings.*` or `luban.*` | with a user outside this repository |
| `os/` | Luban OS itself: stage-0, the service, machines, the `luban` command, the Luban OS app's library | `modules/`, specifications, kits | stays |

`specs/` and `kits/` are kept as independent projects: their names do not say
Luban, their comments and documents do not point into this repository, their
versions move on their own. Moving one to its own repository is a subtree
split and a changed dependency line. `tools/lint_layer_deps.sh` checks the
imports; `tools/lint_c_headers.sh` and `tools/lint_platform_headers.sh` keep C
and system headers in the binding modules (`src/native/`, `xdesk.os`).

The specifications are perspectives on one machine, not layers: openkal
(a kernel's resources; its own repository, a dependency), openhal (which
devices), opensal (a surface), openas (who did what, and who may). They do not
reference one another; implementations connect them.

## specs/

- `opensal` -- a surface: given pixels, gives back events; `opensal.wire` carries one across an address space.
- `openhal` -- devices: which GPUs, displays, audio endpoints and input devices a machine has.
- `openas` -- actors (people, agents, programs, services), what they did and what they may do: the envelope, the actor, the effect classes.
- `opencaps` -- what the parts of a system provide and need, by name and level, so that parts released apart can tell whether they work together.

## kits/

- `opensal-wlr` -- a wlroots compositor's output as an opensal surface.
- `openhal-host` -- openhal's provider on Linux, macOS and Windows.
- `xdesk-base`, `xdesk-layout`, `xdesk-compositor` -- the xdesk desktop components (`xdesk-layout`: where windows go, pure logic).

## os/

- `system` -- stage-0 (`luban-init`), boot, machines, archives.
- `service` -- environments and machines: their life, their events, what this computer can do.
- `vm` -- the machine runtime: supervisor, terminal relay, guest sessions, hypervisor backends.
- `cli` -- the `luban` command.
- `desktop` -- the Luban OS app's window (SDL3 as opensal's host provider) and Dear ImGui.
