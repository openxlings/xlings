# SubOS architecture — implementation plan (single PR, commit checkpoints)

- Design: `.agents/docs/2026-10-05-subos-architecture-design.md` (round 7, all
  review items confirmed, including §27 items 1–5 on test/CI)
- Issue: #640
- Base: `v2026.10.4.1` (`4ca6926`)
- Branch: `feat/subos-architecture-640`, one PR, one commit per checkpoint.
  Every commit builds, keeps the existing suites green, adds its own tests and
  can be reverted on its own.

## 1. How the work is cut

The design lists C0 → T1–T6 → C1–C27. This plan keeps that order and states,
for each checkpoint, which perspective forces it to come where it does. The
perspectives the review asked for are the columns of the table in §3; the
dependency graph is §2.

Principles applied while cutting:

| Perspective | Rule used for ordering |
|---|---|
| Architecture | New code lands in its final module (`modules/subos`, `modules/guard`, `modules/observe`, `modules/testkit`, `apps/xdev`) from the first commit that needs it. Old code moves in "no behaviour change" commits *before* anything new depends on it, so a move and a change never share a commit. |
| Stability | Security defaults change only after the spec compiler exists (C8): every behavioural change is a change of compiled data that has a golden test, not an edit inside an argv builder. |
| Elegance | One answerer per question: `HomeContext` (home), `policy::decide` (may I), `compile` (what do I get), each interface's `probe()` (what can this host do), `xdev ci plan` (what runs). Nothing is added that a second place re-derives. |
| User experience | The user surface (`--sandbox=<preset>`, `subos exec`, `subos status`, exit codes, hints) is defined once and is identical on every platform; a platform that cannot do something says so in the shared hint format. |
| Compatibility | Undeclared instances keep today's behaviour except for the S0 security fixes; `--sandbox` alone means the `dev` preset; `--gpu`, `--cmd`, `--keep`, `--ttl` stay as aliases; `.xlings.json` writers keep unknown keys; layout migrations are one-way and an unknown higher layout is read-only. |
| Cross-platform | Interfaces live in `modules/subos/platform`; Linux implements them, macOS / Windows return `unsupported` with a reason, and the fake provider runs everywhere so flow tests run on all three CI legs. |
| Consistency | Exit codes, event schema, hint format and the agent contract are tables in the design doc *and* fixed by tests (T4). |
| Seamless upgrade | Nothing on disk needs a migration step to keep working. New files (`config/subos/<n>/policy.json`, `logs/subos/<n>/`, `state/isolation-caps.json`, `run/subos/<n>/`) are created on first use; their absence means "defaults". |

## 2. Dependency graph

```
C0 docs
 └─ T1 testkit ── T2 xdev test/report ── T3 CI ── T5 coverage map + lane caps
        └─ T4 contract scan (needs C13 for the agent half; exit-code half first)
C1 guard + observe ─┐
C2 modules/subos skeleton (+gpu, graphics, manifest) ─┐
C3 Ports + HomeView (+userdata, keeper) ──────────────┤
C4 split subos.cpp (model → module, adapter stays) ───┘
C5 HomeContext + .xlings-home mode/layout ── C26 deployment mode S / M read path
C6 writer rules (keep unknown keys, policy under config/subos/)
C8 platform interfaces + SandboxSpec + providers (bwrap/proot/home-redirect/fake)
 ├─ C10 mount topology (F1, F6)          ← C7 write-surface inventory is its L4 baseline
 ├─ C11 env / pid / ipc / uts / die-with-parent / new-session / TIOCSTI / probe text
 ├─ C17 caps probe + cache + must/should + hints + --no-degrade
 │    ├─ C18 Landlock provider
 │    ├─ C20 identity neutralisation, net=none, --disable-userns
 │    ├─ C21 self doctor --isolation [--fix] (root-owned bwrap + narrow AppArmor profile)
 │    └─ C23 net=nat via pasta, --publish (proxy: see §4)
 └─ C9 session model (supervisor, session-init, exec.sock + SCM_RIGHTS, lifecycle/ops events)
      ├─ C12 subos exec / start / stop / --temp / cp ; interface subos_exec
      ├─ C22 observability: exec (seccomp notify), fs, redaction, report, subos_events, XLINGS_TRACE
      └─ C15 broker: auto/ask/deny, install --subos, requests/approve, perm events
C13 audience contract (XLINGS_AGENT_MODE, Asker) ── C15
C14 policy model (presets, overrides, decide, --sandbox=<preset>, subos config/status)
 ├─ C15, C16 rules + policy packs, C19 --mount / --allow
C25 subos doctor + environment matrix + perf budgets (last: it reads everything above)
```

Hard edges (a later item cannot be written without the earlier one):
C8→C10/C11/C17; C9→C12/C15/C22; C13→C15; C14→C15/C16/C19; C1→C3 (userdata
needs guard/observe); C5→C26; T1→every new e2e test.

## 3. Checkpoints with their verification

