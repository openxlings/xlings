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

// Optional instrumentation for structural query tests.  These are the only
// two operations whose fan-out made one-package queries scale with an entire
// home: evaluating catalog metadata and inspecting payload version roots.
struct InventoryTrace {
    std::vector<std::string> metadataIdentities;
    std::vector<std::filesystem::path> payloadVersionDirs;
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
    MetadataLookup(PackageCatalog& catalog, std::string platform,
                   InventoryTrace* trace = nullptr)
        : catalog_(&catalog), platform_(std::move(platform)), trace_(trace) {}

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
        if (trace_ != nullptr) trace_->metadataIdentities.push_back(target);
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
    InventoryTrace* trace_ { nullptr };
    std::map<std::string, std::optional<CatalogMetadata>> exact_;
    std::map<std::string, std::optional<CatalogMetadata>> short_;
};

void trace_payload_visit(InventoryTrace* trace,
                         const std::filesystem::path& versionDir) {
    if (trace != nullptr) trace->payloadVersionDirs.push_back(versionDir);
}

bool identity_matches_filter(std::string_view namespaceName,
                             std::string_view name,
                             std::string_view filter) {
    const auto canonical = canonical_package_name(namespaceName, name);
    return canonical.contains(filter) || name.contains(filter);
}

bool target_may_match_filter(const xvm::VersionDB& db,
                             const std::string& target,
                             const std::string& version,
                             const std::optional<std::string_view>& filter) {
    if (!filter || filter->empty()) return true;
    const auto versionNamespace = xvm::parse_ns_version(version).first;
    if (identity_matches_filter(versionNamespace, target, *filter)) return true;
    const auto parsedFilter = parse_package_target(std::string(*filter));
    if (versionNamespace.empty()
        && !parsedFilter.name.empty() && parsedFilter.name == target) {
        return true;
    }

    const auto infoIt = db.find(target);
    if (infoIt == db.end()) return false;
    const auto dataIt = infoIt->second.versions.find(version);
    if (dataIt == infoIt->second.versions.end()) return false;
    const auto& data = dataIt->second;
    if (data.bindingGroup) {
        const auto provider = parse_package_target(data.bindingGroup->provider);
        if (identity_matches_filter(
                provider.namespaceName, provider.name, *filter)) {
            return true;
        }
    }
    if (const auto coordinate = xvm::coordinate_from_payload_path(data.path)) {
        if (identity_matches_filter(
                coordinate->ns, coordinate->package, *filter)) {
            return true;
        }
    }
    return false;
}

using RelatedCoordinates = std::map<
    std::pair<std::string, std::string>, xvm::InstallCoordinate>;
using TargetVersion = std::pair<std::string, std::string>;

enum class CoordinateMatch {
    contains,
    exact,
};

std::string coordinate_key(const xvm::InstallCoordinate& coordinate) {
    return coordinate.canonical();
}

void push_candidate(std::vector<xvm::InstallCoordinate>& candidates,
                    xvm::InstallCoordinate candidate) {
    if (candidate.empty() || candidate.version.empty()) return;
    if (std::ranges::find(candidates, candidate) != candidates.end()) return;
    candidates.push_back(std::move(candidate));
}

