# AGENTS.md

## Project Overview

`xlings` is a universal package management infrastructure tool with OS-like SubOS isolation. Single static binary, C++23 modules throughout, cross-platform (Linux / macOS / Windows).

Core capabilities:
- **Package management** — install/remove/search/update with multi-version coexistence
- **SubOS isolation** — 3 levels (shell / FS sandbox / image), rootless (except image mode)
- **Decentralized index** — official + third-party + self-hosted package repos
- **Agent integration** — NDJSON interface (`xlings interface`), SubOS for agent-owned envs
- **Version view + ref-counting** — N isolated environments share one copy of package payloads

## Repository Structure

```
src/
├── main.cpp                         # Entry point
├── cli.cppm                         # CLI dispatch (positional + flag parsing)
├── interface.cppm                   # NDJSON programmatic interface (protocol v1.0)
├── core/
│   ├── config.cppm                  # 3-layer config (global → subos → project)
│   ├── subos.cppm                   # SubOS lifecycle (create/use/fork/remove/stop)
│   ├── subos/keeper.cppm            # Auto-keeper primitives (Linux namespace reuse)
│   ├── xself.cppm                   # Self-install/update
│   ├── xself/                       # Self-management submodules
│   ├── xim/                         # Package management subsystem
│   │   ├── installer.cppm           # Install orchestration (type dispatch)
│   │   ├── resolver.cppm            # DAG dependency resolution
│   │   ├── downloader.cppm          # Parallel download + SHA256
│   │   ├── index.cppm               # Package index + cache
│   │   ├── catalog.cppm             # Multi-repo catalog loading
│   │   └── libxpkg/types/           # Per-type handlers:
│   │       ├── type.cppm            #   PlanNode, enums, shared types
│   │       ├── script.cppm          #   type="script" default hooks
│   │       └── subos.cppm           #   type="subos" default hooks
│   └── xvm/                         # Version management
│       ├── db.cppm                  # VersionDB CRUD + JSON
│       ├── shim.cppm                # Multicall shim dispatch
│       └── commands.cppm            # xvm commands (use, list, register)
├── platform.cppm                    # Cross-platform abstractions
├── platform/                        # Platform implementations
├── libs/                            # Vendored libs (json, tinyhttps)
└── ui/                              # TUI (ftxui-based)

tests/
├── e2e/                             # End-to-end shell tests
│   ├── project_test_lib.sh          # Shared helpers (find_xlings_bin, run_xlings)
│   ├── fixtures/                    # Test fixture packages
│   └── subos_xpkg_*.sh             # SubOS-as-xpkg e2e tests
└── (unit tests via `mcpp test`)

.agents/
├── docs/                            # Agent working docs (see .agents/docs/README.md)
├── skills/                          # Agent skills (this section)
├── plans/                           # Implementation plans
└── tasks/                           # Task tracking
```

## Build System

Single build tool: **mcpp** (C++23 modules).

```bash
# Setup (from repo root):
xlings install              # installs mcpp from .xlings.json
xlings use gcc@16.1.0       # switch to glibc-linked dev toolchain

# Build:
mcpp build                   # dev binary → target/<triple>/<fingerprint>/bin/xlings

# Test:
mcpp test                    # unit tests
XLINGS_BIN=$(find target -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/<test>.sh                             # e2e tests
```

For release packaging (static binary):
- Linux: `tools/linux_release.sh` (musl-gcc static)
- macOS: `tools/macos_release.sh` (LLVM)
- Windows: `tools/windows_release.ps1` (MSVC)

## Key Development Patterns

### CLI argparse

Manual positional parsing in each subcommand's `run()` function (see `subos.cppm` line ~1700). Pattern:
```cpp
for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--flag" && i + 1 < argc) { value = argv[++i]; }
    else if (!a.empty() && a[0] != '-' && name.empty()) { name = std::move(a); }
    else { usageError("unknown option: " + a); return 1; }
}
```

### Type-specific install dispatch

