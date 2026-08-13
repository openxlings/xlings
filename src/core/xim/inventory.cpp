module xlings.core.xim.inventory;

import std;
import xlings.core.config;
import xlings.core.profile;
import xlings.core.version_order;
import xlings.core.xim.catalog;
import xlings.core.xim.install_state;
import xlings.core.xim.payload;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;
import xlings.core.xvm.owner;
import xlings.core.xvm.types;
import xlings.platform;

namespace xlings::xim {

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

void trace_payload_visit(InventoryTrace* trace,
                         const std::filesystem::path& versionDir) {
    if (trace != nullptr) trace->payloadVersionDirs.push_back(versionDir);
}

void push_candidate(std::vector<xvm::InstallCoordinate>& candidates,
                    xvm::InstallCoordinate candidate) {
    if (candidate.empty() || candidate.version.empty()) return;
    if (std::ranges::find(candidates, candidate) != candidates.end()) return;
    candidates.push_back(std::move(candidate));
}

xvm::InstallCoordinate direct_coordinate(const std::string& target,
                                         const std::string& version) {
    const auto [namespaceName, bareVersion] = xvm::parse_ns_version(version);
    return {.ns = namespaceName,
            .package = target,
            .version = bareVersion};
}

const xvm::VData* version_data(const xvm::VersionDB& db,
                               const TargetVersion& pair) {
    const auto info = db.find(pair.first);
    if (info == db.end()) return nullptr;
    const auto data = info->second.versions.find(pair.second);
    return data == info->second.versions.end() ? nullptr : &data->second;
}

SelectionPtr bounded_selection(const xvm::VersionDB& db, const TargetVersion& seed, SelectionCache& cache, xvm::BindingSelectionResolver& resolver, InventoryTrace* trace) {
    if (const auto cached = cache.find(seed); cached != cache.end()) {
        return cached->second;
    }

    const auto* seedData = version_data(db, seed);
    if (seedData == nullptr) {
        cache[seed] = nullptr;
        return nullptr;
    }
    const auto seedInfo = db.find(seed.first);
    if (seedInfo == db.end()) {
        cache[seed] = nullptr;
        return nullptr;
    }
    const bool hasCanonicalMetadata = seedData->bindingMembersDeclared
        || !seedData->bindingMembers.empty()
        || seedData->bindingHeadersDeclared
        || !seedData->bindingHeaders.empty()
        || !seedData->bindingIntegrityIssues.empty()
        || !seedData->bindingUnreadable.empty();
    if (!seedData->bindingGroup && !hasCanonicalMetadata
        && seedInfo->second.bindings.empty()) {
        cache[seed] = nullptr;
        return nullptr;
    }

    if (trace != nullptr) {
        trace->bindingSelections.push_back(
            seed.first + "@" + seed.second);
    }
    auto selected = resolver.resolve(seed.first, seed.second);
    if (trace != nullptr) {
        trace->legacyIncomingIndexBuilds =
            resolver.legacy_incoming_index_builds();
    }
    if (!selected) {
        cache[seed] = nullptr;
        return nullptr;
    }
    auto value = std::make_shared<BoundedSelection>();
    value->members = selected->members;
    value->source = selected->source;
    if (seedData->bindingGroup) {
        value->root = TargetVersion{
            seedData->bindingGroup->rootTarget,
            seedData->bindingGroup->rootVersion,
        };
    }

    for (const auto& [target, version] : value->members) {
        cache[{target, version}] = value;
    }
    return value;
}

bool legacy_root_in_selection(const xvm::VersionDB& db,
                              const BoundedSelection& selection,
                              const TargetVersion& candidate) {
    for (const auto& [peerTarget, peerVersion] : selection.members) {
        if (peerTarget == candidate.first) continue;
        const auto peer = db.find(peerTarget);
        if (peer == db.end()) continue;
        const auto binding = peer->second.bindings.find(candidate.first);
        if (binding == peer->second.bindings.end()) continue;
        const auto edge = binding->second.find(peerVersion);
        if (edge != binding->second.end()
            && edge->second == candidate.second) {
            return true;
        }
    }
    return false;
}

std::vector<xvm::InstallCoordinate> direct_owner_candidates_for(
    const xvm::VersionDB& db,
    const TargetVersion& pair) {
    std::vector<xvm::InstallCoordinate> candidates;
    const auto* data = version_data(db, pair);
    if (data && data->bindingGroup
        && !data->bindingGroup->providerVersion.empty()) {
        const auto provider = parse_package_target(
            data->bindingGroup->provider);
        push_candidate(candidates, {
            .ns = provider.namespaceName,
            .package = provider.name,
            .version = xvm::strip_namespace(
                data->bindingGroup->providerVersion),
        });
    }
    if (data) {
        if (auto fromPath = xvm::coordinate_from_payload_path(data->path)) {
            push_candidate(candidates, std::move(*fromPath));
        }
    }
    push_candidate(candidates, direct_coordinate(pair.first, pair.second));
    return candidates;
}

IncomingEdgeIndex build_incoming_edges(const xvm::VersionDB& db) {
    IncomingEdgeIndex incoming;
    for (const auto& [sourceTarget, sourceInfo] : db) {
        for (const auto& [destinationTarget, versions] : sourceInfo.bindings) {
            for (const auto& [sourceVersion, destinationVersion] : versions) {
                incoming[{destinationTarget, destinationVersion}]
                    .emplace_back(sourceTarget, sourceVersion);
            }
        }
    }
    return incoming;
}

std::vector<xvm::InstallCoordinate> shallow_owner_candidates_for(const xvm::VersionDB& db, const TargetVersion& pair, const IncomingEdgeIndex* incoming) {
    auto candidates = direct_owner_candidates_for(db, pair);
    const auto* data = version_data(db, pair);
    if (data && data->bindingGroup) {
        const TargetVersion root{
            data->bindingGroup->rootTarget,
            data->bindingGroup->rootVersion,
        };
        if (root != pair) {
            push_candidate(candidates, direct_coordinate(
                root.first, root.second));
            if (const auto* rootData = version_data(db, root)) {
                if (auto fromPath =
                        xvm::coordinate_from_payload_path(rootData->path)) {
                    push_candidate(candidates, std::move(*fromPath));
                }
            }
        }
        return candidates;
    }

    std::vector<TargetVersion> pending{pair};
    std::set<TargetVersion> visited;
    const auto reach = [&](const TargetVersion& peer) {
        push_candidate(candidates, direct_coordinate(peer.first, peer.second));
        if (const auto* peerData = version_data(db, peer)) {
            if (auto fromPath =
                    xvm::coordinate_from_payload_path(peerData->path)) {
                push_candidate(candidates, std::move(*fromPath));
            }
        }
        if (!visited.contains(peer)) pending.push_back(peer);
    };
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        if (incoming != nullptr) {
            if (const auto found = incoming->find(current);
                found != incoming->end()) {
                for (const auto& source : found->second) reach(source);
            }
        }
        const auto info = db.find(current.first);
        if (info == db.end()) continue;
        for (const auto& [peerTarget, versions] : info->second.bindings) {
            const auto edge = versions.find(current.second);
            if (edge == versions.end()) continue;
            reach(TargetVersion{peerTarget, edge->second});
        }
    }
    return candidates;
}

