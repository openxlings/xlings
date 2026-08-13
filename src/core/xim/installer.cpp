module xlings.core.xim.installer;

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.compat;
import mcpplibs.xpkg.executor;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.compatibility;
import xlings.core.xim.payload;
import xlings.core.xim.install_state;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.log;
import xlings.platform;
import xlings.platform.target;
import xlings.core.config;
import xlings.core.semver;
import xlings.core.entry_binary;
import xlings.core.elf_same_source;
import xlings.core.closure_check;
import xlings.libs.json;
import xlings.core.common;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.subos.manifest;
import xlings.core.xvm.commands;
import xlings.core.xvm.shim;
import xlings.core.xim.libxpkg.types.script;
import xlings.core.xim.libxpkg.types.subos;
import xlings.runtime.cancellation;

namespace xlings::xim {

XpkgSpecSupport xpkg_spec_support(std::string_view spec) {
    if (spec.empty()) return {.supported = true, .declared = 1};
    int value = 0;
    const auto* first = spec.data();
    const auto* last  = spec.data() + spec.size();
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value < 1) {
        return {.supported = false, .declared = 0};
    }
    return {.supported = value <= max_supported_xpkg_spec, .declared = value};
}

std::optional<XpkgResolvedFilesystemEffect>
resolve_xpkg_filesystem_effect(
        const xvm::VersionDB& db,
        const xvm::Workspace& workspace,
        const XpkgFilesystemEffect& effect) {
    XpkgResolvedFilesystemEffect resolved{
        .kind = effect.kind,
        .target = effect.target,
        .version = effect.version,
        .sourceDir = effect.sourceDir,
    };
    if (effect.kind == XpkgFilesystemEffectKind::InstallHeaders
        || effect.kind == XpkgFilesystemEffectKind::RemoveHeaders) {
        // Headers have no program payload to resolve, but they do have an
        // owner, and whether that owner is the active version decides
        // whether these headers belong in the sysroot right now.
        if (!effect.target.empty()) {
            auto headerActiveIt = workspace.find(effect.target);
            resolved.active = headerActiveIt != workspace.end()
                && headerActiveIt->second == effect.version;
        } else {
            // No owner recorded (older plan shape): keep the previous
            // unconditional behavior rather than silently skipping.
            resolved.active = true;
        }
        return resolved;
    }

    auto targetIt = db.find(effect.target);
    if (targetIt == db.end()) return std::nullopt;
    auto versionIt =
        targetIt->second.versions.find(effect.version);
    if (versionIt == targetIt->second.versions.end()) {
        return std::nullopt;
    }
    const auto& data = versionIt->second;
    const auto kind =
        data.kind.empty() ? targetIt->second.type : data.kind;
    // Three kinds, not two. This read `program` or else `lib`, which meant a
    // FileAsset effect was compared against "lib" and could never match an
    // entry registered as "files" -- so every declared file asset resolved to
    // nullopt, the caller logged "validated xvm effect target disappeared or
    // changed kind", and the FileAsset branch below was dead code. The assets
    // still landed, because the activation pass places them separately, so
    // the only visible symptom was a warning on a correct install. It stayed
    // hidden until a real recipe declared one.
    const auto expectedKind =
        effect.kind == XpkgFilesystemEffectKind::ProgramShim
            ? std::string_view{"program"}
        : effect.kind == XpkgFilesystemEffectKind::FileAsset
            ? std::string_view{"files"}
            : std::string_view{"lib"};
    if (kind != expectedKind) return std::nullopt;

    resolved.path = data.path;
    resolved.sourceName = !data.sourceName.empty()
        ? data.sourceName
        : (!targetIt->second.filename.empty()
            ? targetIt->second.filename
            : effect.target);
    resolved.destinationName = !data.destinationName.empty()
        ? data.destinationName
        : (effect.kind == XpkgFilesystemEffectKind::ProgramShim
            ? effect.target
            : resolved.sourceName);
    auto activeIt = workspace.find(effect.target);
    resolved.active =
        activeIt != workspace.end()
        && activeIt->second == effect.version;
    return resolved;
}

std::size_t attach_legacy_header_dir(
        xvm::VersionDB& db,
        const std::string& target,
        const std::string& version,
        const std::vector<XpkgFilesystemEffect>& effects) {
    const std::string* sourceDir = nullptr;
    for (const auto& effect : effects) {
        // Last one wins, matching how the installer used to assign per op.
        if (effect.kind == XpkgFilesystemEffectKind::InstallHeaders
            && !effect.sourceDir.empty()) {
            sourceDir = &effect.sourceDir;
        }
    }
    if (!sourceDir) return 0;

    auto targetIt = db.find(target);
    if (targetIt == db.end()) return 0;
    auto versionIt = targetIt->second.versions.find(version);
    if (versionIt == targetIt->second.versions.end()) return 0;

    versionIt->second.includedir = *sourceDir;
    return 1;
}

std::expected<XpkgRegistrationPlan, XpkgRegistrationError>
normalize_xpkg_registration_plan(
        const PlanNode& node,
        const std::vector<mcpplibs::xpkg::XvmOp>& operations,
        const std::string& versionNamespace,
        const std::filesystem::path& dataDir,
        bool useAfterInstall) {
    auto normalizeVersion = [&](
            const std::string& version,
            std::size_t operationIndex,
            const std::string& target)
        -> std::expected<std::string, XpkgRegistrationError> {
        if (version.empty()) {
            return std::unexpected(XpkgRegistrationError{
                .kind = XpkgRegistrationErrorKind::InvalidVersion,
                .operationIndex = operationIndex,
                .target = target,
                .version = version,
                .message = "xvm registration version is empty",
            });
        }
        const auto separator = version.find(':');
        if (separator == std::string::npos) {
            return xvm::make_ns_version(versionNamespace, version);
        }
        const auto actualNamespace = version.substr(0, separator);
        const auto bareVersion = version.substr(separator + 1);
        if (actualNamespace.empty()
            || bareVersion.empty()
            || bareVersion.contains(':')
            || actualNamespace != versionNamespace) {
            return std::unexpected(XpkgRegistrationError{
                .kind = XpkgRegistrationErrorKind::InvalidVersion,
                .operationIndex = operationIndex,
                .target = target,
                .version = version,
                .message = std::format(
                    "xvm registration version namespace '{}' "
                    "does not match expected namespace '{}'",
                    actualNamespace, versionNamespace),
            });
        }
        return version;
    };
    const auto fallbackPath =
        (node.storeRoot.empty() ? dataDir / "xpkgs" : node.storeRoot)
        / package_store_name(node.namespaceName, node.name)
        / node.version;
    XpkgRegistrationPlan plan{
        .batch = {
            .provider = node.canonicalName.empty()
                ? canonical_package_name(node.namespaceName, node.name)
                : node.canonicalName,
            .providerVersion = node.version,
            // Owner hint for headers that do not name a group. Recipes
            // conventionally register a target under the package's own name,
            // so that target identifies which group the package's headers
            // belong to when several groups exist.
            .primaryTarget = node.name,
            .useAfterInstall = useAfterInstall,
        },
    };

    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        if (operation.op == "headers") {
            plan.batch.headers.push_back({
                .sourceDir = operation.includedir,
            });
            // Carry the owning target and version so materialization can be
            // gated on the version actually becoming active. Installing a
            // version that does not become active used to copy its headers
            // into the sysroot anyway, leaving the active release's headers
            // overwritten by an inactive one.
            plan.effects.push_back({
                .kind = XpkgFilesystemEffectKind::InstallHeaders,
                .target = node.name,
                .version = xvm::make_ns_version(versionNamespace, node.version),
                .sourceDir = operation.includedir,
            });
            continue;
        }
        if (operation.op == "remove_headers") {
            plan.effects.push_back({
                .kind = XpkgFilesystemEffectKind::RemoveHeaders,
                .sourceDir = operation.includedir,
            });
            continue;
        }
        if (operation.op != "add") continue;
        const auto rawVersion = operation.version.empty()
            ? node.version
            : operation.version;
        const auto kind =
            operation.type.empty() ? std::string{"program"} : operation.type;
        // A `files` asset names no artifact to dispatch, so it carries no
        // source or destination *name* -- what it carries is a src/dst pair,
        // below. Treating it like a program would invent a sourceName from
        // the target and then look for an executable that does not exist.
        const auto namesAnArtifact = kind != "group" && kind != "files";
        const auto sourceName = namesAnArtifact
            ? (operation.filename.empty()
                ? operation.name
                : operation.filename)
            : std::string{};
        xvm::RegistrationNode registrationNode{
            .target = operation.name,
            .version = {},
            .path = operation.bindir.empty()
                ? fallbackPath.string()
                : operation.bindir,
            .kind = kind,
            .sourceName = sourceName,
            .destinationName = namesAnArtifact
                ? (kind == "program" ? operation.name : sourceName)
                : std::string{},
            .fileSrc = kind == "files" ? operation.src : std::string{},
            .fileDst = kind == "files" ? operation.dst : std::string{},
        };
        auto exactVersion =
            normalizeVersion(rawVersion, index, operation.name);
        if (!exactVersion) {
            return std::unexpected(std::move(exactVersion.error()));
        }
        registrationNode.version = std::move(*exactVersion);
        // Is there a command behind this name, or is it a pure anchor?
        //
        // The registration layer cannot ask -- it never touches the
        // filesystem -- and nothing else in the record distinguishes the two:
        // an anchor registered with no bindir still gets the package install
        // dir as its `path`, still defaults to kind "program", and still has a
        // sourceName derived from its own target. The payload has run by the
        // time config() does, so the file either is there or it is not, and
        // this is the layer that can look.
        //
        // Same helper the shim dispatches through, so "registration thinks
        // there is a program here" and "the shim finds one" cannot disagree.
        // An alias-mode entry runs something else by definition and is
        // runnable regardless.
        // `operation.alias`, not `registrationNode.alias`: the latter is
        // filled in further down and is still empty here.
        if (kind == "program" && operation.alias.empty()) {
            registrationNode.runnable = !xvm::resolve_executable(
                sourceName.empty() ? operation.name : sourceName,
                registrationNode.path,
                Config::paths().homeDir.string()).empty();
        }
        // Stored verbatim. The core does not rewrite what a recipe wrote --
        // it only expands its OWN marker at execution time
        // (kSubosPlaceholder). A recipe that needs the active subos writes
        // the marker; one that writes a concrete subos path gets the legacy
        // treatment (normalize_subos_paths at dispatch) and a doctor finding
        // telling it so.
        if (!operation.alias.empty()) {
            registrationNode.alias.push_back(operation.alias);
        }
        for (const auto& [key, value] : operation.envs) {
            auto [environmentIt, inserted] =
                registrationNode.envs.emplace(key, value);
            if (!inserted && environmentIt->second != value) {
                return std::unexpected(XpkgRegistrationError{
                    .kind = XpkgRegistrationErrorKind::ConflictingEnvironment,
                    .operationIndex = index,
                    .target = operation.name,
                    .version = key,
                    .message = std::format(
                        "xvm registration environment '{}' has "
                        "conflicting values",
                        key),
                });
            }
        }
        if (!operation.binding.empty()) {
            const auto separator = operation.binding.find('@');
            if (separator == std::string::npos
                || separator == 0
                || separator + 1 == operation.binding.size()
                || operation.binding.find('@', separator + 1)
                    != std::string::npos) {
                return std::unexpected(XpkgRegistrationError{
                    .kind = XpkgRegistrationErrorKind::InvalidBinding,
                    .operationIndex = index,
                    .target = operation.name,
                    .version = operation.binding,
                    .message =
                        "xvm registration binding must be '<root>@<version>'",
                });
            }
            const auto rootTarget =
                operation.binding.substr(0, separator);
            auto rootVersion = normalizeVersion(
                operation.binding.substr(separator + 1),
                index, rootTarget);
            if (!rootVersion) {
                return std::unexpected(std::move(rootVersion.error()));
            }
            registrationNode.binding = xvm::RegistrationBinding{
                .rootTarget = rootTarget,
                .rootVersion = std::move(*rootVersion),
            };
        }
        if (kind == "program" || kind == "lib" || kind == "files") {
            plan.effects.push_back({
                .kind = kind == "program"
                    ? XpkgFilesystemEffectKind::ProgramShim
                    : (kind == "lib"
                        ? XpkgFilesystemEffectKind::Library
                        : XpkgFilesystemEffectKind::FileAsset),
                .target = operation.name,
                .version = registrationNode.version,
            });
        }
        plan.batch.nodes.push_back(std::move(registrationNode));
    }
    return plan;
}

