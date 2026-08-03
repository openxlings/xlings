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
import xlings.core.version_order;

export namespace xlings::xim {

// A client-version constraint an index snapshot declares: [min, max).
// Both optional; empty means unbounded on that side.
//
// Deliberately two bare version strings and NOT a range expression. Two bounds
// already ARE a range, and a second grammar next to semver's would drift from
// it -- while inheriting the defect measured below. See #476 design doc §1.
struct IndexRequirement {
    std::string min;   // inclusive
    std::string max;   // exclusive
    bool empty() const { return min.empty() && max.empty(); }
};

// One addressable index snapshot: an artifact plus what it asks of its client.
struct IndexSnapshot {
    std::string   index_version;
    std::string   generated_at;
    std::string   artifact_name;
    std::string   artifact_sha256;   // lowercase hex
    std::uint64_t artifact_size = 0;
    // consumer name -> {min,max}. xlings evaluates ONLY the "xlings" key; every
    // other key is carried verbatim for whoever it belongs to.
    nlohmann::json requirements = nlohmann::json::object();
};

struct IndexManifest {
    int           format_version = 0;
    std::string   index_version;
    std::string   index_name;        // "xim" for the main index, else sub name
    std::string   generated_at;
    std::string   source_commit;
    std::string   artifact_name;     // e.g. xim-index-0.4.52.tar.gz
    std::string   artifact_sha256;   // lowercase hex
    std::uint64_t artifact_size = 0;

