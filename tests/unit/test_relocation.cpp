// tests/unit/test_relocation.cpp — "was this home written for another root?"
//
// WHAT THESE DEFEND. This predicate is the gate on two destructive repairs:
// deleting a sysroot link and dropping a registration. Measured on 2026.9.4.1
// with the predicate absent, one `self doctor --fix` on a moved home deleted
// 1173 links and dropped 367 registrations while every payload was present
// under the new root. So the load-bearing properties are not "prefix
// substitution works". They are:
//
//   * a detection that cannot point at a real payload REFUSES. A home whose
//     packages were genuinely deleted must keep reaching the prune, and the
//     easiest way to break that is to answer from the records alone;
//   * the root is the MODE, never the longest common prefix. 7 of 2908 records
//     on the measured home name `/usr/bin` and `~/.cargo/bin` — self-managed
//     tools whose paths must not move, and which would drag a common prefix
//     down to `/`;
//   * a prefix ends on a path boundary. `<root>-backup` and `<root>.bak` are
//     exactly the directories a user creates while moving a home around, and
//     rewriting them would invent a path that was never theirs;
//   * separator spelling is not identity, and on Windows neither is case —
//     otherwise every Windows home diagnoses itself as relocated.

#include <gtest/gtest.h>

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.relocation;

namespace xvm = xlings::xvm;

namespace {

xvm::VData vdata(std::string path) {
    xvm::VData d;
    d.path = std::move(path);
    return d;
}

xvm::VersionDB db_with(std::vector<std::pair<std::string, std::string>> entries) {
    xvm::VersionDB db;
    int n = 0;
    for (auto& [target, path] : entries) {
        auto& info = db[target];
        info.type = "program";
        info.versions[std::format("1.{}", n++)] = vdata(path);
    }
    return db;
}

// Everything exists.
const xvm::PathProbe kAllPresent = [](const std::string&) { return true; };
// Nothing exists.
const xvm::PathProbe kNonePresent = [](const std::string&) { return false; };
// The payloads exist here; the OLD ROOT does not -- an ordinary move.
const xvm::PathProbe kOldRootGone = [](const std::string& p) {
    return p.find("/data/xpkgs/") != std::string::npos;
};
// Two paths never resolve to the same place.
const xvm::SamePlaceProbe kNeverSame =
    [](const std::string&, const std::string&) { return false; };
// The old path resolves to this home -- the compensating-symlink workaround.
const xvm::SamePlaceProbe kAlwaysSame =
    [](const std::string&, const std::string&) { return true; };

}  // namespace

TEST(Relocation, SplitsAtTheStoreSegment) {
    auto split = xvm::split_store_path("/home/u/.xlings/data/xpkgs/xim-x-a/1.0");
    ASSERT_TRUE(split.has_value());
    EXPECT_EQ(split->first, "/home/u/.xlings");
    EXPECT_EQ(split->second, "data/xpkgs/xim-x-a/1.0");
}

TEST(Relocation, SplitsAtTheFirstStoreSegmentNotTheLast) {
    // A payload that packages an xlings home of its own contains a second
    // `data/xpkgs`. The OUTER one is the one that says where this record's
    // home was.
    auto split = xvm::split_store_path(
        "/home/u/.xlings/data/xpkgs/xim-x-a/1.0/data/xpkgs/inner");
    ASSERT_TRUE(split.has_value());
    EXPECT_EQ(split->first, "/home/u/.xlings");
}

TEST(Relocation, AcceptsBothSeparators) {
    auto split = xvm::split_store_path(R"(C:\Users\me\.xlings\data\xpkgs\a\1.0)");
    ASSERT_TRUE(split.has_value());
    EXPECT_EQ(split->first, R"(C:\Users\me\.xlings)");

    // Mixed, which is a real shape: the two halves are joined by different
    // code.
    auto mixed = xvm::split_store_path(R"(C:/Users/me/.xlings\data\xpkgs\a\1.0)");
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(mixed->first, "C:/Users/me/.xlings");
}

TEST(Relocation, NonStorePathsHaveNoRoot) {
    EXPECT_FALSE(xvm::split_store_path("/usr/bin").has_value());
    EXPECT_FALSE(xvm::split_store_path("/home/u/.cargo/bin").has_value());
    // `data` and `xpkgs` must be adjacent path components, not a substring.
    EXPECT_FALSE(xvm::split_store_path("/home/u/metadata/xpkgsly").has_value());
}

TEST(Relocation, SamePathTextIgnoresSeparatorSpelling) {
    EXPECT_TRUE(xvm::same_path_text("/a/b", "/a/b/"));
    EXPECT_TRUE(xvm::same_path_text(R"(C:\a\b)", "C:/a/b"));
    EXPECT_FALSE(xvm::same_path_text("/a/b", "/a/bc"));
}

TEST(Relocation, DetectsAMovedHome) {
    auto db = db_with({{"a", "/old/home/data/xpkgs/xim-x-a/1.0"},
                       {"b", "/old/home/data/xpkgs/xim-x-b/2.0"}});
    auto reloc = xvm::detect_relocation(db, "/new/home", kOldRootGone, kNeverSame);
    ASSERT_TRUE(reloc.has_value());
    EXPECT_EQ(reloc->oldRoot, "/old/home");
    EXPECT_EQ(reloc->newRoot, "/new/home");
    EXPECT_EQ(reloc->entries, 2u);
    EXPECT_EQ(reloc->recoverable, 2u);
}

