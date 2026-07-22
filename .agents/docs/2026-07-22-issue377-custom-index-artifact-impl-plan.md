# Custom Index Artifact Source (+ git CA fallback) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let any user-defined index repo declare its own artifact source (pointer + sha256 tarball) with git as fallback (#377), and make the git fallback survive Debian/Ubuntu CA layouts (#378) — one PR, version 0.4.68.

**Architecture:** Per design doc `.agents/docs/2026-07-22-issue377-custom-index-artifact-design.md`: `IndexRepo` gains optional `artifact` (base URL, string or region object) and `source` (auto|artifact|git) fields; `indexfetch.cppm` gets an `ArtifactSource` abstraction that parameterizes the existing pointer/asset URL construction and per-base pointer cache; `repo.cppm` relaxes the artifact gate to `isDefaultOfficial || hasArtifactSource` and routes all three sync loops (global main-list, global subs, project) through one artifact-first helper. `compact::git` pins `GIT_SSL_CAINFO` when the static git's baked-in CA default is absent.

**Tech Stack:** C++23 modules, nlohmann::json, gtest, bash e2e.

## Global Constraints

- Build/test ONLY via `mcpp build` / `mcpp test` (never raw xmake; mcpp sandbox provides gcc 16.1.0). E2e: `bash tests/e2e/<script>.sh` (auto-finds `build/**/debug/**/xlings`).
- Official index path (main + lua-default subs) must have ZERO behavior change; repos without `artifact` stay byte-identical (all new params default off).
- Version bump in this PR: `src/core/config.cppm` `Info::VERSION` `"0.4.67"` → `"0.4.68"`.
- One PR, squash bypass merge (`gh pr merge --squash --admin`), then `release.yml` via `gh workflow run`.
- e2e scripts hermetic (no network).
- Commit style: `type(scope): summary` (see `git log`); frequent commits on branch `feat/issue377-custom-index-artifact` off `origin/main`.

---

### Task 0: Branch setup

- [ ] **Step 1:** `git checkout -b feat/issue377-custom-index-artifact origin/main`
- [ ] **Step 2:** `git add .agents/docs/2026-07-22-issue377-custom-index-artifact-design.md .agents/docs/2026-07-22-issue377-custom-index-artifact-impl-plan.md && git commit -m "docs(agents): issue #377 custom index artifact design + impl plan"`
- [ ] **Step 3:** Baseline: `mcpp build && mcpp test` → all green before touching code.

---

### Task 1: compact::git CA env pin (#378)

**Files:**
- Modify: `src/core/compact/git.cppm`
- Test: `tests/unit/test_main.cpp` (new `CompactGitTest` suite; file already imports `xlings.core.compact`? — check header imports; add `import xlings.core.compact;` if absent)

**Interfaces:**
- Produces: `xlings::compact::git::resolve_ca_bundle(const std::function<bool(const std::string&)>& exists) -> std::string` (exported, pure). `""` = leave env alone.

- [ ] **Step 1: Write the failing tests** (append to `tests/unit/test_main.cpp`)

```cpp
// ── compact::git CA bundle resolution (#378) ──
TEST(CompactGitTest, CaBundleDefaultPresentReturnsEmpty) {
    auto exists = [](const std::string& p) { return p == "/etc/ssl/cert.pem"; };
    EXPECT_EQ(xlings::compact::git::resolve_ca_bundle(exists), "");
}
TEST(CompactGitTest, CaBundleDebianLayout) {
    auto exists = [](const std::string& p) {
        return p == "/etc/ssl/certs/ca-certificates.crt";
    };
    EXPECT_EQ(xlings::compact::git::resolve_ca_bundle(exists),
              "/etc/ssl/certs/ca-certificates.crt");
}
TEST(CompactGitTest, CaBundleRhelLayout) {
    auto exists = [](const std::string& p) {
        return p == "/etc/pki/tls/certs/ca-bundle.crt";
    };
    EXPECT_EQ(xlings::compact::git::resolve_ca_bundle(exists),
              "/etc/pki/tls/certs/ca-bundle.crt");
}
TEST(CompactGitTest, CaBundleNothingFoundReturnsEmpty) {
    auto exists = [](const std::string&) { return false; };
    EXPECT_EQ(xlings::compact::git::resolve_ca_bundle(exists), "");
}
```

- [ ] **Step 2:** `mcpp build` → expect compile FAIL (`resolve_ca_bundle` undefined).
- [ ] **Step 3: Implement** in `src/core/compact/git.cppm`, exported (outside `detail_`, e.g. right after `struct Result`):

```cpp
// #378: CA bundle to pin via GIT_SSL_CAINFO. The linux xim:git static build
// bakes OPENSSLDIR=/etc/ssl, so its default CA FILE is /etc/ssl/cert.pem — a
// BSD/Alpine layout that Debian/Ubuntu/RHEL never create, which kills every
// HTTPS transport. GIT_SSL_CAINFO is the only env that overrides git's
// explicit CURLOPT_CAINFO (SSL_CERT_FILE / CURL_CA_BUNDLE / GIT_SSL_CAPATH
// all lose). Returns "" when the built-in default exists or no known bundle
// is found. Pure; existence probe injected for tests.
std::string resolve_ca_bundle(const std::function<bool(const std::string&)>& exists) {
    if (exists("/etc/ssl/cert.pem")) return {};
    for (const char* f : {"/etc/ssl/certs/ca-certificates.crt",
                          "/etc/pki/tls/certs/ca-bundle.crt",
                          "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                          "/etc/ssl/ca-bundle.pem"}) {
        if (exists(f)) return f;
    }
    return {};
}
```

And in `detail_` (call it at the top of `ensure_available`, before `prepend_current_bin_dir_()`):

```cpp
// #378: xvm-injected GIT_SSL_CAINFO (xim-pkgindex#406) never reaches us —
// compact::git execs the binary directly, bypassing the shim. Pin it here so
// index sync works with system git, XLINGS_COMPACT_GIT_BIN overrides, and
// bootstrap alike. User/CI-set GIT_SSL_CAINFO always wins.
inline void ensure_ca_env_() {
#if defined(__linux__)
    if (!env_or_empty_("GIT_SSL_CAINFO").empty()) return;
    auto bundle = git::resolve_ca_bundle([](const std::string& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    });
    if (!bundle.empty()) {
        platform::set_env_variable("GIT_SSL_CAINFO", bundle);
        log::debug("compact::git: GIT_SSL_CAINFO={} (/etc/ssl/cert.pem absent)", bundle);
    }
#endif
}
```

