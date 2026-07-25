# XVM Provider-Scoped Binding Group Implementation Plan

> **For agentic workers:** use `superpowers:test-driven-development` for each
> behavior slice, `superpowers:subagent-driven-development` for independent
> repository slices, and `superpowers:verification-before-completion` before
> any completion claim.

**Goal:** Make `xlings use` switch a validated binding group from any package,
program, or library member; make uninstall exact and provider-aware; preserve
legacy compatibility; release xlings 0.4.70 with all platform CI green.

**Architecture:** `.agents/docs/2026-07-26-xvm-provider-binding-group-design.md`

**Repositories:** `openxlings/xlings`, `openxlings/libxpkg`,
`openxlings/xim-pkgindex`

## Global constraints

- Never run install/remove/use against the host xlings home.
- Every mutable CLI/E2E invocation must set a temporary `HOME`,
  `XLINGS_HOME`, and neutral working directory.
- Build only through mcpp with GCC 16 on Linux.
- Preserve existing user worktrees and dirty files.
- Write tests before implementation for every behavior slice.
- Keep normal additive git history; no amend/rebase/force-push.
- Commit convention: `<type>(<scope>): <description>`.
- Target versions: xlings `0.4.70`; libxpkg next patch version.

## Task 0: Isolation and baseline

- [x] Create `fix/xvm-binding-groups` worktree from `origin/main`.
- [x] Run `mcpp build`.
- [x] Run `mcpp test`.
- [x] Confirm 11/11 test binaries pass after the required main-binary build.
- [x] Confirm no host `xlings use/install/remove` command was executed.

## Task 1: Production binding-selection model

**Files:**

- Add: `src/core/xvm/bindings.cppm`
- Modify: `src/core/xvm.cppm`
- Modify: `src/core/xvm/types.cppm`
- Modify: `src/core/xvm/db.cppm`
- Test: `tests/unit/test_main.cpp`

**TDD steps:**

- [ ] Add failing group-reference/manifest JSON round-trip and legacy-absence
      tests.
- [ ] Add failing production resolver tests for root/leaf, transformed member
      versions, multiple providers, namespace, and multiple groups.
- [ ] Add failing invalid-graph tests: missing target/version, asymmetric edge,
      conflicting target version, self-edge.
- [ ] Implement `BindingGroupRef`, root member manifests, serialization,
      `BindingSelection`, structured errors, provider-group resolver, and
      strict legacy resolver.
- [ ] Delete the unit test's copied traversal lambda and call production code.
- [ ] Run focused XVM tests, then full `mcpp test`.
- [ ] Commit: `feat(xvm): add provider-scoped binding selection model`

## Task 2: Exact edge-aware removal

**Files:**

- Modify: `src/core/xvm/db.cppm`
- Modify: `src/core/xim/installer.cppm`
- Test: `tests/unit/test_main.cpp`

**TDD steps:**

- [ ] Add failing tests for removing one version from a two-version tree while
      preserving the other version's reciprocal edges.
- [ ] Cover transformed and namespaced member versions.
- [ ] Add failing test proving ambiguous versionless sibling removal cannot
      erase the target.
- [ ] Make `remove_version()` prune both sides and empty maps.
- [ ] Snapshot provider/legacy selections before uninstall mutation.
- [ ] Prefer provider-owned exact members; use validated legacy selection only
      as compatibility fallback.
- [ ] Remove the versionless `db.erase(op.name)` branch.
- [ ] Run focused and full tests.
- [ ] Commit: `fix(xvm): remove binding members by exact owned version`

## Task 3: Batched registration and provider ownership

**Files:**

- Modify: `src/core/xim/installer.cppm`
- Modify: `src/core/xvm/db.cppm`
- Test: `tests/unit/test_main.cpp`

**TDD steps:**

- [ ] Add tests for root-after-child operation order.
- [ ] Add tests rejecting phantom roots, self-binding, duplicate exact nodes,
      cross-provider edges, and two versions of one target in one group.
- [ ] Split add processing into node and binding/ownership passes.
- [ ] Populate provider identity from `PlanNode.canonicalName/version`.
- [ ] Assign stable group labels and retain compatible bidirectional edges.
- [ ] Add every member type to current SubOS `installed[]`.
- [ ] Run focused and full tests.
- [ ] Commit: `feat(xim): register xvm groups as validated provider batches`

## Task 4: Transactional switch and materialized views

**Files:**

- Modify: `src/core/xvm/commands.cppm`
- Modify: `src/core/config.cppm`
- Modify: `src/platform.cppm`
- Modify platform partitions as needed
- Test: `tests/unit/test_main.cpp`

**TDD steps:**

- [ ] Add failing tests proving no workspace mutation on planner/preflight
      errors.
- [ ] Add library and header switch tests for every member in a selection.
- [ ] Add rollback test for an unavailable library/header source.
- [ ] Replace local traversal with `resolve_binding_selection()`.
- [ ] Preflight the complete selection before changes.
- [ ] Stage library/header changes and keep rollback records.
- [ ] Update active/installed entries from one in-memory selection.
- [ ] Atomically replace workspace JSON through a same-directory staged file.
- [ ] Ensure generic program shims only after successful preflight.
- [ ] Run focused and full tests.
- [ ] Commit: `fix(xvm): apply binding group switches transactionally`

## Task 5: Group-aware install/remove fallback

**Files:**

- Modify: `src/core/xim/installer.cppm`
- Modify: `src/core/xvm/commands.cppm`
- Test: `tests/unit/test_main.cpp`

**TDD steps:**

