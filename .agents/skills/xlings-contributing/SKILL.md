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
xlings install           # reads .xlings.json → installs mcpp

# Switch to the correct dev toolchain
xlings use gcc@16.1.0   # Linux dev build (avoids musl/glibc link conflicts)
```

### 2. Verify build works

```bash
mcpp build
mcpp test
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
└── (unit tests via `mcpp test`)
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
# Build and unit tests
mcpp build
mcpp test

# Run your test
XLINGS_BIN=$(find target -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/<feature>_test.sh

# Run existing tests to check for regressions
for t in tests/e2e/subos_xpkg_*.sh; do
  XLINGS_BIN=$XLINGS_BIN bash "$t" | tail -1
done

### Step 5.1: xpkg / resource changes

涉及 `xpm`、官方资源或 `xim-pkgindex` 的改动必须遵守以下契约：

- `libxpkg` 是解析、compat 和资源归一化的唯一入口；不要在 xlings 中新增第二套 Lua 解析器或 URL 模板展开器。
- 默认来源使用 `xpm.source = "xlings-res"` 或 URL template；版本项仍保持原有 `platform -> version` 模型。
- 官方二进制资源为每个受支持平台/架构提供 SHA256。多架构 hash 缺失时索引生成器应 fail closed。
- 保留并测试旧的 `"XLINGS_RES"`、`res = true`、显式 URL、mirror、`ref` 和旧单 hash 写法。
- 资源表达测试至少覆盖 x86_64/aarch64、根级/平台级 source、显式 URL 覆盖、mirror 和旧客户端兼容 fixture。
- 修改资源缓存、下载或发布链时，除 `mcpp build && mcpp test` 外，使用隔离 `XLINGS_HOME` 验证坏缓存自愈、SHA256 校验和实际 release 资产。
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

### Version numbering

Releases are date-based: `YYYY.M.D.N`, e.g. `2026.7.28.1`.

**`N` starts at `1`. Do NOT use `.0` for an ordinary release.**
`.0` is reserved for a formal/milestone release on that date. A routine
same-day fix or feature release is `.1`, the next `.2`, and so on. When in
doubt, it is not a formal release — use `.1`.

```
2026.7.28.1   ← first release of the day (the normal case)
2026.7.28.2   ← second release of the day
2026.7.28.0   ← reserved: formal release, only when explicitly intended
```

**This has been broken once.** `2026.7.29.0` shipped as an ordinary bugfix
release. The rule was already written here; the release was cut by following
`AGENTS.md`, which did not mention it, and this skill was never opened. The
version is picked at the very start of a release — before anyone goes looking
for process docs — so the rule now also lives in `AGENTS.md`. Keep both in
sync; if you change the scheme, change it in both places.

`2026.7.29.0` was left as-is rather than re-cut: the release was already
published, mirrored, and indexed, and renaming a published version costs more
than the wrong digit does.

Note that `semver::parse` rejects a four-component version, so resolution
falls back to lexicographic ordering — always publish and reference these
through an explicit `latest` ref rather than relying on version comparison.

### Steps

After feature PRs merge, if a release is planned:

```bash
# On main:
# Edit BOTH: mcpp.toml `version` and src/core/config.cppm VERSION.
# (mcpp's target fingerprint includes the package version, so a bump moves
#  the build output to a new target/<triple>/<fp>/bin/xlings — check
#  `./that/binary --version` before concluding anything from a manual test.)
git commit -m "chore(2026.7.28.1): bump version for release"
git push origin main

# Trigger release
gh workflow run release.yml --ref main

# Monitor
gh run list --workflow=release.yml --limit 1
```

**Then two steps that `release.yml` cannot do for you.** A green workflow is
not a finished release:

```bash
# 1. Top up the CN mirror — from a CN machine, with a local gtc.
#    The GitHub runner cannot push large assets to GitCode: the upload stalls
#    on the cross-border OBS wall, the job logs it and moves on green.
bash tools/mirror-latest.sh xlings

#    Verify with GET, never HEAD: GitCode answers 401 to HEAD and
#    302 -> CDN 200 to GET, so a HEAD check reports a healthy asset missing
#    and a curl -I sweep "proves" the opposite of the truth.
curl -sSL -o /dev/null -w '%{http_code}\n' \
  https://gitcode.com/xlings-res/xlings/releases/download/<ver>/xlings-<ver>-linux-x86_64.tar.gz

# 2. Bump xim-pkgindex/pkgs/x/xlings.lua — every platform block AND the
#    ["latest"] ref, each with its sha256.
```

`2026.7.30.1` skipped step 1. CN users got `HTTP 404` for three hours while
the same files sat on GitHub, reachable and unlisted. `2026.7.30.2` added a
cross-region fallback (`Config::all_resource_servers_for_`) so one region's
gap is no longer fatal — the manual mirror is an accelerator again, not a
correctness requirement, but it is still expected every time.

### Do not pin the released version into CI

`tests/fresh-install/` deliberately installs whatever the published `latest`
resolves to. It is the only suite that tests what a first-time user actually
gets: the release artifact, `quick_install`, and the index that has to resolve
it on a cold home. Pinning the version turns it into a test of a snapshot
nobody installs, and it rots silently — one release at a time, with everything
still green.

So a release is **not** followed by a "pin the new version in CI" commit. The
pins that do exist there (`MCPP_OLD` / `MCPP_NEW`, `GCC_*`, `LLVM_*`,
`NINJA_VERSION`) hold the two ends of an *upgrade* still so an assertion can
name an exact expected version — those pin the **packages under test**, which
is the point. The xlings **binary doing the installing** is never one of them.

Enforced, not just written down:

```bash
bash tests/fresh-install/no_xlings_version_pin_check.sh
```

It runs in `xlings-ci-linux.yml` (which has a `pull_request` trigger) rather
than in `xlings-ci-fresh-install.yml` (which deliberately does not) — a rule
enforced only after merge is a rule that gets merged. It catches three shapes:
an xlings-named version variable, an `xlings@<number>` coordinate, and a
`QUICK_INSTALL_URL` pointing at a release tag instead of a branch.

### Recipes: where a subos path may and may not appear

Two axes, and **the correct answer is opposite for each**. `gcc.lua` is the
worked example.

| axis | value belongs in | may it name a subos |
|---|---|---|
| **LINK** (ELF interpreter, rpath) | the payload's own config (gcc's `specs`) | **No** — payload-direct |
| **HEADER** (`--sysroot`) | the xvm registration (`alias`) | **Yes** — that is what a sysroot is |