std::expected<xvm::RemovalContext, xvm::RemovalError>
snapshot_xpkg_removal_context(const xvm::VersionDB& db, const xvm::Workspace& workspace, const std::vector<mcpplibs::xpkg::XvmOp>& operations, const std::string& executingProvider, const std::string& executingProviderVersion, const std::string& preferredTarget, const std::string& preferredVersion) {
    xvm::RemovalContext legacyContext;
    std::optional<xvm::RemovalContext> providerContext;
    std::set<std::pair<std::string, std::string>> seeds;

    auto mergeSeed = [&](const std::string& target,
                         const std::string& version)
        -> std::expected<void, xvm::RemovalError> {
        if (target.empty() || version.empty()
            || !seeds.emplace(target, version).second) {
            return {};
        }
        auto exactVersion =
            xvm::resolve_exact_version_key(db, target, version);
        if (!exactVersion) {
            return std::unexpected(std::move(exactVersion.error()));
        }
        const auto& info = db.at(target);
        const auto& data = info.versions.at(*exactVersion);
        const auto hasOutgoingEdge = std::ranges::any_of(
            info.bindings,
            [&](const auto& peer) {
                return peer.second.contains(*exactVersion);
            });
        const auto hasIncomingEdge = std::ranges::any_of(
            db,
            [&](const auto& peer) {
                auto edgeIt = peer.second.bindings.find(target);
                if (edgeIt == peer.second.bindings.end()) return false;
                return std::ranges::any_of(
                    edgeIt->second,
                    [&](const auto& edge) {
                        return edge.second == *exactVersion;
                    });
            });
        const auto isUnboundLegacySingleton =
            !data.bindingGroup
            && !data.bindingMembersDeclared
            && data.bindingMembers.empty()
            && !data.bindingHeadersDeclared
            && data.bindingHeaders.empty()
            && data.bindingIntegrityIssues.empty()
            && !hasOutgoingEdge
            && !hasIncomingEdge;
        if (isUnboundLegacySingleton) {
            auto [it, inserted] =
                legacyContext.members.emplace(target, *exactVersion);
            if (!inserted && it->second != *exactVersion) {
                return std::unexpected(xvm::RemovalError{
                    .kind = xvm::RemovalErrorKind::SelectionInvalid,
                    .target = target,
                    .version = *exactVersion,
                    .message =
                        "legacy removal selections conflict on target version",
                });
            }
            legacyContext.hasSelection = true;
            return {};
        }
        auto selection =
            xvm::snapshot_removal_context(db, target, *exactVersion);
        if (!selection) {
            return std::unexpected(std::move(selection.error()));
        }
        if (!selection->provider.empty()) {
            if (!executingProvider.empty()
                && selection->provider != executingProvider) {
                return std::unexpected(xvm::RemovalError{
                    .kind = xvm::RemovalErrorKind::ProviderMismatch,
                    .target = target,
                    .version = version,
                    .message = std::format(
                        "selected version is owned by provider '{}', not '{}'",
                        selection->provider, executingProvider),
                });
            }
            if (!providerContext) {
                providerContext = std::move(*selection);
            }
            return {};
        }
        if (providerContext) return {};

        for (const auto& [memberTarget, memberVersion] :
             selection->members) {
            auto [it, inserted] =
                legacyContext.members.emplace(memberTarget, memberVersion);
            if (!inserted && it->second != memberVersion) {
                return std::unexpected(xvm::RemovalError{
                    .kind = xvm::RemovalErrorKind::SelectionInvalid,
                    .target = memberTarget,
                    .version = memberVersion,
                    .message =
                        "legacy removal selections conflict on target version",
                });
            }
        }
        legacyContext.hasSelection = true;
        return {};
    };

    if (!preferredTarget.empty() && db.contains(preferredTarget)) {
        auto merged = mergeSeed(preferredTarget, preferredVersion);
        if (!merged) return std::unexpected(std::move(merged.error()));
    }

    for (const auto& operation : operations) {
        if (operation.op != "remove") continue;
        std::string version = operation.version;
        if (version.empty()) {
            auto activeIt = workspace.find(operation.name);
            if (activeIt != workspace.end()) {
                version = activeIt->second;
            }
        }
        if (version.empty() || !db.contains(operation.name)) continue;
        auto merged = mergeSeed(operation.name, version);
        if (!merged) return std::unexpected(std::move(merged.error()));
    }

    if (!providerContext
        && !legacyContext.hasSelection
        && !executingProvider.empty()
        && !executingProviderVersion.empty()) {
        bool foundProviderRelease = false;
        for (const auto& [target, info] : db) {
            for (const auto& [version, data] : info.versions) {
                if (!data.bindingGroup
                    || data.bindingGroup->provider != executingProvider
                    || data.bindingGroup->providerVersion
                        != executingProviderVersion) {
                    continue;
                }
                foundProviderRelease = true;
                auto merged = mergeSeed(target, version);
                if (!merged) {
                    return std::unexpected(std::move(merged.error()));
                }
                break;
            }
            if (foundProviderRelease) break;
        }
    }

    if (providerContext) return std::move(*providerContext);
    return legacyContext;
}