- [ ] Test that installing v2 without `--use` does not repoint a v1 library.
- [ ] Test `--use` activates the complete new group.
- [ ] Test removing a non-active group preserves the active group.
- [ ] Test removing an active group selects one complete surviving group or
      clears all removed members.
- [ ] Route activation/fallback through the common switch plan.
- [ ] Run full tests.
- [ ] Commit: `fix(xim): keep install and remove binding groups coherent`

## Task 6: Integrity diagnostics

**Files:**

- Modify: `src/core/xself/doctor.cppm`
- Test: `tests/e2e/self_doctor_test.sh`

**TDD steps:**

- [ ] Add corrupt owner-group and corrupt legacy-edge fixtures.
- [ ] Assert doctor reports exact target/version pairs and exits nonzero.
- [ ] Assert `--fix` never executes package hooks or removes payloads.
- [ ] Add safe stale workspace/edge repair where unambiguous.
- [ ] Add reinstall/reconfigure hints for unrecoverable registrations.
- [ ] Run doctor E2E in a temporary home.
- [ ] Commit: `feat(xvm): diagnose corrupt binding registrations`

## Task 7: Hermetic binding-tree E2E

**Files:**

- Add: `tests/e2e/binding_tree_use_test.sh`
- Add: `tests/e2e/binding_tree_use_test.ps1`
- Add fixture recipes/files under `tests/e2e/fixtures/`
- Modify: `tests/e2e/run_all.sh`
- Modify macOS/Windows workflows that enumerate E2E tests

**Scenarios:**

- [ ] two-version install;
- [ ] non-package member list;
- [ ] root-to-leaf and leaf-to-root switch;
- [ ] actual execution of every program shim;
- [ ] transformed member version;
- [ ] library/header materialization;
- [ ] library as switch entry point;
- [ ] corrupt graph fails without state changes;
- [ ] non-active and active removal;
- [ ] reinstallation without stale edges;
- [ ] two SubOS instances with independent views.

**Verification:**

- [ ] Linux shell test passes.
- [ ] Windows PowerShell test passes locally where available or through CI.
- [ ] macOS shell path is wired into CI.
- [ ] Commit: `test(xvm): cover bidirectional binding group lifecycle`

## Task 8: libxpkg operation contract

**Worktree:** create a clean libxpkg worktree; do not touch existing dirty
checkouts.

**Files:**

- Modify: `src/lua-stdlib/xim/libxpkg/xvm.lua`
- Modify XVM operation transport if a structured group label is included
- Add/update libxpkg tests
- Bump libxpkg patch version

**TDD steps:**

- [ ] Test add/remove default to the same runtime version.
- [ ] Test `setup/teardown` propagate explicit versions to root/program/lib.
- [ ] Test `remove_all` is the only empty/all-version operation.
- [ ] Test optional group label round trip if shipped in this slice.
- [ ] Build/test all supported libxpkg paths.
- [ ] Commit, push, open PR, and wait for required CI.

## Task 9: xim-pkgindex recipe integrity

**Worktree:** create a clean pkgindex worktree; preserve its current dirty
feature checkout.

**Files:**

- Modify affected recipes and matching snapshots/tests:
  - GCC;
  - musl GCC;
  - aarch64 musl GCC;
  - e2fsprogs;
  - syslinux;
  - openssl;
  - other audited detached members where the intended group is clear.

**TDD steps:**

- [ ] Add static validator/tests for exact binding roots, self-edges, duplicate
      exact registrations, and exact remove versions.
- [ ] Make roots and children use matching exact versions.
- [ ] Remove self-binding and duplicate registration.
- [ ] Include intended sibling executables.
- [ ] Make teardown versions explicit.
- [ ] Run focused and full pkgindex validation.
- [ ] Commit, push, open PR, and wait for required CI.

## Task 10: Isolated real GCC proof

- [ ] Create a temporary OS home and `XLINGS_HOME`.
- [ ] Copy only the built test xlings bootstrap and a local modified index.
- [ ] Hash host global/workspace config before the test.
- [ ] Install GCC 15.1.0 and 16.1.0 in the temporary home.
- [ ] Switch with `gcc`, execute both `gcc` and `g++`.
- [ ] Switch back with `g++`, execute both again.
- [ ] Verify registered library links and exact versions.
- [ ] Remove/reinstall one version and repeat both directions.
- [ ] Hash host global/workspace config after the test and prove equality.
- [ ] Preserve concise logs under `.agents/docs` or PR evidence.

## Task 11: Version, documentation, and xlings PR

- [ ] Update VERSION and `mcpp.toml` to xlings 0.4.70.
- [ ] Update user-facing docs/changelog only where the repository convention
      requires it.
- [ ] Run formatting/style checks.
- [ ] Run `mcpp build` and `mcpp test` from a clean state.
- [ ] Run all relevant shell E2E in isolated homes.
- [ ] Use `superpowers:requesting-code-review`.
- [ ] Address verified findings.
- [ ] Commit: `fix(xvm): switch provider binding groups atomically (0.4.70)`
- [ ] Push normally and open a draft PR.

## Task 12: CI and completion audit

- [ ] Verify Linux required checks.
- [ ] Verify macOS required checks.
- [ ] Verify Windows required checks.
- [ ] Fix failures by reproducing root cause; do not bypass checks.
- [ ] Confirm every cross-repository PR is green.
- [ ] Re-read the user requirements and map each to current evidence.
- [ ] Confirm host xlings config/workspace hashes did not change.
- [ ] Record final PR URLs, commits, versions, tests, runtime proof, and any
      migration action in the comprehensive report.
