export module xlings.core.xim.indexfetch;

// Index-as-Resource fetch (Y-asset; see
// .agents/docs/2026-06-22-index-as-resource-impl-plan.md).
//
// Instead of `git clone`-ing the package index from GitHub at runtime, fetch a
// versioned index *artifact* (tar.gz + manifest.json) published to xlings-res
// over the SAME resource-server path that package binaries use. This routes
// index acquisition through the in-process HTTP downloader, which already has
// the 0.4.49 adaptive mirror reorder + stall watchdog — closing gap G2 (the
// git-clone path never got that resilience).
//
// Trust/discovery anchor: a fixed rolling release tag ("latest") under the
// xim-index repo on each resource server. The pointer asset
// (xim-index[-<name>]-latest.json) is the entry point; it names the artifact +
// its sha256. The fetch verifies sha256 against the manifest, extracts to a
// temp dir, then atomically renames into place. git clone remains the caller's
// fallback.
//
// Left room for the future X-full upgrade (manifest carries format_version +
// signature; fetch is structured so a signed pointer / ETag-304 can slot in
// without a rewrite).

import std;
import xlings.libs.json;
import xlings.libs.tinyhttps;
import xlings.libs.sha256;
import xlings.core.log;
import xlings.core.config;
import xlings.core.mirror;
import xlings.platform;
import xlings.core.xim.extract;

export namespace xlings::xim {

struct IndexManifest {
    int           format_version = 0;
    std::string   index_version;
    std::string   index_name;        // "xim" for the main index, else sub name
    std::string   generated_at;
    std::string   source_commit;
    std::string   artifact_name;     // e.g. xim-index-0.4.52.tar.gz
    std::string   artifact_sha256;   // lowercase hex
    std::uint64_t artifact_size = 0;
};

// Parse a manifest JSON. Returns nullopt if required fields are missing/invalid.
std::optional<IndexManifest> parse_index_manifest(std::string_view jsonText);

// Build candidate download URLs for an asset filename under the xim-index repo
// on every configured resource server (selected/latency-probed server first),
// then GitHub proxy variants. `mirror` selects region (empty => Config default).
// `version` (e.g. "0.4.58" / "72b00a4") selects the release tag: when given,
// the versioned tag `v<version>` is tried first (GitCode only serves assets via
// the versioned tag, not the rolling `latest`), with `latest` kept as fallback.
std::vector<std::string> index_asset_urls(std::string_view filename,
                                          std::string_view mirror = {},
                                          std::string_view version = {});

// Fetch the latest index artifact for `subName` ("" = main index) and atomically
// install it into destIndexDir. Returns true on success; on failure `err` is set
// and destIndexDir is left untouched (caller may fall back to git).
bool fetch_index_artifact(const std::filesystem::path& destIndexDir,
                          std::string& err,
                          std::string_view subName = {});

// Reconcile leftover index temp dirs from crashed / SIGKILL'd runs: restore an
// index dir orphaned by an interrupted swap (`<base>.old.<deadpid>` holding
// pkgs/) and sweep leaked `.artifact.*` / `.tmp.*` / spent `.old.*` staging from
// dead processes. Dirs owned by a live process are left untouched. Idempotent;
// call before any git-vs-artifact gating so a transient gap can't trigger a
// destructive git clone. See 2026-06-30 regression analysis.
void reconcile_index_temps(const std::filesystem::path& dataDir);

} // namespace xlings::xim


