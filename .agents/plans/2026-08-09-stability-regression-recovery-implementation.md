# xlings Stability Regression Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 恢复 xlings 本地命令的即时响应，修复 #513/#514/#506、SubOS 使用体验与 runtime/graphics 稳定性，并通过发布后的跨仓真实 sandbox 路径完成闭环。

**Architecture:** 保留当前 JSON state、共享 immutable payload、SubOS manifest 和 exact resolver，不引入 SQLite 或新 control plane。查询只读路径采用 local-only/targeted reader；昂贵诊断移到 deep audit；libxpkg 用结构化 hook result 与显式 store roots；每个 SubOS 的 exact runtime 是执行权威，mcpp 的 shared compiler owner runtime 与 project target runtime 分责。

**Tech Stack:** C++23 modules、mcpp、GoogleTest、Bash/PowerShell E2E、Lua/libxpkg、NDJSON interface、GitHub Actions、GitHub Releases、GitCode `gtc`、mcpp-index、xim-pkgindex/xlings-res。

## Global Constraints

- 多份 glibc payload 可以并存；一次 SubOS 执行只能有一个 exact active runtime。
- `subos_info.runtime` 是 SubOS runtime 权威；禁止按目录顺序、最高版本或目录改名选择。
- 本轮不引入 SQLite，不重写状态层，不为 mcpp 添加 xlings 专用架构。
- 默认只读命令禁止网络、payload recursive walk、recipe hook 与 child `xlings`。
- 所有 stateful tests 使用临时 `HOME`、`XLINGS_HOME`、`MCPP_HOME`、project 和 SubOS roots；禁止修改真实用户状态。
- 每个 bug 先写会在当前 main 上失败的测试，再做最小实现；每个 task 独立 commit，最终每仓 PR 按仓库规则 squash merge。
- 预期版本仅在远端不前进时成立：libxpkg `0.0.55`、xlings `2026.8.9.3`、mcpp `2026.8.9.1`；cut 前重新计算，常规日期版本绝不使用 `.0`。
- CI 的 running/cancelled/superseded 不算通过；macOS、Windows、aarch64 结论只来自 native runner。
- 发布完成包含 GitHub assets、GitCode GET+sha256、xim-pkgindex latest/hash 和公开 cold-home install；source build 不是发布闭环。
- 最终 runtime/graphics 证据必须包含 `xlings subos use <name> --sandbox --cmd ...`；硬件 cell 使用 `--sandbox --gpu --cmd`，缺设备记录 SKIP reason。

---

## Dependency DAG

```text
T1 #513 libxpkg hook result ─┐
                             ├─> T3 libxpkg source tag ─> T3A mcpp-index + CN asset ─> T8 xlings host integration
T2 #514 explicit roots ──────┘

T4 local-only catalog ─> T5 targeted info/list ─┐
T6 quick/deep doctor ───────────────────────────┤
T7 runtime guards ──────────────────────────────┤
T8 #513/#514 xlings host integration ───────────┤
T9 #506 removal ────────────────────────────────┤─> T11 xlings full gate/release/index A
T10 subos use candidates ───────────────────────┘

T11 published xlings ─> T12/T13 mcpp #392 + #514 integration
T12/T13 ─> T14 mcpp release ─> T15 xim-pkgindex ecosystem PR/index B
T15 ─> T16 public cold/old-home and real sandbox audit
```

513 and 514 share the libxpkg release but remain separate tests and commits. 506 does not depend on libxpkg; it joins only at the xlings integration/release gate. mcpp production code is required for #392, not #514; #514 contributes an mcpp integration test only.

### Task 1: libxpkg #513 bounded hook diagnostics

**Repository:** `openxlings/libxpkg` (isolated worktree)

**Files:**
- Modify: `src/xpkg-executor.cppm`
- Modify: `tests/test_executor.cpp`
- Inspect unchanged contract: `src/lua-stdlib/xim/libxpkg/log.lua`

**Interfaces:**
- Produces: `HookResult{success,error,output,version}` where every failed hook has non-empty `error` and `output` is a bounded, valid-UTF-8 Lua-level transcript; invalid byte sequences become U+FFFD so JSON serialization cannot fail.
- Produces: `inline constexpr std::size_t kMaxHookOutputBytes = 16 * 1024` and deterministic marker `\n[libxpkg: hook output truncated]\n`.
- Capture scope: Lua `print`, `io.write`, `io.stderr:write`; `log.error` follows through its existing `io.write` implementation. Child OS-process fds are explicitly outside this contract.

- [x] **Step 1: Add the false-return/output red test**

Add a recipe in `tests/test_executor.cpp` whose install hook executes:

```lua
print("REPRO stdout")
log.error("REPRO log.error")
io.stderr:write("REPRO stderr\n")
return false
```

Assert `success == false`, `error == "install hook returned false"`, all three markers occur in `output`, and no marker escapes to the test process stdout/stderr capture.

- [x] **Step 2: Run the focused test and record RED**

```bash
mcpp test -- --gtest_filter='*RunHook_CapturesLuaOutputAndNamesFalse*'
```

Expected current-main failure: `HookResult.error` is empty and `HookResult.output` omits the three markers.

- [x] **Step 3: Add transcript bound/concurrency tests**

Write `RunHook_BoundsTranscriptAndKeepsTail` with output larger than 16 KiB followed by `TAIL-MARKER`, plus two executor instances whose markers must not cross. Add `io.write(string.char(0xff))` and assert U+FFFD appears. Assert final `output.size()` is bounded by cap plus marker, keeps the tail, contains exactly one truncation marker, and is valid UTF-8.

- [x] **Step 4: Implement an executor-local Lua sink**

Keep the sink owned by `PackageExecutor`/its Lua state. Before `pcall`, replace the three Lua writers with C callbacks that append UTF-8-preserving bytes to the local tail buffer. Never call `dup2`, `freopen`, or redirect process-global stdout/stderr. Restore/clear per invocation so successive hooks cannot inherit output.

For boolean false set:

```cpp
result.success = false;
result.error = std::string(name) + " hook returned false";
result.output = hookOutput_.finish();
```

Lua exceptions preserve the exception in `error` and attach the same transcript.