std::expected<xvm::RemovalBatchResult, xvm::RemovalError>
apply_xpkg_removal_operations(xvm::VersionDB& db, xvm::Workspace& workspace, xvm::WorkspaceInstalled& installed, const std::vector<mcpplibs::xpkg::XvmOp>& operations, const xvm::RemovalContext& context, const xvm::RemovalBatchOptions& options) {
    std::vector<xvm::RemovalOperation> removals;
    for (const auto& operation : operations) {
        if (operation.op != "remove" && operation.op != "remove_all") {
            continue;
        }
        removals.push_back({
            .op = operation.op,
            .name = operation.name,
            .version = operation.version,
        });
    }
    return xvm::apply_removal_batch(
        db, workspace, installed, removals, context, options);
}

std::expected<XpkgXvmMetadataResult, XpkgXvmMetadataError>
apply_xpkg_xvm_metadata_batch(xvm::VersionDB& db, xvm::Workspace& workspace, xvm::WorkspaceInstalled& installed, const std::vector<mcpplibs::xpkg::XvmOp>& operations, const xvm::RemovalContext& removalContext, const XpkgRegistrationPlan& registration, const xvm::RemovalBatchOptions& removalOptions) {
    auto candidateDb = db;
    auto candidateWorkspace = workspace;
    auto candidateInstalled = installed;

    auto removal = apply_xpkg_removal_operations(
        candidateDb,
        candidateWorkspace,
        candidateInstalled,
        operations,
        removalContext,
        removalOptions);
    if (!removal) {
        return std::unexpected(XpkgXvmMetadataError{
            std::in_place_type<xvm::RemovalError>,
            std::move(removal.error()),
        });
    }

    std::vector<xvm::RegisteredMember> registered;
    std::vector<xvm::DetachedLegacyMember> detachedLegacy;
    if (!registration.batch.nodes.empty()
        || !registration.batch.headers.empty()) {
        auto registrationResult = xvm::apply_registration_batch(
            candidateDb,
            candidateWorkspace,
            candidateInstalled,
            registration.batch,
            &detachedLegacy);
        if (!registrationResult) {
            return std::unexpected(XpkgXvmMetadataError{
                std::in_place_type<xvm::RegistrationError>,
                std::move(registrationResult.error()),
            });
        }
        registered = std::move(*registrationResult);
    }

    db.swap(candidateDb);
    workspace.swap(candidateWorkspace);
    installed.swap(candidateInstalled);
    return XpkgXvmMetadataResult{
        .removal = std::move(*removal),
        .registered = std::move(registered),
        .detachedLegacy = std::move(detachedLegacy),
        .effects = registration.effects,
    };
}

void cleanup_removed_xvm_library_artifacts(
        const std::filesystem::path& libDir,
        const xvm::VersionDB& dbBeforeRemoval,
        const xvm::VersionDB& currentDb,
        const xvm::RemovalBatchResult& removalResult) {
    auto destinationName = [](
            const std::string& target,
            const xvm::VInfo& info,
            const xvm::VData& data) {
        if (!data.destinationName.empty()) {
            return data.destinationName;
        }
        if (!info.filename.empty()) return info.filename;
        return target;
    };

    std::set<std::string> destinations;
    for (const auto& removed : removalResult.removed) {
        auto targetIt = dbBeforeRemoval.find(removed.target);
        if (targetIt == dbBeforeRemoval.end()) continue;
        auto versionIt =
            targetIt->second.versions.find(removed.version);
        if (versionIt == targetIt->second.versions.end()) continue;
        const auto& data = versionIt->second;
        const auto kind = data.kind.empty()
            ? targetIt->second.type
            : data.kind;
        if (kind != "lib") continue;
        destinations.insert(destinationName(
            removed.target, targetIt->second, data));
    }

    for (const auto& name : destinations) {
        const auto hasSurvivingOwner = std::ranges::any_of(
            currentDb,
            [&](const auto& targetEntry) {
                return std::ranges::any_of(
                    targetEntry.second.versions,
                    [&](const auto& versionEntry) {
                        const auto& data = versionEntry.second;
                        const auto kind = data.kind.empty()
                            ? targetEntry.second.type
                            : data.kind;
                        return kind == "lib"
                            && destinationName(
                                targetEntry.first,
                                targetEntry.second,
                                data) == name;
                    });
            });
        if (hasSurvivingOwner) continue;

        const auto destination = libDir / name;
        std::error_code ec;
        if (std::filesystem::exists(destination, ec)
            || std::filesystem::is_symlink(destination, ec)) {
            ec.clear();
            std::filesystem::remove(destination, ec);
        }
    }
}

void cleanup_removed_xvm_program_artifacts(
        const std::filesystem::path& binDir,
        const xvm::VersionDB& dbBeforeRemoval,
        const xvm::VersionDB& currentDb,
        const xvm::WorkspaceInstalled& installed,
        const xvm::RemovalBatchResult& removalResult) {
    std::set<std::string> removedPrograms;
    for (const auto& removed : removalResult.removed) {
        auto targetIt = dbBeforeRemoval.find(removed.target);
        if (targetIt == dbBeforeRemoval.end()) continue;
        auto versionIt =
            targetIt->second.versions.find(removed.version);
        if (versionIt == targetIt->second.versions.end()) continue;
        const auto kind = versionIt->second.kind.empty()
            ? targetIt->second.type
            : versionIt->second.kind;
        if (kind == "program") {
            removedPrograms.insert(removed.target);
        }
    }

    for (const auto& target : removedPrograms) {
        if (xvm::has_usable_workspace_version(
                currentDb, installed, target)) {
            continue;
        }
#ifdef _WIN32
        constexpr std::string_view shimExtension = ".exe";
#else
        constexpr std::string_view shimExtension = "";
#endif
        std::string shimName = target;
        if (!shimExtension.empty()
            && !shimName.ends_with(shimExtension)) {
            shimName += shimExtension;
        }
        const auto shimPath = binDir / shimName;
        std::error_code ec;
        if (!std::filesystem::exists(shimPath, ec)
            && !std::filesystem::is_symlink(shimPath, ec)) {
            continue;
        }
        ec.clear();
        std::filesystem::remove(shimPath, ec);
        if (ec) {
            log::warn("could not remove shim {}: {}; will be replaced "
                      "on next install/use",
                      shimPath.string(), ec.message());
        }
    }
}

bool evict_invalid_archive_cache_(
        const std::filesystem::path& archive,
        const ExtractError& error) {
    if (error.kind != ExtractErrorKind::InvalidInputArchive) return false;
    std::error_code ec;
    bool removed = std::filesystem::remove(archive, ec);
    auto sidecar = archive;
    sidecar += ".meta";
    ec.clear();
    std::filesystem::remove(sidecar, ec);
    return removed;
}

