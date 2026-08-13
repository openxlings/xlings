export module xlings.core.xim.index;

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.index;
import xlings.libs.json;
import xlings.core.log;
import xlings.core.config;
import xlings.platform;

// All libxpkg functions live in mcpplibs::xpkg namespace regardless of module name
namespace xpkg = mcpplibs::xpkg;

namespace xlings::xim::cache_detail_ {

constexpr int CACHE_FORMAT_VERSION = 2;

int type_to_int(xpkg::PackageType t);

xpkg::PackageType int_to_type(int v);

bool save_index_cache(const xpkg::PackageIndex& index,
                      const std::filesystem::path& cacheFile,
                      const std::string& repoHeadHash,
                      const std::string& defaultNamespace);

struct CacheResult {
    std::string repoHeadHash;
    bool valid { false };
};

CacheResult load_index_cache(const std::filesystem::path& cacheFile,
                             xpkg::PackageIndex& index,
                             const std::string& defaultNamespace);

}  // namespace xlings::xim::cache_detail_

export namespace xlings::xim {

class IndexManager {
    xpkg::PackageIndex index_;
    std::filesystem::path repoDir_;
    std::string defaultNamespace_;
    bool loaded_ { false };

public:
    IndexManager() = default;

    explicit IndexManager(const std::filesystem::path& repoDir,
                          std::string defaultNamespace = {})
        : repoDir_(repoDir),
          defaultNamespace_(std::move(defaultNamespace)) {}

    void set_repo_dir(const std::filesystem::path& dir) {
        repoDir_ = dir;
    }

    void set_default_namespace(std::string defaultNamespace) {
        defaultNamespace_ = std::move(defaultNamespace);
    }

    // Build index by scanning pkgs/ directory via libxpkg
    std::expected<void, std::string> rebuild() {
        namespace fs = std::filesystem;

        if (repoDir_.empty()) {
            return std::unexpected("repo directory not set");
        }

        if (!fs::exists(repoDir_ / "pkgs")) {
            return std::unexpected(
                std::format("pkgs/ directory not found in {}", repoDir_.string()));
        }

        log::debug("building package index from {}", repoDir_.string());

        auto result = xpkg::build_index(repoDir_, defaultNamespace_);
        if (!result) {
            return std::unexpected(
                std::format("build_index failed: {}", result.error()));
        }

        index_ = std::move(*result);
        loaded_ = true;

        log::debug("index built: {} entries", index_.entries.size());
        return {};
    }

    // Load from cache if valid, else rebuild and save cache.
    std::expected<void, std::string> load_or_rebuild(
            const std::string& repoHeadHash,
            bool forceRebuild = false) {
        if (repoDir_.empty()) {
            return std::unexpected("repo directory not set");
        }

        auto cacheFile = repoDir_ / ".xlings-index-cache.json";

        // Try loading from cache (unless forced or no git hash)
        if (!forceRebuild && !repoHeadHash.empty()) {
            xpkg::PackageIndex cached;
            auto cacheResult = cache_detail_::load_index_cache(
                cacheFile, cached, defaultNamespace_);
            if (cacheResult.valid && cacheResult.repoHeadHash == repoHeadHash) {
                index_ = std::move(cached);
                loaded_ = true;
                log::debug("index loaded from cache: {} entries", index_.entries.size());
                return {};
            }
        }

        // Cache miss or forced: full rebuild
        auto result = rebuild();
        if (!result) return result;

        // Save cache (best effort)
        if (!cache_detail_::save_index_cache(
                index_, cacheFile, repoHeadHash, defaultNamespace_)) {
            log::warn("failed to save index cache for {}", repoDir_.string());
        }

        return {};
    }

    bool is_loaded() const { return loaded_; }
    std::size_t size() const { return index_.entries.size(); }

    // Search packages by keyword (fuzzy, case-insensitive)
    std::vector<std::string> search(const std::string& keyword) const {
        return xpkg::search(index_, keyword);
    }

    std::vector<std::string>
    find_candidates(
            std::string_view name,
            std::optional<std::string_view> namespaceName = std::nullopt) const {
        return xpkg::find_candidates(index_, name, namespaceName);
    }

    // Match a version query like "gcc@15" to best version "gcc@15.1.0"
    std::optional<std::string> match_version(const std::string& name) const {
        if (name.contains(':') || index_.entries.contains(name)) {
            return xpkg::match_version(index_, name);
        }
        auto candidates = find_candidates(name);
        if (candidates.size() != 1) return std::nullopt;
        return xpkg::match_version(index_, candidates.front());
    }

    // Resolve an alias (e.g., "c" -> "gcc")
    std::string resolve(const std::string& name) const {
        if (name.contains(':') || index_.entries.contains(name)) {
            return xpkg::resolve(index_, name);
        }
        auto candidates = find_candidates(name);
        if (candidates.size() != 1) return name;
        return xpkg::resolve(index_, candidates.front());
    }

    // Get mutex group packages for conflict detection
    std::vector<std::string> mutex_packages(const std::string& name) const {
        return xpkg::mutex_packages(index_, name);
    }

    // Load full Package data for a specific entry
    std::expected<xpkg::Package, std::string>
    load_package(const std::string& name) const {
        auto it = index_.entries.find(name);
        if (it == index_.entries.end()) {
            return std::unexpected(std::format("package '{}' not found in index", name));
        }
        return xpkg::load_package(it->second.path);
    }

    // Get entry by name
    const xpkg::IndexEntry* find_entry(const std::string& name) const {
        auto it = index_.entries.find(name);
        return it != index_.entries.end() ? &it->second : nullptr;
    }

    // Mark installed state
    void mark_installed(const std::string& name, bool installed) {
        xpkg::set_installed(index_, name, installed);
    }

    // Merge another index (for sub-repos)
    void merge(xpkg::PackageIndex other, const std::string& ns = "") {
        index_ = xpkg::merge(std::move(index_), other, ns);
    }

    // Get all entry names (sorted)
    std::vector<std::string> all_names() const {
        std::vector<std::string> names;
        names.reserve(index_.entries.size());
        for (auto& [k, v] : index_.entries) {
            names.push_back(k);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    // Get all installed entry names
    std::vector<std::string> installed_names() const {
        std::vector<std::string> names;
        for (auto& [k, v] : index_.entries) {
            if (v.installed) names.push_back(k);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    // Get the raw index
    const xpkg::PackageIndex& raw_index() const { return index_; }

    // Get entry path for creating executors
    std::optional<std::filesystem::path> entry_path(const std::string& name) const {
        auto it = index_.entries.find(name);
        if (it == index_.entries.end()) return std::nullopt;
        return it->second.path;
    }
};

} // namespace xlings::xim
