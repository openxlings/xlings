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
modules/                             # packages linked into xlings, one per responsibility
├── libs/       json, sha256, tinyhttps        nothing of xlings
├── runtime/    cancellation, guard, observe   shared by core and SubOS core
├── platform/   the OS boundary: every system header and call
│   └── src/platform/  linux, macos, windows, unix, target,
│                      process (fork/exec/signals/sockets), isolation (ns/Landlock/seccomp)
├── ui/         theme, i18n
├── subos/      the SubOS core: model, policy, spec, providers, caps, gates,
│               session (supervisor + session-init), broker, manifest, userdata
└── testkit/    C++ e2e test library (dev-dependency)
apps/
├── gui/        GUI library half
└── xdev/       dev tool: tests, report, requirement map (not shipped)
src/                                 # not yet separated: core, cli, ui, runtime
├── main.cpp                         # entry point
├── cli.cpp, cli/spec.cpp            # CLI dispatch and the command spec
├── interface.cpp                    # NDJSON programmatic interface
└── core/
    ├── config.cppm                  # 3-layer config (global -> subos -> project)
    ├── home/                        # HomeContext: deployment mode, layout, system config
    ├── subos.cpp                    # `subos` commands (new/use/exec/start/config/...)
    ├── subos/sandbox.cpp            # adapter: policy + caps -> spec -> session
    ├── xself/                       # self install/update/doctor
    ├── xim/                         # package management (installer, resolver, index...)
    └── xvm/                         # version management, shims

