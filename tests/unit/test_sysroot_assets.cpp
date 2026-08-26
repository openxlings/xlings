// tests/unit/test_sysroot_assets.cpp — declared file assets: who enumerates
// them, what happens when a release stops wanting one, and the two ways
// writing one can damage something that is not ours.
//
// openxlings/xlings#423. Declared assets shipped in 2026.7.27.0 and were never
// reclaimed by any removal path, in either of the two shapes a removal takes.
// The tests here are grouped by the question each answers, because the bug was
// that three different pieces of code answered ONE question three ways and
// only one of them was right:
//
//   "which destinations does this release own?"  ->  ReleaseFilePlacements
//   "what happens to a destination given up?"    ->  ReclaimDeclaredAssets
//   "where may a placement write?"               ->  PlaceAsset

#include <gtest/gtest.h>

import std;
import xlings.core.config;
import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.commands;
import xlings.core.xvm.switch_plan;
import xlings.core.xvm.removal;
import xlings.core.xim.installer;

namespace fs = std::filesystem;
using xlings::xvm::VersionDB;
using xlings::xvm::Workspace;

namespace {

// A home of our own, set before any test runs.
//
// `place_asset` reads `Config::paths()` to learn where the payload store is --
// it has to, because "is this ancestor a link into a payload" is the only
// question that separates a directory we may unwrap from one we must not
// touch. Pointing the whole process at a temporary home is what makes that
// answerable without involving the user's real one.
class SysrootAssetEnvironment final : public testing::Environment {
public:
    void SetUp() override {
        home_ = fs::temp_directory_path()
            / std::format("xlings-sysroot-assets-{}",
                          std::chrono::steady_clock::now()
                              .time_since_epoch().count());
        fs::create_directories(home_);
#ifdef _WIN32
        _putenv_s("XLINGS_HOME", home_.string().c_str());
#else
        ::setenv("XLINGS_HOME", home_.string().c_str(), 1);
#endif
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(home_, ec);
    }

private:
    fs::path home_;
};

[[maybe_unused]] auto* sysroot_asset_environment_ =
    testing::AddGlobalTestEnvironment(new SysrootAssetEnvironment);

fs::path scratch_(std::string_view label) {
    auto dir = fs::temp_directory_path()
        / std::format("xlings-{}-{}", label,
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool present_(const fs::path& p) {
    // Both halves. A dangling link IS a symlink and is NOT `exists()`; asking
    // only `exists` reads a leaked link as "already cleaned up", which is the
    // exact mistake that let #423's original test pass while the links were
    // still on disk.
    std::error_code ec;
    return fs::exists(p, ec) || fs::is_symlink(p, ec);
}

}  // namespace

// A version this database does not carry answers nothing -- and the question
// is asked before anything else in this file for a reason.
//
// GCC 16 ICEs (`finish_member_declaration`, cp/semantics.cc:4258) when the
// FIRST instantiation of a std container over a module-attached type is
// required from a namespace-scope helper. `VersionDB` is
// `std::map<std::string, VInfo@xlings.core.xvm.types>`, and the fixture below
// is exactly such a helper. Requiring the instantiation from a function body
// first is enough to avoid it, and the crashed cc1plus otherwise leaves a
// truncated .gcm that makes unrelated targets fail with "Bad file data".
//
// So this test is load-bearing twice: it asserts something true, and it must
// stay above the fixture.
TEST(ReleaseFilePlacements, AnUnregisteredCoordinateAnswersNothing) {
    VersionDB db;
    db["demo"].type = "program";
    EXPECT_TRUE(
        xlings::xvm::release_file_placements(db, "demo", "1.0.0").empty());
    EXPECT_TRUE(
        xlings::xvm::release_file_placements(db, "absent", "1.0.0").empty());
}

namespace {

// A release whose assets hang on generated member targets, exactly as
// libxpkg's `xvm.files` writes them: `<pkg>.files.<n>`, where `<n>` is a
// counter assigned in declaration order at install time. Nothing outside can
// predict the name, which is why asking about the package name never worked.
void declare_release_(VersionDB& db,
                      std::string_view pkg,
                      std::string_view version,
                      std::string_view payloadRoot,
                      const std::vector<std::pair<std::string, std::string>>&
                          srcDstPairs) {
    const xlings::xvm::BindingGroupRef ref{
        .provider = std::format("xim:{}", pkg),
        .providerVersion = std::string(version),
        .group = std::string(pkg),
        .rootTarget = std::string(pkg),
        .rootVersion = std::string(version),
    };

    auto& root = db[std::string(pkg)];
    root.type = "program";
    auto& rootData = root.versions[std::string(version)];
    rootData.path = std::string(payloadRoot);
    rootData.kind = "program";
    rootData.bindingGroup = ref;
    rootData.bindingMembers = {{std::string(pkg), std::string(version)}};

    for (std::size_t i = 0; i < srcDstPairs.size(); ++i) {
        const auto name = std::format("{}.files.{}", pkg, i + 1);
        auto& member = db[name];
        member.type = "files";
        auto& data = member.versions[std::string(version)];
        data.path = std::string(payloadRoot);
        data.kind = "files";
        data.fileSrc = srcDstPairs[i].first;
        data.fileDst = srcDstPairs[i].second;
        data.bindingGroup = ref;
        rootData.bindingMembers.emplace(name, std::string(version));
    }
    rootData.bindingMembersDeclared = true;
}

}  // namespace

// ============================================================
// Which destinations does this release own?
// ============================================================

// The regression test for #423 itself.
//
// Every asset registers on a generated member. `file_placement(db, "demo", v)`
// -- what the detach path asked for three releases running -- is empty for
// every release that has ever declared one, so the sysroot teardown reclaimed
// nothing and reported nothing.
TEST(ReleaseFilePlacements, WalksMembersRatherThanThePackageName) {
    VersionDB db;
    declare_release_(db, "demo", "1.0.0", "/pkg/demo/1.0.0",
                     {{"include/a.h", "usr/include/demo/a.h"},
                      {"include/b.h", "usr/include/demo/b.h"}});

    EXPECT_TRUE(xlings::xvm::file_placement(db, "demo", "1.0.0").empty())
        << "the package's own entry is never kind=files -- that is the trap";

    const auto placements =
        xlings::xvm::release_file_placements(db, "demo", "1.0.0");
    ASSERT_EQ(placements.size(), 2u);

    std::set<std::string> destinations;
    for (const auto& placement : placements) {
        destinations.insert(placement.destination);
        // The member coordinate travels with the placement: the caller asked
        // about a package and cannot otherwise say which member answered.
        EXPECT_TRUE(placement.target.starts_with("demo.files."));
        EXPECT_EQ(placement.version, "1.0.0");
    }
    EXPECT_EQ(destinations,
              (std::set<std::string>{"usr/include/demo/a.h",
                                     "usr/include/demo/b.h"}));
}

TEST(ReleaseFilePlacements, AskingAboutAMemberFindsTheWholeRelease) {
    VersionDB db;
    declare_release_(db, "demo", "1.0.0", "/pkg/demo/1.0.0",
                     {{"include/a.h", "usr/include/demo/a.h"},
                      {"include/b.h", "usr/include/demo/b.h"}});

    // `remove` resolves to whichever coordinate the user typed, which can be
    // a member. The answer must not depend on which one.
    EXPECT_EQ(xlings::xvm::release_file_placements(db, "demo.files.2", "1.0.0")
                  .size(), 2u);
}

TEST(ReleaseFilePlacements, ARejectedDestinationYieldsNoPlacement) {
    VersionDB db;
    declare_release_(db, "demo", "1.0.0", "/pkg/demo/1.0.0",
                     {{"include/a.h", "usr/include/demo/a.h"},
                      {"evil", "../../etc/passwd"}});

    const auto placements =
        xlings::xvm::release_file_placements(db, "demo", "1.0.0");
    ASSERT_EQ(placements.size(), 1u);
    EXPECT_EQ(placements.front().destination, "usr/include/demo/a.h");
}

// Taking something out must not depend on understanding what put it in: a
// release whose group metadata no longer resolves still has to be removable,
// or the user is left with no command that gets them out.
TEST(ReleaseFilePlacements, FallsBackToTheNamedEntryWhenTheReleaseIsBroken) {
    VersionDB db;
    declare_release_(db, "demo", "1.0.0", "/pkg/demo/1.0.0",
                     {{"include/a.h", "usr/include/demo/a.h"}});
    // Point the group root at a version that does not exist.
    db["demo.files.1"].versions["1.0.0"].bindingGroup->rootVersion = "9.9.9";

    // No crash, no refusal: the named coordinate still answers for itself.
    const auto member =
        xlings::xvm::release_file_placements(db, "demo.files.1", "1.0.0");
    ASSERT_EQ(member.size(), 1u);
    EXPECT_EQ(member.front().destination, "usr/include/demo/a.h");
}

// ============================================================
// What happens to a destination the release stops wanting?
// ============================================================

TEST(ReclaimDeclaredAssets, RemovesWhatNothingActiveDeclares) {
    const auto root = scratch_("reclaim-basic");
    const auto payload = root / "store" / "demo" / "1.0.0";
    const auto subos = root / "subos";
    fs::create_directories(payload / "include");
    std::ofstream(payload / "include" / "a.h") << "x";
    fs::create_directories(subos / "usr" / "include" / "demo");
    fs::create_symlink(payload / "include" / "a.h",
                       subos / "usr" / "include" / "demo" / "a.h");

    VersionDB db;   // the record is gone: this is a full uninstall
    xlings::xvm::reclaim_declared_assets(
        subos, root / "store", {"usr/include/demo/a.h"}, db, Workspace{});

    EXPECT_FALSE(present_(subos / "usr" / "include" / "demo" / "a.h"));
    // The directory only existed to hold it.
    EXPECT_FALSE(fs::exists(subos / "usr" / "include" / "demo"));
    // The sysroot's own shape stays: a compiler configured with --sysroot
    // cares that this exists.
    EXPECT_TRUE(fs::exists(subos / "usr" / "include"));
    // The payload is not ours to delete -- only the link into it was.
    EXPECT_TRUE(fs::exists(payload / "include" / "a.h"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// `usr/include/scsi` on a real installation: declared by BOTH `xim:glibc` and
// `xim:linux-headers`, whose contents are disjoint. Two mistakes are reachable
// here and the library cleanup makes the first one:
//
//   * "something else declares it, leave it alone" strands a link into the
//     payload being deleted -- the leak becomes a dangling pointer.
//   * "it was declared by what I removed, delete it" takes a header away from
//     a package that is still installed and still declares it.
//
// Re-pointing restores the only declaration left standing. It is not a guess
// about which package should win.
TEST(ReclaimDeclaredAssets, RePointsADestinationAnotherActiveReleaseDeclares) {
    const auto root = scratch_("reclaim-contested");
    const auto storeRoot = root / "store";
    const auto goingPayload = storeRoot / "going" / "1.0.0";
    const auto stayingPayload = storeRoot / "staying" / "2.0.0";
    const auto subos = root / "subos";
    fs::create_directories(goingPayload / "include");
    fs::create_directories(stayingPayload / "include");
    std::ofstream(goingPayload / "include" / "scsi.h") << "going";
    std::ofstream(stayingPayload / "include" / "scsi.h") << "staying";
    fs::create_directories(subos / "usr" / "include");
    // Install order gave the path to the release now being removed.
    fs::create_symlink(goingPayload / "include" / "scsi.h",
                       subos / "usr" / "include" / "scsi.h");

    VersionDB db;
    declare_release_(db, "staying", "2.0.0", stayingPayload.string(),
                     {{"include/scsi.h", "usr/include/scsi.h"}});
    const Workspace activeAfter{{"staying", "2.0.0"},
                                {"staying.files.1", "2.0.0"}};

    xlings::xvm::reclaim_declared_assets(
        subos, storeRoot, {"usr/include/scsi.h"}, db, activeAfter);

    ASSERT_TRUE(present_(subos / "usr" / "include" / "scsi.h"));
    std::error_code ec;
    EXPECT_EQ(fs::read_symlink(subos / "usr" / "include" / "scsi.h", ec),
              stayingPayload / "include" / "scsi.h")
        << "the surviving declaration must own the path now";

    fs::remove_all(root, ec);
}

// A surviving record is not enough: it has to be active HERE. A package
// installed but not opted into this subos declares nothing about this
// subos's sysroot, so leaving the link would leave a pointer into a payload
// this operation is deleting.
TEST(ReclaimDeclaredAssets, RemovesWhenTheSurvivingClaimIsNotActiveHere) {
    const auto root = scratch_("reclaim-inactive-claim");
    const auto storeRoot = root / "store";
    const auto payload = storeRoot / "other" / "2.0.0";
    const auto subos = root / "subos";
    fs::create_directories(payload / "include");
    std::ofstream(payload / "include" / "x.h") << "x";
    fs::create_directories(subos / "usr" / "include");
    fs::create_symlink(payload / "include" / "x.h",
                       subos / "usr" / "include" / "x.h");

    VersionDB db;
    declare_release_(db, "other", "2.0.0", payload.string(),
                     {{"include/x.h", "usr/include/x.h"}});

    xlings::xvm::reclaim_declared_assets(
        subos, storeRoot, {"usr/include/x.h"}, db, Workspace{});

    EXPECT_FALSE(present_(subos / "usr" / "include" / "x.h"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

#if !defined(_WIN32)
// Someone replaced the link after we placed it. That is `xvm-sysroot-drift`,
// their decision, and giving up a release has no business overruling it.
//
// POSIX only: on Windows the asset is a hard link or a copy, which carries no
// origin, so the database is the only authority there and this question
// cannot be asked at all.
TEST(ReclaimDeclaredAssets, LeavesALinkThatPointsSomewhereElse) {
    const auto root = scratch_("reclaim-drift");
    const auto storeRoot = root / "store";
    const auto elsewhere = root / "elsewhere";
    const auto subos = root / "subos";
    fs::create_directories(storeRoot);
    fs::create_directories(elsewhere);
    std::ofstream(elsewhere / "x.h") << "not ours";
    fs::create_directories(subos / "usr" / "include");
    fs::create_symlink(elsewhere / "x.h", subos / "usr" / "include" / "x.h");

    VersionDB db;
    xlings::xvm::reclaim_declared_assets(
        subos, storeRoot, {"usr/include/x.h"}, db, Workspace{});

    EXPECT_TRUE(present_(subos / "usr" / "include" / "x.h"))
        << "a link xlings did not place is not xlings's to remove";

    std::error_code ec;
    fs::remove_all(root, ec);
}
#endif

TEST(PruneEmptyAssetDirs, StopsBeforeTheSysrootsOwnShape) {
    const auto root = scratch_("prune");
    const auto subos = root / "subos";
    fs::create_directories(subos / "usr" / "include" / "glib-2.0" / "gio");

    xlings::xvm::prune_empty_asset_dirs(
        subos / "usr" / "include" / "glib-2.0" / "gio" / "gone.h", subos);

    EXPECT_FALSE(fs::exists(subos / "usr" / "include" / "glib-2.0" / "gio"));
    EXPECT_FALSE(fs::exists(subos / "usr" / "include" / "glib-2.0"));
    EXPECT_TRUE(fs::exists(subos / "usr" / "include"));
    EXPECT_TRUE(fs::exists(subos / "usr"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PruneEmptyAssetDirs, StopsAtADirectoryThatStillHoldsSomething) {
    const auto root = scratch_("prune-nonempty");
    const auto subos = root / "subos";
    fs::create_directories(subos / "usr" / "include" / "X11" / "extensions");
    std::ofstream(subos / "usr" / "include" / "X11" / "Xlib.h") << "x";

    xlings::xvm::prune_empty_asset_dirs(
        subos / "usr" / "include" / "X11" / "extensions" / "gone.h", subos);

    EXPECT_FALSE(fs::exists(subos / "usr" / "include" / "X11" / "extensions"));
    EXPECT_TRUE(fs::exists(subos / "usr" / "include" / "X11" / "Xlib.h"))
        << "another package's header shared this directory";

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(PruneEmptyAssetDirs, RefusesAPathOutsideTheSubos) {
    const auto root = scratch_("prune-escape");
    fs::create_directories(root / "subos");
    fs::create_directories(root / "outside" / "deep" / "deeper");

    xlings::xvm::prune_empty_asset_dirs(
        root / "outside" / "deep" / "deeper" / "x.h", root / "subos");

    EXPECT_TRUE(fs::exists(root / "outside" / "deep" / "deeper"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// ============================================================
// Where may a placement write?
// ============================================================

#if !defined(_WIN32)
// `create_directories` treats an existing symlink-to-directory as "already
// there" and returns success. So placing `usr/include/scsi/sg.h` while
// `usr/include/scsi` is another package's directory-granularity asset writes
// the new entry INSIDE THAT PACKAGE'S PAYLOAD -- a store shared by every
// subos on the machine, which no uninstall will ever clean.
//
// Measured before this guard existed: the link landed in the payload.
//
// The guard converts rather than refuses, and loses nothing while doing it:
// the directory link becomes a real directory holding one link per entry the
// payload offered. Whichever of two packages sharing a directory is installed
// first, neither loses a header.
TEST(PlaceAsset, DoesNotWriteThroughAPayloadDirectoryLink) {
    const auto store = xlings::Config::paths().dataDir / "xpkgs";
    const auto owner = store / "test-owner" / "1.0.0" / "include" / "scsi";
    const auto newcomer =
        store / "test-newcomer" / "1.0.0" / "include" / "scsi";
    fs::create_directories(owner);
    fs::create_directories(newcomer);
    std::ofstream(owner / "owned.h") << "owned";
    std::ofstream(newcomer / "arriving.h") << "arriving";

    const auto root = scratch_("place-through-link");
    const auto subos = root / "subos";
    fs::create_directories(subos / "usr" / "include");
    fs::create_symlink(owner, subos / "usr" / "include" / "scsi");

    xlings::xvm::place_asset(
        (newcomer / "arriving.h").string(),
        subos / "usr" / "include" / "scsi" / "arriving.h");

    EXPECT_FALSE(fs::exists(owner / "arriving.h"))
        << "the placement wrote into another package's payload";

    std::error_code ec;
    EXPECT_FALSE(fs::is_symlink(subos / "usr" / "include" / "scsi", ec))
        << "the directory link should have been unwrapped";
    EXPECT_TRUE(present_(subos / "usr" / "include" / "scsi" / "arriving.h"));
    EXPECT_TRUE(present_(subos / "usr" / "include" / "scsi" / "owned.h"))
        << "unwrapping must not be a deletion: the other package's header "
           "has to survive as its own link";

    fs::remove_all(root, ec);
    fs::remove_all(store / "test-owner", ec);
    fs::remove_all(store / "test-newcomer", ec);
}
#endif

// ============================================================
// `use` between two releases with different asset sets
// ============================================================

// The outgoing release's extra assets used to be listed as stranded members
// and left on disk, which is a sysroot serving two releases at once -- the
// state the binding group exists to make unrepresentable. A declared asset
// has no ownership contest: its destination is decided by the declaration and
// by nothing else.
TEST(PlanUseSwitch, ReclaimsAssetsTheIncomingReleaseDoesNotDeclare) {
    VersionDB db;
    declare_release_(db, "demo", "2.0.0", "/pkg/demo/2.0.0",
                     {{"include/a.h", "usr/include/demo/a.h"},
                      {"include/b.h", "usr/include/demo/b.h"}});
    declare_release_(db, "demo", "1.0.0", "/pkg/demo/1.0.0",
                     {{"include/a.h", "usr/include/demo/a.h"}});
    const Workspace ws{{"demo", "2.0.0"},
                       {"demo.files.1", "2.0.0"},
                       {"demo.files.2", "2.0.0"}};

    auto plan = xlings::xvm::plan_use_switch(db, ws, "demo", "1.0.0");
    ASSERT_TRUE(plan.has_value()) << plan.error().what;

    // Members, not destinations. Until these are deactivated they still
    // declare their paths, and reclaiming would re-point each one back at the
    // release being left -- which is exactly what happened when this carried
    // destinations, and what a plan-only assertion could not see. The e2e
    // (S6) is what proves the file actually goes.
    ASSERT_EQ(plan->reclaimFiles.size(), 1u);
    EXPECT_EQ(plan->reclaimFiles.begin()->first, "demo.files.2");
    EXPECT_EQ(plan->reclaimFiles.begin()->second, "2.0.0");
    // A path both releases declare is a replacement, not a reclaim: unlinking
    // it would open a window with the header simply absent.
    EXPECT_FALSE(plan->reclaimFiles.contains("demo.files.1"));

    for (const auto& member : plan->stranded) {
        EXPECT_NE(member.kind, "files")
            << "a declared asset is reclaimed, not reported";
    }
}

// ============================================================
// The removal batch's list is the input; the policy above is the decision
// ============================================================

// `cleanup_removed_xvm_file_artifacts` is the third member of a family whose
// other two (`..._library_artifacts`, `..._program_artifacts`) have shipped
// since before declared assets existed. All three read the same list --
// `removalResult.removed`, resolved against the database as it was BEFORE the
// removal -- and the first two `continue` past every `kind == "files"` entry
// in it. That skip is the whole of #423: the data needed to clean the sysroot
// flowed right past the code that ignored it.
//
// Tested directly because the two call sites are one line each and the
// interesting behaviour is all here.
TEST(CleanupRemovedFileArtifacts, ReclaimsExactlyTheRemovedFilesEntries) {
    const auto root = scratch_("cleanup-removed");
    const auto storeRoot = root / "store";
    const auto payload = storeRoot / "going" / "1.0.0";
    const auto subos = root / "subos";
    fs::create_directories(payload / "include");
    std::ofstream(payload / "include" / "a.h") << "a";
    fs::create_directories(subos / "usr" / "include" / "going");
    fs::create_symlink(payload / "include" / "a.h",
                       subos / "usr" / "include" / "going" / "a.h");

    VersionDB before;
    declare_release_(before, "going", "1.0.0", payload.string(),
                     {{"include/a.h", "usr/include/going/a.h"}});
    // A program member in the same batch: it must be ignored here exactly as
    // the file members are ignored by the program cleanup.
    const VersionDB after;

    xlings::xim::cleanup_removed_xvm_file_artifacts(
        subos, storeRoot, before, after, Workspace{},
        xlings::xvm::RemovalBatchResult{
            .removed = {{"going", "1.0.0"},
                        {"going.files.1", "1.0.0"}}});

    EXPECT_FALSE(present_(subos / "usr" / "include" / "going" / "a.h"));
    EXPECT_FALSE(fs::exists(subos / "usr" / "include" / "going"));
    EXPECT_TRUE(fs::exists(subos / "usr" / "include"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

// A record that names a destination outside the permitted roots must not be
// able to steer a delete. The registration layer already refuses such a
// destination, so reaching this state means the file was edited by hand --
// which is exactly when re-checking is worth its cost.
TEST(CleanupRemovedFileArtifacts, RefusesADestinationTheRulesWouldNotAllow) {
    const auto root = scratch_("cleanup-escape");
    const auto subos = root / "subos";
    const auto outside = root / "outside";
    fs::create_directories(subos);
    fs::create_directories(outside);
    std::ofstream(outside / "victim") << "not yours";

    VersionDB before;
    declare_release_(before, "going", "1.0.0", "/pkg/going/1.0.0",
                     {{"x", "usr/include/x"}});
    before["going.files.1"].versions["1.0.0"].fileDst = "../outside/victim";

    xlings::xim::cleanup_removed_xvm_file_artifacts(
        subos, root / "store", before, VersionDB{}, Workspace{},
        xlings::xvm::RemovalBatchResult{
            .removed = {{"going.files.1", "1.0.0"}}});

    EXPECT_TRUE(fs::exists(outside / "victim"));

    std::error_code ec;
    fs::remove_all(root, ec);
}

#if !defined(_WIN32)
// The mirror of the place_asset trap, and it bites harder.
//
// `usr/include/scsi/fc/fc_fs.h` where `usr/include/scsi/fc` links into a
// payload does not name a file in the subos. It names the PACKAGE'S OWN FILE,
// in a store every subos on the machine reads and no uninstall audits.
//
// Found by running the fix against real packages rather than fixtures:
// giving up linux-headers in one subos emptied `include/scsi/fc/` out of its
// payload -- four headers gone from a package that was still installed and
// still active in another subos.
TEST(ReclaimDeclaredAssets, NeverRemovesThroughAPayloadDirectoryLink) {
    const auto root = scratch_("reclaim-through-link");
    const auto storeRoot = root / "store";
    const auto payload = storeRoot / "pkg" / "1.0.0" / "include" / "sub";
    const auto subos = root / "subos";
    fs::create_directories(payload);
    std::ofstream(payload / "victim.h") << "the package's own file";
    fs::create_directories(subos / "usr" / "include");
    // The shape an unwrap used to leave behind: a directory link nothing
    // declares, with declared leaves nominally underneath it.
    fs::create_symlink(payload, subos / "usr" / "include" / "sub");

    xlings::xvm::reclaim_declared_assets(
        subos, storeRoot, {"usr/include/sub/victim.h"}, VersionDB{},
        Workspace{});

    EXPECT_TRUE(fs::exists(payload / "victim.h"))
        << "the removal resolved through the link and deleted a file inside "
           "the payload";

    std::error_code ec;
    fs::remove_all(root, ec);
}

// Unwrapping must not put the shape it is removing back one level down.
//
// A sub-directory linked wholesale is a directory asset again -- undeclared
// this time, so nothing reclaims it, and a later removal of a leaf beneath it
// deletes inside the payload (see the test above). Every FILE gets its own
// link; every directory gets a real directory.
TEST(PlaceAsset, UnwrapsSubdirectoriesRatherThanRelinkingThem) {
    const auto store = xlings::Config::paths().dataDir / "xpkgs";
    const auto owner = store / "test-nested-owner" / "1.0.0" / "include" / "sub";
    const auto newcomer =
        store / "test-nested-newcomer" / "1.0.0" / "include" / "sub";
    fs::create_directories(owner / "deep");
    fs::create_directories(newcomer);
    std::ofstream(owner / "top.h") << "top";
    std::ofstream(owner / "deep" / "nested.h") << "nested";
    std::ofstream(newcomer / "arriving.h") << "arriving";

    const auto root = scratch_("unwrap-nested");
    const auto subos = root / "subos";
    fs::create_directories(subos / "usr" / "include");
    fs::create_symlink(owner, subos / "usr" / "include" / "sub");

    xlings::xvm::place_asset(
        (newcomer / "arriving.h").string(),
        subos / "usr" / "include" / "sub" / "arriving.h");

    std::error_code ec;
    const auto deep = subos / "usr" / "include" / "sub" / "deep";
    EXPECT_TRUE(fs::is_directory(deep, ec));
    EXPECT_FALSE(fs::is_symlink(deep, ec))
        << "a sub-directory was relinked wholesale -- the same shape, one "
           "level down, and undeclared";
    EXPECT_TRUE(present_(deep / "nested.h"));
    EXPECT_TRUE(present_(subos / "usr" / "include" / "sub" / "top.h"));
    EXPECT_TRUE(present_(subos / "usr" / "include" / "sub" / "arriving.h"));
    EXPECT_FALSE(fs::exists(owner / "arriving.h"));

    fs::remove_all(root, ec);
    fs::remove_all(store / "test-nested-owner", ec);
    fs::remove_all(store / "test-nested-newcomer", ec);
}
#endif
