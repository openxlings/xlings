// tests/unit/test_shim_table.cpp — the routing table's whole decision.
//
// WHAT THESE DEFEND. A shim file is a link to the entry binary: it carries no
// version and no owner, so it can only mean ROUTING ("this name dispatches
// through xlings"), never STATE ("this subos has that version active"). Before
// this module those two meanings shared one directory, written by two scopes
// and read by three readers that each assumed a different one — and the files
// that most needed reporting, a project-scope install's entries in the global
// bin, were the ones no reader could see at all.
//
// So the load-bearing properties here are not "the diff is a set difference".
// They are:
//
//   * a name is desired because the WORKSPACE says so, never because a file
//     exists or a payload looks healthy (an earlier draft probed the payload
//     and would have dropped every alias package and every broken install out
//     of the table, downgrading dispatch's precise error into the shell's
//     "command not found");
//   * a file that is NOT one of our shims is never removed, however much its
//     name looks like ours;
//   * the entry binary's own link is never removed, and IS created when the
//     workspace actually has it — both directions, for different reasons.

#include <gtest/gtest.h>

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.shim_table;

namespace xvm = xlings::xvm;
namespace fs = std::filesystem;

namespace {

// Platform suffix, spelled out rather than taken from the module under test:
// if `shim_filename` ever stopped adding `.exe` these tests must fail, and
// they cannot if they ask the same function for the answer.
std::string named(std::string_view base) {
#if defined(_WIN32)
    return std::string(base) + ".exe";
#else
    return std::string(base);
#endif
}

xvm::VersionDB db_with_program(const std::string& target,
                               const std::string& version,
                               const std::string& kind = "program") {
    xvm::VersionDB db;
    xvm::add_version(db, target, version, "/pkg/" + target, kind, "", "");
    return db;
}

fs::path make_temp_dir(std::string_view tag) {
    auto root = fs::temp_directory_path()
        / std::format("xlings-shim-table-{}-{}", tag,
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

} // namespace

// ─── compute_desired: the workspace decides, and only the workspace ──

TEST(ShimTableDesired, ActiveProgramIsDesired) {
    auto db = db_with_program("node", "22.17.1");
    xvm::Workspace active{{"node", "22.17.1"}};

    auto desired = xvm::compute_desired(db, active, {});

    EXPECT_TRUE(desired.contains(named("node")));
    EXPECT_EQ(desired.size(), 1u);
}

// The clause that actually excluded all 23 leaked names measured on a real
// home. They were registered, they had payloads — they simply were not active
// in the subos whose bin held the file.
TEST(ShimTableDesired, RegisteredButNotActiveIsNotDesired) {
    auto db = db_with_program("slang", "2025.10");
    xvm::Workspace active;   // nothing active here

    EXPECT_TRUE(xvm::compute_desired(db, active, {}).empty());
}

TEST(ShimTableDesired, EmptyVersionIsNotAnActiveVersion) {
    auto db = db_with_program("node", "22.17.1");
    xvm::Workspace active{{"node", ""}};

    EXPECT_TRUE(xvm::compute_desired(db, active, {}).empty());
}

// lib and files packages are registered in xvm on purpose — they need version
// management too — but nothing execs them, so they get no name on PATH.
TEST(ShimTableDesired, NonProgramKindsAreNotDesired) {
    for (const auto* kind : {"lib", "files"}) {
        auto db = db_with_program("libfoo", "1.0", kind);
        xvm::Workspace active{{"libfoo", "1.0"}};
        EXPECT_TRUE(xvm::compute_desired(db, active, {}).empty())
            << "kind=" << kind;
    }
}

// A name active in the workspace but absent from the DB cannot be spelled or
// dispatched. Skipped rather than guessed at.
TEST(ShimTableDesired, ActiveWithoutDbEntryIsNotDesired) {
    xvm::VersionDB db;
    xvm::Workspace active{{"ghost", "1.0"}};

    EXPECT_TRUE(xvm::compute_desired(db, active, {}).empty());
}

// The reason the table exists at all: a project's own bin is never on PATH, so
// its command names have to appear in the bin directory that is.
TEST(ShimTableDesired, ProjectCommandsJoinTheTable) {
    auto db = db_with_program("cmake", "3.30");
    xvm::Workspace active{{"cmake", "3.30"}};

    std::vector<xvm::ProjectContribution> projects{
        { .root = "/home/u/work/api", .commands = {"node", "npm"} },
    };

    auto desired = xvm::compute_desired(db, active, projects);

    EXPECT_TRUE(desired.contains(named("cmake")));
    EXPECT_TRUE(desired.contains(named("node")));
    EXPECT_TRUE(desired.contains(named("npm")));
}

// A project whose state file is gone or unparseable contributes nothing. This
// IS the reclamation path for a deleted project: its names simply stop being
// desired, and the next rebuild takes them out of every subos.
TEST(ShimTableDesired, UnreadableProjectContributesNothing) {
    xvm::VersionDB db;
    std::vector<xvm::ProjectContribution> projects{
        { .root = "/gone", .commands = {"node"}, .readable = false },
    };

    EXPECT_TRUE(xvm::compute_desired(db, {}, projects).empty());
}

// ─── plan_table: what a rebuild would change ────────────────────────

TEST(ShimTablePlan, AddsMissingAndRemovesStale) {
    std::set<std::string> desired{named("node"), named("npm")};
    xvm::ActualScan actual;
    actual.ours = {named("npm"), named("slang")};

    auto diff = xvm::plan_table(desired, actual, {});

    ASSERT_EQ(diff.toAdd.size(), 1u);
    EXPECT_EQ(diff.toAdd.front(), named("node"));
    ASSERT_EQ(diff.toRemove.size(), 1u);
    EXPECT_EQ(diff.toRemove.front(), named("slang"));
}

TEST(ShimTablePlan, MatchingTableIsEmptyDiff) {
    std::set<std::string> desired{named("node")};
    xvm::ActualScan actual;
    actual.ours = {named("node")};

    EXPECT_TRUE(xvm::plan_table(desired, actual, {}).empty());
}

// Someone's own binary sitting in a bin directory. It is reported and left
// alone — and, critically, a desired name that collides with it is NOT queued
// for creation, because creating it would overwrite their file.
TEST(ShimTablePlan, ForeignFileIsNeverRemovedNorOverwritten) {
    std::set<std::string> desired{named("node"), named("npm")};
    xvm::ActualScan actual;
    actual.ours = {};
    actual.foreign = {named("node")};

    auto diff = xvm::plan_table(desired, actual, {});

    ASSERT_EQ(diff.toAdd.size(), 1u);
    EXPECT_EQ(diff.toAdd.front(), named("npm"));
    EXPECT_TRUE(diff.toRemove.empty());
    ASSERT_EQ(diff.foreign.size(), 1u);
    EXPECT_EQ(diff.foreign.front(), named("node"));
}

// `ensure_subos_shims` puts the entry binary's link in a home subos that never
// installed the package. Nothing in the workspace justifies that file, so an
// unguarded diff reads it as stale and deletes the one file every other shim
// points at.
TEST(ShimTablePlan, ReservedNameIsNeverRemoved) {
    std::set<std::string> desired;            // workspace knows nothing
    xvm::ActualScan actual;
    actual.ours = {named("xlings")};

    auto diff = xvm::plan_table(desired, actual, {"xlings"});

    EXPECT_TRUE(diff.toRemove.empty());
    EXPECT_TRUE(diff.toAdd.empty());
}

// The other direction. `xlings` is also a real package: installing it into a
// subos must give that subos its shim. Protection is removal-only.
TEST(ShimTablePlan, ReservedNameIsStillAddedWhenActive) {
    std::set<std::string> desired{named("xlings")};
    xvm::ActualScan actual;                   // nothing on disk yet

    auto diff = xvm::plan_table(desired, actual, {"xlings"});

    ASSERT_EQ(diff.toAdd.size(), 1u);
    EXPECT_EQ(diff.toAdd.front(), named("xlings"));
}

// ─── scan_actual: which files are ours ──────────────────────────────

TEST(ShimTableScan, ClassifiesLinksToTheEntryBinaryAsOurs) {
    auto root = make_temp_dir("scan");
    auto entry = root / "xlings-entry";
    auto bin = root / "bin";
    fs::create_directories(bin);
    { std::ofstream out(entry); out << "entry"; }

    std::error_code ec;
    fs::create_symlink(entry, bin / named("node"), ec);
    if (ec) {
        // No symlink privilege (unprivileged Windows): a hard link is what
        // `create_shim` falls back to, and `equivalent` must answer for it too.
        ec.clear();
        fs::create_hard_link(entry, bin / named("node"), ec);
    }
    ASSERT_FALSE(ec) << ec.message();

    { std::ofstream out(bin / named("their-tool")); out << "not ours"; }

    auto scan = xvm::scan_actual(bin, entry);

    EXPECT_TRUE(scan.ours.contains(named("node")));
    EXPECT_FALSE(scan.ours.contains(named("their-tool")));
    ASSERT_EQ(scan.foreign.size(), 1u);
    EXPECT_EQ(scan.foreign.front(), named("their-tool"));

    fs::remove_all(root, ec);
}

// Windows-only in origin, asserted everywhere because the filter is.
//
// A shim that cannot be unlinked because it is running gets renamed to
// `<name>.xlings.old` and scheduled for deletion at reboot. That leftover is
// still a link to the entry binary, so an unfiltered scan would claim it,
// find no desired name matching, queue it for removal, and rename it aside
// again — one suffix per rebuild, forever.
TEST(ShimTableScan, DisplacementDebrisIsNotAnEntry) {
    auto root = make_temp_dir("debris");
    auto entry = root / "xlings-entry";
    auto bin = root / "bin";
    fs::create_directories(bin);
    { std::ofstream out(entry); out << "entry"; }

    std::error_code ec;
    for (const auto* name : {"slang.xlings.old", "slang.xlings.old1"}) {
        ec.clear();
        fs::create_symlink(entry, bin / name, ec);
        if (ec) { ec.clear(); fs::create_hard_link(entry, bin / name, ec); }
        ASSERT_FALSE(ec) << ec.message();
    }

    auto scan = xvm::scan_actual(bin, entry);

    EXPECT_TRUE(scan.ours.empty())
        << "displacement debris was claimed as a routing entry";
    EXPECT_TRUE(scan.foreign.empty())
        << "displacement debris was reported as somebody's file";

    fs::remove_all(root, ec);
}

TEST(ShimTableScan, MissingBinDirectoryIsEmptyNotAnError) {
    auto scan = xvm::scan_actual("/nonexistent/xlings/bin", "/nonexistent/x");
    EXPECT_TRUE(scan.ours.empty());
    EXPECT_TRUE(scan.foreign.empty());
}

// ─── apply_table: idempotent, and never touches what is not ours ────

TEST(ShimTableApply, CreatesRemovesAndConverges) {
    auto root = make_temp_dir("apply");
    auto entry = root / "xlings-entry";
    auto bin = root / "bin";
    fs::create_directories(bin);
    { std::ofstream out(entry); out << "entry"; }

    // A stale entry to remove, and a foreign file that must survive.
    std::error_code ec;
    fs::create_symlink(entry, bin / named("stale"), ec);
    if (ec) { ec.clear(); fs::create_hard_link(entry, bin / named("stale"), ec); }
    ASSERT_FALSE(ec) << ec.message();
    { std::ofstream out(bin / named("theirs")); out << "not ours"; }

    auto scan = xvm::scan_actual(bin, entry);
    auto diff = xvm::plan_table({named("node")}, scan, {});
    auto report = xvm::apply_table(diff, bin, entry);

    EXPECT_EQ(report.added, std::vector<std::string>{named("node")});
    EXPECT_EQ(report.removed, std::vector<std::string>{named("stale")});
    EXPECT_TRUE(report.failed.empty());
    EXPECT_TRUE(fs::exists(bin / named("node")));
    EXPECT_FALSE(fs::exists(bin / named("stale")));
    EXPECT_TRUE(fs::exists(bin / named("theirs")))
        << "a file that is not ours was removed";

    // Idempotent: a second pass has nothing to do. A rebuild that keeps
    // finding work is a rebuild whose desired set does not match what it
    // writes, and it would churn the bin directory on every install.
    auto scan2 = xvm::scan_actual(bin, entry);
    auto diff2 = xvm::plan_table({named("node")}, scan2, {});
    EXPECT_TRUE(diff2.empty());

    fs::remove_all(root, ec);
}

// The invariant the whole design turns on, stated as a test so it cannot be
// re-derived away: a file's presence is not a claim about any version.
TEST(ShimTableApply, PresenceOfAFileAssertsNothingAboutActivation) {
    auto root = make_temp_dir("invariant");
    auto entry = root / "xlings-entry";
    auto bin = root / "bin";
    fs::create_directories(bin);
    { std::ofstream out(entry); out << "entry"; }

    // A project contributes `node`; this subos has no `node` active at all.
    auto db = db_with_program("cmake", "3.30");
    xvm::Workspace active{{"cmake", "3.30"}};
    std::vector<xvm::ProjectContribution> projects{
        { .root = "/home/u/work/api", .commands = {"node"} },
    };

    auto desired = xvm::compute_desired(db, active, projects);
    auto report = xvm::apply_table(
        xvm::plan_table(desired, xvm::scan_actual(bin, entry), {}), bin, entry);

    EXPECT_TRUE(fs::exists(bin / named("node")))
        << "a project's command name must reach the bin directory on PATH";
    EXPECT_FALSE(active.contains("node"))
        << "and it must be there without the workspace claiming a version";

    std::error_code ec;
    fs::remove_all(root, ec);
}
