module xlings.core.xvm.inspect;

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.errors;
import xlings.core.xvm.db;

namespace xlings::xvm::detail_ {

bool release_declares_file_assets_(const VersionDB& db,
                                   const std::string& target,
                                   const std::string& version) {
    auto selection = resolve_binding_selection(db, target, version);
    if (!selection) return false;
    for (const auto& [memberTarget, memberVersion] : selection->members) {
        auto infoIt = db.find(memberTarget);
        if (infoIt == db.end()) continue;
        auto dataIt = infoIt->second.versions.find(memberVersion);
        if (dataIt == infoIt->second.versions.end()) continue;
        if (effective_kind(infoIt->second, dataIt->second) == "files") {
            return true;
        }
    }
    return false;
}

bool held_by_another_provider_(const VersionDB& db,
                               const Workspace& workspace,
                               const std::string& memberTarget,
                               const std::string& releaseProvider) {
    const auto activeIt = workspace.find(memberTarget);
    if (activeIt == workspace.end()) return false;
    const auto holderIt = db.find(memberTarget);
    if (holderIt == db.end()) return false;
    const auto heldIt = holderIt->second.versions.find(activeIt->second);
    if (heldIt == holderIt->second.versions.end()) return false;
    // No group metadata means an entry written before providers were
    // recorded. Treated as the same provider, so old homes keep the older,
    // stricter behaviour rather than silently losing a coherence check.
    if (!heldIt->second.bindingGroup) return false;
    return heldIt->second.bindingGroup->provider != releaseProvider;
}

}

