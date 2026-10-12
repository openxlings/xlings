# AGENTS.md

`xlings` is a package manager with OS-like SubOS isolation: one static binary,
C++23 modules, Linux / macOS / Windows. **Luban** is the OS model on top of it
(environments, machines, images; `luban` CLI and the Luban OS app).
Design records live in `.agents/docs/` (the current architecture:
`2026-10-12-luban-os-architecture-design.md`); user docs in `docs/` (en, mirror in `docs/zh/`).

## Layout and layers

```
modules/   shared libraries (libs, runtime, platform, home, ui, cli, store, subos, confine, carrier, image, testkit)
luban/     Luban OS components and services (stage-0, the luban command; desktop subsystem)
apps/      binaries a user runs (luban, gui, xdev)
src/       the xlings frontend and core still being moved into modules/
tests/     unit/ and e2e/ (C++, XTEST + testkit), legacy e2e/*.sh, requirements.toml
```

Imports go downward only (`tools/lint_layer_deps.sh`): `luban/` -> `modules/`;
`modules/` never imports `luban.*` or `src/`; `src/` never imports `luban.*`.
What xlings and Luban both read (boot.json, a root's /etc, image formats) is a module.

## Build and test

```bash
xlings install                 # tools from .xlings.json (mcpp)
mcpp build                     # xlings; `mcpp build --workspace` for every package
XLINGS_TEST_MIRROR=CN mcpp test [pattern]   # set the mirror when testing from China
```

New tests are C++ (`modules/testkit`, `XTEST`, `Home::isolated`); every test
`covers` a requirement ID in `tests/requirements.toml`. Tests use an isolated
home and never touch the user's. Release builds: `tools/{linux,macos}_release.sh`,
`tools/windows_release.ps1`.

## Rules the code is checked against

- **System headers and calls live in `modules/platform`** (plus the desktop
  backends). Branch with `if constexpr (platform::is_windows)`, not `#if`; an `#if`
  elsewhere needs `// platform-if-ok: <why>`.
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
  `luban/service/src/service.cppm` (kVersion) together.
- After `release.yml` is green: push the assets to the CN mirror from a CN machine
  (`tools/mirror-latest.sh xlings`, then `luban-os`; verify with GET), then bump
  `xim-pkgindex/pkgs/x/xlings.lua` (every platform block and `latest`) and
  `pkgs/l/luban-os.lua`.
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