`ensure_available` first line becomes:

```cpp
bool ensure_available(EnsureMode mode = EnsureMode::AutoInstall) {
    detail_::ensure_ca_env_();
    detail_::prepend_current_bin_dir_();
    ...
```

Note: `detail_` is declared before the exported functions in this file; since `resolve_ca_bundle` must be exported and `ensure_ca_env_` (in `detail_`) calls it, define `resolve_ca_bundle` BEFORE the `detail_` namespace or forward-appropriately — simplest: put `resolve_ca_bundle` immediately after `enum class EnsureMode` and reference it as `git::resolve_ca_bundle` is wrong inside same namespace — just call `resolve_ca_bundle(...)` unqualified from `detail_::ensure_ca_env_()` (enclosing namespace lookup finds it).

- [ ] **Step 4:** `mcpp build && mcpp test` → CompactGitTest 4/4 PASS, no regressions.
- [ ] **Step 5:** `git add -A && git commit -m "fix(compact/git): pin GIT_SSL_CAINFO when built-in CA default is absent (#378)"`

---

### Task 2: Config — IndexRepo fields + exported parser

**Files:**
- Modify: `src/core/config.cppm` (struct `IndexRepo` ~line 20; `load_index_repos_from_json_` ~line 123; call sites ~line 493, ~line 601)
- Test: `tests/unit/test_main.cpp` (new `ConfigIndexReposTest` suite)

**Interfaces:**
- Produces: `IndexRepo{name, url, artifactBase, source}`; exported free fn `parse_index_repos_json(const nlohmann::json& json, const std::string& mirror) -> std::vector<IndexRepo>`.

- [ ] **Step 1: Failing tests**

```cpp
// ── #377: index_repos parsing with artifact/source fields ──
TEST(ConfigIndexReposTest, PlainEntryHasNoArtifact) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"a","url":"https://x/a.git"}]})");
    auto repos = parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_EQ(repos[0].name, "a");
    EXPECT_EQ(repos[0].url, "https://x/a.git");
    EXPECT_TRUE(repos[0].artifactBase.empty());
    EXPECT_TRUE(repos[0].source.empty());
}
TEST(ConfigIndexReposTest, ArtifactStringTrimsTrailingSlash) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"m","url":"https://x/m.git",
         "artifact":"https://github.com/xlings-res/mcpp-index/","source":"auto"}]})");
    auto repos = parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_EQ(repos[0].artifactBase, "https://github.com/xlings-res/mcpp-index");
    EXPECT_EQ(repos[0].source, "auto");
}
TEST(ConfigIndexReposTest, ArtifactRegionObjectResolvesMirror) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"m","url":"https://x/m.git",
         "artifact":{"GLOBAL":"https://github.com/o/r","CN":"https://gitcode.com/o/r"}}]})");
    EXPECT_EQ(parse_index_repos_json(j, "CN")[0].artifactBase, "https://gitcode.com/o/r");
    EXPECT_EQ(parse_index_repos_json(j, "")[0].artifactBase, "https://github.com/o/r");
    EXPECT_EQ(parse_index_repos_json(j, "XX")[0].artifactBase, "https://github.com/o/r"); // GLOBAL fallback
}
TEST(ConfigIndexReposTest, MalformedEntriesSkipped) {
    auto j = nlohmann::json::parse(R"({"index_repos":[
        {"name":"a"},{"url":"u"},{"name":"b","url":"https://x/b.git"}]})");
    auto repos = parse_index_repos_json(j, "");
    ASSERT_EQ(repos.size(), 1u);
    EXPECT_EQ(repos[0].name, "b");
}
```

