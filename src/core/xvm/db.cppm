export module xlings.core.xvm.db;

import std;

import xlings.core.xvm.types;
import xlings.libs.json;
import xlings.platform;

export namespace xlings::xvm {

// Parse "ns:ver" into (namespace, version). Plain "ver" → ("", "ver").
std::pair<std::string, std::string> parse_ns_version(const std::string& s);

// Build "ns:ver" from namespace and version. Empty namespace → plain "ver".
std::string make_ns_version(const std::string& ns, const std::string& ver);

// Strip namespace prefix, returning bare version.
std::string strip_namespace(const std::string& s);

// Get namespace prefix (empty if none).
std::string get_namespace(const std::string& s);

// How a (target, version-key) pair is written for a human, and for a shell.
//
// The two forms are REVERSED and that is the whole reason this exists. The
// database keys a version as "ns:ver" -- the namespace rides on the version.
// Everything a user types keys it as "ns:target@ver" -- the namespace rides on
// the front of the coordinate. Code that formats a finding as "{}@{}" produces
// `mcpp@local:0.0.27`, which reads like a version called `local:0.0.27` and
// parses as one too: `xlings install mcpp@local:0.0.27` looks up a version that
// does not exist. Every renderer goes through here instead.
//
//   ("mcpp", "local:0.0.27")  -> "local:mcpp@0.0.27"
//   ("llvm", "20.1.7")        -> "llvm@20.1.7"
//   ("gcc",  "")              -> "gcc"
std::string display_coordinate(std::string_view target,
                               std::string_view versionKey);

// Add or update a version entry in the database.
// If ns is non-empty, the version key becomes "ns:version".
void add_version(VersionDB& db,
                 const std::string& target,
                 const std::string& version,
                 const std::string& path,
                 const std::string& type = "program",
                 const std::string& filename = "",
                 const std::string& alias = "",
                 const std::string& ns = "",
                 const std::string& binding = "");

// Remove a version from the database.
// Namespaced input must match exactly. Bare input resolves only when exactly
// one stored key has that bare version.
enum class RemovalErrorKind {
    VersionNotFound,
    AmbiguousVersion,
    AsymmetricEdge,
    SelectionInvalid,
    ProviderRequired,
    ProviderMismatch,
    ProviderVersionNotFound,
    VersionMismatch,
};

struct RemovalError {
    RemovalErrorKind kind { RemovalErrorKind::VersionNotFound };
    std::string target;
    std::string version;
    std::string peerTarget;
    std::string peerVersion;
    std::string message;
};

// Resolve a version as the user or a recipe wrote it to the key it is stored
// under. An exact key wins. Otherwise a bare query stands for that version
// under any namespace, and a namespaced query stands for the BARE spelling of
// the same version as well -- see version_key_matches. Keys that are twins
// (below) collapse to one answer; anything else that matches more than one
// stored key is ambiguous and says so.
std::expected<std::string, RemovalError>
resolve_exact_version_key(const VersionDB& db,
                          const std::string& target,
                          const std::string& version);

std::expected<std::string, RemovalError>
remove_version(VersionDB& db,
               const std::string& target,
               const std::string& version);

// ── Version-key identity ─────────────────────────────────────────────
//
// A version key is "ns:ver" for a package from a non-default index and bare
// "ver" for one from the default index. That spelling is decided ONCE, when
// the record is first written, and the helpers below exist so that nothing
// re-derives it later:
//
//   - a reader handed one spelling must still find the record stored under
//     the other (version_key_matches, resolve_exact_version_key,
//     match_version);
//   - a writer registering a release this provider already registered must
//     reuse the spelling on disk (registered_namespace_for);
//   - two records that differ only in spelling and name the same payload are
//     one registration written twice, and every reader treats them as one
//     (twin_version_keys, representative_version_key); `self doctor` reports
//     and merges them (plan_twin_merges).
//
// Why this exists, measured on a real home on 2026-09-02: the default index's
// entry had been moved out of `index_repos[0]`, the spelling verdict followed
// it, and the same package keyed its versions two ways on one machine. 767
// targets could no longer be removed, 10 could no longer be installed and 240
// versions were registered twice. The verdict no longer reads array position
// (xim/installer.cpp version_namespace_), and the helpers here make the
// records that were written meanwhile reachable and repairable.
// .agents/docs/2026-09-02-version-key-namespace-flip-plan.md

// May a key written as `query` refer to the record stored as `stored`?
// Equal keys, of course. Otherwise the bare versions must agree and at least
// one side must be the bare spelling: the default index's records are bare,
// while a version under some OTHER namespace is another index's payload.
bool version_key_matches(std::string_view query, std::string_view stored);

// Separators and a trailing slash normalised, nothing else. Two writers of
// the same payload record the same string; this only forgives the spellings
// known to differ between platforms.
std::string normalized_payload_path(std::string_view path);

// Is `candidate` the payload at `root`, or something inside it?
bool payload_path_covers(std::string_view root, std::string_view candidate);

// Every key under `target` that records the same registration as `key`: the
// key itself, plus any other key with the same bare version whose payload
// path is the same. Empty when `target` or `key` is unknown.
std::vector<std::string> twin_version_keys(const VersionDB& db,
                                           const std::string& target,
                                           const std::string& key);

// The member of `key`'s twin set that carries the registration -- the one
// with a binding group -- or `key` itself when none does.
std::string representative_version_key(const VersionDB& db,
                                       const std::string& target,
                                       const std::string& key);

// The namespace (possibly empty) this provider's release is already
// registered under, if it is registered at all: an owned record of
// (provider, providerVersion) anywhere in the database, else an owner-less
// record of `primaryTarget` at that bare version whose payload lies under
// `payloadPath`. nullopt when nothing on disk answers, and the caller minds
// today's verdict.
std::optional<std::string> registered_namespace_for(
        const VersionDB& db,
        const std::string& provider,
        const std::string& providerVersion,
        const std::string& primaryTarget,
        const std::string& payloadPath);

// One twin pair to collapse: `loser` is dropped and every workspace reference
// to it is rewritten to `winner`.
struct TwinMerge {
    std::string target;
    std::string winner;
    std::string loser;
};

// Every twin pair in the database. This is the predicate the doctor reports
// with AND the predicate `--fix` repairs with, so the two cannot drift.
std::vector<TwinMerge> plan_twin_merges(const VersionDB& db);

// Rewrite one workspace's references from the loser to the winner. Returns
// how many entries changed.
std::size_t apply_twin_merge_to_workspace(Workspace& active,
                                          WorkspaceInstalled& installed,
                                          const TwinMerge& merge);

// Drop the loser record and every legacy binding edge that names it.
void apply_twin_merge_to_db(VersionDB& db, const TwinMerge& merge);

// Pick the highest semver key from a version map (descending by dotted numeric components,
// then by component count). Namespace prefix is stripped before comparison, but the
// original key is returned so callers can write it back to the workspace as-is.
// Returns empty string if the map is empty.
// Order two version keys highest-first: dotted numeric components descending,
// then the longer component list, then the raw key as a total tiebreak.
//
// The raw-key tiebreak is what makes this a strict weak ordering. Without it
// two keys that differ only by namespace ("15.1.0" vs "musl:15.1.0") compare
// equal in both directions, so which one a sort leaves in front depends on the
// input order. Callers that pick "the highest" would then return different
// answers for the same set depending on how it happened to be built.
bool version_key_greater(const std::string& lhs, const std::string& rhs);

std::string pick_highest_version(const std::map<std::string, VData>& versions);

// Fuzzy version match: "15" -> "15.1.0", "ns:14.2" -> "ns:14.2.0"
// Matching priority:
//   1. Exact match
//   2. If namespace specified: prefix-match only within that namespace
//   3. If no namespace: prefer bare (unqualified) versions, then fallback to any namespace
std::string match_version(const VersionDB& db,
                          const std::string& target,
                          const std::string& prefix);

// Get the active version for a target from a workspace
std::string get_active_version(const Workspace& workspace,
                               const std::string& target);

// Check if a target has any versions registered
bool has_target(const VersionDB& db, const std::string& target);

// Check if a specific version exists
bool has_version(const VersionDB& db,
                 const std::string& target,
                 const std::string& version);

// Get all version strings for a target
std::vector<std::string> get_all_versions(const VersionDB& db,
                                          const std::string& target);

// Get VData for a specific version
const VData* get_vdata(const VersionDB& db,
                       const std::string& target,
                       const std::string& version);

// Get VInfo for a target
const VInfo* get_vinfo(const VersionDB& db, const std::string& target);

// Both accessors above hand back a pointer INTO the map, so passing a
// temporary map is a use-after-free every time -- the map dies at the end of
// the full expression and the caller is left holding a pointer into freed
// storage.
//
// That is not hypothetical. `Config::versions()` and
// `Config::effective_workspace()` return BY VALUE (global and project state
// are merged into a fresh map), and
//
//     const auto* vinfo = xvm::get_vinfo(Config::versions(), name);
//
// compiled cleanly, shipped in three releases, and SIGSEGV'd in the field
// (#432) when the next line walked the freed map and read a stack address as
// a string length.
//
// A `const&` parameter binds to a temporary, so nothing stopped it. Deleting
// the rvalue overload does: the same line is now a compile error at every
// call site, present and future. `const VersionDB&&` rather than
// `VersionDB&&` so a const temporary is caught too.
//
// The point fix in #432 corrected one call site; this is what makes the class
// of bug unwritable instead of a comment someone has to remember to read.
const VData* get_vdata(const VersionDB&&, const std::string&,
                       const std::string&) = delete;
const VInfo* get_vinfo(const VersionDB&&, const std::string&) = delete;

// Get binding for a target (e.g., g++ -> g++-15 for gcc version 15.1.0)
std::string get_binding(const VersionDB& db,
                        const std::string& target,
                        const std::string& binding_name,
                        const std::string& version);

// ── subos-relative alias/env normalization ───────────────────────────
//
// `gcc.lua` bakes `--sysroot=<subos_sysrootdir()>` -- the ABSOLUTE path of
// whichever subos was active at install time -- into the alias. The versions
// DB is shared by the entire home and `subos use` rewrites nothing, so that
// path outlives every switch: the user switches to `default` and their g++
// keeps compiling against `dev-hello`, against a sysroot that may not even
// exist any more. Re-point such paths at the subos THIS process resolves to,
// at the moment the alias is executed. Doing it here rather than at install
// time is what makes existing homes correct without a reinstall.
//
// Only provably-ours paths are touched: the segment before /subos/ must be the
// home itself, or end in `.xlings` (which is how a PROJECT subos --
// <projectDir>/.xlings/subos/<name>, not under homeDir at all -- is caught).
// A user's own /opt/subos/foo, the flags around the path, and all quoting come
// through byte-identical.
std::string normalize_subos_paths(const std::string& text,
                                  const std::string& xlings_home,
                                  const std::string& active_subos_dir);

// The marker a recipe writes when it needs the ACTIVE subos, and its
// expansion at execution time.
//
// `${XLINGS_HOME}` (expand_path, below) has always been how this codebase
// keeps a home-relative path out of a stored record. This is the same
// mechanism one level down, and it exists for the same reason: the versions
// database is shared by every subos in the home, so a value naming one of
// them is right for the subos that installed the package and wrong for all
// the others -- the rule VData::fileSrc/fileDst has carried since the `files`
// kind was added.
//
// **The core knows this marker and nothing else.** It does not know
// `--sysroot`, `-isysroot` or `--gcc-toolchain=`; how a tool wants to be told
// is the recipe's business, and a new toolchain with a new spelling needs no
// change here. What xlings owns is the dictionary of runtime FACTS a recipe
// can reference by name; what a recipe owns is the syntax it renders them
// into. An earlier attempt inverted that -- registration recognised
// `--sysroot` and the shim re-emitted it -- which put one compiler's flag
// inside a generic version manager and still did nothing for `envs`.
//
// Measured before choosing: of 184 `xvm.add` calls in the package index,
// exactly one (gcc.lua) puts a subos path into an alias. musl-gcc.lua also
// writes absolute paths there, but they point into the PAYLOAD and are
// therefore correct from every subos -- which is why the rule is "no concrete
// SUBOS path", not "no absolute path".
inline constexpr std::string_view kSubosPlaceholder =
    "${XLINGS_DYNAMIC_SUBOS_DIR}";

// Rewrite every concrete subos path of this home to the marker.
//
// A REPAIR primitive, not an ingestion filter: registration stores what the
// recipe wrote, verbatim. This is what `self doctor --fix` applies to a
// record that pinned one subos, and what the detection side asks ("would this
// change?") so that detection and repair cannot drift.
//
// Implemented as normalize_subos_paths with the marker as the destination --
// literally the same traversal, so the two can never disagree about which
// paths are ours.
std::string pin_subos_paths(const std::string& text,
                            const std::string& xlings_home);

// A `${XLINGS_*}` reference in a command this client is about to hand to a
// shell, which that shell would expand to NOTHING. Returns the name
// (`XLINGS_FOO`), or nullopt when there is no such reference.
//
// WHY THIS EXISTS
//
// A record is written by the INDEX and read by whatever client the user has.
// When the index starts using a marker a client predates, that client passes
// it through untouched -- and a shell expands an unset variable to the EMPTY
// STRING.
//
// Empty is worse than literal. `gcc --sysroot=${XLINGS_DYNAMIC_SUBOS_DIR}`
// reaching a shell became `--sysroot=`, which gcc accepts and reads as "the
// host root": self-containment was lost with no error anywhere, for five days,
// and the symptom surfaced three layers away as `cannot find crt1.o`. The
// literal would have made gcc complain on the spot.
//
// WHY THE TEST IS "WOULD EXPAND TO EMPTY" AND NOT "IS A MARKER"
//
// A name list is the obvious implementation and it is wrong in both
// directions. `${XLINGS_SUBOS_LIB}` in an alias is LEGITIMATE -- xlings
// exports that variable, so the recipe is deliberately deferring to the
// environment (the `ld` wrapper does exactly this) -- and a list of "markers
// we expand" would reject it. Meanwhile the marker that actually caused the
// outage is one a list written today cannot contain, because the whole failure
// mode is a name INVENTED AFTER this client shipped.
//
// So ask the question the damage is actually about: at the moment of exec,
// with the child's environment already set up, would this reference vanish?
// That is one `getenv` and it needs no dictionary. It catches a marker from a
// newer index, catches an unexpanded `${XLINGS_DYNAMIC_SUBOS_DIR}` when no
// subos resolved, and lets every genuinely-exported name through.
//
// `${XLINGS_*}` only, never `${...}`: a recipe may reference any other shell
// variable and mean it, and that is not xlings's business to police.
std::optional<std::string> vanishing_xlings_reference(std::string_view text);

std::string expand_subos_placeholder(const std::string& text,
                                     const std::string& subos_dir);

// Expand ${XLINGS_HOME} in a path string
std::string expand_path(const std::string& path, const std::string& xlings_home);

// --- JSON serialization ---

nlohmann::json vdata_to_json(const VData& vdata);

VData vdata_from_json(const nlohmann::json& j);

nlohmann::json vinfo_to_json(const VInfo& info);

VInfo vinfo_from_json(const nlohmann::json& j);

nlohmann::json versions_to_json(const VersionDB& db);

VersionDB versions_from_json(const nlohmann::json& j);

// Resolve a project-style workspace value (string | platform-conditional
// object) to a single version string. Lifted out of workspace_from_json
// so subos_workspace_from_json can reuse the same fallback semantics
// when it falls through to the form-(3) platform branch. Returns
// std::nullopt when the value has no matching platform key and no
// `default` key — caller decides whether to skip or error.
inline std::optional<std::string>
resolve_platform_workspace_value_(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (!value.is_object()) {
        return std::nullopt;
    }
    if (!platform::OS_NAME.empty()) {
        auto platformIt = value.find(std::string(platform::OS_NAME));
        if (platformIt != value.end() && platformIt->is_string()) {
            return platformIt->get<std::string>();
        }
    }
    if (auto defaultIt = value.find("default");
        defaultIt != value.end() && defaultIt->is_string()) {
        return defaultIt->get<std::string>();
    }
    return std::nullopt;
}

Workspace workspace_from_json(const nlohmann::json& j);

nlohmann::json workspace_to_json(const Workspace& ws);

// Subos-flavored workspace parser. Accepts three value shapes per target:
//
//   1. string                      (legacy, pre-0.4.19) — active version,
//                                  installed[] implied to be {active}
//   2. {active, installed[]}       (current, 0.4.19+)   — both fields
//   3. {linux|windows|macosx|...}  (project-style)      — resolved to a
//                                  single string the same way project files
//                                  resolve it; subos files normally don't
//                                  have this shape but we accept it as a
//                                  graceful fallback so users who ever
//                                  hand-edit a subos file aren't surprised
//
// Disambiguation between (2) and (3) is by reserved-key detection: the
// presence of an "active" or "installed" key marks form (2). This is the
// "Plan 1" disambiguation strategy from the C2 design discussion — `active`
// and `installed` cannot meaningfully be platform names, so the two object
// shapes can never collide.
SubosWorkspace subos_workspace_from_json(const nlohmann::json& j);

// Subos workspace serializer. Always emits the C2 object form with
// `active` and `installed` keys. The `installed` set is normalized to
// always include the active version (write-time invariant: an active
// version is implicitly installed). `installed` is omitted from output
// when empty, keeping legacy entries — those that were read in form (1)
// and never had an install/remove since — visually compact.
//
// Stable sort on installed[] so json diffs remain reviewable across runs.
nlohmann::json subos_workspace_to_json(const SubosWorkspace& sws);

} // namespace xlings::xvm
