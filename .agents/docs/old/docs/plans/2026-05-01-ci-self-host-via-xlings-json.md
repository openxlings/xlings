# CI Self-Host: Build xlings's Dependencies Via Its Own `.xlings.json`

**Date**: 2026-05-01
**Status**: planning
**Owner**: TBD

## Goal

Replace the manually-scripted dependency-install steps in xlings's GitHub Actions workflows (CI + release) with a single declarative `.xlings.json` at the repo root, consumed by a pinned bootstrap xlings via `xlings install`. The CI gets simpler, dependency upgrades become a one-line edit, and xlings dogfoods itself.

## Scope

| In scope                                            | Out of scope                          |
|-----------------------------------------------------|---------------------------------------|
| New `.xlings.json` at repo root                     | Adding new packages to xim-pkgindex   |
| Refactor `xlings-ci-{linux,macos,windows}.yml`      | Changing toolchain (Linux still musl-gcc, macOS still LLVM, Windows still MSVC) |
| Refactor `release.yml` (3 build jobs)               | Refactoring the build itself (xmake commands stay the same) |
| Pin bootstrap xlings to a known-good version (0.4.8)| Replacing xim-pkgindex source-of-truth |
| Cache `~/.xlings/data/xpkgs` across runs            | Hot-update bootstrap version (keep it pinned) |

## Resolved decisions

| # | Question | Decision | Rationale |
|---|----------|----------|-----------|
| 1 | Windows toolchain | **MSVC stays default** | Native runner toolchain works; mingw via xlings is opt-in for users only |
| 2 | Add cmake + ninja to deps? | **Yes — all platforms** | xmake invokes them when building/pulling third-party packages; future-proofing avoids surprise CI breakage |
| 3 | Bootstrap xlings version | **Pinned to 0.4.8** | First release shipping `self doctor` + alias-removal + mirror config maturity. Pinned via `BOOTSTRAP_XLINGS_VERSION` env, can be bumped per-PR |
| 4 | Apply to release.yml as well? | **Yes — same refactor** | release.yml duplicates the same env-setup; keeping it in sync prevents drift |
| 5 | mirror config in .xlings.json | **`"mirror": "GLOBAL"`** | CI runs on github.com; gitee endpoints in some packages need auth and aren't reachable in Actions |

## The `.xlings.json` (repo root, new file)

```json
{
  "$schema-comment": "Build dependencies for xlings itself. CI and contributors run `xlings install` from the repo root to install everything below.",
  "mirror": "GLOBAL",

  "workspace": {
    "xmake":  "3.0.7",
    "cmake":  "4.0.2",
    "ninja":  "1.12.1",

    "musl-gcc": {
      "linux":  "15.1.0"
    },
    "llvm": {
      "macosx": "20.1.7"
    }
  }
}
```

Notes:
- `xmake`, `cmake`, `ninja` have no platform map → installed on all three OSes.
- `musl-gcc` is keyed `linux` only; macOS and Windows skip it (no key for that OS = nothing installed).
- `llvm` is keyed `macosx` only.
- Windows declares no compiler in this file — uses MSVC from the GitHub runner image.
- **`"projectScope": false`** opts the file out of project-mode activation. The `xlings install` no-args command still reads `workspace` from this file (the install code parses `.xlings.json` directly), but project subos / state writes / `XLINGS_PROJECT_DIR` env-export are all skipped. Without this, every `xlings` invocation from any subdir of the repo would walk up to this file and treat the repo root as a project root — polluting `<repo>/.xlings/.xlings.json` with state across runs. See `src/core/config.cppm:load_project_config_from_dir_` for the gate.

## Bootstrap xlings (the chicken-and-egg solution)

CI installs a known-good xlings via `quick_install.sh` pinned to a specific tag, then uses **that** xlings to read `.xlings.json` and install everything else.

```yaml
env:
  BOOTSTRAP_XLINGS_VERSION: v0.4.8

# ...
- name: Install bootstrap xlings (pinned)
  env:
    XLINGS_NON_INTERACTIVE: 1
    XLINGS_VERSION: ${{ env.BOOTSTRAP_XLINGS_VERSION }}
  run: |
    curl -fsSL https://raw.githubusercontent.com/d2learn/xlings/main/tools/other/quick_install.sh | bash
    echo "$HOME/.xlings/subos/current/bin" >> "$GITHUB_PATH"
    echo "$HOME/.xlings/bin" >> "$GITHUB_PATH"
```

Pre-condition: `quick_install.sh` must honor `XLINGS_VERSION` to install a specific version. **Verify before merging Phase 2** — if the script doesn't support pinning yet, add support as a small pre-PR. (Worst case: download the release tarball directly and run `self install`.)

## Common CI step (the new shared template)

After bootstrap, every workflow does:

