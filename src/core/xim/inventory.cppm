export module xlings.core.xim.inventory;

import std;
import xlings.core.config;
import xlings.core.profile;
import xlings.core.xim.catalog;
import xlings.core.xim.install_state;
import xlings.core.xim.payload;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;
import xlings.core.xvm.owner;
import xlings.core.xvm.types;

// What is installed, taken from the records that decide it.
//
// Workspace records bound the ownership query. A current-SubOS list never
// resolves VersionDB entries owned only by another SubOS. Stamped payloads are
// the one exception because packages with no runnable target have no
// workspace record; those are found by a shallow scan of the two store roots.
export namespace xlings::xim {

struct InstalledPackageRecord {
    std::string namespaceName;
    std::string name;
    std::string canonicalName;
    std::string version;
    std::set<std::string> suboses;
    bool inCurrentSubos { false };
    bool payloadPresent { false };
    // The payload and the records contradict each other. Reported rather than
    // folded into `payloadPresent`: a package can have every file it needs and
    // still be unusable because nothing registered it, and that case used to
    // render identically to a healthy one. See xim/install_state.cppm.
    bool incomplete { false };
    std::string incompleteReason;
    bool active { false };
    std::string description;
    std::vector<std::string> programs;
    std::filesystem::path payloadPath;
    std::string degradedReason;
};

struct InventoryWorkspace {
    std::string name;
    xvm::Workspace active;
    xvm::WorkspaceInstalled installed;
    bool current { false };
};

// Optional instrumentation for structural query tests. These are the two
// operations whose fan-out made one-package queries scale with an entire
// home: evaluating recipe details and inspecting payload version roots.
struct InventoryTrace {
    std::vector<std::string> metadataIdentities;
    std::vector<std::filesystem::path> payloadVersionDirs;
    std::vector<std::string> bindingSelections;
    std::size_t legacyIncomingIndexBuilds { 0 };
};

std::pair<std::string, std::string> identity_from_store_name(
    std::string_view storeName);

namespace detail {

struct CatalogMetadata {
    std::string namespaceName;
    std::string name;
    std::string canonicalName;
    std::string description;
    std::vector<std::string> programs;
    std::filesystem::path storeRoot;
};

struct CatalogRecord {
    CatalogMetadata metadata;
    std::optional<PackageMatch> match;
    bool detailsLoaded { false };
};

// Identity lookup and recipe metadata are separate operations. Ownership only
// needs to know whether an identity exists. Description/program evaluation is
// deferred until a row has survived workspace scope and filtering.
class MetadataLookup {
public:
    MetadataLookup(PackageCatalog& catalog, InventoryTrace* trace = nullptr);

    explicit MetadataLookup(std::map<std::string, CatalogMetadata> table);

    const CatalogMetadata* by_identity(const std::string& namespaceName,
                                       const std::string& name);

    const CatalogMetadata* by_short_name(const std::string& name);

    const CatalogMetadata* load_details(const std::string& namespaceName,
                                        const std::string& name);

    const CatalogMetadata* load_stamped_details(
        const std::string& namespaceName, const std::string& name);

private:
    const CatalogMetadata* metadata_for_store_(const std::string& storeName);

    std::optional<CatalogRecord> resolve_(const std::string& target);

