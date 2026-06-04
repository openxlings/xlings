# Shim Owner-Anchoring Design (shims bind to their owning home)

> Date: 2026-06-04
> Status: design proposal for review
> Related: `2026-05-29-compact-git-bootstrap-plan.md`, `2026-05-22-selfcontained-detection-windows-bug.md`
> Evidence: mcpp-community/mcpp PR #114 (temp Windows CI repro, branch `test/windows-no-git-user-flow`)

## TL;DR

A shim's behavior must be determined **only by the location of the shim file**,
never by ambient environment variables. `XLINGS_HOME` keeps exactly one
meaning: it selects which sandbox the **xlings CLI** manages (install/update
target). It has **zero effect** on shim dispatch.

This fixes a confirmed field failure (mcpp on Windows: package index can never
sync, `mcpp build` and `mcpp self init --force` fail, while the user's
terminal `git --version` works fine) and removes a whole class of env-leak
bugs around nested/redirected homes.

## 1. Problem

### 1.1 Confirmed failure chain (reproduced in CI)

Default Windows user machine, no system Git. Reproduced end-to-end in
mcpp-community/mcpp PR #114 (windows-latest, preinstalled Git disabled):

1. `xlings install mcpp` works without git (release zip bundles an
   xim-pkgindex snapshot) — but installs the **stale snapshot version**
   (0.0.36 at the time, not latest).
2. `xim:git` install registers an **xvm shim**: the xlings binary
   hardlinked/copied as `git.exe` into `<home>/subos/<x>/bin`
   (`pkgs/g/git.lua` → `xvm.add`, `src/core/xvm/shim.cppm`). The shim
   resolves its versions DB via **env `XLINGS_HOME`**
   (`src/core/config.cppm` ctor, ~L400).
3. User terminal: `XLINGS_HOME=~/.xlings` (set as a User env var by
   `xlings self install`) → shim finds git → `git --version` ✅.
4. mcpp redirects `XLINGS_HOME` to `~/.mcpp/registry` for its vendored
   xlings. The PATH `git.exe` shim inherits the redirected env, looks up
   the registry home's DB, and fails:
   ```
   [error] xlings: 'git' is not installed
   ```
