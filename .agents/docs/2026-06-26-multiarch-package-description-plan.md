# Multi-Arch xpm (V2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> Companion design/rationale doc: [`2026-06-26-multiarch-package-description-design.md`](./2026-06-26-multiarch-package-description-design.md)

**Goal:** Make CPU architecture a first-class, declarative, install-time-resolved dimension of the xpm package schema (V2), so one package recipe can correctly serve `x86_64` and `aarch64` binaries with per-arch checksums — and run the full ecosystem loop (libxpkg release → xlings release → index publish → `xlings update`).

**Architecture:** A package's `xpm[platform][version]` value gains three new *declarative shapes* (per-arch map, URL template + per-arch sha256, and `XLINGS_RES`-with-sha256), detected by table shape so legacy recipes are untouched. Both xpm parsers (libxpkg `xpkg-loader.cppm` and xlings `installer.cppm` sandbox loader) store the raw, arch-agnostic data; **the host arch is resolved only at install time** (exactly like `XLINGS_RES` today), keeping the shared index artifact arch-neutral. A single arch-normalization table (`arm64↔aarch64`, `amd64↔x86_64`) and fail-closed `package.archs` validation remove the current silent mis-install bug.

**Tech Stack:** C++23 modules (GCC 16 / MSVC), xmake + mcpp build, GoogleTest (libxpkg) + xlings unit harness + bash e2e harness, Lua 5.4 recipes, GitHub Actions, GitHub/GitCode release artifacts.

## Global Constraints

- **`package` table stays statically evaluable** — V1 rule: only literals + `string.format()`, no runtime calls in the `package`/`xpm` table. The arch dimension MUST be declarative data keys, NOT `if is_arch()` logic. (Verbatim from design doc §1.5.)
- **Backward compatible** — every existing recipe shape (`"XLINGS_RES"`, `"url-string"`, `{url=..,sha256=..,ref=..}`, mirror map `{GLOBAL=,CN=}`) keeps identical behavior. New behavior is opt-in by new table shape.
- **Arch resolves at INSTALL time on the client, never at index-build time** — the index-as-resource artifact is built once by CI and downloaded by all arches; baking the builder's arch would mis-serve everyone.
- **No artifact without its checksum** — every new per-arch shape must carry a per-arch `sha256` slot; the schema makes "add an arch, forget its hash" structurally hard. (`XLINGS_RES` bare string remains the one grandfathered hash-less form.)
- **Two parsers, kept in lockstep** — `libxpkg/src/xpkg-loader.cppm:296-325` AND `xlings/src/core/xim/installer.cppm::load_platform_entries_` (≈831-920) must accept the identical new shapes. A change to one without the other is a bug.
- **Canonical arch names** internally = `x86_64`, `aarch64`. Accept aliases on input (`amd64`,`x64`,`x86-64`→`x86_64`; `arm64`,`armv8`→`aarch64`). `xpm` arch keys and `package.archs` use canonical names.
- **Conventional commits, PR-per-feature, squash-merge to main.** Types: `feat/fix/chore/docs/test/ci`. CI must pass Linux+macOS+Windows (+aarch64). Never `--no-verify`.
- **Version SoT must stay in sync** — xlings: `src/core/config.cppm:16` `VERSION` AND `mcpp.toml:3` `version` (currently drifted 0.4.60 vs 0.4.59 — fix in the release task). libxpkg: git tag `v0.0.N` + `mcpplibs-index` entry (authoritative) and `mcpp.toml`.

---

## Cross-Repo Map & Parallelization

Three repos participate. **⇄ = can be developed in parallel** under the local override; **→ = hard sequential dependency at landing/release time.**

```
 libxpkg (openxlings/libxpkg)            xlings (openxlings/xlings)              xim-pkgindex (gitee:sunrisepeak / openxlings)
 Phase A: loader V2 schema      ⇄        Phase B: install-time resolution  ⇄    Phase D: V2 spec + recipe fixes
   tag v0.0.42                  ───→       bump add_requires pin 0.0.42  ───→     (recipes only need V2 once xlings 0.4.61 ships)
   publish to mcpplibs-index    ───→       release 0.4.61                ───→     publish index artifact → xlings update
```

