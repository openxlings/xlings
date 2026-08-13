export module xlings.core.xvm.inspect;

import std;

import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.errors;
import xlings.core.xvm.db;   // display_coordinate

// Read-only inspection of the stored binding state.
//
// The selection layer fails closed: a malformed or dangling group makes
// `xlings use` refuse. That is the right call, but until something can *name*
// the offending entry the refusal is a dead end -- `self doctor` reported
// only shims and payloads, so a user whose group metadata went bad had
// nothing to look at but versions.json.
//
// Deliberately a pure function over (db, workspace). It touches no Config, no
// filesystem and no network, so every case below is reachable from a unit
// test, and doctor stays a renderer.
namespace xlings::xvm::detail_ {

// Does the release this version belongs to declare any file asset?
//
// Asked of the release rather than the entry itself because a file asset is
// registered as its own target (`<pkg>.files.<n>`) bound to the release, not
// as a field on the package's own entry.
bool release_declares_file_assets_(const VersionDB& db,
                                   const std::string& target,
                                   const std::string& version);

// Is this member's name currently held by a DIFFERENT provider?
//
// Two packages can register the same program name: node's release registers
// `npm`, and a standalone npm package registers it too. Whichever is active,
// the other release's member is "wrong" by version alone -- and treating that
// as a broken release is wrong twice over. As a report it tells the user to
// run a `use` that would take the name away from the package they chose; as a
// REPAIR it tears down a release that is working. A contest with the SAME
// provider is a genuinely split release and is neither.
//
// Extracted because the reporter and the repairer disagreed about it and that
// disagreement shipped. `analyze_bindings` (INV-2) learned this rule in
// 2026.8.1.1; `plan_incoherent_deactivation` -- which is what `self doctor
// --fix` actually executes -- did not. So doctor said "this home is fine" and
// `--fix`, on the same home, took `gcc` and `ld` down. One predicate, both
// callers: the two cannot drift again.
bool held_by_another_provider_(const VersionDB& db,
                               const Workspace& workspace,
                               const std::string& memberTarget,
                               const std::string& releaseProvider);

}  // namespace xlings::xvm::detail_

