export module xlings.core.xim.catalog;

import std;
import mcpplibs.xpkg;

import xlings.core.config;
import xlings.core.log;
import xlings.core.xim.payload;
import xlings.core.xim.index;
import xlings.core.xim.repo;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.semver;
import xlings.platform.target;

namespace xpkg = mcpplibs::xpkg;

export namespace xlings::xim {

struct RepoIndexSpec {
    std::string name;
    std::string url;
    std::filesystem::path dir;
    PackageScope scope { PackageScope::Global };
    std::string defaultNamespace;
    bool subIndex { false };  // sub-index repos: lower priority for bare-name resolution
};

struct PackageMatch {
    std::string query;
    std::string rawName;
    std::string name;
    std::string version;
    std::string namespaceName;
    std::string canonicalName;
    std::string repoName;
    std::filesystem::path pkgFile;
    std::filesystem::path storeRoot;
    PackageScope scope { PackageScope::Global };
    // The payload directory exists and holds something. Says nothing about
    // WHOSE platform built it -- see payloadForeign.
    bool installed { false };
    // The payload provably belongs to another platform. Kept separate from
    // `installed` because the two answer different questions: install must
    // treat this as "not installed" (so the artifact is downloaded again),
    // while remove, list and everything else must still see the payload that
    // is sitting on disk. Folding it into `installed` made `xlings remove`
    // refuse the very package the user was told to remove.
    bool payloadForeign { false };
    // Candidates that also answered to this bare name and lost on namespace
    // priority. Empty in the ordinary case.
    //
    // Carried on the match rather than logged and forgotten so a test can
    // assert the choice was made AND announced -- a demotion the user cannot
    // see is a silent pick, which is the shape this rule was added to remove,
    // not to introduce.
    std::vector<std::string> demoted;
};

// #374: a single index repo that could not be loaded during rebuild
// (no pkgs/, unsynced, empty redirect target). Recorded instead of
// aborting the whole catalog; the command layer surfaces these on the
// wire so a skipped repo is never silent.
struct RepoLoadWarning {
    std::string name;
    PackageScope scope { PackageScope::Global };
    std::filesystem::path dir;
    std::string error;
};

std::string canonical_package_name(std::string_view namespaceName, std::string_view name);

std::string package_store_name(std::string_view namespaceName, std::string_view name);

std::string package_scope_label(PackageScope scope);

// The parsed shape of a command-line coordinate: `[ns:]name[@version]`.
//
// Exported because the coordinate is now produced in more than one place --
// `self doctor` synthesises remedies -- and a renderer that cannot be checked
// against the parser is a renderer that drifts from it. Round-tripping is a
// unit test, not a hope.
struct ParsedPackageTarget {
    std::string raw;
    std::string name;
    std::string version;
    std::string namespaceName;
    bool explicitNamespace { false };
};

ParsedPackageTarget parse_package_target(std::string target);

std::string format_ambiguous_candidates(std::string_view target,
                                        std::span<const PackageMatch> matches);

namespace detail_ {

struct ParsedTarget_ {
    std::string raw;
    std::string name;
    std::string version;
    std::string namespaceName;
    bool explicitNamespace { false };
};

ParsedTarget_ parse_target_(std::string target);

}  // namespace detail_

// Definition sits here rather than beside the declaration: it delegates to
// detail_::parse_target_, which is what every other resolution path uses, so
// the exported form cannot drift from the internal one.
ParsedPackageTarget parse_package_target(std::string target);

namespace detail_ {

std::string select_version_(const xpkg::Package& pkg,
                            const std::string& platform,
                            const std::string& versionHint);

std::string make_canonical_name_(const std::string& namespaceName,
                                 const std::string& name);

bool same_match_identity_(const PackageMatch& lhs, const PackageMatch& rhs);

std::vector<PackageMatch> dedupe_matches_(
    std::vector<PackageMatch> matches);

// Namespace priority: `local` ranks below every other namespace.
//
// WHY A RULE AND NOT A REFUSAL
//
// Refusing an ambiguous bare name is the right answer when there is NO rule
// -- the deterministic-or-refuse contract (2026.7.30.2) exists because
// picking silently between two equally-valid answers is how a machine ends up
// running something nobody chose. But that argument only holds where the two
// candidates are peers.
//
// `local` is not a peer. It is the side-loading namespace: recipes a user or
// a dev script dropped into the local index, never fetched, never bumped, and
// never removed when they go stale. Measured on this machine: `local:xlings`
// carried a payload from June while `xim:xlings` was current, and because the
// two tied, `xlings self update` REFUSED -- so nothing upgraded the entry
// binary, and every shim in the home dispatched through a six-week-old client
// whose symptom surfaced three layers away as `ld: cannot find crt1.o`.
//
// A refusal is not a safe default when the refused operation is the one that
// repairs the machine. So: rank, announce, and keep the escape hatch.
//
// SCOPE, deliberately narrow:
//   * Only for a target with NO explicit namespace. `local:foo` is untouched;
//     the priority answers "the user did not say which", never "the user said
//     which and was overruled".
//   * Only `local` is demoted. There is no rule between `xim:` and `d2x:` --
//     inventing an order for them would be exactly the silent pick this
//     contract forbids -- so a tie among non-local namespaces still refuses.
//   * The loser is NAMED (PackageMatch::demoted -> one printed line). This
//     turns "undetermined" into "determined, and here is what it beat", which
//     is a different thing from determinism being dropped.
int namespace_rank_(std::string_view namespaceName);

struct NamespaceRankResult_ {
    std::vector<PackageMatch> kept;
    std::vector<std::string> demoted;   // "local:xlings@0.4.51"
};

// BY CONST REFERENCE, and it copies what it keeps.
//
// Both callers use their `matches` again when ranking fails to decide -- to
// build the ambiguity message, which must list every candidate. Taking this by
// value happens to be safe (the copy is what gets moved from), but the safety
// then lives in a signature rather than in the code that depends on it, and
// one edit to `&&` would silently produce an ambiguity message full of empty
// strings. A vector of a handful of matches is not worth that.
NamespaceRankResult_ prefer_namespace_rank_(
    const std::vector<PackageMatch>& matches);

std::vector<PackageMatch> prefer_project_scope_(
    std::vector<PackageMatch> matches);

}  // namespace detail_

namespace catalog_detail {

struct LocalIdentityRepoView {
    std::string repoName;
    PackageScope scope { PackageScope::Global };
    bool subIndex { false };
    const IndexManager* index { nullptr };
    std::filesystem::path storeRoot;
};

std::expected<PackageMatch, std::string>
resolve_local_identity_from_repos(
    std::span<const LocalIdentityRepoView> repos,
    const std::string& target);

}  // namespace catalog_detail

class PackageCatalog {
    struct RepoState {
        RepoIndexSpec spec;
        IndexManager index;
    };

