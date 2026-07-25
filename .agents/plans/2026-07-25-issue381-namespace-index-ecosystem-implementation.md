# Issue #381 Namespace Index Ecosystem Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship namespace-aware same-repo package identity through libxpkg, mcpp-index/resource mirrors, xlings 0.4.69, release infrastructure, and a real latest-version ecosystem smoke test.

**Architecture:** libxpkg owns descriptor identity and indexes every entry by canonical `namespace:name`, while preserving the descriptor's original `package.name` as the short-name lookup token. xlings passes each repo's default namespace into libxpkg, persists identity metadata in cache v2, and lets each repo yield multiple catalog candidates so explicit namespace resolves exactly and unqualified ambiguous names fail visibly. Releases proceed in dependency order and are verified at their public GitHub/GitCode boundaries before downstream consumption.

**Tech Stack:** C++23 modules (`mcpp`, `import std`), Lua xpkg descriptors, Bash E2E, GitHub Actions/`gh`, GitCode `gtc`, xlings/xpkg resource publishing.

## Global Constraints

- Implement `.agents/docs/2026-07-25-issue381-namespace-index-identity-design.md` without reducing its identity, cache, ambiguity, or test scope.
- Preserve the descriptor's original `package.name`; do not strip a `<namespace>.` prefix used by existing mcpp-index packages.
- `effectiveNamespace = package.namespace` when non-empty, otherwise the index repo default namespace supplied by xlings.
- Different namespaces with the same name coexist; duplicate effective `(namespace, name)` identities in one index make `build_index` fail with both paths.
- Bare dependencies keep current global catalog semantics and do not inherit the declaring package namespace.
- Cache v2 rejects v1 and rejects a cache whose `default_namespace` differs from the current repo spec.
- Use `xlings install` for tool installation and `xlings use gcc@16.1.0` for xlings development.
- Every E2E uses an isolated temporary `XLINGS_HOME`.
- Preserve visible git history; use additive commits and normal pushes only.
- PRs merge with `--squash --admin` after required CI passes; no force push, amend, or rebase.
- Target versions: libxpkg `0.0.46`, xlings `0.4.69`, unless a live remote audit shows either version already exists before its release step.

---

### Task 1: Prepare isolated worktrees and baselines

**Files:**
- No source changes
- Worktrees: repo-local `.worktrees/fix-issue381-namespace-identity`

**Interfaces:**
- Consumes: `origin/main` for `openxlings/libxpkg`, `mcpplibs/mcpp-index`, `openxlings/xlings`, and `openxlings/xim-pkgindex`
- Produces: clean feature branches and baseline test evidence

- [ ] **Step 1: Verify `.worktrees` is ignored in each repository**

Run:

```bash
git check-ignore -q .worktrees
```

If absent, add only `/.worktrees/` to that repository's `.gitignore` and commit `chore: ignore local worktrees`.

- [ ] **Step 2: Create feature worktrees from current `origin/main`**

```bash
git worktree add .worktrees/fix-issue381-namespace-identity \
  -b fix/issue381-namespace-identity origin/main
```

Use separate repository roots; never reuse a worktree directory across repositories.

- [ ] **Step 3: Baseline libxpkg**

Run from the libxpkg worktree:

```bash
xlings install
mcpp build
mcpp test
```

Expected: build succeeds and all current libxpkg tests pass before changes.

- [ ] **Step 4: Baseline xlings**

Run from the xlings worktree:

```bash
xlings install
xlings use gcc@16.1.0
mcpp build
mcpp test
```

Expected: build succeeds and all current xlings unit tests pass before changes.

---

### Task 2: Add failing libxpkg identity tests

**Files:**
- Modify: `openxlings/libxpkg/tests/test_loader.cpp`
- Modify: `openxlings/libxpkg/tests/test_index.cpp`
- Create: `openxlings/libxpkg/tests/fixtures/pkgindex-namespaces/pkgs/a/alpha.demo.lua`
- Create: `openxlings/libxpkg/tests/fixtures/pkgindex-namespaces/pkgs/b/beta.demo.lua`
- Create: `openxlings/libxpkg/tests/fixtures/pkgindex-duplicate/pkgs/a/implicit.demo.lua`
- Create: `openxlings/libxpkg/tests/fixtures/pkgindex-duplicate/pkgs/b/explicit.demo.lua`

**Interfaces:**
- Consumes: existing `build_index`, `search`, `resolve`, `match_version`
- Produces: executable requirements for canonical entries, short-name candidates, duplicate rejection, aliases, and namespace-scoped versions

- [ ] **Step 1: Add same-name/different-namespace fixtures**

