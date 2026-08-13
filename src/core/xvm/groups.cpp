module xlings.core.xvm.groups;

import std;
import xlings.core.xvm.types;

namespace xlings::xvm {

std::string binding_group_id(const BindingGroupRef& ref) {
    return ref.provider + "@" + ref.providerVersion + "/" + ref.group;
}

std::string legacy_group_id(const std::map<std::string, std::string>& members) {
    if (members.empty()) return {};
    const auto& [target, version] = *members.begin();
    return "legacy:" + target + "@" + version;
}

namespace detail_ {

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
            if (members.contains(peerTarget)) continue;

            // A peer whose version is not registered is not a member. The
            // edge names something nobody can switch to, so admitting it
            // would build a group with a hole in it -- and a `use` that
            // "succeeds" onto a version that does not exist is worse than the
            // refusal it replaced.
            //
            // This is the difference between the two shapes that both look
            // like a broken edge. `gcc@15.1.0 -> xim-gnu-gcc@15.1.0` where
            // xim-gnu-gcc exists only at 16.1.0 is a dangling edge, and
            // dropping it is repair (see XvmDanglingEdge). A pair where both
            // versions exist but only one direction of the edge was written
            // is a partial write, and reading it as a group is repair too.
            // Membership, not reverse-reachability, is what tells them apart.
            auto peerIt = db.find(peerTarget);
            if (peerIt == db.end()) continue;
            if (!peerIt->second.versions.contains(it->second)) continue;

            queue.emplace_back(peerTarget, it->second);
        }
    }
    return members;
}

}

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

const BindingGroup* find_binding_group(const BindingGroupTable& table,
                                       const std::string& target,
                                       const std::string& version) {
    for (const auto& [_, g] : table) {
        auto it = g.members.find(target);
        if (it != g.members.end() && it->second == version) return &g;
    }
    return nullptr;
}

}
