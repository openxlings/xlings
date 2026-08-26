module xlings.core.xvm.switch_plan;

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.errors;

namespace xlings::xvm::detail_ {

// Which package a (target, version) belongs to, and at which version of it.
//
// Empty when the entry carries no group metadata -- entries written before
// providers were recorded. Callers must treat that as "unknown", not as "a
// different package".
std::pair<std::string, std::string> provider_of_(const VersionDB& db,
                                                 const std::string& target,
                                                 const std::string& version) {
    const auto it = db.find(target);
    if (it == db.end()) return {};
    const auto vit = it->second.versions.find(version);
    if (vit == it->second.versions.end() || !vit->second.bindingGroup) {
        return {};
    }
    return {vit->second.bindingGroup->provider,
            vit->second.bindingGroup->providerVersion};
}

}

namespace xlings::xvm {

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

    // Which package this switch moves between, for the report line.
    std::tie(plan.toProvider, plan.toProviderVersion) =
        detail_::provider_of_(db, target, resolvedVersion);

    // What the release being left had, that this one does not.
    //
    // Best effort by construction: an outgoing release that no longer
    // resolves cannot be enumerated, and failing the switch over that would
    // block the very command that repairs it. Only a name whose *current*
    // active version is the outgoing member's is reported -- anything else
    // was already pointing somewhere of the user's own choosing.
    if (const auto previousIt = workspace.find(target);
        previousIt != workspace.end()
        && !previousIt->second.empty()
        && previousIt->second != resolvedVersion) {
        std::tie(plan.fromProvider, plan.fromProviderVersion) =
            detail_::provider_of_(db, target, previousIt->second);

        // Switching PACKAGES is not a half-finished switch.
        //
        // `use java 25.0.4-zulu` hands the name `java` from one JDK to
        // another: the whole zulu release comes across, and temurin is left
        // untouched, complete, and still active. Nothing fell behind, so
        // there is nothing to report -- and reporting it costs real damage.
        // A group root's name IS the package name, so two packages can never
        // share one; the root of the package being left therefore lands in
        // the report on EVERY cross-package switch, and `--strict` refuses
        // over it, which means `--strict` can never move `java` between two
        // distributions no matter what the user does first. Measured on a
        // real home: `gcc` from the gnu package to the musl one produces 18
        // such lines, and not one of them names something the user can act
        // on -- the musl package has no version of any of them.
        //
        // This is the rule `self doctor` has followed since 2026.8.1.1: a
        // name held by another provider is ownership, not incoherence
        // (inspect.cppm's held_by_another_provider_). `use` was the last
        // place still reading it as a broken toolchain.
        //
        // Unknown metadata counts as the same package -- entries written
        // before providers were recorded keep the older, stricter behaviour
        // instead of silently losing the check. Same fallback direction as
        // held_by_another_provider_.
        const bool samePackage =
            plan.fromProvider.empty() || plan.toProvider.empty()
            || plan.fromProvider == plan.toProvider;

        if (auto outgoingSelection =
                resolve_binding_selection(db, target, previousIt->second)) {
            for (const auto& [memberTarget, memberVersion] :
                     outgoingSelection->members) {
                if (plan.members.contains(memberTarget)) continue;
                const auto activeIt = workspace.find(memberTarget);
                if (activeIt == workspace.end()
                    || activeIt->second != memberVersion) {
                    continue;
                }
                // A member that materializes nothing cannot be left behind.
                // The shim loop in cmd_use asks the same question of the same
                // authority before writing a shim; this report was the one
                // place that never asked.
                auto kind = effective_kind_of(db, memberTarget, memberVersion);
                if (!kind_can_strand(kind)) continue;
                // Within one package, a declared asset is reclaimed rather
                // than reported. See the note on `reclaimFiles`: a program
                // name has an owner and picking one is a guess, while a
                // declared destination is decided by the declaration and by
                // nothing else, so leaving it behind is not restraint -- it
                // is a sysroot holding two releases of one package at once.
                //
                // ACROSS packages it stays a report. The distribution being
                // left is complete and still active, and deciding that its
                // headers should leave the sysroot is a different question
                // from "did this switch finish".
                if (samePackage && kind == "files") {
                    plan.reclaimFiles.emplace(memberTarget, memberVersion);
                    continue;
                }
                (samePackage ? plan.stranded : plan.retainedByOldPackage)
                    .push_back({memberTarget, memberVersion, std::move(kind)});
            }
        }
    }

    return plan;
}

}