TEST(Relocation, RefusesWhenNothingIsRecoverable) {
    // THE property that keeps a genuinely emptied home prunable. Without it,
    // "the recorded path does not exist" would be read as "the home moved"
    // and the dead registrations would be kept forever.
    auto db = db_with({{"a", "/old/home/data/xpkgs/xim-x-a/1.0"}});
    EXPECT_FALSE(
        xvm::detect_relocation(db, "/new/home", kNonePresent, kNeverSame).has_value());
}

TEST(Relocation, HomeThatDidNotMoveIsNotRelocated) {
    auto db = db_with({{"a", "/new/home/data/xpkgs/xim-x-a/1.0"}});
    EXPECT_FALSE(
        xvm::detect_relocation(db, "/new/home", kOldRootGone, kNeverSame).has_value());
}

TEST(Relocation, SelfManagedToolsDoNotDecideTheRoot) {
    // 7 of 2908 records on the measured home look like this. A longest-common
    // -prefix derivation would answer `/`; the mode answers the store.
    auto db = db_with({{"a",  "/old/home/data/xpkgs/xim-x-a/1.0"},
                       {"b",  "/old/home/data/xpkgs/xim-x-b/2.0"},
                       {"rg", "/usr/bin"},
                       {"cg", "/home/u/.cargo/bin"}});
    auto reloc = xvm::detect_relocation(db, "/new/home", kOldRootGone, kNeverSame);
    ASSERT_TRUE(reloc.has_value());
    EXPECT_EQ(reloc->oldRoot, "/old/home");
    // And they are not translated: a tool outside the store keeps its path.
    EXPECT_TRUE(xvm::relocated_path(*reloc, "/usr/bin").empty());
    EXPECT_TRUE(xvm::relocated_path(*reloc, "/home/u/.cargo/bin").empty());
}

TEST(Relocation, PicksTheDominantRootDeterministically) {
    auto db = db_with({{"a", "/oldA/data/xpkgs/xim-x-a/1.0"},
                       {"b", "/oldB/data/xpkgs/xim-x-b/2.0"},
                       {"c", "/oldB/data/xpkgs/xim-x-c/3.0"}});
    auto reloc = xvm::detect_relocation(db, "/new/home", kOldRootGone, kNeverSame);
    ASSERT_TRUE(reloc.has_value());
    EXPECT_EQ(reloc->oldRoot, "/oldB");
}

TEST(Relocation, TranslatesUnderTheCurrentRoot) {
    xvm::HomeRelocation reloc{.oldRoot = "/old/home", .newRoot = "/new/home"};
    EXPECT_EQ(xvm::relocated_path(reloc, "/old/home/data/xpkgs/a/1.0/lib"),
              "/new/home/data/xpkgs/a/1.0/lib");
    // The root itself.
    EXPECT_EQ(xvm::relocated_path(reloc, "/old/home"), "/new/home");
}

TEST(Relocation, PrefixMustEndOnAPathBoundary) {
    xvm::HomeRelocation reloc{.oldRoot = "/old/home", .newRoot = "/new/home"};
    EXPECT_TRUE(xvm::relocated_path(reloc, "/old/home-backup/data").empty());
    EXPECT_TRUE(xvm::relocated_path(reloc, "/old/homely").empty());
    EXPECT_TRUE(xvm::relocated_path(reloc, "/other/home/data").empty());
}

TEST(Relocation, TailIsCopiedVerbatim) {
    // A pathological record survives translation unchanged rather than being
    // quietly normalised into a different path.
    xvm::HomeRelocation reloc{.oldRoot = "/old/home", .newRoot = "/new/home"};
    EXPECT_EQ(xvm::relocated_path(reloc, R"(/old/home\data\xpkgs\a)"),
              R"(/new/home\data\xpkgs\a)");
}

TEST(Relocation, ALiveSecondHomeIsNotAMove) {
    // The project scope. `Config::versions()` merges the global and project
    // databases into one view, so a project's records -- naming
    // `<checkout>/.xlings` while the home is `~/.xlings` -- look exactly like
    // a moved home's, and the same package usually exists in both stores. Left
    // ungated, the repair would re-point the project's payloads into the
    // global store.
    auto db = db_with({{"a", "/home/u/proj/.xlings/data/xpkgs/xim-x-a/1.0"},
                       {"b", "/home/u/proj/.xlings/data/xpkgs/xim-x-b/2.0"}});
    const xvm::PathProbe everythingExists =
        [](const std::string&) { return true; };
    EXPECT_FALSE(xvm::detect_relocation(db, "/home/u/.xlings",
                                        everythingExists, kNeverSame)
                     .has_value());
}

TEST(Relocation, TheCompensatingSymlinkIsStillAMove) {
    // The old path exists, but it resolves to this home: the workaround users
    // reach for. Repairing it is what stops the home depending on that symlink
    // forever.
    auto db = db_with({{"a", "/old/home/data/xpkgs/xim-x-a/1.0"}});
    const xvm::PathProbe everythingExists =
        [](const std::string&) { return true; };
    auto reloc = xvm::detect_relocation(db, "/new/home", everythingExists,
                                        kAlwaysSame);
    ASSERT_TRUE(reloc.has_value());
    EXPECT_EQ(reloc->oldRoot, "/old/home");
}
