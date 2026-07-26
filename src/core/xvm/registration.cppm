export module xlings.core.xvm.registration;

import std;

import xlings.core.xvm.types;
import xlings.core.xvm.bindings;

export namespace xlings::xvm {

struct RegistrationBinding {
    std::string rootTarget;
    std::string rootVersion;
    std::string group;
};

struct RegistrationNode {
    std::string target;
    std::string version;
    std::string path;
    std::string kind;
    std::string sourceName;
    std::string destinationName;
    std::vector<std::string> alias;
    std::map<std::string, std::string> envs;
    std::optional<RegistrationBinding> binding;
};

struct RegistrationHeader {
    std::string sourceDir;
    std::string destinationPrefix;
    std::string group;
};

struct RegistrationBatch {
    std::string provider;
    std::string providerVersion;
    std::vector<RegistrationNode> nodes;
    std::vector<RegistrationHeader> headers;
    bool useAfterInstall { false };
};

struct RegisteredMember {
    std::string target;
    std::string version;
    std::string kind;
};

enum class RegistrationErrorKind {
    InvalidBatchIdentity,
    InvalidNodeIdentity,
    InvalidNodePayload,
    InvalidBindingIdentity,
    DuplicateNode,
    RootNotInBatch,
    SelfBinding,
    GroupConflict,
    TargetVersionConflict,
    OwnershipConflict,
    LegacyPayloadMismatch,
    IncompleteLegacyComponent,
    IncompleteOwnedGroup,
    InvalidHeader,
    HeaderGroupNotFound,
    HeaderAmbiguous,
    BindingValidationFailed,
};

struct RegistrationError {
    RegistrationErrorKind kind {
        RegistrationErrorKind::InvalidBatchIdentity
    };
    std::string path;
    std::string target;
    std::string version;
    std::string message;
};

std::expected<std::vector<RegisteredMember>, RegistrationError>
apply_registration_batch(
    VersionDB& db,
    Workspace& workspace,
    WorkspaceInstalled& installed,
    const RegistrationBatch& batch);

}  // namespace xlings::xvm

namespace xlings::xvm::detail_ {

using RegistrationExactKey = std::pair<std::string, std::string>;
using RegistrationGroupIdentity = std::tuple<
    std::string,
    std::string,
    std::string,
    std::string,
    std::string>;

struct RegistrationGroup {
    std::string label;
    RegistrationExactKey root;
    RegistrationExactKey identityNode;
    std::string identityPath;
    std::map<std::string, std::string> members;
    std::vector<HeaderAsset> headers;
};

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
    return data.kind.empty() ? info.type : data.kind;
}

std::string effective_source_name_(
    const std::string& target,
    const VInfo& info,
    const VData& data,
    std::string_view kind) {
    if (kind == "group") return {};
    if (!data.sourceName.empty()) return data.sourceName;
    return info.filename.empty() ? target : info.filename;
}

std::string effective_destination_name_(
    const std::string& target,
    const VData& data,
    std::string_view kind,
    std::string_view sourceName) {
    if (kind == "group") return {};
    if (!data.destinationName.empty()) return data.destinationName;
    if (kind == "program") return target;
    return std::string(sourceName);
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
    const auto expectedSourceName = node.kind == "group"
        ? std::string_view{}
        : std::string_view{node.sourceName};
    const auto expectedDestinationName = node.kind == "group"
        ? std::string_view{}
        : std::string_view{node.destinationName};
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

}  // namespace xlings::xvm::detail_

namespace xlings::xvm {

std::expected<std::vector<RegisteredMember>, RegistrationError>
apply_registration_batch(
    VersionDB& db,
    Workspace& workspace,
    WorkspaceInstalled& installed,
    const RegistrationBatch& batch) {
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
            && node.kind != "group") {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidNodePayload,
                nodePath + "/kind", node.target, node.version,
                std::format(
                    "unsupported registration node kind '{}'",
                    node.kind)));
        }
        if (node.kind != "group" && node.sourceName.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidNodePayload,
                nodePath + "/sourceName", node.target, node.version,
                "materialized registration source name is empty"));
        }
        if (node.kind != "group" && node.destinationName.empty()) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::InvalidNodePayload,
                nodePath + "/destinationName", node.target, node.version,
                "materialized registration destination name is empty"));
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
        if (persistedIt->second.root != group.root) {
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
        for (const auto& member : persistedIt->second.members) {
            if (nodeIndexes.contains(member)) continue;
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::IncompleteOwnedGroup,
                group.identityPath,
                member.first,
                member.second,
                std::format(
                    "same-owner registration omits an existing member of group '{}'",
                    label)));
        }
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
        } else {
            if (groups.size() != 1) {
                return std::unexpected(detail_::registration_error_(
                    RegistrationErrorKind::HeaderAmbiguous,
                    headerPath + "/group", {}, {},
                    std::format(
                        "ungrouped registration header has {} candidate groups",
                        groups.size())));
            }
            groupIt = groups.begin();
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

        if (auto field = detail_::legacy_payload_mismatch_(
                node, infoIt->second, data)) {
            return std::unexpected(detail_::registration_error_(
                RegistrationErrorKind::LegacyPayloadMismatch,
                nodePath + "/" + *field,
                node.target, node.version,
                std::format(
                    "owner-less legacy payload field '{}' is incompatible",
                    *field)));
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
            registered.push_back({
                .target = target,
                .version = version,
                .kind = data.kind,
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
            if (!candidateWorkspace.contains(target)
                || batch.useAfterInstall) {
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

}  // namespace xlings::xvm