Use literal package definitions with `alpha/demo` and `beta/demo`, distinct descriptions, and no network hooks.

- [ ] **Step 2: Add loader tests**

Tests must assert:

```cpp
auto index = build_index(fixturePath, "repo-default");
ASSERT_TRUE(index);
EXPECT_TRUE(index->entries.contains("alpha:demo"));
EXPECT_TRUE(index->entries.contains("beta:demo"));
EXPECT_EQ(index->short_names.at("demo"),
          (std::vector<std::string> { "alpha:demo", "beta:demo" }));
```

Add a duplicate test where an empty namespace under default `alpha` conflicts with explicit `alpha`; assert `build_index` returns `unexpected` containing both fixture paths.

- [ ] **Step 3: Add index API tests**

Assert:

- `find_candidates(index, "demo", std::nullopt)` returns both canonical identities;
- `find_candidates(index, "demo", "alpha")` returns only `alpha:demo`;
- search returns both canonical entries in sorted order;
- an unqualified alias inherits its own namespace;
- version matching never crosses from `alpha:demo` to `beta:demo`.

- [ ] **Step 4: Verify RED**

Run:

```bash
mcpp test
```

Expected: compilation/test failure because the new identity fields/APIs do not yet exist, or behavior failure because one descriptor is overwritten.

---

### Task 3: Implement libxpkg namespace identity

**Files:**
- Modify: `openxlings/libxpkg/src/xpkg.cppm`
- Modify: `openxlings/libxpkg/src/xpkg-loader.cppm`
- Modify: `openxlings/libxpkg/src/xpkg-index.cppm`
- Modify: `openxlings/libxpkg/mcpp.toml`

**Interfaces:**
- Produces:
  - `PackageIdentity { namespaceName, name, canonical_name() }`
  - `IndexEntry { identity, canonicalName, entryKey, ... }`
  - `PackageIndex::{entries, identity_entries, short_names, mutex_groups}`
  - `find_candidates(index, name, optional namespace)`
  - namespace-scoped `resolve`/`match_version`

- [ ] **Step 1: Add the model**

Follow mcpp naming style: PascalCase types, camelCase fields, snake_case functions, private members with `_`. Keep out-of-line destructors for module/GCC stability.

- [ ] **Step 2: Implement deterministic build**

Collect descriptor paths, sort them, load each package, derive effective namespace and canonical identity, and reject a duplicate before insertion:

```cpp
auto [it, inserted] = index.entries.emplace(entry.entryKey, entry);
if (!inserted) {
    return std::unexpected(std::format(
        "duplicate package identity '{}': '{}' conflicts with '{}'",
        entry.canonicalName, it->second.path.string(), entry.path.string()));
}
```

Populate and sort `identity_entries` and `short_names`.

- [ ] **Step 3: Implement identity-aware lookup**

Explicit namespace lookup is O(1); bare lookup uses `short_names`. Alias resolution occurs after candidate selection and inherits its candidate namespace unless the ref is explicitly namespaced.

- [ ] **Step 4: Preserve existing empty-namespace APIs**

Existing tests using `vscode`, `python@3.12.0`, merge, mutex groups, and installed flags must remain valid with empty namespaces.

- [ ] **Step 5: Bump version**

Set `mcpp.toml` package version to `0.0.46`.

- [ ] **Step 6: Verify GREEN**

```bash
mcpp build
mcpp test
```

Expected: all existing and new tests pass.

- [ ] **Step 7: Commit**

```bash
git add mcpp.toml src tests
git commit -m "fix(index): preserve namespace package identity (0.0.46)"
```

---

### Task 4: Publish libxpkg 0.0.46

**Files:**
- GitHub PR/release state only

**Interfaces:**
- Produces: merged libxpkg main commit, tags `v0.0.46` and/or `0.0.46` as required by the live release workflow, public source archive with a verified SHA256

- [ ] **Step 1: Push and create PR**

Create a PR titled `fix(index): preserve namespace package identity (0.0.46)` referencing `openxlings/xlings#381`.

- [ ] **Step 2: Wait for all libxpkg CI checks**

Use `gh pr checks --watch`; on failure inspect `gh run view --log-failed`, fix by new additive commit, and re-run local tests.

- [ ] **Step 3: Squash merge with bypass**

```bash
gh pr merge <number> --squash --delete-branch --admin
```

- [ ] **Step 4: Trigger/perform release**

Use the repository's checked-in release workflow. Do not invent tag spelling; inspect the workflow and existing `0.0.45` release first.

- [ ] **Step 5: Verify release**

