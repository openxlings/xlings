# Multiplatform User-Experience Contracts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement every P0/P1/P2 optimization in the 2026-08-03 multiplatform UX audit in one xlings pull request and make every required PR check pass.

**Architecture:** Five shared contracts replace local inference: `InventorySnapshot` owns exact installed state, `PackageCompatibility` rejects unsupported targets before planning downloads, `CommandSpec` owns syntax/help/reference data, `RenderPolicy` owns interactive versus static output, and candidate-artifact jobs gate release publication. Existing libxpkg parsing, EventStream, platform abstractions and release scripts remain the execution boundaries.

**Tech Stack:** C++23 modules, mcpplibs cmdline/xpkg, nlohmann JSON, FTXUI, Bash, PowerShell, Python 3, GitHub Actions, mcpp/gtest.

## Global Constraints

- Deliver all changes in one PR; do not merge it.
- Do not publish a release or bump the xlings version.
- Do not modify xim-pkgindex or publish xlings-res assets.
- Treat non-empty xpkg `archs` as authoritative for every supported spec revision.
- Continue using libxpkg as the only Lua/xpkg parser and resource normalizer.
- Keep every stateful E2E in an isolated `HOME` and `XLINGS_HOME`.
- Cover Issue #471 with a first-install E2E whose explicit `XLINGS_HOME` has
  no pre-created `subos/` tree and which verifies persisted installed/active
  state after exit 0.
- Preserve Linux x86_64/aarch64, macOS 14 arm64 and Windows x86_64 release support.
- Preserve the existing `latest`-floating post-release fresh-install suite.
- Use `mcpp build` and `mcpp test`; never install dependencies with apt/brew/curl during local work.
- Commit incrementally without amend, rebase or force-push.

---

## File map

- `src/core/version_order.cppm`: arbitrary-component display ordering and internal-key filtering.
- `src/core/xim/inventory.cppm`: exact installed package inventory collection and catalog enrichment.
- `src/core/xim/compatibility.cppm`: normalized OS/architecture compatibility result and messages.
- `src/cli/spec.cppm`: canonical command tree, lookup, help JSON and command-reference JSON.
- `src/platform.cppm` plus platform partitions: reusable interactive/one-shot shell child execution.
- `src/ui/progress.cppm`, `src/core/xim/downloader.cppm`: explicit static versus rewriting progress.
- `src/cli.cppm`, manual handlers: CommandSpec help/validation and consistent stderr errors.
- `tools/other/quick_install.sh`, `.ps1`: target matrix, source-bound sidecar verification and explicit home behavior.
- `tests/unit/`: pure contract tests.
- `tests/e2e/`: isolated binary behavior tests.
- `tests/scripts/`: installer, docs and workflow contract tests.
- `.github/workflows/`: candidate artifact verification and required platform coverage.
- README/docs/skills: generated command and support/isolation contract.

### Task 1: Stable version ordering and `info` semantics

**Files:**
- Create: `src/core/version_order.cppm`
- Modify: `src/core.cppm`
- Modify: `src/core/xim/commands.cppm`
- Modify: `src/cli.cppm`
- Create: `tests/unit/test_version_order.cpp`
- Create: `tests/e2e/info_output_contract_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Produces: `xlings::version_order::compare(std::string_view, std::string_view) -> std::strong_ordering`
- Produces: `xlings::version_order::is_internal_key(std::string_view) -> bool`
- Produces: `xlings::version_order::sort_desc(std::vector<std::string>&)`
- Changes: `xim::cmd_info(target, stream, allVersions)` with `allVersions` defaulting false

- [ ] **Step 1: Write failing comparator tests**

Add literal expectations:

```cpp
TEST(VersionOrder, FourComponentRevisionIsNumeric) {
    EXPECT_GT(compare("2026.8.3.10", "2026.8.3.9"), 0);
}

TEST(VersionOrder, SemanticVersionsRemainNumeric) {
    std::vector<std::string> v{"0.0.9", "0.0.100", "0.0.11"};
    sort_desc(v);
    EXPECT_EQ(v, (std::vector<std::string>{"0.0.100", "0.0.11", "0.0.9"}));
}

