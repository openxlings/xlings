export module xlings.core.xvm.bindings;

import std;

import xlings.core.xvm.types;

export namespace xlings::xvm {

enum class BindingSource {
    ProviderGroup,
    LegacyGraph,
};

enum class BindingErrorKind {
    InvalidGraph,
    TargetNotFound,
    VersionNotFound,
    RootReferenceMismatch,
    GroupIdentityMismatch,
    RootMissingFromManifest,
    StartMemberMissing,
    MemberReferenceMismatch,
    UnsupportedKind,
    SelfEdge,
    AsymmetricEdge,
    ConflictingTargetVersion,
    PartialProviderMetadata,
    ProviderMetadataInLegacyGraph,
    MetadataIntegrityIssue,
};

struct BindingSelection {
    std::map<std::string, std::string> members;
    BindingSource source { BindingSource::LegacyGraph };
};

struct BindingError {
    BindingErrorKind kind { BindingErrorKind::InvalidGraph };
    std::string target;
    std::string version;
    std::string message;
};

std::expected<BindingSelection, BindingError>
resolve_binding_selection(const VersionDB& db,
                          const std::string& target,
                          const std::string& version);

}  // namespace xlings::xvm

namespace xlings::xvm::detail_ {

BindingError binding_error_(BindingErrorKind kind,
                            const std::string& target,
                            const std::string& version,
                            std::string message) {
    return {
        .kind = kind,
        .target = target,
        .version = version,
        .message = std::move(message),
    };
}

bool same_group_identity_(const BindingGroupRef& lhs,
                          const BindingGroupRef& rhs) {
    return lhs.provider == rhs.provider
        && lhs.providerVersion == rhs.providerVersion
        && lhs.group == rhs.group;
}

bool same_group_(const BindingGroupRef& lhs, const BindingGroupRef& rhs) {
    return same_group_identity_(lhs, rhs)
        && lhs.rootTarget == rhs.rootTarget
        && lhs.rootVersion == rhs.rootVersion;
}

bool supported_kind_(std::string_view kind) {
    return kind == "program" || kind == "lib" || kind == "group";
}

std::optional<std::string_view>
canonical_manifest_path_(const VData& data) {
    if (data.bindingMembersDeclared || !data.bindingMembers.empty()) {
        return "/bindingMembers";
    }
    if (data.bindingHeadersDeclared || !data.bindingHeaders.empty()) {
        return "/bindingHeaders";
    }
    return std::nullopt;
}

bool has_canonical_manifest_(const VData& data) {
    return canonical_manifest_path_(data).has_value();
}

BindingError metadata_integrity_error_(
    const std::string& target,
    const std::string& version,
    std::string_view code,
    std::string_view path) {
    return binding_error_(
        BindingErrorKind::MetadataIntegrityIssue,
        target, version,
        std::format("binding metadata integrity issue '{}' at '{}'",
                    code, path));
}

std::optional<BindingError>
binding_integrity_error_(const VData& data,
                         const std::string& target,
                         const std::string& version) {
    if (data.bindingIntegrityIssues.empty()) return std::nullopt;
    const auto& issue = data.bindingIntegrityIssues.front();
    return metadata_integrity_error_(
        target, version, issue.code, issue.path);
}

std::optional<std::string_view>
invalid_group_identity_path_(const BindingGroupRef& group) {
    if (group.provider.empty()) return "/bindingGroup/provider";
    if (group.providerVersion.empty()) return "/bindingGroup/version";
    if (group.group.empty()) return "/bindingGroup/group";
    if (group.rootTarget.empty()) return "/bindingGroup/rootTarget";
    if (group.rootVersion.empty()) return "/bindingGroup/rootVersion";
    return std::nullopt;
}

std::optional<BindingError>
group_identity_integrity_error_(const BindingGroupRef& group,
                                const std::string& target,
                                const std::string& version) {
    const auto invalidPath = invalid_group_identity_path_(group);
    if (!invalidPath) return std::nullopt;
    return metadata_integrity_error_(
        target, version, "binding-group-field-invalid", *invalidPath);
}

std::optional<BindingError>
non_root_metadata_error_(const VData& data,
                         const std::string& target,
                         const std::string& version,
                         const BindingGroupRef& group) {
    if (target == group.rootTarget && version == group.rootVersion) {
        return std::nullopt;
    }
    const auto metadataPath = canonical_manifest_path_(data);
    if (!metadataPath) return std::nullopt;
    return metadata_integrity_error_(
        target, version, "binding-metadata-on-non-root", *metadataPath);
}

std::expected<BindingSelection, BindingError>
resolve_provider_group_(const VersionDB& db,
                        const std::string& target,
                        const std::string& version,
                        const BindingGroupRef& group) {
    auto rootInfoIt = db.find(group.rootTarget);
    if (rootInfoIt == db.end()) {
        return std::unexpected(binding_error_(
            BindingErrorKind::TargetNotFound,
            group.rootTarget, group.rootVersion,
            "binding root target is missing"));
    }
    auto rootIt = rootInfoIt->second.versions.find(group.rootVersion);
    if (rootIt == rootInfoIt->second.versions.end()) {
        return std::unexpected(binding_error_(
            BindingErrorKind::VersionNotFound,
            group.rootTarget, group.rootVersion,
            "binding root version is missing"));
    }

    const auto& root = rootIt->second;
    if (auto error = binding_integrity_error_(
            root, group.rootTarget, group.rootVersion)) {
        return std::unexpected(std::move(*error));
    }
    if (root.bindingGroup) {
        if (auto error = group_identity_integrity_error_(
                *root.bindingGroup,
                group.rootTarget, group.rootVersion)) {
            return std::unexpected(std::move(*error));
        }
    }
    if (!root.bindingGroup
        || root.bindingGroup->rootTarget != group.rootTarget
        || root.bindingGroup->rootVersion != group.rootVersion) {
        return std::unexpected(binding_error_(
            BindingErrorKind::RootReferenceMismatch,
            group.rootTarget, group.rootVersion,
            "binding root does not reference itself"));
    }
    if (!same_group_identity_(*root.bindingGroup, group)) {
        return std::unexpected(binding_error_(
            BindingErrorKind::GroupIdentityMismatch,
            group.rootTarget, group.rootVersion,
            "binding root group identity is inconsistent"));
    }

    auto startIt = root.bindingMembers.find(target);
    if (startIt == root.bindingMembers.end() || startIt->second != version) {
        return std::unexpected(binding_error_(
            BindingErrorKind::StartMemberMissing,
            target, version, "starting member is absent from binding manifest"));
    }
    auto manifestRootIt = root.bindingMembers.find(group.rootTarget);
    if (manifestRootIt == root.bindingMembers.end()
        || manifestRootIt->second != group.rootVersion) {
        return std::unexpected(binding_error_(
            BindingErrorKind::RootMissingFromManifest,
            group.rootTarget, group.rootVersion,
            "binding root is absent from binding manifest"));
    }

    for (const auto& [memberTarget, memberVersion] : root.bindingMembers) {
        auto memberInfoIt = db.find(memberTarget);
        if (memberInfoIt == db.end()) {
            return std::unexpected(binding_error_(
                BindingErrorKind::TargetNotFound,
                memberTarget, memberVersion,
                "binding member target is missing"));
        }
        auto memberIt = memberInfoIt->second.versions.find(memberVersion);
        if (memberIt == memberInfoIt->second.versions.end()) {
            return std::unexpected(binding_error_(
                BindingErrorKind::VersionNotFound,
                memberTarget, memberVersion,
                "binding member version is missing"));
        }
        if (auto error = binding_integrity_error_(
                memberIt->second, memberTarget, memberVersion)) {
            return std::unexpected(std::move(*error));
        }
        if (memberIt->second.bindingGroup) {
            if (auto error = group_identity_integrity_error_(
                    *memberIt->second.bindingGroup,
                    memberTarget, memberVersion)) {
                return std::unexpected(std::move(*error));
            }
        }
        if (!memberIt->second.bindingGroup
            || !same_group_(*memberIt->second.bindingGroup, group)) {
            return std::unexpected(binding_error_(
                BindingErrorKind::MemberReferenceMismatch,
                memberTarget, memberVersion,
                "binding member back-reference is inconsistent"));
        }
        if (auto error = non_root_metadata_error_(
                memberIt->second, memberTarget, memberVersion, group)) {
            return std::unexpected(std::move(*error));
        }
        if (!supported_kind_(memberIt->second.kind)) {
            return std::unexpected(binding_error_(
                BindingErrorKind::UnsupportedKind,
                memberTarget, memberVersion,
                std::format("unsupported provider-group member kind '{}'",
                            memberIt->second.kind)));
        }
    }

    return BindingSelection{
        .members = root.bindingMembers,
        .source = BindingSource::ProviderGroup,
    };
}

std::expected<BindingSelection, BindingError>
resolve_legacy_graph_(const VersionDB& db,
                      const std::string& target,
                      const std::string& version) {
    BindingSelection selection{
        .source = BindingSource::LegacyGraph,
    };
    std::vector<std::pair<std::string, std::string>> pending{
        {target, version},
    };
    std::set<std::pair<std::string, std::string>> visited;

    while (!pending.empty()) {
        auto [currentTarget, currentVersion] = std::move(pending.back());
        pending.pop_back();
        if (!visited.emplace(currentTarget, currentVersion).second) continue;

        auto infoIt = db.find(currentTarget);
        if (infoIt == db.end()) {
            return std::unexpected(binding_error_(
                BindingErrorKind::TargetNotFound,
                currentTarget, currentVersion,
                "legacy target is missing"));
        }
        auto dataIt = infoIt->second.versions.find(currentVersion);
        if (dataIt == infoIt->second.versions.end()) {
            return std::unexpected(binding_error_(
                BindingErrorKind::VersionNotFound,
                currentTarget, currentVersion,
                "legacy version is missing"));
        }
        if (auto error = binding_integrity_error_(
                dataIt->second, currentTarget, currentVersion)) {
            return std::unexpected(std::move(*error));
        }
        if (dataIt->second.bindingGroup) {
            if (auto error = group_identity_integrity_error_(
                    *dataIt->second.bindingGroup,
                    currentTarget, currentVersion)) {
                return std::unexpected(std::move(*error));
            }
            return std::unexpected(binding_error_(
                BindingErrorKind::ProviderMetadataInLegacyGraph,
                currentTarget, currentVersion,
                "provider-aware version cannot participate in a legacy graph"));
        }
        if (has_canonical_manifest_(dataIt->second)) {
            return std::unexpected(binding_error_(
                BindingErrorKind::PartialProviderMetadata,
                currentTarget, currentVersion,
                "canonical binding manifest is missing its binding group"));
        }
        const auto kind = dataIt->second.kind.empty()
            ? std::string_view{infoIt->second.type}
            : std::string_view{dataIt->second.kind};
        if (!supported_kind_(kind)) {
            return std::unexpected(binding_error_(
                BindingErrorKind::UnsupportedKind,
                currentTarget, currentVersion,
                std::format("unsupported legacy member kind '{}'", kind)));
        }

        if (auto selectedIt = selection.members.find(currentTarget);
            selectedIt != selection.members.end()
            && selectedIt->second != currentVersion) {
            return std::unexpected(binding_error_(
                BindingErrorKind::ConflictingTargetVersion,
                currentTarget, currentVersion,
                "legacy graph selects conflicting target versions"));
        }
        selection.members[currentTarget] = currentVersion;

        for (const auto& [peerTarget, versions] : infoIt->second.bindings) {
            auto edgeIt = versions.find(currentVersion);
            if (edgeIt == versions.end()) continue;
            const auto& peerVersion = edgeIt->second;
            if (peerTarget == currentTarget) {
                return std::unexpected(binding_error_(
                    BindingErrorKind::SelfEdge,
                    currentTarget, currentVersion,
                    "legacy binding contains a self-edge"));
            }

            auto peerInfoIt = db.find(peerTarget);
            if (peerInfoIt == db.end()) {
                return std::unexpected(binding_error_(
                    BindingErrorKind::TargetNotFound,
                    peerTarget, peerVersion,
                    "legacy binding destination target is missing"));
            }
            if (!peerInfoIt->second.versions.contains(peerVersion)) {
                return std::unexpected(binding_error_(
                    BindingErrorKind::VersionNotFound,
                    peerTarget, peerVersion,
                    "legacy binding destination version is missing"));
            }
            auto reverseIt = peerInfoIt->second.bindings.find(currentTarget);
            if (reverseIt == peerInfoIt->second.bindings.end()) {
                return std::unexpected(binding_error_(
                    BindingErrorKind::AsymmetricEdge,
                    peerTarget, peerVersion, "legacy binding is asymmetric"));
            }
            auto reverseVersionIt = reverseIt->second.find(peerVersion);
            if (reverseVersionIt == reverseIt->second.end()
                || reverseVersionIt->second != currentVersion) {
                return std::unexpected(binding_error_(
                    BindingErrorKind::AsymmetricEdge,
                    peerTarget, peerVersion, "legacy binding is asymmetric"));
            }
            if (auto selectedIt = selection.members.find(peerTarget);
                selectedIt != selection.members.end()
                && selectedIt->second != peerVersion) {
                return std::unexpected(binding_error_(
                    BindingErrorKind::ConflictingTargetVersion,
                    peerTarget, peerVersion,
                    "legacy graph selects conflicting target versions"));
            }
            pending.emplace_back(peerTarget, peerVersion);
        }
    }

    return selection;
}

}  // namespace xlings::xvm::detail_

