module;
#include <ctime>

module xlings.core.xim.repo;

import std;
import xlings.libs.json;
import xlings.core.log;
import xlings.core.compact;
import xlings.platform;
import xlings.core.config;
import xlings.core.mirror;
import xlings.core.xim.indexfetch;
import xlings.libs.tinyhttps;

namespace xlings::xim {

namespace detail_ {

bool ensure_local_repo_link_(const std::filesystem::path& localDir,
                             const std::filesystem::path& sourceDir) {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(sourceDir, ec) || !fs::exists(sourceDir / "pkgs", ec)) {
        // #374: caller (syncRepos) now treats this as a best-effort skip and
        // emits the user-facing warning naming the repo; keep this at debug
        // so a tolerated missing-pkgs source is not reported as a hard error.
        log::debug("local index repo missing pkgs/: {}", sourceDir.string());
        return false;
    }

    if (fs::exists(localDir, ec) || fs::is_symlink(localDir, ec)) {
        auto canonicalLocal = fs::weakly_canonical(localDir, ec);
        ec.clear();
        auto canonicalSource = fs::weakly_canonical(sourceDir, ec);
        if (!ec && canonicalLocal == canonicalSource) {
            return true;
        }
        ec.clear();
        fs::remove_all(localDir, ec);
        if (ec) {
            log::error("failed to replace local index repo mapping {}: {}",
                       localDir.string(), ec.message());
            return false;
        }
    }

    fs::create_directories(localDir.parent_path(), ec);
    if (ec) {
        log::error("failed to create repo parent directory {}: {}",
                   localDir.parent_path().string(), ec.message());
        return false;
    }

#if defined(_WIN32)
    if (!platform::create_directory_link(localDir, sourceDir)) {
        log::error("failed to create directory link: {} -> {}",
                   localDir.string(), sourceDir.string());
        return false;
    }
#else
    fs::create_directory_symlink(sourceDir, localDir, ec);
    if (ec) {
        log::error("failed to create symlink: {} -> {} ({})",
                   localDir.string(), sourceDir.string(), ec.message());
        return false;
    }
#endif

    log::debug("linked local index repo: {} -> {}", localDir.string(), sourceDir.string());
    return true;
}

std::vector<IndexRepo> discover_sub_repos_(const std::filesystem::path& repoDir,
                                            const std::string& mirror) {
    namespace fs = std::filesystem;
    auto luaFile = repoDir / "xim-indexrepos.lua";
    if (!fs::exists(luaFile)) return {};

    std::string content;
    try {
        content = platform::read_file_to_string(luaFile.string());
    } catch (...) {
        return {};
    }

    std::vector<IndexRepo> repos;
    // Simple line-based parser to avoid std::regex in modules
    // State machine: look for ["name"] = { blocks, then ["KEY"] = "url" inside
    std::istringstream iss(content);
    std::string line;
    std::string currentName;
    std::string globalUrl, mirrorUrl;
    bool inBlock = false;

    while (std::getline(iss, line)) {
        // Trim leading whitespace
        auto pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos) continue;
        auto trimmed = line.substr(pos);

        if (!inBlock) {
            // Look for ["name"] = {
            if (trimmed.starts_with("[\"")) {
                auto endQuote = trimmed.find("\"]", 2);
                if (endQuote != std::string::npos && trimmed.find("= {") != std::string::npos) {
                    currentName = trimmed.substr(2, endQuote - 2);
                    inBlock = true;
                    globalUrl.clear();
                    mirrorUrl.clear();
                }
            }
        } else {
            // Inside a block - look for closing } or ["KEY"] = "url"
            if (trimmed.starts_with("}")) {
                // End of block
                // Mirror selection for sub-index repos happens here:
                // prefer URL of current mirror key (e.g. CN), fallback to GLOBAL.
                auto url = mirrorUrl.empty() ? globalUrl : mirrorUrl;
                if (!url.empty()) {
                    repos.push_back({currentName, url});
                }
                inBlock = false;
            } else if (trimmed.starts_with("[\"")) {
                auto endQuote = trimmed.find("\"]", 2);
                if (endQuote != std::string::npos) {
                    auto key = trimmed.substr(2, endQuote - 2);
                    // Find the URL value: = "url"
                    auto eqPos = trimmed.find("= \"", endQuote);
                    if (eqPos != std::string::npos) {
                        auto urlStart = eqPos + 3;
                        auto urlEnd = trimmed.find('"', urlStart);
                        if (urlEnd != std::string::npos) {
                            auto val = trimmed.substr(urlStart, urlEnd - urlStart);
                            if (key == "GLOBAL") globalUrl = val;
                            if (key == mirror) mirrorUrl = val;
                        }
                    }
                }
            }
        }
    }

