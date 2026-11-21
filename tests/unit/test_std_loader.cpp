// The runtime a hook's root has at the standard loader path (design
// 2026-11-21-issue-724 §4.1): the decision, the worker's mounts, the
// carrier's link and what a failed hook adds. Pure functions over a root
// directory made here.
#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.subos.spec;
import xlings.subos.std_loader;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace sl = xlings::subos::std_loader;
namespace spec = xlings::subos::spec;

namespace {
const sl::Standard& x64() { return *sl::of_family("linux-x86_64-glibc"); }
const sl::Standard& arm() { return *sl::of_family("linux-aarch64-glibc"); }
}  // namespace

XTEST(StdLoader, EachGlibcFamilyHasItsStandardDirectory, .area = "subos", .covers = {"HOOK-RUNTIME-CONTRACT"}) {
    EXPECT_EQ(x64().path(), fs::path("/lib64/ld-linux-x86-64.so.2"));
    EXPECT_EQ(arm().path(), fs::path("/lib/ld-linux-aarch64.so.1"));
    EXPECT_EQ(sl::of_arch("x86_64"), &x64());
    EXPECT_EQ(sl::of_arch("arm64"), &arm());
    EXPECT_EQ(sl::of_family("linux-x86_64-musl"), nullptr) << "musl's loader does not search its directory";
}

XTEST(StdLoader, TheDecisionIsTheRootsStateOfTheStandardPath, .area = "subos", .covers = {"HOOK-RUNTIME-CONTRACT"},
      .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("std-loader-decide");
    for (const auto* s : {&x64(), &arm()}) {
        const auto root = home.root() / std::string(s->arch);
        fs::create_directories(root);
        // A: no directory -- the family's directory is it.
        EXPECT_EQ(sl::decide(root, *s, true), sl::Need::Link) << s->family;
        // Nothing the home could give.
        EXPECT_EQ(sl::decide(root, *s, false), sl::Need::Unprovided) << s->family;
        // B: a directory without this loader (Alpine's /lib) -- merged.
        fs::create_directories(root / fs::path(s->dir).relative_path());
        tk::write_file(root / fs::path(s->dir).relative_path() / "libz.so.1", "");
        EXPECT_EQ(sl::decide(root, *s, true), sl::Need::Merge) << s->family;
        // C: the root has a runtime there -- its own, whatever the home has.
        tk::write_file(root / s->path().relative_path(), "");
        EXPECT_EQ(sl::decide(root, *s, true), sl::Need::Present) << s->family;
        EXPECT_EQ(sl::decide(root, *s, false), sl::Need::Present) << s->family;
    }
    // A link to nowhere cannot be mounted over.
    const auto dangling = home.root() / "dangling";
    fs::create_directories(dangling);
    fs::create_symlink("/nonexistent", dangling / "lib64");
    EXPECT_EQ(sl::decide(dangling, x64(), true), sl::Need::Unprovided);
    // A root whose directory links to a real one is merged there.
    const auto linked = home.root() / "linked";
    fs::create_directories(linked / "usr/lib");
    fs::create_symlink("usr/lib", linked / "lib");
    EXPECT_EQ(sl::decide(linked, arm(), true), sl::Need::Merge);
}

XTEST(StdLoader, AWorkersMountsAreFixedForLinkAndMerge, .area = "subos", .covers = {"HOOK-RUNTIME-CONTRACT"}) {
    const std::vector<std::pair<std::string, fs::path>> family{
        {"ld-linux-aarch64.so.1", "/h/data/xpkgs/xim-x-glibc/2.44.3/lib/ld-linux-aarch64.so.1"},
        {"libc.so.6", "/h/data/xpkgs/xim-x-glibc/2.44.3/lib/libc.so.6"},
        {"libz.so.1", "/h/data/xpkgs/xim-x-glibc/2.44.3/lib/libz.so.1"},
    };
    using K = spec::MountKind;
    const std::vector<spec::MountOp> link{
        {K::Dir, "", "/lib"},
        {K::RoBind, "/h/data/xpkgs/xim-x-glibc/2.44.3/lib/ld-linux-aarch64.so.1", "/lib/ld-linux-aarch64.so.1"},
        {K::RoBind, "/h/data/xpkgs/xim-x-glibc/2.44.3/lib/libc.so.6", "/lib/libc.so.6"},
        {K::RoBind, "/h/data/xpkgs/xim-x-glibc/2.44.3/lib/libz.so.1", "/lib/libz.so.1"},
    };
    EXPECT_EQ(sl::mounts(sl::Need::Link, "/lib", family, {}), link);
    // The root's own entries win a name; the overlay keeps the hook's writes in memory.
    const std::vector<spec::MountOp> merge{
        {K::TmpOverlay, "/lib", "/lib"},
        {K::RoBind, "/h/data/xpkgs/xim-x-glibc/2.44.3/lib/ld-linux-aarch64.so.1", "/lib/ld-linux-aarch64.so.1"},
        {K::RoBind, "/h/data/xpkgs/xim-x-glibc/2.44.3/lib/libc.so.6", "/lib/libc.so.6"},
    };
    EXPECT_EQ(sl::mounts(sl::Need::Merge, "/lib", family, {"libz.so.1", "modules"}), merge);
    EXPECT_TRUE(sl::mounts(sl::Need::Present, "/lib", family, {}).empty());
    EXPECT_TRUE(sl::mounts(sl::Need::Unprovided, "/lib", family, {}).empty());
}

