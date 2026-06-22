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
std::vector<std::string> index_asset_urls(std::string_view filename,
                                          std::string_view mirror = {});

// Fetch the latest index artifact for `subName` ("" = main index) and atomically
// install it into destIndexDir. Returns true on success; on failure `err` is set
// and destIndexDir is left untouched (caller may fall back to git).
bool fetch_index_artifact(const std::filesystem::path& destIndexDir,
                          std::string& err,
                          std::string_view subName = {});

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
                                          std::string_view mirror) {
    std::vector<std::string> urls;
    auto add = [&](const std::string& u) {
        if (!u.empty() && std::ranges::find(urls, u) == urls.end()) urls.push_back(u);
    };

    auto repo = detail_::index_repo_name_();
    auto tag  = detail_::index_tag_();

    // Selected (latency-probed) server first, then all other candidates.
    auto selected = Config::resource_server(mirror);
    auto buildFor = [&](const std::string& server) {
        return std::format("{}/{}/releases/download/{}/{}", server, repo, tag, filename);
    };
    if (!selected.empty()) add(buildFor(selected));
    for (auto& server : Config::resource_servers(mirror)) add(buildFor(server));
    // Always include GLOBAL (github) servers as fallback for the index — even
    // under CN. Unlike package binaries (where the regional mirror is
    // authoritative), the index must stay reachable if the regional mirror
    // lacks the asset; combined with 404-fallthrough this gives CN: gitcode ->
    // github -> github proxies.
    for (auto& server : Config::resource_servers("GLOBAL")) add(buildFor(server));

    // GitHub proxy variants (ghfast/ghproxy/...) for the github-hosted URLs.
    std::vector<std::string> expanded;
    for (auto& u : urls) {
        for (auto& m : mirror::expand(u, {.type = mirror::ResourceType::Release}))
            expanded.push_back(std::move(m));
    }
    for (auto& u : expanded) add(u);
    return urls;
}

bool fetch_index_artifact(const std::filesystem::path& destIndexDir,
                          std::string& err,
                          std::string_view subName) {
    namespace fs = std::filesystem;
    auto mirrorKey = Config::mirror();

    std::string base = subName.empty()
        ? "xim-index"
        : std::format("xim-index-{}", subName);
    // Pointer is a .tar.gz (containing manifest.json), NOT a .json: GitCode
    // serves .tar.gz release assets (200) but 404s .json, so a .json pointer
    // forced CN clients to fall through to dead github proxies (multi-minute
    // hang). A .tar.gz pointer is fetched from the regional mirror natively.
    std::string pointerName = base + "-latest.tar.gz";

    // User-facing progress: index sync used to run silently (esp. when a CN
    // mirror was slow), looking like a hang. Announce what we're fetching.
    std::string label = subName.empty()
        ? std::string("package index")
        : std::format("sub-index '{}'", subName);
    log::info("[index] fetching {} (mirror={})...",
              label, mirrorKey.empty() ? "GLOBAL" : mirrorKey);

    std::error_code ec;
    auto tmpRoot = fs::path(destIndexDir.string() + ".artifact." +
                            std::to_string(platform::get_pid()));
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);
    if (ec) { err = std::format("create temp dir failed: {}", ec.message()); return false; }
    struct Cleanup { fs::path p; ~Cleanup(){ std::error_code e; fs::remove_all(p,e);} } cleanup{tmpRoot};

    // Optional base override (testing / private mirror / offline bundle): a
    // local directory (plain path or file://) is copied directly — this is also
    // the offline/bundled-release path — while a remote base goes over HTTP.
    // No override => the resource-server release URLs (index_asset_urls).
    std::string baseOverride;
    std::optional<fs::path> localBase;
    if (auto* ov = std::getenv("XLINGS_INDEX_BASE_URL"); ov && *ov) {
        baseOverride = ov;
        while (!baseOverride.empty() && baseOverride.back() == '/') baseOverride.pop_back();
        std::string p = baseOverride;
        if (p.starts_with("file://")) localBase = fs::path(p.substr(7));
        else if (p.find("://") == std::string::npos) localBase = fs::path(p);
    }
    auto obtain = [&](const std::string& filename, const fs::path& dest,
                      std::string_view wantSha) -> std::string {
        if (localBase) {
            std::error_code ec2;
            auto src = *localBase / filename;
            if (!fs::exists(src, ec2)) return std::format("local file not found: {}", src.string());
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec2);
            if (ec2) return std::format("copy {} failed: {}", src.string(), ec2.message());
            if (!wantSha.empty()) {
                auto digest = sha256::hex_file(dest);
                auto want = detail_::lower_hex_(std::string(wantSha));
                if (!digest || *digest != want)
                    return std::format("sha256 mismatch (local {}): got {}, want {}",
                                       src.string(), digest ? *digest : "<unreadable>", want);
            }
            return {};
        }
        auto urls = baseOverride.empty()
            ? index_asset_urls(filename, mirrorKey)
            : std::vector<std::string>{ baseOverride + "/" + filename };
        return detail_::download_candidates_(std::move(urls), dest, wantSha);
    };

    // 1. Manifest pointer (a .tar.gz containing manifest.json).
    auto pointerFile = tmpRoot / pointerName;
    if (auto e = obtain(pointerName, pointerFile, {}); !e.empty()) {
        err = std::format("fetch index manifest failed: {}", e);
        return false;
    }
    auto ptrDir = tmpRoot / "ptr";
    auto ptrEx = extract_archive(pointerFile, ptrDir);
    if (!ptrEx) { err = std::format("extract index pointer failed: {}", ptrEx.error()); return false; }
    std::string manifestText;
    {
        std::ifstream in(ptrDir / "manifest.json", std::ios::binary);
        std::stringstream ss; ss << in.rdbuf(); manifestText = ss.str();
    }
    auto manifest = parse_index_manifest(manifestText);
    if (!manifest) { err = "index manifest parse failed"; return false; }
    if (manifest->format_version != 1) {
        err = std::format("unsupported index manifest format_version {}", manifest->format_version);
        return false;
    }

    // 2. Artifact (sha256-pinned by the manifest).
    auto artifactFile = tmpRoot / manifest->artifact_name;
    if (auto e = obtain(manifest->artifact_name, artifactFile, manifest->artifact_sha256); !e.empty()) {
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
        v << manifest->index_version;
    }

    auto backup = fs::path(destIndexDir.string() + ".old." +
                           std::to_string(platform::get_pid()));
    fs::remove_all(backup, ec);
    if (fs::exists(destIndexDir, ec)) {
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
              manifest->artifact_name,
              manifest->index_version.empty() ? "?" : manifest->index_version);
    return true;
}

} // namespace xlings::xim
