export module xlings.core.xim.inventory;

import std;
import xlings.core.config;
import xlings.core.profile;
import xlings.core.version_order;
import xlings.core.xim.catalog;
import xlings.core.xim.payload;
import xlings.core.xvm.db;
import xlings.core.xvm.owner;
import xlings.core.xvm.types;
import xlings.platform;

export namespace xlings::xim {

struct InstalledPackageRecord {
    std::string namespaceName;
    std::string name;
    std::string canonicalName;
    std::string version;
    std::set<std::string> suboses;
    bool inCurrentSubos { false };
    bool payloadPresent { false };
    bool active { false };
    std::string description;
    std::vector<std::string> programs;
    std::filesystem::path payloadPath;
    std::string degradedReason;
};

struct InventoryWorkspace {
    std::string name;
    xvm::Workspace active;
    xvm::WorkspaceInstalled installed;
    bool current { false };
};

std::pair<std::string, std::string> identity_from_store_name(
    std::string_view storeName) {
    const auto separator = storeName.find("-x-");
    if (separator == std::string_view::npos) {
        return {{}, std::string(storeName)};
    }
    return {std::string(storeName.substr(0, separator)),
            std::string(storeName.substr(separator + 3))};
}

namespace detail {

struct CatalogMetadata {
    std::string namespaceName;
    std::string name;
    std::string canonicalName;
    std::string description;
    std::vector<std::string> programs;
};

std::string coordinate_key(const xvm::InstallCoordinate& coordinate) {
    return coordinate.canonical();
}

xvm::InstallCoordinate owner_for(
    const xvm::VersionDB& db,
    const std::string& target,
    const std::string& version,
    const std::filesystem::path& storeRoot,
    const std::map<std::string, CatalogMetadata>& metadata) {
    const auto candidates = xvm::owner_candidates(db, target, version);
    for (const auto& candidate : candidates) {
        if (metadata.contains(package_store_name(candidate.ns,
                                                 candidate.package))) {
            return candidate;
        }
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_directory(
                storeRoot / package_store_name(candidate.ns, candidate.package)
                    / candidate.version, ec)) {
            return candidate;
        }
    }
    // Legacy DB entries may only carry the target's short name. The catalog
    // can restore its namespace when that short name is unique; it still does
    // not decide whether the record exists or which exact version is owned.
    const CatalogMetadata* unique = nullptr;
    for (const auto& [_, item] : metadata) {
        if (item.name != target) continue;
        if (unique) {
            unique = nullptr;
            break;
        }
        unique = &item;
    }
    if (unique) {
        const auto [_, bareVersion] = xvm::parse_ns_version(version);
        return {.ns = unique->namespaceName,
                .package = unique->name,
                .version = bareVersion};
    }
    if (!candidates.empty()) return candidates.front();
    const auto [ns, bareVersion] = xvm::parse_ns_version(version);
    return {.ns = ns, .package = target, .version = bareVersion};
}

std::vector<InstalledPackageRecord> assemble_inventory(
    const xvm::VersionDB& db,
    std::span<const InventoryWorkspace> workspaces,
    const std::filesystem::path& storeRoot,
    const std::map<std::string, CatalogMetadata>& metadata,
    bool includePayloadMetadata) {
    std::map<std::string, InstalledPackageRecord> records;

    const auto ensure_record = [&](const xvm::InstallCoordinate& coordinate)
        -> InstalledPackageRecord& {
        auto [it, inserted] = records.try_emplace(coordinate_key(coordinate));
        auto& record = it->second;
        if (inserted) {
            record.namespaceName = coordinate.ns;
            record.name = coordinate.package;
            record.canonicalName = coordinate.ns.empty()
                ? coordinate.package
                : coordinate.ns + ":" + coordinate.package;
            record.version = coordinate.version;
            const auto storeName = package_store_name(coordinate.ns,
                                                       coordinate.package);
            record.payloadPath = storeRoot / storeName / coordinate.version;
        }
        return record;
    };

    for (const auto& workspace : workspaces) {
        for (const auto& [target, versions] : workspace.installed) {
            for (const auto& version : versions) {
                auto coordinate = owner_for(db, target, version,
                                            storeRoot, metadata);
                auto& record = ensure_record(coordinate);
                record.suboses.insert(workspace.name);
                record.inCurrentSubos |= workspace.current;
            }
        }
        for (const auto& [target, version] : workspace.active) {
            auto coordinate = owner_for(db, target, version,
                                        storeRoot, metadata);
            auto& record = ensure_record(coordinate);
            record.suboses.insert(workspace.name);
            record.inCurrentSubos |= workspace.current;
            record.active = true;
        }
    }

    // A payload directory is not an install record. Only the installer-owned
    // sidecar/marker can add a payload-only package (for example a package
    // with no runnable target). Arbitrary leftover directories are ignored.
    if (includePayloadMetadata && std::filesystem::exists(storeRoot)) {
        for (const auto& packageDir : platform::dir_entries(storeRoot)) {
            std::error_code ec;
            if (!packageDir.is_directory(ec)) continue;
            const auto storeName = packageDir.path().filename().string();
            auto [namespaceName, name] = identity_from_store_name(storeName);
            for (const auto& versionDir : platform::dir_entries(packageDir.path())) {
                if (!versionDir.is_directory(ec)) continue;
                const auto stamped = std::filesystem::is_regular_file(
                    versionDir.path() / std::filesystem::path(kPayloadStampFile), ec);
                ec.clear();
                const auto legacyMarked = std::filesystem::is_regular_file(
                    versionDir.path() / ".xim-installed", ec);
                if (!stamped && !legacyMarked) continue;
                ensure_record({.ns = namespaceName,
                               .package = name,
                               .version = versionDir.path().filename().string()});
            }
        }
    }

    std::vector<InstalledPackageRecord> result;
    result.reserve(records.size());
    for (auto& [_, record] : records) {
        record.payloadPresent = payload_has_content(record.payloadPath);
        const auto storeName = package_store_name(record.namespaceName,
                                                   record.name);
        if (const auto found = metadata.find(storeName); found != metadata.end()) {
            record.namespaceName = found->second.namespaceName;
            record.name = found->second.name;
            record.canonicalName = found->second.canonicalName;
            record.description = found->second.description;
            record.programs = found->second.programs;
        }
        if (!record.payloadPresent) {
            record.degradedReason = "payload missing";
        } else if (!metadata.contains(storeName)) {
            record.degradedReason = "index entry unavailable";
        }
        result.push_back(std::move(record));
    }

    std::ranges::stable_sort(result, [](const auto& lhs, const auto& rhs) {
        if (lhs.canonicalName != rhs.canonicalName) {
            return lhs.canonicalName < rhs.canonicalName;
        }
        return version_order::compare(lhs.version, rhs.version) > 0;
    });
    return result;
}

}  // namespace detail

