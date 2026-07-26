export module xlings.core.xvm.switch_plan;

import std;

import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.errors;

// Decide what `xlings use` should do, without doing any of it.
//
// The old path interleaved the decision with the work: it swapped the entry
// target's headers, then walked binding edges by hand to find everything else.
// Two consequences. The walk was keyed by target rather than by
// (target, version) and never checked that what it reached existed, so a
// stale edge put a version with no payload into the active workspace and the
// shim failed later, far from the command that caused it. And because the
// swap ran first, a failure part-way through left the sysroot holding one
// release while the workspace claimed another.
//
// Separating the two makes the failure mode "an error message" rather than
// "a half-switched toolchain", and makes the interesting logic reachable from
// a unit test -- cmd_use needs a Config singleton and a real filesystem, so
// nothing inside it was testable before.
export namespace xlings::xvm {

// What has to change on disk for one member of the release.
struct MemberSwitch {
    std::string target;
    std::string version;
    std::string previousVersion;   // empty when the member was not active
    std::string removeIncludeDir;  // empty when there is nothing to remove
    std::string removeLibDir;
    std::string installIncludeDir;
    std::string installLibDir;
};

struct UseSwitchPlan {
    std::map<std::string, std::string> members;  // target -> version
    std::vector<MemberSwitch> switches;          // only members that move
};

// Resolve the release and work out the filesystem changes.
//
// Fails closed: an unresolvable group, or a member with no payload record,
// yields an error and no plan. Callers have not touched anything yet, so the
// error is safe to report as "nothing was changed".
std::expected<UseSwitchPlan, XvmUserError> plan_use_switch(
        const VersionDB& db,
        const Workspace& workspace,
        const std::string& target,
        const std::string& resolvedVersion) {
    auto selection = resolve_binding_selection(db, target, resolvedVersion);
    if (!selection) {
        return std::unexpected(describe(selection.error()));
    }

    UseSwitchPlan plan{.members = selection->members};

    // Preflight every member before emitting any change, so "the third of
    // five members is missing" is an error rather than a toolchain switched
    // two fifths of the way.
    for (const auto& [memberTarget, memberVersion] : plan.members) {
        auto infoIt = db.find(memberTarget);
        if (infoIt == db.end()
            || !infoIt->second.versions.contains(memberVersion)) {
            return std::unexpected(XvmUserError{
                .code = "xvm-switch-member-missing",
                .what = std::format(
                    "cannot switch to {}@{}: member '{}@{}' has no payload "
                    "record", target, resolvedVersion, memberTarget,
                    memberVersion),
                .target = memberTarget,
                .version = memberVersion,
                .hint = "run `xlings self doctor` to see the offending entry",
            });
        }
    }

    for (const auto& [memberTarget, memberVersion] : plan.members) {
        const auto activeIt = workspace.find(memberTarget);
        const std::string previous =
            activeIt == workspace.end() ? std::string{} : activeIt->second;

        const auto& current = db.at(memberTarget).versions.at(memberVersion);
        if (previous == memberVersion) {
            // Already the active version, but the sysroot may not reflect it:
            // a removal that fell back to this release takes the removed
            // release's headers out and puts nothing back. Re-materializing
            // is idempotent, so `use` doubles as the repair for that -- and
            // as a no-op when nothing is wrong. Nothing to remove first.
            if (current.includedir.empty() && current.libdir.empty()) continue;
            plan.switches.push_back({
                .target = memberTarget,
                .version = memberVersion,
                .previousVersion = previous,
                .installIncludeDir = current.includedir,
                .installLibDir = current.libdir,
            });
            continue;
        }

        MemberSwitch change{
            .target = memberTarget,
            .version = memberVersion,
            .previousVersion = previous,
        };
        if (!previous.empty()) {
            auto infoIt = db.find(memberTarget);
            if (infoIt != db.end()) {
                auto prevIt = infoIt->second.versions.find(previous);
                if (prevIt != infoIt->second.versions.end()) {
                    change.removeIncludeDir = prevIt->second.includedir;
                    change.removeLibDir = prevIt->second.libdir;
                }
            }
        }
        change.installIncludeDir = current.includedir;
        change.installLibDir = current.libdir;
        plan.switches.push_back(std::move(change));
    }

    return plan;
}

}  // namespace xlings::xvm