TEST(VersionOrder, ResourceSentinelsAreInternal) {
    EXPECT_TRUE(is_internal_key("res_versioned"));
    EXPECT_FALSE(is_internal_key("2026.8.3.1"));
}
```

- [ ] **Step 2: Verify comparator tests fail**

Run: `mcpp test version_order`

Expected: FAIL because `xlings.core.version_order` does not exist.

- [ ] **Step 3: Implement the display-only comparator**

Parse dot-separated numeric components without changing `semver::parse` or
range behavior. Compare numeric vectors component-by-component, then stable
lexicographic fallback for non-numeric values. Export the three interfaces
above and import the module from `src/core.cppm`.

- [ ] **Step 4: Verify comparator tests pass**

Run: `mcpp test version_order`

Expected: all new tests PASS.

- [ ] **Step 5: Write the failing `info` E2E**

Create an isolated fixture with versions `0.0.9`, `0.0.10`,
`2026.8.3.9`, `2026.8.3.10`, alias `latest`, and internal key
`res_versioned`. Assert:

```bash
out="$(RUN info infofixture)"
[[ "$out" != *res_versioned* ]] || fail "info leaked res_versioned"
[[ "$out" == *"2026.8.3.10"* ]] || fail "latest date version missing"
[[ "$(printf '%s' "$out" | awk 'length > 240 { print }')" == "" ]] \
  || fail "default info emitted an unbounded line"

all="$(RUN info infofixture --all-versions)"
case "$all" in
  *"2026.8.3.10"*"2026.8.3.9"*"0.0.10"*"0.0.9"*) ;;
  *) fail "all versions are not deterministically descending" ;;
