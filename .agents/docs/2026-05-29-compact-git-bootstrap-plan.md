# Compact Git Bootstrap Plan

> Date: 2026-05-29
> Status: implementation plan for review

## Goal

Create a centralized `compact` layer for xlings runtime compatibility helpers, and use it to remove direct Git CLI calls from the xlings binary runtime.

The first concrete problem solved by this layer is Git availability:

- `xlings update` should not fail immediately just because the host has no `git`.
- package-index sync and `.git` package downloads should use one Git command wrapper.
- when Git is missing, xlings may run `xlings install xim:git -y` once, then retry.
- the wrapper should document that the external Git CLI is a compatibility bridge and may later be replaced by an embedded Git library.

## Current Findings

### Runtime Git use in the xlings binary

These paths should be migrated to the new `compact` Git wrapper.

| File | Current Git use | Trigger |
| --- | --- | --- |
| `src/core/xim/repo.cppm` | `git --version` | package-index sync preflight |
| `src/core/xim/repo.cppm` | `git clone --depth 1 --quiet` | first-time index clone |
| `src/core/xim/repo.cppm` | `git -C <repo> remote set-url origin <url>` | index repo URL repair before update |
| `src/core/xim/repo.cppm` | `git -C <repo> pull --ff-only` | normal index update |
| `src/core/xim/repo.cppm` | `git -C <repo> fetch origin && git -C <repo> reset --hard origin/HEAD` | index update fallback |
| `src/core/xim/downloader.cppm` | `git -C <dest> pull --ff-only` | update an existing `.git` package source checkout |
| `src/core/xim/downloader.cppm` | `git clone --depth 1 --recursive --quiet <url> <dest>` | clone `.git` package sources |
| `src/core/xim/downloader.cppm` | `platform::spawn_command("git clone ...")` | cancellable `.git` source clone |

Primary user-visible triggers:

- `xlings update`
- `xlings self update`, because it shells out to `xlings update`
- `xlings install`, when the package index is missing and xlings tries to sync it
- `xlings install <pkg>`, when a package source URL ends with `.git`

### Non-runtime Git use

These are real Git uses, but should not block the first runtime implementation.

| Area | Files | Recommendation |
| --- | --- | --- |
| release index packaging | `tools/package_xim_index.sh`, `tools/package_xim_index.ps1` | keep script-level Git for now; CI/build machines should provide Git |
| e2e fixtures | `tests/e2e/*.sh`, `tests/e2e/*.ps1` | keep direct Git unless a test specifically validates no-system-git behavior |
| GitHub Actions | `.github/workflows/*.yml` | keep `actions/checkout` and explicit `git` install; this is CI bootstrap, not runtime |
| package recipes | `xim-pkgindex` recipes such as `git-autosync.lua` | handle as package-author dependency policy; not part of xlings binary wrapper |

## Proposed File Layout

Use `compact`, not the old `xself/compat.cppm` location.

Reason: the existing xself compatibility shim is scoped to self-upgrade and cross-version migrations, while `xim/repo.cppm` and `xim/downloader.cppm` are package-manager runtime modules. The source files should be managed under `src/core/compact/`, while each exported module can keep the namespace that matches its callers.

Initial layout:

```text
src/core/compact.cppm
src/core/compact/git.cppm
src/core/compact/xself.cppm
```

Implementation rule:

- `src/core/compact.cppm` is the public compact entry module.
- `src/core/compact/git.cppm` owns the Git CLI/library compatibility boundary.
- `src/core/compact/xself.cppm` owns the existing xself cross-version compatibility shims but keeps `export module xlings.core.xself.compat` to minimize caller churn.
- future compatibility helpers should be placed under `src/core/compact/<name>.cppm`; only genuinely tiny, uncategorized glue should live directly in `compact.cppm`.

## Module Contract

`src/core/compact.cppm` should export `xlings.core.compact`.

Recommended namespace:

```cpp
export module xlings.core.compact;

import std;
import xlings.core.log;
import xlings.core.config;
import xlings.platform;
import xlings.runtime.cancellation;

export namespace xlings::compact::git {
    struct Result {
        int rc = 1;
        std::string output;
    };

    enum class EnsureMode {
        NeverInstall,
        AutoInstall,
    };

    bool available();
    bool ensure_available(EnsureMode mode = EnsureMode::AutoInstall);

    Result capture(const std::vector<std::string>& args,
                   EnsureMode mode = EnsureMode::AutoInstall);

    int exec(const std::vector<std::string>& args,
             EnsureMode mode = EnsureMode::AutoInstall);

    platform::ProcessHandle spawn(const std::vector<std::string>& args,
                                  EnsureMode mode = EnsureMode::AutoInstall);

    Result clone_shallow(const std::string& url,
                         const std::filesystem::path& dest,
                         bool recursive = false);

    Result pull_ff_only(const std::filesystem::path& repo);
    Result set_origin(const std::filesystem::path& repo, const std::string& url);
    Result fetch_reset_origin_head(const std::filesystem::path& repo);
}
```