std::vector<xvm::InstallCoordinate> owner_candidates_for(const xvm::VersionDB& db, const TargetVersion& pair, SelectionCache& selectionCache, xvm::BindingSelectionResolver& resolver, InventoryTrace* trace) {
    auto candidates = direct_owner_candidates_for(db, pair);

    const auto selection = bounded_selection(
        db, pair, selectionCache, resolver, trace);
    if (!selection) return candidates;
    if (selection->source == xvm::BindingSource::ProviderGroup) {
        if (selection->root && *selection->root != pair) {
            push_candidate(candidates, direct_coordinate(
                selection->root->first, selection->root->second));
            if (const auto* rootData = version_data(db, *selection->root)) {
                if (auto fromPath =
                        xvm::coordinate_from_payload_path(rootData->path)) {
                    push_candidate(candidates, std::move(*fromPath));
                }
            }
        }
        return candidates;
    }

    for (const auto& [target, version] : selection->members) {
        const TargetVersion candidate{target, version};
        if (candidate == pair
            || !legacy_root_in_selection(db, *selection, candidate)) {
            continue;
        }
        push_candidate(candidates, direct_coordinate(target, version));
        if (const auto* rootData = version_data(db, candidate)) {
            if (auto fromPath =
                    xvm::coordinate_from_payload_path(rootData->path)) {
                push_candidate(candidates, std::move(*fromPath));
            }
        }
    }
    return candidates;
}

