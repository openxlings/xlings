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
import xlings.platform.target;

// What is installed, taken from the records that decide it.
//
// The walk starts from the workspace and the version DB and asks the catalog
// only about the names it finds there. Materialising the whole index first and
// filtering it down to the installed rows is the other direction, and it made
// `info` -- a question about ONE package -- proportional to the size of the
// index. It also loaded every recipe twice, because building the matches
// already evaluates each one.
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
    // Where this package's payload lives. A project-scoped index repo installs
    // under the project data dir, so a single hard-coded store root reports
    // every project-scoped install as "payload missing".
    std::filesystem::path storeRoot;
};

// Targeted catalog lookups, memoised. Replaces a full-index scan: the only
// names asked about are the ones the install records mention.
class MetadataLookup {
public:
    MetadataLookup(PackageCatalog& catalog, std::string platform)
        : catalog_(&catalog), platform_(std::move(platform)) {}

    // A fixed table with no catalog behind it. Lets assemble_inventory be
    // exercised against a stated index rather than whatever the machine
    // running the test happens to have installed.
    explicit MetadataLookup(std::map<std::string, CatalogMetadata> table) {
        for (auto& [storeName, item] : table) {
            exact_.emplace(storeName, std::move(item));
        }
    }

    // Metadata for EXACTLY this identity. A hit means the index holds a
    // package whose own store name is the one asked for -- resolving
    // `xpkg-helper` to `xim:xpkg-helper` is not a hit for the namespace-less
    // identity, because those two are different rows in the inventory and
    // merging them here is how one installed package came to be listed twice.
    const CatalogMetadata* by_identity(const std::string& namespaceName,
                                       const std::string& name) {
        const auto storeName = package_store_name(namespaceName, name);
        if (const auto found = exact_.find(storeName); found != exact_.end()) {
            return found->second ? &*found->second : nullptr;
        }
        auto resolved = resolve_(namespaceName.empty()
            ? name : namespaceName + ":" + name);
        if (resolved
            && package_store_name(resolved->namespaceName, resolved->name)
                   != storeName) {
            resolved.reset();
        }
        const auto [it, _] = exact_.emplace(storeName, std::move(resolved));
        return it->second ? &*it->second : nullptr;
    }

    // For a target whose namespace the records never recorded. The catalog can
    // restore one when the short name is unambiguous; resolve_target reports
    // ambiguity rather than guessing, so a name shared by two namespaces stays
    // unresolved and the row keeps the identity it already had.
    const CatalogMetadata* by_short_name(const std::string& name) {
        if (const auto found = short_.find(name); found != short_.end()) {
            return found->second ? &*found->second : nullptr;
        }
        const auto [it, _] = short_.emplace(name, resolve_(name));
        return it->second ? &*it->second : nullptr;
    }

private:
    std::optional<CatalogMetadata> resolve_(const std::string& target) {
        if (catalog_ == nullptr) return std::nullopt;
        auto match = catalog_->resolve_target(target, platform_);
        if (!match) return std::nullopt;
        auto pkg = catalog_->load_package(*match);
        return CatalogMetadata{
            .namespaceName = match->namespaceName,
            .name = match->name,
            .canonicalName = match->canonicalName,
            .description = pkg ? std::string(pkg->description) : std::string{},
            .programs = pkg ? pkg->programs : std::vector<std::string>{},
            .storeRoot = match->storeRoot,
        };
    }

    PackageCatalog* catalog_ { nullptr };
    std::string platform_;
    std::map<std::string, std::optional<CatalogMetadata>> exact_;
    std::map<std::string, std::optional<CatalogMetadata>> short_;
};

std::string coordinate_key(const xvm::InstallCoordinate& coordinate) {
    return coordinate.canonical();
}

xvm::InstallCoordinate owner_for(
    const xvm::VersionDB& db,
    const std::string& target,
    const std::string& version,
    const std::filesystem::path& storeRoot,
    MetadataLookup& metadata) {
    const auto candidates = xvm::owner_candidates(db, target, version);
    for (const auto& candidate : candidates) {
        if (metadata.by_identity(candidate.ns, candidate.package)) {
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
    if (const auto* unique = metadata.by_short_name(target)) {
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
    MetadataLookup& metadata,
    bool includePayloadOnly) {
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
    if (includePayloadOnly && std::filesystem::exists(storeRoot)) {
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
        auto root = storeRoot;
        const auto* found = metadata.by_identity(record.namespaceName,
                                                 record.name);
        if (found) {
            record.namespaceName = found->namespaceName;
            record.name = found->name;
            record.canonicalName = found->canonicalName;
            record.description = found->description;
            record.programs = found->programs;
            if (!found->storeRoot.empty()) root = found->storeRoot;
        }
        record.payloadPath = root
            / package_store_name(record.namespaceName, record.name)
            / record.version;
        record.payloadPresent = payload_has_content(record.payloadPath);
        if (!record.payloadPresent) {
            record.degradedReason = "payload missing";
        } else if (!found) {
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

constexpr std::string_view inventory_platform() {
    return platform::build_os();
}

// `allSubos` widens which workspaces are read. `includePayloadOnly` decides
// whether a package with no runnable target -- present only as a stamped
// payload -- appears at all. They used to be one flag, which made such a
// package visible from `list --all` and invisible from `list`, for no reason a
// user could name.
std::vector<InstalledPackageRecord> collect_inventory(
    PackageCatalog& catalog, bool allSubos, bool includePayloadOnly = true) {
    detail::MetadataLookup metadata(catalog,
                                    std::string(inventory_platform()));

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
        Config::global_data_dir() / "xpkgs", metadata, includePayloadOnly);
}

// One package's rows, without materialising the rest of the inventory. This is
// what `info` needs: whether the package is installed at all, and whether the
// exact version it selected is.
std::vector<InstalledPackageRecord> collect_package_inventory(
    PackageCatalog& catalog, const std::string& canonicalName) {
    auto records = collect_inventory(catalog, /*allSubos=*/true);
    std::erase_if(records, [&](const auto& record) {
        return record.canonicalName != canonicalName;
    });
    return records;
}

}  // namespace xlings::xim