namespace xlings::xvm {

std::expected<BindingSelection, BindingError>
resolve_binding_selection(const VersionDB& db,
                          const std::string& target,
                          const std::string& version) {
    auto infoIt = db.find(target);
    if (infoIt == db.end()) {
        return std::unexpected(detail_::binding_error_(
            BindingErrorKind::TargetNotFound,
            target, version, "binding target is missing"));
    }
    auto versionIt = infoIt->second.versions.find(version);
    if (versionIt == infoIt->second.versions.end()) {
        return std::unexpected(detail_::binding_error_(
            BindingErrorKind::VersionNotFound,
            target, version, "binding version is missing"));
    }
    if (auto error = detail_::binding_integrity_error_(
            versionIt->second, target, version)) {
        return std::unexpected(std::move(*error));
    }
    if (!versionIt->second.bindingGroup
        && detail_::has_canonical_manifest_(versionIt->second)) {
        return std::unexpected(detail_::binding_error_(
            BindingErrorKind::PartialProviderMetadata,
            target, version,
            "canonical binding manifest is missing its binding group"));
    }
    if (versionIt->second.bindingGroup) {
        if (auto error = detail_::group_identity_integrity_error_(
                *versionIt->second.bindingGroup, target, version)) {
            return std::unexpected(std::move(*error));
        }
        if (auto error = detail_::non_root_metadata_error_(
                versionIt->second, target, version,
                *versionIt->second.bindingGroup)) {
            return std::unexpected(std::move(*error));
        }
        return detail_::resolve_provider_group_(
            db, target, version, *versionIt->second.bindingGroup);
    }
    return detail_::resolve_legacy_graph_(db, target, version);
}

}  // namespace xlings::xvm
