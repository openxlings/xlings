module xlings.core.xvm.registration;

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.bindings;

namespace xlings::xvm::detail_ {

RegistrationError registration_error_(
    RegistrationErrorKind kind,
    std::string path,
    std::string target,
    std::string version,
    std::string message) {
    return {
        .kind = kind,
        .path = std::move(path),
        .target = std::move(target),
        .version = std::move(version),
        .message = std::move(message),
    };
}

bool same_registration_group_(
    const BindingGroupRef& lhs,
    const BindingGroupRef& rhs) {
    return lhs.provider == rhs.provider
        && lhs.providerVersion == rhs.providerVersion
        && lhs.group == rhs.group
        && lhs.rootTarget == rhs.rootTarget
        && lhs.rootVersion == rhs.rootVersion;
}

RegistrationGroupIdentity registration_group_identity_(
    const BindingGroupRef& group) {
    return {
        group.provider,
        group.providerVersion,
        group.group,
        group.rootTarget,
        group.rootVersion,
    };
}

std::string effective_kind_(
    const VInfo& info,
    const VData& data) {
    return effective_kind(info, data);
}

std::string effective_source_name_(
    const std::string& target,
    const VInfo& info,
    const VData& data,
    std::string_view kind) {
    return effective_source_name(target, info, data, kind);
}

std::string effective_destination_name_(
    const std::string& target,
    const VData& data,
    std::string_view kind,
    std::string_view sourceName) {
    return effective_destination_name(target, data, kind, sourceName);
}

std::optional<std::string> legacy_payload_mismatch_(
    const RegistrationNode& node,
    const VInfo& info,
    const VData& data) {
    const auto kind = effective_kind_(info, data);
    const auto sourceName = effective_source_name_(
        node.target, info, data, kind);
    const auto destinationName = effective_destination_name_(
        node.target, data, kind, sourceName);
    const auto namesAnArtifact =
        node.kind != "group" && node.kind != "files";
    const auto expectedSourceName = namesAnArtifact
        ? std::string_view{node.sourceName}
        : std::string_view{};
    const auto expectedDestinationName = namesAnArtifact
        ? std::string_view{node.destinationName}
        : std::string_view{};
    if (data.path != node.path) return "path";
    if (kind != node.kind) return "kind";
    if (sourceName != expectedSourceName) return "sourceName";
    if (destinationName != expectedDestinationName) {
        return "destinationName";
    }
    if (data.alias != node.alias) return "alias";
    if (data.envs != node.envs) return "envs";
    return std::nullopt;
}

std::string registration_path_token_(std::string_view token) {
    std::string escaped;
    escaped.reserve(token.size());
    for (const auto ch : token) {
        if (ch == '~') {
            escaped += "~0";
        } else if (ch == '/') {
            escaped += "~1";
        } else {
            escaped += ch;
        }
    }
    return escaped;
}

std::string normalized_payload_path_(std::string_view path) {
    std::string out{path};
    std::ranges::replace(out, '\\', '/');
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

bool payload_path_covers_(std::string_view root, std::string_view candidate) {
    if (root.empty() || candidate.empty()) return false;
    if (candidate == root) return true;
    return candidate.size() > root.size()
        && candidate.starts_with(root)
        && candidate[root.size()] == '/';
}

void erase_exact_registration_edges_(
    VersionDB& db,
    const std::set<RegistrationExactKey>& exactNodes) {
    for (auto& [sourceTarget, info] : db) {
        for (auto bindingIt = info.bindings.begin();
             bindingIt != info.bindings.end();) {
            const auto& peerTarget = bindingIt->first;
            std::erase_if(
                bindingIt->second,
                [&](const auto& edge) {
                    const auto& [sourceVersion, peerVersion] = edge;
                    return exactNodes.contains(
                               RegistrationExactKey{
                                   sourceTarget, sourceVersion})
                        || exactNodes.contains(
                               RegistrationExactKey{
                                   peerTarget, peerVersion});
                });
            if (bindingIt->second.empty()) {
                bindingIt = info.bindings.erase(bindingIt);
            } else {
                ++bindingIt;
            }
        }
    }
}

}

namespace xlings::xvm {

std::expected<std::vector<RegisteredMember>, RegistrationError>
apply_registration_batch(
    VersionDB& db,
    Workspace& workspace,
    WorkspaceInstalled& installed,
    const RegistrationBatch& batch,
    std::vector<DetachedLegacyMember>* detached) {
    if (batch.provider.empty()) {
        return std::unexpected(detail_::registration_error_(
            RegistrationErrorKind::InvalidBatchIdentity,
            "/provider", {}, {}, "registration provider is empty"));
    }
    if (batch.providerVersion.empty()) {
        return std::unexpected(detail_::registration_error_(
            RegistrationErrorKind::InvalidBatchIdentity,
            "/providerVersion", {}, {},
            "registration provider release is empty"));
    }

    std::map<detail_::RegistrationExactKey, std::size_t> nodeIndexes;
    for (std::size_t index = 0; index < batch.nodes.size(); ++index) {
        const auto& node = batch.nodes[index];
        const auto nodePath = std::format("/nodes/{}", index);
        if (node.target.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidNodeIdentity,
                nodePath + "/target", node.target, node.version,
                "registration target is empty"));
        }
        if (node.version.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidNodeIdentity,
                nodePath + "/version", node.target, node.version,
                "registration version is empty"));
        }
        if (node.kind != "program"
            && node.kind != "lib"
            && node.kind != "group"
            && node.kind != "files") {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidNodePayload,
                nodePath + "/kind", node.target, node.version,
                std::format(
                    "unsupported registration node kind '{}'",
                    node.kind)));
        }
        // `group` and `files` name no artifact to dispatch, so neither
        // carries a source or destination *name*. A files entry is validated
        // on its src/dst pair instead -- that is what it does carry.
        const bool namesAnArtifact =
            node.kind != "group" && node.kind != "files";
        if (namesAnArtifact && node.sourceName.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidNodePayload,
                nodePath + "/sourceName", node.target, node.version,
                "materialized registration source name is empty"));
        }
        if (namesAnArtifact && node.destinationName.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidNodePayload,
                nodePath + "/destinationName", node.target, node.version,
                "materialized registration destination name is empty"));
        }
        if (node.kind == "files") {
            if (node.fileSrc.empty()) {
                return std::unexpected(detail_::registration_error_(
                    RegistrationErrorKind::InvalidNodePayload,
                    nodePath + "/src", node.target, node.version,
                    "file asset declares no source"));
            }
            if (!is_permitted_file_destination(node.fileDst)) {
                return std::unexpected(detail_::registration_error_(
                    RegistrationErrorKind::InvalidNodePayload,
                    nodePath + "/dst", node.target, node.version,
                    std::format(
                        "file asset destination '{}' is not allowed: it must "
                        "be relative to the subos root, must not walk "
                        "upward, and must live under usr/, etc/ or share/",
                        node.fileDst)));
            }
        }
        if (node.binding && node.binding->rootTarget.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidBindingIdentity,
                nodePath + "/binding/rootTarget",
                node.target, node.version,
                "registration binding root target is empty"));
        }
        if (node.binding && node.binding->rootVersion.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidBindingIdentity,
                nodePath + "/binding/rootVersion",
                node.target, node.version,
                "registration binding root version is empty"));
        }

        const detail_::RegistrationExactKey key{
            node.target,
            node.version,
        };
        if (const auto [_, inserted] =
                nodeIndexes.emplace(key, index);
            !inserted) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::DuplicateNode,
                nodePath, node.target, node.version,
                "duplicate exact registration node"));
        }
    }

    std::map<std::string, detail_::RegistrationGroup> groups;
    std::map<detail_::RegistrationExactKey, std::string> rootGroups;
    std::map<detail_::RegistrationExactKey, std::string> nodeGroups;
    const auto addMember = [&](
        detail_::RegistrationGroup& group,
        const RegistrationNode& node,
        std::size_t index)
        -> std::optional<RegistrationError> {
        auto [memberIt, inserted] =
            group.members.emplace(node.target, node.version);
        if (!inserted && memberIt->second != node.version) {
            return detail_::registration_error_(
                RegistrationErrorKind::TargetVersionConflict,
                std::format("/nodes/{}", index),
                node.target, node.version,
                std::format(
                    "group '{}' selects two versions of target '{}'",
                    group.label, node.target));
        }
        return std::nullopt;
    };

    for (std::size_t index = 0; index < batch.nodes.size(); ++index) {
        const auto& node = batch.nodes[index];
        if (!node.binding) continue;
        const auto nodePath = std::format("/nodes/{}", index);
        const detail_::RegistrationExactKey key{
            node.target,
            node.version,
        };
        const detail_::RegistrationExactKey root{
            node.binding->rootTarget,
            node.binding->rootVersion,
        };
        if (key == root) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::SelfBinding,
                nodePath + "/binding", node.target, node.version,
                "registration node cannot bind to itself"));
        }
        if (!nodeIndexes.contains(root)) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::RootNotInBatch,
                nodePath + "/binding",
                root.first, root.second,
                "registration binding root is not an exact batch node"));
        }

        const auto label = node.binding->group.empty()
            ? node.binding->rootTarget
            : node.binding->group;
        if (auto groupIt = groups.find(label);
            groupIt != groups.end() && groupIt->second.root != root) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::GroupConflict,
                nodePath + "/binding/group",
                node.target, node.version,
                std::format(
                    "registration group '{}' has multiple roots", label)));
        }
        if (auto rootIt = rootGroups.find(root);
            rootIt != rootGroups.end() && rootIt->second != label) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::GroupConflict,
                nodePath + "/binding/group",
                node.target, node.version,
                "registration root is assigned to multiple groups"));
        }
        if (auto nodeIt = nodeGroups.find(key);
            nodeIt != nodeGroups.end() && nodeIt->second != label) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::GroupConflict,
                nodePath + "/binding/group",
                node.target, node.version,
                "registration node is assigned to multiple groups"));
        }
        if (auto rootNodeIt = nodeGroups.find(root);
            rootNodeIt != nodeGroups.end()
            && rootNodeIt->second != label) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::GroupConflict,
                nodePath + "/binding/group",
                node.target, node.version,
                "registration root is assigned to multiple groups"));
        }

        auto [groupIt, _] = groups.try_emplace(
            label,
            detail_::RegistrationGroup{
                .label = label,
                .root = root,
                .identityNode = key,
                .identityPath = node.binding->group.empty()
                    ? nodePath + "/binding/rootTarget"
                    : nodePath + "/binding/group",
            });
        rootGroups[root] = label;
        nodeGroups[key] = label;
        nodeGroups[root] = label;
        if (auto error = addMember(groupIt->second, node, index)) {
            return std::unexpected(std::move(*error));
        }
        const auto& rootNode =
            batch.nodes[nodeIndexes.at(root)];
        if (auto error = addMember(
                groupIt->second, rootNode, nodeIndexes.at(root))) {
            return std::unexpected(std::move(*error));
        }
    }

    for (std::size_t index = 0; index < batch.nodes.size(); ++index) {
        const auto& node = batch.nodes[index];
        const detail_::RegistrationExactKey key{
            node.target,
            node.version,
        };
        if (nodeGroups.contains(key)) continue;
        const auto label = node.target;
        if (auto groupIt = groups.find(label);
            groupIt != groups.end() && groupIt->second.root != key) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::GroupConflict,
                std::format("/nodes/{}", index),
                node.target, node.version,
                std::format(
                    "registration group '{}' has multiple roots", label)));
        }
        auto [groupIt, _] = groups.try_emplace(
            label,
            detail_::RegistrationGroup{
                .label = label,
                .root = key,
                .identityNode = key,
                .identityPath =
                    std::format("/nodes/{}/target", index),
            });
        nodeGroups[key] = label;
        rootGroups[key] = label;
        if (auto error = addMember(groupIt->second, node, index)) {
            return std::unexpected(std::move(*error));
        }
    }

    struct PersistedRegistrationGroup {
        detail_::RegistrationExactKey root;
        std::set<detail_::RegistrationExactKey> members;
    };
    std::map<std::string, PersistedRegistrationGroup> persistedGroups;
    for (const auto& [target, info] : db) {
        for (const auto& [version, data] : info.versions) {
            if (!data.bindingGroup) continue;
            const auto& ref = *data.bindingGroup;
            if (ref.provider != batch.provider
                || ref.providerVersion != batch.providerVersion) {
                continue;
            }
            const detail_::RegistrationExactKey root{
                ref.rootTarget,
                ref.rootVersion,
            };
            auto [groupIt, inserted] = persistedGroups.try_emplace(
                ref.group,
                PersistedRegistrationGroup{
                    .root = root,
                });
            if (!inserted && groupIt->second.root != root) {
                return std::unexpected(detail_::registration_error_(
                    RegistrationErrorKind::GroupConflict,
                    "/bindingGroups/"
                        + detail_::registration_path_token_(ref.group),
                    target, version,
                    std::format(
                        "persisted registration group '{}' has multiple roots",
                        ref.group)));
            }
            groupIt->second.members.emplace(target, version);
        }
    }
    for (const auto& [label, group] : groups) {
        const auto persistedIt = persistedGroups.find(label);
        if (persistedIt == persistedGroups.end()) continue;
        const auto missingMemberIt = std::ranges::find_if(
            persistedIt->second.members,
            [&](const auto& member) {
                return !nodeIndexes.contains(member);
            });
        if (persistedIt->second.root != group.root) {
            if (missingMemberIt
                == persistedIt->second.members.end()) {
                continue;
            }
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::GroupConflict,
                group.identityPath,
                group.identityNode.first,
                group.identityNode.second,
                std::format(
                    "registration group '{}' conflicts with persisted root '{}@{}'",
                    label,
                    persistedIt->second.root.first,
                    persistedIt->second.root.second)));
        }
        if (missingMemberIt
            == persistedIt->second.members.end()) {
            continue;
        }
        return std::unexpected(detail_::registration_error_(
            RegistrationErrorKind::IncompleteOwnedGroup,
            group.identityPath,
            missingMemberIt->first,
            missingMemberIt->second,
            std::format(
                "same-owner registration omits an existing member of group '{}'",
                label)));
    }

    for (std::size_t index = 0; index < batch.headers.size(); ++index) {
        const auto& header = batch.headers[index];
        const auto headerPath = std::format("/headers/{}", index);
        if (header.sourceDir.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidHeader,
                headerPath + "/sourceDir", {}, {},
                "registration header source directory is empty"));
        }

        auto groupIt = groups.end();
        if (!header.group.empty()) {
            groupIt = groups.find(header.group);
            if (groupIt == groups.end()) {
                return std::unexpected(detail_::registration_error_(
                    RegistrationErrorKind::HeaderGroupNotFound,
                    headerPath + "/group", header.group, {},
                    std::format(
                        "registration header group '{}' does not exist",
                        header.group)));
            }
        } else if (groups.size() == 1) {
            groupIt = groups.begin();
        } else {
            // More than one candidate. A recipe registering several
            // independent targets (a program plus a library, say) makes every
            // ungrouped node its own singleton group, so requiring exactly one
            // candidate would reject perfectly ordinary recipes. Break the tie
            // with the package's own target: headers shipped by package `p`
            // belong with `p`.
            //
            // Only when that target resolves to exactly one group. If it is
            // absent, or registered at versions that landed in different
            // groups, naming it identifies no single owner and guessing would
            // attach the headers to an arbitrary group.
            std::string_view resolved;
            bool spansGroups = false;
            if (!batch.primaryTarget.empty()) {
                for (const auto& [key, label] : nodeGroups) {
                    if (key.first != batch.primaryTarget) continue;
                    if (resolved.empty()) {
                        resolved = label;
                    } else if (resolved != label) {
                        spansGroups = true;
                        break;
                    }
                }
            }
            if (resolved.empty() || spansGroups) {
                return std::unexpected(detail_::registration_error_(
                    RegistrationErrorKind::HeaderAmbiguous,
                    headerPath + "/group", batch.primaryTarget, {},
                    std::format(
                        "ungrouped registration header has {} candidate "
                        "groups and primaryTarget '{}' {}; name the owning "
                        "group on the header",
                        groups.size(), batch.primaryTarget,
                        spansGroups ? "spans several of them"
                                    : "is not one of them")));
            }
            groupIt = groups.find(std::string(resolved));
        }
        groupIt->second.headers.push_back({
            .sourceDir = header.sourceDir,
            .destinationPrefix = header.destinationPrefix,
        });
    }
    for (auto& [_, group] : groups) {
        std::ranges::sort(
            group.headers,
            [](const HeaderAsset& lhs, const HeaderAsset& rhs) {
                return std::tie(lhs.sourceDir, lhs.destinationPrefix)
                    < std::tie(rhs.sourceDir, rhs.destinationPrefix);
            });
        group.headers.erase(
            std::unique(
                group.headers.begin(),
                group.headers.end(),
                [](const HeaderAsset& lhs, const HeaderAsset& rhs) {
                    return lhs.sourceDir == rhs.sourceDir
                        && lhs.destinationPrefix
                            == rhs.destinationPrefix;
                }),
            group.headers.end());
    }

    // Owner-less entries this batch takes over, and the field that differed.
    // Collected during validation and reported with the result; see the
    // adoption comment below.
    std::map<detail_::RegistrationExactKey, std::string> adoptedLegacy;

    for (std::size_t index = 0; index < batch.nodes.size(); ++index) {
        const auto& node = batch.nodes[index];
        const auto infoIt = db.find(node.target);
        if (infoIt == db.end()) continue;
        const auto dataIt = infoIt->second.versions.find(node.version);
        if (dataIt == infoIt->second.versions.end()) continue;
        const auto& data = dataIt->second;
        const auto nodePath = std::format("/nodes/{}", index);

        if (data.bindingGroup) {
            const auto& owner = *data.bindingGroup;
            if (owner.provider != batch.provider
                || owner.providerVersion != batch.providerVersion) {
                return std::unexpected(detail_::registration_error_(
                    RegistrationErrorKind::OwnershipConflict,
                    nodePath, node.target, node.version,
                    std::format(
                        "exact registration is owned by '{}@{}'",
                        owner.provider, owner.providerVersion)));
            }

            std::set<detail_::RegistrationExactKey> oldMembers;
            for (const auto& [target, info] : db) {
                for (const auto& [version, member] : info.versions) {
                    if (member.bindingGroup
                        && detail_::same_registration_group_(
                            *member.bindingGroup, owner)) {
                        oldMembers.emplace(target, version);
                    }
                }
            }
            const auto rootInfoIt = db.find(owner.rootTarget);
            if (rootInfoIt != db.end()) {
                const auto rootIt = rootInfoIt->second.versions.find(
                    owner.rootVersion);
                if (rootIt != rootInfoIt->second.versions.end()
                    && rootIt->second.bindingGroup
                    && detail_::same_registration_group_(
                        *rootIt->second.bindingGroup, owner)) {
                    for (const auto& [target, version] :
                         rootIt->second.bindingMembers) {
                        const auto memberInfoIt = db.find(target);
                        if (memberInfoIt != db.end()
                            && memberInfoIt->second.versions.contains(
                                version)) {
                            oldMembers.emplace(target, version);
                        }
                    }
                }
            }
            for (const auto& oldMember : oldMembers) {
                if (nodeIndexes.contains(oldMember)) continue;
                return std::unexpected(detail_::registration_error_(
                    RegistrationErrorKind::IncompleteOwnedGroup,
                    nodePath, oldMember.first, oldMember.second,
                    "same-owner registration omits an existing group member"));
            }
            continue;
        }

        // Owner-less entry with different contents: ADOPT it, do not refuse.
        //
        // This used to be RegistrationErrorKind::LegacyPayloadMismatch, whose
        // hint told the user to uninstall the package first. Nothing owns an
        // owner-less entry, so there is no claim to protect -- and `remove`
        // keeps exactly these entries when another subos still references the
        // version (#443), so the hint pointed at a door that does not open.
        // The two ends met and the only remaining exit was hand-editing the
        // state file (#422).
        //
        // Adoption is also a repair, not just an escape: the entry comes out
        // of it OWNED (the group pass below assigns bindingGroup), so the next
        // provider that tries to take the same name@version meets
        // OwnershipConflict -- a protection the owner-less entry could never
        // raise. The refusal preserved damage it could not fix; the measured
        // case is an llvm@20.1.7 whose recorded path used Windows backslashes
        // on Linux.
        //
        // The group-integrity checks below are NOT relaxed: a batch that would
        // rewrite part of a bound group is still rejected.
        if (auto field = detail_::legacy_payload_mismatch_(
                node, infoIt->second, data)) {
            adoptedLegacy.insert_or_assign(
                detail_::RegistrationExactKey{node.target, node.version},
                *field);
        }
        auto selection = resolve_binding_selection(
            db, node.target, node.version);
        if (!selection) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::BindingValidationFailed,
                nodePath, selection.error().target,
                selection.error().version,
                selection.error().message));
        }
        for (const auto& [target, version] : selection->members) {
            if (nodeIndexes.contains(
                    detail_::RegistrationExactKey{target, version})) {
                continue;
            }
            // The batch does not cover the whole legacy component.
            //
            // Refusing outright is right when the missing member could belong
            // to somebody else. It is wrong when the member is plainly this
            // package's own leftover, and then it is a trap: the component was
            // recorded by a client on ANOTHER PLATFORM, which registered names
            // this platform's recipe never produces. Measured on a real home,
            // an llvm@20.1.7 registered on Windows leaves `cl`, `lib`, `link`
            // and `rc` bound to it; on Linux the recipe emits no such nodes, so
            // every install refused -- and `remove` refused too, for its own
            // reasons, so nothing could move.
            //
            // Three conditions make the member safe to detach instead:
            //   1. it is OWNER-LESS. Nothing claims it, so no provider's
            //      selection is being rewritten behind its back.
            //   2. its payload lies inside a payload this batch is
            //      registering. That is the evidence it belongs to this
            //      package rather than merely resembling it.
            //
            // Being ACTIVE is deliberately NOT a third condition. It was one at
            // first, and it turned out to veto exactly the case this exists
            // for: on the measured home `cl`, `lib`, `link` and `rc` are all
            // active, so the refusal survived the fix. It also protects
            // nothing. Detaching does not deregister the entry and does not
            // deactivate it -- the shim still dispatches to the same place. All
            // that changes is that the entry stops claiming membership in a
            // release that no longer registers it, which is simply true.
            //
            // Detach, not delete: the entry stays in the database, only its
            // edges to this batch go (erase_exact_registration_edges_ below
            // drops them, since the peer side IS in the batch). What is left
            // is a standalone owner-less record with a payload that does not
            // resolve -- which `self doctor` already knows how to see and, at
            // the end of its ladder, to prune. Deleting it here would make a
            // registration silently destroy state on behalf of a repair that
            // has its own preview and its own reporting.
            const VData* memberData = nullptr;
            if (const auto memberInfoIt = db.find(target);
                memberInfoIt != db.end()) {
                const auto memberDataIt =
                    memberInfoIt->second.versions.find(version);
                if (memberDataIt != memberInfoIt->second.versions.end()) {
                    memberData = &memberDataIt->second;
                }
            }
            const bool ownerLess = memberData && !memberData->bindingGroup;
            const auto memberPath = memberData
                ? detail_::normalized_payload_path_(memberData->path)
                : std::string{};
            const bool insideBatchPayload = !memberPath.empty()
                && std::ranges::any_of(batch.nodes, [&](const auto& n) {
                    return detail_::payload_path_covers_(
                        detail_::normalized_payload_path_(n.path), memberPath);
                });
            if (ownerLess && insideBatchPayload) {
                if (detached) detached->push_back({target, version});
                continue;
            }
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::IncompleteLegacyComponent,
                nodePath, target, version,
                "legacy component is not completely represented in batch"));
        }
    }

    auto candidateDb = db;
    auto candidateWorkspace = workspace;
    auto candidateInstalled = installed;

    for (const auto& [_, index] : nodeIndexes) {
        const auto& node = batch.nodes[index];
        auto& info = candidateDb[node.target];
        if (info.type.empty()) info.type = node.kind;
        if (info.filename.empty() && node.kind != "group") {
            info.filename = node.sourceName;
        }
        auto& data = info.versions[node.version];
        data.path = node.path;
        data.kind = node.kind;
        data.fileSrc = node.fileSrc;
        data.fileDst = node.fileDst;
        data.sourceName =
            node.kind == "group" ? std::string{} : node.sourceName;
        data.destinationName =
            node.kind == "group" ? std::string{} : node.destinationName;
        data.alias = node.alias;
        data.envs = node.envs;
        data.bindingGroup.reset();
        data.bindingMembers.clear();
        data.bindingMembersDeclared = false;
        data.bindingHeaders.clear();
        data.bindingHeadersDeclared = false;
        data.bindingIntegrityIssues.clear();
    }
    std::set<detail_::RegistrationExactKey> exactNodes;
    for (const auto& [key, _] : nodeIndexes) {
        exactNodes.insert(key);
    }
    detail_::erase_exact_registration_edges_(
        candidateDb, exactNodes);

    std::vector<RegisteredMember> registered;
    for (const auto& [_, group] : groups) {
        // Decide activation once for the release, not once per member.
        //
        // Per member, a release that adds a name the previous one did not
        // have would activate just that name: the new member goes to this
        // release while everything else stays on the old one, and the
        // workspace ends up spanning two of them -- the exact state the
        // binding-group model exists to prevent, produced by install.
        //
        // Leaving a name alone when something already owns it is also the
        // right answer across providers: installing gcc must not silently
        // take `cc` away from an active llvm.
        //
        // But leaving the name alone is all that was ever intended, and this
        // used to do much more than that: ANY member name being active vetoed
        // the WHOLE group, including members nobody was contesting. Installing
        // node while a standalone npm package held `npm` therefore activated
        // nothing at all -- `node` and `npx`, which no other package provides,
        // came out installed and unreachable, with no shim and no error. The
        // user met it as `/usr/bin/env: 'node': No such file or directory`
        // from a tool that had nothing to do with xlings.
        //
        // Who holds a contested name decides whether it vetoes:
        //
        //   same provider   -- an older release of this same package. Moving
        //                      part of the workspace to the new release and
        //                      leaving the rest on the old one is exactly the
        //                      split the binding-group model exists to
        //                      prevent, and choosing between two releases of
        //                      one package is the user's call: `use`. Veto.
        //   other provider  -- a different package owns that name and keeps
        //                      it. That is ownership, not incoherence, and it
        //                      says nothing about this release's other names.
        //                      No veto; the contested name is skipped below.
        //   unknown         -- an entry with no group metadata, written before
        //                      providers were recorded. Treated as the same
        //                      provider, so old homes keep today's behaviour.
        const auto contested_by =
            [&](const std::string& memberTarget)
            -> std::optional<std::string> {
            const auto activeIt = candidateWorkspace.find(memberTarget);
            if (activeIt == candidateWorkspace.end()) return std::nullopt;
            const auto dbIt = candidateDb.find(memberTarget);
            if (dbIt == candidateDb.end()) return std::string{};
            const auto verIt = dbIt->second.versions.find(activeIt->second);
            if (verIt == dbIt->second.versions.end()) return std::string{};
            if (!verIt->second.bindingGroup) return std::string{};
            return verIt->second.bindingGroup->provider;
        };

        std::set<std::string> contested;
        bool sameProviderContest = false;
        for (const auto& member : group.members) {
            const auto owner = contested_by(member.first);
            if (!owner) continue;
            contested.insert(member.first);
            if (*owner == batch.provider) sameProviderContest = true;
        }

        // Is there anything left worth activating?
        //
        // A virtual root exists only to anchor the release its libraries
        // belong to; there is no command behind it. Activating one writes a
        // shim that can only print "no active version", which is issue #452:
        // `self doctor` called the file an orphan, `--fix` deleted it, and the
        // next install put it straight back. A group whose only uncontested
        // members are anchors therefore stays inactive, exactly as before.
        //
        // Neither the path nor the kind can answer this on its own. A real
        // anchor carries a payload path like everything else --
        // `xim-musl-gnu-gcc` records musl-gcc's own directory, and the #452
        // fixture's root, registered with no bindir at all, still ends up with
        // the package install dir -- and `kind` defaults to "program" for both.
        // So the batch's own answer (RegistrationNode::runnable, set by
        // whoever materialised the payload) is what decides, with the kind and
        // path as the cheap checks in front of it.
        const bool anyRealUncontested = std::ranges::any_of(
            group.members, [&](const auto& member) {
                if (contested.contains(member.first)) return false;
                const auto dbIt = candidateDb.find(member.first);
                if (dbIt == candidateDb.end()) return false;
                const auto verIt = dbIt->second.versions.find(member.second);
                if (verIt == dbIt->second.versions.end()) return false;
                if (verIt->second.path.empty()) return false;
                if (effective_kind(dbIt->second, verIt->second) != "program") {
                    return false;
                }
                const auto nodeIt = std::ranges::find_if(
                    batch.nodes, [&](const RegistrationNode& n) {
                        return n.target == member.first
                            && n.version == member.second;
                    });
                return nodeIt == batch.nodes.end() || nodeIt->runnable;
            });

        // `contested.empty()` first, and not folded into the clause after it.
        //
        // The anchor guard exists to stop #452 coming back through the door
        // the cross-provider rule opens, so it applies only to that door. A
        // release nobody is contesting activates exactly as it always has --
        // including a batch that is nothing but a virtual root, which is how
        // a library-only package registers and which has to stay activated
        // for anything to bind to it.
        const bool activateGroup =
            batch.useAfterInstall
            || contested.empty()
            || (!sameProviderContest && anyRealUncontested);

        const BindingGroupRef ref{
            .provider = batch.provider,
            .providerVersion = batch.providerVersion,
            .group = group.label,
            .rootTarget = group.root.first,
            .rootVersion = group.root.second,
        };
        for (const auto& [target, version] : group.members) {
            auto& data =
                candidateDb.at(target).versions.at(version);
            data.bindingGroup = ref;
            data.bindingMembers.clear();
            data.bindingMembersDeclared = false;
            data.bindingHeaders.clear();
            data.bindingHeadersDeclared = false;
            const auto adoptedIt = adoptedLegacy.find(
                detail_::RegistrationExactKey{target, version});
            registered.push_back({
                .target = target,
                .version = version,
                .kind = data.kind,
                .adoptedLegacy = adoptedIt != adoptedLegacy.end(),
                .adoptedLegacyField = adoptedIt != adoptedLegacy.end()
                    ? adoptedIt->second : std::string{},
            });
            auto& versions = candidateInstalled[target];
            bool foundVersion = false;
            std::erase_if(
                versions,
                [&](const std::string& installedVersion) {
                    if (installedVersion != version) return false;
                    if (!foundVersion) {
                        foundVersion = true;
                        return false;
                    }
                    return true;
                });
            if (!foundVersion) {
                versions.push_back(version);
            }
            // A contested name stays with whoever holds it. `useAfterInstall`
            // is the one exception, and it is not an exception to the rule so
            // much as a different question: the user typed the switch, so
            // taking the name is what they asked for.
            if (activateGroup
                && (batch.useAfterInstall || !contested.contains(target))) {
                candidateWorkspace[target] = version;
            }
        }

        auto& root =
            candidateDb.at(group.root.first)
                .versions.at(group.root.second);
        root.bindingMembers = group.members;
        root.bindingMembersDeclared = true;
        root.bindingHeaders = group.headers;
        root.bindingHeadersDeclared = !group.headers.empty();
        for (const auto& [target, version] : group.members) {
            if (target == group.root.first
                && version == group.root.second) {
                continue;
            }
            candidateDb.at(group.root.first)
                .bindings[target][group.root.second] = version;
            candidateDb.at(target)
                .bindings[group.root.first][version] = group.root.second;
        }
    }

    std::map<
        detail_::RegistrationGroupIdentity,
        detail_::RegistrationExactKey> candidateGroups;
    for (const auto& [target, info] : candidateDb) {
        for (const auto& [version, data] : info.versions) {
            if (!data.bindingGroup) continue;
            const auto& ref = *data.bindingGroup;
            if (ref.provider != batch.provider
                || ref.providerVersion != batch.providerVersion) {
                continue;
            }
            candidateGroups.try_emplace(
                detail_::registration_group_identity_(ref),
                target, version);
        }
    }
    for (const auto& [_, representative] : candidateGroups) {
        const auto& ref =
            *candidateDb.at(representative.first)
                 .versions.at(representative.second)
                 .bindingGroup;
        auto selection = resolve_binding_selection(
            candidateDb, representative.first, representative.second);
        if (!selection) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::BindingValidationFailed,
                "/bindingGroups/"
                    + detail_::registration_path_token_(ref.group),
                selection.error().target,
                selection.error().version,
                selection.error().message));
        }
    }

    db.swap(candidateDb);
    workspace.swap(candidateWorkspace);
    installed.swap(candidateInstalled);
    return registered;
}

}