- [x] **Step 5: Run libxpkg tests and commit**

```bash
mcpp test -- --gtest_filter='*RunHook_*'
mcpp test
git diff --check
git add src/xpkg-executor.cppm tests/test_executor.cpp
git commit -m "fix(executor): preserve bounded hook diagnostics"
```

### Task 2: libxpkg #514 explicit dependency store roots

**Repository:** `openxlings/libxpkg` (isolated worktree)

**Files:**
- Modify: `src/xpkg-executor.cppm`
- Modify: `src/lua-stdlib/xim/libxpkg/pkginfo.lua`
- Modify: `tests/test_executor.cpp`

**Interfaces:**
- Consumes: existing `ExecutionContext::resolved_deps` for xlings runtime dependencies.
- Produces: `std::vector<fs::path> ExecutionContext::dependency_store_roots`, injected as `_RUNTIME.dependency_store_roots` in supplied order.
- Resolution order: valid exact record; exact namespace/version in explicit roots; old-context compatibility scan/XVM. An invalid supplied record fails and never falls through.

- [x] **Step 1: Add the explicit-root red fixture**

Create only `<tmp>/registry/data/xpkgs/compat-x-zlib/1.3.2`, add a same-bare-name decoy `<tmp>/member/data/xpkgs/other-x-zlib/1.3.2`, set `resolved_deps={}` and `dependency_store_roots={registry/data/xpkgs}`. A hook calling `pkginfo.install_dir("compat:zlib", "1.3.2")` must return the registry path.

- [x] **Step 2: Run RED**

```bash
mcpp test -- --gtest_filter='*PkgInfo_UsesExplicitDependencyStoreRoots*'
```

Expected current-main failure: `_RUNTIME` has no roots field and the global registry payload is not found.

- [x] **Step 3: Add authority/fail tests**

Add cases for: exact record wins over roots; exact record points to missing payload and returns a semantic error/no decoy; wrong namespace is rejected; explicit roots do not include an unrelated `$MCPP_HOME`; no new field preserves legacy scan with one bounded warning.

- [x] **Step 4: Implement context injection and exact scan**

Add the vector field, inject it with the existing `push_string_array`, and split Lua resolution into:

```lua
local function _resolve_dep_via_explicit_roots(dep_name, dep_version)
  local ns, bare = _parse_namespace(dep_name)
  for _, root in ipairs((_RUNTIME and _RUNTIME.dependency_store_roots) or {}) do
    local hit = _scan_dir(root, ns, bare, dep_version)
    if hit then return hit end
  end
end
```

Require an exact version when this function is used. Correct comments that call `resolved_deps` total across host dependency domains. Detect presence of the new field by `type(...) == "table"`; a modern context miss returns nil/error without legacy heuristic.

- [x] **Step 5: Run full tests and commit**

```bash
mcpp test -- --gtest_filter='*PkgInfo_*'
mcpp test
git diff --check
git add src/xpkg-executor.cppm src/lua-stdlib/xim/libxpkg/pkginfo.lua tests/test_executor.cpp
git commit -m "fix(pkginfo): resolve host dependencies from explicit stores"
```

### Task 3: Publish libxpkg dependency release

**Repository:** `openxlings/libxpkg` (isolated worktree)

**Files:**
- Modify: `mcpp.toml`
- Verify unchanged dependency lock: `mcpp.lock`

**Interfaces:**
- Consumes: Tasks 1-2 green commits.
- Produces: released version consumed by xlings Task 8; expected `0.0.55` if unused at cut time.

- [x] **Step 1: Re-fetch and compute next version**

```bash
git fetch origin --tags
git log --oneline --decorate -8 origin/main
git tag --list | sort -V | tail -20
```

Abort the literal `0.0.55` choice if it already exists; choose the next patch version without rewriting history.

- [x] **Step 2: Bump version and run release preflight**

Update `mcpp.toml`, then:

```bash
mcpp test
git diff --check
git status --short
git add mcpp.toml
git commit -m "chore(release): bump libxpkg to 0.0.55"
```

- [x] **Step 3: Push one Draft PR and require all CI terminal green**

PR body must link #513/#514, state xlings is the downstream consumer, list exact tests and compatibility boundary, and contain no claim that `$MCPP_HOME` is scanned.

- [x] **Step 4: Admin/bypass squash merge and source tag**

After head SHA and every required check are terminal green, squash merge through the authorized bypass path, fetch main, and verify both task commits are represented by the squash. Follow the current 0.0.47–0.0.54 convention: create and push an annotated, unprefixed `0.0.55` tag on the merged main commit with a precise release message; do not invent a binary GitHub Release because this source library currently publishes through tags. Verify `git ls-remote --tags` exposes the annotated tag and peels to the merged commit. Package-manager resolution is a separate registry publication dependency in Task 3A; a source tag alone is deliberately not called installable.

### Task 3A: Publish libxpkg 0.0.55 through mcpp-index and the CN mirror

**Repository:** `mcpplibs/mcpp-index` (isolated worktree)

**Files:**
- Modify: `pkgs/x/xpkg.lua`
- External asset: exact GitHub tag archive mirrored as `mcpp-res/xpkg` release asset `xpkg-0.0.55.tar.gz`

**Interfaces:**
- Consumes: Task 3 annotated source tag `0.0.55` peeled to merged libxpkg commit `7ecf628fe8cc19fa3419abe59e9d4fba1d93d2d8`.
- Produces: identical GLOBAL/CN bytes and one `0.0.55` descriptor entry on Linux, macOS and Windows.
- Required by: Task 8 dependency bump. `mcpp add/build` must resolve through the mcpplibs index; the libxpkg Git tag is necessary but not sufficient.

- [x] **Step 1: Download the immutable GLOBAL archive twice and compute the descriptor hash**

Use GET for `https://github.com/openxlings/libxpkg/archive/refs/tags/0.0.55.tar.gz`; two independent downloads must have the same SHA-256. Inspect the archive root and its `mcpp.toml` version before publication.

- [x] **Step 2: Publish the exact bytes to GitCode with local `gtc`**