    std::vector<RepoState> projectRepos_;
    std::vector<RepoState> globalRepos_;
    std::vector<RepoLoadWarning> loadWarnings_;
    // Recipe -> parsed package, for the lifetime of the process. See
    // load_package() for why.
    std::unordered_map<std::string, xpkg::Package> packageCache_;
    // What announce_demotion_ has already said. `mutable` because resolution
    // is const and the announcement is a property of the process, not of the
    // catalog's contents.
    mutable std::unordered_set<std::string> demotionsAnnounced_;
    bool loaded_ { false };

    static std::vector<RepoIndexSpec> repo_specs_();

    static RepoState make_state_(const RepoIndexSpec& spec);

    static std::vector<PackageMatch>
    build_matches_(const RepoState& state,
                   const detail_::ParsedTarget_& parsed,
                   const std::string& platform,
                   bool forSearch = false);

    std::vector<PackageMatch> collect_matches_(const std::string& target,
                                               const std::string& platform) const;

public:
    std::expected<void, std::string> rebuild(bool forceRebuild = false);

    bool is_loaded() const;

    // #374: repos skipped during the last rebuild (best-effort). The
    // command layer emits these on the EventStream so a degraded
    // multi-repo config is visible to interface consumers (mcpp), not
    // silently swallowed.
    const std::vector<RepoLoadWarning>& load_warnings() const;

    // When project and global have the same package, keep only project-scoped match
    static std::vector<PackageMatch> prefer_project_scope_(std::vector<PackageMatch> matches);

    // Say which candidate the namespace priority beat, exactly once.
    //
    // Emitted HERE rather than at each command, because `resolve_target` has
    // sixteen callers and several of them resolve the same target twice in one
    // run (planner, then installer). A per-command line would have to be added
    // sixteen times and would be forgotten somewhere -- the forgotten one is
    // then a silent pick, which is the whole thing this rule must not become.
    // One writer, deduplicated by what it actually said.
    void announce_demotion_(const std::string& target,
                            const PackageMatch& chosen) const;

    std::expected<PackageMatch, std::string>
    resolve_target(const std::string& target,
                   const std::string& platform) const;

    // Resolve an already-indexed package identity without evaluating its Lua
    // recipe, selecting a version, syncing a repository, or touching payload
    // contents. Inventory uses this as a leaf existence/uniqueness check so a
    // missing stamped identity cannot turn into an all-recipe scan.
    std::expected<PackageMatch, std::string>
    resolve_local_identity(const std::string& target) const;

    std::vector<PackageMatch> search(const std::string& query, const std::string& platform);

    std::expected<xpkg::Package, std::string> load_package(const PackageMatch& match);

    void mark_installed(const PackageMatch& match, bool installed);
};

// Resolve one package coordinate against an already-loaded catalog. This is a
// deliberately narrow leaf for local diagnostics: it never rebuilds or syncs
// the catalog, and it uses the same recipe version/ref selection, namespace
// ambiguity and project-preference rules as normal package resolution.
std::optional<PackageMatch>
resolve_local_coordinate(const PackageCatalog& catalog,
                         std::string_view coordinate);

}  // namespace xlings::xim