namespace detail_ {

std::string format_hook_failure(
        std::string_view hookName,
        const mcpplibs::xpkg::HookResult& result) {
    auto trim = [](std::string_view text) {
        while (!text.empty()
               && std::isspace(static_cast<unsigned char>(text.front()))) {
            text.remove_prefix(1);
        }
        while (!text.empty()
               && std::isspace(static_cast<unsigned char>(text.back()))) {
            text.remove_suffix(1);
        }
        return text;
    };

    auto error = trim(result.error);
    auto output = trim(result.output);
    std::string message = std::format("{} hook failed", hookName);
    if (!error.empty()) {
        message += ": ";
        message.append(error);
    }
    if (!output.empty() && output != error) {
        message.push_back('\n');
        message.append(result.output);
    }
    return message;
}

bool dep_version_matches_(std::string_view nodeVersion,
                          std::string_view depVersion) {
    if (depVersion.empty()) return true;
    if (nodeVersion == depVersion) return true;
    return semver::satisfies_expr(nodeVersion, depVersion);
}

std::filesystem::path
configure_xpkg_execution_artifact_paths_(
        mcpplibs::xpkg::ExecutionContext& context) {
    auto subosDir = Config::xvm_artifact_subos_dir();
    context.bin_dir = subosDir / "bin";
    context.subos_sysrootdir = subosDir.string();
    return subosDir;
}

void configure_dependency_store_roots_(
        mcpplibs::xpkg::ExecutionContext& context,
        const std::filesystem::path& selectedStore) {
    auto append = [&](std::filesystem::path root) {
        if (root.empty()) return;
        root = root.lexically_normal();
        if (std::ranges::find(context.dependency_store_roots, root)
            == context.dependency_store_roots.end()) {
            context.dependency_store_roots.push_back(std::move(root));
        }
    };

    append(selectedStore);
    auto projectDataDir = Config::project_data_dir();
    if (!projectDataDir.empty()) append(projectDataDir / "xpkgs");
    append(Config::global_data_dir() / "xpkgs");
}

std::string pkgindex_root_for_(const std::filesystem::path& pkgFile, bool* outHasLibs) {
    namespace fs = std::filesystem;
    std::error_code ec;

    auto derived = pkgFile.parent_path().parent_path().parent_path();
    if (fs::is_directory(derived / "libs", ec)) {
        if (outHasLibs) *outHasLibs = true;
        return derived.string();
    }

    // Only the primary index. Not "any index that happens to have libs": picking
    // between several would be a guess about which ecosystem a recipe belongs to,
    // and `xim.pkgindex.*` is one shared namespace by construction.
    auto primary = Config::global_data_dir() / "xim-pkgindex";
    if (fs::is_directory(primary / "libs", ec)) {
        log::debug("pkgindex modules: {} has no libs/, using {}",
                   derived.string(), primary.string());
        if (outHasLibs) *outHasLibs = true;
        return primary.string();
    }

    if (outHasLibs) *outHasLibs = false;
    return derived.string();
}

bool executing_provider_owns_no_version(
        const xvm::VersionDB& db,
        std::string_view target,
        std::string_view executingProvider) {
    if (executingProvider.empty()) return false;
    auto it = db.find(std::string(target));
    if (it == db.end()) return true;
    for (const auto& [_, data] : it->second.versions) {
        if (!data.bindingGroup || data.bindingGroup->provider.empty()) continue;
        if (data.bindingGroup->provider == executingProvider) return false;
    }
    return true;
}

std::string effective_store_name_(std::string_view namespaceName, std::string_view name) {
    return package_store_name(namespaceName, name);
}

std::string effective_store_name_(const PlanNode& node) {
    return effective_store_name_(node.namespaceName, node.name);
}

std::string effective_store_name_(const PackageMatch& match) {
    return effective_store_name_(match.namespaceName, match.name);
}

std::string version_namespace_(std::string_view namespaceName) {
    const auto& globalRepos = Config::global_index_repos();
    const bool isPrimary = !globalRepos.empty()
        && namespaceName == globalRepos.front().name;
    if (isPrimary || namespaceName.empty()) return {};
    return std::string(namespaceName);
}

std::string plan_key_(const PlanNode& node) {
    auto name = node.canonicalName.empty()
        ? canonical_package_name(node.namespaceName, node.name)
        : node.canonicalName;
    if (node.version.empty()) return name;
    return name + "@" + node.version;
}

std::filesystem::path data_root_for_(const std::filesystem::path& targetRoot) {
    if (targetRoot.filename() == "xpkgs") return targetRoot.parent_path();
    return targetRoot;
}

std::filesystem::path runtime_dir_(const PlanNode& node,
                                   const std::filesystem::path& fallbackDataDir) {
    auto targetRoot = node.storeRoot.empty() ? (fallbackDataDir / "xpkgs") : node.storeRoot;
    auto dataRoot = data_root_for_(targetRoot);
    return dataRoot / "runtimedir";
}

std::filesystem::path xpkg_snapshot_file_(const std::filesystem::path& installDir) {
    return installDir / ".xpkg.lua";
}

std::expected<void, std::string>
save_xpkg_snapshot_(const std::filesystem::path& sourcePkgFile,
                    const std::filesystem::path& installDir) {
    namespace fs = std::filesystem;
    std::error_code ec;

    if (sourcePkgFile.empty()
        || !fs::exists(sourcePkgFile, ec)
        || !fs::is_regular_file(sourcePkgFile, ec)) {
        return std::unexpected(
            std::format("xpkg snapshot source not found: {}", sourcePkgFile.string()));
    }

    fs::create_directories(installDir, ec);
    if (ec) {
        return std::unexpected(
            std::format("failed to create install dir for xpkg snapshot: {}", ec.message()));
    }

    auto snapshotFile = xpkg_snapshot_file_(installDir);
    ec.clear();
    if (fs::exists(snapshotFile, ec)) {
        ec.clear();
        if (fs::equivalent(sourcePkgFile, snapshotFile, ec) && !ec) return {};
    }

    ec.clear();
    fs::copy_file(sourcePkgFile, snapshotFile, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return std::unexpected(
            std::format("failed to save xpkg snapshot {}: {}",
                        snapshotFile.string(), ec.message()));
    }
    return {};
}

std::filesystem::path uninstall_xpkg_file_(const std::filesystem::path& indexPkgFile,
                                           const std::filesystem::path& installDir) {
    std::error_code ec;
    auto snapshotFile = xpkg_snapshot_file_(installDir);
    if (std::filesystem::exists(snapshotFile, ec)) {
        return snapshotFile;
    }
    return indexPkgFile;
}

bool is_archive_(const std::filesystem::path& path) {
    auto filename = path.filename().string();
    return filename.ends_with(".tar.gz")
        || filename.ends_with(".tar.xz")
        || filename.ends_with(".tar.bz2")
        || filename.ends_with(".tgz")
        || filename.ends_with(".zip");
}

std::string detect_arch_() {
    return std::string(xlings::platform::build_arch());
}

std::string default_res_server_() {
    auto server = Config::resource_server();
    if (!server.empty()) return server;
    return "https://github.com/xlings-res";
}

std::string build_xlings_res_url_with_server_(std::string_view server,
                                              std::string_view pkgName,
                                              std::string_view version,
                                              std::string_view platform) {
    auto ext = std::string(platform) == "windows" ? "zip" : "tar.gz";
    return std::format("{}/{}/releases/download/{}/{}-{}-{}-{}.{}",
                       server,
                       pkgName,
                       version,
                       pkgName,
                       version,
                       platform,
                       detect_arch_(),
                       ext);
}

std::string build_xlings_res_url_(std::string_view pkgName,
                                  std::string_view version,
                                  std::string_view platform) {
    return build_xlings_res_url_with_server_(default_res_server_(), pkgName, version, platform);
}

std::vector<std::string> build_xlings_res_fallback_urls_(std::string_view pkgName,
                                                         std::string_view version,
                                                         std::string_view platform) {
    auto selected = default_res_server_();
    auto candidates = Config::resource_servers_with_cross_region();
    std::vector<std::string> fallbacks;
    for (auto& server : candidates) {
        if (server != selected) {
            fallbacks.push_back(
                build_xlings_res_url_with_server_(server, pkgName, version, platform));
        }
    }
    return fallbacks;
}

std::expected<DownloadResource_, std::string> resolve_download_resource_(
        const mcpplibs::xpkg::PlatformMatrix& matrix,
        std::string_view name,
        std::string_view requestedVersion,
        std::string_view platform,
        std::string_view arch,
        std::string_view preferredMirror) {
    auto resolved = mcpplibs::xpkg::resolve_resource(matrix, {
        .name = std::string(name),
        .version = std::string(requestedVersion),
        .platform = std::string(platform),
        .arch = std::string(arch),
    });
    if (!resolved) return std::unexpected(resolved.error());

    DownloadResource_ result {
        .version = resolved->version,
        .url = resolved->url,
        .sha256 = resolved->sha256,
        .mirrors = resolved->mirrors,
        .useResFallbacks = resolved->kind
            == mcpplibs::xpkg::SourceKind::XlingsRes,
    };
    if (result.useResFallbacks) {
        result.url = build_xlings_res_url_(name, result.version, platform);
    }

    auto preferred = preferredMirror.empty()
        ? std::string_view{"GLOBAL"}
        : preferredMirror;
    if (auto it = result.mirrors.find(std::string(preferred));
            it != result.mirrors.end()) {
        result.url = it->second;
    }
    if (result.url.empty())
        return std::unexpected("resolved resource URL is empty");
    return result;
}

bool has_directory_entries_(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return false;
    return std::filesystem::directory_iterator(dir, ec) != std::default_sentinel;
}

bool stage_extracted_payload_(const std::filesystem::path& extractRoot,
                              const std::filesystem::path& installDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(extractRoot, ec) || !fs::is_directory(extractRoot, ec)) return false;

