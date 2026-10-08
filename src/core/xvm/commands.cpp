module xlings.core.xvm.commands;

import std;
import xlings.core.config;
import xlings.core.home;
import xlings.core.home.layers;
import xlings.core.log;
import xlings.core.diag;
import xlings.core.version_order;
import xlings.core.palette;
import xlings.subos.manifest;
import xlings.platform;
import xlings.runtime;
import xlings.libs.json;
import xlings.core.semver;
import xlings.core.entry_binary;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.lock;
import xlings.core.xvm.bindings;
import xlings.core.xvm.inspect;
import xlings.core.xvm.errors;
import xlings.core.xvm.owner;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.shim;
import xlings.core.xvm.materialize;
import xlings.i18n;

namespace xlings::xvm {

void create_link_(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
#if defined(_WIN32)
    if (fs::is_directory(src)) {
        // Use directory junction on Windows (no admin required)
        platform::create_directory_link(dst.string(), src.string());
    } else {
        fs::create_hard_link(src, dst, ec);
    }
#else
    fs::create_symlink(src, dst, ec);
#endif
    if (ec) log::warn("[xvm] link failed: {} -> {}",
                      Config::display_path(dst), Config::display_path(src));
}

fs::path header_destination_(const HeaderAsset& asset,
                             const fs::path& sysroot_include) {
    return asset.destinationPrefix.empty()
        ? sysroot_include
        : sysroot_include / fs::path(asset.destinationPrefix);
}

bool sysroot_source_is_local_(const fs::path& src) {
    std::error_code ec;
    const auto canon = fs::weakly_canonical(src, ec);
    const auto& probe = ec ? src : canon;
    const auto under = [&](const fs::path& root) {
        if (root.empty()) return false;
        std::error_code rec;
        auto canonRoot = fs::weakly_canonical(root, rec);
        const auto& r = rec ? root : canonRoot;
        auto a = probe.string(), b = r.string();
        return a == b || a.starts_with(b + static_cast<char>(fs::path::preferred_separator))
            || a.starts_with(b + "/");
    };
    const auto& p = Config::paths();
    if (under(p.dataDir) || under(p.homeDir)) return true;
    const auto versions = Config::versions();
    for (const auto& [target, info] : versions)
        for (const auto& [version, data] : info.versions)
            if (!data.sourceHome.empty() && home::layers::owns_source_payload(data.sourceHome, src)) return true;
    return false;
}

namespace {
fs::path materialization_root_(const fs::path& destination, std::span<const materialize::AssetClaim> claims = {}) {
    const auto scope = fs::absolute(Config::paths().subosDir).lexically_normal();
    const auto path = fs::absolute(destination).lexically_normal();
    const auto relative = path.lexically_relative(scope);
    if (!relative.empty() && *relative.begin() != "..") return scope;
    // Standalone callers supply an isolated sysroot and explicit old claims.
    auto root = path.parent_path();
    for (const auto& claim : claims) {
        const auto asset = fs::absolute(claim.destination).lexically_normal();
        while (root != root.parent_path()) {
            const auto from = asset.lexically_relative(root);
            if (!from.empty() && *from.begin() != "..") break;
            root = root.parent_path();
        }
        if (root == asset) root = root.parent_path();
    }
    return root;
}
MaterializationResult apply_assets_(std::vector<materialize::AssetChange> changes,
    const fs::path& root, std::span<const materialize::AssetClaim> supplied) {
    std::vector<materialize::AssetClaim> claims(supplied.begin(), supplied.end());
    if (supplied.empty()) {
        auto recorded = materialize::collect_claims(Config::versions(), Config::workspace_installed(),
            Config::paths().subosDir, Config::paths().libDir, Config::paths().homeDir.string());
        if (!recorded) return std::unexpected(recorded.error());
        claims = std::move(*recorded);
    }
    auto prepared = materialize::preflight_materialization(changes, claims, root);
    if (!prepared) return std::unexpected(prepared.error());
    auto applied = prepared->execute();
    if (!applied) return std::unexpected(applied.error());
    auto proofs = applied->proofs();
    auto committed = applied->commit();
    if (!committed) return std::unexpected(committed.error());
    return proofs;
}
}

MaterializationResult install_headers(const HeaderAsset& asset, const fs::path& includeRoot,
    std::span<const materialize::AssetClaim> claims) {
    if (!asset.sourceDir.empty() && !sysroot_source_is_local_(asset.sourceDir)) {
        log::warn("[xvm] linking headers from outside this home: {}", Config::display_path(asset.sourceDir));
    }
    std::vector<materialize::AssetChange> changes;
    auto expanded = materialize::append_headers(changes, asset, includeRoot);
    if (!expanded) return std::unexpected(expanded.error());
    return apply_assets_(std::move(changes), materialization_root_(includeRoot, claims), claims);
}
MaterializationResult install_headers(const std::string& source, const fs::path& includeRoot,
    std::span<const materialize::AssetClaim> claims) {
    return install_headers(HeaderAsset{source, ""}, includeRoot, claims);
}
std::expected<void, std::string> remove_headers(const HeaderAsset& asset, const fs::path& includeRoot,
    std::span<const materialize::AssetClaim> claims) {
    std::vector<materialize::AssetChange> changes;
    auto expanded = materialize::append_headers(changes, asset, includeRoot, true);
    if (!expanded) return std::unexpected(expanded.error());
    auto applied = apply_assets_(std::move(changes), materialization_root_(includeRoot, claims), claims);
    if (!applied) return std::unexpected(applied.error());
    return {};
}
std::expected<void, std::string> remove_headers(const std::string& source, const fs::path& includeRoot,
    std::span<const materialize::AssetClaim> claims) {
    return remove_headers(HeaderAsset{source, ""}, includeRoot, claims);
}
MaterializationResult place_asset(const std::string& source, const fs::path& destination,
    std::span<const materialize::AssetClaim> claims) {
    if (source.empty() || destination.empty()) return std::vector<materialize::Proof>{};
    return apply_assets_({{source, destination, false}}, materialization_root_(destination, claims), claims);
}
std::expected<void, std::string> remove_asset(const fs::path& destination,
    std::span<const materialize::AssetClaim> claims) {
    if (destination.empty()) return {};
    auto removed = apply_assets_({{{}, destination, true}}, materialization_root_(destination, claims), claims);
    if (!removed) return std::unexpected(removed.error());
    return {};
}

void prune_empty_asset_dirs(const fs::path& absolute,
                            const fs::path& subosRoot) {
    std::error_code ec;
    const auto relativeToRoot = fs::relative(absolute, subosRoot, ec);
    if (ec || relativeToRoot.empty()) return;
    // Outside the subos entirely -- `fs::relative` climbs out with "..", and a
    // path that has to climb out is not one whose parents we own.
    const auto relativeString = relativeToRoot.generic_string();
    if (relativeString == ".." || relativeString.starts_with("../")) return;

    // Component count by walking parents rather than by iterating the path:
    // libc++ gives the path iterators only what C++20 requires, and both the
    // range-for and `std::distance` spellings fail to compile there.
    const auto components = [](fs::path path) {
        std::size_t count = 0;
        while (!path.empty() && path != path.parent_path()) {
            ++count;
            path = path.parent_path();
        }
        return count;
    };

    auto relative = relativeToRoot.parent_path();
    while (components(relative) >= 3) {
        std::error_code rmEc;
        const auto directory = subosRoot / relative;
        if (!fs::is_directory(fs::symlink_status(directory, rmEc)) || rmEc ||
            !platform::remove_empty_directory(directory)) break;
        relative = relative.parent_path();
    }
}

std::expected<void, std::string> reclaim_declared_assets(const fs::path& subosDir,
    const fs::path&, const std::set<std::string>& destinations,
    const VersionDB& db, const Workspace& activeAfter,
    std::span<const materialize::AssetClaim> claims) {
    std::vector<materialize::AssetChange> changes;
    for (const auto& destination : destinations) {
        bool put = false;
        for (const auto& [target, version] : activeAfter) {
            const auto placement = file_placement(db, target, version, Config::paths().homeDir.string());
            if (placement.destination == destination && !placement.empty()) {
                changes.push_back({placement.source, subosDir / destination, false});
                put = true;
            }
        }
        if (!put) changes.push_back({{}, subosDir / destination, true});
    }
    auto result = apply_assets_(std::move(changes), subosDir, claims);
    if (!result) return std::unexpected(result.error());
    return {};
}
MaterializationResult place_library(const std::string& source, const std::string& name,
    const fs::path& sysroot_lib) {
    if (name.empty()) return std::vector<materialize::Proof>{};
    return place_asset(source, sysroot_lib / name);
}
std::expected<void, std::string> remove_library(const std::string& name, const fs::path& sysroot_lib) {
    if (name.empty()) return {};
    return remove_asset(sysroot_lib / name);
}

bool runtime_activation_refused_(const VersionDB& db,
                                 const std::string& target,
                                 const std::string& requestedVersion) {
    namespace mf = xlings::subos::manifest;

    const auto doc = mf::read_document(Config::subos_scope().root);
    if (!doc) return false;
    const auto info = mf::parse(*doc);
    if (!mf::is_binding(info.runtime)
        || target != mf::binding_name(info.runtime)) {
        return false;
    }

    const auto it = db.find(target);
    if (it == db.end() || it->second.versions.empty()) return false;
    const auto resolved = requestedVersion == "latest"
        ? pick_highest_version(it->second.versions)
        : match_version(db, target, requestedVersion);
    if (resolved.empty()) return false;

    bool payloadExists = false;
    if (const auto* data = get_vdata(db, target, resolved);
        data && !data->path.empty()) {
        std::error_code ec;
        const auto expanded = expand_path(
            data->path, Config::paths().homeDir.string());
        payloadExists = fs::exists(expanded, ec) && !ec;
    }

    const auto mismatch = mf::check_runtime_activation(
        info, resolved, payloadExists);
    if (!mismatch) return false;

    const auto prospective = mismatch->active.empty()
        ? std::format("{}@{}", target, resolved) : mismatch->active;
    if (mismatch->payloadMissing
        && prospective == mismatch->declared) {
        log::error("[xlings:use] runtime activation refused: {} has no "
                   "payload", mismatch->declared);
        log::error("  nothing was changed");
        // `install` has no `--force`; the parser rejects it. A registered
        // version whose payload is gone is exactly the state the installer
        // re-runs the install hook for, so the plain install IS the reinstall.
        log::error("  hint: put the payload back with `xlings install {}`",
                   mismatch->declared);
        return true;
    }

    log::error("[xlings:use] runtime activation refused: this SubOS "
               "declares {}, but the requested runtime is {}",
               mismatch->declared, prospective);
    log::error("  nothing was changed");
    // Two ways out, and both stay inside this SubOS. Sending the user to
    // `subos new` implies neither exists, which was never true: activating the
    // DECLARED version is exactly what this guard permits.
    log::error("  hint: adopt what this SubOS runs -- `xlings self doctor --fix`");
    log::error("        or migrate to it -- `xlings install {}` then "
               "`xlings use {} {}`",
               mismatch->declared, target, mf::binding_version(mismatch->declared));
    return true;
}

int cmd_use(const std::string& target, const std::string& version, EventStream& stream, bool strict) {
    // Serialize against any other xlings mutating this home, then re-read
    // state under the lock: Config loaded it at process start, outside the
    // lock, so acting on that snapshot is how two commands lose each other's
    // work. See xvm/lock.cppm.
    auto stateLock = xvm::acquire_state_lock(Config::paths().homeDir);
    if (!stateLock) {
        log::error("{}", stateLock.error());
        return 1;
    }
    Config::reload_state();

    if (runtime_activation_refused_(Config::versions(), target, version)) {
        return 1;
    }

    // Self-heal a dangling legacy edge before planning anything.
    //
    // State written by <= 0.4.69 can carry a pairwise edge pointing at a
    // version that is not registered. Such an edge describes no member
    // anyone could switch to -- it can only make the release unresolvable --
    // so dropping it needs no guessing, which is exactly why it does not
    // need the user's permission either.
    //
    // Until now it was reported by doctor and only repaired by
    // `doctor --fix`, which meant an upgraded user hit a refusal from `use`
    // with no idea that a second command existed. Repairing it here makes
    // the upgrade what it was supposed to be: silent. doctor keeps the
    // report and the flag for anyone inspecting rather than switching.
    if (auto pruning = plan_dangling_edge_pruning(Config::versions());
        !pruning.empty()) {
        auto& mutableDb = Config::versions_mut();
        const auto dropped =
            apply_dangling_edge_pruning(mutableDb, pruning);
        if (dropped > 0) {
            Config::save_versions();
            log::debug("[xvm] pruned {} dangling binding edge(s) written by "
                       "an older xlings", dropped);
        }
    }

    auto db = Config::versions();
    auto& p  = Config::paths();
    const auto versionStatePath = &Config::versions_mut() == &Config::global_versions()
        ? p.homeDir / ".xlings.json" : Config::project_state_path();
    const auto workspaceStatePath = Config::workspace_config_path(false);

    std::optional<home::layers::BorrowPlan> borrowed;
    std::string resolved;
    if (has_target(db, target)) {
        auto owned = db.at(target);
        std::erase_if(owned.versions, [](const auto& entry) { return !entry.second.sourceHome.empty(); });
        const VersionDB preferred{{target, std::move(owned)}};
        resolved = version == "latest" ? pick_highest_version(preferred.at(target).versions)
                                       : match_version(preferred, target, version);
        if (resolved.empty()) resolved = version == "latest" ? pick_highest_version(db.at(target).versions)
                                                            : match_version(db, target, version);
    }
    const auto* existing = resolved.empty() ? nullptr : get_vdata(db, target, resolved);
    if (resolved.empty() || (existing && !existing->sourceHome.empty())) {
        auto declared = home::read_system_layer();
        if (!declared) { log::error("{}", declared.error()); return 1; }
        if (*declared) {
            auto layer = home::layers::read_source_snapshot(**declared);
            if (!layer) { log::error("{}", layer.error()); return 1; }
            if (existing && existing->sourceHome != layer->sourceHome.string()) {
                log::error("{}: borrowed registration belongs to a different system layer", target);
                return 1;
            }
            const auto chosen = has_target(layer->versions, target)
                ? (version == "latest" ? pick_highest_version(layer->versions.at(target).versions)
                                        : match_version(layer->versions, target, version))
                : std::string{};
            if (!chosen.empty()) {
                auto closure = home::layers::plan_borrow(*layer, db, target, chosen);
                if (!closure) { log::error("{}", closure.error()); return 1; }
                borrowed = std::move(*closure);
                resolved = chosen;
                for (const auto& [name, imported] : borrowed->registrations) {
                    auto& info = db[name];
                    info.type = imported.type;
                    info.filename = imported.filename;
                    for (const auto& [key, data] : imported.versions) info.versions[key] = data;
                    for (const auto& [peer, edges] : imported.bindings)
                        for (const auto& [key, linked] : edges) info.bindings[peer][key] = linked;
                }
            } else if (existing) {
                log::error("{}: borrowed release is no longer registered by its source layer", target);
                return 1;
            }
        } else if (existing) {
            log::error("{}: the declared system layer for this borrowed release is missing", target);
            return 1;
        }
    }
    if (resolved.empty()) {
        if (!has_target(db, target)) {
            log::error("[xlings:use] '{}' not found in version database", target);
            log::error("  hint: install it first with `xlings install {}`", target);
        } else {
            log::error("version '{}' not found for '{}'", version, target);
            std::string available;
            for (const auto& key : get_all_versions(db, target)) {
                if (!available.empty()) available += ", ";
                available += key;
            }
            if (!available.empty()) log::error("  available: {}", available);
        }
        return 1;
    }
    if (runtime_activation_refused_(db, target, resolved)) return 1;

    log::debug("fuzzy version match: {} -> {}", version, resolved);

    // A version registered somewhere in this home is not a version this subos
    // can use.
    //
    // `use` used to opt the current subos in silently whenever the payload
    // existed anywhere (auto-add, 0.4.19+), on the reasoning that the payload
    // is shared so activation is free. That is true of a self-contained
    // package and false of everything with a dependency: gcc's glibc is not a
    // member of gcc's release, and the versions DB records no dependency
    // information at all, so activating gcc here activated exactly gcc. The
    // result reported success, put a working `g++` on PATH, printed the right
    // `-print-sysroot` -- and could not compile, because `usr/include` was
    // empty. Nothing said so.
    //
    // Owned payloads still require install's scope opt-in. The system-layer
    // branch above is a separate proof: it validated every recorded runtime
    // dependency and plans their views together, so it can opt that complete
    // borrowed closure in without running or inheriting another scope's config.
    if (!borrowed && filter_to_subos_installed_(target, {resolved}).empty()) {
        const auto origin = Config::version_origin(target);
        // The package that records `target@resolved`, if a record proves one;
        // `target` itself is a program name and may not be installable.
        std::string installCoordinate;
        {
            const auto dbNow = Config::versions();
            if (auto owner = recorded_owner(dbNow, target, resolved)) {
                installCoordinate = owner->canonical();
            }
        }
        diag::emit(not_in_subos({
            .target            = target,
            .subos             = Config::subos_scope().name,
            .suggestedVersion  = resolved,
            .source            = origin.source,
            .fromProject       = origin.fromProjectManifest,
            .nothingChanged    = true,
            .installCoordinate = std::move(installCoordinate),
        }));
        return 1;
    }

    // Resolve the whole release before touching anything.
    //
    // This used to walk the binding edges by hand, keyed by target rather
    // than by (target, version), and without checking that what it reached
    // actually existed. A stale edge would take it to a version with no
    // VData, which it then wrote into the active workspace -- the shim
    // failed later, far from the command that caused it. Header and library
    // switching ran *before* that walk and only for the entry target, so a
    // failure part-way through left the sysroot holding one release and the
    // workspace claiming another.
    //
    // resolve_binding_selection validates the whole group and fails closed.
    // Running it first means a bad group costs the user an error message
    // instead of a half-switched toolchain.
    auto workspace = Config::effective_workspace();
    auto plan = plan_use_switch(db, workspace, target, resolved,
                                Config::paths().homeDir.string());
    if (!plan) {
        log::error("{}", render(plan.error(), true));
        return 1;
    }
    if (borrowed) {
        // Each package release is planned against the SAME outgoing workspace.
        // Dependencies are materialized as well as the requested release.
        for (const auto& [name, key] : borrowed->members) {
            if (plan->members.contains(name)) continue;
            auto dependency = plan_use_switch(db, workspace, name, key, p.homeDir.string());
            if (!dependency) { log::error("{}", render(dependency.error(), true)); return 1; }
            for (const auto& [member, memberVersion] : dependency->members) {
                const auto found = plan->members.find(member);
                if (found != plan->members.end() && found->second != memberVersion) {
                    log::error("{}: contradictory layer closure switch", member);
                    return 1;
                }
                plan->members[member] = memberVersion;
            }
            plan->switches.insert(plan->switches.end(), dependency->switches.begin(), dependency->switches.end());
            plan->removeHeaders.insert(plan->removeHeaders.end(), dependency->removeHeaders.begin(), dependency->removeHeaders.end());
            plan->installHeaders.insert(plan->installHeaders.end(), dependency->installHeaders.begin(), dependency->installHeaders.end());
            plan->stranded.insert(plan->stranded.end(), dependency->stranded.begin(), dependency->stranded.end());
            plan->reclaimFiles.insert(dependency->reclaimFiles.begin(), dependency->reclaimFiles.end());
        }
        // Refuse unreadable local authorities before persisting imported metadata.
        auto versionDocument = home::read_json_for_update(versionStatePath);
        auto scopeDocument = home::read_json_for_update(workspaceStatePath);
        if (!versionDocument || !scopeDocument) {
            log::error("{}", !versionDocument ? versionDocument.error() : scopeDocument.error());
            return 1;
        }
    }
    const auto& to_switch = plan->members;

    // A program the outgoing release had and this one does not keeps
    // resolving to the release being left -- see StrandedMember. `--strict`
    // is for callers that would rather not switch at all than end up holding
    // two releases, and it has to refuse HERE, while nothing has moved yet.
    //
    // `stranded` only ever holds members of the SAME package (the planner
    // sorts a package switch into retainedByOldPackage instead), so this can
    // no longer refuse a move between two distributions of one tool -- a
    // refusal the user could not have satisfied, since the two packages have
    // no name in common to move first.
    //
    // Always lists every entry, verbose or not: this is the error path, and
    // the reason for a refusal is not a detail.
    if (strict && !plan->stranded.empty()) {
        log::error("[xlings:use] --strict: not switching {} to {}",
                   target, resolved);
        log::error("  {} name(s) would stay on the old release:",
                   plan->stranded.size());
        for (const auto& member : plan->stranded) {
            if (member.kind == "program") {
                log::error("    {} (still {})", member.target, member.version);
            } else {
                log::error("    {} (still {}, {})", member.target,
                           member.version, member.kind);
            }
        }
        log::error("  hint: drop --strict, or move each one first");
        return 1;
    }

    // Prepare the entire new projection before changing either authority.
    const auto previousDb = Config::versions_mut();
    const auto previousActive = Config::workspace();
    const auto previousInstalled = Config::workspace_installed();
    auto versionDocument = home::read_json_for_update(versionStatePath);
    auto workspaceDocument = home::read_json_for_update(workspaceStatePath);
    if (!versionDocument || !workspaceDocument) {
        log::error("{}", !versionDocument ? versionDocument.error() : workspaceDocument.error());
        return 1;
    }
    auto candidateDb = previousDb;
    if (borrowed) {
        for (const auto& [name, imported] : borrowed->registrations) {
            auto& info = candidateDb[name];
            info.type = imported.type;
            info.filename = imported.filename;
            for (const auto& [key, data] : imported.versions) info.versions[key] = data;
            for (const auto& [peer, edges] : imported.bindings)
                for (const auto& [key, linked] : edges) info.bindings[peer][key] = linked;
        }
    }
    auto candidateActive = previousActive;
    auto candidateInstalled = previousInstalled;
    for (const auto& [name, key] : to_switch) {
        candidateActive[name] = key;
        auto& installed = candidateInstalled[name];
        if (std::ranges::find(installed, key) == installed.end()) installed.push_back(key);
    }
    for (auto& [name, keys] : candidateInstalled) {
        std::ranges::sort(keys);
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    }
    for (const auto& [name, key] : plan->reclaimFiles) {
        const auto found = candidateActive.find(name);
        if (found != candidateActive.end() && found->second == key) candidateActive.erase(found);
    }
    auto oldClaims = materialize::collect_claims(Config::versions(), previousInstalled,
        p.subosDir, p.libDir, p.homeDir.string());
    reclaim_conflicting_file_bindings(candidateDb, candidateActive, to_switch, p.homeDir.string());
    WorkspaceInstalled selected;
    for (const auto& [name, key] : candidateActive) selected[name] = {key};
    auto desired = materialize::collect_claims(db, selected, p.subosDir, p.libDir, p.homeDir.string(),
        materialize::ClaimSource::Present);
    if (!oldClaims || !desired) {
        log::error("{}", !oldClaims ? oldClaims.error() : desired.error());
        return 1;
    }
    auto obsolete = materialize::obsolete_assets(*oldClaims, *desired);
    if (!obsolete) { log::error("{}", obsolete.error()); return 1; }
    auto changes = std::move(*obsolete);
    for (const auto& claim : *desired) {
        changes.push_back({claim.source, claim.destination, false});
    }
    auto prepared = materialize::preflight_materialization(changes, *oldClaims, p.subosDir);
    if (!prepared) { log::error("{}", prepared.error()); return 1; }
    auto applied = prepared->execute();
    if (!applied) { log::error("{}", applied.error()); return 1; }
    bool metadataTouched = false;
    const auto restore = [&](const std::string& reason) {
        log::error("{}", reason);
        auto restored = applied->rollback();
        if (!restored) log::error("materializer recovery: {}", restored.error());
        Config::versions_mut() = previousDb;
        Config::workspace_mut() = previousActive;
        Config::workspace_installed_mut() = previousInstalled;
        if (metadataTouched) {
            try {
                if (borrowed) platform::write_file_atomic(versionStatePath.string(), versionDocument->dump(2));
                platform::write_file_atomic(workspaceStatePath.string(), workspaceDocument->dump(2));
            } catch (const std::exception& error) {
                log::error("activation metadata recovery failed: {}", error.what());
                return;
            }
            const auto sync = xself::sync_shim_tables();
            if (!sync.root_error.empty()) log::error("previous root projection recovery failed: {}", sync.root_error);
        }
    };
    Config::versions_mut() = candidateDb;
    Config::workspace_mut() = candidateActive;
    Config::workspace_installed_mut() = candidateInstalled;
    metadataTouched = true;
    try {
        if (borrowed) Config::save_versions();
        Config::save_workspace();
        auto recorded = home::read_json_for_update(workspaceStatePath);
        if (!recorded || !recorded->contains("workspace")) {
            restore("activation workspace could not be persisted");
            return 1;
        }
        const auto actual = subos_workspace_from_json((*recorded)["workspace"]);
        if (actual.active != candidateActive || actual.installed != candidateInstalled) {
            restore("activation workspace write did not record the planned state");
            return 1;
        }
        if (borrowed) {
            auto recordedDb = home::read_json_for_update(versionStatePath);
            if (!recordedDb || !recordedDb->contains("versions") ||
                (*recordedDb)["versions"] != versions_to_json(candidateDb)) {
                restore("borrowed layer metadata could not be persisted");
                return 1;
            }
        }
    } catch (const std::exception& error) {
        restore(std::string("activation metadata write failed: ") + error.what());
        return 1;
    }

    // The routing table follows the workspace that was just written.
    //
    // This used to be a loop that created one shim per switched target and
    // then mirrored each into the global bin. Two things were wrong with it.
    //
    // The mirror wrote a PROJECT-scope decision into the GLOBAL bin, where
    // nothing recorded why the file was there and nothing could take it back
    // (measured: 23 such files on a real home, none reachable, none reported).
    //
    // And it named the file `vinfo->filename` when that was set, while the
    // installer named the same file after the TARGET -- so one package could
    // get two spellings depending on which path created it, and only one of
    // them dispatches: `shim_dispatch` looks the name up in the workspace,
    // which is keyed by target. `sync_shim_tables` has one spelling.
#ifdef _WIN32
    auto xlings_bin = p.homeDir / "bin" / "xlings.exe";
    constexpr std::string_view shim_ext = ".exe";
#else
    auto xlings_bin = p.homeDir / "bin" / "xlings";
    constexpr std::string_view shim_ext = "";
#endif
    if (!fs::exists(xlings_bin)) {
        xlings_bin = p.homeDir / "xlings";
    }

    if (const auto sync = xself::sync_shim_tables(); !sync.root_error.empty()) {
        restore(sync.root_error);
        return 1;
    }

    // Self-replace: when the user switches to a different version of xlings
    // (or its multicall aliases xim/xvm), physically replace the bootstrap
    // binary with that version. Symmetric with the install-time replace in
    // installer.cppm — they share the same condition: "we just made this
    // version active for the running binary's identity".
    //
    // main.cpp's multiplexer short-circuits xlings/xim/xvm names to the
    // local cli::run() without consulting the workspace at runtime, so
    // updating workspace[xlings] alone has no observable effect; the
    // bootstrap file itself must change for `xlings --version` etc. to
    // reflect the switch.
    if (is_xlings_binary(target) && fs::exists(xlings_bin)) {
        auto* vd = get_vdata(db, target, resolved);
        if (vd && !vd->path.empty()) {
            auto active_bin = fs::path(vd->path)
                            / ("xlings" + std::string(shim_ext));
            if (fs::exists(active_bin)) {
                const auto owner = recorded_owner(db, target, resolved);
                if (!owner) {
                    restore(std::format("{}@{}: entry activation has no proven package owner", target, resolved));
                    return 1;
                }
                const auto provider = owner->ns.empty() ? owner->package
                                                       : owner->ns + ":" + owner->package;
                if (!xself::replace_entry_binary(
                    active_bin, xlings_bin, std::format("{}@{}", target, resolved), resolved,
                    xself::PackageEntryActivation{provider, vd->sourceHome})) {
                    restore("entry activation failed");
                    return 1;
                }
            }
        }
        // COMPAT(0.4.8 → drop in 0.6.0): opportunistically drop legacy
        // alias symlinks (xim/xvm/...) left over from xlings ≤ 0.4.7.
        // Lands on this path during `xlings self update`, which ends with
        // `xlings use xlings latest` — so first-upgrade self-heals.
        xself::compat::v0_4_8::cleanup_legacy_alias_shims(p.binDir, xlings_bin);
    }

    auto committed = applied->commit();
    if (!committed) { log::error("{}", committed.error()); return 1; }

    // Which release did this actually move?
    //
    // `xlings use java 25.0.4-zulu` names a MEMBER, not a package, so the
    // bare line leaves the user unable to tell which package -- and which
    // version of it -- they just selected, or that they changed packages at
    // all. The clause carries the provider name (what they typed at install
    // time) and the provider version, which is not the same string as the
    // target's version: `xim:jdk-temurin 25.0.4+7` vs `java 25.0.4+7-temurin`.
    //
    // The `{target} -> {version}` half is unchanged, deliberately: it is what
    // scripts and tests grep for.
    //
    // Only a switch that moved something says so. `self update` runs `use`
    // twice (the install's `--use`, then `use xlings latest`), and on a home
    // that was already current both printed `xlings -> <v>` -- two claims of
    // a switch that did not happen.
    std::string line;
    if (plan->toProvider.empty()) {
        line = std::format("{} -> {}", target, resolved);   // no group metadata
    } else if (plan->fromProvider.empty()) {
        line = std::format("{} -> {}  ({} {})", target, resolved,
                           plan->toProvider, plan->toProviderVersion);
    } else if (plan->fromProvider == plan->toProvider) {
        line = std::format("{} -> {}  ({} {} -> {})", target, resolved,
                           plan->toProvider, plan->fromProviderVersion,
                           plan->toProviderVersion);
    } else {
        line = std::format("{} -> {}  ({} {} -> {} {})", target, resolved,
                           plan->fromProvider, plan->fromProviderVersion,
                           plan->toProvider, plan->toProviderVersion);
    }
    if (plan->alreadyActive) log::debug("{}", line);
    else                     log::info("{}", line);

    // `-v` is the global flag; nothing here needs an option of its own.
    const bool verbose = log::get_level() <= log::Level::Debug;
    // The release being left. `workspace` is the copy read before anything
    // moved -- the writes above went through Config::workspace_mut() -- so it
    // still answers for the state the user is coming from.
    const auto leftIt = workspace.find(target);
    const std::string leftRelease =
        leftIt == workspace.end() ? std::string{} : leftIt->second;

    // Say what the switch did NOT cover.
    //
    // The single `llvm -> 20.1.7` line above was the entire output of a
    // command that left `clang` answering 22.1.8, and the user found out from
    // their compiler, not from xlings. Naming each one and what it still
    // resolves to is the difference between a mixed toolchain the user chose
    // and one they were handed.
    //
    // Collapsed to one line unless asked: a real case runs to dozens of
    // entries, and a report nobody finishes reading protects nobody. The line
    // has to carry all three facts on its own -- how many, why they did not
    // come along, and what they are now -- or it just puzzles the reader.
    if (!plan->stranded.empty()) {
        // Kept to short lines on purpose: core cannot wrap (it does not
        // depend on ui, which owns the width contract), so the only way these
        // stay inside a narrow terminal is to be written that way.
        if (!verbose) {
            log::warn("{} name(s) not in {}@{} still run from {} — -v to list",
                      plan->stranded.size(), target, resolved, leftRelease);
        } else {
            log::warn("{} name(s) not in {}@{}, still on the old release:",
                      plan->stranded.size(), target, resolved);
            for (const auto& member : plan->stranded) {
                if (member.kind == "program") {
                    log::warn("    {} (still {})", member.target,
                              member.version);
                } else {
                    log::warn("    {} (still {}, {})", member.target,
                              member.version, member.kind);
                }
            }
            log::warn("  move: xlings use {} <version>",
                      plan->stranded.front().target);
            log::warn("  drop: xlings remove {}@{}",
                      plan->stranded.front().target,
                      plan->stranded.front().version);
        }
    }

    // Switching packages: what the old one still owns.
    //
    // Silent by default, and that is the point. These names did not fall
    // behind -- the package they belong to is untouched and still active, and
    // the incoming package has no version of them to offer. There is no
    // action to recommend, so a warning would be pure noise on a command that
    // did exactly what it was asked. The `(A -> B)` clause above already says
    // the package changed; `-v` is where the list belongs.
    if (verbose && !plan->retainedByOldPackage.empty()) {
        log::warn("{} name(s) still come from {}:",
                  plan->retainedByOldPackage.size(), plan->fromProvider);
        for (const auto& member : plan->retainedByOldPackage) {
            if (member.kind == "program") {
                log::warn("    {} ({})", member.target, member.version);
            } else {
                log::warn("    {} ({}, {})", member.target, member.version,
                          member.kind);
            }
        }
        log::warn("  {} has no version of them to switch to.",
                  plan->toProvider);
    }

    return 0;
}

std::expected<VersionCandidates, int>
collect_version_candidates_(const std::string& target, bool all) {
    auto db = Config::versions();

    if (!has_target(db, target)) {
        diag::emit({
            .code    = "xvm.unknown_target",
            .summary = std::format("'{}' is not a name this home knows", target),
            .actions = {
                { "install it", std::format("xlings install {}", target) },
                { "search",     std::format("xlings search {}", target) },
            },
        });
        return std::unexpected(1);
    }

    auto workspace = Config::effective_workspace();
    VersionCandidates out;
    out.active = get_active_version(workspace, target);
    auto global_all = get_all_versions(db, target);

    if (all) {
        out.versions = global_all;
        out.title = target + " " + std::string(i18n::tr("versions (all subos)"));
        return out;
    }

    out.versions = filter_to_subos_installed_(target, global_all);
    // Newest first, like every other candidate list. `get_all_versions` walks
    // a std::map, so without this the picker offers 0.0.100 above 0.0.24 and
    // the panel does too.
    version_order::sort_desc(out.versions);
    if (out.versions.empty()) {
        // Installed somewhere, just not opted into here. This used to print
        // three separate `log::error` lines -- the negation, the evidence and
        // the hint all in red bold -- for a state where two of the three lines
        // were not errors at all. One block, one marker, and the actions lead
        // with the thing the user almost certainly wants.
        const auto origin = Config::version_origin(target);
        std::string installCoordinate;
        if (!global_all.empty()) {
            auto newest = global_all;
            version_order::sort_desc(newest);
            if (auto owner = recorded_owner(db, target, newest.front())) {
                installCoordinate = owner->canonical();
            }
        }
        auto d = not_in_subos({
            .target            = target,
            .subos             = Config::subos_scope().name,
            .versionsElsewhere = global_all,
            .source            = origin.source,
            .fromProject       = origin.fromProjectManifest,
            .installCoordinate = std::move(installCoordinate),
        });
        if (!global_all.empty()) {
            d.actions.push_back({ "see every subos",
                std::format("xlings use {} --all", target) });
        }
        diag::emit(d);
        return std::unexpected(1);
    }
    out.title = target + " " + std::string(i18n::tr("versions (current subos)"));
    return out;
}

void emit_version_panel_(const std::string& target,
                         const VersionCandidates& candidates,
                         EventStream& stream) {
    auto db = Config::versions();
    nlohmann::json fieldsJson = nlohmann::json::array();
    for (auto& ver : candidates.versions) {
        auto vdata = get_vdata(db, target, ver);
        std::string path_info;
        // `@xlings/...` rather than the absolute path. `self config` and
        // `self doctor` already abbreviate; this panel did not, and it is the
        // one whose rows are payload paths — the ~20 columns the prefix costs
        // are exactly what pushed it past the terminal.
        if (vdata && !vdata->path.empty()) path_info = Config::display_path(vdata->path);
        bool highlight = (ver == candidates.active);
        fieldsJson.push_back({{"label", ver}, {"value", path_info}, {"highlight", highlight}});
    }
    nlohmann::json payload;
    payload["title"] = candidates.title;
    payload["fields"] = std::move(fieldsJson);
    stream.emit(DataEvent{"info_panel", payload.dump()});
}

int cmd_list_versions(const std::string& target, EventStream& stream, bool all) {
    auto candidates = collect_version_candidates_(target, all);
    if (!candidates) return candidates.error();
    emit_version_panel_(target, *candidates, stream);
    if (candidates->versions.size() > 1) {
        nlohmann::json tip;
        tip["message"] = std::format("xlings use {} <version>", target);
        stream.emit(DataEvent{"tip", tip.dump()});
    }
    return 0;
}

int cmd_use_by_name(const std::string& target, EventStream& stream, bool all, bool strict) {
    auto candidates = collect_version_candidates_(target, all);
    if (!candidates) return candidates.error();

    // NO single-candidate auto-switch.
    //
    // `--help` has always said "omit to list installed versions", and from
    // 2026.7.30.2 until now the code switched instead whenever the candidate
    // count happened to be 1. The same typed command was therefore a QUERY or
    // a MUTATION depending on a number the user cannot see: the candidate set
    // is "versions opted into THIS subos", not "versions I have installed".
    // Measured on a real home -- `xlings use gcc --all` listed five while
    // `xlings use gcc` wrote state, because only one of the five was opted in
    // here.
    //
    // The argument that introduced it (2026.7.30.2: "whether the command has
    // a single correct outcome IS detectable") is sound on its own terms, but
    // it buys determinism of the OUTCOME at the cost of determinism of the
    // MEANING -- and the meaning was already documented.
    //
    // The sysroot repair that rode along on this branch -- re-running `use` on
    // the active version to re-materialize headers and libraries -- keeps
    // working as `xlings use <name> <version>`. That spelling is explicit and
    // holds at any candidate count, rather than only at exactly one.

    // Somebody to ask, and something to ask about: ask.
    //
    // `ui/selector.cpp` has had a working inline version picker since 2026-07
    // with ZERO callers -- the 2026-07-29 survey recorded it as dead code and
    // it was still dead a year later. The command that most obviously wants it
    // is this one: `xlings use gcc` knows the answer is one of five and made
    // the user read a panel and retype.
    //
    // Core does not (and must not) know about ftxui, so the question goes out
    // as a Request and the frontend decides how to render it. Non-interactive
    // frontends never see it -- EventStream refuses rather than guessing --
    // and the panel below stays the answer for them.
    if (!all && stream.interactive()) {
        PromptEvent pick;
        pick.id = "select_version";
        pick.question = std::format("Which {} ?", target);
        pick.options = candidates->versions;
        pick.defaultValue = candidates->active;
        // Stated, not inherited.  defaults to Select and this IS a
        // selection, so the behaviour was already right -- but relying on the
        // default is how the wrong tier goes unnoticed when a prompt is later
        // copied into a confirmation. The asker declares what it built.
        pick.kind = PromptEvent::Kind::Select;

        std::optional<int> done;
        std::visit(EventStream::on{
            [&](EventStream::Chosen&& c) {
                done = cmd_use(target, c.value, stream, strict);
            },
            [&](EventStream::Cancelled&&) {
                log::println("cancelled");
                done = 0;
            },
            // Nobody there: fall through to the panel below, which is a
            // complete answer rather than an error.
            [&](EventStream::NobodyToAsk&&) {},
        }, stream.prompt(std::move(pick)));
        if (done) return *done;
    }

    return cmd_list_versions(target, stream, all);
}

void register_version(const std::string& target,
                      const std::string& version,
                      const std::string& path,
                      const std::string& type,
                      const std::string& filename) {
    add_version(Config::versions_mut(), target, version, path, type, filename);
    Config::save_versions();
}

}
