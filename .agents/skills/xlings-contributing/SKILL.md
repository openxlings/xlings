---
name: xlings-contributing
description: xlings 项目贡献规范流程 — 从 issue 到 PR 合入的完整 agent 开发工作流。Use when implementing features, fixing bugs, or contributing code to xlings. Covers environment setup, branching, coding, testing, PR creation, and CI verification.
---

# xlings Contributing Workflow

## Overview

This skill defines the standard contribution flow for AI agents working on the xlings codebase. Follow this process for any code change — feature, bugfix, or refactoring.

## Prerequisites

### 1. Build environment setup

```bash
# Install xlings itself (bootstrap)
curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash

# From repo root — install build dependencies
xlings install           # reads .xlings.json → xmake, cmake, ninja, toolchain

# Switch to the correct dev toolchain
xlings use gcc@16.1.0   # Linux dev build (avoids musl/glibc link conflicts)
```

### 2. Verify build works

```bash
xmake f --local_libxpkg=/path/to/libxpkg -y   # only if modifying upstream libxpkg
xmake build xlings                              # ~15s on warm cache
```

### 3. Repository structure awareness

```
src/
├── main.cpp                    # entry point
├─��� cli.cppm                    # CLI command dispatch
├── core/
│   ├── config.cppm             # 3-layer config (.xlings.json)
│   ├── subos.cppm              # SubOS management (create/use/remove/fork)
│   ├── subos/keeper.cppm       # Auto-keeper primitives
│   ├── xself.cppm              # Self-install/update
│   ├── xim/
│   │   ├── installer.cppm      # Package install orchestration
│   │   ├── resolver.cppm       # DAG dependency resolution
│   │   ├── downloader.cppm     # Parallel download + SHA256
│   │   └── libxpkg/types/      # Per-type handlers (script.cppm, subos.cppm)
│   └── xvm/                    # Version management (shim, db, commands)
├── interface.cppm              # NDJSON programmatic interface
└── platform.cppm               # Cross-platform abstractions
tests/
├── e2e/                        # End-to-end shell tests
│   ├── project_test_lib.sh     # Shared test helpers
│   └── fixtures/               # Test fixture packages
└── (unit tests via xmake build xlings_tests)
```

## Standard Contribution Flow

### Step 1: Issue

- Check existing issues: `gh issue list`
- If no issue exists for your change, create one:
  ```bash
  gh issue create --title "feat/fix: <description>" --body "<details>"
  ```
- Reference the issue number in your PR

### Step 2: Branch

```bash
git fetch origin main
git switch -c <type>/<short-description> origin/main
```

Branch naming: `feat/xxx`, `fix/xxx`, `chore/xxx`, `docs/xxx`

### Step 3: Implement

- Follow existing code patterns (C++23 modules, `import std;`)
- Type-specific dispatch: see `installer.cppm` for `pkgType == N` pattern
- CLI argparse: see `subos.cppm` `run()` function for manual arg parsing pattern
- Keep changes minimal and focused

### Step 4: Write tests

**E2E tests** (preferred for user-facing features):

```bash
# Create test file
touch tests/e2e/<feature>_test.sh
chmod +x tests/e2e/<feature>_test.sh
```

Test template:
```bash
#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_test_lib.sh"

# Setup
RUNTIME_DIR="$ROOT_DIR/tests/e2e/runtime/<test_name>"
HOME_DIR="$RUNTIME_DIR/home"
cleanup() { rm -rf "$RUNTIME_DIR"; }
trap cleanup EXIT
cleanup

mkdir -p "$HOME_DIR/subos/default/bin"
cp "$(find_xlings_bin)" "$HOME_DIR/xlings"
# ... write .xlings.json, set up index, etc.

# Test
log "Testing <feature>..."
run_xlings "$HOME_DIR" "$ROOT_DIR" <command> || fail "<what failed>"

# Assertions
[[ <condition> ]] || fail "<what's wrong>"

log "PASS: <feature> works"
```

Key helpers from `project_test_lib.sh`:
- `find_xlings_bin` — locates the built binary
- `run_xlings "$HOME_DIR" "$ROOT_DIR" <args>` — runs xlings with isolated XLINGS_HOME
- `require_fixture_index` — ensures test pkgindex is available
- `log` / `fail` — logging with consistent prefix

### Step 5: Build + test locally

```bash
# Build
xmake build xlings

# Run your test
XLINGS_BIN=$(find build -path '*/release/xlings' -type f | head -1) \
  bash tests/e2e/<feature>_test.sh

# Run existing tests to check for regressions
for t in tests/e2e/subos_xpkg_*.sh; do
  XLINGS_BIN=$XLINGS_BIN bash "$t" | tail -1
done
```

### Step 6: Commit

```bash
git add <files>
git commit -m "<type>(<scope>): <short description>

<optional body explaining why>

Refs: #<issue-number>"
```

Commit message convention:
- `feat(subos): add --from flag for fork`
- `fix(xim): resolver handles empty namespace`
- `chore(0.4.37): bump version for release`
- `docs: update README`
- `test(subos): cover --cmd exit code propagation`

### Step 7: Push + PR

```bash
git push -u origin <branch>
gh pr create --draft --title "<type>(<scope>): <description>" --body "..."
```

PR body should include:
- **Summary** (what + why)
- **Test plan** (which tests cover this)
- Link to issue (`Closes #N` or `Refs #N`)

### Step 8: CI verification

```bash
# Check CI status
gh pr checks <pr-number>

# If failing, read logs:
gh run view <run-id> --log-failed | tail -50
```

CI runs on 3 platforms (Linux + macOS + Windows). All must pass.

Common CI failures:
- **Link error with musl**: CI uses musl-gcc for static binary. Ensure new code doesn't introduce glibc-only symbols.
- **Windows compile error**: Check `#if defined(_WIN32)` guards for POSIX-only code.
- **Test timeout**: E2E tests have implicit timeouts; ensure no hanging processes.

### Step 9: Review + merge

- Mark PR as "Ready for review" when CI passes
- For admin-privilege merge (if branch protection requires review):
  ```bash
  gh pr merge <number> --squash --delete-branch --admin
  ```

## Version bumping (release flow)

After feature PRs merge, if a release is planned:

```bash
# On main:
# Edit src/core/config.cppm VERSION
git commit -m "chore(0.4.XX): bump version for release"
git push origin main

# Trigger release
gh workflow run release.yml --ref main

# Monitor
gh run list --workflow=release.yml --limit 1
```

## Key conventions

- **Build with xlings**: always use `xlings install` + `xlings use gcc@16.1.0` for dev env
- **No manual apt/brew**: use `xlings install <tool>` (dogfood the project)
- **Test isolation**: every e2e test uses a temp `XLINGS_HOME` (never touches real user env)
- **One feature per PR**: keep PRs focused and reviewable
- **Squash merge**: PRs are squash-merged to keep main history clean