XTEST(StdLoader, AFamilysEntriesAreItsFilesResolved, .area = "subos", .covers = {"HOOK-RUNTIME-CONTRACT"},
      .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("std-loader-entries");
    const auto payload = home.root() / "glibc/lib64";
    tk::write_file(payload / "ld-linux-x86-64.so.2", "loader");
    tk::write_file(payload / "libc.so.6", "libc");
    const auto family = home.root() / "abi/linux-x86_64-glibc";
    fs::create_directories(family);
    fs::create_symlink(payload / "ld-linux-x86-64.so.2", family / "ld-linux-x86-64.so.2");
    fs::create_symlink(payload / "libc.so.6", family / "libc.so.6");
    fs::create_symlink(payload / "gone.so", family / "gone.so");
    const auto entries = sl::family_entries(family);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "ld-linux-x86-64.so.2");
    EXPECT_EQ(entries[0].second, fs::canonical(payload / "ld-linux-x86-64.so.2"));
    EXPECT_EQ(sl::names_in(payload), (std::set<std::string>{"ld-linux-x86-64.so.2", "libc.so.6"}));
    EXPECT_TRUE(sl::names_in(home.root() / "none").empty());
}

XTEST(StdLoader, AnOwnRootLinksItsStandardDirectoryOnlyWhenAbsent, .area = "subos",
      .covers = {"HOOK-RUNTIME-CONTRACT", "CARRIER-GUEST-CURRENT"}, .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("std-loader-link");
    // x86_64: /lib64 is the loader's own directory; absent in a carrier.
    const auto root = home.root() / "carrier";
    fs::create_directories(root);
    auto linked = sl::link_if_absent(root, x64(), "/xlings/abi/linux-x86_64-glibc");
    ASSERT_TRUE(linked) << linked.error();
    EXPECT_TRUE(*linked);
    EXPECT_EQ(fs::read_symlink(root / "lib64"), fs::path("/xlings/abi/linux-x86_64-glibc"));
    // Idempotent.
    linked = sl::link_if_absent(root, x64(), "/xlings/abi/linux-x86_64-glibc");
    ASSERT_TRUE(linked);
    EXPECT_FALSE(*linked);
    // aarch64: /lib is a general directory (WSL makes it for /lib/modules):
    // one that exists is never touched -- a worker merges into it instead.
    fs::create_directories(root / "lib/modules");
    linked = sl::link_if_absent(root, arm(), "/xlings/abi/linux-aarch64-glibc");
    ASSERT_TRUE(linked);
    EXPECT_FALSE(*linked);
    EXPECT_TRUE(fs::is_directory(fs::symlink_status(root / "lib")));
}

XTEST(StdLoader, AFailedHookSaysWhatRuntimeItSaw, .area = "subos", .covers = {"HOOK-RUNTIME-CONTRACT"}) {
    const auto& s = x64();
    // The contract met: nothing to add.
    EXPECT_TRUE(sl::note(s, {.need = sl::Need::Link}).empty());
    EXPECT_TRUE(sl::note(s, {.need = sl::Need::Present, .rootGlibc = "2.44.3", .envGlibc = "2.44.3"}).empty());
    EXPECT_TRUE(sl::note(s, {.need = sl::Need::Present, .rootGlibc = "2.45", .envGlibc = "2.44.3"}).empty());
    EXPECT_TRUE(sl::note(s, {.need = sl::Need::Present}).empty()) << "no environment runtime to compare with";
    // No worker: what is missing and how isolation is made available.
    const auto worker = sl::note(s, {.need = sl::Need::Link, .unavailable = "no usable bwrap"});
    EXPECT_NE(worker.find("/lib64/ld-linux-x86-64.so.2"), std::string::npos) << worker;
    EXPECT_NE(worker.find("no usable bwrap"), std::string::npos) << worker;
    EXPECT_NE(worker.find("xlings self doctor --isolation"), std::string::npos) << worker;
    // Nothing to give: the recipe's dependency.
    const auto none = sl::note(s, {.need = sl::Need::Unprovided});
    EXPECT_NE(none.find("declare that runtime as a dependency"), std::string::npos) << none;
    // The root's own, older or not glibc.
    const auto older = sl::note(s, {.need = sl::Need::Present, .rootGlibc = "2.27", .envGlibc = "2.44.3"});
    EXPECT_NE(older.find("glibc 2.27"), std::string::npos) << older;
    EXPECT_NE(older.find("2.44.3"), std::string::npos) << older;
    const auto compat = sl::note(s, {.need = sl::Need::Present, .envGlibc = "2.44.3"});
    EXPECT_NE(compat.find("not glibc"), std::string::npos) << compat;
}
