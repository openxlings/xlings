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
std::string binding_group_id(const BindingGroupRef& ref) {
    return ref.provider + "@" + ref.providerVersion + "/" + ref.group;
}

// A legacy edge component has no recorded provider, so it gets one derived
// from its own content. Deterministic and independent of iteration order: the
// lexicographically smallest member names the group, so the same component
// yields the same id on every load, which is what makes the normalized table
// comparable across runs.
std::string legacy_group_id(const std::map<std::string, std::string>& members) {
    if (members.empty()) return {};
    const auto& [target, version] = *members.begin();
    return "legacy:" + target + "@" + version;
}

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
                 const std::string& startVersion) {
    std::map<std::string, std::string> members;
    std::vector<std::pair<std::string, std::string>> queue;
    queue.emplace_back(startTarget, startVersion);

    while (!queue.empty()) {
        auto [target, version] = queue.back();
        queue.pop_back();
        if (!members.emplace(target, version).second) continue;

        auto infoIt = db.find(target);
        if (infoIt == db.end()) continue;
        for (const auto& [peerTarget, ownVersions] : infoIt->second.bindings) {
            auto it = ownVersions.find(version);
            if (it == ownVersions.end()) continue;
            if (!members.contains(peerTarget)) {
                queue.emplace_back(peerTarget, it->second);
            }
        }
    }
    return members;
}

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
BindingGroupTable normalize_binding_groups(const VersionDB& db) {
    BindingGroupTable table;

    // Pass 1 -- root manifests. The manifest lives on one member; find it and
    // lift it into a group of its own.
    for (const auto& [target, info] : db) {
        for (const auto& [version, data] : info.versions) {
            if (!data.bindingGroup) continue;
            if (data.bindingMembers.empty()
                && !data.bindingMembersDeclared) continue;

            auto id = binding_group_id(*data.bindingGroup);
            if (id == "@/") continue;  // identity entirely blank: not a group

            auto& g = table[id];
            if (!g.id.empty()) {
                // Two entries claim the same identity. Union the members
                // rather than letting iteration order pick a winner -- a
                // disagreement here is exactly the drift the top-level table
                // exists to make visible, and dropping half of it would hide
                // the evidence.
                for (const auto& [t, v] : data.bindingMembers) g.members.emplace(t, v);
                continue;
            }
            g.id              = id;
            g.provider        = data.bindingGroup->provider;
            g.providerVersion = data.bindingGroup->providerVersion;
            g.group           = data.bindingGroup->group;
            g.members         = data.bindingMembers;
            g.headers         = data.bindingHeaders;
            g.origin          = "root-manifest";
            // The holder is a member even when the manifest forgot to say so.
            // Under the old model that omission was an error
            // (RootMissingFromManifest); here the fact that the entry carries
            // the manifest already establishes membership.
            g.members.emplace(target, version);
        }
    }

    // Which (target, version) pairs pass 1 already accounts for. A legacy
    // component overlapping any of them describes a group that has since been
    // rewritten in the newer form, so it is not a second group.
    std::set<std::pair<std::string, std::string>> claimed;
    for (const auto& [_, g] : table) {
        for (const auto& [t, v] : g.members) claimed.emplace(t, v);
    }

    // Pass 2 -- legacy pairwise edges (<= 0.4.69).
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& [target, info] : db) {
        // Seeds are (target, ownVersion). The own version is the SECOND key --
        // `bindings[peerTarget][ownVersion]` -- so it has to be collected from
        // the inner maps. Reading it off the first key instead names a peer,
        // and every component then starts from a pair that does not exist.
        std::set<std::string> ownVersions;
        for (const auto& [peerTarget, versionMap] : info.bindings) {
            for (const auto& [ownVersion, peerVersion] : versionMap) {
                ownVersions.insert(ownVersion);
            }
        }
        for (const auto& version : ownVersions) {
            if (claimed.contains({target, version})) continue;
            if (seen.contains({target, version})) continue;

            auto members = detail_::legacy_component(db, target, version);
            for (const auto& [t, v] : members) seen.emplace(t, v);
            if (members.size() < 2) continue;  // an edge to nowhere is not a group

            // Overlap with a newer-form group anywhere in the component means
            // the component is a stale remnant of it, not a group of its own.
            bool overlaps = false;
            for (const auto& [t, v] : members) {
                if (claimed.contains({t, v})) { overlaps = true; break; }
            }
            if (overlaps) continue;

            auto id = legacy_group_id(members);

            // Components are disjoint, so their smallest members are distinct
            // and a collision here cannot happen. It is still handled by
            // merging rather than by `continue`: an earlier draft of the walk
            // transposed the edge nesting, which made components overlap, and
            // the `continue` then discarded 351 of 398 of them without a word.
            // Dropping on a should-be-impossible condition is how that stayed
            // invisible; unioning keeps the evidence if the invariant breaks.
            auto [it, inserted] = table.try_emplace(id);
            if (!inserted) {
                for (auto& [t, v] : members) it->second.members.emplace(t, v);
                continue;
            }
            it->second.id      = id;
            it->second.group   = id;
            it->second.members = std::move(members);
            it->second.origin  = "legacy-edges";
        }
    }

    return table;
}

// The group a given entry belongs to, or nullptr.
//
// What `resolve_provider_group_`'s 105 lines of cross-checking degrade to once
// the group owns its members: one lookup and one membership test.
const BindingGroup* find_binding_group(const BindingGroupTable& table,
                                       const std::string& target,
                                       const std::string& version) {
    for (const auto& [_, g] : table) {
        auto it = g.members.find(target);
        if (it != g.members.end() && it->second == version) return &g;
    }
    return nullptr;
}

} // namespace xlings::xvm
