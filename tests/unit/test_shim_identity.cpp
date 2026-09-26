// tests/unit/test_shim_identity.cpp — what a file in a subos bin IS (#615).
//
// WHAT THESE DEFEND. A shim used to be "ours" and "current" for one reason:
// it was the entry binary's file object. Windows shims are hard links, and an
// upgrade replaces the entry's file object on purpose, so after `self update`
// every shim was at once "somebody else's file" and still running the
// previous client -- with nothing able to put it back.
//
// The load-bearing properties:
//
//   * ownership comes from CONTENT (the marker, or the legacy fingerprint),
//     never from what a file links to;
//   * currency comes from identity or identical bytes, so the copy fallback
//     is not rewritten on every rebuild;
//   * an unreadable file is Unknown, never Stale -- nothing that could not be
//     read is replaced;
//   * the #615 mechanism itself, reproduced on POSIX with hard links, so the
//     Linux CI exercises the code the Windows fix depends on.

#include <gtest/gtest.h>

import std;
import xlings.core.xvm.shim_identity;
import xlings.core.xvm.shim_table;
import xlings.core.entry_binary;
import xlings.platform;

namespace xvm = xlings::xvm;
namespace fs = std::filesystem;

namespace {

fs::path make_temp_dir(std::string_view tag) {
    auto root = fs::temp_directory_path()
        / std::format("xlings-shim-identity-{}-{}", tag,
                      std::chrono::steady_clock::now()
                          .time_since_epoch().count());
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& p, std::string_view content) {
    std::ofstream out(p, std::ios::binary);
    out << content;
}

// A "build": the marker plus something that makes this build's bytes its own.
std::string marked_build(std::string_view tag) {
    return std::format("MZ..{}..{}..", xvm::kMulticallMarker, tag);
}

std::string legacy_build(std::string_view tag) {
    return std::format("MZ..[xlings:self]: failed to create shim {{}} - {{}}..{}",
                       tag);
}

bool hard_link(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_hard_link(from, to, ec);
    return !ec;
}

} // namespace

TEST(ShimIdentity, SameFileObjectIsCurrent) {
    auto root = make_temp_dir("same");
    auto entry = root / "entry";
    write_file(entry, marked_build("v2"));
    ASSERT_TRUE(hard_link(entry, root / "node"));

    xvm::ShimClassifier c(entry);
    EXPECT_EQ(c.classify(root / "node").state, xvm::ShimState::Current);
    fs::remove_all(root);
}

TEST(ShimIdentity, SymlinkToTheEntryIsCurrent) {
    auto root = make_temp_dir("symlink");
    auto entry = root / "entry";
    write_file(entry, "anything");
    std::error_code ec;
    fs::create_symlink(entry, root / "node", ec);
    if (ec) GTEST_SKIP() << "no symlink privilege";

    xvm::ShimClassifier c(entry);
    EXPECT_EQ(c.classify(root / "node").state, xvm::ShimState::Current);
    fs::remove_all(root);
}

// What `create_shim` writes on a filesystem without hard links. Calling it
// stale would rewrite it on every rebuild, forever.
TEST(ShimIdentity, ByteIdenticalCopyIsCurrent) {
    auto root = make_temp_dir("copy");
    auto entry = root / "entry";
    write_file(entry, marked_build("v2"));
    fs::copy_file(entry, root / "node");

    xvm::ShimClassifier c(entry);
    EXPECT_EQ(c.classify(root / "node").state, xvm::ShimState::Current);
    fs::remove_all(root);
}

// The #615 state: a pre-marker build left behind by an older client.
TEST(ShimIdentity, LegacyBuildIsStaleWithoutHandoff) {
    auto root = make_temp_dir("legacy");
    auto entry = root / "entry";
    write_file(entry, marked_build("v2"));
    write_file(root / "mcpp", legacy_build("2026.9.20.1"));

    xvm::ShimClassifier c(entry);
    auto id = c.classify(root / "mcpp");
    EXPECT_EQ(id.state, xvm::ShimState::Stale);
    EXPECT_FALSE(id.handoffCapable)
        << "a legacy build runs its own dispatcher; it cannot hand off";
    fs::remove_all(root);
}

TEST(ShimIdentity, MarkedOlderBuildIsStaleAndHandsOff) {
    auto root = make_temp_dir("marked");
    auto entry = root / "entry";
    write_file(entry, marked_build("v2"));
    write_file(root / "clang", marked_build("v1"));

    xvm::ShimClassifier c(entry);
    auto id = c.classify(root / "clang");
    EXPECT_EQ(id.state, xvm::ShimState::Stale);
    EXPECT_TRUE(id.handoffCapable);
    fs::remove_all(root);
}

// The scanner reads in chunks; a marker straddling a chunk boundary must
// still be found, or a large real build would read as somebody's program.
TEST(ShimIdentity, MarkerAcrossAChunkBoundaryIsFound) {
    auto root = make_temp_dir("boundary");
    auto entry = root / "entry";
    write_file(entry, marked_build("v2"));
    std::string big((1u << 20) - 10, 'x');
    big += xvm::kMulticallMarker;
    big += "tail";
    write_file(root / "big", big);

    xvm::ShimClassifier c(entry);
    auto id = c.classify(root / "big");
    EXPECT_EQ(id.state, xvm::ShimState::Stale);
    EXPECT_TRUE(id.handoffCapable);
    fs::remove_all(root);
}