xvm::InstallCoordinate owner_from_candidates(
    std::span<const xvm::InstallCoordinate> candidates,
    const std::string& target,
    const std::string& version,
    const std::filesystem::path& storeRoot,
    MetadataLookup& metadata,
    InventoryTrace* trace = nullptr) {
    for (const auto& candidate : candidates) {
        if (metadata.by_identity(candidate.ns, candidate.package)) {
            return candidate;
        }
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        const auto versionDir = storeRoot
            / package_store_name(candidate.ns, candidate.package)
            / candidate.version;
        trace_payload_visit(trace, versionDir);
        if (std::filesystem::is_directory(versionDir, ec)) {
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

xvm::InstallCoordinate owner_for(
    const xvm::VersionDB& db,
    const std::string& target,
    const std::string& version,
    const std::filesystem::path& storeRoot,
    MetadataLookup& metadata,
    InventoryTrace* trace = nullptr) {
    const auto candidates = xvm::owner_candidates(db, target, version);
    return owner_from_candidates(
        candidates, target, version, storeRoot, metadata, trace);
}

std::vector<InstalledPackageRecord> assemble_inventory(
    const xvm::VersionDB& db,
    std::span<const InventoryWorkspace> workspaces,
    const std::filesystem::path& storeRoot,
    MetadataLookup& metadata,
    bool includePayloadOnly,
    std::optional<std::string_view> identityFilter = std::nullopt,
    InventoryTrace* trace = nullptr,
    const RelatedCoordinates* filteredCoordinates = nullptr) {
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

    const auto resolve_workspace_coordinate = [&]
        (const std::string& target, const std::string& version)
        -> std::optional<xvm::InstallCoordinate> {
        if (filteredCoordinates != nullptr) {
            if (const auto found = filteredCoordinates->find({target, version});
                found != filteredCoordinates->end()) {
                return found->second;
            }
            if (const auto infoIt = db.find(target); infoIt != db.end()
                && infoIt->second.versions.contains(version)) {
                return std::nullopt;
            }
        }
        if (!target_may_match_filter(
                db, target, version, identityFilter)) {
            return std::nullopt;
        }
        auto coordinate = owner_for(
            db, target, version, storeRoot, metadata, trace);
        if (identityFilter && !identity_matches_filter(
                coordinate.ns, coordinate.package, *identityFilter)) {
            return std::nullopt;
        }
        return coordinate;
    };

    for (const auto& workspace : workspaces) {
        for (const auto& [target, versions] : workspace.installed) {
            for (const auto& version : versions) {
                auto coordinate = resolve_workspace_coordinate(target, version);
                if (!coordinate) continue;
                auto& record = ensure_record(*coordinate);
                record.suboses.insert(workspace.name);
                record.inCurrentSubos |= workspace.current;
            }
        }
        for (const auto& [target, version] : workspace.active) {
            auto coordinate = resolve_workspace_coordinate(target, version);
            if (!coordinate) continue;
            auto& record = ensure_record(*coordinate);
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
            if (identityFilter && !identity_matches_filter(
                    namespaceName, name, *identityFilter)) continue;
            for (const auto& versionDir : platform::dir_entries(packageDir.path())) {
                if (!versionDir.is_directory(ec)) continue;
                trace_payload_visit(trace, versionDir.path());
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
        trace_payload_visit(trace, record.payloadPath);
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

std::vector<InventoryWorkspace> load_inventory_workspaces(bool allSubos) {
    std::vector<InventoryWorkspace> workspaces;
    const auto currentName = Config::paths().subosDir.filename().string();
    workspaces.push_back({
        .name = currentName.empty() ? std::string{"current"} : currentName,
        .active = Config::effective_workspace(),
        .installed = Config::workspace_installed(),
        .current = true,
    });
    if (!allSubos) return workspaces;

    for (auto& snapshot : profile::load_subos_snapshots(
             Config::paths().homeDir)) {
        std::error_code equivalentEc;
        const auto samePath = std::filesystem::equivalent(
            snapshot.dir, Config::paths().subosDir, equivalentEc);
        if (snapshot.name == currentName || samePath) continue;
        workspaces.push_back({
            .name = snapshot.name,
            .active = std::move(snapshot.workspace.active),
            .installed = std::move(snapshot.workspace.installed),
            .current = false,
        });
    }
    return workspaces;
}

std::vector<TargetVersion> reciprocal_legacy_component(
    const xvm::VersionDB& db, const TargetVersion& seed) {
    std::vector<TargetVersion> component;
    std::vector<TargetVersion> pending{seed};
    std::set<TargetVersion> seen;
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (!seen.insert(current).second) continue;

        const auto infoIt = db.find(current.first);
        if (infoIt == db.end()) continue;
        const auto dataIt = infoIt->second.versions.find(current.second);
        if (dataIt == infoIt->second.versions.end()
            || dataIt->second.bindingGroup) {
            continue;
        }
        component.push_back(current);

        for (const auto& [peerTarget, ownVersions] :
             infoIt->second.bindings) {
            const auto edgeIt = ownVersions.find(current.second);
            if (edgeIt == ownVersions.end()) continue;
            const auto peerInfoIt = db.find(peerTarget);
            if (peerInfoIt == db.end()
                || !peerInfoIt->second.versions.contains(edgeIt->second)) {
                continue;
            }
            const auto reverseIt =
                peerInfoIt->second.bindings.find(current.first);
            if (reverseIt == peerInfoIt->second.bindings.end()) continue;
            const auto reverseVersionIt =
                reverseIt->second.find(edgeIt->second);
            if (reverseVersionIt == reverseIt->second.end()
                || reverseVersionIt->second != current.second) {
                continue;
            }
            pending.emplace_back(peerTarget, edgeIt->second);
        }
    }
    return component;
}

// Build the reverse (target, version) -> package coordinate index once,
// before reading any SubOS workspace. Modern records name their provider
// directly; older records retain a payload-store coordinate or a reciprocal
// binding component. A filter bounds both metadata lookup and component
// traversal to candidates that can contribute to the requested result.
RelatedCoordinates build_related_coordinates(
    const xvm::VersionDB& db,
    std::string_view filter,
    const std::filesystem::path& storeRoot,
    MetadataLookup& metadata,
    InventoryTrace* trace = nullptr,
    CoordinateMatch match = CoordinateMatch::contains) {
    RelatedCoordinates related;
    std::vector<TargetVersion> legacySeeds;

    const auto basic_candidates = [&](const std::string& target,
                                      const std::string& version,
                                      const xvm::VData& data) {
        std::vector<xvm::InstallCoordinate> candidates;
        if (data.bindingGroup && !data.bindingGroup->providerVersion.empty()) {
            const auto provider = parse_package_target(
                data.bindingGroup->provider);
            push_candidate(candidates, {
                .ns = provider.namespaceName,
                .package = provider.name,
                .version = xvm::strip_namespace(
                    data.bindingGroup->providerVersion),
            });
        }
        if (const auto fromPath =
                xvm::coordinate_from_payload_path(data.path)) {
            push_candidate(candidates, *fromPath);
        }
        const auto [entryNamespace, bareVersion] =
            xvm::parse_ns_version(version);
        push_candidate(candidates, {.ns = entryNamespace,
                                    .package = target,
                                    .version = bareVersion});
        return candidates;
    };

    const auto matches = [&](const xvm::InstallCoordinate& candidate) {
        if (match == CoordinateMatch::exact) {
            return canonical_package_name(candidate.ns, candidate.package)
                == filter;
        }
        return identity_matches_filter(
            candidate.ns, candidate.package, filter);
    };
    const auto may_match = [&](const auto& candidates) {
        return filter.empty() || std::ranges::any_of(candidates,
            [&](const auto& candidate) { return matches(candidate); });
    };
    const auto keep_owner = [&](const xvm::InstallCoordinate& owner) {
        return filter.empty() || matches(owner);
    };

    for (const auto& [target, info] : db) {
        for (const auto& [version, data] : info.versions) {
            auto candidates = basic_candidates(target, version, data);
            if (!may_match(candidates)) continue;
            const TargetVersion key{target, version};
            if (match == CoordinateMatch::exact) {
                const auto candidate = std::ranges::find_if(
                    candidates, [&](const auto& item) {
                        return matches(item);
                    });
                if (candidate != candidates.end()) {
                    related.try_emplace(key, *candidate);
                }
                if (!data.bindingGroup && !info.bindings.empty()) {
                    legacySeeds.push_back(key);
                }
                continue;
            }
            if (!data.bindingGroup && !info.bindings.empty()) {
                legacySeeds.push_back(key);
            } else {
                auto owner = owner_from_candidates(
                    candidates, target, version, storeRoot, metadata, trace);
                if (keep_owner(owner)) related.try_emplace(key, std::move(owner));
            }
        }
    }

    std::set<TargetVersion> processed;
    for (const auto& seed : legacySeeds) {
        if (processed.contains(seed)) continue;
        auto component = reciprocal_legacy_component(db, seed);
        std::ranges::sort(component);
        processed.insert(component.begin(), component.end());

        if (match == CoordinateMatch::exact) {
            const auto seedCoordinate = related.find(seed);
            if (seedCoordinate == related.end()) continue;
            for (const auto& current : component) {
                const auto& data =
                    db.at(current.first).versions.at(current.second);
                if (const auto fromPath =
                        xvm::coordinate_from_payload_path(data.path);
                    fromPath && !matches(*fromPath)) {
                    continue;
                }
                related.try_emplace(current, seedCoordinate->second);
            }
            continue;
        }

        for (const auto& [target, version] : component) {
            const auto& data = db.at(target).versions.at(version);
            auto candidates = basic_candidates(target, version, data);
            for (const auto& [memberTarget, memberVersion] : component) {
                if (memberTarget == target && memberVersion == version) continue;
                const auto [memberNamespace, memberBareVersion] =
                    xvm::parse_ns_version(memberVersion);
                push_candidate(candidates, {
                    .ns = memberNamespace,
                    .package = memberTarget,
                    .version = memberBareVersion,
                });
                const auto& memberData =
                    db.at(memberTarget).versions.at(memberVersion);
                if (const auto fromPath =
                        xvm::coordinate_from_payload_path(memberData.path)) {
                    push_candidate(candidates, *fromPath);
                }
            }
            auto owner = owner_from_candidates(
                candidates, target, version, storeRoot, metadata, trace);
            if (keep_owner(owner)) {
                related.try_emplace(
                    TargetVersion{target, version}, std::move(owner));
            }
        }
    }
    return related;
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
    PackageCatalog& catalog, bool allSubos,
    std::optional<std::string_view> canonicalFilter = std::nullopt,
    InventoryTrace* trace = nullptr) {
    detail::MetadataLookup metadata(catalog,
                                    std::string(inventory_platform()), trace);
    auto workspaces = detail::load_inventory_workspaces(allSubos);
    const auto db = Config::versions();
    const auto storeRoot = Config::global_data_dir() / "xpkgs";
    auto filteredCoordinates = detail::build_related_coordinates(
        db, canonicalFilter.value_or(std::string_view{}),
        storeRoot, metadata, trace);

    return detail::assemble_inventory(db, workspaces,
        storeRoot, metadata,
        /*includePayloadOnly=*/true, canonicalFilter, trace,
        &filteredCoordinates);
}

// One package's rows, without materialising the rest of the inventory. This is
// what `info` needs: whether the package is installed at all, and whether the
// exact version it selected is.
std::vector<InstalledPackageRecord> collect_package_inventory(
    PackageCatalog& catalog, const std::string& canonicalName,
    bool allSubos = true, InventoryTrace* trace = nullptr) {
    const auto parsed = parse_package_target(canonicalName);
    if (parsed.name.empty() || !parsed.version.empty()) return {};

    detail::MetadataLookup metadata(
        catalog, std::string(inventory_platform()), trace);
    const auto* found = metadata.by_identity(
        parsed.namespaceName, parsed.name);
    const auto storeRoot = found && !found->storeRoot.empty()
        ? found->storeRoot : Config::global_data_dir() / "xpkgs";
    const auto db = Config::versions();
    const auto workspaces = detail::load_inventory_workspaces(allSubos);
    auto related = detail::build_related_coordinates(
        db, canonicalName, storeRoot, metadata, trace,
        detail::CoordinateMatch::exact);
    std::erase_if(related, [&](const auto& item) {
        const auto& coordinate = item.second;
        return canonical_package_name(coordinate.ns, coordinate.package)
            != canonicalName;
    });
    std::map<std::string, InstalledPackageRecord> records;

    const auto ensure_record = [&](const xvm::InstallCoordinate& coordinate)
        -> InstalledPackageRecord& {
        auto [it, inserted] = records.try_emplace(coordinate.canonical());
        auto& record = it->second;
        if (inserted) {
            record.namespaceName = coordinate.ns;
            record.name = coordinate.package;
            record.canonicalName = canonical_package_name(
                coordinate.ns, coordinate.package);
            record.version = coordinate.version;
        }
        return record;
    };

    bool needsLegacyBareIdentity = false;
    const auto add_direct_version = [&](const std::string& version,
                                        bool allowLegacyBare) {
        const auto key = std::pair{parsed.name, version};
        if (related.contains(key)) return;
        const auto [versionNamespace, bareVersion] =
            xvm::parse_ns_version(version);
        if (versionNamespace != parsed.namespaceName
            && !(allowLegacyBare && versionNamespace.empty())) {
            if (versionNamespace.empty()) needsLegacyBareIdentity = true;
            return;
        }
        related.try_emplace(key, xvm::InstallCoordinate{
            .ns = parsed.namespaceName,
            .package = parsed.name,
            .version = bareVersion,
        });
    };
    const auto add_direct_workspace_versions = [&](const auto& workspace,
                                                   bool allowLegacyBare) {
        if (const auto installedIt = workspace.installed.find(parsed.name);
            installedIt != workspace.installed.end()) {
            for (const auto& version : installedIt->second) {
                add_direct_version(version, allowLegacyBare);
            }
        }
        if (const auto activeIt = workspace.active.find(parsed.name);
            activeIt != workspace.active.end()) {
            add_direct_version(activeIt->second, allowLegacyBare);
        }
    };

    for (const auto& workspace : workspaces) {
        add_direct_workspace_versions(
            workspace, parsed.namespaceName.empty());
    }
    if (needsLegacyBareIdentity && !parsed.namespaceName.empty()) {
        const auto* unique = metadata.by_short_name(parsed.name);
        if (unique && unique->namespaceName == parsed.namespaceName
            && unique->name == parsed.name) {
            for (const auto& workspace : workspaces) {
                add_direct_workspace_versions(
                    workspace, /*allowLegacyBare=*/true);
            }
        }
    }

    for (const auto& workspace : workspaces) {
        for (const auto& [targetVersion, coordinate] : related) {
            const auto& [target, version] = targetVersion;
            if (const auto installedIt = workspace.installed.find(target);
                installedIt != workspace.installed.end()
                && std::ranges::find(installedIt->second, version)
                    != installedIt->second.end()) {
                auto& record = ensure_record(coordinate);
                record.suboses.insert(workspace.name);
                record.inCurrentSubos |= workspace.current;
            }
            if (const auto activeIt = workspace.active.find(target);
                activeIt != workspace.active.end()
                && activeIt->second == version) {
                auto& record = ensure_record(coordinate);
                record.suboses.insert(workspace.name);
                record.inCurrentSubos |= workspace.current;
                record.active = true;
            }
        }
    }

    // Payload-only/data packages have no workspace target.  Probe the one
    // exact store directory selected by the already-resolved identity; never
    // enumerate sibling package roots.
    const auto packageDir = storeRoot
        / package_store_name(parsed.namespaceName, parsed.name);
    std::error_code ec;
    if (std::filesystem::is_directory(packageDir, ec)) {
        for (const auto& versionDir : platform::dir_entries(packageDir)) {
            if (!versionDir.is_directory(ec)) continue;
            detail::trace_payload_visit(trace, versionDir.path());
            const auto stamped = std::filesystem::is_regular_file(
                versionDir.path() / std::filesystem::path(kPayloadStampFile), ec);
            ec.clear();
            const auto legacyMarked = std::filesystem::is_regular_file(
                versionDir.path() / ".xim-installed", ec);
            if (!stamped && !legacyMarked) continue;
            ensure_record({.ns = parsed.namespaceName,
                           .package = parsed.name,
                           .version = versionDir.path().filename().string()});
        }
    }

    std::vector<InstalledPackageRecord> result;
    result.reserve(records.size());
    for (auto& [_, record] : records) {
        if (found) {
            record.description = found->description;
            record.programs = found->programs;
        }
        record.payloadPath = storeRoot
            / package_store_name(record.namespaceName, record.name)
            / record.version;
        detail::trace_payload_visit(trace, record.payloadPath);
        record.payloadPresent = payload_has_content(record.payloadPath);
        if (!record.payloadPresent) {
            record.degradedReason = "payload missing";
        } else if (!found) {
            record.degradedReason = "index entry unavailable";
        }
        result.push_back(std::move(record));
    }

    std::ranges::stable_sort(result, [](const auto& lhs, const auto& rhs) {
        return version_order::compare(lhs.version, rhs.version) > 0;
    });
    return result;
}

}  // namespace xlings::xim
