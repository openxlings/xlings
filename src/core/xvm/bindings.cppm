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

class BindingSelectionResolver {
public:
    explicit BindingSelectionResolver(const VersionDB& db);

    std::expected<BindingSelection, BindingError>
    resolve(const std::string& target, const std::string& version);

    [[nodiscard]] std::size_t legacy_incoming_index_builds() const;

private:
    using TargetVersion = std::pair<std::string, std::string>;
    using IncomingEdges = std::map<
        TargetVersion, std::vector<TargetVersion>>;

    const IncomingEdges& legacy_incoming_edges_();

    const VersionDB& db_;
    std::optional<IncomingEdges> legacyIncomingEdges_;
    std::size_t legacyIncomingIndexBuilds_ { 0 };
};

std::expected<BindingSelection, BindingError>
resolve_binding_selection(const VersionDB& db,
                          const std::string& target,
                          const std::string& version);

// The header directories a release puts into the sysroot.
//
// Headers are a property of the *group*, not of a member: `gcc` and `g++`
// share one set of C++ headers, and which member the user happened to name is
// not supposed to change what lands in the sysroot. So the declaration lives
// on the group root as `bindingHeaders`, and this resolves any member of the
// release to the same list.
//
// Until now nothing read that list. Materialization went through
// `VData::includedir` instead, a single string the installer writes on the
// root with last-op-wins semantics. For a recipe declaring one header
// directory the two agree. For a recipe declaring two -- a toolchain with
// `include/c++/<ver>` alongside `include-fixed`, or one calling `xvm.setup`
// more than once -- they do not: install materializes both (it walks the
// effect list), while `xlings use` only ever remembers the last. Switching
// away therefore left the other directories' headers in the sysroot and
// switching in never put the new release's copies there. The sysroot held a
// mix of two releases, which is the exact state the binding-group work exists
// to make unrepresentable.
//
// Falls back to `includedir` when the entry has no group headers to report:
// state written by 0.4.69 has no `bindingHeaders` at all, and neither does a
// registration that is not part of a provider group.
std::vector<HeaderAsset> group_header_assets(const VersionDB& db,
                                             const std::string& target,
                                             const std::string& version);

// Where one library entry has to be placed in the sysroot.
//
// `source` is the absolute file inside the payload; `name` is what it is
// called in the sysroot lib directory. Both empty when the entry is not a
// library, has no payload path, or resolves to no name.
struct LibraryPlacement {
    std::string source;
    std::string name;

    [[nodiscard]] bool empty() const;
};

// Resolve an entry to its library placement.
//
// Reads `path` + `sourceName` + `destinationName` through the shared
// accessors in xvm.types -- the same three fields the install path has always
// used. The switch planner used to look at `VData::libdir` instead, a field
// with no writer anywhere in the tree, so it emitted no library work and
// `xlings use` silently did nothing for libraries.
LibraryPlacement library_placement(const VersionDB& db,
                                   const std::string& target,
                                   const std::string& version);

// True when some other entry names this one as the root of its release.
//
// Recipes for library-only packages write `xvm.add(package.name)` with no
// bindir and no programs -- they need a name to hang the release on, and the
// model offers no way to say "this is only a name". With `type` unset the C++
// side defaults it to "program", so the entry claims to be an executable that
// does not exist. On a real installation 31 entries are in exactly this
// state, and `self doctor` reported every one as a broken payload.
//
// Both shapes are recognised: the provider group written since 0.4.70
// (`bindingGroup.rootTarget`) and the legacy pairwise edges that precede it,
// where a member records `bindings[root][memberVersion] = rootVersion`.
bool is_binding_root(const VersionDB& db,
                     const std::string& target,
                     const std::string& version);

// Where one declared file asset has to be placed.
//
// `source` is absolute, inside the payload. `destination` is **relative to
// the subos root** and deliberately left unresolved: a payload is shared
// between subos, so the same asset lands at a different absolute path in
// each. The caller joins it with the subos it is materializing into.
struct FilePlacement {
    std::string source;
    std::string destination;

    [[nodiscard]] bool empty() const;
};

// Whether a package may write this destination.
//
// Absolute paths and anything walking upward are refused: the first would be
// right for one subos and wrong for the rest, the second escapes the subos
// altogether. `bin/` is excluded because it belongs to the shims. A recipe
// that trips this gets no placement rather than a surprising one.
bool is_permitted_file_destination(std::string_view destination);

FilePlacement file_placement(const VersionDB& db,
                             const std::string& target,
                             const std::string& version);

}  // namespace xlings::xvm

namespace xlings::xvm::detail_ {

BindingError binding_error_(BindingErrorKind kind,
                            const std::string& target,
                            const std::string& version,
                            std::string message);

bool has_canonical_manifest_(const VData& data);

std::optional<BindingError>
binding_integrity_error_(const VData& data,
                         const std::string& target,
                         const std::string& version);

std::optional<BindingError>
group_identity_integrity_error_(const BindingGroupRef& group,
                                const std::string& target,
                                const std::string& version);

std::optional<BindingError>
non_root_metadata_error_(const VData& data,
                         const std::string& target,
                         const std::string& version,
                         const BindingGroupRef& group);

std::expected<BindingSelection, BindingError>
resolve_provider_group_(const VersionDB& db,
                        const std::string& target,
                        const std::string& version,
                        const BindingGroupRef& group);

std::expected<BindingSelection, BindingError>
resolve_legacy_graph_(const VersionDB& db,
                      const std::string& target,
                      const std::string& version,
                      const std::map<
                          std::pair<std::string, std::string>,
                          std::vector<std::pair<
                              std::string, std::string>>>& incomingEdges);

}  // namespace xlings::xvm::detail_

namespace xlings::xvm {

const BindingSelectionResolver::IncomingEdges&
BindingSelectionResolver::legacy_incoming_edges_() {
    if (legacyIncomingEdges_) return *legacyIncomingEdges_;
    legacyIncomingEdges_.emplace();
    ++legacyIncomingIndexBuilds_;
    for (const auto& [sourceTarget, sourceInfo] : db_) {
        for (const auto& [destinationTarget, versions] :
             sourceInfo.bindings) {
            for (const auto& [sourceVersion, destinationVersion] :
                 versions) {
                (*legacyIncomingEdges_)[
                    {destinationTarget, destinationVersion}]
                    .emplace_back(sourceTarget, sourceVersion);
            }
        }
    }
    return *legacyIncomingEdges_;
}

std::expected<BindingSelection, BindingError>
BindingSelectionResolver::resolve(const std::string& target,
                                  const std::string& version) {
    const auto& db = db_;
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
    return detail_::resolve_legacy_graph_(
        db, target, version, legacy_incoming_edges_());
}

std::expected<BindingSelection, BindingError>
resolve_binding_selection(const VersionDB& db,
                          const std::string& target,
                          const std::string& version);

std::vector<HeaderAsset> group_header_assets(const VersionDB& db,
                                             const std::string& target,
                                             const std::string& version);

LibraryPlacement library_placement(const VersionDB& db,
                                   const std::string& target,
                                   const std::string& version);

bool is_permitted_file_destination(std::string_view destination);

FilePlacement file_placement(const VersionDB& db,
                             const std::string& target,
                             const std::string& version);

bool is_binding_root(const VersionDB& db,
                     const std::string& target,
                     const std::string& version);

}  // namespace xlings::xvm