namespace xlings::xvm {

std::vector<BindingFinding> inspect_binding_state(
        const VersionDB& db,
        const Workspace& workspace) {
    std::vector<BindingFinding> findings;

    // ── Metadata integrity ───────────────────────────────────────────
    for (const auto& [target, info] : db) {
        for (const auto& [version, data] : info.versions) {
            for (const auto& issue : data.bindingIntegrityIssues) {
                findings.push_back({
                    .code = "xvm-binding-metadata-corrupt",
                    .summary = std::format(
                        "stored binding metadata is unreadable ({})",
                        issue.code),
                    .target = target,
                    .version = version,
                    .field = issue.path,
                    // Reinstall first: it restores the release. The reset
                    // only discards it, and is the answer when the package
                    // is no longer installable.
                    .hint = "reinstall the package that owns this entry, or "
                            "run `xlings self doctor --reset-metadata` to "
                            "discard the unreadable release metadata",
                });
            }
        }
    }

    // ── INV-1: the workspace points at something that exists ─────────
    for (const auto& [target, version] : workspace) {
        auto infoIt = db.find(target);
        if (infoIt != db.end() && infoIt->second.versions.contains(version)) {
            continue;
        }
        findings.push_back({
            .code = "xvm-active-version-missing",
            .summary = "the active version is not registered",
            .target = target,
            .version = version,
            .hint = std::format(
                "run `xlings install {}` to restore it, or `xlings use {} "
                "<other-version>`", target, target),
        });
    }

    // ── INV-4: every group resolves ──────────────────────────────────
    //
    // One report per group rather than per member: a five-member toolchain
    // with one missing member is one problem, and printing it five times
    // buries the rest of the output.
    std::set<std::tuple<std::string, std::string, std::string>> seenGroups;
    for (const auto& [target, info] : db) {
        for (const auto& [version, data] : info.versions) {
            if (!data.bindingGroup) continue;
            if (!data.bindingIntegrityIssues.empty()) continue;  // already reported
            const auto& ref = *data.bindingGroup;
            if (!seenGroups.emplace(ref.provider, ref.providerVersion,
                                    ref.group).second) {
                continue;
            }
            auto selection = resolve_binding_selection(db, target, version);
            if (selection) continue;
            const auto described = describe(selection.error());
            findings.push_back({
                .code = described.code,
                .summary = std::format("release {}@{} does not resolve: {}",
                                       ref.provider, ref.providerVersion,
                                       described.what),
                .target = described.target,
                .version = described.version,
                .hint = described.hint,
            });
        }
    }

    // ── Dangling legacy edges ────────────────────────────────────────
    //
    // The INV-4 pass above only looks at entries that carry a
    // `bindingGroup`. Everything written before 0.4.70 does not, so its
    // pairwise edges were never checked -- and a pairwise edge pointing at a
    // version that no longer exists makes the whole release unresolvable.
    //
    // This is not hypothetical. On a real installation `gcc@15.1.0` carries
    // an edge to `xim-gnu-gcc@15.1.0`, an anchor that only exists at 16.1.0,
    // so `xlings use gcc 15.1.0` -- which worked on 0.4.68 -- is refused with
    // "a member of this release is registered at no such version". Nothing
    // reported it and `--fix` could not repair it, leaving reinstall as the
    // only way out. That is the upgrade being anything but seamless.
    //
    // An edge whose destination does not exist carries no information: it
    // cannot name a member to switch, it can only cause a refusal. Reporting
    // it separately from INV-4 keeps the remedy separate too -- this one is
    // repairable without guessing, and INV-4's is not.
    for (const auto& [target, info] : db) {
        for (const auto& [peerTarget, edges] : info.bindings) {
            auto peerIt = db.find(peerTarget);
            for (const auto& [ownVersion, peerVersion] : edges) {
                // Two ways an edge can name nothing, and both were treated
                // very differently for no reason.
                //
                // The peer side was reported and repaired. The SOURCE side --
                // an edge keyed by a version of the owner that is not itself
                // registered -- was skipped by this loop and by the pruning
                // plan, so it was invisible AND permanent. It is not
                // harmless: resolve_binding_selection walks it and fails with
                // "legacy binding source version is missing", which makes
                // every future registration of that target refuse. Measured
                // on a real home, `xim-musl-gnu-gcc` carries five such edges
                // keyed at `15.1.0-musl` while only `15.1.0` is registered,
                // and `xlings install musl-gcc@15.1.0` had been failing on it
                // with no finding anywhere that named the cause.
                const bool sourceMissing =
                    !info.versions.contains(ownVersion);
                const bool peerMissing =
                    peerIt == db.end()
                    || !peerIt->second.versions.contains(peerVersion);
                if (!sourceMissing && !peerMissing) continue;
                findings.push_back({
                    .code = "xvm-legacy-edge-dangling",
                    .summary = sourceMissing
                        ? std::format(
                            "a binding edge is keyed at '{}', which is not "
                            "registered",
                            display_coordinate(target, ownVersion))
                        : std::format(
                            "a binding edge points at '{}', which is not "
                            "registered",
                            display_coordinate(peerTarget, peerVersion)),
                    .target = target,
                    .version = ownVersion,
                    .field = std::format("/bindings/{}", peerTarget),
                    .hint = "run `xlings self doctor --fix` to drop the edge; "
                            "it names nothing and only blocks switching",
                });
            }
        }
    }

    // ── Sysroot assets placed before this client tracked them ────────
    //
    // Before 2026.7.27.0 a recipe put headers into the sysroot with plain
    // Lua inside `config()` -- `os.cp`, or `sysroot.install_headers`.
    // Nothing recorded it, so those files are invisible to the version
    // database and `xlings use` cannot move them.
    //
    // On its own that is not new: such a version could never switch its
    // headers. What upgrading changes is that it becomes *inconsistent* --
    // one version of a package now switches its headers and another
    // silently does not, and the failure surfaces as a build picking up the
    // wrong header, long after the `use` that reported success.
    //
    // Hence the pairing requirement: reported only when another version of
    // the same target does declare file assets. Without it, every package
    // that simply ships no headers would be flagged.
    for (const auto& [target, info] : db) {
        if (info.versions.size() < 2) continue;
        std::vector<std::string> untracked;
        bool anyTracked = false;
        for (const auto& [version, data] : info.versions) {
            // Only the entry point users actually switch. Members of a
            // release resolve to the same set, so checking them too would
            // report one problem once per library in the toolchain.
            if (effective_kind(info, data) != "program") continue;
            if (detail_::release_declares_file_assets_(db, target, version)) {
                anyTracked = true;
            } else {
                untracked.push_back(version);
            }
        }
        if (!anyTracked || untracked.empty()) continue;
        for (const auto& version : untracked) {
            findings.push_back({
                .code = "xvm-sysroot-untracked",
                .summary = std::format(
                    "installed before file tracking: sysroot files for {}@{} "
                    "will not follow a version switch", target, version),
                .target = target,
                .version = version,
                .hint = std::format(
                    "run `xlings install {}` to re-register them",
                    display_coordinate(target, version)),
                .severity = BindingSeverity::Notice,
            });
        }
    }

    // ── INV-2: an active release is active as a whole ────────────────
    std::set<std::string> reportedIncoherent;
    for (const auto& [target, version] : workspace) {
        auto infoIt = db.find(target);
        if (infoIt == db.end()) continue;
        auto dataIt = infoIt->second.versions.find(version);
        if (dataIt == infoIt->second.versions.end()) continue;
        if (!dataIt->second.bindingGroup) continue;
        auto selection = resolve_binding_selection(db, target, version);
        if (!selection) continue;  // reported by INV-4 already

        for (const auto& [memberTarget, memberVersion] : selection->members) {
            auto activeIt = workspace.find(memberTarget);
            const bool coherent = activeIt != workspace.end()
                && activeIt->second == memberVersion;
            if (coherent) continue;

            if (detail_::held_by_another_provider_(
                    db, workspace, memberTarget,
                    dataIt->second.bindingGroup->provider)) {
                continue;
            }
            const auto key = dataIt->second.bindingGroup->provider + "@"
                + dataIt->second.bindingGroup->providerVersion + "/"
                + memberTarget;
            if (!reportedIncoherent.insert(key).second) continue;
            findings.push_back({
                .code = "xvm-active-group-incoherent",
                .summary = std::format(
                    "'{}' is active at {} but '{}' of the same release is {}",
                    target, version, memberTarget,
                    activeIt == workspace.end()
                        ? std::string("not active")
                        : std::format("active at {}", activeIt->second)),
                .target = memberTarget,
                .version = memberVersion,
                .hint = std::format(
                    "run `xlings use {} {}` to bring the whole release back "
                    "in step", target, version),
            });
        }
    }

    return findings;
}

bool subos_claims(const Workspace& active,
                  const WorkspaceInstalled& installed,
                  const std::string& target,
                  const std::string& version) {
    if (auto it = active.find(target);
        it != active.end() && it->second == version) {
        return true;
    }
    if (auto it = installed.find(target); it != installed.end()) {
        return std::ranges::find(it->second, version) != it->second.end();
    }
    return false;
}

OwnershipVerdict subos_ownership(const Workspace& active,
                                 const WorkspaceInstalled& installed,
                                 const std::vector<SubosRef>& others,
                                 const std::string& target,
                                 const std::string& version) {
    OwnershipVerdict verdict;
    verdict.ownedHere = subos_claims(active, installed, target, version);
    for (const auto& other : others) {
        if (subos_claims(other.active, other.installed, target, version)) {
            verdict.otherSubos.push_back(other.subos);
        }
    }
    std::ranges::sort(verdict.otherSubos);
    auto dup = std::ranges::unique(verdict.otherSubos);
    verdict.otherSubos.erase(dup.begin(), dup.end());
    return verdict;
}

std::vector<BindingFinding> inspect_subos_references(
        const VersionDB& db,
        const std::vector<SubosRef>& others) {
    std::vector<BindingFinding> findings;

    const auto registered = [&](const std::string& target,
                                const std::string& version) {
        auto it = db.find(target);
        return it != db.end() && it->second.versions.contains(version);
    };

    for (const auto& other : others) {
        for (const auto& [target, version] : other.active) {
            if (version.empty() || registered(target, version)) continue;
            findings.push_back({
                .code = "xvm-subos-active-missing",
                .summary = std::format(
                    "subos '{}' has '{}' active at {}, which is not registered",
                    other.subos, target, version),
                .target = target,
                .version = version,
                .hint = std::format(
                    "run `xlings subos use {}` then `xlings install {}` to "
                    "restore it, or `xlings use {} <other-version>` there",
                    other.subos, display_coordinate(target, version), target),
            });
        }
        for (const auto& [target, versions] : other.installed) {
            for (const auto& version : versions) {
                if (version.empty() || registered(target, version)) continue;
                // Notice, not Broken: nothing dispatches through installed[],
                // so this subos still works. It matters because removal counts
                // references through this set -- a dangling entry keeps a
                // payload pinned against a version that no longer exists.
                findings.push_back({
                    .code = "xvm-subos-installed-dangling",
                    .summary = std::format(
                        "subos '{}' lists '{}' as installed, but it is not "
                        "registered", other.subos,
                        display_coordinate(target, version)),
                    .target = target,
                    .version = version,
                    .hint = std::format(
                        "run `xlings subos use {}` then `xlings remove {}` "
                        "to drop the stale entry", other.subos,
                        display_coordinate(target, version)),
                    .severity = BindingSeverity::Notice,
                });
            }
        }
    }
    return findings;
}

std::vector<BindingFinding> inspect_sysroot_ownership(
        const VersionDB& db,
        const Workspace& workspace,
        const std::vector<SysrootEntry>& entries,
        std::string_view payloadRoot) {
    std::vector<BindingFinding> findings;
    if (payloadRoot.empty()) return findings;

    // Destinations the active selection says should be ours.
    std::map<std::string, std::pair<std::string, std::string>> claimed;
    for (const auto& [target, version] : workspace) {
        auto infoIt = db.find(target);
        if (infoIt == db.end()) continue;
        auto dataIt = infoIt->second.versions.find(version);
        if (dataIt == infoIt->second.versions.end()) continue;
        if (!dataIt->second.fileDst.empty()) {
            claimed.emplace(dataIt->second.fileDst,
                            std::pair{target, version});
        }
    }

    for (const auto& entry : entries) {
        const bool ours = !entry.linkTarget.empty()
            && std::string_view{entry.linkTarget}.starts_with(payloadRoot);
        if (ours) continue;

        if (auto it = claimed.find(entry.path); it != claimed.end()) {
            // Declared, but what is on disk is not the link we would have
            // made. Something replaced it after the fact.
            findings.push_back({
                .code = "xvm-sysroot-drift",
                .summary = std::format(
                    "'{}' is declared by {}@{} but is not the link xlings "
                    "placed", entry.path, it->second.first,
                    it->second.second),
                .target = it->second.first,
                .version = it->second.second,
                .hint = std::format(
                    "run `xlings use {} {}` to put it back",
                    it->second.first, it->second.second),
            });
            continue;
        }

        // Nothing claims it. Could be the host image, could be a package
        // that placed it by hand and is now gone -- either way xlings will
        // not move it on a version switch, and cannot remove it.
        findings.push_back({
            .code = "xvm-sysroot-unmanaged",
            .summary = std::format(
                "'{}' is in the subos but no package declares it", entry.path),
            .hint = "left as-is; xlings only moves what a package declares",
            .severity = BindingSeverity::Notice,
        });
    }
    return findings;
}

SubosMetadataRepair plan_subos_metadata_repair(
        const VersionDB& db,
        const std::vector<SubosRef>& others) {
    const auto registered = [&](const std::string& target,
                                const std::string& version) {
        auto it = db.find(target);
        return it != db.end() && it->second.versions.contains(version);
    };

    SubosMetadataRepair plan;
    for (const auto& other : others) {
        SubosMetadataRepair::Entry entry{.subos = other.subos};
        for (const auto& [target, version] : other.active) {
            if (version.empty() || registered(target, version)) continue;
            entry.deactivate.push_back(target);
        }
        for (const auto& [target, versions] : other.installed) {
            for (const auto& version : versions) {
                if (version.empty() || registered(target, version)) continue;
                entry.dropInstalled.emplace_back(target, version);
            }
        }
        if (entry.deactivate.empty() && entry.dropInstalled.empty()) continue;
        plan.entries.push_back(std::move(entry));
    }
    return plan;
}

std::size_t apply_subos_metadata_repair(
        SubosWorkspace& workspace,
        const SubosMetadataRepair::Entry& entry) {
    std::size_t dropped = 0;
    for (const auto& target : entry.deactivate) {
        dropped += workspace.active.erase(target);
    }
    for (const auto& [target, version] : entry.dropInstalled) {
        auto it = workspace.installed.find(target);
        if (it == workspace.installed.end()) continue;
        const auto before = it->second.size();
        std::erase(it->second, version);
        dropped += before - it->second.size();
        if (it->second.empty()) workspace.installed.erase(it);
    }
    return dropped;
}

std::vector<std::string> plan_unregistered_active_deactivation(
        const VersionDB& db,
        const Workspace& workspace) {
    std::vector<std::string> targets;
    for (const auto& [target, version] : workspace) {
        if (version.empty()) continue;
        auto it = db.find(target);
        if (it != db.end() && it->second.versions.contains(version)) continue;
        targets.push_back(target);
    }
    return targets;
}

DeactivationPlan plan_incoherent_deactivation(const VersionDB& db,
                                              const Workspace& workspace) {
    DeactivationPlan plan;
    for (const auto& [target, version] : workspace) {
        auto infoIt = db.find(target);
        if (infoIt == db.end()) continue;
        auto dataIt = infoIt->second.versions.find(version);
        if (dataIt == infoIt->second.versions.end()) continue;
        if (!dataIt->second.bindingGroup) continue;
        auto selection = resolve_binding_selection(db, target, version);
        if (!selection) continue;  // unresolvable is a different problem

        const auto& ref = *dataIt->second.bindingGroup;

        // Exactly the predicate INV-2 reports with. A member whose name is
        // held by another provider is not this release breaking step, so it
        // must not put this release on the teardown list either -- which it
        // did until 2026.8.1.2, making `--fix` demolish releases `self doctor`
        // had just called healthy.
        const bool coherent = std::ranges::all_of(
            selection->members, [&](const auto& member) {
                auto activeIt = workspace.find(member.first);
                if (activeIt != workspace.end()
                    && activeIt->second == member.second) {
                    return true;
                }
                return detail_::held_by_another_provider_(
                    db, workspace, member.first, ref.provider);
            });
        if (coherent) continue;

        const auto label =
            std::format("{}@{}", ref.provider, ref.providerVersion);
        // Take down every member of this release that is active, not just
        // the entry point: leaving the rest behind would swap one
        // incoherent state for another.
        for (const auto& [memberTarget, memberVersion] : selection->members) {
            auto activeIt = workspace.find(memberTarget);
            if (activeIt == workspace.end()) continue;
            if (activeIt->second != memberVersion) continue;
            plan.targets.emplace(memberTarget, label);
        }
        plan.targets.emplace(target, label);
    }
    return plan;
}

MetadataReset plan_metadata_reset(const VersionDB& db) {
    MetadataReset plan;
    for (const auto& [target, info] : db) {
        for (const auto& [version, data] : info.versions) {
            if (data.bindingIntegrityIssues.empty()) continue;
            MetadataReset::Entry entry{.target = target, .version = version};
            for (const auto& issue : data.bindingIntegrityIssues) {
                entry.codes.push_back(issue.code);
            }
            plan.entries.push_back(std::move(entry));
        }
    }
    return plan;
}

std::size_t apply_metadata_reset(VersionDB& db, const MetadataReset& plan) {
    std::size_t cleared = 0;
    for (const auto& entry : plan.entries) {
        auto infoIt = db.find(entry.target);
        if (infoIt == db.end()) continue;
        auto dataIt = infoIt->second.versions.find(entry.version);
        if (dataIt == infoIt->second.versions.end()) continue;
        auto& data = dataIt->second;
        if (data.bindingIntegrityIssues.empty()) continue;

        // Everything that describes the release, including the preserved
        // originals -- leaving those behind would resurrect the corruption
        // on the next save, which is the one outcome a reset must not have.
        data.bindingGroup.reset();
        data.bindingMembers.clear();
        data.bindingHeaders.clear();
        data.bindingMembersDeclared = false;
        data.bindingHeadersDeclared = false;
        data.bindingIntegrityIssues.clear();
        data.bindingUnreadable.clear();
        ++cleared;
    }
    return cleared;
}

DanglingEdgePruning plan_dangling_edge_pruning(const VersionDB& db) {
    DanglingEdgePruning plan;
    for (const auto& [target, info] : db) {
        for (const auto& [peerTarget, edges] : info.bindings) {
            auto peerIt = db.find(peerTarget);
            for (const auto& [ownVersion, peerVersion] : edges) {
                // Source side as well as peer side -- see the matching note in
                // inspect_binding_state. Skipping the source side left an edge
                // that no command could remove and that made every
                // registration of its owner refuse.
                const bool sourceMissing =
                    !info.versions.contains(ownVersion);
                const bool peerMissing =
                    peerIt == db.end()
                    || !peerIt->second.versions.contains(peerVersion);
                if (!sourceMissing && !peerMissing) continue;
                plan.edges.push_back({target, ownVersion,
                                      peerTarget, peerVersion});
            }
        }
    }
    return plan;
}

std::size_t apply_dangling_edge_pruning(VersionDB& db,
                                        const DanglingEdgePruning& plan) {
    std::size_t dropped = 0;
    for (const auto& edge : plan.edges) {
        auto infoIt = db.find(edge.target);
        if (infoIt == db.end()) continue;
        auto peerIt = infoIt->second.bindings.find(edge.peerTarget);
        if (peerIt == infoIt->second.bindings.end()) continue;
        auto versionIt = peerIt->second.find(edge.version);
        if (versionIt == peerIt->second.end()) continue;
        if (versionIt->second != edge.peerVersion) continue;
        peerIt->second.erase(versionIt);
        ++dropped;
        if (peerIt->second.empty()) {
            infoIt->second.bindings.erase(peerIt);
        }
    }
    return dropped;
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xvm {

[[nodiscard]] bool SubosMetadataRepair::empty() const { return entries.empty(); }

[[nodiscard]] std::size_t SubosMetadataRepair::size() const {
    std::size_t n = 0;
    for (const auto& e : entries) {
        n += e.deactivate.size() + e.dropInstalled.size();
    }
    return n;
}

[[nodiscard]] bool MetadataReset::empty() const { return entries.empty(); }

[[nodiscard]] bool DanglingEdgePruning::empty() const { return edges.empty(); }

} // namespace xlings::xvm
