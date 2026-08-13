module xlings.core.xim.index;

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.index;
import xlings.libs.json;
import xlings.core.log;
import xlings.core.config;
import xlings.platform;

namespace xlings::xim::cache_detail_ {

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

}
