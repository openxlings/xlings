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

int type_to_int(xpkg::PackageType t) {
    return static_cast<int>(t);
}

xpkg::PackageType int_to_type(int v) {
    switch (v) {
        case 1: return xpkg::PackageType::Script;
        case 2: return xpkg::PackageType::Template;
        case 3: return xpkg::PackageType::Config;
        case 4: return xpkg::PackageType::Subos;
        default: return xpkg::PackageType::Package;
    }
}

bool save_index_cache(const xpkg::PackageIndex& index,
                      const std::filesystem::path& cacheFile,
                      const std::string& repoHeadHash,
                      const std::string& defaultNamespace) {
    try {
        nlohmann::json entries = nlohmann::json::object();
        for (auto& [key, entry] : index.entries) {
            entries[key] = {
                {"identity", {
                    {"namespace", entry.identity.namespaceName},
                    {"name", entry.identity.name}
                }},
                {"canonical_name", entry.canonicalName},
                {"entry_key", entry.entryKey},
                {"name", entry.name},
                {"version", entry.version},
                {"path", entry.path.string()},
                {"type", type_to_int(entry.type)},
                {"description", entry.description},
                {"ref", entry.ref}
            };
        }

        nlohmann::json mutexGroups = nlohmann::json::object();
        for (auto& [gkey, members] : index.mutex_groups) {
            mutexGroups[gkey] = members;
        }

        nlohmann::json root = {
            {"version", CACHE_FORMAT_VERSION},
            {"repo_head_hash", repoHeadHash},
            {"default_namespace", defaultNamespace},
            {"entries", std::move(entries)},
            {"mutex_groups", std::move(mutexGroups)}
        };

        std::filesystem::create_directories(cacheFile.parent_path());
        platform::write_string_to_file(cacheFile.string(), root.dump());
        return true;
    } catch (...) {
        return false;
    }
}

struct CacheResult {
    std::string repoHeadHash;
    bool valid { false };
};

CacheResult load_index_cache(const std::filesystem::path& cacheFile,
                             xpkg::PackageIndex& index,
                             const std::string& defaultNamespace) {
    CacheResult result;
    if (!std::filesystem::exists(cacheFile)) return result;

    try {
        auto content = platform::read_file_to_string(cacheFile.string());
        auto root = nlohmann::json::parse(content, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return result;

        if (root.value("version", 0) != CACHE_FORMAT_VERSION) return result;
        if (root.value("default_namespace", std::string{}) != defaultNamespace) {
            return result;
        }

        result.repoHeadHash = root.value("repo_head_hash", "");

        if (!root.contains("entries") || !root["entries"].is_object()) {
            return result;
        }
        for (auto it = root["entries"].begin(); it != root["entries"].end(); ++it) {
            auto& val = it.value();
            if (!val.is_object()
                    || !val.contains("identity")
                    || !val["identity"].is_object()) {
                return result;
            }
            auto& identity = val["identity"];
            if (!identity.contains("namespace")
                    || !identity["namespace"].is_string()
                    || !identity.contains("name")
                    || !identity["name"].is_string()
                    || !val.contains("canonical_name")
                    || !val["canonical_name"].is_string()
                    || !val.contains("entry_key")
                    || !val["entry_key"].is_string()) {
                return result;
            }

            xpkg::IndexEntry entry;
            entry.identity.namespaceName = identity["namespace"].get<std::string>();
            entry.identity.name = identity["name"].get<std::string>();
            entry.canonicalName = val["canonical_name"].get<std::string>();
            entry.entryKey = val["entry_key"].get<std::string>();
            entry.name = val.value("name", "");
            entry.version = val.value("version", "");
            entry.path = std::filesystem::path(val.value("path", ""));
            entry.type = int_to_type(val.value("type", 0));
            entry.description = val.value("description", "");
            entry.ref = val.value("ref", "");

            if (entry.identity.name.empty()
                    || entry.identity.canonical_name() != entry.canonicalName
                    || entry.entryKey != it.key()
                    || entry.name != entry.identity.name) {
                return result;
            }

            auto [_, inserted] = index.entries.emplace(entry.entryKey, entry);
            if (!inserted) return result;
            index.identityEntries[entry.canonicalName].push_back(entry.entryKey);
            index.shortNames[entry.identity.name].push_back(entry.canonicalName);
        }

        for (auto& [_, candidates] : index.identityEntries) {
            std::ranges::sort(candidates);
            auto uniqueEnd = std::ranges::unique(candidates).begin();
            candidates.erase(uniqueEnd, candidates.end());
        }
        for (auto& [_, candidates] : index.shortNames) {
            std::ranges::sort(candidates);
            auto uniqueEnd = std::ranges::unique(candidates).begin();
            candidates.erase(uniqueEnd, candidates.end());
        }

        if (root.contains("mutex_groups") && root["mutex_groups"].is_object()) {
            for (auto it = root["mutex_groups"].begin(); it != root["mutex_groups"].end(); ++it) {
                std::vector<std::string> members;
                for (auto& m : it.value()) {
                    members.push_back(m.get<std::string>());
                }
                index.mutex_groups[it.key()] = std::move(members);
            }
        }

        result.valid = true;
    } catch (...) {
        // Corrupt cache — caller will rebuild
    }
    return result;
}

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
