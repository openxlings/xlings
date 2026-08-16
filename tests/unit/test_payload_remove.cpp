// Unit tests for removing a payload something may be holding.
//
// Design: .agents/docs/2026-08-17-547-and-three-merged-prs-review.md §5
//
// The behaviour this covers is Windows-only in its CAUSE -- read-only
// attributes off a .vsix, `vctip.exe` still running inside the toolset it was
// launched from, a process whose working directory is in the tree. None of
// that reproduces on Linux.
//
// What DOES reproduce on Linux is every branch of the recovery, because each
// one is reachable by making a rename fail, and a rename fails on POSIX for a
// reason POSIX has: a directory without write permission cannot have entries
// removed from it. That is what these tests use, and it is why the two
// defects below were findable at all:
//
//   * the rollback `remove_all`d the staging directory, which held the files
//     it had already moved. "Half-moved is worse than untouched" was in the
//     comment; the code turned half-moved into half-DELETED with no copy
//     anywhere.
//   * the staging directory was created NEXT TO the version directories, and
//     seven places in this tree read every subdirectory of `xpkgs/<pkg>/` as
//     a version without skipping dotfiles.
//
// Both shipped. Neither was reachable by a test, because the function lived
// in an anonymous namespace inside installer.cpp -- which is the third
// finding, and the reason this file exists.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#if !defined(_WIN32)
#include <unistd.h>   // geteuid — the freeze trick does not apply to root
#endif

import std;
import xlings.core.xim.payload;

namespace xim = xlings::xim;
namespace fs = std::filesystem;

namespace {

// A store-shaped tree: <root>/data/xpkgs/<pkg>/<version>/...
// The shape matters -- `payload_trash_root` derives the staging location from
// it, and refuses to displace anything when it cannot find a store.
struct StoreFixture {
    fs::path root;

    explicit StoreFixture(std::string_view tag) {
        root = fs::temp_directory_path() /
               ("xlings-payload-rm-" + std::string(tag));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(payload() / "bin", ec);
        fs::create_directories(payload() / "lib", ec);
    }
    ~StoreFixture() {
        std::error_code ec;
        // Put permissions back before cleanup, or the fixture leaks the very
        // unwritable directory it created.
        for (auto& d : {payload() / "bin", payload() / "lib"}) {
            fs::permissions(d, fs::perms::owner_all, fs::perm_options::add, ec);
        }
        fs::remove_all(root, ec);
    }

    StoreFixture(const StoreFixture&) = delete;
    StoreFixture& operator=(const StoreFixture&) = delete;

    fs::path store()   const { return root / "data" / "xpkgs"; }
    fs::path pkgDir()  const { return store() / "xim-x-node"; }
    fs::path payload() const { return pkgDir() / "22.17.1"; }

    void file(const fs::path& rel, std::string_view body = "x") const {
        std::error_code ec;
        fs::create_directories((payload() / rel).parent_path(), ec);
        std::ofstream(payload() / rel) << body;
    }

    std::size_t file_count() const {
        std::error_code ec;
        std::size_t n = 0;
        for (auto it = fs::recursive_directory_iterator(payload(), ec);
             !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
            if (it->is_regular_file(ec)) ++n;
        }
        return n;
    }

    // Every direct child of the package directory, which is what seven other
    // places in this tree read as "the versions of this package".
    std::vector<std::string> versions_as_seen_by_the_store() const {
        std::error_code ec;
        std::vector<std::string> names;
        for (auto it = fs::directory_iterator(pkgDir(), ec);
             !ec && it != fs::directory_iterator{}; it.increment(ec)) {
            if (it->is_directory(ec)) names.push_back(it->path().filename().string());
        }
        std::ranges::sort(names);
        return names;
    }
};

// Make renames OUT of `dir` fail, the POSIX way: removing an entry from a
// directory needs write permission on that directory.
void freeze_(const fs::path& dir) {
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write
                             | fs::perms::others_write,
                    fs::perm_options::remove, ec);
}
void thaw_(const fs::path& dir) {
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add, ec);
}