Verify tag commit ancestry, GitHub release state, source archive readability, `mcpp.toml` version, and SHA256.

---

### Task 5: Update mcpp-index and libxpkg mirrors

**Files:**
- Modify: `mcpplibs/mcpp-index/pkgs/x/xpkg.lua`
- Modify only if the live contract requires it: `mcpplibs/mcpp-index/tests/check_package_name.lua`
- Resource state: `github.com/mcpp-res/xpkg` or current configured resource owner, plus GitCode `mcpp-res/xpkg`

**Interfaces:**
- Consumes: verified libxpkg 0.0.46 source archive and SHA256
- Produces: mcpp-index recipe for Linux/macOS/Windows and byte-identical GLOBAL/CN resources

- [ ] **Step 1: Add 0.0.46 to all platform matrices**

Use the public release URL/tag actually produced in Task 4 and one verified SHA256 for the identical source archive.

- [ ] **Step 2: Run mcpp-index validation**

Run repository-prescribed Lua/static/index tests and an isolated `mcpp` dependency resolution smoke for `mcpplibs:xpkg@0.0.46`.

- [ ] **Step 3: Publish missing CN resource**

If automation has not published GitCode, use the repository-provided `gtc` path with explicit target `mcpp-res/xpkg`, then verify with a ranged GET and compare SHA256 to GLOBAL.

- [ ] **Step 4: PR, CI, squash merge**

Create a versioned PR, wait for all required checks, merge with `--squash --admin`, and verify `origin/main`.

- [ ] **Step 5: Verify mcpp-index artifact**

Confirm the post-merge artifact/pointer workflow contains the new commit and both public source URLs remain downloadable.

---

### Task 6: Add failing xlings unit and E2E tests

**Files:**
- Modify: `openxlings/xlings/tests/unit/test_main.cpp`
- Create: `openxlings/xlings/tests/e2e/index_same_name_namespace_test.sh`
- Modify: `openxlings/xlings/tests/e2e/index_cache_test.sh`
- Create fixtures under: `openxlings/xlings/tests/fixtures/index-same-name/`

**Interfaces:**
- Consumes: libxpkg 0.0.46 identity API
- Produces: observable requirements for explicit lookup, bare ambiguity, search, dependency resolution, duplicate rejection, cache v1 invalidation, cache-context invalidation

- [ ] **Step 1: Upgrade only the test dependency edge**

Set xlings `mcpp.toml` xpkg dependency to `0.0.46`, install from the updated mcpp-index, and keep production source unchanged.

- [ ] **Step 2: Add unit tests**

Test `IndexManager` and `PackageCatalog` with one repo containing `alpha:demo` and `beta:demo`. Assert explicit success, bare ambiguity, deterministic candidate order, search completeness, project/global precedence, and bare dependency non-inheritance.

- [ ] **Step 3: Add cache tests**

Assert cache version 2, both entries, cache hit without rewrite, v1 rejection, corrupt identity rejection, and same HEAD/different default namespace rejection.

- [ ] **Step 4: Add isolated E2E**

Run `search`, `info alpha:demo`, `info beta:demo`, bare `info demo`, and interface `plan_install` without downloading package payloads.

- [ ] **Step 5: Verify RED**

```bash
mcpp test
XLINGS_BIN=$(find target -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/index_same_name_namespace_test.sh
```

Expected: production xlings cannot yet produce the required candidate behavior/cache.

---

### Task 7: Implement xlings identity-aware index, cache, and catalog

**Files:**
- Modify: `openxlings/xlings/src/core/xim/index.cppm`
- Modify: `openxlings/xlings/src/core/xim/catalog.cppm`
- Modify as required by compile-time consumers:
  - `src/core/xim/resolver.cppm`
  - `src/core/xim/installer.cppm`
  - `src/core/xim/commands.cppm`
- Modify: `openxlings/xlings/src/core/config.cppm`
- Modify: `openxlings/xlings/mcpp.toml`
- Add approved design and this plan under `.agents/docs` / `.agents/plans`

**Interfaces:**
- Produces: cache v2, default-namespace-aware `IndexManager`, multi-match catalog path, canonical entry-key install state

- [ ] **Step 1: Pass repo default namespace**

`PackageCatalog::make_state_` configures each `IndexManager` with `RepoIndexSpec::defaultNamespace`; `rebuild()` calls libxpkg with it.

- [ ] **Step 2: Implement cache v2**

Persist `default_namespace`, identity, canonical name, entry key, version and path. Rebuild `identity_entries` and `short_names` on load. Any v1/context/identity mismatch is a cache miss.

