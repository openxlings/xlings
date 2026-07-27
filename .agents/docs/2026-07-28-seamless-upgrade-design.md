# Seamless upgrade: what the simulation found, and what to build

2026-07-28 · evidence branch `test/legacy-upgrade-simulation` (PR #434)

## 0. What "无感升级" has to mean

Three independent properties. A release can satisfy any two and still not be
seamless.

| axis | claim | status |
|---|---|---|
| **不中断** — nothing that worked stops working | packages installed by the old client keep resolving, switching and running | **holds** (measured) |
| **不费时** — the upgrade is not something you wait for | `self update` wall clock | **holds**: 3.8 s on a GitHub runner, 16.8 s on a home connection |
| **不需要动作** — the user does not have to know an upgrade exists | a client that is behind says so; a package that is behind can be moved forward | **does not hold** — this is the whole gap |

Everything below is about the third axis. The first two were measured, not
assumed, and they are fine.

## 1. How the evidence was produced

`.agents/tools/simulate-legacy-upgrade.sh`. It installs the **real** 0.4.69
release into an isolated `HOME`, installs real packages **through it**
(`gcc@15.1.0`, `gcc@16.1.0`, `llvm@20.1.7`, `llvm@22.1.8`, `mcpp`), runs the
real `xlings self update`, then drives what a user drives the next day.

Two properties of the harness matter for reading its output:

- **`XLINGS_HOME` is never set.** The client derives the home from `HOME`
  exactly as it does for a real user, so home derivation is exercised rather
  than bypassed.
- **Exit codes are not trusted.** The first run reported 23/26 green,
  including `remove llvm` at 166 ms and `install llvm` at 333 ms — durations
  that cannot contain the work they claim. `rc=0` from this CLI means nothing
  raised, not that anything happened. Every state change is now asserted
  separately: is it still listed, does the shim report the version that was
  switched to.

Findings below reproduced **independently** on a clean GitHub runner and on a
developer machine. They are not artefacts of one dirty home.

## 2. Findings

### B1 — `self update` upgrades the binary and leaves the recorded version behind

After `self update`, `bin/xlings` is 2026.7.27.4 and `xlings --version` agrees.
`.xlings.json`'s `version` field still reads `v0.4.69`.

That field is not decoration. `src/core/xself/install.cppm:577` writes it, and
`:425` reads it back as `installedVersion` to decide what a later
`self install` is doing. `self update` never goes through that path — it shells
out to `xlings install xlings@latest` plus `xlings use xlings latest` — so the
field goes stale on the first upgrade and stays stale forever.

**This is the load-bearing one.** Any "you are behind" notification has to
compare something against latest. The obvious something is this field, and it
is wrong on precisely the machines that have upgraded.

### B2 — a package with a non-latest version installed reports as not installed

`catalog.cppm:333` computes `match.installed` as: does
`<store>/<pkg>/<select_version_(…)>` exist — and `select_version_` with no
version hint returns **latest**.

So with `llvm@20.1.7` installed, active, and on disk:

```
$ xlings info llvm
versions        20.1.7, 22.1.8, latest -> 22.1.8
installed       no                                  ← it is installed
$ xlings list --all
… 9 packages, llvm not among them                   ← invisible
```

The package is installed, active, and running, and the tool cannot see it.

### B3 — `install <bare-name>` answers the opposite question, in the same second

`cmd_install` pins a bare name to the version already active in the sub-OS
(`pin_to_active_if_satisfies_`, `src/core/xim/commands.cppm`). This is a
deliberate anti-surprise rule and it is a good rule.

But composed with B2 the same binary says both of these about the same package
at the same moment:

```
$ xlings info llvm      → installed  no
$ xlings install llvm   → xim:llvm@20.1.7 is already installed
```

Neither statement is wrong under its own resolution rule. There are two rules.

### B4 — nothing moves a package forward

`install <bare>` pins to active (B3). `self update` upgrades the client only.
Nothing reports that a newer version exists (#425). The only way forward is
`install name@<exact-version>` — which requires knowing a version number
nothing ever showed you.

An upgraded client therefore keeps running the packages it had, indefinitely,
and reports them as not installed while doing it.

### B5 — `install` and `remove` disagree about which version a bare name means

With 20.1.7 and 22.1.8 both installed:

- `xlings remove llvm` removes **22.1.8** (the latest)
- `xlings install llvm` addresses **20.1.7** (the active)

Both disclose what they picked, so neither is silent. They are simply two
different answers to "which llvm".

Composed, they produce the state in B2: remove the latest, and the package
disappears from `list` while still being installed and active.

### B6 — removing a version leaves an orphan shim

After removing `llvm@22.1.8`, `self doctor` reports

```
✗ orphan shim  @xlings/subos/default/bin/clang-22 exists but workspace has
               no active version for clang-22
```

Same family as #423 (file assets not removed on uninstall).

### B7 — `self doctor` fails on a healthy, freshly-upgraded home, with a remedy that cannot work

On the GitHub runner, immediately after upgrade and before the user does
anything:

```
✗ broken payload [active]  linux-headers@5.11.1 executable 'linux-headers'
                           not found in …/xim-x-linux-headers/5.11.1
  → run  xlings install linux-headers@5.11.1
broken payloads  1
warnings         1
```

exit 1.

`linux-headers` ships headers and no executable. Running the suggested command
reinstalls the same payload and produces the same finding. Meanwhile doctor
already has the right concept for exactly this shape and applies it to others
in the same report:

```
ⓘ release anchor  glibc@2.39 registers no program of its own; it names the
                  release its libraries belong to
```

So the classification exists; `linux-headers` is not getting it. The
consequence is worse than one bad line: `self doctor` exits non-zero on a
correct machine, which is how a health check gets ignored.

### What did NOT break (worth stating)

- **Version switching survives the upgrade.** `use gcc 15.1.0` then
  `use gcc 16.1.0`, confirmed through the shim's own `--version` output, not
  through `use`'s exit code. Both directions, on a home built by 0.4.69.
- **Every package the old client installed still installs, resolves and runs**
  under the new client.
- **The field dead-end did not reproduce.** The 2026-07-28 report — an
  `llvm@20.1.7` recorded with backslash paths and `.exe` aliases on Linux,
  which could be neither reinstalled (`xvm-legacy-payload-mismatch`) nor
  removed (`recipe removal target is outside the owned selection`) — does not
  occur with 0.4.69 plus the current index. 0.4.69 on Linux writes clean
  forward-slash paths and non-`.exe` aliases, and the current recipe registers
  every binary in `bin/`, so the reinstall re-runs `config()` and succeeds.
  Reproducing it needs the **old index too**, not just the old client.

  Its provenance cannot be recreated by today's code at all: `detect_platform()`
  is a compile-time `#if defined(__linux__)`, so a Linux binary cannot select
  the Windows `xpm` table. That does not make the dead end less real — it
  makes it a **recovery** problem rather than a **prevention** one.

## 3. The shape of the defect

xlings has no single answer to "what is installed". It has three stores and
they are consulted by different commands:

| store | holds | consulted by |
|---|---|---|
| package store on disk `data/xpkgs/<pkg>/<version>` | payloads | `catalog.installed`, against **latest only** |
| xvm version DB `.xlings.json:versions` | registered targets and versions | `use`, `remove`, `doctor` |
| sub-OS workspace `subos/<n>/.xlings.json:workspace` | active + installed per program | `pin_to_active_if_satisfies_`, `doctor` |

B2, B3 and B5 are all one bug wearing three coats: **the same question is
answered by whichever store the command happens to reach for.** B1 is the same
disease at the client level — the recorded client version and the actual client
binary are two stores, and only one gets written.

Fixing the notification without fixing this produces a notification that is
wrong on upgraded machines (B1) about packages it thinks are not installed (B2).
Order matters.

## 4. Design

### D1 — one writer for the recorded client version *(prerequisite)*

`self update` must write `.xlings.json:version` when it succeeds. Better: the
field stops being written by `self install` specially, and becomes a
post-condition of "the client binary changed", asserted in one place that both
`self install` and `self update` call.

Verification is already written: the harness compares the recorded version
before and after and files a blocker when it does not move. It currently files
one. It must stop filing one.

**This is a prerequisite for D2, not a parallel task.**

### D2 — the client says it is behind

A check with a hard latency budget, because axis 2 is currently satisfied and
must stay satisfied:

- **Never on the hot path.** No network in `install`, `use`, `list`, `run`.
- **Cached with a floor.** One check per 24 h, recorded with a timestamp in
  the home config. A cache miss does not block the command that noticed it —
  it schedules, and the *next* invocation prints the result.
- **Printed once per new version**, not once per invocation. A notification
  that appears on every command is a notification that gets filtered out.
- **Silenceable and CI-safe**: honour `NO_COLOR`-style opt-out and never print
  when stdout is not a TTY.

What it prints matters as much as when:

```
xlings 2026.7.27.4 is available (you have 0.4.69) — run `xlings self update`
```

The version it compares against is D1's field. Without D1 this line is a lie
on every machine that has already upgraded once.

### D3 — `installed` means "any version is installed"

`catalog.cppm:333` must not decide installedness from the latest version alone.
The match should carry both:

- `installed` — is **any** version of this package present
- `installedVersion` — which one(s)
- `resolvedVersion` — what a bare name would resolve to right now

`list` and `info` then say what is true:

```
llvm    20.1.7 installed · 22.1.8 available
```

This single change removes B2, and removes the contradiction in B3 without
touching the pinning rule — which stays, because it is right.

### D4 — one bare-name resolution rule, stated once

`install`, `remove`, `info` and `list` must share a resolver with an explicit
policy, rather than each reaching for whichever store is nearest:

> A bare name means **the active version if there is one, otherwise latest**.

Under that rule `remove llvm` takes 20.1.7 (active), not 22.1.8 (latest), and
B5 disappears. `remove --all` covers the "take the whole package out" intent
that bare `remove` is being used for today.

### D5 — a command that moves packages forward

Given D3 there is something to move *to*, and given D4 a bare name has one
meaning, so the forward step can be explicit and safe:

- `xlings upgrade [pkg…]` — move named packages (or everything installed) to
  latest, showing the plan first
- `xlings install <pkg>@latest` — already expressible, keeps working

The pinning rule in `install` stays exactly as it is. Moving forward becomes a
thing the user asks for by name, which is the correct default for a tool that
manages toolchains.

### D6 — doctor must be quiet on a healthy machine

Two changes, both narrow:

1. A package that ships no executable is a **release anchor**, not a broken
   payload. The classification already exists and already fires for `glibc`,
   `zlib`, `libxml2` and others in the same report; `linux-headers` has to
   reach it too. The general rule: *if the recipe registers no program, absence
   of a program is not a defect.*
2. **Never suggest a remedy that cannot work.** `→ run xlings install
   linux-headers@5.11.1` for a package with no executable is a loop. A finding
   whose remedy does not change the finding should not carry one.

Exit code stays as designed (`unresolved != 0`) — it only needs the input to be
correct.

### D7 — recovery from a legacy record the new client refuses

Not fixed in this pass, deliberately: it did not reproduce, and the two guards
involved came from #384 with intent behind them. What the evidence does
establish is the requirement:

> **There must always be a command that takes something out.**

`removal.cppm` already argues this for itself, in a comment written after an
earlier field report:

> *"A state that cannot be resolved already blocks `use`. Letting it block
> `remove` as well leaves the user unable to switch or to take the package out:
> a dead end with no command that gets out of it. … Taking something out needs
> no understanding of what put it in."*

The `clang++.exe` failure is the same principle unapplied one level down: a
recipe naming a removal target that **is not registered at all** is asking to
remove nothing, and refusing is a dead end. The guard exists to stop
cross-provider removal, and a target absent from the DB cannot be another
provider's. Narrowing it to "registered, but outside the selection" preserves
every protection it actually provides.

The registration side (`xvm-legacy-payload-mismatch`) is the harder half: its
prescribed escape is "uninstall it before reinstalling", which was itself
broken, and an owner-less legacy record has no recorded owner to uninstall.
That deserves its own investigation with a reproduction — **against an old
index**, which is the next scenario for the harness — not a guess.

## 5. Order of work

D1 → D3 → D2 is a chain; the others are independent.

| step | unblocks | risk |
|---|---|---|
| **D1** record the version on update | D2 | low; one write site |
| **D3** `installed` means any version | D2, D5, removes B2/B3 | low; additive fields |
| **D6** doctor classification + no impossible remedies | makes doctor usable as a gate | low; two narrow rules |
| **D2** upgrade notification | axis 3 | medium; latency budget is the whole design |
| **D4** shared bare-name resolution | removes B5 | medium; changes `remove`'s target |
| **D5** `xlings upgrade` | axis 3 | medium; new surface |
| **D7** legacy recovery | the field dead end | needs the old-index reproduction first |
| **B6** orphan shims on removal | #423 | independent |

## 6. Keeping it honest

The simulation is the acceptance test, and it already fails on B1, B2/B3
(as assertions), B7 and B6. Each design step above corresponds to a specific
assertion flipping.

### Cross-platform result

All three platforms now run the simulation end to end, and **B1, B2 and B4/B5
reproduce on every one of them**:

| finding | linux-x86_64 | macosx-arm64 | windows-x86_64 |
|---|---|---|---|
| B1 recorded version stays `v0.4.69` after `self update` | ✗ | ✗ | ✗ |
| B2/B5 `llvm@20.1.7` invisible after removing the latest version | ✗ | ✗ | ✗ |
| B3/B4 `install llvm` does not bring `22.1.8` back | ✗ | ✗ | ✗ |
| B7 `self doctor` exits 1 on a healthy home | ✗ | — | — |
| steps run | 34 | 22 | 22 |

B7 is Linux-only because `linux-headers` is a Linux-only dependency; the
classification defect it exposes is not platform-specific. The macOS and
Windows runs are shorter because `gcc` has no macOS `xpm` table and only a
mingw-delegating Windows one, so `llvm` carries the two-version role there.

The first 3-platform attempt reported 19 "blockers" each on macOS and Windows,
all of them the harness failing to start — bash 3.2 treating an empty array
expansion as unbound under `set -u`, and `mv "$top"/*` skipping the release
package's `.xlings.json` marker so `self install` could not find its own
package. Both fixed; noting them because a harness that fails loudly in a way
that looks like product findings is worse than no harness.

### Still not covered

- **the old-index scenario** — pin the home's `xim-pkgindex` to an old commit
  before `populate`, which is what is needed to reproduce the field dead end
  (D7) faithfully rather than by synthesis