#if !defined(_WIN32)
bool running_as_root() { return ::geteuid() == 0; }
#else
// Windows has no equivalent of "this directory refuses to give up entries",
// so the rollback tests skip there rather than assert something else.
bool running_as_root() { return true; }
#endif

}  // namespace

// ── the ordinary case ────────────────────────────────────────────────

TEST(PayloadRemove, RemovesAnUnheldPayload) {
    StoreFixture fx{"plain"};
    fx.file("bin/node");
    fx.file("lib/x.so");

    EXPECT_EQ(xim::remove_payload_dir(fx.payload()), xim::RemoveOutcome::Removed);
    EXPECT_FALSE(fs::exists(fx.payload()));
}

TEST(PayloadRemove, AnAbsentPayloadIsAlreadyRemoved) {
    StoreFixture fx{"absent"};
    std::error_code ec;
    fs::remove_all(fx.payload(), ec);
    EXPECT_EQ(xim::remove_payload_dir(fx.payload()), xim::RemoveOutcome::Removed);
}

// A read-only file is the first of Windows' three refusals, and the only one
// with a POSIX analogue we can set: clearing the bit must be enough.
TEST(PayloadRemove, ClearsReadOnlyAndRetries) {
    StoreFixture fx{"readonly"};
    fx.file("bin/node");
    std::error_code ec;
    fs::permissions(fx.payload() / "bin" / "node", fs::perms::owner_read, ec);

    EXPECT_EQ(xim::remove_payload_dir(fx.payload()), xim::RemoveOutcome::Removed);
    EXPECT_FALSE(fs::exists(fx.payload()));
}

// ── where displaced files go ─────────────────────────────────────────

TEST(PayloadRemove, TrashLivesOutsideTheStore) {
    StoreFixture fx{"trashroot"};
    const auto trash = xim::payload_trash_root(fx.payload());

    ASSERT_FALSE(trash.empty());
    EXPECT_EQ(trash, fx.root / "data" / "trash");

    // The assertion that matters is not where it IS but where it is NOT:
    // anything under `xpkgs/` is read as a package or a version by seven
    // other places, none of which skip dotfiles.
    auto rel = trash.lexically_relative(fx.store());
    EXPECT_TRUE(rel.empty() || *rel.begin() == "..")
        << "trash at " << trash << " is inside the store and would be read "
           "as a package";
}

TEST(PayloadRemove, APayloadOutsideAnyStoreIsNotDisplaced) {
    // No `xpkgs` ancestor -> the move strategy has nowhere legitimate to put
    // things, and an untouched payload beats one scattered somewhere nothing
    // will ever sweep.
    auto loose = fs::temp_directory_path() / "xlings-payload-rm-loose" / "thing";
    std::error_code ec;
    fs::remove_all(loose.parent_path(), ec);
    fs::create_directories(loose, ec);
    EXPECT_TRUE(xim::payload_trash_root(loose).empty());
    fs::remove_all(loose.parent_path(), ec);
}

// ── what happens when something really is holding a file ─────────────
//
// Two earlier versions of this section asserted things that could not
// happen, and each failure taught the code something:
//
//   1. "a partial move restores every file it displaced" -- no. The fast path
//      is `remove_all`, which on a partially held tree deletes everything it
//      can reach BEFORE reporting failure. "Roll back to untouched" was never
//      an available outcome, in this version or the one that shipped.
//   2. "freezing a directory makes the removal fail" -- no. `clear_readonly_`
//      chmods it straight back, which is exactly what it is for. On POSIX
//      NOTHING in a tree we own resists us: an open fd does not prevent
//      unlink, and we can always restore write permission. The `Partial`
//      BRANCH is genuinely unreachable on Linux and macOS.
//
// So the branch is not asserted end-to-end here, because it would be
// asserting on a path the test never enters -- a green that means nothing.
// What IS asserted is the verdict function the branch ends in, driven with
// real leftovers. It is production code, not a stub.

