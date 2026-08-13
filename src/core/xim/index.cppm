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
                          std::string defaultNamespace = {});

    void set_repo_dir(const std::filesystem::path& dir);

    void set_default_namespace(std::string defaultNamespace);

    // Build index by scanning pkgs/ directory via libxpkg
    std::expected<void, std::string> rebuild();

    // Load from cache if valid, else rebuild and save cache.
    std::expected<void, std::string> load_or_rebuild(
            const std::string& repoHeadHash,
            bool forceRebuild = false);

    bool is_loaded() const;
    std::size_t size() const;

    // Search packages by keyword (fuzzy, case-insensitive)
    std::vector<std::string> search(const std::string& keyword) const;

    std::vector<std::string>
    find_candidates(
            std::string_view name,
            std::optional<std::string_view> namespaceName = std::nullopt) const;

    // Match a version query like "gcc@15" to best version "gcc@15.1.0"
    std::optional<std::string> match_version(const std::string& name) const;

    // Resolve an alias (e.g., "c" -> "gcc")
    std::string resolve(const std::string& name) const;

    // Get mutex group packages for conflict detection
    std::vector<std::string> mutex_packages(const std::string& name) const;

    // Load full Package data for a specific entry
    std::expected<xpkg::Package, std::string>
    load_package(const std::string& name) const;

    // Get entry by name
    const xpkg::IndexEntry* find_entry(const std::string& name) const;

    // Mark installed state
    void mark_installed(const std::string& name, bool installed);

    // Merge another index (for sub-repos)
    void merge(xpkg::PackageIndex other, const std::string& ns = "");

    // Get all entry names (sorted)
    std::vector<std::string> all_names() const;

    // Get all installed entry names
    std::vector<std::string> installed_names() const;

    // Get the raw index
    const xpkg::PackageIndex& raw_index() const;

    // Get entry path for creating executors
    std::optional<std::filesystem::path> entry_path(const std::string& name) const;
};

} // namespace xlings::xim