export namespace xlings::xvm {

enum class BindingSeverity {
    // Something is wrong now: a command will refuse, or has already done
    // the wrong thing.
    Broken,
    // Legacy state that still behaves exactly as it always did, but is
    // worth acting on. Reporting these as broken would paint an upgraded
    // installation red for a condition the upgrade did not create.
    Notice,
};

struct BindingFinding {
    std::string code;     // shares the XvmUserError code space
    std::string summary;
    std::string target;
    std::string version;
    std::string field;    // JSON Pointer, when the problem is metadata-level
    std::string hint;
    BindingSeverity severity { BindingSeverity::Broken };
};

// Invariants checked, in the order a user would want to hear about them:
//
//   INV-1  every active version is registered
//   INV-4  every group member is registered and its group resolves
//   INV-2  members of one release are active together, or not at all
//
// Metadata integrity comes first: a corrupt field explains every downstream
// symptom, so reporting the symptoms above it would just be noise.
std::vector<BindingFinding> inspect_binding_state(
        const VersionDB& db,
        const Workspace& workspace);

// One other subos's workspace, as the caller read it off disk.
//
// `active` and `installed` are that subos's own maps; the versions DB they
// refer to is shared by every subos in the home, which is the whole reason
// this type exists.
struct SubosRef {
    std::string        subos;
    Workspace          active;
    WorkspaceInstalled installed;
};

// Who owns a (target, version), from the point of view of the subos asking.
struct OwnershipVerdict {
    // The asking subos has it active or in its installed[] set.
    bool ownedHere { false };
    // Other subos that do, in name order. Empty AND !ownedHere means nobody
    // claims it -- see the note on `subos_ownership`.
    std::vector<std::string> otherSubos;
};

// Does this subos claim (target, version)?
//
// Either half counts: `active` is what the shims dispatch through, and
// `installed[]` is the per-subos opt-in list, which keeps a version claimed
// while some other version of the same target is the active one.
bool subos_claims(const Workspace& active,
                  const WorkspaceInstalled& installed,
                  const std::string& target,
                  const std::string& version);

// Ownership of a (target, version) across the home.
//
// Callers use this to decide whether a repair belongs to them. Note what
// "nobody claims it" means and does NOT mean: a home written by a client
// older than 0.4.19 has no `installed[]` at all, so every version that is not
// currently active comes back unclaimed. Treating unclaimed as "not mine to
// repair" would therefore disable the migration repair on exactly the cohort
// it was built for. Unclaimed is shared, and the subos asking may repair it.
OwnershipVerdict subos_ownership(const Workspace& active,
                                 const WorkspaceInstalled& installed,
                                 const std::vector<SubosRef>& others,
                                 const std::string& target,
                                 const std::string& version);

// What the OTHER subos of this home point at that the shared DB no longer has.
//
// Nothing else looks here. doctor's checks are scoped to the subos it runs in
// -- its binDir, its sysroot, its workspace -- while the versions DB and the
// payload store underneath are shared by all of them. So a subos whose active
// version was taken out from under it (by a removal in a third subos, or by a
// hand-edited state file) has no command that reports it: running doctor there
// finds it, but nothing tells you to go there.
//
// Read-only and never repaired from here. Fixing another subos's workspace
// means deciding what that subos should be pointing at, which is `xlings use`
// deciding, not `doctor` deciding. See §5 of
// .agents/docs/2026-07-28-multi-subos-repair-design.md.
//
// Pure over (db, others): the caller reads the files.
std::vector<BindingFinding> inspect_subos_references(
        const VersionDB& db,
        const std::vector<SubosRef>& others);

// One entry found in the subos, as the caller read it off disk.
struct SysrootEntry {
    // Relative to the subos root, e.g. "usr/include/openssl".
    std::string path;
    // Where the symlink points, absolute. Empty when the entry is a real
    // file or directory rather than a link.
    std::string linkTarget;
};

// Which sysroot entries xlings put there, and which it merely found.
//
// The 0.4.70 design calls for a persisted ledger, and also says why one is
// not required to answer this question: "ledger 是派生数据（可由扫描 +
// Selection 重建），存盘只是为了避免每次全量扫描 sysroot". The saved copy is
// an optimisation for the reconciler, and the reconciler does not exist yet.
// Deriving the answer instead means there is no second record that can drift
// from the filesystem -- which, for a question that is *about* whether the
// filesystem matches the records, would be the wrong kind of state to add.
//
// The rule is decidable from one readlink: everything xlings materializes is
// a link into the payload store -- declared file assets, libraries and the
// header farm alike. So an entry that is not such a link is not ours.
//
// Pure over (db, workspace, entries), like the rest of this module: the
// caller does the reading, every case below is reachable from a unit test.
std::vector<BindingFinding> inspect_sysroot_ownership(
        const VersionDB& db,
        const Workspace& workspace,
        const std::vector<SysrootEntry>& entries,
        std::string_view payloadRoot);

// What another subos's state file has to lose to stop pointing at nothing.
//
// Both repairs are pure deletions of a reference to a version the shared
// database does not have, which is why they can be carried out from a subos
// the user is not in: nothing is installed, nothing is chosen on their behalf,
// and no package is pulled into the current subos. That boundary is the one
// from 2026-07-28-multi-subos-repair-design.md §5 and it still holds -- what
// changes is only that a deletion doctor could already describe exactly is now
// performed instead of dictated.
//
// `deactivate` is deliberately not "re-point at some surviving version".
// Picking one would be deciding what that subos should use, which is `xlings
// use` deciding. An inactive target is a visible problem with a one-command
// cure; a silently re-pointed one is a build that fails much later.
struct SubosMetadataRepair {
    struct Entry {
        std::string subos;
        // Targets to drop from `active`: they name a version nothing has.
        std::vector<std::string> deactivate;
        // (target, version) pairs to drop from `installed[]`: they pin a
        // payload against a version that no longer exists.
        std::vector<std::pair<std::string, std::string>> dropInstalled;
    };
    std::vector<Entry> entries;