The implementation should build shell commands from argv-style vectors with `platform::shell_quote()` instead of accepting raw command strings. This keeps quoting consistent and removes most direct string-format Git command construction from callers.

Required comment near the Git wrapper:

```cpp
// Compact compatibility boundary for Git operations.
//
// xlings currently uses the external `git` CLI for package-index sync and
// `.git` source downloads. Keep all CLI invocation behind this module so
// missing-tool bootstrap, quoting, logging, and retry behavior stay uniform.
//
// Future direction: replace this CLI-backed implementation with an embedded
// Git library once clone, pull, recursive submodule, auth, proxy, and mirror
// fallback behavior are covered.
```

## Git Auto-Install Behavior

`ensure_available(AutoInstall)` should follow this sequence:

1. Run `git --version` through the wrapper command path.
2. If it succeeds, return true.
3. If `XLINGS_NO_AUTO_INSTALL_GIT=1`, return false with a clear log message.
4. If `XLINGS_COMPACT_GIT_BOOTSTRAP=1`, return false to avoid recursion.
5. Invoke the current xlings executable, not a random `xlings` from PATH:

   ```text
   "<current-executable>" install xim:git -y
   ```

6. Set `XLINGS_COMPACT_GIT_BOOTSTRAP=1` for that child process.
7. Preserve the current `XLINGS_HOME` so the install targets the active isolated/user home.
8. After install, retry `git --version`.
9. If retry still fails, return false and include the manual command:

   ```text
   xlings install xim:git -y
   ```

Important constraints:

- Do not auto-install Git while the package target being installed is `xim:git`.
- Do not try to sync remote index repos just to install Git. The bootstrap can only work when a bundled/local package index is already available.
- If the package index is unavailable and Git is unavailable, fail with a direct explanation instead of looping.
- The first implementation should use exactly `xlings install xim:git -y`. If validation shows the installed Git is not visible in the current subos, then change the install scope deliberately rather than silently adding `-g`.

## Caller Migration Plan

### Task 1: Add compact module

Files:

- Create: `src/core/compact.cppm`
- Create: `src/core/compact/git.cppm`
- Move: `src/core/xself/compat.cppm` to `src/core/compact/xself.cppm`
- Modify: `src/core.cppm`

Steps:

1. Add `export module xlings.core.compact`.
2. Export `xlings::compact::git` with `available`, `ensure_available`, `capture`, `exec`, and `spawn`.
3. Implement argv-to-shell command construction with `platform::shell_quote`.
4. Implement recursion guard and `XLINGS_NO_AUTO_INSTALL_GIT`.
5. Re-export `xlings.core.compact` from `src/core.cppm`.

Expected build impact:

- no xmake changes are required because `xmake.lua` already includes `src/**.cppm`;
- module dependency order should be inferred by C++20 module imports.

### Task 2: Replace package-index Git calls

Files:

- Modify: `src/core/xim/repo.cppm`

Steps:

1. Import `xlings.core.compact`.
2. Delete `detail_::git_available_()`.
3. Replace the initial preflight with `compact::git::ensure_available()`.
4. Replace clone with `compact::git::clone_shallow(url, tmpDir, false)`.
5. Replace remote repair with `compact::git::set_origin(localDir, url)`.
6. Replace pull with `compact::git::pull_ff_only(localDir)`.
7. Replace fetch/reset fallback with `compact::git::fetch_reset_origin_head(localDir)`.

Behavior to preserve:

- mirror fallback order;
- temporary clone directory cleanup;
- sync stamp throttle;
- `pull --ff-only` before hard reset fallback;
- non-fatal remote set-url failure.

### Task 3: Replace `.git` downloader Git calls

Files:

- Modify: `src/core/xim/downloader.cppm`

Steps:

1. Import `xlings.core.compact`.
2. Replace existing checkout update with `compact::git::pull_ff_only(destDir)`.
3. Replace non-cancellable clone with `compact::git::clone_shallow(url, destDir, true)`.
4. Replace cancellable clone command construction with `compact::git::spawn({"clone", "--depth", "1", "--recursive", "--quiet", url, destDir.string()})`.
5. Keep the existing mirror fallback and partial clone cleanup logic.

Behavior to preserve:

- existing checkout pull before reclone;
- recursive clone for package sources;
- cancellation and pause handling;
- per-URL fallback cleanup.

### Task 4: Add tests for the wrapper boundary

Files:

- Modify or create under `tests/unit/`

Recommended unit coverage:

- command builder quotes paths with spaces;
- `available()` returns false for a missing Git command;
- `ensure_available(NeverInstall)` does not invoke bootstrap;
- recursion guard blocks nested bootstrap when `XLINGS_COMPACT_GIT_BOOTSTRAP=1`;
- `XLINGS_NO_AUTO_INSTALL_GIT=1` disables bootstrap.