Query the public GitCode release first because upload is non-idempotent. If `0.0.55`/`xpkg-0.0.55.tar.gz` is absent, publish it to `mcpp-res/xpkg` with local `gtc`, targeting `main`. Download the public CN URL with GET and require HTTP 200 plus the same SHA-256 as GLOBAL. Never upload a repacked archive.

- [x] **Step 3: Add all-platform descriptor entries and run focused lint**

Prepend the same `0.0.55` URL table and SHA-256 to `xpm.linux`, `xpm.macosx`, and `xpm.windows`. Run `mcpp xpkg parse pkgs/x/xpkg.lua --json`, `lua5.4 tests/check_platform_version_parity.lua pkgs/x/xpkg.lua`, `lua5.4 tests/check_mirror_urls.lua pkgs/x/xpkg.lua`, and verify `tests/list_cn_urls.lua` emits the new URL/hash exactly once. Commit only the descriptor.

- [x] **Step 4: Push one Draft PR, require all applicable CI, and bypass squash merge**

The PR must cite libxpkg PR #39 and xlings #513/#514, record GLOBAL/CN GET hashes, and explain the failed pre-index `E_NOT_FOUND` probe. Mark ready and bypass squash only after the exact head's required checks are terminal green. Fetch `origin/main` and verify the merged descriptor on all three platforms.

- [x] **Step 5: Re-run a public dependency-resolution probe**

In isolated/temp state, refresh the `mcpplibs` index and run a minimal `mcpp add mcpplibs.xpkg@0.0.55` plus `mcpp build`. Require `mcpp.lock` to resolve `mcpplibs.xpkg` at `0.0.55` and the downloaded source's `mcpp.toml` to report `0.0.55`. Only then may Task 8 consume the release.

### Task 4: xlings local-only catalog policy

**Repository:** `openxlings/xlings` (isolated worktree)

**Files:**
- Modify: `src/core/xim/commands.cppm`
- Modify: `src/core/subos/sandbox.cppm`
- Create: `tests/e2e/local_query_no_index_sync_test.sh`
- Modify: `tests/e2e/run_all.sh`
- Preserve: `tests/e2e/install_subindex_first_run_test.sh`

**Interfaces:**
- Produces: `export enum class CatalogAccess { LocalOnly, InstallReady };`
- Produces: `export PackageCatalog& get_catalog(CatalogAccess access = CatalogAccess::LocalOnly);`
- `LocalOnly` rebuilds only local catalog/cache. `InstallReady` alone may `sync_all_repos(true)` for failed catalog or missing sub-index initialization.

- [x] **Step 1: Write no-network red E2E**

Use a cold isolated home and a PATH-local `git` recorder that writes a marker then exits 97. Under `timeout 2s`, run `list`, `info gcc`, `search gcc`, `why gcc`, `config`, `subos list`, `subos info`, `self doctor`, `--help` and `--version`; assert none invoke the recorder and each returns either local data or the existing prompt local-index diagnostic. Also cover remove lookup and sandbox backend lookup. The NDJSON interface remains a long-lived server and is tested through one request plus clean shutdown, not as a one-shot timing command.

- [x] **Step 2: Run RED**

```bash
mcpp build
XLINGS_BIN="$(find target -type f -path '*/bin/xlings' -perm -111 | head -1)"
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/local_query_no_index_sync_test.sh
```

Expected current-main failure: the cold query calls `sync_all_repos(true)` and the recorder marker exists.

- [x] **Step 3: Split catalog access policy**

Refactor the existing global factory so local rebuild never fetches. Call `get_catalog(CatalogAccess::InstallReady)` only from install/explicit acquisition flows. Keep search/list/info/remove/sandbox on `LocalOnly`. Update/update-index remains explicitly syncing and must not depend on implicit factory behavior.

- [x] **Step 4: Prove first-install sub-index behavior survives and commit**

```bash
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/install_subindex_first_run_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/local_query_no_index_sync_test.sh
git diff --check
git add src/core/xim/commands.cppm src/core/subos/sandbox.cppm tests/e2e/local_query_no_index_sync_test.sh tests/e2e/run_all.sh
git commit -m "fix(xim): keep local queries off index sync"
```

### Task 5: xlings targeted info and filter-first list

**Repository:** `openxlings/xlings` (isolated worktree)

**Files:**
- Modify: `src/core/xim/inventory.cppm`
- Modify: `src/core/xim/commands.cppm`
- Modify: `tests/unit/test_xim_install.cpp`
- Create: `tests/e2e/query_heavy_home_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Produces: `InventoryTrace {metadataIdentities, payloadVersionDirs}` as an optional test seam.
- Produces: `collect_package_inventory(catalog, canonicalIdentity, allSubos, InventoryTrace* = nullptr)` that never calls the full collector.
- Produces: `collect_inventory(catalog, allSubos, optionalCanonicalFilter, InventoryTrace* = nullptr)`, with the filter applied before metadata lookup/row assembly.
- Preserves: namespace identity, project/global stores, zero-target/data-only stamps, missing payload, inactive versions and `--all` attribution.

- [x] **Step 1: Add structural RED tests**

Construct one target, 100 unrelated stamped package roots and 30 SubOS workspace JSON files. Add a test seam/counter around metadata and payload visits. `info target` may read the SubOS metadata files but must inspect only target version roots; `list target` must not load unrelated recipe metadata. Seed a namespace collision and project-store target.

- [x] **Step 2: Run RED**

```bash
mcpp test -- --gtest_filter='XimCommandsTest.InfoKnownPackage'
mcpp test -- --gtest_filter='XimCommandsTest.ListWithFilter'
```

Expected current-main failure: info delegates to full inventory then erases unrelated rows; list filters after assembly.

- [x] **Step 3: Implement targeted collectors**

Read each workspace map and select only the canonical key; query only related VersionDB provider/targets; for payload-only state inspect only the target store/package/version and stamp. For list, apply canonical/bare filter before `MetadataLookup` and do only shallow package/version enumeration. Do not enter payload content trees or execute Lua recipes.

- [x] **Step 4: Run semantic and heavy-home gates and commit**

```bash
mcpp test -- --gtest_filter='XimCommandsTest.*'
for t in list_exact_inventory_test.sh info_output_contract_test.sh query_heavy_home_test.sh; do
  XLINGS_BIN="$XLINGS_BIN" bash "tests/e2e/$t"