bool identity_matches(std::string_view namespaceName,
                      std::string_view name,
                      std::string_view filter,
                      CoordinateMatch match) {
    if (filter.empty()) return true;
    const auto canonical = canonical_package_name(namespaceName, name);
    if (match == CoordinateMatch::exact) return canonical == filter;
    return canonical.contains(filter) || name.contains(filter);
}

bool candidate_may_match(const xvm::InstallCoordinate& candidate,
                         std::string_view filter,
                         CoordinateMatch match) {
    return identity_matches(
        candidate.ns, candidate.package, filter, match);
}

OwnedCoordinate resolve_owner(std::span<const xvm::InstallCoordinate> candidates, const TargetVersion& pair, std::span<const std::filesystem::path> storeRoots, MetadataLookup& metadata, InventoryTrace* trace) {
    for (const auto& candidate : candidates) {
        if (const auto* found = metadata.by_identity(
                candidate.ns, candidate.package)) {
            return {.coordinate = candidate, .storeRoot = found->storeRoot};
        }
    }
    for (const auto& candidate : candidates) {
        for (const auto& root : storeRoots) {
            const auto versionDir = root
                / package_store_name(candidate.ns, candidate.package)
                / candidate.version;
            trace_payload_visit(trace, versionDir);
            std::error_code ec;
            if (std::filesystem::is_directory(versionDir, ec)) {
                return {.coordinate = candidate, .storeRoot = root};
            }
        }
    }
    if (const auto* unique = metadata.by_short_name(pair.first)) {
        const auto [_, bareVersion] = xvm::parse_ns_version(pair.second);
        return {
            .coordinate = {.ns = unique->namespaceName,
                           .package = unique->name,
                           .version = bareVersion},
            .storeRoot = unique->storeRoot,
        };
    }
    if (!candidates.empty()) return {.coordinate = candidates.front()};
    return {.coordinate = direct_coordinate(pair.first, pair.second)};
}