    [[nodiscard]] bool empty() const { return entries.empty(); }
    [[nodiscard]] std::size_t size() const {
        std::size_t n = 0;
        for (const auto& e : entries) {
            n += e.deactivate.size() + e.dropInstalled.size();
        }
        return n;
    }
};

// Pure: returns the plan, applies nothing. Mirrors inspect_subos_references
// one-for-one, so anything reported is repairable and anything repaired was
// reported.
SubosMetadataRepair plan_subos_metadata_repair(
        const VersionDB& db,
        const std::vector<SubosRef>& others);

// Apply one subos's share of the plan to its own workspace maps.
// Returns how many references were dropped.
std::size_t apply_subos_metadata_repair(
        SubosWorkspace& workspace,
        const SubosMetadataRepair::Entry& entry);

// Targets whose ACTIVE version this subos no longer has registered.
//
// The current-subos twin of `xvm-subos-active-missing`, and the one INV-1 case
// `--fix` never had an answer for. Same remedy for the same reason: an active
// pointer into nothing cannot dispatch, and choosing a replacement would be
// choosing for the user.
std::vector<std::string> plan_unregistered_active_deactivation(
        const VersionDB& db,
        const Workspace& workspace);

struct DeactivationPlan {
    // Targets to drop from the active workspace, and the release each
    // belonged to, so the caller can tell the user what to re-select.
    std::map<std::string, std::string> targets;  // target -> "provider@version"
};

// Bring the workspace back to INV-2 by deactivating every release whose
// members disagree.
//
// Deactivating rather than repairing is deliberate. Repair would mean
// choosing which member is "right", and there is no basis for that choice:
// the user may have meant either release, and picking one silently is how
// the incoherent state arises in the first place. An inactive toolchain is a
// visible problem the user resolves with one `xlings use`; an incoherent one
// reports the right version from `gcc --version` and fails much later.
//
// Pure: returns the plan, applies nothing.
DeactivationPlan plan_incoherent_deactivation(const VersionDB& db,
                                              const Workspace& workspace);

// Entries whose binding metadata cannot be read, and would be discarded.
struct MetadataReset {
    struct Entry {
        std::string target;
        std::string version;
        // The codes that made this entry unreadable, so the user sees what
        // is being thrown away rather than a bare count.
        std::vector<std::string> codes;
    };
    std::vector<Entry> entries;

    [[nodiscard]] bool empty() const { return entries.empty(); }
};

// Collect every entry carrying unreadable binding metadata.
//
// Unlike the two repairs above this one *loses information*: the group, its
// members and its header assets are dropped and the entry becomes a legacy
// singleton, switchable on its own. That is why it sits behind its own flag
// instead of plain `--fix`.
//
// It is only offerable at all because load/save is now lossless for corrupt
// entries (`VData::bindingUnreadable`). Before that, the original was already
// gone by the time anyone could ask for it, and "reset" would have been
// indistinguishable from the silent rewrite that destroyed it.
//
// Pure: returns the plan, applies nothing.
MetadataReset plan_metadata_reset(const VersionDB& db);

// Apply the reset. Returns how many entries were cleared.
std::size_t apply_metadata_reset(VersionDB& db, const MetadataReset& plan);

// Edges to drop: (owner target, owner version, peer target, peer version).
struct DanglingEdgePruning {
    struct Edge {
        std::string target;
        std::string version;
        std::string peerTarget;
        std::string peerVersion;
    };
    std::vector<Edge> edges;

    [[nodiscard]] bool empty() const { return edges.empty(); }
};

// Collect every pairwise binding edge whose destination is not registered.
//
// Safe to apply without guessing, which is what separates it from the
// incoherent-release case: the edge names a version that does not exist, so
// it cannot be describing a member anyone could switch to. Keeping it only
// makes `resolve_binding_selection` refuse the release.
//
// Pure: returns the plan, applies nothing.
DanglingEdgePruning plan_dangling_edge_pruning(const VersionDB& db);

// Apply the pruning. Returns how many edges were dropped.
std::size_t apply_dangling_edge_pruning(VersionDB& db,
                                        const DanglingEdgePruning& plan);

}  // namespace xlings::xvm
