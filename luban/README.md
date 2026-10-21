# luban/

Luban OS: its components and services, and the specifications, reusable
components and desktop it is built from. Each kind of package has its own
directory; every package is its own `mcpp.toml` (design 2026-10-19 §28).

| Directory | What | May import | Moves out |
|---|---|---|---|
| `specs/` | Specifications: a contract, its declarations, its conformance check (`README.md`, `SPEC.md`, `src/`, `conformance/`) | the standard library and itself; never another specification | with its second implementer, or a user outside this repository |
| `kits/` | Libraries linked into a program: a specification's in-process implementation or a reusable component -- no process, endpoint or state of their own | the standard library, specifications, other kits, their C libraries and mcpp-index packages; never `xlings.*` or `luban.*` | with a user outside this repository |
| `services/` | Base services: a process of its own (or one a system's agent hosts) that owns an endpoint and its state, has a lifecycle and says what it does (statistics, logs); its library is how programs reach it (`src/`), its program in `tool/` | as a kit, and kits | with a user outside this repository |
| `xdesk/` | The xdesk desktop, one project: its libraries and its program (`base/`, `layout/`, `compositor/`, `app/`) | as a kit, and services' libraries (how a program reaches a service: XBus) | as a whole |
| `apps/` | Programs that run inside a Luban system and are not a project of their own (made with its first member) | `modules/`, specifications, kits | stays |
| `os/` | Luban OS itself: stage-0, the service, machines, the `luban` command, the Luban OS app's window, compatibility | `modules/`, specifications, kits | stays |
| `host/` | Luban's programs on the host: `luban` (and stage-0 as `luban-init`), `luban-os` (the app) | as `os/` | stays |
| `tests/` | Luban's tests (`unit/`, `e2e/`, the app's layout baselines in `luban-os/`) and its `requirements.toml` | as `os/` | stays |
| `tools/` | Luban's release scripts (luban, luban-os, xdesk, the pointer theme) and the app's icon and keyframe tools | -- | stays |

A package with one module keeps its files in `src/`; one with several has a
directory per module (`native/` for C bindings). xlings' own programs on the
host (gui, xdev) are in the top-level `apps/`.

## Luban and xlings

Luban is kept in this repository until it has its own (design 2026-10-20
WuXian §16): `luban/` with its tests, tools and documents (`docs/luban`) moves
as a whole, a directory split and changed dependency lines. What it uses of
xlings is in three layers, and `DEPENDENCIES.toml` says which each package may
import (`tools/lint_layer_deps.sh`):

- **base** -- libraries that know nothing of packages: the platform, json and
  sha, words, the journal, the command-line model, the test kit;
- **shared** -- what both read and the one door to what both do: a system
  root's format, the home's layout, images, carriers, the external programs'
  table, the administrator's door;
- **xlings' domain** -- packages, indexes, the store, isolation policy,
  sessions -- only through a protocol: the `Port` capabilities and xlings' JSON
  events. What still imports it is listed under `[migrating]` with what replaces
  it; a new use fails.

Luban's tests are found with xlings' (`mcpp.toml` `[test] discover`) and named
the same way (`unit/test_luban_cli`); `xdev report` reads both requirements
files.

`specs/`, `kits/` and `services/` are kept as independent projects: their names do not say
Luban, their comments and documents do not point into this repository, their
versions move on their own. Moving one to its own repository is a subtree
split and a changed dependency line. `tools/lint_layer_deps.sh` checks the
imports; `tools/lint_c_headers.sh` and `tools/lint_platform_headers.sh` keep C
and system headers in the binding modules (`src/native/`, `xdesk.os`).

The specifications are perspectives on one machine, not layers: openkal
(a kernel's resources; its own repository, a dependency), openhal (which
devices), opensal (a surface), openas (who did what, and who may), opencaps (what
parts provide and need), openwx (the plane a desktop is). They do not
reference one another; implementations connect them.

## specs/

- `opensal` -- a surface: given pixels, gives back events; `opensal.wire` carries one across an address space.
- `openhal` -- devices: which GPUs, displays, audio endpoints and input devices a machine has.
- `openas` -- actors (people, agents, programs, services), what they did and what they may do: the envelope, the actor, the effect classes.
- `opencaps` -- what the parts of a system provide and need, by name and level, so that parts released apart can tell whether they work together.
- `openwx` -- open WuXian: one infinite plane a desktop is, workspaces on it as windows of a kind, two views; its state (`wuxian.json`) and conformance cases.

## kits/

- `opensal-wlr` -- a wlroots compositor's output as an opensal surface.
- `openhal-host` -- openhal's provider on Linux, macOS and Windows.
- `imkit` -- immediate-mode widgets over Dear ImGui.
- `tray` -- a status-area icon and its menu (StatusNotifierItem, the macOS menu bar, the Windows notification area).
- `i18n` (i18nkit) -- words in a person's language: catalogues by domain and language, a fallback chain to English, a check of the catalogues; xlings.i18n and xdesk.words are built on it.
- `telemetry` -- what a program says about itself: counters, gauges and histograms by name, a flight recorder, a structured log (one JSON object a line), profiling spans exported as a trace, and dumps; `telemetry::control` is the whole interface a bus or a control socket carries (openbus `openbus.telemetry@0.1`).
- `cursor` (cursorkit) -- pointers drawn from vector shapes at exactly the size asked, Xcursor themes, and the shake that says someone is looking for the pointer; its tool `cursor-make` makes the Luban pointer theme (`os/window/cursors/luban.path`).

## services/

What tells a service from a kit: a kit is linked and does nothing on its own;
a service runs -- one per host or per system -- and is reached, by its
library, over its protocol. Every service is observable the same way (its
statistics, its log, a command that shows both) and has a specification it
implements.

- `xbus` -- openbus's implementation: the router (one per host and per system), the node library, links between routers, the `xbus` command (`ls`, `call`, `pub`, `sub`, `monitor`, `top`, `stats`, `bench`, `router`).

## xdesk/

- `base` (xdesk-base) -- the system calls, a terminal's screen, applications, settings.
- `layout` (xdesk-layout) -- where windows go: pure logic.
- `compositor` (xdesk-compositor) -- the Wayland compositor whose output is an opensal surface, its drawing and shell.
- `app` (xdesk-app) -- the `xdesk` program a Luban system's desktop session runs.

## os/

- `system` -- stage-0 (`luban-init`), boot, machines, archives.
- `service` -- environments and machines: their life, their events, what this computer can do.
- `vm` -- the machine runtime: supervisor, terminal relay, guest sessions, hypervisor backends.
- `cli` -- the `luban` command.
- `window` -- the Luban OS app's window (SDL3 as opensal's host provider) and Dear ImGui.
- `compat` -- what is kept only for a released older part, each with when it goes.
