export module xlings.core.xvm.inspect;

import std;

import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.errors;

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
export namespace xlings::xvm {

struct BindingFinding {
    std::string code;     // shares the XvmUserError code space
    std::string summary;
    std::string target;
    std::string version;
    std::string field;    // JSON Pointer, when the problem is metadata-level
    std::string hint;
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
                    .hint = "reinstall the package that owns this entry",
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

        const bool coherent = std::ranges::all_of(
            selection->members, [&](const auto& member) {
                auto activeIt = workspace.find(member.first);
                return activeIt != workspace.end()
                    && activeIt->second == member.second;
            });
        if (coherent) continue;

        const auto& ref = *dataIt->second.bindingGroup;
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

}  // namespace xlings::xvm