    std::vector<fs::path> entries;
    for (fs::directory_iterator it(extractRoot, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        entries.push_back(it->path());
    }
    if (ec || entries.empty()) return false;

    fs::path payloadRoot = extractRoot;
    if (entries.size() == 1 && fs::is_directory(entries.front(), ec) && !ec) {
        payloadRoot = entries.front();
    }

    if (fs::exists(installDir, ec)) {
        if (fs::is_empty(installDir, ec)) {
            ec.clear();
            fs::remove_all(installDir, ec);
            if (ec) return false;
        } else {
            return true;
        }
    }

    fs::create_directories(installDir.parent_path(), ec);
    if (ec) return false;

    if (payloadRoot != extractRoot) {
        fs::rename(payloadRoot, installDir, ec);
        if (!ec) return true;
        ec.clear();
    }

    fs::create_directories(installDir, ec);
    if (ec) return false;

    auto move_entry = [&](const fs::path& source) -> bool {
        auto dest = installDir / source.filename();
        fs::rename(source, dest, ec);
        if (!ec) return true;
        ec.clear();
        fs::copy(source, dest,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
        if (ec) return false;
        ec.clear();
        fs::remove_all(source, ec);
        return !ec;
    };

    if (payloadRoot == extractRoot) {
        for (auto& entry : entries) {
            if (!move_entry(entry)) return false;
        }
        return true;
    }

    std::vector<fs::path> payloadEntries;
    for (fs::directory_iterator it(payloadRoot, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        payloadEntries.push_back(it->path());
    }
    if (ec) return false;
    for (auto& entry : payloadEntries) {
        if (!move_entry(entry)) return false;
    }
    return true;
}

bool normalize_file_install_(const std::filesystem::path& installPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(installPath, ec) || !fs::is_regular_file(installPath, ec)) return true;

    auto parent = installPath.parent_path();
    auto fileName = installPath.filename();
    auto tempFile = parent / (fileName.string() + ".xlings.tmp");

    fs::rename(installPath, tempFile, ec);
    if (ec) return false;

    fs::create_directories(installPath, ec);
    if (ec) {
        ec.clear();
        fs::rename(tempFile, installPath, ec);
        return false;
    }

    fs::rename(tempFile, installPath / fileName, ec);
    if (!ec) return true;

    ec.clear();
    fs::copy_file(tempFile, installPath / fileName, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    ec.clear();
    fs::remove(tempFile, ec);
    return !ec;
}

std::filesystem::path current_workspace_config_path_() {
    if (Config::has_project_config()) {
        if (Config::project_subos_mode() == ProjectSubosMode::Named) {
            return Config::project_dir() / ".xlings" / "subos" / Config::project_subos_name() / ".xlings.json";
        }
        if (Config::project_subos_mode() == ProjectSubosMode::Anonymous) {
            return Config::project_state_path();
        }
        return Config::project_state_path();
    }
    return Config::paths().homeDir / "subos" / Config::paths().activeSubos / ".xlings.json";
}

xvm::SubosWorkspace load_workspace_file_(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    try {
        auto content = platform::read_file_to_string(path.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        if (json.is_discarded() || !json.is_object()) return {};
        if (!json.contains("workspace") || !json["workspace"].is_object()) return {};
        return xvm::subos_workspace_from_json(json["workspace"]);
    } catch (...) {
        return {};
    }
}

std::vector<std::filesystem::path> workspace_config_paths_for_scope_(PackageScope scope) {
    namespace fs = std::filesystem;
    (void)scope;
    std::vector<fs::path> paths;
    auto push = [&](fs::path p) {
        if (p.empty()) return;
        if (std::ranges::find(paths, p) != paths.end()) return;
        paths.push_back(std::move(p));
    };

    if (Config::has_project_config()) {
        auto projectRoot = Config::project_dir();
        if (!projectRoot.empty()) {
            push(Config::project_manifest_path());
            push(Config::project_state_path());
            auto subosRoot = projectRoot / ".xlings" / "subos";
            std::error_code ec;
            if (fs::exists(subosRoot, ec) && fs::is_directory(subosRoot, ec)) {
                for (auto& entry : platform::dir_entries(subosRoot)) {
                    if (entry.is_directory()) {
                        push(entry.path() / ".xlings.json");
                    }
                }
            }
        }
    }

    auto subosRoot = Config::paths().homeDir / "subos";
    std::error_code ec;
    if (fs::exists(subosRoot, ec) && fs::is_directory(subosRoot, ec)) {
        for (auto& entry : platform::dir_entries(subosRoot)) {
            if (entry.is_directory()) {
                auto name = entry.path().filename().string();
                if (name != "current") {
                    push(entry.path() / ".xlings.json");
                }
            }
        }
    }
    return paths;
}

bool is_version_referenced_anywhere_(PackageScope scope, const std::string& target, const std::string& version, const std::filesystem::path& excludePath) {
    std::error_code ec;
    auto excludeCanonical = excludePath.empty() ? std::filesystem::path{} : std::filesystem::weakly_canonical(excludePath, ec);
    for (auto& configPath : workspace_config_paths_for_scope_(scope)) {
        auto canonical = std::filesystem::weakly_canonical(configPath, ec);
        if (!excludeCanonical.empty() && !ec && canonical == excludeCanonical) {
            continue;
        }
        // 0.4.19+: a payload is "referenced" by a subos if EITHER its
        // active version equals `version` OR `version` appears in that
        // subos's installed[] set. Pre-0.4.19 the active-only check was
        // sufficient because installed[] didn't exist; with C2 schema a
        // user can `xlings remove` the active version while still having
        // other installed versions, and a payload that only appears in
        // some other subos's installed[] (but not its active) must still
        // pin its xpkgs/ directory against GC.
        //
        // Stored values are namespaced (`ns:1.0` for non-primary repos,
        // bare for primary). New removal callers pass the exact stored key;
        // retain bare matching here for legacy workspace files.
        auto matches = [&](std::string_view stored) {
            return stored == version
                || xvm::strip_namespace(std::string(stored)) == version;
        };
        auto sws = load_workspace_file_(configPath);
        if (auto it = sws.active.find(target);
            it != sws.active.end() && matches(it->second)) return true;
        if (auto it = sws.installed.find(target); it != sws.installed.end()) {
            for (auto& v : it->second) {
                if (matches(v)) return true;
            }
        }
    }
    return false;
}

void remove_target_shims_(const std::string& target, const std::string& version) {
    namespace fs = std::filesystem;
    auto db = Config::versions();
    auto* vinfo = xvm::get_vinfo(db, target);
    auto& binDir = Config::paths().binDir;
    std::error_code ec;

    // A program's shim is a generic dispatcher to the bootstrap; the actual
    // version it routes to is the workspace pointer (read at runtime). So
    // the shim is only stale when **the current subos** has dropped its
    // last version of the name. With surviving versions still in this
    // subos's installed[], the shim must stay — auto-fallback in
    // detach_current_subos_ updates `active` to the highest remaining
    // version and the shim resolves through that.
    //
    // 0.4.19+: this check is now scoped to the **current subos's
    // installed[]**, not the global versions DB. Pre-0.4.19 the gate
    // was `is_last_version_for(global)`, which under-removed: subos A
    // could drain its installed[] for a target while subos B still had
    // some other version of it, leaving a stale shim in A's bin/. With
    // C2 schema each subos's bin/ is independent — so is each subos's
    // shim-lifetime decision.
    //
    // Caller contract: detach_current_subos_ has already pruned
    // `version` from this subos's installed[] before calling us. So
    // checking "is the post-removal installed[] empty?" gives the
    // correct answer.
    const auto& wsi = Config::workspace_installed();
    auto subos_has_any_version_of = [&](const std::string& name) {
        auto it = wsi.find(name);
        return it != wsi.end() && !it->second.empty();
    };

    auto mainName = (vinfo && !vinfo->filename.empty()) ? vinfo->filename : target;
    if (!subos_has_any_version_of(target)) {
        auto mainShim = binDir / mainName;
        if (fs::exists(mainShim, ec) || fs::is_symlink(mainShim, ec)) {
            ec.clear();
            fs::remove(mainShim, ec);
        }
    }

    if (!vinfo) return;
    for (auto& [bindingName, vermap] : vinfo->bindings) {
        auto vit = vermap.find(version);
        if (vit == vermap.end()) continue;
        if (subos_has_any_version_of(bindingName)) continue;  // still in use here
        auto bindPath = binDir / bindingName;
        ec.clear();
        if (fs::exists(bindPath, ec) || fs::is_symlink(bindPath, ec)) {
            ec.clear();
            fs::remove(bindPath, ec);
        }
    }
}

void detach_current_subos_(const std::string& target, const std::string& version, bool persist) {
    auto& ws = Config::workspace_mut();
    auto& wsi = Config::workspace_installed_mut();

    auto matches = [&](std::string_view stored) {
        return stored == version;
    };

    // Step 1 (0.4.19+): always drop `version` from this subos's
    // installed[] set, even when it isn't the currently active version.
    // The set is the per-subos opt-in list; an explicit `xlings remove
    // foo@X` always means "this subos no longer wants X".
    bool installedChanged = false;
    if (auto it = wsi.find(target); it != wsi.end()) {
        auto& list = it->second;
        auto pred = [&](const std::string& v) { return matches(v); };
        auto erased = std::remove_if(list.begin(), list.end(), pred);
        if (erased != list.end()) {
            list.erase(erased, list.end());
            installedChanged = true;
            if (list.empty()) wsi.erase(it);
        }
    }

    // Step 2: if `version` was the active pointer, do the actual subos
    // teardown (remove headers / libs / shims) and then either fall
    // back to another installed version or clear the pointer entirely.
    auto wit = ws.find(target);
    bool wasActive = wit != ws.end() && matches(wit->second);

    if (wasActive) {
        auto db = Config::versions();
        auto sysroot_include = Config::paths().subosDir / "usr" / "include";
        auto sysroot_lib = Config::paths().libDir;
        // The whole release's headers, not just whatever single directory
        // landed in this entry's `includedir`: a release declaring more than
        // one header directory would otherwise leave all but the last behind
        // in the sysroot, pointing into a payload that is being deleted.
        for (const auto& asset :
                 xvm::group_header_assets(db, target, version)) {
            xvm::remove_headers(asset, sysroot_include);
        }
        if (const auto placement =
                xvm::library_placement(db, target, version);
            !placement.empty()) {
            xvm::remove_library(placement.name, sysroot_lib);
        }
        if (const auto file = xvm::file_placement(db, target, version);
            !file.empty()) {
            xvm::remove_asset(Config::paths().subosDir / file.destination);
        }
        remove_target_shims_(target, version);

        // Auto-fallback: when the user removes the active version but
        // still has other versions installed in this subos, switch
        // active to the highest remaining (installed[] is stored
        // sorted ascending by subos_workspace_to_json — `back()` is
        // the lexicographically-highest, which approximates "newest"
        // for typical semver-ish version strings).
        //
        // The shim for the fallback version already exists from its
        // earlier install — only the active pointer needs updating;
        // headers/libs are NOT auto-relinked (lazy: next call to
        // `xlings use` would do that explicitly). This avoids a
        // surprise sysroot reshuffle on every remove.
        if (auto sit = wsi.find(target); sit != wsi.end() && !sit->second.empty()) {
            wit->second = sit->second.back();
        } else {
            ws.erase(wit);
        }
    }

    if (persist && (wasActive || installedChanged)) {
        Config::save_workspace();
    }
}

bool apply_subos_env_ops_(const std::vector<mcpplibs::xpkg::XvmOp>& operations,
                          const PlanNode& node) {
    namespace mf = xlings::subos::manifest;

    std::vector<const mcpplibs::xpkg::XvmOp*> declarations;
    for (const auto& op : operations)
        if (op.op == "subos_env") declarations.push_back(&op);
    if (declarations.empty()) return true;

    const auto canonical = node.canonicalName.empty()
        ? canonical_package_name(node.namespaceName, node.name)
        : node.canonicalName;

    const auto subosDir = Config::xvm_artifact_subos_dir();
    auto docPath = mf::config_path(subosDir);
    auto doc = mf::read_document(subosDir);
    if (!doc) {
        // Absent is recoverable (an old subos predates the block); unreadable
        // is not, and must not be papered over by starting from {} -- that
        // would discard a workspace we never managed to parse.
        std::error_code ec;
        if (std::filesystem::exists(docPath, ec)) {
            log::error("[xim] {} is not readable JSON; refusing to record "
                       "subos env declarations for {}@{}",
                       docPath.string(), canonical, node.version);
            return false;
        }
        nlohmann::json fresh;
        fresh["workspace"] = nlohmann::json::object();
        fresh[std::string(mf::BLOCK)] = mf::make_block(
            mf::DEFAULT_RUNTIME, std::format("xlings {}", Info::VERSION),
            platform::host_glibc_version());
        doc = std::move(fresh);
    }
    if (!doc->contains(std::string(mf::BLOCK))
        || !doc->at(std::string(mf::BLOCK)).is_object()) {
        (*doc)[std::string(mf::BLOCK)] = mf::make_block(
            mf::DEFAULT_RUNTIME, std::format("xlings {}", Info::VERSION),
            platform::host_glibc_version());
    }

    // C1 -- the subos layer's "exactly one", for the case where nothing else
    // can supply it.
    //
    // The store keeps many versions by design; each consumer freezes one into
    // its own RPATH; the subos in between holds exactly one. Nothing enforced
    // that, so installing a second version appended a second provider section
    // and BOTH contributed. Measured: mesa@25.0.7 and mesa@25.0.7.1 both bound
    // in one subos, both on __EGL_VENDOR_LIBRARY_DIRS, EGL enumerating the
    // device twice, doctor silent. The two records agreed -- on an answer the
    // model forbids.
    //
    // The live version is normally decided by xvm and read back at activation
    // (manifest::select_effective), so a package WITH an active version needs
    // nothing here: installing a second version leaves the first live until
    // `xlings use` says otherwise, which is what install and use have meant
    // since 2026.7.31. Superseding here as well would make install a second
    // selector -- another answerer to a question that has one, which is the
    // defect, not the fix.
    //
    // What is left is the case where xvm has no answer at all. Measured: a bare
    // `xvm.add(name)` DOES record an active version, so an ordinary install
    // never reaches this branch -- it covers a manifest whose workspace record
    // was lost while its declarations survived (a payload pruned out from under
    // it, a subos config copied between homes, a hand edit). There every
    // provider contributes, so a second version really does double the
    // environment, and the only moment anything can choose is now, while a
    // human is naming a version. So: supersede only when nothing else can.
    //
    // Before the ownership checks below, deliberately: those return false on a
    // malformed declaration, and unbinding first would leave the subos holding
    // neither version.
    std::vector<std::string> superseded;
    {
        const auto active = mf::active_versions(*doc);
        std::set<std::string> ownNames;
        for (const auto* op : declarations) {
            if (!mf::is_binding(op->binding)) continue;
            ownNames.insert(std::string(mf::binding_name(op->binding)));
        }
        for (const auto& name : ownNames) {
            if (active.contains(name)) continue;   // xvm decides, not install
            for (const auto& existing : mf::providers_named(*doc, name)) {
                const bool sameBinding = std::ranges::any_of(
                    declarations, [&](const auto* op) {
                        return op->binding == existing;
                    });
                if (sameBinding) continue;     // re-install of the same version
                superseded.push_back(existing);
            }
        }
    }

    bool changed = false;
    // Collected per binding first, so each section can be written as a whole.
    // std::map, not unordered: the write order of sections shows up in the file
    // a user reads and in diffs of it.
    std::map<std::string, std::vector<mf::EnvDecl>> perBinding;
    for (const auto* op : declarations) {
        if (!mf::is_binding(op->binding)) {
            log::error("[xim] {}@{} declared env '{}' with binding '{}' "
                       "(expected <name>@<version>); nothing recorded",
                       canonical, node.version, op->var, op->binding);
            return false;
        }
        const auto owner = mf::binding_name(op->binding);
        if (owner != node.name && owner != canonical) {
            log::error("[xim] {}@{} declared env '{}' for '{}', which it does "
                       "not own; nothing recorded",
                       canonical, node.version, op->var, op->binding);
            return false;
        }
        // B5 / B3: a declaration that can put our code into someone else's
        // process is a privileged operation, and install is the only moment a
        // human is watching. Reported, not refused -- nvidia-gl-host-link
        // legitimately needs one today, and refusing would leave the user with
        // no GPU and no way forward. What must not happen is that it lands
        // silently: the last one did, and `xlings subos use` returned a
        // /bin/bash that died of SIGSEGV before printing a character.
        //
        // Default-deny by variable name (manifest::names_only_data), so a
        // variable nobody has classified reads as privileged. See AD-3.
        if (mf::is_privileged_env(op->var, op->value)) {
            log::warn("[xim] {}@{} declares {} = {}", canonical, node.version,
                      op->var, op->value);
            log::warn("      This variable can load code from our payload into "
                      "processes we do not own, including host binaries running "
                      "under the host loader. Prefer RPATH on the consumer; use "
                      "this only where RPATH cannot reach (a library that opens "
                      "its plugins itself), and say why in the recipe.");
        }
        perBinding[op->binding].push_back(
            {.var = op->var, .op = op->mode, .value = op->value});
    }

    // Written as a REPLACEMENT of each binding's section, not a union with it.
    //
    // `add_env` is idempotent per (var, op, value) triple, which keeps a re-run
    // of the same declarations steady and silently accumulates a second
    // generation when the recipe's declarations CHANGE. Measured while moving
    // mesa's discovery paths from `${pkgdir}` to `${subosdir}`: the section held
    // both, so LIBGL_DRIVERS_PATH resolved to the new subos directory followed
    // by a stale payload path and __EGL_VENDOR_LIBRARY_DIRS listed one directory
    // twice -- with nothing reporting it, and the stale entry becoming a dead
    // directory as soon as that payload is collected.
    //
    // Only bindings that declared something THIS run are touched. A binding
    // belonging to another package keeps its section; that is the same
    // ownership-by-binding rule that lets uninstall need no recipe cleanup.
    //
    // A package that stops declaring env entirely is NOT handled here and must
    // not be: `declarations` is empty then, this function returns before the
    // ownership checks, and the stale section is removed by uninstall or by
    // `superseded` above. Clearing on an empty batch would delete a live
    // section every time an unrelated dependent's install re-ran a hook that
    // happens to declare nothing.
    for (const auto& [binding, decls] : perBinding) {
        changed |= mf::set_env_section(*doc, binding, decls);
    }

    // Only now that every declaration passed. Reported at info level, not
    // debug: a version silently leaving a subos is exactly the kind of state
    // change users later cannot account for.
    for (const auto& binding : superseded) {
        if (!mf::remove_provider(*doc, binding)) continue;
        changed = true;
        log::info("[xim] subos '{}' now holds {}@{}; unbound {} (it declares no "
                  "xvm version, so nothing else could say which is live)",
                  Config::paths().activeSubos.empty()
                      ? std::string{"default"} : Config::paths().activeSubos,
                  canonical, node.version, binding);
        log::info("      Its payload is untouched -- the store keeps every "
                  "installed version. To keep both active, use separate subos.");
    }

    if (!changed) return true;

    if (auto findings = mf::validate_block(*doc); !findings.empty()) {
        log::error("[xim] recording env declarations for {}@{} would leave an "
                   "invalid subos manifest: {}", canonical, node.version,
                   mf::describe(findings.front().kind));
        return false;
    }

    try {
        platform::write_string_to_file(docPath.string(), doc->dump(2));
    } catch (const std::exception& e) {
        log::error("[xim] failed to write {}: {}", docPath.string(), e.what());
        return false;
    }
    log::debug("[xim] recorded {} subos env declaration(s) for {}@{}",
               declarations.size(), canonical, node.version);
    return true;
}

bool process_xvm_operations_(const PlanNode& node,
                             const std::filesystem::path& dataDir,
                             mcpplibs::xpkg::PackageExecutor& executor,
                             bool useAfterInstall) {
    auto xvm_ops = executor.xvm_operations();

    // Before the early return below. A package that declares only env and
    // registers nothing with xvm has an empty registration batch, and would
    // otherwise install cleanly with its declarations dropped on the floor --
    // the exact shape where "nothing happened" and "it worked" look alike.
    if (!apply_subos_env_ops_(xvm_ops, node)) return false;

    auto& paths = Config::paths();
    auto& scopedDb = Config::versions_mut();
    auto& scopedWorkspace = Config::workspace_mut();
    auto& scopedInstalled = Config::workspace_installed_mut();
    const auto artifactSubosDir =
        Config::xvm_artifact_subos_dir();
    const auto artifactBinDir = artifactSubosDir / "bin";
    const auto sysroot_lib = artifactSubosDir / "lib";
    const auto sysroot_include =
        artifactSubosDir / "usr" / "include";

    // Locate xlings binary for shim creation
#ifdef _WIN32
    auto xlings_bin = paths.homeDir / "bin" / "xlings.exe";
    constexpr std::string_view shim_ext = ".exe";
#else
    auto xlings_bin = paths.homeDir / "bin" / "xlings";
    constexpr std::string_view shim_ext = "";
#endif
    if (!std::filesystem::exists(xlings_bin))
        xlings_bin = paths.homeDir / "xlings";

    // Primary repo versions retain their historical bare keys. Other repos
    // retain their namespace so install and exact removal resolve identically.
    const auto versionNamespace = version_namespace_(node.namespaceName);

    auto registration = normalize_xpkg_registration_plan(
        node, xvm_ops, versionNamespace, dataDir, useAfterInstall);
    if (!registration) {
        log::warn(
            "xvm registration normalization failed for {}@{}: {} "
            "(operation={}, target='{}', version='{}')",
            node.canonicalName.empty()
                ? canonical_package_name(node.namespaceName, node.name)
                : node.canonicalName,
            node.version,
            registration.error().message,
            registration.error().operationIndex,
            registration.error().target,
            registration.error().version);
        return false;
    }
    const bool hasRemoval = std::ranges::any_of(
        xvm_ops,
        [](const auto& operation) {
            return operation.op == "remove"
                || operation.op == "remove_all";
        });
    if (!hasRemoval
        && registration->batch.nodes.empty()
        && registration->batch.headers.empty()
        && registration->effects.empty()) {
        return true;
    }

    xvm::RemovalContext removalContext;
    if (hasRemoval) {
        auto snapshot = snapshot_xpkg_removal_context(
            scopedDb, scopedWorkspace, xvm_ops,
            registration->batch.provider,
            registration->batch.providerVersion);
        if (!snapshot) {
            // Selection runs before any mutation, so nothing has moved yet.
            log::error("{}", xvm::render(
                xvm::describe(
                    snapshot.error(),
                    std::format("{}@{}",
                                registration->batch.provider,
                                registration->batch.providerVersion)),
                /*nothingChanged=*/true));
            return false;
        }
        removalContext = std::move(*snapshot);
    }

    const auto dbBeforeRemoval = scopedDb;
    const auto workspaceBeforeRemoval = scopedWorkspace;
    auto metadata = apply_xpkg_xvm_metadata_batch(
        scopedDb,
        scopedWorkspace,
        scopedInstalled,
        xvm_ops,
        removalContext,
        *registration);
    if (!metadata) {
        const auto owner = std::format("{}@{}",
                                       registration->batch.provider,
                                       registration->batch.providerVersion);
        // apply_xpkg_xvm_metadata_batch works on copies and only swaps them
        // in on success, so a failure here has left the stored state alone.
        if (std::holds_alternative<xvm::RemovalError>(
                metadata.error())) {
            log::error("{}", xvm::render(
                xvm::describe(
                    std::get<xvm::RemovalError>(metadata.error()), owner),
                /*nothingChanged=*/true));
        } else {
            log::error("{}", xvm::render(
                xvm::describe(
                    std::get<xvm::RegistrationError>(metadata.error()), owner),
                /*nothingChanged=*/true));
        }
        return false;
    }

    cleanup_removed_xvm_library_artifacts(
        sysroot_lib,
        dbBeforeRemoval,
        scopedDb,
        metadata->removal);

    // Removal takes the removed release's headers out of the sysroot. When
    // it then falls back to a surviving release, nothing put that release's
    // headers back -- the sysroot ended up with none at all, and `xlings use`
    // could not repair it because switching to an already-active version is
    // a no-op. Re-materialize whatever the fallback made active.
    for (const auto& [target, version] : scopedWorkspace) {
        auto beforeIt = workspaceBeforeRemoval.find(target);
        if (beforeIt != workspaceBeforeRemoval.end()
            && beforeIt->second == version) {
            continue;  // unchanged
        }
        auto infoIt = scopedDb.find(target);
        if (infoIt == scopedDb.end()) continue;
        auto dataIt = infoIt->second.versions.find(version);
        if (dataIt == infoIt->second.versions.end()) continue;
        for (const auto& asset :
                 xvm::group_header_assets(scopedDb, target, version)) {
            xvm::install_headers(asset, sysroot_include);
        }
        if (const auto placement =
                xvm::library_placement(scopedDb, target, version);
            !placement.empty()) {
            xvm::place_library(placement.source, placement.name, sysroot_lib);
        }
        if (const auto file = xvm::file_placement(scopedDb, target, version);
            !file.empty()) {
            xvm::place_asset(file.source,
                             Config::paths().subosDir / file.destination);
        }
    }

    // Written after the batch, not inside it: the batch owns the group model
    // and must not be taught a legacy field. This runs against the committed
    // scopedDb, so a package whose own name is not a registered target simply
    // gets no marker rather than a phantom entry.
    if (const auto headerVersion =
            xvm::make_ns_version(versionNamespace, node.version);
        attach_legacy_header_dir(
            scopedDb, node.name, headerVersion, metadata->effects) == 0) {
        const bool declaresHeaders = std::ranges::any_of(
            metadata->effects, [](const XpkgFilesystemEffect& effect) {
                return effect.kind == XpkgFilesystemEffectKind::InstallHeaders;
            });
        if (declaresHeaders) {
            log::debug(
                "[xim] headers declared but '{}@{}' is not a registered "
                "target; `xlings use` will not swap them",
                node.name, headerVersion);
        }
    }

    for (const auto& effect : metadata->effects) {
        auto resolved = resolve_xpkg_filesystem_effect(
            scopedDb, scopedWorkspace, effect);
        if (!resolved) {
            log::warn(
                "validated xvm effect target disappeared or changed kind: "
                "{}@{}",
                effect.target, effect.version);
            continue;
        }
        if (resolved->kind
            == XpkgFilesystemEffectKind::InstallHeaders) {
            if (!resolved->active) {
                // Installing a non-active version must not disturb the
                // sysroot: `xlings use` is what moves headers, and it cannot
                // undo this because switching to an already-active version
                // is a no-op.
                log::debug("[xim] headers for {}@{} not installed: not the "
                           "active version", resolved->target, resolved->version);
                continue;
            }
            xvm::install_headers(
                resolved->sourceDir, sysroot_include);
            continue;
        }
        if (resolved->kind
            == XpkgFilesystemEffectKind::RemoveHeaders) {
            xvm::remove_headers(
                resolved->sourceDir, sysroot_include);
            continue;
        }
        if (resolved->kind
            == XpkgFilesystemEffectKind::ProgramShim) {
            // A shim is only meaningful for a name that has an active
            // version -- that is what shim_dispatch resolves against, and
            // without one the file can only ever print "no active version
            // of 'X' in current subos". Writing it anyway is what produced
            // doctor's `orphan shim`, an error the user could not fix:
            // `--fix` deleted the file and the next install recreated it.
            //
            // Registration deliberately withholds activation from a release
            // whose group already has an active member (see
            // registration.cppm, `activateGroup`), so every name that is new
            // in that release lands here. The sibling effects below already
            // guard on activation; this one did not.
            //
            // The question is about the NAME, not this version: a second
            // version of an active program must not delete or skip the shim
            // its active sibling needs. And a name activated later still
            // gets its file -- `cmd_use` creates shims for every member of
            // the release it switches to (xvm/commands.cppm).
            const auto activeIt = scopedWorkspace.find(resolved->target);
            const bool nameHasActiveVersion =
                activeIt != scopedWorkspace.end()
                && !activeIt->second.empty();
            if (!nameHasActiveVersion) {
                log::debug(
                    "[xim] shim for {}@{} not created: no active version of "
                    "'{}' in this subos",
                    resolved->target, effect.version, resolved->target);
                continue;
            }
            if (std::filesystem::exists(xlings_bin)) {
                std::string shimName = resolved->target;
                if (!shim_ext.empty() && !shimName.ends_with(shim_ext)) {
                    shimName += shim_ext;
                }
                std::filesystem::create_directories(artifactBinDir);
                xself::create_shim(
                    xlings_bin, artifactBinDir / shimName);
                if (artifactBinDir
                    != Config::global_subos_bin_dir()) {
                    common::mirror_shim_to_global_bin(
                        xlings_bin, shimName);
                }
            }

            if (resolved->active
                && xvm::is_xlings_binary(resolved->target)
                && std::filesystem::exists(xlings_bin)
                && !resolved->path.empty()
                && !resolved->sourceName.empty()) {
                auto activeName = resolved->sourceName;
                if (!shim_ext.empty()
                    && !activeName.ends_with(shim_ext)) {
                    activeName += shim_ext;
                }
                const auto activeBin =
                    std::filesystem::path(resolved->path)
                    / activeName;
                if (std::filesystem::exists(activeBin)) {
                    // The same writer `xlings use xlings <v>` goes through.
                    // Two independent replacements of the one file every shim
                    // dispatches through is how a home ends up running a
                    // client nobody chose -- see entry_binary.cppm.
                    entry_binary::replace_with(
                        activeBin, xlings_bin,
                        std::format("{}@{}", resolved->target, effect.version),
                        effect.version);
                }
                xself::compat::v0_4_8::cleanup_legacy_alias_shims(
                    artifactBinDir, xlings_bin);
            }
            continue;
        }
        if (resolved->kind == XpkgFilesystemEffectKind::FileAsset) {
            if (!resolved->active) {
                log::debug("[xim] file asset {}@{} not placed: not the "
                           "active version", resolved->target,
                           resolved->version);
                continue;
            }
            if (const auto file = xvm::file_placement(
                    scopedDb, resolved->target, resolved->version);
                !file.empty()) {
                xvm::place_asset(file.source,
                                 artifactSubosDir / file.destination);
            } else {
                log::warn("[xim] file asset {}@{} declares no usable "
                          "destination; nothing placed",
                          resolved->target, resolved->version);
            }
            continue;
        }
        if (resolved->kind != XpkgFilesystemEffectKind::Library
            || resolved->path.empty()) {
            continue;
        }
        if (!resolved->active) {
            // Installing a version that does not become active must not
            // disturb the sysroot. `InstallHeaders` has been gated this way
            // since 0.4.70; `Library` was not, so installing a second version
            // of a package overwrote the active version's library while its
            // headers stayed put -- the sysroot then held a library from one
            // release beside headers from another, which compiles and fails
            // at run time. `xlings use` is what moves libraries.
            log::debug("[xim] library {}@{} not placed: not the active "
                       "version", resolved->target, resolved->version);
            continue;
        }

        const auto source =
            std::filesystem::path(resolved->path)
            / resolved->sourceName;
        const auto destination =
            sysroot_lib / resolved->destinationName;
        std::filesystem::create_directories(sysroot_lib);
        std::error_code ec;
        if (std::filesystem::exists(destination, ec)
            || std::filesystem::is_symlink(destination, ec)) {
            std::filesystem::remove(destination, ec);
        }
        ec.clear();
        if (std::filesystem::exists(source, ec)) {
            std::filesystem::create_symlink(
                source, destination, ec);
        }
    }

    cleanup_removed_xvm_program_artifacts(
        artifactBinDir,
        dbBeforeRemoval,
        scopedDb,
        scopedInstalled,
        metadata->removal);

    // Announce any owner-less entry this install took over.
    //
    // Adoption replaces the contents recorded for a name@version that an
    // older client wrote without ownership (xvm/registration.cppm). It is the
    // right thing to do — the refusal it replaced left the user with no exit
    // (#422) — but it is still a payload changing behind a name, so it is
    // said out loud rather than applied silently.
    for (const auto& member : metadata->registered) {
        if (!member.adoptedLegacy) continue;
        log::warn("[xvm] adopted a pre-ownership registration: {}@{} ('{}' changed)",
                  member.target, member.version, member.adoptedLegacyField);
    }

    // Announce anything the batch left behind rather than refused over.
    //
    // These are names an older client registered for this same payload on a
    // platform whose recipe produces them and this one's does not (a Windows
    // llvm leaves `cl`, `lib`, `link`, `rc`). They keep their record and lose
    // their edge to this release; `self doctor` reports them and can prune
    // them. Silence here would make a release quietly shed members.
    for (const auto& member : metadata->detachedLegacy) {
        log::warn("[xvm] detached a legacy entry this platform no longer "
                  "registers: {} (run `xlings self doctor --fix` to clear it)",
                  xvm::display_coordinate(member.target, member.version));
    }

    if (!metadata->removal.removed.empty()
        || !metadata->registered.empty()) {
        Config::save_versions();
        Config::save_workspace();
    }
    return true;
}

bool run_config_hook_(const PlanNode& node, const std::filesystem::path& dataDir, mcpplibs::xpkg::PackageExecutor& executor, mcpplibs::xpkg::ExecutionContext& ctx, std::function<void(const InstallStatus&)> onStatus, bool useAfterInstall, std::string* failureMessage) {
    if (!executor.has_hook(mcpplibs::xpkg::HookType::Config)) return true;
    if (onStatus) {
        onStatus({ node.name, InstallPhase::Configuring, 0.8f, "" });
    }
    ScopedCurrentDir_ configCwd(ctx.install_dir);
    auto hookResult = executor.run_hook(mcpplibs::xpkg::HookType::Config, ctx);
    if (!hookResult.success) {
        if (failureMessage) {
            *failureMessage = format_hook_failure("config", hookResult);
        }
        return false;
    }
    if (!process_xvm_operations_(node, dataDir, executor, useAfterInstall)) {
        if (failureMessage) *failureMessage = "config hook failed";
        return false;
    }
    return true;
}

std::vector<std::string> unfulfilled_program_promises_(const InstallPlan& plan) {
    std::vector<std::string> broken;
    // By value, per Config::versions()'s contract -- it merges global and
    // project state into a fresh map, so a reference into the temporary would
    // dangle at the end of the full expression.
    const auto db = Config::versions();

    for (const auto& node : plan.nodes) {
        // Build-only deps are intentionally never registered or shimmed.
        if (node.kind == DepKind::Build) continue;
        if (node.programs.empty()) continue;

        const bool anyRegistered = std::ranges::any_of(
            node.programs,
            [&](const std::string& prog) { return xvm::has_target(db, prog); });
        if (anyRegistered) continue;

        std::string names;
        for (const auto& prog : node.programs) {
            if (!names.empty()) names += ", ";
            names += prog;
        }
        log::error("{} installed but registered none of the programs it "
                   "declares: {}", node.name, names);
        broken.push_back(node.name);
    }
    return broken;
}

}

}
