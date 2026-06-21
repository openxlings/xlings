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
    // auto (default): fetch the artifact only when the index is fresh (missing)
    // or already artifact-managed. An existing *git* index (e.g. a test fixture
    // or a legacy checkout) keeps using git, so git-based e2e tests are
    // unaffected. XLINGS_INDEX_SOURCE=artifact forces the artifact path;
    // =git disables it entirely.
    bool attemptArtifact = (indexSource == "artifact")
        || (indexSource != "git" && (mainArtifactManaged || !mainHasIndex));
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

    // Merge: Lua-discovered repos + locally-added JSON repos (JSON overrides)
    std::unordered_map<std::string, IndexRepo> merged;
    for (auto& repo : luaSubRepos) merged[repo.name] = repo;
    for (auto& repo : jsonSubRepos) merged[repo.name] = repo;

    std::vector<IndexRepo> allSubRepos;
    std::vector<IndexRepo> syncedSubRepos;
    for (auto& [name, repo] : merged) allSubRepos.push_back(repo);

    auto subReposRoot = sub_repos_dir();
    fs::create_directories(subReposRoot);

    for (auto& repo : allSubRepos) {
        auto repoDir = sub_repo_dir_for(repo);
        // URLs are already mirror-selected from xim-indexrepos.lua, no further replacement needed
        if (sync_repo(repoDir, repo.url, force)) {
            syncedSubRepos.push_back(repo);
        } else {
            log::warn("failed to sync sub-index repo: {} ({})", repo.name, repo.url);
        }
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
    if (!fs::exists(headFile)) return {};

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