Add `using xlings::parse_index_repos_json;` (match the config module's export namespace — `IndexRepo`/`Config` are exported at global scope in module `xlings.core.config`; follow suit and export the free function at the same scope, then reference it unqualified in the test with the module already imported).

- [ ] **Step 2:** `mcpp build` → FAIL (undefined).
- [ ] **Step 3: Implement.** Extend struct:

```cpp
export struct IndexRepo {
    std::string name;
    std::string url;
    std::string artifactBase;  // #377: resolved artifact base URL ("" = git-only)
    std::string source;        // #377: per-repo override: "" | "auto" | "artifact" | "git"
};
```

Add exported free function (near `IndexRepo`, after `Config` class or in an appropriate spot with access to `utils::trim_string`):

```cpp
// #377: parse index_repos entries. `artifact` is a flat string or a region
// object {"GLOBAL":..,"CN":..} (same shape as xim.index-base), resolved
// against `mirror` with GLOBAL fallback. `source` optionally overrides the
// global index source for this repo only.
export std::vector<IndexRepo> parse_index_repos_json(const nlohmann::json& json,
                                                     const std::string& mirror) {
    std::vector<IndexRepo> out;
    if (!json.contains("index_repos") || !json["index_repos"].is_array()) return out;
    for (auto it = json["index_repos"].begin(); it != json["index_repos"].end(); ++it) {
        if (!it->is_object() || !it->contains("name") || !it->contains("url")) continue;
        IndexRepo repo;
        repo.name = (*it)["name"].get<std::string>();
        repo.url  = (*it)["url"].get<std::string>();
        if (repo.name.empty() || repo.url.empty()) continue;
        if (it->contains("artifact")) {
            auto& a = (*it)["artifact"];
            std::string base;
            if (a.is_string()) base = a.get<std::string>();
            else if (a.is_object()) {
                std::string key = mirror.empty() ? "GLOBAL" : mirror;
                if (a.contains(key) && a[key].is_string()) base = a[key].get<std::string>();
                else if (a.contains("GLOBAL") && a["GLOBAL"].is_string())
                    base = a["GLOBAL"].get<std::string>();
            }
            base = utils::trim_string(base);
            while (base.size() > 1 && base.ends_with('/')) base.pop_back();
            repo.artifactBase = base;
        }
        if (it->contains("source") && (*it)["source"].is_string())
            repo.source = (*it)["source"].get<std::string>();
        out.push_back(std::move(repo));
    }
    return out;
}
```

Delete private `load_index_repos_from_json_` and update the two call sites (`mirror_` is already parsed before both):

```cpp
globalIndexRepos_ = parse_index_repos_json(json, mirror_);    // ~line 493
...
projectIndexRepos_ = parse_index_repos_json(json, mirror_);   // ~line 601
```

(Placement note: `parse_index_repos_json` must be defined before `Config`'s member functions use it — put the definition between `IndexRepo` and `class Config`, and if `utils` isn't imported at that point it already is, config.cppm uses `utils::trim_string` in `parse_server_list_`.)

- [ ] **Step 4:** `mcpp build && mcpp test` → PASS.
- [ ] **Step 5:** `git add -A && git commit -m "feat(config): IndexRepo artifact/source fields + exported index_repos parser (#377)"`

---

### Task 3: indexfetch — ArtifactSource + select_manifest + custom URL construction

**Files:**
- Modify: `src/core/xim/indexfetch.cppm`
- Test: `tests/unit/test_indexfetch.cpp`

**Interfaces:**
- Produces (all exported from `xlings::xim`):

```cpp
struct ArtifactSource {
    std::string base;      // no trailing slash
    std::string server;    // forge only: scheme://host/org ("" => flat/local)
    std::string repoName;  // last path segment of base — pointer file prefix
    std::string key;       // manifest lookup key (config repo name)
    std::optional<std::filesystem::path> localDir;  // set when base is local
    bool forge() const { return !server.empty(); }
};
std::optional<ArtifactSource> artifact_source_for(const IndexRepo& repo);
const IndexManifest* select_manifest(const std::map<std::string, IndexManifest>& pointers,
                                     std::string_view key, bool soleEntryFallback);
// existing fns gain a trailing `const ArtifactSource* custom = nullptr` param:
std::vector<std::string> index_asset_urls(std::string_view filename,
                                          std::string_view mirror = {},
                                          std::string_view version = {},
                                          const ArtifactSource* custom = nullptr);
std::vector<std::string> index_pointer_urls(std::string_view filename,
                                            std::string_view mirror,
                                            const ArtifactSource* custom = nullptr);
const std::map<std::string, IndexManifest>& load_index_pointers(std::string_view mirror,
                                            const ArtifactSource* custom = nullptr);
bool fetch_index_artifact(const std::filesystem::path& destIndexDir, std::string& err,
                          std::string_view subName = {},
                          const ArtifactSource* custom = nullptr);
```

- [ ] **Step 1: Failing tests** (append to `tests/unit/test_indexfetch.cpp`; it already imports the module and `IndexRepo` comes from `xlings.core.config` — add `import xlings.core.config;` if missing)

```cpp
// ── #377: per-repo artifact sources ──
static IndexRepo mkrepo(std::string name, std::string base) {
    IndexRepo r; r.name = std::move(name); r.url = "https://x/y.git";
    r.artifactBase = std::move(base); return r;
}

TEST(ArtifactSourceFor, GithubForgeBase) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("mcpplibs", "https://github.com/xlings-res/mcpp-index"));
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->forge());
    EXPECT_EQ(s->server, "https://github.com/xlings-res");
    EXPECT_EQ(s->repoName, "mcpp-index");
    EXPECT_EQ(s->key, "mcpplibs");
    EXPECT_FALSE(s->localDir.has_value());
}
TEST(ArtifactSourceFor, GitcodeForgeBase) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("m", "https://gitcode.com/xlings-res/mcpp-index/"));
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->forge());
    EXPECT_EQ(s->server, "https://gitcode.com/xlings-res");
    EXPECT_EQ(s->repoName, "mcpp-index");
}
TEST(ArtifactSourceFor, FlatHttpBase) {
    auto s = xlings::xim::artifact_source_for(mkrepo("m", "https://example.com/idx/myindex"));
    ASSERT_TRUE(s.has_value());
    EXPECT_FALSE(s->forge());
    EXPECT_EQ(s->repoName, "myindex");
    EXPECT_FALSE(s->localDir.has_value());
}
TEST(ArtifactSourceFor, LocalDirAndFileUrl) {
    auto s1 = xlings::xim::artifact_source_for(mkrepo("m", "/tmp/serve/myindex"));
    ASSERT_TRUE(s1.has_value());
    ASSERT_TRUE(s1->localDir.has_value());
    EXPECT_EQ(s1->repoName, "myindex");
    auto s2 = xlings::xim::artifact_source_for(mkrepo("m", "file:///tmp/serve/myindex"));
    ASSERT_TRUE(s2.has_value());
    ASSERT_TRUE(s2->localDir.has_value());
    EXPECT_EQ(s2->localDir->string(), "/tmp/serve/myindex");
}
TEST(ArtifactSourceFor, EmptyBaseIsNullopt) {
    EXPECT_FALSE(xlings::xim::artifact_source_for(mkrepo("m", "")).has_value());
}

TEST(SelectManifest, ExactMatchWins) {
    std::map<std::string, xlings::xim::IndexManifest> p;
    p["a"].index_name = "a"; p["b"].index_name = "b";
    auto* m = xlings::xim::select_manifest(p, "b", true);
    ASSERT_NE(m, nullptr); EXPECT_EQ(m->index_name, "b");
}
TEST(SelectManifest, SoleEntryFallbackOnlyWhenEnabled) {
    std::map<std::string, xlings::xim::IndexManifest> p;
    p["mcpp"].index_name = "mcpp";
    EXPECT_NE(xlings::xim::select_manifest(p, "mcpplibs", true), nullptr);
    EXPECT_EQ(xlings::xim::select_manifest(p, "mcpplibs", false), nullptr);  // official: exact only
}
TEST(SelectManifest, MultiEntryMissIsNull) {
    std::map<std::string, xlings::xim::IndexManifest> p;
    p["a"].index_name = "a"; p["b"].index_name = "b";
    EXPECT_EQ(xlings::xim::select_manifest(p, "c", true), nullptr);
}

TEST(IndexAssetUrls, CustomForgeVersionedTagFirstSingleServer) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("m", "https://github.com/xlings-res/mcpp-index"));
    auto urls = index_asset_urls("mcpp-index-2e23e20.tar.gz", "GLOBAL", "2e23e20", &*s);
    ASSERT_EQ(urls.size(), 2u);
    EXPECT_EQ(urls[0], "https://github.com/xlings-res/mcpp-index/releases/download/v2e23e20/mcpp-index-2e23e20.tar.gz");
    EXPECT_EQ(urls[1], "https://github.com/xlings-res/mcpp-index/releases/download/latest/mcpp-index-2e23e20.tar.gz");
}
TEST(IndexAssetUrls, CustomFlatBase) {
    auto s = xlings::xim::artifact_source_for(mkrepo("m", "https://example.com/idx/myindex"));
    auto urls = index_asset_urls("a.tar.gz", "GLOBAL", "1", &*s);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://example.com/idx/myindex/a.tar.gz");
}
TEST(IndexPointerUrls, CustomForgeRawUrl) {
    auto s = xlings::xim::artifact_source_for(
        mkrepo("m", "https://github.com/xlings-res/mcpp-index"));
    auto urls = xlings::xim::index_pointer_urls("mcpp-index-pointers.json", "GLOBAL", &*s);
    ASSERT_EQ(urls.size(), 1u);
    EXPECT_EQ(urls[0], "https://raw.githubusercontent.com/xlings-res/mcpp-index/main/mcpp-index-pointers.json");
}
```

- [ ] **Step 2:** `mcpp build` → FAIL.
- [ ] **Step 3: Implement** in `indexfetch.cppm`:

(a) Export `ArtifactSource` + declarations in the export block (after `IndexManifest`/`parse_index_manifest`).

(b) `artifact_source_for`:

```cpp
std::optional<ArtifactSource> artifact_source_for(const IndexRepo& repo) {
    std::string base = repo.artifactBase;
    if (base.empty()) return std::nullopt;
    while (base.size() > 1 && base.ends_with('/')) base.pop_back();

    ArtifactSource src;
    src.base = base;
    src.key  = repo.name;

    std::string pathPart = base;
    if (base.starts_with("file://")) {
        src.localDir = std::filesystem::path(base.substr(7)).lexically_normal();
        pathPart = src.localDir->generic_string();
    } else if (base.find("://") == std::string::npos) {
        src.localDir = std::filesystem::path(base).lexically_normal();
        pathPart = src.localDir->generic_string();
    }

    auto slash = pathPart.find_last_of("/\\");
    src.repoName = slash == std::string::npos ? pathPart : pathPart.substr(slash + 1);
    if (src.repoName.empty()) return std::nullopt;

    if (!src.localDir) {
        auto rest = base.substr(base.find("://") + 3);
        auto firstSlash = rest.find('/');
        auto host = firstSlash == std::string::npos ? rest : rest.substr(0, firstSlash);
        if (host.find("github.com") != std::string::npos ||
            host.find("gitcode.com") != std::string::npos) {
            src.server = base.substr(0, base.find_last_of('/'));  // scheme://host/org
        }
    }
    return src;
}
```

(c) `select_manifest`:

```cpp
// Select the manifest for `key`: exact match; else, for CUSTOM sources only
// (soleEntryFallback), the sole entry — a single-index pointer need not repeat
// the consumer's configured repo name (mcpp publishes key "mcpp", consumers
// configure "mcpplibs"). NEVER sole-fallback for the official combined pointer:
// a missing sub entry there must fail, not silently serve the main index.
const IndexManifest* select_manifest(const std::map<std::string, IndexManifest>& pointers,
                                     std::string_view key, bool soleEntryFallback) {
    if (auto it = pointers.find(std::string(key)); it != pointers.end()) return &it->second;
    if (soleEntryFallback && pointers.size() == 1) return &pointers.begin()->second;
    return nullptr;
}
```

(d) `index_asset_urls` — insert at the top, before the official logic:

```cpp
if (custom) {
    if (custom->forge()) {
        std::vector<std::string> urls;
        std::vector<std::string> tags;
        if (!version.empty()) tags.push_back("v" + std::string(version));
        tags.push_back("latest");   // GitHub rolling fallback; a 404 falls through
        for (auto& tag : tags)
            urls.push_back(std::format("{}/{}/releases/download/{}/{}",
                                       custom->server, custom->repoName, tag, filename));
        return urls;
    }
    if (!custom->localDir) return { custom->base + "/" + std::string(filename) };
    return {};  // local dir: obtain_file consumes a forced BaseOverride instead
}
```

(e) `index_pointer_urls` — refactor its `rawFor` lambda to take the repo name too (`rawFor(server, repoName)`, official call sites pass `repo`), then insert at the top:

```cpp
if (custom) {
    if (custom->forge()) {
        if (auto u = rawFor(custom->server, custom->repoName); !u.empty()) return {u};
        return {};
    }
    if (!custom->localDir) return { custom->base + "/" + std::string(filename) };
    return {};
}
```

(f) `detail_::obtain_file` — add `const BaseOverride* forced = nullptr` last param; first line becomes `auto b = forced ? *forced : resolve_base_();` (forced isolates custom sources from the global `XLINGS_INDEX_BASE_URL` / `xim.index-base` override). Add a small helper next to it:

```cpp
// Forced base for a custom source: local dir copy, flat-remote base, or (for
// forge bases) an EMPTY override meaning "use the passed URL list, but do NOT
// apply the global index-base override".
BaseOverride base_override_for_(const ArtifactSource& src) {
    BaseOverride b;
    if (src.localDir) { b.local = src.localDir; b.base = src.base; }
    else if (!src.forge()) b.base = src.base;
    return b;
}
```

(g) `load_index_pointers(mirror, custom)` — replace the `once_flag` body with a per-base keyed cache (same single-fetch-per-process semantics; official base key `""`):

```cpp
const std::map<std::string, IndexManifest>& load_index_pointers(std::string_view mirror,
                                                                const ArtifactSource* custom) {
    static std::mutex mu;
    static std::map<std::string, std::map<std::string, IndexManifest>> cacheByBase;
    static std::set<std::string> fetched;
    std::string cacheKey = custom ? custom->base : std::string{};
    std::scoped_lock lk(mu);
    auto& cache = cacheByBase[cacheKey];
    if (fetched.contains(cacheKey)) return cache;
    fetched.insert(cacheKey);

    std::string filename = custom ? custom->repoName + "-pointers.json"
                                  : std::string("xim-index-pointers.json");
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() /
               std::format("xim-index-pointers.{}.{}.json", platform::get_pid(), fetched.size());
    std::error_code ec; fs::remove(tmp, ec);
    detail_::BaseOverride forcedStorage;
    const detail_::BaseOverride* forced = nullptr;
    if (custom) { forcedStorage = detail_::base_override_for_(*custom); forced = &forcedStorage; }
    auto err = detail_::obtain_file(filename, index_pointer_urls(filename, mirror, custom),
                                    tmp, {}, forced);
    if (!err.empty()) { log::warn("[index] pointer fetch failed: {}", err); return cache; }
    std::string text;
    { std::ifstream in(tmp, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); text = ss.str(); }
    fs::remove(tmp, ec);
    try {
        auto j = nlohmann::json::parse(text);
        if (j.contains("indexes") && j["indexes"].is_object())
            for (auto it = j["indexes"].begin(); it != j["indexes"].end(); ++it)
                if (auto m = parse_index_manifest(it.value().dump())) cache[it.key()] = *m;
    } catch (...) { log::warn("[index] pointer parse failed"); }
    return cache;
}
```

Careful: `obtain_file`'s local branch requires the file to exist in `*b.local`; for custom local `b.base` also set — its local branch runs first, correct.

(h) `fetch_index_artifact(destIndexDir, err, subName, custom)`:
- `std::string key = custom ? custom->key : (subName.empty() ? std::string("xim") : std::string(subName));`
- pointer lookup: `auto& pointers = load_index_pointers(mirrorKey, custom);` then
  ```cpp
  const IndexManifest* pm = select_manifest(pointers, key, custom != nullptr);
  if (!pm) { err = std::format("no pointer entry for '{}' ({} entries)", key, pointers.size()); return false; }
  const IndexManifest& manifest = *pm;
  ```
- asset obtain: build the forced override once and pass it:
  ```cpp
  detail_::BaseOverride forcedStorage;
  const detail_::BaseOverride* forced = nullptr;
  if (custom) { forcedStorage = detail_::base_override_for_(*custom); forced = &forcedStorage; }
  if (auto e = detail_::obtain_file(manifest.artifact_name,
                  index_asset_urls(manifest.artifact_name, mirrorKey, manifest.index_version, custom),
                  artifactFile, manifest.artifact_sha256, forced); !e.empty()) { ... }
  ```
- Everything downstream (extract, pkgs check, version marker, atomic swap) unchanged.

- [ ] **Step 4:** `mcpp build && mcpp test` → all new tests PASS, existing IndexManifest/IndexAssetUrls/ReconcileIndexTemps tests untouched-green.
- [ ] **Step 5:** `git add -A && git commit -m "feat(xim): per-repo ArtifactSource — custom pointer/asset URLs + keyed pointer cache (#377)"`

---

### Task 4: repo.cppm — gate relaxation + artifact-first sync in all three loops

**Files:**
- Modify: `src/core/xim/repo.cppm`
- Test: `tests/unit/test_main.cpp` (`XimSubReposTest` additions)

**Interfaces:**
- Consumes: `artifact_source_for`, `fetch_index_artifact(..., custom)` from Task 3.
- Produces: `sub_should_attempt_artifact(bool isDefaultOfficial, const std::string& indexSource, bool subManaged, bool subHasPkgs, bool mainArtifactManaged, bool hasArtifactSource = false)`; `sync_repo_with_artifact(const IndexRepo&, const std::filesystem::path&, const std::string& globalIndexSource, const std::function<bool()>& syncFallback) -> bool`.

- [ ] **Step 1: Failing tests**

```cpp
// ── #377: custom repos with a declared artifact source ──
TEST(XimSubReposTest, SubArtifactCustomAutoAlwaysAttempts) {
    // custom + artifact declared: attempts even for an existing git checkout
    // with main not artifact-managed (no C1 gate — atomic swap migrates safely)
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        false, "auto", false, true, false, true));
}
TEST(XimSubReposTest, SubArtifactCustomForcedArtifact) {
    EXPECT_TRUE(xlings::xim::sub_should_attempt_artifact(
        false, "artifact", false, true, false, true));
}
TEST(XimSubReposTest, SubArtifactCustomGitForced) {
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        false, "git", false, true, false, true));
}
TEST(XimSubReposTest, SubArtifactNoSourceStaysGit) {
    // default param: prior behavior for repos without artifact declarations
    EXPECT_FALSE(xlings::xim::sub_should_attempt_artifact(
        false, "auto", false, false, true));
}
```

- [ ] **Step 2:** `mcpp build` → FAIL (arity/behavior).
- [ ] **Step 3: Implement.**

`sub_should_attempt_artifact` (update doc comment to mention #377):

```cpp
bool sub_should_attempt_artifact(bool isDefaultOfficial,
                                 const std::string& indexSource,
                                 bool subManaged, bool subHasPkgs,
                                 bool mainArtifactManaged,
                                 bool hasArtifactSource = false) {
    if (indexSource == "artifact") return isDefaultOfficial || hasArtifactSource;
    if (indexSource == "git")      return false;
    return hasArtifactSource
        || (isDefaultOfficial && (subManaged || !subHasPkgs || mainArtifactManaged));
}
```

New helper (after `sync_repo`):

```cpp
// #377: sync one CUSTOM repo that declares its own artifact source: artifact
// first (per the repo's effective source mode), then the repo's pre-existing
// git/local path via syncFallback. source:"artifact" hard-fails (no fallback),
// mirroring the main index's XLINGS_INDEX_SOURCE=artifact semantics.
bool sync_repo_with_artifact(const IndexRepo& repo,
                             const std::filesystem::path& repoDir,
                             const std::string& globalIndexSource,
                             const std::function<bool()>& syncFallback) {
    auto src = artifact_source_for(repo);
    std::string mode = repo.source.empty() ? globalIndexSource : repo.source;
    if (src && sub_should_attempt_artifact(false, mode, false, false, false, true)) {
        std::string ferr;
        if (fetch_index_artifact(repoDir, ferr, repo.name, &*src)) return true;
        if (mode == "artifact") {
            log::error("[index] '{}' artifact fetch failed and git fallback disabled "
                       "(source=artifact): {}", repo.name, ferr);
            return false;
        }
        log::warn("[index] '{}' artifact fetch failed ({}); falling back to git",
                  repo.name, ferr);
    }
    return syncFallback();
}
```

Integration (all inside `sync_all_repos`):

(a) `syncRepos` lambda — restructure the loop body so the existing local/git handling becomes the fallback:

```cpp
for (auto& repo : repos) {
    ++total;
    auto repoDir = Config::repo_dir_for(repo, projectScope);
    if (!projectScope && mainArtifactManaged && repoDir == mainDir) { ++ok; continue; }

    auto fallbackSync = [&]() -> bool {
        if (Config::is_local_repo_source(repo, projectScope)) {
            auto sourceDir = Config::resolve_repo_source(repo, projectScope);
            if (!detail_::ensure_local_repo_link_(repoDir, sourceDir)) {
                log::warn("index repo '{}' skipped: local source has no pkgs/ ({})",
                          repo.name, sourceDir.string());
                return false;
            }
            return true;
        }
        return sync_repo(repoDir, detail_::sync_repo_url_(repo.url, mirror), force);
    };

    // #377: a repo-declared artifact source is tried first; git/local remains
    // the fallback (source:"artifact" disables it inside the helper).
    bool okOne = repo.artifactBase.empty()
        ? fallbackSync()
        : sync_repo_with_artifact(repo, repoDir, indexSource, fallbackSync);
    if (!okOne) {
        log::warn("index repo '{}' skipped: sync failed ({})", repo.name, repo.url);
        continue;
    }
    ++ok;
}
```

(Drop the now-duplicated standalone warn after `sync_repo` — the message above covers both paths; keep messages stable where tests grep them: `interface_multi_repo_error_visibility_test.sh` greps — check it for exact expected strings BEFORE changing warn texts and preserve any it asserts.)

(b) global sub loop:

```cpp
std::optional<ArtifactSource> customSrc;
if (!subDefaultOfficial) customSrc = artifact_source_for(repo);
std::string subMode = repo.source.empty() ? indexSource : repo.source;
bool subAttemptArtifact = sub_should_attempt_artifact(
    subDefaultOfficial, subMode, subManaged,
    fs::exists(repoDir / "pkgs"), mainArtifactManaged, customSrc.has_value());

bool ok = false;
if (subAttemptArtifact) {
    std::string ferr;
    ok = fetch_index_artifact(repoDir, ferr, repo.name,
                              customSrc ? &*customSrc : nullptr);
    if (!ok)
        log::warn("[index] sub-index '{}' artifact fetch failed: {}", repo.name, ferr);
}
if (!ok && !(subMode == "artifact" && (subDefaultOfficial || customSrc.has_value()))) {
    ok = sync_repo(repoDir, repo.url, force);
}
```

(c) project sub loop:

```cpp
for (auto& [name, repo] : projMerged) {
    auto repoDir = sub_repo_dir_for(repo, true);
    auto fallbackSync = [&]{ return sync_repo(repoDir, repo.url, force); };
    bool okOne = repo.artifactBase.empty()
        ? fallbackSync()
        : sync_repo_with_artifact(repo, repoDir, indexSource, fallbackSync);
    if (okOne) projSyncedSubs.push_back(repo);
    else log::warn("failed to sync project sub-index repo: {} ({})", repo.name, repo.url);
}
```

- [ ] **Step 4:** `mcpp build && mcpp test` → PASS (old 5-arg call sites in tests still compile via default param).
- [ ] **Step 5:** `git add -A && git commit -m "feat(xim): artifact-first sync for repos with declared artifact sources (#377)"`

---

### Task 5: xim-indexrepos.json object-format persistence

**Files:**
- Modify: `src/core/xim/repo.cppm` (`load_sub_repos_json`, `save_sub_repos_json`)
- Test: `tests/unit/test_main.cpp` (`XimSubReposTest`)

**Interfaces:**
- Produces: value schema `"<name>": "<url>"` (legacy) OR `"<name>": {"url":..,"artifact":..,"source":..}`.

- [ ] **Step 1: Failing test**

```cpp
TEST(XimSubReposTest, SubReposJsonObjectFormatRoundTrip) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "xlings-test-subrepos-json";
    fs::create_directories(dir);
    auto file = dir / "xim-indexrepos.json";

    std::vector<IndexRepo> repos;
    repos.push_back({"plain", "https://x/plain.git", "", ""});
    repos.push_back({"custom", "https://x/custom.git",
                     "https://github.com/o/custom-index", "auto"});
    xlings::xim::save_sub_repos_json(file, repos);

    auto loaded = xlings::xim::load_sub_repos_json(file);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].name, "plain");
    EXPECT_TRUE(loaded[0].artifactBase.empty());
    EXPECT_EQ(loaded[1].name, "custom");
    EXPECT_EQ(loaded[1].url, "https://x/custom.git");
    EXPECT_EQ(loaded[1].artifactBase, "https://github.com/o/custom-index");
    EXPECT_EQ(loaded[1].source, "auto");

    // plain entries must persist as plain strings (old-xlings tolerant)
    auto text = xlings::platform::read_file_to_string(file.string());
    auto j = nlohmann::json::parse(text);
    EXPECT_TRUE(j["plain"].is_string());
    EXPECT_TRUE(j["custom"].is_object());
    fs::remove_all(dir);
}
```

- [ ] **Step 2:** `mcpp test` → FAIL (object values dropped on load; save always writes strings).
- [ ] **Step 3: Implement.**

`load_sub_repos_json` loop body:

```cpp
for (auto it = json.begin(); it != json.end(); ++it) {
    IndexRepo repo;
    repo.name = it.key();
    if (it.value().is_string()) {
        repo.url = it.value().get<std::string>();
    } else if (it.value().is_object()) {
        // #377: object form carries the artifact declaration.
        auto& v = it.value();
        if (v.contains("url") && v["url"].is_string()) repo.url = v["url"].get<std::string>();
        if (v.contains("artifact") && v["artifact"].is_string())
            repo.artifactBase = v["artifact"].get<std::string>();
        if (v.contains("source") && v["source"].is_string())
            repo.source = v["source"].get<std::string>();
    }
    if (!repo.url.empty()) repos.push_back(std::move(repo));
}
```

`save_sub_repos_json` loop body:

```cpp
for (auto& repo : repos) {
    if (repo.artifactBase.empty() && repo.source.empty()) {
        json[repo.name] = repo.url;       // legacy string form
    } else {
        nlohmann::json v;
        v["url"] = repo.url;
        if (!repo.artifactBase.empty()) v["artifact"] = repo.artifactBase;
        if (!repo.source.empty())       v["source"]   = repo.source;
        json[repo.name] = v;
    }
}
```

- [ ] **Step 4:** `mcpp build && mcpp test` → PASS.
- [ ] **Step 5:** `git add -A && git commit -m "feat(xim): object-format xim-indexrepos.json entries for artifact repos (#377)"`

---

### Task 6: e2e — custom index artifact + fallback matrix

**Files:**
- Create: `tests/e2e/custom_index_artifact_test.sh` (executable)
- Modify: `tests/e2e/run_all.sh` (register `E2E-30 |custom_index_artifact_test.sh||` after E2E-29)

Model on `tests/e2e/index_artifact_update_test.sh` (same binary discovery, pass/fail helpers, mktemp WORK + trap). Scenarios, each with a FRESH `XLINGS_HOME`:

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# E2E-30 (#377): user-defined index repos with a declared artifact source.
#   A. source=artifact + dead git URL  -> installs purely from artifact (exact key)
#   B. sole-entry key fallback         -> pointer key != repo name still resolves
#   C. auto + broken artifact          -> falls back to the local-link path
#   D. existing git checkout + artifact-> migrates (.git gone, marker present)
#   E. source=git + artifact declared  -> artifact never fetched
# Hermetic: official main index served from a local XLINGS_INDEX_BASE_URL dir;
# custom repo uses its own local flat base (also proves the global base
# override does NOT leak into custom sources).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XLINGS_BIN="${1:-}"
if [[ -z "$XLINGS_BIN" ]]; then
  XLINGS_BIN="$(find "$PROJECT_DIR/build" -path '*debug*' -name xlings -type f -perm -111 2>/dev/null | head -1)"
fi
[[ -x "$XLINGS_BIN" ]] || { echo "[test] SKIP: no xlings binary (build first)"; exit 0; }
pass() { echo "[test] OK: $*"; }
fail() { echo "[test] FAIL: $*" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xim-custom-idx.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ── shared fixtures ──────────────────────────────────────────────
# official main index artifact (hermetic main sync)
MAIN_SRC="$WORK/main-src"; mkdir -p "$MAIN_SRC/pkgs/p"
echo 'package({name="patchelf"})' > "$MAIN_SRC/pkgs/p/patchelf.lua"
MAIN_SERVE="$WORK/main-serve"; mkdir -p "$MAIN_SERVE"
bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" --version 9.9.9 --out "$MAIN_SERVE" --src "$MAIN_SRC"
python3 - "$MAIN_SERVE"/xim-index-9.9.9.manifest.json "$MAIN_SERVE/xim-index-pointers.json" <<'PY'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {"xim": m}}, open(sys.argv[2], "w"))
PY

# custom index artifact under its own flat local base dir "myindex"
CUSTOM_SRC="$WORK/custom-src"; mkdir -p "$CUSTOM_SRC/pkgs/h"
echo 'package({name="hellopkg"})' > "$CUSTOM_SRC/pkgs/h/hellopkg.lua"
CUSTOM_BASE="$WORK/myindex"; mkdir -p "$CUSTOM_BASE"
bash "$PROJECT_DIR/tools/build_xim_index_artifact.sh" --version 1.2.3 --out "$CUSTOM_BASE" --src "$CUSTOM_SRC"
CUSTOM_MANIFEST="$(ls "$CUSTOM_BASE"/*.manifest.json | head -1)"
make_pointer() {  # $1=key  -> writes myindex-pointers.json
python3 - "$CUSTOM_MANIFEST" "$CUSTOM_BASE/myindex-pointers.json" "$1" <<'PY'
import sys, json
m = json.load(open(sys.argv[1]))
json.dump({"format_version": 1, "indexes": {sys.argv[3]: m}}, open(sys.argv[2], "w"))
PY
}

# local fallback source (a plain dir with pkgs/ -> local-link fallback path)
LOCAL_GIT="$WORK/localsrc"; mkdir -p "$LOCAL_GIT/pkgs/z"
echo 'package({name="zpkg"})' > "$LOCAL_GIT/pkgs/z/zpkg.lua"

fresh_home() {  # $1=name $2=repo-json-entry -> sets XLINGS_HOME
  export XLINGS_HOME="$WORK/home-$1"
  mkdir -p "$XLINGS_HOME"
  "$XLINGS_BIN" self init >/dev/null 2>&1 || fail "self init failed ($1)"
  cat > "$XLINGS_HOME/.xlings.json" <<EOF
{"index_repos":[${2}]}
EOF
}
export XLINGS_INDEX_BASE_URL="$MAIN_SERVE"   # official main only; custom uses its own base

# ── A. artifact-only custom repo, dead git URL, exact key ────────
make_pointer custom1
fresh_home a "{\"name\":\"custom1\",\"url\":\"https://127.0.0.1:1/dead.git\",\"artifact\":\"$CUSTOM_BASE\",\"source\":\"artifact\"}"
"$XLINGS_BIN" update >"$WORK/a.log" 2>&1 || { cat "$WORK/a.log" >&2; fail "A: update failed"; }
D="$XLINGS_HOME/data/custom1"
[[ -f "$D/pkgs/h/hellopkg.lua" ]]        || { cat "$WORK/a.log" >&2; fail "A: custom index not installed from artifact"; }
[[ -f "$D/.xlings-index-version" ]]      || fail "A: version marker missing"
[[ ! -d "$D/.git" ]]                     || fail "A: unexpected .git (went to git path)"
pass "A: custom repo installed from artifact (source=artifact, dead git URL)"

# ── B. sole-entry key fallback (pointer key != repo name) ────────
make_pointer weirdkey
fresh_home b "{\"name\":\"custom1\",\"url\":\"https://127.0.0.1:1/dead.git\",\"artifact\":\"$CUSTOM_BASE\",\"source\":\"artifact\"}"
"$XLINGS_BIN" update >"$WORK/b.log" 2>&1 || { cat "$WORK/b.log" >&2; fail "B: update failed"; }
[[ -f "$XLINGS_HOME/data/custom1/pkgs/h/hellopkg.lua" ]] || fail "B: sole-entry key fallback broken"
pass "B: sole-entry pointer key fallback"

# ── C. auto + broken artifact -> local-link fallback ─────────────
fresh_home c "{\"name\":\"custom1\",\"url\":\"$LOCAL_GIT\",\"artifact\":\"$WORK/empty-base/none\",\"source\":\"auto\"}"
"$XLINGS_BIN" update >"$WORK/c.log" 2>&1 || { cat "$WORK/c.log" >&2; fail "C: update failed"; }
[[ -e "$XLINGS_HOME/data/custom1/pkgs/z/zpkg.lua" ]] || { cat "$WORK/c.log" >&2; fail "C: fallback did not link local source"; }
grep -qi "falling back" "$WORK/c.log" || fail "C: no fallback warning logged"
pass "C: auto mode fell back past a broken artifact base"

# ── D. migration: pre-existing git checkout replaced by artifact ─
make_pointer custom1
fresh_home d "{\"name\":\"custom1\",\"url\":\"https://127.0.0.1:1/dead.git\",\"artifact\":\"$CUSTOM_BASE\",\"source\":\"auto\"}"
D="$XLINGS_HOME/data/custom1"; mkdir -p "$D/.git" "$D/pkgs/old"
echo 'package({name="oldpkg"})' > "$D/pkgs/old/oldpkg.lua"
"$XLINGS_BIN" update >"$WORK/d.log" 2>&1 || { cat "$WORK/d.log" >&2; fail "D: update failed"; }
[[ -f "$D/pkgs/h/hellopkg.lua" ]]   || fail "D: artifact content missing after migration"
[[ ! -d "$D/.git" ]]                 || fail "D: .git survived migration"
[[ -f "$D/.xlings-index-version" ]]  || fail "D: marker missing after migration"
pass "D: existing git checkout migrated to artifact"

# ── E. source=git ignores the artifact declaration ───────────────
fresh_home e "{\"name\":\"custom1\",\"url\":\"$LOCAL_GIT\",\"artifact\":\"$CUSTOM_BASE\",\"source\":\"git\"}"
"$XLINGS_BIN" update >"$WORK/e.log" 2>&1 || { cat "$WORK/e.log" >&2; fail "E: update failed"; }
[[ -e "$XLINGS_HOME/data/custom1/pkgs/z/zpkg.lua" ]] || fail "E: git/local path not used"
[[ ! -f "$XLINGS_HOME/data/custom1/.xlings-index-version" ]] || fail "E: artifact fetched despite source=git"
pass "E: source=git forces the git/local path"

echo "[test] all custom index artifact scenarios passed"
```

- [ ] **Step 2:** `chmod +x tests/e2e/custom_index_artifact_test.sh`; register in `run_all.sh` (`"E2E-30 |custom_index_artifact_test.sh||"` line after E2E-29).
- [ ] **Step 3:** Run: `bash tests/e2e/custom_index_artifact_test.sh` → all scenarios pass. Debug notes: scenario A requires Task 3+4 wired through `syncRepos` (custom1 is a global main-list repo — defaults prepend `xim` ahead of it); C's `$LOCAL_GIT` plain-dir URL exercises the `is_local_repo_source` fallback branch inside the new `fallbackSync`.
- [ ] **Step 4:** Also re-run neighbors: `bash tests/e2e/index_artifact_update_test.sh && bash tests/e2e/interface_multi_repo_error_visibility_test.sh` → still green.
- [ ] **Step 5:** `git add -A && git commit -m "test(e2e): E2E-30 custom index artifact source matrix (#377)"`

---

### Task 7: Docs + version bump

**Files:**
- Modify: `docs/quick-start/custom-index.md` (add `artifact` + `source` fields with the mcpp example), `docs/spec/xlings-json-schema.md` (index_repos entry schema), `src/core/config.cppm` (VERSION).

- [ ] **Step 1:** Read both docs; add a concise section mirroring design §2.1–§2.2 (config fields, producer contract, `auto|artifact|git` semantics, region-object base). Keep each doc's existing language/tone (check whether they're zh/en first).
- [ ] **Step 2:** `Info::VERSION = "0.4.68";`
- [ ] **Step 3:** `mcpp build && mcpp test` (final full green), `git add -A && git commit -m "docs+chore: custom index artifact docs; bump 0.4.67 -> 0.4.68"`

---

### Task 8: PR, CI green

- [ ] **Step 1:** `git push -u origin feat/issue377-custom-index-artifact`
- [ ] **Step 2:** `gh pr create` — title: `feat(xim): custom index artifact sources with git fallback (#377); compact git CA pin (#378)`; body: problem summary, design-doc ref, scenario matrix, `Closes #377, Closes #378`, version bump note.
- [ ] **Step 3:** Watch ALL workflows: `gh pr checks <n> --watch` (linux, linux-e2e, linux-root, macos, windows, aarch64, archlinux). Fix failures with follow-up commits (each: build+test locally first). Windows caveat: new e2e is bash-only (registered in run_all.sh which linux-e2e runs); `resolve_ca_bundle` is exported cross-platform (pure fn) while `ensure_ca_env_` body is `#if defined(__linux__)` — keep the function compiled (not preprocessed away) on all platforms so unit tests link everywhere.

### Task 9: Squash bypass merge

- [ ] **Step 1:** After CI green: `gh pr merge <n> --squash --admin --delete-branch` with subject `feat(xim): custom index artifact sources with git fallback (#377) (#<pr>)` and a body paragraph in the style of cf9b60d (what + why + `Bumps 0.4.67 -> 0.4.68. Refs .agents/docs/2026-07-22-issue377-custom-index-artifact-design.md`).

### Task 10: Release 0.4.68

- [ ] **Step 1:** `gh workflow run release.yml --ref main` (version auto-read from config.cppm). Watch: `gh run watch <id>`.
- [ ] **Step 2:** Post-release per memory `project_release_cancel_recovery`: verify GitCode mirror sidecars ran (mirror-binaries) and the xim-pkgindex `xlings` recipe bump PR landed (bump-index can no-op on releases/latest API lag — if so, bump manually).

### Task 11: Ecosystem verification (real)

- [ ] **Step 1:** Fresh `XLINGS_HOME=$(mktemp -d)`, install released 0.4.68 (`XLINGS_VERSION=v0.4.68 quick_install.sh` path or `self update` from an existing install), write `.xlings.json` with `{"name":"mcpplibs","url":"https://github.com/mcpp-community/mcpp-index.git","artifact":"https://github.com/xlings-res/mcpp-index"}` and run `xlings update` → expect `[index] updated from artifact mcpp-index-<sha>.tar.gz` for the custom repo, `data/mcpplibs/.xlings-index-version` present, then `xlings search` finds an mcpp package.
- [ ] **Step 2:** #378 real check on this Ubuntu host (no `/etc/ssl/cert.pem`): `env -u GIT_SSL_CAINFO XLINGS_COMPACT_GIT_BIN=$HOME/.xlings/... xlings update` with a git-source custom repo → sync succeeds (previously: `error adding trust anchors`).
- [ ] **Step 3:** mcpp scenario: hand-edit `~/.mcpp/registry/.xlings.json` mcpplibs entry to add the artifact field; `mcpp index update` → artifact path taken.

### Task 12: mcpp adaptation issue

- [ ] **Step 1:** `gh issue create -R mcpp-community/mcpp` — title `feat(index): adopt xlings 0.4.68 per-repo artifact source for mcpplibs`; body from design §2.5: (1) config template + `seed_xlings_json` passthrough of `artifact`/`source`, (2) one-time migration for existing registry `.xlings.json` (precedent: `migrate_xlings_json_index_names`), (3) optional `[indices]` artifact field; link the xlings design doc, PR, release, and openxlings/xlings#377.

---

## Self-review checklist (done at write time)

- Spec coverage: design §2.1 (Task 2), §2.2 producer contract (consumed as-is; validated by Task 11), §2.3 layouts (Task 3), §2.4 items 1–4 (Tasks 2/3/4/5), §2.5 (Task 12), §2.6 (Task 1), testing section (Tasks 1–6), version/PR/release (Tasks 7–10).
- Known intentional deviations: none.
- Type consistency: `artifactBase`/`source` names used identically across config/repo/indexfetch/tests; `select_manifest(pointers, key, soleEntryFallback)` 3-arg everywhere; `sub_should_attempt_artifact` 6th param defaulted.
