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
//
// A library member is one file: `<payload>/<sourceName>` has to appear in the
// sysroot lib directory as `<destinationName>`. Headers are not per member --
// they belong to the release as a whole and live on the plan, below.
//
// This used to be phrased as a *directory* to link wholesale
// (`installLibDir`), read from `VData::libdir`. Nothing ever wrote that field
// -- on a real installation it is absent from all 372 entries -- so the
// planner emitted no library work at all and `xlings use` was a no-op for
// libraries. Every fact needed to place one was already in the entry:
// `path` + `sourceName` + `destinationName`, exactly what the install path
// has always used. The planner was simply reading the wrong field.
struct MemberSwitch {
    std::string target;
    std::string version;
    std::string previousVersion;   // empty when the member was not active

    // Library placement. Empty for members that are not libraries.
    std::string installLibSource;  // absolute: <payload>/<sourceName>
    std::string installLibName;    // basename in the sysroot lib directory

    // Declared file asset. Empty for members that are not `files` entries.
    // The destination is relative to the subos root -- see FilePlacement.
    std::string installFileSource;
    std::string installFileDest;
    // What the outgoing version occupied, when it is a different path.
    std::string removeFileDest;
    // What the outgoing version occupied. Usually the same name as
    // installLibName -- two versions of one library share a soname -- in
    // which case the placement replaces it and nothing is unlinked.
    std::string removeLibName;
};

struct UseSwitchPlan {
    std::map<std::string, std::string> members;  // target -> version
    std::vector<MemberSwitch> switches;          // only members that move

    // Header assets for the whole release, deduplicated, with the two lists
    // already disjoint.
    //
    // Per-member header lists would be wrong twice over. Every member of a
    // group resolves to the same declaration, so the same directory would be
    // linked once per member. Worse, the loop that applied them removed and
    // installed one member at a time: member B's removal of the outgoing
    // release could delete links member A had just installed for the incoming
    // one, whenever the two releases share a header name -- which for two
    // versions of one toolchain is every header it ships.
    //
    // Hoisting them here makes the order unambiguous: everything the outgoing
    // release put in the sysroot comes out, then everything the incoming one
    // needs goes in.
    std::vector<HeaderAsset> removeHeaders;
    std::vector<HeaderAsset> installHeaders;
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

    // Accumulate the release's header assets while walking the members, then
    // reconcile the two sets once at the end. Appending to a vector and
    // deduplicating afterwards keeps the declaration order the recipe wrote,
    // which decides which copy wins when two assets ship a header of the same
    // name.
    std::vector<HeaderAsset> incoming;
    std::vector<HeaderAsset> outgoing;

    for (const auto& [memberTarget, memberVersion] : plan.members) {
        const auto activeIt = workspace.find(memberTarget);
        const std::string previous =
            activeIt == workspace.end() ? std::string{} : activeIt->second;

        auto currentHeaders =
            group_header_assets(db, memberTarget, memberVersion);
        incoming.insert(incoming.end(),
                        currentHeaders.begin(), currentHeaders.end());

        auto placement = library_placement(db, memberTarget, memberVersion);
        auto installSource = std::move(placement.source);
        auto installName = std::move(placement.name);
        auto file = file_placement(db, memberTarget, memberVersion);

        if (previous == memberVersion) {
            // Already the active version, but the sysroot may not reflect it:
            // a removal that fell back to this release takes the removed
            // release's headers out and puts nothing back. Re-materializing
            // is idempotent, so `use` doubles as the repair for that -- and
            // as a no-op when nothing is wrong. Nothing to remove first.
            if (installSource.empty() && file.empty()) continue;
            plan.switches.push_back({
                .target = memberTarget,
                .version = memberVersion,
                .previousVersion = previous,
                .installLibSource = std::move(installSource),
                .installLibName = std::move(installName),
                .installFileSource = std::move(file.source),
                .installFileDest = std::move(file.destination),
            });
            continue;
        }

        MemberSwitch change{
            .target = memberTarget,
            .version = memberVersion,
            .previousVersion = previous,
            .installLibSource = std::move(installSource),
            .installLibName = std::move(installName),
            .installFileSource = std::move(file.source),
            .installFileDest = std::move(file.destination),
        };
        if (!previous.empty()) {
            auto previousHeaders =
                group_header_assets(db, memberTarget, previous);
            outgoing.insert(outgoing.end(),
                            previousHeaders.begin(), previousHeaders.end());
            auto previousPlacement =
                library_placement(db, memberTarget, previous);
            // Only worth recording when the outgoing version occupied a
            // *different* name. The common case -- same soname across two
            // versions -- is a replacement, and unlinking first would open a
            // window with the library missing.
            if (!previousPlacement.name.empty()
                && previousPlacement.name != change.installLibName) {
                change.removeLibName = std::move(previousPlacement.name);
            }
            // Same reasoning for a file asset: only unlink when the outgoing
            // version occupied a path the incoming one does not reuse.
            auto previousFile =
                file_placement(db, memberTarget, previous);
            if (!previousFile.destination.empty()
                && previousFile.destination != change.installFileDest) {
                change.removeFileDest = std::move(previousFile.destination);
            }
        }
        plan.switches.push_back(std::move(change));
    }

    auto dedup = [](std::vector<HeaderAsset>& assets) {
        std::set<std::pair<std::string, std::string>> seen;
        std::erase_if(assets, [&](const HeaderAsset& asset) {
            return asset.sourceDir.empty()
                || !seen.emplace(asset.sourceDir,
                                 asset.destinationPrefix).second;
        });
    };
    dedup(incoming);
    dedup(outgoing);

    // An asset the incoming release also needs must not be taken out first.
    // Two releases of the same toolchain can share a header directory, and
    // `use` re-materializes the active release on every call to repair a
    // drifted sysroot -- removing and relinking would open a window on every
    // one of those calls where the header is simply absent, long enough for a
    // concurrent build to fail on it. install_headers already skips a link
    // that is correct; this makes sure nothing removes it beforehand.
    std::set<std::pair<std::string, std::string>> kept;
    for (const auto& asset : incoming) {
        kept.emplace(asset.sourceDir, asset.destinationPrefix);
    }
    std::erase_if(outgoing, [&](const HeaderAsset& asset) {
        return kept.contains({asset.sourceDir, asset.destinationPrefix});
    });

    plan.installHeaders = std::move(incoming);
    plan.removeHeaders = std::move(outgoing);

    return plan;
}

}  // namespace xlings::xvm