RelatedCoordinates build_owner_coordinates(const xvm::VersionDB& db, const std::set<TargetVersion>& requested, std::string_view filter, std::span<const std::filesystem::path> storeRoots, MetadataLookup& metadata, InventoryTrace* trace, CoordinateMatch match) {
    RelatedCoordinates related;
    SelectionCache selectionCache;
    xvm::BindingSelectionResolver bindingResolver{db};
    // Only a filtered query can skip a pair, so only a filtered query pays for
    // the reverse index the skip decision needs to be safe.
    std::optional<IncomingEdgeIndex> incoming;
    if (!filter.empty()) incoming = build_incoming_edges(db);
    for (const auto& pair : requested) {
        if (!filter.empty()) {
            const auto shallow = shallow_owner_candidates_for(
                db, pair, incoming ? &*incoming : nullptr);
            auto mayMatch = std::ranges::any_of(
                shallow, [&](const auto& candidate) {
                    return candidate_may_match(candidate, filter, match);
                });
            if (!mayMatch) {
                const auto* unique = metadata.by_short_name(pair.first);
                mayMatch = unique != nullptr
                    && identity_matches(unique->namespaceName, unique->name,
                                        filter, match);
            }
            if (!mayMatch) continue;
        }
        auto candidates = owner_candidates_for(
            db, pair, selectionCache, bindingResolver, trace);
        auto owner = resolve_owner(
            candidates, pair, storeRoots, metadata, trace);
        if (!identity_matches(owner.coordinate.ns, owner.coordinate.package,
                              filter, match)) {
            continue;
        }
        related.emplace(pair, std::move(owner));
    }
    return related;
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

std::set<TargetVersion> requested_pairs(
    std::span<const InventoryWorkspace> workspaces) {
    std::set<TargetVersion> requested;
    for (const auto& workspace : workspaces) {
        for (const auto& [target, versions] : workspace.installed) {
            for (const auto& version : versions) {
                requested.emplace(target, version);
            }
        }
        for (const auto& [target, version] : workspace.active) {
            requested.emplace(target, version);
        }
    }
    return requested;
}

std::vector<std::filesystem::path> inventory_store_roots() {
    std::vector<std::filesystem::path> roots;
    if (Config::has_project_config()) {
        const auto projectData = Config::project_data_dir();
        if (!projectData.empty()) roots.push_back(projectData / "xpkgs");
    }
    const auto global = Config::global_data_dir() / "xpkgs";
    if (std::ranges::find(roots, global) == roots.end()) {
        roots.push_back(global);
    }
    return roots;
}

std::vector<InstalledPackageRecord> assemble_inventory(const xvm::VersionDB& db, std::span<const InventoryWorkspace> workspaces, std::span<const std::filesystem::path> storeRoots, MetadataLookup& metadata, std::string_view filter, CoordinateMatch match, InventoryTrace* trace, bool includePayloadOnly) {
    const auto requested = requested_pairs(workspaces);
    const auto related = build_owner_coordinates(
        db, requested, filter, storeRoots, metadata, trace, match);
    // Built once for the whole inventory rather than per record: the query
    // contract says local commands answer immediately, and one pass over the
    // DB is what keeps `info` proportional to the answer instead of the home.
    const LedgerIndex ledgerIndex(db, Config::paths().homeDir.string());
    std::map<std::string, InstalledPackageRecord> records;
    std::map<std::string, std::filesystem::path> catalogFallbacks;
    std::set<std::string> workspaceDerived;
    std::set<std::string> stampDerived;

    const auto ensure_record = [&](const OwnedCoordinate& owned)
        -> InstalledPackageRecord& {
        const auto& coordinate = owned.coordinate;
        auto [it, inserted] = records.try_emplace(coordinate.canonical());
        auto& record = it->second;
        if (inserted) {
            record.namespaceName = coordinate.ns;
            record.name = coordinate.package;
            record.canonicalName = canonical_package_name(
                coordinate.ns, coordinate.package);
            record.version = coordinate.version;
        }
        if (!owned.storeRoot.empty()) {
            catalogFallbacks.try_emplace(coordinate.canonical(),
                owned.storeRoot
                / package_store_name(coordinate.ns, coordinate.package)
                / coordinate.version);
        }
        return record;
    };

    for (const auto& workspace : workspaces) {
        for (const auto& [target, versions] : workspace.installed) {
            for (const auto& version : versions) {
                const auto found = related.find({target, version});
                if (found == related.end()) continue;
                auto& record = ensure_record(found->second);
                workspaceDerived.insert(found->second.coordinate.canonical());
                record.suboses.insert(workspace.name);
                record.inCurrentSubos |= workspace.current;
            }
        }
        for (const auto& [target, version] : workspace.active) {
            const auto found = related.find({target, version});
            if (found == related.end()) continue;
            auto& record = ensure_record(found->second);
            workspaceDerived.insert(found->second.coordinate.canonical());
            record.suboses.insert(workspace.name);
            record.inCurrentSubos |= workspace.current;
            record.active = true;
        }
    }

    // Project comes first. If both stores contain the same canonical stamp,
    // the project-scoped payload is the one the catalog also prefers.
    if (includePayloadOnly) for (const auto& storeRoot : storeRoots) {
        std::error_code ec;
        if (!std::filesystem::is_directory(storeRoot, ec)) continue;
        for (const auto& packageDir : platform::dir_entries(storeRoot)) {
            if (!packageDir.is_directory(ec)) continue;
            const auto storeName = packageDir.path().filename().string();
            const auto [namespaceName, name] =
                identity_from_store_name(storeName);
            if (!identity_matches(namespaceName, name, filter, match)) continue;
            for (const auto& versionDir :
                    platform::dir_entries(packageDir.path())) {
                if (!versionDir.is_directory(ec)) continue;
                trace_payload_visit(trace, versionDir.path());
                const auto stamped = std::filesystem::is_regular_file(
                    versionDir.path()
                        / std::filesystem::path(kPayloadStampFile), ec);
                ec.clear();
                const auto legacyMarked = std::filesystem::is_regular_file(
                    versionDir.path() / ".xim-installed", ec);
                if (!stamped && !legacyMarked) continue;
                OwnedCoordinate owned{
                    .coordinate = {.ns = namespaceName,
                                   .package = name,
                                   .version = versionDir.path()
                                                  .filename().string()},
                    .storeRoot = storeRoot,
                };
                auto& record = ensure_record(owned);
                if (stampDerived.insert(
                        owned.coordinate.canonical()).second) {
                    record.payloadPath = versionDir.path();
                }
            }
        }
    }

    std::vector<InstalledPackageRecord> result;
    result.reserve(records.size());
    for (auto& [coordinate, record] : records) {
        const auto* found = workspaceDerived.contains(coordinate)
            ? metadata.load_details(record.namespaceName, record.name)
            : metadata.load_stamped_details(
                  record.namespaceName, record.name);
        if (found) {
            record.namespaceName = found->namespaceName;
            record.name = found->name;
            record.canonicalName = found->canonicalName;
            record.description = found->description;
            record.programs = found->programs;
            if (!found->storeRoot.empty()) {
                catalogFallbacks.try_emplace(coordinate,
                    found->storeRoot
                    / package_store_name(record.namespaceName, record.name)
                    / record.version);
            }
        }
        if (record.payloadPath.empty()) {
            for (const auto& root : storeRoots) {
                const auto candidate = root
                    / package_store_name(record.namespaceName, record.name)
                    / record.version;
                trace_payload_visit(trace, candidate);
                std::error_code ec;
                if (std::filesystem::is_directory(candidate, ec)) {
                    record.payloadPath = candidate;
                    break;
                }
            }
        }
        if (record.payloadPath.empty()) {
            if (const auto fallback = catalogFallbacks.find(coordinate);
                fallback != catalogFallbacks.end()) {
                record.payloadPath = fallback->second;
            }
        }
        if (record.payloadPath.empty() && !storeRoots.empty()) {
            record.payloadPath = storeRoots.back()
                / package_store_name(record.namespaceName, record.name)
                / record.version;
        }
        trace_payload_visit(trace, record.payloadPath);
        record.payloadPresent = payload_has_content(record.payloadPath);
        // Asked here rather than derived from the two flags above, because the
        // interesting cases are exactly the ones those flags agree on: a
        // stamped payload with no ledger entry has `payloadPresent` true and an
        // index entry, and is unusable. One answerer, asked by everyone.
        if (const auto state = installation_state(
                ledgerIndex, record.namespaceName, record.name,
                record.version, record.payloadPath);
            state.is_incomplete()) {
            record.incomplete = true;
            record.incompleteReason = state.reason;
        }
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

std::vector<InstalledPackageRecord> assemble_inventory(
    const xvm::VersionDB& db,
    std::span<const InventoryWorkspace> workspaces,
    const std::filesystem::path& storeRoot,
    MetadataLookup& metadata,
    bool includePayloadOnly) {
    const std::array roots{storeRoot};
    return assemble_inventory(
        db, workspaces, roots, metadata, {}, CoordinateMatch::contains,
        nullptr, includePayloadOnly);
}

}

std::vector<InstalledPackageRecord> collect_inventory_impl(
    PackageCatalog& catalog, bool allSubos,
    std::optional<std::string_view> filter,
    detail::CoordinateMatch match,
    InventoryTrace* trace) {
    detail::MetadataLookup metadata(catalog, trace);
    const auto workspaces = detail::load_inventory_workspaces(allSubos);
    const auto storeRoots = detail::inventory_store_roots();
    return detail::assemble_inventory(
        Config::versions(), workspaces, storeRoots, metadata,
        filter.value_or(std::string_view{}), match, trace);
}

std::vector<InstalledPackageRecord> collect_inventory(PackageCatalog& catalog, bool allSubos, std::optional<std::string_view> canonicalFilter, InventoryTrace* trace) {
    return collect_inventory_impl(
        catalog, allSubos, canonicalFilter,
        detail::CoordinateMatch::contains, trace);
}

std::vector<InstalledPackageRecord> collect_package_inventory(PackageCatalog& catalog, const std::string& canonicalName, bool allSubos, InventoryTrace* trace) {
    const auto parsed = parse_package_target(canonicalName);
    if (parsed.name.empty() || !parsed.version.empty()) return {};
    return collect_inventory_impl(
        catalog, allSubos, std::string_view{canonicalName},
        detail::CoordinateMatch::exact, trace);
}

}