done
git diff --check
git add src/core/xim/inventory.cppm src/core/xim/commands.cppm tests/unit/test_xim_install.cpp tests/e2e/query_heavy_home_test.sh tests/e2e/run_all.sh
git commit -m "fix(xim): make package queries proportional to their target"
```

### Task 6: xlings quick/deep doctor and internal coordinate probe

**Repository:** `openxlings/xlings` (isolated worktree)

**Files:**
- Modify: `src/cli/spec.cppm`
- Modify: `src/core/xself.cppm`
- Modify: `src/core/xself/doctor.cppm`
- Modify: `src/core/xself/repair.cppm`
- Modify: `src/core/xim/catalog.cppm`
- Create: `tests/e2e/self_doctor_depth_test.sh`
- Modify: `tests/e2e/self_doctor_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Produces: `self doctor --deep [--scope package[@version]]`.
- Default quick mode keeps metadata/manifest/shim/workspace checks and performs zero recursive payload scan, zero glibc `getent` runtime probe, zero child `xlings`.
- `--fix` implies deep detection so existing repairs remain reachable; `--dry-run` changes writes, not detection depth.
- Produces the cycle-free leaf helper `std::optional<PackageMatch> resolve_local_coordinate(const PackageCatalog&, std::string_view)`; it reads only the supplied local catalog and performs no sync.

- [x] **Step 1: Add 500-ELF structural RED test**

Seed 500 copies/reflinks of `/bin/true` in an isolated payload and a PATH-local patchelf recorder. Assert default doctor makes zero recorder calls; `--deep --scope fixture@1` makes exactly two calls per ELF and emits scan scope before work.

- [x] **Step 2: Run RED**

```bash
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/self_doctor_depth_test.sh
```

Expected current-main failure: default doctor invokes patchelf 1,000 times and `--deep` is unknown.

- [x] **Step 3: Introduce audit depth and move expensive checks**

Parse `--deep` and `--scope` in both command spec and manual runner. Guard `scan_payload`, glibc `getent`, runtime functionality probes and coordinate remedy resolution behind deep mode. Emit one immediate scope/progress event. Keep `--fix` deep to preserve repair coverage.

- [x] **Step 4: Replace recursive CLI probe**

Add a read-only `catalog` helper that evaluates a coordinate from already-local index state without sync. Inject/call it from doctor instead of `repair::probe_coordinate`, and assert a fake command runner sees zero child `xlings` in both quick and deep paths.

- [x] **Step 5: Run convergence suites and commit**

```bash
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/self_doctor_depth_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/self_doctor_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/self_doctor_multi_subos_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/doctor_fix_convergence_test.sh
git diff --check
git add src/cli/spec.cppm src/core/xself.cppm src/core/xself/doctor.cppm src/core/xself/repair.cppm src/core/xim/catalog.cppm tests/e2e/self_doctor_depth_test.sh tests/e2e/self_doctor_test.sh tests/e2e/run_all.sh
git commit -m "fix(doctor): keep deep payload audits explicit"
```

### Task 7: xlings bidirectional runtime safety and SubOS runtime equality

**Repository:** `openxlings/xlings` (isolated worktree)

**Files:**
- Modify: `src/core/elf_same_source.cppm`
- Modify: `tests/unit/test_elf_same_source.cpp`
- Modify: `src/core/subos/manifest.cppm`
- Modify: `src/core/xself/doctor.cppm`
- Modify: `src/core/xvm/commands.cppm`
- Create: `tests/e2e/elf_host_loader_payload_libc_guard_test.sh`
- Create: `tests/e2e/subos_runtime_authority_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Extends `elfcheck::Finding` with `enum class Reason { None, PayloadMismatch, HostLoaderPayloadCore }`.
- Produces `bool directory_contains_core_runtime(const fs::path&)` and an injectable `CoreDirProbe` used by `check`; classification is based on resolved directory contents: `libc.so.6`, `ld-linux-*.so.*`, and musl loader equivalents.
- Produces `RuntimeActivationMismatch {declared, active, payloadMissing}` and `check_runtime_activation(const manifest::Info&, std::string_view activeVersion, bool payloadExists)`; callers read XVM state, so the manifest leaf does not import VersionDB.

- [x] **Step 1: Add reverse same-source RED cases**

In `test_elf_same_source.cpp`, use temporary real directories and assert:

```text
host PT_INTERP + payload dir containing libc.so.6       => violation
host PT_INTERP + payload dir containing only libX11.so => pass
payload loader 2.44 + payload libc 2.44                => pass
payload loader 2.44 + payload libc 2.39                => violation
SubOS symlink resolving to payload libc                => violation
```

- [x] **Step 2: Run reverse-guard RED**

```bash
mcpp test -- --gtest_filter='SameSource.*'
```

Expected current-main failure: a host interpreter returns clean before RPATH is inspected.

- [x] **Step 3: Implement the shared bidirectional predicate**

Resolve `$ORIGIN` against the tested ELF directory and resolve symlinks without escaping errors. For a host interpreter, only core-runtime-containing payload paths violate; payload leaf libraries remain legal. Keep the existing exact payload-loader/provider comparison. Include interpreter and offending path in `describe`.

- [x] **Step 4: Add manifest/workspace equality RED**

Seed a SubOS manifest declaring `glibc@2.44` with workspace/XVM active `glibc@2.39`. Require quick doctor to report both values. Attempt ordinary `xlings use glibc@2.39` in the 2.44 SubOS and require refusal with an explicit migration hint; both payload directories remain untouched.

- [x] **Step 5: Implement runtime equality guard**

Add the shared predicate in `subos/manifest.cppm`; call it from doctor and before an xvm use that would change the runtime family named by the current SubOS manifest. Do not implement migration in this task. Exact match continues normally; another package family is unaffected.

- [x] **Step 6: Run unit/install/runtime suites and commit**

```bash
mcpp test -- --gtest_filter='SameSource.*'
mcpp test -- --gtest_filter='*SubosRuntime*'
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/elf_host_loader_payload_libc_guard_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/subos_runtime_authority_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/subos_env_libc_guard_test.sh
git diff --check
git add src/core/elf_same_source.cppm tests/unit/test_elf_same_source.cpp tests/unit/test_subos_manifest.cpp src/core/subos/manifest.cppm src/core/xself/doctor.cppm src/core/xvm/commands.cppm tests/e2e/elf_host_loader_payload_libc_guard_test.sh tests/e2e/subos_runtime_authority_test.sh tests/e2e/run_all.sh
git commit -m "fix(runtime): enforce one coherent SubOS core runtime"
```

Recorded delivery: xlings commit `71b996c`; no-cache and full unit gates both
passed 36/36 targets, focused SameSource 16/16 and SubosRuntime 5/5, and all
three runtime E2Es passed.

### Task 8: xlings #513/#514 libxpkg host integration

**Repository:** `openxlings/xlings` (isolated worktree)

**Files:**
- Modify: `mcpp.toml`
- Modify: `mcpp.lock`
- Modify: `src/core/xim/installer.cppm`
- Modify: `tests/e2e/install_silent_failure_test.sh`
- Create: `tests/e2e/shared_registry_dep_install_dir_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Consumes: released libxpkg from Task 3.
- Produces: one `format_hook_failure(hookName, HookResult)` path used by install/config/uninstall.
- Populates `ExecutionContext::dependency_store_roots` as ordered/deduplicated exact `<data>/xpkgs` paths: selected/current store first, project store when distinct, global store last. It never reads `MCPP_HOME`.

