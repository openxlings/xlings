# AGENTS.md

`xlings` is a package manager with OS-like SubOS isolation: one static binary,
C++23 modules, Linux / macOS / Windows. **Luban** is the OS model on top of it
(environments, machines, images; `luban` CLI and the Luban OS app).
Design records live in `.agents/docs/` (the current architecture:
`2026-10-12-luban-os-architecture-design.md`); user docs in `docs/` (en, mirror in `docs/zh/`).

## Layout and layers

```
modules/   shared libraries (libs, runtime, platform, home, i18n, ui, cli, subos, confine, carrier, image, testkit)
luban/     specs/ (opensal, openhal, openas, opencaps, openwx, openbus), kits/ (libraries: their
           in-process implementations, reusable components), services/ (base services with a process
           and a protocol: xbus), xdesk/ (the desktop: base, layout, compositor, app) -- independent
           projects kept here -- apps/ (programs inside a system), os/ (Luban OS: stage-0, service,
           vm, the luban command, compat, the app's window), host/ (luban and luban-os, the programs
           on the host), tests/ (Luban's tests and requirements.toml), tools/ (Luban's release scripts)
apps/      xlings' programs on the host (gui, xdev)
src/       the xlings frontend and core still being moved into modules/
tests/     unit/ and e2e/ (C++, XTEST + testkit), legacy e2e/*.sh, requirements.toml
```