tests/
├── unit/, e2e/                      # C++ tests (`mcpp test`, XTEST + testkit)
├── e2e/*.sh                         # legacy shell e2e (tests/suites.toml)
├── requirements.toml                # requirement IDs every test `covers`
└── suites.toml                      # legacy suites run through xdev
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
mcpp test                    # unit tests + C++ e2e tests (tests/**/test_*.cpp)
XLINGS_BIN=$(find target -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/<test>.sh                             # legacy e2e scripts

# xdev (dev tool, not shipped): one report for C++ tests and legacy suites
mcpp build -p xdev
XDEV=$(find target -path '*/bin/xdev/xdev' | head -1)
$XDEV doctor                 # which capabilities this machine can test
$XDEV test [pattern] [--suite contract-scripts|lint|e2e-shell]
```

New tests are C++ with `modules/testkit` (`XTEST`, `Home::isolated`); see
`tests/README.md`.

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

### Platform code lives in `xlings.platform`

**System headers and system calls are in `modules/platform` and nowhere
else** (`tools/lint_platform_headers.sh`, in the lint suite). What another
module needs from the OS -- a fork, a socket that carries descriptors, a
Landlock fence, a file's owner -- is a function there, declared on every
platform and reporting "unavailable" where it does not exist; the caller
holds the logic and no `#include`. Two exceptions, both outside the
product: `modules/testkit` (the harness does not share the product's failure
modes) and `tests/` (a test may call the kernel to check what it does).

Branch on the platform with `if constexpr (platform::is_linux)` (also
`is_windows`, `is_macos`, `is_posix`): every build then compiles every branch,
so the Linux CI catches a typo in the macOS one. `#if` is for inside
`modules/platform`, where the headers are. testkit, which does not import
the product, carries the same constants under the same names.

### Core and interaction surfaces are separate (2026.10, #640)

Who is reading the output (a person or an agent) and how it is drawn (cli,
tui, `--json`, the NDJSON interface) are two independent axes, and neither
belongs in core code. Core returns structured results and asks questions only
through the `Asker` port (`xlings.guard`); it never branches on the audience
and never assembles user-facing text. The audience is DECLARED, never
inferred: `--agent` or `XLINGS_AGENT_MODE=1` (`0` turns it off). In agent mode
a command never waits for input -- a question it cannot answer becomes a
structured error with the candidates and the exact flag that answers it.

The SubOS design this comes from, and the module layout it implies
(`modules/runtime`, `modules/subos`, `src/core/home/`), is
`.agents/docs/2026-10-05-subos-architecture-design.md`.

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

**Third: a derived table derives "remove everything" from an input of
nothing.** That is the sharp edge of rebuilding rather than auditing, and it
cost 172 routing entries on a real home — `gcc` active at 16.1.0 and not on
PATH — plus every `fresh-install (core)` cell from 2026-09-03 to 2026-09-20.
Two independent ways to hand it nothing, and neither implies the other:

* **asking the wrong question.** "Which subos does this command act on"
  (`paths_.activeSubos`) and "which subos is the GLOBAL one"
  (`Config::global_subos_name_()`) agree outside a project and differ inside
  one. They were spelled the same way. If you add a reader of either, say in
  one line which question it is asking.
* **reading "could not" as "empty".** A missing subos DIRECTORY is not
  observed; a missing workspace FILE inside an existing one is observed and
  empty (a fresh subos); an unparseable file is not observed. Only the middle
  case may be rebuilt from — `Config::global_workspace_observed()` carries
  that fact, and `sync_shim_tables` refuses on the others. The refusal is
  binary, never a threshold: "it wanted to delete suspiciously many" is a new
  heuristic and therefore a second answerer.

**Which commands put a lost entry back.** Any global-scope `install` / `use` /
`remove` rebuilds the whole table, so `self update` repairs a damaged home as a
side effect of installing the new xlings — verified, not assumed. `self init`
rebuilds it too (so `self install` and an explicit `xlings self init` work even
when nothing is being installed). Read-only commands do not, correctly.

Do NOT write "`self init` runs on update" — it does not. The xlings recipe says
so in as many words ("a FRESH install gets ... an UPGRADE
(`xlings install xlings@latest`, `xlings self update`) does not"), and
`main.cpp`'s profile self-heal exists precisely because `xlings update xlings`
only flips the xvm pointer. `self doctor --fix` is no longer the only way back,
but it is the install path that carries the repair, not init.

**Fourth: "is this shim ours" and "is it current" are two questions (#615).**
A Windows shim is a hard link to the entry's FILE OBJECT, and every upgrade
replaces that object (a running image cannot be overwritten in place). While
both questions were answered by "same file as the entry", an upgrade made
every shim in the home read as somebody else's file AND left it running the
previous client — `xlings --version` on PATH printed the old release with
`self update` at exit 0. Now `xvm::ShimClassifier` answers both, and is the
only thing that does:

* **ours** = the file IS an xlings build: it carries `kMulticallMarker`, or
  (builds before 2026.9.26.3, `COMPAT … drop in 2027.3`) `create_shim`'s own
  error format string. What it links to is irrelevant.
* **current** = the entry's file object, or byte-identical to it.
* **unreadable** = Unknown, never Stale: nothing that could not be read is
  replaced.

`xself::replace_entry_binary` is the one way `use`/`install` switch the
client, and it re-points every stale shim in **every** subos and known
project right after (relinking changes no name, so it is not a routing
decision). A stale shim that is itself a marked build hands off to the entry
at startup (`main.cpp`, `xvm::handoff_target`), so it runs the entry's code
even before it is relinked; `XLINGS_HANDOFF_TRACE=1` prints the handoff.
`self doctor` reports a legacy stale shim as `outdated shim` (an error), and
prints its remedy with the entry's full path, because `xlings` on PATH is one
of the stale files. A home an older client upgraded needs that command once
(`& "$env:USERPROFILE\.xlings\bin\xlings.exe" self init` also works): new code
never runs there on its own.

### SubOS user data

**A SubOS's home is user data. Only a deletion the user initiated may remove
it, and only after they confirmed.** The maintainer's rule, 2026-09-26:

| What | Examples | Who may delete it |
|---|---|---|
| derived data | payloads in `data/xpkgs/`, sysroot links into them, shims in `bin/`, `generations/`, registrations, index copies | any flow — a reinstall or `use` puts it back |
| user data | `home/`, `home.img`, and **any regular file in the SubOS xlings cannot prove it owns** (a sandbox writes the user's `/usr` and `/etc` into this tree) | only a command that *is* a SubOS deletion (`subos remove`, `self uninstall`, `self install`'s overwrite), confirmed at a terminal or with an explicit `-y` / `"yes": true` |

What is **never** a way to delete user data: `self doctor --fix` and its
children, upgrades (`self update`, `self install`'s normal path), a rollback,
GC, a printed remedy. With nobody to ask and no auto-confirm, the command
deletes nothing and exits 2, saying what it would have deleted.

How it is enforced, so it does not depend on remembering it:

* `subos::userdata::delete_subos` is the one whole-SubOS deletion. It takes a
  `confirm::UserConfirmed`, which only `confirm::ask()` produces — a path that
  never asked cannot call it. It refuses the `subos/` root, `current`, symlinks
  and (Linux) a tree with a live mount.
* `tools/lint_subos_remove_all.sh` (in `xlings-ci-linux`) fails any other
  `remove_all` on a SubOS path. A legitimate one carries
  `subos-remove-all-ok: <why>` on its line.
* Every destructive operation appends to
  `<XLINGS_HOME>/logs/destructive.ndjson` (path, size, how confirmed, command,
  parent process). The loss that produced this rule could not be attributed
  because nothing recorded it — read that file first next time.

"Could not read" is never "empty" here either: GC refuses when a SubOS config
is unreadable instead of collecting what that SubOS uses.

### Install configures once per scope (2026.9.29.1, #632)

A payload in `data/xpkgs/` is a HOME fact; having run its `config()` is a fact
about ONE scope (a subos, or a project's). Each scope's `.xlings.json` carries
`configured: { "<ns>:<name>@<version>": <revision> }`, a top-level sibling of
`workspace` (never inside it: an older client reads every `workspace` key as a
target name). `install` leaves a present payload's config alone only when
`xim::configured_verdict` says so -- the record names the recipe's current
revision AND every ledger entry the payload owns is claimed by the scope's
`installed[]`. No record means "configure" (the safe direction; old homes
migrate on their next install). An install whose whole closure is configured
does only the report, the activation and one routing-table rebuild.
`--reconfig` (interface: `reconfig: true`) restores the old run-everything
behaviour.

Rules that follow from it:

* **A recipe that changes what its config does must bump `revision`.** That is
  the only signal every scope acts on; each reconfigures on its next install.
* **A config hook must not write scope-specific data into the shared payload**
  (two scopes would fight, and with this record neither rewrites it again).
  gcc's specs are payload-direct for this reason.
* **The routing table is rebuilt per node, never once per plan.** Later hooks
  in the same plan run earlier nodes' commands by name (musl-gcc → `patchelf`,
  gcc config → `<bindir>/gcc-specs-config`); the elfpatch step's PATH prepend
  is deliberately left in place for them. What was slow in a rebuild was
  re-reading the 3.6 MB home config for `knownProjects` -- now cached against
  the file's size/mtime -- not the table itself.
* A printed remedy that needs config to run again must say `--reconfig`.

### Download failures are attributed; hook output goes to a log (2026.9.30.1)

**A failed download says whose fault it was** (`xlings::tinyhttps::FailureKind`,
decided in one place: the xhttp wrapper's candidate loop). What landed on disk
is compared with what the transfer reported BEFORE the content is judged: a
file the disk cut short hashes to the wrong digest too, and until this release
that verdict demoted the host and skipped its retries. Measured on a real home:
a full disk kept 220,979,200 of 420,831,054 bytes, the client reported all of
them written (tinyhttps ≤0.3.1 never checks `ofs.write`), and the user was told
`sha256 mismatch`.

| Kind | Example | What happens |
|---|---|---|
| Source | complete file, wrong sha256; HTTP ≥400 | next candidate; bad bytes demote the host |
| Transfer | short read, timeout, stall | retried on a later round |
| Local | file shorter than received, cannot open the destination, not enough space | stop every candidate, demote nobody, `E_DISK_FULL` with the free space |

Do not add a caller that decides the kind from the wording of `error`.

**The commands a hook runs write to `<home>/logs/hooks/<ns>-<name>@<ver>.<hook>.log`**
(libxpkg ≥0.0.60 `ExecutionContext::hook_log`, set by the installer before
every `run_hook`). A failure prints the log's last 20 lines and its path; a
hook running past 15 s prints a heartbeat line. A recipe command that must
reach the terminal passes `system.exec(cmd, { tty = true })`; older clients
ignore the option and inherit anyway. `XLINGS_HOOK_OUTPUT=inherit` / `-v`
restore the terminal for everything (never in the interface or the TUI).

**`use` announces only a switch that moved something.** `self update` runs
`use` twice; on a current home both printed `xlings -> <v>`. The plan's
`alreadyActive` is true only when the target AND every member of its release
are already at the selected versions.

### Dependency names resolve in the declaring index

A bare dependency in a recipe (`deps = { "ncurses" }`) means the package of
that name in **the index the recipe came from**; only a name that index lacks
falls through to the global rule. A version or platform the declaring index
cannot satisfy is an error naming it — never a reason to take another index's
package of the same name. The resolver records its choice as a `DepEdge` on the
plan node; the installer and the remove guard read the edge and must not
re-derive "which package did this name mean".

An `index_repos` entry whose name and url match a sub-index the default index
declares is configuration for that sub-index (a pin, an artifact base), not a
second repository: one directory (`xim::effective_repo_dir`), sub-index rank.
Same name with another url is refused. See
`.agents/docs/2026-09-25-dep-name-resolution-optimization-plan.md`.

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