- [ ] **Step 3: Convert single match to candidate vector**

Replace `build_match_` with `build_matches_`, run alias/version resolution per canonical candidate, and retain current project/global/primary/sub-index precedence.

- [ ] **Step 4: Update all entry-key consumers**

Use canonical/entry keys for load, installed state, entry paths, install/uninstall, and recursive dependencies. Do not add same-namespace inheritance for bare dependencies.

- [ ] **Step 5: Bump xlings**

Set xpkg dependency to `0.0.46`, `mcpp.toml` package version and `src/core/config.cppm` runtime version to `0.4.69`.

- [ ] **Step 6: Verify GREEN**

```bash
mcpp build
mcpp test
XLINGS_BIN=$(find target -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/index_same_name_namespace_test.sh
XLINGS_BIN=$(find target -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/index_cache_test.sh
```

- [ ] **Step 7: Run existing E2E regression set**

Run every repository-required E2E suite, especially multi-repo, project, subos-xpkg, interface, install/remove, and index artifact tests.

- [ ] **Step 8: Commit**

```bash
git add .agents mcpp.toml src tests
git commit -m "fix(xim): preserve namespace index identity (0.4.69)"
```

---

### Task 8: PR, CI, squash-bypass merge xlings 0.4.69

**Files:**
- GitHub PR state only

- [ ] **Step 1: Push and create versioned PR**

Title: `fix(xim): preserve namespace index identity (0.4.69)`. Body includes Issue #381, libxpkg 0.0.46, cache v2 migration, test commands, and release intent.

- [ ] **Step 2: Wait for all required CI**

Linux, macOS, Windows and relevant E2E checks must pass. Fix failures with additive commits only.

- [ ] **Step 3: Squash merge with bypass**

```bash
gh pr merge <number> --squash --delete-branch --admin
```

- [ ] **Step 4: Verify merge**

Confirm the squash commit is on `origin/main`, contains version `0.4.69`, closes #381, and all required checks belong to the merged head.

---

### Task 9: Release xlings 0.4.69 and publish resources

**Files:**
- GitHub Actions/release state
- `openxlings/xim-pkgindex` xlings descriptor if automation does not update it
- GitHub/GitCode `xlings-res` release assets

- [ ] **Step 1: Trigger release workflow**

Use the checked-in `release.yml` on merged main and monitor every platform/package job to terminal success.

- [ ] **Step 2: Verify GitHub release**

Check all expected Linux/macOS/Windows assets, sidecars/checksums, version output, archive readability, and commit/tag ancestry.

- [ ] **Step 3: Verify xlings-res publication**

Inspect the post-release ecosystem workflow. If GitCode assets are absent or incomplete, follow the checked-in mirror-latest runbook and use local `gtc` only for missing assets.

- [ ] **Step 4: Update xim-pkgindex if required**

If automation did not create/merge the `xlings 0.4.69` bump, update the descriptor through its normal PR/CI/squash path. Verify GLOBAL/CN URLs and hashes.

- [ ] **Step 5: Verify public install metadata**

Fresh index/artifact pointers must resolve xlings 0.4.69 as latest without relying on a local checkout.

---

### Task 10: Real latest-ecosystem validation and completion audit

**Files:**
- Create/update: `openxlings/xlings/.agents/docs/2026-07-25-issue381-namespace-index-identity-validation.md`

- [ ] **Step 1: Bootstrap latest release in isolated home**

Use a temporary `XLINGS_HOME` and the public quick-install/release path. Confirm `xlings --version` is `0.4.69`.

- [ ] **Step 2: Validate package ecosystem**

From public indexes/resources:

- search/install/use/remove a normal unique package;
- install an mcpp workspace consuming `mcpplibs:xpkg@0.0.46`;
- load one GLOBAL resource and one CN/GitCode resource;
- run the Issue #381 same-repo fixture and verify both explicit identities plus bare ambiguity;
- verify cache v2 cold build and warm reload;
- run `xlings interface plan_install` and confirm canonical identities;
- verify `xlings update` and latest index artifact pointers.

- [ ] **Step 3: Record evidence**

Document exact release URLs, PRs, merge commits, workflow run IDs, asset hashes, commands, outputs, and any intentionally skipped platform-local checks. Do not infer three-platform runtime behavior from Linux; use CI artifacts/checks as the evidence for macOS/Windows.

- [ ] **Step 4: Audit every original requirement**

Mark each dependency release, index/resource update, xlings implementation, tests, PR, CI, squash bypass merge, release, mirror, and real smoke as proven or incomplete. Do not declare completion with any missing external asset or non-terminal workflow.
