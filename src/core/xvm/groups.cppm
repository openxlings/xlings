export module xlings.core.xvm.groups;

import std;
import xlings.core.xvm.types;

export namespace xlings::xvm {

// A binding group as a first-class record.
//
// P2.1 of the 0.4.70 group-transaction design (§2.3). Today a group's
// properties -- its member table and its header assets -- live on the entry
// that happens to be one of its members, the "root". That placement is what
// creates three classes of problem the design enumerates: the root must
// reference itself (two separate invariants), every member carries a full
// copy of the group identity that can drift from it, and deleting the root
// deletes the whole manifest so removal needs a root-migration step.
//
// Here the group owns its own members and members point at it, which is what
// rpm, dpkg and Nix all do. Nothing downstream reads this yet -- resolver,
// registration and removal keep their current paths until P2.2 -- so this
// change adds a representation without altering behaviour.
struct BindingGroup {
    std::string id;
    std::string provider;
    std::string providerVersion;
    std::string group;
    std::map<std::string, std::string> members;  // target -> version
    std::vector<HeaderAsset> headers;

    // Which on-disk form this was reconstructed from. Kept because the three
    // forms do not carry the same information -- a legacy component has no
    // recorded provider at all -- and a caller deciding whether a group can
    // be trusted needs to know which it is looking at.
    std::string origin;  // "root-manifest" | "legacy-edges"
};

// groupId -> group. Sibling to VersionDB rather than a member of it, the same
// way Workspace is: VersionDB is a bare map alias that ~120 call sites take by
// reference, and changing its shape to carry a second table would touch every
// one of them for no gain at this step.
using BindingGroupTable = std::map<std::string, BindingGroup>;

// Group identity is (provider, providerVersion, group) -- the same triple
// `same_group_identity_` in bindings.cppm compares. rootTarget/rootVersion are
// deliberately NOT part of it: which member happens to hold the manifest is
// exactly the accident this normalization removes.
std::string binding_group_id(const BindingGroupRef& ref);

// A legacy edge component has no recorded provider, so it gets one derived
// from its own content. Deterministic and independent of iteration order: the
// lexicographically smallest member names the group, so the same component
// yields the same id on every load, which is what makes the normalized table
// comparable across runs.
std::string legacy_group_id(const std::map<std::string, std::string>& members);

namespace detail_ {

// The component of legacy pairwise edges reachable from (target, version).
//
// The nesting is `bindings[peerTarget][ownVersion] = peerVersion` -- peer
// first, own version second. Worth stating because it reads backwards and
// getting it wrong is not loud: db.cppm writes both directions of every edge
//
//     db[peer].bindings[target][peer_ver] = ver_key;
//     db[target].bindings[peer][ver_key]  = peer_ver;
//
// so a traversal that transposes the first two levels still finds *something*
// to walk and still returns plausible components. Against a real 246-target
// database the transposed version produced 398 overlapping "components" where
// there are 47 real ones.
//
// The relation is symmetric, so the component is closed under it. Iterative
// rather than recursive: a corrupt state file can describe a component as
// large as the whole database, and this runs on every load.
std::map<std::string, std::string>
legacy_component(const VersionDB& db,
                 const std::string& startTarget,
                 const std::string& startVersion);

} // namespace detail_

// Reconstruct every group in the database, from whichever form recorded it.
//
// Read-three-write-one (design §2.4), with one correction: the design assumed
// the root-manifest form had never shipped, because P0-P4 were to land on a
// single integration branch. P2 was then deferred to 0.4.71 and 0.4.70 went
// out, so root-manifest IS released -- a real installation on 2026-07-27
// carried 62 `bindingGroup` entries, 7 `bindingMembers` and 221 targets with
// legacy edges, all in the same file. All three are live and none can be
// dropped without a deprecation window.
//
// Precedence: root-manifest wins over legacy edges for the same identity. A
// state file that has both is one an upgraded client wrote over a legacy
// home, and the newer form is the one that carries the provider.
//
// Pure over `db`, like inspect.cppm: no reads of the filesystem or the clock,
// so every branch is reachable from a unit test.
BindingGroupTable normalize_binding_groups(const VersionDB& db);

// The group a given entry belongs to, or nullptr.
//
// What `resolve_provider_group_`'s 105 lines of cross-checking degrade to once
// the group owns its members: one lookup and one membership test.
const BindingGroup* find_binding_group(const BindingGroupTable& table,
                                       const std::string& target,
                                       const std::string& version);

} // namespace xlings::xvm