- [x] **Step 1: Bump to the released libxpkg and rebuild**

Update dependency and lock using the repository's normal mcpp resolution command. Verify the lock names the exact published version and checksum/source, not a local path.

```bash
mcpp build
```

- [x] **Step 2: Extend #513 E2E and record RED before xlings formatting**

Make the failing fixture emit stdout, stderr and `log.error`, then return false. CLI output must include the semantic reason and markers. For `interface install_packages`, parse every stdout line as JSON and require one `E_INTERNAL` message containing the bounded transcript; no raw Lua line may break NDJSON.

- [x] **Step 3: Centralize hook failure formatting**

Use the same helper in install/config/uninstall. It must never return empty, must avoid duplicating identical error/output text, and must preserve libxpkg's truncation marker. Pass the result through existing `InstallStatus.message`/EventStream rather than direct stdout.

- [x] **Step 4: Add #514 shared-registry RED E2E**

Use `XLINGS_HOME=<tmp>/registry` and `XLINGS_PROJECT_DIR=<tmp>/member/.mcpp`. Seed only `registry/data/xpkgs/compat-x-zlib/1.3.2`, install the consumer project-locally, and have its hook record `pkginfo.install_dir("compat:zlib","1.3.2")`. Assert the exact global path is returned. Add `other-x-zlib` decoys in project/global roots and an out-of-list `MCPP_HOME` decoy.

- [x] **Step 5: Populate explicit roots and run E2Es**

```bash
mcpp build
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/install_silent_failure_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/shared_registry_dep_install_dir_test.sh
git diff --check
git add mcpp.toml mcpp.lock src/core/xim/installer.cppm tests/e2e/install_silent_failure_test.sh tests/e2e/shared_registry_dep_install_dir_test.sh tests/e2e/run_all.sh
git commit -m "fix(xim): preserve hook failures and shared dependency paths"
```

**Delivered:** `8a325be`. The public lock resolves `mcpplibs.xpkg@0.0.55`
without a local path; the #513 and #514 focused E2Es and the full 36-target
unit gate passed.

### Task 9: xlings #506 provider-aware zero-target removal

**Repository:** `openxlings/xlings` (isolated worktree)