namespace xlings::xim {

namespace detail_ {

// Repo + tag are fixed discovery anchors; overridable for testing/mirrors.
std::string index_repo_name_() {
    if (auto* e = std::getenv("XLINGS_INDEX_REPO"); e && *e) return e;
    return "xim-index";
}
std::string index_tag_() {
    if (auto* e = std::getenv("XLINGS_INDEX_TAG"); e && *e) return e;
    return "latest";
}

std::string lower_hex_(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

// Download the first working candidate URL into destFile, trying each candidate
// explicitly and continuing past ANY failure — including HTTP 404. This differs
// from tinyhttps::download_file's internal candidate loop, which treats 404 as a
// definitive not-found and stops (correct for a single authoritative asset, but
// wrong for index mirrors: one server 404ing must fall through to the next, e.g.
// gitcode 404 -> github). When wantSha256 is set, a candidate is accepted only
// if its bytes match. Returns empty string on success, else the last error.
std::string download_candidates_(std::vector<std::string> urls,
                                 const std::filesystem::path& destFile,
                                 std::string_view wantSha256) {
    if (urls.empty()) return "no candidate URLs";
    // With a pinned sha256, mirrors may race ahead of the authoritative URL;
    // without one (the manifest pointer), keep the authoritative server first.
    urls = mirror::adaptive::reorder(std::move(urls), !wantSha256.empty());

    std::string want = wantSha256.empty() ? std::string{} : lower_hex_(std::string(wantSha256));
    std::string lastErr;
    for (const auto& u : urls) {
        tinyhttps::DownloadOptions opts;
        opts.destFile = destFile;
        opts.urls = { u };          // one URL per call: we own the fallthrough
        opts.retryCount = 1;        // breadth over depth; don't hammer a 404
        opts.onUrlAttemptFailed = [](const std::string& url, const std::string& e) {
            // Demote a host that stalled / served bad bytes / timed out / gave no
            // response so later fetches this session skip it. Do NOT penalize on
            // plain 404/401: the asset is just absent on that mirror (e.g. gitcode
            // 404s .json) — the host is fine for other assets.
            if (e.find("404") == std::string::npos && e.find("401") == std::string::npos)
                mirror::adaptive::penalize_host(url);
        };
        if (!want.empty()) {
            opts.onVerify = [destFile, want](const std::string& url) -> std::string {
                auto digest = sha256::hex_file(destFile);
                if (digest && *digest == want) return {};
                return std::format("sha256 mismatch (source {}): got {}, want {}",
                                   url, digest ? *digest : "<unreadable>", want);
            };
        }
        auto r = tinyhttps::download_file(opts);
        if (r.success) return {};
        lastErr = r.error.empty() ? ("failed: " + u) : r.error;
        log::debug("[index] candidate failed ({}): {}", u, lastErr);
    }
    return lastErr.empty() ? "all candidates failed" : lastErr;
}

// Index source base override (env now; config key folded in by the caller via
// Config). When set to a local dir / file:// it's copied; a remote base means
// "<base>/<filename>". Empty => use the passed remoteUrls (xlings-res default).
struct BaseOverride {
    std::string base;                      // remote base, or empty
    std::optional<std::filesystem::path> local;  // local dir, if base is local
};
BaseOverride resolve_base_() {
    BaseOverride b;
    std::string v;
    if (auto* e = std::getenv("XLINGS_INDEX_BASE_URL"); e && *e) v = e;
    else v = Config::index_base();   // .xlings.json xim.index-base (region-aware), else ""
    if (v.empty()) return b;
    while (!v.empty() && v.back() == '/') v.pop_back();
    b.base = v;
    if (v.starts_with("file://")) b.local = std::filesystem::path(v.substr(7));
    else if (v.find("://") == std::string::npos) b.local = std::filesystem::path(v);
    return b;
}

// Obtain `filename` into `dest`: local-dir copy, or remote (base override, else
// the given remoteUrls). Verifies sha256 when wantSha is non-empty.
std::string obtain_file(const std::string& filename, std::vector<std::string> remoteUrls,
                        const std::filesystem::path& dest, std::string_view wantSha) {
    namespace fs = std::filesystem;
    auto b = resolve_base_();
    if (b.local) {
        std::error_code ec;
        auto src = *b.local / filename;
        if (!fs::exists(src, ec)) return std::format("local file not found: {}", src.string());
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) return std::format("copy {} failed: {}", src.string(), ec.message());
        if (!wantSha.empty()) {
            auto digest = sha256::hex_file(dest);
            auto want = lower_hex_(std::string(wantSha));
            if (!digest || *digest != want)
                return std::format("sha256 mismatch (local {}): got {}, want {}",
                                   src.string(), digest ? *digest : "<unreadable>", want);
        }
        return {};
    }
    auto urls = b.base.empty() ? std::move(remoteUrls)
                               : std::vector<std::string>{ b.base + "/" + filename };
    return download_candidates_(std::move(urls), dest, wantSha);
}

} // namespace detail_

std::optional<IndexManifest> parse_index_manifest(std::string_view jsonText) {
    try {
        auto j = nlohmann::json::parse(jsonText);
        if (!j.is_object() || !j.contains("artifact") || !j["artifact"].is_object())
            return std::nullopt;
        auto& art = j["artifact"];
        if (!art.contains("name") || !art.contains("sha256")) return std::nullopt;

        IndexManifest m;
        m.format_version  = j.value("format_version", 0);
        m.index_version   = j.value("index_version", std::string{});
        m.index_name      = j.value("index_name", std::string{"xim"});
        m.generated_at    = j.value("generated_at", std::string{});
        m.source_commit   = j.value("source_commit", std::string{});
        m.artifact_name   = art.value("name", std::string{});
        m.artifact_sha256 = detail_::lower_hex_(art.value("sha256", std::string{}));
        m.artifact_size   = art.value("size", std::uint64_t{0});
        if (m.artifact_name.empty() || m.artifact_sha256.empty()) return std::nullopt;
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<std::string> index_asset_urls(std::string_view filename,
                                          std::string_view mirror,
                                          std::string_view version) {
    std::vector<std::string> urls;
    auto add = [&](const std::string& u) {
        if (!u.empty() && std::ranges::find(urls, u) == urls.end()) urls.push_back(u);
    };

    auto repo = detail_::index_repo_name_();

    // Release tag(s) to try, in order. GitCode only serves release assets via
    // the VERSIONED tag (`v<version>`); its rolling `latest` tag 404s on the
    // `releases/download/latest/<asset>` path (GitHub serves both). So when we
    // know the version, try `v<version>` first and keep `latest` as a fallback.
    std::vector<std::string> tags;
    if (!version.empty()) tags.push_back("v" + std::string(version));
    if (auto def = detail_::index_tag_(); std::ranges::find(tags, def) == tags.end())
        tags.push_back(def);

    // Selected (latency-probed) server first, then all other candidates.
    auto selected = Config::resource_server(mirror);
    auto buildFor = [&](const std::string& server, const std::string& tag) {
        return std::format("{}/{}/releases/download/{}/{}", server, repo, tag, filename);
    };
    auto addServer = [&](const std::string& server) {
        if (server.empty()) return;
        for (auto& tag : tags) add(buildFor(server, tag));
    };
    addServer(selected);
    for (auto& server : Config::resource_servers(mirror)) addServer(server);
    // Always include GLOBAL (github) servers as fallback for the index — even
    // under CN. Unlike package binaries (where the regional mirror is
    // authoritative), the index must stay reachable if the regional mirror
    // lacks the asset; combined with 404-fallthrough this gives CN: gitcode ->
    // github -> github proxies.
    for (auto& server : Config::resource_servers("GLOBAL")) addServer(server);

    // NB: deliberately NO github-proxy expansion (ghfast/ghproxy/kkgithub) for
    // the index. Those proxies are often TCP-reachable but don't actually serve
    // the asset, so a fast-latency-but-dead proxy ranked ahead of github causes
    // a ~30s timeout per dead host (the CN "fetching package index" hang). The
    // index is small; gitcode + github-direct (region-ordered) is enough, and a
    // gitcode 404/miss falls through to github fast.
    return urls;
}

// Raw-file URLs for the rolling pointer (a file committed to the index repo,
// updated by git push, served raw). github.com/<org> ->
// raw.githubusercontent.com/<org>/<repo>/main/<f>; gitcode.com/<org> ->
// raw.gitcode.com/<org>/<repo>/raw/main/<f>. Region-ordered + GLOBAL fallback.
std::vector<std::string> index_pointer_urls(std::string_view filename,
                                            std::string_view mirror) {
    auto repo = detail_::index_repo_name_();
    std::vector<std::string> urls;
    auto add = [&](const std::string& u){
        if (!u.empty() && std::ranges::find(urls, u) == urls.end()) urls.push_back(u);
    };
    auto rawFor = [&](const std::string& server) -> std::string {
        auto pos = server.find("://");
        std::string rest = pos == std::string::npos ? server : server.substr(pos + 3);
        auto slash = rest.find('/');
        if (slash == std::string::npos) return {};
        std::string host = rest.substr(0, slash);
        std::string org  = rest.substr(slash + 1);
        while (!org.empty() && org.back() == '/') org.pop_back();
        if (host.find("github.com") != std::string::npos)
            return std::format("https://raw.githubusercontent.com/{}/{}/main/{}", org, repo, filename);
        if (host.find("gitcode.com") != std::string::npos)
            // Correct raw form is .../<o>/<r>/raw/main/<f> — it serves the RAW
            // bytes (incl .json). The .../main/<f> form (no /raw/) returns the
            // HTML web page. (403s seen while probing were request rate-limiting.)
            return std::format("https://raw.gitcode.com/{}/{}/raw/main/{}", org, repo, filename);
        return {};
    };
    auto selected = Config::resource_server(mirror);
    if (!selected.empty()) add(rawFor(selected));
    for (auto& s : Config::resource_servers(mirror))   add(rawFor(s));
    for (auto& s : Config::resource_servers("GLOBAL")) add(rawFor(s));
    return urls;
}

// Cached fetch of the COMBINED pointer file (xim-index-pointers.json): ONE raw
// fetch per process covering ALL indexes (main + subs). One fetch (vs one per
// index) avoids gitcode raw rate-limiting, and is the single file a self-hosted
// index server must serve. Format:
//   {"format_version":1,"indexes":{"xim":{<manifest>},"awesome":{<manifest>},...}}
const std::map<std::string, IndexManifest>& load_index_pointers(std::string_view mirror) {
    static std::map<std::string, IndexManifest> cache;
    static std::once_flag once;
    std::call_once(once, [&] {
        namespace fs = std::filesystem;
        auto tmp = fs::temp_directory_path() /
                   std::format("xim-index-pointers.{}.json", platform::get_pid());
        std::error_code ec; fs::remove(tmp, ec);
        auto err = detail_::obtain_file("xim-index-pointers.json",
                       index_pointer_urls("xim-index-pointers.json", mirror), tmp, {});
        if (!err.empty()) { log::warn("[index] pointer fetch failed: {}", err); return; }
        std::string text;
        { std::ifstream in(tmp, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); text = ss.str(); }
        fs::remove(tmp, ec);
        try {
            auto j = nlohmann::json::parse(text);
            if (j.contains("indexes") && j["indexes"].is_object())
                for (auto it = j["indexes"].begin(); it != j["indexes"].end(); ++it)
                    if (auto m = parse_index_manifest(it.value().dump())) cache[it.key()] = *m;
        } catch (...) { log::warn("[index] pointer parse failed"); }
    });
    return cache;
}

bool fetch_index_artifact(const std::filesystem::path& destIndexDir,
                          std::string& err,
                          std::string_view subName) {
    namespace fs = std::filesystem;
    auto mirrorKey = Config::mirror();
    std::string key = subName.empty() ? std::string("xim") : std::string(subName);

    std::string label = subName.empty()
        ? std::string("package index")
        : std::format("sub-index '{}'", subName);
    log::info("[index] fetching {} (mirror={})...",
              label, mirrorKey.empty() ? "GLOBAL" : mirrorKey);

    auto& pointers = load_index_pointers(mirrorKey);
    auto pit = pointers.find(key);
    if (pit == pointers.end()) { err = std::format("no pointer entry for '{}'", key); return false; }
    const IndexManifest& manifest = pit->second;
    if (manifest.format_version != 1) {
        err = std::format("unsupported index manifest format_version {}", manifest.format_version);
        return false;
    }

    std::error_code ec;
    auto tmpRoot = fs::path(destIndexDir.string() + ".artifact." +
                            std::to_string(platform::get_pid()));
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);
    if (ec) { err = std::format("create temp dir failed: {}", ec.message()); return false; }
    struct Cleanup { fs::path p; ~Cleanup(){ std::error_code e; fs::remove_all(p,e);} } cleanup{tmpRoot};

    // Artifact (sha256-pinned by the manifest); release asset, versioned name.
    auto artifactFile = tmpRoot / manifest.artifact_name;
    if (auto e = detail_::obtain_file(manifest.artifact_name,
                    index_asset_urls(manifest.artifact_name, mirrorKey, manifest.index_version),
                    artifactFile, manifest.artifact_sha256); !e.empty()) {
        err = std::format("fetch index artifact failed: {}", e);
        return false;
    }

    // 3. Extract to a staging dir, sanity-check, atomically swap into place.
    auto stage = tmpRoot / "stage";
    auto extracted = extract_archive(artifactFile, stage);
    if (!extracted) { err = std::format("extract index artifact failed: {}", extracted.error()); return false; }
    if (!fs::exists(stage / "pkgs", ec)) {
        err = "extracted index missing pkgs/";
        return false;
    }

    // Record the installed version for diagnostics / future ETag-style checks.
    {
        std::ofstream v(stage / ".xlings-index-version", std::ios::binary);
        v << manifest.index_version;
    }

    auto backup = fs::path(destIndexDir.string() + ".old." +
                           std::to_string(platform::get_pid()));
    fs::remove_all(backup, ec);
    if (fs::exists(destIndexDir, ec)) {
        // Preferred path: a single atomic exchange of the live dir and the staged
        // dir. There is no instant where destIndexDir is missing, so a crash /
        // SIGKILL mid-swap cannot strand the index (which previously degraded the
        // official index to a destructive git clone on the next run — see
        // .agents/docs/2026-06-30-index-artifact-git-regression-analysis.md).
        if (platform::atomic_swap_paths(destIndexDir, stage)) {
            // After the exchange, `stage` now holds the OLD tree — drop it.
            fs::remove_all(stage, ec);
            log::info("[index] updated from artifact {} ({})",
                      manifest.artifact_name,
                      manifest.index_version.empty() ? "?" : manifest.index_version);
            return true;
        }
        // Fallback (non-Linux / kernel without renameat2): manual move-aside.
        // Narrow the kill-window by keeping the backup recoverable — a leftover
        // `.old.<pid>` that still holds pkgs/ is restored on the next run by
        // reconcile_index_temps() before any git fallback can fire.
        fs::rename(destIndexDir, backup, ec);
        if (ec) { err = std::format("swap (move old) failed: {}", ec.message()); return false; }
    }
    fs::rename(stage, destIndexDir, ec);
    if (ec) {
        // Roll back to the old tree if the swap-in failed.
        std::error_code e2;
        if (fs::exists(backup, e2)) fs::rename(backup, destIndexDir, e2);
        err = std::format("swap (move new) failed: {}", ec.message());
        return false;
    }
    fs::remove_all(backup, ec);

    log::info("[index] updated from artifact {} ({})",
              manifest.artifact_name,
              manifest.index_version.empty() ? "?" : manifest.index_version);
    return true;
}

// Reconcile leftover index temp dirs from crashed / SIGKILL'd runs. Two jobs:
//   1) RECOVER: if a live index dir is missing or empty but a sibling
//      `<base>.old.<pid>` (from an interrupted artifact swap) still holds pkgs/,
//      move it back into place — BEFORE any caller mistakes the gap for "no
//      index" and triggers a destructive git clone (the regression root cause).
//   2) SWEEP: delete `<base>.artifact.<pid>` / `.tmp.<pid>` / spent `.old.<pid>`
//      staging dirs whose owning process is dead. The RAII cleanup in
//      fetch_index_artifact / sync_repo never runs on SIGKILL, so these
//      otherwise accumulate without bound (10+ observed in the wild).
// Dirs still owned by a live process are left untouched (concurrency-safe).
// See .agents/docs/2026-06-30-index-artifact-git-regression-analysis.md.
void reconcile_index_temps(const std::filesystem::path& dataDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dataDir, ec)) return;