    // ── #476: version contract + addressable history ──────────────────
    // All optional. A pointer that predates them parses exactly as before,
    // which is why they are ADDED rather than the manifest being restructured:
    // the parser below hard-requires `artifact` at this level, so moving it
    // under a `latest` node would make every released client reject the
    // pointer outright.
    nlohmann::json              requirements = nlohmann::json::object();
    std::vector<IndexSnapshot>  history;            // newest first; [0] == this
    bool                        history_truncated = false;
};

// The requirement `consumer` must meet, or nullopt when unconstrained.
std::optional<IndexRequirement> requirement_for(const nlohmann::json& requirements,
                                                std::string_view consumer);

// Does `selfVersion` satisfy `req`?
//
// Compared with version_order::compare, NOT semver::satisfies_expr. Measured
// on 2026-08-04: semver::Version is three components, so it cannot parse
// xlings's own four-component YYYY.M.D.N at all --
// `satisfies_expr("2026.8.3.2", ">=2026.8.3.1")` returns FALSE. Building the
// contract on it would make every client fail every check and report "no
// compatible snapshot", which reads as a publishing problem rather than a
// parser that does not know the version scheme.
bool satisfies_requirement(std::string_view selfVersion, const IndexRequirement& req);

// Every addressable snapshot of a manifest, newest first. A manifest with no
// history yields exactly one -- itself -- so callers have a single shape.
std::vector<IndexSnapshot> snapshots_of(const IndexManifest& manifest);

struct SnapshotChoice {
    IndexSnapshot snapshot;
    bool          isNewest = true;
    std::string   reason;   // why we stepped back; empty when isNewest
};

// Pick the snapshot this client should use.
//
//   pin empty / "latest" -> newest snapshot whose contract this client meets
//   pin set              -> that exact snapshot, contract check bypassed
//                           (you asked for it) but sha256 still enforced
//
// Returns an error rather than falling back: silently handing back the newest
// snapshot would give the client exactly the thing it was routing away from.
std::expected<SnapshotChoice, std::string> choose_snapshot(
    const IndexManifest& manifest, std::string_view selfVersion, std::string_view pin);

// Parse a manifest JSON. Returns nullopt if required fields are missing/invalid.
std::optional<IndexManifest> parse_index_manifest(std::string_view jsonText);

// #377: a per-repo artifact source derived from IndexRepo.artifactBase.
// Forge bases (github/gitcode host) use raw-file pointer + releases/download
// assets; any other base (plain http(s), file://, local path) is a flat
// directory serving <base>/<filename>. The pointer file is
// `<repoName>-pointers.json` (repoName = last path segment of the base).
struct ArtifactSource {
    std::string base;      // no trailing slash
    std::string server;    // forge only: scheme://host/org ("" => flat/local)
    std::string repoName;  // last path segment of base — pointer file prefix
    std::string key;       // manifest lookup key (config repo name)
    std::optional<std::filesystem::path> localDir;  // set when base is local
    bool forge() const { return !server.empty(); }
};

// Build an ArtifactSource for a repo, or nullopt when it declares none.
std::optional<ArtifactSource> artifact_source_for(const IndexRepo& repo);

// Select the manifest for `key`: exact match; else, for CUSTOM sources only
// (soleEntryFallback), the sole entry — a single-index pointer need not repeat
// the consumer's configured repo name (mcpp publishes key "mcpp", consumers
// configure "mcpplibs"). NEVER sole-fallback for the official combined pointer:
// a missing sub entry there must fail, not silently serve the main index.
const IndexManifest* select_manifest(const std::map<std::string, IndexManifest>& pointers,
                                     std::string_view key, bool soleEntryFallback);

// Build candidate download URLs for an asset filename under the xim-index repo
// on every configured resource server (selected/latency-probed server first),
// then GitHub proxy variants. `mirror` selects region (empty => Config default).
// `version` (e.g. "0.4.58" / "72b00a4") selects the release tag: when given,
// the versioned tag `v<version>` is tried first (GitCode only serves assets via
// the versioned tag, not the rolling `latest`), with `latest` kept as fallback.
// #377: `custom` overrides the official server+repo with a per-repo source.
std::vector<std::string> index_asset_urls(std::string_view filename,
                                          std::string_view mirror = {},
                                          std::string_view version = {},
                                          const ArtifactSource* custom = nullptr);

// Raw-file URLs for the rolling pointer (see definition). #377: `custom`
// targets a per-repo source instead of the official xim-index repo.
std::vector<std::string> index_pointer_urls(std::string_view filename,
                                            std::string_view mirror,
                                            const ArtifactSource* custom = nullptr);

// Cached fetch of a combined pointer file: one raw fetch per process PER BASE
// ("" = the official xim-index-pointers.json; a custom source's base fetches
// `<repoName>-pointers.json` from that base).
const std::map<std::string, IndexManifest>& load_index_pointers(std::string_view mirror,
                                            const ArtifactSource* custom = nullptr);

// Fetch the latest index artifact for `subName` ("" = main index) and atomically
// install it into destIndexDir. Returns true on success; on failure `err` is set
// and destIndexDir is left untouched (caller may fall back to git).
// #377: `custom` fetches from a per-repo artifact source (pointer key =
// custom->key with sole-entry fallback) instead of the official pointer.
bool fetch_index_artifact(const std::filesystem::path& destIndexDir,
                          std::string& err,
                          std::string_view subName = {},
                          const ArtifactSource* custom = nullptr,
                          std::string_view pin = {});

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
// #377: `forced` replaces the global env/config base override entirely — a
// custom source must never be redirected by XLINGS_INDEX_BASE_URL; a forge
// custom source passes an EMPTY override ("use the URL list as-is").
std::string obtain_file(const std::string& filename, std::vector<std::string> remoteUrls,
                        const std::filesystem::path& dest, std::string_view wantSha,
                        const BaseOverride* forced = nullptr) {
    namespace fs = std::filesystem;
    auto b = forced ? *forced : resolve_base_();
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

// #377: forced base for a custom source: local-dir copy, flat-remote base, or
// (for forge bases) an EMPTY override meaning "use the passed URL list, but do
// NOT apply the global index-base override".
BaseOverride base_override_for_(const ArtifactSource& src) {
    BaseOverride b;
    if (src.localDir) { b.local = src.localDir; b.base = src.base; }
    else if (!src.forge()) b.base = src.base;
    return b;
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

        // #476, all optional: a pointer without them parses exactly as before.
        if (j.contains("requires") && j["requires"].is_object()) {
            m.requirements = j["requires"];
        }
        m.history_truncated = j.value("history_truncated", false);
        if (j.contains("history") && j["history"].is_array()) {
            for (const auto& e : j["history"]) {
                if (!e.is_object() || !e.contains("artifact")
                    || !e["artifact"].is_object()) continue;
                const auto& ea = e["artifact"];
                IndexSnapshot s;
                s.index_version   = e.value("index_version", std::string{});
                s.generated_at    = e.value("generated_at", std::string{});
                s.artifact_name   = ea.value("name", std::string{});
                s.artifact_sha256 = detail_::lower_hex_(ea.value("sha256", std::string{}));
                s.artifact_size   = ea.value("size", std::uint64_t{0});
                if (e.contains("requires") && e["requires"].is_object()) {
                    s.requirements = e["requires"];
                }
                // A history entry with no artifact identity is unusable; drop it
                // rather than carrying a row that cannot be selected.
                if (s.index_version.empty() || s.artifact_name.empty()
                    || s.artifact_sha256.empty()) continue;
                m.history.push_back(std::move(s));
            }
        }
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<IndexRequirement> requirement_for(const nlohmann::json& requirements,
                                                std::string_view consumer) {
    if (!requirements.is_object()) return std::nullopt;
    const auto it = requirements.find(std::string(consumer));
    if (it == requirements.end() || !it->is_object()) return std::nullopt;
    IndexRequirement req;
    req.min = it->value("min", std::string{});
    req.max = it->value("max", std::string{});
    if (req.empty()) return std::nullopt;
    return req;
}

bool satisfies_requirement(std::string_view selfVersion, const IndexRequirement& req) {
    if (!req.min.empty() && version_order::compare(selfVersion, req.min) < 0) return false;
    if (!req.max.empty() && version_order::compare(selfVersion, req.max) >= 0) return false;
    return true;
}

std::vector<IndexSnapshot> snapshots_of(const IndexManifest& manifest) {
    if (!manifest.history.empty()) return manifest.history;
    IndexSnapshot only;
    only.index_version   = manifest.index_version;
    only.generated_at    = manifest.generated_at;
    only.artifact_name   = manifest.artifact_name;
    only.artifact_sha256 = manifest.artifact_sha256;
    only.artifact_size   = manifest.artifact_size;
    only.requirements    = manifest.requirements;
    return {only};
}

namespace detail_ {

std::string describe_requirement_(const IndexSnapshot& s) {
    const auto req = requirement_for(s.requirements, "xlings");
    if (!req) return "no requirement";
    std::string out;
    if (!req->min.empty()) out += ">= " + req->min;
    if (!req->max.empty()) out += (out.empty() ? "" : " ") + std::string("< ") + req->max;
    return out;
}

}  // namespace detail_

std::expected<SnapshotChoice, std::string> choose_snapshot(
    const IndexManifest& manifest, std::string_view selfVersion, std::string_view pin) {
    const auto snapshots = snapshots_of(manifest);
    if (snapshots.empty()) return std::unexpected("pointer lists no index snapshot");

    const auto available = [&] {
        std::string list;
        for (const auto& s : snapshots) {
            if (!list.empty()) list += ", ";
            list += s.index_version;
        }
        if (manifest.history_truncated) list += ", … (history truncated)";
        return list;
    };

    // "newest": take the head of the list whatever it requires. This is the
    // escape hatch `self update` runs on, and it is what breaks the deadlock a
    // routed-back client would otherwise sit in forever -- an old snapshot's
    // own xlings recipe names an old `latest`, so a client routed there could
    // never see, let alone install, the version that would let it move on.
    if (pin == "newest") {
        SnapshotChoice choice{snapshots.front(), true, {}};
        return choice;
    }

    // Explicit pin: honour it exactly. It bypasses the contract check -- asking
    // for a specific snapshot is a deliberate act, often to reproduce a bug --
    // but never the sha256 check downstream.
    if (!pin.empty() && pin != "latest") {
        for (const auto& s : snapshots) {
            if (s.index_version == pin) {
                SnapshotChoice choice{s, &s == &snapshots.front(), {}};
                if (!choice.isNewest) {
                    choice.reason = std::format("pinned to {}", pin);
                }
                return choice;
            }
        }
        return std::unexpected(std::format(
            "E_INDEX_VERSION_NOT_FOUND: pinned to '{}', which the pointer does "
            "not offer; available: {}", pin, available()));
    }

    // Automatic routing: newest snapshot whose contract this client meets.
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const auto& s = snapshots[i];
        const auto req = requirement_for(s.requirements, "xlings");
        if (req && !satisfies_requirement(selfVersion, *req)) continue;
        SnapshotChoice choice{s, i == 0, {}};
        if (i != 0) {
            choice.reason = std::format(
                "{} requires xlings {}, this is {}",
                snapshots.front().index_version,
                detail_::describe_requirement_(snapshots.front()), selfVersion);
        }
        return choice;
    }

    return std::unexpected(std::format(
        "E_INDEX_NO_COMPATIBLE_SNAPSHOT: this xlings is {} and no published "
        "snapshot accepts it (newest {} requires xlings {}); available: {}",
        selfVersion, snapshots.front().index_version,
        detail_::describe_requirement_(snapshots.front()), available()));
}

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

const IndexManifest* select_manifest(const std::map<std::string, IndexManifest>& pointers,
                                     std::string_view key, bool soleEntryFallback) {
    if (auto it = pointers.find(std::string(key)); it != pointers.end()) return &it->second;
    if (soleEntryFallback && pointers.size() == 1) return &pointers.begin()->second;
    return nullptr;
}

std::vector<std::string> index_asset_urls(std::string_view filename,
                                          std::string_view mirror,
                                          std::string_view version,
                                          const ArtifactSource* custom) {
    // #377: a per-repo source replaces the official server set entirely.
    if (custom) {
        if (custom->forge()) {
            std::vector<std::string> tags;
            if (!version.empty()) tags.push_back("v" + std::string(version));
            tags.push_back("latest");   // GitHub rolling fallback; a 404 falls through
            std::vector<std::string> urls;
            for (auto& tag : tags)
                urls.push_back(std::format("{}/{}/releases/download/{}/{}",
                                           custom->server, custom->repoName, tag, filename));
            return urls;
        }
        if (!custom->localDir) return { custom->base + "/" + std::string(filename) };
        return {};  // local dir: obtain_file consumes a forced BaseOverride instead
    }

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
                                            std::string_view mirror,
                                            const ArtifactSource* custom) {
    auto rawFor = [&](const std::string& server, const std::string& repo) -> std::string {
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

    // #377: per-repo source — one authoritative location, no official mirrors.
    if (custom) {
        if (custom->forge()) {
            if (auto u = rawFor(custom->server, custom->repoName); !u.empty()) return {u};
            return {};
        }
        if (!custom->localDir) return { custom->base + "/" + std::string(filename) };
        return {};  // local dir: obtain_file consumes a forced BaseOverride instead
    }

    auto repo = detail_::index_repo_name_();
    std::vector<std::string> urls;
    auto add = [&](const std::string& u){
        if (!u.empty() && std::ranges::find(urls, u) == urls.end()) urls.push_back(u);
    };
    auto selected = Config::resource_server(mirror);
    if (!selected.empty()) add(rawFor(selected, repo));
    for (auto& s : Config::resource_servers(mirror))   add(rawFor(s, repo));
    for (auto& s : Config::resource_servers("GLOBAL")) add(rawFor(s, repo));
    return urls;
}

namespace detail_ {
// base -> consumer -> newest version, filled by load_index_pointers().
inline std::map<std::string, std::map<std::string, std::string>>& client_latest_cache_() {
    static std::map<std::string, std::map<std::string, std::string>> value;
    return value;
}
}  // namespace detail_

// Newest published version of `consumer` as the pointer advertises it, or ""
// when the pointer does not say. Requires load_index_pointers() to have run
// for the same source (fetch_index_artifact always does).
std::string client_latest_for(std::string_view consumer, const ArtifactSource* custom) {
    const std::string cacheKey = custom ? custom->base : std::string{};
    const auto& all = detail_::client_latest_cache_();
    const auto base = all.find(cacheKey);
    if (base == all.end()) return {};
    const auto hit = base->second.find(std::string(consumer));
    return hit == base->second.end() ? std::string{} : hit->second;
}

// Cached fetch of the COMBINED pointer file (xim-index-pointers.json): ONE raw
// fetch per process covering ALL indexes (main + subs). One fetch (vs one per
// index) avoids gitcode raw rate-limiting, and is the single file a self-hosted
// index server must serve. Format:
//   {"format_version":1,"indexes":{"xim":{<manifest>},"awesome":{<manifest>},...}}
const std::map<std::string, IndexManifest>& load_index_pointers(std::string_view mirror,
                                                                const ArtifactSource* custom) {
    // #377: keyed per base (official = ""), same fetch-once-per-process
    // semantics the old once_flag gave the official pointer.
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
        // #476: newest CLIENT version, carried at pointer top level so it is
        // readable no matter which index snapshot this client routes to. That
        // is the half of the deadlock fix a routed-back client depends on: an
        // old snapshot's own xlings recipe names an old `latest`, so without
        // this the client could never learn a newer one exists.
        if (j.contains("client_latest") && j["client_latest"].is_object()) {
            auto& slot = detail_::client_latest_cache_()[cacheKey];
            for (auto it = j["client_latest"].begin(); it != j["client_latest"].end(); ++it)
                if (it.value().is_string()) slot[it.key()] = it.value().get<std::string>();
        }
    } catch (...) { log::warn("[index] pointer parse failed"); }
    return cache;
}

bool fetch_index_artifact(const std::filesystem::path& destIndexDir,
                          std::string& err,
                          std::string_view subName,
                          const ArtifactSource* custom,
                          std::string_view pin) {
    namespace fs = std::filesystem;
    auto mirrorKey = Config::mirror();
    std::string key = custom ? custom->key
                             : (subName.empty() ? std::string("xim") : std::string(subName));

    std::string label = subName.empty()
        ? std::string("package index")
        : std::format("sub-index '{}'", subName);
    log::info("[index] fetching {} (mirror={})...",
              label, mirrorKey.empty() ? "GLOBAL" : mirrorKey);

    auto& pointers = load_index_pointers(mirrorKey, custom);
    const IndexManifest* pm = select_manifest(pointers, key, custom != nullptr);
    if (!pm) {
        err = std::format("no pointer entry for '{}' ({} entries)", key, pointers.size());
        return false;
    }
    const IndexManifest& manifest = *pm;
    if (manifest.format_version != 1) {
        err = std::format("unsupported index manifest format_version {}", manifest.format_version);
        return false;
    }

    // #476: route to the newest snapshot this client's version accepts, or to
    // an explicit pin. On failure the local index tree is left alone -- keeping
    // the last snapshot that worked beats replacing it with a wrong one.
    // XLINGS_INDEX_PIN overrides the per-repo pin: an escape hatch for
    // debugging, and the mechanism `self update` uses to reach a newer client
    // through an index this version would not otherwise route to.
    std::string effectivePin{pin};
    if (const auto* env = std::getenv("XLINGS_INDEX_PIN"); env && *env) {
        effectivePin = env;
    }
    const auto choice = choose_snapshot(manifest, Info::VERSION, effectivePin);
    if (!choice) { err = choice.error(); return false; }
    const IndexSnapshot& snapshot = choice->snapshot;

    // Say it when we step back. A client that silently sits on an older index
    // looks to its user exactly like one on the newest, right up until a
    // package it expects is missing.
    if (!choice->isNewest) {
        log::warn("[index] {}: using {} instead of {}", key,
                  snapshot.index_version, manifest.index_version);
        log::warn("[index]   {}", choice->reason);
        if (auto newer = client_latest_for("xlings", custom);
            !newer.empty() && version_order::compare(newer, Info::VERSION) > 0) {
            log::warn("[index]   upgrade to {}: xlings self update", newer);
        }
    }

    std::error_code ec;
    auto tmpRoot = fs::path(destIndexDir.string() + ".artifact." +
                            std::to_string(platform::get_pid()));
    fs::remove_all(tmpRoot, ec);
    fs::create_directories(tmpRoot, ec);
    if (ec) { err = std::format("create temp dir failed: {}", ec.message()); return false; }
    struct Cleanup { fs::path p; ~Cleanup(){ std::error_code e; fs::remove_all(p,e);} } cleanup{tmpRoot};

    // Artifact (sha256-pinned by the manifest); release asset, versioned name.
    auto artifactFile = tmpRoot / snapshot.artifact_name;
    detail_::BaseOverride forcedStorage;
    const detail_::BaseOverride* forced = nullptr;
    if (custom) { forcedStorage = detail_::base_override_for_(*custom); forced = &forcedStorage; }
    if (auto e = detail_::obtain_file(snapshot.artifact_name,
                    index_asset_urls(snapshot.artifact_name, mirrorKey,
                                     snapshot.index_version, custom),
                    artifactFile, snapshot.artifact_sha256, forced); !e.empty()) {
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
    // This is the version of the snapshot that actually lands, NOT the head of
    // the pointer -- routing (#476) makes those differ, and both readers care:
    // a human debugging "where did my package go" reads this file, and
    // get_repo_head_hash() keys the parsed-index cache on it. Writing the head
    // here would name a tree that is not on disk, and would change on every
    // publish for a client that keeps landing on the same old snapshot --
    // rebuilding that client's cache forever.
    {
        std::ofstream v(stage / ".xlings-index-version", std::ios::binary);
        v << snapshot.index_version;
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
                      snapshot.artifact_name,
                      snapshot.index_version.empty() ? "?" : snapshot.index_version);
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

    // The SNAPSHOT that landed, not the manifest's head. Reporting the head
    // here would print `updated from xim-index-new0003` immediately after
    // "using old0001 instead of new0003" -- and this line is what the release
    // verification runbook greps to decide which artifact a run actually used
    // (reference_index_publish_lag).
    log::info("[index] updated from artifact {} ({})",
              snapshot.artifact_name,
              snapshot.index_version.empty() ? "?" : snapshot.index_version);
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