    return repos;
}

std::string sync_repo_url_(const std::string& url, const std::string& mirror [[maybe_unused]]) {
    // Repo URLs are already resolved by mirror in config/xim-indexrepos.lua.
    return url;
}

// Strip a trailing `.git` and trailing slashes so two spellings of one repo
// compare equal. Deliberately does NOT touch scheme or host: a mirror is a
// different source, and treating gitee's copy as "the same repo" as github's
// would be the very conflation this whole change removes.
std::string normalize_repo_url_(std::string_view url) {
    std::string s{url};
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.ends_with(".git")) s.resize(s.size() - 4);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

}

std::vector<IndexRepo> discover_sub_repos(const std::filesystem::path& repoDir,
                                           const std::string& mirror) {
    return detail_::discover_sub_repos_(repoDir, mirror);
}

std::string sync_repo_url(const std::string& url, const std::string& mirror) {
    return detail_::sync_repo_url_(url, mirror);
}

std::vector<IndexRepo> merge_sub_repos(const std::vector<IndexRepo>& luaDefaults,
                                       const std::vector<IndexRepo>& jsonRepos) {
    std::unordered_map<std::string, IndexRepo> seen;
    std::vector<IndexRepo> out;
    out.reserve(luaDefaults.size() + jsonRepos.size());
    for (auto& r : luaDefaults) {
        if (seen.emplace(r.name, r).second) out.push_back(r);
    }
    for (auto& r : jsonRepos) {
        if (seen.emplace(r.name, r).second) out.push_back(r);
    }
    return out;
}

std::string index_pointer_key(const IndexRepo& repo) {
    if (auto src = artifact_source_for(repo)) return src->key;
    return repo.name;
}

bool url_matches_declared_source(std::string_view repoUrl,
                                 std::string_view declaredUrl) {
    if (repoUrl.empty() || declaredUrl.empty()) return false;
    return detail_::normalize_repo_url_(repoUrl)
        == detail_::normalize_repo_url_(declaredUrl);
}

bool is_local_repo_url(std::string_view u) {
    return u.starts_with("file://")
        || u.starts_with("/")
        || u.starts_with("./")
        || u.starts_with("../");
}