    struct Temp { fs::path path; fs::path base; std::string kind; int pid; };
    std::vector<Temp> temps;
    // Compare against std::default_sentinel (not a second directory_iterator):
    // clang/libc++ only provides operator==(default_sentinel_t), so a two-iterator
    // range-for fails to compile there. Increment with the error_code overload so
    // a bad entry never throws out of this best-effort reconciler.
    for (auto it = fs::directory_iterator(dataDir, ec);
         it != std::default_sentinel; it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        std::error_code dec;
        if (!entry.is_directory(dec)) continue;
        auto name = entry.path().filename().string();
        for (std::string_view kind : {".artifact.", ".old.", ".tmp."}) {
            auto pos = name.rfind(kind);
            if (pos == std::string::npos) continue;
            auto pidStr = name.substr(pos + kind.size());
            int pid = 0;
            auto* begin = pidStr.data();
            auto* end = begin + pidStr.size();
            auto [ptr, errc] = std::from_chars(begin, end, pid);
            if (errc != std::errc{} || ptr != end) break;  // suffix isn't a bare pid
            temps.push_back({entry.path(), dataDir / name.substr(0, pos),
                             std::string(kind), pid});
            break;
        }
    }

    for (auto& t : temps) {
        // Never touch staging owned by an in-flight xlings process.
        if (t.pid > 0 && platform::is_process_alive(t.pid)) continue;

        bool baseUsable   = fs::exists(t.base / "pkgs", ec);
        bool orphanUsable = fs::exists(t.path / "pkgs", ec);

        if (t.kind == ".old." && orphanUsable && !baseUsable) {
            // Interrupted swap: this orphan is the only surviving index copy.
            fs::remove_all(t.base, ec);          // clear an empty / partial base
            fs::rename(t.path, t.base, ec);
            if (!ec)
                log::warn("[index] recovered interrupted index swap: restored {} from {}",
                          t.base.filename().string(), t.path.filename().string());
            continue;
        }
        // Spent staging / leaked temp from a dead process — reclaim the space.
        fs::remove_all(t.path, ec);
    }
}

} // namespace xlings::xim