Imports go downward only (`tools/lint_layer_deps.sh`): `luban/` -> `modules/`;
`modules/` never imports `luban.*` or `src/`; `src/` never imports `luban.*`. In `luban/`:
a spec imports only the standard library and itself; a kit (and xdesk) never imports `xlings.*` or
`luban.*`; `os/service` imports no presentation (`luban/README.md`).
What xlings and Luban both read (boot.json, a root's /etc, image formats) is a module.
A kit (`luban/kits/*`) is an independent project that xlings may build on too
(`xlings.i18n` is the i18n kit; nlohmann.json comes from mcpp-index, one copy).
Luban is kept here until it has its own repository: what of xlings each Luban package may
import is `luban/DEPENDENCIES.toml` (base and shared modules; xlings' domain through `Port`).

## Build and test

```bash
xlings install                 # tools from .xlings.json (mcpp)
mcpp build                     # xlings; `mcpp build --workspace` for every package
XLINGS_TEST_MIRROR=CN mcpp test [pattern]   # set the mirror when testing from China
```

New tests are C++ (`modules/testkit`, `XTEST`, `Home::isolated`); every test
`covers` a requirement ID in `tests/requirements.toml` (Luban's: `luban/tests/`). Tests use an isolated
home and never touch the user's. Release builds: `tools/{linux,macos}_release.sh`,
`tools/windows_release.ps1`. How CI splits the tests over machines (shards, workers,
one build reused per round) is `tests/lanes.toml`; `xdev test -j N --shard i/n` does
the same locally (`tests/README.md`).

## Rules the code is checked against

- **System headers and calls live in `modules/platform`** (plus the desktop
  backends). Branch with `if constexpr (platform::is_windows)`, not `#if`; an `#if`
  elsewhere needs `// platform-if-ok: <why>`.
- **C headers stay in their binding modules**: a library's header is read in one
  module that exports the names used (macros as constants) and is imported
  everywhere else (`xdesk.native.*`, `luban.desktop.imgui`; `tools/lint_c_headers.sh`).
- **External programs come from one table** (`xlings.subos.tools`) and administrator
  rights through one door (`xlings.subos.elevation`, logged; never asked where
  nobody can answer). A host dependency is T0 (the OS), T1 (a package in the index:
  hint `xlings install <pkg>`) or T2 (done in-process). Never a host package manager.
- **Core returns structured results; frontends render.** Questions go through the
  `Asker` port. Agent mode (`--agent`, `XLINGS_AGENT_MODE=1`) never waits for input:
  it fails with the exact flag that answers it.
- **Default output is the conclusion and the next step**; `-v` adds steps; details
  and external tools' output go to logs under `<home>/logs/`.
- **The command tree is designed** (`src/cli/spec.cpp`, `luban.cli`): a capability is
  configuration first; a new top-level command is discussed first; plumbing starts
  with `__` and is never in help or completion; every addition updates the spec,
  `docs/generated/`, and a test.
- **"Could not read" is never "empty".** A broken file stops only what needs it and
  is named with its fix (`docs/specs/upgrade-and-compatibility.md`).
- **A SubOS's home is user data**: only a confirmed SubOS deletion removes it
  (`subos::userdata::delete_subos`, `tools/lint_subos_remove_all.sh`); derived state
  (payloads, shims, generations, registrations) may be rebuilt by any flow.
- **The home's format moves forward** (`xlings.home.format`): readers tolerate the
  previous layout, steps bring an older home forward once, idempotently, recorded.
  The marker's integer `layout` rises only for a change an older client would undo.
- **A recipe that changes what its config does bumps `revision`.** Dependency names
  resolve in the declaring index.
- **A recipe never runs `sudo`.** It declares the step (`system.admin_step`); xlings asks
  once after the install through the one door, or keeps it for `self doctor --fix`. A child
  run for nobody (a repair, a JSON call) has no terminal (`platform::run_shell_unattended`,
  `platform::stream::run`).
- **The runtime is the environment's**: a package's libc and C++ runtime are bound through
  `<home>/abi/<family>` (`xlings.core.abi`), never rewritten per consumer; a Luban system
  carries its own in its generation.
- **The sandbox a policy compiles to is a golden** (`tests/fixtures/intent-eq`);
  regenerate with `XLINGS_INTENT_GOLDEN_WRITE=1` only on purpose.
- **nlohmann::json brace-init**: `json{x}` of a single value makes an array; build
  objects with `{{"key", v}}` pairs or `json::object()` and check the shape you meant.

## Diagnosing

- A shim behaves strangely: first `$XLINGS_HOME/bin/xlings --version` (every shim
  runs that file). `xlings self doctor` reports drift; `--fix` repairs derived state.
- A local e2e looks stuck: check the mirror before suspecting the code.
- `XLINGS_TRACE=timing` names the slow phase; `logs/` holds upgrade, destructive
  and elevation records.

## Versions and releases

- Releases are `YYYY.M.D.N`; **N starts at 1**, `.0` is reserved for a deliberate
  formal release. Bump `mcpp.toml`, `src/core/config.cppm` and
  `luban/os/service/src/service.cppm` (kVersion) together.
- After `release.yml` is green: push the assets to the CN mirror from a CN machine
  (`tools/mirror-latest.sh xlings`, then `luban`, `luban-os`, `xdesk` and `luban-cursors`; verify with GET), then
  write the release into an xim-pkgindex checkout with `tools/bump_index.py <version> <index>`
  (xlings, luban, luban-os and xim:xdesk, every platform block and `latest`; all or nothing) and open
  the index PR; bump `pkgs/luban/cursors.lua` by hand when the theme changed. These four packages have
  no `autoupdate`: their versions are written by this release only.
- Never pin a released xlings version in CI (`tests/fresh-install/` follows `latest`).

## Contributing

- Install tools with xlings (`xlings install <tool>`), never apt/brew/curl.
- Commits: `<type>(<scope>): <description>`; PRs are squash-merged; CI must pass on
  Linux, macOS and Windows; no `--no-verify`.
- No unrequested changes (comments, refactors). Match the surrounding code.
- **Privacy**: nothing in the repository identifies a person — no personal paths
  (`~/.xlings`, `/home/user` instead), names, emails or attributions
  (`tools/lint_privacy.sh`; `privacy-ok: <why>` for a line that must).
- Skills in `.agents/skills/`: xlings-usage, xlings-contributing, xlings-build,
  xlings-docs-writing, system-design, mcpp-style-ref.