- **DEV phase is fully parallel:** build xlings against in-flight libxpkg source with
  `xmake f --local_libxpkg=/home/speak/workspace/github/mcpplibs/libxpkg` (xlings `xmake.lua:42-46`). No release needed to co-develop + test Phases A+B+D together.
- **LANDING phase is sequential (the closure):**
  1. Land + tag **libxpkg `v0.0.42`** → publish `mcpplibs-xpkg 0.0.42` to **`mcpplibs/mcpplibs-index`** (separate repo; libxpkg's own `release.yml` is stale, so this is a manual `git tag` + index-PR step).
  2. Bump xlings `xmake.lua:45` `add_requires("mcpplibs-xpkg 0.0.42")` (in the feature PR or a dedicated pin-bump PR).
  3. Merge xlings feature PR(s) (CI gate: linux, linux-e2e, linux-root, macos, windows, aarch64).
  4. `chore(release): bump version to 0.4.61` PR editing `config.cppm:16` + `mcpp.toml:3` → merge → manually run the **`Release`** `workflow_dispatch` → tags `v0.4.61`, builds 4 platforms, `publish-index` job republishes index artifacts to `xlings-res/xim-index` (GH+GitCode).
  5. Land V2 recipes in **xim-pkgindex** (can land anytime ≥ step 3; clients only parse V2 once running 0.4.61) → publish index artifact (`tools/publish_xim_index.sh` or the repo's `publish-sub-indexes.yml`) → **`xlings update`** on a client pulls the new index → **ecosystem closed**.

> **HUMAN-GATED steps (require secrets / are by-design manual / irreversible):** triggering the `Release` `workflow_dispatch`; index publish needing `XLINGS_RES_TOKEN`/`GITCODE_TOKEN`; pushing to `mcpplibs-index` and remote PRs. The plan PREPARES all of these (branches, commits, version bumps, exact commands, PR bodies); a maintainer fires the final triggers.

---

## File Structure

**libxpkg** (`/home/speak/workspace/github/mcpplibs/libxpkg`):
- Modify `src/xpkg.cppm` — extend `PlatformResource` with V2 fields (per-arch map, sha256-by-arch, arch_alias, is_res).
- Modify `src/xpkg-loader.cppm:296-325` — parse the new shapes into those fields.
- Create `src/arch.cppm` (or fold into existing util module) — `normalize_arch()` + alias table. One responsibility: arch name canonicalization. Shared so xlings can mirror it.
- Modify `tests/test_loader.cppm` — V2 parse tests (GoogleTest).
- Create `tests/fixtures/pkgindex/pkgs/_v2/*.lua` — fixture recipes exercising each new shape.

**xlings** (`/home/speak/workspace/github/openxlings/xlings`):
- Modify `src/core/xim/installer.cppm` — (a) `normalize_arch_()` wrapping `detect_arch_()` (123-140); (b) `load_platform_entries_` sandbox parser (≈831-920) accept new shapes; (c) install-time arch resolution in the download path (≈1012-1065, where `XLINGS_RES`/mirror are resolved); (d) `package.archs` fail-closed validation before download.
- Modify `src/core/xim/index.cppm:40-100` — serialize/deserialize the new resource fields so the index cache round-trips raw arch data.
- Modify `src/core/xim/resolver.cppm` — thread arch into dep/version resolution only where it already reads resources (no arch logic needed if resolution stays install-time; verify it passes resources through opaquely).
- Modify `xmake.lua:45` — bump `mcpplibs-xpkg` pin to `0.0.42` (landing).
- Modify `src/core/config.cppm:16` + `mcpp.toml:3` — version bump to `0.4.61` (release task).
- Create `tests/e2e/multiarch_resolution_test.sh` — e2e: a fixture multi-arch recipe resolves to the host arch's URL+sha256.
- Modify xlings unit test harness (`tests/unit/test_main.cpp` per recon) — add arch-normalization + validation unit tests.

**xim-pkgindex** (`/home/speak/workspace/github/openxlings/xlings/.xlings-home-dev/data/xim-pkgindex`, remote gitee:sunrisepeak / openxlings):
- Create `docs/V2/xpackage-spec.md` + `docs/V2/xpackage-template.lua` (derive from V1, add arch shapes).
- Modify `pkgs/g/github-gh.lua`, `pkgs/n/node.lua` — fix the broken/inconsistent arch declarations using V2 shapes (these are the documented bugs).
- Modify `pkgs/c/cmake.lua`, `pkgs/b/bun.lua` — migrate to V2 `archs`/`res+sha256` where they currently lean on `XLINGS_RES` or npm.
- Create/modify `docs/CHANGELOG.md` (or `docs/V2/changes.md`) — **record version changes** per the user's requirement.

**Design/plan docs (this repo, dynamic updates):**
- Update `2026-06-26-multiarch-package-description-design.md` — append "V2 final schema (as built)" + version-change log as implementation lands.
- Keep this plan's checkboxes current as tasks complete.

---

## Phase A — libxpkg V2 loader schema  *(parallel-able with B & D)*

### Task A1: Arch normalization utility

**Files:**
- Create: `/home/speak/workspace/github/mcpplibs/libxpkg/src/arch.cppm`
- Test: `/home/speak/workspace/github/mcpplibs/libxpkg/tests/test_loader.cppm` (add an `ArchTest` suite, or a new `tests/test_arch.cppm` wired in `tests/xmake.lua`)

**Interfaces:**
- Produces: `std::string xlings::xpkg::normalize_arch(std::string_view raw)` → canonical (`x86_64`|`aarch64`|`x86`|raw-lowered if unknown). Also `bool xlings::xpkg::arch_matches(std::string_view a, std::string_view b)` (normalize both, compare).

- [ ] **Step 1: Write the failing test**
```cpp
TEST(ArchTest, NormalizesAliases) {
    using xlings::xpkg::normalize_arch;
    EXPECT_EQ(normalize_arch("amd64"),  "x86_64");
    EXPECT_EQ(normalize_arch("x64"),    "x86_64");
    EXPECT_EQ(normalize_arch("x86-64"), "x86_64");
    EXPECT_EQ(normalize_arch("arm64"),  "aarch64");
    EXPECT_EQ(normalize_arch("armv8"),  "aarch64");
    EXPECT_EQ(normalize_arch("x86_64"), "x86_64");   // canonical passthrough
    EXPECT_EQ(normalize_arch("AArch64"),"aarch64");  // case-insensitive
}
```
- [ ] **Step 2: Run, expect FAIL** — `cd /home/speak/workspace/github/mcpplibs/libxpkg && xmake build xpkg_loader_test` → unresolved `normalize_arch`.
- [ ] **Step 3: Implement `src/arch.cppm`**
```cpp
export module xlings.xpkg.arch;
import std;
export namespace xlings::xpkg {
inline std::string normalize_arch(std::string_view raw) {
    std::string s; s.reserve(raw.size());
    for (char c : raw) s += static_cast<char>(std::tolower((unsigned char)c));
    if (s == "amd64" || s == "x64" || s == "x86-64" || s == "x86_64") return "x86_64";
    if (s == "arm64" || s == "armv8" || s == "aarch64")               return "aarch64";
    if (s == "x86"   || s == "i386" || s == "i686")                   return "x86";
    return s;
}
inline bool arch_matches(std::string_view a, std::string_view b) {
    return normalize_arch(a) == normalize_arch(b);
}
}
```
- [ ] **Step 4: Wire module into `src/xmake.lua` / `tests/xmake.lua`** (add `add_files("src/arch.cppm")` to the loader target's sources; ensure tests `add_deps` it).
- [ ] **Step 5: Run, expect PASS.**
- [ ] **Step 6: Commit** — `git commit -m "feat(arch): add arch name normalization (x86_64/aarch64 aliases)"`

### Task A2: Extend `PlatformResource` with V2 fields

**Files:** Modify `/home/speak/workspace/github/mcpplibs/libxpkg/src/xpkg.cppm:15-20`

**Interfaces:**
- Produces: extended `PlatformResource`. New fields default-empty so legacy parse is unchanged.
```cpp
struct PlatformResource {
    std::string url;        // single url | template ("...${arch}...") | "XLINGS_RES" | "" when `archs` used
    std::string sha256;     // single-arch sha256 | "" when sha256_by_arch used
    std::string ref;        // version alias
    std::unordered_map<std::string,std::string> mirrors;          // region: GLOBAL/CN
    // ---- V2 additions (all empty => legacy V1 resource) ----
    std::unordered_map<std::string, PlatformResource> archs;      // canonical-arch -> {url,sha256,mirrors}  (Scheme B)
    std::unordered_map<std::string,std::string> sha256_by_arch;   // canonical-arch -> sha256  (Scheme C/res)
    std::unordered_map<std::string,std::string> arch_alias;       // canonical-arch -> upstream token for ${arch_alias}
    bool is_res { false };                                        // res=true (XLINGS_RES with checksums)
};
```
- [ ] **Step 1:** Add the four fields (above). Build the model target: `xmake build mcpplibs-xpkg`. Expected PASS (additive).
- [ ] **Step 2: Commit** — `git commit -m "feat(xpkg): add V2 per-arch fields to PlatformResource"`

### Task A3: Parse Scheme B (per-arch map) in the loader

**Files:** Modify `/home/speak/workspace/github/mcpplibs/libxpkg/src/xpkg-loader.cppm:296-325`; fixture `tests/fixtures/pkgindex/pkgs/.../v2b.lua`; test `tests/test_loader.cppm`.

**Interfaces:**
- Consumes: `normalize_arch` (A1), extended `PlatformResource` (A2).
- Shape detected: version value is a TABLE with NO `url`/`ref`/`sha256` keys but ≥1 key whose `normalize_arch(key)` ∈ known arches. Each sub-value parsed as a resource (`url` str|mirror-map + `sha256`).

- [ ] **Step 1: Fixture recipe** `tests/fixtures/.../pkgs/v/v2map.lua`:
```lua
package = { spec = "2", name = "v2map", type = "package", archs = {"x86_64","aarch64"},
  xpm = { linux = { ["1.0.0"] = {
      x86_64  = { url = "https://ex/x.tgz", sha256 = "aaaa" },
      aarch64 = { url = "https://ex/a.tgz", sha256 = "bbbb" },
  } } } }
```
- [ ] **Step 2: Failing test**
```cpp
TEST(LoaderTest, V2_PerArchMap_ParsesBothArches) {
    auto pkg = load_fixture("v2map");
    auto& r = pkg.xpm.entries.at("linux").at("1.0.0");
    ASSERT_EQ(r.archs.size(), 2u);
    EXPECT_EQ(r.archs.at("x86_64").url, "https://ex/x.tgz");
    EXPECT_EQ(r.archs.at("x86_64").sha256, "aaaa");
    EXPECT_EQ(r.archs.at("aarch64").url, "https://ex/a.tgz");
    EXPECT_TRUE(r.url.empty());  // no single-arch url
}
```
- [ ] **Step 3: Run, expect FAIL** (`archs` empty).
- [ ] **Step 4: Implement** in the version-entry TABLE branch (after the existing `url`/`sha256`/`ref` reads): if `url`/`ref`/`sha256` all empty, iterate the table's string keys; for each key K where `normalize_arch(K)` is a known arch, recurse-parse its value (reuse the existing `url`-string-or-mirror-map + `sha256` logic) into `res.archs[normalize_arch(K)]`. Keep the existing reserved-key skip (`deps`,`inherits`) and add `exports`,`runtime`,`build` to the skip-list (recon flagged they currently leak).
- [ ] **Step 5: Run, expect PASS.**
- [ ] **Step 6: Commit** — `git commit -m "feat(xpkg): parse V2 per-arch resource map"`

### Task A4: Parse Scheme C (template + sha256-by-arch) and `res=true`

**Files:** same loader + new fixtures + tests.

**Interfaces:** Shape C detected: TABLE has `url` (string) AND `sha256` is a TABLE. Shape res detected: TABLE has `res == true`.
- [ ] **Step 1: Fixtures** `v2tmpl.lua` (`url="https://ex/${name}-${version}-${os}-${arch_alias}.${ext}"`, `sha256={x86_64="a",aarch64="b"}`, `arch_alias={aarch64="arm64"}`) and `v2res.lua` (`["1.0.0"]={ res=true, sha256={x86_64="a",aarch64="b"} }`).
- [ ] **Step 2: Failing tests** asserting `r.url=="...template..."`, `r.sha256_by_arch.at("aarch64")=="b"`, `r.arch_alias.at("aarch64")=="arm64"`; and for res: `r.is_res==true`, `r.sha256_by_arch.size()==2`.
- [ ] **Step 3: Run, expect FAIL.**
- [ ] **Step 4: Implement** — when `sha256` is a Lua table, read it via `get_str_map` into `sha256_by_arch` (normalize keys); read optional `arch_alias` map; read `res` boolean → `is_res`. Leave `url` as the literal template string (no expansion in the loader — install-time only).
- [ ] **Step 5: Run, expect PASS.**
- [ ] **Step 6: Commit** — `git commit -m "feat(xpkg): parse V2 URL template + per-arch sha256 + res shape"`

### Task A5: libxpkg build + full test + tag prep

- [ ] **Step 1:** `mcpp test` (or `xmake build && xmake run xpkg_loader_test`) — all green (CI filters macOS-only `ExecutorTest.ApplyElfpatchAuto_*`).
- [ ] **Step 2:** Bump `mcpp.toml` version `0.0.39`→`0.0.42` to match the intended tag (note: tag is authoritative, but keep metadata aligned).
- [ ] **Step 3: Commit** — `git commit -m "chore: bump libxpkg to 0.0.42 (V2 per-arch xpm schema)"`. **Do NOT tag/push yet** — tagging is the landing step (Phase E).

---

## Phase B — xlings install-time arch resolution  *(parallel-able; build via `--local_libxpkg`)*

### Task B1: `normalize_arch_()` in xlings

**Files:** Modify `installer.cppm` (add near `detect_arch_()` 123-140); Test: xlings unit harness.
- [ ] **Step 1: Failing unit test** (mirror A1 cases) for a new `detail_::normalize_arch_(std::string_view)`.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `normalize_arch_` identical to A1's table; refactor `detect_arch_()` to return canonical (`aarch64` even on Apple) and add `detect_arch_raw_()` that preserves Apple `arm64` ONLY for legacy `XLINGS_RES` bare-string expansion (keep existing artifact URLs working).
- [ ] **Step 4: Run, expect PASS. Commit** — `feat(xim): add arch normalization helper`.

### Task B2: Sandbox loader parses V2 shapes (lockstep with A3/A4)

**Files:** Modify `installer.cppm::load_platform_entries_` (≈831-920).
- [ ] **Step 1:** Extend the local resource struct used by the sandbox loader to carry `archs` / `sha256_by_arch` / `arch_alias` / `is_res` (mirror A2).
- [ ] **Step 2: Failing test** via an existing loader-level test or a new fixture under `tests/fixtures/xim-pkgindex/pkgs/` parsed through the install path.
- [ ] **Step 3: Implement** the same shape detection as A3/A4 (per-arch map; template+sha256-table; res=true). **Lockstep:** keep field names identical to libxpkg.
- [ ] **Step 4: Run, expect PASS. Commit** — `feat(xim): parse V2 per-arch xpm shapes in sandbox loader`.

### Task B3: Install-time arch resolution

**Files:** Modify `installer.cppm` download path (≈1012-1065, the `XLINGS_RES`/mirror resolution site).

**Interfaces:** new `detail_::resolve_arch_resource_(const Resource& r, std::string_view host_arch) -> {url, sha256, fallbacks}`.
- [ ] **Step 1: Failing test** — given a Scheme-B resource and `host_arch="aarch64"`, returns the aarch64 url+sha256; given Scheme-C, interpolates `${arch}`/`${arch_alias}`/`${version}`/`${os}`/`${ext}` and returns `sha256_by_arch[host]`; given res, builds the `XLINGS_RES` URL for host + attaches `sha256_by_arch[host]`; **unknown arch → error/empty (fail-closed).**
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `resolve_arch_resource_` (order: ref already followed upstream; then archs map; then template via existing `build_xlings_res_url_`-style interpolation generalized to a `${...}` substituter; then is_res; else legacy single url/sha256). Mirror selection (`GLOBAL`/`CN`) applies AFTER arch pick (per-arch resource may carry its own mirrors). Wire it where `res.url`/`res.sha256` are finalized (1032-1065).
- [ ] **Step 4: Run, expect PASS. Commit** — `feat(xim): resolve host arch + per-arch sha256 at install time`.

### Task B4: `package.archs` fail-closed validation

**Files:** Modify `installer.cppm` (pre-download) and consume `Package::archs` (`libxpkg xpkg.cppm:88`).
- [ ] **Step 1: Failing test** — installing a recipe whose `archs={"x86_64"}` on an `aarch64` host yields a clear error `unsupported architecture: aarch64 (supported: x86_64)` BEFORE any download attempt. (Legacy recipes with EMPTY `archs` → warn-only, to avoid breaking the long tail.)
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** the check in the resolver/installer entry (after plan build, before download). Normalize both sides via `normalize_arch_`.
- [ ] **Step 4: Run, expect PASS. Commit** — `feat(xim): fail-closed validation of host arch against package.archs`.

### Task B5: Index cache serde round-trips V2 fields

**Files:** Modify `index.cppm:40-100`.
- [ ] **Step 1: Failing test** — serialize a resource with `archs`/`sha256_by_arch`/`arch_alias`/`is_res`, deserialize, assert equality (raw, unresolved). Bump cache `format_version` if one exists; tolerate old caches (absent fields → empty).
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** the JSON read/write for the four fields.
- [ ] **Step 4: Run, expect PASS. Commit** — `feat(xim): persist V2 per-arch resource fields in index cache`.

### Task B6: e2e resolution test + full build

**Files:** Create `tests/e2e/multiarch_resolution_test.sh`; fixture recipe under `tests/fixtures/xim-pkgindex/`.
- [ ] **Step 1:** Write the e2e: with a temp `XLINGS_HOME` and a fixture multi-arch recipe pointing at local `file://` tarballs for both arches, `xlings install v2map` selects the host-arch tarball + verifies its sha256. Use `tests/e2e/project_test_lib.sh` helpers.
- [ ] **Step 2:** `xmake f --local_libxpkg=/home/speak/workspace/github/mcpplibs/libxpkg && xmake` then run unit tests + `XLINGS_BIN=... bash tests/e2e/multiarch_resolution_test.sh`. Expected PASS.
- [ ] **Step 3:** Run the broader e2e set touching index (`index_cache_test.sh`, `mirror_fallback_test.sh`) to confirm no regression.
- [ ] **Step 4: Commit** — `test(e2e): multi-arch install-time resolution`.

---

## Phase D — xim-pkgindex V2 spec, recipe fixes, version records  *(parallel-able)*

### Task D1: V2 spec doc + template
- [ ] Create `docs/V2/xpackage-spec.md` (derive from V1; document the three new shapes, `spec="2"`, arch normalization, fail-closed semantics, the `${...}` template vocabulary). Create `docs/V2/xpackage-template.lua`.
- [ ] Commit — `docs(spec): add XPackage Spec V2 (multi-arch shapes)`.

### Task D2: Fix the documented bugs
- [ ] `pkgs/g/github-gh.lua` → V2 per-arch map with real `linux_arm64`/`macOS_arm64` URLs + per-arch sha256 (it currently ships `amd64` to every arch). Update the `install()` hook's hardcoded `gh_%s_linux_amd64` dir to derive from `os.arch()`.
- [ ] `pkgs/n/node.lua` → V2 template shape using `${arch_alias}` (node uses `x64`/`arm64`/`darwin-arm64`); correct `archs` to `{"x86_64","aarch64"}`; fix the `darwin-arm64`-only macOS URL.
- [ ] Commit each — `fix(github-gh): serve correct binary per arch (V2)`, `fix(node): V2 multi-arch URLs`.

### Task D3: Migrate XLINGS_RES users to checksummed `res`
- [ ] `pkgs/c/cmake.lua` → `res=true` + `sha256={x86_64=,aarch64=}` (was bare `XLINGS_RES`, no checksum).
- [ ] Commit — `feat(cmake): V2 res shape with per-arch checksums`.

### Task D4: Record version changes (user requirement)
- [ ] Create `docs/CHANGELOG.md` entry (or per-recipe header `-- changelog:` line): for each touched recipe, record `spec 1 → 2`, the version(s) added, and the arch coverage change. Note in the index repo README that V2 requires xlings ≥ 0.4.61.
- [ ] Commit — `docs(changelog): record V2 schema + per-recipe version changes`.

---

## Phase E — Landing, release, index, ecosystem closure  *(SEQUENTIAL; human-gated triggers flagged)*

### Task E1: libxpkg release v0.0.42
- [ ] Push libxpkg branch, open PR, pass `ci.yml`, squash-merge to `main`.
- [ ] `git tag v0.0.42 && git push origin v0.0.42`. **[HUMAN-GATED: push access]**
- [ ] Add/update `mcpplibs-xpkg 0.0.42` in **`mcpplibs/mcpplibs-index`** (separate repo): version entry + source URL/hash; open PR there. **[HUMAN-GATED: separate repo]**

### Task E2: xlings pin bump + feature PR(s)
- [ ] Bump `xmake.lua:45` → `add_requires("mcpplibs-xpkg 0.0.42")`. Verify a clean (non-`--local_libxpkg`) build resolves the released package.
- [ ] Push xlings feature branch(es), open PR(s) (`feat(xim): multi-arch xpm V2 …`). **[HUMAN-GATED: push/PR]**
- [ ] Pass the PR CI gate: linux, linux-e2e, linux-root, macos, windows, aarch64. Fix failures; never `--no-verify`. Squash-merge.

### Task E3: xlings release 0.4.61
- [ ] `chore(release): bump version to 0.4.61` PR editing `src/core/config.cppm:16` AND `mcpp.toml:3` (fix the existing 0.4.59 drift in the same edit). Merge.
- [ ] Run the **`Release`** workflow (`workflow_dispatch`, optional `version` input). **[HUMAN-GATED: manual by design]** → tags `v0.4.61`, builds linux-x86_64 / linux-aarch64(musl) / macosx-arm64 / windows-x86_64, creates the GitHub release.
- [ ] Confirm the `publish-index` job republished index + sub-index artifacts + merged pointer to `xlings-res/xim-index` (GH+GitCode). Needs `XLINGS_RES_TOKEN`/`GITCODE_TOKEN`. **[HUMAN-GATED: secrets]**

### Task E4: Land V2 recipes + publish index + close loop
- [ ] Merge the xim-pkgindex V2 PR (Phase D) to its main. **[HUMAN-GATED: push/PR]**
- [ ] Publish the index artifact: `tools/build_xim_index_artifact.sh` + `tools/publish_xim_index.sh` (or the repo's `publish-sub-indexes.yml` `workflow_dispatch`). **[HUMAN-GATED: secrets]**
- [ ] **Closure verification:** on a clean client with xlings 0.4.61, `xlings update` (pulls new index artifact, git fallback) then `xlings install github-gh` on BOTH an x86_64 and an aarch64 host → each fetches the correct binary, sha256 verifies, `gh --version` runs. Capture output as evidence.

### Task E5: Dynamic doc + version-record finalization
- [ ] Update `2026-06-26-multiarch-package-description-design.md` with a "V2 final schema (as shipped)" section + the libxpkg 0.0.42 / xlings 0.4.61 / spec-V2 version-change record.
- [ ] Tick all checkboxes in this plan; note any deviations.

---

## Self-Review

**Spec coverage** (against the goal directive): 方案细节→doc ✓ (design doc + this plan); 跨仓库并行标明 ✓ (Cross-Repo Map, ⇄/→); 拆分实施步骤 ✓ (Phases A–E, TDD steps); 开发 ✓ (A,B,D); 测试 ✓ (GoogleTest A, unit+e2e B); 动态更新文档 ✓ (D1/D4/E5); PR ✓ (E1/E2); CI ✓ (E2 gate); 发布新版本带版本号 ✓ (libxpkg 0.0.42, xlings 0.4.61, spec V2); 更新 xim-pkgindex ✓ (D); 更新 index 生态闭环 ✓ (E4 closure verification); 记录版本变化 ✓ (D4/E5).

**Placeholder scan:** code/test snippets are concrete for the foundational tasks (A1–A4, B1–B5); repetitive recipe edits (D) reference exact files + the documented bug. No "TBD".

**Type consistency:** field names `archs` / `sha256_by_arch` / `arch_alias` / `is_res` and `normalize_arch` are used identically across libxpkg (A2/A3/A4) and xlings (B1/B2/B3/B5). `detect_arch_()` canonicalization + `detect_arch_raw_()` split is consistent between B1 and B3.

**Open risk flagged:** libxpkg `release.yml` is stale → publish is manual git-tag + mcpplibs-index PR (E1). C++23-module build across two repos may need `xlings use gcc@16.1.0` (per project memory).
