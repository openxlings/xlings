module xlings.core.xvm.bindings;

import std;
import xlings.core.xvm.types;

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
    return kind == "program" || kind == "lib" || kind == "group"
        || kind == "files";
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

std::string json_pointer_token_(std::string_view token) {
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

std::optional<BindingError>
root_content_integrity_error_(const VData& root,
                              const std::string& target,
                              const std::string& version) {
    for (const auto& [memberTarget, memberVersion] : root.bindingMembers) {
        if (memberTarget.empty()) {
            return metadata_integrity_error_(
                target, version,
                "binding-member-target-empty", "/bindingMembers/");
        }
        if (memberVersion.empty()) {
            return metadata_integrity_error_(
                target, version,
                "binding-member-version-empty",
                "/bindingMembers/" + json_pointer_token_(memberTarget));
        }
    }
    for (std::size_t index = 0;
         index < root.bindingHeaders.size(); ++index) {
        if (root.bindingHeaders[index].sourceDir.empty()) {
            return metadata_integrity_error_(
                target, version,
                "binding-header-source-dir-empty",
                std::format("/bindingHeaders/{}/sourceDir", index));
        }
    }
    return std::nullopt;
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
    if (auto error = root_content_integrity_error_(
            root, group.rootTarget, group.rootVersion)) {
        return std::unexpected(std::move(*error));
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
                      const std::string& version,
                      const std::map<
                          std::pair<std::string, std::string>,
                          std::vector<std::pair<
                              std::string, std::string>>>& incomingEdges) {
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

        const auto incomingIt =
            incomingEdges.find({currentTarget, currentVersion});
        if (incomingIt != incomingEdges.end()) {
            for (const auto& [sourceTarget, sourceVersion] :
                 incomingIt->second) {
                if (sourceTarget == currentTarget) {
                    return std::unexpected(binding_error_(
                        BindingErrorKind::SelfEdge,
                        currentTarget, currentVersion,
                        "legacy binding contains a self-edge"));
                }
                const auto& sourceInfo = db.at(sourceTarget);
                if (!sourceInfo.versions.contains(sourceVersion)) {
                    return std::unexpected(binding_error_(
                        BindingErrorKind::VersionNotFound,
                        sourceTarget, sourceVersion,
                        "legacy binding source version is missing"));
                }

                const auto reciprocalIt =
                    infoIt->second.bindings.find(sourceTarget);
                if (reciprocalIt == infoIt->second.bindings.end()) {
                    return std::unexpected(binding_error_(
                        BindingErrorKind::AsymmetricEdge,
                        sourceTarget, sourceVersion,
                        "legacy binding is asymmetric"));
                }
                const auto reciprocalVersionIt =
                    reciprocalIt->second.find(currentVersion);
                if (reciprocalVersionIt == reciprocalIt->second.end()
                    || reciprocalVersionIt->second != sourceVersion) {
                    return std::unexpected(binding_error_(
                        BindingErrorKind::AsymmetricEdge,
                        sourceTarget, sourceVersion,
                        "legacy binding is asymmetric"));
                }
            }
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

}

namespace xlings::xvm {

std::expected<BindingSelection, BindingError>
resolve_binding_selection(const VersionDB& db,
                          const std::string& target,
                          const std::string& version) {
    BindingSelectionResolver resolver{db};
    return resolver.resolve(target, version);
}

std::vector<HeaderAsset> group_header_assets(const VersionDB& db,
                                             const std::string& target,
                                             const std::string& version) {
    auto targetIt = db.find(target);
    if (targetIt == db.end()) return {};
    auto versionIt = targetIt->second.versions.find(version);
    if (versionIt == targetIt->second.versions.end()) return {};
    const VData& entry = versionIt->second;

    const VData* root = &entry;
    if (entry.bindingGroup) {
        const auto& ref = *entry.bindingGroup;
        if (auto rootTargetIt = db.find(ref.rootTarget);
            rootTargetIt != db.end()) {
            if (auto rootVersionIt =
                    rootTargetIt->second.versions.find(ref.rootVersion);
                rootVersionIt != rootTargetIt->second.versions.end()) {
                root = &rootVersionIt->second;
            }
        }
        // A dangling root reference is not repaired here. The selection layer
        // already refuses such a release, and this is only ever called for
        // one that resolved -- so falling back to the member's own view is
        // the conservative answer rather than a second opinion on validity.
    }

    if (!root->bindingHeaders.empty()) return root->bindingHeaders;
    if (!entry.includedir.empty()) {
        return {HeaderAsset{.sourceDir = entry.includedir}};
    }
    return {};
}

LibraryPlacement library_placement(const VersionDB& db,
                                   const std::string& target,
                                   const std::string& version) {
    auto targetIt = db.find(target);
    if (targetIt == db.end()) return {};
    auto versionIt = targetIt->second.versions.find(version);
    if (versionIt == targetIt->second.versions.end()) return {};

    const VInfo& info = targetIt->second;
    const VData& data = versionIt->second;
    const auto kind = effective_kind(info, data);
    if (kind != "lib" || data.path.empty()) return {};

    const auto sourceName = effective_source_name(target, info, data, kind);
    const auto destinationName =
        effective_destination_name(target, data, kind, sourceName);
    if (sourceName.empty() || destinationName.empty()) return {};

    return {
        .source = (std::filesystem::path(data.path) / sourceName).string(),
        .name = destinationName,
    };
}

bool is_permitted_file_destination(std::string_view destination) {
    if (destination.empty()) return false;
    const std::filesystem::path p{destination};
    if (p.is_absolute()) return false;
    std::string first;
    for (const auto& part : p) {
        if (part == "..") return false;
        if (first.empty()) first = part.string();
    }
    // Windows drive-relative forms ("C:foo") are absolute in spirit.
    if (destination.size() > 1 && destination[1] == ':') return false;
    return first == "usr" || first == "etc" || first == "share";
}

FilePlacement file_placement(const VersionDB& db,
                             const std::string& target,
                             const std::string& version) {
    auto targetIt = db.find(target);
    if (targetIt == db.end()) return {};
    auto versionIt = targetIt->second.versions.find(version);
    if (versionIt == targetIt->second.versions.end()) return {};

    const VData& data = versionIt->second;
    if (effective_kind(targetIt->second, data) != "files") return {};
    if (data.path.empty() || data.fileSrc.empty() || data.fileDst.empty()) {
        return {};
    }
    if (!is_permitted_file_destination(data.fileDst)) return {};

    return {
        .source = (std::filesystem::path(data.path) / data.fileSrc).string(),
        .destination = data.fileDst,
    };
}

bool is_binding_root(const VersionDB& db,
                     const std::string& target,
                     const std::string& version) {
    for (const auto& [peerTarget, peerInfo] : db) {
        for (const auto& [peerVersion, peerData] : peerInfo.versions) {
            if (peerTarget == target && peerVersion == version) continue;
            if (peerData.bindingGroup
                && peerData.bindingGroup->rootTarget == target
                && peerData.bindingGroup->rootVersion == version) {
                return true;
            }
        }
        if (peerTarget == target) continue;
        // Legacy pairwise edge: the member side records the root it belongs
        // to, keyed by its own version.
        const auto edge = peerInfo.bindings.find(target);
        if (edge == peerInfo.bindings.end()) continue;
        for (const auto& [_, rootVersion] : edge->second) {
            if (rootVersion == version) return true;
        }
    }
    return false;
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xvm {

BindingSelectionResolver::BindingSelectionResolver(const VersionDB& db) : db_(db) {}

[[nodiscard]] std::size_t BindingSelectionResolver::legacy_incoming_index_builds() const {
    return legacyIncomingIndexBuilds_;
}

[[nodiscard]] bool LibraryPlacement::empty() const { return source.empty(); }

[[nodiscard]] bool FilePlacement::empty() const { return source.empty(); }

} // namespace xlings::xvm
