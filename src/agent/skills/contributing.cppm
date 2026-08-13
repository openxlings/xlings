// Skill: contributing — xlings project development workflow for AI agents.
// Written as system-prompt-style instructions to enforce correct dev flow.

export module xlings.agent.skills.contributing;

import std;
import xlings.agent.skill;

namespace xlings::agent::skills {

export class ContributingSkill : public Skill {
public:
    auto name() const -> std::string_view override;
    auto description() const -> std::string_view override;
    auto content() const -> std::string_view override;

private:
    static constexpr std::string_view kContent = R"SKILL([SYSTEM INSTRUCTION — xlings development workflow for AI agents]

You are contributing code to the xlings project. Follow these rules
STRICTLY. Violations (e.g. pushing directly to main, releasing
without CI) cause real damage to users.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
ABSOLUTE RULES — NEVER violate these
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. NEVER push directly to main. ALL changes go through a PR.
2. NEVER trigger a release without CI passing on the PR first.
3. NEVER amend or force-push commits that others may have pulled.
4. ALWAYS create a feature branch from the latest main.
5. ALWAYS run `mcpp build` and `mcpp test` before pushing.
6. ALWAYS use `xlings install` for tools, never apt/brew/curl.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
DEVELOPMENT FLOW — follow this sequence exactly
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

STEP 1: Create a branch from main
  git fetch origin main
  git switch -c <type>/<short-name> origin/main

  Branch naming:
    feat/xxx    — new feature
    fix/xxx     — bug fix
    chore/xxx   — maintenance, version bump
    docs/xxx    — documentation only

STEP 2: Implement the change
  - Follow existing code patterns (C++23 modules, import std;)
  - Keep changes minimal and focused — one feature per branch
  - Do NOT add comments/docstrings/refactors beyond what's asked

STEP 3: Build and verify locally
  mcpp build
  mcpp test
  <path-to-built-xlings> <relevant-command>    # manual smoke test

STEP 4: Commit
  git add <specific-files>
  git commit -m "<type>(<scope>): <description>"

  Commit message examples:
    feat(agent): add xlings agent subcommand
    fix(xim): resolver handles empty namespace
    chore: bump version to 0.4.38

STEP 5: Push and create PR
  git push -u origin <branch>
  gh pr create --title "<type>(<scope>): <description>" --body "..."

  PR body MUST include:
  - Summary (what changed and why)
  - Test plan (how it was verified)

STEP 6: Wait for CI
  gh pr checks <pr-number>

  ALL platforms must pass: Linux + macOS + Windows.
  If CI fails, fix the issue and push again. Do NOT bypass.

STEP 7: Merge via PR
  gh pr merge <number> --squash --delete-branch --admin

  NEVER merge a PR that hasn't passed CI.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
VERSION BUMP + RELEASE — separate from feature work
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Releasing a new version is a MULTI-STEP process. Do NOT skip steps.

VERSION NUMBERING — date-based, YYYY.M.D.N (e.g. 2026.7.28.1).

  N STARTS AT 1. Do NOT use .0 for an ordinary release.
  .0 is reserved for a formal / milestone release on that date.
  A routine same-day fix or feature release is .1, the next .2, and
  so on. If you are unsure whether it is formal: it is not. Use .1.

    2026.7.28.1   first release of the day (the normal case)
    2026.7.28.2   second release of the day
    2026.7.28.0   RESERVED — formal release, only when explicitly asked

  semver::parse rejects a four-component version, so resolution falls
  back to lexicographic order. Always publish and reference these
  through an explicit `latest` ref, never by version comparison.

STEP 1: AFTER all feature PRs are merged to main, create a
        version-bump PR (yes, a PR — not a direct push):
  git switch -c chore/bump-2026.7.28.1 origin/main
  # Edit src/core/config.cppm: VERSION = "2026.7.28.1"
  git add src/core/config.cppm
  git commit -m "chore: bump version to 2026.7.28.1"
  git push -u origin chore/bump-2026.7.28.1
  gh pr create --title "chore: bump version to 2026.7.28.1"

STEP 2: Wait for CI to pass on the bump PR.

STEP 3: Merge the bump PR.
  gh pr merge <number> --squash --delete-branch --admin

STEP 4: ONLY THEN trigger the release workflow:
  gh workflow run release.yml --ref main

STEP 5: Monitor the release:
  gh run list --workflow=release.yml --limit 1
  gh run watch <run-id>

DO NOT combine version bump with feature changes.
DO NOT trigger release.yml before the bump PR is merged.
DO NOT push version bumps directly to main.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
BUILD ENVIRONMENT
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Setup (from repo root):
  xlings install              # installs mcpp
  xlings use gcc@16.1.0       # dev build toolchain (Linux)

Build:
  mcpp build                   # dev binary

Test:
  mcpp test                    # unit tests

Release scripts (CI uses these):
  Linux:   tools/linux_release.sh    (musl-gcc static binary)
  macOS:   tools/macos_release.sh    (LLVM)
  Windows: tools/windows_release.ps1 (MSVC)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
CODE CONVENTIONS
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

- C++23 modules: every .cppm file starts with `export module ...`
- Use `import std;` not `#include <...>` for standard library
- Commit convention: <type>(<scope>): <description>
- PRs are squash-merged to main (one clean commit per feature)
- Test isolation: e2e tests use temp XLINGS_HOME, never real env
- No unnecessary changes: don't refactor code beyond what's asked

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
COMMON MISTAKES TO AVOID
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

- Pushing to main directly → ALWAYS use a PR
- Creating branch from wrong base → ALWAYS branch from origin/main
- Bumping version + feature in same commit → SEPARATE them
- Triggering release before CI passes → WAIT for green CI
- Using apt/brew to install tools → USE `xlings install`
- Force-pushing to shared branches → DON'T

[END OF INSTRUCTION])SKILL";
};

}  // namespace xlings::agent::skills
