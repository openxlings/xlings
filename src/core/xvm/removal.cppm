export module xlings.core.xvm.removal;

import std;

import xlings.core.log;
import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;

export namespace xlings::xvm {

struct RemovalOperation {
    std::string op;
    std::string name;
    std::string version;
};

struct RemovalContext {
    std::map<std::string, std::string> members;
    std::string provider;
    bool hasSelection { false };
};

struct RemovedVersion {
    std::string target;
    std::string version;
};

struct RemovalBatchResult {
    std::vector<RemovedVersion> removed;
};

struct RemovalBatchOptions {
    bool purgeSelection { false };
};

bool has_usable_workspace_version(
        const VersionDB& db,
        const WorkspaceInstalled& installed,
        const std::string& target) {
    auto installedIt = installed.find(target);
    auto targetIt = db.find(target);
    if (installedIt == installed.end() || targetIt == db.end()) {
        return false;
    }
    return std::ranges::any_of(
        installedIt->second,
        [&](const std::string& version) {
            return targetIt->second.versions.contains(version);
        });
}

std::expected<RemovalContext, RemovalError>
snapshot_removal_context(const VersionDB& db,
                         const std::string& target,
                         const std::string& version) {
    auto exactVersion =
        resolve_exact_version_key(db, target, version);
    if (!exactVersion) {
        return std::unexpected(std::move(exactVersion.error()));
    }
    const auto& data = db.at(target).versions.at(*exactVersion);
    auto resolveSelection = [&](const std::string& memberTarget,
                                const std::string& memberVersion)
        -> std::expected<BindingSelection, RemovalError> {
        auto selection = resolve_binding_selection(
            db, memberTarget, memberVersion);
        if (!selection) {
            return std::unexpected(RemovalError{
                .kind = RemovalErrorKind::SelectionInvalid,
                .target = selection.error().target,
                .version = selection.error().version,
                .message = selection.error().message,
            });
        }
        return std::move(*selection);
    };

    auto firstSelection = resolveSelection(target, *exactVersion);
    if (!firstSelection) {
        // Removal must not depend on the release resolving.
        //
        // A state that cannot be resolved -- a legacy edge pointing at a
        // version that no longer exists, say -- already blocks `use`. Letting
        // it block `remove` as well leaves the user unable to switch *or* to
        // take the package out: a dead end with no command that gets out of
        // it. Reported from the field on a real installation, where
        // `use gcc 15` and `remove gcc 15` both refused.
        //
        // Taking something out needs no understanding of what put it in.
        // Fall back to the one member we are certain of -- the exact
        // (target, version) the user named -- and let the rest of the
        // release be whatever it is. The same reasoning keeps removal
        // outside the xpackage spec gate in xim/installer.cppm.
        log::warn("[xvm] {}@{} does not resolve as a release ({}); removing "
                  "just this entry",
                  target, *exactVersion, firstSelection.error().message);
        BindingSelection single;
        single.members.emplace(target, *exactVersion);
        firstSelection = std::move(single);
    }
    if (!data.bindingGroup) {
        return RemovalContext{
            .members = std::move(firstSelection->members),
            .hasSelection = true,
        };
    }

    const auto& owner = *data.bindingGroup;
    std::set<std::tuple<std::string, std::string, std::string>> groups;
    for (const auto& [_, info] : db) {
        for (const auto& [__, member] : info.versions) {
            if (!member.bindingGroup
                || member.bindingGroup->provider != owner.provider
                || member.bindingGroup->providerVersion
                    != owner.providerVersion) {
                continue;
            }
            groups.emplace(
                member.bindingGroup->group,
                member.bindingGroup->rootTarget,
                member.bindingGroup->rootVersion);
        }
    }

    std::map<std::string, std::string> members;
    for (const auto& [_, rootTarget, rootVersion] : groups) {
        auto selection = resolveSelection(rootTarget, rootVersion);
        if (!selection) {
            return std::unexpected(std::move(selection.error()));
        }
        for (const auto& [memberTarget, memberVersion] :
             selection->members) {
            auto [it, inserted] =
                members.emplace(memberTarget, memberVersion);
            if (!inserted && it->second != memberVersion) {
                return std::unexpected(RemovalError{
                    .kind = RemovalErrorKind::SelectionInvalid,
                    .target = memberTarget,
                    .version = memberVersion,
                    .message =
                        "provider release selects conflicting member versions",
                });
            }
        }
    }

    return RemovalContext{
        .members = std::move(members),
        .provider = owner.provider,
        .hasSelection = true,
    };
}

std::expected<RemovalBatchResult, RemovalError>
apply_removal_batch(VersionDB& db,
                    Workspace& workspace,
                    WorkspaceInstalled& installed,
                    const std::vector<RemovalOperation>& operations,
                    const RemovalContext& context,
                    const RemovalBatchOptions& options = {}) {
    std::vector<RemovedVersion> removals;
    std::vector<RemovedVersion> selectedRecipeMembers;
    std::set<std::pair<std::string, std::string>> seen;

    for (const auto& operation : operations) {
        if (operation.op == "remove_all") {
            if (context.provider.empty()) {
                return std::unexpected(RemovalError{
                    .kind = RemovalErrorKind::ProviderRequired,
                    .target = operation.name,
                    .version = operation.version,
                    .message =
                        "remove_all requires a canonical provider context",
                });
            }
            auto targetIt = db.find(operation.name);
            bool matched = false;
            if (targetIt != db.end()) {
                for (const auto& [version, data] : targetIt->second.versions) {
                    if (!data.bindingGroup
                        || data.bindingGroup->provider != context.provider) {
                        continue;
                    }
                    matched = true;
                    if (seen.emplace(operation.name, version).second) {
                        removals.push_back({
                            .target = operation.name,
                            .version = version,
                        });
                    }
                }
            }
            if (!matched) {
                return std::unexpected(RemovalError{
                    .kind = RemovalErrorKind::ProviderVersionNotFound,
                    .target = operation.name,
                    .version = operation.version,
                    .message = std::format(
                        "target has no version owned by provider '{}'",
                        context.provider),
                });
            }
            continue;
        }
        if (operation.op != "remove") continue;

        if (context.hasSelection) {
            auto memberIt = context.members.find(operation.name);
            if (memberIt == context.members.end()) {
                // A name this release does not own is skipped, not fatal.
                //
                // Recipes name their removal targets unconditionally, from a
                // static list or from whatever the payload happens to contain,
                // and neither has to match what this platform registered.
                // Measured on a real home: a Windows llvm payload under Linux
                // makes `uninstall()` ask for `clang++.exe` (never registered
                // here) and for `cc` (registered, but by gcc). Refusing either
                // aborted the whole batch, so the package could not be removed
                // at all -- and the install side was refusing the same records
                // for its own reasons, which left no command that got the user
                // out.
                //
                // Skipping protects strictly more than refusing did. The
                // selection is already the authority on what may be deleted:
                // had the release actually owned this name, it would be IN the
                // selection and removed below. So there is no entry that
                // refusing saved and skipping loses -- refusing only turned
                // "one op does nothing" into "nothing happens at all".
                //
                // Said out loud: a recipe naming something it does not own is
                // worth knowing about, it is just not worth stopping for.
                if (db.contains(operation.name)) {
                    log::warn("[xvm] recipe asked to remove '{}', which this "
                              "release does not own — skipped",
                              operation.name);
                }
                continue;
            }
            auto selectedVersion = resolve_exact_version_key(
                db, operation.name, memberIt->second);
            if (!selectedVersion) {
                return std::unexpected(
                    std::move(selectedVersion.error()));
            }
            if (!operation.version.empty()) {
                auto recipeVersion = resolve_exact_version_key(
                    db, operation.name, operation.version);
                if (!recipeVersion) {
                    return std::unexpected(
                        std::move(recipeVersion.error()));
                }
                if (*recipeVersion != *selectedVersion) {
                    return std::unexpected(RemovalError{
                        .kind = RemovalErrorKind::VersionMismatch,
                        .target = operation.name,
                        .version = operation.version,
                        .message = std::format(
                            "recipe removal version does not match "
                            "selected owned version '{}'",
                            *selectedVersion),
                    });
                }
            }
            selectedRecipeMembers.push_back({
                .target = operation.name,
                .version = *selectedVersion,
            });
            continue;
        }

        std::expected<std::string, RemovalError> exactVersion =
            std::unexpected(RemovalError{
                .kind = RemovalErrorKind::VersionNotFound,
                .target = operation.name,
                .version = operation.version,
                .message = "removal operation has no exact version",
            });
        if (!operation.version.empty()) {
            exactVersion = resolve_exact_version_key(
                db, operation.name, operation.version);
        } else if (auto targetIt = db.find(operation.name);
                   targetIt != db.end()
                   && targetIt->second.versions.size() > 1) {
            exactVersion = std::unexpected(RemovalError{
                .kind = RemovalErrorKind::AmbiguousVersion,
                .target = operation.name,
                .version = operation.version,
                .message =
                    "versionless removal has no unique validated selection",
            });
        }
        if (!exactVersion) {
            return std::unexpected(std::move(exactVersion.error()));
        }
        if (seen.emplace(operation.name, *exactVersion).second) {
            removals.push_back({
                .target = operation.name,
                .version = *exactVersion,
            });
        }
    }

    for (const auto& removal : selectedRecipeMembers) {
        if (seen.emplace(removal.target, removal.version).second) {
            removals.push_back(removal);
        }
    }
    if (options.purgeSelection || !selectedRecipeMembers.empty()) {
        for (const auto& [target, version] : context.members) {
            auto exactVersion =
                resolve_exact_version_key(db, target, version);
            if (!exactVersion) {
                return std::unexpected(
                    std::move(exactVersion.error()));
            }
            if (seen.emplace(target, *exactVersion).second) {
                removals.push_back({
                    .target = target,
                    .version = *exactVersion,
                });
            }
        }
    }

    auto candidateDb = db;
    auto candidateWorkspace = workspace;
    auto candidateInstalled = installed;
    std::set<std::string> touchedTargets;

    for (const auto& removal : removals) {
        auto result =
            remove_version(candidateDb, removal.target, removal.version);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        touchedTargets.insert(removal.target);

        if (auto installedIt = candidateInstalled.find(removal.target);
            installedIt != candidateInstalled.end()) {
            std::erase(installedIt->second, removal.version);
            if (installedIt->second.empty()) {
                candidateInstalled.erase(installedIt);
            }
        }
        if (auto activeIt = candidateWorkspace.find(removal.target);
            activeIt != candidateWorkspace.end()
            && activeIt->second == removal.version) {
            candidateWorkspace.erase(activeIt);
        }
    }

    // ── Group-coherent reactivation ──────────────────────────────────
    //
    // Removing the active release leaves its members with no active version.
    // Choosing a replacement per target independently is how `gcc` ends up on
    // GCC 15 while `g++` lands on musl's 15: two targets, two searches, no
    // shared answer. That is the mixed-toolchain state this whole model
    // exists to prevent, reintroduced at the moment of removal.
    //
    // Move a whole surviving release at once or move nothing. A candidate
    // qualifies only when every member of its group is still registered, is
    // opted into this subos, and does not contradict an already-active
    // version. Otherwise the group stays inactive and the user re-selects
    // explicitly -- an inactive toolchain is a visible problem, an
    // incoherent one is not.
    const auto memberAvailable =
        [&](const std::string& target, const std::string& version) {
            auto dbIt = candidateDb.find(target);
            if (dbIt == candidateDb.end()
                || !dbIt->second.versions.contains(version)) {
                return false;
            }
            auto installedIt = candidateInstalled.find(target);
            if (installedIt == candidateInstalled.end()
                || std::ranges::find(installedIt->second, version)
                       == installedIt->second.end()) {
                return false;
            }
            auto activeIt = candidateWorkspace.find(target);
            return activeIt == candidateWorkspace.end()
                || activeIt->second == version;
        };

    for (const auto& target : touchedTargets) {
        if (candidateWorkspace.contains(target)) continue;
        auto installedIt = candidateInstalled.find(target);
        if (installedIt == candidateInstalled.end()) continue;

        // Highest surviving release first, by version rather than by the
        // order the user happened to install things in.
        auto candidates = installedIt->second;
        std::ranges::sort(candidates, version_key_greater);

        for (const auto& candidate : candidates) {
            auto dbIt = candidateDb.find(target);
            if (dbIt == candidateDb.end()
                || !dbIt->second.versions.contains(candidate)) {
                continue;
            }
            auto selection =
                resolve_binding_selection(candidateDb, target, candidate);
            if (!selection) continue;
            if (!std::ranges::all_of(
                    selection->members, [&](const auto& member) {
                        return memberAvailable(member.first, member.second);
                    })) {
                continue;
            }
            for (const auto& [memberTarget, memberVersion] :
                 selection->members) {
                candidateWorkspace[memberTarget] = memberVersion;
            }
            break;
        }
    }

    db.swap(candidateDb);
    workspace.swap(candidateWorkspace);
    installed.swap(candidateInstalled);
    return RemovalBatchResult{
        .removed = std::move(removals),
    };
}

}  // namespace xlings::xvm