`installer.cppm` checks `node.pkgType`:
- 0 = Package (standard: extract + hook)
- 1 = Script (default_install copies .lua)
- 4 = Subos (default_install creates skeleton + .xlings.json)

### E2E test pattern

Use `project_test_lib.sh` helpers. Key functions:
- `find_xlings_bin` — locates built binary
- `run_xlings "$HOME_DIR" "$ROOT_DIR" <args>` — isolated execution
- `require_fixture_index` — ensures pkgindex fixture present

**Running the suite locally: set `XLINGS_TEST_MIRROR=CN`.**

```
XLINGS_TEST_MIRROR=CN bash tests/e2e/run_all.sh build/release.tar.gz
```

An isolated home defaults to the GLOBAL mirror, because CI runs on github.com
and cannot reach the CN endpoints. From inside China that default makes every
index sync wait on an unreachable host until it times out — and that does not
present as a network problem, it presents as the command under test hanging.
Measured on one local run of this suite: `subos_events` 817s,
`subos_profile_upgrade` 650s, `cli_short_alias_removal` 406s, essentially all
of it spent waiting. The same three take seconds with the knob set.

Corollary when a local e2e looks stuck: check the mirror before reading it as a
regression. `xlings self init` on a fresh home measured 17s on one binary and
2m13s on another purely from mirror reachability, which is easy to mistake for
a performance change in the code under test.

### Diagnosing a shim that behaves strangely

**First command: `$XLINGS_HOME/bin/xlings --version`.**

Every shim in a home is a link to that one file — `subos/<s>/bin/gcc`,
`.../ld`, `.../node` all resolve to it — so its version decides how every
tool in the home is dispatched. It is written on purpose by `use`/`install`
of the xlings package itself, which means **activating an older xlings
rewrites it**, and a downgrade there changes the behaviour of the entire
toolchain.

What that looks like from the outside is nothing like a version problem.
Measured: an entry rolled back six weeks stopped expanding
`${XLINGS_DYNAMIC_SUBOS_DIR}`, so gcc's alias reached a shell as
`--sysroot=` (empty, therefore the host root), and the first visible
symptom was `ld: cannot find crt1.o` — three layers away from anything
naming xlings.

`xlings info xim:xlings` and the `active` binding can both disagree with
that file. `xlings self doctor` reports the divergence; the file is the
authority on what is actually running.

Corollary for any verification that goes through a shim: **swapping the
binary under test without swapping the dispatcher is not a control.**

**Second thing to know: a shim file asserts ROUTING, not STATE.** Its
presence means "this name is dispatched through xlings" and nothing more —
it carries no version and no owner. Which version runs is the workspace's
answer, resolved at exec time. So `bin/` holding a name whose workspace has
no active version is not, by itself, a defect: a project's command names
live in the global subos's `bin/` because a project's own bin is never on
PATH, and outside that project the shim hands the name back to PATH and runs
the host's copy.

The directory is a derived table with one writer (`xself::sync_shim_tables`,
called by install / use / remove) and is rebuilt from the workspace plus
`knownProjects` rather than audited against it. `xlings self doctor` reports
the difference as `shim table`; `--fix` applies it and names what it removed.
Reading a shim's existence as an activation claim is what produced the class
of bug that design removed — see
`.agents/docs/2026-09-03-project-shim-routing-vs-state-design.md`.

### Upstream dependency

`mcpplibs/libxpkg` provides the xpkg loader/executor. Referenced via `mcpp.toml`:
```toml
[dependencies.mcpplibs]
xpkg = "0.0.42"
```
For joint development, use mcpp's local dependency override/workspace mechanism.

## Version numbering

Releases are date-based: `YYYY.M.D.N`, e.g. `2026.7.29.1`.

**`N` starts at `1`. `.0` is reserved.** A `.0` means a formal / stable /
milestone release for that date, and is only used when someone deliberately
intends one. Every ordinary fix or feature release is `.1`, then `.2`, and so
on. If you are not sure, it is not a formal release — use `.1`.