    PackageCatalog* catalog_ { nullptr };
    InventoryTrace* trace_ { nullptr };
    std::map<std::string, std::optional<CatalogRecord>> exact_;
    std::map<std::string, std::optional<std::string>> shortKeys_;
};

using TargetVersion = std::pair<std::string, std::string>;

struct OwnedCoordinate {
    xvm::InstallCoordinate coordinate;
    std::filesystem::path storeRoot;
};

using RelatedCoordinates = std::map<TargetVersion, OwnedCoordinate>;

enum class CoordinateMatch {
    contains,
    exact,
};

void trace_payload_visit(InventoryTrace* trace,
                         const std::filesystem::path& versionDir);

void push_candidate(std::vector<xvm::InstallCoordinate>& candidates,
                    xvm::InstallCoordinate candidate);

xvm::InstallCoordinate direct_coordinate(const std::string& target,
                                         const std::string& version);

const xvm::VData* version_data(const xvm::VersionDB& db,
                               const TargetVersion& pair);

struct BoundedSelection {
    std::map<std::string, std::string> members;
    xvm::BindingSource source { xvm::BindingSource::LegacyGraph };
    std::optional<TargetVersion> root;
};

using SelectionPtr = std::shared_ptr<const BoundedSelection>;
using SelectionCache = std::map<TargetVersion, SelectionPtr>;

// A standalone target needs no graph work. Once an entry carries binding
// state, use the same fail-closed resolver as switching and removal so an
// asymmetric incoming edge cannot manufacture a different inventory owner.
// Cache every validated component member so one requested group is resolved
// at most once.
SelectionPtr bounded_selection(const xvm::VersionDB& db,
                               const TargetVersion& seed,
                               SelectionCache& cache,
                               xvm::BindingSelectionResolver& resolver,
                               InventoryTrace* trace = nullptr);

bool legacy_root_in_selection(const xvm::VersionDB& db,
                              const BoundedSelection& selection,
                              const TargetVersion& candidate);

std::vector<xvm::InstallCoordinate> direct_owner_candidates_for(
    const xvm::VersionDB& db,
    const TargetVersion& pair);

using IncomingEdgeIndex =
    std::map<TargetVersion, std::vector<TargetVersion>>;

// Every `source -> destination` edge, read backwards. Built at most once per
// query and only when a filter is present.
IncomingEdgeIndex build_incoming_edges(const xvm::VersionDB& db);

// A cheap over-approximation of owner_candidates_for, used only to decide
// whether a filtered query can skip a pair before paying for full binding
// resolution. It MUST NOT be narrower than the real thing: a name this misses
// is a row that `list <filter>` silently omits while plain `list` shows it,
// and nothing reports the difference.
//
// That is why the legacy walk follows edges in BOTH directions. The real
// resolver builds an incoming index and reaches members through it; a walk
// that only follows a version's own outgoing edges misses every peer that
// binds TO it -- and `legacy_root_in_selection`, which decides which members
// become candidates, is precisely a question about incoming edges.
std::vector<xvm::InstallCoordinate> shallow_owner_candidates_for(
    const xvm::VersionDB& db,
    const TargetVersion& pair,
    const IncomingEdgeIndex* incoming = nullptr);

std::vector<xvm::InstallCoordinate> owner_candidates_for(
    const xvm::VersionDB& db,
    const TargetVersion& pair,
    SelectionCache& selectionCache,
    xvm::BindingSelectionResolver& resolver,
    InventoryTrace* trace = nullptr);

bool identity_matches(std::string_view namespaceName,
                      std::string_view name,
                      std::string_view filter,
                      CoordinateMatch match);

bool candidate_may_match(const xvm::InstallCoordinate& candidate,
                         std::string_view filter,
                         CoordinateMatch match);

OwnedCoordinate resolve_owner(
    std::span<const xvm::InstallCoordinate> candidates,
    const TargetVersion& pair,
    std::span<const std::filesystem::path> storeRoots,
    MetadataLookup& metadata,
    InventoryTrace* trace = nullptr);

RelatedCoordinates build_owner_coordinates(
    const xvm::VersionDB& db,
    const std::set<TargetVersion>& requested,
    std::string_view filter,
    std::span<const std::filesystem::path> storeRoots,
    MetadataLookup& metadata,
    InventoryTrace* trace = nullptr,
    CoordinateMatch match = CoordinateMatch::contains);

std::vector<InventoryWorkspace> load_inventory_workspaces(bool allSubos);

std::set<TargetVersion> requested_pairs(
    std::span<const InventoryWorkspace> workspaces);

std::vector<std::filesystem::path> inventory_store_roots();

std::vector<InstalledPackageRecord> assemble_inventory(
    const xvm::VersionDB& db,
    std::span<const InventoryWorkspace> workspaces,
    std::span<const std::filesystem::path> storeRoots,
    MetadataLookup& metadata,
    std::string_view filter,
    CoordinateMatch match,
    InventoryTrace* trace = nullptr,
    bool includePayloadOnly = true);

// Compatibility seam for the deterministic inventory tests and callers that
// intentionally exclude payload-only records. Production queries use the
// multi-root overload above.
std::vector<InstalledPackageRecord> assemble_inventory(
    const xvm::VersionDB& db,
    std::span<const InventoryWorkspace> workspaces,
    const std::filesystem::path& storeRoot,
    MetadataLookup& metadata,
    bool includePayloadOnly);

}  // namespace detail

std::vector<InstalledPackageRecord> collect_inventory_impl(
    PackageCatalog& catalog, bool allSubos,
    std::optional<std::string_view> filter,
    detail::CoordinateMatch match,
    InventoryTrace* trace);

std::vector<InstalledPackageRecord> collect_inventory(
    PackageCatalog& catalog, bool allSubos,
    std::optional<std::string_view> canonicalFilter = std::nullopt,
    InventoryTrace* trace = nullptr);

std::vector<InstalledPackageRecord> collect_package_inventory(
    PackageCatalog& catalog, const std::string& canonicalName,
    bool allSubos = true, InventoryTrace* trace = nullptr);

}  // namespace xlings::xim
