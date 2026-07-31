export module xlings.core.xim.installer;

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.compat;
import mcpplibs.xpkg.executor;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.xim.payload;
import xlings.core.xim.index;
import xlings.core.xim.catalog;
import xlings.core.xim.resolver;
import xlings.core.xim.downloader;
import xlings.core.log;
import xlings.platform;
import xlings.core.config;
import xlings.libs.json;
import xlings.core.common;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.bindings;
import xlings.core.xvm.removal;
import xlings.core.xvm.registration;
import xlings.core.xvm.errors;
import xlings.core.xvm.commands;
import xlings.core.xvm.shim;
import xlings.core.xim.libxpkg.types.script;
import xlings.core.xim.libxpkg.types.subos;
import xlings.runtime.cancellation;

export namespace xlings::xim {

enum class XpkgRegistrationErrorKind {
    InvalidVersion,
    InvalidBinding,
    ConflictingEnvironment,
};

struct XpkgRegistrationError {
    XpkgRegistrationErrorKind kind {
        XpkgRegistrationErrorKind::InvalidVersion
    };
    std::size_t operationIndex { 0 };
    std::string target;
    std::string version;
    std::string message;
};

enum class XpkgFilesystemEffectKind {
    ProgramShim,
    Library,
    FileAsset,
    InstallHeaders,
    RemoveHeaders,
};

struct XpkgFilesystemEffect {
    XpkgFilesystemEffectKind kind {
        XpkgFilesystemEffectKind::ProgramShim
    };
    std::string target;
    std::string version;
    std::string sourceDir;
};

struct XpkgResolvedFilesystemEffect {
    XpkgFilesystemEffectKind kind {
        XpkgFilesystemEffectKind::ProgramShim
    };
    std::string target;
    std::string version;
    std::string sourceDir;
    std::string path;
    std::string sourceName;
    std::string destinationName;
    bool active { false };
};

struct XpkgRegistrationPlan {
    xvm::RegistrationBatch batch;
    std::vector<XpkgFilesystemEffect> effects;
};

// The highest xpackage spec revision this build understands.
//
// Bump only together with the code that implements the new revision.
inline constexpr int max_supported_xpkg_spec = 2;

struct XpkgSpecSupport {
    bool supported { true };
    // 0 when the declared value could not be read as a revision at all.
    int  declared { 1 };
};

// Decide whether a recipe's `spec` describes semantics this build implements.
//
// Reading a spec we do not implement is not a missing feature, it is a
// different install than the author wrote. spec "2" is the case in point: it
// made `archs` fail-closed, so a client that ignores the field does not skip
// a nicety -- it installs the wrong architecture without a word. Refusing is
// the only answer that cannot silently produce the wrong bytes.
//
// Absent is V1: the field predates being meaningful and "" has always meant
// V1. Unreadable is refused rather than assumed -- a value we cannot parse
// (say a future "2.1") is precisely the case where guessing is unsafe.
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

using XpkgXvmMetadataError = std::variant<
    xvm::RemovalError,
    xvm::RegistrationError>;

struct XpkgXvmMetadataResult {
    xvm::RemovalBatchResult removal;
    std::vector<xvm::RegisteredMember> registered;
    // Legacy component members this batch could not re-register and detached
    // instead of refusing over. See apply_registration_batch.
    std::vector<xvm::DetachedLegacyMember> detachedLegacy;
    std::vector<XpkgFilesystemEffect> effects;
};

// Keep the release readable by an older xlings.
//
// Materialization now resolves headers through `xvm::group_header_assets`,
// which reads the group's declared assets from the root and only falls back
// to this field when there are none. So this is no longer what makes
// `xlings use` move headers -- the group assets are.
//
// It is still written, for one reason: 0.4.69 knows nothing about
// `bindingHeaders` and switches headers by reading `includedir` alone.
// Dropping it would make a downgrade from 0.4.70 silently stop moving
// headers, and the release notes promise the rollback is safe. It records
// only the last of the declared directories, which is exactly the limitation
// 0.4.69 had before -- a downgrade gets the old behavior back, not a worse
// one.
//
// One deliberate difference from the pre-batch behavior: this never brings a
// target or a version into existence. The old code indexed both maps with
// operator[], so a `headers` op from a package that registers no target of
// its own materialized an entry with no path and no kind — precisely the
// phantom state the binding-group work exists to prevent.
//
// Returns the number of entries updated (0 or 1), so callers can log the
// no-op case instead of assuming it worked.
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
snapshot_xpkg_removal_context(
        const xvm::VersionDB& db,
        const xvm::Workspace& workspace,
        const std::vector<mcpplibs::xpkg::XvmOp>& operations,
        const std::string& executingProvider,
        const std::string& executingProviderVersion,
        const std::string& preferredTarget = {},
        const std::string& preferredVersion = {}) {
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
apply_xpkg_removal_operations(
        xvm::VersionDB& db,
        xvm::Workspace& workspace,
        xvm::WorkspaceInstalled& installed,
        const std::vector<mcpplibs::xpkg::XvmOp>& operations,
        const xvm::RemovalContext& context,
        const xvm::RemovalBatchOptions& options = {}) {
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
apply_xpkg_xvm_metadata_batch(
        xvm::VersionDB& db,
        xvm::Workspace& workspace,
        xvm::WorkspaceInstalled& installed,
        const std::vector<mcpplibs::xpkg::XvmOp>& operations,
        const xvm::RemovalContext& removalContext,
        const XpkgRegistrationPlan& registration,
        const xvm::RemovalBatchOptions& removalOptions = {}) {
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

std::filesystem::path
configure_xpkg_execution_artifact_paths_(
        mcpplibs::xpkg::ExecutionContext& context) {
    auto subosDir = Config::xvm_artifact_subos_dir();
    context.bin_dir = subosDir / "bin";
    context.subos_sysrootdir = subosDir.string();
    return subosDir;
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
#if defined(__aarch64__) || defined(_M_ARM64)
    // Per-OS arch token, matching LLVM's release naming: Linux/Windows use the
    // GNU/uname-m spelling `aarch64` (consistent with the aarch64-linux-*
    // toolchain triples), while Apple uses its own `arm64`.
  #if defined(__APPLE__)
    return "arm64";
  #else
    return "aarch64";
  #endif
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
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

// Build fallback URLs from all candidate resource servers (excluding the
// selected one).
//
// Deliberately the cross-region list: the same release is published to every
// xlings-res host, and the regional buckets only say which one to *prefer*.
// Reading `resource_servers()` here made the fallback list empty for any
// region configured with a single host -- CN, in the shipped defaults -- so a
// host that was merely missing an asset produced a hard 404 with a working
// copy one hop away. See Config::all_resource_servers_for_.
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

struct DownloadResource_ {
    std::string version;
    std::string url;
    std::string sha256;
    std::unordered_map<std::string, std::string> mirrors;
    bool useResFallbacks { false };
};

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

class ScopedCurrentDir_ {
    std::filesystem::path oldDir_;
    bool changed_ { false };

public:
    explicit ScopedCurrentDir_(const std::filesystem::path& newDir) {
        std::error_code ec;
        oldDir_ = std::filesystem::current_path(ec);
        if (ec || newDir.empty()) return;
        std::filesystem::create_directories(newDir, ec);
        if (ec) return;
        std::filesystem::current_path(newDir, ec);
        changed_ = !ec;
    }

    ~ScopedCurrentDir_() {
        if (!changed_) return;
        std::error_code ec;
        std::filesystem::current_path(oldDir_, ec);
    }
};

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

// Every workspace file that can pin a payload against deletion.
//
// The UNION of the project-side and home-side files, deliberately, and not
// the scope's own side only. That is what it used to return, and the two sets
// never met: a Global-scope package mapped into a project subos was invisible
// to a `remove` run from the global subos, which then deleted a payload the
// project was still pointing at. The project got an unregistered active
// version out of a command it never ran.
//
// Erring toward the union can only over-retain -- a stale project subos file
// keeps a payload on disk longer than needed, which `xlings self clean`'s GC
// already sweeps (and which walks all of them too). The opposite error
// deletes something in use, and cannot be undone from the subos it broke.
//
// `scope` is retained for callers that still describe what they are removing,
// but no longer selects a side.
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

bool is_version_referenced_anywhere_(PackageScope scope,
                                     const std::string& target,
                                     const std::string& version,
                                     const std::filesystem::path& excludePath = {}) {
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

void detach_current_subos_(const std::string& target,
                           const std::string& version,
                           bool persist = true) {
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

bool process_xvm_operations_(const PlanNode& node,
                             const std::filesystem::path& dataDir,
                             mcpplibs::xpkg::PackageExecutor& executor,
                             bool useAfterInstall) {
    auto xvm_ops = executor.xvm_operations();
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

    // Determine namespace for version keys: primary repo gets bare versions,
    // other repos get "ns:version" to allow coexistence.
    std::string version_ns;
    {
        auto& globalRepos = Config::global_index_repos();
        bool isPrimary = !globalRepos.empty()
            && node.namespaceName == globalRepos[0].name;
        if (!isPrimary && !node.namespaceName.empty()) {
            version_ns = node.namespaceName;
        }
    }

    auto registration = normalize_xpkg_registration_plan(
        node, xvm_ops, version_ns, dataDir, useAfterInstall);
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
            xvm::make_ns_version(version_ns, node.version);
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
                    if (platform::atomic_replace_executable(
                            activeBin, xlings_bin)) {
                        log::debug(
                            "[self-replace] bootstrap {} <- {}",
                            xlings_bin.string(), activeBin.string());
                    } else {
                        log::warn(
                            "[self-replace] failed: bootstrap {} <- {}",
                            xlings_bin.string(), activeBin.string());
                    }
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

bool run_config_hook_(const PlanNode& node,
                      const std::filesystem::path& dataDir,
                      mcpplibs::xpkg::PackageExecutor& executor,
                      mcpplibs::xpkg::ExecutionContext& ctx,
                      std::function<void(const InstallStatus&)> onStatus,
                      bool useAfterInstall) {
    if (!executor.has_hook(mcpplibs::xpkg::HookType::Config)) return true;
    if (onStatus) {
        onStatus({ node.name, InstallPhase::Configuring, 0.8f, "" });
    }
    ScopedCurrentDir_ configCwd(ctx.install_dir);
    auto hookResult = executor.run_hook(mcpplibs::xpkg::HookType::Config, ctx);
    if (!hookResult.success) {
        log::warn("config hook failed for {}: {}", node.name, hookResult.error);
        return false;
    }
    return process_xvm_operations_(
        node, dataDir, executor, useAfterInstall);
}

// A package that promised programs and delivered none of them.
//
// `✓ N package(s) installed` counts recipes that did not raise, which is not
// the same as packages that work. Two real installs printed that checkmark and
// left the user with nothing: llvm on Windows registered no `clang` because
// its recipe's directory listing came back empty, and gcc on Windows
// registered nothing because the toolchain it delegates to aborted. Neither
// emitted a single diagnostic (openxlings/xlings#447).
//
// Recipes state what they provide in `package.programs`, so that promise can
// simply be checked against the version database.
//
// Deliberately checked after the WHOLE plan has run, not per node: a package
// may legitimately register nothing itself and delegate to a deferred install
// -- gcc on Windows hands off to mingw-w64 -- and that dep has not been
// installed yet at the moment its consumer's config hook returns.
//
// Only total absence is reported. Partial registration is normal and correct:
// recipes routinely declare one cross-platform program list and register the
// subset that exists on this host. Zero out of N is what never has a benign
// reading -- the package cannot do the thing it exists to do.
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

}  // namespace detail_

using InstallRequestHandler = std::function<void(const std::vector<mcpplibs::xpkg::InstallRequest>&)>;

class Installer {
    IndexManager* index_ { nullptr };
    PackageCatalog* catalog_ { nullptr };

public:
    explicit Installer(IndexManager& index) : index_(&index) {}
    explicit Installer(PackageCatalog& catalog) : catalog_(&catalog) {}

    // Resolve a build_deps entry (e.g. "gcc", "ns:gcc@15") to its
    // payload directory inside xpkgs/<store>/<version>. Plan is in topo
    // order, so by the time a Runtime node runs its install hook all of
    // its build_deps already have their payloads laid down on disk.
    static std::filesystem::path
    locate_dep_install_dir_(const InstallPlan& plan,
                            const std::filesystem::path& dataDir,
                            std::string_view depRef) {
        auto at = depRef.find('@');
        auto baseRef = (at == std::string_view::npos) ? depRef
                                                       : depRef.substr(0, at);
        auto colon = baseRef.find(':');
        std::string ns = (colon == std::string_view::npos)
            ? std::string{}
            : std::string(baseRef.substr(0, colon));
        std::string bare = (colon == std::string_view::npos)
            ? std::string(baseRef)
            : std::string(baseRef.substr(colon + 1));
        for (auto& n : plan.nodes) {
            bool match = (n.name == bare)
                       || (n.canonicalName == baseRef)
                       || (n.rawName == depRef);
            if (!ns.empty()) match = match && (n.namespaceName == ns);
            if (!match) continue;
            auto root = n.storeRoot.empty() ? (dataDir / "xpkgs") : n.storeRoot;
            return root / detail_::effective_store_name_(n) / n.version;
        }
        return {};
    }

    // Execute an install plan.
    // useAfterInstall: when true, force the installed program version to
    // become the active one even if another version is currently active.
    // Default behavior preserves the existing active version.
    std::expected<void, std::string>
    execute(const InstallPlan& plan,
            const DownloaderConfig& dlConfig,
            std::function<void(const InstallStatus&)> onStatus,
            InstallRequestHandler onInstallRequests = nullptr,
            DownloadProgressRenderer onRender = nullptr,
            CancellationToken* cancel = nullptr,
            bool useAfterInstall = false) {

        if (plan.has_errors()) {
            return std::unexpected(
                std::format("plan has errors: {}", plan.errors[0]));
        }

        auto dataDir = Config::paths().dataDir;
        auto platform = detect_platform_();

        // Phase 1: Collect download tasks for non-installed packages
        log::debug("installer: {} node(s) in plan, dataDir={}", plan.nodes.size(), dataDir.string());
        std::vector<DownloadTask> dlTasks;
        std::unordered_set<std::string> plannedDownloads;
        // Nodes a gate below refused. Phase 2 walks plan.nodes again and is
        // otherwise unaware of anything decided here, so `continue` alone
        // only skips the download -- the package still runs install() and
        // config() and ends up registered. This set is what makes a refusal
        // mean refused.
        std::unordered_set<std::string> refusedNodes;
        for (auto& node : plan.nodes) {
            if (node.alreadyInstalled) continue;

            const std::string hostArch =
                mcpplibs::xpkg::normalize_arch(detail_::detect_arch_());
            auto pkg = mcpplibs::xpkg::load_package(node.pkgFile, {
                .platform = platform,
                .arch = hostArch,
            });
            if (!pkg) {
                log::warn("skipping {}: {}", node.name, pkg.error());
                continue;
            }

            // Refuse a recipe written against a spec this build does not
            // implement, rather than falling through to V1 semantics.
            //
            // Install only. An already-installed package whose recipe later
            // moved past us must still be removable -- taking something out
            // needs no understanding of the spec that put it in.
            const auto specSupport = xpkg_spec_support(pkg->spec);
            if (!specSupport.supported) {
                if (specSupport.declared > 0) {
                    log::error("{}: xpackage spec \"{}\" is newer than this "
                               "xlings understands (max {})",
                               node.name, pkg->spec, max_supported_xpkg_spec);
                    log::info("try: xlings self update");
                } else {
                    log::error("{}: unreadable xpackage spec \"{}\"",
                               node.name, pkg->spec);
                }
                refusedNodes.insert(detail_::plan_key_(node));
                continue;
            }

            // Fail-closed arch gate: refuse a package whose declared `archs`
            // don't include the host arch. Gated on spec >= "2": in V1 the
            // `archs` field was never enforced and is frequently
            // under-declared (e.g. an x86_64-only list on a recipe that
            // actually resolves an aarch64 asset via XLINGS_RES), so enforcing
            // it would break installs that worked before. V2 authors opt into
            // correct per-arch declarations and want this enforced. Empty
            // `archs` is always exempt.
            // `>= 2` rather than `== "2"`: a spec revision we do implement
            // keeps the enforcement its predecessor introduced.
            if (specSupport.declared >= 2 && !pkg->archs.empty()) {
                const std::string hostArchCheck =
                    mcpplibs::xpkg::normalize_arch(detail_::detect_arch_());
                bool supported = false;
                for (auto& a : pkg->archs)
                    if (mcpplibs::xpkg::arch_matches(a, hostArchCheck)) { supported = true; break; }
                if (!supported) {
                    std::string have;
                    for (auto& a : pkg->archs) { if (!have.empty()) have += ", "; have += a; }
                    log::error("{}: unsupported architecture '{}' (supported: {})",
                               node.name, hostArchCheck, have);
                    // Same reason as the spec gate: without this the package
                    // is skipped for download and then installed anyway.
                    refusedNodes.insert(detail_::plan_key_(node));
                    continue;
                }
            }

            auto resource = detail_::resolve_download_resource_(
                pkg->xpm, node.name, node.version, platform, hostArch,
                dlConfig.preferredMirror);
            if (!resource) {
                log::warn("skipping {}: {}", node.name, resource.error());
                continue;
            }

            DownloadTask task;
            task.name = detail_::plan_key_(node);
            task.url = resource->url;
            task.sha256 = resource->sha256;
            task.cacheIdentity = std::format(
                "{}/{}/{}/{}/{}",
                node.name, resource->version, platform, hostArch,
                resource->useResFallbacks ? "xlings-res" : resource->url);
            task.destDir = detail_::runtime_dir_(node, dataDir);

            if (resource->useResFallbacks) {
                task.fallbackUrls = detail_::build_xlings_res_fallback_urls_(
                    node.name, resource->version, platform);
            }

            // Add remaining mirrors as fallbacks
            if (!resource->mirrors.empty()) {
                for (auto& [key, mirrorUrl] : resource->mirrors) {
                    if (mirrorUrl != resource->url) {
                        task.fallbackUrls.push_back(mirrorUrl);
                    }
                }
            }
            plannedDownloads.insert(task.name);
            dlTasks.push_back(std::move(task));
        }

        std::unordered_map<std::string, DownloadResult> downloadResults;

        // Download all
        if (!dlTasks.empty()) {
            log::debug("downloading {} package(s)...", dlTasks.size());
            auto results = download_all(dlTasks, dlConfig, onRender,
                [&](std::string_view name, float progress) {
                    if (onStatus) {
                        InstallStatus status;
                        status.name = std::string(name);
                        status.phase = progress >= 0
                            ? InstallPhase::Downloading
                            : InstallPhase::Failed;
                        status.progress = std::max(0.0f, progress);
                        onStatus(status);
                    }
                }, cancel);

            for (auto& r : results) {
                if (!r.success) {
                    log::error("download failed for {}: {}", r.name, r.error);
                } else {
                    downloadResults[r.name] = r;
                }
            }
        }

        // Phase 2: Install each package in topological order
        for (auto& node : plan.nodes) {
            // Refused by a gate in phase 1. Skipping here rather than there
            // is what keeps the refusal per-package: the rest of the plan
            // installs normally.
            if (refusedNodes.contains(detail_::plan_key_(node))) continue;
            if (cancel && cancel->is_cancelled()) {
                return std::unexpected(std::string("cancelled"));
            }
            if (onStatus) {
                onStatus({ node.name, InstallPhase::Installing, 0.5f, "" });
            }

            // Create executor and run hooks
            auto execResult = mcpplibs::xpkg::create_executor(node.pkgFile);
            if (!execResult) {
                log::error("failed to create executor for {}: {}",
                           node.name, execResult.error());
                if (onStatus) {
                    onStatus({ node.name, InstallPhase::Failed, 0.0f,
                               execResult.error() });
                }
                continue;
            }

            auto& executor = *execResult;
            executor.set_log_level(std::string(log::level_string()));

            // Build execution context
            mcpplibs::xpkg::ExecutionContext ctx;
            ctx.pkg_name = node.name;
            ctx.version = node.version;
            ctx.platform = platform;
            auto targetRoot = node.storeRoot.empty() ? (dataDir / "xpkgs") : node.storeRoot;
            ctx.install_dir = targetRoot / detail_::effective_store_name_(node) / node.version;
            detail_::configure_xpkg_execution_artifact_paths_(
                ctx);
            ctx.xpkg_dir = node.pkgFile.parent_path();
            ctx.project_data_dir = Config::project_data_dir();
            // pkgindex_dir: package index repo root (for custom module loading).
            // pkgFile layout: <pkgindex>/pkgs/<letter>/<name>.lua → 3 levels up.
            ctx.pkgindex_dir = node.pkgFile.parent_path()
                                           .parent_path()
                                           .parent_path().string();
            ctx.deps_list = node.deps;                  // legacy union (compat)
            ctx.runtime_deps_list = node.runtime_deps;  // split form
            ctx.build_deps_list   = node.build_deps;

            // self_exports: pre-resolve relative paths to absolute by
            // joining with this package's own install_dir.
            if (!node.exports.loader.empty()) {
                ctx.self_exports.loader = (ctx.install_dir / node.exports.loader).string();
            }
            ctx.self_exports.abi = node.exports.abi;
            for (auto& d : node.exports.libdirs) {
                ctx.self_exports.libdirs.push_back((ctx.install_dir / d).string());
            }

            // deps_exports: walk the topo plan to find each runtime dep
            // node and pull its exports (already populated by resolver).
            // Resolve relative paths against THAT dep's install_dir, not
            // ours. Strip @version when matching dep_spec → dep node, so
            // a runtime_deps entry "xim:glibc@2.39" matches the plan
            // node whose canonicalName == "xim:glibc" + version "2.39".
            for (auto& dep_spec : node.runtime_deps) {
                std::string dep_base = dep_spec;
                std::string dep_ver;
                if (auto at = dep_base.find('@'); at != std::string::npos) {
                    dep_ver = dep_base.substr(at + 1);
                    dep_base.resize(at);
                }
                for (auto& depNode : plan.nodes) {
                    bool name_match = (depNode.canonicalName == dep_base
                                    || depNode.name == dep_base
                                    || depNode.rawName == dep_base);
                    bool ver_match = dep_ver.empty() || depNode.version == dep_ver;
                    if (!name_match || !ver_match) continue;
                    if (depNode.exports.loader.empty() && depNode.exports.libdirs.empty()) {
                        // Dep declared no exports — skip (predicate falls
                        // through to convention later).
                        break;
                    }
                    auto depRoot = depNode.storeRoot.empty()
                        ? (dataDir / "xpkgs") : depNode.storeRoot;
                    auto depInstallDir = depRoot
                        / detail_::effective_store_name_(depNode) / depNode.version;
                    mcpplibs::xpkg::DepExport e;
                    if (!depNode.exports.loader.empty()) {
                        e.loader = (depInstallDir / depNode.exports.loader).string();
                    }
                    e.abi = depNode.exports.abi;
                    for (auto& d : depNode.exports.libdirs) {
                        e.libdirs.push_back((depInstallDir / d).string());
                    }
                    ctx.deps_exports[dep_spec] = std::move(e);
                    break;
                }
            }

            auto planKey = detail_::plan_key_(node);
            auto dlIt = downloadResults.find(planKey);

            std::optional<std::filesystem::path> extractedRoot;
            if (plannedDownloads.contains(planKey) && dlIt == downloadResults.end()) {
                log::error("download artifact missing for {}", node.name);
                if (onStatus) {
                    onStatus({ node.name, InstallPhase::Failed, 0.0f, "download artifact missing" });
                }
                continue;
            }
            // run_dir is the user's working directory (where xlings was invoked),
            // not the download/runtime directory. Hooks use it for resolving
            // relative paths and determining project install locations.
            ctx.run_dir = std::filesystem::current_path();

            if (dlIt != downloadResults.end()) {
                ctx.install_file = dlIt->second.localFile;
                if (detail_::is_archive_(dlIt->second.localFile)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Extracting, 0.35f, "" });
                    }
                    // Extract into the same runtime dir as the download
                    auto runtimeDir = dlIt->second.localFile.parent_path();
                    auto extracted = extract_archive_detailed(
                        dlIt->second.localFile, runtimeDir);
                    if (!extracted) {
                        auto error = std::move(extracted).error();
                        if (evict_invalid_archive_cache_(
                                dlIt->second.localFile, error)) {
                            log::warn(
                                "evicted invalid archive cache for {}: {}",
                                node.name, dlIt->second.localFile.string());
                        }
                        log::error("extract failed for {}: {}",
                                   node.name, error.message);
                        if (onStatus) {
                            onStatus({
                                node.name, InstallPhase::Failed, 0.0f,
                                error.message});
                        }
                        continue;
                    }
                    extractedRoot = *extracted;
                }
            }
            // No download artifact: install_file stays empty (type-only packages
            // like auto-config don't need a payload).  Do NOT fall back to
            // node.pkgFile — that is the package *definition* file; an install
            // hook calling os.mv(install_file, install_dir) would move it out
            // of the package index, destroying the index entry.

            // Ensure install_dir exists so hooks can mv/cp into it
            {
                std::error_code ec;
                std::filesystem::create_directories(ctx.install_dir, ec);
                // A genuine failure here (commonly EACCES on a root-owned
                // dir left by a prior `sudo` install) used to be swallowed,
                // surfacing later as a confusing hook failure. Surface it.
                if (ec && !std::filesystem::is_directory(ctx.install_dir)) {
                    log::warn("create install dir {} failed: {}",
                              ctx.install_dir.string(), ec.message());
                }
            }

            // A payload that belongs to another platform is not installed,
            // whatever the records say. Only a PROVABLE mismatch overrides
            // them: Unknown (scripts, data, a type-only package) keeps the
            // fast path, so nothing that works today starts reinstalling.
            const auto payloadVerdict =
                classify_payload_platform(ctx.install_dir);
            const bool foreignPayload =
                payloadVerdict == PayloadPlatform::Foreign;
            if (foreignPayload) {
                log::warn("{}: installed payload is not for {} — reinstalling",
                          node.name, host_platform_tag());
                log::warn("  payload: {}",
                          Config::display_path(ctx.install_dir));
            }

            bool payloadInstalled = node.alreadyInstalled && !foreignPayload;

            // Check if already installed via hook
            if (foreignPayload) {
                // The hooks below all answer "is it installed", and every one
                // of them would say yes about the wrong platform's files.
            }
            else if (!payloadInstalled && executor.has_hook(mcpplibs::xpkg::HookType::Installed)) {
                auto hookResult = executor.check_installed(ctx);
                if (hookResult.success && !hookResult.version.empty()) {
                    log::debug("{} already installed (version {})",
                              node.name, hookResult.version);
                    payloadInstalled = true;
                }
            }
            // Default: check xvm version database when no installed hook.
            //
            // Trust-but-verify: a DB entry alone isn't enough. If the
            // payload directory the entry points to is missing or empty
            // (broken-payload state — payload manually rm'd, partial
            // uninstall, FS corruption), fall through and let the install
            // hook re-create the payload.  Without this fall-through,
            // `xlings install <pkg>@<ver>` reports success without
            // actually doing anything — the user can never recover from
            // a broken payload via the obvious command.
            else if (!payloadInstalled) {
                auto db = Config::versions();
                auto resolved = xvm::match_version(db, node.name, node.version);
                if (!resolved.empty()) {
                    bool payload_ok = true;
                    if (auto* vdata = xvm::get_vdata(db, node.name, resolved);
                        vdata && !vdata->path.empty()) {
                        auto expanded = xvm::expand_path(
                            vdata->path, Config::paths().homeDir.string());
                        std::error_code ec;
                        payload_ok = std::filesystem::is_directory(expanded, ec)
                                  && payload_has_content(expanded);
                    }
                    if (payload_ok) {
                        log::debug("{} already installed in xvm (version {})",
                                  node.name, resolved);
                        payloadInstalled = true;
                    } else {
                        log::debug("{} registered as {} in xvm but payload "
                                  "missing — re-running install hook",
                                  node.name, resolved);
                    }
                }
            }

            // Run install hook
            if (!payloadInstalled && executor.has_hook(mcpplibs::xpkg::HookType::Install)) {
                log::debug("installing {}...", node.name);
                log::debug("installer: install_dir={}", ctx.install_dir.string());
                // Set cwd to the download/runtime directory so hooks can
                // find downloaded files via relative paths.  ctx.run_dir
                // (user's original cwd) is exposed via system.rundir().
                // When there is no download artifact we fall back to the
                // runtime dir rather than install_dir: hooks commonly start
                // with os.tryrm(install_dir) to get a clean slate, which
                // would delete the CWD and cause subsequent getcwd() calls
                // to fail.
                auto hookCwd = (dlIt != downloadResults.end())
                    ? dlIt->second.localFile.parent_path()
                    : detail_::runtime_dir_(node, dataDir); // fallback to runtime dir
                detail_::ScopedCurrentDir_ installCwd(hookCwd);

                // Inject XLINGS_BUILDDEP_<UPPER>_PATH for every build_dep
                // declared by this node, and prepend their bin/ dirs to
                // PATH for the duration of the hook. The hook (and any
                // subprocess it spawns) sees both forms; xpkg-side
                // pkginfo.build_dep() picks the env path first.
                std::string oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
                std::vector<std::string> setEnvKeys;
                std::string newPath = oldPath;
                for (auto& bd : node.build_deps) {
                    auto bdDir = locate_dep_install_dir_(plan, dataDir, bd);
                    if (bdDir.empty()) {
                        log::debug("[{}] build_dep '{}' not found in plan",
                                   node.name, bd);
                        continue;
                    }
                    std::string nameOnly = bd;
                    if (auto a = nameOnly.find('@'); a != std::string::npos)
                        nameOnly.resize(a);
                    if (auto c = nameOnly.find(':'); c != std::string::npos)
                        nameOnly = nameOnly.substr(c + 1);
                    std::string upper;
                    upper.reserve(nameOnly.size());
                    for (char c : nameOnly) {
                        upper += std::isalnum(static_cast<unsigned char>(c))
                            ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                            : '_';
                    }
                    auto envKey = "XLINGS_BUILDDEP_" + upper + "_PATH";
                    platform::set_env_variable(envKey, bdDir.string());
                    setEnvKeys.push_back(envKey);
                    auto bdBin = (bdDir / "bin").string();
                    if (std::filesystem::exists(bdDir / "bin")) {
                        newPath = bdBin + std::string(1, platform::PATH_SEPARATOR) + newPath;
                    }
                    log::debug("[{}] build_dep {} -> {} (env {})",
                               node.name, bd, bdDir.string(), envKey);
                }
                if (!setEnvKeys.empty()) {
                    platform::set_env_variable("PATH", newPath);
                }

                auto hookResult = executor.run_hook(
                    mcpplibs::xpkg::HookType::Install, ctx);

                // Restore env regardless of hook outcome
                if (!setEnvKeys.empty()) {
                    platform::set_env_variable("PATH", oldPath);
                    for (auto& k : setEnvKeys) {
                        platform::set_env_variable(k, "");
                    }
                }

                if (!hookResult.success) {
                    log::error("install hook failed for {}: {}",
                               node.name, hookResult.error);
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   hookResult.error });
                    }
                    continue;
                }
            } else if (!payloadInstalled && node.pkgType == 1 /* Script */) {
                log::debug("installing script {}...", node.name);
                if (!script::default_install(node, ctx)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default script install failed" });
                    }
                    continue;
                }
            } else if (!payloadInstalled && node.pkgType == 4 /* Subos */) {
                log::debug("installing subos base {}...", node.name);
                if (!subos::default_install(node, ctx)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default subos install failed" });
                    }
                    continue;
                }
            }

            if (!payloadInstalled && extractedRoot && !detail_::has_directory_entries_(ctx.install_dir)) {
                if (!detail_::stage_extracted_payload_(*extractedRoot, ctx.install_dir)) {
                    log::error("failed to stage extracted payload for {}", node.name);
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "failed to stage extracted payload" });
                    }
                    continue;
                }
            }

            if (!payloadInstalled && !detail_::normalize_file_install_(ctx.install_dir)) {
                log::error("failed to normalize file install layout for {}", node.name);
                if (onStatus) {
                    onStatus({ node.name, InstallPhase::Failed, 0.0f,
                               "failed to normalize file install layout" });
                }
                continue;
            }

            // Auto-stamp: drop a `.xim-installed` marker in install_dir if
            // it's still empty after every install path (hook + extracted-
            // payload fallback + script default). Wrapper packages
            // (linux-headers, fromsource:* aliases) legitimately leave
            // install_dir empty because their real payload lives in a
            // separately-installed dep — without a stamp, xlings's
            // installed-probe (`is_directory && !is_empty`) reports
            // "not installed" and re-runs install + config on every
            // dependent install. Critical: this MUST come after
            // stage_extracted_payload_ above; otherwise a stamp written
            // earlier would falsely make install_dir look "non-empty"
            // and skip the extracted-payload fallback (which exists for
            // packages whose hook silently no-ops, e.g. patchelf where
            // the tarball has no top-level dir).
            if (!payloadInstalled && node.pkgType != 3 /* Config */) {
                // TODO(config): formalize config packages as repeatable,
                // no-install procedures in libxpkg/spec. Keep current hook
                // semantics for now; just avoid xlings-owned markers that
                // would make an otherwise empty config directory look
                // installed.
                executor.apply_install_stamp_if_empty(ctx);
            }

            // Record which platform produced this payload. Written on every
            // path that just installed one, and also when the fast path was
            // taken and the heuristic had to be consulted -- that second case
            // is the self-heal: a pre-stamp payload is classified once and
            // never again.
            // Record which platform produced this payload -- but ONLY after a
            // path that actually installed one, and only if the files on disk
            // agree. Stamping unconditionally is worse than not stamping at
            // all: an install hook that started with os.tryrm(install_dir) and
            // then had no artifact to unpack (the shape a foreign payload
            // reaches here in) would leave an empty directory carrying this
            // platform's stamp, and every later run would believe it.
            if (!payloadInstalled && node.pkgType != 3 /* Config */
                && classify_payload_content(ctx.install_dir)
                       != PayloadPlatform::Foreign) {
                write_payload_stamp(ctx.install_dir, node.version);
            }

            // Apply elfpatch auto-patching if the install hook enabled it
            if (!payloadInstalled) {
                // Ensure binDir is in PATH so elfpatch can find patchelf
                auto binDir = ctx.bin_dir.string();
                auto curPath = std::string(std::getenv("PATH") ? std::getenv("PATH") : "");
                if (!binDir.empty() && curPath.find(binDir) == std::string::npos) {
                    platform::set_env_variable("PATH",
                        binDir + std::string(1, platform::PATH_SEPARATOR) + curPath);
                }
                auto epResult = executor.apply_elfpatch_auto();
                if (epResult.success && !epResult.output.empty()) {
                    log::debug("{}: elfpatch auto: {}", node.name, epResult.output);
                } else if (!epResult.success) {
                    log::debug("{}: elfpatch auto failed: {}", node.name, epResult.error);
                }
            }

            // Process deferred pkgmanager.install()/remove() requests synchronously
            // before config hook, so config can access sub-dependencies
            {
                auto reqs = executor.install_requests();
                if (!reqs.empty() && onInstallRequests) {
                    for (auto& req : reqs) {
                        log::debug("[{}] deferred {}: {}", node.name, req.op, req.target);
                    }
                    onInstallRequests(reqs);
                }
            }

            if (node.kind == DepKind::Build) {
                // Build-only deps: payload is on disk and resolvable via
                // pkginfo.build_dep() / XLINGS_BUILDDEP_*_PATH. We skip
                // config hook + xvm operations entirely so the dep does
                // NOT take over a workspace slot or get a PATH shim.
                log::debug("[{}] kind=Build: skipping config hook / workspace activation",
                           node.name);
            } else if (!executor.has_hook(mcpplibs::xpkg::HookType::Config) && node.pkgType == 1 /* Script */) {
                if (!script::default_config(node, dataDir)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default script config failed" });
                    }
                    continue;
                }
            } else if (!executor.has_hook(mcpplibs::xpkg::HookType::Config) && node.pkgType == 4 /* Subos */) {
                if (!subos::default_config(node, dataDir)) {
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   "default subos config failed" });
                    }
                    continue;
                }
            } else if (!detail_::run_config_hook_(node, dataDir, executor, ctx,
                                                  onStatus, useAfterInstall)) {
                if (onStatus) {
                    onStatus({ node.name, InstallPhase::Failed, 0.0f,
                               "config hook failed" });
                }
                continue;
            }

            if (node.pkgType != 3 /* Config */) {
                // TODO(config): same soft policy as the install stamp. A
                // config package with no author-created payload must not
                // become installed solely because xlings copied metadata into
                // install_dir. Future libxpkg/spec work should define the
                // stricter config contract.
                if (auto snapshot = detail_::save_xpkg_snapshot_(node.pkgFile, ctx.install_dir);
                    !snapshot) {
                    log::error("failed to save xpkg snapshot for {}: {}",
                               node.name, snapshot.error());
                    if (onStatus) {
                        onStatus({ node.name, InstallPhase::Failed, 0.0f,
                                   snapshot.error() });
                    }
                    continue;
                }
            }

            if (catalog_) {
                catalog_->mark_installed(PackageMatch{
                    .rawName = node.rawName,
                    .name = node.name,
                    .version = node.version,
                    .namespaceName = node.namespaceName,
                    .canonicalName = node.canonicalName,
                    .repoName = node.repoName,
                    .pkgFile = node.pkgFile,
                    .storeRoot = node.storeRoot,
                    .scope = node.scope,
                    .installed = true,
                }, true);
            } else {
                index_->mark_installed(node.name, true);
            }

            if (onStatus) {
                onStatus({ node.name, InstallPhase::Done, 1.0f,
                           payloadInstalled ? "already installed" : "" });
            }
            if (payloadInstalled) {
                log::debug("{}@{} mapping to current subos", node.name, node.version);
            } else {
                log::debug("{}@{} installed successfully", node.name, node.version);
            }
        }

        // Every hook returned success; that is not the same as the packages
        // being usable. Fail the install rather than let a checkmark stand for
        // a package that registered none of the programs it promised.
        if (auto broken = detail_::unfulfilled_program_promises_(plan);
            !broken.empty()) {
            for (const auto& name : broken) {
                if (onStatus) {
                    onStatus({ name, InstallPhase::Failed, 0.0f,
                               "declared programs were not registered" });
                }
            }
            return std::unexpected(std::format(
                "{} installed but registered none of its declared programs",
                broken.front()));
        }

        return {};
    }

    // What `uninstall` actually did.
    //
    // Two very different things share this entry point, and they used to be
    // indistinguishable to the caller — which is how `xlings remove glibc`
    // came to print the same "✓ removed" whether the payload and its
    // registration were deleted or left fully intact (#443). A removal that
    // only detaches the current subos is the CORRECT behaviour when another
    // subos still uses the version; reporting it as a full removal is not.
    struct UninstallOutcome {
        // True when the version survived because another subos still pins it:
        // the current subos was detached, the registration and payload stayed.
        bool        detachedOnly { false };
        std::string target;   // resolved name, as stored in the version DB
        std::string version;  // resolved version, as stored (may be namespaced)
    };

    // Uninstall a package
    std::expected<UninstallOutcome, std::string>
    uninstall(const std::string& name) {
        auto platform = detect_platform_();
        auto currentWorkspacePath = detail_::current_workspace_config_path_();

        auto parse_target = [](std::string target) {
            auto at = target.find('@');
            if (at == std::string::npos) return std::pair{target, std::string{}};
            return std::pair{target.substr(0, at), target.substr(at + 1)};
        };

        auto [targetName, requestedVersion] = parse_target(name);
        if (requestedVersion.empty()) {
            requestedVersion = xvm::get_active_version(Config::effective_workspace(), targetName);
        }
        std::string resolvedTarget;
        if (requestedVersion.empty()) {
            resolvedTarget = targetName;
        } else {
            auto [ns, bareVersion] = xvm::parse_ns_version(requestedVersion);
            if (ns.empty()) {
                resolvedTarget = targetName + "@" + bareVersion;
            } else {
                resolvedTarget = ns + ":" + targetName + "@" + bareVersion;
            }
        }

        std::filesystem::path pkgFile;
        std::filesystem::path installDir;
        std::optional<PackageMatch> resolvedMatch;

        if (catalog_) {
            auto match = catalog_->resolve_target(resolvedTarget, platform);
            if (!match) return std::unexpected(match.error());
            resolvedMatch = *match;
            pkgFile = match->pkgFile;
            installDir = (match->storeRoot.empty() ? (Config::paths().dataDir / "xpkgs") : match->storeRoot)
                / detail_::effective_store_name_(*match)
                / match->version;
        } else {
            auto* entry = index_->find_entry(name);
            if (!entry) {
                return std::unexpected(std::format("package '{}' not found", name));
            }
            pkgFile = entry->path;
            installDir = Config::paths().dataDir / "xpkgs" / name;
        }

        auto detachTarget = resolvedMatch ? resolvedMatch->name : targetName;
        auto detachVersion = resolvedMatch ? resolvedMatch->version : requestedVersion;
        if (resolvedMatch) {
            auto& globalRepos = Config::global_index_repos();
            const bool isPrimary = !globalRepos.empty()
                && resolvedMatch->namespaceName == globalRepos[0].name;
            if (!isPrimary && !resolvedMatch->namespaceName.empty()) {
                detachVersion = xvm::make_ns_version(
                    resolvedMatch->namespaceName,
                    resolvedMatch->version);
            }
        }
        auto executingProvider = resolvedMatch
            ? (resolvedMatch->canonicalName.empty()
                ? canonical_package_name(
                    resolvedMatch->namespaceName,
                    resolvedMatch->name)
                : resolvedMatch->canonicalName)
            : targetName;
        auto executingProviderVersion = resolvedMatch
            ? resolvedMatch->version
            : xvm::strip_namespace(detachVersion);

        auto removalContext = snapshot_xpkg_removal_context(
            Config::versions_mut(), Config::workspace(), {},
            executingProvider, executingProviderVersion,
            detachTarget, detachVersion);
        if (!removalContext) {
            return std::unexpected(std::format(
                "xvm removal selection failed for {}@{}: {} "
                "(target='{}', version='{}')",
                executingProvider, executingProviderVersion,
                removalContext.error().message,
                removalContext.error().target,
                removalContext.error().version));
        }
        if (auto memberIt = removalContext->members.find(detachTarget);
            memberIt != removalContext->members.end()) {
            detachVersion = memberIt->second;
        }

        auto stillReferenced = !detachVersion.empty()
            && detail_::is_version_referenced_anywhere_(
                resolvedMatch ? resolvedMatch->scope : PackageScope::Global,
                detachTarget,
                detachVersion,
                currentWorkspacePath);

        if (stillReferenced) {
            detail_::detach_current_subos_(detachTarget, detachVersion);
            log::debug("{}@{} detached from current subos; payload retained",
                      detachTarget, detachVersion);
            // Reported, not just logged at debug: this is the branch whose
            // silence made a no-op look like a removal (#443).
            return UninstallOutcome{
                .detachedOnly = true,
                .target       = detachTarget,
                .version      = detachVersion,
            };
        }

        auto executorPkgFile = detail_::uninstall_xpkg_file_(pkgFile, installDir);
        if (executorPkgFile != pkgFile) {
            log::debug("using xpkg snapshot for uninstall: {}", executorPkgFile.string());
        }

        auto execResult = mcpplibs::xpkg::create_executor(executorPkgFile);
        if (!execResult) {
            return std::unexpected(execResult.error());
        }

        auto& executor = *execResult;
        executor.set_log_level(std::string(log::level_string()));

        mcpplibs::xpkg::ExecutionContext ctx;
        ctx.pkg_name = resolvedMatch ? resolvedMatch->name : name;
        ctx.version = resolvedMatch ? resolvedMatch->version : std::string{};
        ctx.platform = platform;
        const auto artifactSubosDir =
            detail_::configure_xpkg_execution_artifact_paths_(
                ctx);
        ctx.install_dir = installDir;
        ctx.xpkg_dir = pkgFile.parent_path();
        ctx.pkgindex_dir = pkgFile.parent_path()
                                  .parent_path()
                                  .parent_path().string();

        bool useDefaultRemoval = false;
        if (executor.has_hook(mcpplibs::xpkg::HookType::Uninstall)) {
            log::debug("uninstalling {}...", name);
            auto result = executor.run_hook(
                mcpplibs::xpkg::HookType::Uninstall, ctx);
            if (!result.success) {
                return std::unexpected(
                    std::format("uninstall hook failed: {}", result.error));
            }
        } else {
            // Check if this is a script-type or subos-type package and run default uninstall
            bool isScriptType = false;
            bool isSubosType  = false;
            if (catalog_ && resolvedMatch) {
                auto pkg = catalog_->load_package(*resolvedMatch);
                if (pkg) {
                    isScriptType = (pkg->type == mcpplibs::xpkg::PackageType::Script);
                    isSubosType  = (pkg->type == mcpplibs::xpkg::PackageType::Subos);
                }
            } else if (index_) {
                auto* entry = index_->find_entry(targetName);
                if (entry) {
                    isScriptType = (entry->type == mcpplibs::xpkg::PackageType::Script);
                    isSubosType  = (entry->type == mcpplibs::xpkg::PackageType::Subos);
                }
            }
            useDefaultRemoval = isScriptType || isSubosType;
        }

        // Process xvm operations collected by uninstall hook
        auto xvm_ops = executor.xvm_operations();
        if (useDefaultRemoval) {
            xvm_ops.push_back({
                .op = "remove",
                .name = detachTarget,
                .version = detachVersion,
            });
        }
        auto sysroot_include =
            artifactSubosDir / "usr" / "include";

        const auto dbBeforeRemoval = Config::versions_mut();
        auto removalResult = apply_xpkg_removal_operations(
            Config::versions_mut(),
            Config::workspace_mut(),
            Config::workspace_installed_mut(),
            xvm_ops,
            *removalContext,
            xvm::RemovalBatchOptions{
                .purgeSelection = true,
            });
        if (!removalResult) {
            return std::unexpected(std::format(
                "xvm removal batch failed for {}@{}: {} "
                "(target='{}', version='{}')",
                executingProvider, executingProviderVersion,
                removalResult.error().message,
                removalResult.error().target,
                removalResult.error().version));
        }

        for (const auto& op : xvm_ops) {
            if (op.op == "remove_headers") {
                xvm::remove_headers(op.includedir, sysroot_include);
            }
        }

        bool detachedByBatch = false;
        for (const auto& removed : removalResult->removed) {
            if (removed.target == detachTarget
                && removed.version == detachVersion) {
                detachedByBatch = true;
            }
        }
        cleanup_removed_xvm_library_artifacts(
            artifactSubosDir / "lib",
            dbBeforeRemoval,
            Config::versions_mut(),
            *removalResult);
        cleanup_removed_xvm_program_artifacts(
            artifactSubosDir / "bin",
            dbBeforeRemoval,
            Config::versions_mut(),
            Config::workspace_installed(),
            *removalResult);
        if (!detachedByBatch && !detachVersion.empty()) {
            detail_::detach_current_subos_(
                detachTarget, detachVersion, false);
        }

        Config::save_versions();
        Config::save_workspace();

        if (catalog_) {
            catalog_->mark_installed(*resolvedMatch, false);
        } else {
            index_->mark_installed(name, false);
        }
        std::error_code ec;
        std::filesystem::remove_all(installDir, ec);
        if (ec) {
            log::warn("failed to remove payload dir {}: {}", installDir.string(), ec.message());
        }

        // installDir is the version directory (e.g. .../xim-x-node/22.17.1).
        // Its parent is the per-package directory (.../xim-x-node) which
        // holds one subdirectory per installed version of the same package.
        // After we delete the version we just uninstalled, the package
        // directory may be left as an empty stub if this was its last
        // version — sweep it.
        //
        // Why "list-then-remove" instead of "remove and tolerate ENOTEMPTY":
        // fs::remove (non-recursive) silently returns false for non-empty
        // directories, which would conflate the "still has versions, leave
        // it alone" expected case with real errors (permission denied, IO
        // failure). It is also a maintenance trap — the next reader can
        // upgrade it to fs::remove_all and wipe sibling versions. So check
        // emptiness explicitly first.
        //
        // Cross-platform: directory_iterator and fs::remove are part of
        // std::filesystem and behave identically on Linux/macOS/Windows
        // for the cases we care about (empty dir → end iterator; remove of
        // an empty directory → succeeds; non-existent or unreadable parent
        // → ec set, we skip). One platform-specific edge case worth naming:
        // file managers occasionally drop hidden metadata into the dir
        // (.DS_Store on macOS, Thumbs.db on Windows). Those count as
        // non-empty, so we will leave the package directory alone — that
        // is the right behavior; never silently delete files we did not
        // create.
        auto parent = installDir.parent_path();
        std::error_code listEc;
        auto first = std::filesystem::directory_iterator(parent, listEc);
        if (!listEc && first == std::default_sentinel) {
            std::error_code rmEc;
            if (std::filesystem::remove(parent, rmEc)) {
                log::debug("swept empty package dir: {}", parent.string());
            } else if (rmEc) {
                log::warn("failed to sweep empty package dir {}: {}",
                          parent.string(), rmEc.message());
            }
        }

        log::debug("{} uninstalled", resolvedTarget);
        return UninstallOutcome{
            .detachedOnly = false,
            .target       = detachTarget,
            .version      = detachVersion,
        };
    }

private:
    static std::string detect_platform_() {
        #if defined(__linux__)
            return "linux";
        #elif defined(__APPLE__)
            return "macosx";
        #elif defined(_WIN32)
            return "windows";
        #else
            return "unknown";
        #endif
    }
};

} // namespace xlings::xim
