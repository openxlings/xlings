module;
#include <ctime>

export module xlings.core.xim.repo;

import std;
import xlings.libs.json;
import xlings.core.log;
import xlings.core.compact;
import xlings.platform;
import xlings.core.config;
import xlings.core.mirror;
import xlings.core.xim.indexfetch;
import xlings.libs.tinyhttps;

export namespace xlings::xim {

namespace detail_ {

bool ensure_local_repo_link_(const std::filesystem::path& localDir,
                             const std::filesystem::path& sourceDir) {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!fs::exists(sourceDir, ec) || !fs::exists(sourceDir / "pkgs", ec)) {
        log::error("local index repo missing pkgs/: {}", sourceDir.string());
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

// Parse xim-indexrepos.lua to discover sub-index repositories.
// Format: xim_indexrepos["name"] = { ["GLOBAL"] = "url", ["CN"] = "url" }
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

}  // namespace detail_

// Exported wrapper for discover_sub_repos (used by catalog)
std::vector<IndexRepo> discover_sub_repos(const std::filesystem::path& repoDir,
                                           const std::string& mirror) {
    return detail_::discover_sub_repos_(repoDir, mirror);
}

std::string sync_repo_url(const std::string& url, const std::string& mirror) {
    return detail_::sync_repo_url_(url, mirror);
}

// Merge the main index's lua-declared default sub-indexes with the
// persisted / locally-added json list.
//
// Lua defaults are AUTHORITATIVE for their own names: if the official index
// moves a default to a new org/URL, the lua URL wins over a stale json entry
// (which save_sub_repos_json wrote from a previous sync), and the json is
// healed on the next save. The json contributes ONLY names that are NOT lua
// defaults — i.e. user-added sub-indexes (a private fork, or a once-default
// repo that has since been dropped from the lua, like `fromsource`).
//
// This is what lets an org-drifted default (json pinned to the old org) keep
// classifying as default-official so it can migrate to the artifact path; a
// json-overrides-lua merge would leave it stuck on git forever.
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

// Decide whether the MAIN index should be fetched as a versioned artifact (vs
// git clone/pull). The OFFICIAL remote index always converges to artifact in
// auto mode — git is only ever a fallback, never a terminal state:
//   - indexSource "artifact": force artifact (only for the official remote).
//   - indexSource "git":      always git.
//   - indexSource "auto":     artifact whenever the configured main index is the
//     official index from a remote source. This is the symmetric counterpart of
//     the sub-index C1 migration gate: a stranded git main (pkgs/ present, marker
//     lost — e.g. after an interrupted swap or a destructive git fallback) heals
//     itself on the next update instead of being frozen on git forever. Local
//     (file://)/fork/custom-URL mains are excluded by isOfficialRemote, so git
//     fixtures and private indexes are unaffected.
// `hasMarker`/`hasPkgs` are kept in the signature for symmetry/diagnostics and
// future ETag-style gating; auto no longer needs them because an official remote
// always attempts artifact (a redundant attempt on an already-current index is a
// cheap pointer fetch + sha match).
bool main_should_attempt_artifact(bool isOfficialRemote,
                                  const std::string& indexSource,
                                  bool /*hasMarker*/, bool /*hasPkgs*/) {
    if (indexSource == "artifact") return isOfficialRemote;
    if (indexSource == "git")      return false;
    return isOfficialRemote;
}

// Decide whether a sub-index should be fetched as a versioned artifact (vs git
// clone/pull). Mirrors the main index's gating:
//   - indexSource "artifact": force artifact for default-official subs.
//   - indexSource "git":      always git.
//   - indexSource "auto":     artifact for a default-official sub that is fresh
//     (no pkgs), already artifact-managed, OR — once the MAIN index is
//     artifact-managed — to MIGRATE an existing git checkout (the whole index
//     is one unit; this is the C1 migration gate so existing installs don't
//     stay split: main on artifact, subs on git).
// Non-default / URL-overridden / local sub-indexes are never artifact-fetched.
bool sub_should_attempt_artifact(bool isDefaultOfficial,
                                 const std::string& indexSource,
                                 bool subManaged, bool subHasPkgs,
                                 bool mainArtifactManaged) {
    if (indexSource == "artifact") return isDefaultOfficial;
    if (indexSource == "git")      return false;
    return isDefaultOfficial
        && (subManaged || !subHasPkgs || mainArtifactManaged);
}

// A local repo source (file:// URL or a filesystem path) has no network host,
// so the TCP reachability probe below always reports it "unreachable" and would
// wrongly skip it. Local sources are always reachable — let git clone/pull
// validate the path instead. (Fixes e2e sub-index file:// fixtures being
// "host unreachable within 3000ms" and never synced.)
bool is_local_repo_url(std::string_view u) {
    return u.starts_with("file://")
        || u.starts_with("/")
        || u.starts_with("./")
        || u.starts_with("../");
}

// Sync a single git repository (clone or pull)
// Returns true on success
bool sync_repo(const std::filesystem::path& localDir,
               const std::string& url,
               bool force = false) {
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

// Extract repo directory name from URL (matches Lua _to_repodir behavior)
// e.g. "https://github.com/d2learn/xim-pkgindex-d2x.git" -> "xim-pkgindex-d2x"
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

// Load sub-repos from the JSON file (written by sync or Lua RepoManager)
std::vector<IndexRepo> load_sub_repos_json(const std::filesystem::path& jsonFile) {
    namespace fs = std::filesystem;
    if (!fs::exists(jsonFile)) return {};

    try {
        auto content = platform::read_file_to_string(jsonFile.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return {};

        std::vector<IndexRepo> repos;
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (!it.value().is_string()) continue;
            auto url = it.value().get<std::string>();
            if (!url.empty()) {
                repos.push_back({it.key(), url});
            }
        }
        return repos;
    } catch (...) {
        return {};
    }
}

// Save sub-repos to JSON file
void save_sub_repos_json(const std::filesystem::path& jsonFile,
                         const std::vector<IndexRepo>& repos) {
    nlohmann::json json = nlohmann::json::object();
    for (auto& repo : repos) {
        json[repo.name] = repo.url;
    }
    try {
        std::filesystem::create_directories(jsonFile.parent_path());
        platform::write_string_to_file(jsonFile.string(), json.dump(2));
    } catch (...) {
        log::warn("failed to save sub-repos JSON: {}", jsonFile.string());
    }
}

// Get the sub-repos index directory (global or project-local)
std::filesystem::path sub_repos_dir(bool projectScope = false) {
    auto base = projectScope ? Config::project_data_dir() : Config::global_data_dir();
    if (base.empty()) return {};
    return base / "xim-index-repos";
}

// Get the sub-repos JSON file path
std::filesystem::path sub_repos_json_path(bool projectScope = false) {
    auto dir = sub_repos_dir(projectScope);
    if (dir.empty()) return {};
    return dir / "xim-indexrepos.json";
}

// Get directory for a sub-repo
std::filesystem::path sub_repo_dir_for(const IndexRepo& repo, bool projectScope = false) {
    return sub_repos_dir(projectScope) / url_to_dirname(repo.url);
}

// Return all discovered global sub-repos (for use by PackageCatalog)
std::vector<IndexRepo> discovered_global_sub_repos() {
    return load_sub_repos_json(sub_repos_json_path(false));
}

// Return all discovered project-local sub-repos
std::vector<IndexRepo> discovered_project_sub_repos() {
    auto path = sub_repos_json_path(true);
    if (path.empty()) return {};
    return load_sub_repos_json(path);
}

// #366: true once the default/global sub-indexes have been synced at least
// once (the persisted xim-indexrepos.json marker exists). On a fresh machine
// this is false even though the main index rebuilds fine, which is exactly why
// get_catalog() must force a first-run sync — otherwise sub-index packages
// (scode/awesome/d2x) never resolve until the user runs `xlings update`.
bool sub_indexes_initialized() {
    auto p = sub_repos_json_path(false);
    return !p.empty() && std::filesystem::exists(p);
}

// Get the main index repo directory path (always global)
std::filesystem::path main_repo_dir() {
    auto& repos = Config::global_index_repos();
    if (!repos.empty()) {
        return Config::repo_dir_for(repos[0], false);
    }
    return Config::global_data_dir() / "xim-pkgindex";
}

// Sync all configured repos.
// Global repos live under XLINGS_HOME/data, project repos under project .xlings/data.
bool sync_all_repos(bool force = false) {
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

    // ── Index-as-Resource (Y-asset) ─────────────────────────────────
    // Fetch the main index as a versioned artifact over the resource-server
    // HTTP path (gains adaptive mirror reorder + stall watchdog; closes G2).
    // git clone is the fallback. XLINGS_INDEX_SOURCE=git|artifact|auto (auto).
    // An artifact-managed index has no .git, so once installed via artifact we
    // must NOT let the git path try to clone it again — the .xlings-index-version
    // marker records that state.
    std::string indexSource = "auto";
    if (auto* e = std::getenv("XLINGS_INDEX_SOURCE"); e && *e) indexSource = e;

    bool mainArtifactManaged = fs::exists(mainDir / ".xlings-index-version");
    bool mainHasIndex = fs::exists(mainDir / "pkgs");

    // The artifact is the OFFICIAL xim index. In auto mode it applies ONLY when
    // the configured main index is the official index from a *remote* source —
    // never when the user points index_repos at a local path (file://, abs
    // path: test fixtures, project-local indexes) or a custom fork URL, which
    // must be managed by their own source (git/local).
    auto globalRepos = Config::global_index_repos();
    bool mainIsOfficialRemote =
        !globalRepos.empty()
        && !Config::is_local_repo_source(globalRepos.front(), false)
        && (globalRepos.front().url.find("openxlings/xim-pkgindex") != std::string::npos
            || globalRepos.front().url.find("sunrisepeak/xim-pkgindex") != std::string::npos);

    // auto: the official remote main index always converges to artifact (a
    // stranded git main self-heals; see main_should_attempt_artifact). git stays
    // the fallback. XLINGS_INDEX_SOURCE=artifact forces it (power user / tests);
    // =git disables. Non-official/local/fork mains are excluded upstream.
    bool attemptArtifact = main_should_attempt_artifact(
        mainIsOfficialRemote, indexSource, mainArtifactManaged, mainHasIndex);

    if (attemptArtifact) {
        std::string ferr;
        if (fetch_index_artifact(mainDir, ferr)) {
            mainArtifactManaged = true;
        } else if (indexSource == "artifact") {
            log::error("[index] artifact fetch failed and git fallback disabled "
                       "(XLINGS_INDEX_SOURCE=artifact): {}", ferr);
            return false;
        } else {
            log::warn("[index] artifact fetch failed ({}); falling back to git", ferr);
        }
    }

    auto syncRepos = [&](const std::vector<IndexRepo>& repos, bool projectScope) {
        auto rootDir = projectScope ? Config::project_data_dir() : Config::global_data_dir();
        if (rootDir.empty()) return true;
        fs::create_directories(rootDir);
        for (auto& repo : repos) {
            auto repoDir = Config::repo_dir_for(repo, projectScope);
            // The main index is artifact-managed (no .git); never git-sync it.
            if (!projectScope && mainArtifactManaged && repoDir == mainDir) continue;
            if (Config::is_local_repo_source(repo, projectScope)) {
                auto sourceDir = Config::resolve_repo_source(repo, projectScope);
                if (!detail_::ensure_local_repo_link_(repoDir, sourceDir)) {
                    return false;
                }
                continue;
            }

            auto url = detail_::sync_repo_url_(repo.url, mirror);
            if (!sync_repo(repoDir, url, force)) {
                return false;
            }
        }
        return true;
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

    // Default sub-indexes — those provided by the main index's
    // xim-indexrepos.lua at their declared (now-authoritative) URL and from a
    // remote source — are fetched as artifacts, same model as the main index.
    // User-added sub-indexes (in xim-indexrepos.json but NOT a lua default),
    // URL-overridden ones, and local (file://) sources keep git.
    //
    // The signal is "name is a lua default AND its URL matches the lua default".
    // Because merge_sub_repos makes lua authoritative, repo.url == luaUrl for
    // every lua default, so this also recognises a default whose official org
    // moved (the stale json copy no longer overrides it).
    std::unordered_map<std::string, std::string> luaUrl;
    for (auto& r : luaSubRepos) luaUrl[r.name] = r.url;

    auto subReposRoot = sub_repos_dir();
    fs::create_directories(subReposRoot);

    for (auto& repo : allSubRepos) {
        auto repoDir = sub_repo_dir_for(repo);
        auto luaIt = luaUrl.find(repo.name);
        bool subDefaultOfficial =
            luaIt != luaUrl.end() && luaIt->second == repo.url
            && !Config::is_local_repo_source(repo, false);
        bool subManaged = fs::exists(repoDir / ".xlings-index-version");
        bool subAttemptArtifact = sub_should_attempt_artifact(
            subDefaultOfficial, indexSource, subManaged,
            fs::exists(repoDir / "pkgs"), mainArtifactManaged);

        bool ok = false;
        if (subAttemptArtifact) {
            std::string ferr;
            ok = fetch_index_artifact(repoDir, ferr, repo.name);
            if (!ok)
                log::warn("[index] sub-index '{}' artifact fetch failed: {}", repo.name, ferr);
        }
        // git fallback (and the path for user-added / local sub-indexes), unless
        // an official sub was explicitly pinned to the artifact source.
        if (!ok && !(indexSource == "artifact" && subDefaultOfficial)) {
            ok = sync_repo(repoDir, repo.url, force);
        }
        if (ok) syncedSubRepos.push_back(repo);
        else log::warn("failed to sync sub-index repo: {} ({})", repo.name, repo.url);
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
            auto repoDir = sub_repo_dir_for(repo, true);
            if (sync_repo(repoDir, repo.url, force)) {
                projSyncedSubs.push_back(repo);
            } else {
                log::warn("failed to sync project sub-index repo: {} ({})", repo.name, repo.url);
            }
        }
        save_sub_repos_json(sub_repos_json_path(true), projSyncedSubs);
    }
    return true;
}

// Read the git HEAD hash for a repo directory.
// Returns empty string for non-git repos or on any error.
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
        if (!fs::exists(refFile)) return {};

        auto hash = platform::read_file_to_string(refFile.string());
        while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r' || hash.back() == ' '))
            hash.pop_back();
        return hash;
    } catch (...) {
        return {};
    }
}

} // namespace xlings::xim