5. Index sync is git-only (`src/core/xim/repo.cppm`: "git is required to
   update package index", no HTTP fallback). The git auto-bootstrap
   (`xlings install xim:git`) itself needs the index → recursion guard
   (`XLINGS_COMPACT_GIT_BOOTSTRAP`) fires → **deadlock**. The registry home
   never gets an index; ninja/llvm can't install; `mcpp build` fails;
   `mcpp self init --force` reports `init incomplete: ninja missing from
   sandbox`.

Smoking-gun probe from the CI run:

```
probe A: git --version  XLINGS_HOME=~/.xlings        → git version 2.51.0.windows.2 (exit 0)
probe B: git --version  XLINGS_HOME=~/.mcpp/registry → [error] xlings: 'git' is not installed (exit 1)
```

### 1.2 Root cause, generalized

`XLINGS_HOME` currently serves two roles that must be separated:

| Role | Who needs it | Correct source |
| --- | --- | --- |
| **Management target** — where the xlings CLI installs/updates | xlings CLI (`argv[0] == "xlings"`) | env var (current behavior is right) |
| **Dispatch identity** — which sandbox owns a shim | every shim (`argv[0] == <tool>`) | the shim file's own location |

Because role 2 currently reads the env var, *any* process tree that
redirects `XLINGS_HOME` (mcpp today; any future embedder; nested sandboxes;
CI) silently breaks **every shim-resolved tool on PATH** (git, python,
node, …) inside that tree.

Note the irony: exe-anchoring already exists in the codebase — the
"self-contained" detection in the `Config` ctor — but only as a fallback
when the env var is *absent*. The bug is precisely that the env var
unconditionally wins.

## 2. Semantic contract (the design, in one sentence)

> **A shim is an artifact of exactly one home. Invoking it always resolves
> against that home — "which shim file PATH hits" is the selector, ambient
> env is not.**

Corollaries:

- "I want home B's tools" has exactly one correct spelling: put B's
  `subos/<x>/bin` on PATH (source B's shell profile — already generated
  per-home under `config/shell/xlings-profile.*`).
- "A's shim borrowing B's payload" (today's accidental behavior when env
  points at B) becomes **deprecated**: kept alive during a transition
  window as a lower-priority fallback (§3, §6), then removed. It was never
  coherent: it silently swaps the entire versions DB under a file the user
  thinks belongs to A.
- No `XLINGS_SHIM_HOME` override env var. The contract's value is its
  absoluteness; an escape hatch would reintroduce ambient-env dependence.
  (The temporary `XLINGS_SHIM_ANCHOR` rollback flag is a release-safety
  concern, not part of the contract — see §6.)

This mirrors rustup/pyenv/volta shim design: shims hardcode/derive their
root; env selects nothing at dispatch time.

## 3. Resolution algorithm

### Compatibility-ordered lookup (transition default): owner → env → default

Three possible orderings were considered:

| Ordering | mcpp bug | "borrowing" (env home has the tool, owner doesn't) | both homes have the tool | semantics |
| --- | --- | --- | --- | --- |
| A. owner only (strict) | fixed | hard error | owner wins | purest |
| **B. owner → env → default** | **fixed (first hop)** | **kept (second hop)** | **owner wins (precedence flip vs today)** | **good** |
| C. env → owner → default | fixed (fallback rescue) | unchanged | env wins (byte-for-byte today) | poor: shims stay env-driven |

C is the only zero-behavior-change ordering, but it re-entrenches the wrong
semantics (env still primary for shims). **B is the shipping default**: its
only behavior delta vs today is the precedence flip when *both* homes have
the tool — exactly the workflow being deprecated — and that case is cheaply
**detectable** (after an owner hit, probe the env home once; if it could
also serve, log a warning instead of silently changing behavior). A is the
documented end state once the deprecation window closes.

```
shim_dispatch(program, argv):
    owner = resolve_owner_home(exe_path())

    # hop 1: owner home (the shim's own home) — primary, the contract
    if owner and has_program(owner, program):
        if env XLINGS_HOME set and env != owner and has_program(env_home, program):
            log::warn("shim '<tool>' owned by <owner> also resolvable in "
                      "$XLINGS_HOME=<env>; using owner (env-based shim "
                      "switching is deprecated — put <env>/subos/<x>/bin on PATH)")
        Config::override_home(owner)
        dispatch

    # hop 2: env XLINGS_HOME — DEPRECATED compat fallback ("borrowing")
    elif env XLINGS_HOME set and has_program(env_home, program):
        log::warn("shim '<tool>' not installed in its owning home <owner>; "
                  "falling back to $XLINGS_HOME=<env> (deprecated, will be removed)")
        Config::override_home(env_home)
        dispatch

    # hop 3: default ~/.xlings (covers orphan shims copied out of a home)
    elif has_program(default_home, program):
        Config::override_home(default_home)
        dispatch

    else:
        error:
            xlings: '<tool>' is not installed in <owner home>
              hint: xlings install <tool>     (this shim belongs to <owner home>)
              (also checked: $XLINGS_HOME=<env>, ~/.xlings)
```

`has_program(home, p)` = home's workspace/DB has an active or installed
version of `p` (the tri-state diagnostic in `shim_dispatch` already
computes this). A *registered-but-broken* entry (versions exist, payload
path missing) counts as a hit — the error is then reported against that
home rather than silently jumping homes, so genuine breakage is not
masked.

Every hop is logged at debug level; the warn-level messages fire only on
the two deprecated paths, giving users one release cycle of visible
migration signal before hop 2 is removed (→ ordering A).

### 3.1 `resolve_owner_home(exe_path)`

Walk `exe_path` parents upward; the first directory matching the
**structural home signature** wins:

```cpp
bool is_home_root(dir) =
       exists(dir / ".xlings.json")
    && exists(dir / "bin" / ("xlings" + EXE_SUFFIX))
    && is_directory(dir / "subos");          // structural disambiguator
```

Platform notes:

- **Windows**: shims are hardlinks/copies; `GetModuleFileNameW` returns the
  invoked shim path (e.g. `<home>/subos/current/bin/git.exe`) → walk-up
  finds `<home>`. The `subos/current` dir itself can never false-match: it
  has no `subos/` subdirectory. This **replaces and deletes** the fragile
  JSON-content disambiguation hack in the `Config` ctor (the
  `version`+`activeSubos` key check documented in
  `2026-05-22-selfcontained-detection-windows-bug.md`) with a structural
  test.
- **Linux/macOS**: shims are (relative) symlinks to `<home>/bin/xlings`;
  prefer absolutized `argv[0]` (the shim's own path), fall back to
  `/proc/self/exe` / `_NSGetExecutablePath` — both land inside the owning
  home, so the walk-up converges either way.
- **mcpp registry** (`~/.mcpp/registry`): has `.xlings.json` +
  `bin/xlings.exe` + `subos/` → qualifies as a home root regardless of
  which JSON keys mcpp's seed file contains. Its own shims
  (`registry/subos/default/bin/ninja.exe`, …) anchor to the registry —
  correct, and **no longer dependent on mcpp setting env at all**.
- **Nested homes** (e.g. a registry under some outer home's
  `data/xpkgs/...`): the upward walk meets the **innermost** home root
  first — innermost wins, which is the correct owner.
- **Project trees**: see §3.2 — project shims anchor to the **global**
  home (their payloads live there), never to the project state dir.

### 3.2 Project shims: anchor to the global home, overlay the project workspace

**Requirement (important):** a shim living in a local project tree
(`<project>/.xlings/subos/<x>/bin/<tool>`) must still use the **global
home's xpkgs payloads**. Project mode shares payloads with the user home —
the project only contributes *workspace state* (which version is active
here) and optionally additive project data dirs; it is **not** a home and
owns no `data/xpkgs` payload store of its own.

The design satisfies this by construction:

- Project shims are created from `p.homeDir / "bin" / "xlings"` — the
  **global home's** binary (`xvm/commands.cppm`; in project mode only
  `binDir`/`subosDir` move into the project tree, `homeDir` stays the user
  home). So:
  - **Unix**: the shim symlink resolves into `<global home>/bin/xlings` →
    owner anchoring lands on the **global home**. `$XLINGS_HOME`
    placeholders in vdata expand against it → payload lookups hit the
    global `data/xpkgs` ✓.
  - **Windows**: the hardlink/copy path is inside the project tree; the
    upward walk finds **no** home root there — deliberately:
    `<project>/.xlings` has `subos/` but no `bin/xlings.exe`, so the
    structural signature excludes it. The orphan fallback (env →
    `~/.xlings`) then lands on the global home ✓.
- The **project state dir must never qualify as a home**. This is exactly
  why the signature requires all three of `.xlings.json` +
  `bin/xlings[.exe]` + `subos/` — anchoring a project shim to
  `<project>/.xlings` would point payload expansion at a directory that
  has no xpkgs and break every project tool.
- **Version selection is unaffected**: after `override_home(global)`, the
  existing project discovery (cwd walk + `XLINGS_PROJECT_DIR` +
  `effective_workspace()` merge: project > subos > global) still decides
  *which version* runs. Anchoring decides *whose payloads*; the project
  overlay decides *which version*. The two axes stay orthogonal.
- Packages installed via additive project data roots (custom project
  indices, e.g. mcpp's `<project>/.mcpp/.xlings/data`) record
  absolute/project-relative vdata paths and are unaffected by the home
  choice.

Optional refinement (separate follow-up): when the Windows walk-up passes
through a project state dir (`*/.xlings` with `subos/` but no
`bin/xlings`), the shim location itself reveals the project root — we
could export `XLINGS_PROJECT_DIR=<parent>` if unset, making project shims
behave correctly even when invoked from outside the project cwd. Unix gets
the same for free only when cwd is inside the project; today both
platforms rely on cwd/env, so this is a strict improvement, not a
regression fix.

### 3.3 What stays untouched

`Config::override_home()` is a pre-init injection of `paths_.homeDir`;
everything downstream is reused verbatim:

- subos selection: project mode > `XLINGS_ACTIVE_SUBOS` > home
  `.xlings.json` `activeSubos` (`update_effective_paths_()`),
- project discovery: cwd walk + `XLINGS_PROJECT_DIR` + the 0.4.20
  xlings-home boundary check (`load_project_config_()`),
- `effective_workspace()` merge order (project > subos > global),
- `expand_path(vdata->path, home)` — now always expands `$XLINGS_HOME`
  placeholders with the owner home, so **DB and payload are always from
  the same home** (also kills the latent "A's vdata expanded with B's
  path" mismatch).
- The xlings CLI multicall short-circuit in `main.cpp` — CLI home
  resolution (env > self-contained > default) is **completely unchanged**.

## 4. Impact analysis

| # | Scenario | Today | After | Change |
| --- | --- | --- | --- | --- |
| 1 | Single home, env set by installer | env hit | anchor = same home | none |
| 2 | env unset (manual/portable install) | self-contained detection | anchor, equivalent but sturdier | none |
| 3 | **mcpp redirects env; PATH hits global git shim** | `'git' is not installed` (bug) | anchors to `~/.xlings` → git works | **fixed** |
| 4 | mcpp registry's own shims (ninja, …) | works only because mcpp sets env | anchors to registry; env irrelevant | sturdier |
| 5a | Multi-home "borrowing": A's shim on PATH, env→B, tool only in B | resolves against B | still resolves against B (hop 2) + deprecation warning | none during transition; removed at end state |
| 5b | Multi-home, **both** A and B have the tool, env→B | B wins | **A (owner) wins** + ambiguity warning | ⚠ only behavior delta of the transition default; correct spelling is "put B's bin on PATH" |
| 6 | Residue shim, tool uninstalled from its home | error (against env home) | error naming the owner home; `xlings doctor` orphan-shim check already cleans these | clearer |
| 7 | project mode / named·anonymous subos / `XLINGS_ACTIVE_SUBOS` | — | unchanged (orthogonal, applied after `override_home`) | none |
| 8 | Portable home moved wholesale | env points at stale path → breakage | anchor follows the new location | sturdier |
| 9 | xlings CLI itself (incl. mcpp invoking `registry/bin/xlings.exe` with env set) | env-driven | env-driven (untouched) | none |
| 10 | Shim physically in subos-X's bin while activeSubos=Y | resolves Y's workspace | same (anchoring stops at home level) | none — see §7 |

Net: every currently-working scenario keeps its result; the only behavior
change is #5, which replaces an accident with a contract.

## 5. Implementation plan

1. `xvm::resolve_owner_home(exe_path) -> optional<fs::path>` — pure
   function + structural signature (new, ~30 lines; unit-testable with a
   tmpdir home layout).
2. `Config::override_home(const fs::path&)` — static pre-init hook on the
   lazy singleton; assert it is called before first `instance_()` use.
3. `main.cpp` multicall: when `program_name` is not `xlings`, run the §3
   lookup chain and inject the chosen home before `shim_dispatch`. Factor
   `has_program(home, name)` so each hop is a cheap DB probe (reuse the
   tri-state diagnostic); `Config` must support constructing the probe
   against a non-final home (lightweight read-only workspace load, or
   defer singleton finalization until the home is chosen).
4. `shim_dispatch` miss-path error text: name the owner home and list the
   other consulted homes (see §3); warn on hop-2 use and 5b ambiguity.
5. Delete the Windows JSON-content disambiguation in the `Config` ctor;
   replace the self-contained check with `is_home_root` (shared helper).
6. `xlings doctor`: new check — every shim under `<home>/subos/*/bin`
   anchors back to `<home>`; orphans flagged (extends the existing
   orphan-shim check in `xself/doctor.cppm`).
7. Docs + CHANGELOG: state the contract explicitly
   ("shims are bound to their owning home; `XLINGS_HOME` only selects the
   management target of the xlings CLI").

### Testing

- Unit: `resolve_owner_home` over layouts — real home; `subos/current`
  false-positive; project tree; nested home; orphan copy in `/tmp`.
- E2E (Linux): create home A and home B; matrix over the §3 orderings:
  - tool only in A (owner), env→B → A's tool (hop 1).
  - tool only in B, env→B, A's shim → B's tool + deprecation warning
    (hop 2); under Phase 2 strict mode → error naming A.
  - tool in both, env→B, A's shim → A's tool + ambiguity warning.
- E2E (project mode): `xlings install <tool>` inside a project, invoke the
  project-tree shim → must execute the payload from the **global**
  `data/xpkgs` (§3.2), with the project workspace's pinned version.
- E2E (Windows CI): replay the mcpp PR #114 flow — after this change,
  probe B (`XLINGS_HOME=~/.mcpp/registry; git --version`) must succeed.
- Audit xlings's own test suite for accidental reliance on scenario #5
  (env-borrowing through a foreign shim); rewrite to source the target
  home's profile instead.

## 6. Rollout: two phases

**Phase 1 (next release) — ordering B (owner → env → default):**

- Fixes the mcpp/Windows failure structurally (owner consulted first).
- Keeps "borrowing" alive as a deprecated hop-2 fallback with a visible
  warning; flags the both-homes ambiguity (5b) with a warning instead of
  silently flipping.
- `XLINGS_SHIM_ANCHOR=legacy` reverts to env-first dispatch (release
  insurance only — not part of the contract).
- `log::debug` every resolution: `shim home: owner=<p|none> env=<p|unset>
  chosen=<p> (hop N)`.

**Phase 2 (after 1–2 releases of soak) — ordering A (strict binding):**

- Remove hop 2 (env) — shims resolve only against their owning home
  (hop 3 default stays, for orphan shims).
- Remove `XLINGS_SHIM_ANCHOR`.
- Gate: no field reports / warning sightings from hop 2 during the soak
  window.

## 7. Explicitly out of scope (follow-ups)

- **Subos-level anchoring** (shim in `subos/devkit/bin` → prefer devkit's
  workspace over activeSubos): more intuitive under the same
  "PATH decides" principle, but an existing-behavior change — separate
  issue for maintainers to decide.
- **compact::git true-binary resolution** (defense in depth): when
  `git --version` output carries the `xlings:` prefix (shim self-failure
  signature), skip the `install xim:git` bootstrap and probe
  `data/xpkgs/xim-x-git/<ver>/{cmd,bin}/git` across candidate homes. Much
  less likely to trigger after anchoring lands, but cheap and orthogonal.
  See `2026-05-29-compact-git-bootstrap-plan.md`.
- **Git-less index sync** (HTTP zip/codeload fallback in
  `src/core/xim/repo.cppm`): solves the cold-start deadlock on machines
  with no git at all; orthogonal to this design.
- **Layered homes** (`read chain, write top` — registry reuses the global
  home's index snapshot and payloads instead of re-downloading): the
  long-term architectural answer to embedders like mcpp; tracked
  separately.

## 8. Effect on mcpp (downstream)

- The git/index deadlock's primary cause disappears; the mcpp↔xlings
  contract narrows to "env selects the CLI's management target" — no PATH
  ordering or env propagation subtleties left.
- mcpp-side hygiene still recommended (independent of this design):
  scope its `XLINGS_HOME`/PATH mutation to the xlings child process
  (Windows currently mutates its own process env via `_putenv_s`), and
  seed the registry index from `~/.xlings/data/xim-pkgindex` at init to
  skip one online clone.