```yaml
- name: Cache xlings packages
  uses: actions/cache@v4
  with:
    path: |
      ~/.xlings/data/xpkgs
    key: xlings-deps-${{ runner.os }}-${{ hashFiles('.xlings.json') }}
    restore-keys: |
      xlings-deps-${{ runner.os }}-

- name: Install build dependencies (declared in .xlings.json)
  run: |
    cd $GITHUB_WORKSPACE
    xlings install -y      # reads ./.xlings.json, installs all deps under workspace
```

After this step, `xmake`, `cmake`, `ninja` are on PATH (via xlings shims under `~/.xlings/subos/current/bin`). For tools whose install path xmake needs explicitly (musl-gcc SDK root, llvm prefix), exports follow:

```yaml
# Linux only
- name: Export toolchain paths
  run: |
    MUSL_SDK="$HOME/.xlings/data/xpkgs/musl-gcc/15.1.0"
    test -d "$MUSL_SDK" || { echo "musl-gcc not installed"; exit 1; }
    echo "MUSL_SDK=$MUSL_SDK" >> "$GITHUB_ENV"
    echo "$MUSL_SDK/bin"    >> "$GITHUB_PATH"
    echo "CC=x86_64-linux-musl-gcc"  >> "$GITHUB_ENV"
    echo "CXX=x86_64-linux-musl-g++" >> "$GITHUB_ENV"
    bash tools/setup_musl_runtime.sh "$MUSL_SDK"
```

Idea is to keep the xmake-configure + build steps unchanged so the diff is purely "drop the manual download in favor of declarative install".

## Per-workflow diffs (skeleton)

### `xlings-ci-linux.yml`

**Removed**:
- "Install xmake (bundled v3.0.7)" — replaced by xlings install
- "Install musl-gcc 15.1" curl/tar — replaced by xlings install
- "Install Xlings" (already used quick_install — repurpose to bootstrap pinned)

**Added**:
- Cache action keyed on `.xlings.json` hash
- "Install build dependencies" runs `xlings install -y`
- Compact "Export toolchain paths" using `$HOME/.xlings/data/xpkgs/musl-gcc/15.1.0` as the SDK root

Net change: ~−15 lines.

### `xlings-ci-macos.yml`

**Removed**:
- "Bootstrap legacy xlings" hand-rolled tarball download (lines 34-47)
- "Install LLVM toolchain with legacy xlings" hand-rolled `install llvm@... -y` (lines 49-67)
- The `xmake-io/github-action-setup-xmake` action

**Added**:
- Bootstrap pinned xlings via quick_install (same as Linux)
- Cache action
- `xlings install -y` reads `.xlings.json` → installs xmake + cmake + ninja + llvm

The `LLVM_PREFIX` export is still needed for `xmake f --sdk=...`. Path: `$HOME/.xlings/data/xpkgs/llvm/20.1.7` (or whatever xlings-pkgindex stores).

Net change: ~−25 lines.

### `xlings-ci-windows.yml`

**Removed**:
- The `xmake-io/github-action-setup-xmake@v1 latest` action

**Added**:
- Bootstrap pinned xlings (Windows-flavored quick_install — verify it exists and works on PowerShell)
- Cache action
- `xlings install -y` reads `.xlings.json` → installs xmake + cmake + ninja

MSVC remains auto-detected from the runner. No toolchain export needed.

Net change: ~−5 lines.

### `release.yml`

Three jobs (`build-linux`, `build-macos`, `build-windows`), each currently duplicates the corresponding CI workflow's setup. Apply the same diffs:

- Replace per-job manual downloads with bootstrap-xlings + xlings-install + cache.
- Keep the release-specific steps (artifact upload, GitHub Release creation, etc.) untouched.

Net change: ~−45 lines across the 3 jobs.

## Caching: expected savings

Cache stores `~/.xlings/data/xpkgs` (the actual binary payloads) plus the xlings DB.

| Phase | Linux time | macOS time | Windows time |
|-------|-----------|-----------|--------------|
| Today's setup phase | ~90-120s | ~120-180s | ~30-60s |
| New (cache hit, common case) | **~5-10s** | **~5-10s** | **~5-10s** |
| New (cache miss, after `.xlings.json` change) | ~90-120s | ~120-180s | ~60-90s |

Across PR + release CI runs per day, expect ~3-5 minutes/day savings, plus less variance.

## Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| Bootstrap 0.4.8 has a bug that breaks `xlings install` | Pin is exactly so we control the version. Bump bootstrap to 0.4.9 in a separate PR after verifying release |
| `quick_install.sh` doesn't honor `XLINGS_VERSION` | Verify pre-Phase-2; if missing, add support as a tiny preliminary PR |
| `XLINGS_RES` token (used by musl-gcc / cmake / ninja / llvm) resolves to gitee, unreachable in CI | `mirror: "GLOBAL"` + workflow `XLINGS_RELEASE_MIRROR: GLOBAL` env force github resolution. If any package only has gitee, that package needs a GLOBAL URL — independent xim-pkgindex PR |
| Cache poisoning (a broken package gets cached) | Cache key includes `.xlings.json` hash; bumping any version invalidates. Manual cache-bust by appending a version comment to `.xlings.json` |
| GitHub Actions rate-limits hit during quick_install | Bootstrap step retries 3× with exponential backoff. The release tarball download is small (<10 MB), well under typical limits |
| musl-gcc tarball is ~600 MB; cache-miss runs may time out | Set `actions/cache` `enableCrossOsArchive` and `fail-on-cache-miss: false`; cap the install step at 10 min explicit timeout |
| .xlings.json conflict with project consumers | This file is conceptually "xlings's own build config". Document at the top that it's for xlings devs, not the same as a consumer project |

## Pre-flight checks (before Phase 2)

Run these before refactoring CI to catch blockers early:

1. **`quick_install.sh` version-pin support**: read `tools/other/quick_install.sh`; confirm it honors `XLINGS_VERSION` env. If not — add it (tiny PR).
2. **musl-gcc / cmake / ninja / llvm GLOBAL URL resolution**: for each package, with `XLINGS_RELEASE_MIRROR=GLOBAL`, does the URL resolve to a publicly reachable github / gitcode endpoint? If any one is gitee-only, fix in xim-pkgindex first.
3. **xmake on PATH after `xlings install`**: verify the shim approach works — `xmake --version` from a fresh CI runner after `xlings install -y` should print 3.0.7.
4. **macOS LLVM payload path**: confirm `$HOME/.xlings/data/xpkgs/llvm/20.1.7/bin/clang++` exists after install (matches what current CI assumes).

## Implementation phases

Each phase is one PR. Each PR is independently reviewable + revertible.

### Phase 1 — Add `.xlings.json` (non-breaking, this is the planning PR)

- New file: `/.xlings.json` (above contents)
- README contributor section: "To build xlings, install xlings (any recent version), then `xlings install` in this repo to fetch xmake / cmake / ninja / toolchains automatically."
- No CI workflow changes
- **Acceptance**: `xlings install` from repo root completes successfully on Linux + macOS + Windows for a contributor. CI is unchanged.

### Phase 2 — Refactor `xlings-ci-linux.yml`

- Remove: "Install xmake (bundled)", "Install musl-gcc 15.1" hand-rolled steps
- Repurpose: existing "Install Xlings" → "Install bootstrap xlings (pinned 0.4.8)"
- Add: cache action, "Install build dependencies", "Export toolchain paths"
- **Acceptance**: PR CI runs end-to-end (build + all 18 e2e steps) with comparable or better timing. Cache miss + cache hit paths both validated.

### Phase 3 — Refactor `xlings-ci-macos.yml`

- Remove: "Bootstrap legacy xlings" + "Install LLVM toolchain with legacy xlings" + xmake action
- Replace with the same template as Linux (bootstrap pinned + cache + xlings install)
- Update toolchain export to use `$HOME/.xlings/data/xpkgs/llvm/20.1.7` as `LLVM_PREFIX`
- **Acceptance**: Same as Phase 2 (Linux). The hand-rolled `LLVM_ALIAS_ROOT` symlinking can be dropped.

### Phase 4 — Refactor `xlings-ci-windows.yml`

- Remove: xmake action
- Add: bootstrap pinned xlings (PowerShell flavored) + cache + xlings install
- MSVC remains auto-detected
- **Acceptance**: Same as Phase 2.

### Phase 5 — Refactor `release.yml`

- Same structural changes as Phases 2-4 across the three release jobs
- Keep release-specific steps (tarball/zip artifacts, GitHub Release upload) intact
- **Acceptance**: A test release run produces identical artifacts to a current release.

### Phase 6 (optional) — Cleanup

- Once all 4 workflows are using `.xlings.json`, delete `BOOTSTRAP_LLVM_VERSION` env (it's now read from `.xlings.json`'s llvm version)
- Delete any orphaned env vars
- Document the bootstrap-xlings bump procedure in `tools/`

## Future-evolution markers

This is **permanent infrastructure code** — no `COMPAT` markers needed. The `BOOTSTRAP_XLINGS_VERSION` is the only piece that will need periodic bumps; track those in commit history, not in a TODO/COMPAT scheme.

When xlings adopts a `xlings.lock` (lockfile) mechanism, this `.xlings.json` would gain a paired `.xlings.lock` for reproducible CI, but that's a separate feature.

## Out of scope (deferred to follow-up issues)

- Self-doctor check that warns when the repo's `.xlings.json` declared deps don't match what's currently installed
- Pre-commit hook that runs `xlings install --dry-run` to verify the file parses
- A `tools/dev-bootstrap.sh` to install bootstrap xlings + run `xlings install` for one-step contributor onboarding
- Lockfile generation