**Files:**
- Modify: `src/core/xim/installer.cppm`
- Modify: `src/core/xim/commands.cppm`
- Create: `tests/e2e/remove_foreign_provider_delegator_test.sh`
- Modify: `tests/e2e/config_install_no_implicit_dir_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Replaces target-empty D4 with `executing_provider_owns_no_version(db, target, executingProvider)`.
- A foreign provider's versions never block this provider's uninstall and are never removed by it.
- A payloadless dependency-free config with an executable local/snapshot recipe reaches uninstall exactly once; arbitrary absent packages remain NotFound.

- [x] **Step 1: Write foreign-provider delegator RED**

Fixture provider A installs/uninstalls a delegator and writes an uninstall marker but registers no `gcc` version. Provider B owns `gcc@local:1` through `bindingGroup->provider`. Removing A must run its marker and preserve B's version, shim and workspace entry.

- [x] **Step 2: Write payloadless config RED**

Extend the existing config fixture with an uninstall counter. After install, no payload directory is required. Remove must increment exactly once. Removing a never-installed config coordinate must still fail and leave the counter unchanged.

- [x] **Step 3: Run RED**

```bash
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/remove_foreign_provider_delegator_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/config_install_no_implicit_dir_test.sh
```

Expected current-main failures: D4 sees B's target as nonempty; command-level disk/workspace guard rejects payloadless config before uninstall.

- [x] **Step 4: Implement provider ownership and narrow recipe proof**

Scan target versions and compare only non-malformed `bindingGroup->provider` to `executingProvider`. Preserve fail behavior when this provider owns a mismatched exact version. Widen the early command guard only when the selected local/snapshot executable recipe proves the payloadless uninstall path; do not turn unknown absence into success.

- [x] **Step 5: Run removal matrix and commit**

```bash
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/remove_foreign_provider_delegator_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/config_install_no_implicit_dir_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/remove_orphan_payload_test.sh
git diff --check
git add src/core/xim/installer.cppm src/core/xim/commands.cppm tests/e2e/remove_foreign_provider_delegator_test.sh tests/e2e/config_install_no_implicit_dir_test.sh tests/e2e/run_all.sh
git commit -m "fix(remove): select versions by executing provider"
```

**Delivered:** `1436867`. The foreign-provider preservation, payloadless
config and orphan removal E2Es passed; the focused XVM removal matrix passed
8/8 and the full unit gate passed 36/36.

### Task 10: xlings `subos use` candidate discovery and safe fuzzy match

**Repository:** `openxlings/xlings` (isolated worktree)

**Files:**
- Modify: `src/cli/spec.cppm`
- Modify: `src/core/subos.cppm`
- Modify: `src/capabilities.cppm`
- Modify: `src/cli.cppm`
- Modify: `src/agent/text_renderer.cppm`
- Modify: `src/ui/info_panel.cppm`
- Modify: `docs/generated/command-reference.md`
- Create: `tests/e2e/subos_use_candidates_test.sh`
- Modify: `tests/unit/test_command_spec.cpp`
- Modify: `tests/e2e/subos_events_test.sh`
- Modify: `tests/e2e/run_all.sh`

**Interfaces:**
- Produces: shared sorted `SubosCandidateView` consumed by list, use validation/resolution and interface capability.
- Produces match order: case-sensitive exact; case-insensitive exact if unique; unique case-insensitive prefix; substring/edit-distance suggestions only.
- Exit codes: no name 0; ambiguous 2; not found 1; selected command/shell preserves downstream exit.
- DataEvent: `subos_candidates {reason,query,candidates:[{name,active,dir,pkgCount}],auto_selected,selected?,hint?}`.

- [x] **Step 1: Write candidate behavior RED E2E**

Create `alpha`, `alpine`, `beta`. Snapshot home manifest and current link. Assert no-arg returns 0, lists all, emits no shell env and mutates nothing; exact beta works; `bet --cmd 'printf MATCHED'` resolves beta; `alp` returns 2 and names both without executing; unknown returns 1 with suggestions.

- [x] **Step 2: Add legacy default and spec RED**

Create `subos/default/.xlings.json` without registry entry; list/use must expose exactly one synthesized default without writing home config. Change command spec name argument to optional and assert manual validation accepts an empty argv.

- [x] **Step 3: Run RED**

```bash
mcpp test -- --gtest_filter='CommandSpec.*'
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/subos_use_candidates_test.sh
```

Expected current-main failure: command spec and parser reject missing name; validation is exact-only.

- [x] **Step 4: Implement one candidate source/resolver**

Refactor `list_all()` into the shared view. Synthesize default only when its manifest exists and registry lacks it; never scan arbitrary directories. Resolve the name once before dispatching global/shell/spawn/sandbox/cmd/gpu. No-name routes to the same renderer/event as list and exits 0.

- [x] **Step 5: Run SubOS modes and commit**

```bash
mcpp test -- --gtest_filter='CommandSpec.*'
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/subos_use_candidates_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/subos_xpkg_use_cmd_test.sh
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/subos_shell_level_test.sh
git diff --check
git add src/cli/spec.cppm src/core/subos.cppm src/capabilities.cppm tests/e2e/subos_use_candidates_test.sh tests/unit/test_command_spec.cpp tests/e2e/run_all.sh
git commit -m "fix(subos): discover and resolve use candidates safely"
```

**Delivered:** `1fee812`. CommandSpec passed 7/7; generated command reference
and CLI spec parity passed (240 option spellings and 64 parser literals); the
full unit gate passed 36/36. Candidate discovery plus the five affected SubOS
and local-query E2Es passed with an absolute candidate binary and fail-fast
execution.

### Task 11: xlings integrated gate, squash merge, release, mirror and index pointer A

**Repository:** `openxlings/xlings` (isolated worktree)

**Files:**
- Modify: `mcpp.toml`
- Modify: `src/core/config.cppm`
- Modify: `.agents/docs/2026-08-09-stability-regression-recovery-design.md`
- Include: `.agents/plans/2026-08-09-stability-regression-recovery-implementation.md`
- Downstream PR A: `xim-pkgindex/pkgs/x/xlings.lua` and `latest.ref`

**Interfaces:**
- Consumes: Tasks 4-10 and released libxpkg.
- Produces: one xlings Draft PR and one released/publicly installable xlings version.
- Expected version: `2026.8.9.3` only if it remains the next ordinary version at cut time.

- [ ] **Step 1: Rebase-free integration audit**

Fetch origin and verify the branch base remains an ancestor; merge origin/main normally if needed, resolve without rewriting task commits, and rerun focused tests affected by conflicts. Confirm only intended files are present with `git status --short` and `git diff --check`.

- [ ] **Step 2: Run complete local Linux gates**

```bash
env -u NO_COLOR TERM=xterm mcpp test
XLINGS_BIN="$(find target -type f -path '*/bin/xlings' -perm -111 | head -1)"
XLINGS_BIN="$XLINGS_BIN" bash tests/e2e/run_all.sh
bash tests/fresh-install/no_xlings_version_pin_check.sh
```

Also run a synthetic 100-package/30-SubOS benchmark and record median/p95, structural network/process/payload counters, and first-output latency. No timing pass may waive a structural violation.

- [ ] **Step 3: Version bump and release-candidate build**

Re-fetch tags/releases, choose the next ordinary version, update both `mcpp.toml` and `Config::VERSION`, build the Linux static release with `tools/linux_release.sh`, and verify its `--version`, local query, doctor quick/deep, #513/#514/#506 and SubOS candidate contracts in an isolated home.

- [ ] **Step 4: Push one integrated Draft PR**

The PR body must contain a task-to-issue matrix, dependency on exact libxpkg release, before/after timings, compatibility notes, native/hardware evidence boundaries, and release dependency chain. Preserve small commits locally; do not force-push, amend or rebase.

- [ ] **Step 5: Wait for every xlings native check**

Require Linux, Linux E2E/root, macOS, Windows, aarch64 and policy gates at the final head SHA. If a check fails, use the GitHub CI debugging skill, reproduce the failing native contract where possible, add a regression test, push an additive fix commit, and restart the final-head audit.

- [ ] **Step 6: Authorized bypass squash merge and publish release**

Use admin/bypass only after all required checks are terminal green. Squash merge, verify merged main contains the intended diff, trigger release workflow for the exact version, and wait for all platform assets and sidecars. A green release job with missing assets is failure.

- [ ] **Step 7: Top up GitCode and verify GET+sha256**

```bash
bash tools/mirror-latest.sh xlings
```

For Linux x86_64/aarch64, macOS arm64 and Windows x86_64, perform GET (not HEAD) from GitHub and GitCode, follow redirects, and compare downloaded sha256 with release sidecars.

- [ ] **Step 8: Merge index pointer A immediately**

Run the repository release/index bump flow against the released xlings version, verify all platform blocks and `latest`, open the minimal xim-pkgindex PR A, require its CI green, bypass squash merge, then verify a cold public `quick_install`/index resolution obtains exactly the new version. Do not pin xlings in fresh-install CI.

### Task 12: mcpp #392 exact compiler-owner fixup with coexisting runtimes

**Repository:** `mcpp-community/mcpp` (isolated worktree)

**Files:**
- Modify: `src/xlings.cppm`
- Modify: `src/toolchain/post_install.cppm`
- Modify: `src/build/prepare.cppm`
- Modify: `src/toolchain/lifecycle.cppm`
- Modify: `tests/unit/test_runtime_binding_fallback.cpp`
- Create: `tests/e2e/167_fixup_uses_owner_runtime.sh`
- Register in the native E2E runner

**Interfaces:**
- Produces: exact compiler-owner/default-SubOS runtime lookup; it is distinct from the current project's target runtime.
- Changes `ensure_post_install_fixup` to consume an exact owner binding/resolved `PayloadPaths`; deletes `find_sandbox_glibc_lib`.
- Increments `kFixupRev` so old arbitrary fingerprints cannot suppress correction.

- [ ] **Step 1: Add exact-authority unit RED cases**

With 2.39 and 2.44 payload directories present, assert explicit owner binding 2.44 returns 2.44, explicit 2.39 returns 2.39, and empty authority with multiple payloads refuses. Reverse directory creation order and require identical results.

- [ ] **Step 2: Add shared-compiler alternating-build RED E2E**

Seed one compiler owned by default/2.44 and a sibling project SubOS bound to 2.39. Alternate both clean builds repeatedly. Shared compiler cfg, PT_INTERP and `.mcpp-fixup.json.glibcLib` must remain 2.44; each project target must name its own runtime; both payloads remain.

- [ ] **Step 3: Run RED**

```bash
mcpp test -- --gtest_filter='*RuntimeBinding*'
MCPP="$(find target -type f -path '*/bin/mcpp' -perm -111 | head -1)"
MCPP="$MCPP" bash tests/e2e/167_fixup_uses_owner_runtime.sh
```

Expected current-main failure: post-install fixup takes the first `directory_iterator` entry.

- [ ] **Step 4: Thread the exact owner authority**

Read the compiler owner's/default SubOS manifest from the compiler's registry home, resolve exact payload through existing `probe_payload_paths(compilerBin,binding)`, and pass it to all fixup call sites. The current project's runtime remains the later target link-model authority. On missing/ambiguous owner authority, emit a typed diagnostic and do not rewrite.

- [ ] **Step 5: Run runtime suites and commit**

```bash
mcpp test -- --gtest_filter='*RuntimeBinding*'
mcpp test -- --gtest_filter='*ToolchainProbe*'
MCPP="$MCPP" bash tests/e2e/167_fixup_uses_owner_runtime.sh
git diff --check
git add src/xlings.cppm src/toolchain/post_install.cppm src/build/prepare.cppm src/toolchain/lifecycle.cppm tests/unit/test_runtime_binding_fallback.cpp tests/e2e/167_fixup_uses_owner_runtime.sh
git commit -m "fix(toolchain): bind shared fixups to their owner runtime"
```

### Task 13: mcpp target environment safety and real #514 integration

**Repository:** `mcpp-community/mcpp` (isolated worktree)

**Files:**
- Modify: `src/build/plan.cppm`
- Modify: `src/build/execute.cppm`
- Modify: `src/platform/process.cppm` only if the shared content predicate belongs there
- Modify: `tests/e2e/166_run_env_no_private_glibc.sh`
- Create: `tests/e2e/168_shared_registry_hook_reuse.sh`
- Register both run/test paths in native E2E

**Interfaces:**
- Runtime-library environment retains non-core dependency libdirs and excludes any directory whose resolved contents include libc/loader.
- Same plan field drives `mcpp run` and `mcpp test`; cached and uncached paths agree.
- #514 test consumes xlings/libxpkg behavior; mcpp adds no `MCPP_HOME` path inference.

- [ ] **Step 1: Turn unsafe positive test into RED**

Update test 166 so a runtime dependency's non-core directory remains but no private glibc directory appears. The target must execute `system("/bin/sh -c 'printf CHILD_OK'")` and print `CHILD_OK`. Add the same grandchild assertion to `mcpp test`.

- [ ] **Step 2: Run RED**

```bash
MCPP="$MCPP" bash tests/e2e/166_run_env_no_private_glibc.sh
```

Expected current-main failure: plan appends `tc.payloadPaths->glibcLib` and the host-loader grandchild inherits it.

- [ ] **Step 3: Remove core runtime from process-global env**

Delete the glibc append. Before emitting `LD_LIBRARY_PATH`, filter every directory with a content-based libc/loader predicate so renamed/namespaced payloads are covered. Keep dependency and compiler non-core runtime directories.

- [ ] **Step 4: Add real shared-registry #514 integration**

Create a custom-index member whose `mcpp.deps` preinstalls `compat:zlib@1.3.2` into the mcpp registry while its consumer installs project-locally. The hook records `pkginfo.install_dir`; assert it is the exact registry payload and a same-bare-name foreign namespace decoy is ignored.

- [ ] **Step 5: Run run/test/reuse gates and commit**

```bash
MCPP="$MCPP" bash tests/e2e/166_run_env_no_private_glibc.sh
MCPP="$MCPP" bash tests/e2e/168_shared_registry_hook_reuse.sh
mcpp test -- --gtest_filter='*PlatformEnv*'
git diff --check
git add src/build/plan.cppm src/build/execute.cppm src/platform/process.cppm tests/e2e/166_run_env_no_private_glibc.sh tests/e2e/168_shared_registry_hook_reuse.sh
git commit -m "fix(runtime): keep private libc out of target environments"
```

### Task 14: mcpp integrated PR, native CI, release and mirror

**Repository:** `mcpp-community/mcpp` (isolated worktree)

**Files:**
- Modify: `mcpp.toml`
- Modify: `src/version.cppm`
- Update exact xlings pins only where mcpp release policy requires its fixed published consumer

**Interfaces:**
- Consumes: Tasks 12-13 and publicly released xlings from Task 11.
- Produces: one mcpp PR and release; expected `2026.8.9.1` if it remains the first ordinary release that day.

- [ ] **Step 1: Full local and isolated #392 acceptance**

Run all unit/E2E suites, then a real two-glibc/two-SubOS/shared-compiler case using the published xlings/index. Inspect cfg, PT_INTERP, RPATH, `LD_LIBRARY_PATH`, dependency load, host `/bin/sh` grandchild and byte stability across alternating clean builds.

- [ ] **Step 2: Version, Draft PR and final-head native CI**

Compute next date version, update both version sources, push one Draft PR, and require Linux, macOS, Windows, aarch64, fresh-install and release-policy checks terminal green. Keep hardware claims out of this PR unless a capable runner actually ran them.

- [ ] **Step 3: Authorized bypass squash merge and release**

Squash merge after all checks, publish all platform assets, verify public downloads and cold mcpp install, then:

```bash
bash tools/mirror-latest.sh mcpp  # from an xlings checkout
```

Verify GitHub/GitCode GET and sha256 for each mcpp asset.

### Task 15: xim-pkgindex #506/graphics ecosystem closure and pointer B

**Repository:** `openxlings/xim-pkgindex` (isolated worktree)

**Files:**
- Modify: `.github/scripts/windows-test.ps1`
- Modify: `.agents/tools/graphics/verify-stack.sh`
- Modify: `pkgs/n/nvidia-gl-host-link.lua`
- Modify: `pkgs/m/mcpp.lua`
- Create: `tests/graphics/test_verify_stack_targets_named_subos.sh`
- Modify: `tests/n/test_nvidia_gl_host_link.py`
- Update generated/index latest artifacts through repository tooling

**Interfaces:**
- Deletes the delegated-gcc #506 tolerance; real uninstall marker/preservation becomes a hard native Windows gate.
- `verify-stack.sh --subos NAME` uses explicit named scope for install, query, mcpp build and sandbox probe.
- NVIDIA reports `built` separately from `closure_complete`, records unresolved SONAMEs, and fails the empty-host acceptance cell when unresolved is nonempty.

- [ ] **Step 1: Add named-scope and unresolved-closure RED tests**

Set global active SubOS to default, invoke verifier for gfxverify with a stub/isolated real xlings, and assert every scope-sensitive call sees gfxverify. Mock one unresolved NVIDIA SONAME and require summary/doctor to say incomplete rather than complete.

- [ ] **Step 2: Run RED**

```bash
bash tests/graphics/test_verify_stack_targets_named_subos.sh
/usr/bin/python3 -m pytest -q tests/n/test_nvidia_gl_host_link.py
```

- [ ] **Step 3: Fix verifier/recipe and remove #506 tolerance**

Use an `XLINGS_ACTIVE_SUBOS="$SUBOS"` wrapper for install/query/build setup, unique per-run logs, and explicit post-install workspace/runtime assertions. Capture libxpkg interposer result and persist unresolved names. Remove the Windows try/catch/tolerance around delegated gcc removal so the test requires success and foreign-provider preservation.

- [ ] **Step 4: Update mcpp release pointer and run index gates**

Apply the released mcpp version/hashes/latest; preserve the already-merged xlings pointer. Run index build, version-check, Lua/static recipe tests, POSIX lifecycle and native Windows lifecycle.

- [ ] **Step 5: Push one ecosystem PR B and merge**

PR body lists exact released xlings/libxpkg/mcpp refs, #506/#392 links, graphics PASS/SKIP cells and resource hashes. Require all native checks terminal green, use authorized squash merge, publish index artifacts/pointers, and verify GLOBAL/CN GET+sha256.

### Task 16: Public cold-home, old-home and real SubOS sandbox audit

**Repositories:** released xlings/libxpkg/mcpp and merged xim-pkgindex/xlings-res only; source worktrees are evidence references, not binaries under test.

**Artifacts:**
- Create/update evidence section in `.agents/docs/2026-08-09-stability-regression-recovery-design.md`
- Post concise closure comments to xlings #506/#513/#514 and mcpp #392 with exact refs and evidence
- Keep architecture discussion in xlings #518

- [ ] **Step 1: Cold public install**

Use a new temp root and public `quick_install`/latest index. Verify exact xlings/mcpp versions, package install, #513 interface NDJSON, #514 shared registry, #506 remove and no hidden query fetch.

- [ ] **Step 2: Real sandbox command path**

Create a fresh named SubOS and run at minimum:

```bash
xlings subos use gfxverify --sandbox --cmd 'xlings --version'
xlings subos use gfxverify --sandbox --cmd 'mcpp --version'
xlings subos use gfxverify --sandbox --cmd '<llvmpipe pixel and Wayland probe>'
```

The 2026-08-09 preflight host exposes an NVIDIA GeForce RTX 4080, driver 550.144.03, `/dev/nvidia0` and readable `/dev/dri/renderD128`; unless that external state disappears, run the NVIDIA `--sandbox --gpu --cmd` cell and capture renderer, device nodes, loaded-object provenance, pixel output and interposer closure. Record AMD/Intel/nouveau/WSL2 as SKIP unless a matching native runner/device is actually exercised; never promote llvmpipe to hardware pass.

- [ ] **Step 3: Long-lived and legacy-home read-only audit**

Run released `list`, `info gcc`, quick doctor and deep scoped doctor against a read-only snapshot or the real home without mutation. Record same-machine warm medians and compare to 2026.8.2.1 baseline; require immediate first output and no network/child recursion. Exercise legacy default SubOS candidate compatibility in an isolated copy.

- [ ] **Step 4: Ecosystem provenance audit**

Verify two glibc payloads still coexist while each SubOS reports exactly one active runtime; inspect mcpp target/shared compiler runtime split; validate Mesa/ncurses/Wayland payload contents and DT_NEEDED closure; verify GitHub/GitCode/index resource hashes.

- [ ] **Step 5: Final clean-state and issue closure**

Verify remote/local merged SHAs, tags, release assets, clean intended worktrees, `git diff --check`, public latest refs and all CI terminal states. Update the design evidence table, close only issues whose original reproduction is green, and leave unavailable hardware cells explicitly open/not exercised.