*Why LINK must not:* one payload is shared by every subos in the home, and a
direct `<install_dir>/bin/gcc` (mcpp, downstream tools) never goes through a
shim at all. A subos path there points the shared copy at one subos and cannot
be repaired by anything — exec-time normalization only rewrites the xvm
record, and `self doctor` never reads payload file contents. The only fix is a
reinstall, which is why the specs stamp carries a schema suffix
(`.specs-rewritten-<ver>-payload.stamp`): it forces one.

*Why HEADER must:* gcc's header search needs an FHS-shaped tree and the subos
*is* that composite view.

For the HEADER axis, write the **portable spelling**, not the install-time
subos:

```lua
local dir = system.subos_sysrootdir()
return (dir:gsub("([/\\])subos([/\\])[^/\\]+", "%1subos%2current", 1))
```

`<home>/subos/current` is the symlink `self init` creates and
`subos use --global` maintains. It needs no placeholder, no capability probe
and no libxpkg change; an old client follows it instead of freezing at install
time, and a current client normalizes it like any other subos path (which is
what keeps `XLINGS_ACTIVE_SUBOS` and project subos correct — a symlink cannot
follow those). `self doctor` keys its baked-path check on this spelling, so a
recipe that writes it carries no standing warning.

## Key conventions

- **No command may block on a prompt, and none may report success having done
  nothing.** Whether a human is at the keyboard is *not* detectable — agents
  and terminal tooling routinely allocate a pty, so `stdin_is_terminal()` is
  true for them. What is detectable is whether the command has a single
  correct outcome, and that is what decides:
  - unambiguous → do it, exit `0`;
  - ambiguous → change nothing, print the candidates and the exact command,
    exit `2`;
  - interactive selection is **opt-in** (`--pick`), never the default path,
    and when it cannot run it says so rather than falling back to a no-op.

  A TTY may decide *presentation* (picker vs panel), never semantics.
  `tests/e2e/non_interactive_contract_test.sh` runs every prompting command
  under `stdin=/dev/null`, `XLINGS_NON_INTERACTIVE=1` **and a pseudo-TTY**,
  each under `timeout`, so a reintroduced prompt fails instead of hanging CI.
- **Build with xlings**: always use `xlings install` + `xlings use gcc@16.1.0` for dev env
- **No manual apt/brew**: use `xlings install <tool>` (dogfood the project)
- **Test isolation**: every e2e test uses a temp `XLINGS_HOME` (never touches real user env)
- **One feature per PR**: keep PRs focused and reviewable
- **Squash merge**: PRs are squash-merged to keep main history clean