bool sync_repo(const std::filesystem::path& localDir, const std::string& url, bool force) {
    namespace fs = std::filesystem;

    if (!compact::git::ensure_available()) {
        log::error("git is required to update package index: {}", url);
        return false;
    }

    if (!fs::exists(localDir / ".git")) {
        // ── Non-destructive guard (regression fix, see
        // .agents/docs/2026-06-30-index-artifact-git-regression-analysis.md) ──
        // A directory with pkgs/ but no .git is an artifact-managed index (or an
        // extracted snapshot). The git-clone path below does fs::remove_all(localDir)
        // before swapping the clone in — that wipes .xlings-index-version and strands
        // the index on git PERMANENTLY. Never let a fallback clone destroy a usable
        // artifact index: keep it (the caller's next artifact attempt migrates it
        // atomically). An explicit XLINGS_INDEX_SOURCE=git opts out of this guard.
        {
            std::error_code gec;
            bool indexSourceGit = false;
            if (auto* e = std::getenv("XLINGS_INDEX_SOURCE"); e && *e)
                indexSourceGit = std::string_view(e) == "git";
            if (!indexSourceGit && fs::exists(localDir / "pkgs", gec)) {
                log::warn("[index] keeping existing artifact-managed index at {} "
                          "(refusing destructive git clone); will retry artifact next update",
                          localDir.string());
                return true;
            }
        }

        // Build mirror fallback list for the index repo URL. Mirror::expand
        // returns just [url] when mirror_fallback=off or url is non-github.
        auto urls = mirror::expand(url, {.type = mirror::ResourceType::Git});
        if (urls.empty()) urls.push_back(url);

        std::string lastErr;
        auto tmpDir = fs::path(localDir.string() + ".tmp." + std::to_string(platform::get_pid()));
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
        for (std::size_t i = 0; i < urls.size(); ++i) {
            log::debug("cloning index repo attempt {}/{}: {}",
                       i + 1, urls.size(), urls[i]);
            // git has no connect timeout, so a firewalled host (e.g. github
            // from a network that blocks it — common in CN / on Termux) stalls
            // the clone on the OS TCP timeout (~127s) per URL. Probe reachability
            // first with a short TCP connect; skip unreachable URLs so a
            // github-only sub-index can't freeze `xlings update` for minutes.
            constexpr int kReachMs = 3000;
            if (!is_local_repo_url(urls[i])
                && !std::isfinite(tinyhttps::probe_latency(urls[i], kReachMs))) {
                log::debug("index repo host unreachable within {}ms, skipping: {}",
                           kReachMs, urls[i]);
                lastErr = "host unreachable: " + urls[i];
                continue;
            }
            auto clone = compact::git::clone_shallow(urls[i], tmpDir, false);
            if (clone.rc == 0) {
                if (i > 0)
                    log::info("[mirror] index repo fallback succeeded via {}", urls[i]);
                fs::remove_all(localDir, ec);
                if (ec) {
                    log::error("failed to replace index repo {}: {}",
                               localDir.string(), ec.message());
                    fs::remove_all(tmpDir, ec);
                    return false;
                }
                fs::rename(tmpDir, localDir, ec);
                if (ec) {
                    log::error("failed to move cloned index repo into place {}: {}",
                               localDir.string(), ec.message());
                    fs::remove_all(tmpDir, ec);
                    return false;
                }
                return true;
            }
            lastErr = clone.output;
            fs::remove_all(tmpDir, ec);
        }
        log::error("all index repo clone URLs failed: {}", lastErr);
        return false;
    }

    // Check throttle: skip if pulled within 7 days (unless forced)
    if (!force) {
        auto stampFile = localDir / ".xlings-sync-stamp";
        if (fs::exists(stampFile)) {
            auto lastWrite = fs::last_write_time(stampFile);
            auto now = fs::file_time_type::clock::now();
            auto age = std::chrono::duration_cast<std::chrono::hours>(now - lastWrite);
            if (age.count() < 7 * 24) {
                log::debug("skipping sync for {} (last sync {}h ago)",
                           localDir.string(), age.count());
                return true;
            }
        }
    }

    log::debug("updating index repo: {}", localDir.filename().string());
    // Same fast-fail as the clone path: skip the refresh if the origin is
    // unreachable (firewalled github) rather than stalling git pull ~127s. The
    // existing local copy stays valid, so this is non-fatal.
    if (!is_local_repo_url(url)
        && !std::isfinite(tinyhttps::probe_latency(url, 3000))) {
        log::debug("index repo origin unreachable, keeping existing copy: {}", url);
        return true;
    }
    auto setOrigin = compact::git::set_origin(localDir, url);
    if (setOrigin.rc != 0) {
        log::warn("failed to set index repo origin: {}", setOrigin.output);
    }

    auto pull = compact::git::pull_ff_only(localDir);
    if (pull.rc != 0) {
        log::warn("git pull failed, trying reset: {}", pull.output);
        auto reset = compact::git::fetch_reset_origin_head(localDir);
        if (reset.rc != 0) {
            log::error("git reset failed: {}", reset.output);
            return false;
        }
    }

    // Update stamp file
    auto stampFile = localDir / ".xlings-sync-stamp";
    std::ofstream(stampFile).put('.');
    return true;
}

std::string url_to_dirname(const std::string& url) {
    auto name = std::filesystem::path(url).stem().string();
    if (name.empty()) {
        // fallback: strip trailing .git and take last path component
        auto tmp = url;
        if (tmp.ends_with(".git")) tmp.resize(tmp.size() - 4);
        auto pos = tmp.find_last_of('/');
        name = pos != std::string::npos ? tmp.substr(pos + 1) : tmp;
    }
    return name;
}

