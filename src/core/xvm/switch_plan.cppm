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
namespace xlings::xvm::detail_ {

}  // namespace xlings::xvm::detail_

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

// A name the outgoing release had and the incoming one does not.
//
// `use` only ever writes the members of the release it switches TO, so a name
// that release has no version of is left exactly where it was -- still
// active, still pointing into the release the user just left. The result is a
// silently mixed toolchain: `xlings use llvm 20.1.7` prints one line about
// llvm, and `clang` keeps answering 22.1.8.
//
// How often that happens was overstated here until 2026.8.2.1. The claim was
// that two releases of one package "routinely" register different program
// sets; re-measured across a 246-target installation, exactly ONE same-package
// pair disagreed -- llvm 20.1.7 vs 22.1.8, the very case above -- and that one
// was a Windows payload sitting in a Linux store, registering 29 unrunnable
// `.exe` names (xim-pkgindex#454, fixed 2026-07-30). So the founding evidence
// for this report was itself a recipe bug.
//
// The report stays: a package really can add or drop a tool between versions,
// and finding out from your compiler is the worst way to learn it. What
// changed is its weight -- collapsed to one line unless `-v` asks, because on
// real state it fires far less often than the noise it used to print.
struct StrandedMember {
    std::string target;
    std::string version;   // what it still resolves to
    // "program" | "lib" | "files". Never "group": a name that materializes
    // nothing cannot be left behind -- see kind_can_strand.
    std::string kind;
};

struct UseSwitchPlan {
    std::map<std::string, std::string> members;  // target -> version
    std::vector<MemberSwitch> switches;          // only members that move

    // Reported, never acted on: deactivating them would be a guess about
    // intent, and picking a replacement version even more so. `use --strict`
    // turns the report into a refusal for callers that want neither.
    //
    // Only ever filled for a switch WITHIN one package. Switching packages is
    // not a half-finished switch -- see the note above the stranded loop.
    //
    // `kind == "files"` is deliberately NOT here; it is in `reclaimFileDests`
    // below. The reason the rest of this list is only reported is that a name
    // has an owner and picking one is a guess -- `clang` could reasonably
    // belong to either llvm release. A declared asset has no such contest: its
    // destination is decided by the declaration and by nothing else, so
    // leaving it behind is not restraint, it is a sysroot holding two
    // releases at once. Reporting 29 of them and acting on none was also not
    // usable advice.
    std::vector<StrandedMember> stranded;

    // File members of the outgoing release the incoming one has no version
    // of: target -> the version they are still active at.
    //
    // Members rather than destinations, and that distinction is the whole
    // reason this works. Reclaiming a destination asks "does anything ACTIVE
    // still declare it" -- and until these members are deactivated, they do,
    // so reclaiming re-pointed each one straight back at the release being
    // left and the switch changed nothing. Measured before it was noticed:
    // `use demo 1.0.0` after 2.0.0 left 2.0.0's extra header exactly where it
    // was, and the plan-level test passed because it only ever looked at the
    // plan.
    //
    // Only ever filled for a switch WITHIN one package, same as `stranded`.
    // Moving `java` from one JDK to another leaves the other distribution
    // complete and active, and taking its headers out of the sysroot is not
    // this command's decision to make.
    std::map<std::string, std::string> reclaimFiles;

    // Switching packages: the names the package being left still owns.
    //
    // Kept apart from `stranded` on purpose. Nothing here is a problem: the
    // old package is untouched, complete, and still active, and the incoming
    // package has no version of these names to switch them to. They exist so
    // `-v` can answer "what did NOT come along", and they must never reach
    // `--strict` -- refusing over them would mean `java` can never move
    // between two JDK distributions at all.
    std::vector<StrandedMember> retainedByOldPackage;

    // The release coordinate this switch moves between, for the command line
    // that reports it. `from*` is empty on a first activation; all four are
    // empty when the entry carries no group metadata, and the caller then
    // falls back to the bare `target -> version` line.
    //
    // The provider name (`xim:jdk-zulu`) rather than the binding root name
    // (`xim-musl-gnu-gcc`): the first is what the user typed at install time,
    // the second is an implementation detail they have never seen.
    std::string fromProvider;
    std::string fromProviderVersion;
    std::string toProvider;
    std::string toProviderVersion;

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
        const std::string& resolvedVersion);

}  // namespace xlings::xvm