TEST(ShimIdentity, SomebodysProgramIsForeign) {
    auto root = make_temp_dir("foreign");
    auto entry = root / "entry";
    write_file(entry, marked_build("v2"));
    write_file(root / "theirs", "\x7f" "ELF a real program that is not xlings");

    xvm::ShimClassifier c(entry);
    EXPECT_EQ(c.classify(root / "theirs").state, xvm::ShimState::Foreign);
    fs::remove_all(root);
}

// A symlink to anything but the entry is not a shim this home wrote -- even
// when its target happens to be an xlings build.
TEST(ShimIdentity, SymlinkElsewhereIsForeignWhateverItPointsAt) {
    auto root = make_temp_dir("symlink-elsewhere");
    auto entry = root / "entry";
    write_file(entry, marked_build("v2"));
    write_file(root / "other-xlings", marked_build("v1"));
    std::error_code ec;
    fs::create_symlink(root / "other-xlings", root / "node", ec);
    if (ec) GTEST_SKIP() << "no symlink privilege";

    xvm::ShimClassifier c(entry);
    EXPECT_EQ(c.classify(root / "node").state, xvm::ShimState::Foreign);
    fs::remove_all(root);
}

// "Could not read" is not evidence. A file xlings would replace must be one
// it could prove is an xlings build.
TEST(ShimIdentity, UnreadableIsUnknownNeverStale) {
    if constexpr (xlings::platform::OS_NAME == "windows") {
        GTEST_SKIP() << "POSIX permission bits";
    } else {
        auto root = make_temp_dir("unreadable");
        auto entry = root / "entry";
        write_file(entry, marked_build("v2"));
        write_file(root / "locked", legacy_build("old"));
        fs::permissions(root / "locked", fs::perms::none);
        std::ifstream probe(root / "locked");
        if (probe.is_open()) {
            fs::permissions(root / "locked", fs::perms::owner_all);
            fs::remove_all(root);
            GTEST_SKIP() << "running with permission to read anything (root)";
        }

        xvm::ShimClassifier c(entry);
        EXPECT_EQ(c.classify(root / "locked").state, xvm::ShimState::Unknown);
        fs::permissions(root / "locked", fs::perms::owner_all);
        fs::remove_all(root);
    }
}

// ─── the #615 mechanism, reproduced where CI can see it ─────────────
//
// Hard links, as on Windows: replace the entry the way an upgrade does, and
// the shim that was current a moment ago is an older build than the entry.
// The table then relinks it -- the name stays, the file behind it changes.
TEST(ShimIdentity, ReplacingTheEntryDetachesHardLinkShimsAndRepointHeals) {
    auto root = make_temp_dir("mechanism");
    auto bin = root / "bin";
    fs::create_directories(bin);
    auto entry = root / "entry";
    write_file(entry, marked_build("v1"));
    ASSERT_TRUE(hard_link(entry, bin / "mcpp"));
    ASSERT_EQ(xvm::ShimClassifier(entry).classify(bin / "mcpp").state,
              xvm::ShimState::Current);

    auto payload = root / "payload";
    write_file(payload, marked_build("v2"));
    ASSERT_TRUE(xlings::platform::atomic_replace_executable(payload, entry));

    auto scan = xvm::scan_actual(bin, entry);
    ASSERT_TRUE(scan.stale.contains("mcpp"))
        << "an upgrade detached the shim and nothing noticed";
    EXPECT_TRUE(scan.foreign.empty())
        << "an older xlings build was read as somebody else's program";

    auto diff = xvm::plan_table({"mcpp"}, scan, {});
    ASSERT_EQ(diff.toRepoint, std::vector<std::string>{"mcpp"});
    EXPECT_TRUE(diff.toAdd.empty());
    EXPECT_TRUE(diff.toRemove.empty());

    auto report = xvm::apply_table(diff, bin, entry);
    EXPECT_EQ(report.repointed, std::vector<std::string>{"mcpp"});
    EXPECT_TRUE(report.failed.empty());
    EXPECT_TRUE(xvm::same_bytes(bin / "mcpp", entry));
    EXPECT_TRUE(xvm::plan_table({"mcpp"}, xvm::scan_actual(bin, entry), {})
                    .empty())
        << "a relink that leaves work behind churns on every rebuild";
    fs::remove_all(root);
}

// `self update` replaced the entry twice per run with the same bytes, and on
// Windows every replacement detaches every shim. Identical bytes are not a
// replacement.
TEST(ShimIdentity, ReplacingWithIdenticalBytesKeepsTheFileObject) {
    auto root = make_temp_dir("identical");
    auto entry = root / "entry";
    write_file(entry, marked_build("v2"));
    ASSERT_TRUE(hard_link(entry, root / "shim"));
    auto payload = root / "payload";
    fs::copy_file(entry, payload);

    ASSERT_TRUE(xlings::entry_binary::replace_with(payload, entry,
                                                   "xlings@2", "2"));
    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(root / "shim", entry, ec))
        << "an identical payload replaced the entry's file object anyway";
    fs::remove_all(root);
}

TEST(ShimIdentity, SameBytes) {
    auto root = make_temp_dir("bytes");
    write_file(root / "a", "same");
    write_file(root / "b", "same");
    write_file(root / "c", "diff");
    EXPECT_TRUE(xvm::same_bytes(root / "a", root / "b"));
    EXPECT_FALSE(xvm::same_bytes(root / "a", root / "c"));
    EXPECT_FALSE(xvm::same_bytes(root / "a", root / "missing"))
        << "a read failure must never read as 'nothing to do'";
    fs::remove_all(root);
}