std::vector<IndexRepo> load_sub_repos_json(const std::filesystem::path& jsonFile) {
    namespace fs = std::filesystem;
    if (!fs::exists(jsonFile)) return {};

    try {
        auto content = platform::read_file_to_string(jsonFile.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return {};

        std::vector<IndexRepo> repos;
        for (auto it = json.begin(); it != json.end(); ++it) {
            IndexRepo repo;
            repo.name = it.key();
            if (it.value().is_string()) {
                repo.url = it.value().get<std::string>();
            } else if (it.value().is_object()) {
                // #377: object form carries the artifact declaration.
                auto& v = it.value();
                if (v.contains("url") && v["url"].is_string())
                    repo.url = v["url"].get<std::string>();
                if (v.contains("artifact") && v["artifact"].is_string())
                    repo.artifactBase = v["artifact"].get<std::string>();
                if (v.contains("source") && v["source"].is_string())
                    repo.source = v["source"].get<std::string>();
            }
            if (!repo.url.empty()) repos.push_back(std::move(repo));
        }
        return repos;
    } catch (...) {
        return {};
    }
}

void save_sub_repos_json(const std::filesystem::path& jsonFile,
                         const std::vector<IndexRepo>& repos) {
    nlohmann::json json = nlohmann::json::object();
    for (auto& repo : repos) {
        if (repo.artifactBase.empty() && repo.source.empty()) {
            json[repo.name] = repo.url;       // legacy string form
        } else {
            // #377: object form; old xlings skips non-string values gracefully.
            nlohmann::json v;
            v["url"] = repo.url;
            if (!repo.artifactBase.empty()) v["artifact"] = repo.artifactBase;
            if (!repo.source.empty())       v["source"]   = repo.source;
            json[repo.name] = v;
        }
    }
    try {
        std::filesystem::create_directories(jsonFile.parent_path());
        platform::write_string_to_file(jsonFile.string(), json.dump(2));
    } catch (...) {
        log::warn("failed to save sub-repos JSON: {}", jsonFile.string());
    }
}

std::filesystem::path sub_repos_dir(bool projectScope) {
    auto base = projectScope ? Config::project_data_dir() : Config::global_data_dir();
    if (base.empty()) return {};
    return base / "xim-index-repos";
}

std::filesystem::path sub_repos_json_path(bool projectScope) {
    auto dir = sub_repos_dir(projectScope);
    if (dir.empty()) return {};
    return dir / "xim-indexrepos.json";
}

std::filesystem::path sub_repo_dir_for(const IndexRepo& repo, bool projectScope) {
    return sub_repos_dir(projectScope) / url_to_dirname(repo.url);
}

std::vector<IndexRepo> discovered_global_sub_repos() {
    return load_sub_repos_json(sub_repos_json_path(false));
}

std::vector<IndexRepo> discovered_project_sub_repos() {
    auto path = sub_repos_json_path(true);
    if (path.empty()) return {};
    return load_sub_repos_json(path);
}

bool sub_indexes_initialized() {
    auto p = sub_repos_json_path(false);
    return !p.empty() && std::filesystem::exists(p);
}

std::filesystem::path main_repo_dir() {
    return Config::global_data_dir() / Config::DEFAULT_INDEX_REPO_DIR;
}

std::string declared_sub_index_url(std::string_view name) {
    // Reads the GLOBAL default index's declaration, in project scope too. A
    // project sub-index declared only by a PROJECT index repo therefore finds
    // no declaration and stays on git -- the safe direction, and what it did
    // before this change. Widening it means scanning every project index
    // repo's lua, which is a separate decision; the failure mode of NOT doing
    // it is "no artifact acceleration", never wrong content.
    auto mirror = Config::mirror();
    for (auto& r : detail_::discover_sub_repos_(main_repo_dir(),
                                                mirror.empty() ? "GLOBAL" : mirror)) {
        if (r.name == name) return r.url;
    }
    return {};
}

bool artifact_is_declared_for(const IndexRepo& repo, bool projectScope) {
    // #377: a repo that names its own artifact base has declared its source.
    if (!repo.artifactBase.empty()) return true;
    // A local tree is served by the filesystem, not by a pointer.
    if (Config::is_local_repo_source(repo, projectScope)) return false;

    auto declared = Config::declared_index_repo_url(repo.name);
    if (declared.empty()) declared = declared_sub_index_url(repo.name);
    return url_matches_declared_source(repo.url, declared);
}

bool sync_one_repo(const IndexRepo& repo,
                   const std::filesystem::path& repoDir,
                   const std::string& globalIndexSource,
                   const std::string& mirror,
                   bool projectScope,
                   bool force,
                   bool linkLocalSource) {
    const std::string mode = repo.source.empty() ? globalIndexSource : repo.source;
    auto custom = artifact_source_for(repo);

    if (mode != "git" && artifact_is_declared_for(repo, projectScope)) {
        // Does a published pointer describe an index under this name? The
        // pointer is CONFIGURED (xim.index-base), so this is the home
        // answering, not a pattern compiled into xlings. A name no pointer
        // carries -- `dsh`, a private index -- has no artifact and uses git.
        const auto key = index_pointer_key(repo);
        auto& pointers = load_index_pointers(mirror, custom ? &*custom : nullptr);
        if (select_manifest(pointers, key, custom.has_value())) {
            std::string ferr;
            if (fetch_index_artifact(repoDir, ferr, key,
                                     custom ? &*custom : nullptr, repo.version)) {
                return true;
            }
            if (mode == "artifact") {
                log::error("[index] '{}' artifact fetch failed and git fallback is "
                           "disabled (source=artifact): {}", repo.name, ferr);
                return false;
            }
            log::warn("[index] '{}' artifact fetch failed ({}); falling back to git",
                      repo.name, ferr);
        }
    }

    if (mode == "artifact") {
        log::error("[index] '{}' has no artifact published under its name and "
                   "source=artifact disables the git fallback", repo.name);
        return false;
    }

    if (linkLocalSource && Config::is_local_repo_source(repo, projectScope)) {
        auto sourceDir = Config::resolve_repo_source(repo, projectScope);
        if (!detail_::ensure_local_repo_link_(repoDir, sourceDir)) {
            log::warn("index repo '{}' skipped: local source has no pkgs/ ({})",
                      repo.name, sourceDir.string());
            return false;
        }
        return true;
    }

    auto url = detail_::sync_repo_url_(repo.url, mirror);
    if (!sync_repo(repoDir, url, force)) {
        log::warn("index repo '{}' skipped: sync failed ({})", repo.name, url);
        return false;
    }
    return true;
}

bool sync_all_repos(bool force) {
    namespace fs = std::filesystem;
    auto mirror = Config::mirror();

    auto mainDir = main_repo_dir();

    // Heal leftovers from any crashed/killed run BEFORE deciding git-vs-artifact:
    // restores an index dir orphaned by an interrupted swap and reclaims leaked
    // staging dirs, so the gating below never sees a spurious "no index" gap.
    if (auto dataDir = Config::global_data_dir(); !dataDir.empty()) {
        reconcile_index_temps(dataDir);
        reconcile_index_temps(sub_repos_dir());
    }

    // ── Index-as-Resource (Y-asset), peer edition ───────────────────
    // Every index_repos entry is fetched as a versioned artifact when one is
    // published under ITS OWN name, over the resource-server HTTP path (adaptive
    // mirror reorder + stall watchdog). git clone is the fallback.
    // XLINGS_INDEX_SOURCE=git|artifact|auto (auto), overridable per repo via
    // `source`.
    //
    // There is no "main index" branch any more. There used to be: index_repos[0]
    // was fetched with a hardcoded pointer key into a directory derived from
    // that entry's name, so the destination and the content were independent
    // parameters and a sub-index listed first received the official index.
    std::string indexSource = "auto";
    if (auto* e = std::getenv("XLINGS_INDEX_SOURCE"); e && *e) indexSource = e;

    // #374: best-effort. A single degenerate/unreachable index repo (no
    // pkgs/, empty default-namespace redirect target, dead URL) must NOT
    // abort the whole sync and collapse the catalog — that is the exact
    // ">=2 index_repos -> silent exit 1" trigger. Skip the bad repo with a
    // warning and keep syncing the rest. Returns false only when NOTHING
    // in a non-empty list could be synced; the catalog rebuild's load-gate
    // is the real "nothing usable" safety net.
    auto syncRepos = [&](const std::vector<IndexRepo>& repos, bool projectScope) {
        auto rootDir = projectScope ? Config::project_data_dir() : Config::global_data_dir();
        if (rootDir.empty()) return true;
        fs::create_directories(rootDir);
        // The DEFAULT entry syncs first. Not because position means anything
        // any more -- it deliberately does not -- but because the default index
        // is what DECLARES the others (xim-indexrepos.lua), and
        // artifact_is_declared_for reads that file off disk. On a fresh home,
        // judging another entry before the default has landed would find no
        // declaration and quietly drop it to git for one run.
        //
        // Order of the rest is the config's, untouched.
        std::vector<const IndexRepo*> ordered;
        ordered.reserve(repos.size());
        for (auto& repo : repos)
            if (repo.name == Config::DEFAULT_INDEX_REPO_NAME) ordered.push_back(&repo);
        for (auto& repo : repos)
            if (repo.name != Config::DEFAULT_INDEX_REPO_NAME) ordered.push_back(&repo);

        int total = 0, ok = 0;
        for (auto* repo : ordered) {
            ++total;
            if (sync_one_repo(*repo, Config::repo_dir_for(*repo, projectScope),
                              indexSource, mirror, projectScope, force,
                              /*linkLocalSource=*/true)) ++ok;
        }
        return total == 0 || ok > 0;
    };

    if (!syncRepos(Config::global_index_repos(), false)) return false;

    // Discover and sync sub-index repos from the main repo's xim-indexrepos.lua
    auto luaSubRepos = detail_::discover_sub_repos_(mainDir, mirror.empty() ? "GLOBAL" : mirror);
    auto jsonSubRepos = load_sub_repos_json(sub_repos_json_path());

    // Merge: lua defaults are authoritative for their own names; json adds only
    // user-added sub-indexes (names absent from the lua defaults). See
    // merge_sub_repos for why json must NOT override lua defaults (an
    // org-drifted default would otherwise stay pinned to a stale json URL and
    // never migrate to the artifact path).
    auto allSubRepos = merge_sub_repos(luaSubRepos, jsonSubRepos);
    std::vector<IndexRepo> syncedSubRepos;

    // Sub-indexes are peers of the top-level entries and go through the same
    // function. `artifact_is_declared_for` makes the same judgement the inline
    // `subDefaultOfficial` check used to make here — the name must be declared
    // AND the URL must match what declared it — except it is now the general
    // rule rather than a special case that only sub-indexes got.
    auto subReposRoot = sub_repos_dir();
    fs::create_directories(subReposRoot);

    for (auto& repo : allSubRepos) {
        if (sync_one_repo(repo, sub_repo_dir_for(repo), indexSource, mirror, false, force,
                          /*linkLocalSource=*/false))
            syncedSubRepos.push_back(repo);
        else
            log::warn("failed to sync sub-index repo: {} ({})", repo.name, repo.url);
    }

    save_sub_repos_json(sub_repos_json_path(), syncedSubRepos);

    if (Config::has_project_config() && !Config::project_index_repos().empty()) {
        if (!syncRepos(Config::project_index_repos(), true)) return false;

        // Discover and sync sub-index repos from project index repos
        auto mirrorKey = mirror.empty() ? "GLOBAL" : mirror;
        auto& projRepos = Config::project_index_repos();
        std::unordered_map<std::string, IndexRepo> projMerged;
        for (auto& repo : projRepos) {
            auto repoDir = Config::repo_dir_for(repo, true);
            for (auto& sub : detail_::discover_sub_repos_(repoDir, mirrorKey)) {
                projMerged[sub.name] = sub;
            }
        }
        auto projJsonSubs = load_sub_repos_json(sub_repos_json_path(true));
        for (auto& repo : projJsonSubs) projMerged[repo.name] = repo;

        std::vector<IndexRepo> projSyncedSubs;
        auto projSubRoot = sub_repos_dir(true);
        fs::create_directories(projSubRoot);
        for (auto& [name, repo] : projMerged) {
            if (sync_one_repo(repo, sub_repo_dir_for(repo, true),
                              indexSource, mirror, true, force,
                              /*linkLocalSource=*/false)) {
                projSyncedSubs.push_back(repo);
            } else {
                log::warn("failed to sync project sub-index repo: {} ({})", repo.name, repo.url);
            }
        }
        save_sub_repos_json(sub_repos_json_path(true), projSyncedSubs);
    }
    return true;
}

std::string get_repo_head_hash(const std::filesystem::path& repoDir) {
    namespace fs = std::filesystem;
    auto headFile = repoDir / ".git" / "HEAD";
    if (!fs::exists(headFile)) {
        // Artifact-managed index (no .git): key the parsed-index cache
        // (.xlings-index-cache.json) on the .xlings-index-version marker.
        // Artifacts are version-unique, so the marker is a stable cache key
        // that changes only on re-install. Without this the hash is empty and
        // load_or_rebuild disables caching -> a full index rebuild (the noisy
        // "[1/7] awesome::..." scan) on EVERY xlings install/search.
        auto verFile = repoDir / ".xlings-index-version";
        if (fs::exists(verFile)) {
            try {
                auto v = platform::read_file_to_string(verFile.string());
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
                    v.pop_back();
                if (!v.empty()) return "artifact:" + v;
            } catch (...) {}
        }
        return {};
    }

    try {
        auto content = platform::read_file_to_string(headFile.string());
        // Trim trailing whitespace/newlines
        while (!content.empty() && (content.back() == '\n' || content.back() == '\r' || content.back() == ' '))
            content.pop_back();

        // Direct hash (detached HEAD)
        if (!content.starts_with("ref: ")) return content;

        // Symbolic ref: read the referenced file
        auto ref = content.substr(5);
        auto refFile = repoDir / ".git" / ref;
        if (fs::exists(refFile)) {
            auto hash = platform::read_file_to_string(refFile.string());
            while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r' || hash.back() == ' '))
                hash.pop_back();
            if (!hash.empty()) return hash;
        }

        // Packed refs, when the loose file is absent.
        //
        // NOT "the common shape for a freshly synced index" -- that claim was
        // inherited from the reader this replaces and it does not survive
        // measurement: all four index repos on the machine this was written on
        // have BOTH a `packed-refs` and a loose `refs/heads/<branch>`, and a
        // plain `git clone` writes the loose ref for the branch it checks out.
        //
        // What actually produces a packed-only ref is `git gc` / `git
        // pack-refs`, which runs on its own schedule inside a long-lived
        // index checkout. So this is a real fallback on a real timeline, just
        // not the default one -- and worth saying precisely, because a
        // fallback believed to be the common path is one nobody tests.
        auto packedFile = repoDir / ".git" / "packed-refs";
        if (!fs::exists(packedFile)) return {};
        auto packed = platform::read_file_to_string(packedFile.string());
        std::size_t pos = 0;
        while (pos < packed.size()) {
            auto eol = packed.find('\n', pos);
            auto line = packed.substr(pos, eol == std::string::npos
                                            ? std::string::npos : eol - pos);
            pos = (eol == std::string::npos) ? packed.size() : eol + 1;
            // Strip CR here as well as at every other read in this function.
            // Doing it in one place and not the other is how a CRLF file
            // turns into "no revision" with nothing said -- the caller then
            // prints the message it was trying to improve on.
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == '^') continue;
            auto sp = line.find(' ');
            if (sp == std::string::npos) continue;
            if (line.substr(sp + 1) == ref) return line.substr(0, sp);
        }
        return {};
    } catch (...) {
        return {};
    }
}