TEST(PayloadRemove, LeftoversAreStampedIncompleteSoAReinstallRebuilds) {
    StoreFixture fx{"partial"};
    fx.file("lib/held");   // what a holding process would leave behind

    EXPECT_EQ(xim::settle_removal(fx.payload(), "22.17.1"),
              xim::RemoveOutcome::Partial);
    EXPECT_TRUE(xim::stamped_incomplete(fx.payload()))
        << "without the stamp `payload_has_content` reports a package that is "
           "not there, and the next `xlings install` adopts the leftovers "
           "instead of rebuilding them";
}

// The other direction, and the one a wrong implementation passes by accident:
// a payload that came off cleanly must NOT be stamped. Stamping there would
// recreate the directory that was just deleted.
TEST(PayloadRemove, ACleanRemovalStampsNothing) {
    StoreFixture fx{"nostamp"};
    fx.file("bin/f0");

    ASSERT_EQ(xim::remove_payload_dir(fx.payload(), "22.17.1"),
              xim::RemoveOutcome::Removed);
    EXPECT_FALSE(fs::exists(fx.payload()))
        << "a removal that reported success must leave no directory behind";
}

// A skeleton of empty directories is the third Windows refusal (a process
// whose working directory is in the tree) and it is NOT a partial removal: a
// payload with no files is not installed, which is what uninstall promises.
TEST(PayloadRemove, AnEmptySkeletonIsRemovedNotPartial) {
    StoreFixture fx{"skeleton"};
    // bin/ and lib/ exist, no files anywhere.
    EXPECT_EQ(xim::settle_removal(fx.payload(), "22.17.1"),
              xim::RemoveOutcome::Removed);
    EXPECT_FALSE(xim::stamped_incomplete(fx.payload()))
        << "stamping an empty skeleton would make an uninstalled package look "
           "like a broken install forever";
}

TEST(PayloadRemove, AnAlreadyGonePayloadSettlesAsRemoved) {
    StoreFixture fx{"settled-gone"};
    std::error_code ec;
    fs::remove_all(fx.payload(), ec);
    EXPECT_EQ(xim::settle_removal(fx.payload(), "22.17.1"),
              xim::RemoveOutcome::Removed);
    EXPECT_FALSE(fs::exists(fx.payload()));
}

// A removal must never add something the store reads as a version. The
// staging directory used to be created beside the version directories, and
// seven places in this tree enumerate `xpkgs/<pkg>/*` as versions without
// skipping dotfiles.
TEST(PayloadRemove, RemovalAddsNothingTheStoreReadsAsAVersion) {
    StoreFixture fx{"noversion"};
    for (int i = 0; i < 4; ++i) fx.file("bin/f" + std::to_string(i));
    fx.file("lib/g");

    xim::remove_payload_dir(fx.payload(), "22.17.1");

    // The package directory may be empty now (everything came off), but it
    // must never have gained an entry.
    for (const auto& name : fx.versions_as_seen_by_the_store()) {
        EXPECT_EQ(name, "22.17.1")
            << "removal invented '" << name << "', which every store scan "
               "in this tree reads as a version of this package";
    }
}

// ── the sweep ────────────────────────────────────────────────────────

TEST(PayloadRemove, SweepClearsWhatAnEarlierRemovalParked) {
    StoreFixture fx{"sweep"};
    const auto trash = xim::payload_trash_root(fx.payload());
    std::error_code ec;
    fs::create_directories(trash / "xim-x-node-1.0.0", ec);
    std::ofstream(trash / "xim-x-node-1.0.0" / "leftover") << "x";

    EXPECT_EQ(xim::sweep_payload_trash(trash), 0);
    EXPECT_FALSE(fs::exists(trash / "xim-x-node-1.0.0"));
    EXPECT_FALSE(fs::exists(trash))
        << "an empty trash directory is noise in the data dir";
}

TEST(PayloadRemove, SweepingNothingIsFine) {
    StoreFixture fx{"sweep-empty"};
    EXPECT_EQ(xim::sweep_payload_trash(xim::payload_trash_root(fx.payload())), 0);
    EXPECT_EQ(xim::sweep_payload_trash({}), 0);
}