```
2026.7.29.1   ← first release of the day (the normal case)
2026.7.29.2   ← second release of the day
2026.7.29.0   ← reserved: formal/stable release only
```

This rule already lived in `.agents/skills/xlings-contributing/SKILL.md`, and
`2026.7.29.0` still shipped as an ordinary bugfix release because that skill was
never opened — the release was cut from this file's instructions alone. It is
repeated here for that reason: **the version is chosen before any skill is
read**, so the rule has to be where the release decision is made.

`semver::parse` rejects a four-component version, so resolution falls back to
lexicographic ordering. Always publish and reference releases through an
explicit `latest` ref rather than relying on version comparison.

### A release is not finished when `release.yml` goes green

Two steps run outside the workflow, and skipping either leaves a release that
looks published and is not installable:

1. **Top up the CN mirror from a CN machine**: `bash tools/mirror-latest.sh xlings`.
   The GitHub runner cannot push large assets to GitCode (the upload stalls on
   the cross-border OBS wall and the job says so, in a green step), so the four
   tarballs have to be pushed with a local `gtc`. Verify with **GET, not HEAD** —
   GitCode answers `401` to HEAD and `302 → CDN 200` to GET.
2. **Bump `xim-pkgindex/pkgs/x/xlings.lua`**, all platform blocks plus the
   `["latest"]` ref, with the sha256 of each asset.

`2026.7.30.1` skipped step 1 and CN users got a flat `HTTP 404` for three
hours. `2026.7.30.2` added a cross-region download fallback so a gap in one
region's mirror is no longer fatal — that makes the manual step an
accelerator again, not a correctness requirement, but it is still expected on
every release.

### Never pin a released xlings version into CI

`tests/fresh-install/` installs whatever the published `latest` resolves to,
on purpose: it is the only workflow that tests what a first-time user
actually gets — the release artifact, `quick_install`, and the index that
resolves it. A pinned version turns that into a test of a snapshot nobody
installs, and it goes stale silently, one release at a time.

So a release does **not** come with a follow-up "pin the new version in CI"
commit. Pins in that suite are for things it deliberately holds still while
testing something else (`MCPP_OLD` / `MCPP_NEW` pin the two ends of an
*upgrade*, which needs two known versions); the xlings version under test is
never one of them.

This is enforced, not merely written down —
`tests/fresh-install/no_xlings_version_pin_check.sh`, run from
`xlings-ci-linux.yml` because that workflow has a `pull_request` trigger and
the fresh-install workflow deliberately does not. A rule enforced only after
merge is a rule that gets merged.

## Agent Skills

| Skill | Purpose |
|-------|---------|
| `xlings-usage` | Complete xlings usage guide (install, subos, project mode, agent workflows) |
| `xlings-contributing` | Contribution workflow (issue → implement → test → PR → CI) |
| `xlings-build` | Platform-specific build instructions (Linux musl / macOS LLVM / Windows MSVC) |
| `xlings-quickstart` | Quick-start operations (legacy, see xlings-usage for updated version) |
| `system-design` | System design patterns |
| `mcpp-style-ref` | C++ coding style reference |

## Important Rules

1. **Use xlings for tool installation** — always `xlings install <tool>`, never apt/brew/curl
2. **Toolchain**: `xlings use gcc@16.1.0` for dev builds (avoids musl/glibc link conflicts)
3. **Test isolation**: every e2e test uses temp XLINGS_HOME, never touches real user env
4. **Commit convention**: `<type>(<scope>): <description>` — feat/fix/chore/docs/test
5. **Squash merge**: PRs are squash-merged to main, one clean commit per feature
6. **Version numbering**: releases are `YYYY.M.D.N` and **N starts at 1**.
   `.0` is reserved for a formal/stable release and must never be used for a
   routine one. See below.
6. **CI must pass**: Linux + macOS + Windows; don't bypass with `--no-verify`
7. **No unnecessary changes**: don't add comments/docstrings/refactors beyond what's asked