std::string get_repo_revision_label(const std::filesystem::path& repoDir) {
    auto hash = get_repo_head_hash(repoDir);
    if (hash.starts_with("artifact:")) return hash;
    // Only shorten what is actually a sha. A short string that is not one is
    // returned whole rather than truncated into something that looks like a
    // sha and is not.
    const bool hex = hash.size() >= 7
        && std::ranges::all_of(hash, [](unsigned char c) {
               return std::isxdigit(c) != 0;
           });
    return hex ? hash.substr(0, 7) : hash;
}

long long get_repo_sync_age_seconds(const std::filesystem::path& repoDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // FETCH_HEAD is written by every fetch, so it dates the SYNC rather than
    // the commit -- an index cloned once and never refreshed has an old
    // FETCH_HEAD and a recent HEAD mtime, and the question here is staleness.
    // `.xlings-index-version` plays the same role for the artifact shape.
    const fs::path candidates[] = {
        repoDir / ".git" / "FETCH_HEAD",
        repoDir / ".xlings-index-version",
        repoDir / ".git" / "HEAD",
    };
    std::optional<fs::file_time_type> newest;
    for (const auto& c : candidates) {
        auto t = fs::last_write_time(c, ec);
        if (ec) { ec.clear(); continue; }
        if (!newest || t > *newest) newest = t;
    }
    if (!newest) return -1;
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(
        fs::file_time_type::clock::now() - *newest).count();
    return age < 0 ? 0 : age;
}

}