If direct command injection is needed for deterministic tests, add a private test-only environment override:

```text
XLINGS_COMPACT_GIT_BIN=/path/to/fake-git
```

Keep it undocumented for users unless it proves useful outside tests.

### Task 5: Add no-system-git e2e coverage

Files:

- Create or modify: `tests/e2e/*git_bootstrap*`

Scenarios:

1. Isolated home with bundled/local index and no system Git visible:
   - run `xlings update`;
   - expect xlings to attempt `xlings install xim:git -y`;
   - expect no raw `git is required` failure.

2. Isolated home without usable index and no system Git visible:
   - run `xlings update`;
   - expect a clear failure explaining that Git cannot be bootstrapped without a local index.

3. `.git` package source with no system Git visible:
   - install a fixture package whose URL ends with `.git`;
   - expect Git bootstrap before clone.

4. Bootstrap disabled:
   - set `XLINGS_NO_AUTO_INSTALL_GIT=1`;
   - expect failure with the manual command hint.

The test environment must not mutate the developer's real `~/.xlings`.

Use this pattern:

```bash
XLINGS_TEST_HOME="$(mktemp -d /tmp/xlings-compact-git-home.XXXXXX)"
export XLINGS_HOME="$XLINGS_TEST_HOME"
```

If mcpp is used for local validation, isolate it too:

```bash
MCPP_TEST_HOME="$(mktemp -d /tmp/mcpp-compact-git-home.XXXXXX)"
export MCPP_HOME="$MCPP_TEST_HOME"
```

### Task 6: Keep scripts and CI separate

No runtime wrapper should be used by:

- `.github/workflows/*.yml`
- `tools/package_xim_index.sh`
- `tools/package_xim_index.ps1`
- fixture-preparation scripts that intentionally require Git

Those paths prepare build/test inputs and can continue to require host Git.

## Local Verification Requirements

Do not run validation against the system `~/.xlings`.

Minimum validation environment:

```bash
export XLINGS_HOME="$(mktemp -d /tmp/xlings-compact-git-home.XXXXXX)"
export MCPP_HOME="$(mktemp -d /tmp/mcpp-compact-git-home.XXXXXX)"
```

For xlings C++ module builds:

```bash
xmake f -m debug
xmake build xlings
xmake build xlings_tests
xmake run xlings_tests
```

For local mcpp-based validation, use GCC 16 or LLVM only, and keep the isolated homes above:

```bash
# Example only; choose the locally available compiler package/toolchain.
export XLINGS_HOME="$(mktemp -d /tmp/xlings-mcpp-compact-git.XXXXXX)"
export MCPP_HOME="$(mktemp -d /tmp/mcpp-compact-git.XXXXXX)"

# Use gcc 16 or LLVM for mcpp validation. Do not let mcpp/xlings reuse
# ~/.xlings or ~/.mcpp from the developer machine.
mcpp --version
mcpp build
```

Before and after validation, check that the real home was not touched:

```bash
test -z "$XLINGS_HOME" && false
test "$XLINGS_HOME" != "$HOME/.xlings"
```

## Risk Assessment

| Risk | Mitigation |
| --- | --- |
| recursive `xlings install xim:git -y` loop | `XLINGS_COMPACT_GIT_BOOTSTRAP=1` guard and target check for `xim:git` |
| no bundled/local index | clear non-recoverable error; do not try remote sync without Git |
| installed Git not visible to current process | retry detection after install; if needed, prepend `Config::paths().binDir` for child Git commands |
| Windows package config side effects | validate `xim:git` Windows behavior; if needed, add package-level quiet/bootstrap mode later |
| hidden dependency on `tar` for Linux `xim:git` | document in failure message if bootstrap install fails |
| quote/injection regressions | vector args plus `platform::shell_quote()` in one place |
| future Git library migration blocked by CLI-specific callers | keep callers on `compact::git::*`, not raw command strings |

## Review Decisions Needed

1. Should automatic Git install use exactly `xlings install xim:git -y`, or should xlings introduce a tool/bootstrap scope for runtime dependencies?
2. Should bootstrap be enabled by default for `xlings update`, `.git` source downloads, or both?
3. Should tests use a private `XLINGS_COMPACT_GIT_BIN` override to simulate missing Git deterministically?

## Recommended First PR Scope

Do in one PR:

- create `src/core/compact.cppm`;
- create `src/core/compact/git.cppm`;
- move existing xself compat source into `src/core/compact/xself.cppm`;
- add `compact::git` wrapper with bootstrap guard;
- migrate `repo.cppm`;
- migrate `downloader.cppm`;
- add unit tests for command construction and bootstrap guards;
- add one Linux e2e covering `xlings update` without host Git in an isolated `XLINGS_HOME`.

Defer:

- release script migration;
- package recipe migration;
- replacing Git CLI with an embedded Git library;
- changing `xim:git` package behavior beyond what bootstrap requires.