| # | Content | Verification tier |
|---|---|---|
| C0 | Design doc; fix the isolation matrix in existing docs; AGENTS.md: core vs interaction surfaces, `xdev` | docs |
| T1 | `modules/testkit` (`XTEST`-style metadata, `Home::isolated`, process runner with env/timeout/pty, JSON + event assertions, failure capture) | testkit unit tests, 3 OS legs |
| T2 | `apps/xdev test / report` (mcpp NDJSON + legacy sh/ps1/py adapter → one report) | same output locally and in CI |
| T3 | CI: one release build per OS feeding the e2e job; asan off the PR critical path (push to main, nightly, `ci:asan` label); step summary from `xdev report` | measured PR critical path |
| T4 | Contract scan: exit-code table, agent never blocks on input (pty, no input) across the CLI spec | L2 |
| T5 | Requirement-ID map (`tests/requirements.toml`), "uncovered ID fails", lane capability declaration (fixes F13) | `xdev report --coverage` |
| C1 | `modules/guard` (UserConfirmed + Asker port), `modules/observe` (event schema, journal incl. destructive log, redaction) | existing tests |
| C2 | `modules/subos` skeleton; `gpu`, `graphics`, `manifest` moved | existing tests |
| C3 | `Ports`, `HomeView`; `userdata`, `keeper` moved | existing tests |
| C4 | Pure model of `subos.cpp` (counts, candidates, edit distance) moved; adapter stays in `src/core/subos` | existing tests |
| C5 | `src/core/home/`: `HomeContext`, `.xlings-home` `mode` + `layout` | L1 + L5 |
| C6 | Writers keep unknown keys; policy file location | L1 |
| C8 | `FsGate … RootfsRuntime` interfaces + `probe()`; `SandboxSpec`; bwrap / proot / home-redirect / fake providers; spec golden tests | L1 golden |
| C10 | `$XLINGS_HOME` RO, only own subos RW, other instances / logs / state / run hidden (C7 inventory as the L4 baseline) | L4 (F1, F6) |
| C11 | env allow-list, `--unshare-pid/ipc/uts`, `--die-with-parent`, `--new-session` (exec) + TIOCSTI seccomp (interactive), probe text without the sysctl advice | L4 (F3, F4, F7, F8, F12) |
| C9 | supervisor (fork + wait, no more `execvp`), session-init, `exec.sock` with fd passing, lifecycle / ops events, `subos ps` / `subos log` | L3 + L4 |
| C12 | `subos exec` / `start` / `stop` / `--temp` / `cp`; interface `subos_exec` | L2 exit codes + L3 |
| C13 | `XLINGS_AGENT_MODE` / `--agent`, `Asker`, agent errors / candidates / never-blocking | L2 scan |
| C14 | presets dev / private / locked, overrides, merge, `policy::decide`, `--sandbox=<preset>`, `subos config` / `status`, unknown security field → refuse | L1 + L3 |
| C15 | client + broker, auto / ask / deny, `install --subos`, `requests` / `approve`, `perm` events | L3 + L4 |
| C16 | ordered rules; policy packs (`type = "subos-policy"` payload carrying `policy.json`) | L1 + L3 |
| C17 | caps probe, cache in `state/isolation-caps.json`, must / should, hint format, `--no-degrade` | L3 (fake caps) + L6 |
| C18 | Landlock provider (ABI ≥ 1, optional net / scope at higher ABIs) | L4 |
| C19 | `--mount src:dst[:ro|rw]`, `--allow display|audio|camera|gpu|ssh-agent|dbus|host-loopback` | L3 + L4 |
| C20 | neutral identity, `net=none`, `--disable-userns` | L4 (F5, F7) |
| C21 | `self doctor --isolation [--fix]`; bwrap recipe change goes to xim-pkgindex separately | L6 (F10) |
| C22 | exec audit (seccomp user notification), fs change list, redaction, `subos report`, interface `subos_events`, `XLINGS_TRACE` | L3 + L4 + L8 |
| C23 | `net=nat` (pasta, `--publish`, `--allow host-loopback`) | L4 (F2, F5) |
| C24 | `fetch=layer` | L3 + L4 |
| C25 | `subos doctor`; environment matrix; perf budgets as assertions | L6 + L7 |
| C26 | deployment mode S; system layer (M) read path | L5 |
| C27 | rootfs — maintainer decision, see §4 | — |

## 4. Scope decisions inside this PR

Recorded here so the PR description and the final report say the same thing:

- **C27 rootfs** stays out of this PR (the design leaves it to the maintainer;
  it needs subuid mapping and a rootfs package format, neither of which the
  rest depends on). The `RootfsRuntime` interface exists and reports
  `unsupported`.
- **`net=proxy`** is specified but not built here: it needs the in-sandbox
  forwarder plus the supervisor bridge, and nothing else depends on it.
  Selecting it reports the shared "not implemented on this platform yet" hint
  and, under `--no-degrade`, refuses. `nat` and `none` ship.
- **`fetch=layer`** (C24) and **resolving packages from the system layer
  (M)** are one mechanism -- a layered payload lookup in xvm's version DB and
  the installer -- and both follow the PR (`PERM-FETCH-LAYER`,
  `HOME-LAYER-RESOLVE`, deferred with that reason). Until then `layer` is
  refused wherever it appears; before this change it was granted and
  installed into the shared home. The system layer is found and reported.
- **Policy packages** are selected per instance (`subos config --sandbox
  ns:name`); a pack named on a single call is not taken (a call tightens
  presets and overrides only).
- **Landlock** ships as an explicit backend (`--sandbox landlock`), never an
  automatic stand-in for bwrap: it fences writes and hides nothing.
- **T6** (`xdev lint` / `xdev release`) follows the PR, as the design allows.
- **macOS / Windows** keep home-redirect; every interface reports
  `unsupported` with the reason, which is what `subos status` prints.
- Anything in this list that turns out cheap during implementation is pulled
  in and this section is updated in the same commit.

## 5. Release

Version: the date the PR lands, `.1` (`2026.10.6.1`). After `release.yml`
publishes the assets: top up GitCode with the local `gtc` immediately (do not
wait for the release workflow's mirror step), bump `xim-pkgindex/pkgs/x/xlings.lua`,
then verify the ecosystem on a real home: quick install, `self update`,
`subos new / use --sandbox / exec`, `install --subos`, mcpp build of xlings.