esac
```

Also install only an older version and assert package-level `installed: yes`
while `selected installed: no` for the latest selection.

- [ ] **Step 6: Verify the info E2E fails on baseline**

Run: `XLINGS_BIN=$(find target/x86_64-linux-gnu -path '*/bin/xlings' -type f | head -1) bash tests/e2e/info_output_contract_test.sh`

Expected: FAIL on sentinel/order/`--all-versions` handling.

- [ ] **Step 7: Implement bounded info fields and CLI flag**

Filter internal keys, sort concrete versions, render default recent/selected/
active/installed summary with embedded newlines, add `--all-versions`, and
separate package-level from selected-version installed state.

- [ ] **Step 8: Verify and commit Task 1**

Run:

```bash
mcpp build
mcpp test version_order
XLINGS_BIN=$(find target/x86_64-linux-gnu -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/info_output_contract_test.sh
```

Commit:

```bash
git add src/core.cppm src/core/version_order.cppm src/core/xim/commands.cppm \
  src/cli.cppm tests/unit/test_version_order.cpp \
  tests/e2e/info_output_contract_test.sh tests/e2e/run_all.sh
git commit -m "fix(xim): make package information deterministic"
```

### Task 2: Exact installed inventory

**Files:**
- Create: `src/core/xim/inventory.cppm`
- Modify: `src/core/xim.cppm`
- Modify: `src/core/xim/commands.cppm`
- Modify: `src/capabilities.cppm`
- Create: `tests/unit/test_xim_inventory.cpp`
- Create: `tests/e2e/list_exact_inventory_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Produces: `InstalledPackageRecord`
- Produces: `collect_inventory(PackageCatalog&, bool allSubos) -> std::vector<InstalledPackageRecord>`
- Consumes: `version_order::compare`
- Preserves: `cmd_list(filter, stream, all)` public signature

- [ ] **Step 1: Write failing inventory unit tests**

Use temporary home/subos JSON and payload manifests to assert literal records
for two non-latest versions, an index-deleted version, a missing payload, a
no-program package, a shared payload, and two namespaces with the same short
name. The mutation each test catches is catalog-latest filtering or identity
collapse.

- [ ] **Step 2: Verify inventory unit tests fail**

Run: `mcpp test xim_inventory`

Expected: FAIL because the collector is missing.

- [ ] **Step 3: Implement inventory collection**

Read real subos directories, ignore the `current` symlink as a duplicate,
normalize workspace installed values, read `.xpkg-install.json` payload
metadata, union by canonical identity/version, then enrich exact catalog
matches without dropping unmatched records. Mark missing payload records
`degraded`.

- [ ] **Step 4: Verify unit tests pass**

Run: `mcpp test xim_inventory`

- [ ] **Step 5: Write the failing end-to-end inventory contract**

Install two fixture versions whose `latest` is a third version, remove one
version from the fixture index after install, and assert default list,
`list --all`, `list --agent` and `interface list_packages` all retain both
exact installed versions with the same canonical identities.

- [ ] **Step 6: Verify E2E fails on baseline**

Run:

```bash
XLINGS_BIN=$(find target/x86_64-linux-gnu -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/list_exact_inventory_test.sh
```

Expected: baseline omits both non-latest rows.

- [ ] **Step 7: Route every list consumer through the snapshot**

Replace `catalog.search()` as the inventory source in `cmd_list`. Keep filter
matching after collection. Ensure the capability path calls the same function
and structured/plain renderers receive identical items.

- [ ] **Step 8: Verify and commit Task 2**

Run `mcpp build`, `mcpp test xim_inventory`, and the new E2E.

Commit:

```bash
git add src/core/xim.cppm src/core/xim/inventory.cppm \
  src/core/xim/commands.cppm src/capabilities.cppm \
  tests/unit/test_xim_inventory.cpp tests/e2e/list_exact_inventory_test.sh \
  tests/e2e/run_all.sh
git commit -m "fix(xim): enumerate exact installed package inventory"
```

### Task 3: Fail-closed OS/architecture compatibility

**Files:**
- Create: `src/core/xim/compatibility.cppm`
- Modify: `src/core/xim.cppm`
- Modify: `src/core/xim/resolver.cppm`
- Modify: `src/core/xim/installer.cppm`
- Modify: `src/core/xim/libxpkg/types/type.cppm`
- Create: `tests/unit/test_xpkg_compatibility.cpp`
- Create: `tests/e2e/xpkg_arch_fail_closed_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Produces: `TargetCompatibility { bool supported; std::string target; std::vector<std::string> supportedTargets; }`
- Produces: `check_target_compatibility(const mcpplibs::xpkg::Package&, std::string_view os, std::string_view arch)`
- Changes: `resolve(PackageCatalog&, std::span<const std::string>, const std::string& platform, const ActiveVersionFn& activeOf, const std::string& hostArch) -> std::expected<InstallPlan, std::string>`, with `activeOf` and `hostArch` defaulted for existing callers
- Adds: `PlanNode::targetCompatibility` only if the installer invariant needs the resolved result

- [ ] **Step 1: Write failing compatibility tests**

Cover spec 1 and spec 2 packages with `archs={"x86_64"}`, alias matching
(`arm64` equals `aarch64`), empty `archs`, compatible packages and a transitive
dependency path. Assert the exact error contains the root, failing dependency,
`linux-aarch64`, and `linux-x86_64`.

- [ ] **Step 2: Verify RED**

Run: `mcpp test xpkg_compatibility`

Expected: spec 1 is currently accepted.

- [ ] **Step 3: Implement and invoke the common gate in resolver**

After loading each package and before creating/expanding its node, call the
common compatibility helper. Append one causal plan error and do not traverse
that node's hooks/resources. Use libxpkg normalization helpers.

- [ ] **Step 4: Keep installer defense consistent**

Replace the spec-2-only condition in `installer.cppm` with the common helper.
On refusal, mark the node refused and skip download, hook and registration
audit paths.

- [ ] **Step 5: Write the zero-download E2E**

Serve a fixture index and an HTTP counter endpoint. Request an x86_64-only root
and a compatible root with an incompatible dependency under simulated
aarch64. Assert non-zero exit, `unsupported architecture`, request count zero,
and absent install/config marker files.

- [ ] **Step 6: Verify GREEN and commit Task 3**

Run unit test, E2E, `mcpp build`, then commit:

```bash
git add src/core/xim.cppm src/core/xim/compatibility.cppm \
  src/core/xim/resolver.cppm src/core/xim/installer.cppm \
  src/core/xim/libxpkg/types/type.cppm \
  tests/unit/test_xpkg_compatibility.cpp \
  tests/e2e/xpkg_arch_fail_closed_test.sh tests/e2e/run_all.sh
git commit -m "fix(xim): reject unsupported architectures before download"
```

### Task 4: Cross-platform SubOS one-shot execution

**Files:**
- Modify: `src/platform.cppm`
- Modify: `src/platform/unix.cppm`
- Modify: `src/platform/windows.cppm`
- Modify: `src/core/subos.cppm`
- Modify: `src/core/subos/sandbox.cppm`
- Create: `tests/unit/test_shell_command.cpp`
- Create: `tests/e2e/subos_cmd_contract_test.sh`
- Create: `tests/e2e/subos_cmd_contract_test.ps1`
- Modify: `tests/e2e/run_all.sh`
- Modify: `.github/workflows/xlings-ci-macos.yml`
- Modify: `.github/workflows/xlings-ci-windows.yml`

**Interfaces:**
- Produces: `platform::run_shell(std::string_view command, bool interactive) -> int`
- Consumes: environment already prepared by the SubOS caller

- [ ] **Step 1: Write failing argument-construction unit tests**

Assert POSIX one-shot arguments are `shell,-c,command`; PowerShell arguments
include `-NoLogo,-NonInteractive,-Command,command`; cmd fallback uses
`/d,/s,/c,command`. Assert an exit status of 37 maps to 37.

- [ ] **Step 2: Verify RED**

Run: `mcpp test shell_command`

- [ ] **Step 3: Extract and implement the platform helper**

Move duplicated CreateProcess/exec argument policy from `subos.cppm` and
`sandbox.cppm` behind the helper. Preserve handle inheritance and exact exit
codes. Keep environment mutation in SubOS code.

- [ ] **Step 4: Route sandbox and non-sandbox calls through it**

macOS and Windows sandbox branches call the helper with the non-empty `cmd`.
Interactive branches call the same helper without a command. Linux bwrap/proot
argv behavior remains unchanged.

- [ ] **Step 5: Add native marker/exit E2E**

Each script creates an isolated SubOS and runs:

```text
subos use probe --sandbox --cmd <write marker and exit 37>
```

Assert the marker contains redirected HOME/USERPROFILE and the xlings exit code
is exactly 37. Wire scripts as required steps in macOS and Windows workflows.

- [ ] **Step 6: Verify Linux locally and commit Task 4**

Run `mcpp build`, `mcpp test shell_command`, and the POSIX E2E. Commit all
listed files with `fix(subos): execute sandbox commands on every platform`.

### Task 5: Canonical CommandSpec, nested help and parser errors

**Files:**
- Create: `src/cli/spec.cppm`
- Modify: `src/cli.cppm`
- Modify: `src/core/subos.cppm`
- Modify: `src/core/xself.cppm`
- Modify: `src/core/profile.cppm`
- Modify: `src/agent/skills/usage.cppm`
- Modify: `src/ui/banner.cppm`
- Modify: `src/ui/layout.cppm`
- Create: `tests/unit/test_command_spec.cpp`
- Create: `tests/e2e/cli_help_contract_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Produces: `cli::spec::root() -> const CommandSpec&`
- Produces: `cli::spec::find(std::span<const std::string_view>) -> const CommandSpec*`
- Produces: `cli::spec::help_json(const CommandSpec&) -> nlohmann::json`
- Produces: `cli::spec::reference_json() -> nlohmann::json`
- Produces: `cli::spec::validate_manual_argv(const CommandSpec&, std::span<const std::string_view>) -> expected<ParsedManualArgs, CliError>`

- [ ] **Step 1: Write failing command-tree tests**

Assert all 15 top-level commands, `--help`, `--version`, list `--all`, info
`--all-versions`, every SubOS/self nested command and their real options.
Assert aliases resolve to the canonical node and unknown options return
`E_INVALID_INPUT`.

- [ ] **Step 2: Verify RED**

Run: `mcpp test command_spec`

- [ ] **Step 3: Implement the immutable tree and JSON renderer**

Use exported C++ structs with owned/static strings and vectors. Keep actions
outside the tree. Add top-level, nested and option lookup plus deterministic
JSON serialization.

- [ ] **Step 4: Replace handwritten help interception**

Resolve the command path ending before `-h/--help`, emit help for that exact
node, and remove the local `SubHelp` tables and `known_cmds`. Update the banner
to render the root spec. Wrap usage rather than truncate it at narrow widths.

- [ ] **Step 5: Validate manual handlers from the tree**

Use the shared validator in subos/self/profile. Reject `self doctor --bogus`,
missing values and surplus args. Keep business parsing only for converting
validated strings to domain values.

- [ ] **Step 6: Generate Agent reference from the tree**

Replace the handwritten command/flag reference section with deterministic
rendering of `reference_json()`. Keep decision-tree prose that is not command
metadata.

- [ ] **Step 7: Write and verify the help E2E**

For every node run `-h`, assert required usage/options, and verify output has
no ESC/NUL. Assert unknown command/option diagnostics are stderr-only and do
not append a full help page. Under a 32-column pseudo-TTY, assert the usage
tokens remain present across wrapped lines.

- [ ] **Step 8: Commit Task 5**

After `mcpp build`, unit and E2E pass, commit with
`refactor(cli): derive parsing and help from one command spec`.

### Task 6: Static progress and consistent error channels

**Files:**
- Modify: `src/ui/layout.cppm`
- Modify: `src/ui/progress.cppm`
- Modify: `src/core/xim/downloader.cppm`
- Modify: `src/cli.cppm`
- Modify: `src/core/log.cppm`
- Create: `tests/unit/test_progress_output.cpp`
- Create: `tests/e2e/non_tty_progress_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Produces: `ui::RenderPolicy { interactive, color, rewrite, agent }`
- Produces: `ui::render_policy()` initialized once by CLI startup
- Changes: download renderer receives `rewrite` explicitly

- [ ] **Step 1: Write failing progress byte tests**

Render the same in-progress state with rewrite true and false. Assert false
contains no byte `0x1b`, `0x00` or `\r`; assert true contains the expected erase
sequence. Assert two unchanged static states produce no duplicate frame.

- [ ] **Step 2: Verify RED**

Run: `mcpp test progress_output`

- [ ] **Step 3: Implement RenderPolicy and static renderer**

Initialize from stdout TTY, `NO_COLOR`, `--agent` and interface mode. Append
`ESC[J`, cursor hide/show and cursor-up only when rewrite is true. In static
mode, render phase transitions and the final state only.

- [ ] **Step 4: Stop the 200 ms loop in static mode**

Downloader invokes the static renderer only when observable state changes and
once after completion. Interactive behavior retains the existing timer.

- [ ] **Step 5: Normalize CLI errors**

Translate cmdline parser exceptions/results into EventStream errors. Human
diagnostics use `Error:`/`Hint:` on stderr; Agent behavior stays equivalent;
structured errors retain `E_*` codes. Remove secondary install audits for
nodes already refused or whose download failed.

- [ ] **Step 6: Add redirected install E2E**

Use a local fixture archive and capture stdout/stderr as bytes. Assert ESC=0,
NUL=0, no repeated full frame, error text on stderr, and non-zero failure.

- [ ] **Step 7: Verify and commit Task 6**

Run unit/E2E and commit with `fix(ui): make non-interactive progress static`.

### Task 7: Quick installer integrity, explicit home and target matrix

**Files:**
- Modify: `tools/other/quick_install.sh`
- Modify: `tools/other/quick_install.ps1`
- Create: `tests/scripts/test_quick_install.sh`
- Create: `tests/scripts/test_quick_install.ps1`
- Modify: `tests/e2e/windows_quick_install_resource_probe_test.sh`
- Modify: `.github/workflows/xlings-ci-linux.yml`
- Modify: `.github/workflows/xlings-ci-macos.yml`
- Modify: `.github/workflows/xlings-ci-windows.yml`

**Interfaces:**
- Candidate fields: source, tag, version, target, archive URL, checksum URL
- Explicit input: `XLINGS_HOME` remains the authoritative target
- Test input: existing release metadata/source overrides, extended only where a deterministic local server requires it

- [ ] **Step 1: Write failing POSIX installer tests**

Run a local HTTP server with two sources. Cover matching checksum, mismatch,
missing sidecar, malformed sidecar, archive from source A/checksum from source
B, new explicit `XLINGS_HOME` while PATH contains another xlings, `.9` versus
`.10`, non-TTY/NO_COLOR bytes, and unsupported macOS x86_64.

- [ ] **Step 2: Verify POSIX RED**

Run: `bash tests/scripts/test_quick_install.sh`

- [ ] **Step 3: Write failing PowerShell installer tests**

Cover the same candidate binding, checksum and explicit-home behaviors using a
local HTTP listener and temporary USERPROFILE.

- [ ] **Step 4: Verify PowerShell RED where pwsh is available**

Run: `pwsh -File tests/scripts/test_quick_install.ps1`

- [ ] **Step 5: Implement target matrix and source-bound sidecars**

Resolve archive and `.sha256` together. Download both from one candidate,
strictly parse the digest, compute SHA256 and compare before extraction. Fail
closed. Add the fourth numeric POSIX sort key.

- [ ] **Step 6: Honor explicit home and clean output**

Accept a nonexistent explicit target without PATH discovery. Detect
TTY/NO_COLOR and use curl silent/show-error when redirected. Print the actual
modified profile or a neutral restart-shell instruction.

- [ ] **Step 7: Make platform quick-install checks required**

Replace public-network `continue-on-error` smoke with deterministic local
installer tests in normal macOS/Windows PR CI. Keep public floating tests in
fresh-install workflow.

- [ ] **Step 8: Verify and commit Task 7**

Run both script suites, workflow YAML parsing test, and commit with
`fix(install): verify release checksums and explicit targets`.

### Task 8: Documentation, support matrix and executable examples

**Files:**
- Modify: `README.md`
- Modify: `README.zh.md`
- Modify: `docs/README.md`
- Modify: `docs/quick-start/multi-version.md`
- Modify: `docs/quick-start/subos-and-agent.md`
- Modify: `docs/design/subos-isolation.md`
- Modify: `docs/design/interface-protocol.md`
- Modify: `.agents/skills/xlings-usage/SKILL.md`
- Modify: `src/agent/skills/usage.cppm`
- Create: `tools/generate_command_reference.cppm` only if the built binary cannot expose the JSON directly; otherwise add a hidden read-only CLI output backed by CommandSpec
- Create: `tests/scripts/test_docs_examples.py`
- Create: `tests/scripts/test_generated_command_reference.py`
- Modify: `.github/workflows/xlings-ci-linux.yml`

**Interfaces:**
- Consumes: `CommandSpec::reference_json()`
- Produces: checked-in command-reference blocks delimited by stable markers

- [ ] **Step 1: Write failing docs/example tests**

The Python test extracts opted-in command blocks, rejects `subos create` and
`subos enter`, validates every help-safe command through the current binary,
executes interface examples and validates terminal NDJSON events. The
generation test compares checked-in command blocks with CommandSpec output.

- [ ] **Step 2: Verify RED**

Run:

```bash
python3 tests/scripts/test_docs_examples.py --xlings "$(find target/x86_64-linux-gnu -path '*/bin/xlings' -type f | head -1)"
python3 tests/scripts/test_generated_command_reference.py \
  --xlings "$(find target/x86_64-linux-gnu -path '*/bin/xlings' -type f | head -1)"
```

- [ ] **Step 3: Update support and isolation claims**

Add the exact OS/architecture table before Quick Start. State Linux filesystem
isolation versus macOS/Windows HOME redirection and explicitly warn that the
latter is not for untrusted code. Follow the xlings docs-writing language and
header rules.

- [ ] **Step 4: Correct commands and interface examples**

Use `subos new/use`; use `xlings interface <capability> --args '<json>'`;
remove stale 0.4.36 labels that claim current behavior. Do not turn historical
design dates into current version promises.

- [ ] **Step 5: Generate and verify command references**

Regenerate README/skill marked blocks from CommandSpec, run both tests and
`git diff --check`.

- [ ] **Step 6: Commit Task 8**

Commit with `docs: publish exact platform and command contracts`.

### Task 9: Pre-publication candidate release gates

**Files:**
- Modify: `.github/workflows/release.yml`
- Modify: `.github/workflows/xlings-ci-aarch64.yml`
- Modify: `.github/workflows/xlings-ci-fresh-install.yml`
- Create: `tests/candidate-install/smoke.sh`
- Create: `tests/candidate-install/smoke.ps1`
- Create: `tests/scripts/test_release_candidate_gate.py`
- Modify: `tests/scripts/test_release_resource_contract.py`

**Interfaces:**
- Candidate scripts consume a local archive path and matching sidecar path
- `create-release.needs` consumes all build and candidate job results

- [ ] **Step 1: Write failing workflow contract tests**

Parse release YAML with a loader that preserves `on`. Assert sidecars are
generated before candidate jobs, candidate jobs exist for linux-x86_64,
linux-aarch64, macos-arm64 and windows-x86_64, and `create-release.needs`
contains every candidate job. Assert candidate jobs do not create tags or
releases.

- [ ] **Step 2: Verify RED**

Run: `python3 tests/scripts/test_release_candidate_gate.py`

- [ ] **Step 3: Write candidate smoke scripts**

Extract the archive into a temporary cold HOME, verify its sidecar, run self
install, then execute search/install/run/list/info/use/remove, SubOS normal
`--cmd`, platform sandbox `--cmd`, marker/exit 37 and `self doctor`. Linux
aarch64 additionally asserts compatible ninja and zero-download unsupported
d2x/backend behavior.

- [ ] **Step 4: Add candidate jobs before release publication**

Generate sidecars in a preparation job, upload them with candidate artifacts,
download the matching artifact on each native runner, and run the candidate
script. Use `ubuntu-24.04-arm` for aarch64 and `macos-14` for the floor.

- [ ] **Step 5: Rewire create-release dependencies**

Require all candidate jobs. Keep publish-index, mirror-binaries and bump-index
strictly downstream. Do not trigger or publish a release from PR CI.

- [ ] **Step 6: Strengthen normal aarch64 PR CI**

Run the built aarch64 binary natively on the ARM runner for target-compatible
and fail-closed fixture tests, rather than only QEMU `--version`.

- [ ] **Step 7: Verify workflow contracts and commit Task 9**

Run both Python workflow tests plus `actionlint` only if already available via
xlings/project tools. Commit with `ci(release): gate publication on native candidate smoke`.

### Task 10: Full local regression and PR publication

**Files:**
- Modify only files required by failures found in verification
- Update: `docs/superpowers/plans/2026-08-03-multiplatform-user-experience-contracts.md` checkboxes

**Interfaces:**
- Consumes every contract above
- Produces one Draft PR against `main`

- [ ] **Step 1: Run formatting/static checks**

```bash
git diff --check origin/main...HEAD
bash tests/fresh-install/no_xlings_version_pin_check.sh
python3 tests/scripts/test_release_resource_contract.py
python3 tests/scripts/test_release_candidate_gate.py
python3 tests/scripts/test_docs_examples.py --xlings "$(find target/x86_64-linux-gnu -path '*/bin/xlings' -type f | head -1)"
```

- [ ] **Step 2: Run full build and unit suite**

```bash
mcpp build
mcpp test
```

Expected: every test target PASS and final result `0 failed`.

- [ ] **Step 3: Run the complete Linux E2E suite**

```bash
XLINGS_BIN=$(find target/x86_64-linux-gnu -path '*/bin/xlings' -type f | head -1) \
  bash tests/e2e/run_all.sh
```

Expected: every non-platform-skipped E2E PASS in isolated homes.

- [ ] **Step 4: Run focused candidate/installer suites**

Run POSIX installer tests, local candidate smoke, output byte audit, inventory,
architecture and SubOS command E2Es again after the full suite.

- [ ] **Step 5: Review scope and commits**

```bash
git status --short
git diff --stat origin/main...HEAD
git log --oneline origin/main..HEAD
```

Confirm no version bump, no external-repo edits, and no generated runtime files.

- [ ] **Step 6: Invoke verification-before-completion and requesting-code-review skills**

Perform the required self-review against the design requirement matrix and fix
every discrepancy before publication.

- [ ] **Step 7: Publish one Draft PR**

Use `github:yeet` to confirm scope, push
`feat/multiplatform-ux-contracts`, and create one Draft PR. The body lists all
P0/P1/P2 items, local evidence and the explicit non-goals.

- [ ] **Step 8: Monitor every PR check to terminal state**

Use `github:gh-fix-ci` for any failure. Reproduce the exact failing command,
write or tighten a regression test, commit the fix normally, push, and wait for
the replacement run. Do not treat running, skipped-required, canceled or
superseded jobs as green.

- [ ] **Step 9: Final completion audit**

For every row in the design's requirement-to-evidence matrix, link the exact
test or native job that proves it. Confirm all required checks are terminal
success and the PR remains unmerged. Only then report completion.