std::vector<InstalledPackageRecord> collect_inventory(PackageCatalog& catalog,
                                                       bool allSubos) {
#if defined(_WIN32)
    constexpr std::string_view platformName = "windows";
#elif defined(__APPLE__)
    constexpr std::string_view platformName = "macosx";
#else
    constexpr std::string_view platformName = "linux";
#endif
    std::map<std::string, detail::CatalogMetadata> metadata;
    for (auto& match : catalog.search("", std::string(platformName))) {
        auto pkg = catalog.load_package(match);
        metadata.emplace(package_store_name(match.namespaceName, match.name),
            detail::CatalogMetadata{
                .namespaceName = match.namespaceName,
                .name = match.name,
                .canonicalName = match.canonicalName,
                .description = pkg ? std::string(pkg->description) : std::string{},
                .programs = pkg ? pkg->programs : std::vector<std::string>{},
            });
    }

    std::vector<InventoryWorkspace> workspaces;
    const auto currentName = Config::paths().subosDir.filename().string();
    workspaces.push_back({
        .name = currentName.empty() ? std::string{"current"} : currentName,
        .active = Config::effective_workspace(),
        .installed = Config::workspace_installed(),
        .current = true,
    });
    if (allSubos) {
        for (auto& snapshot : profile::load_subos_snapshots(
                 Config::paths().homeDir)) {
            std::error_code equivalentEc;
            const auto samePath = std::filesystem::equivalent(
                snapshot.dir, Config::paths().subosDir,
                equivalentEc);
            if (snapshot.name == currentName || samePath) continue;
            workspaces.push_back({
                .name = snapshot.name,
                .active = std::move(snapshot.workspace.active),
                .installed = std::move(snapshot.workspace.installed),
                .current = false,
            });
        }
    }

    return detail::assemble_inventory(Config::versions(), workspaces,
        Config::global_data_dir() / "xpkgs", metadata, allSubos);
}

}  // namespace xlings::xim
